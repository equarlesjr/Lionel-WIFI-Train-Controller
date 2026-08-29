#include "speed_led.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "speed_led";

#define SPEED_LED_GPIO          (1) /* XIAO ESP32-S3 D0 */
#define SPEED_LED_TASK_STACK    (2048)
#define SPEED_LED_TASK_PRIORITY (5)
#define SPEED_LED_POLL_MS       (100)
#define SPEED_LED_BOOT_FLASH_COUNT (3)
#define SPEED_LED_BOOT_FLASH_ON_MS (200)
#define SPEED_LED_BOOT_FLASH_OFF_MS (200)

static volatile uint8_t s_commanded_percent = 0;
static TaskHandle_t s_speed_led_task = NULL;

static void speed_led_gpio_set(bool on)
{
    gpio_set_level(SPEED_LED_GPIO, on ? 1 : 0);
}

static void speed_led_boot_flash(void)
{
    for (int i = 0; i < SPEED_LED_BOOT_FLASH_COUNT; i++) {
        speed_led_gpio_set(true);
        vTaskDelay(pdMS_TO_TICKS(SPEED_LED_BOOT_FLASH_ON_MS));
        speed_led_gpio_set(false);
        vTaskDelay(pdMS_TO_TICKS(SPEED_LED_BOOT_FLASH_OFF_MS));
    }
}

static uint32_t blink_half_period_ms(uint8_t percent)
{
    if (percent <= 25) {
        return 500; /* 1000 ms full cycle */
    }
    if (percent <= 50) {
        return 300; /* 600 ms full cycle */
    }
    if (percent <= 75) {
        return 150; /* 300 ms full cycle */
    }
    return 75; /* 150 ms full cycle for 76-99% */
}

static void speed_led_task(void *arg)
{
    bool led_on = false;

    while (1) {
        uint8_t percent = s_commanded_percent;

        if (percent == 0) {
            speed_led_gpio_set(false);
            vTaskDelay(pdMS_TO_TICKS(SPEED_LED_POLL_MS));
            continue;
        }

        if (percent >= 100) {
            speed_led_gpio_set(true);
            vTaskDelay(pdMS_TO_TICKS(SPEED_LED_POLL_MS));
            continue;
        }

        led_on = !led_on;
        speed_led_gpio_set(led_on);
        vTaskDelay(pdMS_TO_TICKS(blink_half_period_ms(percent)));
    }
}

void speed_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SPEED_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    speed_led_gpio_set(false);

    xTaskCreate(speed_led_task, "speed_led", SPEED_LED_TASK_STACK, NULL,
                SPEED_LED_TASK_PRIORITY, &s_speed_led_task);

    ESP_LOGI(TAG, "Speed LED init on GPIO%d (XIAO D0)", SPEED_LED_GPIO);
}

void speed_led_signal_ready(void)
{
    if (s_speed_led_task != NULL) {
        vTaskSuspend(s_speed_led_task);
    }

    speed_led_boot_flash();

    if (s_speed_led_task != NULL) {
        vTaskResume(s_speed_led_task);
    }

    ESP_LOGI(TAG, "System ready (3-flash signal complete)");
}

void speed_led_set_percent(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_commanded_percent = percent;
}
