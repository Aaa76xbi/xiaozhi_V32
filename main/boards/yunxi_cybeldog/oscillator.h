#ifndef __OSCILLATOR_H__
#define __OSCILLATOR_H__

#include "driver/ledc.h"      // ESP32 PWM硬件驱动
#include "esp_log.h"          // ESP日志
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define M_PI 3.14159265358979323846   // 圆周率常量

#ifndef DEG2RAD
#define DEG2RAD(g) ((g) * M_PI) / 180 // 角度转弧度宏
#endif

// 舵机相关常量定义
#define SERVO_MIN_PULSEWIDTH_US 500           // 舵机最小脉宽（微秒）
#define SERVO_MAX_PULSEWIDTH_US 2500          // 舵机最大脉宽（微秒）
#define SERVO_MIN_DEGREE -90                  // 舵机最小角度
#define SERVO_MAX_DEGREE 90                   // 舵机最大角度
#define SERVO_TIMEBASE_RESOLUTION_HZ 1000000  // PWM时间基准1MHz
#define SERVO_TIMEBASE_PERIOD 20000           // PWM周期20ms

// 振荡器类，用于控制单个舵机的运动
class Oscillator {
public:
    Oscillator(int trim = 0);         // 构造函数，trim为舵机微调
    ~Oscillator();                    // 析构函数

    void Attach(int pin, bool rev = false); // 绑定舵机到指定引脚，rev为是否反向
    void Detach();                          // 解绑舵机

    // 参数设置相关函数
    void SetA(unsigned int amplitude) { amplitude_ = amplitude; }; // 设置振幅
    void SetO(int offset) { offset_ = offset; };                   // 设置偏移
    void SetPh(double Ph) { phase0_ = Ph; };                       // 设置初始相位
    void SetT(unsigned int period);                                // 设置周期
    void SetTrim(int trim) { trim_ = trim; };                      // 设置微调
    void SetLimiter(int diff_limit) { diff_limit_ = diff_limit; }; // 设置速度限制
    void DisableLimiter() { diff_limit_ = 0; };                    // 关闭速度限制
    int GetTrim() { return trim_; };                               // 获取微调值

    void SetPosition(int position);    // 设置舵机目标角度
    void Stop() { stop_ = true; };     // 停止周期性运动
    void Play() { stop_ = false; };    // 恢复周期性运动
    void Reset() { phase_ = 0; };      // 重置相位
    void Refresh();                    // 刷新舵机位置（周期性运动）
    int GetPosition() { return pos_; } // 获取当前舵机角度
    int GetPosition_init();                // 分别获取当前舵机角度,TODO待实现

private:
    bool NextSample();                 // 判断是否到达下一个采样点
    void Write(int position);          // 实际写入舵机角度
    uint32_t AngleToCompare(int angle);// 角度转PWM脉宽

private:
    bool is_attached_;                 // 是否已绑定到引脚

    // 振荡器参数
    unsigned int amplitude_;  // 振幅（最大摆动角度）
    int offset_;              // 偏移量（中心点偏移）
    unsigned int period_;     // 周期（毫秒）
    double phase0_;           // 初始相位（弧度）

    // 内部状态变量
    int pos_;                       // 当前舵机角度
    int pin_;                       // 舵机连接的引脚
    int trim_;                      // 微调角度
    double phase_;                  // 当前相位
    double inc_;                    // 每次采样相位递增量
    double number_samples_;         // 一个周期内的采样点数
    unsigned int sampling_period_;  // 采样周期（毫秒）

    long previous_millis_;          // 上次采样时间
    long current_millis_;           // 当前采样时间

    bool stop_;                     // 是否停止周期性运动
    bool rev_;                      // 是否反向
    int diff_limit_;                // 速度限制参数
    long previous_servo_command_millis_; // 上次舵机命令时间

    ledc_channel_t ledc_channel_;   // LEDC PWM通道号
    ledc_mode_t ledc_speed_mode_;   // LEDC模式（低速/高速）
};

#endif  // __OSCILLATOR_H__