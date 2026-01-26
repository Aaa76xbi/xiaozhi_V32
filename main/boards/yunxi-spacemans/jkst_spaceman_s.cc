#include "wifi_board.h"
#include "audio/codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"

#include "led/single_led.h"
#include "led/circular_strip.h"
// #include "led_strip_control.h" // kevin c3板子配置
// #include "esp32_camera.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_spiffs.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#include "otto_emoji_display.h"


#include "ws2812_controller_mcp.h"
// #include "wallpaper/wallpaper_downloader.h"

#include <wifi_station.h>
#include <esp_wifi.h>

#include "power_save_timer.h"
#include "adc_battery_monitor.h"

// #include <driver/rtc_io.h>
// #include <esp_sleep.h>

#include <lvgl.h>

#define LCD_TYPE_GC9A01_SERIAL

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif
 
#define TAG "YUNXI_SPACEMANS"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

class YUNXI_SPACEMANS : public WifiBoard {
private:
 
    Button boot_button_;
    Button touch_button_;

    LcdDisplay* display_;
    // Esp32Camera *camera_;

    // CircularStrip *led_strip_;
    ws2812::Ws2812ControllerMCP *ws2812_controller_ = nullptr;

    esp_lcd_panel_handle_t panel = nullptr;

    PowerSaveTimer *power_save_timer_ = nullptr;
    AdcBatteryMonitor *adc_battery_monitor_ = nullptr;

    void InitializePowerManager()
    {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_0, 100000, 100000, GPIO_NUM_NC);
        adc_battery_monitor_->OnChargingStatusChanged([this](bool is_charging)
                                                      {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            } });
    }

    void InitializePowerSaveTimer()
    {
        power_save_timer_ = new PowerSaveTimer(-1,60, 300);
        power_save_timer_->OnEnterSleepMode([this]()
                                            { GetDisplay()->SetPowerSaveMode(true); });
        power_save_timer_->OnExitSleepMode([this]()
                                           { GetDisplay()->SetPowerSaveMode(false); });
        power_save_timer_->SetEnabled(true);
    }
    void InitializeSpi()
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void MountStorage() {
        // Mount the SPIFFS partition at /spiffs so wallpaper cache can be used.
        esp_vfs_spiffs_conf_t conf = {
            .base_path = "/spiffs",
            .partition_label = "spiffs",
            .max_files = 5,
            .format_if_mount_failed = true,
        };
        esp_err_t r = esp_vfs_spiffs_register(&conf);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");
        } else {
            ESP_LOGW(TAG, "Failed to mount SPIFFS at /spiffs: %d", r);
        }
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };        
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif
        
        esp_lcd_panel_reset(panel);
 

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef  LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
//         display_ = new SpiLcdDisplay(panel_io, panel,
//                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
//                                     {
//                                         .text_font = &font_puhui_16_4,
//                                         .icon_font = &font_awesome_16_4,
// #if CONFIG_USE_WECHAT_MESSAGE_STYLE
//                                         .emoji_font = font_emoji_32_init(),
// #else
//                                         .emoji_font = DISPLAY_HEIGHT >= 240 ? font_emoji_64_init() : font_emoji_32_init(),
// #endif
//                                     });

        display_ = new OttoEmojiDisplay(
            panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        // Board-specific default: show lock-screen time at top with a 10px margin.
        // if (display_) {
        //     display_->SetLockTimeTop(10);
        // }
        // Defer wallpaper download until network is ready to avoid calling
        // esp_http_client before the TCP/IP stack / netif is up.
        // We'll schedule a small FreeRTOS task that waits for WiFi to be connected
        // and then starts the downloader.
        // auto StartWallpaperWhenReady = [this]() {
        //     // If display was destroyed or we never have networking, abort later.
        //     // Poll for WiFi connection with a timeout.
        //     const TickType_t kDelay = pdMS_TO_TICKS(1000);
        //     int attempts = 0;
        //     const int max_attempts = 30; // wait up to ~30s
        //     while (attempts++ < max_attempts) {
        //         if (WifiStation::GetInstance().IsConnected()) break;
        //         vTaskDelay(kDelay);
        //     }
        //     if (!WifiStation::GetInstance().IsConnected()) {
        //         ESP_LOGW(TAG, "Wallpaper: WiFi not connected, skipping download");
        //         return;
        //     }

        //     ESP_LOGI(TAG, "Wallpaper: WiFi connected, starting wallpaper downloader");
        //        // Start wallpaper downloader with SPIFFS caching enabled (conditional GET)
        //        WallpaperDownloader::Start("http://tonyguo123.xyz/wallpaper/1.jpg", false,
        //         // apply callback (LVGL thread)
        //         [this](const void* img_dsc){
        //             if (img_dsc && display_) {
        //                 const lv_img_dsc_t* img = static_cast<const lv_img_dsc_t*>(img_dsc);
        //                 ESP_LOGI(TAG, "apply_cb: img ptr=%p w=%d h=%d data_size=%u", img->data, (int)img->header.w, (int)img->header.h, (unsigned)img->data_size);
        //                 display_->SetLockBackground(img);
        //                 ESP_LOGI(TAG, "Applied wallpaper via apply_cb");
        //             }
        //         },
        //         // done callback
        //         [this](bool success){
        //             if (!success) ESP_LOGW(TAG, "wallpaper download failed");
        //         });
        // };

        // // Start a detached FreeRTOS task to wait and launch wallpaper downloader.
        // auto wallpaper_task = [](void* arg) {
        //     std::function<void()>* fn = static_cast<std::function<void()>*>(arg);
        //     (*fn)();
        //     delete fn;
        //     vTaskDelete(nullptr);
        // };
        // // allocate on heap so task can own it
        // auto* fnptr = new std::function<void()>(StartWallpaperWhenReady);
        // xTaskCreate(wallpaper_task, "wallpaper_start", 4 * 1024, fnptr, tskIDLE_PRIORITY + 1, nullptr);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
        touch_button_.OnClick([this]()
                              {
                                    auto& app = Application::GetInstance();
                                    if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                                        ResetWifiConfiguration();
                                    }
                                    app.ToggleChatState();
                                });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        ws2812_controller_ = new ws2812::Ws2812ControllerMCP();
    }

    // void InitializeCamera()
    // {
    //     // Open camera power
    //     // pca9557_->SetOutputState(2, 0);

    //     // 手动拉低 PWDN
    //     gpio_config_t io_conf = {};
    //     io_conf.intr_type = GPIO_INTR_DISABLE;
    //     io_conf.mode = GPIO_MODE_OUTPUT;
    //     io_conf.pin_bit_mask = (1ULL << CAMERA_PIN_PWDN);
    //     io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    //     io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    //     gpio_config(&io_conf);
    //     gpio_set_level(CAMERA_PIN_PWDN, 0); // 拉低

    //     camera_config_t config = {};
    //     // config.ledc_channel = LEDC_CHANNEL_2; // LEDC通道选择  用于生成XCLK时钟 但是S3不用
    //     // config.ledc_timer = LEDC_TIMER_2;     // LEDC timer选择  用于生成XCLK时钟 但是S3不用

    //     config.pin_d0 = CAMERA_PIN_D0;
    //     config.pin_d1 = CAMERA_PIN_D1;
    //     config.pin_d2 = CAMERA_PIN_D2;
    //     config.pin_d3 = CAMERA_PIN_D3;
    //     config.pin_d4 = CAMERA_PIN_D4;
    //     config.pin_d5 = CAMERA_PIN_D5;
    //     config.pin_d6 = CAMERA_PIN_D6;
    //     config.pin_d7 = CAMERA_PIN_D7;
    //     config.pin_xclk = CAMERA_PIN_XCLK;
    //     config.pin_pclk = CAMERA_PIN_PCLK;
    //     config.pin_vsync = CAMERA_PIN_VSYNC;
    //     config.pin_href = CAMERA_PIN_HREF;
    //     config.pin_sccb_sda = CAMERA_PIN_SIOD; // 这里写-1 表示使用已经初始化的I2C接口
    //     config.pin_sccb_scl = CAMERA_PIN_SIOC;
    //     config.sccb_i2c_port = 1; 
    //     config.pin_pwdn = CAMERA_PIN_PWDN;
    //     config.pin_reset = CAMERA_PIN_RESET;
    //     config.xclk_freq_hz = XCLK_FREQ_HZ;
    //     config.pixel_format = PIXFORMAT_RGB565;
    //     config.frame_size = FRAMESIZE_QVGA;
    //     config.jpeg_quality = 12;
    //     config.fb_count = 1;
    //     config.fb_location = CAMERA_FB_IN_PSRAM;
    //     config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    //     camera_ = new Esp32Camera(config);
    // }

