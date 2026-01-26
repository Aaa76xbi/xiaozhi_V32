// ws2812_controller_mcp.cc
#include "ws2812_controller_mcp.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include "audio_led_meter.h"

#include "audio_led_meter.h"

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
        if (self->effect_type_ == EFFECT_BREATH) {
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
        else if (self->effect_type_ == EFFECT_BLINK)
        {
            // ✅ 闪烁灯逻辑
            for (int i = 0; i < WS2812_LED_NUM_USED; i++)
            {
                uint8_t r = self->blink_state_ ? self->scale(self->color_r_) : 0;
                uint8_t g = self->blink_state_ ? self->scale(self->color_g_) : 0;
                uint8_t b = self->blink_state_ ? self->scale(self->color_b_) : 0;
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
        "self.ws2812.breath",
        "呼吸灯效果",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            audio_led_meter_enable(0);
            ESP_LOGI(TAG, "设置呼吸灯效果");
            StopEffectTask();
            for (int i = 0; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(led_strip_);
            effect_type_ = EFFECT_BREATH;
            StartEffectTask();
            return true;
        });

    mcp_server.AddTool(
        "self.ws2812.set_breath_delay",
        "设置呼吸灯速度，单位ms，越大越慢，最大不能超过500",
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
        PropertyList({Property("value", kPropertyTypeInteger, 40, 0, 100)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int val = properties["value"].value<int>();
            if (val < 0) val = 0;
            if (val > 100) val = 100;
            brightness_ = val;
            audio_led_meter_set_brightness(val);
            ESP_LOGI(TAG, "设置亮度为%d%%", brightness_);
            return true;
        });

    mcp_server.AddTool(
        "self.ws2812.volume",
        "开启音量律动效果",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
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

    mcp_server.AddTool(
        "self.ws2812.rainbow",
        "彩虹灯效",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
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
        "跑马灯",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
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

    mcp_server.AddTool(
        "self.ws2812.off",
        "关闭灯带",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            audio_led_meter_enable(0);
            effect_type_ = EFFECT_OFF;
            StopEffectTask();
            ESP_LOGI(TAG, "关闭灯带");
            for (int i = 0; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(led_strip_);
            return true;
        });
    mcp_server.AddTool(
    "self.ws2812.scroll",
    "滚动灯效果",
    PropertyList(),
    [this](const PropertyList& properties) -> ReturnValue {
        ESP_LOGI(TAG, "设置滚动灯效果");
        StopEffectTask();
        effect_type_ = EFFECT_SCROLL;
        StartEffectTask();
        return true;
    });

mcp_server.AddTool(
    "self.ws2812.blink",
    "闪烁灯效果",
    PropertyList({
        Property("r", kPropertyTypeInteger, 255, 0, 255),
        Property("g", kPropertyTypeInteger, 0, 0, 255),
        Property("b", kPropertyTypeInteger, 0, 0, 255),
        Property("interval", kPropertyTypeInteger, 500, 100, 2000)
    }),
    [this](const PropertyList& properties) -> ReturnValue {
        // StripColor color = {
        //     properties["r"].value<int>(),
        //     properties["g"].value<int>(),
        //     properties["b"].value<int>()
        // };
        color_r_ = properties["r"].value<int>();
        color_g_ = properties["g"].value<int>();
        color_b_ = properties["b"].value<int>();
        
        int interval = properties["interval"].value<int>();
        ESP_LOGI(TAG, "设置闪烁灯效果: %d,%d,%d @ %dms", color_r_, color_g_, color_b_, interval);
        StopEffectTask();
        // blink_color_ = {color_r_, color_g_, color_b_};
        blink_interval_ = interval;
        effect_type_ = EFFECT_BLINK;
        StartEffectTask();
        return true;
    });

    audio_led_meter_enable(0);
}

void Ws2812ControllerMCP::StartEffect(Ws2812EffectType effect) {
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
        for (int i = 0; i < WS2812_LED_NUM; i++) {
            led_strip_set_pixel(led_strip_, i, scale(r), scale(g), scale(b));
        }
    }
}

void Ws2812ControllerMCP::StartScrollEffect(int interval_ms) {
    if (effect_type_ != EFFECT_SCROLL) {
        effect_type_ = EFFECT_SCROLL;
        scroll_offset_ = 0;
        StartEffectTask();
    }
}

void Ws2812ControllerMCP::StartBlinkEffect(int interval_ms) {
    blink_interval_ = interval_ms;
    if (effect_type_ != EFFECT_BLINK) {
        effect_type_ = EFFECT_BLINK;
        StartEffectTask();
    }
}

// 音量律动效果的实现
void Ws2812ControllerMCP::StartVolumeEffect() {
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

void Ws2812ControllerMCP::TurnOff()
{
    audio_led_meter_enable(0);
    effect_type_ = EFFECT_OFF;
    StopEffectTask();
    ESP_LOGI(TAG, "关闭灯带");
    for (int i = 0; i < WS2812_LED_NUM; i++)
    {
        led_strip_set_pixel(led_strip_, i, 0, 0, 0);
    }
    led_strip_refresh(led_strip_);
}

void Ws2812ControllerMCP::OnStateChanged() {
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();

    switch (device_state) {
        case kDeviceStateStarting: {
            // 示例：启动时设置为呼吸灯
            // StartEffect(EFFECT_BREATH);
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
            StartEffect(EFFECT_BREATH);
            break;
        }
        case kDeviceStateListening: {
            // 蓝色呼吸灯
            // ClearLED();
            StartEffect(EFFECT_BREATH);
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
            StartEffect(EFFECT_BREATH);
            break;
        }
        case kDeviceStateActivating: {
            // 慢速绿色闪烁
            StartEffect(EFFECT_BREATH);
            break;
        }
        default:
            ESP_LOGW("Ws2812ControllerMCP", "未知设备状态: %d", device_state);
            return;
    }
}



} // namespace ws2812

// static ws2812::Ws2812ControllerMCP* g_ws2812_controller = nullptr;

// void InitializeWs2812ControllerMCP() {
//     if (g_ws2812_controller == nullptr) {
//         g_ws2812_controller = new ws2812::Ws2812ControllerMCP();
//         ESP_LOGI(TAG, "WS2812控制器MCP版已初始化,并注册MCP工具");
//     }
// }