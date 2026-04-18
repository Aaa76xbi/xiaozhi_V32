#include "esp_crt_bundle.h"
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "application.h"
#include "wifi_station.h"
#include "nvs_flash.h"
#include "esp_event.h"

// ===================== 1. 配置区域 =====================

#define LOGIN_URL     "https://papi.11yzh.com/api/rest/data/login"
#define WS_URL        "wss://papi.11yzh.com/wss?"
#define LOGIN_BODY    "user=522601002006&password=Admin2189666"

static const char *TAG = "YZH_SENSOR";

// 全局变量
static char *g_current_uid = NULL;
static bool g_is_sitting = false;
static int64_t g_sit_start_time = 0;
static bool g_has_alerted = false;
static esp_websocket_client_handle_t g_ws_client = NULL;

// ===================== 辅助功能 =====================

int64_t get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void send_to_ai(const char* text) {
    auto protocol = Application::GetInstance().GetProtocol();
    if (protocol) {
        ESP_LOGI(TAG, "🤖 触发 AI: %s", text);
        protocol->SendText(text);
    }
}

// ===================== HTTP 登录模块 =====================

// ===================== [修正后的 HTTP 事件处理] =====================

static esp_err_t sensor_login_event_handler(esp_http_client_event_t *evt) {
    static char *response_buffer = NULL;
    static int response_len = 0;

    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // [关键修改] 删除了 !is_chunked 的判断，无论什么格式都接收数据
            if (1) { 
                if (response_buffer == NULL) {
                    // [优化] 加大缓存到 2048 字节，防止 Token 太长截断
                    response_buffer = (char *)calloc(1, 2048);
                    response_len = 0;
                }
                if (response_buffer && response_len < 2047) {
                    // 防止缓冲区溢出
                    int copy_len = (evt->data_len < (2047 - response_len)) ? evt->data_len : (2047 - response_len);
                    memcpy(response_buffer + response_len, evt->data, copy_len);
                    response_len += copy_len;
                    response_buffer[response_len] = 0; // 保持字符串结尾
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            if (response_buffer != NULL) {
                // 打印一下接收到的原始数据，方便调试
                ESP_LOGI(TAG, "Server Response (%d bytes): %s", response_len, response_buffer);
                
                cJSON *root = cJSON_Parse(response_buffer);
                if (root) {
                    cJSON *code = cJSON_GetObjectItem(root, "code");
                    cJSON *data = cJSON_GetObjectItem(root, "data");
                    
                    // 兼容 data 字段直接是 Token 字符串的情况
                    if (code && code->valueint == 1 && cJSON_IsString(data)) {
                        if (g_current_uid) free(g_current_uid);
                        g_current_uid = strdup(data->valuestring);
                        ESP_LOGI(TAG, "✅ 获取 UID 成功: %s", g_current_uid);
                    } else {
                        ESP_LOGW(TAG, "❌ 登录返回格式不对或 code!=1");
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGE(TAG, "❌ JSON 解析失败");
                }
                
                free(response_buffer);
                response_buffer = NULL;
                response_len = 0;
            }
            break;
        default: break;
    }
    return ESP_OK;
}

bool perform_login() {
    ESP_LOGI(TAG, "🚀 [第一步] 正在登录获取 UID...");
    
    esp_http_client_config_t config = {};
    config.url = LOGIN_URL;
    config.event_handler = sensor_login_event_handler;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    // config.skip_cert_common_name_check = true; 
    // [关键修复] 必须挂载证书包，否则 HTTPS 无法初始化
    config.crt_bundle_attach = esp_crt_bundle_attach; 
    
    // 同时允许跳过 CN 检查（针对自签名或宽松验证）
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded;charset=UTF-8");
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (ESP32-Xiaozhi)");
    esp_http_client_set_post_field(client, LOGIN_BODY, strlen(LOGIN_BODY));

    esp_err_t err = esp_http_client_perform(client);
    bool success = (err == ESP_OK && g_current_uid != NULL);
    
    if (!success) {
        ESP_LOGE(TAG, "❌ 登录失败: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return success;
}

// ===================== WebSocket 模块 =====================

// ===================== [修正后的消息处理逻辑] =====================

// ===================== [修正后的消息处理逻辑：支持 Unicode 转义] =====================

void handle_ws_message(const char *payload) {
    // 仅打印前80字节，避免 base64 图片数据刷屏
    ESP_LOGI(TAG, "📩 [消息] %.80s%s", payload, strlen(payload) > 80 ? "..." : "");

    // 1. 处理心跳
    cJSON *root = cJSON_Parse(payload);
    if (root) {
        cJSON *params = cJSON_GetObjectItem(root, "parameters");
        if (params) {
            cJSON *info = cJSON_GetObjectItem(params, "info");
            if (cJSON_IsString(info) && strcmp(info->valuestring, "alive") == 0) {
                const char *pong_msg = "{\"type\":0,\"info\":\"ok\"}";
                if (g_ws_client && esp_websocket_client_is_connected(g_ws_client)) {
                    esp_websocket_client_send_text(g_ws_client, pong_msg, strlen(pong_msg), portMAX_DELAY);
                    ESP_LOGD(TAG, "❤️ 回复心跳");
                }
                cJSON_Delete(root);
                return;
            }
        }
        cJSON_Delete(root);
    }

    // 2. 业务逻辑 - 改用【转义字符匹配法】
    // 服务器发送的是 Unicode 编码，例如 "\u5728\u5750" 代表 "在坐"
    // 我们直接匹配这些编码字符串
    
    // 关键词："\u5728\u5750" (在坐) 或者 "\u5728\u5367" (在卧)
    // 注意：在C语言字符串里，反斜杠需要写两次 "\\"
    bool is_sitting_msg = (strstr(payload, "\\u5728\\u5750") != NULL) || 
                          (strstr(payload, "\\u5728\\u5367") != NULL);

    // 关键词："\u79bb\u5750" (离坐) 或者 "\u79bb\u5367" (离卧)
    bool is_leave_msg = (strstr(payload, "\\u79bb\\u5750") != NULL) || 
                        (strstr(payload, "\\u79bb\\u5367") != NULL);

    // --- 场景 A: 坐下 ---
    if (is_sitting_msg) {
        ESP_LOGI(TAG, "🔔 匹配到：在坐/在卧 (Unicode)");

        if (!g_is_sitting) {
            g_is_sitting = true;
            g_sit_start_time = get_time_ms();
            g_has_alerted = false;
            ESP_LOGI(TAG, "👇 状态更新：已坐下，触发 AI...");

            // 短暂等待让网络栈处理当前帧，避免瞬间拥堵
            vTaskDelay(pdMS_TO_TICKS(200));

            send_to_ai("系统检测到主人刚刚坐下了。请用热情、温暖的语气问候主人，并询问是否需要打开电视或打开窗帘？");
        }
    }
    // --- 场景 B: 离开 ---
    else if (is_leave_msg) {
        ESP_LOGI(TAG, "🔔 匹配到：离坐/离卧 (Unicode)");
        
        if (g_is_sitting) {
            int64_t duration = (get_time_ms() - g_sit_start_time) / 1000;
            ESP_LOGI(TAG, "👆 状态更新：已离开，共坐了 %lld 秒", duration);
            g_is_sitting = false;
            g_has_alerted = false;
        }
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ [第二步] WS 连接建立，发送鉴权...");
            if (g_current_uid) {
                char auth_msg[512];
                snprintf(auth_msg, sizeof(auth_msg), "{\"uid\":\"%s\"}", g_current_uid);
                esp_websocket_client_send_text(data->client, auth_msg, strlen(auth_msg), portMAX_DELAY);
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 1 && data->data_len > 0) {
                char *buf = (char *)malloc(data->data_len + 1);
                if (buf) {
                    memcpy(buf, data->data_ptr, data->data_len);
                    buf[data->data_len] = 0;
                    handle_ws_message(buf);
                    free(buf);
                }
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "🔌 WS 连接断开");
            break;
    }
}

/* [暂时禁用] 坐垫检测任务
void sensor_monitor_task(void *pvParameters) {
    // 等待 WiFi 连接（事件驱动，最多等 60 秒，避免固定 20 秒硬等）
    ESP_LOGW(TAG, "⏳ 传感器任务已启动，等待 WiFi 连接...");
    if (!WifiStation::GetInstance().WaitForConnected(60000)) {
        ESP_LOGE(TAG, "WiFi 连接超时（60s），传感器任务退出");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "✅ WiFi 已连接，开始监控...");

    // 1. 登录循环
    while (1) {
        if (perform_login()) break;
        ESP_LOGE(TAG, "登录失败，5秒后重试...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // 2. 启动 WebSocket
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = WS_URL;
    // ws_cfg.skip_cert_common_name_check = true; 
    // [关键修复] WebSocket 也需要挂载证书包
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.skip_cert_common_name_check = true;

    ESP_LOGI(TAG, "⏳ 准备连接 WebSocket...");
    g_ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(g_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)g_ws_client);
    esp_websocket_client_start(g_ws_client);

    // 3. 监控久坐
    const int64_t ALERT_THRESHOLD_MS = 2 * 60 * 1000; // 2分钟

    while (1) {
        if (esp_websocket_client_is_connected(g_ws_client)) {
            if (g_is_sitting && !g_has_alerted) {
                int64_t now = get_time_ms();
                if ((now - g_sit_start_time) > ALERT_THRESHOLD_MS) {
                    ESP_LOGI(TAG, "⚠️ [实时触发] 久坐超过 2 分钟！");
                    send_to_ai("系统检测到老人已经坐了超过两分钟了。请温柔地提醒老人，坐太久了，建议站起来走动走动，活动一下身体。");
                    g_has_alerted = true;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
*/ // [暂时禁用] 坐垫检测任务结束

// ===================== 程序入口 =====================

extern "C" void app_main(void)
{
    // 初始化默认事件循环（Wi-Fi/IP 事件依赖此）
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 启动主程序
    Application::GetInstance().Start();

    // 传感器监控任务已禁用（WebSocket 消息会干扰 AI 对话）
    // xTaskCreate(sensor_monitor_task, "sensor_task", 10240, NULL, 2, NULL);
}