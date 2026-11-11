#include "whistle_http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sound_catalog.h"
#include "whistle.h"
#include "wifi.h"

static const char *TAG = "whistle_http";

static void add_cors(httpd_req_t *req, const char *origin);
static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json);

extern const unsigned char wifi_setup_html_start[] asm("_binary_wifi_setup_html_start");
extern const unsigned char wifi_setup_html_end[] asm("_binary_wifi_setup_html_end");
extern const unsigned char sound_console_html_start[] asm("_binary_sound_console_html_start");
extern const unsigned char sound_console_html_end[] asm("_binary_sound_console_html_end");

static const char SAVED_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Whistleblower Wi-Fi</title></head><body>"
    "<h1>Saved</h1>"
    "<p>The board is joining that network. Reconnect this phone to the same network, "
    "then read the assigned IP from the serial log. "
    "A health check is at <code>/api/health</code>.</p>"
    "</body></html>";

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *text)
{
    char *read = text;
    char *write = text;

    while (*read != '\0') {
        if (*read == '+') {
            *write++ = ' ';
            read++;
        } else if (*read == '%' && hex_nibble(read[1]) >= 0 && hex_nibble(read[2]) >= 0) {
            *write++ = (char)((hex_nibble(read[1]) << 4) | hex_nibble(read[2]));
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static void trim(char *text)
{
    char *start = text;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
}

static bool printable_text(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = body;

    out[0] = '\0';
    while (*p != '\0') {
        if ((p == body || p[-1] == '&') && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            size_t i = 0;
            while (*p != '\0' && *p != '&' && i + 1 < out_len) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            url_decode(out);
            trim(out);
            return true;
        }
        while (*p != '\0' && *p != '&') {
            p++;
        }
        if (*p == '&') {
            p++;
        }
    }
    return false;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[40];
    const char *p;
    size_t i = 0;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    out[0] = '\0';
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    while (*p != '\0' && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    trim(out);
    return out[0] != '\0' || *p == '"';
}

static void drain_body(httpd_req_t *req)
{
    char tmp[64];
    int remaining = req->content_len;

    while (remaining > 0) {
        int chunk = remaining > (int)sizeof(tmp) ? (int)sizeof(tmp) : remaining;
        int n = httpd_req_recv(req, tmp, chunk);
        if (n <= 0) {
            break;
        }
        remaining -= n;
    }
}

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int remaining;
    int off = 0;

    if (req->content_len <= 0 || (size_t)req->content_len >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf + off, remaining);
        if (n <= 0) {
            return ESP_FAIL;
        }
        off += n;
        remaining -= n;
    }
    buf[off] = '\0';
    return ESP_OK;
}

static esp_err_t send_html(httpd_req_t *req, const char *status, const char *html)
{
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_embedded_html(httpd_req_t *req, const unsigned char *start, const unsigned char *end)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)start, (size_t)(end - start));
}

static esp_err_t setup_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "setup page %s", req->uri);
    return send_embedded_html(req, wifi_setup_html_start, wifi_setup_html_end);
}

static esp_err_t console_get_handler(httpd_req_t *req)
{
    return send_embedded_html(req, sound_console_html_start, sound_console_html_end);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (wifi_is_provisioning()) {
        return setup_get_handler(req);
    }
    return console_get_handler(req);
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t post_only_get_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Allow", "POST");
    return send_json(req, "405 Method Not Allowed",
                     "{\"error\":\"use POST\",\"page\":\"/\"}");
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    wifi_status_t wifi = {0};
    char body[256];
    bool wifi_connected = false;
    bool audio_ready;

    if (wifi_get_status(&wifi) == ESP_OK) {
        wifi_connected = wifi.sta_connected;
    }
    audio_ready = whistle_dfplayer_connected();

    snprintf(body, sizeof(body),
             "{\n  \"service\": \"whistleblower\",\n  \"status\": \"%s\",\n"
             "  \"audio_ready\": %s,\n"
             "  \"wifi_connected\": %s,\n  \"dfplayer_connected\": %s\n}\n",
             audio_ready ? "ok" : "starting",
             audio_ready ? "true" : "false",
             wifi_connected ? "true" : "false",
             audio_ready ? "true" : "false");
    return send_json(req, NULL, body);
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char body[512];
    char ssid[WIFI_SSID_MAX + 1] = {0};
    char password[WIFI_PASS_MAX + 1] = {0};
    bool from_json = false;

    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return send_html(req, "400 Bad Request",
                         "<!DOCTYPE html><html><body><p>Could not read the form.</p></body></html>");
    }

    from_json = strstr(body, "\"ssid\"") != NULL;
    if (from_json) {
        json_get_string(body, "ssid", ssid, sizeof(ssid));
        json_get_string(body, "password", password, sizeof(password));
    } else {
        form_get(body, "ssid", ssid, sizeof(ssid));
        form_get(body, "password", password, sizeof(password));
    }

    if (ssid[0] == '\0' || strlen(ssid) > WIFI_SSID_MAX || !printable_text(ssid) ||
        strlen(password) > WIFI_PASS_MAX || !printable_text(password)) {
        memset(password, 0, sizeof(password));
        if (from_json) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_send(req, "{\"error\":\"ssid is required\"}", HTTPD_RESP_USE_STRLEN);
        }
        return send_html(req, "400 Bad Request",
                         "<!DOCTYPE html><html><body><p>Enter a network name. "
                         "<a href=\"/\">Back</a></p></body></html>");
    }

    if (wifi_save_and_connect(ssid, password) != ESP_OK) {
        memset(password, 0, sizeof(password));
        return send_html(req, "500 Internal Server Error",
                         "<!DOCTYPE html><html><body><p>Could not save that network.</p></body></html>");
    }
    memset(password, 0, sizeof(password));

    if (from_json) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }
    return send_html(req, NULL, SAVED_HTML);
}

