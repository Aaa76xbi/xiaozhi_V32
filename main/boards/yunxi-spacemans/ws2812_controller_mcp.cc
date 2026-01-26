// ws2812_controller_mcp.cc
#include "ws2812_controller_mcp.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include "audio_led_meter.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "application.h"
#include "led/led.h"  // 确保引入 Led 接口定义

#define TAG "Ws2812ControllerMCP"

namespace ws2812 {

Ws2812ControllerMCP::Ws2812ControllerMCP() {
    ESP_LOGI(TAG, "初始化WS2812灯带控制器");
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_GPIO,
        .max_leds = WS2812_LED_NUM,
        .led_model = LED_MODEL_WS2812,
        .flags = {
            .invert_out = false
        }
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = {
            .with_dma = false
        }
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    audio_led_meter_set_strip(led_strip_);
    RegisterMcpTools();
    

    ESP_LOGI(TAG, "TEST: WS2812灯带初始化完成");
}

Ws2812ControllerMCP::~Ws2812ControllerMCP() {
    StopEffectTask();
}

uint8_t Ws2812ControllerMCP::scale(uint8_t c) const {
    return (uint8_t)((int)c * brightness_ / 100);
}

void Ws2812ControllerMCP::EffectTask(void* arg) {
    Ws2812ControllerMCP* self = static_cast<Ws2812ControllerMCP*>(arg);
    int dir = 1, brightness = 0;
    int rainbow_base = 0;
    int marquee_pos = 0;

    ESP_LOGI(TAG, "WS2812灯效任务开始运行");
    while (self->running_) {
        if (self->effect_type_ == EFFECT_BREATHING) {
            for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
                uint8_t r = self->scale(self->color_r_ * brightness / 80);
                uint8_t g = self->scale(self->color_g_ * brightness / 80);
                uint8_t b = self->scale(self->color_b_ * brightness / 80);
                led_strip_set_pixel(self->led_strip_, i, r, g, b);
            }
            for (int i = WS2812_LED_NUM_USED; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(self->led_strip_);
            brightness += dir * 5;
            if (brightness >= 80) {
                brightness = 80;
                dir = -1;
            }
            if (brightness <= 0) {
                brightness = 0;
                dir = 1;
            }
            vTaskDelay(pdMS_TO_TICKS(self->breath_delay_ms_));
        } else if (self->effect_type_ == EFFECT_RAINBOW_FLOW) {
            for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
                int group_size = RAINBOW_COLORS_COUNT + COLOR_GAP;
                int pos = (self->rainbow_flow_pos_ + i) % group_size;

                if (pos < RAINBOW_COLORS_COUNT) {
                    uint8_t r = self->rainbow_colors_[pos][0];
                    uint8_t g = self->rainbow_colors_[pos][1];
                    uint8_t b = self->rainbow_colors_[pos][2];
                    led_strip_set_pixel(self->led_strip_, i, self->scale(r), self->scale(g), self->scale(b));
                } else {
                    led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
                }
            }
            for (int i = WS2812_LED_NUM_USED; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(self->led_strip_);
            self->rainbow_flow_pos_ = (self->rainbow_flow_pos_ + 1) % (RAINBOW_COLORS_COUNT + COLOR_GAP);
            vTaskDelay(pdMS_TO_TICKS(self->breath_delay_ms_));
        } else if (self->effect_type_ == EFFECT_RAINBOW) {
            for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
                int pos = (rainbow_base + i * 256 / WS2812_LED_NUM_USED) % 256;
                uint8_t r, g, b;
                if (pos < 85) {
                    r = pos * 3;
                    g = 255 - pos * 3;
                    b = 0;
                } else if (pos < 170) {
                    pos -= 85;
                    r = 255 - pos * 3;
                    g = 0;
                    b = pos * 3;
                } else {
                    pos -= 170;
                    r = 0;
                    g = pos * 3;
                    b = 255 - pos * 3;
                }
                led_strip_set_pixel(self->led_strip_, i, self->scale(r), self->scale(g), self->scale(b));
            }
            for (int i = WS2812_LED_NUM_USED; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(self->led_strip_);
            rainbow_base = (rainbow_base + 5) % 256;
            vTaskDelay(pdMS_TO_TICKS(50));
        } else if (self->effect_type_ == EFFECT_MARQUEE) {
            for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
                if (i == marquee_pos)
                    led_strip_set_pixel(self->led_strip_, i, self->scale(self->color_r_), self->scale(self->color_g_), self->scale(self->color_b_));
                else
                    led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
            }
            for (int i = WS2812_LED_NUM_USED; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(self->led_strip_);
            marquee_pos = (marquee_pos + 1) % WS2812_LED_NUM_USED;
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        else if (self->effect_type_ == EFFECT_SCROLL)
        {
            // ✅ 滚动灯逻辑
            for (int i = 0; i < WS2812_LED_NUM_USED; i++)
            {
                if (i == self->scroll_offset_)
                {
                    led_strip_set_pixel(self->led_strip_, i, self->scale(self->color_r_), self->scale(self->color_g_), self->scale(self->color_b_));
                }
                else
                {
                    led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
                }
            }
            led_strip_refresh(self->led_strip_);
            self->scroll_offset_ = (self->scroll_offset_ + 1) % WS2812_LED_NUM_USED;
            vTaskDelay(pdMS_TO_TICKS(100)); // 可配置为参数
        }
        else if (self->effect_type_ == EFFECT_NIGHTLIGHT)
        {
            // 小夜灯：柔和常亮，使用 nightlight_* 颜色与 brightness
            for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
                uint8_t r = (uint8_t)((int)self->nightlight_r_ * self->nightlight_brightness_ / 100);
                uint8_t g = (uint8_t)((int)self->nightlight_g_ * self->nightlight_brightness_ / 100);
                uint8_t b = (uint8_t)((int)self->nightlight_b_ * self->nightlight_brightness_ / 100);
                led_strip_set_pixel(self->led_strip_, i, self->scale(r), self->scale(g), self->scale(b));
            }
            for (int i = WS2812_LED_NUM_USED; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(self->led_strip_);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
            else if (self->effect_type_ == EFFECT_BLINK)
            {
                // ✅ 闪烁灯逻辑（使用独立的 blink_* 颜色，避免覆盖通用 color_*）
                for (int i = 0; i < WS2812_LED_NUM_USED; i++)
                {
                    uint8_t r = self->blink_state_ ? self->scale(self->blink_r_) : 0;
                    uint8_t g = self->blink_state_ ? self->scale(self->blink_g_) : 0;
                    uint8_t b = self->blink_state_ ? self->scale(self->blink_b_) : 0;
                    led_strip_set_pixel(self->led_strip_, i, r, g, b);
                }
                led_strip_refresh(self->led_strip_);
                self->blink_state_ = !self->blink_state_;
                vTaskDelay(pdMS_TO_TICKS(self->blink_interval_));
            }
        else
        {
            for (int i = 0; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(self->led_strip_);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    for (int i = 0; i < WS2812_LED_NUM; i++) {
        led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
    }
    led_strip_refresh(self->led_strip_);
    self->effect_task_handle_ = nullptr;
    vTaskDelete(NULL);
}

void Ws2812ControllerMCP::StartEffectTask() {
    if (!running_) {
        // 不在这里自动改变 enabled_，只允许显式工具或 SetEnabled 解除/设置
        running_ = true;
        xTaskCreate(EffectTask, "ws2812_effect", 4096, this, 5, &effect_task_handle_);
    }
}



void Ws2812ControllerMCP::StopEffectTask() {
    running_ = false;
    effect_type_ = EFFECT_OFF;
    while (effect_task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void Ws2812ControllerMCP::RegisterMcpTools() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.ws2812.breathing",
        "开启呼吸灯灯效",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!enabled_) {
                ESP_LOGI(TAG, "呼吸灯请求被忽略：灯效被禁用");
                return std::string("disabled");
            }
            audio_led_meter_enable(0);
            ESP_LOGI(TAG, "设置呼吸灯（breathing）效果");
            StopEffectTask();
            for (int i = 0; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(led_strip_);
            effect_type_ = EFFECT_BREATHING;
            StartEffectTask();
            return true;
        });

    mcp_server.AddTool(
        "self.ws2812.set_breath_delay",
        "设置呼吸灯（breathing）速度，单位ms，越大越慢，最大不能超过500",
        PropertyList({Property("delay", kPropertyTypeInteger, 40, 10, 500)}),
        [this](const PropertyList &properties) -> ReturnValue
        {
            
            int val = properties["delay"].value<int>();
            ESP_LOGI(TAG, "val is %dms", val);
            if (val < 10)
                val = 10;
            if (val > 500)
                val = 500;
            breath_delay_ms_ = val;
            ESP_LOGI(TAG, "设置呼吸灯延迟为%dms", breath_delay_ms_);
            return true;
        });



    mcp_server.AddTool(
        "self.ws2812.set_brightness",
        "设置灯带亮度，0~100",
        PropertyList({Property("value", kPropertyTypeInteger, 75, 0, 100)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int val = properties["value"].value<int>();
            if (val < 0) val = 0;
            if (val > 100) val = 100;
            brightness_ = val;
            audio_led_meter_set_brightness(val);
            ESP_LOGI(TAG, "设置亮度为%d%%", brightness_);
            // 如果当前没有运行灯效任务且灯效允许，则立即根据当前颜色刷新实际 LED
            if (effect_type_ == EFFECT_OFF && enabled_) {
                for (int i = 0; i < WS2812_LED_NUM; i++) {
                    led_strip_set_pixel(led_strip_, i, scale(color_r_), scale(color_g_), scale(color_b_));
                }
                led_strip_refresh(led_strip_);
            }
            return true;
        });

        // 参数化启动灯效：通过 name 字符串启动对应效果（降低语义匹配混淆）
        mcp_server.AddTool(
            "self.ws2812.start_effect",
            "按名称启动指定灯效，例如 name=\"breathing\"。可选参数 force_enable=true 表示若灯效被禁用则先启用再启动",
            PropertyList({Property("name", kPropertyTypeString), Property("force_enable", kPropertyTypeBoolean, false)}),
            [this](const PropertyList& properties) -> ReturnValue {
                try {
                    std::string name = properties["name"].value<std::string>();
                    bool force_enable = false;
                    try {
                        force_enable = properties["force_enable"].value<bool>();
                    } catch (...) {
                        force_enable = false;
                    }

                    if (!enabled_) {
                        ESP_LOGI(TAG, "start_effect requested while effects disabled: force_enable=%s", force_enable ? "true" : "false");
                        if (!force_enable) {
                            // 返回明确的字符串，调用方可以据此向用户提示是否要启用
                            return std::string("disabled");
                        }
                        // 强制启用灯效
                        enabled_ = true;
                        ESP_LOGI(TAG, "start_effect: forced enable (enabled_ = true)");
                    }

                    bool ok = StartEffectByName(name);
                    ESP_LOGI(TAG, "start_effect(%s) -> %s", name.c_str(), ok ? "started" : "not_found");
                    if (ok) return std::string("started");
                    return std::string("not_found");
                } catch (const std::exception &e) {
                    ESP_LOGW(TAG, "start_effect: missing or invalid 'name' property: %s", e.what());
                    return std::string("error");
                }
            });

        // 列出可用效果（返回 JSON 数组）
        mcp_server.AddTool(
            "self.ws2812.list_effects",
            "列出可用的灯效名称",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                cJSON* arr = cJSON_CreateArray();
                for (const auto &n : GetAvailableEffects()) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(n.c_str()));
                }
                return arr;
            });

    // 查询当前亮度
    mcp_server.AddTool(
        "self.ws2812.get_brightness",
        "获取当前灯带亮度（0~100）",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            return brightness_;
        });

    mcp_server.AddTool(
        "self.ws2812.volume",
        "开启音量律动效果",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!enabled_) {
                ESP_LOGI(TAG, "音量律动请求被忽略：灯效被禁用");
                return std::string("disabled");
            }
            StopEffectTask();
            ESP_LOGI(TAG, "设置音量律动效果");
            for (int i = 0; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(led_strip_);
            audio_led_meter_enable(1);
            return true;
        });

    mcp_server.AddTool(
        "self.ws2812.random_meter_colors",
        "随机更换音量律动的灯带配色",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            audio_led_meter_init_colors();
            ESP_LOGI(TAG, "已随机更换音量律动的灯带配色");
            return true;
        });

