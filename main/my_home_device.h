#ifndef MY_HOME_DEVICE_H
#define MY_HOME_DEVICE_H

#include <string>
#include <esp_log.h>

// ================= 配置区域 =================
// 你的 Home Assistant 地址 (注意：ESP32代码里用 HTTP 协议更简单稳定)
// 对应你的 ws://home.dalinziyo.site/api/websocket
#define HA_BASE_URL "http://home.dalinziyo.site/api"

// 你的 Long-Lived Access Token (直接从你的Python代码复制)
#define HA_ACCESS_TOKEN "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJmOTFmMWYxODc5ODY0ZjhlOTg3ZjFlZTAxYmM2MGI1MyIsImlhdCI6MTc1ODAxNjI3NywiZXhwIjoyMDczMzc2Mjc3fQ.KvrSEmDIfUveaXjPUR4mTqBseCO_1cimXF-nfna3l-o"

// 设备实体 ID (从你的Python代码复制)
#define ENTITY_MAIN_SWITCH "switch.ji_liang_guo_qian_ya_kai_guan_2_quan_wu_zong_zha_switch"
#define ENTITY_TV          "switch.cuco_cn_482540485_cp1_on_p_2_1"
#define ENTITY_GAS_VALVE   "switch.wbzhi_neng_fa_men_5_switch_1"
#define ENTITY_WATER_VALVE "switch.zhi_neng_shui_fa_switch_1"
// ===========================================

class MyHomeDevice {
public:
    static MyHomeDevice& GetInstance() {
        static MyHomeDevice instance;
        return instance;
    }

    // 注册工具给 AI
    void RegisterHomeDeviceTools();

    // 通用的控制函数 (对应 HA 的 Call Service)
    // domain: 比如 "switch"
    // service: 比如 "turn_on" 或 "turn_off"
    // entity_id: 设备ID
    void CallService(const char* domain, const char* service, const char* entity_id);

private:
    MyHomeDevice() = default;
    ~MyHomeDevice() = default;
};

#endif // MY_HOME_DEVICE_H