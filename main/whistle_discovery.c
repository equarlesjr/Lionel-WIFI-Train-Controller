#include "whistle_discovery.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "wifi.h"

static const char *TAG = "whistle_find";

static SemaphoreHandle_t s_lock;
static bool s_available;
static char s_ip[16];
static char s_base_url[48];
static char s_body[320];
static int s_body_len;
static bool s_mdns_ready;

static void publish(bool available, const char *ip)
{
    bool changed;

    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    changed = s_available != available || (available && strcmp(s_ip, ip != NULL ? ip : "") != 0);
    s_available = available;
    if (!available || ip == NULL) {
        s_ip[0] = '\0';
        s_base_url[0] = '\0';
    } else {
        snprintf(s_ip, sizeof(s_ip), "%s", ip);
        snprintf(s_base_url, sizeof(s_base_url), "http://%s", ip);
    }
    xSemaphoreGive(s_lock);
    if (!changed) {
        return;
    }
    if (available) {
        ESP_LOGI(TAG, "Whistleblower verified at %s", s_base_url);
    } else {
        ESP_LOGW(TAG, "Whistleblower unavailable");
    }
}

void whistle_discovery_get(whistle_discovery_info_t *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    info->available = s_available;
    if (s_available) {
        snprintf(info->base_url, sizeof(info->base_url), "%s", s_base_url);
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    int room;
    int copy;

    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    room = (int)sizeof(s_body) - 1 - s_body_len;
    copy = evt->data_len;
    if (copy > room) {
        copy = room;
    }
    if (copy > 0) {
        memcpy(s_body + s_body_len, evt->data, (size_t)copy);
        s_body_len += copy;
        s_body[s_body_len] = '\0';
    }
    return ESP_OK;
}

static bool health_ok(const char *ip)
{
    char url[64];
    esp_http_client_config_t config = {
        .timeout_ms = 2000,
        .event_handler = on_http,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    int status;
    bool ok;

    if (ip == NULL || ip[0] == '\0') {
        return false;
    }
    snprintf(url, sizeof(url), "http://%s/api/health", ip);
    config.url = url;
    s_body_len = 0;
    s_body[0] = '\0';
    client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ok = err == ESP_OK && status == 200 && strstr(s_body, "\"service\"") != NULL &&
         strstr(s_body, "whistleblower") != NULL;
    return ok;
}

static bool dotted_ipv4(const char *text)
{
    const char *cursor = text;
    int parts = 0;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    while (parts < 4) {
        int value = 0;
        int digits = 0;

        while (*cursor >= '0' && *cursor <= '9' && digits < 3) {
            value = value * 10 + (*cursor - '0');
            cursor++;
            digits++;
        }
        if (digits == 0 || value > 255) {
            return false;
        }
        parts++;
        if (parts == 4) {
            break;
        }
        if (*cursor++ != '.') {
            return false;
        }
    }
    return *cursor == '\0';
}

static bool address_from_result(const mdns_result_t *result, char *ip, size_t ip_len)
{
    const mdns_ip_addr_t *addr;
    esp_ip4_addr_t resolved;

    for (addr = result->addr; addr != NULL; addr = addr->next) {
        if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
            return esp_ip4addr_ntoa(&addr->addr.u_addr.ip4, ip, (int)ip_len) != NULL;
        }
    }
    if (result->hostname != NULL && mdns_query_a(result->hostname, 1000, &resolved) == ESP_OK) {
        return esp_ip4addr_ntoa(&resolved, ip, (int)ip_len) != NULL;
    }
    return false;
}

static bool discover_mdns(void)
{
    mdns_result_t *results = NULL;
    esp_err_t err;
    bool found = false;

    if (!s_mdns_ready) {
        return false;
    }
    err = mdns_query_ptr("_whistleblower", "_tcp", 2000, 4, &results);
    if (err == ESP_OK) {
        for (mdns_result_t *item = results; item != NULL && !found; item = item->next) {
            char ip[16];

            if (!address_from_result(item, ip, sizeof(ip))) {
                continue;
            }
            if (health_ok(ip)) {
                publish(true, ip);
                found = true;
            }
        }
    }
    mdns_query_results_free(results);
    return found;
}

static void ensure_mdns(void)
{
    esp_err_t err;

    if (s_mdns_ready) {
        return;
    }
    err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("train");
    s_mdns_ready = true;
    ESP_LOGI(TAG, "mDNS client ready");
}

static void discovery_task(void *arg)
{
    int failures = 0;

    (void)arg;
    while (true) {
        wifi_status_t wifi = {0};
        bool connected = wifi_get_status(&wifi) == ESP_OK && wifi.sta_connected && wifi.ip[0] != '\0';
        int wait_ms = 8000;
        char cached[16];
        bool have_cached = false;

        if (!connected) {
            publish(false, NULL);
            failures = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ensure_mdns();

        if (s_lock != NULL) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_ip[0] != '\0') {
                snprintf(cached, sizeof(cached), "%s", s_ip);
                have_cached = true;
            }
            xSemaphoreGive(s_lock);
        }
        if (have_cached && health_ok(cached)) {
            publish(true, cached);
            failures = 0;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }

        publish(false, NULL);
        if (discover_mdns()) {
            failures = 0;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }
        if (dotted_ipv4(TRAIN_WHISTLE_FALLBACK_IP) && health_ok(TRAIN_WHISTLE_FALLBACK_IP)) {
            ESP_LOGI(TAG, "Using configured fallback address");
            publish(true, TRAIN_WHISTLE_FALLBACK_IP);
            failures = 0;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }

        failures++;
        if (failures <= 1) {
            wait_ms = 2000;
        } else if (failures == 2) {
            wait_ms = 4000;
        } else if (failures == 3) {
            wait_ms = 8000;
        } else {
            wait_ms = 15000;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

void whistle_discovery_start(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "discovery lock was not created");
        return;
    }
    if (xTaskCreate(discovery_task, "whistle_find", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "discovery task did not start");
    }
}
