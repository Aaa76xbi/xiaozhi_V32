#include <esp_log.h>
#include "driver/gpio.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mcp_server.h"
#include "boards/yunxi-chajie/lamp_controller.h"

#define TAG "LampControllerG"

// 简单的电机开关控制
#ifndef LAMP_GPIO
#define LAMP_GPIO GPIO_NUM_11 // 默认使用 GPIO 11
#endif

void LampControllerG::SetGpio(bool on) {
    gpio_set_level(LAMP_GPIO, on ? 1 : 0);
}

void LampControllerG::TurnOn() {
    state_ = true;
    SetGpio(true);
    ESP_LOGI(TAG, "灯已打开");
}

void LampControllerG::TurnOff() {
    state_ = false;
    SetGpio(false);
    ESP_LOGI(TAG, "灯已关闭");
}

void LampControllerG::Trigger() {
    SetGpio(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    SetGpio(false);
    ESP_LOGI(TAG, "已模拟按键触发");
}

void LampControllerG::RegisterMcpTools() {
    auto& mcp_server = McpServer::GetInstance();

    // 开灯工具
    mcp_server.AddTool(
        "self.lamp.on",
        "打开灯（高电平）",
        PropertyList(),
        [this](const PropertyList &) -> ReturnValue {
            TurnOn();
            return true;
        });

    // 关灯工具
    mcp_server.AddTool(
        "self.lamp.off",
        "关闭灯（低电平）",
        PropertyList(),
        [this](const PropertyList &) -> ReturnValue {
            TurnOff();
            return true;
        });

    // 查询状态工具
    mcp_server.AddTool(
        "self.lamp.get_state",
        "获取当前灯状态",
        PropertyList(),
        [this](const PropertyList &) -> ReturnValue {
            std::string state = state_ ? "on" : "off";
            ESP_LOGI(TAG, "获取灯状态: %s", state.c_str());
            return state;
        });

    // 模拟按键触发工具
    mcp_server.AddTool(
        "self.lamp.trigger",
        "模拟按键触发（高电平100ms后恢复低电平）",
        PropertyList(),
        [this](const PropertyList &) -> ReturnValue {
            Trigger();
            return true;
        });
}

LampControllerG::LampControllerG()
{
    ESP_LOGI(TAG, "初始化开关控制器，GPIO=%d", LAMP_GPIO);

    // 初始化 GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LAMP_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    SetGpio(false);
    RegisterMcpTools();
}

LampControllerG::~LampControllerG() {
    SetGpio(false);
}

// 创建全局实例
static LampControllerG* g_lamp_controller = nullptr;

// 初始化函数
extern "C" void initialize_lamp_controller() {
    if (g_lamp_controller == nullptr) {
        g_lamp_controller = new LampControllerG();
        ESP_LOGI(TAG, "台灯控制器已初始化，并注册MCP工具");
    }
}