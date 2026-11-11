#pragma once

#include <stdbool.h>

/* Bench-only address used when mDNS finds nothing. Leave empty in normal use.
 * The page never contains this address. One health check is required before it is cached.
 */
#define TRAIN_WHISTLE_FALLBACK_IP ""

typedef struct {
    bool available;
    char base_url[48];
} whistle_discovery_info_t;

/* Background discovery. Does not block motor control or the web server. */
void whistle_discovery_start(void);

void whistle_discovery_get(whistle_discovery_info_t *info);

/* The device answered HTTP on this cached endpoint. Clears the transport-failure count. */
void whistle_discovery_note_success(const char *base_url);

/* TCP failure or timeout talking to this cached endpoint. Does not erase it. */
void whistle_discovery_note_transport_failure(const char *base_url);
