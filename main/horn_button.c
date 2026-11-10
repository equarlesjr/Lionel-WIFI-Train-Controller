#include "horn_button.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "whistle_discovery.h"

static const char *TAG = "horn_button";

#define HORN_BUTTON_GPIO     44 /* XIAO ESP32-S3 D7 */
#define HORN_POLL_MS         10
#define HORN_DEBOUNCE_MS     30
#define HORN_HTTP_TIMEOUT_MS 2000

static QueueHandle_t s_presses;

static void post_horn(void)
{
    whistle_discovery_info_t info;
    char url[64];
    esp_http_client_config_t config = {
        .method = HTTP_METHOD_POST,
        .timeout_ms = HORN_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    int status;

    whistle_discovery_get(&info);
    if (!info.available || info.base_url[0] == '\0') {
        ESP_LOGW(TAG, "Horn press ignored; Whistleblower is not available");
        return;
    }
    snprintf(url, sizeof(url), "%s/api/horn", info.base_url);
    config.url = url;
    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Horn request was not sent");
        return;
    }
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Horn request failed: %s", esp_err_to_name(err));
        return;
    }
    if (status == 202) {
        ESP_LOGI(TAG, "Horn accepted");
    } else if (status == 409) {
        ESP_LOGW(TAG, "Whistleblower is already playing");
    } else if (status == 503) {
        ESP_LOGW(TAG, "Whistleblower unavailable");
    } else {
        ESP_LOGW(TAG, "Horn request returned HTTP %d", status);
    }
}

static void horn_worker(void *arg)
{
    uint8_t token;

    (void)arg;
    while (true) {
        if (xQueueReceive(s_presses, &token, portMAX_DELAY) == pdTRUE) {
            post_horn();
        }
    }
}

static void queue_press(void)
{
    uint8_t token = 1;

    if (s_presses == NULL || xQueueSend(s_presses, &token, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Horn press ignored; a request is already queued");
        return;
    }
    ESP_LOGI(TAG, "Horn pressed");
}

static void horn_button_task(void *arg)
{
    int last = gpio_get_level(HORN_BUTTON_GPIO);
    int stable_ms = HORN_DEBOUNCE_MS;
    bool held = last == 0;

    (void)arg;
    while (true) {
        int level = gpio_get_level(HORN_BUTTON_GPIO);

        if (level != last) {
            last = level;
            stable_ms = 0;
        } else if (stable_ms < HORN_DEBOUNCE_MS) {
            stable_ms += HORN_POLL_MS;
            if (stable_ms >= HORN_DEBOUNCE_MS) {
                if (!held && level == 0) {
                    held = true;
                    queue_press();
                } else if (held && level != 0) {
                    held = false;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(HORN_POLL_MS));
    }
}

void horn_button_init(void)
{
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << HORN_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&button);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d was not configured: %s", HORN_BUTTON_GPIO, esp_err_to_name(err));
        return;
    }
    s_presses = xQueueCreate(1, sizeof(uint8_t));
    if (s_presses == NULL ||
        xTaskCreate(horn_worker, "horn_http", 8192, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(horn_button_task, "horn_btn", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Horn button task did not start");
        return;
    }
    ESP_LOGI(TAG, "Horn button ready on D7/GPIO%d", HORN_BUTTON_GPIO);
}
