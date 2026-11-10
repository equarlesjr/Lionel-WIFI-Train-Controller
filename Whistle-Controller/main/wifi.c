#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "wifi";

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_KEY_SSID  "ssid"
#define WIFI_NVS_KEY_PASS  "pass"
#define WIFI_AP_CHANNEL    1
#define WIFI_AP_MAX_CONN   4
#define WIFI_STA_RETRIES   8
#define WIFI_AP_HOLD_MS    20000
#define WIFI_STA_TIMEOUT_US (30LL * 1000000LL)

#define WIFI_CMD_FORGET     (1u << 0)
#define WIFI_CMD_CONNECT    (1u << 1)
#define WIFI_CMD_STA_FAIL   (1u << 2)
#define WIFI_CMD_GOT_IP     (1u << 3)
#define WIFI_CMD_PROVISION  (1u << 4)
#define WIFI_CMD_RETRY      (1u << 5)

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_wifi_task;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_sta_deadline;
static int s_sta_retries;
static bool s_want_sta;
static bool s_wifi_started;
static wifi_status_t s_status;

static void arm_sta_deadline(void)
{
    if (s_sta_deadline == NULL) {
        return;
    }
    esp_timer_stop(s_sta_deadline);
    esp_timer_start_once(s_sta_deadline, WIFI_STA_TIMEOUT_US);
}

static void disarm_sta_deadline(void)
{
    if (s_sta_deadline != NULL) {
        esp_timer_stop(s_sta_deadline);
    }
}

static void build_ap_ssid(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_len, "Whistleblower-%02X%02X", mac[4], mac[5]);
}

static bool nvs_load(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t handle;
    size_t required = ssid_len;

    ssid[0] = '\0';
    password[0] = '\0';

    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &required);
    if (err != ESP_OK || ssid[0] == '\0') {
        nvs_close(handle);
        return false;
    }

    required = password_len;
    err = nvs_get_str(handle, WIFI_NVS_KEY_PASS, password, &required);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        password[0] = '\0';
        return true;
    }
    return err == ESP_OK;
}

static esp_err_t nvs_store(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PASS, password != NULL ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t ensure_started(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        s_wifi_started = true;
    } else {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void start_softap(void)
{
    char ap_ssid[WIFI_SSID_MAX + 1];
    wifi_config_t ap = {0};

    disarm_sta_deadline();
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_want_sta = false;
    s_sta_retries = 0;
    s_status.state = WIFI_STATE_PROVISIONING;
    s_status.provisioning = true;
    s_status.sta_connected = false;
    s_status.ip[0] = '\0';
    strlcpy(s_status.ap_ssid, ap_ssid, sizeof(s_status.ap_ssid));
    xSemaphoreGive(s_lock);

    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)ap.ap.password, WIFI_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.channel = WIFI_AP_CHANNEL;
    ap.ap.max_connection = WIFI_AP_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        ESP_LOGE(TAG, "setup AP configuration failed");
        return;
    }
    (void)esp_wifi_disconnect();
    if (ensure_started() != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG, "setup AP \"%s\" password \"%s\" open http://192.168.4.1", ap_ssid, WIFI_AP_PASSWORD);
}

static void start_sta(const char *ssid, const char *password)
{
    wifi_config_t sta = {0};
    wifi_mode_t mode = WIFI_MODE_NULL;
    bool keep_ap = false;

    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, password != NULL ? password : "", sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_want_sta = true;
    s_sta_retries = 0;
    s_status.state = WIFI_STATE_CONNECTING;
    s_status.provisioning = true;
    s_status.sta_connected = false;
    s_status.ip[0] = '\0';
    strlcpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid));
    xSemaphoreGive(s_lock);

    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        keep_ap = true;
    }
    if (esp_wifi_set_mode(keep_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) {
        ESP_LOGE(TAG, "station configuration failed");
        start_softap();
        return;
    }

    if (!s_wifi_started) {
        if (ensure_started() != ESP_OK) {
            return;
        }
    } else if (esp_wifi_connect() != ESP_OK) {
        ESP_LOGW(TAG, "connect start failed");
    }

    arm_sta_deadline();
    ESP_LOGI(TAG, "joining \"%s\"", ssid);
}

