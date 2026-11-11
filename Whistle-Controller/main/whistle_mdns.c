#include "whistle_mdns.h"

#include "esp_log.h"
#include "mdns.h"
#include "wifi.h"

static const char *TAG = "whistle_mdns";

static bool s_mdns_ready;
static bool s_horn_advertised;

void whistle_mdns_start(void)
{
    esp_err_t err;

    if (s_mdns_ready) {
        return;
    }
    err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    err = mdns_hostname_set(WIFI_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname was not set: %s", esp_err_to_name(err));
    }
    err = mdns_instance_name_set("Whistleblower");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance name was not set: %s", esp_err_to_name(err));
    }
    s_mdns_ready = true;
    ESP_LOGI(TAG, "mDNS is ready. Horn service stays hidden until the DFPlayer is ready.");
}

void whistle_mdns_set_horn_ready(bool ready)
{
    mdns_txt_item_t txt[] = {
        {"service", "whistleblower"},
    };
    esp_err_t err;

    if (ready && !s_mdns_ready) {
        whistle_mdns_start();
    }
    if (!s_mdns_ready) {
        return;
    }
    if (ready && !s_horn_advertised) {
        err = mdns_service_add("Whistleblower", "_whistleblower", "_tcp", 80, txt, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Horn service was not advertised: %s", esp_err_to_name(err));
            return;
        }
        s_horn_advertised = true;
        ESP_LOGI(TAG, "Advertising _whistleblower._tcp on port 80 as %s.local", WIFI_HOSTNAME);
        return;
    }
    if (!ready && s_horn_advertised) {
        err = mdns_service_remove("_whistleblower", "_tcp");
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Horn service was not withdrawn: %s", esp_err_to_name(err));
            return;
        }
        s_horn_advertised = false;
        ESP_LOGW(TAG, "Horn service withdrawn; DFPlayer is not ready");
    }
}
