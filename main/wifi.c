#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "wifi";

#define NVS_NS           "wifi_cfg"
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASS     "pass"
#define WIFI_STA_RETRIES 8
#define WIFI_AP_HOLD_MS  20000

#define WIFI_CMD_CONNECT (1u << 0)
#define WIFI_CMD_FORGET  (1u << 1)
#define WIFI_CMD_GOT_IP  (1u << 2)
#define WIFI_CMD_STA_FAIL (1u << 3)

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_wifi_task;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static int s_retry;
static bool s_want_sta;
static bool s_wifi_started;
static wifi_status_t s_status;

static void apply_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid),
             "Train-%02X%02X", mac[4], mac[5]);
}

static bool load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    size_t slen = ssid_len;
    size_t plen = pass_len;
    esp_err_t err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &slen);
    if (err != ESP_OK || ssid[0] == '\0') {
        nvs_close(nvs);
        return false;
    }

    err = nvs_get_str(nvs, NVS_KEY_PASS, pass, &plen);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        pass[0] = '\0';
        return true;
    }
    return err == ESP_OK;
}

static esp_err_t store_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_SSID, ssid ? ssid : ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_PASS, pass ? pass : ""));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    return ESP_OK;
}

static void erase_creds(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASS);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void set_state_locked(wifi_state_t state, bool provisioning, bool connected)
{
    s_status.state = state;
    s_status.provisioning = provisioning;
    s_status.sta_connected = connected;
    if (!connected) {
        s_status.ip[0] = '\0';
    }
}

static void start_softap(void)
{
    apply_ap_ssid();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_want_sta = false;
    s_retry = 0;
    s_status.sta_ssid[0] = '\0';
    set_state_locked(WIFI_STATE_PROVISIONING, true, false);
    strlcpy(s_status.ip, "192.168.4.1", sizeof(s_status.ip));
    xSemaphoreGive(s_lock);

    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, s_status.ap_ssid, strlen(s_status.ap_ssid));
    ap.ap.ssid_len = strlen(s_status.ap_ssid);
    memcpy(ap.ap.password, WIFI_AP_PASSWORD, strlen(WIFI_AP_PASSWORD));
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    (void)esp_wifi_disconnect();
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }

    ESP_LOGI(TAG, "setup AP \"%s\"  password \"%s\"  open http://192.168.4.1/wifi",
             s_status.ap_ssid, WIFI_AP_PASSWORD);
}

static void start_sta(void)
{
    char ssid[WIFI_SSID_MAX + 1] = {0};
    char pass[WIFI_PASS_MAX + 1] = {0};
    if (!load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        start_softap();
        return;
    }

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_want_sta = true;
    s_retry = 0;
    strlcpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid));
    set_state_locked(WIFI_STATE_CONNECTING, false, false);
    xSemaphoreGive(s_lock);

    /* Keep the setup AP up while joining so the phone can see success + the LAN IP. */
    wifi_mode_t mode = WIFI_MODE_STA;
    bool keep_ap = (esp_wifi_get_mode(&mode) == ESP_OK &&
                    (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_mode(keep_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        esp_wifi_connect();
    }

    ESP_LOGI(TAG, "joining \"%s\"", ssid);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_START) {
        bool want_sta = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        want_sta = s_want_sta;
        xSemaphoreGive(s_lock);
        if (want_sta) {
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        bool retry = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_want_sta && s_retry < WIFI_STA_RETRIES) {
            s_retry++;
            retry = true;
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retry, WIFI_STA_RETRIES);
        }
        set_state_locked(s_want_sta ? WIFI_STATE_CONNECTING : WIFI_STATE_PROVISIONING,
                         !s_want_sta, false);
        xSemaphoreGive(s_lock);
        if (retry) {
            esp_wifi_connect();
        } else if (s_want_sta && s_wifi_task) {
            xTaskNotify(s_wifi_task, WIFI_CMD_STA_FAIL, eSetBits);
        }
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "phone joined setup AP");
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_retry = 0;
    strlcpy(s_status.ip, ip, sizeof(s_status.ip));
    set_state_locked(WIFI_STATE_CONNECTED, false, true);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "STA IP %s", ip);

    if (s_wifi_task) {
        xTaskNotify(s_wifi_task, WIFI_CMD_GOT_IP, eSetBits);
    }
}

