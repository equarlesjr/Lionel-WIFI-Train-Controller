#include "dfplayer_pro.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "dfplayer";

#define CMD_MAX          96
#define RX_IDLE_US       40000
#define READ_SLICE_MS    50
#define UART_RX_BUF      1024
#define UART_TX_BUF      0

static bool s_ready;
static uart_port_t s_port;
static size_t s_last_rx_len;

const char *dfplayer_pro_err_str(dfplayer_err_t err)
{
    switch (err) {
    case DFPLAYER_OK:
        return "ok";
    case DFPLAYER_ERR_INVALID_ARG:
        return "invalid argument";
    case DFPLAYER_ERR_NOT_READY:
        return "UART is not initialized";
    case DFPLAYER_ERR_UART:
        return "UART driver error";
    case DFPLAYER_ERR_TIMEOUT:
        return "no response before timeout";
    case DFPLAYER_ERR_RESPONSE:
        return "module rejected the command";
    default:
        return "unknown error";
    }
}

bool dfplayer_pro_is_ready(void)
{
    return s_ready;
}

static void log_payload(const char *direction, const char *text)
{
    char shown[180];
    size_t out = 0;

    if (text == NULL) {
        text = "";
    }

    for (size_t i = 0; text[i] != '\0' && out + 4 < sizeof(shown); i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\r') {
            shown[out++] = '\\';
            shown[out++] = 'r';
        } else if (ch == '\n') {
            shown[out++] = '\\';
            shown[out++] = 'n';
        } else if (ch < 32 || ch > 126) {
            shown[out++] = '.';
        } else {
            shown[out++] = (char)ch;
        }
    }
    shown[out] = '\0';
    ESP_LOGI(TAG, "%s %s", direction, shown[0] != '\0' ? shown : "<empty>");
}

static bool line_is_ok(const char *start, size_t length)
{
    while (length > 0 && isspace((unsigned char)start[length - 1])) {
        length--;
    }

    size_t index = 0;
    while (index < length && isspace((unsigned char)start[index])) {
        index++;
    }

    return (length - index) == 2 &&
           tolower((unsigned char)start[index]) == 'o' &&
           tolower((unsigned char)start[index + 1]) == 'k';
}

static bool response_has_ok(const char *response)
{
    const char *cursor = response;

    if (response == NULL) {
        return false;
    }

    while (*cursor != '\0') {
        const char *end = strpbrk(cursor, "\r\n");
        size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        if (line_is_ok(cursor, length)) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }

    return false;
}

static bool response_has_token(const char *response, const char *token)
{
    size_t token_len;

    if (response == NULL || token == NULL) {
        return false;
    }

    token_len = strlen(token);
    for (const char *cursor = response; *cursor != '\0'; cursor++) {
        size_t i = 0;
        while (i < token_len && cursor[i] != '\0' &&
               tolower((unsigned char)cursor[i]) == tolower((unsigned char)token[i])) {
            i++;
        }
        if (i == token_len) {
            return true;
        }
    }

    return false;
}

dfplayer_err_t dfplayer_pro_init(const dfplayer_pro_config_t *config)
{
    uart_config_t uart_config;
    esp_err_t err;

    if (s_ready) {
        return DFPLAYER_OK;
    }

    if (config == NULL || config->baud_rate <= 0 || config->tx_gpio < 0 || config->rx_gpio < 0 ||
        config->uart_port < 0 || config->uart_port >= UART_NUM_MAX) {
        ESP_LOGE(TAG, "UART init failed: invalid configuration");
        return DFPLAYER_ERR_INVALID_ARG;
    }

    memset(&uart_config, 0, sizeof(uart_config));
    uart_config.baud_rate = config->baud_rate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    s_port = (uart_port_t)config->uart_port;

    err = uart_driver_install(s_port, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: uart_driver_install returned %s", esp_err_to_name(err));
        return DFPLAYER_ERR_UART;
    }

    err = uart_param_config(s_port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: uart_param_config returned %s", esp_err_to_name(err));
        uart_driver_delete(s_port);
        return DFPLAYER_ERR_UART;
    }

    err = uart_set_pin(s_port, config->tx_gpio, config->rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: uart_set_pin returned %s", esp_err_to_name(err));
        uart_driver_delete(s_port);
        return DFPLAYER_ERR_UART;
    }

    uart_flush_input(s_port);
    s_ready = true;
    ESP_LOGI(TAG, "UART%d ready: TX GPIO%d, RX GPIO%d, %d baud, 8N1",
             (int)s_port, config->tx_gpio, config->rx_gpio, config->baud_rate);
    return DFPLAYER_OK;
}

