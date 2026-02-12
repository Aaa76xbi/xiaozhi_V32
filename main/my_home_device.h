#ifndef MY_HOME_DEVICE_H
#define MY_HOME_DEVICE_H

#include <string>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_websocket_client.h> 

// =================【配置 A：老设备 (外网 HA)】=================
#define HA_OLD_URL   "http://home.dalinziyo.site/api"
#define HA_OLD_TOKEN "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJmOTFmMWYxODc5ODY0ZjhlOTg3ZjFlZTAxYmM2MGI1MyIsImlhdCI6MTc1ODAxNjI3NywiZXhwIjoyMDczMzc2Mjc3fQ.KvrSEmDIfUveaXjPUR4mTqBseCO_1cimXF-nfna3l-o"

#define ENTITY_MAIN_SWITCH "switch.ji_liang_guo_qian_ya_kai_guan_2_quan_wu_zong_zha_switch"
#define ENTITY_TV          "switch.cuco_cn_482540485_cp1_on_p_2_1"
#define ENTITY_GAS_VALVE   "switch.wbzhi_neng_fa_men_5_switch_1"
#define ENTITY_WATER_VALVE "switch.zhi_neng_shui_fa_switch_1"

// =================【配置 B：新设备 (本地 HA)】=================
#define HA_NEW_URL   "http://192.168.0.12:8123/api"
#define HA_NEW_TOKEN "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJlMjhhZTU4ZjFjZmE0ZGI5YjNiNTM0NmM4MzZmMjIyYiIsImlhdCI6MTc3MDI5NTY5OSwiZXhwIjoyMDg1NjU1Njk5fQ.8AkqWHvGNxOfH1BsN0JrNcVBX-d1SMS6tY5wBQuIkTM"

#define ENTITY_SMART_PLUG  "switch.zhi_neng_cha_zuo_socket_1"
#define ENTITY_DOOR_SENSOR "binary_sensor.isa_dw2hl_2dde_contact_state"

// =================【配置 C：老人监控平台】=================
#define MONITOR_LOGIN_URL "https://papi.11yzh.com/api/rest/data/login"
#define MONITOR_WS_URL    "wss://papi.11yzh.com/wss?"
#define MONITOR_USER      "522601002006"
#define MONITOR_PASS      "Admin2189666"

class MyHomeDevice {
public:
    static MyHomeDevice& GetInstance() {
        static MyHomeDevice instance;
        return instance;
    }

    // ❌ 删除了这里的 GetProtocol，因为它属于 Application 类，不属于这里

    // 注册 HA 控制工具给 AI
    void RegisterHomeDeviceTools();

    // 启动老人监控功能 (请在 main 函数网络连接成功后调用)
    void StartElderlyMonitor();

    // HA 通用控制函数
    void CallService(const char* base_url, const char* token, const char* domain, const char* service, const char* entity_id);
    std::string GetEntityState(const char* base_url, const char* token, const char* entity_id);

    // 【核心】将传感器状态上报给 AI 大脑
    // content: 事件描述
    // is_urgent: 是否为紧急事件
    void ReportStatusToAI(const std::string& content, bool is_urgent = false);

    // 启动门磁监控
    void StartDoorMonitor();

private:
    MyHomeDevice() = default;
    ~MyHomeDevice() = default;

    // --- 监控相关私有变量 ---
    std::string current_uid;      // 存储 Token
    int64_t sit_start_time = 0;   // 坐下开始时间 (0表示未坐)
    bool has_alerted = false;     // 是否已触发超时警告
    esp_websocket_client_handle_t ws_client = nullptr;

    // --- 门磁监控 ---
    std::string last_door_state_ = "off";  // 上次门状态，用于检测 off->on
    static void DoorMonitorTask(void* arg);

    // --- 监控相关私有函数 ---
    bool PerformMonitorLogin();   // HTTP 登录拿 Token
    static void WebSocketEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
    static void MonitorWatchdogTask(void *arg); // 后台计时任务
};

#endif // MY_HOME_DEVICE_H