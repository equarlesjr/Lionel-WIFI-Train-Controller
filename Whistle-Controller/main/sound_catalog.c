#include "sound_catalog.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "catalog";

#define SOUND_CATALOG_MAX 16

extern const char sounds_json_start[] asm("_binary_sounds_json_start");
extern const char sounds_json_end[] asm("_binary_sounds_json_end");

static sound_catalog_entry_t s_sounds[SOUND_CATALOG_MAX];
static size_t s_count;
static bool s_ready;
static char s_selected[SOUND_NAME_MAX + 1];
static SemaphoreHandle_t s_select_lock;

static void store_selection(const char *name)
{
    if (s_select_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_select_lock, portMAX_DELAY);
    if (name == NULL || name[0] == '\0') {
        s_selected[0] = '\0';
    } else {
        snprintf(s_selected, sizeof(s_selected), "%s", name);
    }
    xSemaphoreGive(s_select_lock);
}

static bool has_control_char(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return true;
        }
    }
    return false;
}

static bool path_ok(const char *path)
{
    size_t len = strlen(path);

    if (len < 2 || len > SOUND_PATH_MAX || path[0] != '/') {
        return false;
    }
    return !has_control_char(path);
}

static bool name_ok(const char *name)
{
    size_t len = strlen(name);

    if (len == 0 || len > SOUND_NAME_MAX) {
        return false;
    }
    return !has_control_char(name);
}

static bool duplicate_name(const char *name, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(s_sounds[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

void sound_catalog_load(void)
{
    const char *reason = "sounds.json is not valid JSON";
    cJSON *root = NULL;
    cJSON *sounds = NULL;
    int count;
    bool ok = false;

    const char *json = &sounds_json_start[0];
    const char *json_end = &sounds_json_end[0];

    if (s_select_lock == NULL) {
        s_select_lock = xSemaphoreCreateMutex();
        if (s_select_lock == NULL) {
            ESP_LOGE(TAG, "selection lock was not created");
        }
    }
    s_count = 0;
    s_ready = false;

    if (json_end <= json || json[0] == '\0') {
        reason = "sounds.json is empty";
        goto done;
    }
    root = cJSON_Parse(json);
    if (root == NULL) {
        goto done;
    }
    sounds = cJSON_GetObjectItemCaseSensitive(root, "sounds");
    if (!cJSON_IsArray(sounds)) {
        reason = "sounds.json has no sounds array";
        goto done;
    }
    count = cJSON_GetArraySize(sounds);
    if (count < 1) {
        reason = "sounds.json has no sounds";
        goto done;
    }
    if (count > SOUND_CATALOG_MAX) {
        reason = "sounds.json has too many sounds";
        goto done;
    }

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(sounds, i);
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "path");
        const char *name_text;
        const char *path_text;

        if (!cJSON_IsString(name) || !cJSON_IsString(path) ||
            name->valuestring == NULL || path->valuestring == NULL) {
            reason = "a sound is missing name or path";
            goto done;
        }
        name_text = name->valuestring;
        path_text = path->valuestring;
        if (!name_ok(name_text)) {
            reason = "a sound name is empty, too long, or contains a control character";
            goto done;
        }
        if (!path_ok(path_text)) {
            reason = "a sound path must start with / and fit the DFPlayer command";
            goto done;
        }
        if (duplicate_name(name_text, s_count)) {
            reason = "sounds.json contains a duplicate name";
            goto done;
        }
        snprintf(s_sounds[s_count].name, sizeof(s_sounds[s_count].name), "%s", name_text);
        snprintf(s_sounds[s_count].path, sizeof(s_sounds[s_count].path), "%s", path_text);
        s_count++;
    }
    ok = s_count > 0;

done:
    cJSON_Delete(root);
    if (!ok) {
        s_count = 0;
        s_ready = false;
        store_selection(NULL);
        ESP_LOGE(TAG, "%s. Playback stays unavailable.", reason);
        return;
    }
    s_ready = true;
    store_selection(s_sounds[0].name);
    ESP_LOGI(TAG, "Loaded %u sounds. Default horn is %s", (unsigned)s_count, s_sounds[0].name);
    for (size_t i = 0; i < s_count; i++) {
        ESP_LOGI(TAG, "  [%u] %s -> %s", (unsigned)i, s_sounds[i].name, s_sounds[i].path);
    }
}

bool sound_catalog_ready(void)
{
    return s_ready;
}

const sound_catalog_entry_t *sound_catalog_entries(size_t *count)
{
    if (count != NULL) {
        *count = s_ready ? s_count : 0;
    }
    return s_sounds;
}

const sound_catalog_entry_t *sound_catalog_find(const char *name)
{
    if (!s_ready || name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_sounds[i].name, name) == 0) {
            return &s_sounds[i];
        }
    }
    return NULL;
}

const sound_catalog_entry_t *sound_catalog_default(void)
{
    if (!s_ready || s_count == 0) {
        return NULL;
    }
    return &s_sounds[0];
}

esp_err_t sound_selection_get(char *name, size_t name_len)
{
    if (name == NULL || name_len == 0 || s_select_lock == NULL || !s_ready) {
        return ESP_FAIL;
    }
    xSemaphoreTake(s_select_lock, portMAX_DELAY);
    if (s_selected[0] == '\0') {
        xSemaphoreGive(s_select_lock);
        return ESP_FAIL;
    }
    snprintf(name, name_len, "%s", s_selected);
    xSemaphoreGive(s_select_lock);
    return ESP_OK;
}

esp_err_t sound_selection_set(const char *name)
{
    const sound_catalog_entry_t *sound;

    if (!s_ready || s_select_lock == NULL) {
        return ESP_FAIL;
    }
    sound = sound_catalog_find(name);
    if (sound == NULL) {
        return (name == NULL || name[0] == '\0') ? ESP_ERR_INVALID_ARG : ESP_ERR_NOT_FOUND;
    }
    store_selection(sound->name);
    ESP_LOGI(TAG, "Selected horn is %s", sound->name);
    return ESP_OK;
}
