#include "my_home_device.h"
#include <mcp_server.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <esp_log.h>
#include <string.h>
#include <atomic>
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

#define TAG "HomeDevice"

// AI 视觉服务器（从服务端 capabilities 里获取，由 McpServer::ParseCapabilities 注入）
static std::string s_vision_url;
static std::string s_vision_token;

// 前向声明（实现在文件末尾）
static bool DownloadJpegFromHA(std::string& jpeg_data, const char* url = HA_CAMERA_PROXY_URL);
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
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 5000;
    config.event_handler = _http_event_handler;
    config.user_data = &response_buffer;

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
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5000;

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
    char url[256];
    snprintf(url, sizeof(url), "http://192.168.1.104:8123/api/services/%s/%s", domain, service);

    esp_http_client_config_t cfg = {};
    cfg.url        = url;
    cfg.method     = HTTP_METHOD_POST;
    cfg.timeout_ms = 5000;

    auto client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer " HA_CAMERA_TOKEN);
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
// Part 1.5: 提醒/闹钟
// =================================================================================

struct Reminder {
    int64_t trigger_us;   // esp_timer_get_time() 到期时间（微秒）
    std::string message;
    bool fired;
};

static std::mutex          s_reminder_mutex;
static std::vector<Reminder> s_reminders;

