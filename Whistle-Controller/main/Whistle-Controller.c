#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "dfplayer_pro.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sound_catalog.h"
#include "whistle.h"
#include "whistle_http.h"
#include "whistle_mdns.h"
#include "wifi.h"

static const char *TAG = "whistle";

static char s_response[DFPLAYER_PRO_RESPONSE_MAX];
static SemaphoreHandle_t s_state_lock;
static QueueHandle_t s_play_queue;
static bool s_playing;
static bool s_dfplayer_ready;
static int s_module_file_count = -1;
static char s_current_sound[sizeof(((whistle_status_t *)0)->current_sound)];
static char s_last_error[sizeof(((whistle_status_t *)0)->last_error)];

static void note_copy(char *dest, size_t dest_len, const char *text)
{
    snprintf(dest, dest_len, "%s", text);
}

static dfplayer_err_t setup_link(void)
{
    dfplayer_pro_config_t config = {
        .uart_port = BOARD_DFPLAYER_UART_PORT,
        .tx_gpio = BOARD_DFPLAYER_TX_GPIO,
        .rx_gpio = BOARD_DFPLAYER_RX_GPIO,
        .baud_rate = BOARD_DFPLAYER_BAUD,
    };

    return dfplayer_pro_init(&config);
}

static bool command_ok(const char *what, dfplayer_err_t err);
static int query_seconds(int query_code);

static int longer_delay(int delay_ms)
{
    if (delay_ms >= BOARD_DFPLAYER_RETRY_MAX_MS / 2) {
        return BOARD_DFPLAYER_RETRY_MAX_MS;
    }
    return delay_ms * 2;
}

/* One pass of the startup sequence. The playback task repeats it until it succeeds. */
static bool initialize_audio(int attempt, int *file_count)
{
    dfplayer_err_t err;
    int files;

    ESP_LOGI(TAG, "DFPlayer handshake attempt %d", attempt);
    err = dfplayer_pro_at_test(s_response, sizeof(s_response), BOARD_DFPLAYER_CMD_TIMEOUT_MS);
    if (err == DFPLAYER_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "DFPlayer handshake timeout");
        return false;
    }
    if (err != DFPLAYER_OK) {
        ESP_LOGW(TAG, "DFPlayer handshake failed: %s", dfplayer_pro_err_str(err));
        return false;
    }
    ESP_LOGI(TAG, "DFPlayer handshake OK");

    files = query_seconds(2);
    if (files < 0) {
        ESP_LOGE(TAG, "DFPlayer initialization failed: file-count query");
        return false;
    }

    err = dfplayer_pro_set_amplifier(true, s_response, sizeof(s_response), BOARD_DFPLAYER_CMD_TIMEOUT_MS);
    if (!command_ok("Amplifier", err)) {
        ESP_LOGE(TAG, "DFPlayer initialization failed: amplifier");
        return false;
    }

    err = dfplayer_pro_set_volume(BOARD_DFPLAYER_VOLUME, s_response, sizeof(s_response),
                                  BOARD_DFPLAYER_CMD_TIMEOUT_MS);
    if (!command_ok("Volume 20", err)) {
        ESP_LOGE(TAG, "DFPlayer initialization failed: volume");
        return false;
    }

    err = dfplayer_pro_set_play_mode(BOARD_DFPLAYER_SINGLE_PLAY_MODE, s_response, sizeof(s_response),
                                     BOARD_DFPLAYER_CMD_TIMEOUT_MS);
    if (!command_ok("Single-play mode", err)) {
        ESP_LOGE(TAG, "DFPlayer initialization failed: play mode");
        return false;
    }

    *file_count = files;
    return true;
}

static bool command_ok(const char *what, dfplayer_err_t err)
{
    if (err == DFPLAYER_OK) {
        ESP_LOGI(TAG, "%s accepted", what);
        return true;
    }

    ESP_LOGE(TAG, "%s failed: %s. Response: %s", what, dfplayer_pro_err_str(err), s_response);
    return false;
}

static int query_seconds(int query_code)
{
    int value = -1;
    dfplayer_err_t err = dfplayer_pro_query_value(query_code, &value, s_response, sizeof(s_response),
                                                  BOARD_DFPLAYER_CMD_TIMEOUT_MS);
    if (err != DFPLAYER_OK) {
        ESP_LOGW(TAG, "AT+QUERY=%d failed: %s. Response: %s",
                 query_code, dfplayer_pro_err_str(err), s_response);
        return -1;
    }
    if (value < 0) {
        ESP_LOGW(TAG, "AT+QUERY=%d returned a negative value", query_code);
        return -1;
    }
    return value;
}

