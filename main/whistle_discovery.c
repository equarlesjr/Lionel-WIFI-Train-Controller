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

#define HEALTH_TIMEOUT_MS                         2000
#define DISCOVERY_BACKOFF_MAX_MS                  15000
#define HORN_TRANSPORT_FAILURES_BEFORE_REDISCOVERY 5

static SemaphoreHandle_t s_lock;
static char s_ip[16];
static uint16_t s_port;
static char s_base_url[48];
static char s_net_ssid[33];
static char s_net_ip[16];
static bool s_have_net;
static int s_transport_failures;
static bool s_rediscover;
static char s_body[320];
static int s_body_len;
static bool s_mdns_ready;

static void format_endpoint(const char *ip, uint16_t port, char *out, size_t out_len)
{
    if (port == 0 || port == 80) {
        snprintf(out, out_len, "http://%s", ip);
    } else {
        snprintf(out, out_len, "http://%s:%u", ip, (unsigned)port);
    }
}

static void copy_info_locked(whistle_discovery_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->available = s_base_url[0] != '\0';
    if (info->available) {
        snprintf(info->base_url, sizeof(info->base_url), "%s", s_base_url);
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
    copy_info_locked(info);
    xSemaphoreGive(s_lock);
}

static bool endpoint_matches_locked(const char *base_url)
{
    return base_url != NULL && s_base_url[0] != '\0' && strcmp(s_base_url, base_url) == 0;
}

void whistle_discovery_note_success(const char *base_url)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (endpoint_matches_locked(base_url)) {
        s_transport_failures = 0;
        s_rediscover = false;
    }
    xSemaphoreGive(s_lock);
}

void whistle_discovery_note_transport_failure(const char *base_url)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (endpoint_matches_locked(base_url)) {
        s_transport_failures++;
        if (s_transport_failures >= HORN_TRANSPORT_FAILURES_BEFORE_REDISCOVERY) {
            s_rediscover = true;
        }
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

static bool health_check(const char *ip, uint16_t port, char *reason, size_t reason_len)
{
    char path[80];
    char url[sizeof(path) + sizeof("/api/health") - 1];
    esp_http_client_config_t config = {
        .timeout_ms = HEALTH_TIMEOUT_MS,
        .event_handler = on_http,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    int status;

    format_endpoint(ip, port, path, sizeof(path));
    snprintf(url, sizeof(url), "%s/api/health", path);
    config.url = url;
    s_body_len = 0;
    s_body[0] = '\0';
    client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(reason, reason_len, "HTTP client was not created");
        return false;
    }
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        snprintf(reason, reason_len, "%s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        snprintf(reason, reason_len, "HTTP %d", status);
        return false;
    }
    if (strstr(s_body, "\"service\"") == NULL || strstr(s_body, "whistleblower") == NULL) {
        snprintf(reason, reason_len, "response was not whistleblower");
        return false;
    }
    return true;
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

static bool address_from_result(const mdns_result_t *result, char *ip, size_t ip_len, uint16_t *port)
{
    const mdns_ip_addr_t *addr;

    for (addr = result->addr; addr != NULL; addr = addr->next) {
        if (addr->addr.type == ESP_IPADDR_TYPE_V4 &&
            esp_ip4addr_ntoa(&addr->addr.u_addr.ip4, ip, (int)ip_len) != NULL) {
            *port = result->port == 0 ? 80 : result->port;
            return true;
        }
    }
    if (result->hostname != NULL) {
        esp_ip4_addr_t resolved;
        if (mdns_query_a(result->hostname, 1000, &resolved) == ESP_OK &&
            esp_ip4addr_ntoa(&resolved, ip, (int)ip_len) != NULL) {
            *port = result->port == 0 ? 80 : result->port;
            return true;
        }
    }
    return false;
}

static bool station_now(char *ssid, size_t ssid_len, char *ip, size_t ip_len)
{
    wifi_status_t wifi = {0};

    if (wifi_get_status(&wifi) != ESP_OK || !wifi.sta_connected || wifi.ip[0] == '\0' || wifi.sta_ssid[0] == '\0') {
        return false;
    }
    snprintf(ssid, ssid_len, "%s", wifi.sta_ssid);
    snprintf(ip, ip_len, "%s", wifi.ip);
    return true;
}

static bool same_network_locked(const char *ssid, const char *ip)
{
    return s_have_net && strcmp(s_net_ssid, ssid) == 0 && strcmp(s_net_ip, ip) == 0;
}

/* NET_DOWN keeps the cached endpoint. NET_CHANGED clears it. */
enum {
    NET_DOWN = 0,
    NET_SAME,
    NET_CHANGED,
};

static int observe_network(void)
{
    char ssid[33];
    char ip[16];
    char previous_ssid[33];
    char previous_ip[16];
    bool changed = false;

    if (!station_now(ssid, sizeof(ssid), ip, sizeof(ip))) {
        return NET_DOWN;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_have_net) {
        snprintf(s_net_ssid, sizeof(s_net_ssid), "%s", ssid);
        snprintf(s_net_ip, sizeof(s_net_ip), "%s", ip);
        s_have_net = true;
        xSemaphoreGive(s_lock);
        return NET_SAME;
    }
    if (strcmp(s_net_ssid, ssid) != 0 || strcmp(s_net_ip, ip) != 0) {
        snprintf(previous_ssid, sizeof(previous_ssid), "%s", s_net_ssid);
        snprintf(previous_ip, sizeof(previous_ip), "%s", s_net_ip);
        snprintf(s_net_ssid, sizeof(s_net_ssid), "%s", ssid);
        snprintf(s_net_ip, sizeof(s_net_ip), "%s", ip);
        s_ip[0] = '\0';
        s_port = 0;
        s_base_url[0] = '\0';
        s_transport_failures = 0;
        s_rediscover = false;
        changed = true;
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        ESP_LOGW(TAG, "Station network changed from \"%s\" %s to \"%s\" %s; cached Whistleblower endpoint cleared",
                 previous_ssid, previous_ip, ssid, ip);
        return NET_CHANGED;
    }
    return NET_SAME;
}

static bool cache_verified(const char *ip, uint16_t port, const char *net_ssid, const char *net_ip)
{
    char url[48];
    bool replaced = false;
    bool same = false;

    format_endpoint(ip, port, url, sizeof(url));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!same_network_locked(net_ssid, net_ip)) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "Discarded %s because the station network changed", url);
        return false;
    }
    if (strcmp(s_base_url, url) == 0) {
        s_transport_failures = 0;
        s_rediscover = false;
        same = true;
    } else {
        replaced = s_base_url[0] != '\0';
        snprintf(s_ip, sizeof(s_ip), "%s", ip);
        s_port = port == 0 ? 80 : port;
        snprintf(s_base_url, sizeof(s_base_url), "%s", url);
        s_transport_failures = 0;
        s_rediscover = false;
    }
    xSemaphoreGive(s_lock);
    if (same) {
        ESP_LOGI(TAG, "Rediscovery confirmed cached endpoint %s", url);
    } else if (replaced) {
        ESP_LOGI(TAG, "Cached Whistleblower endpoint updated to %s", url);
    } else {
        ESP_LOGI(TAG, "Cached Whistleblower endpoint: %s", url);
        ESP_LOGI(TAG, "Discovery complete; periodic health polling disabled");
    }
    return true;
}

