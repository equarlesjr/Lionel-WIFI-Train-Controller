#include "train_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "train_control.h"
#include "wifi.h"

static const char *TAG = "train_http";

extern const uint8_t lionel_trains_jpg_start[] asm("_binary_lionel_trains_jpg_start");
extern const uint8_t lionel_trains_jpg_end[] asm("_binary_lionel_trains_jpg_end");

static const char TRAIN_INDEX_HTML[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Train Controller</title>"
    "<style>"
    ":root{"
    "--cream:#f4eedc;--red:#c41e3a;--blue:#1e4a7a;--silver:#d4d8dc;"
    "--gold:#f0c419;--ink:#1f1f1f;--paper:#ebe3cf;"
    "}"
    "*{box-sizing:border-box;}"
    "body{"
    "margin:0;min-height:100vh;padding:20px 14px 28px;"
    "font-family:Georgia,'Times New Roman',serif;"
    "color:var(--ink);"
    "background:"
    "repeating-linear-gradient(90deg,rgba(0,0,0,.03) 0 2px,transparent 2px 18px),"
    "linear-gradient(180deg,#efe6d2 0%,var(--cream) 45%,#eadfc8 100%);"
    "}"
    ".card{"
    "max-width:520px;margin:0 auto;"
    "background:var(--paper);"
    "border:4px solid var(--blue);"
    "border-radius:14px;"
    "box-shadow:0 10px 28px rgba(30,74,122,.22);"
    "overflow:hidden;"
    "}"
    ".stripe{height:10px;background:linear-gradient(90deg,var(--red) 0 55%,var(--gold) 55% 100%);}"
    ".masthead{padding:18px 18px 8px;text-align:center;background:linear-gradient(180deg,#fff8ea 0%,var(--paper) 100%);}"
    ".brand{"
    "margin:0;font-size:1.65rem;letter-spacing:.08em;"
    "font-family:'Arial Black',Impact,sans-serif;"
    "color:var(--blue);text-transform:uppercase;"
    "text-shadow:1px 1px 0 #fff,2px 2px 0 rgba(30,74,122,.15);"
    "}"
    ".tagline{"
    "margin:8px 0 0;font-size:1.05rem;font-style:italic;color:var(--red);"
    "font-family:'Brush Script MT','Segoe Script',cursive;"
    "}"
    ".hero{padding:0 16px 12px;text-align:center;}"
    ".hero-frame{"
    "display:inline-block;padding:8px;"
    "background:#fff;border:3px solid var(--blue);border-radius:10px;"
    "box-shadow:inset 0 0 0 2px var(--gold),0 6px 16px rgba(0,0,0,.12);"
    "}"
    ".hero img{display:block;width:100%;max-width:420px;height:auto;border-radius:4px;}"
    ".controls{padding:6px 16px 20px;}"
    ".controls-label{"
    "margin:0 0 12px;text-align:center;font-size:.95rem;letter-spacing:.12em;"
    "text-transform:uppercase;color:var(--blue);font-weight:bold;"
    "font-family:Arial,Helvetica,sans-serif;"
    "}"
    ".source{"
    "margin:0 0 12px;padding:8px 12px;text-align:center;"
    "font-size:.85rem;letter-spacing:.14em;text-transform:uppercase;"
    "font-weight:bold;color:var(--blue);"
    "background:#fff;border:2px solid var(--blue);border-radius:999px;"
    "font-family:Arial,Helvetica,sans-serif;"
    "}"
    ".source.source-off{color:#5c6672;border-color:#8a9199;}"
    ".source.source-knob{color:var(--red);border-color:var(--red);}"
    ".buttons{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;}"
    "button{"
    "padding:22px 8px;font-size:1.15rem;font-weight:bold;cursor:pointer;"
    "font-family:Arial,Helvetica,sans-serif;"
    "border:2px solid #5a6470;border-radius:10px;"
    "background:linear-gradient(180deg,#f8f9fb 0%,var(--silver) 100%);"
    "color:var(--ink);"
    "box-shadow:0 3px 0 #8a9199,inset 0 1px 0 #fff;"
    "transition:transform .08s ease,box-shadow .08s ease;"
    "}"
    "button:active{transform:translateY(2px);box-shadow:0 1px 0 #8a9199,inset 0 1px 0 #fff;}"
    "button[data-mode=\"off\"]{grid-column:span 2;}"
    "button.selected{"
    "background:linear-gradient(180deg,#e34a5f 0%,var(--red) 100%);"
    "color:#fff;border-color:#8b1428;"
    "box-shadow:0 3px 0 #6f1020,inset 0 0 0 2px var(--gold);"
    "}"
    "button:disabled{opacity:.45;cursor:not-allowed;box-shadow:none;transform:none;}"
    ".status{"
    "margin:18px 0 0;padding:12px 14px;text-align:center;"
    "font-size:1.15rem;font-weight:bold;"
    "background:#fff;border:2px dashed var(--blue);border-radius:999px;"
    "font-family:Arial,Helvetica,sans-serif;"
    "}"
    ".footer{"
    "padding:10px 16px 14px;text-align:center;font-size:.78rem;letter-spacing:.06em;"
    "text-transform:uppercase;color:#5c6672;"
    "border-top:2px solid rgba(30,74,122,.15);"
    "font-family:Arial,Helvetica,sans-serif;"
    "}"
    ".footer a{color:var(--blue);text-decoration:none;}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"card\">"
    "<div class=\"stripe\"></div>"
    "<header class=\"masthead\">"
    "<h1 class=\"brand\">Train Controller</h1>"
    "<p class=\"tagline\">Santa Fe &mdash; red streak of the prairies</p>"
    "</header>"
    "<figure class=\"hero\">"
    "<div class=\"hero-frame\">"
    "<img src=\"/train.jpg\" alt=\"Lionel Santa Fe diesel locomotive\">"
    "</div>"
    "</figure>"
    "<section class=\"controls\">"
    "<p class=\"controls-label\">Throttle</p>"
    "<p id=\"source\" class=\"source\">Control: --</p>"
    "<div class=\"buttons\">"
    "<button type=\"button\" data-mode=\"off\">OFF</button>"
    "<button type=\"button\" data-mode=\"1\">1</button>"
    "<button type=\"button\" data-mode=\"2\">2</button>"
    "<button type=\"button\" data-mode=\"3\">3</button>"
    "<button type=\"button\" data-mode=\"4\">4</button>"
    "<button type=\"button\" data-mode=\"5\">5</button>"
    "<button type=\"button\" data-mode=\"6\">6</button>"
    "</div>"
    "<p id=\"status\" class=\"status\">Train speed: --</p>"
    "</section>"
    "<footer class=\"footer\">Lionel O Gauge &middot; Wi-Fi Throttle &middot; <a href=\"/wifi\">Wi-Fi setup</a></footer>"
    "</div>"
    "<script>"
    "const buttons=document.querySelectorAll('button[data-mode]');"
    "const statusEl=document.getElementById('status');"
    "const sourceEl=document.getElementById('source');"
    "function modeLabel(mode){return mode==='off'?'OFF':mode;}"
    "function sourceLabel(src){"
    "if(src==='web')return 'WEB';"
    "if(src==='knob')return 'KNOB';"
    "return 'OFF';"
    "}"
    "function applyStatus(j){"
    "const src=j.control_source||'off';"
    "sourceEl.textContent='Control: '+sourceLabel(src);"
    "sourceEl.className='source source-'+src;"
    "const locked=src!=='web';"
    "buttons.forEach(function(b){"
    "b.disabled=locked;"
    "b.classList.toggle('selected',b.dataset.mode===j.mode);"
    "});"
    "statusEl.textContent='Train speed: '+modeLabel(j.mode);"
    "}"
    "async function refreshStatus(){"
    "const r=await fetch('/status');"
    "if(!r.ok){return;}"
    "applyStatus(await r.json());"
    "}"
    "buttons.forEach(function(b){"
    "b.addEventListener('click',async function(){"
    "if(b.disabled){return;}"
    "const r=await fetch('/speed?mode='+encodeURIComponent(b.dataset.mode));"
    "if(!r.ok){await refreshStatus();return;}"
    "applyStatus(await r.json());"
    "});"
    "});"
    "refreshStatus();"
    "setInterval(refreshStatus,400);"
    "</script>"
    "</body>"
    "</html>";

static esp_err_t wifi_page_get_handler(httpd_req_t *req);

static esp_err_t train_index_get_handler(httpd_req_t *req)
{
    /* Captive-portal browsers open /. Serving the throttle page here
     * during setup looks like "site can't be reached" after JS/image load.
     */
    if (wifi_is_provisioning()) {
        ESP_LOGI(TAG, "Provisioning: serving Wi-Fi setup for %s", req->uri);
        return wifi_page_get_handler(req);
    }

    ESP_LOGI(TAG, "Serving train control page for %s", req->uri);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, TRAIN_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t train_image_get_handler(httpd_req_t *req)
{
    const size_t len = lionel_trains_jpg_end - lionel_trains_jpg_start;

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    return httpd_resp_send(req, (const char *)lionel_trains_jpg_start, len);
}

static esp_err_t train_favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static int format_status_json(char *buf, size_t buflen)
{
    train_control_status_t status;
    train_control_get_status(&status);
    return snprintf(buf, buflen,
                    "{\"control_source\":\"%s\",\"mode\":\"%s\",\"percent\":%u}",
                    status.source_name, status.mode, (unsigned)status.percent);
}

static esp_err_t send_status_json(httpd_req_t *req)
{
    char response[96];
    int len = format_status_json(response, sizeof(response));
    if (len < 0 || len >= (int)sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Response error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, len);
}

static esp_err_t train_speed_get_handler(httpd_req_t *req)
{
    char query[32];
    char mode[16] = {0};
    size_t query_len = httpd_req_get_url_query_len(req);

    if (query_len == 0 || query_len >= sizeof(query)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing mode parameter");
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query");
        return ESP_FAIL;
    }

    if (httpd_query_key_value(query, "mode", mode, sizeof(mode)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing mode parameter");
        return ESP_FAIL;
    }

    esp_err_t err = train_control_set_mode(mode);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Control source is not WEB");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode");
        return ESP_FAIL;
    }

    return send_status_json(req);
}

static esp_err_t train_status_get_handler(httpd_req_t *req)
{
    return send_status_json(req);
}

static const httpd_uri_t train_index = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = train_index_get_handler,
};

