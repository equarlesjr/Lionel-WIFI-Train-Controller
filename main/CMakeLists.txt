set(requires esp-tls nvs_flash esp_netif esp_http_server)
idf_build_get_property(target IDF_TARGET)

set(srcs "main.c")

if(${target} STREQUAL "linux")
    list(APPEND requires esp_stubs protocol_examples_common)
else()
    list(APPEND requires esp_wifi esp_eth esp_driver_ledc esp_driver_gpio)
    list(APPEND srcs "train_pwm.c" "speed_led.c" "train_control.c" "train_http.c")
    set(embed_files "${CMAKE_CURRENT_SOURCE_DIR}/../images/lionel-trains.jpg")
endif()

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES ${requires}
                    EMBED_FILES ${embed_files})
