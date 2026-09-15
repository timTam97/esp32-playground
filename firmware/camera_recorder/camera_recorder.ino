#include <Arduino.h>
#include "SD_MMC.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "avi_writer.h"

#ifndef RECORDER_CLIP_SECONDS
#define RECORDER_CLIP_SECONDS 300
#endif

constexpr unsigned TARGET_FPS = 10;
constexpr uint16_t VIDEO_WIDTH = 1600, VIDEO_HEIGHT = 1200;
constexpr unsigned JPEG_QUALITY = 12;
constexpr unsigned BUTTON_GPIO = 0, STATUS_LED = 2;
constexpr uint32_t CLIP_MS = RECORDER_CLIP_SECONDS * 1000UL;
constexpr uint32_t INDEX_CAPACITY = RECORDER_CLIP_SECONDS * TARGET_FPS + 2;
constexpr uint64_t FREE_RESERVE = 32ULL * 1024 * 1024;
constexpr size_t IO_BUFFER_BYTES = 32768;
constexpr char DIRECTORY[] = "/recordings";
constexpr char MOUNT[] = "/sdcard";

enum class Mode { Starting, Recording, Finalizing, Stopped, Error };
Mode mode = Mode::Starting;
bool mounted = false, cameraActive = false;
uint32_t nextNumber = 1;
uint64_t clipStartedUs = 0, nextCaptureUs = 0, lastSensorUs = 0;
uint64_t freeAtClipStart = 0;
uint32_t lastSyncMs = 0, lastLogMs = 0, rejectedFrames = 0;
char currentName[24] = {}, partialPath[96] = {}, finalPath[96] = {};
char lastSaved[24] = {}, lastError[96] = {};
char commandBuffer[96] = {};
size_t commandLength = 0;
uint8_t *ioBuffer = nullptr;
recorder::IndexEntry *frameIndex = nullptr;
recorder::AviWriter avi;
recorder::HoldButton button;
recorder::VideoStats lastStats;

class SdSink final : public recorder::Sink {
 public:
  bool openNew(const char *path) {
    const int fd = ::open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return false;
    file_ = fdopen(fd, "w+b");
    if (!file_) {
      ::close(fd);
      return false;
    }
    setvbuf(file_, reinterpret_cast<char *>(ioBuffer), _IOFBF, IO_BUFFER_BYTES);
    return true;
  }
  size_t write(const void *bytes, size_t count) override {
    return file_ ? fwrite(bytes, 1, count, file_) : 0;
  }
  bool seek(uint32_t offset) override {
    return file_ && fseek(file_, offset, SEEK_SET) == 0;
  }
  bool sync() override {
    return file_ && fflush(file_) == 0 && fsync(fileno(file_)) == 0;
  }
  bool close() {
    if (!file_) return true;
    FILE *file = file_;
    file_ = nullptr;
    return fclose(file) == 0;
  }
  bool isOpen() const { return file_ != nullptr; }
 private:
  FILE *file_ = nullptr;
} output;

const char *modeName() {
  switch (mode) {
    case Mode::Starting: return "starting";
    case Mode::Recording: return "recording";
    case Mode::Finalizing: return "finalizing";
    case Mode::Stopped: return "stopped";
    default: return "error";
  }
}

void printStatus() {
  const auto &stats = mode == Mode::Recording ? avi.stats() : lastStats;
  const uint32_t elapsed = mode == Mode::Recording
      ? uint32_t((esp_timer_get_time() - clipStartedUs) / 1000) : stats.durationMs;
  Serial.printf(
      "STATUS {\"state\":\"%s\",\"file\":\"%s\",\"frames\":%lu,"
      "\"elapsed_ms\":%lu,\"clip_ms\":%lu,\"target_fps\":%u,"
      "\"width\":%u,\"height\":%u,\"rejected_frames\":%lu,"
      "\"safe_to_unplug\":%s,\"error\":\"%s\"}\n",
      modeName(), mode == Mode::Recording ? currentName : lastSaved,
      static_cast<unsigned long>(stats.frames),
      static_cast<unsigned long>(elapsed), static_cast<unsigned long>(CLIP_MS),
      TARGET_FPS, VIDEO_WIDTH, VIDEO_HEIGHT,
      static_cast<unsigned long>(rejectedFrames),
      mode == Mode::Stopped && !mounted ? "true" : "false", lastError);
}

bool mountCard() {
  if (mounted) return true;
  // IO2 is both the onboard LED and SD D0. Release it before claiming SD.
  pinMode(STATUS_LED, INPUT_PULLUP);
  mounted = SD_MMC.begin(MOUNT, true, false, SDMMC_FREQ_DEFAULT, 5);
  return mounted;
}

