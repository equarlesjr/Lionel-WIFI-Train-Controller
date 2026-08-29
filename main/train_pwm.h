#pragma once

#include <stdint.h>

#define TRAIN_PWM_MAX_PERCENT 90

void train_pwm_init(void);
void train_pwm_set_percent(uint8_t percent);
