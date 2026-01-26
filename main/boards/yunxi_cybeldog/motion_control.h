
#include "driver/ledc.h"

#ifdef __cplusplus
extern "C" {
#endif

void motor_ledc_init(void);
void fire_motor_start(int pwm_percent);
void fire_motor_stop(void);

#ifdef __cplusplus
}
#endif