/*
 * DFR0768 has no playback-finished event. AT+QUERY=4 is the duration in whole
 * seconds and AT+QUERY=3 is the elapsed whole seconds. Poll those. A file is
 * finished when elapsed reaches the reported duration and the next sample does
 * not advance, or when the elapsed clock stops changing after playback has
 * started. The extra stalled sample avoids cutting off the last partial second.
 *
 * A duration of 0, or a failed duration query, means the module did not give a
 * usable length. Elapsed time is then watched until it stops, with a short cap
 * when the clock never leaves 0 so a sub-second file cannot block the test.
 * That cap can add a few seconds after a very short file. It is not a fixed
 * delay applied to every file. The per-file wait also stops at
 * BOARD_DFPLAYER_MAX_FILE_MS.
 */
static bool wait_for_file(int duration_s, char *note, size_t note_len, bool *link_down)
{
    int64_t started_us = esp_timer_get_time();
    int64_t last_change_us = started_us;
    int last_position = -1;
    int baseline = -1;
    bool tracking = false;
    bool saw_progress = false;
    int failed_polls = 0;

    *link_down = false;

    while (true) {
        int waited_ms = (int)((esp_timer_get_time() - started_us) / 1000);
        int position;
        int stalled_ms;

        if (waited_ms >= BOARD_DFPLAYER_MAX_FILE_MS) {
            note_copy(note, note_len, "playback wait reached the per-file time cap");
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(BOARD_DFPLAYER_POLL_MS));
        position = query_seconds(3);
        if (position < 0) {
            failed_polls++;
            if (failed_polls >= 5) {
                *link_down = true;
                note_copy(note, note_len, "elapsed-time query failed");
                return false;
            }
            continue;
        }
        failed_polls = 0;

        if (!tracking) {
            if (baseline < 0) {
                baseline = position;
            }
            if (position == 0 || position < baseline) {
                tracking = true;
                last_position = -1;
                last_change_us = esp_timer_get_time();
            } else if (waited_ms >= BOARD_DFPLAYER_STALL_MS) {
                tracking = true;
            }
            if (!tracking) {
                continue;
            }
        }

        if (position != last_position) {
            if (last_position >= 0 && position > last_position) {
                saw_progress = true;
            }
            last_position = position;
            last_change_us = esp_timer_get_time();
        }

        stalled_ms = (int)((esp_timer_get_time() - last_change_us) / 1000);
        if (duration_s > 0 && saw_progress && position >= duration_s && stalled_ms >= BOARD_DFPLAYER_POLL_MS) {
            return true;
        }
        if (saw_progress && stalled_ms >= BOARD_DFPLAYER_STALL_MS) {
            if (duration_s > 0 && position + 1 < duration_s) {
                note_copy(note, note_len, "elapsed clock stopped before the reported duration");
                return false;
            }
            return true;
        }
        if (!saw_progress && duration_s <= 0 && waited_ms >= BOARD_DFPLAYER_SHORT_FILE_MS) {
            note_copy(note, note_len,
                      "duration was 0 or unavailable and elapsed stayed at 0; ended after the short-file cap");
            return true;
        }
        if (!saw_progress && duration_s > 0 && waited_ms >= (duration_s * 1000) + BOARD_DFPLAYER_SHORT_FILE_MS) {
            note_copy(note, note_len, "elapsed time never advanced");
            return false;
        }
    }
}

static void set_idle(const char *error)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_playing = false;
    s_current_sound[0] = '\0';
    if (error != NULL && error[0] != '\0') {
        snprintf(s_last_error, sizeof(s_last_error), "%s", error);
    } else {
        s_last_error[0] = '\0';
    }
    xSemaphoreGive(s_state_lock);
}

static bool run_playback(const sound_catalog_entry_t *sound)
{
    dfplayer_err_t err;
    int duration = -1;
    char note[80];
    bool link_down = false;

    note[0] = '\0';
    ESP_LOGI(TAG, "Playing %s", sound->path);
    err = dfplayer_pro_play_file(sound->path, s_response, sizeof(s_response),
                                 BOARD_DFPLAYER_PLAY_TIMEOUT_MS);
    if (err != DFPLAYER_OK) {
        ESP_LOGE(TAG, "Play command failed: %s. Response: %s", dfplayer_pro_err_str(err), s_response);
        set_idle(dfplayer_pro_err_str(err));
        return err == DFPLAYER_ERR_TIMEOUT || err == DFPLAYER_ERR_UART || err == DFPLAYER_ERR_NOT_READY;
    }
    ESP_LOGI(TAG, "DFPlayer accepted %s", sound->name);

    for (int attempt = 0; attempt < 4 && duration < 0; attempt++) {
        duration = query_seconds(4);
        if (duration < 0) {
            vTaskDelay(pdMS_TO_TICKS(BOARD_DFPLAYER_POLL_MS));
        }
    }
    if (duration < 0) {
        ESP_LOGW(TAG, "Duration query failed. Waiting on the elapsed-time clock instead.");
    }

    if (!wait_for_file(duration, note, sizeof(note), &link_down)) {
        ESP_LOGW(TAG, "Playback ended: %s", note[0] != '\0' ? note : "incomplete");
        set_idle(note[0] != '\0' ? note : "incomplete");
        return link_down;
    }

    ESP_LOGI(TAG, "Playback completed (elapsed clock; the module has no finished event)");
    set_idle(NULL);
    return false;
}

