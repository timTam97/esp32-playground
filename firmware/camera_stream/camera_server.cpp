#include "camera_server.h"
#include <Arduino.h>
#include "camera_page.h"
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

namespace {

struct Resolution {
  const char* name;
  framesize_t size;
  uint16_t width;
  uint16_t height;
};

constexpr Resolution RESOLUTIONS[] = {
  {"UXGA", FRAMESIZE_UXGA, 1600, 1200},
  {"SXGA", FRAMESIZE_SXGA, 1280, 1024},
  {"XGA", FRAMESIZE_XGA, 1024, 768},
  {"SVGA", FRAMESIZE_SVGA, 800, 600},
  {"VGA", FRAMESIZE_VGA, 640, 480},
  {"QVGA", FRAMESIZE_QVGA, 320, 240},
};
constexpr char BOUNDARY[] = "garage-camera-frame";
constexpr char STREAM_TYPE[] = "multipart/x-mixed-replace;boundary=garage-camera-frame";

struct StreamStats {
  bool active = false;
  bool stop = false;
  char viewer[33] = {};
  uint32_t frames = 0;
  uint64_t bytes = 0;
  uint32_t captureErrors = 0;
  int64_t startedUs = 0;
  int64_t lastFrameUs = 0;
  double fps = 0;
  double mbps = 0;
  double captureMs = 0;
  double sendMs = 0;
  uint32_t frameBytes = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

httpd_handle_t server = nullptr;
SemaphoreHandle_t cameraMutex = nullptr;
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
StreamStats stats;
// These settings are accessed only by the HTTP server task.
const Resolution* selectedResolution = &RESOLUTIONS[0];
int selectedQuality = 12;

StreamStats readStats() {
  portENTER_CRITICAL(&statsMux);
  StreamStats copy = stats;
  portEXIT_CRITICAL(&statsMux);
  return copy;
}

esp_err_t json(httpd_req_t* req, const char* body, const char* status = "200 OK") {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

bool viewerId(httpd_req_t* req, char (&viewer)[33]) {
  char query[96];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "viewer", viewer, sizeof(viewer)) != ESP_OK ||
      viewer[0] == '\0') return false;
  for (const char* c = viewer; *c; ++c) {
    if (!isxdigit(static_cast<unsigned char>(*c))) return false;
  }
  return true;
}

esp_err_t pageHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  return httpd_resp_send(req, CAMERA_PAGE, sizeof(CAMERA_PAGE) - 1);
}

esp_err_t statusHandler(httpd_req_t* req) {
  const StreamStats snapshot = readStats();
  const sensor_t* sensor = esp_camera_sensor_get();
  const int64_t now = esp_timer_get_time();
  const int64_t ageMs = snapshot.lastFrameUs ? (now - snapshot.lastFrameUs) / 1000 : -1;
  const bool flowing = snapshot.active && ageMs >= 0 && ageMs < 3000;
  char response[1024];
  snprintf(response, sizeof(response),
           "{\"resolution\":\"%s\",\"width\":%u,\"height\":%u,\"quality\":%d,"
           "\"streaming\":%s,\"viewer\":\"%s\",\"fps\":%.2f,\"mbps\":%.2f,"
           "\"capture_ms\":%.2f,\"send_ms\":%.2f,"
           "\"frames\":%lu,\"bytes\":%llu,\"frame_bytes\":%lu,"
           "\"frame_width\":%u,\"frame_height\":%u,\"last_frame_ms\":%lld,"
           "\"capture_errors\":%lu,\"rssi\":%d,\"free_heap\":%u,"
           "\"free_psram\":%u,\"sensor_pid\":%u,\"xclk_mhz\":%u,"
           "\"tcp_send_buffer\":%u,\"uptime_s\":%llu}",
           selectedResolution->name, selectedResolution->width, selectedResolution->height,
           selectedQuality, snapshot.active ? "true" : "false", snapshot.viewer,
           flowing ? snapshot.fps : 0, flowing ? snapshot.mbps : 0,
           snapshot.captureMs, snapshot.sendMs,
           static_cast<unsigned long>(snapshot.frames),
           static_cast<unsigned long long>(snapshot.bytes),
           static_cast<unsigned long>(snapshot.frameBytes), snapshot.width, snapshot.height,
           static_cast<long long>(ageMs), static_cast<unsigned long>(snapshot.captureErrors),
           WiFi.RSSI(), ESP.getFreeHeap(), ESP.getFreePsram(),
           sensor ? sensor->id.PID : 0, sensor ? sensor->xclk_freq_hz / 1000000 : 0,
           static_cast<unsigned>(CAMERA_TCP_SEND_BUFFER_BYTES),
           static_cast<unsigned long long>(now / 1000000));
  return json(req, response);
}