static const httpd_uri_t train_index_compat = {
    .uri = "/hello",
    .method = HTTP_GET,
    .handler = train_index_get_handler,
};

static const httpd_uri_t train_image = {
    .uri = "/train.jpg",
    .method = HTTP_GET,
    .handler = train_image_get_handler,
};

static const httpd_uri_t train_favicon = {
    .uri = "/favicon.ico",
    .method = HTTP_GET,
    .handler = train_favicon_get_handler,
};

static const httpd_uri_t train_speed = {
    .uri = "/speed",
    .method = HTTP_GET,
    .handler = train_speed_get_handler,
};

static const httpd_uri_t train_status = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = train_status_get_handler,
};

extern const uint8_t wifi_setup_html_start[] asm("_binary_wifi_setup_html_start");
extern const uint8_t wifi_setup_html_end[] asm("_binary_wifi_setup_html_end");

static esp_err_t http_send_json(httpd_req_t *req, const char *status, const char *json)
{
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    char json[128];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", message);
    return http_send_json(req, status, json);
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 2 < out_len; i++) {
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

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        out[0] = '\0';
        return false;
    }
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
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

static esp_err_t format_wifi_json(char *buf, size_t buf_size)
{
    wifi_status_t st;
    if (wifi_get_status(&st) != ESP_OK) {
        snprintf(buf, buf_size, "{\"error\":\"Failed to read Wi-Fi status\"}");
        return ESP_FAIL;
    }

    char ap_ssid[WIFI_SSID_MAX * 2 + 1];
    char sta_ssid[WIFI_SSID_MAX * 2 + 1];
    json_escape(st.ap_ssid, ap_ssid, sizeof(ap_ssid));
    json_escape(st.sta_ssid, sta_ssid, sizeof(sta_ssid));
    snprintf(buf, buf_size,
             "{\"state\":\"%s\",\"provisioning\":%s,\"sta_connected\":%s,"
             "\"ap_ssid\":\"%s\",\"sta_ssid\":\"%s\",\"ip\":\"%s\"}",
             wifi_state_str(st.state),
             st.provisioning ? "true" : "false",
             st.sta_connected ? "true" : "false",
             ap_ssid, sta_ssid, st.ip);
    return ESP_OK;
}

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    int remaining = req->content_len;
    int off = 0;
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