    mcp_server.AddTool(
        "self.ws2812.set_meter_single_color",
        "设置音量律动为单色",
        PropertyList({
            Property("r", kPropertyTypeInteger, 0, 0, 255),
            Property("g", kPropertyTypeInteger, 255, 0, 255),
            Property("b", kPropertyTypeInteger, 0, 0, 255)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            uint8_t r = properties["r"].value<int>();
            uint8_t g = properties["g"].value<int>();
            uint8_t b = properties["b"].value<int>();
            audio_led_meter_set_single_color(r, g, b);
            ESP_LOGI(TAG, "设置音量律动为单色: %d,%d,%d", r, g, b);
            return true;
        });

    // 设置音量律动显示模式：0=LEFT_TO_RIGHT, 1=CENTER_OUT, 2=SIDES_IN
    mcp_server.AddTool(
        "self.ws2812.set_meter_mode",
        "设置音量律动显示模式: 0=左->右, 1=中间->两侧, 2=两侧->中间",
        PropertyList({Property("mode", kPropertyTypeInteger, 1, 0, 2)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int mode = properties["mode"].value<int>();
            audio_led_meter_set_mode(static_cast<AudioLedMeterMode>(mode));
            ESP_LOGI(TAG, "设置音量律动显示模式为: %d", mode);
            return true;
        });

    mcp_server.AddTool(
        "self.ws2812.rainbow",
        "彩虹灯效",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!enabled_) {
                ESP_LOGI(TAG, "彩虹灯请求被忽略：灯效被禁用");
                return std::string("disabled");
            }
            audio_led_meter_enable(0);
            StopEffectTask();
            ESP_LOGI(TAG, "设置彩虹灯效");
            for (int i = 0; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(led_strip_);
            effect_type_ = EFFECT_RAINBOW;
            StartEffectTask();
            return true;
        });

    mcp_server.AddTool(
        "self.ws2812.rainbow_flow",
        "彩虹流动灯效，7种颜色依次流动显示",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!enabled_) {
                ESP_LOGI(TAG, "彩虹流动请求被忽略：灯效被禁用");
                return std::string("disabled");
            }
            audio_led_meter_enable(0);
            StopEffectTask();
            ESP_LOGI(TAG, "设置彩虹流动灯效");
            for (int i = 0; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(led_strip_);
            rainbow_flow_pos_ = 0;
            effect_type_ = EFFECT_RAINBOW_FLOW;
            StartEffectTask();
            return true;
        });