static void wifi_ctrl_task(void *arg)
{
    (void)arg;
    uint32_t bits = 0;

    while (1) {
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (bits & WIFI_CMD_FORGET) {
            ESP_LOGW(TAG, "forgetting saved network");
            erase_creds();
            start_softap();
        }
        if (bits & WIFI_CMD_CONNECT) {
            start_sta();
        }
        if (bits & WIFI_CMD_STA_FAIL) {
            ESP_LOGW(TAG, "could not join STA — opening setup AP");
            start_softap();
        }
        if (bits & WIFI_CMD_GOT_IP) {
            wifi_mode_t mode = WIFI_MODE_NULL;
            if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
                ESP_LOGI(TAG, "joined STA — keeping setup AP %d s so the phone can show the new IP",
                         WIFI_AP_HOLD_MS / 1000);
                vTaskDelay(pdMS_TO_TICKS(WIFI_AP_HOLD_MS));
            }
            bool still_connected = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            still_connected = s_status.sta_connected;
            xSemaphoreGive(s_lock);
            if (still_connected) {
                ESP_LOGI(TAG, "closing setup AP; control page is at http://%s/", s_status.ip);
                esp_wifi_set_mode(WIFI_MODE_STA);
            }
        }
    }
}

esp_err_t wifi_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    apply_ap_ssid();
    set_state_locked(WIFI_STATE_IDLE, false, false);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    const BaseType_t ok = xTaskCreate(wifi_ctrl_task, "wifi_ctrl", 4096, NULL, 5, &s_wifi_task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    char ssid[WIFI_SSID_MAX + 1] = {0};
    char pass[WIFI_PASS_MAX + 1] = {0};
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        start_sta();
    } else {
        start_softap();
    }
    return ESP_OK;
}

bool wifi_is_provisioning(void)
{
    bool provisioning = true;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        provisioning = s_status.provisioning;
        xSemaphoreGive(s_lock);
    }
    return provisioning;
}

esp_err_t wifi_get_status(wifi_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t wifi_save_and_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > WIFI_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) > WIFI_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "saving network \"%s\"", ssid);
    ESP_ERROR_CHECK(store_creds(ssid, password ? password : ""));
    return ESP_OK;
}

esp_err_t wifi_connect_saved(void)
{
    if (s_wifi_task) {
        xTaskNotify(s_wifi_task, WIFI_CMD_CONNECT, eSetBits);
        return ESP_OK;
    }
    start_sta();
    return ESP_OK;
}

esp_err_t wifi_forget(void)
{
    if (s_wifi_task) {
        xTaskNotify(s_wifi_task, WIFI_CMD_FORGET, eSetBits);
        return ESP_OK;
    }
    erase_creds();
    start_softap();
    return ESP_OK;
}

esp_err_t wifi_scan(wifi_scan_ap_t *aps, size_t max_aps, size_t *count)
{
    if (!aps || !count || max_aps == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    wifi_scan_config_t scan = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    /* Scan needs APSTA or STA. SoftAP-only would fail. */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }

    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t n = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&n));
    if (n == 0) {
        return ESP_OK;
    }

    wifi_ap_record_t *records = calloc(n, sizeof(*records));
    if (!records) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&n, records));

    size_t out = 0;
    for (uint16_t i = 0; i < n && out < max_aps; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        strlcpy(aps[out].ssid, (const char *)records[i].ssid, sizeof(aps[out].ssid));
        aps[out].rssi = records[i].rssi;
        aps[out].open = records[i].authmode == WIFI_AUTH_OPEN;
        out++;
    }
    *count = out;
    free(records);
    return ESP_OK;
}