void unmountCard() {
  if (mounted) SD_MMC.end();
  mounted = false;
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH);  // The onboard IO2 LED is active-low.
}

void stopCamera() {
  if (cameraActive) esp_camera_deinit();
  cameraActive = false;
}

void failRecorder(const char *reason) {
  snprintf(lastError, sizeof(lastError), "%s", reason);
  output.close();
  stopCamera();
  unmountCard();
  mode = Mode::Error;
  Serial.printf("ERROR %s; any unfinished .part file has been preserved\n", reason);
  printStatus();
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
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&config) != ESP_OK) return false;
  cameraActive = true;
  Serial.printf("CAMERA sensor=0x%04x psram=%u\n",
                esp_camera_sensor_get()->id.PID,
                static_cast<unsigned>(ESP.getPsramSize()));
  for (unsigned i = 0; i < 4; ++i) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) return false;
    esp_camera_fb_return(frame);
  }
  return true;
}

bool validName(const char *name, bool allowPartial = false) {
  const size_t length = strlen(name);
  if (length != 15 && !(allowPartial && length == 16)) return false;
  if (strncmp(name, "REC", 3)) return false;
  for (unsigned i = 3; i < 11; ++i)
    if (name[i] < '0' || name[i] > '9') return false;
  return !strcmp(name + 11, ".avi") ||
         (allowPartial && !strcmp(name + 11, ".part"));
}

bool findNextNumber() {
  if (!SD_MMC.exists(DIRECTORY) && !SD_MMC.mkdir(DIRECTORY)) return false;
  File directory = SD_MMC.open(DIRECTORY);
  if (!directory || !directory.isDirectory()) return false;
  nextNumber = 1;
  while (File entry = directory.openNextFile()) {
    const String path = entry.path();
    const char *name = strrchr(path.c_str(), '/');
    name = name ? name + 1 : path.c_str();
    if (!entry.isDirectory() && validName(name, true)) {
      const uint32_t number = strtoul(name + 3, nullptr, 10);
      if (number >= nextNumber) nextNumber = number + 1;
      if (strstr(name, ".part"))
        Serial.printf("UNFINISHED %s (preserved for recovery)\n", name);
    }
    entry.close();
  }
  directory.close();
  return nextNumber <= 99999999;
}

bool openClip() {
  const uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
  if (total <= used || total - used <= FREE_RESERVE + 2 * 1024 * 1024) {
    snprintf(lastError, sizeof(lastError), "Card full; existing recordings preserved");
    return false;
  }
  freeAtClipStart = total - used;
  bool opened = false;
  while (nextNumber <= 99999999) {
    const uint32_t number = nextNumber++;
    snprintf(currentName, sizeof(currentName), "REC%08lu.avi",
             static_cast<unsigned long>(number));
    snprintf(finalPath, sizeof(finalPath), "%s%s/%s", MOUNT, DIRECTORY, currentName);
    snprintf(partialPath, sizeof(partialPath), "%s%s/REC%08lu.part",
             MOUNT, DIRECTORY, static_cast<unsigned long>(number));
    struct stat existing;
    if (stat(finalPath, &existing) == 0 || stat(partialPath, &existing) == 0) continue;
    if (!output.openNew(partialPath)) return false;
    opened = true;
    break;
  }
  if (!opened || !avi.begin(output, frameIndex, INDEX_CAPACITY, VIDEO_WIDTH,
                            VIDEO_HEIGHT, TARGET_FPS)) return false;
  clipStartedUs = esp_timer_get_time();
  nextCaptureUs = clipStartedUs;
  lastSyncMs = millis();
  mode = Mode::Recording;
  Serial.printf("CLIP_OPEN file=%s clip_ms=%lu\n", currentName,
                static_cast<unsigned long>(CLIP_MS));
  return true;
}

bool finalizeClip() {
  if (!output.isOpen()) return true;
  mode = Mode::Finalizing;
  const uint32_t duration = (esp_timer_get_time() - clipStartedUs) / 1000;
  if (!avi.stats().frames) {
    const bool closed = output.close();
    return closed && unlink(partialPath) == 0;
  }
  if (!avi.finish(duration)) {
    output.close();
    return false;
  }
  lastStats = avi.stats();
  if (!output.close()) return false;
  struct stat existing;
  if (stat(finalPath, &existing) == 0 || rename(partialPath, finalPath) != 0)
    return false;
  snprintf(lastSaved, sizeof(lastSaved), "%s", currentName);
  Serial.printf(
      "CLIP_CLOSED file=%s frames=%lu duration_ms=%lu bytes=%lu fps=%.2f\n",
      lastSaved, static_cast<unsigned long>(lastStats.frames),
      static_cast<unsigned long>(duration),
      static_cast<unsigned long>(avi.estimatedFinalBytes()),
      lastStats.frames * 1000.0 / (duration ? duration : 1));
  return true;
}

