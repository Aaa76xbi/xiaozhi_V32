
/*
    仿照Otto机器人控制器代码结构实现cyberdog控制器
*/

#include <esp_log.h>
#include <cstring>

#include "application.h"
#include "board.h"
#include "config.h"
#include "iot/thing.h"
#include "dog_movements.h"
#include "sdkconfig.h"

#define TAG "DogController"

namespace iot
{

    // 动作参数结构体，描述一次动作的所有参数
    struct DogActionParams
    {
        int action_type; // 动作类型
        int steps;       // 步数或次数
        int speed;       // 速度（数值越小越快）
        // int direction;   // 方向（1=左，-1=右，0=同时）
    };

    // 小狗机器人控制器类，继承自Thing（IoT设备基类）
    class DogController : public Thing
    {
    private:
        Dog dog_;                                 // 动作实现对象
        TaskHandle_t action_task_handle_ = nullptr; // 动作任务句柄
        QueueHandle_t action_queue_;                // 动作队列
        // bool has_hands_ = false;                    // 是否有手部舵机

        // 动作类型枚举
        enum ActionType
        {
            ACTION_FORWARD = 1,        // 前进
            ACTION_BACK = 2,       // 后退
            ACTION_TURN_LEFT = 3,   // 向左转弯
            ACTION_TURN_RIGHT = 4,  // 向右转弯
            ACTION_FROUNT_BACK = 5, // 前后摇摆
            ACTION_SHAKE_HAND = 6,  // 摇手
            ACTION_SIT = 7,         // 蹲下
            ACTION_REST = 8         // 休息一下，趴在地上
        };

        // 限制参数在[min, max]范围内
        static int Limit(int value, int min, int max)
        {
            if (value < min)
            {
                ESP_LOGW(TAG, "参数 %d 小于最小值 %d，设置为最小值", value, min);
                return min;
            }
            if (value > max)
            {
                ESP_LOGW(TAG, "参数 %d 大于最大值 %d，设置为最大值", value, max);
                return max;
            }
            return value;
        }

