#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>

#include "camera_server.h"
#include "wifi_credentials.h"

constexpr char HOSTNAME[] = "garage-camera";
constexpr uint32_t WIFI_RETRY_MS = 15000;
bool serverStarted = false;
bool mdnsStarted = false;

bool initCamera() {
  if (!psramFound()) {
    Serial.println("PSRAM not found. Select the ESP32 Wrover Module board.");
    return false;
  }

  // Freenove ESP32-WROVER / OV2640 pin mapping.
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
  // Allocate at the largest resolution so later changes fit the same buffers.
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    Serial.printf("Camera init failed: %s (0x%x)\n", esp_err_to_name(result), result);
    return false;
  }
  Serial.printf("Camera ready: 1600x1200 JPEG, compression 12, PSRAM %u bytes\n",
                ESP.getPsramSize());
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Garage Camera: local streaming ===");
  if (!initCamera()) {
    // Leave the diagnostic visible instead of repeatedly resetting the board.
    while (true) delay(1000);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Continuous streaming deliberately trades battery life for throughput.
  WiFi.setSleep(false);
  Serial.println("Connecting to Wi-Fi...");
}

void loop() {
  static bool wasConnected = false;
  static uint32_t lastAttempt = millis();
  const bool connected = WiFi.status() == WL_CONNECTED;

  if (connected) {
    if (!wasConnected) {
      Serial.printf("Camera page: http://%s/\n", WiFi.localIP().toString().c_str());
      Serial.printf("Wi-Fi signal: %d dBm\n", WiFi.RSSI());
      lastAttempt = millis();
    }
    if (!serverStarted) {
      serverStarted = startCameraServer();
      if (!serverStarted) {
        Serial.println("HTTP server failed; retrying in 5 seconds.");
        delay(5000);
      }
    }
    if (!mdnsStarted && MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
      Serial.printf("Local name: http://%s.local/\n", HOSTNAME);
    }
  } else {
    if (wasConnected) {
      Serial.println("Wi-Fi disconnected; reconnecting...");
      if (mdnsStarted) MDNS.end();
      mdnsStarted = false;
    }
    if (millis() - lastAttempt >= WIFI_RETRY_MS) {
      Serial.println("Still waiting for Wi-Fi; retrying...");
      WiFi.reconnect();
      lastAttempt = millis();
    }
  }

  wasConnected = connected;
  delay(250);
}
