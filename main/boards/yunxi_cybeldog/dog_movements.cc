
#include "dog_movements.h"
#include <algorithm>
#include <cstring>

#include "oscillator.h"

static const char *TAG = "DogMovements";

#define HAND_HOME_POSITION 90

Dog::Dog()
{
    is_dog_resting_ = false;
    // has_hands_ = false;
    // 初始化所有舵机管脚为-1（未连接）
    for (int i = 0; i < SERVO_COUNT; i++)
    {
        servo_pins_[i] = -1;
        servo_trim_[i] = 0;
    }
}

Dog::~Dog()
{
    DetachServos();
}

// 获取当前时间（毫秒），用于动作定时
unsigned long IRAM_ATTR millis()
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

// 初始化舵机引脚，绑定舵机
void Dog::Init(int left_front_leg, int right_front_leg, int left_behind_leg, int right_behind_leg)
{
    servo_pins_[LEFT_FRONT_LEG] = left_front_leg;
    servo_pins_[RIGHT_FRONT_LEG] = right_front_leg;
    servo_pins_[LEFT_BEHIND_LEG] = left_behind_leg;
    servo_pins_[RIGHT_BEHIND_LEG] = right_behind_leg;

    AttachServos();

    is_dog_resting_ = false;
}

// 绑定所有已配置的舵机
void Dog::AttachServos()
{
    for (int i = 0; i < SERVO_COUNT; i++)
    {
        if (servo_pins_[i] != -1)
        {
            // 0=左前, 1=右前, 2=左后, 3=右后
            bool rev = false; 
            servo_[i].Attach(servo_pins_[i], rev);
            ESP_LOGI(TAG, "ATTACH PIN %d irev=%d ", servo_pins_[i],rev);
                }
    }
}

// 解绑所有已配置的舵机
void Dog::DetachServos()
{
    for (int i = 0; i < SERVO_COUNT; i++)
    {
        if (servo_pins_[i] != -1)
        {
            servo_[i].Detach();
        }
    }
}

// 是一个成员函数，用于设置机器狗的伺服电机修正值（trim）。
//该函数根据输入参数更新各部位的修正值，并在条件允许时更新手部修正值，
//最后将修正值应用到所有有效的伺服电机上。

// 设置每个舵机的修正值（trim），用于微调零点
void Dog::SetTrims(int left_front_leg, int right_front_leg, int left_behind_leg, int right_behind_leg)
{

    servo_trim_[LEFT_FRONT_LEG] = left_front_leg;
    servo_trim_[RIGHT_FRONT_LEG] = right_front_leg;
    servo_trim_[LEFT_BEHIND_LEG] = left_behind_leg;
    servo_trim_[RIGHT_BEHIND_LEG] = right_behind_leg;



    for (int i = 0; i < SERVO_COUNT; i++)
    {
        if (servo_pins_[i] != -1)
        {
            servo_[i].SetTrim(servo_trim_[i]);
        }
    }
}