    mcp_server.AddTool(
        "self.ws2812.marquee",
        "跑马灯效果",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!enabled_) {
                ESP_LOGI(TAG, "跑马灯请求被忽略：灯效被禁用");
                return std::string("disabled");
            }
            audio_led_meter_enable(0);
            StopEffectTask();
            ESP_LOGI(TAG, "设置跑马灯效果");
            for (int i = 0; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(led_strip_);
            effect_type_ = EFFECT_MARQUEE;
            StartEffectTask();
            return true;
        });

    mcp_server.AddTool(
        "self.ws2812.set_color",
        "设置颜色",
        PropertyList({Property("r", kPropertyTypeInteger, 0, 0, 255),
                      Property("g", kPropertyTypeInteger, 255, 0, 255),
                      Property("b", kPropertyTypeInteger, 0, 0, 255)}),
        [this](const PropertyList& properties) -> ReturnValue {
            color_r_ = properties["r"].value<int>();
            color_g_ = properties["g"].value<int>();
            color_b_ = properties["b"].value<int>();
            return true;
        });



    // 获取当前灯效是否启用
    mcp_server.AddTool("self.ws2812.status",
                       "当查询当前灯带或灯效是否打开时，返回灯带或灯效当前的状态（true=打开，false=关闭）",
                       PropertyList(),
                       [this](const PropertyList& properties) -> ReturnValue {
                           return enabled_;
                       });

    // 添加一个 MCP 工具以允许恢复自动响应（启用灯效）
    mcp_server.AddTool(
        "self.ws2812.on",
        "打开灯效或者打开灯带或灯箱，或打开所有灯效（灯效被禁用时启用）",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            enabled_ = true;
            ESP_LOGI(TAG, "启用灯效");
            // 触发一次状态检查以让灯根据当前设备状态恢复
            OnStateChanged();
            return true;
        });
    mcp_server.AddTool(
        "self.ws2812.off",
        "关闭灯效或者关闭灯带，或关闭灯箱，或关闭所有灯效，当客户端请求关闭灯效时，会调用此方法",
        PropertyList(),
        [this](const PropertyList &properties) -> ReturnValue
        {
            // 用户手动关闭：通过 TurnOff(true) 设置 manual_off_
            TurnOff(true);
            return true;
        });
    // 手动开启小夜灯（锁定其他灯效）
    mcp_server.AddTool(
        "self.ws2812.nightlight_on",
        "手动开启小夜灯，锁定其他灯效直到手动关闭",
        PropertyList(),
        [this](const PropertyList &properties) -> ReturnValue {
            ESP_LOGI(TAG, "MCP: nightlight_on 请求，开启小夜灯并锁定");
            TurnOnNightlightManual();
            return true;
        });

    // 手动关闭小夜灯，恢复默认灯语
    mcp_server.AddTool(
        "self.ws2812.nightlight_off",
        "关闭小夜灯并恢复默认灯语",
        PropertyList(),
        [this](const PropertyList &properties) -> ReturnValue {
            ESP_LOGI(TAG, "MCP: nightlight_off 请求，解除小夜灯锁定并恢复状态");
            TurnOffNightlightManual();
            return true;
        });

        // 设置小夜灯亮度（0-100）
        mcp_server.AddTool(
            "self.ws2812.set_nightlight_brightness",
            "设置小夜灯亮度，0~100（越大越亮）",
            PropertyList({Property("value", kPropertyTypeInteger, 80, 0, 100)}),
            [this](const PropertyList &properties) -> ReturnValue {
                int val = properties["value"].value<int>();
                SetNightlightBrightness(val);
                ESP_LOGI(TAG, "MCP: 设置小夜灯亮度为 %d", val);
                return true;
            });

        // 设置小夜灯颜色
        mcp_server.AddTool(
            "self.ws2812.set_nightlight_color",
            "设置小夜灯颜色，参数 r/g/b 0~255",
            PropertyList({Property("r", kPropertyTypeInteger, 255, 0, 255), Property("g", kPropertyTypeInteger, 223, 0, 255), Property("b", kPropertyTypeInteger, 127, 0, 255)}),
            [this](const PropertyList &properties) -> ReturnValue {
                uint8_t r = properties["r"].value<int>();
                uint8_t g = properties["g"].value<int>();
                uint8_t b = properties["b"].value<int>();
                SetNightlightColor(r, g, b);
                ESP_LOGI(TAG, "MCP: 设置小夜灯颜色为 %d,%d,%d", r, g, b);
                return true;
            });
    mcp_server.AddTool(
    "self.ws2812.scroll",
    "滚动灯效果",
    PropertyList(),
    [this](const PropertyList& properties) -> ReturnValue {
        if (!enabled_) {
            ESP_LOGI(TAG, "滚动灯请求被忽略：灯效被禁用");
            return std::string("disabled");
        }
        ESP_LOGI(TAG, "设置滚动灯效果");
        StopEffectTask();
        effect_type_ = EFFECT_SCROLL;
        StartEffectTask();
        return true;
    });

    mcp_server.AddTool(
        "self.ws2812.blink",
        "闪烁灯效果（使用当前颜色；参数仅 interval）",
        PropertyList({
            Property("interval", kPropertyTypeInteger, 500, 100, 2000)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!enabled_) {
                ESP_LOGI(TAG, "闪烁灯请求被忽略：灯效被禁用");
                return std::string("disabled");
            }

            // 使用当前通用颜色作为闪烁颜色（保持与呼吸/滚动颜色一致）
            blink_r_ = color_r_;
            blink_g_ = color_g_;
            blink_b_ = color_b_;

            int interval = properties["interval"].value<int>();
            ESP_LOGI(TAG, "设置闪烁灯效果: %d,%d,%d @ %dms (使用通用 color_)", blink_r_, blink_g_, blink_b_, interval);
            StopEffectTask();
            blink_interval_ = interval;
            effect_type_ = EFFECT_BLINK;
                StartEffectTask();
            return true;
        });

    audio_led_meter_enable(0);
}

