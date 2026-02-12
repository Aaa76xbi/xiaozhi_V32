、#include "my_home_device.h"
#include "application.h"
#include <mcp_server.h>
#include <esp_http_client.h>
#include <cJSON.h>
#include <esp_log.h>
#include <string.h>

#define TAG "HomeDevice"

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
        default: break;
    }
    return ESP_OK;
}

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
                if (cJSON_IsString(state_item)) result_state = std::string(state_item->valuestring);
                cJSON_Delete(root);
            }
        }
    }
    esp_http_client_cleanup(client);
    return result_state;
}

void MyHomeDevice::CallService(const char* base_url, const char* token, const char* domain, const char* service, const char* entity_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/services/%s/%s", base_url, domain, service);
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth_header);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "entity_id", entity_id);
    const char *post_data = cJSON_PrintUnformatted(root);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_http_client_perform(client);
    cJSON_Delete(root);
    free((void*)post_data);
    esp_http_client_cleanup(client);
}

void MyHomeDevice::RegisterHomeDeviceTools() {
    auto& server = McpServer::GetInstance();
    server.AddTool("control_home_device", "控制家电(plug/tv/water_valve/gas_valve/main_switch),action(on/off/query)",
        PropertyList({Property("device", kPropertyTypeString), Property("action", kPropertyTypeString)}),
        [this](const PropertyList& props) -> ReturnValue {
            std::string device = props["device"].value<std::string>();
            std::string action = props["action"].value<std::string>();
            const char* target_entity = (device == "plug") ? ENTITY_SMART_PLUG : (device == "tv") ? ENTITY_TV : (device == "main_switch") ? ENTITY_MAIN_SWITCH : nullptr;
            if (!target_entity) return std::string("找不到设备");
            if (action == "query") return GetEntityState(HA_NEW_URL, HA_NEW_TOKEN, target_entity);
            CallService(HA_NEW_URL, HA_NEW_TOKEN, "switch", action == "on" ? "turn_on" : "turn_off", target_entity);
            return std::string("已执行");
        });
}