dfplayer_err_t dfplayer_pro_send(const char *command)
{
    char framed[CMD_MAX + 3];
    int length;
    int written;

    if (!s_ready) {
        return DFPLAYER_ERR_NOT_READY;
    }
    if (command == NULL || command[0] == '\0' || strlen(command) > CMD_MAX) {
        return DFPLAYER_ERR_INVALID_ARG;
    }
    if (strstr(command, "\r") != NULL || strstr(command, "\n") != NULL) {
        return DFPLAYER_ERR_INVALID_ARG;
    }

    length = snprintf(framed, sizeof(framed), "%s\r\n", command);
    if (length < 0 || (size_t)length >= sizeof(framed)) {
        return DFPLAYER_ERR_INVALID_ARG;
    }

    uart_flush_input(s_port);
    log_payload("TX", framed);

    written = uart_write_bytes(s_port, framed, (size_t)length);
    if (written != length) {
        ESP_LOGE(TAG, "UART write failed (%d of %d bytes)", written, length);
        return DFPLAYER_ERR_UART;
    }

    if (uart_wait_tx_done(s_port, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGE(TAG, "UART transmit did not finish");
        return DFPLAYER_ERR_UART;
    }

    return DFPLAYER_OK;
}

dfplayer_err_t dfplayer_pro_read(char *response, size_t response_len, uint32_t timeout_ms)
{
    size_t used = 0;
    int64_t deadline;
    int64_t last_byte = 0;
    bool saw_newline = false;

    if (!s_ready) {
        return DFPLAYER_ERR_NOT_READY;
    }
    if (response == NULL || response_len < 2 || timeout_ms == 0) {
        return DFPLAYER_ERR_INVALID_ARG;
    }

    response[0] = '\0';
    s_last_rx_len = 0;
    deadline = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);

    while (esp_timer_get_time() < deadline && used + 1 < response_len) {
        uint8_t byte = 0;
        int64_t remaining_us = deadline - esp_timer_get_time();
        int slice_ms = READ_SLICE_MS;

        if (remaining_us < (int64_t)slice_ms * 1000) {
            slice_ms = (int)(remaining_us / 1000);
            if (slice_ms < 1) {
                slice_ms = 1;
            }
        }

        int n = uart_read_bytes(s_port, &byte, 1, pdMS_TO_TICKS(slice_ms));
        if (n < 0) {
            ESP_LOGE(TAG, "UART read failed");
            return DFPLAYER_ERR_UART;
        }
        if (n == 0) {
            if (used > 0 && saw_newline &&
                (esp_timer_get_time() - last_byte) >= RX_IDLE_US) {
                break;
            }
            continue;
        }

        response[used++] = (char)byte;
        response[used] = '\0';
        last_byte = esp_timer_get_time();
        if (byte == '\n') {
            saw_newline = true;
        }
    }

    s_last_rx_len = used;
    if (used == 0) {
        ESP_LOGW(TAG, "RX timeout after %u ms", (unsigned)timeout_ms);
        return DFPLAYER_ERR_TIMEOUT;
    }

    log_payload("RX", response);
    return DFPLAYER_OK;
}

static void clear_response(char *response, size_t response_len)
{
    if (response != NULL && response_len > 0) {
        response[0] = '\0';
    }
}