void Ws2812ControllerMCP::StartEffect(Ws2812EffectType effect) {
    if (!enabled_) {
        ESP_LOGI(TAG, "StartEffect: 被忽略，因为灯效已禁用");
        return;
    }
    // 若小夜灯被手动锁定且当前要启动的不是小夜灯，则忽略请求
    if (nightlight_locked_ && effect != EFFECT_NIGHTLIGHT) {
        ESP_LOGI(TAG, "StartEffect: 忽略，因为小夜灯处于手动锁定状态");
        return;
    }
    if (effect_type_ != effect) {
        effect_type_ = effect;
        StartEffectTask();  // 启动灯效任务
    }
}

void Ws2812ControllerMCP::SetColor(uint8_t r, uint8_t g, uint8_t b) {
    color_r_ = r;
    color_g_ = g;
    color_b_ = b;

    // 如果当前没有运行灯效任务，则直接设置颜色
    if (effect_type_ == EFFECT_OFF) {
        // 如果灯效被禁用，不要点亮灯
        if (!enabled_) return;
        for (int i = 0; i < WS2812_LED_NUM; i++) {
            led_strip_set_pixel(led_strip_, i, scale(r), scale(g), scale(b));
        }
    }
}

void Ws2812ControllerMCP::StartScrollEffect(int interval_ms) {
    if (!enabled_) {
        ESP_LOGI(TAG, "StartScrollEffect: 被忽略，因为灯效已禁用");
        return;
    }
    if (effect_type_ != EFFECT_SCROLL) {
        effect_type_ = EFFECT_SCROLL;
        scroll_offset_ = 0;
        StartEffectTask();
    }
}

