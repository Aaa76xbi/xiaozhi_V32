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

// ===================== 1. 配置区域 (来自你的 Python 脚本) =====================

#define LOGIN_URL     "https://papi.11yzh.com/api/rest/data/login"
#define WS_URL        "wss://papi.11yzh.com/wss?"

// 登录数据 (application/x-www-form-urlencoded 格式)
#define LOGIN_BODY    "user=522601002006&password=Admin2189666"

static const char *TAG = "YZH_SENSOR";

// 全局变量
static char *g_current_uid = NULL;      // 存储登录获取的 UID
static bool g_is_sitting = false;       // 是否坐着
static int64_t g_sit_start_time = 0;    // 坐下的开始时间 (系统时间戳)
static bool g_has_alerted = false;      // 是否已经提醒过
static esp_websocket_client_handle_t g_ws_client = NULL;

// ===================== 辅助功能 =====================

// 获取当前毫秒级时间戳
int64_t get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 给小智 AI 发送指令
void send_to_ai(const char* text) {
    auto protocol = Application::GetInstance().GetProtocol();
    if (protocol) {
        ESP_LOGI(TAG, "🤖 触发 AI: %s", text);
        protocol->SendText(text);
    }
}

// ===================== HTTP 登录模块 =====================

// [修改点] 改名并加 static，防止和 my_home_device.cc 冲突
static esp_err_t sensor_login_event_handler(esp_http_client_event_t *evt) {
    static char *response_buffer = NULL;
    static int response_len = 0;

    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // 简单拼接数据
                if (response_buffer == NULL) {
                    response_buffer = (char *)calloc(1, 1024); // 申请 1KB 缓存
                    response_len = 0;
                }
                if (response_buffer && response_len < 1023) {
                    int copy_len = (evt->data_len < (1023 - response_len)) ? evt->data_len : (1023 - response_len);
                    memcpy(response_buffer + response_len, evt->data, copy_len);
                    response_len += copy_len;
                    response_buffer[response_len] = 0;
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            if (response_buffer != NULL) {
                ESP_LOGI(TAG, "登录返回: %s", response_buffer);
                cJSON *root = cJSON_Parse(response_buffer);
                if (root) {
                    // 解析结构: {"code": 1, "data": "UID_STRING..."}
                    cJSON *code = cJSON_GetObjectItem(root, "code");
                    cJSON *data = cJSON_GetObjectItem(root, "data");
                    
                    if (code && code->valueint == 1 && cJSON_IsString(data)) {
                        if (g_current_uid) free(g_current_uid);
                        g_current_uid = strdup(data->valuestring);
                        ESP_LOGI(TAG, "✅ 获取 UID 成功: %s", g_current_uid);
                    }
                    cJSON_Delete(root);
                }
                free(response_buffer);
                response_buffer = NULL;
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
    config.event_handler = sensor_login_event_handler; // [修改点] 使用新名字
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    
    // [修改点] 移除 crt_bundle，彻底跳过证书检查
    config.skip_cert_common_name_check = true; 

    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // 设置 Header
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded;charset=UTF-8");
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (ESP32-Xiaozhi)");
    
    // 设置 Body
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

void handle_ws_message(const char *payload) {
    cJSON *root = cJSON_Parse(payload);
    if (!root) return;

    // 1. 处理心跳保持
    cJSON *params = cJSON_GetObjectItem(root, "parameters");
    if (params) {
        cJSON *info = cJSON_GetObjectItem(params, "info");
        if (cJSON_IsString(info) && strcmp(info->valuestring, "alive") == 0) {
            // 回复心跳
            const char *pong_msg = "{\"type\":0,\"info\":\"ok\"}";
            if (g_ws_client && esp_websocket_client_is_connected(g_ws_client)) {
                esp_websocket_client_send_text(g_ws_client, pong_msg, strlen(pong_msg), portMAX_DELAY);
                ESP_LOGD(TAG, "❤️ 回复心跳");
            }
            cJSON_Delete(root);
            return;
        }
    }

    // 2. 处理业务消息
    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (type_item && type_item->valueint == 2) {
        cJSON *info = cJSON_GetObjectItem(root, "info");
        cJSON *event_title_item = cJSON_GetObjectItem(info, "event_title");
        
        if (cJSON_IsString(event_title_item)) {
            const char *title = event_title_item->valuestring;
            ESP_LOGI(TAG, "🔔 收到事件: %s", title);

            // --- 场景 A: 坐下 ---
            if (strstr(title, "在坐") || strstr(title, "在卧")) {
                if (!g_is_sitting) {
                    g_is_sitting = true;
                    g_sit_start_time = get_time_ms();
                    g_has_alerted = false;
                    ESP_LOGI(TAG, "👇 老人坐下了，开始计时...");
                    
                    // 触发礼貌问候
                    send_to_ai("系统检测到老人刚刚坐下了。请用温柔的语气问候老人，并询问是否需要打开电视或拉开窗帘？");
                }
            }
            // --- 场景 B: 起来 ---
            else if (strstr(title, "离坐") || strstr(title, "离卧")) {
                if (g_is_sitting) {
                    int64_t duration = (get_time_ms() - g_sit_start_time) / 1000;
                    ESP_LOGI(TAG, "👆 老人起来了，共坐了 %lld 秒", duration);
                    g_is_sitting = false;
                    g_has_alerted = false;
                }
            }
        }
    }
    cJSON_Delete(root);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ [第二步] WS 连接建立，发送鉴权...");
            if (g_current_uid) {
                // 发送鉴权: {"uid": "..."}
                char auth_msg[512];
                snprintf(auth_msg, sizeof(auth_msg), "{\"uid\":\"%s\"}", g_current_uid);
                esp_websocket_client_send_text(data->client, auth_msg, strlen(auth_msg), portMAX_DELAY);
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 1 && data->data_len > 0) { // 文本帧
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

// ===================== 主监控任务 =====================

void sensor_monitor_task(void *pvParameters) {
    // ============================================================
    // [新增关键修复] 防止系统刚启动就联网导致崩溃
    // ============================================================
    ESP_LOGW(TAG, "⏳ 传感器任务已启动，等待 20 秒让 WiFi 先连接...");
    
    // 这里等待 20 秒，确保 WiFi 已经连上并且获取到了 IP
    // 如果你的网络很慢，可以把 20000 改成 30000
    vTaskDelay(pdMS_TO_TICKS(20000)); 
    
    ESP_LOGI(TAG, "✅ 预热结束，开始执行监控逻辑...");

    // 1. 循环直到登录成功
    while (1) {
        if (perform_login()) break;
        // 如果登录失败，休息 5 秒再试
        ESP_LOGE(TAG, "登录失败，等待重试...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // 2. 启动 WebSocket
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = WS_URL;
    ws_cfg.skip_cert_common_name_check = true; // 忽略 SSL 校验

    ESP_LOGI(TAG, "⏳ 准备连接 WebSocket...");
    g_ws_client = esp_websocket_client_init(&ws_cfg);

    // 注册事件 (使用旧版函数名)
    esp_websocket_register_events(g_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)g_ws_client);
    
    esp_websocket_client_start(g_ws_client);

    // 3. 后台巡逻线程
    const int64_t ALERT_THRESHOLD_MS = 2 * 60 * 1000; // 2分钟

    while (1) {
        // 只有当 WebSocket 连接成功时才进行业务逻辑
        if (esp_websocket_client_is_connected(g_ws_client)) {
            if (g_is_sitting && !g_has_alerted) {
                int64_t now = get_time_ms();
                if ((now - g_sit_start_time) > ALERT_THRESHOLD_MS) {
                    ESP_LOGI(TAG, "⚠️ [实时触发] 久坐超过 2 分钟！");
                    
                    // 触发久坐提醒
                    send_to_ai("系统检测到老人已经坐了超过两分钟了。请温柔地提醒老人，坐太久了，建议站起来走动走动，活动一下身体。");
                    
                    g_has_alerted = true; // 避免重复提醒
                }
            }
        } else {
            // 如果断线了，可以在这里打印一下，或者不做处理等待重连
            // ESP_LOGW(TAG, "WebSocket 未连接...");
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // 每秒检查一次
    }
}
// ===================== 程序入口 =====================

extern "C" void app_main(void)
{
    // ... 原有的初始化代码 (NVS, Wi-Fi 等) ...
    // 请保留 Application::GetInstance().Start() 之前的初始化内容
    
    

    Application::GetInstance().Start();
    // [插入] 启动我们的监控任务
    xTaskCreate(sensor_monitor_task, "sensor_task", 8192, NULL, 5, NULL);
}