dfplayer_err_t dfplayer_pro_transact(const char *command, char *response, size_t response_len,
                                     uint32_t timeout_ms)
{
    dfplayer_err_t err;

    clear_response(response, response_len);
    err = dfplayer_pro_send(command);
    if (err != DFPLAYER_OK) {
        return err;
    }

    err = dfplayer_pro_read(response, response_len, timeout_ms);
    if (err != DFPLAYER_OK) {
        return err;
    }

    if (!response_has_ok(response)) {
        ESP_LOGW(TAG, "Response to %s did not contain OK", command);
        return DFPLAYER_ERR_RESPONSE;
    }

    return DFPLAYER_OK;
}

dfplayer_err_t dfplayer_pro_at_test(char *response, size_t response_len, uint32_t timeout_ms)
{
    return dfplayer_pro_transact("AT", response, response_len, timeout_ms);
}

dfplayer_err_t dfplayer_pro_set_volume(int volume, char *response, size_t response_len,
                                       uint32_t timeout_ms)
{
    char command[16];

    if (volume < 0 || volume > 30) {
        return DFPLAYER_ERR_INVALID_ARG;
    }

    snprintf(command, sizeof(command), "AT+VOL=%d", volume);
    return dfplayer_pro_transact(command, response, response_len, timeout_ms);
}

dfplayer_err_t dfplayer_pro_query_volume(char *response, size_t response_len, uint32_t timeout_ms)
{
    dfplayer_err_t err;

    clear_response(response, response_len);
    err = dfplayer_pro_send("AT+VOL=?");
    if (err != DFPLAYER_OK) {
        return err;
    }

    err = dfplayer_pro_read(response, response_len, timeout_ms);
    if (err != DFPLAYER_OK) {
        return err;
    }

    if (!response_has_token(response, "VOL")) {
        ESP_LOGW(TAG, "Volume query response did not contain VOL");
        return DFPLAYER_ERR_RESPONSE;
    }

    return DFPLAYER_OK;
}

dfplayer_err_t dfplayer_pro_set_play_mode(int mode, char *response, size_t response_len,
                                          uint32_t timeout_ms)
{
    char command[20];

    if (mode < 1 || mode > 5) {
        return DFPLAYER_ERR_INVALID_ARG;
    }

    snprintf(command, sizeof(command), "AT+PLAYMODE=%d", mode);
    return dfplayer_pro_transact(command, response, response_len, timeout_ms);
}

dfplayer_err_t dfplayer_pro_play_file(const char *path, char *response, size_t response_len,
                                      uint32_t timeout_ms)
{
    char command[CMD_MAX];

    if (path == NULL || path[0] != '/' || strlen(path) > 64) {
        return DFPLAYER_ERR_INVALID_ARG;
    }

    snprintf(command, sizeof(command), "AT+PLAYFILE=%s", path);
    return dfplayer_pro_transact(command, response, response_len, timeout_ms);
}

dfplayer_err_t dfplayer_pro_play_number(int file_number, char *response, size_t response_len,
                                        uint32_t timeout_ms)
{
    char command[20];

    if (file_number < 1 || file_number > 9999) {
        return DFPLAYER_ERR_INVALID_ARG;
    }

    snprintf(command, sizeof(command), "AT+PLAYNUM=%d", file_number);
    return dfplayer_pro_transact(command, response, response_len, timeout_ms);
}

dfplayer_err_t dfplayer_pro_set_amplifier(bool enabled, char *response, size_t response_len,
                                          uint32_t timeout_ms)
{
    return dfplayer_pro_transact(enabled ? "AT+AMP=ON" : "AT+AMP=OFF", response, response_len, timeout_ms);
}

dfplayer_err_t dfplayer_pro_query(const char *command, char *response, size_t response_len,
                                  uint32_t timeout_ms)
{
    dfplayer_err_t err;

    clear_response(response, response_len);
    err = dfplayer_pro_send(command);
    if (err != DFPLAYER_OK) {
        return err;
    }

    return dfplayer_pro_read(response, response_len, timeout_ms);
}

