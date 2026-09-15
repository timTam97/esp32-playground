#include <Arduino.h>
#include "SD_MMC.h"
#include "esp_camera.h"
#include "esp_rom_crc.h"

// Diagnostic only. Never formats the card or runs a test without a RUN command.
constexpr size_t BLOCK_BYTES = 32768;
constexpr size_t DATA_BYTES = 8 * 1024 * 1024;
constexpr unsigned IMAGE_COUNT = 10;
char testDirectory[64] = {};
uint32_t imageSizes[IMAGE_COUNT] = {};
uint32_t imageChecksums[IMAGE_COUNT] = {};
bool ran = false;
bool passed = false;
bool mounted = false;
bool cameraActive = false;
uint8_t *buffer = nullptr;
uint8_t *expected = nullptr;

bool fail(const char *reason) {
  Serial.printf("RESULT FAIL %s\n", reason);
  return false;
}

bool mountCard() {
  // Freenove: CLK=14, CMD=15, D0=2. One-bit mode leaves camera GPIO4 free.
  // Disable automatic formatting; use Espressif's standard 20 MHz SD clock.
  mounted = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5);
  return mounted || fail("SD_MMC mount failed");
}

bool remountCard() {
  SD_MMC.end();
  mounted = false;
  return mountCard();
}

String dataPath() { return String(testDirectory) + "/data.bin"; }

String imagePath(unsigned index) {
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "/image-%02u.jpg", index);
  return String(testDirectory) + suffix;
}

void fillBlock(uint8_t *bytes, uint32_t block) {
  uint32_t state = 0x9e3779b9U ^ (block + 1);
  for (size_t i = 0; i < BLOCK_BYTES; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    bytes[i] = static_cast<uint8_t>(state);
  }
}

bool testData() {
  File file = SD_MMC.open(dataPath(), FILE_WRITE);
  if (!file) return fail("Cannot create diagnostic data file");
  uint32_t start = millis();
  for (size_t block = 0; block < DATA_BYTES / BLOCK_BYTES; ++block) {
    fillBlock(buffer, block);
    if (file.write(buffer, BLOCK_BYTES) != BLOCK_BYTES) {
      file.close();
      return fail("Short data write");
    }
  }
  file.flush();
  file.close();
  Serial.printf("DATA_WRITE bytes=%u ms=%lu\n", unsigned(DATA_BYTES),
                static_cast<unsigned long>(millis() - start));

  if (!remountCard()) return false;
  file = SD_MMC.open(dataPath(), FILE_READ);
  if (!file || file.size() != DATA_BYTES) {
    file.close();
    return fail("Data file size changed after remount");
  }
  start = millis();
  for (size_t block = 0; block < DATA_BYTES / BLOCK_BYTES; ++block) {
    fillBlock(expected, block);
    if (file.read(buffer, BLOCK_BYTES) != BLOCK_BYTES ||
        memcmp(buffer, expected, BLOCK_BYTES) != 0) {
      file.close();
      return fail("Data readback mismatch");
    }
  }
  file.close();
  Serial.printf("DATA_READ PASS bytes=%u ms=%lu\n", unsigned(DATA_BYTES),
                static_cast<unsigned long>(millis() - start));
  return true;
}

bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 4;
  config.pin_d1 = 5;
  config.pin_d2 = 18;
  config.pin_d3 = 19;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 21;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = -1;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    Serial.printf("CAMERA_ERROR %s\n", esp_err_to_name(result));
    return fail("Camera initialization failed with SD mounted");
  }
  cameraActive = true;
  Serial.printf("CAMERA sensor=0x%04x width=1600 height=1200\n",
                esp_camera_sensor_get()->id.PID);
  return true;
}

