#include "my_home_device.h"
#include "ha_config.h"
#include <mcp_server.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <esp_log.h>
#include <string.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "board.h"
#include "system_info.h"
#include <network_interface.h>
#include <stdexcept>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_jpeg_dec.h>
#include "application.h"
#include "display/lvgl_display/lvgl_display.h"
#include "display/lvgl_display/lvgl_image.h"
#include "assets/lang_config.h"
#include <esp_websocket_client.h>
#include <mbedtls/base64.h>
#include <opus_encoder.h>

#define TAG "HomeDevice"

// HTTP 超时标准（毫秒）
static constexpr int HTTP_TIMEOUT_LOCAL_MS  = 5000;   // 本地 HA API 查询/控制
static constexpr int HTTP_TIMEOUT_MEDIA_MS  = 8000;   // 本地媒体下载（JPEG 摄像头）
static constexpr int HTTP_TIMEOUT_REMOTE_MS = 10000;  // 外网服务（音频上传/下载）

// AI 视觉服务器（从服务端 capabilities 里获取，由 McpServer::ParseCapabilities 注入）
static std::string s_vision_url;
static std::string s_vision_token;

// 前向声明（实现在文件末尾）
static bool DownloadJpegFromHA(std::string& jpeg_data, const char* url, const std::string& token = "");
static void ShowJpegOnDisplay(const std::string& jpeg_data);

void SetVisionUrl(const std::string& url, const std::string& token) {
    s_vision_url = url;
    s_vision_token = token;
    ESP_LOGI(TAG, "Vision URL set: %s", url.c_str());
}


// =================================================================================
// Part 1: Home Assistant 基础功能 (HTTP Helper)
// =================================================================================

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (evt->user_data) {
                    std::string* response = (std::string*)evt->user_data;
                    response->append((char*)evt->data, evt->data_len);
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

std::string MyHomeDevice::GetEntityState(const char* base_url, const char* token, const char* entity_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/states/%s", base_url, entity_id);

    ESP_LOGI(TAG, "Querying State: %s", url);

    std::string response_buffer;

    esp_http_client_config_t config = {};
    config.url                = url;
    config.method             = HTTP_METHOD_GET;
    config.timeout_ms         = 5000;
    config.crt_bundle_attach  = esp_crt_bundle_attach;
    config.event_handler      = _http_event_handler;
    config.user_data          = &response_buffer;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_perform(client);
    std::string result_state = "unknown";

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200 && !response_buffer.empty()) {
            cJSON *root = cJSON_Parse(response_buffer.c_str());
            if (root) {
                cJSON *state_item = cJSON_GetObjectItem(root, "state");
                if (cJSON_IsString(state_item) && (state_item->valuestring != NULL)) {
                    result_state = std::string(state_item->valuestring);
                }
                cJSON_Delete(root);
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        result_state = "error";
    }

    esp_http_client_cleanup(client);
    return result_state;
}

void MyHomeDevice::CallService(const char* base_url, const char* token, const char* domain, const char* service, const char* entity_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/services/%s/%s", base_url, domain, service);

    ESP_LOGI(TAG, "Calling Service: %s for %s", url, entity_id);

    esp_http_client_config_t config = {};
    config.url               = url;
    config.method            = HTTP_METHOD_POST;
    config.timeout_ms        = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth_header);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "entity_id", entity_id);
    const char *post_data = cJSON_PrintUnformatted(root);

    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Request failed: %s", esp_err_to_name(err));
    }

    cJSON_Delete(root);
    free((void*)post_data);
    esp_http_client_cleanup(client);
}

// 通用：POST 到本地 HA，body 为自定义 JSON 字符串
static void CallLocalHaService(const char* domain, const char* service, const std::string& body_json) {
    auto& ha = HaConfig::GetInstance();
    std::string url_str   = ha.ha_camera_url() + "/api/services/" + domain + "/" + service;
    std::string auth_hdr  = "Bearer " + ha.ha_camera_token();

    esp_http_client_config_t cfg = {};
    cfg.url               = url_str.c_str();
    cfg.method            = HTTP_METHOD_POST;
    cfg.timeout_ms        = 5000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    auto client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_hdr.c_str());
    esp_http_client_set_post_field(client, body_json.c_str(), (int)body_json.size());

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || (status != 200 && status != 201))
        ESP_LOGE(TAG, "CallLocalHaService %s/%s failed: err=%d status=%d", domain, service, err, status);
    else
        ESP_LOGI(TAG, "CallLocalHaService %s/%s ok", domain, service);
}

// =================================================================================
// Part 1.6: 总台语音通话（对讲机模式）
// =================================================================================

// ---------- 服务器配置 ----------
#define STATION_SERVER      "http://yzhserver.111yzh.cn:8000"
#define STATION_WS_URL      "ws://yzhserver.111yzh.cn:8000/device/2189666?token=xiaozhi2025"
#define STATION_TOKEN       "xiaozhi2025"
#define STATION_ROOM_ID     "2189666"
#define STATION_ROOM_NAME   "小智设备01"
#define STATION_SAMPLE_RATE 16000
#define STATION_MAX_SEC     60   // 最长录音时间（秒）

// ---------- 状态机 ----------
enum class StationCallState { kIdle, kReady, kRecording, kSending, kWaitingReply };
static std::atomic<StationCallState> s_sc_state{StationCallState::kIdle};
static std::mutex                   s_sc_mutex;
static std::vector<int16_t>         s_sc_pcm;
static esp_websocket_client_handle_t s_sc_ws = nullptr;
static TaskHandle_t                  s_sc_record_task = nullptr;
static std::atomic<int64_t>          s_sc_record_start_us{0};

// ScSendTask 用 PSRAM 栈（CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y）
// 避免从内部 SRAM 分配大连续块导致失败
static StackType_t*  s_sc_send_stack = nullptr;   // 首次用时从 PSRAM 分配
static StaticTask_t  s_sc_send_tcb;               // TCB 放内部 SRAM 即可
static std::atomic<bool> s_sc_sending{false};     // 防止并发发送

// ---------- 留言队列（最多保留5条）----------
struct ScReply { std::string msg_id; std::string url; };
static std::mutex            s_sc_reply_mutex;
static std::deque<ScReply>   s_sc_replies;    // 待播放的回复（msg_id + url），deque保证O(1)头删

// 更新屏幕上的留言计数（必须在主线程 / Schedule 内调用）
static void ScUpdateReplyBadge() {
    auto* display = Board::GetInstance().GetDisplay();
    if (!display) return;
    size_t unread;
    {
        std::lock_guard<std::mutex> lk(s_sc_reply_mutex);
        unread = s_sc_replies.size();
    }
    if (unread == 0) {
        display->SetChatMessage("system", "📞 总台通话就绪\n按住按键说话，松开发送");
    } else {
        std::string msg = "📩 收到总台 " + std::to_string(unread) + " 条留言\n说「播放留言」收听";
        display->SetChatMessage("system", msg.c_str());
    }
}

bool IsStationCallActive() {
    return s_sc_state.load() != StationCallState::kIdle;
}

// ---------- 工具函数 ----------
static void ScShowStatus(const char* text) {
    Application::GetInstance().Schedule([t = std::string(text)]() {
        auto* display = Board::GetInstance().GetDisplay();
        if (display) display->ShowNotification(t.c_str(), 4000);
    });
}

// OGG/Opus 容器构建工具
// OGG CRC32（多项式 0x04C11DB7，无反射，初值 0）
static uint32_t OggCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

// 向 ogg 缓冲区追加一个 OGG 页面（自动计算并填充 CRC）
static void OggAppendPage(std::vector<uint8_t>& ogg, uint8_t header_type,
                           uint64_t granule_pos, uint32_t serial, uint32_t& seq_no,
                           const uint8_t* payload, size_t payload_len) {
    std::vector<uint8_t> segs;
    size_t rem = payload_len;
    while (rem >= 255) { segs.push_back(255); rem -= 255; }
    segs.push_back((uint8_t)rem);

    size_t hdr_len   = 27 + segs.size();
    size_t page_start = ogg.size();
    ogg.resize(page_start + hdr_len + payload_len);
    uint8_t* p = ogg.data() + page_start;

    memcpy(p, "OggS", 4);
    p[4] = 0; p[5] = header_type;
    for (int i = 0; i < 8; i++) p[6+i]  = (granule_pos >> (i*8)) & 0xFF;
    for (int i = 0; i < 4; i++) p[14+i] = (serial     >> (i*8)) & 0xFF;
    for (int i = 0; i < 4; i++) p[18+i] = (seq_no     >> (i*8)) & 0xFF;
    seq_no++;
    memset(p+22, 0, 4);                       // CRC 占位
    p[26] = (uint8_t)segs.size();
    memcpy(p+27, segs.data(), segs.size());
    memcpy(p + hdr_len, payload, payload_len);

    uint32_t crc = OggCrc32(p, hdr_len + payload_len);
    p[22] = crc & 0xFF; p[23] = (crc>>8) & 0xFF;
    p[24] = (crc>>16) & 0xFF; p[25] = (crc>>24) & 0xFF;
}

// 将 Opus 包列表封装为合法的 OGG/Opus 字节流
static std::vector<uint8_t> BuildOgg(const std::vector<std::vector<uint8_t>>& pkts, int sample_rate) {
    const uint32_t serial = 0x12345678;
    uint32_t seq_no = 0;
    std::vector<uint8_t> ogg;
    ogg.reserve(512 + pkts.size() * 300);

    // Page 0: OpusHead
    const uint16_t pre_skip = 312;
    uint8_t head[19] = {
        'O','p','u','s','H','e','a','d', 1, 1,
        (uint8_t)(pre_skip & 0xFF), (uint8_t)(pre_skip >> 8),
        (uint8_t)(sample_rate & 0xFF), (uint8_t)((sample_rate>>8) & 0xFF),
        (uint8_t)((sample_rate>>16) & 0xFF), (uint8_t)((sample_rate>>24) & 0xFF),
        0, 0, 0
    };
    OggAppendPage(ogg, 0x02, 0, serial, seq_no, head, sizeof(head));

    // Page 1: OpusTags
    const char* vendor = "ESP32SC";
    std::vector<uint8_t> tags = {'O','p','u','s','T','a','g','s',
        7,0,0,0, 'E','S','P','3','2','S','C', 0,0,0,0};
    OggAppendPage(ogg, 0x00, 0, serial, seq_no, tags.data(), tags.size());

    // granule_pos 必须用实际采样率计算（如16kHz×60ms=960采样/帧，不能硬编码48kHz的2880）
    const uint32_t spf = static_cast<uint32_t>(sample_rate) * 60 / 1000;
    uint64_t granule = pre_skip;
    for (size_t i = 0; i < pkts.size(); i++) {
        granule += spf;
        uint8_t htype = (i == pkts.size()-1) ? 0x04 : 0x00;
        OggAppendPage(ogg, htype, granule, serial, seq_no, pkts[i].data(), pkts[i].size());
    }
    return ogg;
}

