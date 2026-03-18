#include "my_home_device.h"
#include <mcp_server.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <esp_log.h>
#include <string.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "board.h"
#include "system_info.h"
#include <network_interface.h>
#include <stdexcept>

#define TAG "HomeDevice"

// AI 视觉服务器（从服务端 capabilities 里获取，由 McpServer::ParseCapabilities 注入）
static std::string s_vision_url;
static std::string s_vision_token;

void SetVisionUrl(const std::string& url, const std::string& token) {
    s_vision_url = url;
    s_vision_token = token;
    ESP_LOGI(TAG, "Vision URL set: %s", url.c_str());
}

// 报警状态：armed=AI已调用trigger_alarm(等TTS结束才开始倒计时), pending=倒计时进行中
static std::atomic<bool> alarm_armed(false);
static std::atomic<bool> alarm_pending(false);
static std::atomic<bool> alarm_cancelled(false);

void CancelAlarm() {
    if (alarm_armed.load() || alarm_pending.load()) {
        alarm_armed.store(false);
        alarm_cancelled.store(true);
        alarm_pending.store(false);
        ESP_LOGI(TAG, "STT检测到取消词，报警已直接取消");
    }
}

void ArmAlarm() {
    if (!alarm_armed.load() && !alarm_pending.load()) {
        alarm_armed.store(true);
        alarm_cancelled.store(false);
        ESP_LOGW(TAG, "检测到紧急词，报警已就绪（等AI说完后开始6秒倒计时）");
    }
}

void StartAlarmCountdown() {
    if (!alarm_armed.load()) return;  // 未就绪
    alarm_armed.store(false);
    alarm_pending.store(true);
    alarm_cancelled.store(false);
    ESP_LOGW(TAG, "报警倒计时6秒，可说'取消报警'来取消...");

    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(6000));

        if (alarm_cancelled.load()) {
            ESP_LOGI(TAG, "报警任务：已被取消，不发送");
            alarm_pending.store(false);
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGW(TAG, "正在发送报警请求...");
        const char* alarm_url = "https://admin.11yzh.com/api/hotline/openarchives";

        esp_http_client_config_t config = {};
        config.url = alarm_url;
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = 10000;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.skip_cert_common_name_check = true;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "User-Agent", "Apifox/1.0.0 (https://apifox.com)");
        esp_http_client_set_header(client, "Accept", "*/*");

        const char* post_data = "{\"eps400\":\"4000000126\",\"caller\":\"19808555455\"}";
        esp_http_client_set_post_field(client, post_data, strlen(post_data));

        esp_err_t err = esp_http_client_perform(client);
        int status_code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK && status_code == 200) {
            ESP_LOGW(TAG, "✅ 报警成功发送，状态码: %d", status_code);
        } else {
            ESP_LOGE(TAG, "❌ 报警发送失败: %s, 状态码: %d", esp_err_to_name(err), status_code);
        }
        alarm_pending.store(false);
        vTaskDelete(NULL);
    }, "alarm_task", 8192, nullptr, 3, nullptr);
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
                    if (state_raw == "on") state_cn = "打开";
                    else if (state_raw == "off") state_cn = "关闭";
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

    // ===================== 报警工具 =====================
    // 取消报警工具
    server.AddTool(
        "cancel_alarm",
        "当用户说取消报警、不需要报警、没事了时，调用此工具取消即将发出的报警",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            if (alarm_armed.load() || alarm_pending.load()) {
                alarm_armed.store(false);
                alarm_cancelled.store(true);
                alarm_pending.store(false);
                ESP_LOGI(TAG, "报警已取消");
                return std::string("好的，报警已取消，如有需要随时告诉我。");
            }
            return std::string("当前没有待发送的报警。");
        });

    // 触发报警工具：调用后等 AI 说完话(tts stop)再开始6秒倒计时
    server.AddTool(
        "trigger_alarm",
        "当用户确认需要报警、呼救、紧急求助时调用。AI说完话后开始6秒倒计时，期间用户可说'取消报警'取消。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            if (alarm_armed.load() || alarm_pending.load()) {
                return std::string("报警已在准备中，请稍候。");
            }
            alarm_armed.store(true);
            ESP_LOGW(TAG, "报警已就绪，等待AI说完后开始倒计时...");
            return std::string("好的，我说完后将为你启动6秒报警倒计时！如果不需要请说'取消报警'。救援人员很快就会联系你，请保持镇定！");
        });

    ESP_LOGI(TAG, "Alarm Tools Registered.");

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
                if (direction == "上")       { axis = "tilt"; value = "UP"; }
                else if (direction == "下")  { axis = "tilt"; value = "DOWN"; }
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
        "查看客厅摄像头并让AI描述画面内容。当用户问'客厅有没有人'、'门口什么情况'、'帮我看看监控'等时调用。",
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

    ESP_LOGI(TAG, "HA Camera Tools Registered.");
}
