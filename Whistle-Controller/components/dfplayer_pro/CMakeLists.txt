idf_component_register(SRCS "dfplayer_pro.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_timer log freertos)
