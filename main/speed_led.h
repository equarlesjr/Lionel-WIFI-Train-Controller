#pragma once

#include <stdint.h>

void speed_led_init(void);
void speed_led_signal_ready(void);
void speed_led_set_percent(uint8_t percent);
