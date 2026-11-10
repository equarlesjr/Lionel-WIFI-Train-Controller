#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SOUND_NAME_MAX 47
#define SOUND_PATH_MAX 64

typedef struct {
    char name[SOUND_NAME_MAX + 1];
    char path[SOUND_PATH_MAX + 1];
} sound_catalog_entry_t;

/* Parse the embedded sounds.json. Safe to call once at startup. */
void sound_catalog_load(void);

bool sound_catalog_ready(void);

const sound_catalog_entry_t *sound_catalog_entries(size_t *count);

const sound_catalog_entry_t *sound_catalog_find(const char *name);

/* sounds[0], or NULL when the catalog did not load. */
const sound_catalog_entry_t *sound_catalog_default(void);

/*
 * Volatile session horn. Starts as sounds[0] after a successful load.
 * A reboot resets it. Wi-Fi reconnects do not. This does not write sounds.json or NVS.
 */
esp_err_t sound_selection_get(char *name, size_t name_len);
esp_err_t sound_selection_set(const char *name);