static esp_err_t wifi_page_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving Wi-Fi setup for %s", req->uri);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)wifi_setup_html_start,
                           wifi_setup_html_end - wifi_setup_html_start);
}

static const httpd_uri_t wifi_page = {
    .uri = "/wifi",
    .method = HTTP_GET,
    .handler = wifi_page_get_handler,
};

static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    char json[256];
    if (format_wifi_json(json, sizeof(json)) != ESP_OK) {
        return http_send_json_error(req, "500 Internal Server Error", "Failed to read Wi-Fi status");
    }
    return http_send_json(req, NULL, json);
}

static const httpd_uri_t wifi_status = {
    .uri = "/wifi/status",
    .method = HTTP_GET,
    .handler = wifi_status_get_handler,
};

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_scan_ap_t *aps = calloc(WIFI_SCAN_MAX, sizeof(*aps));
    char *json = malloc(2048);
    if (!aps || !json) {
        free(aps);
        free(json);
        return http_send_json_error(req, "500 Internal Server Error", "Out of memory");
    }

    ESP_LOGI(TAG, "Wi-Fi scan requested");
    size_t count = 0;
    esp_err_t err = wifi_scan(aps, WIFI_SCAN_MAX, &count);
    if (err != ESP_OK) {
        free(aps);
        free(json);
        return http_send_json_error(req, "500 Internal Server Error", "Wi-Fi scan failed");
    }

    int off = snprintf(json, 2048, "{\"aps\":[");
    for (size_t i = 0; i < count && off > 0 && off < 2048; i++) {
        char ssid[WIFI_SSID_MAX * 2 + 1];
        json_escape(aps[i].ssid, ssid, sizeof(ssid));
        int n = snprintf(json + off, (size_t)(2048 - off),
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                         (i == 0) ? "" : ",", ssid, (int)aps[i].rssi,
                         aps[i].open ? "true" : "false");
        if (n < 0 || n >= 2048 - off) {
            free(aps);
            free(json);
            return http_send_json_error(req, "500 Internal Server Error", "Failed to format scan result");
        }
        off += n;
    }
    if (off + 3 > 2048) {
        free(aps);
        free(json);
        return http_send_json_error(req, "500 Internal Server Error", "Failed to format scan result");
    }
    json[off++] = ']';
    json[off++] = '}';
    json[off] = '\0';
    err = http_send_json(req, NULL, json);
    free(aps);
    free(json);
    return err;
}

