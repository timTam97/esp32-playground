#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>

#include "secrets.h"

// ===== CONFIGURATION =====
// Deep sleep: 5 minutes
#define SLEEP_DURATION_US (5 * 60 * 1000000ULL)

// ===== Freenove ESP32-WROVER camera pins =====
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    21
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      19
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM       5
#define Y2_GPIO_NUM       4
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

void goToSleep() {
  Serial.println("Sleeping for 5 minutes...");
  WiFi.disconnect(true);
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
  esp_deep_sleep_start();
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;  // 640x480
  config.jpeg_quality = 12;
  config.fb_count     = 1;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }
  return true;
}

bool connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed");
    return false;
  }

  Serial.printf("Connected, IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Garage Monitor ===");

  if (!initCamera()) {
    goToSleep();
    return;
  }

  if (!connectWiFi()) {
    goToSleep();
    return;
  }

  // Capture image
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    goToSleep();
    return;
  }
  Serial.printf("Captured: %d bytes\n", fb->len);

  // POST to Lambda
  HTTPClient http;
  http.begin(LAMBDA_URL);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(15000);

  int httpCode = http.POST(fb->buf, fb->len);

  if (httpCode > 0) {
    Serial.printf("HTTP %d: %s\n", httpCode, http.getString().c_str());
  } else {
    Serial.printf("HTTP error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  esp_camera_fb_return(fb);

  goToSleep();
}

void loop() {
  // Never reached — deep sleep resets the chip
}
