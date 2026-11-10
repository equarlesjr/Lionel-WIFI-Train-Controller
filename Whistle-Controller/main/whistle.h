#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    bool playing;
    bool dfplayer_connected;
    char current_sound[48];
    char last_error[80];
} whistle_status_t;

/* Create the single playback task. Call before the HTTP server starts. */
esp_err_t whistle_playback_start(void);

/* True after the startup AT handshake. A later play failure does not clear it. */
void whistle_set_dfplayer_ready(bool ready);

bool whistle_dfplayer_connected(void);

/* AT+QUERY=2 result from startup. Negative when the query did not answer. */
void whistle_set_module_file_count(int count);

int whistle_module_file_count(void);

/*
 * Queue one catalogued sound. Returns when the request is accepted or rejected.
 * ESP_OK means accepted. ESP_ERR_INVALID_STATE means one is already active.
 * ESP_ERR_NOT_FOUND means the name is not in the catalog. ESP_FAIL means the
 * DFPlayer did not answer at startup. Playback itself runs on the playback task.
 */
esp_err_t whistle_request_play(const char *name);

/* Queue the session horn. Same result codes as whistle_request_play(). */
esp_err_t whistle_play_default(void);

void whistle_get_status(whistle_status_t *status);
