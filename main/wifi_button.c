#include "wifi_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi.h"

static const char *TAG = "wifi_button";

#define WIFI_FORGET_GPIO       (7) /* XIAO ESP32-S3 D8 */
#define WIFI_FORGET_HOLD_MS    (4000)
#define WIFI_FORGET_POLL_MS    (20)

static void wifi_button_task(void *arg)
{
    (void)arg;
    bool last_pressed = false;
    bool fired = false;
    int64_t press_started_us = 0;

    while (1) {
        const int64_t now = esp_timer_get_time();
        const bool pressed = gpio_get_level(WIFI_FORGET_GPIO) == 0;

        if (pressed && !last_pressed) {
            press_started_us = now;
            fired = false;
        } else if (pressed && last_pressed && !fired) {
            if ((now - press_started_us) / 1000 >= WIFI_FORGET_HOLD_MS) {
                fired = true;
                ESP_LOGI(TAG, "GPIO%d held %d ms — forget Wi-Fi",
                         WIFI_FORGET_GPIO, WIFI_FORGET_HOLD_MS);
                (void)wifi_forget();
            }
        }

        last_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(WIFI_FORGET_POLL_MS));
    }
}

void wifi_button_init(void)
{
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << WIFI_FORGET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    const BaseType_t ok = xTaskCreate(wifi_button_task, "wifi_btn", 3072, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start forget-button task");
        return;
    }

    ESP_LOGI(TAG, "Forget Wi-Fi: hold D8/GPIO%d to GND for %d ms",
             WIFI_FORGET_GPIO, WIFI_FORGET_HOLD_MS);
}