static void ReminderTask(void*) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));  // 每 5 秒检查一次

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
            auto& audio = Application::GetInstance().GetAudioService();
            for (int i = 0; i < 3; i++) {
                audio.PlaySound(Lang::Sounds::OGG_VIBRATION);
                vTaskDelay(pdMS_TO_TICKS(1200));
            }
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

                const char* target_entity = nullptr;
                const char* target_url = nullptr;
                const char* target_token = nullptr;
                std::string device_name_cn = "";
                std::string domain = "switch";

                // --- 路由逻辑 ---
                if (device == "plug") {
                    target_entity = ENTITY_SMART_PLUG;
                    device_name_cn = "智能插座";
                    target_url = HA_NEW_URL;
                    target_token = HA_NEW_TOKEN;
                }
                else if (device == "door") {
                    target_entity = ENTITY_DOOR_SENSOR;
                    device_name_cn = "大门传感器";
                    domain = "binary_sensor";
                    target_url = HA_NEW_URL;
                    target_token = HA_NEW_TOKEN;
                }
                else if (device == "tv") {
                    target_entity = ENTITY_TV;
                    device_name_cn = "电视";
                    target_url = HA_OLD_URL;
                    target_token = HA_OLD_TOKEN;
                }
                else if (device == "water_valve") {
                    target_entity = ENTITY_WATER_VALVE;
                    device_name_cn = "水阀";
                    target_url = HA_OLD_URL;
                    target_token = HA_OLD_TOKEN;
                }
                else if (device == "gas_valve") {
                    target_entity = ENTITY_GAS_VALVE;
                    device_name_cn = "气阀";
                    target_url = HA_OLD_URL;
                    target_token = HA_OLD_TOKEN;
                }
                else if (device == "main_switch") {
                    target_entity = ENTITY_MAIN_SWITCH;
                    device_name_cn = "总闸";
                    target_url = HA_OLD_URL;
                    target_token = HA_OLD_TOKEN;
                }

                if (!target_entity) {
                    return std::string("错误: 找不到该设备。");
                }

                // 1. 查询
                if (action == "query") {
                    std::string state_raw = GetEntityState(target_url, target_token, target_entity);
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

                CallService(target_url, target_token, domain.c_str(), service.c_str(), target_entity);
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
        "控制客厅TP-Link摄像头云台。参数: direction(上/下/左/右), distance(移动距离0.1~1.0, 默认0.3), speed(速度0.1~1.0, 默认0.5)",
        PropertyList({
            Property("direction", kPropertyTypeString),
            Property("distance", kPropertyTypeString, std::string("0.3")),
            Property("speed",    kPropertyTypeString, std::string("0.5")),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            try {
                std::string direction = properties["direction"].value<std::string>();
                float distance = std::stof(properties["distance"].value<std::string>());
                float speed    = std::stof(properties["speed"].value<std::string>());

                std::string axis, value;
                if (direction == "上")       { axis = "tilt"; value = "DOWN"; }
                else if (direction == "下")  { axis = "tilt"; value = "UP"; }
                else if (direction == "左")  { axis = "pan";  value = "LEFT"; }
                else if (direction == "右")  { axis = "pan";  value = "RIGHT"; }
                else return std::string("错误: 不支持的方向，请用 上/下/左/右");

                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "entity_id", HA_CAMERA_ENTITY);
                cJSON_AddStringToObject(root, axis.c_str(), value.c_str());
                cJSON_AddNumberToObject(root, "distance", distance);
                cJSON_AddNumberToObject(root, "speed", speed);
                const char* post_data = cJSON_PrintUnformatted(root);

                esp_http_client_config_t config = {};
                config.url = HA_CAMERA_PTZ_URL;
                config.method = HTTP_METHOD_POST;
                config.timeout_ms = 5000;

                esp_http_client_handle_t client = esp_http_client_init(&config);
                esp_http_client_set_header(client, "Content-Type", "application/json");
                esp_http_client_set_header(client, "Authorization", "Bearer " HA_CAMERA_TOKEN);
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
        "查看客厅摄像头并让AI描述画面内容。当用户问'客厅有什么'、'客厅什么情况'、'客厅有没有人'、'门口什么情况'、'帮我看看监控'等时调用。"
        "【重要】调用此工具前，必须先对用户说一句话，例如'稍等，我来分析下客厅环境'（可灵活表达，但必须先说再调用）。",
        PropertyList({
            Property("question", kPropertyTypeString, std::string("请详细描述画面中看到了什么，有没有人，有哪些物体"))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            try {
                if (s_vision_url.empty()) {
                    return std::string("AI视觉服务未配置，无法分析画面");
                }
                auto question = properties["question"].value<std::string>();
                auto network = Board::GetInstance().GetNetwork();

                // Step 1: 用 esp_http_client 下载 JPEG（支持大型二进制响应，避免 HttpClient 8KB 缓冲死锁）
                std::string jpeg_data;
                {
                    esp_http_client_config_t dl_config = {};
                    dl_config.url = HA_CAMERA_PROXY_URL;
                    dl_config.method = HTTP_METHOD_GET;
                    dl_config.timeout_ms = 15000;
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
                    esp_http_client_set_header(dl_client, "Authorization", "Bearer " HA_CAMERA_TOKEN);

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
        "把客厅摄像头当前画面截图并显示到设备屏幕上。当用户说'把画面传到屏幕'、"
        "'显示监控画面'、'在屏幕上看'、'让我看看画面'等时调用。",
        PropertyList(std::vector<Property>{}),
        [](const PropertyList&) -> ReturnValue {
            try {
                std::string jpeg_data;
                // 用缩图 URL（480px宽），避免解码 7MB+ 大图造成 OOM
                if (!DownloadJpegFromHA(jpeg_data, HA_CAMERA_PROXY_SMALL_URL))
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
                std::vector<const char*> entities;
                if (curtain == "1")   entities = { ENTITY_CURTAIN_1 };
                else if (curtain == "2") entities = { ENTITY_CURTAIN_2 };
                else                  entities = { ENTITY_CURTAIN_1, ENTITY_CURTAIN_2 };

                const char* svc = nullptr;
                if      (action == "open")  svc = "open_cover";
                else if (action == "close") svc = "close_cover";
                else if (action == "stop")  svc = "stop_cover";
                else if (action == "set")   svc = "set_cover_position";
                else return std::string("不支持的动作，请用 open/close/stop/set");

                for (const char* eid : entities) {
                    cJSON* body = cJSON_CreateObject();
                    cJSON_AddStringToObject(body, "entity_id", eid);
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

                cJSON* body = cJSON_CreateObject();
                cJSON_AddStringToObject(body, "entity_id", ENTITY_SPEAKER);

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

                const char* entity_id = (mode == "command") ? ENTITY_SPEAKER_CMD : ENTITY_SPEAKER_TTS;

                cJSON* body = cJSON_CreateObject();
                cJSON_AddStringToObject(body, "entity_id", entity_id);
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

            // ── 老HA：电视 / 水阀 / 气阀 / 总闸 ──────────────────────
            struct SwitchDev { const char* name; const char* entity; };
            SwitchDev old_devs[] = {
                {"电视",   ENTITY_TV},
                {"水阀",   ENTITY_WATER_VALVE},
                {"气阀",   ENTITY_GAS_VALVE},
                {"总闸",   ENTITY_MAIN_SWITCH},
            };
            for (auto& d : old_devs) {
                std::string s = GetEntityState(HA_OLD_URL, HA_OLD_TOKEN, d.entity);
                if (s == "on") {
                    CallService(HA_OLD_URL, HA_OLD_TOKEN, "switch", "turn_off", d.entity);
                    closed.push_back(d.name);
                } else if (s != "error" && s != "unknown") {
                    skipped.push_back(d.name);
                }
            }

            // ── 新HA：智能插座 ────────────────────────────────────────
            {
                std::string s = GetEntityState(HA_NEW_URL, HA_NEW_TOKEN, ENTITY_SMART_PLUG);
                if (s == "on") {
                    CallService(HA_NEW_URL, HA_NEW_TOKEN, "switch", "turn_off", ENTITY_SMART_PLUG);
                    closed.push_back("智能插座");
                } else if (s == "off") {
                    skipped.push_back("智能插座");
                }
            }

            // ── 新HA：窗帘1 / 窗帘2 ──────────────────────────────────
            struct CoverDev { const char* name; const char* entity; };
            CoverDev covers[] = {
                {"窗帘1", ENTITY_CURTAIN_1},
                {"窗帘2", ENTITY_CURTAIN_2},
            };
            for (auto& c : covers) {
                // cover 状态：open / opening / closed / closing / stopped
                std::string s = GetEntityState(HA_NEW_URL, HA_CAMERA_TOKEN, c.entity);
                if (s != "closed" && s != "closing" && s != "error" && s != "unknown") {
                    cJSON* body = cJSON_CreateObject();
                    cJSON_AddStringToObject(body, "entity_id", c.entity);
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
                {
                    std::lock_guard<std::mutex> lock(s_reminder_mutex);
                    s_reminders.push_back({trigger_us, message, false});
                }

                // 生成人性化时间描述
                int h   = delay_sec / 3600;
                int m   = (delay_sec % 3600) / 60;
                int s   = delay_sec % 60;
                std::string when;
                if (h > 0) when += std::to_string(h) + "小时";
                if (m > 0) when += std::to_string(m) + "分钟";
                if (s > 0 && h == 0) when += std::to_string(s) + "秒";

                ESP_LOGI(TAG, "Reminder set: in %ds → %s", delay_sec, message.c_str());
                return "好的，" + when + "后我会提醒你：" + message;
            } catch (...) {
                return std::string("设置提醒失败，请检查参数");
            }
        });

    ESP_LOGI(TAG, "Reminder Tool Registered.");

    // 启动后台跌倒检测任务、提醒任务和门磁监控任务
    StartFallDetectionMonitor();
    StartReminderTask();
    StartDoorMonitor();
}

// ============================================================
//  跌倒检测：共用工具函数
// ============================================================

// 从 HA camera proxy 下载 JPEG（用 esp_http_client 避免 8KB 缓冲死锁）
static bool DownloadJpegFromHA(std::string& jpeg_data, const char* url) {
    jpeg_data.clear();
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 12000;
    cfg.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data)
            ((std::string*)evt->user_data)->append((const char*)evt->data, evt->data_len);
        return ESP_OK;
    };
    cfg.user_data = &jpeg_data;
    cfg.buffer_size = 8192;

    auto client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", "Bearer " HA_CAMERA_TOKEN);
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
//  跌倒检测后台任务
// ============================================================
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
                vTaskDelay(pdMS_TO_TICKS(1200));
            }
        }
    }
    vTaskDelete(nullptr);
}

void StartFallDetectionMonitor() {
    xTaskCreate(FallDetectionMonitorTask, "fall_detect", 8192, nullptr, 1, nullptr);
}

// ============================================================
//  门磁监控后台任务：门打开时触发语音提醒
// ============================================================
static void DoorMonitorTask(void*) {
    static const int POLL_MS     = 3000;   // 每 3 秒轮询一次
    static const int COOLDOWN_MS = 30000;  // 报警后 30 秒内不重复提醒

    bool last_open = false;              // 上次检测到的状态（false=关, true=开）
    bool initialized = false;           // 第一次采样只记录状态，不报警
    int64_t last_alert_ms = -(int64_t)COOLDOWN_MS;

    ESP_LOGI(TAG, "DoorMonitor started, poll=3s cooldown=30s");

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
                    vTaskDelay(pdMS_TO_TICKS(1200));
                }
            }
        }

        last_open = door_open;
    }
}

void StartDoorMonitor() {
    xTaskCreate(DoorMonitorTask, "door_monitor", 4096, nullptr, 1, nullptr);
}