static void drop_to_sta_only(void)
{
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        ESP_LOGW(TAG, "could not close setup AP");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.sta_connected) {
        s_status.state = WIFI_STATE_CONNECTED;
        s_status.provisioning = false;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "setup AP closed");
}

static void sta_deadline_cb(void *arg)
{
    bool joining = false;

    (void)arg;
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    joining = s_want_sta && !s_status.sta_connected;
    xSemaphoreGive(s_lock);
    if (joining && s_wifi_task != NULL) {
        ESP_LOGW(TAG, "station join exceeded 30 s; opening setup AP");
        xTaskNotify(s_wifi_task, WIFI_CMD_STA_FAIL, eSetBits);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        bool want_sta = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        want_sta = s_want_sta;
        xSemaphoreGive(s_lock);
        if (want_sta) {
            esp_wifi_connect();
        }
        return;
    }

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "phone joined setup AP");
        return;
    }

    if (id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }

    wifi_event_sta_disconnected_t *event = data;
    bool want_sta = false;
    bool retry = false;
    bool give_up = false;
    bool was_connected = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    want_sta = s_want_sta;
    was_connected = s_status.sta_connected || s_status.state == WIFI_STATE_CONNECTED;
    s_status.sta_connected = false;
    s_status.ip[0] = '\0';
    if (want_sta) {
        s_status.state = WIFI_STATE_CONNECTING;
        s_status.provisioning = true;
        s_sta_retries++;
        give_up = s_sta_retries > WIFI_STA_RETRIES;
        retry = !give_up;
    }
    xSemaphoreGive(s_lock);

    if (event != NULL) {
        ESP_LOGW(TAG, "disconnected reason=%d", event->reason);
    }
    if (!want_sta || s_wifi_task == NULL) {
        return;
    }
    if (was_connected) {
        arm_sta_deadline();
    }
    if (give_up) {
        ESP_LOGW(TAG, "giving up after %d retries; saved network kept", WIFI_STA_RETRIES);
        xTaskNotify(s_wifi_task, WIFI_CMD_STA_FAIL, eSetBits);
        return;
    }
    if (retry) {
        xTaskNotify(s_wifi_task, WIFI_CMD_RETRY, eSetBits);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    ip_event_got_ip_t *event = data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));

    disarm_sta_deadline();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sta_retries = 0;
    s_status.sta_connected = true;
    s_status.state = WIFI_STATE_CONNECTED;
    strlcpy(s_status.ip, ip, sizeof(s_status.ip));
    ESP_LOGI(TAG, "STA connected ssid=\"%s\" ip=%s open http://%s/ hostname=%s.local",
             s_status.sta_ssid, ip, ip, WIFI_HOSTNAME);
    xSemaphoreGive(s_lock);

    if (s_wifi_task != NULL) {
        xTaskNotify(s_wifi_task, WIFI_CMD_GOT_IP, eSetBits);
    }
}

static void handle_command(uint32_t bits, char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    if (bits & WIFI_CMD_FORGET) {
        nvs_clear();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.sta_ssid[0] = '\0';
        xSemaphoreGive(s_lock);
        start_softap();
        return;
    }
    if (bits & WIFI_CMD_CONNECT) {
        if (nvs_load(ssid, ssid_len, password, password_len)) {
            start_sta(ssid, password);
        } else {
            start_softap();
        }
        memset(password, 0, password_len);
        return;
    }
    if (bits & WIFI_CMD_PROVISION) {
        start_softap();
        return;
    }
    if (bits & WIFI_CMD_STA_FAIL) {
        bool connected = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        connected = s_status.sta_connected;
        xSemaphoreGive(s_lock);
        if (!connected) {
            start_softap();
        }
        return;
    }
    if (bits & WIFI_CMD_GOT_IP) {
        uint32_t extra = 0;
        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
            if (xTaskNotifyWait(0, UINT32_MAX, &extra, pdMS_TO_TICKS(WIFI_AP_HOLD_MS)) == pdTRUE) {
                handle_command(extra, ssid, ssid_len, password, password_len);
                return;
            }
        }
        bool connected = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        connected = s_status.sta_connected;
        xSemaphoreGive(s_lock);
        if (connected) {
            drop_to_sta_only();
        }
        return;
    }
    if (bits & WIFI_CMD_RETRY) {
        int attempt = 1;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        attempt = s_sta_retries > 0 ? s_sta_retries : 1;
        xSemaphoreGive(s_lock);
        int shift = attempt - 1;
        if (shift > 3) {
            shift = 3;
        }
        uint32_t extra = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &extra, pdMS_TO_TICKS(1000u << shift)) == pdTRUE) {
            handle_command(extra, ssid, ssid_len, password, password_len);
            return;
        }
        bool still_joining = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        still_joining = s_want_sta && !s_status.sta_connected;
        xSemaphoreGive(s_lock);
        if (still_joining) {
            ESP_LOGI(TAG, "reconnecting attempt %d", attempt);
            esp_wifi_connect();
        }
    }
}