static esp_err_t forget_post_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        drain_body(req);
    }
    if (wifi_forget() != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"Failed to forget Wi-Fi\"}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;

    for (size_t i = 0; in != NULL && in[i] != '\0' && o + 2 < out_len; i++) {
        if (in[i] == '"' || in[i] == '\\') {
            if (o + 3 >= out_len) {
                break;
            }
            out[o++] = '\\';
        }
        out[o++] = in[i];
    }
    if (out_len > 0) {
        out[o] = '\0';
    }
}

static const char *wifi_state_str(wifi_state_t state)
{
    switch (state) {
    case WIFI_STATE_PROVISIONING:
        return "provisioning";
    case WIFI_STATE_CONNECTING:
        return "connecting";
    case WIFI_STATE_CONNECTED:
        return "connected";
    default:
        return "idle";
    }
}

static bool read_dec(const char **cursor, int *out, int max_digits)
{
    const char *s = *cursor;
    int value = 0;
    int digits = 0;

    while (*s >= '0' && *s <= '9' && digits < max_digits) {
        value = value * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    *out = value;
    *cursor = s;
    return true;
}

static bool port_suffix_ok(const char *text)
{
    int port = 0;

    if (*text == '\0') {
        return true;
    }
    if (*text != ':') {
        return false;
    }
    text++;
    if (!read_dec(&text, &port, 5) || *text != '\0' || port < 1 || port > 65535) {
        return false;
    }
    return true;
}

static bool private_or_loopback_host(const char *host)
{
    int a = 0;
    int b = 0;
    int c = 0;
    int d = 0;
    const char *cursor = host;

    if (strncmp(host, "localhost", 9) == 0) {
        return port_suffix_ok(host + 9);
    }
    if (!read_dec(&cursor, &a, 3) || *cursor++ != '.' ||
        !read_dec(&cursor, &b, 3) || *cursor++ != '.' ||
        !read_dec(&cursor, &c, 3) || *cursor++ != '.' ||
        !read_dec(&cursor, &d, 3)) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    if (!(a == 127 || a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31))) {
        return false;
    }
    return port_suffix_ok(cursor);
}

static bool origin_is_allowed(const char *origin)
{
    if (origin == NULL || origin[0] == '\0') {
        return false;
    }
    if (strcmp(origin, WHISTLE_CORS_ORIGIN) == 0) {
        return true;
    }
    if (strncmp(origin, "http://", 7) != 0) {
        return false;
    }
    return private_or_loopback_host(origin + 7);
}