esp_err_t settingsHandler(httpd_req_t* req) {
  char body[96] = {};
  if (req->content_len == 0 || req->content_len >= sizeof(body)) {
    return json(req, "{\"error\":\"Invalid settings body\"}", "400 Bad Request");
  }
  size_t received = 0;
  while (received < req->content_len) {
    const int count = httpd_req_recv(req, body + received, req->content_len - received);
    if (count <= 0) return json(req, "{\"error\":\"Settings request timed out\"}", "408 Request Timeout");
    received += count;
  }
  char resolutionName[12], qualityText[8];
  if (httpd_query_key_value(body, "resolution", resolutionName, sizeof(resolutionName)) != ESP_OK ||
      httpd_query_key_value(body, "quality", qualityText, sizeof(qualityText)) != ESP_OK) {
    return json(req, "{\"error\":\"Provide resolution and quality\"}", "400 Bad Request");
  }
  const Resolution* requested = nullptr;
  for (const auto& option : RESOLUTIONS) {
    if (strcmp(option.name, resolutionName) == 0) requested = &option;
  }
  char* end = nullptr;
  const long quality = strtol(qualityText, &end, 10);
  if (!requested || end == qualityText || *end != '\0' || quality < 10 || quality > 40) {
    return json(req, "{\"error\":\"Unsupported resolution or compression (10–40)\"}", "400 Bad Request");
  }
  // The frame remains owned until its JPEG has been sent. Never change the
  // sensor while that buffer is being captured, sent, or returned.
  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    return json(req, "{\"error\":\"Camera busy; pause the stream and retry\"}", "503 Service Unavailable");
  }
  sensor_t* sensor = esp_camera_sensor_get();
  const bool changedResolution = requested != selectedResolution;
  const bool changedQuality = quality != selectedQuality;
  bool ok = true;
  if (changedResolution) ok = sensor->set_framesize(sensor, requested->size) == 0;
  if (ok && changedQuality) ok = sensor->set_quality(sensor, quality) == 0;
  if (!ok) {
    sensor->set_framesize(sensor, selectedResolution->size);
    sensor->set_quality(sensor, selectedQuality);
  } else {
    selectedResolution = requested;
    selectedQuality = quality;
  }
  // Two buffers can still contain frames from the old sensor configuration.
  if (changedResolution || changedQuality) {
    for (int i = 0; i < 2; ++i) {
      camera_fb_t* stale = esp_camera_fb_get();
      if (stale) esp_camera_fb_return(stale);
    }
  }
  xSemaphoreGive(cameraMutex);
  if (!ok) return json(req, "{\"error\":\"Camera rejected these settings\"}", "500 Internal Server Error");
  return statusHandler(req);
}

esp_err_t stopHandler(httpd_req_t* req) {
  char viewer[33];
  if (!viewerId(req, viewer)) {
    return json(req, "{\"error\":\"Missing viewer ID\"}", "400 Bad Request");
  }
  // Closing one page must not stop a different page's viewing session.
  portENTER_CRITICAL(&statsMux);
  if (stats.active && strcmp(stats.viewer, viewer) == 0) stats.stop = true;
  portEXIT_CRITICAL(&statsMux);
  return json(req, "{\"ok\":true}");
}