        // 动作执行任务（FreeRTOS任务函数）
        static void ActionTask(void *arg)
        {
            DogController *controller = static_cast<DogController *>(arg);
            DogActionParams params;
            // controller->dog_.AttachServos(); // 绑定所有舵机

            while (true)
            {
                // 从队列取出一个动作参数
                if (xQueueReceive(controller->action_queue_, &params, pdMS_TO_TICKS(1000)) == pdTRUE)
                {
                    ESP_LOGI(TAG, "执行动作: %d", params.action_type);

                    // 根据动作类型调用对应的Otto动作
                    switch (params.action_type)
                    {
                    case ACTION_FORWARD:
                        controller->dog_.WalkForward(params.steps, params.speed);
                        break;
                    case ACTION_BACK:
                        controller->dog_.WalkBackward(params.steps, params.speed); // 后退
                        break;
                    case ACTION_TURN_LEFT:
                        controller->dog_.TurnLeft(params.steps, params.speed); // 左转
                        break;
                    case ACTION_TURN_RIGHT:
                        controller->dog_.TurnRight(params.steps, params.speed); // 右转
                        break;
                    case ACTION_FROUNT_BACK:
                        controller->dog_.FROUNT_BACK(params.steps, params.speed);
                        break;
                    case ACTION_SHAKE_HAND:
                        controller->dog_.ShakeHand(params.steps, params.speed);
                        break;
                    case ACTION_SIT:
                        controller->dog_.Sit(params.steps, params.speed);
                        break;
                    case ACTION_REST:
                        controller->dog_.Rest(params.steps, params.speed);
                    }
                    vTaskDelay(pdMS_TO_TICKS(200));                               // 动作间隔
                    // controller->dog_.Home(params.action_type < ACTION_HANDS_UP); // 动作后复位
                }
                else
                {
                    // 队列为空，任务完成，复位并释放舵机
                    if (uxQueueMessagesWaiting(controller->action_queue_) == 0)
                    {
                        ESP_LOGI(TAG, "动作队列为空，任务完成");
                        // controller->dog_.Home();
                        vTaskDelay(pdMS_TO_TICKS(500));
                        // controller->dog_.DetachServos();
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        controller->action_task_handle_ = nullptr;
                        vTaskDelete(NULL);
                        break;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(20)); // 任务循环延时
            }
        }

    public:
        // 构造函数，初始化Otto、动作队列、IoT方法
        DogController() : Thing("DogController", "四足机器狗的控制器")
        {
            dog_.Init(LEFT_FRONT_LEG_PIN, RIGHT_FRONT_LEG_PIN, LEFT_BEHIND_LEG_PIN, RIGHT_BEHIND_PIN);

            // 初始化时不绑定舵机，等到执行动作时再绑定
            dog_.ZeroServos(); // 复位所有舵机到中位

            dog_.Home();
            // dog_.TestServos(); // 测试舵机是否正常工作


            ESP_LOGI(TAG, "小狗复位");                                 // 初始化时完全复位，包括手部
            action_queue_ = xQueueCreate(10, sizeof(DogActionParams)); // 创建动作队列

            // 添加suspend方法：清空队列并中断动作
            methods_.AddMethod("suspend", "清空动作队列,中断Dog机器人动作", ParameterList(),
                               [this](const ParameterList &parameters)
                               {
                                   ESP_LOGI(TAG, "停止Dog机器人动作");
                                   if (action_task_handle_ != nullptr)
                                   {
                                       vTaskDelete(action_task_handle_);
                                       action_task_handle_ = nullptr;
                                   }
                                   xQueueReset(action_queue_);
                                   dog_.Home(); // 中断动作时完全复位
                               });

            // 添加AIControl方法：将动作加入队列
            methods_.AddMethod(
                "AIControl", "AI把机器狗待执行动作加入队列,动作需要时间",
                ParameterList(
                    {Parameter("action_type", "动作类型,包含8种，分别如下: 1=前进,2=后退,3=向左转,4=向右转,5=前后摇摆,6=挥手,7=蹲下,8=休息一下", kValueTypeNumber, false),
                     Parameter("steps", "步数,默认为1", kValueTypeNumber, false),
                     Parameter("speed", "速度 (越小越快500-1000)默认1000.", kValueTypeNumber, false),
                    }),
                [this](const ParameterList &parameters)
                {
                    // 读取参数
                    ESP_LOGI(TAG, "AIControl方法被调用,参数: %d %d %d ",parameters["action_type"].number(),
                             parameters["steps"].number(), parameters["speed"].number());
                    int action_type = parameters["action_type"].number();
                    int steps = parameters["steps"].number();
                    int speed = parameters["speed"].number();

                    // 参数范围限制
                    action_type = Limit(action_type, ACTION_FORWARD, ACTION_REST);
                    steps = Limit(steps, 1, 10);
                    speed = Limit(speed, 500, 1000);

                    ESP_LOGI(TAG, "AI控制: 动作类型=%d, 步数=%d, 速度=%d",action_type, steps, speed);

                    // 封装参数并加入队列
                    DogActionParams params;
                    params.action_type = action_type;
                    params.steps = steps;
                    params.speed = speed;
                    // params.direction = direction;

                    xQueueSend(action_queue_, &params, portMAX_DELAY);

                    StartActionTaskIfNeeded();
                });
        }

        // 如果动作任务未启动，则启动
        void StartActionTaskIfNeeded()
        {
            if (action_task_handle_ == nullptr)
            {
                xTaskCreate(ActionTask, "dog_action", 1024 * 3, this, 4, &action_task_handle_);
            }
        }

        // 析构函数，释放资源
        ~DogController()
        {
            if (action_task_handle_ != nullptr)
            {
                vTaskDelete(action_task_handle_);
            }
            vQueueDelete(action_queue_);
        }
    };

} // namespace iot

// 宏，注册为IoT设备
DECLARE_THING(DogController);