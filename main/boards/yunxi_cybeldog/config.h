/// @file
/// @brief 2.8存屏幕配置42针的开发板配置文件
///因为屏幕针脚和https://rcnv1t9vps13.feishu.cn/wiki/Zq62wST38iuNxZkwg9JcjkSSnSd 和音量
/// 按键针脚不一样，所以需要重新定义 pin 号。调整屏幕DC引脚为38，RST引脚为45，CS引脚为41，CLK引脚为21，MOSI引脚为47，背光引脚为42

#ifndef _TEST_BOARD_H_
#define _TEST_BOARD_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// 如果使用 Duplex I2S 模式，请注释下面一行
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#else

#define AUDIO_I2S_GPIO_WS GPIO_NUM_4
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7

#endif

#define BUILTIN_LED_GPIO GPIO_NUM_NC
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define TOUCH_BUTTON_GPIO GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO GPIO_NUM_38
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_42 // BLK

#define DISPLAY_CS_PIN GPIO_NUM_41 // CS
#define DISPLAY_DC_PIN GPIO_NUM_40 // DC
#define DISPLAY_RST_PIN GPIO_NUM_45 // RST
#define DISPLAY_MOSI_PIN GPIO_NUM_47 // SDA
#define DISPLAY_CLK_PIN GPIO_NUM_21 // SCL

#define DISPLAY_SPI_SCLK_HZ (40 * 1000 * 1000)

// #ifdef CONFIG_LCD_GC9A01_240X240
#define LCD_TYPE_GC9A01_SERIAL
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 240
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR true
#define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0

// SG90 9G舵机配置
// #define SERVO_GPIO 17                   // 你的舵机信号线接的GPIO
// #define SERVO_MIN_PULSEWIDTH_US (500)  // 0度对应的脉宽(微秒)
// #define SERVO_MAX_PULSEWIDTH_US (2400) // 180度对应的脉宽(微秒)
// #define SERVO_MAX_DEGREE (180)         // 最大角度
// #define SERVO_PWM_FREQUENCY (50)       // 舵机一般用50Hz


// 运动控制器配置
// #define RIGHT_LEG_PIN -1
// #define RIGHT_FOOT_PIN -1

// #define LEFT_LEG_PIN GPIO_NUM_17
// #define LEFT_FOOT_PIN GPIO_NUM_18


//拼装模块对应针脚
// #define LEFT_FRONT_LEG_PIN  GPIO_NUM_17
// #define LEFT_BEHIND_LEG_PIN GPIO_NUM_8


// #define RIGHT_FRONT_LEG_PIN GPIO_NUM_13
// #define RIGHT_BEHIND_PIN   GPIO_NUM_14

// 狗子开发板

// 运动控制器配置
// 运动控制器对应针脚
#define DJ1 GPIO_NUM_17
#define DJ2 GPIO_NUM_18
#define DJ3 GPIO_NUM_8
#define DJ4 GPIO_NUM_9
#define DJ5 GPIO_NUM_10
#define DJ6 GPIO_NUM_11
#define DJ7 GPIO_NUM_14
#define DJ8 GPIO_NUM_48

// 拼装模块对应针脚
#define LEFT_FRONT_LEG_PIN DJ1
#define LEFT_BEHIND_LEG_PIN DJ2

#define RIGHT_FRONT_LEG_PIN DJ8
#define RIGHT_BEHIND_PIN DJ7

// 电机配置
// #define LEDC_M1_CHANNEL_B1_PIN GPIO_NUM_13
// #define LEDC_M1_CHANNEL_F1_PIN GPIO_NUM_12

// #define LEFT_HAND_PIN -1
// #define RIGHT_HAND_PIN  -1


// 4G模块配置
// #define ML307_RX_PIN GPIO_NUM_43
// #define ML307_TX_PIN GPIO_NUM_44
// #define ML307_EN_PIN GPIO_NUM_2


// WS2812灯珠配置
#define WS2812_GPIO GPIO_NUM_2 // WS2812灯珠数据线GPIO
#define WS2812_LED_NUM 64     // 总灯珠数
#define WS2812_LED_NUM_USED 8 // 实际用的灯珠数
#endif // _BOARD_CONFIG_H_