void Ws2812ControllerMCP::StartBlinkEffect(int interval_ms) {
    if (!enabled_) {
        ESP_LOGI(TAG, "StartBlinkEffect: 被忽略，因为灯效已禁用");
        return;
    }
    // 在启动闪烁效果时，使用当前通用颜色作为闪烁颜色，
    // 这样无论是通过 MCP 还是内部状态触发（OnStateChanged）都会一致。
    blink_r_ = color_r_;
    blink_g_ = color_g_;
    blink_b_ = color_b_;

    blink_interval_ = interval_ms;
    if (effect_type_ != EFFECT_BLINK) {
        effect_type_ = EFFECT_BLINK;
        StartEffectTask();
    }
}

// 音量律动效果的实现
void Ws2812ControllerMCP::StartVolumeEffect() {
    if (!enabled_) {
        ESP_LOGI(TAG, "StartVolumeEffect: 被忽略，因为灯效已禁用");
        return;
    }
    StopEffectTask();
    ESP_LOGI(TAG, "设置音量律动效果");
    for (int i = 0; i < WS2812_LED_NUM; i++)
    {
        led_strip_set_pixel(led_strip_, i, 0, 0, 0);
    }
    led_strip_refresh(led_strip_);
    audio_led_meter_enable(1);
}

// 设置彩色律动效果
void Ws2812ControllerMCP::StartColorVolumeEffect()
{
    StartVolumeEffect();
    audio_led_meter_init_colors(); // 重新随机一组颜色
    ESP_LOGI(TAG, "已随机更换音量律动的灯带配色");
}

