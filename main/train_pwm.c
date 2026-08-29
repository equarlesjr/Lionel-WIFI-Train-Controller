#include "train_pwm.h"

#include "speed_led.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "train_pwm";

#define TRAIN_PWM_TIMER LEDC_TIMER_0
#define TRAIN_PWM_MODE LEDC_LOW_SPEED_MODE
#define TRAIN_PWM_CHANNEL LEDC_CHANNEL_0
#define TRAIN_PWM_OUTPUT_IO (2) /* XIAO ESP32-S3 D1 */
#define TRAIN_PWM_DUTY_RES LEDC_TIMER_10_BIT
#define TRAIN_PWM_FREQUENCY_HZ (10000)
#define TRAIN_PWM_DUTY_MAX ((1 << 10) - 1)

void train_pwm_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = TRAIN_PWM_MODE,
        .duty_resolution = TRAIN_PWM_DUTY_RES,
        .timer_num = TRAIN_PWM_TIMER,
        .freq_hz = TRAIN_PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = TRAIN_PWM_MODE,
        .channel = TRAIN_PWM_CHANNEL,
        .timer_sel = TRAIN_PWM_TIMER,
        .gpio_num = TRAIN_PWM_OUTPUT_IO,
        .duty = 0, /* start at 0% so the train does not move on boot */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ESP_LOGI(TAG, "PWM init on GPIO%d, %d Hz, 10-bit, duty 0%%",
             TRAIN_PWM_OUTPUT_IO, TRAIN_PWM_FREQUENCY_HZ);
}

void train_pwm_set_percent(uint8_t percent)
{
    if (percent > TRAIN_PWM_MAX_PERCENT) {
        percent = TRAIN_PWM_MAX_PERCENT;
    }

    uint32_t duty = (TRAIN_PWM_DUTY_MAX * percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(TRAIN_PWM_MODE, TRAIN_PWM_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(TRAIN_PWM_MODE, TRAIN_PWM_CHANNEL));

    ESP_LOGI(TAG, "PWM duty set to %u%% (raw=%lu)", (unsigned)percent, (unsigned long)duty);
    speed_led_set_percent(percent);
}
