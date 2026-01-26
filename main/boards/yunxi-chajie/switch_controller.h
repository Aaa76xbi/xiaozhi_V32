// switch_controller.h
#ifndef XIAOZHI_ESP32_YUNXI_CHAJIE_SWITCH_CONTROLLER_H
#define XIAOZHI_ESP32_YUNXI_CHAJIE_SWITCH_CONTROLLER_H

#include <driver/gpio.h>
#include "config.h"

class SwitchController
{
private:
    bool state_ = false;
    void SetGpio(bool on);

public:
    explicit SwitchController();
    ~SwitchController();

    void RegisterMcpTools();
    
    // 控制接口
    void TurnOn();
    void TurnOff();
    void Trigger();
    bool GetState() const { return state_; }
};

// 全局初始化函数
extern "C" void initialize_switch_controller();

#endif // XIAOZHI_ESP32_YUNXI_CHAJIE_SWITCH_CONTROLLER_H