static void add_cors(httpd_req_t *req, const char *origin)
{
    if (origin == NULL || origin[0] == '\0') {
        return;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", origin);
    httpd_resp_set_hdr(req, "Vary", "Origin");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    char origin[96] = {0};

    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK &&
        origin_is_allowed(origin)) {
        add_cors(req, origin);
    }
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    wifi_status_t status = {0};
    char ap_ssid[WIFI_SSID_MAX * 2 + 1];
    char sta_ssid[WIFI_SSID_MAX * 2 + 1];
    char json[384];

    if (wifi_get_status(&status) != ESP_OK) {
        return send_json(req, "500 Internal Server Error", "{\"error\":\"Failed to read Wi-Fi status\"}");
    }
    json_escape(status.ap_ssid, ap_ssid, sizeof(ap_ssid));
    json_escape(status.sta_ssid, sta_ssid, sizeof(sta_ssid));
    snprintf(json, sizeof(json),
             "{\"state\":\"%s\",\"provisioning\":%s,\"sta_connected\":%s,"
             "\"ap_ssid\":\"%s\",\"sta_ssid\":\"%s\",\"ip\":\"%s\"}",
             wifi_state_str(status.state),
             status.provisioning ? "true" : "false",
             status.sta_connected ? "true" : "false",
             ap_ssid, sta_ssid, status.ip);
    return send_json(req, NULL, json);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_scan_ap_t *aps = calloc(WIFI_SCAN_MAX, sizeof(*aps));
    char *json = malloc(2048);
    size_t count = 0;
    int off;
    esp_err_t err;

    if (aps == NULL || json == NULL) {
        free(aps);
        free(json);
        return send_json(req, "500 Internal Server Error", "{\"error\":\"Out of memory\"}");
    }

    ESP_LOGI(TAG, "Wi-Fi scan requested");
    err = wifi_scan(aps, WIFI_SCAN_MAX, &count);
    if (err != ESP_OK) {
        free(aps);
        free(json);
        return send_json(req, "500 Internal Server Error", "{\"error\":\"Wi-Fi scan failed\"}");
    }

    off = snprintf(json, 2048, "{\"aps\":[");
    for (size_t i = 0; i < count && off > 0 && off < 2048; i++) {
        char ssid[WIFI_SSID_MAX * 2 + 1];
        int n;

        json_escape(aps[i].ssid, ssid, sizeof(ssid));
        n = snprintf(json + off, (size_t)(2048 - off),
                     "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                     (i == 0) ? "" : ",", ssid, (int)aps[i].rssi,
                     aps[i].open ? "true" : "false");
        if (n < 0 || n >= 2048 - off) {
            free(aps);
            free(json);
            return send_json(req, "500 Internal Server Error", "{\"error\":\"Failed to format scan result\"}");
        }
        off += n;
    }
    if (off < 0 || off + 3 > 2048) {
        free(aps);
        free(json);
        return send_json(req, "500 Internal Server Error", "{\"error\":\"Failed to format scan result\"}");
    }
    json[off++] = ']';
    json[off++] = '}';
    json[off] = '\0';
    err = send_json(req, NULL, json);
    free(aps);
    free(json);
    return err;
}

static esp_err_t play_result(httpd_req_t *req, const char *name, esp_err_t err)
{
    char escaped[96];
    char body[160];

    if (err == ESP_OK) {
        json_escape(name, escaped, sizeof(escaped));
        snprintf(body, sizeof(body), "{\"accepted\":true,\"sound\":\"%s\"}", escaped);
        return send_json(req, "202 Accepted", body);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json(req, "409 Conflict", "{\"accepted\":false,\"error\":\"busy\"}");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json(req, "404 Not Found", "{\"error\":\"unknown sound\"}");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json(req, "400 Bad Request", "{\"error\":\"name is required\"}");
    }
    if (!whistle_dfplayer_connected()) {
        return send_json(req, "503 Service Unavailable", "{\"accepted\":false,\"error\":\"audio_not_ready\"}");
    }
    return send_json(req, "503 Service Unavailable", "{\"accepted\":false,\"error\":\"dfplayer unavailable\"}");
}

static esp_err_t horn_post_handler(httpd_req_t *req)
{
    char name[SOUND_NAME_MAX + 1];
    esp_err_t err;

    if (req->content_len > 0) {
        drain_body(req);
    }
    if (!whistle_dfplayer_connected()) {
        ESP_LOGW(TAG, "HTTP horn rejected: audio_not_ready");
        return send_json(req, "503 Service Unavailable", "{\"accepted\":false,\"error\":\"audio_not_ready\"}");
    }
    if (sound_selection_get(name, sizeof(name)) != ESP_OK) {
        return send_json(req, "503 Service Unavailable", "{\"accepted\":false,\"error\":\"no selected sound\"}");
    }
    ESP_LOGI(TAG, "HTTP horn request");
    err = whistle_request_play(name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP horn rejected: %s", esp_err_to_name(err));
    }
    return play_result(req, name, err);
}

