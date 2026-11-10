#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFPLAYER_PRO_RESPONSE_MAX 256

typedef enum {
    DFPLAYER_OK = 0,
    DFPLAYER_ERR_INVALID_ARG,
    DFPLAYER_ERR_NOT_READY,
    DFPLAYER_ERR_UART,
    DFPLAYER_ERR_TIMEOUT,
    DFPLAYER_ERR_RESPONSE,
} dfplayer_err_t;

typedef struct {
    int uart_port;
    int tx_gpio;
    int rx_gpio;
    int baud_rate;
} dfplayer_pro_config_t;

const char *dfplayer_pro_err_str(dfplayer_err_t err);

dfplayer_err_t dfplayer_pro_init(const dfplayer_pro_config_t *config);

bool dfplayer_pro_is_ready(void);

/* command is the AT text without the CRLF terminator. */
dfplayer_err_t dfplayer_pro_send(const char *command);

/* Stores a NUL-terminated response. Returns DFPLAYER_OK when any byte arrives. */
dfplayer_err_t dfplayer_pro_read(char *response, size_t response_len, uint32_t timeout_ms);

/* Send, read, and require a response line of OK. */
dfplayer_err_t dfplayer_pro_transact(const char *command, char *response, size_t response_len,
                                     uint32_t timeout_ms);

dfplayer_err_t dfplayer_pro_at_test(char *response, size_t response_len, uint32_t timeout_ms);

dfplayer_err_t dfplayer_pro_set_volume(int volume, char *response, size_t response_len,
                                       uint32_t timeout_ms);

/* Succeeds when the module returns a VOL report, which is not an OK line. */
dfplayer_err_t dfplayer_pro_query_volume(char *response, size_t response_len, uint32_t timeout_ms);

dfplayer_err_t dfplayer_pro_set_play_mode(int mode, char *response, size_t response_len,
                                          uint32_t timeout_ms);

dfplayer_err_t dfplayer_pro_play_file(const char *path, char *response, size_t response_len,
                                      uint32_t timeout_ms);

dfplayer_err_t dfplayer_pro_play_number(int file_number, char *response, size_t response_len,
                                        uint32_t timeout_ms);

dfplayer_err_t dfplayer_pro_set_amplifier(bool enabled, char *response, size_t response_len,
                                          uint32_t timeout_ms);

/* Query that is successful when the module answers, whether or not the body is OK. */
dfplayer_err_t dfplayer_pro_query(const char *command, char *response, size_t response_len,
                                  uint32_t timeout_ms);

/*
 * AT+QUERY=1..4. The DFR0768 returns an integer, not OK.
 * QUERY=2 is the playable file count, QUERY=3 is elapsed seconds,
 * and QUERY=4 is the current file's duration in seconds.
 */
dfplayer_err_t dfplayer_pro_query_value(int query_code, int *value, char *response, size_t response_len,
                                        uint32_t timeout_ms);

/* AT+QUERY=5. Copies the current filename into name. The raw module text stays in response. */
dfplayer_err_t dfplayer_pro_query_filename(char *name, size_t name_len, char *response, size_t response_len,
                                           uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
