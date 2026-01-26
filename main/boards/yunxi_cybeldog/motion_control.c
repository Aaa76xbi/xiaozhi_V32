#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "motion_control.h"
#include <esp_log.h>

#define LEDC_TIMER                  LEDC_TIMER_2
#define LEDC_MODE                   LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES               LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY              (4000)

#define LEDC_M1_CHANNEL_A           LEDC_CHANNEL_5
#define LEDC_M1_CHANNEL_B           LEDC_CHANNEL_6
#define LEDC_M1_CHANNEL_A_IO        (12)
#define LEDC_M1_CHANNEL_B_IO        (13)

static float m1_coefficient = 1.0;

static const char *TAG = "MotionControl";
void motor_ledc_init(void)
{
  ESP_LOGI(TAG, "Initializing motor LEDC...");
  ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_MODE,
      .timer_num = LEDC_TIMER,
      .duty_resolution = LEDC_DUTY_RES,
      .freq_hz = LEDC_FREQUENCY,
      .clk_cfg = LEDC_AUTO_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

  ledc_channel_config_t ledc_channel_a = {
      .speed_mode = LEDC_MODE,
      .channel = LEDC_M1_CHANNEL_A,
      .timer_sel = LEDC_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = LEDC_M1_CHANNEL_A_IO,
      .duty = 0,
      .hpoint = 0};
  ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_a));

  ledc_channel_config_t ledc_channel_b = {
      .speed_mode = LEDC_MODE,
      .channel = LEDC_M1_CHANNEL_B,
      .timer_sel = LEDC_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = LEDC_M1_CHANNEL_B_IO,
      .duty = 0,
      .hpoint = 0};
  ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_b));
}

// 正转（发射）
void fire_motor_start(int pwm_percent)
{
    if (pwm_percent < 0) pwm_percent = 0;
    if (pwm_percent > 100) pwm_percent = 100;
    uint32_t duty = (uint32_t)((pwm_percent * m1_coefficient * 8192) / 100);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_A, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_A));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_B, 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_B));
}

// 停止
void fire_motor_stop(void)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_A, 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_A));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_B, 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_B));
}