// 下载 URL 音频字节并通过 AudioService 播放；播放后通过 WS 发送已读回执
static void ScDownloadAndPlay(const std::string& msg_id, const std::string& url) {
    std::string audio_data;
    esp_http_client_config_t cfg = {};
    cfg.url                = url.c_str();
    cfg.method             = HTTP_METHOD_GET;
    cfg.timeout_ms         = HTTP_TIMEOUT_REMOTE_MS;
    cfg.buffer_size        = 4096;
    cfg.crt_bundle_attach  = esp_crt_bundle_attach;  // 支持 HTTPS 下载
    cfg.event_handler = [](esp_http_client_event_t* e) -> esp_err_t {
        if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data)
            ((std::string*)e->user_data)->append((const char*)e->data, e->data_len);
        return ESP_OK;
    };
    cfg.user_data = &audio_data;

    auto client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || audio_data.empty()) {
        ESP_LOGE(TAG, "ScDownloadAndPlay: download failed err=%d status=%d", err, status);
        Application::GetInstance().Schedule([]() {
            auto* display = Board::GetInstance().GetDisplay();
            if (display) display->ShowNotification("❌ 留言下载失败", 3000);
        });
        return;
    }
    ESP_LOGI(TAG, "ScDownloadAndPlay: downloaded %d bytes, playing...", (int)audio_data.size());
    // 验证 OGG 格式魔数；若不以 "OggS" 开头则 PlaySound 会静默失败
    if (audio_data.size() < 4 ||
        !(audio_data[0]=='O' && audio_data[1]=='g' && audio_data[2]=='g' && audio_data[3]=='S')) {
        ESP_LOGE(TAG, "ScDownloadAndPlay: not OGG format! header=%02X %02X %02X %02X",
                 (uint8_t)audio_data[0], (uint8_t)audio_data[1],
                 (uint8_t)audio_data[2], (uint8_t)audio_data[3]);
        Application::GetInstance().Schedule([]() {
            auto* display = Board::GetInstance().GetDisplay();
            if (display) display->ShowNotification("❌ 音频格式错误(非OGG)", 3000);
        });
        return;
    }
    // 在主线程排队播放
    Application::GetInstance().Schedule([audio_data]() {
        auto& audio = Application::GetInstance().GetAudioService();
        audio.PlaySound(std::string_view(audio_data.data(), audio_data.size()));
    });
    // 发送已读回执（音频已排队，稍后即播放）
    if (!msg_id.empty() && s_sc_ws &&
        esp_websocket_client_is_connected(s_sc_ws)) {
        std::string read_msg = "{\"type\":\"read\",\"msg_id\":\"" + msg_id + "\"}";
        esp_websocket_client_send_text(s_sc_ws, read_msg.c_str(),
                                       (int)read_msg.size(), pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "ScDownloadAndPlay: sent read receipt for %s", msg_id.c_str());
    }
}

// Base64 编码
static std::string ScBase64Encode(const uint8_t* data, size_t len) {
    size_t out_len = 0;
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    std::string out(out_len, '\0');
    mbedtls_base64_encode((unsigned char*)out.data(), out_len, &out_len, data, len);
    out.resize(out_len);
    return out;
}

// HTTP 上传音频（OGG/Opus 格式，相比 WAV 节省约 8 倍带宽）
static bool ScUploadAudio(const std::vector<int16_t>& pcm) {
    if (pcm.empty()) return false;

    // 编码 PCM → Opus packets（960 采样/帧 = 16kHz × 60ms）
    const int frame_size = STATION_SAMPLE_RATE * 60 / 1000; // 960
    OpusEncoderWrapper encoder(STATION_SAMPLE_RATE, 1, 60);
    // ScSendTask 运行在 32KB PSRAM 栈上，可以承受更高 complexity
    // complexity=5 比 complexity=0 音质显著提升，栈消耗约增加 4KB，在 32KB 内安全
    encoder.SetComplexity(5);

    std::vector<std::vector<uint8_t>> pkts;
    for (size_t off = 0; off + (size_t)frame_size <= pcm.size(); off += frame_size) {
        std::vector<int16_t> frame(pcm.begin() + off, pcm.begin() + off + frame_size);
        std::vector<uint8_t> opus_pkt;
        if (encoder.Encode(std::move(frame), opus_pkt) && !opus_pkt.empty()) {
            pkts.push_back(std::move(opus_pkt));
        }
    }
    if (pkts.empty()) return false;

    auto ogg  = BuildOgg(pkts, STATION_SAMPLE_RATE);
    float dur = (float)pcm.size() / STATION_SAMPLE_RATE;
    ESP_LOGI(TAG, "ScUploadAudio: pcm=%d samples → OGG %d bytes (%.1fs)",
             (int)pcm.size(), (int)ogg.size(), dur);

    std::string b64 = ScBase64Encode(ogg.data(), ogg.size());

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "token",      STATION_TOKEN);
    cJSON_AddStringToObject(root, "room_id",    STATION_ROOM_ID);
    cJSON_AddStringToObject(root, "room_name",  STATION_ROOM_NAME);
    cJSON_AddStringToObject(root, "audio_data", b64.c_str());
    cJSON_AddNumberToObject(root, "duration",   (int)dur);
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    std::string resp_buf;
    esp_http_client_config_t cfg = {};
    cfg.url           = STATION_SERVER "/api/voice/upload";
    cfg.method        = HTTP_METHOD_POST;
    cfg.timeout_ms    = HTTP_TIMEOUT_REMOTE_MS;
    cfg.event_handler = [](esp_http_client_event_t* e) -> esp_err_t {
        if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data)
            ((std::string*)e->user_data)->append((const char*)e->data, e->data_len);
        return ESP_OK;
    };
    cfg.user_data = &resp_buf;

    auto client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    bool ok = (err == ESP_OK && status == 200);
    ESP_LOGI(TAG, "ScUploadAudio: err=%d status=%d dur=%.1fs ok=%d", err, status, dur, ok);
    return ok;
}

// 后台录音任务（循环调用 ReadAudioData 直到 state 不是 kRecording）
static void ScRecordTask(void*) {
    auto& audio  = Application::GetInstance().GetAudioService();
    const int chunk = STATION_SAMPLE_RATE / 10; // 100ms per read
    const int maxsamples = STATION_SAMPLE_RATE * STATION_MAX_SEC;
    std::vector<int16_t> buf;
    int last_display_sec = -1;

    while (s_sc_state.load() == StationCallState::kRecording) {
        if (audio.ReadAudioData(buf, STATION_SAMPLE_RATE, chunk)) {
            std::lock_guard<std::mutex> lk(s_sc_mutex);
            if ((int)(s_sc_pcm.size() + buf.size()) < maxsamples) {
                s_sc_pcm.insert(s_sc_pcm.end(), buf.begin(), buf.end());
            } else {
                ESP_LOGW(TAG, "Station: max record length reached");
                break;
            }
        }
        // 每秒更新一次屏幕计时显示
        int64_t elapsed_us = esp_timer_get_time() - s_sc_record_start_us.load();
        int elapsed_sec = (int)(elapsed_us / 1000000);
        if (elapsed_sec != last_display_sec) {
            last_display_sec = elapsed_sec;
            Application::GetInstance().Schedule([elapsed_sec]() {
                auto* display = Board::GetInstance().GetDisplay();
                if (display) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "🔴 录音中... %02ds / %ds", elapsed_sec, STATION_MAX_SEC);
                    display->ShowNotification(buf, 1500);
                }
            });
        }
    }
    // 录音结束后恢复唤醒词检测（与 StationCallStartRecord 中的禁用对称）
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    s_sc_record_task = nullptr;
    vTaskDelete(nullptr);
}

// 上传任务（在后台执行，避免阻塞按键回调）
static void ScSendTask(void*) {
    std::vector<int16_t> pcm_copy;
    {
        std::lock_guard<std::mutex> lk(s_sc_mutex);
        pcm_copy = s_sc_pcm;
        s_sc_pcm.clear();
    }

    float dur = (float)pcm_copy.size() / STATION_SAMPLE_RATE;
    ESP_LOGI(TAG, "Station: uploading %.1fs audio", dur);

    if (dur < 0.5f) {
        ScShowStatus("录音太短，已忽略");
        s_sc_state = StationCallState::kReady;
        s_sc_sending = false;
        vTaskDelete(nullptr);
        return;
    }

    ScShowStatus("📤 正在发送给总台...");
    bool ok = ScUploadAudio(pcm_copy);

    if (ok) {
        s_sc_state = StationCallState::kReady;   // 保持 kReady，允许立即再次录音
        ScShowStatus("✅ 留言已发送，可继续说话");
    } else {
        s_sc_state = StationCallState::kReady;
        ScShowStatus("❌ 发送失败，请重试");
    }
    s_sc_sending = false;
    vTaskDelete(nullptr);
}

