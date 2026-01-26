#include <esp_log.h>
#include "application.h"
#include "board.h"
#include "iot/thing.h"
#include "motion_control.h"

#define TAG "FireMotorController"

namespace iot
{
    // 动作参数结构体
    struct FireMotorActionParams
    {
        int action_type; // 1=发射(正转), 0=停止
        int pwm_percent; // PWM占空比(0~100)
        int duration_ms; // 持续时间(ms)，0为一直转
    };

    class FireMotorController : public Thing
    {
    private:
        TaskHandle_t action_task_handle_ = nullptr;
        QueueHandle_t action_queue_;

        enum ActionType
        {
            ACTION_FIRE = 1,
            ACTION_STOP = 0
        };

        static void ActionTask(void *arg)
        {
            FireMotorController *controller = static_cast<FireMotorController *>(arg);
            FireMotorActionParams params;
            while (true)
            {
                if (xQueueReceive(controller->action_queue_, &params, portMAX_DELAY) == pdTRUE)
                {
                    ESP_LOGI(TAG, "执行动作: %d, pwm=%d, duration=%d", params.action_type, params.pwm_percent, params.duration_ms);
                    if (params.action_type == ACTION_FIRE)
                    {
                        fire_motor_start(params.pwm_percent);
                        if (params.duration_ms > 0)
                        {
                            vTaskDelay(pdMS_TO_TICKS(params.duration_ms));
                            fire_motor_stop();
                        }
                    }
                    else if (params.action_type == ACTION_STOP)
                    {
                        fire_motor_stop();
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

    public:
            FireMotorController() : Thing("FireMotorController", "发射小球电机控制器,包含发射,停止发射两个功能")
        {
            action_queue_ = xQueueCreate(5, sizeof(FireMotorActionParams));
            motor_ledc_init(); // 初始化PWM
                // IoT方法：发射
                methods_.AddMethod(
                    "fire", "发射小球，正转指定时间",
                    ParameterList({Parameter("pwm_percent", "PWM占空比(0~100),默认100", kValueTypeNumber, false),
                                   Parameter("duration_ms", "持续时间(ms)，0为一直转", kValueTypeNumber, false)}),
                    [this](const ParameterList &parameters)
                    {
                        FireMotorActionParams params;
                        params.action_type = ACTION_FIRE;
                        params.pwm_percent = parameters["pwm_percent"].number();
                        params.duration_ms = parameters["duration_ms"].number();
                        xQueueSend(action_queue_, &params, portMAX_DELAY);
                        StartActionTaskIfNeeded();
                    });

            // IoT方法：停止
            methods_.AddMethod(
                "stop", "停止发射",
                ParameterList(),
                [this](const ParameterList &)
                {
                    FireMotorActionParams params;
                    params.action_type = ACTION_STOP;
                    params.pwm_percent = 0;
                    params.duration_ms = 0;
                    xQueueSend(action_queue_, &params, portMAX_DELAY);
                    StartActionTaskIfNeeded();
                });
        }

        void StartActionTaskIfNeeded()
        {
            if (action_task_handle_ == nullptr)
            {
                ESP_LOGI(TAG, "启动发射电机动作任务");
                xTaskCreate(ActionTask, "fire_motor_action", 1024*3, this, 4, &action_task_handle_);
            }
        }
        ~FireMotorController()
        {
            // 不要手动 vTaskDelete(action_task_handle_);
            if (action_queue_ != nullptr)
            {
                vQueueDelete(action_queue_);
            }
        }

    };

} // namespace iot

// 注册为IoT设备
DECLARE_THING(FireMotorController);