void stopRecording() {
  if (mode != Mode::Recording) return;
  Serial.println("STOPPING finishing video and unmounting SD");
  if (!finalizeClip()) {
    failRecorder("Could not finalize the current clip");
    return;
  }
  stopCamera();
  unmountCard();
  mode = Mode::Stopped;
  Serial.println("STOPPED SAFE_TO_UNPLUG (IO2 double blink)");
  printStatus();
}

void startRecording() {
  if (mode == Mode::Recording || mode == Mode::Finalizing) return;
  mode = Mode::Starting;
  lastError[0] = '\0';
  if (!ioBuffer || !frameIndex) {
    failRecorder("PSRAM buffers unavailable");
    return;
  }
  if (!mountCard() || !findNextNumber()) {
    failRecorder("SD card unavailable or recording directory inaccessible");
    return;
  }
  if (!initCamera()) {
    failRecorder("Camera initialization failed");
    return;
  }
  lastSensorUs = 0;
  rejectedFrames = 0;
  if (!openClip()) {
    if (!output.isOpen() && lastError[0]) {
      stopCamera();
      unmountCard();
      mode = Mode::Stopped;
      printStatus();
    } else failRecorder("Cannot create a new recording");
    return;
  }
  printStatus();
}

void recordFrame() {
  uint64_t now = esp_timer_get_time();
  if (uint32_t((now - clipStartedUs) / 1000) >= CLIP_MS) {
    if (!finalizeClip()) {
      failRecorder("Clip rotation could not finalize video");
      return;
    }
    if (!openClip()) {
      if (!output.isOpen() && lastError[0]) {
        stopCamera();
        unmountCard();
        mode = Mode::Stopped;
        printStatus();
      } else failRecorder("Cannot open the next clip");
      return;
    }
    now = esp_timer_get_time();
  }
  if (now < nextCaptureUs) return;
  nextCaptureUs = now + 1000000 / TARGET_FPS;
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) {
    failRecorder("Camera capture failed");
    return;
  }
  const uint64_t sensorUs = uint64_t(frame->timestamp.tv_sec) * 1000000 +
                            frame->timestamp.tv_usec;
  if (sensorUs <= lastSensorUs) {
    esp_camera_fb_return(frame);
    ++rejectedFrames;
    return;
  }
  lastSensorUs = sensorUs;
  if (uint64_t(avi.estimatedFinalBytes()) + frame->len + FREE_RESERVE +
          2 * 1024 * 1024 >= freeAtClipStart) {
    esp_camera_fb_return(frame);
    snprintf(lastError, sizeof(lastError), "Card full; existing recordings preserved");
    stopRecording();
    return;
  }
  recorder::AppendResult result =
      frame->format == PIXFORMAT_JPEG && frame->width == VIDEO_WIDTH &&
              frame->height == VIDEO_HEIGHT
          ? avi.append(frame->buf, frame->len)
          : recorder::AppendResult::InvalidJpeg;
  if (result == recorder::AppendResult::Rotate) {
    if (!finalizeClip() || !openClip()) {
      esp_camera_fb_return(frame);
      failRecorder("Could not rotate at the clip size limit");
      return;
    }
    result = avi.append(frame->buf, frame->len);
    nextCaptureUs = esp_timer_get_time() + 1000000 / TARGET_FPS;
  }
  esp_camera_fb_return(frame);
  if (result != recorder::AppendResult::Stored) {
    // Close the usable frames when a malformed camera JPEG is encountered.
    if (result == recorder::AppendResult::InvalidJpeg) {
      ++rejectedFrames;
      if (!finalizeClip()) {
        failRecorder("Could not finalize after a camera error");
        return;
      }
    }
    failRecorder(result == recorder::AppendResult::InvalidJpeg
                     ? "Invalid camera JPEG; recording stopped"
                     : "SD video write failed");
    return;
  }
  if (uint32_t(millis() - lastSyncMs) >= 1000) {
    if (!avi.sync()) {
      failRecorder("SD flush failed");
      return;
    }
    lastSyncMs = millis();
  }
}