void Ws2812ControllerMCP::ClearLED()
{
    StopEffectTask();
    ESP_LOGI(TAG, "设置音量律动效果");
    for (int i = 0; i < WS2812_LED_NUM; i++)
    {
        led_strip_set_pixel(led_strip_, i, 0, 0, 0);
    }
    led_strip_refresh(led_strip_);
    ESP_LOGI(TAG, "清除所有LED灯");
}

void Ws2812ControllerMCP::TurnOff(bool user_initiated)
{
    audio_led_meter_enable(0);
    effect_type_ = EFFECT_OFF;
    StopEffectTask();
    if (user_initiated) {
        // 用户通过 MCP off 禁用灯效
        enabled_ = false;
        ESP_LOGI(TAG, "用户禁用灯效（enabled_ = false）");
    } else {
        ESP_LOGI(TAG, "系统请求关闭灯带（不改变 enabled_）");
    }
    for (int i = 0; i < WS2812_LED_NUM; i++)
    {
        led_strip_set_pixel(led_strip_, i, 0, 0, 0);
    }
    led_strip_refresh(led_strip_);
}

void Ws2812ControllerMCP::OnStateChanged() {
    // 如果灯效被全局禁用（enabled_ == false），则忽略设备状态变化
    if (!enabled_) {
        return;
    }
    // 若手动开启了小夜灯，则不允许设备自动状态改变覆盖小夜灯
    if (nightlight_locked_) {
        ESP_LOGI(TAG, "OnStateChanged: 小夜灯被手动锁定，忽略设备状态变化");
        return;
    }
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();

    switch (device_state) {
        case kDeviceStateStarting: {
            // 示例：启动时设置为呼吸灯（breathing）
            // StartEffect(EFFECT_BREATHING);
            StartScrollEffect(100); // 启动滚动灯
            
            break;
        }
        case kDeviceStateWifiConfiguring: {
            // 闪烁表示 WiFi 配置中
            // StartEffect(EFFECT_BREATH);
            StartBlinkEffect(500); // 
            break;
        }
        case kDeviceStateIdle: {
            // 熄灭
            TurnOff();
            break;
        }
        case kDeviceStateConnecting: {
            // 蓝色常亮
            SetColor(0, 0, 255);
            StartEffect(EFFECT_BREATHING);
            break;
        }
        case kDeviceStateListening: {
            // 蓝色呼吸灯
            // ClearLED();
            StartEffect(EFFECT_BREATHING);
            break;
        }
        case kDeviceStateSpeaking: {
            // 绿色呼吸灯
            // StartEffect(EFFECT_BREATH);
            // StartEffect(EFFECT_RAINBOW_FLOW);

            // 音量律动
            StartColorVolumeEffect();
            break;
        }
        case kDeviceStateUpgrading: {
            // 快速绿色闪烁
            StartEffect(EFFECT_BREATHING);
            break;
        }
        case kDeviceStateActivating: {
            // 慢速绿色闪烁
            StartEffect(EFFECT_BREATHING);
            break;
        }
        default:
            ESP_LOGW("Ws2812ControllerMCP", "未知设备状态: %d", device_state);
            return;
    }
}