static bool find_verified(const char *net_ssid, const char *net_ip)
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
            char reason[48];
            uint16_t port = 80;

            if (!address_from_result(item, ip, sizeof(ip), &port)) {
                continue;
            }
            ESP_LOGI(TAG, "mDNS discovered Whistleblower at %s", ip);
            if (!health_check(ip, port, reason, sizeof(reason))) {
                ESP_LOGW(TAG, "Whistleblower identity check failed for %s: %s", ip, reason);
                continue;
            }
            ESP_LOGI(TAG, "Whistleblower identity verified");
            {
                char now_ssid[33];
                char now_ip[16];
                if (!station_now(now_ssid, sizeof(now_ssid), now_ip, sizeof(now_ip)) ||
                    strcmp(now_ssid, net_ssid) != 0 || strcmp(now_ip, net_ip) != 0) {
                    ESP_LOGW(TAG, "Discarded %s because the station network changed", ip);
                    break;
                }
            }
            found = cache_verified(ip, port, net_ssid, net_ip);
        }
    }
    mdns_query_results_free(results);
    if (!found && dotted_ipv4(TRAIN_WHISTLE_FALLBACK_IP)) {
        char reason[48];
        if (health_check(TRAIN_WHISTLE_FALLBACK_IP, 80, reason, sizeof(reason))) {
            ESP_LOGI(TAG, "Whistleblower identity verified");
            found = cache_verified(TRAIN_WHISTLE_FALLBACK_IP, 80, net_ssid, net_ip);
        }
    }
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

static int backoff_ms(int misses)
{
    if (misses <= 1) {
        return 2000;
    }
    if (misses == 2) {
        return 4000;
    }
    if (misses == 3) {
        return 8000;
    }
    return DISCOVERY_BACKOFF_MAX_MS;
}

static void discovery_task(void *arg)
{
    int misses = 0;
    bool announced = false;

    (void)arg;
    while (true) {
        char net_ssid[33];
        char net_ip[16];
        bool have_endpoint = false;
        bool rediscover = false;
        int failures = 0;
        int net = observe_network();

        if (net == NET_CHANGED) {
            announced = false;
            misses = 0;
        }
        if (net == NET_DOWN) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ensure_mdns();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        have_endpoint = s_base_url[0] != '\0';
        rediscover = s_rediscover;
        failures = s_transport_failures;
        if (s_rediscover) {
            s_rediscover = false;
            s_transport_failures = 0;
        }
        snprintf(net_ssid, sizeof(net_ssid), "%s", s_net_ssid);
        snprintf(net_ip, sizeof(net_ip), "%s", s_net_ip);
        xSemaphoreGive(s_lock);

        if (have_endpoint && !rediscover) {
            misses = 0;
            announced = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (rediscover && have_endpoint) {
            ESP_LOGI(TAG, "Rediscovery initiated after %d consecutive Horn transport failures", failures);
        } else if (!announced) {
            ESP_LOGI(TAG, "Whistleblower discovery starting");
            announced = true;
        }

        if (find_verified(net_ssid, net_ip)) {
            misses = 0;
            announced = false;
            continue;
        }
        if (have_endpoint) {
            ESP_LOGW(TAG, "Rediscovery found no verified address; retaining cached Whistleblower endpoint");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        misses++;
        int wait_ms = backoff_ms(misses);
        ESP_LOGW(TAG, "Whistleblower was not verified; retrying discovery in %d ms", wait_ms);
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
