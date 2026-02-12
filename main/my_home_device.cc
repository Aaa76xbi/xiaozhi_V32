#include "my_home_device.h"
#include "application.h"
#include <mcp_server.h>
#include <esp_http_client.h>
#include <cJSON.h>
#include <esp_log.h>
#include <string.h>
#include "board.h"

#define TAG "HomeDevice"

// =================================================================================
// Part 1: Home Assistant 基础工具 (HTTP 辅助函数)
// =================================================================================

// HTTP 事件处理：用于拼接 HA 返回的 JSON 数据
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

// 获取 HA 实体状态 (例如查询灯是开还是关)
std::string MyHomeDevice::GetEntityState(const char* base_url, const char* token, const char* entity_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/states/%s", base_url, entity_id);

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
    }
    esp_http_client_cleanup(client);
    return result_state;
}

// 调用 HA 服务 (发送控制指令，例如打开插座)
void MyHomeDevice::CallService(const char* base_url, const char* token, const char* domain, const char* service, const char* entity_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/services/%s/%s", base_url, domain, service);

    ESP_LOGI(TAG, "Calling HA Service: %s for %s", url, entity_id);

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
        ESP_LOGE(TAG, "HA Request failed: %s", esp_err_to_name(err));
    }

    cJSON_Delete(root);
    free((void*)post_data);
    esp_http_client_cleanup(client);
}


// =================================================================================
// Part 2: 注册小智 AI 控制工具 (让 AI 能帮你控制家电)
// =================================================================================

void MyHomeDevice::RegisterHomeDeviceTools() {
    auto& server = McpServer::GetInstance();

    server.AddTool(
        "control_home_device",
        "控制或查询家电状态。参数: device(plug/door/tv/water_valve/gas_valve/main_switch), action(on/off/query)",
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

                // --- 路由逻辑：根据 AI 指令选择对应的 HA 实体 ---
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

                // 1. 处理查询指令
                if (action == "query") {
                    std::string state_raw = GetEntityState(target_url, target_token, target_entity);
                    std::string state_cn = state_raw;
                    if (state_raw == "on") state_cn = "打开";
                    else if (state_raw == "off") state_cn = "关闭";
                    return device_name_cn + "当前的状态是：" + state_cn;
                }

                // 2. 处理控制指令
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

    ESP_LOGI(TAG, "Home Assistant Tools Registered");
}