void Ws2812ControllerMCP::SetEnabled(bool enabled)
{
    enabled_ = enabled;
    ESP_LOGI(TAG, "SetEnabled: enabled_ = %s", enabled ? "true" : "false");
}

bool Ws2812ControllerMCP::StartEffectByName(const std::string& name) {
    // 规范化输入：去首尾空白，转小写，移除空格和常见后缀（灯/灯效/灯带/效果）以支持中文变体
    std::string n = name;
    // trim
    auto is_space = [](char c){ return std::isspace((unsigned char)c); };
    while (!n.empty() && is_space(n.front())) n.erase(n.begin());
    while (!n.empty() && is_space(n.back())) n.pop_back();
    // 转小写
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c){ return std::tolower(c); });
    // 移除中间空格和常见标点
    n.erase(std::remove_if(n.begin(), n.end(), [](unsigned char c){ return std::isspace(c) || c == '_' || c == '-' || c == '\u3000'; }), n.end());

    // 移除常见中文后缀以增加匹配鲁棒性
    const std::vector<std::string> suffixes = {"灯效", "灯带", "灯光", "灯", "效果"};
    for (const auto &suf : suffixes) {
        // 因为已经转为小写，比较时也使用小写
        // 注意：中文 UTF-8 的比较仍然有效于 std::string 的 find
        auto pos = n.find(suf);
        if (pos != std::string::npos && pos + suf.size() == n.size()) {
            n.erase(pos, suf.size());
            break;
        }
    }

    ESP_LOGI(TAG, "StartEffectByName: normalized name='%s' (orig='%s')", n.c_str(), name.c_str());

    // 采用 substring/包含匹配，允许部分匹配例如 "跑马灯" -> "跑马"
    if (n.find("breath") != std::string::npos || n.find("呼吸") != std::string::npos) {
        StartEffect(EFFECT_BREATHING);
        return true;
    }
    if (n.find("rainbowflow") != std::string::npos || n.find("rainbow_flow") != std::string::npos || n.find("flow") != std::string::npos || n.find("彩虹流动") != std::string::npos || n.find("彩虹") != std::string::npos) {
        // 优先判断更具体的 rainbow_flow
        if (n.find("flow") != std::string::npos || n.find("rainbowflow") != std::string::npos || n.find("彩虹流动") != std::string::npos) {
            StartEffect(EFFECT_RAINBOW_FLOW);
        } else {
            StartEffect(EFFECT_RAINBOW);
        }
        return true;
    }
    if (n.find("marquee") != std::string::npos || n.find("跑马") != std::string::npos || n.find("跑马灯") != std::string::npos) {
        StartEffect(EFFECT_MARQUEE);
        return true;
    }
    if (n.find("scroll") != std::string::npos || n.find("滚动") != std::string::npos) {
        StartEffect(EFFECT_SCROLL);
        return true;
    }
    if (n.find("blink") != std::string::npos || n.find("闪烁") != std::string::npos) {
        StartEffect(EFFECT_BLINK);
        return true;
    }
    if (n.find("volume") != std::string::npos || n.find("音量") != std::string::npos || n.find("律动") != std::string::npos) {
        StartVolumeEffect();
        return true;
    }
    // 小夜灯匹配（中文/英文）
    if (n.find("night") != std::string::npos || n.find("小夜") != std::string::npos || n.find("夜灯") != std::string::npos || n.find("nightlight") != std::string::npos) {
        TurnOnNightlightManual();
        return true;
    }

    ESP_LOGI(TAG, "StartEffectByName: 未识别的灯效名称：'%s'", name.c_str());
    return false;
}