static void playback_task(void *arg)
{
    int attempt = 0;
    int delay_ms = BOARD_DFPLAYER_BOOT_DELAY_MS;
    bool ever_ready = false;

    (void)arg;
    while (true) {
        if (!whistle_dfplayer_connected()) {
            const sound_catalog_entry_t *stale = NULL;
            int files = -1;
            dfplayer_err_t link_err = DFPLAYER_OK;

            while (xQueueReceive(s_play_queue, &stale, 0) == pdTRUE) {
                ESP_LOGW(TAG, "Dropped a queued horn; audio is not ready");
                set_idle("audio_not_ready");
            }
            (void)stale;

            if (!dfplayer_pro_is_ready()) {
                link_err = setup_link();
                if (link_err != DFPLAYER_OK) {
                    ESP_LOGE(TAG, "DFPlayer initialization failed: UART");
                    vTaskDelay(pdMS_TO_TICKS(delay_ms));
                    delay_ms = longer_delay(delay_ms);
                    continue;
                }
            }

            ESP_LOGI(TAG, "Waiting %d ms for the DFPlayer to finish starting", delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            attempt++;
            if (!initialize_audio(attempt, &files)) {
                whistle_set_dfplayer_ready(false);
                delay_ms = longer_delay(delay_ms);
                continue;
            }

            whistle_set_module_file_count(files);
            whistle_set_dfplayer_ready(true);
            whistle_mdns_set_horn_ready(true);
            if (ever_ready) {
                ESP_LOGI(TAG, "DFPlayer audio recovered");
            } else {
                ESP_LOGI(TAG, "DFPlayer audio ready");
            }
            ever_ready = true;
            attempt = 0;
            delay_ms = BOARD_DFPLAYER_BOOT_DELAY_MS;
            {
                size_t catalog_count = 0;
                sound_catalog_entries(&catalog_count);
                ESP_LOGI(TAG, "DFPlayer reports %d files; catalog lists %u configured names",
                         files, (unsigned)catalog_count);
            }
            continue;
        }

        const sound_catalog_entry_t *sound = NULL;
        if (xQueueReceive(s_play_queue, &sound, portMAX_DELAY) != pdTRUE || sound == NULL) {
            continue;
        }
        if (run_playback(sound)) {
            whistle_set_dfplayer_ready(false);
            whistle_set_module_file_count(-1);
            whistle_mdns_set_horn_ready(false);
            attempt = 0;
            delay_ms = BOARD_DFPLAYER_BOOT_DELAY_MS;
            ESP_LOGW(TAG, "DFPlayer communication lost");
        }
    }
}

esp_err_t whistle_playback_start(void)
{
    if (s_play_queue != NULL) {
        return ESP_OK;
    }
    s_state_lock = xSemaphoreCreateMutex();
    s_play_queue = xQueueCreate(1, sizeof(const sound_catalog_entry_t *));
    if (s_state_lock == NULL || s_play_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(playback_task, "playback", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void whistle_set_dfplayer_ready(bool ready)
{
    if (s_state_lock == NULL) {
        s_dfplayer_ready = ready;
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_dfplayer_ready = ready;
    xSemaphoreGive(s_state_lock);
}

bool whistle_dfplayer_connected(void)
{
    bool ready = false;
    if (s_state_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    ready = s_dfplayer_ready;
    xSemaphoreGive(s_state_lock);
    return ready;
}

void whistle_set_module_file_count(int count)
{
    if (s_state_lock == NULL) {
        s_module_file_count = count;
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_module_file_count = count;
    xSemaphoreGive(s_state_lock);
}

int whistle_module_file_count(void)
{
    int count = -1;
    if (s_state_lock == NULL) {
        return -1;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    count = s_module_file_count;
    xSemaphoreGive(s_state_lock);
    return count;
}

esp_err_t whistle_request_play(const char *name)
{
    const sound_catalog_entry_t *sound;
    bool busy = false;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sound_catalog_ready()) {
        ESP_LOGE(TAG, "playback rejected; sound catalog is unavailable");
        return ESP_FAIL;
    }
    sound = sound_catalog_find(name);
    if (sound == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_state_lock == NULL || s_play_queue == NULL || !whistle_dfplayer_connected()) {
        return ESP_FAIL;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    busy = s_playing;
    if (!busy) {
        s_playing = true;
        snprintf(s_current_sound, sizeof(s_current_sound), "%s", sound->name);
        s_last_error[0] = '\0';
    }
    xSemaphoreGive(s_state_lock);
    if (busy) {
        ESP_LOGW(TAG, "playback rejected; already playing");
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_play_queue, &sound, 0) != pdTRUE) {
        set_idle("playback queue full");
        ESP_LOGW(TAG, "playback rejected; already playing");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Playback queued: %s", sound->name);
    return ESP_OK;
}

esp_err_t whistle_play_default(void)
{
    char name[SOUND_NAME_MAX + 1];

    if (sound_selection_get(name, sizeof(name)) != ESP_OK) {
        ESP_LOGE(TAG, "No selected horn");
        return ESP_FAIL;
    }
    return whistle_request_play(name);
}

void whistle_get_status(whistle_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (s_state_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    status->playing = s_playing;
    status->dfplayer_connected = s_dfplayer_ready;
    snprintf(status->current_sound, sizeof(status->current_sound), "%s", s_current_sound);
    snprintf(status->last_error, sizeof(status->last_error), "%s", s_last_error);
    xSemaphoreGive(s_state_lock);
}

static void button_loop(void)
{
    int stable = gpio_get_level(BOARD_HORN_BUTTON_GPIO);
    int candidate = stable;
    int candidate_ms = 0;
    bool holding = false;
    int held_ms = 0;
    bool long_press_sent = false;

    while (true) {
        int level = gpio_get_level(BOARD_HORN_BUTTON_GPIO);

        if (level != candidate) {
            candidate = level;
            candidate_ms = 0;
        } else if (candidate_ms < BOARD_BUTTON_DEBOUNCE_MS) {
            candidate_ms += BOARD_BUTTON_POLL_MS;
            if (candidate_ms >= BOARD_BUTTON_DEBOUNCE_MS && candidate != stable) {
                stable = candidate;
                if (stable == 0) {
                    holding = true;
                    held_ms = 0;
                    long_press_sent = false;
                } else if (holding) {
                    holding = false;
                    if (!long_press_sent) {
                        esp_err_t play_err = whistle_play_default();
                        if (play_err == ESP_OK) {
                            ESP_LOGI(TAG, "Button pressed");
                        } else if (!whistle_dfplayer_connected()) {
                            ESP_LOGW(TAG, "Button press ignored; audio is not ready");
                        } else if (play_err == ESP_FAIL && !sound_catalog_ready()) {
                            ESP_LOGW(TAG, "Button press ignored; sound catalog is unavailable");
                        } else if (play_err == ESP_FAIL) {
                            ESP_LOGW(TAG, "Button press ignored; DFPlayer is unavailable");
                        }
                    }
                    ESP_LOGI(TAG, "Button released");
                }
            }
        }

        if (holding && stable == 0 && !long_press_sent) {
            if (held_ms < BOARD_BUTTON_LONG_PRESS_MS) {
                held_ms += BOARD_BUTTON_POLL_MS;
            }
            if (held_ms >= BOARD_BUTTON_LONG_PRESS_MS) {
                long_press_sent = true;
                ESP_LOGI(TAG, "Provisioning requested");
                esp_err_t err = wifi_start_provisioning();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Provisioning request failed: %s", esp_err_to_name(err));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BOARD_BUTTON_POLL_MS));
    }
}

static void init_network(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition is unusable and will be formatted");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
        return;
    }
    err = wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
        return;
    }
    whistle_mdns_start();
    err = whistle_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP init failed: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    esp_err_t gpio_err;
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << BOARD_HORN_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_LOGI(TAG, "Logs use USB Serial/JTAG. DFPlayer UART is UART%d on GPIO%d (TX) and GPIO%d (RX).",
             BOARD_DFPLAYER_UART_PORT, BOARD_DFPLAYER_TX_GPIO, BOARD_DFPLAYER_RX_GPIO);

    sound_catalog_load();

    if (whistle_playback_start() != ESP_OK) {
        ESP_LOGE(TAG, "Playback task did not start");
    }
    init_network();

    gpio_err = gpio_config(&button);
    if (gpio_err != ESP_OK) {
        ESP_LOGE(TAG, "Button GPIO%d configuration failed: %s", BOARD_HORN_BUTTON_GPIO, esp_err_to_name(gpio_err));
    }

    ESP_LOGI(TAG, "Horn button ready on GPIO%d", BOARD_HORN_BUTTON_GPIO);
    button_loop();
}