bool beginCardRead() {
  if (mode != Mode::Stopped && mode != Mode::Error) {
    Serial.println("ERR Stop recording before reading files");
    return false;
  }
  if (!ioBuffer) {
    Serial.println("ERR File buffer unavailable");
    return false;
  }
  if (mountCard()) return true;
  unmountCard();
  Serial.println("ERR Cannot mount card for reading");
  return false;
}

void listClips() {
  if (!beginCardRead()) return;
  File directory = SD_MMC.open(DIRECTORY);
  if (directory) {
    while (File entry = directory.openNextFile()) {
      if (!entry.isDirectory())
        Serial.printf("FILE %s %lu\n", entry.name(),
                      static_cast<unsigned long>(entry.size()));
      entry.close();
    }
    directory.close();
  }
  unmountCard();
  Serial.println("LIST_END");
}

FILE *openStoredClip(const char *name) {
  if (!name || !validName(name)) {
    Serial.println("ERR Invalid clip name");
    return nullptr;
  }
  if (!beginCardRead()) return nullptr;
  char path[96];
  snprintf(path, sizeof(path), "%s%s/%s", MOUNT, DIRECTORY, name);
  FILE *file = fopen(path, "rb");
  if (!file) {
    unmountCard();
    Serial.println("ERR Clip not found");
  }
  return file;
}

void exportClip(const char *name) {
  FILE *file = openStoredClip(name);
  if (!file) return;
  struct stat info;
  if (fstat(fileno(file), &info) != 0 || info.st_size < recorder::kHeaderBytes) {
    fclose(file);
    unmountCard();
    Serial.println("ERR Invalid file size");
    return;
  }
  Serial.printf("FILE_BEGIN %lu\n", static_cast<unsigned long>(info.st_size));
  Serial.flush();
  uint32_t crc = 0;
  size_t total = 0, count;
  while ((count = fread(ioBuffer, 1, IO_BUFFER_BYTES, file)) != 0) {
    crc = esp_rom_crc32_le(crc, ioBuffer, count);
    Serial.write(ioBuffer, count);
    total += count;
  }
  const bool okay = !ferror(file) && total == size_t(info.st_size);
  fclose(file);
  unmountCard();
  Serial.flush();
  Serial.printf("\nFILE_END %08lx %s\n", static_cast<unsigned long>(crc),
                okay ? "OK" : "ERROR");
}

bool readAt(FILE *file, uint32_t offset, void *bytes, size_t count) {
  return fseek(file, offset, SEEK_SET) == 0 &&
         fread(bytes, 1, count, file) == count;
}

void verifyClip(const char *name) {
  FILE *file = openStoredClip(name);
  if (!file) return;
  uint8_t header[recorder::kHeaderBytes], entry[16], chunk[10], end[2];
  struct stat info;
  bool okay = fstat(fileno(file), &info) == 0 &&
              readAt(file, 0, header, sizeof(header));
  uint32_t frames = 0, dataBytes = 0, indexPosition = 0, duration = 0;
  if (okay) {
    frames = recorder::get32(header + 48);
    const uint32_t moviSize = recorder::get32(header + 216);
    const uint32_t scale = recorder::get32(header + 128);
    const uint32_t rate = recorder::get32(header + 132);
    okay = !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "AVI ", 4) &&
           !memcmp(header + 108, "vidsMJPG", 8) &&
           !memcmp(header + 220, "movi", 4) && frames && frames <= 100000 &&
           moviSize >= 4 && scale && rate &&
           recorder::get32(header + 140) == frames &&
           uint64_t(recorder::get32(header + 4)) + 8 == uint64_t(info.st_size);
    if (okay) {
      dataBytes = moviSize - 4;
      indexPosition = recorder::kHeaderBytes + dataBytes;
      duration = uint64_t(scale) * frames * 1000 / rate;
      okay = uint64_t(indexPosition) + 8 + uint64_t(frames) * 16 ==
                 uint64_t(info.st_size) &&
             readAt(file, indexPosition, chunk, 8) &&
             !memcmp(chunk, "idx1", 4) && recorder::get32(chunk + 4) == frames * 16;
    }
  }
  uint32_t expectedOffset = 4;
  for (uint32_t i = 0; okay && i < frames; ++i) {
    okay = readAt(file, indexPosition + 8 + i * 16, entry, sizeof(entry));
    if (!okay) break;
    const uint32_t offset = recorder::get32(entry + 8);
    const uint32_t size = recorder::get32(entry + 12);
    okay = !memcmp(entry, "00dc", 4) && (recorder::get32(entry + 4) & 0x10) &&
           offset == expectedOffset && size >= 4 && size <= 2U * 1024U * 1024U &&
           uint64_t(recorder::kMoviOrigin) + offset + 8 + size <= indexPosition;
    if (!okay) break;
    const uint32_t position = recorder::kMoviOrigin + offset;
    okay = readAt(file, position, chunk, sizeof(chunk)) &&
           !memcmp(chunk, "00dc", 4) && recorder::get32(chunk + 4) == size &&
           chunk[8] == 0xff && chunk[9] == 0xd8 &&
           readAt(file, position + 8 + size - 2, end, sizeof(end)) &&
           end[0] == 0xff && end[1] == 0xd9;
    expectedOffset += 8 + size + (size & 1);
    delay(1);
  }
  okay = okay && recorder::kMoviOrigin + expectedOffset == indexPosition;
  fclose(file);
  unmountCard();
  Serial.printf(
      "%s {\"file\":\"%s\",\"frames\":%lu,\"duration_ms\":%lu,\"bytes\":%lu}\n",
      okay ? "VERIFIED" : "VERIFY_FAILED", name,
      static_cast<unsigned long>(frames), static_cast<unsigned long>(duration),
      okay ? static_cast<unsigned long>(info.st_size) : 0);
}

