#include "audio_led_meter.h"
#include <mutex>
#include <cstdint>
#include <array>
#include <tuple>
#include <cstdlib>
#include <ctime>
#include "led_strip.h"
#include "esp_log.h"

static led_strip_handle_t g_led_strip = nullptr;
static std::mutex g_led_mutex;

static int g_led_meter_enabled = 0; // 开启关闭控制，默认关闭

static int g_brightness = 100; // 亮度百分比

// 显示模式
static int g_mode = 1; // 默认使用 CENTER_OUT（1），保留兼容的 LEFT_TO_RIGHT(0)


// 彩色律动：每个灯珠一个颜色
static std::array<std::tuple<uint8_t, uint8_t, uint8_t>, WS2812_LED_NUM_USED> g_led_colors;

// 初始化颜色（可在初始化或切换到律动模式时调用一次）
void audio_led_meter_init_colors() {
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    for (auto& color : g_led_colors) {
        uint8_t r = std::rand() % 256;
        uint8_t g = std::rand() % 256;
        uint8_t b = std::rand() % 256;
        color = std::make_tuple(r, g, b);
    }
}

void audio_led_meter_enable(int enable)
{
    g_led_meter_enabled = enable;
    if (enable) {
        audio_led_meter_init_colors(); // 开启时初始化颜色
    }
}

void audio_led_meter_set_strip(void* led_strip) {
    g_led_strip = reinterpret_cast<led_strip_handle_t>(led_strip);
}

#if 0
void audio_led_meter_update(const int16_t* pcm, size_t samples) {
    if (!g_led_meter_enabled)
        return;

    if (!g_led_strip)
        return;

    int64_t sum = 0;
    for (size_t i = 0; i < samples; ++i) {
        sum += abs(pcm[i]);
    }
    int avg = sum / samples;
    int level = avg / 1000;
    if (level > WS2812_LED_NUM_USED) level = WS2812_LED_NUM_USED;

    std::lock_guard<std::mutex> lock(g_led_mutex);
    for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
        if (i < level) {
            auto [r, g, b] = g_led_colors[i];
            led_strip_set_pixel(g_led_strip, i, r, g, b);
        } else {
            led_strip_set_pixel(g_led_strip, i, 0, 0, 0);
        }
    }

    for (int i = WS2812_LED_NUM_USED; i < WS2812_LED_NUM; i++) {
        led_strip_set_pixel(g_led_strip, i, 0, 0, 0);
    }
    led_strip_refresh(g_led_strip);
}
#else
void audio_led_meter_update(const int16_t *pcm, size_t samples)
{
    if (!g_led_meter_enabled)
        return;

    if (!g_led_strip)
        return;

    int64_t sum = 0;
    for (size_t i = 0; i < samples; ++i)
    {
        sum += abs(pcm[i]);
    }
    int avg = sum / samples;
    int level = avg / 1000;
    if (level > WS2812_LED_NUM_USED)
        level = WS2812_LED_NUM_USED;

    std::lock_guard<std::mutex> lock(g_led_mutex);
    if (g_mode == 0) {
        // LEFT_TO_RIGHT: 旧实现，从左侧开始点亮
        for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
            if (i < level) {
                auto [r, g, b] = g_led_colors[i];
                r = r * g_brightness / 100;
                g = g * g_brightness / 100;
                b = b * g_brightness / 100;
                led_strip_set_pixel(g_led_strip, i, r, g, b);
            } else {
                led_strip_set_pixel(g_led_strip, i, 0, 0, 0);
            }
        }
    } else if (g_mode == 1) {
        // CENTER_OUT: 从中间向两侧扩展（默认）
        int center = WS2812_LED_NUM_USED / 2;
        int half = level / 2;
        for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
            bool on = false;
            if (level % 2 == 0) {
                if (i >= center - half && i < center + half) on = true;
            } else {
                if (i >= center - half && i <= center + half) on = true;
            }
            if (on) {
                auto [r, g, b] = g_led_colors[i];
                r = r * g_brightness / 100;
                g = g * g_brightness / 100;
                b = b * g_brightness / 100;
                led_strip_set_pixel(g_led_strip, i, r, g, b);
            } else {
                led_strip_set_pixel(g_led_strip, i, 0, 0, 0);
            }
        }
    } else {
        // SIDES_IN: 从两边向中间点亮
        int remaining = level;
        int l = 0;
        int r = WS2812_LED_NUM_USED - 1;
        // 先清空
        for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
            led_strip_set_pixel(g_led_strip, i, 0, 0, 0);
        }
        while (remaining > 0 && l <= r) {
            // 左侧
            if (remaining > 0) {
                auto [cr, cg, cb] = g_led_colors[l];
                uint8_t rr = cr * g_brightness / 100;
                uint8_t gg = cg * g_brightness / 100;
                uint8_t bb = cb * g_brightness / 100;
                led_strip_set_pixel(g_led_strip, l, rr, gg, bb);
                remaining--;
            }
            // 右侧
            if (remaining > 0 && l != r) {
                auto [cr, cg, cb] = g_led_colors[r];
                uint8_t rr = cr * g_brightness / 100;
                uint8_t gg = cg * g_brightness / 100;
                uint8_t bb = cb * g_brightness / 100;
                led_strip_set_pixel(g_led_strip, r, rr, gg, bb);
                remaining--;
            }
            l++;
            r--;
        }
    }

    for (int i = WS2812_LED_NUM_USED; i < WS2812_LED_NUM; i++) {
        led_strip_set_pixel(g_led_strip, i, 0, 0, 0);
    }
    led_strip_refresh(g_led_strip);
}
#endif

// 设置亮度，范围0-100
void audio_led_meter_set_brightness(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    g_brightness = percent;
}

// 支持外部设置颜色
void audio_led_meter_set_colors(const std::vector<std::tuple<uint8_t, uint8_t, uint8_t>> &colors)
{
    for (size_t i = 0; i < g_led_colors.size(); ++i)
    {
        if (i < colors.size())
        {
            g_led_colors[i] = colors[i];
        }
    }
}
void audio_led_meter_set_single_color(uint8_t r, uint8_t g, uint8_t b)
{
    for (auto &color : g_led_colors)
    {
        color = std::make_tuple(r, g, b);
    }
}

// 模式设置/获取
void audio_led_meter_set_mode(AudioLedMeterMode mode)
{
    g_mode = static_cast<int>(mode);
}

AudioLedMeterMode audio_led_meter_get_mode()
{
    return static_cast<AudioLedMeterMode>(g_mode);
}