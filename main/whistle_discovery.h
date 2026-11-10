#pragma once

#include <stdbool.h>

/* Bench-only address used after mDNS finds nothing. Leave empty in normal use.
 * The page never contains this address. A health check is still required.
 */
#define TRAIN_WHISTLE_FALLBACK_IP ""

typedef struct {
    bool available;
    char base_url[48];
} whistle_discovery_info_t;

/* Background discovery. Does not block motor control or the web server. */
void whistle_discovery_start(void);

void whistle_discovery_get(whistle_discovery_info_t *info);