public:
    YUNXI_SPACEMANS() : boot_button_(BOOT_BUTTON_GPIO),
                       touch_button_(TOUCH_BUTTON_GPIO)
    {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        // Ensure storage is mounted before display init / wallpaper downloader runs
        MountStorage();
        InitializeLcdDisplay();
        InitializeButtons();
        // InitializeCamera();
        InitializeIot();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
    }
    //尝试测试
    virtual Led *GetLed() override
    {
        return ws2812_controller_;
    }

    // //单个灯组LampController* GetLamp() override 
    // virtual Led* GetLed() override {
    //     static SingleLed led(BUILTIN_LED_GPIO);
    //     return &led;
    // }


    //灯带控制方法
    // 测试下来可以改编灯的颜色，但是其他功能无效
    // virtual Led *GetLed() override
    // {
    //     static CircularStrip led(WS2812_GPIO, WS2812_LED_NUM_USED);
    //     return &led;
    // }

    // 试试kevin c3灯带效果
// #if CONFIG_IOT_PROTOCOL_MCP
//     virtual Led *GetLed() override
//     {
//         return led_strip_;
//     }
// #endif
    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, 
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif

#if 0
        // 添加：在音频初始化时，强制发送64个SCK脉冲以唤醒INMP441
        // 根据INMP441数据手册，上电后需要提供时钟信号使其退出待机模式
        gpio_set_direction(AUDIO_I2S_MIC_GPIO_SCK, GPIO_MODE_OUTPUT);
        for (int i = 0; i < 64; ++i) {
            gpio_set_level(AUDIO_I2S_MIC_GPIO_SCK, 1);
            usleep(100); // 100us延迟
            gpio_set_level(AUDIO_I2S_MIC_GPIO_SCK, 0);
            usleep(100); // 100us延迟
        }

        // 设置为输入模式，交由I2S控制器控制
        gpio_set_direction(AUDIO_I2S_MIC_GPIO_SCK, GPIO_MODE_INPUT);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override
    {
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override
    {
        if (!enabled)
        {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }
///// 如果没有相机 使用MCP的话，千万不要打开
    // virtual Camera *GetCamera() override
    // {
    //     return camera_;
    // }
};

DECLARE_BOARD(YUNXI_SPACEMANS);