// 多舵机同步移动到目标角度，time为总用时（毫秒），servo_target为目标角度数组
void Dog::MoveServos(int time, int servo_target[])
{
    ESP_LOGI(TAG, "MoveServos time = %d servo_target =%d  %d %d %d servo_now = %d %d %d %d", time, servo_target[0],
             servo_target[1], servo_target[2], servo_target[3],
             servo_[0].GetPosition(), servo_[1].GetPosition(), servo_[2].GetPosition(), servo_[3].GetPosition());
    if (GetRestState() == true)
    {
        SetRestState(false);
    }

    final_time_ = millis() + time;

    // 如果动作时间大于10ms，计算每个舵机每10ms应该移动多少度（步进增量）。
    // 这样可以让舵机平滑地分多步移动，而不是一下子跳到目标角度。
    // if (time > 10)
    if(0)
    {
        for (int i = 0; i < SERVO_COUNT; i++)
        {
            if (servo_pins_[i] != -1)
            {
                // 计算每个舵机的步进增量
                increment_[i] = (servo_target[i] - servo_[i].GetPosition()) / (time / 10.0);
            }
        }

        // 逐步移动舵机到目标角度
        // 这里使用了一个循环，每次迭代都更新舵机的位置，直到达到目标角度。
        // 在整个动作时间内，每10ms推进一次，每次让舵机角度加上步进增量。
        // 这样所有舵机会同步、平滑地移动到目标角度。
        for (int iteration = 1; millis() < final_time_; iteration++)
        {
            partial_time_ = millis() + 10;
            for (int i = 0; i < SERVO_COUNT; i++)
            {
                if (servo_pins_[i] != -1)
                {
                    servo_[i].SetPosition(servo_[i].GetPosition() + increment_[i]);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // 如果动作时间小于等于10ms，直接设置舵机到目标角度
    else
    {
        for (int i = 0; i < SERVO_COUNT; i++)
        {
            if (servo_pins_[i] != -1)
            {
                servo_[i].SetPosition(servo_target[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(time));
    }

    // final adjustment to the target.
    // 在动作结束后，进行一次最终的微小调整，确保所有舵机都到达目标角度。
    // 这里使用了一个循环，最多尝试10次，每次检查所有舵机的当前角度是否等于目标角度。
    bool f = true;
    int adjustment_count = 0;

    // 动作完成后，最多再微调10次，确保所有舵机都精确到达目标角度。
    // 这是因为有些舵机可能因为精度或信号延迟，最后一步没完全到位。

    while (f && adjustment_count < 10)
    {
        f = false;
        for (int i = 0; i < SERVO_COUNT; i++)
        {
            if (servo_pins_[i] != -1 && servo_target[i] != servo_[i].GetPosition())
            {
                f = true;
                break;
            }
        }
        if (f)
        {
            for (int i = 0; i < SERVO_COUNT; i++)
            {
                if (servo_pins_[i] != -1)
                {
                    servo_[i].SetPosition(servo_target[i]);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            adjustment_count++;
        }
    };
}

// 参数顺序：rf, rb, lf, lb
void Dog::StepServos(int rf_now, int rb_now, int lf_now, int lb_now)
{
    if (GetRestState())
    {
        SetRestState(false);
    }
    // 你的舵机编号：0=左前, 1=右前, 2=左后, 3=右后
    // JS顺序：rf=右前, rb=右后, lf=左前, lb=左后
    int target[4];
    target[0] = lf_now; // 左前
    target[1] = rf_now; // 右前
    target[2] = lb_now; // 左后
    target[3] = rb_now; // 右后

    int last[4] = {
        servo_[0].GetPosition(),
        servo_[1].GetPosition(),
        servo_[2].GetPosition(),
        servo_[3].GetPosition()
    };

    float step[4];
    for (int i = 0; i < 4; ++i)
        step[i] = (target[i] - last[i]) / 30.0f;

    for (int a = 0; a < 30; ++a)
    {
        for (int i = 0; i < 4; ++i)
        {
            last[i] += step[i];
            servo_[i].SetPosition(last[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
// 单个舵机移动到目标角度，position为目标角度，servo_number为舵机编号
// 这里的舵机编号是一个常量数组，表示每个舵机在servo_pins_数组中的索引位置。
void Dog::MoveSingle(int position, int servo_number)
{

    // 限制舵机角度在0到180度之间
    // 这里的限制是为了防止舵机超出物理范围，导致损坏或异常行为。
    if (position > 180)
        position = 90;
    if (position < 0)
        position = 90;

    // 如果机器狗处于休息状态，先将其唤醒
    // 这里的休息状态是指机器狗处于静止状态，所有舵机都在默认位置。
    if (GetRestState() == true)
    {
        SetRestState(false);
    }

    if (servo_number >= 0 && servo_number < SERVO_COUNT && servo_pins_[servo_number] != -1)
    {
        servo_[servo_number].SetPosition(position);
    }
}

// 正弦波驱动舵机，amplitude为振幅数组，offset为偏移数组，period为周期，phase_diff为相位差数组，cycle为周期数
// 这里的振幅、偏移、周期和相位差都是针对每个舵机的，可以实现复杂的运动模式。
// void Dog::OscillateServos(
//     int amplitude[SERVO_COUNT],     // 每个舵机的振幅（最大摆动角度）
//     int offset[SERVO_COUNT],        // 每个舵机的偏移量（中心点）
//     int period,                     // 一个完整周期的时长（毫秒）
//     double phase_diff[SERVO_COUNT], // 每个舵机的相位差（决定动作配合）
//     float cycle = 1                 // 要执行的周期数（可为小数）
//     )

// 举例
// 如果你要让四条腿交替摆动，可以设置不同的振幅、偏移和相位差，OscillateServos 会自动让它们配合运动。

    void Dog::OscillateServos(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                         double phase_diff[SERVO_COUNT], float cycle = 1)
{
    for (int i = 0; i < SERVO_COUNT; i++)
    {
        // 设置每个舵机的正弦波参数
        // 振幅、偏移、周期、相位差等都通过 SetA、SetO、SetT、SetPh 设置到每个舵机对象

            if (servo_pins_[i] != -1)
        {
            // ESP_LOGI(TAG, "SET PIN %d",i);
            ESP_LOGI(TAG,"SET PIN O  A T Ph  %d %d %d %d %f",i,offset[i],amplitude[i],period,phase_diff[i]);
            servo_[i].SetO(offset[i]);
            servo_[i].SetA(amplitude[i]);
            servo_[i].SetT(period);
            servo_[i].SetPh(phase_diff[i]);
        }
    }

    // 计算动作结束时间
    // 这样可以控制动作持续多少个周期。

    double ref = millis();
    double end_time = period * cycle + ref;

    // 循环刷新舵机位置
    // 在动作持续期间，不断调用每个舵机的 Refresh()，让它们按照正弦波轨迹运动。

    while (millis() < end_time)
    {
        for (int i = 0; i < SERVO_COUNT; i++)
        {
            if (servo_pins_[i] != -1)
            {
                servo_[i].Refresh();
                ESP_LOGI(TAG, "REFRESH %d",i);
                ESP_LOGI(TAG, "REFRESH %d %d",i,servo_[i].GetPosition());
            }
        }
        vTaskDelay(5); // 控制刷新频率，保证动作平滑。
    }

    // 最后等待10ms，确保所有舵机都完成最后的动作。
    // 稍作延时，保证动作完整。动作结束后微调
    vTaskDelay(pdMS_TO_TICKS(10));// 
}

// 批量执行周期性动作，比如走路 3 步、摆动 2 次等。
// 它会调用 OscillateServos 多次，实现“多周期动作”。
//      int amplitude[SERVO_COUNT],         // 每个舵机的振幅
//     int offset[SERVO_COUNT],        // 每个舵机的偏移
//     int period,                     // 一个周期的时长
//     double phase_diff[SERVO_COUNT], // 每个舵机的相位差
//     float steps = 1.0               // 要执行的周期数（如3.5步）

// 举例，比如如果你要让狗走 2.5 步，Execute 会先让它完整走2步，再走半步，动作自然连贯。
void  Dog::Execute(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                double phase_diff[SERVO_COUNT], float steps = 1.0)
{
    // 唤醒
    if (GetRestState() == true)
    {
        SetRestState(false);
    }

    //  分整数和小数步执行
    // 这里的 steps 是一个浮点数，表示要执行的周期数。
    // 先取整数部分，执行完整的周期数，然后再执行小数部分的周期。
    int cycles = (int)steps;

    //-- Execute complete cycles
    if (cycles >= 1)
        for (int i = 0; i < cycles; i++)
            OscillateServos(amplitude, offset, period, phase_diff);

    //-- Execute the final not complete cycle
    // 这里的 steps - cycles 是小数部分，表示要执行的剩余周期数。
    // 这个部分的周期数可能小于1，所以需要单独处理。
    OscillateServos(amplitude, offset, period, phase_diff, (float)steps - cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
}

///////////////////////////////////////////////////////////////////
//-- HOME = Dog at rest position -------------------------------//
///////////////////////////////////////////////////////////////////
void Dog::Home()
{
    if (is_dog_resting_ == false)
    { // Go to rest position only if necessary
        // 为所有舵机准备初始位置值
        int homes[SERVO_COUNT];
        for (int i = 0; i < SERVO_COUNT; i++)
        { 
                // 腿部和脚部舵机始终复位
                homes[i] = 90;
        }

        
        MoveServos(3000, homes); // 500ms内复位
        ESP_LOGI(TAG, "Dog is at rest position %d", homes[0]);
        is_dog_resting_ = true;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}

void Dog::ZeroServos()
{
    ESP_LOGI(TAG, ">>>>>>>>>>>>>>>ZeroServos>>>>>>>>>>>>>>>>>>>>>>>");
    for (int i = 0; i < SERVO_COUNT; i++)
    {
        if (servo_pins_[i] != -1)
        {
            // 先全部回中位
            servo_[i].SetPosition(90);
            vTaskDelay(pdMS_TO_TICKS(20));
            
        }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}
// 舵机测试
void Dog::TestServos()
{
    ESP_LOGI(TAG, ">>>>>>>>>>>>>>>TestServos>>>>>>>>>>>>>>>>>>>>>>>");
    for (int i = 0; i < SERVO_COUNT; i++)
    {
        if (servo_pins_[i] != -1)
        {
            // 先全部回中位
            servo_[i].SetPosition(90);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    for (int i = 0; i < SERVO_COUNT; i++)
    {
        if (servo_pins_[i] != -1)
        {
            // 让第i个舵机摆动
            ESP_LOGI(TAG, "测试舵机编号 %d", i);
            servo_[i].SetPosition(120); // 向一侧摆动
            vTaskDelay(pdMS_TO_TICKS(800));
            servo_[i].SetPosition(60);  // 向另一侧摆动
            vTaskDelay(pdMS_TO_TICKS(800));
            servo_[i].SetPosition(90);  // 回中位
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void Dog::WalkForward(int steps, int step_time)
{
    ESP_LOGI(TAG, "WalkForward: steps=%d, step_time=%d", steps, step_time);
    int poses[8][4];


    int temp[8][4] = {
        {90, 130, 50, 90},
        {130, 130, 50, 50},
        {130, 90, 90, 50},
        {90, 90, 90, 90},
        {50, 90, 90, 130},
        {50, 50, 130, 130},
        {90, 50, 130, 90},
        {90, 90, 90, 90}};
    memcpy(poses, temp, sizeof(poses));

    int pose_count = sizeof(poses) / sizeof(poses[0]);
    int sub_action_time = step_time / pose_count;

    for (int s = 0; s < steps; ++s)
    {
        for (int p = 0; p < pose_count; ++p)
        {
            MoveServos(sub_action_time, poses[p]);
            vTaskDelay(pdMS_TO_TICKS(sub_action_time)); // 可选
        }
    }
    int stand[4] = {90, 90, 90, 90};
    MoveServos(sub_action_time, stand);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// void Dog::WalkForward(int steps, int step_time)
// {
//     // 步态参数可参考esp-hi或自己调优
//     int amplitude[4] = {30, 30, 30, 30};       // 振幅
//     int offset[4] = {90, 90, 90, 90};          // 中位
//     double phase_diff[4] = {0, M_PI, M_PI, 0}; // 相位差，前后腿交替
//     int period = step_time;                    // 一个周期的时长

//     Execute(amplitude, offset, period, phase_diff, steps);
// }

void Dog::WalkBackward(int steps, int step_time)
{
    ESP_LOGI(TAG, "WalkBack: steps=%d, step_time=%d", steps, step_time);
    int poses[8][4];

    int temp[8][4] = {
        {90, 50, 130, 90},
        {50, 50, 130, 130},
        {50, 90, 90, 130},
        {90, 90, 90, 90},
        {130, 90, 90, 50},
        {130, 130, 50, 50},
        {90, 130, 50, 90},
        {90, 90, 90, 90}};
    memcpy(poses, temp, sizeof(poses));

    int pose_count = sizeof(poses) / sizeof(poses[0]);
    int sub_action_time = step_time / pose_count;

    for (int s = 0; s < steps; ++s)
    {
        for (int p = 0; p < pose_count; ++p)
        {
            MoveServos(sub_action_time, poses[p]);
            vTaskDelay(pdMS_TO_TICKS(sub_action_time)); // 可选
        }
    }
    int stand[4] = {90, 90, 90, 90};
    MoveServos(sub_action_time, stand);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void Dog::TurnRight(int steps, int step_time)
{
    ESP_LOGI(TAG, "turn right : steps=%d, step_time=%d", steps, step_time);
    int poses[4][4];

    int temp[4][4] = {
        {90, 50, 50, 90},
        {130, 50, 50, 130},
        {130, 90, 90, 130},
        {90, 90, 90, 90}};
    memcpy(poses, temp, sizeof(poses));
    int pose_count = sizeof(poses) / sizeof(poses[0]);
    int sub_action_time = step_time / pose_count;

    for (int s = 0; s < steps; ++s)
    {
        for (int p = 0; p < pose_count; ++p)
        {
            MoveServos(sub_action_time, poses[p]);
            vTaskDelay(pdMS_TO_TICKS(sub_action_time)); // 可选
        }
    }
    int stand[4] = {90, 90, 90, 90};
    MoveServos(sub_action_time, stand);
    vTaskDelay(pdMS_TO_TICKS(10));
}

void Dog::TurnLeft(int steps, int step_time)
{
    ESP_LOGI(TAG, "Step4turn left : steps=%d, step_time=%d", steps, step_time);
    int poses[4][4];

    int temp[4][4] = {
        {130, 90, 90, 130},
        {130, 50, 50, 130},
        {90, 50, 90, 90},
        {90, 90, 90, 90}};
    memcpy(poses, temp, sizeof(poses));
    int pose_count = sizeof(poses) / sizeof(poses[0]);
    int sub_action_time = step_time / pose_count;

    for (int s = 0; s < steps; ++s)
    {
        for (int p = 0; p < pose_count; ++p)
        {
            MoveServos(sub_action_time, poses[p]);
            vTaskDelay(pdMS_TO_TICKS(sub_action_time)); // 可选
        }
    }
    int stand[4] = {90, 90, 90, 90};
    MoveServos(sub_action_time, stand);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// 趴下
void Dog::Rest(int steps, int step_time)
{
    ESP_LOGI(TAG, "Step rest ");
    // int rest[4] = {180, 0, 0, 180}; 后
    int rest[4] = {0, 180, 0, 180};
    MoveServos(1000, rest);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// 前后摇摆
void Dog::FROUNT_BACK(int steps, int step_time)
{
     steps = 2; // 前后摇摆动作只执行一次
     step_time=1000; // 前后摇摆动作持续时间为1000毫秒
    ESP_LOGI(TAG, "Step4turn FR ");
    int poses[4][4];

    int temp[4][4] = {
        {30, 150, 30, 150},
        {90, 90, 90, 90},
        {150, 30, 150, 30},
        {90, 90, 90, 90}};
    memcpy(poses, temp, sizeof(poses));
    int pose_count = sizeof(poses) / sizeof(poses[0]);
    int sub_action_time = step_time / pose_count;

    for (int s = 0; s < steps; ++s)
    {
        for (int p = 0; p < pose_count; ++p)
        {
            MoveServos(sub_action_time, poses[p]);
            vTaskDelay(pdMS_TO_TICKS(sub_action_time)); // 可选
        }
    }
    int stand[4] = {90, 90, 90, 90};
    MoveServos(sub_action_time, stand);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// 打招呼
void Dog::ShakeHand(int steps, int step_time)
{
    # if 1
    ESP_LOGI(TAG, "Step4turn welcome,shake hand");
    int poses[4][4];
     steps = 1; // 摇手动作只执行一次
     step_time = 1000; // 摇手动作持续时间为1000毫秒

    int temp[4][4] = {
        {90, 110, 90, 90},
        {90, 145, 90, 40},
        {90, 180, 90, 90},
        {90, 145, 90, 90}};
    memcpy(poses, temp, sizeof(poses));
    int pose_count = sizeof(poses) / sizeof(poses[0]);
    int sub_action_time = step_time / pose_count;

    for (int s = 0; s < steps; ++s)
    {
        for (int p = 0; p < pose_count; ++p)
        {
            MoveServos(sub_action_time, poses[p]);
            vTaskDelay(pdMS_TO_TICKS(sub_action_time)); // 可选
        }
    }
    int stand[4] = {90, 90, 90, 90};
    MoveServos(sub_action_time, stand);
    vTaskDelay(pdMS_TO_TICKS(10));
    # else
    int steps = 1;        // 前后摇摆动作只执行一次
    int step_time = 1000; // 前后摇摆动作持续时间为1000毫秒
    ESP_LOGI(TAG, "Step4turn FR ");
    int poses[4][4];

    int temp[4][4] = {
        {30, 150, 30, 150},
        {90, 90, 90, 90},
        {150, 30, 150, 30},
        {90, 90, 90, 90}};
    memcpy(poses, temp, sizeof(poses));
    int pose_count = sizeof(poses) / sizeof(poses[0]);
    int sub_action_time = step_time / pose_count;

    for (int s = 0; s < steps; ++s)
    {
        for (int p = 0; p < pose_count; ++p)
        {
            MoveServos(sub_action_time, poses[p]);
            vTaskDelay(pdMS_TO_TICKS(sub_action_time)); // 可选
        }
    }
    int stand[4] = {90, 90, 90, 90};
    MoveServos(sub_action_time, stand);
    vTaskDelay(pdMS_TO_TICKS(10));
    # endif
}

// 蹲下,抬头
void Dog::Sit(int steps, int step_time)
{
    # if 1
    ESP_LOGI(TAG, "Step4turn sit");
    step_time = 1000;
    int sit[4] = {90, 90, 0, 179};
    MoveServos(step_time, sit);
    vTaskDelay(pdMS_TO_TICKS(10));
    #else

    int steps = 1;        // 前后摇摆动作只执行一次
    int step_time = 1000; // 前后摇摆动作持续时间为1000毫秒
    ESP_LOGI(TAG, "Step4turn FR ");
    int poses[4][4];

    int temp[4][4] = {
        {30, 150, 30, 150},
        {90, 90, 90, 90},
        {150, 30, 150, 30},
        {90, 90, 90, 90}};
    memcpy(poses, temp, sizeof(poses));
    int pose_count = sizeof(poses) / sizeof(poses[0]);
    int sub_action_time = step_time / pose_count;

    for (int s = 0; s < steps; ++s)
    {
        for (int p = 0; p < pose_count; ++p)
        {
            MoveServos(sub_action_time, poses[p]);
            vTaskDelay(pdMS_TO_TICKS(sub_action_time)); // 可选
        }
    }
    int stand[4] = {90, 90, 90, 90};
    MoveServos(sub_action_time, stand);
    vTaskDelay(pdMS_TO_TICKS(10));
    #endif
}

bool Dog::GetRestState()
{
    return is_dog_resting_;
}

void Dog::SetRestState(bool state)
{
    is_dog_resting_ = state;
}



void Dog::EnableServoLimit(int diff_limit)
{
    for (int i = 0; i < SERVO_COUNT; i++)
    {
        if (servo_pins_[i] != -1)
        {
            servo_[i].SetLimiter(diff_limit);
        }
    }
}

void Dog::DisableServoLimit()
{
    for (int i = 0; i < SERVO_COUNT; i++)
    {
        if (servo_pins_[i] != -1)
        {
            servo_[i].DisableLimiter();
        }
    }
}
