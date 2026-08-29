#include "train_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "train_control.h"

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
    "<footer class=\"footer\">Lionel O Gauge &middot; Wi-Fi Throttle</footer>"
    "</div>"
    "<script>"
    "const buttons=document.querySelectorAll('button[data-mode]');"
    "const statusEl=document.getElementById('status');"
    "function modeLabel(mode){return mode==='off'?'OFF':mode;}"
    "function setSelected(mode){"
    "buttons.forEach(function(b){"
    "b.classList.toggle('selected',b.dataset.mode===mode);"
    "});"
    "statusEl.textContent='Train speed: '+modeLabel(mode);"
    "}"
    "async function refreshStatus(){"
    "const r=await fetch('/status');"
    "if(!r.ok){return;}"
    "const j=await r.json();"
    "setSelected(j.mode);"
    "}"
    "buttons.forEach(function(b){"
    "b.addEventListener('click',async function(){"
    "const mode=b.dataset.mode;"
    "const r=await fetch('/speed?mode='+encodeURIComponent(mode));"
    "if(!r.ok){alert('Failed to set speed');return;}"
    "const j=await r.json();"
    "setSelected(j.mode);"
    "});"
    "});"
    "refreshStatus();"
    "</script>"
    "</body>"
    "</html>";

static esp_err_t train_index_get_handler(httpd_req_t *req)
{
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

    if (train_control_set_mode(mode) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode");
        return ESP_FAIL;
    }

    char response[64];
    int len = snprintf(response, sizeof(response),
                       "{\"mode\":\"%s\",\"percent\":%u}",
                       train_control_get_mode(),
                       (unsigned)train_control_get_percent());
    if (len < 0 || len >= (int)sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Response error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, len);
}

static esp_err_t train_status_get_handler(httpd_req_t *req)
{
    char response[64];
    int len = snprintf(response, sizeof(response),
                       "{\"mode\":\"%s\",\"percent\":%u}",
                       train_control_get_mode(),
                       (unsigned)train_control_get_percent());
    if (len < 0 || len >= (int)sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Response error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, len);
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

void train_http_register_uri_handlers(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Registering train control URI handlers");
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_index_compat));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_image));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_speed));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &train_favicon));
}
