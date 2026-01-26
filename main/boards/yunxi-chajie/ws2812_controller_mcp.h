// ws2812_controller_mcp.h
#ifndef WS2812_CONTROLLER_MCP_H
#define WS2812_CONTROLLER_MCP_H

#include <led_strip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/Task.h>
#include <mcp_server.h>
#include "led/led.h"



namespace ws2812 {

    enum Ws2812EffectType
    {
        EFFECT_OFF = 0,
        EFFECT_BREATH = 1,
        EFFECT_VOLUME = 2,
        EFFECT_RAINBOW = 3,
        EFFECT_MARQUEE = 4,
        EFFECT_RAINBOW_FLOW = 5,
        EFFECT_SCROLL = 6, // ✅ 新增：滚动灯
        EFFECT_BLINK = 7   // ✅ 新增：闪烁灯

    };

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

        uint8_t scale(uint8_t c) const;

        static void EffectTask(void *arg);
        void StartEffectTask();
        void StopEffectTask();

    public:
        explicit Ws2812ControllerMCP();
        ~Ws2812ControllerMCP();

        void RegisterMcpTools();
        void TurnOff();
        void SetColor(uint8_t r, uint8_t g, uint8_t b);
        void StartEffect(Ws2812EffectType effect);
        void StartScrollEffect(int interval_ms);
        void StartBlinkEffect(int interval_ms);
        void StartVolumeEffect();
        void StartColorVolumeEffect();
        void ClearLED();

        void OnStateChanged() override;
        };

} // namespace ws2812

// extern "C"  void InitializeWs2812ControllerMCP();

#endif // WS2812_CONTROLLER_MCP_H