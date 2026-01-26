// ws2812_controller_mcp.h
#ifndef XIAOZHI_ESP32_DANMAI_WS2812_CONTROLLER_MCP_H
#define XIAOZHI_ESP32_DANMAI_WS2812_CONTROLLER_MCP_H

#include <led_strip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/Task.h>
#include <mcp_server.h>
#include "led/led.h"
#include <string>
#include <vector>



namespace ws2812 {

    enum Ws2812EffectType
    {
        EFFECT_OFF = 0,
    EFFECT_BREATHING = 1,
        EFFECT_VOLUME = 2,
        EFFECT_RAINBOW = 3,
        EFFECT_MARQUEE = 4,
        EFFECT_RAINBOW_FLOW = 5,
        EFFECT_SCROLL = 6, // ✅ 新增：滚动灯
        EFFECT_BLINK = 7,  // ✅ 新增：闪烁灯
        EFFECT_NIGHTLIGHT = 8 // 小夜灯模式，柔和常亮，用户手动开启/关闭

    };

    // 兼容旧名称：保留 EFFECT_BREATH 的定义，映射到 EFFECT_BREATHING
#ifndef EFFECT_BREATH
#define EFFECT_BREATH EFFECT_BREATHING
#endif

    class Ws2812ControllerMCP : public Led
    {
    private:
        led_strip_handle_t led_strip_ = nullptr;
        TaskHandle_t effect_task_handle_ = nullptr;
        volatile Ws2812EffectType effect_type_ = EFFECT_OFF;
        volatile bool running_ = false;

    uint8_t color_r_ = 0;
    uint8_t color_g_ = 255;
    uint8_t color_b_ = 0;

    // 闪烁灯独立颜色，避免修改通用 color_* 导致呼吸灯颜色被覆盖
    uint8_t blink_r_ = 255;
    uint8_t blink_g_ = 0;
    uint8_t blink_b_ = 0;

        int breath_delay_ms_ = 40;
        int brightness_ = 50;

        int scroll_offset_ = 0;    // 滚动灯偏移
        // StripColor blink_color_;   // 闪烁灯颜色
        int blink_interval_ = 500; // 闪烁间隔
        bool blink_state_ = false; // 当前闪烁状态

        static const int RAINBOW_COLORS_COUNT = 7;
        static const int COLOR_GAP = 3;
        const uint8_t rainbow_colors_[RAINBOW_COLORS_COUNT][3] = {
            {255, 0, 0},   // 红
            {255, 127, 0}, // 橙
            {255, 255, 0}, // 黄
            {0, 255, 0},   // 绿
            {0, 0, 255},   // 蓝
            {75, 0, 130},  // 靛
            {148, 0, 211}  // 紫
        };
        int rainbow_flow_pos_ = 0;

        // 小夜灯相关
        volatile bool nightlight_locked_ = false; // true=手动开启小夜灯，阻止其他灯效覆盖
        // 默认改为更亮的暖色以便小夜灯可见性更好
        uint8_t nightlight_r_ = 255; // 暖白偏红
        uint8_t nightlight_g_ = 223; // 暖白偏绿
        uint8_t nightlight_b_ = 127; // 暖白偏蓝
        int nightlight_brightness_ = 90; // 0-100，提升默认亮度

    // 标志：控制是否允许灯效自动/基于状态启动
    // true = 灯效允许（默认），false = 禁用所有自动灯效
    volatile bool enabled_ = true;

        uint8_t scale(uint8_t c) const;

        static void EffectTask(void *arg);
        void StartEffectTask();
        void StopEffectTask();

    public:
        explicit Ws2812ControllerMCP();
        ~Ws2812ControllerMCP();

        void RegisterMcpTools();
    // user_initiated=true 表示由用户主动调用（将禁用灯效 enabled_ = false）
    void TurnOff(bool user_initiated = false);
        void SetColor(uint8_t r, uint8_t g, uint8_t b);
        void StartEffect(Ws2812EffectType effect);
        void StartScrollEffect(int interval_ms);
        void StartBlinkEffect(int interval_ms);
        void StartVolumeEffect();
        void StartColorVolumeEffect();
        void ClearLED();

        // 小夜灯控制
        void StartNightlight();
        void StopNightlight();
        void TurnOnNightlightManual();
        void TurnOffNightlightManual();
        bool IsNightlightActive() const;
        // 可调小夜灯参数
        void SetNightlightBrightness(int value);
        void SetNightlightColor(uint8_t r, uint8_t g, uint8_t b);

    // 参数化接口：按名称启动灯效，返回是否成功匹配到效果并启动
    bool StartEffectByName(const std::string& name);
    // 获取可用效果名称列表（用于 list_effects MCP 调用）
    std::vector<std::string> GetAvailableEffects() const;

        void OnStateChanged() override;
    // 设置是否允许灯效（true = 允许）
    void SetEnabled(bool enabled);
    bool IsEnabled() const;
        };

} // namespace ws2812

// extern "C"  void InitializeWs2812ControllerMCP();

#endif // XIAOZHI_ESP32_DANMAI_WS2812_CONTROLLER_MCP_H