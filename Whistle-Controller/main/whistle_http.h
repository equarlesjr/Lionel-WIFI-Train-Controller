#pragma once

#include "esp_err.h"

/* Setup page on the SoftAP and GET /api/health on the LAN. */
esp_err_t whistle_http_start(void);
