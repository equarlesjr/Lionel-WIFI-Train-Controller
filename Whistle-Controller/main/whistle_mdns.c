#include "whistle_mdns.h"

#include "esp_log.h"
#include "mdns.h"
#include "wifi.h"

static const char *TAG = "whistle_mdns";

void whistle_mdns_start(void)
{
    static bool started = false;
    mdns_txt_item_t txt[] = {
        {"service", "whistleblower"},
    };
    esp_err_t err;

    if (started) {
        return;
    }
    err = mdns_init();
    if (err != ESP_OK) {
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
    err = mdns_service_add("Whistleblower", "_whistleblower", "_tcp", 80, txt, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS service was not advertised: %s", esp_err_to_name(err));
        return;
    }
    started = true;
    ESP_LOGI(TAG, "Advertising _whistleblower._tcp on port 80 as %s.local", WIFI_HOSTNAME);
}
