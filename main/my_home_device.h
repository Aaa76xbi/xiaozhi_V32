#ifndef MY_HOME_DEVICE_H
#define MY_HOME_DEVICE_H

#include <string>

// =================【配置 A：老设备 (外网 HA)】=================
#define HA_OLD_URL   "http://home.dalinziyo.site/api"
#define HA_OLD_TOKEN "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJmOTFmMWYxODc5ODY0ZjhlOTg3ZjFlZTAxYmM2MGI1MyIsImlhdCI6MTc1ODAxNjI3NywiZXhwIjoyMDczMzc2Mjc3fQ.KvrSEmDIfUveaXjPUR4mTqBseCO_1cimXF-nfna3l-o"

#define ENTITY_MAIN_SWITCH "switch.ji_liang_guo_qian_ya_kai_guan_2_quan_wu_zong_zha_switch"
#define ENTITY_TV          "switch.cuco_cn_482540485_cp1_on_p_2_1"
#define ENTITY_GAS_VALVE   "switch.wbzhi_neng_fa_men_5_switch_1"
#define ENTITY_WATER_VALVE "switch.zhi_neng_shui_fa_switch_1"

// =================【配置 B：新设备 (本地 HA)】=================
// #define HA_NEW_URL   "http://192.168.0.12:8123/api"
// #define HA_NEW_URL   "http://192.168.1.107:8123/api"
#define HA_NEW_URL   "https://ha.111yzh.cn/api"
#define HA_NEW_TOKEN "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJlMjhhZTU4ZjFjZmE0ZGI5YjNiNTM0NmM4MzZmMjIyYiIsImlhdCI6MTc3MDI5NTY5OSwiZXhwIjoyMDg1NjU1Njk5fQ.8AkqWHvGNxOfH1BsN0JrNcVBX-d1SMS6tY5wBQuIkTM"

#define ENTITY_SMART_PLUG  "switch.zhi_neng_cha_zuo_socket_1"
#define ENTITY_DOOR_SENSOR "binary_sensor.isa_dw2hl_2dde_contact_state"

// 窗帘
#define ENTITY_CURTAIN_1   "cover.giot_v5icm_e44c_curtain"
#define ENTITY_CURTAIN_2   "cover.giot_v5icm_ddc4_curtain"
// 小米音箱
#define ENTITY_SPEAKER     "media_player.xiaomi_l05b_030f_play_control"
#define ENTITY_SPEAKER_TTS "text.xiaomi_l05b_030f_play_text"
#define ENTITY_SPEAKER_CMD "text.xiaomi_l05b_030f_execute_text_directive"

// =================【配置 C：TP-Link 摄像头（客厅）】=================
// #define HA_CAMERA_URL    "http://192.168.1.107:8123"
#define HA_CAMERA_URL    "https://ha.111yzh.cn"
#define HA_CAMERA_TOKEN  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJjM2ZiNzlhNjJmMGE0ZTA1YjViNzFmZDNjZjM2ZjRiYiIsImlhdCI6MTc3MzgzNjQ3NCwiZXhwIjoyMDg5MTk2NDc0fQ.Euaz_kC1FTB_UMq4g7hBCuMMMF5cNb1DdDScAMyRaiM"
#define HA_CAMERA_ENTITY "camera.ke_ting_she_xiang_tou_mainstream"
#define HA_CAMERA_PTZ_URL         HA_CAMERA_URL "/api/services/onvif/ptz"
// AI描述用：640px宽（公网访问体积约30-40KB，避免下载超时）
#define HA_CAMERA_PROXY_URL       HA_CAMERA_URL "/api/camera_proxy/" HA_CAMERA_ENTITY "?width=640"
// 显示用缩图（480px宽，HA服务端缩放）：解码后约230KB，适合240×240圆屏
#define HA_CAMERA_PROXY_SMALL_URL HA_CAMERA_URL "/api/camera_proxy/" HA_CAMERA_ENTITY "?width=600"
// Person Detection 传感器（有人在画面时为 on）
#define HA_CAMERA_MOTION_URL     HA_CAMERA_URL "/api/states/binary_sensor.ke_ting_she_xiang_tou_person_detection"
// 有人时每 10 秒分析一次；跌倒报警后冷却 120 秒再重新报警
#define FALL_DETECT_POLL_SEC     5
#define FALL_DETECT_COOLDOWN_SEC 120

// =================【配置 D：老人监控平台】=================
#define MONITOR_LOGIN_URL "https://papi.11yzh.com/api/rest/data/login"
#define MONITOR_WS_URL    "wss://papi.11yzh.com/wss?"
#define MONITOR_USER      "522601002006"
#define MONITOR_PASS      "Admin2189666"

// 由 McpServer::ParseCapabilities 调用，保存 AI 视觉服务器地址
void SetVisionUrl(const std::string& url, const std::string& token);

// 启动后台跌倒检测监控任务（在 RegisterHomeDeviceTools 内自动调用）
void StartFallDetectionMonitor();
// 启动门磁监控后台任务（在 RegisterHomeDeviceTools 内自动调用）
void StartDoorMonitor();

// 闹钟状态查询与关闭（供 Board 按键回调调用）
bool IsAlarmRinging();
void DismissAlarm();

// 总台语音通话（供 Board 按键回调调用）
bool IsStationCallActive();
void StationCallConnect();
void StationCallStartRecord();
void StationCallStopRecord();

// 紧急呼叫手机（当找不到手机或用户主动要求时调用）
void TriggerEmergencyCall();


class MyHomeDevice {
public:
    static MyHomeDevice& GetInstance() {
        static MyHomeDevice instance;
        return instance;
    }

    // 注册 HA 控制工具给 AI
    void RegisterHomeDeviceTools();

    // HA 通用控制函数
    void CallService(const char* base_url, const char* token, const char* domain, const char* service, const char* entity_id);
    std::string GetEntityState(const char* base_url, const char* token, const char* entity_id);

private:
    MyHomeDevice() = default;
    ~MyHomeDevice() = default;
};

#endif // MY_HOME_DEVICE_H