bool testCameraFiles() {
  if (!initCamera()) return false;
  for (unsigned i = 0; i < 4; ++i) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) return fail("Camera warmup failed");
    esp_camera_fb_return(frame);
  }
  for (unsigned i = 0; i < IMAGE_COUNT; ++i) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) return fail("Camera capture failed");
    if (frame->format != PIXFORMAT_JPEG || frame->width != 1600 ||
        frame->height != 1200 || frame->len < 4 ||
        frame->buf[0] != 0xff || frame->buf[1] != 0xd8 ||
        frame->buf[frame->len - 2] != 0xff || frame->buf[frame->len - 1] != 0xd9) {
      esp_camera_fb_return(frame);
      return fail("Camera did not supply a complete 1600x1200 JPEG");
    }
    imageSizes[i] = frame->len;
    imageChecksums[i] = esp_rom_crc32_le(0, frame->buf, frame->len);
    const uint32_t start = millis();
    File file = SD_MMC.open(imagePath(i), FILE_WRITE);
    bool written = file && file.write(frame->buf, frame->len) == frame->len;
    file.flush();
    file.close();
    esp_camera_fb_return(frame);
    if (!written) return fail("Camera JPEG write failed");
    Serial.printf("JPEG_WRITE index=%u bytes=%lu ms=%lu crc=%08lx\n", i,
                  static_cast<unsigned long>(imageSizes[i]),
                  static_cast<unsigned long>(millis() - start),
                  static_cast<unsigned long>(imageChecksums[i]));
  }
  esp_camera_deinit();
  cameraActive = false;
  if (!remountCard()) return false;
  for (unsigned i = 0; i < IMAGE_COUNT; ++i) {
    File file = SD_MMC.open(imagePath(i), FILE_READ);
    if (!file || file.size() != imageSizes[i]) {
      file.close();
      return fail("Saved JPEG size changed after remount");
    }
    uint32_t crc = 0;
    size_t remaining = imageSizes[i];
    while (remaining) {
      const size_t amount = remaining < BLOCK_BYTES ? remaining : BLOCK_BYTES;
      if (file.read(buffer, amount) != amount) {
        file.close();
        return fail("Short JPEG read");
      }
      crc = esp_rom_crc32_le(crc, buffer, amount);
      remaining -= amount;
    }
    file.close();
    if (crc != imageChecksums[i]) return fail("JPEG readback CRC mismatch");
  }
  Serial.printf("JPEG_READ PASS count=%u\n", IMAGE_COUNT);
  return true;
}

bool runTest() {
  if (!psramFound()) return fail("PSRAM not available");
  buffer = static_cast<uint8_t *>(ps_malloc(BLOCK_BYTES));
  expected = static_cast<uint8_t *>(ps_malloc(BLOCK_BYTES));
  if (!buffer || !expected) return fail("Cannot allocate diagnostic buffers");
  if (!mountCard()) return false;
  const uint64_t total = SD_MMC.totalBytes();
  const uint64_t used = SD_MMC.usedBytes();
  Serial.printf("CARD bytes=%llu filesystem_bytes=%llu used_bytes=%llu type=%u\n",
                SD_MMC.cardSize(), total, used, unsigned(SD_MMC.cardType()));
  if (total < used || total - used < 64ULL * 1024 * 1024)
    return fail("Less than 64 MiB free; no test files created");
  bool created = false;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    snprintf(testDirectory, sizeof(testDirectory), "/esp32-sd-test-%08lx",
             static_cast<unsigned long>(esp_random()));
    if (!SD_MMC.exists(testDirectory) && SD_MMC.mkdir(testDirectory)) {
      created = true;
      break;
    }
  }
  if (!created) {
    testDirectory[0] = '\0';
    return fail("Cannot create a unique test directory");
  }
  Serial.printf("TEST_DIRECTORY %s\n", testDirectory);
  return testData() && testCameraFiles();
}

void exportImage(unsigned index) {
  if (!passed || index >= IMAGE_COUNT) {
    Serial.println("EXPORT_ERROR No verified image at that index");
    return;
  }
  File file = SD_MMC.open(imagePath(index), FILE_READ);
  if (!file || file.size() != imageSizes[index]) {
    file.close();
    Serial.println("EXPORT_ERROR Cannot open image");
    return;
  }
  Serial.printf("JPEG_BEGIN %lu\n", static_cast<unsigned long>(file.size()));
  Serial.flush();
  while (file.available()) {
    size_t count = file.read(buffer, BLOCK_BYTES);
    if (!count) break;
    Serial.write(buffer, count);
  }
  file.close();
  Serial.flush();
  Serial.println("\nJPEG_END");
}

void cleanup() {
  if (cameraActive) {
    esp_camera_deinit();
    cameraActive = false;
  }
  bool okay = true;
  if (testDirectory[0]) {
    if (!mounted && !mountCard()) return;
    if (SD_MMC.exists(dataPath())) okay = SD_MMC.remove(dataPath()) && okay;
    for (unsigned i = 0; i < IMAGE_COUNT; ++i) {
      if (SD_MMC.exists(imagePath(i))) okay = SD_MMC.remove(imagePath(i)) && okay;
    }
    okay = SD_MMC.rmdir(testDirectory) && okay;
    if (okay) testDirectory[0] = '\0';
  }
  if (mounted) SD_MMC.end();
  mounted = false;
  passed = false;
  Serial.println(okay ? "CLEANUP PASS" : "CLEANUP FAIL");
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(1000);
  delay(750);
  Serial.println("SD_TEST READY commands=RUN,EXPORT0..9,CLEANUP");
}

void loop() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command == "RUN" && !ran) {
      ran = true;
      passed = runTest();
      if (cameraActive) {
        esp_camera_deinit();
        cameraActive = false;
      }
      if (passed) Serial.println("RESULT PASS");
    } else if (command.startsWith("EXPORT") && command.length() == 7 &&
               command[6] >= '0' && command[6] <= '9') {
      exportImage(command[6] - '0');
    } else if (command == "CLEANUP") {
      cleanup();
    } else {
      Serial.println("COMMAND_ERROR");
    }
  }
  delay(10);
}
