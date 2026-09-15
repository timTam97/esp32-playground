#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_camera.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "esp_vfs_dev.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "jpeg_wire.h"

// Lab firmware reuses the already ignored local configuration.
// No credentials are present in tracked source or serial output.
#include "../../camera_stream/wifi_credentials.h"

namespace {
constexpr const char* tag = "garage_webrtc";
constexpr int connected_bit = BIT0;
constexpr int worker_stopped_bit = BIT1;
constexpr size_t max_command = 16384;
constexpr uint16_t no_channel = UINT16_MAX;
EventGroupHandle_t network;
QueueHandle_t commands;
esp_peer_handle_t peer = nullptr;
std::atomic<uint32_t> generation{0};
char session[65] = {};
std::atomic<bool> connected{false}, running{false}, close_requested{false}, worker_running{false};
std::atomic<uint16_t> jpeg_channel{no_channel}, control_channel{no_channel};
std::atomic<uint32_t> worker_loops{0};
int requested_fps = 2, quality = 12, pending_quality = 12;
uint64_t frames_sent = 0, bytes_sent = 0, frames_dropped = 0, send_errors = 0;
uint64_t send_time_us = 0, capture_time_us = 0;
camera_fb_t* current_frame = nullptr;
uint32_t frame_id = 0;
size_t frame_offset = 0;
int64_t frame_started = 0, next_frame = 0, next_chunk = 0, last_stats = 0;
uint8_t* chunk = nullptr;
struct Command { cJSON* json; uint32_t generation; bool from_control; };

void emit(cJSON* value) {
    if (!value) return;
    cJSON_AddStringToObject(value, "session", session);
    char* text = cJSON_PrintUnformatted(value);
    if (text) {
        flockfile(stdout);
        printf("@signal %s\n", text);
        fflush(stdout);
        funlockfile(stdout);
        free(text);
    }
    cJSON_Delete(value);
}

cJSON* message(const char* type) {
    cJSON* result = cJSON_CreateObject();
    if (result) cJSON_AddStringToObject(result, "type", type);
    return result;
}

void control(cJSON* value) {
    if (!value) return;
    if (peer && control_channel != no_channel) {
        char* text = cJSON_PrintUnformatted(value);
        if (text) {
            esp_peer_data_frame_t frame = {};
            frame.type = ESP_PEER_DATA_CHANNEL_STRING;
            frame.stream_id = control_channel;
            frame.data = reinterpret_cast<uint8_t*>(text);
            frame.size = strlen(text);
            esp_peer_send_data(peer, &frame);
            free(text);
        }
    }
    emit(value);
}

void error(const char* detail) {
    cJSON* value = message("error");
    cJSON_AddStringToObject(value, "message", detail);
    control(value);
}

void release_frame(bool dropped) {
    if (current_frame) {
        esp_camera_fb_return(current_frame);
        current_frame = nullptr;
        if (dropped) frames_dropped++;
    }
    frame_offset = 0;
}

void stats() {
    wifi_ap_record_t ap = {};
    esp_wifi_sta_get_ap_info(&ap);
    cJSON* value = message("stats");
    cJSON_AddBoolToObject(value, "connected", connected);
    cJSON_AddBoolToObject(value, "running", running);
    cJSON_AddNumberToObject(value, "width", 1600);
    cJSON_AddNumberToObject(value, "height", 1200);
    cJSON_AddNumberToObject(value, "quality", quality);
    cJSON_AddNumberToObject(value, "requested_fps", requested_fps);
    cJSON_AddNumberToObject(value, "frames_sent", frames_sent);
    cJSON_AddNumberToObject(value, "bytes_sent", bytes_sent);
    cJSON_AddNumberToObject(value, "frames_dropped", frames_dropped);
    cJSON_AddNumberToObject(value, "send_errors", send_errors);
    cJSON_AddNumberToObject(value, "send_time_ms", send_time_us / 1000);
    cJSON_AddNumberToObject(value, "capture_time_ms", capture_time_us / 1000);
    cJSON_AddNumberToObject(value, "worker_loops", worker_loops);
    cJSON_AddNumberToObject(value, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(value, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(value, "largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(value, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(value, "rssi", ap.rssi);
    cJSON_AddNumberToObject(value, "uptime_ms", esp_timer_get_time() / 1000);
    control(value);
}

int on_state(esp_peer_state_t state, void*) {
    if (state == ESP_PEER_STATE_CONNECTED) connected = true;
    if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CONNECT_FAILED ||
        state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED) {
        connected = running = false;
        close_requested = true;
    }
    cJSON* value = message("state");
    cJSON_AddNumberToObject(value, "state", state);
    emit(value);
    return 0;
}

int on_message(esp_peer_msg_t* info, void*) {
    if (info->type != ESP_PEER_MSG_TYPE_SDP && info->type != ESP_PEER_MSG_TYPE_CANDIDATE) return 0;
    char* data = static_cast<char*>(malloc(info->size + 1));
    if (!data) return -1;
    memcpy(data, info->data, info->size);
    data[info->size] = 0;
    cJSON* value = message(info->type == ESP_PEER_MSG_TYPE_SDP ? "answer" : "candidate");
    cJSON_AddStringToObject(value, info->type == ESP_PEER_MSG_TYPE_SDP ? "sdp" : "candidate", data);
    free(data);
    emit(value);
    return 0;
}

int on_channel(esp_peer_data_channel_info_t* info, void*) {
    if (strcmp(info->label, "jpeg") == 0) jpeg_channel = info->stream_id;
    if (strcmp(info->label, "control") == 0) control_channel = info->stream_id;
    cJSON* value = message("channel");
    cJSON_AddStringToObject(value, "label", info->label);
    emit(value);
    return 0;
}

int on_channel_close(esp_peer_data_channel_info_t* info, void*) {
    if (info->stream_id == jpeg_channel) { jpeg_channel = no_channel; running = false; }
    if (info->stream_id == control_channel) { control_channel = no_channel; running = false; }
    return 0;
}

int on_data(esp_peer_data_frame_t* data, void*) {
    if (data->stream_id != control_channel || data->size <= 0 || data->size > 2048) return -1;
    cJSON* value = cJSON_ParseWithLength(reinterpret_cast<const char*>(data->data), data->size);
    if (!value) return -1;
    Command command = {value, generation, true};
    if (xQueueSend(commands, &command, 0) != pdTRUE) {
        cJSON_Delete(value);
        return -1;
    }
    return 0;
}

void close_peer() {
    running = connected = false;
    release_frame(true);
    generation++;
    if (peer) {
        worker_running = false;
        if (!(xEventGroupWaitBits(network, worker_stopped_bit, pdFALSE, pdTRUE,
                                 pdMS_TO_TICKS(10000)) & worker_stopped_bit)) {
            error("Peer receive task did not stop; restart required");
            close_requested = false;
            return;
        }
        esp_peer_close(peer);
        peer = nullptr;
    }
    jpeg_channel = control_channel = no_channel;
    close_requested = false;
    emit(message("closed"));
    session[0] = 0;
}

void peer_worker(void*) {
    while (worker_running) {
        esp_peer_main_loop(peer);
        worker_loops++;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    xEventGroupSetBits(network, worker_stopped_bit);
    vTaskDelete(nullptr);
}

bool open_peer(const char* new_session) {
    strlcpy(session, new_session, sizeof(session));
    generation++;
    esp_peer_default_cfg_t options = {};
    options.agent_recv_timeout = 5;
    options.keep_role = true;
    options.max_candidates = 8;
    options.data_ch_cfg.cache_timeout = 1000;
    options.data_ch_cfg.send_cache_size = 128 * 1024;
    options.data_ch_cfg.recv_cache_size = 16 * 1024;
    esp_peer_cfg_t config = {};
    config.role = ESP_PEER_ROLE_CONTROLLED;
    config.enable_data_channel = true;
    config.manual_ch_create = true;
    config.no_auto_reconnect = true;
    config.extra_cfg = &options;
    config.extra_size = sizeof(options);
    config.on_state = on_state;
    config.on_msg = on_message;
    config.on_channel_open = on_channel;
    config.on_channel_close = on_channel_close;
    config.on_data = on_data;
    const int result = esp_peer_open(&config, esp_peer_get_default_impl(), &peer);
    if (result != ESP_PEER_ERR_NONE) {
        peer = nullptr;
        error("Could not allocate peer");
        session[0] = 0;
        return false;
    }
    frames_sent = bytes_sent = frames_dropped = send_errors = 0;
    send_time_us = capture_time_us = 0;
    worker_loops = 0;
    // Opening allocates the peer; new_connection starts ICE gathering even
    // when this device is the answerer. Without it an offer is only parsed.
    if (esp_peer_new_connection(peer) != ESP_PEER_ERR_NONE) {
        xEventGroupSetBits(network, worker_stopped_bit);
        close_requested = true;
        return false;
    }
    xEventGroupClearBits(network, worker_stopped_bit);
    worker_running = true;
    if (xTaskCreatePinnedToCore(peer_worker, "peer_receive", 8192, nullptr, 5,
                               nullptr, 0) != pdPASS) {
        worker_running = false;
        xEventGroupSetBits(network, worker_stopped_bit);
        close_requested = true;
        return false;
    }
    return true;
}

const char* string(cJSON* value, const char* name) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(value, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

void handle(Command command) {
    cJSON* value = command.json;
    if (command.from_control && command.generation != generation) return;
    const char* type = string(value, "type");
    const char* incoming_session = string(value, "session");
    if (strcmp(type, "offer") == 0 && !command.from_control) {
        if (peer) { error("Camera busy"); return; }
        const char* sdp = string(value, "sdp");
        if (strlen(incoming_session) == 0 || strlen(incoming_session) > 64 ||
            strlen(sdp) == 0 || strlen(sdp) > 12000) {
            error("Invalid offer"); return;
        }
        if (!(xEventGroupGetBits(network) & connected_bit)) { error("Wi-Fi unavailable"); return; }
        if (open_peer(incoming_session)) {
            esp_peer_msg_t offer = {};
            offer.type = ESP_PEER_MSG_TYPE_SDP;
            offer.data = reinterpret_cast<uint8_t*>(const_cast<char*>(sdp));
            offer.size = strlen(sdp);
            if (esp_peer_send_msg(peer, &offer) != ESP_PEER_ERR_NONE) close_requested = true;
        }
        return;
    }
    if (!command.from_control && strcmp(type, "status") != 0 &&
        strcmp(incoming_session, session) != 0) return;
    if (strcmp(type, "candidate") == 0 && peer && !command.from_control) {
        const char* candidate = string(value, "candidate");
        esp_peer_msg_t info = {};
        info.type = ESP_PEER_MSG_TYPE_CANDIDATE;
        info.data = reinterpret_cast<uint8_t*>(const_cast<char*>(candidate));
        info.size = strlen(candidate);
        if (info.size > 0 && info.size < 2048) esp_peer_send_msg(peer, &info);
    } else if (strcmp(type, "close") == 0) {
        close_requested = true;
    } else if (strcmp(type, "start") == 0) {
        if (!connected || jpeg_channel == no_channel || control_channel == no_channel) {
            error("Data channels are not ready"); return;
        }
        running = true;
        next_frame = next_chunk = esp_timer_get_time();
        stats();
    } else if (strcmp(type, "stop") == 0) {
        running = false;
        release_frame(true);
        stats();
    } else if (strcmp(type, "settings") == 0) {
        cJSON* fps = cJSON_GetObjectItemCaseSensitive(value, "fps");
        cJSON* jpeg_quality = cJSON_GetObjectItemCaseSensitive(value, "quality");
        if (!cJSON_IsNumber(fps) || !cJSON_IsNumber(jpeg_quality) ||
            !std::isfinite(fps->valuedouble) || !std::isfinite(jpeg_quality->valuedouble) ||
            fps->valuedouble != fps->valueint || jpeg_quality->valuedouble != jpeg_quality->valueint ||
            fps->valueint < 2 || fps->valueint > 5 || jpeg_quality->valueint < 10 || jpeg_quality->valueint > 40) {
            error("Use 2–5 fps and JPEG quality 10–40"); return;
        }
        requested_fps = fps->valueint;
        pending_quality = jpeg_quality->valueint;
    } else if (strcmp(type, "ping") == 0) {
        cJSON* pong = message("pong");
        cJSON* sent = cJSON_GetObjectItemCaseSensitive(value, "sent");
        if (cJSON_IsNumber(sent)) cJSON_AddNumberToObject(pong, "sent", sent->valuedouble);
        cJSON_AddNumberToObject(pong, "device_ms", esp_timer_get_time() / 1000);
        control(pong);
    } else if (strcmp(type, "status") == 0) stats();
}

void send_next_chunk() {
    if (!peer || !running || jpeg_channel == no_channel) return;
    const int64_t now = esp_timer_get_time();
    if (!current_frame) {
        if (now < next_frame || now < next_chunk) return;
        if (quality != pending_quality) {
            sensor_t* sensor = esp_camera_sensor_get();
            if (sensor->set_quality(sensor, pending_quality) == 0) quality = pending_quality;
            // Discard two frames buffered at the former compression level.
            for (int i = 0; i < 2; ++i) {
                camera_fb_t* old = esp_camera_fb_get();
                if (old) esp_camera_fb_return(old);
            }
        }
        const int64_t capture_start = esp_timer_get_time();
        current_frame = esp_camera_fb_get();
        capture_time_us += esp_timer_get_time() - capture_start;
        frame_started = esp_timer_get_time();
        next_frame = frame_started + 1000000 / requested_fps;
        if (!current_frame || current_frame->format != PIXFORMAT_JPEG ||
            current_frame->width != 1600 || current_frame->height != 1200 ||
            current_frame->len == 0 || current_frame->len > jpeg_wire::maximum_frame) {
            release_frame(true);
            return;
        }
        frame_id++;
        frame_offset = 0;
    }
    if (now - frame_started > 1000000) { release_frame(true); return; }
    if (now < next_chunk) return;
    const size_t payload = std::min(jpeg_wire::chunk_size, current_frame->len - frame_offset);
    const uint64_t captured_ms = uint64_t(current_frame->timestamp.tv_sec) * 1000 +
                                 current_frame->timestamp.tv_usec / 1000;
    jpeg_wire::header(chunk, frame_id, captured_ms, current_frame->len,
                      frame_offset / jpeg_wire::chunk_size, payload);
    memcpy(chunk + jpeg_wire::header_size, current_frame->buf + frame_offset, payload);
    esp_peer_data_frame_t frame = {};
    frame.type = ESP_PEER_DATA_CHANNEL_DATA;
    frame.stream_id = jpeg_channel;
    frame.data = chunk;
    frame.size = jpeg_wire::header_size + payload;
    const int64_t send_start = esp_timer_get_time();
    const int result = esp_peer_send_data(peer, &frame);
    send_time_us += esp_timer_get_time() - send_start;
    if (result == ESP_PEER_ERR_WOULD_BLOCK || result == ESP_PEER_ERR_OVER_LIMITED) {
        next_chunk = esp_timer_get_time() + 5000;
        return;
    }
    if (result != ESP_PEER_ERR_NONE) {
        send_errors++;
        release_frame(true);
        return;
    }
    frame_offset += payload;
    bytes_sent += payload;
    // 250,000 JPEG bytes/sec = 2 Mbps. Pace chunks, not just whole frames.
    // Encryption/transmission time already consumes the byte budget.
    // Adding a fresh delay after send_data would throttle the same bytes twice.
    next_chunk = send_start + payload * 4;
    if (frame_offset == current_frame->len) {
        frames_sent++;
        release_frame(false);
    }
}

void serial_task(void*) {
    char* line = static_cast<char*>(malloc(max_command + 1));
    if (!line) abort();
    size_t used = 0;
    bool overflow = false;
    uint8_t input[256];
    while (true) {
        const int count = uart_read_bytes(UART_NUM_0, input, sizeof(input), pdMS_TO_TICKS(100));
        for (int i = 0; i < count; ++i) {
            const char c = input[i];
            if (c == '\n') {
                if (!overflow) {
                    line[used] = 0;
                    cJSON* value = cJSON_ParseWithLength(line, used);
                    if (value) {
                        Command command = {value, 0, false};
                        if (xQueueSend(commands, &command, pdMS_TO_TICKS(50)) != pdTRUE) cJSON_Delete(value);
                    }
                }
                used = 0; overflow = false;
            } else if (c != '\r' && !overflow) {
                if (used == max_command) overflow = true;
                else line[used++] = c;
            }
        }
    }
}

void wifi_event(void*, esp_event_base_t base, int32_t event, void*) {
    if (base == WIFI_EVENT && event == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == WIFI_EVENT && event == WIFI_EVENT_STA_DISCONNECTED) xEventGroupClearBits(network, connected_bit);
    if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(network, connected_bit);
        emit(message("wifi_ready"));
    }
}

void init_camera() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
    c.pin_d0 = 4; c.pin_d1 = 5; c.pin_d2 = 18; c.pin_d3 = 19;
    c.pin_d4 = 36; c.pin_d5 = 39; c.pin_d6 = 34; c.pin_d7 = 35;
    c.pin_xclk = 21; c.pin_pclk = 22; c.pin_vsync = 25; c.pin_href = 23;
    c.pin_sccb_sda = 26; c.pin_sccb_scl = 27; c.pin_pwdn = -1; c.pin_reset = -1;
    c.xclk_freq_hz = 20000000; c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size = FRAMESIZE_UXGA; c.jpeg_quality = quality; c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM; c.grab_mode = CAMERA_GRAB_LATEST;
    ESP_ERROR_CHECK(esp_camera_init(&c));
}
}

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    network = xEventGroupCreate();
    commands = xQueueCreate(8, sizeof(Command));
    chunk = static_cast<uint8_t*>(malloc(jpeg_wire::header_size + jpeg_wire::chunk_size));
    if (!network || !commands || !chunk) abort();
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 8192, 0, 0, nullptr, 0));
    esp_vfs_dev_uart_use_driver(UART_NUM_0);
    if (xTaskCreate(serial_task, "serial_signal", 4096, nullptr, 3, nullptr) != pdPASS) abort();
    init_camera();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    wifi_config_t wifi = {};
    strlcpy(reinterpret_cast<char*>(wifi.sta.ssid), WIFI_SSID, sizeof(wifi.sta.ssid));
    strlcpy(reinterpret_cast<char*>(wifi.sta.password), WIFI_PASSWORD, sizeof(wifi.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(tag, "2 MP JPEG data-channel experiment; no AWS connection in this build");
    emit(message("ready"));
    int64_t last_wifi_attempt = esp_timer_get_time();
    while (true) {
        Command command;
        while (xQueueReceive(commands, &command, 0) == pdTRUE) {
            handle(command);
            cJSON_Delete(command.json);
        }
        const int64_t now = esp_timer_get_time();
        if (!(xEventGroupGetBits(network) & connected_bit)) {
            if (peer) close_requested = true;
            if (now - last_wifi_attempt > 15000000) {
                esp_wifi_connect();
                last_wifi_attempt = now;
            }
        }
        if (close_requested) close_peer();
        if (peer && !close_requested) send_next_chunk();
        if (now - last_stats > 2000000) { stats(); last_stats = now; }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
