#include <esp_log.h>
#include "driver/gpio.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mcp_server.h"
#include "boards/yunxi-chajie/switch_controller.h"

#define TAG "SwitchController"

// 简单的电机开关控制
#ifndef SWITCH_GPIO
#define SWITCH_GPIO GPIO_NUM_10 // 默认使用 GPIO 10
#endif

void SwitchController::SetGpio(bool on) {
    gpio_set_level(SWITCH_GPIO, on ? 1 : 0);
}

void SwitchController::TurnOn() {
    state_ = true;
    SetGpio(true);
    ESP_LOGI(TAG, "开关已打开");
}

void SwitchController::TurnOff() {
    state_ = false;
    SetGpio(false);
    ESP_LOGI(TAG, "开关已关闭");
}

void SwitchController::Trigger() {
    SetGpio(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    SetGpio(false);
    ESP_LOGI(TAG, "已模拟按键触发");
}

void SwitchController::RegisterMcpTools() {
    auto& mcp_server = McpServer::GetInstance();

    // 开
    mcp_server.AddTool(
        "self.switch.on",
        "打开开关（高电平）",
        PropertyList(),
        [this](const PropertyList &) -> ReturnValue {
            TurnOn();
            return true;
        });

    // 关
    mcp_server.AddTool(
        "self.switch.off",
        "关闭开关（低电平）",
        PropertyList(),
        [this](const PropertyList &) -> ReturnValue {
            TurnOff();
            return true;
        });

    // 查询状态
    mcp_server.AddTool(
        "self.switch.get_state",
        "获取当前开关状态",
        PropertyList(),
        [this](const PropertyList &) -> ReturnValue {
            std::string state = state_ ? "on" : "off";
            ESP_LOGI(TAG, "获取开关状态: %s", state.c_str());
            return state;
        });

    // 模拟按键触发
    mcp_server.AddTool(
        "self.switch.trigger",
        "模拟按键触发（高电平100ms后恢复低电平）",
        PropertyList(),
        [this](const PropertyList &) -> ReturnValue {
            Trigger();
            return true;
        });
}

SwitchController::SwitchController()
{
    ESP_LOGI(TAG, "初始化开关控制器，GPIO=%d", SWITCH_GPIO);

    // 初始化 GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << SWITCH_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    SetGpio(false);
    RegisterMcpTools();
}

SwitchController::~SwitchController() {
    SetGpio(false);
}

// 创建全局实例
static SwitchController* g_switch_controller = nullptr;

// 初始化函数
extern "C" void initialize_switch_controller() {
    if (g_switch_controller == nullptr) {
        g_switch_controller = new SwitchController();
        ESP_LOGI(TAG, "开关控制器已初始化，并注册MCP工具");
    }
}