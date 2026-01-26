#include "my_home_device.h"
#include <mcp_server.h>
#include <esp_http_client.h>
#include <cJSON.h>
#include <esp_log.h>

#define TAG "HomeDevice"

// 发送 HTTP 请求给 Home Assistant
void MyHomeDevice::CallService(const char* domain, const char* service, const char* entity_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/services/%s/%s", HA_BASE_URL, domain, service);

    ESP_LOGI(TAG, "Calling HA Service: %s for %s", url, entity_id);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5000;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", HA_ACCESS_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth_header);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "entity_id", entity_id);
    const char *post_data = cJSON_PrintUnformatted(root);
    
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HA Status = %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "HA Request failed: %s", esp_err_to_name(err));
    }

    cJSON_Delete(root);
    free((void*)post_data);
    esp_http_client_cleanup(client);
}

// 注册工具给 AI (这里修改了语法以适配项目)
void MyHomeDevice::RegisterHomeDeviceTools() {
    auto& server = McpServer::GetInstance();

    // 注册工具：控制家电
    server.AddTool(
        "control_home_device",                                      // 工具名称
        "控制家电设备（电视、水阀、气阀、总闸）。参数：device (tv/water_valve/gas_valve/main_switch), action (on/off)", // 描述
        PropertyList({                                              // 参数列表
            Property("device", kPropertyTypeString),
            Property("action", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {     // 回调函数
            try {
                std::string device = properties["device"].value<std::string>();
                std::string action = properties["action"].value<std::string>();

                // 2. 映射设备名称到 Entity ID
                const char* target_entity = nullptr;
                std::string device_name_cn = "";

                if (device == "tv") {
                    target_entity = ENTITY_TV;
                    device_name_cn = "电视";
                } else if (device == "water_valve") {
                    target_entity = ENTITY_WATER_VALVE;
                    device_name_cn = "水阀";
                } else if (device == "gas_valve") {
                    target_entity = ENTITY_GAS_VALVE;
                    device_name_cn = "气阀";
                } else if (device == "main_switch") {
                    target_entity = ENTITY_MAIN_SWITCH;
                    device_name_cn = "总闸";
                }

                if (!target_entity) {
                    return std::string("Error: Unknown device.");
                }

                std::string service = (action == "on") ? "turn_on" : "turn_off";
                std::string action_cn = (action == "on") ? "打开" : "关闭";

                // 执行请求
                CallService("switch", service.c_str(), target_entity);

                return "已帮你" + action_cn + device_name_cn;

            } catch (...) {
                return std::string("Error: Invalid arguments");
            }
        });

    ESP_LOGI(TAG, "Home Assistant Tools Registered");
}