void handleCommand(char *command) {
  char *argument = strchr(command, ' ');
  if (argument) *argument++ = '\0';
  if (!strcmp(command, "STATUS")) printStatus();
  else if (!strcmp(command, "STOP")) {
    if (mode == Mode::Recording) stopRecording();
    else printStatus();
  } else if (!strcmp(command, "START")) {
    if (mode == Mode::Recording) printStatus();
    else startRecording();
  }
  else if (!strcmp(command, "LIST")) listClips();
  else if (!strcmp(command, "GET")) exportClip(argument);
  else if (!strcmp(command, "VERIFY")) verifyClip(argument);
  else Serial.println("ERR Commands: STATUS STOP START LIST GET name VERIFY name");
}

void serviceSerial() {
  static bool overflow = false;
  unsigned budget = 64;
  while (budget-- && Serial.available()) {
    const char value = Serial.read();
    if (value == '\r') continue;
    if (value == '\n') {
      if (overflow) Serial.println("ERR Command too long");
      else if (commandLength) {
        commandBuffer[commandLength] = '\0';
        handleCommand(commandBuffer);
      }
      commandLength = 0;
      overflow = false;
    } else if (!overflow) {
      if (commandLength + 1 < sizeof(commandBuffer))
        commandBuffer[commandLength++] = value;
      else overflow = true;
    }
  }
}

void updateIndicator() {
  if (mounted) return;  // Never drive the SD data pin while it belongs to SDMMC.
  const uint32_t phase = millis() % 2000;
  if (mode == Mode::Stopped)
    digitalWrite(STATUS_LED, phase < 200 || (phase >= 400 && phase < 600) ? LOW : HIGH);
  else if (mode == Mode::Error)
    digitalWrite(STATUS_LED, millis() % 250 < 125 ? LOW : HIGH);
}

void setup() {
  static_assert(RECORDER_CLIP_SECONDS > 0 && RECORDER_CLIP_SECONDS <= 600,
                "Clip duration must be 1 to 600 seconds");
  Serial.begin(115200);
  pinMode(BUTTON_GPIO, INPUT_PULLUP);
  delay(750);
  Serial.printf("RECORDER_BOOT clip_ms=%lu target_fps=%u wifi=off\n",
                static_cast<unsigned long>(CLIP_MS), TARGET_FPS);
  if (psramFound()) {
    // Keep stdio's SD transfer buffer in DMA-capable internal memory while
    // camera DMA continues filling its separate PSRAM frame buffers.
    ioBuffer = static_cast<uint8_t *>(
        heap_caps_malloc(IO_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    frameIndex = static_cast<recorder::IndexEntry *>(
        ps_malloc(INDEX_CAPACITY * sizeof(recorder::IndexEntry)));
  }
  startRecording();
}

void loop() {
  if (button.update(digitalRead(BUTTON_GPIO) == LOW, millis())) {
    Serial.println("BOOT_HOLD");
    if (mode == Mode::Recording) stopRecording();
    else if (mode == Mode::Stopped || mode == Mode::Error) startRecording();
  }
  serviceSerial();
  if (mode == Mode::Recording) {
    recordFrame();
    if (uint32_t(millis() - lastLogMs) >= 15000) {
      printStatus();
      lastLogMs = millis();
    }
  }
  updateIndicator();
  delay(1);
}
