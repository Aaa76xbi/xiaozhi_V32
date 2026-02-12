#ifndef MY_HOME_DEVICE_H
#define MY_HOME_DEVICE_H

#include <string>

// Home Assistant 配置
#define HA_NEW_URL   "http://192.168.0.12:8123/api"
#define HA_NEW_TOKEN "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJlMjhhZTU4ZjFjZmE0ZGI5YjNiNTM0NmM4MzZmMjIyYiIsImlhdCI6MTc3MDI5NTY5OSwiZXhwIjoyMDg1NjU1Njk5fQ.8AkqWHvGNxOfH1BsN0JrNcVBX-d1SMS6tY5wBQuIkTM"

#define ENTITY_SMART_PLUG  "switch.zhi_neng_cha_zuo_socket_1"
#define ENTITY_TV          "switch.cuco_cn_482540485_cp1_on_p_2_1"
#define ENTITY_MAIN_SWITCH "switch.ji_liang_guo_qian_ya_kai_guan_2_quan_wu_zong_zha_switch"

class MyHomeDevice {
public:
    static MyHomeDevice& GetInstance() {
        static MyHomeDevice instance;
        return instance;
    }

    void RegisterHomeDeviceTools();
    void CallService(const char* base_url, const char* token, const char* domain, const char* service, const char* entity_id);
    std::string GetEntityState(const char* base_url, const char* token, const char* entity_id);

private:
    MyHomeDevice() = default;
    ~MyHomeDevice() = default;
};

#endif // MY_HOME_DEVICE_H