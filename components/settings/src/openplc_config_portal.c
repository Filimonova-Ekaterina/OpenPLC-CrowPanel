#include "openplc_config_portal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "settings_config.h"
#include "settings_events.h"
#include "wifi_ctrl.h"

#define OPENPLC_PORTAL_FORM_LIMIT 512

static const char* TAG = "openplc_portal";
static httpd_handle_t s_server;
static char s_local_url[64];
static char s_connected_ssid[33];

static const char* PAGE_PREFIX =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OpenPLC OPC UA Setup</title><style>"
    ":root{color-scheme:dark;--bg:#080808;--panel:#181818;--muted:#a0a0a0;--line:#555;--accent:#2a96ff}"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;padding:24px;font:18px/1.45 sans-serif;color:#fff;"
    "background:var(--bg);display:flex;align-items:center;justify-content:center}"
    ".card{width:min(720px,100%);background:var(--panel);border:2px solid var(--line);border-radius:24px;padding:30px}"
    "h1{margin:0 0 10px;font-size:32px}.lead,.foot{color:var(--muted)}label{display:grid;gap:10px;margin:24px 0;font-weight:600}"
    "input{width:100%;border:2px solid #555;border-radius:16px;background:#080808;color:#fff;padding:16px;font:inherit}"
    "button{width:100%;border:0;border-radius:16px;padding:16px;font:inherit;font-weight:700;background:var(--accent);color:#fff}"
    ".status{padding:12px 16px;border:1px solid var(--accent);border-radius:14px;color:var(--muted)}"
    "</style></head><body><div class='card'><h1>OpenPLC OPC UA Setup</h1>"
    "<p class='lead'>Configure the OPC UA server used by this CrowPanel.</p>";

static const char* PAGE_FORM_PREFIX =
    "<form method='POST' action='/save'><label>OPC UA endpoint"
    "<input name='endpoint' maxlength='255' placeholder='opc.tcp://192.168.1.10:4840' value='";

static const char* PAGE_SUFFIX =
    "'></label><button type='submit'>Save and connect</button></form>"
    "<p class='foot'>The endpoint must start with opc.tcp://</p></div></body></html>";

static void url_decode(char* value)
{
    char* source = value;
    char* destination = value;
    while (*source != '\0') {
        if (*source == '+') {
            *destination++ = ' ';
            source++;
        } else if (*source == '%' && isxdigit((unsigned char)source[1]) && isxdigit((unsigned char)source[2])) {
            char digits[3] = {source[1], source[2], '\0'};
            *destination++ = (char)strtol(digits, NULL, 16);
            source += 3;
        } else {
            *destination++ = *source++;
        }
    }
    *destination = '\0';
}

static void html_escape(const char* source, char* destination, size_t destination_size)
{
    size_t offset = 0;
    while (*source != '\0' && offset + 1 < destination_size) {
        const char* replacement = NULL;
        if (*source == '&') replacement = "&amp;";
        else if (*source == '<') replacement = "&lt;";
        else if (*source == '>') replacement = "&gt;";
        else if (*source == '\'') replacement = "&#39;";
        else if (*source == '"') replacement = "&quot;";

        if (replacement != NULL) {
            size_t length = strlen(replacement);
            if (offset + length >= destination_size) break;
            memcpy(destination + offset, replacement, length);
            offset += length;
        } else {
            destination[offset++] = *source;
        }
        source++;
    }
    destination[offset] = '\0';
}

