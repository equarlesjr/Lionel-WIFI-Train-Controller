#include "train_control.h"

#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "train_pwm.h"

static const char *TAG = "train_control";

/* Calibrated speed levels — do not replace these values for manual mode. */
static const train_speed_level_t TRAIN_SPEED_LEVELS[] = {
    {"off", TRAIN_SPEED_OFF_PERCENT},
    {"1",   TRAIN_SPEED_1_PERCENT},
    {"2",   TRAIN_SPEED_2_PERCENT},
    {"3",   TRAIN_SPEED_3_PERCENT},
    {"4",   TRAIN_SPEED_4_PERCENT},
    {"5",   TRAIN_SPEED_5_PERCENT},
    {"6",   TRAIN_SPEED_6_PERCENT},
};

static SemaphoreHandle_t s_lock;
static train_control_source_t s_source = TRAIN_SOURCE_OFF;
static const char *s_current_mode = "off";

static const char *source_name(train_control_source_t source)
{
    switch (source) {
    case TRAIN_SOURCE_WEB:
        return "web";
    case TRAIN_SOURCE_KNOB:
        return "knob";
    case TRAIN_SOURCE_OFF:
    default:
        return "off";
    }
}

static const train_speed_level_t *find_level(const char *mode)
{
    if (mode == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(TRAIN_SPEED_LEVELS) / sizeof(TRAIN_SPEED_LEVELS[0]); i++) {
        if (strcasecmp(mode, TRAIN_SPEED_LEVELS[i].mode) == 0) {
            return &TRAIN_SPEED_LEVELS[i];
        }
    }
    return NULL;
}

static uint8_t percent_for_mode(const char *mode)
{
    for (size_t i = 0; i < sizeof(TRAIN_SPEED_LEVELS) / sizeof(TRAIN_SPEED_LEVELS[0]); i++) {
        if (mode == TRAIN_SPEED_LEVELS[i].mode) {
            return TRAIN_SPEED_LEVELS[i].percent;
        }
    }
    return TRAIN_SPEED_OFF_PERCENT;
}

static void apply_level_locked(const train_speed_level_t *level)
{
    if (s_current_mode == level->mode) {
        return;
    }

    s_current_mode = level->mode;
    train_pwm_set_percent(level->percent);
    ESP_LOGI(TAG, "Speed %s (%u%%) source=%s", s_current_mode,
             (unsigned)level->percent, source_name(s_source));
}

static void command_off_locked(void)
{
    apply_level_locked(&TRAIN_SPEED_LEVELS[0]);
}

static bool take_lock(void)
{
    if (s_lock == NULL) {
        return false;
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void give_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

void train_control_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    s_source = TRAIN_SOURCE_OFF;
    s_current_mode = TRAIN_SPEED_LEVELS[0].mode;
    train_pwm_set_percent(TRAIN_SPEED_OFF_PERCENT);
    ESP_LOGI(TAG, "Train control initialized: source=off, mode=off, percent=0");
}

void train_control_set_source(train_control_source_t source)
{
    if (source != TRAIN_SOURCE_WEB && source != TRAIN_SOURCE_KNOB) {
        source = TRAIN_SOURCE_OFF;
    }

    if (!take_lock()) {
        train_pwm_set_percent(TRAIN_SPEED_OFF_PERCENT);
        return;
    }

    if (s_source != source) {
        ESP_LOGI(TAG, "Control source %s -> %s; commanding OFF",
                 source_name(s_source), source_name(source));
        s_source = source;
        command_off_locked();
    } else if (source == TRAIN_SOURCE_OFF) {
        command_off_locked();
    }

    give_lock();
}

train_control_source_t train_control_get_source(void)
{
    train_control_source_t source = TRAIN_SOURCE_OFF;

    if (take_lock()) {
        source = s_source;
        give_lock();
    }
    return source;
}

const char *train_control_get_source_name(void)
{
    return source_name(train_control_get_source());
}

void train_control_get_status(train_control_status_t *status)
{
    if (status == NULL) {
        return;
    }

    if (!take_lock()) {
        status->source = TRAIN_SOURCE_OFF;
        status->source_name = "off";
        status->mode = "off";
        status->percent = TRAIN_SPEED_OFF_PERCENT;
        return;
    }

    status->source = s_source;
    status->source_name = source_name(s_source);
    status->mode = s_current_mode;
    status->percent = percent_for_mode(s_current_mode);
    give_lock();
}

esp_err_t train_control_set_mode(const char *mode)
{
    const train_speed_level_t *level = find_level(mode);
    if (level == NULL) {
        ESP_LOGW(TAG, "Invalid mode requested: %s", mode ? mode : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    if (!take_lock()) {
        train_pwm_set_percent(TRAIN_SPEED_OFF_PERCENT);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_source != TRAIN_SOURCE_WEB) {
        ESP_LOGW(TAG, "Rejected browser command '%s'; source=%s",
                 level->mode, source_name(s_source));
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    apply_level_locked(level);
    give_lock();
    return ESP_OK;
}

esp_err_t train_control_apply_knob_mode(const char *mode)
{
    const train_speed_level_t *level = find_level(mode);
    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!take_lock()) {
        train_pwm_set_percent(TRAIN_SPEED_OFF_PERCENT);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_source != TRAIN_SOURCE_KNOB) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    apply_level_locked(level);
    give_lock();
    return ESP_OK;
}

const char *train_control_get_mode(void)
{
    const char *mode = "off";

    if (take_lock()) {
        mode = s_current_mode;
        give_lock();
    }
    return mode;
}

uint8_t train_control_get_percent(void)
{
    uint8_t percent = TRAIN_SPEED_OFF_PERCENT;

    if (take_lock()) {
        percent = percent_for_mode(s_current_mode);
        give_lock();
    }
    return percent;
}
