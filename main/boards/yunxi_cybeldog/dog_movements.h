#ifndef __DOG_MOVEMENTS_H__
#define __DOG_MOVEMENTS_H__

// ESP32/FreeRTOS/舵机相关头文件
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oscillator.h"

// -- 常量定义（动作参数/方向/幅度等）
#define FORWARD 1      // 前进
#define BACKWARD -1    // 后退
#define LEFT 1         // 左
#define RIGHT -1       // 右
#define BOTH 0         // 同时
#define SMALL 5        // 小幅度
#define MEDIUM 15      // 中幅度
#define BIG 30         // 大幅度

// -- 舵机速度限制（单位：度/秒）
#define SERVO_LIMIT_DEFAULT 240

// -- 舵机编号（用于数组索引，方便管理每个舵机）
#define LEFT_FRONT_LEG 0
#define RIGHT_FRONT_LEG 1
#define LEFT_BEHIND_LEG 2
#define RIGHT_BEHIND_LEG 3
// #define LEFT_HAND 4
// #define RIGHT_HAND 5
#define SERVO_COUNT 4   // 舵机总数

// Otto 机器人动作控制类
class Dog
{
public:
    Dog();   // 构造函数
    ~Dog();  // 析构函数

    // -- 初始化函数
    // 传入每个舵机的引脚编号，左前，右前，左后，右后
    void Init(int left_front_leg, int right_front_leg, int left_behind_leg, int right_behind_leg);

    // -- 舵机绑定/解绑
    void AttachServos();   // 绑定所有舵机
    void DetachServos();   // 解绑所有舵机

    // -- 舵机微调
    void SetTrims(int left_front_leg, int right_front_leg, int left_behind_leg, int right_behind_leg);

    // -- 基础动作函数
    void MoveServos(int time, int servo_target[]); // 多舵机同步移动到目标角度
    void MoveSingle(int position, int servo_number); // 单个舵机移动
    void OscillateServos(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                         double phase_diff[SERVO_COUNT], float cycle); // 正弦波驱动舵机

    // -- 休息/归位
    void Home(); // 所有舵机归位
    bool GetRestState();               // 获取是否处于休息状态
    void SetRestState(bool state);     // 设置休息状态


    // -- 舵机限速
    void EnableServoLimit(int speed_limit_degree_per_sec = SERVO_LIMIT_DEFAULT); // 启用限速
    void DisableServoLimit();
    void StepServos(int rf_now, int rb_now, int lf_now, int lb_now);
    void TestServos(); // 测试舵机是否正常工作
    void ZeroServos(); // 复位所有舵机到中位

    void WalkForward(int steps, int step_time);
    void WalkBackward(int steps, int step_time);
    void TurnRight(int steps, int step_time);
    void TurnLeft(int steps, int step_time);
    void FROUNT_BACK(int steps, int step_time);
    void ShakeHand(int steps, int step_time);
    void Sit(int steps, int step_time);
    void Rest(int steps, int step_time);

private:
    Oscillator servo_[SERVO_COUNT]; // 舵机对象数组
    int servo_pins_[SERVO_COUNT];   // 舵机引脚数组
    int servo_trim_[SERVO_COUNT];   // 舵机微调数组

    unsigned long final_time_;      // 动作结束时间
    unsigned long partial_time_;    // 动作分步时间
    float increment_[SERVO_COUNT];  // 每步增量

    bool is_dog_resting_;          // 是否处于休息状态，归位状态
    // bool has_hands_;                // 是否有手部舵机

    // -- 动作执行核心（正弦波参数驱动所有舵机）
    void Execute(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                 double phase_diff[SERVO_COUNT], float steps);
};

#endif // __DOG_MOVEMENTS_H__