static esp_err_t root_get_handler(httpd_req_t* request)
{
    char endpoint[SETTINGS_OPCUA_ENDPOINT_LENGTH] = {0};
    const size_t escaped_endpoint_size = SETTINGS_OPCUA_ENDPOINT_LENGTH * 6;
    char* escaped_endpoint = calloc(1, escaped_endpoint_size);
    if (escaped_endpoint == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    settings_config_load_opcua_endpoint(endpoint, sizeof(endpoint), CONFIG_OPCUA_SERVER_ENDPOINT);
    html_escape(endpoint, escaped_endpoint, escaped_endpoint_size);

    httpd_resp_set_type(request, "text/html");
    httpd_resp_sendstr_chunk(request, PAGE_PREFIX);
    char escaped_ssid[sizeof(s_connected_ssid) * 6] = {0};
    char escaped_local_url[sizeof(s_local_url) * 6] = {0};
    html_escape(s_connected_ssid, escaped_ssid, sizeof(escaped_ssid));
    html_escape(s_local_url, escaped_local_url, sizeof(escaped_local_url));
    char connection_status[768];
    snprintf(connection_status, sizeof(connection_status),
             "<p class='status'>Connected to: %s<br>Setup page: %s</p>", escaped_ssid, escaped_local_url);
    httpd_resp_sendstr_chunk(request, connection_status);
    httpd_resp_sendstr_chunk(request, PAGE_FORM_PREFIX);
    httpd_resp_sendstr_chunk(request, escaped_endpoint);
    httpd_resp_sendstr_chunk(request, PAGE_SUFFIX);
    free(escaped_endpoint);
    return httpd_resp_sendstr_chunk(request, NULL);
}

static esp_err_t save_post_handler(httpd_req_t* request)
{
    if (request->content_len <= 0 || request->content_len >= OPENPLC_PORTAL_FORM_LIMIT) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form size");
    }

    char body[OPENPLC_PORTAL_FORM_LIMIT] = {0};
    size_t total_received = 0;
    while (total_received < request->content_len) {
        int received = httpd_req_recv(request, body + total_received, request->content_len - total_received);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Cannot read form");
        }
        total_received += (size_t)received;
    }
    body[total_received] = '\0';

    const char* prefix = "endpoint=";
    if (strncmp(body, prefix, strlen(prefix)) != 0) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Endpoint is missing");
    }
    char* encoded_endpoint = body + strlen(prefix);
    char* separator = strchr(encoded_endpoint, '&');
    if (separator != NULL) *separator = '\0';
    if (strlen(encoded_endpoint) >= SETTINGS_OPCUA_ENDPOINT_LENGTH) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Endpoint is too long");
    }
    char endpoint[SETTINGS_OPCUA_ENDPOINT_LENGTH] = {0};
    strlcpy(endpoint, encoded_endpoint, sizeof(endpoint));
    url_decode(endpoint);
    if (strncmp(endpoint, "opc.tcp://", 10) != 0) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid OPC UA endpoint");
    }

    esp_err_t result = settings_config_save_opcua_endpoint(endpoint);
    if (result == ESP_OK) {
        app_settings_uart_opcua_endpoint_data_t payload = {0};
        strlcpy(payload.endpoint, endpoint, sizeof(payload.endpoint));
        payload.length = (uint16_t)strlen(payload.endpoint);
        result = esp_event_post(APP_SETTINGS_UART_EVENTS, APP_SETTINGS_UART_EVENT_RECEIVED_OPCUA_ENDPOINT,
                                &payload, sizeof(payload), portMAX_DELAY);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot apply endpoint from portal: %s", esp_err_to_name(result));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot save endpoint");
    }

    httpd_resp_set_type(request, "text/html");
    return httpd_resp_sendstr(request,
        "<!doctype html><html><body style='background:#080808;color:white;font:22px sans-serif;text-align:center;padding:60px'>"
        "<h1 style='color:#2a96ff'>Saved</h1><p>The OPC UA client is reconnecting.</p></body></html>");
}

esp_err_t openplc_config_portal_start(void)
{
    if (s_server != NULL) return ESP_OK;
    if (! wifi_ctrl_is_connected()) return ESP_ERR_INVALID_STATE;

    esp_netif_t* interface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (interface == NULL || esp_netif_get_ip_info(interface, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_local_url, sizeof(s_local_url), "http://" IPSTR, IP2STR(&ip_info.ip));
    wifi_ap_record_t access_point = {0};
    if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        strlcpy(s_connected_ssid, (const char*)access_point.ssid, sizeof(s_connected_ssid));
    } else {
        strlcpy(s_connected_ssid, "Wi-Fi", sizeof(s_connected_ssid));
    }

    httpd_config_t configuration = HTTPD_DEFAULT_CONFIG();
    configuration.max_uri_handlers = 4;
    /* The request handler reads NVS and renders escaped HTML. The default
     * 4 KiB HTTP task stack is not sufficient on ESP-IDF 6/open62541 builds. */
    configuration.stack_size = 8192;
    esp_err_t result = httpd_start(&s_server, &configuration);
    if (result != ESP_OK) {
        s_server = NULL;
        return result;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post_handler};
    result = httpd_register_uri_handler(s_server, &root);
    if (result == ESP_OK) result = httpd_register_uri_handler(s_server, &save);
    if (result != ESP_OK) {
        openplc_config_portal_stop();
        return result;
    }
    ESP_LOGI(TAG, "Portal started at %s", s_local_url);
    return ESP_OK;
}

void openplc_config_portal_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    s_local_url[0] = '\0';
    s_connected_ssid[0] = '\0';
}

bool openplc_config_portal_is_running(void)
{
    return s_server != NULL;
}

const char* openplc_config_portal_get_local_url(void)
{
    return s_local_url[0] != '\0' ? s_local_url : NULL;
}

const char* openplc_config_portal_get_connected_ssid(void)
{
    return s_connected_ssid[0] != '\0' ? s_connected_ssid : NULL;
}
