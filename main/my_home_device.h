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
#define HA_NEW_URL   "http://192.168.0.12:8123/api"
#define HA_NEW_TOKEN "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJlMjhhZTU4ZjFjZmE0ZGI5YjNiNTM0NmM4MzZmMjIyYiIsImlhdCI6MTc3MDI5NTY5OSwiZXhwIjoyMDg1NjU1Njk5fQ.8AkqWHvGNxOfH1BsN0JrNcVBX-d1SMS6tY5wBQuIkTM"

#define ENTITY_SMART_PLUG  "switch.zhi_neng_cha_zuo_socket_1"
#define ENTITY_DOOR_SENSOR "binary_sensor.isa_dw2hl_2dde_contact_state"

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