static bool line_is_integer(const char *line, size_t length, int *value)
{
    size_t index = 0;
    int parsed = 0;

    while (index < length && isspace((unsigned char)line[index])) {
        index++;
    }
    if (index >= length || !isdigit((unsigned char)line[index])) {
        return false;
    }

    while (index < length && isdigit((unsigned char)line[index])) {
        parsed = parsed * 10 + (line[index] - '0');
        if (parsed > 100000) {
            return false;
        }
        index++;
    }
    while (index < length && isspace((unsigned char)line[index])) {
        index++;
    }
    if (index != length) {
        return false;
    }

    *value = parsed;
    return true;
}

/* Use the last line that is only an integer, so an echoed AT+QUERY=n is ignored. */
static bool parse_query_number(const char *response, int *value)
{
    const char *cursor = response;
    bool found = false;

    if (response == NULL) {
        return false;
    }

    while (*cursor != '\0') {
        const char *end = strpbrk(cursor, "\r\n");
        size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        int parsed = 0;

        if (line_is_integer(cursor, length, &parsed)) {
            *value = parsed;
            found = true;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }

    return found;
}

static void copy_ascii_filename(const uint8_t *raw, size_t raw_len, char *name, size_t name_len)
{
    size_t out = 0;

    for (size_t i = 0; i < raw_len && out + 1 < name_len; i++) {
        unsigned char ch = raw[i];
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            break;
        }
        if (ch < 32 || ch > 126) {
            continue;
        }
        name[out++] = (char)ch;
    }
    name[out] = '\0';
}

static bool looks_like_utf16le(const uint8_t *raw, size_t raw_len)
{
    size_t pairs = 0;
    size_t zero_high = 0;

    if (raw_len < 4) {
        return false;
    }

    for (size_t i = 0; i + 1 < raw_len; i += 2) {
        if (raw[i] == '\r' || raw[i] == '\n') {
            break;
        }
        pairs++;
        if (raw[i + 1] == 0) {
            zero_high++;
        }
    }

    return pairs >= 2 && zero_high == pairs;
}

static void copy_utf16le_filename(const uint8_t *raw, size_t raw_len, char *name, size_t name_len)
{
    size_t out = 0;

    for (size_t i = 0; i + 1 < raw_len && out + 1 < name_len; i += 2) {
        if (raw[i + 1] != 0 || raw[i] == '\r' || raw[i] == '\n' || raw[i] == '\0') {
            break;
        }
        if (raw[i] < 32 || raw[i] > 126) {
            continue;
        }
        name[out++] = (char)raw[i];
    }
    name[out] = '\0';
}

dfplayer_err_t dfplayer_pro_query_value(int query_code, int *value, char *response, size_t response_len,
                                        uint32_t timeout_ms)
{
    char command[16];
    dfplayer_err_t err;

    if (value == NULL || query_code < 1 || query_code > 4) {
        return DFPLAYER_ERR_INVALID_ARG;
    }

    snprintf(command, sizeof(command), "AT+QUERY=%d", query_code);
    err = dfplayer_pro_query(command, response, response_len, timeout_ms);
    if (err != DFPLAYER_OK) {
        return err;
    }

    if (!parse_query_number(response, value)) {
        ESP_LOGW(TAG, "QUERY=%d response had no integer line", query_code);
        return DFPLAYER_ERR_RESPONSE;
    }

    return DFPLAYER_OK;
}

dfplayer_err_t dfplayer_pro_query_filename(char *name, size_t name_len, char *response, size_t response_len,
                                           uint32_t timeout_ms)
{
    dfplayer_err_t err;

    if (name == NULL || name_len < 2) {
        return DFPLAYER_ERR_INVALID_ARG;
    }

    name[0] = '\0';
    err = dfplayer_pro_query("AT+QUERY=5", response, response_len, timeout_ms);
    if (err != DFPLAYER_OK) {
        return err;
    }

    if (looks_like_utf16le((const uint8_t *)response, s_last_rx_len)) {
        copy_utf16le_filename((const uint8_t *)response, s_last_rx_len, name, name_len);
    } else {
        copy_ascii_filename((const uint8_t *)response, s_last_rx_len, name, name_len);
    }

    if (name[0] == '\0') {
        ESP_LOGW(TAG, "QUERY=5 response did not contain a filename");
        return DFPLAYER_ERR_RESPONSE;
    }

    return DFPLAYER_OK;
}