void streamTask(void* context) {
  auto* req = static_cast<httpd_req_t*>(context);
  const int socket = httpd_req_to_sockfd(req);
  const httpd_handle_t handle = req->handle;
  httpd_resp_set_type(req, STREAM_TYPE);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  // End the session after streaming; never reuse a partially sent response.
  httpd_resp_set_hdr(req, "Connection", "close");
  uint32_t windowFrames = 0;
  uint64_t windowBytes = 0;
  int64_t windowCaptureUs = 0, windowSendUs = 0;
  int64_t windowStart = esp_timer_get_time();
  esp_err_t result = ESP_OK;

  while (!readStats().stop && WiFi.status() == WL_CONNECTED) {
    const int64_t captureStart = esp_timer_get_time();
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) break;
    camera_fb_t* frame = esp_camera_fb_get();
    if (!frame || frame->format != PIXFORMAT_JPEG) {
      if (frame) esp_camera_fb_return(frame);
      xSemaphoreGive(cameraMutex);
      portENTER_CRITICAL(&statsMux);
      ++stats.captureErrors;
      portEXIT_CRITICAL(&statsMux);
      result = ESP_FAIL;
      break;
    }
    const size_t length = frame->len;
    const int64_t capturedAt = esp_timer_get_time();
    const uint16_t width = frame->width, height = frame->height;
    char header[180];
    const int headerLength = snprintf(
      header, sizeof(header),
      "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n"
      "X-Timestamp: %lld.%06ld\r\n\r\n", BOUNDARY, static_cast<unsigned>(length),
      static_cast<long long>(frame->timestamp.tv_sec), static_cast<long>(frame->timestamp.tv_usec));
    result = httpd_resp_send_chunk(req, header, headerLength);
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(frame->buf), length);
    }
    esp_camera_fb_return(frame);
    xSemaphoreGive(cameraMutex);
    if (result != ESP_OK) break;

    const int64_t now = esp_timer_get_time();
    ++windowFrames;
    windowBytes += length;
    windowCaptureUs += capturedAt - captureStart;
    windowSendUs += now - capturedAt;
    const int64_t elapsed = now - windowStart;
    portENTER_CRITICAL(&statsMux);
    ++stats.frames;
    stats.bytes += length;
    stats.lastFrameUs = now;
    stats.frameBytes = length;
    stats.width = width;
    stats.height = height;
    if (elapsed >= 1000000) {
      stats.fps = windowFrames * 1000000.0 / elapsed;
      stats.mbps = windowBytes * 8.0 / elapsed;
      stats.captureMs = windowCaptureUs / (windowFrames * 1000.0);
      stats.sendMs = windowSendUs / (windowFrames * 1000.0);
    }
    portEXIT_CRITICAL(&statsMux);
    if (elapsed >= 1000000) {
      windowFrames = 0;
      windowBytes = 0;
      windowCaptureUs = windowSendUs = 0;
      windowStart = now;
    }
    // Let idle/network tasks run even at a small resolution.
    vTaskDelay(1);
  }
  if (result == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
  httpd_req_async_handler_complete(req);
  httpd_sess_trigger_close(handle, socket);
  portENTER_CRITICAL(&statsMux);
  stats.active = false;
  stats.fps = 0;
  stats.mbps = 0;
  portEXIT_CRITICAL(&statsMux);
  Serial.printf("Stream ended (%s)\n", esp_err_to_name(result));
  vTaskDelete(nullptr);
}

esp_err_t streamHandler(httpd_req_t* req) {
  char viewer[33];
  if (!viewerId(req, viewer)) {
    return json(req, "{\"error\":\"Supply a hexadecimal viewer ID\"}", "400 Bad Request");
  }
  portENTER_CRITICAL(&statsMux);
  const bool busy = stats.active;
  if (!busy) {
    stats = StreamStats{};
    stats.active = true;
    stats.startedUs = esp_timer_get_time();
    strlcpy(stats.viewer, viewer, sizeof(stats.viewer));
  }
  portEXIT_CRITICAL(&statsMux);
  if (busy) return json(req, "{\"error\":\"One viewer at a time; close the other stream\"}", "503 Service Unavailable");

  httpd_req_t* asyncRequest = nullptr;
  esp_err_t result = httpd_req_async_handler_begin(req, &asyncRequest);
  if (result == ESP_OK &&
      xTaskCreate(streamTask, "camera-stream", 6144, asyncRequest, 4, nullptr) == pdPASS) {
    return ESP_OK;
  }
  if (asyncRequest) {
    json(asyncRequest, "{\"error\":\"Could not start stream\"}", "503 Service Unavailable");
    httpd_req_async_handler_complete(asyncRequest);
  } else {
    json(req, "{\"error\":\"Could not start stream\"}", "503 Service Unavailable");
  }
  portENTER_CRITICAL(&statsMux);
  stats.active = false;
  portEXIT_CRITICAL(&statsMux);
  return ESP_OK;
}

}  // namespace

bool startCameraServer() {
  if (server) return true;
  if (!cameraMutex) cameraMutex = xSemaphoreCreateMutex();
  if (!cameraMutex) return false;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 6144;
  config.max_open_sockets = 5;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 3;
  config.send_wait_timeout = 3;
  if (httpd_start(&server, &config) != ESP_OK) {
    server = nullptr;
    return false;
  }
  struct Route {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
  };
  const Route routes[] = {
    {"/", HTTP_GET, pageHandler},
    {"/status", HTTP_GET, statusHandler},
    {"/settings", HTTP_POST, settingsHandler},
    {"/stream", HTTP_GET, streamHandler},
    {"/stop", HTTP_POST, stopHandler},
  };
  for (const auto& route : routes) {
    httpd_uri_t uri = {};
    uri.uri = route.uri;
    uri.method = route.method;
    uri.handler = route.handler;
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
      httpd_stop(server);
      server = nullptr;
      return false;
    }
  }
  Serial.println("HTTP camera server ready on port 80.");
  return true;
}