static esp_err_t sounds_get_handler(httpd_req_t *req)
{
    size_t count = 0;
    const sound_catalog_entry_t *sounds = sound_catalog_entries(&count);
    const sound_catalog_entry_t *def = sound_catalog_default();
    int module_count = whistle_module_file_count();
    char *body = malloc(2048);
    int off;
    esp_err_t err;

    if (body == NULL) {
        return send_json(req, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
    }
    off = snprintf(body, 2048, "{\"source\":\"configured\",\"default_sound\":");
    if (off > 0 && off < 2048) {
        if (def == NULL) {
            off += snprintf(body + off, (size_t)(2048 - off), "null");
        } else {
            char escaped[SOUND_NAME_MAX * 2 + 1];
            json_escape(def->name, escaped, sizeof(escaped));
            off += snprintf(body + off, (size_t)(2048 - off), "\"%s\"", escaped);
        }
    }
    if (off > 0 && off < 2048) {
        off += snprintf(body + off, (size_t)(2048 - off), ",\"module_file_count\":");
    }
    if (off > 0 && off < 2048) {
        if (module_count < 0) {
            off += snprintf(body + off, (size_t)(2048 - off), "null");
        } else {
            off += snprintf(body + off, (size_t)(2048 - off), "%d", module_count);
        }
    }
    if (off > 0 && off < 2048) {
        off += snprintf(body + off, (size_t)(2048 - off), ",\"sounds\":[");
    }
    for (size_t i = 0; i < count && off > 0 && off < 2048; i++) {
        char name[SOUND_NAME_MAX * 2 + 1];
        char path[SOUND_PATH_MAX * 2 + 1];
        int n;

        json_escape(sounds[i].name, name, sizeof(name));
        json_escape(sounds[i].path, path, sizeof(path));
        n = snprintf(body + off, (size_t)(2048 - off),
                     "%s{\"name\":\"%s\",\"path\":\"%s\"}",
                     (i == 0) ? "" : ",", name, path);
        if (n < 0 || n >= 2048 - off) {
            free(body);
            return send_json(req, "500 Internal Server Error", "{\"error\":\"catalog too large\"}");
        }
        off += n;
    }
    if (off < 0 || off + 3 > 2048) {
        free(body);
        return send_json(req, "500 Internal Server Error", "{\"error\":\"catalog too large\"}");
    }
    body[off++] = ']';
    body[off++] = '}';
    body[off] = '\0';
    err = send_json(req, NULL, body);
    free(body);
    return err;
}

static esp_err_t sounds_play_post_handler(httpd_req_t *req)
{
    char raw[256];
    char name[48] = {0};
    esp_err_t err;

    if (recv_body(req, raw, sizeof(raw)) != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"error\":\"name is required\"}");
    }
    if (!json_get_string(raw, "name", name, sizeof(name)) || name[0] == '\0') {
        return send_json(req, "400 Bad Request", "{\"error\":\"name is required\"}");
    }
    if (!whistle_dfplayer_connected()) {
        ESP_LOGW(TAG, "HTTP play rejected: audio_not_ready");
        return send_json(req, "503 Service Unavailable", "{\"accepted\":false,\"error\":\"audio_not_ready\"}");
    }
    ESP_LOGI(TAG, "HTTP play request name=\"%s\"", name);
    err = whistle_request_play(name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP play rejected: %s", esp_err_to_name(err));
    }
    return play_result(req, name, err);
}

static esp_err_t playback_status_get_handler(httpd_req_t *req)
{
    whistle_status_t status;
    char current[128];
    char error[192];
    char body[512];

    whistle_get_status(&status);
    if (status.playing && status.current_sound[0] != '\0') {
        char escaped[80];
        json_escape(status.current_sound, escaped, sizeof(escaped));
        snprintf(current, sizeof(current), "\"%s\"", escaped);
    } else {
        snprintf(current, sizeof(current), "null");
    }
    if (status.last_error[0] != '\0') {
        char escaped[120];
        json_escape(status.last_error, escaped, sizeof(escaped));
        snprintf(error, sizeof(error), "\"%s\"", escaped);
    } else {
        snprintf(error, sizeof(error), "null");
    }
    snprintf(body, sizeof(body),
             "{\"playing\":%s,\"current_sound\":%s,\"last_error\":%s}",
             status.playing ? "true" : "false", current, error);
    return send_json(req, NULL, body);
}

static esp_err_t selection_get_handler(httpd_req_t *req)
{
    char selected[SOUND_NAME_MAX + 1];
    char selected_esc[SOUND_NAME_MAX * 2 + 1];
    char default_esc[SOUND_NAME_MAX * 2 + 1];
    char body[256];
    const sound_catalog_entry_t *def = sound_catalog_default();

    if (def == NULL || sound_selection_get(selected, sizeof(selected)) != ESP_OK) {
        return send_json(req, "503 Service Unavailable", "{\"error\":\"sound catalog unavailable\"}");
    }
    json_escape(selected, selected_esc, sizeof(selected_esc));
    json_escape(def->name, default_esc, sizeof(default_esc));
    snprintf(body, sizeof(body), "{\"selected\":\"%s\",\"default\":\"%s\"}", selected_esc, default_esc);
    return send_json(req, NULL, body);
}

