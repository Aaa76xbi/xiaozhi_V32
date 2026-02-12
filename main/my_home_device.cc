#include "my_home_device.h"
#include "application.h"
#include <mcp_server.h>
#include <esp_http_client.h>
#include <esp_websocket_client.h>
#include <cJSON.h>
#include <esp_log.h>
#include <string.h>
#include <esp_timer.h>
#include "protocols/protocol.h"
#include "board.h"

#define TAG "HomeDevice"
#define MON_TAG "ElderlyMonitor"

// =================================================================================
// Part 1: Home Assistant 基础功能 (HTTP Helper)
// =================================================================================

// HTTP 事件处理
esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
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

// 获取设备状态
std::string MyHomeDevice::GetEntityState(const char* base_url, const char* token, const char* entity_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/states/%s", base_url, entity_id);

    // ESP_LOGI(TAG, "Querying State: %s", url); // 减少日志刷屏

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

// 调用服务 (控制设备)
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
// Part 2: 老人看护监控 (AI 联动版)
// =================================================================================

// 【核心函数】上报状态给 AI 管家
// ✅ 修改后：先检查 Session ID，没有连接好就不发，保护连接

// 修改 main/my_home_device.cc

// 在 main/my_home_device.cc 中

void MyHomeDevice::ReportStatusToAI(const std::string& content, bool is_urgent) {
    ESP_LOGI(MON_TAG, "📤 [上报 AI] 事件: %s | 紧急: %d", content.c_str(), is_urgent);

    auto* app = &Application::GetInstance();
    
    if (app) {
        // ---------------------------------------------------------
        // 策略：门磁触发只做本地语音播报，不发送给 AI 对话，防止断网。
        // ---------------------------------------------------------

        if (is_urgent) {
            ESP_LOGI(MON_TAG, "🔊 触发本地提醒 (门开了)");
            
            // ✅ 1. 强制设置最大音量 (0-100)
            // 修改说明：SetOutputVolume 是 AudioCodec 的方法，必须通过 Board 获取
            auto* codec = Board::GetInstance().GetAudioCodec();
            if (codec) {
                codec->SetOutputVolume(100); 
            }
            
            // ✅ 2. 播放本地音频 (建议连续播两次以防吞音)
            app->PlaySound("common/exclamation.ogg");
            // vTaskDelay(pdMS_TO_TICKS(500)); // 可选：间隔一下
            // app->PlaySound("common/exclamation.ogg"); // 可选：再播一次加强提醒
        }

        // 屏蔽网络发送，只保留本地提醒
        ESP_LOGW(MON_TAG, "🚫 为了保持连接稳定，本次仅本地提醒，不发送给 AI");
    }
}
// 自动登录获取 UID
bool MyHomeDevice::PerformMonitorLogin() {
    ESP_LOGI(MON_TAG, "正在登录监控平台...");

    char post_data[128];
    snprintf(post_data, sizeof(post_data), "user=%s&password=%s", MONITOR_USER, MONITOR_PASS);

    esp_http_client_config_t config = {};
    config.url = MONITOR_LOGIN_URL;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    config.cert_pem = NULL;
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded;charset=UTF-8");
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (ESP32)");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    bool success = false;

    if (err == ESP_OK) {
        int content_len = esp_http_client_get_content_length(client);
        if (content_len > 0) {
            char *buffer = (char *)malloc(content_len + 1);
            if (buffer) {
                if (esp_http_client_read(client, buffer, content_len) > 0) {
                    buffer[content_len] = 0; 
                    cJSON *root = cJSON_Parse(buffer);
                    if (root) {
                        cJSON *code = cJSON_GetObjectItem(root, "code");
                        cJSON *data = cJSON_GetObjectItem(root, "data");
                        if (code && code->valueint == 1 && data && cJSON_IsString(data)) {
                            this->current_uid = std::string(data->valuestring);
                            ESP_LOGI(MON_TAG, "✅ 登录成功! UID已获取");
                            success = true;
                        }
                        cJSON_Delete(root);
                    }
                }
                free(buffer);
            }
        }
    } else {
        ESP_LOGE(MON_TAG, "登录请求失败: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}

// WebSocket 事件处理
void MyHomeDevice::WebSocketEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    MyHomeDevice* self = (MyHomeDevice*)handler_args;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            ESP_LOGI(MON_TAG, "WS 已连接，发送鉴权...");
            cJSON *auth_json = cJSON_CreateObject();
            cJSON_AddStringToObject(auth_json, "uid", self->current_uid.c_str());
            char *auth_str = cJSON_PrintUnformatted(auth_json);
            esp_websocket_client_send_text(data->client, auth_str, strlen(auth_str), portMAX_DELAY);
            free(auth_str);
            cJSON_Delete(auth_json);
            break;
        }
        case WEBSOCKET_EVENT_DATA: {
            if (data->data_len <= 0) break;
            char *json_str = (char *)malloc(data->data_len + 1);
            if (!json_str) break;
            memcpy(json_str, data->data_ptr, data->data_len);
            json_str[data->data_len] = 0;

            cJSON *root = cJSON_Parse(json_str);
            if (root) {
                cJSON *type = cJSON_GetObjectItem(root, "type");
                cJSON *params = cJSON_GetObjectItem(root, "parameters");
                
                // (A) 握手成功
                if (type && type->valueint == 1) {
                    ESP_LOGI(MON_TAG, "🎉 监控链路已打通");
                }
                // (B) 心跳保活
                if (params) {
                    cJSON *info = cJSON_GetObjectItem(params, "info");
                    if (info && strcmp(info->valuestring, "alive") == 0) {
                        const char *pong = "{\"type\":0,\"info\":\"ok\"}";
                        esp_websocket_client_send_text(data->client, pong, strlen(pong), portMAX_DELAY);
                    }
                }
                // (C) 状态变化事件 (Type 2)
                if (type && type->valueint == 2) {
                    cJSON *info = cJSON_GetObjectItem(root, "info");
                    if (info) {
                        cJSON *title = cJSON_GetObjectItem(info, "event_title");
                        if (title && cJSON_IsString(title)) {
                            const char* t_str = title->valuestring;
                            
                            // 场景 1: 坐下
                            if (strstr(t_str, "在坐") || strstr(t_str, "在卧")) {
                                if (self->sit_start_time == 0) {
                                    self->sit_start_time = esp_timer_get_time();
                                    self->has_alerted = false;
                                    
                                    // 上报给 AI，不紧急
                                    self->ReportStatusToAI("传感器感知：老人刚刚坐下了。", false);
                                }
                            }
                            // 场景 2: 离开
                            else if (strstr(t_str, "离坐") || strstr(t_str, "离卧")) {
                                if (self->sit_start_time != 0) {
                                    int64_t duration_sec = (esp_timer_get_time() - self->sit_start_time) / 1000000;
                                    self->sit_start_time = 0;
                                    self->has_alerted = false;

                                    char msg_buf[128];
                                    snprintf(msg_buf, sizeof(msg_buf), "传感器感知：老人起身离开了，本次久坐时长 %lld 秒。", duration_sec);
                                    
                                    // 上报给 AI，不紧急
                                    self->ReportStatusToAI(std::string(msg_buf), false);
                                }
                            }
                        }
                    }
                }
                cJSON_Delete(root);
            }
            free(json_str);
            break;
        }
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(MON_TAG, "WS 断开，尝试重连...");
            break;
    }
}

// 后台计时线程 (只负责发超时信号给 AI)
void MyHomeDevice::MonitorWatchdogTask(void *arg) {
    MyHomeDevice* self = (MyHomeDevice*)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 

        if (self->sit_start_time != 0) {
            int64_t now = esp_timer_get_time();
            int64_t duration_sec = (now - self->sit_start_time) / 1000000; 

            // 阈值：120秒
            if (duration_sec > 120 && !self->has_alerted) {
                // 上报给 AI，标记为【紧急】
                self->ReportStatusToAI("警告：检测到老人久坐已超过2分钟，请立即进行健康提醒！", true);
                
                self->has_alerted = true; 
            }
        }
    }
}