// WebSocket 事件回调
static void ScWsEventHandler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
    auto* d = (esp_websocket_event_data_t*)event_data;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Station WS connected");
        s_sc_state = StationCallState::kReady;
        ScShowStatus("📞 总台已连接\n按住按键说话，松开发送");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Station WS disconnected");
        if (s_sc_state.load() != StationCallState::kIdle) {
            ScShowStatus("📴 总台连接断开");
            s_sc_state = StationCallState::kIdle;
        }
    } else if (event_id == WEBSOCKET_EVENT_DATA && d->op_code == 0x01 /* text frame */) {
        // 解析总台回复
        std::string raw((const char*)d->data_ptr, d->data_len);
        cJSON* json = cJSON_Parse(raw.c_str());
        if (!json) return;

        const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(json, "type"));
        if (type) {
            if (strcmp(type, "ping") == 0) {
                // 心跳回应
                esp_websocket_client_send_text(s_sc_ws, "pong", 4, pdMS_TO_TICKS(1000));
            } else if (strcmp(type, "reply") == 0) {
                const char* url_c    = cJSON_GetStringValue(cJSON_GetObjectItem(json, "audio_url"));
                const char* msgid_c  = cJSON_GetStringValue(cJSON_GetObjectItem(json, "msg_id"));
                std::string url    = url_c   ? url_c   : "";
                std::string msg_id = msgid_c ? msgid_c : "";
                ESP_LOGI(TAG, "Station: reply received, msg_id=%s", msg_id.c_str());
                {
                    std::lock_guard<std::mutex> lk(s_sc_reply_mutex);
                    if (!url.empty()) {
                        if (s_sc_replies.size() >= 5) s_sc_replies.pop_front();
                        s_sc_replies.push_back({msg_id, url});
                    }
                }
                // 震动音 + 持久屏幕提示（不自动消失）
                Application::GetInstance().Schedule([]() {
                    auto& audio = Application::GetInstance().GetAudioService();
                    audio.PlaySound(Lang::Sounds::OGG_VIBRATION);
                    auto* display = Board::GetInstance().GetDisplay();
                    if (display) {
                        display->SetEmotion("happy");
                        ScUpdateReplyBadge();
                    }
                });
                s_sc_state = StationCallState::kReady;
            }
        }
        cJSON_Delete(json);
    }
}

// 连接 WS，进入通话模式
void StationCallConnect() {
    // 已经处于通话模式时，只刷新提示，不重建 WS（避免断开事件竞态导致状态变 kIdle）
    if (s_sc_state.load() != StationCallState::kIdle) {
        ESP_LOGI(TAG, "StationCallConnect: already active, refresh display only");
        Application::GetInstance().Schedule([]() { ScUpdateReplyBadge(); });
        return;
    }
    if (s_sc_ws) {
        esp_websocket_client_stop(s_sc_ws);    // 先 stop 再 destroy，避免异步 DISCONNECT 覆盖新状态
        esp_websocket_client_destroy(s_sc_ws);
        s_sc_ws = nullptr;
    }
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri          = STATION_WS_URL;
    ws_cfg.reconnect_timeout_ms = 5000;
    ws_cfg.ping_interval_sec    = 30;
    s_sc_ws = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s_sc_ws, WEBSOCKET_EVENT_ANY, ScWsEventHandler, nullptr);
    esp_websocket_client_start(s_sc_ws);
    s_sc_state = StationCallState::kReady;

    // 屏幕反馈，通过 Schedule 投递到主线程
    Application::GetInstance().Schedule([]() {
        auto* display = Board::GetInstance().GetDisplay();
        if (display) {
            display->SetEmotion("thinking");
            display->SetStatus("总台通话");
            display->SetChatMessage("system", "📞 正在连接总台...\n连接后按住按键说话");
        }
    });
}

// 断开通话
void StationCallDisconnect() {
    s_sc_state = StationCallState::kIdle;
    if (s_sc_ws) {
        esp_websocket_client_stop(s_sc_ws);
        esp_websocket_client_destroy(s_sc_ws);
        s_sc_ws = nullptr;
    }
    // 等待发送任务结束，再释放 PSRAM 栈（StaticTask 不自动回收栈内存）
    for (int i = 0; i < 300 && s_sc_sending.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_sc_send_stack) {
        heap_caps_free(s_sc_send_stack);
        s_sc_send_stack = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(s_sc_reply_mutex);
        s_sc_replies.clear();
    }
    Application::GetInstance().Schedule([]() {
        auto* display = Board::GetInstance().GetDisplay();
        if (display) {
            display->SetStatus("待机");
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
        }
    });
}

// 按键按下：开始录音（从按键回调调用，不可阻塞，不可直接操作 LVGL）
void StationCallStartRecord() {
    if (s_sc_state.load() != StationCallState::kReady) {
        ESP_LOGW(TAG, "StationCallStartRecord: blocked, state=%d (0=idle,1=ready,2=rec,3=send,4=wait)",
                 (int)s_sc_state.load());
        return;
    }
    s_sc_state = StationCallState::kRecording;
    {
        std::lock_guard<std::mutex> lk(s_sc_mutex);
        s_sc_pcm.clear();
    }
    // 禁用唤醒词检测：防止 AudioInputTask 与 ScRecordTask 争抢 I2S DMA 帧
    // 若两个任务同时消费同一 DMA 缓冲区，ScRecordTask 只能录到约一半的帧，
    // 导致播放速度偏快、音频失真。禁用后 AudioInputTask 阻塞，ScRecordTask 独占帧。
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
    // 显示操作必须投递到主线程，避免在按键回调中直接操作 LVGL 导致崩溃
    // 同时停止 AI 监听，防止录音内容被 AI 误识别
    Application::GetInstance().Schedule([]() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateListening) {
            app.StopListening();
        }
        auto* display = Board::GetInstance().GetDisplay();
        if (display) {
            display->SetEmotion("thinking");  // 使用存在的表情，"listening" 不在表情库中
            display->ShowNotification("🔴 录音中...", 60000);
        }
    });
    s_sc_record_start_us = esp_timer_get_time();
    xTaskCreate(ScRecordTask, "sc_record", 4096, nullptr, 5, &s_sc_record_task);
    ESP_LOGI(TAG, "Station: recording started");
}

