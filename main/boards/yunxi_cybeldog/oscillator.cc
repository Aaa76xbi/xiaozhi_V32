#include "oscillator.h"

#include <driver/ledc.h>      // ESP32的PWM硬件驱动
#include <esp_timer.h>        // ESP32的定时器
#include <algorithm>
#include <cmath>

static const char* TAG = "Oscillator";

// 外部定义的毫秒计时函数，通常返回系统启动后的毫秒数
extern unsigned long IRAM_ATTR millis();

// 全局静态通道占用表（假设只用1~4号通道，0号不用）
static bool servo_channel_used[8] = {false, false, false, false, false, false, false, false};

// 构造函数，初始化成员变量
Oscillator::Oscillator(int trim) {
    trim_ = trim;                // 舵机微调
    diff_limit_ = 0;             // 舵机速度限制，0为不限制
    is_attached_ = false;        // 是否已绑定到引脚

    sampling_period_ = 30;       // 采样周期（毫秒）
    period_ = 2000;              // 一个完整周期的时长（毫秒）
    number_samples_ = period_ / sampling_period_; // 一个周期内的采样点数
    inc_ = 2 * M_PI / number_samples_;            // 每次采样相位递增量

    amplitude_ = 45;             // 振幅
    phase_ = 0;                  // 当前相位
    phase0_ = 0;                 // 初始相位
    offset_ = 0;                 // 偏移量
    stop_ = false;               // 是否停止
    rev_ = false;                // 是否反向

    pos_ = 90;                   // 当前角度
    previous_millis_ = 0;        // 上次采样时间
}

// 析构函数，解绑舵机
Oscillator::~Oscillator() {
    Detach();
}

// 角度转为PWM脉宽（微秒），用于舵机控制
uint32_t Oscillator::AngleToCompare(int angle) {
    return (angle - SERVO_MIN_DEGREE) * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US) /
               (SERVO_MAX_DEGREE - SERVO_MIN_DEGREE) +
           SERVO_MIN_PULSEWIDTH_US;
}

// 判断是否到达下一个采样点
bool Oscillator::NextSample() {
    current_millis_ = millis();

    if (current_millis_ - previous_millis_ > sampling_period_) {
        previous_millis_ = current_millis_;
        return true;
    }

    return false;
}

// 绑定舵机到指定引脚，并初始化PWM
void Oscillator::Attach(int pin, bool rev) {
    ESP_LOGI(TAG, "ATTACH PIN %d rev = %d", pin,rev);
    if (is_attached_) {
        Detach();
    }

    pin_ = pin;
    rev_ = rev;

    // 配置PWM定时器
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 50, // 舵机常用50Hz
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 动态分配未被占用的通道（1~4号）
    int found_channel = -1;
    for (int i = 1; i <= 4; ++i)
    {
        if (!servo_channel_used[i])
        {
            found_channel = i;
            servo_channel_used[i] = true;
            break;
        }
    }
    if (found_channel == -1)
    {
        ESP_LOGE(TAG, "No free LEDC channel for servo!");
        return;
    }
    ledc_channel_ = (ledc_channel_t)found_channel;
    ESP_LOGI(TAG, "Using LEDC channel %d", found_channel);

    // 配置PWM通道
    ledc_channel_config_t ledc_channel = {
        .gpio_num = pin_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_channel_,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ledc_speed_mode_ = LEDC_LOW_SPEED_MODE;

    previous_servo_command_millis_ = millis();

    is_attached_ = true;
}

// 解绑舵机，停止PWM
void Oscillator::Detach() {
    if (!is_attached_)
        return;

    ESP_ERROR_CHECK(ledc_stop(ledc_speed_mode_, ledc_channel_, 0));

    // 释放通道占用
    if (ledc_channel_ >= 1 && ledc_channel_ <= 4)
    {
        servo_channel_used[ledc_channel_] = false;
    }
    is_attached_ = false;
}

// 设置周期（毫秒）
void Oscillator::SetT(unsigned int T) {
    period_ = T;

    number_samples_ = period_ / sampling_period_;
    inc_ = 2 * M_PI / number_samples_;
}

// 设置舵机目标角度
void Oscillator::SetPosition(int position) {
    Write(position);
}

// 刷新舵机位置（用于周期性动作，如摆动）
void Oscillator::Refresh() {
    if (NextSample()) { // 到达采样点才刷新
        if (!stop_) {
            // 计算正弦波位置，实现周期性运动
            int pos = std::round(amplitude_ * std::sin(phase_ + phase0_) + offset_);
            if (rev_)
                pos = -pos;
            ESP_LOGI(TAG, "pos = %d REV_ %d", pos,rev_);
            Write(pos + 90); // 90为中位
        }

        phase_ = phase_ + inc_; // 相位递增
    }
}

// 实际写入舵机角度，并做速度限制和微调
void Oscillator::Write(int position) {
    if (!is_attached_)
        return;

    long currentMillis = millis();
    // 如果设置了速度限制
    if (diff_limit_ > 0) {
        int limit = std::max(
            1, (((int)(currentMillis - previous_servo_command_millis_)) * diff_limit_) / 1000);
        // 如果目标角度变化过大，分步逼近
        if (abs(position - pos_) > limit) {
            pos_ += position < pos_ ? -limit : limit;
        } else {
            pos_ = position;
        }
    } else {
        pos_ = position;
    }
    previous_servo_command_millis_ = currentMillis;

    int angle = pos_ + trim_; // 加上微调

    // 限制角度在0~180度
    angle = std::min(std::max(angle, 0), 180);

    // 计算PWM占空比（适配ESP32的LEDC PWM）
    uint32_t duty = (uint32_t)(((angle / 180.0) * 2.0 + 0.5) * 8191 / 20.0);

    // 设置PWM占空比，驱动舵机
    ESP_ERROR_CHECK(ledc_set_duty(ledc_speed_mode_, ledc_channel_, duty));
    ESP_ERROR_CHECK(ledc_update_duty(ledc_speed_mode_, ledc_channel_));
}