// 启动入口
void MyHomeDevice::StartElderlyMonitor() {
    if (!PerformMonitorLogin()) {
        ESP_LOGE(MON_TAG, "启动失败：无法登录监控平台");
        return;
    }

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = MONITOR_WS_URL;
    ws_cfg.cert_pem = NULL;
    ws_cfg.skip_cert_common_name_check = true; 

    ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, WebSocketEventHandler, (void*)this);
    esp_websocket_client_start(ws_client);

    // 启动后台计时线程
    xTaskCreate(MonitorWatchdogTask, "monitor_task", 4096, this, 5, NULL);
}

// =================================================================================
// Part 2.5: 门磁监控（检测到开门则上报 AI，可打断当前播报并提醒）
// =================================================================================

#define DOOR_POLL_INTERVAL_MS  5000
#define DOOR_TAG "DoorMonitor"

void MyHomeDevice::DoorMonitorTask(void* arg) {
    MyHomeDevice* self = &MyHomeDevice::GetInstance();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(DOOR_POLL_INTERVAL_MS));
        std::string state = self->GetEntityState(HA_NEW_URL, HA_NEW_TOKEN, ENTITY_DOOR_SENSOR);
        if (state == "error" || state == "unknown") continue;
        if (state == "on" && self->last_door_state_ == "off") {
            ESP_LOGI(DOOR_TAG, "检测到有人开门，上报 AI 并提醒用户");
            self->ReportStatusToAI("检测到有人开门，请提醒用户注意。", true);
        }
        self->last_door_state_ = state;
    }
}

void MyHomeDevice::StartDoorMonitor() {
    xTaskCreate(DoorMonitorTask, "door_monitor", 4096, nullptr, 4, NULL);
    ESP_LOGI(DOOR_TAG, "门磁监控已启动，轮询间隔 %d 秒", DOOR_POLL_INTERVAL_MS / 1000);
}

// =================================================================================
// Part 3: 注册 HA 控制工具 (MCP Tool)
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

    ESP_LOGI(TAG, "Hybrid Home Assistant Tools Registered");
}