static void wifi_ctrl_task(void *arg)
{
    char ssid[WIFI_SSID_MAX + 1];
    char password[WIFI_PASS_MAX + 1];

    (void)arg;

    if (nvs_load(ssid, sizeof(ssid), password, sizeof(password))) {
        start_sta(ssid, password);
    } else {
        ESP_LOGI(TAG, "no saved network");
        start_softap();
    }
    memset(password, 0, sizeof(password));

    while (true) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        handle_command(bits, ssid, sizeof(ssid), password, sizeof(password));
        memset(password, 0, sizeof(password));
    }
}

esp_err_t wifi_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        return ESP_FAIL;
    }
    esp_netif_set_hostname(s_sta_netif, WIFI_HOSTNAME);
    ESP_LOGI(TAG, "DHCP hostname %s", WIFI_HOSTNAME);
    ESP_LOGI(TAG, "mDNS is not in this ESP-IDF tree and the train controller does not advertise it");

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = sta_deadline_cb,
        .name = "sta_deadline",
    };
    err = esp_timer_create(&timer_args, &s_sta_deadline);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "station deadline timer unavailable: %s", esp_err_to_name(err));
    }

    if (xTaskCreate(wifi_ctrl_task, "wifi_ctrl", 6144, NULL, 5, &s_wifi_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool wifi_is_provisioning(void)
{
    bool provisioning = true;
    if (s_lock == NULL) {
        return true;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    provisioning = s_status.provisioning;
    xSemaphoreGive(s_lock);
    return provisioning;
}

esp_err_t wifi_get_status(wifi_status_t *status)
{
    if (status == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t wifi_save_and_connect(const char *ssid, const char *password)
{
    esp_err_t err;

    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > WIFI_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > WIFI_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_store(ssid, password != NULL ? password : "");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save network: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "saved network \"%s\"", ssid);

    if (s_wifi_task != NULL) {
        xTaskNotify(s_wifi_task, WIFI_CMD_CONNECT, eSetBits);
    }
    return ESP_OK;
}

esp_err_t wifi_connect_saved(void)
{
    if (s_wifi_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_wifi_task, WIFI_CMD_CONNECT, eSetBits);
    return ESP_OK;
}

esp_err_t wifi_forget(void)
{
    if (s_wifi_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_wifi_task, WIFI_CMD_FORGET, eSetBits);
    return ESP_OK;
}

esp_err_t wifi_start_provisioning(void)
{
    if (s_wifi_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_wifi_task, WIFI_CMD_PROVISION, eSetBits);
    return ESP_OK;
}

esp_err_t wifi_scan(wifi_scan_ap_t *aps, size_t max_aps, size_t *count)
{
    wifi_scan_config_t scan = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_ap_record_t *records = NULL;
    uint16_t found = 0;
    size_t out = 0;
    esp_err_t err;

    if (aps == NULL || count == NULL || max_aps == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (!s_wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scan mode change failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK) {
        return err;
    }
    if (found == 0) {
        return ESP_OK;
    }

    records = calloc(found, sizeof(*records));
    if (records == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&found, records);
    if (err != ESP_OK) {
        free(records);
        return err;
    }

    for (uint16_t i = 0; i < found && out < max_aps; i++) {
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
    ESP_LOGI(TAG, "scan found %u networks", (unsigned)out);
    return ESP_OK;
}