static const httpd_uri_t wifi_scan_uri = {
    .uri = "/wifi/scan",
    .method = HTTP_GET,
    .handler = wifi_scan_get_handler,
};

static esp_err_t wifi_connect_post_handler(httpd_req_t *req)
{
    char body[256];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return http_send_json_error(req, "400 Bad Request", "Invalid Wi-Fi request");
    }

    char ssid[WIFI_SSID_MAX + 1] = {0};
    char password[WIFI_PASS_MAX + 1] = {0};
    json_get_string(body, "ssid", ssid, sizeof(ssid));
    json_get_string(body, "password", password, sizeof(password));
    if (ssid[0] == '\0') {
        return http_send_json(req, "400 Bad Request", "{\"error\":\"ssid is required\"}");
    }

    if (wifi_save_and_connect(ssid, password) != ESP_OK) {
        return http_send_json_error(req, "500 Internal Server Error", "Failed to save Wi-Fi credentials");
    }
    /* Reply first so the phone sees acknowledgement before we start joining. */
    esp_err_t err = http_send_json(req, NULL, "{\"ok\":true}");
    (void)wifi_connect_saved();
    return err;
}

static const httpd_uri_t wifi_connect = {
    .uri = "/wifi/connect",
    .method = HTTP_POST,
    .handler = wifi_connect_post_handler,
};

static esp_err_t wifi_forget_post_handler(httpd_req_t *req)
{
    char dummy[8];
    if (req->content_len > 0) {
        (void)recv_body(req, dummy, sizeof(dummy));
    }
    if (wifi_forget() != ESP_OK) {
        return http_send_json_error(req, "500 Internal Server Error", "Failed to forget Wi-Fi");
    }
    return http_send_json(req, NULL, "{\"ok\":true}");
}

static const httpd_uri_t wifi_forget_uri = {
    .uri = "/wifi/forget",
    .method = HTTP_POST,
    .handler = wifi_forget_post_handler,
};

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (wifi_is_provisioning()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

void train_http_register_uri_handlers(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Registering train control URI handlers");
    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_index_compat));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_image));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_speed));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_favicon));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_scan_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_connect));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_forget_uri));
}
