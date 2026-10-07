#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_AP_PASSWORD      "trainsetup"
#define WIFI_SSID_MAX         32
#define WIFI_PASS_MAX         64
#define WIFI_SCAN_MAX         16

typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_PROVISIONING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
} wifi_state_t;

typedef struct {
    wifi_state_t state;
    bool provisioning;
    bool sta_connected;
    char ap_ssid[WIFI_SSID_MAX + 1];
    char sta_ssid[WIFI_SSID_MAX + 1];
    char ip[16];
} wifi_status_t;

typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    int8_t rssi;
    bool open;
} wifi_scan_ap_t;

/**
 * @brief Create STA+AP netifs, start Wi-Fi, join NVS network or open setup AP.
 *
 * Requires nvs_flash_init(), esp_netif_init(), and the default event loop.
 * Does not block waiting for a station IP.
 */
esp_err_t wifi_init(void);

/** @brief True while the setup SoftAP is the way to configure the train. */
bool wifi_is_provisioning(void);

esp_err_t wifi_get_status(wifi_status_t *status);

/**
 * @brief Save credentials. Does not log the password. Call wifi_connect_saved() to join.
 */
esp_err_t wifi_save_and_connect(const char *ssid, const char *password);

/** @brief Start STA using credentials already in NVS. */
esp_err_t wifi_connect_saved(void);

/** @brief Erase saved network and return to setup AP. */
esp_err_t wifi_forget(void);

esp_err_t wifi_scan(wifi_scan_ap_t *aps, size_t max_aps, size_t *count);

#ifdef __cplusplus
}
#endif