// 小夜灯控制实现
void Ws2812ControllerMCP::StartNightlight()
{
    if (!enabled_) {
        ESP_LOGI(TAG, "StartNightlight: 被忽略，因为灯效已禁用");
        return;
    }
    StopEffectTask();
    audio_led_meter_enable(0);
    effect_type_ = EFFECT_NIGHTLIGHT;
    StartEffectTask();
}

void Ws2812ControllerMCP::StopNightlight()
{
    if (effect_type_ == EFFECT_NIGHTLIGHT) {
        effect_type_ = EFFECT_OFF;
        StopEffectTask();
    }
}

void Ws2812ControllerMCP::TurnOnNightlightManual()
{
    // 手动开启小夜灯并锁定，阻止后续自动/其他灯效覆盖
    enabled_ = true; // 确保允许点灯
    nightlight_locked_ = true;
    ESP_LOGI(TAG, "TurnOnNightlightManual: 手动开启小夜灯 (锁定)");
    StartNightlight();
}

void Ws2812ControllerMCP::TurnOffNightlightManual()
{
    // 解除手动锁定并恢复默认自动行为
    nightlight_locked_ = false;
    ESP_LOGI(TAG, "TurnOffNightlightManual: 关闭小夜灯并解除锁定");
    StopNightlight();
    // 恢复基于设备状态的灯效
    OnStateChanged();
}

bool Ws2812ControllerMCP::IsNightlightActive() const
{
    return nightlight_locked_ || effect_type_ == EFFECT_NIGHTLIGHT;
}

void Ws2812ControllerMCP::SetNightlightBrightness(int value)
{
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    nightlight_brightness_ = value;
    ESP_LOGI(TAG, "SetNightlightBrightness: %d", nightlight_brightness_);
    // 如果正在运行小夜灯，立即刷新（EffectTask 会在下一次循环使用新值）
}

void Ws2812ControllerMCP::SetNightlightColor(uint8_t r, uint8_t g, uint8_t b)
{
    nightlight_r_ = r;
    nightlight_g_ = g;
    nightlight_b_ = b;
    ESP_LOGI(TAG, "SetNightlightColor: %d,%d,%d", r, g, b);
}

std::vector<std::string> Ws2812ControllerMCP::GetAvailableEffects() const {
    return std::vector<std::string>{
        "breathing",
        "rainbow",
        "rainbow_flow",
        "marquee",
        "scroll",
        "blink",
        "volume",
        "nightlight"
    };
}

bool Ws2812ControllerMCP::IsEnabled() const
{
    return enabled_;
}



} // namespace ws2812

// static ws2812::Ws2812ControllerMCP* g_ws2812_controller = nullptr;

// void InitializeWs2812ControllerMCP() {
//     if (g_ws2812_controller == nullptr) {
//         g_ws2812_controller = new ws2812::Ws2812ControllerMCP();
//         ESP_LOGI(TAG, "WS2812控制器MCP版已初始化,并注册MCP工具");
//     }
// }