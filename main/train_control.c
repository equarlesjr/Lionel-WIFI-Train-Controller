#include "train_control.h"

#include <strings.h>

#include "esp_log.h"
#include "train_pwm.h"

static const char *TAG = "train_control";

/* Provisional speed levels — tune percentages here after bench testing. */
static const train_speed_level_t TRAIN_SPEED_LEVELS[] = {
    {"off", TRAIN_SPEED_OFF_PERCENT},
    {"1",   TRAIN_SPEED_1_PERCENT},
    {"2",   TRAIN_SPEED_2_PERCENT},
    {"3",   TRAIN_SPEED_3_PERCENT},
    {"4",   TRAIN_SPEED_4_PERCENT},
    {"5",   TRAIN_SPEED_5_PERCENT},
    {"6",   TRAIN_SPEED_6_PERCENT},
};

static const char *s_current_mode = "off";

void train_control_init(void)
{
    s_current_mode = "off";
    train_pwm_set_percent(TRAIN_SPEED_OFF_PERCENT);
    ESP_LOGI(TAG, "Train control initialized: mode=off, percent=0");
}

esp_err_t train_control_set_mode(const char *mode)
{
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(TRAIN_SPEED_LEVELS) / sizeof(TRAIN_SPEED_LEVELS[0]); i++) {
        if (strcasecmp(mode, TRAIN_SPEED_LEVELS[i].mode) == 0) {
            s_current_mode = TRAIN_SPEED_LEVELS[i].mode;
            train_pwm_set_percent(TRAIN_SPEED_LEVELS[i].percent);
            ESP_LOGI(TAG, "Mode set to %s (%u%%)", s_current_mode,
                     (unsigned)TRAIN_SPEED_LEVELS[i].percent);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Invalid mode requested: %s", mode);
    return ESP_ERR_INVALID_ARG;
}

const char *train_control_get_mode(void)
{
    return s_current_mode;
}

uint8_t train_control_get_percent(void)
{
    for (size_t i = 0; i < sizeof(TRAIN_SPEED_LEVELS) / sizeof(TRAIN_SPEED_LEVELS[0]); i++) {
        if (s_current_mode == TRAIN_SPEED_LEVELS[i].mode) {
            return TRAIN_SPEED_LEVELS[i].percent;
        }
    }
    return TRAIN_SPEED_OFF_PERCENT;
}
