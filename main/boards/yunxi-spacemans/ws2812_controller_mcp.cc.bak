
//  参考OTTO的格式引入mcp控制方法
#include <esp_log.h>
// #include "iot/thing.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "audio_led_meter.h"
#include "mcp_server.h"

#define TAG "Ws2812ControllerMCP"

// uint8_t g_color_r = 0;
// uint8_t g_color_g = 255;
// uint8_t g_color_b = 0;

    enum Ws2812EffectType {
        EFFECT_OFF = 0,
        EFFECT_BREATH = 1,
        EFFECT_VOLUME = 2,
        EFFECT_RAINBOW = 3,
        EFFECT_MARQUEE = 4,
        EFFECT_RAINBOW_FLOW = 5  // 新增彩虹流动效果
    };

    class Ws2812ControllerMCP
    {
    private:
        led_strip_handle_t led_strip_ = nullptr;
        TaskHandle_t effect_task_handle_ = nullptr;
        volatile Ws2812EffectType effect_type_ = EFFECT_OFF;
        volatile bool running_ = false;

        // 动态颜色
        uint8_t color_r_ = 0;
        uint8_t color_g_ = 255;
        uint8_t color_b_ = 0;

        int breath_delay_ms_ = 40; // 默认40ms，呼吸频率减慢一半
        int brightness_ = 50;     // 亮度百分比，0~100

        // 亮度缩放工具
        uint8_t scale(uint8_t c) const {
            return (uint8_t)((int)c * brightness_ / 100);
        }

        // 新增彩虹流动颜色数组
        static const int RAINBOW_COLORS_COUNT = 7;
        static const int COLOR_GAP = 3;  // 每组颜色之间的间隔数
        const uint8_t rainbow_colors_[RAINBOW_COLORS_COUNT][3] = {
            {255, 0, 0},    // 红
            {255, 127, 0},  // 橙
            {255, 255, 0},  // 黄
            {0, 255, 0},    // 绿
            {0, 0, 255},    // 蓝
            {75, 0, 130},   // 靛
            {148, 0, 211}   // 紫
        };
        int rainbow_flow_pos_ = 0;  // 彩虹流动位置

        static void EffectTask(void *arg) {
            Ws2812ControllerMCP *self = static_cast<Ws2812ControllerMCP *>(arg);
            int dir = 1, brightness = 0;
            static int rainbow_base = 0;
            static int marquee_pos = 0;
            ESP_LOGI(TAG, "WS2812灯效任务开始运行");
            while (self->running_) {
                if (self->effect_type_ == EFFECT_BREATH) {
                    // 呼吸灯
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
                    if (brightness >= 80) { brightness = 80; dir = -1; }
                    if (brightness <= 0)  { brightness = 0; dir = 1; }
                    vTaskDelay(pdMS_TO_TICKS(self->breath_delay_ms_));
                // } else if (self->effect_type_ == EFFECT_RAINBOW_FLOW) {
                //     // 彩虹流动效果
                //     for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
                //         int color_idx = (self->rainbow_flow_pos_ + i) % RAINBOW_COLORS_COUNT;
                //         uint8_t r = self->rainbow_colors_[color_idx][0];
                //         uint8_t g = self->rainbow_colors_[color_idx][1];
                //         uint8_t b = self->rainbow_colors_[color_idx][2];
                //         led_strip_set_pixel(self->led_strip_, i, 
                //             self->scale(r), self->scale(g), self->scale(b));
                //     }
                //     for (int i = WS2812_LED_NUM_USED; i < WS2812_LED_NUM; i++) {
                //         led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
                //     }
                //     led_strip_refresh(self->led_strip_);
                //     self->rainbow_flow_pos_ = (self->rainbow_flow_pos_ + 1) % RAINBOW_COLORS_COUNT;
                //     vTaskDelay(pdMS_TO_TICKS(self->breath_delay_ms_));
                }else if (self->effect_type_ == EFFECT_RAINBOW_FLOW) {
                    // 彩虹流动效果
                    for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
                        // 计算当前LED的位置在一组彩虹色+间隔中的哪里
                        int group_size = RAINBOW_COLORS_COUNT + COLOR_GAP; // 7个彩虹色加上间隔的总长度
                        int pos = (self->rainbow_flow_pos_ + i) % group_size;
                        
                        if (pos < RAINBOW_COLORS_COUNT) {
                            // 在彩虹色范围内，显示对应颜色
                            uint8_t r = self->rainbow_colors_[pos][0];
                            uint8_t g = self->rainbow_colors_[pos][1];
                            uint8_t b = self->rainbow_colors_[pos][2];
                            led_strip_set_pixel(self->led_strip_, i, 
                                self->scale(r), self->scale(g), self->scale(b));
                        } else {
                            // 在间隔范围内，显示黑色
                            led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
                        }
                    }
                    
                    // 关闭未使用的LED
                    for (int i = WS2812_LED_NUM_USED; i < WS2812_LED_NUM; i++) {
                        led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
                    }
                    
                    led_strip_refresh(self->led_strip_);
                    self->rainbow_flow_pos_ = (self->rainbow_flow_pos_ + 1) % 
                        (RAINBOW_COLORS_COUNT + COLOR_GAP);
                    vTaskDelay(pdMS_TO_TICKS(self->breath_delay_ms_));
                } else if (self->effect_type_ == EFFECT_RAINBOW) {
                    // 彩虹灯效
                    for (int i = 0; i < WS2812_LED_NUM_USED; i++) {
                        int pos = (rainbow_base + i * 256 / WS2812_LED_NUM_USED) % 256;
                        uint8_t r, g, b;
                        if (pos < 85) {
                            r = pos * 3; g = 255 - pos * 3; b = 0;
                        } else if (pos < 170) {
                            pos -= 85;
                            r = 255 - pos * 3; g = 0; b = pos * 3;
                        } else {
                            pos -= 170;
                            r = 0; g = pos * 3; b = 255 - pos * 3;
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
                    // 跑马灯
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
                } else {
                    // 关灯
                    for (int i = 0; i < WS2812_LED_NUM; i++) {
                        led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
                    }
                    led_strip_refresh(self->led_strip_);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            // 退出时关灯
            for (int i = 0; i < WS2812_LED_NUM; i++) {
                led_strip_set_pixel(self->led_strip_, i, 0, 0, 0);
            }
            led_strip_refresh(self->led_strip_);
            self->effect_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }

        void StartEffectTask() {
            if (!running_) {
                running_ = true;
                xTaskCreate(EffectTask, "ws2812_effect", 4096, this, 5, &effect_task_handle_);
            }
        }

        void StopEffectTask() {
            running_ = false;
            effect_type_ = EFFECT_OFF;
            while (effect_task_handle_ != nullptr) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

    public:
        Ws2812ControllerMCP()
        {
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

            // 传给 audio_led_meter
            audio_led_meter_set_strip(led_strip_);
            RegisterMcpTools();

            ESP_LOGI(TAG, "TEST: WS2812灯带初始化完成");

        }
        void RegisterMcpTools() {
            auto& mcp_server = McpServer::GetInstance();
            // 呼吸灯
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
                "设置呼吸灯速度，单位ms，越大越慢",
                PropertyList({Property("delay", kPropertyTypeInteger, 40, 10, 500)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    int val = properties["delay"].value<int>();
                    if (val < 10)
                        val = 10;
                    if (val > 500)
                        val = 500;
                    breath_delay_ms_ = val;
                    ESP_LOGI(TAG, "设置呼吸灯延迟为%dms", breath_delay_ms_);
                    return true;
                });

            // 设置亮度
            mcp_server.AddTool(
                "self.ws2812.set_brightness",
                "设置灯带亮度，0~100",
                PropertyList({Property("value", kPropertyTypeInteger, 100, 0, 100)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    int val = properties["value"].value<int>();
                    if (val < 0) val = 0;
                    if (val > 100) val = 100;
                    brightness_ = val;
                    audio_led_meter_set_brightness(val); // 同步到音量律动
                    ESP_LOGI(TAG, "设置亮度为%d%%", brightness_);
                    return true;
                }
            );

            // 音量律动
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
                    audio_led_meter_init_colors(); // 重新随机一组颜色
                    ESP_LOGI(TAG, "已随机更换音量律动的灯带配色");
                    return true;
                }
            );

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

            // 彩虹灯效
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

            // 新增彩虹流动灯效
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
                    rainbow_flow_pos_ = 0;  // 重置位置
                    effect_type_ = EFFECT_RAINBOW_FLOW;
                    StartEffectTask();
                    return true;
                });

            // 跑马灯
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

            // 设置颜色
            mcp_server.AddTool(
                "self.ws2812.set_color",
                "设置颜色",
                PropertyList({Property("r", kPropertyTypeInteger, 0, 0, 255),
                              Property("g", kPropertyTypeInteger, 255, 0, 255),
                              Property("b", kPropertyTypeInteger, 0, 0, 255)}),
                [this](const PropertyList &properties) -> ReturnValue
                {
                    color_r_ = properties["r"].value<int>();
                    color_g_ = properties["g"].value<int>();
                    color_b_ = properties["b"].value<int>();
                    // g_color_r = color_r_;
                    // g_color_g = color_g_;
                    // g_color_b = color_b_;
                    return true;
                });

            // 关灯
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

            // 默认关闭音量律动
            audio_led_meter_enable(0);
        }

        ~Ws2812ControllerMCP() {
            StopEffectTask();
        }
    };

static Ws2812ControllerMCP* g_ws2812_controller = nullptr;

void InitializeWs2812ControllerMCP() {
    if (g_ws2812_controller == nullptr) {
        g_ws2812_controller = new Ws2812ControllerMCP();
        ESP_LOGI(TAG, "WS2812控制器MCP版已初始化,并注册MCP工具");
    }
}