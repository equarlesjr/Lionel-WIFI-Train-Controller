idf_component_register(SRCS "whistle_http.c" "wifi.c" "Whistle-Controller.c" "sound_catalog.c" "whistle_mdns.c"
                       INCLUDE_DIRS "."
                       REQUIRES dfplayer_pro freertos log esp_timer esp_driver_gpio
                                nvs_flash esp_wifi esp_netif esp_event esp_http_server
                       EMBED_FILES "wifi_setup.html" "sound_console.html"
                       EMBED_TXTFILES "../sounds.json")