// 按键松开：停止录音并发送（从按键回调调用，不可阻塞）
void StationCallStopRecord() {
    if (s_sc_state.load() != StationCallState::kRecording) return;
    // 若上次发送还没结束（极端情况），直接忽略
    if (s_sc_sending.exchange(true)) {
        ESP_LOGW(TAG, "ScSendTask still running, skip");
        return;
    }
    s_sc_state = StationCallState::kSending;
    Application::GetInstance().Schedule([]() {
        auto* display = Board::GetInstance().GetDisplay();
        if (display) display->SetEmotion("neutral");
    });

    // 首次使用时从 PSRAM 分配 32KB 栈（CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y）
    // 避免从碎片化内部 SRAM 中申请大连续块失败
    if (!s_sc_send_stack) {
        s_sc_send_stack = (StackType_t*)heap_caps_malloc(
            32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_sc_send_stack) {
        ESP_LOGE(TAG, "ScSendTask: PSRAM alloc failed");
        ScShowStatus("❌ 内存不足，发送失败");
        s_sc_sending = false;
        s_sc_state = StationCallState::kReady;
        return;
    }

    xTaskCreateStaticPinnedToCore(ScSendTask, "sc_send", 32768,
                                  nullptr, 3, s_sc_send_stack,
                                  &s_sc_send_tcb, tskNO_AFFINITY);
    ESP_LOGI(TAG, "Station: recording stopped, sending...");
}

// =================================================================================
// Part 1.5: 提醒/闹钟
// =================================================================================
// =================================================================================

struct Reminder {
    int     id;           // 唯一编号，自增
    int64_t trigger_us;   // esp_timer_get_time() 到期时间（微秒）
    std::string message;
    bool fired;
};

static std::mutex          s_reminder_mutex;
static std::vector<Reminder> s_reminders;
static int s_reminder_id_counter = 0;
static std::atomic<bool> s_alarm_active{false};  // 当前是否有闹钟正在响

bool IsAlarmRinging() { return s_alarm_active.load(); }
void DismissAlarm()   { s_alarm_active = false; }

static void ReminderTask(void*) {
    while (true) {
        // 方案四：自适应轮询间隔
        // - 无提醒：30 秒轮询一次（节省 CPU）
        // - 最近提醒在 60 秒内：1 秒轮询（高精度触发）
        // - 其他：5 秒轮询
        int32_t next_check_ms = 30000;
        {
            std::lock_guard<std::mutex> lock(s_reminder_mutex);
            if (!s_reminders.empty()) {
                int64_t now_us = esp_timer_get_time();
                int64_t min_remaining_ms = INT64_MAX;
                for (auto& r : s_reminders) {
                    int64_t rem_ms = (r.trigger_us - now_us) / 1000LL;
                    if (rem_ms < min_remaining_ms) min_remaining_ms = rem_ms;
                }
                if (min_remaining_ms < 60000LL) next_check_ms = 1000;
                else next_check_ms = 5000;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(next_check_ms));

        int64_t now_us = esp_timer_get_time();
        std::vector<std::string> to_fire;

        {
            std::lock_guard<std::mutex> lock(s_reminder_mutex);
            for (auto& r : s_reminders) {
                if (!r.fired && now_us >= r.trigger_us) {
                    r.fired = true;
                    to_fire.push_back(r.message);
                }
            }
            s_reminders.erase(
                std::remove_if(s_reminders.begin(), s_reminders.end(),
                    [](const Reminder& r) { return r.fired; }),
                s_reminders.end()
            );
        }

        for (auto& msg : to_fire) {
            ESP_LOGI(TAG, "Reminder fired: %s", msg.c_str());
            s_alarm_active = true;
            std::string fire_msg = msg;

            // ① 屏幕显示闹钟界面：scare 表情 + 提醒内容 + 操作提示
            Application::GetInstance().Schedule([fire_msg]() {
                Application::GetInstance().Alert(
                    "⏰ 提醒时间到！",
                    ("🔔 " + fire_msg + "\n\n按键关闭").c_str(),
                    "scare"
                );
            });

            // ② 重复响铃循环：每次响一声，每 500ms 检查一次，5 秒后再响
            auto& audio = Application::GetInstance().GetAudioService();
            while (s_alarm_active) {
                audio.PlaySound(Lang::Sounds::OGG_XIAOZHI_MORNING_ALARM);
                for (int i = 0; i < 10 && s_alarm_active; i++) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }

            // ③ 用户按键关闭 — 显示确认界面
            Application::GetInstance().Schedule([]() {
                auto display = Board::GetInstance().GetDisplay();
                display->SetStatus("✅ 好的~");
                display->SetEmotion("happy");
                display->SetChatMessage("system", "提醒已关闭 😊");
            });
            vTaskDelay(pdMS_TO_TICKS(1500));

            // ④ 恢复待机界面 + 通知 AI 语音播报
            Application::GetInstance().Schedule([fire_msg]() {
                auto display = Board::GetInstance().GetDisplay();
                display->SetStatus(Lang::Strings::STANDBY);
                display->SetEmotion("neutral");
                display->SetChatMessage("system", "");
                auto* proto = Application::GetInstance().GetProtocol();
                if (proto) {
                    proto->SendSensorEvent("提醒时间到了：" + fire_msg + "。请用语音告知用户。");
                }
            });
        }
    }
    vTaskDelete(nullptr);
}

static void StartReminderTask() {
    xTaskCreate(ReminderTask, "reminder", 4096, nullptr, 1, nullptr);
    ESP_LOGI(TAG, "ReminderTask started");
}

// =================================================================================
// Part 2: 注册 HA 控制工具 (MCP Tool)
// =================================================================================

void MyHomeDevice::RegisterHomeDeviceTools() {
    auto& server = McpServer::GetInstance();

    server.AddTool(
        "control_home_device",
        "控制或查询家电。参数: device(plug/door/tv/water_valve/gas_valve/main_switch), action(on/off/query)",
        PropertyList({
            Property("device", kPropertyTypeString),
            Property("action", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            try {
                std::string device = properties["device"].value<std::string>();
                std::string action = properties["action"].value<std::string>();

                auto& ha = HaConfig::GetInstance();
                std::string target_entity;
                std::string target_url;
                std::string target_token;
                std::string device_name_cn;
                std::string domain = "switch";

                // --- 路由逻辑 ---
                if (device == "plug") {
                    target_entity  = ha.entity_smart_plug();
                    device_name_cn = "智能插座";
                    target_url     = ha.ha_new_url();
                    target_token   = ha.ha_new_token();
                }
                else if (device == "door") {
                    target_entity  = ha.entity_door_sensor();
                    device_name_cn = "大门传感器";
                    domain         = "binary_sensor";
                    target_url     = ha.ha_new_url();
                    target_token   = ha.ha_new_token();
                }
                else if (device == "tv") {
                    target_entity  = ha.entity_tv();
                    device_name_cn = "电视";
                    target_url     = ha.ha_old_url();
                    target_token   = ha.ha_old_token();
                }
                else if (device == "water_valve") {
                    target_entity  = ha.entity_water_valve();
                    device_name_cn = "水阀";
                    target_url     = ha.ha_old_url();
                    target_token   = ha.ha_old_token();
                }
                else if (device == "gas_valve") {
                    target_entity  = ha.entity_gas_valve();
                    device_name_cn = "气阀";
                    target_url     = ha.ha_old_url();
                    target_token   = ha.ha_old_token();
                }
                else if (device == "main_switch") {
                    target_entity  = ha.entity_main_switch();
                    device_name_cn = "总闸";
                    target_url     = ha.ha_old_url();
                    target_token   = ha.ha_old_token();
                }

                if (target_entity.empty()) {
                    return std::string("错误: 找不到该设备。");
                }

                // 1. 查询
                if (action == "query") {
                    std::string state_raw = GetEntityState(target_url.c_str(), target_token.c_str(), target_entity.c_str());
                    std::string state_cn = state_raw;
                    if (device == "door") {
                        // binary_sensor contact: on=接触=门已关闭, off=断开=门已打开
                        if (state_raw == "on") state_cn = "已关闭";
                        else if (state_raw == "off") state_cn = "已打开";
                    } else {
                        if (state_raw == "on") state_cn = "打开";
                        else if (state_raw == "off") state_cn = "关闭";
                    }
                    return device_name_cn + "当前的状态是：" + state_cn;
                }

                // 2. 控制
                if (domain == "binary_sensor") {
                    return std::string("错误: 传感器只能查询，不能控制。");
                }

                std::string service = (action == "on") ? "turn_on" : "turn_off";
                std::string action_cn = (action == "on") ? "打开" : "关闭";

                CallService(target_url.c_str(), target_token.c_str(), domain.c_str(), service.c_str(), target_entity.c_str());
                return "好的，已帮你" + action_cn + device_name_cn;

            } catch (...) {
                return std::string("Error: Invalid arguments");
            }
        });

    ESP_LOGI(TAG, "Home Assistant Tools Registered.");

    // ===================== TP-Link 客厅摄像头工具 =====================

    // 云台 PTZ 控制
    server.AddTool(
        "control_ha_camera",
        "控制摄像头云台。entity_id留空=客厅主摄像头；控制其他摄像头时填入其 HA entity_id。"
        "direction: 上/下/左/右。distance: 移动距离0.1~1.0（默认0.3）。speed: 速度0.1~1.0（默认0.5）。",
        PropertyList({
            Property("direction", kPropertyTypeString),
            Property("distance", kPropertyTypeString, std::string("0.3")),
            Property("speed",    kPropertyTypeString, std::string("0.5")),
            Property("entity_id", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            try {
                std::string direction = properties["direction"].value<std::string>();
                std::string entity_id = properties["entity_id"].value<std::string>();
                float distance = std::stof(properties["distance"].value<std::string>());
                float speed    = std::stof(properties["speed"].value<std::string>());

                std::string axis, value;
                if (direction == "上")       { axis = "tilt"; value = "DOWN"; }
                else if (direction == "下")  { axis = "tilt"; value = "UP"; }
                else if (direction == "左")  { axis = "pan";  value = "LEFT"; }
                else if (direction == "右")  { axis = "pan";  value = "RIGHT"; }
                else return std::string("错误: 不支持的方向，请用 上/下/左/右");

                auto& ha_cam = HaConfig::GetInstance();
                std::string ptz_url, cam_auth, target_entity;
                if (entity_id.empty()) {
                    ptz_url       = ha_cam.ha_camera_ptz_url();
                    cam_auth      = "Bearer " + ha_cam.ha_camera_token();
                    target_entity = ha_cam.ha_camera_entity();
                } else {
                    // 从自定义设备列表查找该摄像头的 HA 实例
                    auto devs = ha_cam.GetCustomDevices();
                    std::string ha_type = "new";
                    for (const auto& d : devs) {
                        if (d.entity == entity_id) { ha_type = d.ha; break; }
                    }
                    std::string base_url = (ha_type == "old") ? ha_cam.ha_old_url() : ha_cam.ha_new_url();
                    if (base_url.size() >= 4 && base_url.compare(base_url.size()-4, 4, "/api") == 0)
                        base_url.resize(base_url.size() - 4);
                    ptz_url       = base_url + "/api/services/onvif/ptz";
                    cam_auth      = "Bearer " + ((ha_type == "old") ? ha_cam.ha_old_token() : ha_cam.ha_new_token());
                    target_entity = entity_id;
                }

                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "entity_id", target_entity.c_str());
                cJSON_AddStringToObject(root, axis.c_str(), value.c_str());
                cJSON_AddNumberToObject(root, "distance", distance);
                cJSON_AddNumberToObject(root, "speed", speed);
                const char* post_data = cJSON_PrintUnformatted(root);

                esp_http_client_config_t config = {};
                config.url               = ptz_url.c_str();
                config.method            = HTTP_METHOD_POST;
                config.timeout_ms        = 5000;
                config.crt_bundle_attach = esp_crt_bundle_attach;

                esp_http_client_handle_t client = esp_http_client_init(&config);
                esp_http_client_set_header(client, "Content-Type", "application/json");
                esp_http_client_set_header(client, "Authorization", cam_auth.c_str());
                esp_http_client_set_post_field(client, post_data, strlen(post_data));

                esp_err_t err = esp_http_client_perform(client);
                int status = esp_http_client_get_status_code(client);
                esp_http_client_cleanup(client);
                cJSON_Delete(root);
                free((void*)post_data);

                if (err == ESP_OK && (status == 200 || status == 201)) {
                    return "好的，摄像头已向" + direction + "移动";
                }
                return std::string("摄像头移动失败，状态码: ") + std::to_string(status);
            } catch (...) {
                return std::string("摄像头控制出错");
            }
        });

    // AI 描述画面
    server.AddTool(
        "describe_ha_camera",
        "查看摄像头画面并让AI描述内容。entity_id留空=客厅主摄像头；需要看其他摄像头时填入该摄像头的 HA entity_id。"
        "当用户问'客厅有什么'、'客厅什么情况'、'帮我看看监控'等时调用（entity_id留空）；"
        "当用户指定某个摄像头时填入对应实体ID。"
        "【重要】调用前必须先说一句话，如'稍等，我来分析下画面'。",
        PropertyList({
            Property("question", kPropertyTypeString, std::string("请详细描述画面中看到了什么，有没有人，有哪些物体")),
            Property("entity_id", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            try {
                if (s_vision_url.empty()) {
                    return std::string("AI视觉服务未配置，无法分析画面");
                }
                auto question  = properties["question"].value<std::string>();
                auto entity_id = properties["entity_id"].value<std::string>();
                auto network = Board::GetInstance().GetNetwork();

                // Step 1: 用 esp_http_client 下载 JPEG（支持大型二进制响应，避免 HttpClient 8KB 缓冲死锁）
                std::string jpeg_data;
                {
                    auto& ha_d = HaConfig::GetInstance();
                    std::string proxy_url, dl_auth;
                    if (entity_id.empty()) {
                        proxy_url = ha_d.ha_camera_proxy_url();
                        dl_auth   = "Bearer " + ha_d.ha_camera_token();
                    } else {
                        // 从自定义设备列表查找该 entity 属于哪个 HA 实例
                        auto devs = ha_d.GetCustomDevices();
                        std::string ha_type = "new";
                        for (const auto& d : devs) {
                            if (d.entity == entity_id) { ha_type = d.ha; break; }
                        }
                        std::string base_url = (ha_type == "old") ? ha_d.ha_old_url() : ha_d.ha_new_url();
                        std::string tok      = (ha_type == "old") ? ha_d.ha_old_token() : ha_d.ha_new_token();
                        if (base_url.size() >= 4 && base_url.compare(base_url.size()-4, 4, "/api") == 0)
                            base_url.resize(base_url.size() - 4);
                        proxy_url = base_url + "/api/camera_proxy/" + entity_id + "?width=640";
                        dl_auth   = "Bearer " + tok;
                    }

                    esp_http_client_config_t dl_config = {};
                    dl_config.url               = proxy_url.c_str();
                    dl_config.method            = HTTP_METHOD_GET;
                    dl_config.timeout_ms        = HTTP_TIMEOUT_MEDIA_MS;
                    dl_config.crt_bundle_attach = esp_crt_bundle_attach;
                    dl_config.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
                        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
                            auto* buf = (std::string*)evt->user_data;
                            buf->append((const char*)evt->data, evt->data_len);
                        }
                        return ESP_OK;
                    };
                    dl_config.user_data = &jpeg_data;
                    dl_config.buffer_size = 8192;

                    esp_http_client_handle_t dl_client = esp_http_client_init(&dl_config);
                    esp_http_client_set_header(dl_client, "Authorization", dl_auth.c_str());

                    esp_err_t err = esp_http_client_perform(dl_client);
                    int dl_status = esp_http_client_get_status_code(dl_client);
                    esp_http_client_cleanup(dl_client);

                    if (err != ESP_OK || dl_status != 200) {
                        return std::string("摄像头下载失败，状态码: ") + std::to_string(dl_status);
                    }
                }
                if (jpeg_data.empty()) {
                    return std::string("摄像头返回了空图像");
                }
                ESP_LOGI(TAG, "Downloaded JPEG from HA: %d bytes", (int)jpeg_data.size());

                // Step 2: multipart POST 发给 AI 视觉服务器
                auto http = network->CreateHttp();
                std::string boundary = "----ESP32_HA_CAM_BOUNDARY";
                http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
                http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
                if (!s_vision_token.empty()) {
                    http->SetHeader("Authorization", "Bearer " + s_vision_token);
                }
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                http->SetHeader("Transfer-Encoding", "chunked");

                if (!http->Open("POST", s_vision_url)) {
                    return std::string("无法连接到AI视觉服务器");
                }

                std::string q_field = "--" + boundary + "\r\n"
                    "Content-Disposition: form-data; name=\"question\"\r\n\r\n"
                    + question + "\r\n";
                http->Write(q_field.c_str(), q_field.size());

                std::string f_header = "--" + boundary + "\r\n"
                    "Content-Disposition: form-data; name=\"file\"; filename=\"ha_camera.jpg\"\r\n"
                    "Content-Type: image/jpeg\r\n\r\n";
                http->Write(f_header.c_str(), f_header.size());

                http->Write(jpeg_data.c_str(), jpeg_data.size());

                std::string footer = "\r\n--" + boundary + "--\r\n";
                http->Write(footer.c_str(), footer.size());
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    return std::string("AI视觉服务器返回错误，状态码: ") + std::to_string(http->GetStatusCode());
                }
                std::string result = http->ReadAll();
                http->Close();

                ESP_LOGI(TAG, "HA camera describe result: %s", result.c_str());
                return result;

            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "describe_ha_camera error: %s", e.what());
                return std::string("查看监控失败：") + std::string(e.what());
            }
        });

    // 把摄像头画面显示到设备屏幕
    server.AddTool(
        "show_camera_on_screen",
        "把摄像头当前画面截图并显示到设备屏幕。entity_id留空=客厅主摄像头；"
        "需要显示其他摄像头时填入该摄像头的 HA entity_id。"
        "当用户说'把画面传到屏幕'、'显示监控画面'、'在屏幕上看'、'让我看看画面'等时调用。",
        PropertyList({
            Property("entity_id", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& props) -> ReturnValue {
            try {
                std::string entity_id = props["entity_id"].value<std::string>();
                std::string jpeg_data;
                auto& ha = HaConfig::GetInstance();
                std::string url, token;
                if (entity_id.empty()) {
                    url   = ha.ha_camera_proxy_small_url();
                    token = ha.ha_camera_token();
                } else {
                    auto devs = ha.GetCustomDevices();
                    std::string ha_type = "new";
                    for (const auto& d : devs) {
                        if (d.entity == entity_id) { ha_type = d.ha; break; }
                    }
                    std::string base_url = (ha_type == "old") ? ha.ha_old_url() : ha.ha_new_url();
                    token    = (ha_type == "old") ? ha.ha_old_token() : ha.ha_new_token();
                    // ha_old_url/ha_new_url 末尾带 /api，camera_proxy 需要不带 /api 的基础 URL
                    if (base_url.size() >= 4 && base_url.compare(base_url.size()-4, 4, "/api") == 0)
                        base_url.resize(base_url.size() - 4);
                    url      = base_url + "/api/camera_proxy/" + entity_id + "?width=600";
                }
                ESP_LOGI(TAG, "show_camera_on_screen: entity='%s' url=%s", entity_id.c_str(), url.c_str());
                if (!DownloadJpegFromHA(jpeg_data, url.c_str(), token))
                    return std::string("摄像头截图下载失败，请检查网络连接");
                ShowJpegOnDisplay(jpeg_data);
                return std::string("好的，画面已显示到屏幕上了");
            } catch (const std::exception& e) {
                return std::string("显示失败：") + std::string(e.what());
            }
        });

    ESP_LOGI(TAG, "HA Camera Tools Registered.");

    // ===================== 窗帘控制工具 =====================
    server.AddTool(
        "control_curtain",
        "【必须调用此工具】控制家里客厅实体窗帘，不可直接回答。"
        "curtain: 1=窗帘1, 2=窗帘2, all=全部。"
        "action: open(打开/拉开), close(关闭/合上), stop(暂停), set(设置开合度)。"
        "position: 0~100，仅 action=set 时有效（0全关，100全开）。"
        "用户说'打开窗帘''关窗帘''拉开帘子'等均调用此工具。",
        PropertyList({
            Property("curtain", kPropertyTypeString),
            Property("action",  kPropertyTypeString),
            Property("position", kPropertyTypeString, std::string("50")),
        }),
        [](const PropertyList& props) -> ReturnValue {
            try {
                std::string curtain  = props["curtain"].value<std::string>();
                std::string action   = props["action"].value<std::string>();
                std::string pos_str  = props["position"].value<std::string>();

                // 确定要控制哪些实体
                auto& ha_c = HaConfig::GetInstance();
                std::vector<std::string> entities;
                if (curtain == "1")      entities = { ha_c.entity_curtain_1() };
                else if (curtain == "2") entities = { ha_c.entity_curtain_2() };
                else                     entities = { ha_c.entity_curtain_1(), ha_c.entity_curtain_2() };

                const char* svc = nullptr;
                if      (action == "open")  svc = "open_cover";
                else if (action == "close") svc = "close_cover";
                else if (action == "stop")  svc = "stop_cover";
                else if (action == "set")   svc = "set_cover_position";
                else return std::string("不支持的动作，请用 open/close/stop/set");

                for (const std::string& eid : entities) {
                    cJSON* body = cJSON_CreateObject();
                    cJSON_AddStringToObject(body, "entity_id", eid.c_str());
                    if (action == "set") {
                        int pos = atoi(pos_str.c_str());
                        if (pos < 0) pos = 0;
                        if (pos > 100) pos = 100;
                        cJSON_AddNumberToObject(body, "position", pos);
                    }
                    char* body_str = cJSON_PrintUnformatted(body);
                    CallLocalHaService("cover", svc, body_str);
                    free(body_str);
                    cJSON_Delete(body);
                }

                std::string who = (curtain == "all") ? "所有窗帘" :
                                  (curtain == "1")   ? "窗帘1" : "窗帘2";
                std::string act_cn = (action == "open")  ? "打开" :
                                     (action == "close") ? "关闭" :
                                     (action == "stop")  ? "暂停" :
                                     "设置开合度为 " + pos_str + "%";
                return "好的，已" + act_cn + who;
            } catch (...) {
                return std::string("窗帘控制失败，请检查参数");
            }
        });

    // ===================== 小米音箱播放控制 =====================
    server.AddTool(
        "control_speaker",
        "【必须调用此工具】控制家里客厅实体小米音箱的播放状态，不可用自带音乐服务替代。"
        "action: play(播放/继续), pause(暂停), next(下一首/切歌), prev(上一首), volume(设音量)。"
        "volume_level: 0~100，仅 action=volume 时有效。"
        "用户说'暂停音乐''下一首''调音量'等均调用此工具。",
        PropertyList({
            Property("action",       kPropertyTypeString),
            Property("volume_level", kPropertyTypeString, std::string("50")),
        }),
        [](const PropertyList& props) -> ReturnValue {
            try {
                std::string action = props["action"].value<std::string>();
                std::string vol_str = props["volume_level"].value<std::string>();

                std::string spk_entity = HaConfig::GetInstance().entity_speaker();
                cJSON* body = cJSON_CreateObject();
                cJSON_AddStringToObject(body, "entity_id", spk_entity.c_str());

                const char* svc    = nullptr;
                std::string result = "";

                if      (action == "play")  { svc = "media_play";           result = "已开始播放"; }
                else if (action == "pause") { svc = "media_pause";          result = "已暂停播放"; }
                else if (action == "next")  { svc = "media_next_track";     result = "已切换下一首"; }
                else if (action == "prev")  { svc = "media_previous_track"; result = "已切换上一首"; }
                else if (action == "volume") {
                    svc = "volume_set";
                    int vol = atoi(vol_str.c_str());
                    if (vol < 0) vol = 0;
                    if (vol > 100) vol = 100;
                    cJSON_AddNumberToObject(body, "volume_level", vol / 100.0);
                    result = "已将音量设为 " + vol_str + "%";
                } else {
                    cJSON_Delete(body);
                    return std::string("不支持的操作，请用 play/pause/next/prev/volume");
                }

                char* body_str = cJSON_PrintUnformatted(body);
                CallLocalHaService("media_player", svc, body_str);
                free(body_str);
                cJSON_Delete(body);
                return result;
            } catch (...) {
                return std::string("音箱控制失败");
            }
        });

    // ===================== 小米音箱 TTS 播报 / 指令 =====================
    server.AddTool(
        "speaker_say",
        "【必须调用此工具】让家里客厅实体小米音箱朗读文字或执行语音指令，不可直接回答。"
        "mode: tts=让音箱朗读文字内容（如通知、提醒）; command=让音箱执行指令（如'播放音乐''播放轻音乐'）。"
        "text: 朗读内容或指令文字。"
        "用户说'让音箱说…''让音箱播…''让音箱播放轻音乐'等均调用此工具。",
        PropertyList({
            Property("mode", kPropertyTypeString),
            Property("text", kPropertyTypeString),
        }),
        [](const PropertyList& props) -> ReturnValue {
            try {
                std::string mode = props["mode"].value<std::string>();
                std::string text = props["text"].value<std::string>();
                if (text.empty()) return std::string("请提供文字内容");

                auto& ha_spk = HaConfig::GetInstance();
                std::string entity_id = (mode == "command") ? ha_spk.entity_speaker_cmd() : ha_spk.entity_speaker_tts();

                cJSON* body = cJSON_CreateObject();
                cJSON_AddStringToObject(body, "entity_id", entity_id.c_str());
                cJSON_AddStringToObject(body, "value", text.c_str());
                char* body_str = cJSON_PrintUnformatted(body);
                CallLocalHaService("text", "set_value", body_str);
                free(body_str);
                cJSON_Delete(body);

                if (mode == "command")
                    return "已向音箱发送指令：" + text;
                else
                    return "已让音箱播报：" + text;
            } catch (...) {
                return std::string("音箱播报失败");
            }
        });

    // ===================== 紧急求救（拨打救援电话）=====================
    server.AddTool(
        "call_emergency",
        "【紧急工具】当用户说'救命''救救我''帮我报警''打120''我需要帮助'等求救词，且用户已确认需要求救时，立即调用此工具拨打救援电话。",
        PropertyList(std::vector<Property>{}),
        [](const PropertyList&) -> ReturnValue {
            const char* url  = "https://admin.11yzh.com/api/hotline/openarchives";
            const char* body = "{\"eps400\":\"4000000126\",\"caller\":\"19808555455\"}";

            esp_http_client_config_t cfg = {};
            cfg.url                     = url;
            cfg.method                  = HTTP_METHOD_POST;
            cfg.timeout_ms              = 10000;
            cfg.crt_bundle_attach       = esp_crt_bundle_attach;
            cfg.skip_cert_common_name_check = true;

            auto client = esp_http_client_init(&cfg);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_header(client, "Accept", "*/*");
            esp_http_client_set_post_field(client, body, (int)strlen(body));

            esp_err_t err  = esp_http_client_perform(client);
            int       status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            if (err == ESP_OK && (status == 200 || status == 201)) {
                ESP_LOGI(TAG, "Emergency call sent ok");
                return std::string("求救电话已成功拨出，救援正在赶来，请保持冷静");
            } else {
                ESP_LOGE(TAG, "Emergency call failed: err=%d status=%d", err, status);
                return std::string("求救电话拨打失败，请立即手动拨打120");
            }
        });

    // ===================== 睡眠模式：一键关闭所有设备 =====================
    server.AddTool(
        "sleep_mode",
        "【必须调用此工具】睡眠模式：当用户说'睡觉了''晚安''要睡了''睡了''去睡觉'等有睡眠意图时调用。"
        "【重要】调用此工具前，必须先对用户说一句话，例如'好的，正在为你检查所有设备'（可灵活表达，但必须先说再调用）。"
        "自动检查并关闭所有未关闭的设备（电视、水阀、气阀、总闸、大门（客厅门）、插座、音箱、窗帘），并返回所有设备的最终状态，你必须逐一向用户播报每个设备的状态。不能说所有设备已关闭",
        PropertyList(std::vector<Property>{}),
        [this](const PropertyList&) -> ReturnValue {
            std::vector<std::string> closed;   // 本次关闭的设备
            std::vector<std::string> skipped;  // 已经关闭的设备

            auto& ha_sl = HaConfig::GetInstance();
            std::string old_url   = ha_sl.ha_old_url();
            std::string old_tok   = ha_sl.ha_old_token();
            std::string new_url   = ha_sl.ha_new_url();
            std::string new_tok   = ha_sl.ha_new_token();

            // ── 老HA：电视 / 水阀 / 气阀 / 总闸 ──────────────────────
            struct SwitchDev { const char* name; std::string entity; };
            SwitchDev old_devs[] = {
                {"电视",   ha_sl.entity_tv()},
                {"水阀",   ha_sl.entity_water_valve()},
                {"气阀",   ha_sl.entity_gas_valve()},
                {"总闸",   ha_sl.entity_main_switch()},
            };
            for (auto& d : old_devs) {
                std::string s = GetEntityState(old_url.c_str(), old_tok.c_str(), d.entity.c_str());
                if (s == "on") {
                    CallService(old_url.c_str(), old_tok.c_str(), "switch", "turn_off", d.entity.c_str());
                    closed.push_back(d.name);
                } else if (s != "error" && s != "unknown") {
                    skipped.push_back(d.name);
                }
            }

            // ── 新HA：智能插座 ────────────────────────────────────────
            {
                std::string plug = ha_sl.entity_smart_plug();
                std::string s = GetEntityState(new_url.c_str(), new_tok.c_str(), plug.c_str());
                if (s == "on") {
                    CallService(new_url.c_str(), new_tok.c_str(), "switch", "turn_off", plug.c_str());
                    closed.push_back("智能插座");
                } else if (s == "off") {
                    skipped.push_back("智能插座");
                }
            }

            // ── 新HA：窗帘1 / 窗帘2 ──────────────────────────────────
            struct CoverDev { const char* name; std::string entity; };
            CoverDev covers[] = {
                {"窗帘1", ha_sl.entity_curtain_1()},
                {"窗帘2", ha_sl.entity_curtain_2()},
            };
            for (auto& c : covers) {
                // cover 状态：open / opening / closed / closing / stopped
                std::string s = GetEntityState(new_url.c_str(), new_tok.c_str(), c.entity.c_str());
                if (s != "closed" && s != "closing" && s != "error" && s != "unknown") {
                    cJSON* body = cJSON_CreateObject();
                    cJSON_AddStringToObject(body, "entity_id", c.entity.c_str());
                    char* bs = cJSON_PrintUnformatted(body);
                    CallLocalHaService("cover", "close_cover", bs);
                    free(bs);
                    cJSON_Delete(body);
                    closed.push_back(c.name);
                } else if (s == "closed") {
                    skipped.push_back(c.name);
                }
            }

            // ── 构建回复（完整报备所有设备状态）──────────────────────
            std::string result;
            if (!closed.empty()) {
                result += "本次已为你关闭：";
                for (size_t i = 0; i < closed.size(); i++) {
                    if (i > 0) result += "、";
                    result += closed[i];
                }
                result += "。";
            }
            if (!skipped.empty()) {
                result += "以下设备本来就是关闭状态：";
                for (size_t i = 0; i < skipped.size(); i++) {
                    if (i > 0) result += "、";
                    result += skipped[i];
                }
                result += "。";
            }
            if (result.empty()) {
                result = "家里所有设备都已确认关闭。";
            }
            result += "所有设备已全部确认安全，晚安好梦～";
            return result;
        });

    ESP_LOGI(TAG, "Curtain & Speaker Tools Registered.");

    // ===================== 提醒/闹钟工具 =====================
    server.AddTool(
        "set_reminder",
        "【必须调用此工具】设置提醒/闹钟，不可直接承诺而不调用。"
        "当用户说'X分钟后提醒我...'、'X小时后提醒我...'、'X点提醒我...'等时调用。"
        "AI需将时间转换为秒数（10分钟=600，1小时=3600，下午8点距现在的秒数）。"
        "message: 提醒内容（如'吃药'、'出门'）。delay_seconds: 从现在起多少秒后提醒。",
        PropertyList({
            Property("message",       kPropertyTypeString),
            Property("delay_seconds", kPropertyTypeString),
        }),
        [](const PropertyList& props) -> ReturnValue {
            try {
                std::string message  = props["message"].value<std::string>();
                std::string delay_s  = props["delay_seconds"].value<std::string>();
                int delay_sec = atoi(delay_s.c_str());

                if (message.empty()) return std::string("提醒内容不能为空");
                if (delay_sec <= 0)  return std::string("延迟时间必须大于0秒");

                int64_t trigger_us = esp_timer_get_time() + (int64_t)delay_sec * 1000000LL;
                int reminder_id = 0;
                {
                    std::lock_guard<std::mutex> lock(s_reminder_mutex);
                    reminder_id = ++s_reminder_id_counter;
                    s_reminders.push_back({reminder_id, trigger_us, message, false});
                }

                // 生成人性化时间描述
                int h   = delay_sec / 3600;
                int m   = (delay_sec % 3600) / 60;
                int s   = delay_sec % 60;
                std::string when;
                if (h > 0) when += std::to_string(h) + "小时";
                if (m > 0) when += std::to_string(m) + "分钟";
                if (s > 0 && h == 0) when += std::to_string(s) + "秒";

                ESP_LOGI(TAG, "Reminder set: id=%d in %ds → %s", reminder_id, delay_sec, message.c_str());
                return "好的，" + when + "后我会提醒你：" + message + "（编号：" + std::to_string(reminder_id) + "）";
            } catch (...) {
                return std::string("设置提醒失败，请检查参数");
            }
        });

    // ===================== 查询提醒列表 =====================
    server.AddTool(
        "list_reminders",
        "查询当前所有待触发的提醒列表，并显示剩余时间和编号。"
        "当用户问'我有什么提醒'、'我设了什么闹钟'、'有几个提醒'时调用。",
        PropertyList(std::vector<Property>()),
        [](const PropertyList&) -> ReturnValue {
            std::lock_guard<std::mutex> lock(s_reminder_mutex);
            if (s_reminders.empty()) {
                return std::string("当前没有待触发的提醒");
            }
            int64_t now_us = esp_timer_get_time();
            std::string result = "当前有" + std::to_string(s_reminders.size()) + "个提醒：";
            int idx = 1;
            for (auto& r : s_reminders) {
                int64_t rem_sec = (r.trigger_us - now_us) / 1000000LL;
                if (rem_sec < 0) rem_sec = 0;
                int h = (int)(rem_sec / 3600);
                int m = (int)((rem_sec % 3600) / 60);
                int s = (int)(rem_sec % 60);
                std::string when;
                if (h > 0) when += std::to_string(h) + "小时";
                if (m > 0) when += std::to_string(m) + "分钟";
                if (s > 0 && h == 0) when += std::to_string(s) + "秒";
                if (when.empty()) when = "即将触发";
                result += std::to_string(idx++) + ". " + when + "后：" + r.message
                        + "（编号：" + std::to_string(r.id) + "）；";
            }
            return result;
        });

    // ===================== 取消指定提醒 =====================
    server.AddTool(
        "cancel_reminder",
        "取消指定的提醒/闹钟。"
        "参数 keyword: 提醒内容关键词（如'吃药'）或提醒编号（如'3'）。"
        "当用户说'取消XXX提醒'、'取消编号N的提醒'时调用。",
        PropertyList({
            Property("keyword", kPropertyTypeString),
        }),
        [](const PropertyList& props) -> ReturnValue {
            try {
                std::string keyword = props["keyword"].value<std::string>();
                if (keyword.empty()) return std::string("请提供要取消的提醒内容关键词或编号");

                // 判断是否为纯数字（按编号取消）
                bool by_id = !keyword.empty();
                for (char c : keyword) {
                    if (!isdigit((unsigned char)c)) { by_id = false; break; }
                }
                int target_id = by_id ? atoi(keyword.c_str()) : 0;

                std::vector<std::string> cancelled;
                {
                    std::lock_guard<std::mutex> lock(s_reminder_mutex);
                    auto it = s_reminders.begin();
                    while (it != s_reminders.end()) {
                        bool match = by_id
                            ? (it->id == target_id)
                            : (it->message.find(keyword) != std::string::npos);
                        if (match) {
                            cancelled.push_back(it->message);
                            it = s_reminders.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                if (cancelled.empty()) return std::string("未找到匹配的提醒：") + keyword;
                std::string result = "已取消" + std::to_string(cancelled.size()) + "个提醒：";
                for (auto& msg : cancelled) result += msg + " ";
                return result;
            } catch (...) {
                return std::string("取消提醒失败，请检查参数");
            }
        });

    ESP_LOGI(TAG, "Reminder Tool Registered.");

    // ===================== 总台语音通话 =====================
    server.AddTool(
        "connect_station",
        "【必须调用此工具】连接总台语音通话。"
        "当用户说'接通总台'、'接通响应点'、'帮我给总台留言'、'呼叫总台'等有通话意图时调用。"
        "调用后设备进入对讲机模式：按住按键录音，松开自动发送给总台。",
        PropertyList(std::vector<Property>()),
        [](const PropertyList&) -> ReturnValue {
            if (IsStationCallActive()) {
                return std::string("已经在通话模式中，按住按键说话，松开发送。若要退出请说'结束通话'。");
            }
            StationCallConnect();
            return std::string("已连接总台通话！\n请按住按键说话，松开后自动发送给总台。\n等待总台回复时会自动播放。\n说'结束通话'可退出。");
        });

    server.AddTool(
        "play_station_reply",
        "播放总台发回的语音留言。"
        "当用户说'播放留言'、'听留言'、'播放总台回复'、'听听总台说什么'时调用。"
        "播放最新一条未听留言；若无留言则告知用户。",
        PropertyList(std::vector<Property>()),
        [](const PropertyList&) -> ReturnValue {
            ScReply reply;
            size_t remaining = 0;
            {
                std::lock_guard<std::mutex> lk(s_sc_reply_mutex);
                if (s_sc_replies.empty()) {
                    return std::string("当前没有未读留言。");
                }
                reply = s_sc_replies.back();   // 取最新一条
                s_sc_replies.pop_back();
                remaining = s_sc_replies.size();
            }
            // 后台下载并播放，避免阻塞 AI 响应
            struct PlayArg { std::string msg_id; std::string url; };
            auto* arg = new PlayArg{reply.msg_id, reply.url};
            xTaskCreate([](void* a) {
                auto* pa = (PlayArg*)a;
                ScDownloadAndPlay(pa->msg_id, pa->url);
                Application::GetInstance().Schedule([]() {
                    ScUpdateReplyBadge();
                });
                delete pa;
                vTaskDelete(nullptr);
            }, "sc_play", 8192, arg, 3, nullptr);

            std::string result = "正在播放总台留言...";
            if (remaining > 0) result += "还有" + std::to_string(remaining) + "条留言未播放。";
            return result;
        });

    server.AddTool(
        "disconnect_station",
        "断开总台通话连接，退出对讲机模式。"
        "当用户说'结束通话'、'挂断'、'退出通话模式'时调用。",
        PropertyList(std::vector<Property>()),
        [](const PropertyList&) -> ReturnValue {
            if (!IsStationCallActive()) {
                return std::string("当前未处于通话模式。");
            }
            StationCallDisconnect();
            return std::string("已断开总台通话，退出对讲机模式。");
        });

    ESP_LOGI(TAG, "Station Call Tools Registered.");

    // ===================== 紧急呼叫手机工具 =====================
    server.AddTool(
        "trigger_phone_call",
        "紧急呼叫手机。当用户说'打个电话给我'、'给我手机拨个电话'、'帮我找找手机'等时，或当摄像头找不到手机时，调用此工具向用户手机发送紧急呼叫信号。"
        "该工具会通过网络向ntfy.sh服务发送通知，手机端会收到标题为'Smart Home Call'、优先级最高的紧急呼叫信号，促使用户找到设备或手机。",
        PropertyList(std::vector<Property>()),
        [](const PropertyList&) -> ReturnValue {
            TriggerEmergencyCall();
            return std::string("好的，已向你的手机发送紧急呼叫信号！信号已发出，请查看你的手机。");
        });

    ESP_LOGI(TAG, "Emergency Call Tool Registered.");

    // ── 自定义设备工具（从 NVS ha_cfg::custom_devs 读取，重启后生效）──────────
    {
        auto custom_devs = HaConfig::GetInstance().GetCustomDevices();
        for (const auto& dev : custom_devs) {
            std::string tool_name = "control_" + dev.id;
            std::string desc = "控制" + dev.name + "（实体：" + dev.entity + "）";
            if (dev.domain == "cover")
                desc += "。action: open(打开)/close(关闭)/stop(停止)/query(查询)";
            else if (dev.domain == "climate")
                desc += "。action: on(打开)/off(关闭)/query(查询)";
            else if (dev.domain == "camera")
                desc += "。这是摄像头设备（不支持云台转动）。"
                        "查看画面: show_camera_on_screen(entity_id=\"" + dev.entity + "\")；"
                        "分析画面: describe_ha_camera(entity_id=\"" + dev.entity + "\")。"
                        "action仅支持: query(查询状态)。禁止调用 control_ha_camera。";
            else if (dev.domain == "camera_ptz")
                desc += "。这是支持云台转动的摄像头。"
                        "查看画面: show_camera_on_screen(entity_id=\"" + dev.entity + "\")；"
                        "分析画面: describe_ha_camera(entity_id=\"" + dev.entity + "\")；"
                        "云台控制: control_ha_camera(entity_id=\"" + dev.entity + "\", direction=上/下/左/右)。"
                        "action仅支持: query(查询状态)";
            else
                desc += "。action: on(打开)/off(关闭)/query(查询)";

            server.AddTool(tool_name, desc,
                PropertyList({
                    Property("action", kPropertyTypeString),
                }),
                [dev](const PropertyList& props) -> ReturnValue {
                    try {
                        auto& ha = HaConfig::GetInstance();
                        std::string url   = (dev.ha == "old") ? ha.ha_old_url()   : ha.ha_new_url();
                        std::string token = (dev.ha == "old") ? ha.ha_old_token() : ha.ha_new_token();
                        std::string action = props["action"].value<std::string>();

                        if (action == "query") {
                            std::string state = MyHomeDevice::GetInstance().GetEntityState(
                                url.c_str(), token.c_str(), dev.entity.c_str());
                            return dev.name + "当前状态：" + state;
                        }

                        if (dev.domain == "camera" || dev.domain == "camera_ptz") {
                            return std::string("摄像头设备请用 show_camera_on_screen 或 describe_ha_camera，传入 entity_id=") + dev.entity;
                        }

                        std::string svc;
                        if (dev.domain == "cover") {
                            if      (action == "open"  || action == "on")  svc = "open_cover";
                            else if (action == "close" || action == "off") svc = "close_cover";
                            else if (action == "stop")                     svc = "stop_cover";
                        } else {
                            if      (action == "on")  svc = "turn_on";
                            else if (action == "off") svc = "turn_off";
                        }
                        if (svc.empty()) return std::string("不支持的操作: ") + action;

                        MyHomeDevice::GetInstance().CallService(
                            url.c_str(), token.c_str(),
                            dev.domain.c_str(), svc.c_str(), dev.entity.c_str());

                        std::string act_cn = (action=="on"||action=="open") ? "打开" :
                                             (action=="off"||action=="close") ? "关闭" : "停止";
                        return "好的，已" + act_cn + dev.name;
                    } catch (...) {
                        return std::string("操作失败");
                    }
                });
            ESP_LOGI(TAG, "Custom device tool registered: %s", tool_name.c_str());
        }
    }

    // 启动后台监控任务（跌倒检测和门磁监控暂时禁用）
    // StartFallDetectionMonitor();
    StartReminderTask();
    // StartDoorMonitor();
}

// ============================================================
//  跌倒检测：共用工具函数
// ============================================================

// 从 HA camera proxy 下载 JPEG（用 esp_http_client 避免 8KB 缓冲死锁）
static bool DownloadJpegFromHA(std::string& jpeg_data, const char* url, const std::string& token) {
    jpeg_data.clear();
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.method            = HTTP_METHOD_GET;
    cfg.timeout_ms        = 12000;
    cfg.buffer_size       = 8192;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data)
            ((std::string*)evt->user_data)->append((const char*)evt->data, evt->data_len);
        return ESP_OK;
    };
    cfg.user_data = &jpeg_data;

    std::string dl_auth = "Bearer " + (token.empty() ? HaConfig::GetInstance().ha_camera_token() : token);
    auto client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", dl_auth.c_str());
    esp_err_t err = esp_http_client_perform(client);
    int status   = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || jpeg_data.empty()) {
        ESP_LOGE(TAG, "DownloadJpegFromHA failed: err=%d status=%d size=%d", err, status, (int)jpeg_data.size());
        return false;
    }
    ESP_LOGI(TAG, "DownloadJpegFromHA ok: %d bytes", (int)jpeg_data.size());
    return true;
}

// 将 JPEG 数据发给 AI 视觉服务，返回描述文本
static bool AnalyzeJpeg(const std::string& jpeg_data, const std::string& question, std::string& result) {
    if (s_vision_url.empty()) { result = ""; return false; }
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp();
    std::string boundary = "----ESP32_FALL_BOUNDARY";
    http->SetHeader("Device-Id",  SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id",  Board::GetInstance().GetUuid().c_str());
    if (!s_vision_token.empty())
        http->SetHeader("Authorization", "Bearer " + s_vision_token);
    http->SetHeader("Content-Type",      "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");

    if (!http->Open("POST", s_vision_url)) { return false; }

    std::string q = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n" + question + "\r\n";
    http->Write(q.c_str(), q.size());
    std::string fh = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"cam.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    http->Write(fh.c_str(), fh.size());
    http->Write(jpeg_data.c_str(), jpeg_data.size());
    std::string ft = "\r\n--" + boundary + "--\r\n";
    http->Write(ft.c_str(), ft.size());
    http->Write("", 0);

    if (http->GetStatusCode() != 200) { http->Close(); return false; }
    result = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "AnalyzeJpeg result: %s", result.c_str());
    return !result.empty();
}

// 解码 JPEG 并显示到屏幕（PSRAM 分配，240×240 圆屏）
static void ShowJpegOnDisplay(const std::string& jpeg_data) {
    auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (!display) return;

    // 解码 JPEG → RGB565
    jpeg_dec_config_t dec_cfg = {};
    dec_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    dec_cfg.rotate      = JPEG_ROTATE_0D;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&dec_cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed");
        return;
    }

    jpeg_dec_io_t io = {};
    io.inbuf     = (uint8_t*)jpeg_data.data();
    io.inbuf_len = jpeg_data.size();

    jpeg_dec_header_info_t hdr = {};
    if (jpeg_dec_parse_header(dec, &io, &hdr) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_parse_header failed");
        jpeg_dec_close(dec);
        return;
    }

    int w = hdr.width, h = hdr.height;
    size_t rgb_size = (size_t)w * h * 2;
    // outbuf 必须 16 字节对齐（esp_jpeg_dec 要求）
    uint8_t* rgb = (uint8_t*)heap_caps_aligned_alloc(16, rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb) {
        ESP_LOGE(TAG, "OOM for RGB565 buf (%d bytes), need PSRAM", (int)rgb_size);
        jpeg_dec_close(dec);
        return;
    }

    io.outbuf   = rgb;
    io.out_size = rgb_size;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_process failed");
        heap_caps_free(rgb);
        jpeg_dec_close(dec);
        return;
    }
    jpeg_dec_close(dec);
    ESP_LOGI(TAG, "JPEG decoded: %dx%d → %d bytes RGB565", w, h, (int)rgb_size);

    // LvglAllocatedImage 接管 rgb 内存所有权（析构时 free）
    try {
        auto img = std::make_unique<LvglAllocatedImage>(rgb, rgb_size, w, h, w * 2, LV_COLOR_FORMAT_RGB565);
        display->SetPreviewImage(std::move(img));
    } catch (...) {
        heap_caps_free(rgb);
    }
}

// ============================================================
//  跌倒检测后台任务 [暂时禁用]
// ============================================================
/*
static void FallDetectionMonitorTask(void*) {
    static const int POLL_MS     = FALL_DETECT_POLL_SEC     * 1000;
    static const int COOLDOWN_MS = FALL_DETECT_COOLDOWN_SEC * 1000;

    // last_alert_ms：上次报警时间（报警后冷却 120s 才再次报警）
    int64_t last_alert_ms = -(int64_t)COOLDOWN_MS;

    ESP_LOGI(TAG, "FallDetectionMonitor started, poll=%ds alert_cooldown=%ds",
             FALL_DETECT_POLL_SEC, FALL_DETECT_COOLDOWN_SEC);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        // 视觉服务未就绪则等待
        if (s_vision_url.empty()) continue;

        // 1. 查询 Person Detection 传感器状态
        bool person_on = false;
        {
            std::string resp;
            esp_http_client_config_t cfg = {};
            cfg.url           = HA_CAMERA_MOTION_URL;
            cfg.method        = HTTP_METHOD_GET;
            cfg.timeout_ms    = 5000;
            cfg.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
                if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data)
                    ((std::string*)evt->user_data)->append((const char*)evt->data, evt->data_len);
                return ESP_OK;
            };
            cfg.user_data = &resp;
            auto client = esp_http_client_init(&cfg);
            esp_http_client_set_header(client, "Authorization", "Bearer " HA_CAMERA_TOKEN);
            esp_err_t err = esp_http_client_perform(client);
            esp_http_client_cleanup(client);

            if (err == ESP_OK && !resp.empty()) {
                auto* root  = cJSON_Parse(resp.c_str());
                auto* state = root ? cJSON_GetObjectItem(root, "state") : nullptr;
                if (cJSON_IsString(state))
                    person_on = (strcmp(state->valuestring, "on") == 0);
                cJSON_Delete(root);
            }
        }

        // 2. 没人就不分析
        if (!person_on) continue;

        ESP_LOGI(TAG, "FallDetect: motion detected, downloading frame...");

        // 3. 下载缩图（480px宽够 AI 分析，避免解码 OOM）
        std::string jpeg_data;
        if (!DownloadJpegFromHA(jpeg_data, HA_CAMERA_PROXY_SMALL_URL)) continue;

        // 4. AI 分析是否有人跌倒
        std::string analysis;
        std::string question = "请仔细分析画面，判断是否有人跌倒、摔倒、倒地或出现异常姿态。"
                               "如果发现有人跌倒，请以'【跌倒警报】'开头详细描述情况；"
                               "如果画面正常，只需回复'正常'两字。";
        if (!AnalyzeJpeg(jpeg_data, question, analysis)) continue;

        // 5. 判断是否触发跌倒警报
        bool fall = analysis.find("跌倒警报") != std::string::npos ||
                    analysis.find("跌倒")     != std::string::npos ||
                    analysis.find("摔倒")     != std::string::npos ||
                    analysis.find("倒地")     != std::string::npos ||
                    analysis.find("摔跤")     != std::string::npos ||
                    analysis.find("倒下")     != std::string::npos;
        if (!fall) {
            ESP_LOGI(TAG, "FallDetect: no fall, analysis=%s", analysis.c_str());
            continue;
        }

        ESP_LOGW(TAG, "FallDetect: FALL DETECTED! %s", analysis.c_str());

        // 6. 报警冷却：120s 内不重复报警
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_alert_ms < COOLDOWN_MS) {
            ESP_LOGW(TAG, "FallDetect: alert cooldown, skip");
            continue;
        }
        last_alert_ms = now_ms;

        // 7. 连续播放提示音三次
        {
            auto& audio = Application::GetInstance().GetAudioService();
            for (int i = 0; i < 3; i++) {
                audio.PlaySound(Lang::Sounds::OGG_VIBRATION);
                vTaskDelay(pdMS_TO_TICKS(600));  // 原1200ms，缩短为600ms更及时
            }
        }
    }
    vTaskDelete(nullptr);
}

void StartFallDetectionMonitor() {
    xTaskCreate(FallDetectionMonitorTask, "fall_detect", 8192, nullptr, 1, nullptr);
}
*/

// ============================================================
//  门磁监控后台任务 [暂时禁用]
// ============================================================
/*
static void DoorMonitorTask(void*) {
    static const int POLL_MS     = 2000;   // 每 2 秒轮询一次（原3秒，提高响应速度）
    static const int COOLDOWN_MS = 30000;  // 报警后 30 秒内不重复提醒

    bool last_open = false;              // 上次检测到的状态（false=关, true=开）
    bool initialized = false;           // 第一次采样只记录状态，不报警
    int64_t last_alert_ms = -(int64_t)COOLDOWN_MS;

    ESP_LOGI(TAG, "DoorMonitor started, poll=2s cooldown=30s");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        // 拉取门磁状态
        std::string state_raw = MyHomeDevice::GetInstance().GetEntityState(
            HA_NEW_URL, HA_NEW_TOKEN, ENTITY_DOOR_SENSOR);

        if (state_raw.empty()) continue;

        // binary_sensor contact: on=接触=门关闭, off=无接触=门打开
        bool door_open = (state_raw == "off");

        if (!initialized) {
            last_open = door_open;
            initialized = true;
            continue;
        }

        // 门从关→开，且已过冷却时间，才报警
        if (door_open && !last_open) {
            int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000);
            if (now_ms - last_alert_ms >= COOLDOWN_MS) {
                last_alert_ms = now_ms;
                ESP_LOGI(TAG, "Door opened! Playing alert sound.");
                auto& audio = Application::GetInstance().GetAudioService();
                for (int i = 0; i < 3; i++) {
                    audio.PlaySound(Lang::Sounds::OGG_VIBRATION);
                    vTaskDelay(pdMS_TO_TICKS(600));  // 原1200ms，缩短为600ms更及时
                }
            }
        }

        last_open = door_open;
    }
}

void StartDoorMonitor() {
    xTaskCreate(DoorMonitorTask, "door_monitor", 4096, nullptr, 1, nullptr);
}
*/

// =================================================================================
// 紧急呼叫手机功能（发送HTTP请求到ntfy.sh）
// =================================================================================

void TriggerEmergencyCall() {
    // 发送紧急呼叫到ntfy.sh服务
    const char* topic = "iQP7X0XVYOVrSIAA";
    const char url_template[] = "https://ntfy.sh/%s";
    char url[256];
    snprintf(url, sizeof(url), url_template, topic);

    ESP_LOGI(TAG, "TriggerEmergencyCall: sending request to %s", url);

    // 创建HTTP客户端
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "TriggerEmergencyCall: Failed to initialize HTTP client");
        return;
    }

    // 设置请求头
    esp_http_client_set_header(client, "Title", "Smart Home Call");
    esp_http_client_set_header(client, "Priority", "5");
    esp_http_client_set_header(client, "Tags", "phone,warning");

    // 消息内容
    const char* message = "智能管家呼叫！";
    
    // 设置POST数据（UTF-8编码）
    esp_http_client_set_post_field(client, message, strlen(message));

    // 执行请求
    esp_err_t err = esp_http_client_perform(client);
    
    // 获取状态码
    int status_code = esp_http_client_get_status_code(client);
    
    // 清理资源
    esp_http_client_cleanup(client);

    // 返回结果
    if (err == ESP_OK && status_code == 200) {
        ESP_LOGI(TAG, "TriggerEmergencyCall: ✅ 紧急呼叫已发送，状态码: %d", status_code);
        
        // 屏幕显示反馈信息
        Application::GetInstance().Schedule([]() {
            auto* display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ShowNotification("☎️ 已向手机发送呼叫信号", 3000);
            }
        });
    } else {
        ESP_LOGE(TAG, "TriggerEmergencyCall: ❌ 紧急呼叫发送失败，HTTP错误: %s, 状态码: %d", 
                 esp_err_to_name(err), status_code);
        
        // 屏幕显示错误信息
        Application::GetInstance().Schedule([]() {
            auto* display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ShowNotification("❌ 手机呼叫失败，请检查网络", 3000);
            }
        });
    }
}