static esp_err_t selection_post_handler(httpd_req_t *req)
{
    char raw[256];
    char name[SOUND_NAME_MAX + 1] = {0};
    char escaped[SOUND_NAME_MAX * 2 + 1];
    char body[128];
    esp_err_t err;

    if (recv_body(req, raw, sizeof(raw)) != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"error\":\"name is required\"}");
    }
    if (!json_get_string(raw, "name", name, sizeof(name)) || name[0] == '\0') {
        return send_json(req, "400 Bad Request", "{\"error\":\"name is required\"}");
    }
    err = sound_selection_set(name);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json(req, "404 Not Found", "{\"error\":\"unknown sound\"}");
    }
    if (err != ESP_OK) {
        return send_json(req, "503 Service Unavailable", "{\"error\":\"sound catalog unavailable\"}");
    }
    if (sound_selection_get(name, sizeof(name)) != ESP_OK) {
        return send_json(req, "503 Service Unavailable", "{\"error\":\"sound catalog unavailable\"}");
    }
    json_escape(name, escaped, sizeof(escaped));
    snprintf(body, sizeof(body), "{\"selected\":\"%s\"}", escaped);
    return send_json(req, NULL, body);
}

static esp_err_t api_options_handler(httpd_req_t *req)
{
    char origin[96] = {0};

    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK &&
        origin_is_allowed(origin)) {
        add_cors(req, origin);
    }
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (strncmp(req->uri, "/api/", 5) == 0) {
        return send_json(req, "404 Not Found", "{\"error\":\"not found\"}");
    }
    if (wifi_is_provisioning()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

esp_err_t whistle_http_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t console = {.uri = "/console", .method = HTTP_GET, .handler = console_get_handler};
    httpd_uri_t favicon = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler};
    httpd_uri_t wifi_page = {.uri = "/wifi", .method = HTTP_GET, .handler = setup_get_handler};
    httpd_uri_t connect = {.uri = "/wifi/connect", .method = HTTP_POST, .handler = connect_post_handler};
    httpd_uri_t forget = {.uri = "/wifi/forget", .method = HTTP_POST, .handler = forget_post_handler};
    httpd_uri_t status = {.uri = "/wifi/status", .method = HTTP_GET, .handler = wifi_status_get_handler};
    httpd_uri_t scan = {.uri = "/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get_handler};
    httpd_uri_t health = {.uri = "/api/health", .method = HTTP_GET, .handler = health_get_handler};
    httpd_uri_t horn = {.uri = "/api/horn", .method = HTTP_POST, .handler = horn_post_handler};
    httpd_uri_t horn_get = {.uri = "/api/horn", .method = HTTP_GET, .handler = post_only_get_handler};
    httpd_uri_t sounds = {.uri = "/api/sounds", .method = HTTP_GET, .handler = sounds_get_handler};
    httpd_uri_t play = {.uri = "/api/sounds/play", .method = HTTP_POST, .handler = sounds_play_post_handler};
    httpd_uri_t play_get = {.uri = "/api/sounds/play", .method = HTTP_GET, .handler = post_only_get_handler};
    httpd_uri_t playback = {.uri = "/api/status", .method = HTTP_GET, .handler = playback_status_get_handler};
    httpd_uri_t selection = {.uri = "/api/selection", .method = HTTP_GET, .handler = selection_get_handler};
    httpd_uri_t selection_post = {.uri = "/api/selection", .method = HTTP_POST, .handler = selection_post_handler};
    httpd_uri_t api_options = {.uri = "/api/*", .method = HTTP_OPTIONS, .handler = api_options_handler};
    esp_err_t err;

    config.lru_purge_enable = true;
    config.max_uri_handlers = 24;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &console);
    httpd_register_uri_handler(server, &favicon);
    httpd_register_uri_handler(server, &wifi_page);
    httpd_register_uri_handler(server, &connect);
    httpd_register_uri_handler(server, &forget);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &health);
    httpd_register_uri_handler(server, &horn);
    httpd_register_uri_handler(server, &horn_get);
    httpd_register_uri_handler(server, &sounds);
    httpd_register_uri_handler(server, &play);
    httpd_register_uri_handler(server, &play_get);
    httpd_register_uri_handler(server, &playback);
    httpd_register_uri_handler(server, &selection);
    httpd_register_uri_handler(server, &selection_post);
    httpd_register_uri_handler(server, &api_options);
    ESP_LOGI(TAG, "HTTP server listening on port %d", config.server_port);
    return ESP_OK;
}
