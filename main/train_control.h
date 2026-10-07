#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Provisional PWM percentages for each discrete speed level. */
#define TRAIN_SPEED_OFF_PERCENT 0
#define TRAIN_SPEED_1_PERCENT   65
#define TRAIN_SPEED_2_PERCENT   70
#define TRAIN_SPEED_3_PERCENT   75
#define TRAIN_SPEED_4_PERCENT   80
#define TRAIN_SPEED_5_PERCENT   85
#define TRAIN_SPEED_6_PERCENT   90

typedef struct {
    const char *mode;
    uint8_t percent;
} train_speed_level_t;

typedef enum {
    TRAIN_SOURCE_WEB = 0,
    TRAIN_SOURCE_OFF,
    TRAIN_SOURCE_KNOB,
} train_control_source_t;

typedef struct {
    train_control_source_t source;
    const char *source_name;
    const char *mode;
    uint8_t percent;
} train_control_status_t;

void train_control_init(void);
void train_control_set_source(train_control_source_t source);
train_control_source_t train_control_get_source(void);
const char *train_control_get_source_name(void);
void train_control_get_status(train_control_status_t *status);

esp_err_t train_control_set_mode(const char *mode);
esp_err_t train_control_apply_knob_mode(const char *mode);
const char *train_control_get_mode(void);
uint8_t train_control_get_percent(void);
