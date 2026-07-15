#include "config_portal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "esp_netif_net_stack.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "settings_config.h"
#include "settings_events.h"
#include "uart_handler.h"
#include "credentials.h"
#include "wifi_ctrl.h"

static const char *TAG = "config_portal";

static const char *AP_SSID = "ESP32-P4-Config";
static const char *AP_PASSWORD = "configure123";
static const char *AP_DEFAULT_URL = "http://192.168.4.1";

enum {
    DNS_PORT = 53,
    HTTP_PORT = 80,
    MAX_FORM_BODY = 2048,
    MAX_API_KEY_LEN = 1024,
    MAX_PORTAL_SCAN_RESULTS = 20,
    MAX_PORTAL_SCAN_RECORDS = 40,
};

typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} portal_scan_result_t;

static httpd_handle_t s_http_server = NULL;
static TaskHandle_t s_dns_task = NULL;
static int s_dns_sock = -1;
static bool s_dns_running = false;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_portal_running = false;
static bool s_runtime_mode = false;
static config_portal_access_mode_t s_access_mode = CONFIG_PORTAL_ACCESS_NONE;
static char s_ap_url[32] = "http://192.168.4.1";
static char s_local_url[32] = {0};
static char s_connected_ssid[33] = {0};
static char s_current_wifi_ssid[33] = {0};
static char s_current_wifi_pass[65] = {0};
static SemaphoreHandle_t s_scan_mutex = NULL;
static portal_scan_result_t s_scan_results[MAX_PORTAL_SCAN_RESULTS];
static size_t s_scan_result_count = 0;
static char *s_cached_html_page = NULL;
static bool s_html_cache_valid = false;
static bool s_data_saved = false;
static bool s_portal_starting = false;

static const char *kWifiSetupPageTemplate =
    "<!doctype html>"
    "<html lang='en'>"
    "<head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WiFi Setup</title>"
    "<style>"
    ":root{color-scheme:dark;--bg:#0f1218;--panel:#171c25;--muted:#98a2b3;--line:#273142;--accent:#3fb983;--accent2:#5ba6ff;}"
    "*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;padding:24px;font:16px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;color:#fff;"
    "background:radial-gradient(circle at top,#1a2330,#0b0d12 68%%);display:flex;align-items:center;justify-content:center}"
    ".card{width:min(700px,100%%);background:rgba(23,28,37,.96);border:1px solid var(--line);border-radius:24px;padding:28px;"
    "box-shadow:0 24px 60px rgba(0,0,0,.35)}"
    "h1{margin:0 0 10px;font-size:32px}"
    ".lead{margin:0 0 18px;color:var(--muted)}"
    ".ap-info{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin:0 0 20px}"
    ".ap-info div{padding:14px 16px;border-radius:16px;background:#11161f;border:1px solid var(--line)}"
    ".ap-info small{display:block;margin-bottom:6px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;font-size:11px}"
    ".scan-section{margin:0 0 22px}"
    ".scan-section h2{margin:0 0 12px;font-size:20px}"
    ".scan-section p{margin:0 0 12px;color:var(--muted);font-size:14px}"
    ".selected-note{padding:12px 14px;border-radius:14px;background:#10151d;border:1px solid var(--line);color:#dbe4f3;font-size:14px;margin-bottom:12px}"
    ".wifi-list{display:grid;gap:10px;max-height:280px;overflow:auto;padding-right:4px;margin-bottom:16px}"
    ".wifi-item{width:100%%;display:flex;align-items:center;justify-content:space-between;gap:14px;padding:14px 16px;border-radius:16px;"
    "border:1px solid #2f3b4f;background:#10151d;color:#fff;text-align:left;cursor:pointer}"
    ".wifi-item:hover{border-color:#4a6287}"
    ".wifi-item.selected{border-color:var(--accent2);box-shadow:0 0 0 1px rgba(91,166,255,.32) inset}"
    ".wifi-name{display:block;font-weight:700}"
    ".wifi-meta{display:flex;align-items:center;justify-content:flex-end;gap:10px;flex-wrap:wrap;color:var(--muted);font-size:13px}"
    ".wifi-tag{display:inline-flex;align-items:center;padding:5px 8px;border-radius:999px;background:rgba(63,185,131,.16);color:#d8ffe8;font-size:12px}"
    ".wifi-tag-open{background:rgba(91,166,255,.16);color:#dbe9ff}"
    ".scan-empty{padding:16px;border-radius:16px;background:#10151d;border:1px dashed #324055;color:var(--muted)}"
    "form{display:grid;gap:20px}"
    "label{display:grid;gap:8px;font-weight:600}"
    "input{width:100%%;border:1px solid #324055;border-radius:14px;background:#0f141c;color:#fff;padding:14px 16px;font:inherit}"
    ".hint{margin-top:-6px;color:var(--muted);font-size:13px}"
    "button{border:0;border-radius:14px;padding:14px 18px;font:inherit;font-weight:700;cursor:pointer;background:linear-gradient(135deg,var(--accent),var(--accent2));color:#081018}"
    ".refresh-btn{background:var(--line);color:#fff;padding:8px 16px;border-radius:8px;margin-left:12px;font-size:13px}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h1>WiFi Setup</h1>"
    "<p class='lead'>Connect to the board Wi-Fi and select your network.</p>"
    "<div class='ap-info'>"
    "<div><small>Access Point</small><strong>%s</strong></div>"
    "<div><small>Password</small><strong>%s</strong></div>"
    "<div><small>Setup Page</small><strong>%s</strong></div>"
    "</div>"
    "<div class='scan-section'>"
    "<h2>Available Networks</h2>"
    "<p>Tap a network to select it. <button class='refresh-btn' onclick='location.reload()'>Refresh</button></p>"
    "<div class='selected-note' id='selectedNote'>Select a network or enter SSID manually.</div>"
    "<div class='wifi-list'>%s</div>"
    "</div>"
    "<form method='POST' action='/save'>"
    "<label>WiFi SSID"
    "<input type='text' name='wifi_ssid' id='wifi_ssid' maxlength='32' required value='%s'>"
    "</label>"
    "<label>WiFi Password"
    "<div style='position: relative;'>"
    "<input type='password' name='wifi_pass' id='wifi_pass' maxlength='64' value='%s'>"
    "<button type='button' onclick='togglePassword()' style='position: absolute; right: 10px; top: 50%%; transform: translateY(-50%%); background: none; border: none; color: var(--accent); cursor: pointer;'>Show</button>"
    "</div>"
    "</label>"
    "<div class='hint'>Leave password empty for open networks.</div>"
    "<button type='submit'>Save and Connect</button>"
    "</form>"
    "</div>"
    "<script>"
    "function togglePassword(){"
    "var x=document.getElementById('wifi_pass');"
    "if(x.type==='password'){x.type='text';}else{x.type='password';}"
    "}"
    "function selectNetwork(btn){"
    "var ssid=btn.getAttribute('data-ssid')||'';"
    "var secure=btn.getAttribute('data-secure')==='1';"
    "var ssidInput=document.getElementById('wifi_ssid');"
    "var passInput=document.getElementById('wifi_pass');"
    "var note=document.getElementById('selectedNote');"
    "var selected=document.querySelectorAll('.wifi-item.selected');"
    "for(var i=0;i<selected.length;i++){selected[i].classList.remove('selected');}"
    "btn.classList.add('selected');"
    "ssidInput.value=ssid;"
    "if(secure){"
    "note.textContent='Selected '+ssid+'. Enter password below.';"
    "}else{"
    "passInput.value='';"
    "note.textContent='Selected '+ssid+'. This network is open.';"
    "}"
    "}"
    "</script>"
    "</body>"
    "</html>";

static const char *kConfigPageTemplate =
    "<!doctype html>"
    "<html lang='en'>"
    "<head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Configuration</title>"
    "<style>"
    ":root{color-scheme:dark;--bg:#0f1218;--panel:#171c25;--muted:#98a2b3;--line:#273142;--accent:#3fb983;--accent2:#5ba6ff;}"
    "*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;padding:24px;font:16px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;color:#fff;"
    "background:radial-gradient(circle at top,#1a2330,#0b0d12 68%%);display:flex;align-items:center;justify-content:center}"
    ".card{width:min(700px,100%%);background:rgba(23,28,37,.96);border:1px solid var(--line);border-radius:24px;padding:28px;"
    "box-shadow:0 24px 60px rgba(0,0,0,.35)}"
    "h1{margin:0 0 10px;font-size:32px}"
    ".lead{margin:0 0 18px;color:var(--muted)}"
    ".wifi-status{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:0 0 20px;align-items:center}"
    ".wifi-status div{padding:14px 16px;border-radius:16px;background:#11161f;border:1px solid var(--accent);display:flex;flex-direction:column;justify-content:center}"
    ".wifi-status small{display:block;margin-bottom:6px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;font-size:11px}"
    ".wifi-status strong{font-size:15px}"
    ".wifi-status .connected{color:#3fb983}"
    ".wifi-status .setup-url{color:var(--muted)}"
    "form{display:grid;gap:20px}"
    "label{display:grid;gap:8px;font-weight:600}"
    "input,textarea{width:100%%;border:1px solid #324055;border-radius:14px;background:#0f141c;color:#fff;padding:14px 16px;font:inherit}"
    "textarea{min-height:100px;resize:vertical}"
    ".hint{margin-top:-6px;color:var(--muted);font-size:13px}"
    "button{border:0;border-radius:14px;padding:14px 18px;font:inherit;font-weight:700;cursor:pointer;background:linear-gradient(135deg,var(--accent),var(--accent2));color:#081018}"
    ".foot{margin-top:20px;color:var(--muted);font-size:13px;text-align:center}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h1>Configuration</h1>"
    "<p class='lead'>Configure OpenWeatherMap API Key and Google Calendar URL.</p>"
    "<div class='wifi-status'>"
    "<div><small>Connected to</small><strong class='connected'>%s</strong></div>"
    "<div><small>Setup Page</small><strong class='setup-url'>%s</strong></div>"
    "</div>"
    "<form method='POST' action='/save'>"
    "<label>OpenWeatherMap API Key"
    "<input type='text' name='weather_key' maxlength='1024' placeholder='Enter your OpenWeatherMap API key' value='%s'>"
    "<div class='hint'>Required for weather display</div>"
    "</label>"
    "<label>Google Calendar ICS URL"
    "<textarea name='calendar_key' maxlength='2048' placeholder='https://calendar.google.com/calendar/ical/.../basic.ics'>%s</textarea>"
    "<div class='hint'>Required for calendar events</div>"
    "</label>"
    "<button type='submit'>Save Configuration</button>"
    "</form>"
    "<div class='foot'>Configuration will be saved to device memory.</div>"
    "</div>"
    "</body>"
    "</html>";

static const char *kSuccessPage =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    "<style>"
    "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px;"
    "background:#0b0d12;color:#fff;font:16px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
    ".card{max-width:520px;background:#171c25;border:1px solid #273142;border-radius:24px;padding:28px;box-shadow:0 24px 60px rgba(0,0,0,.35);text-align:center}"
    "h1{margin:0 0 10px;font-size:28px;color:#3fb983}"
    "p{margin:0;color:#98a2b3}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h1>Saved!</h1>"
    "<p>Your settings have been saved successfully.</p>"
    "</div>"
    "</body></html>";

static char *html_escape_dup(const char *src);
static bool form_get_value(const char *body, const char *key, char *out, size_t out_size);
static esp_err_t handle_root_get(httpd_req_t *req);
static esp_err_t handle_favicon_get(httpd_req_t *req);
static esp_err_t handle_save_post(httpd_req_t *req);
static esp_err_t handle_not_found(httpd_req_t *req, httpd_err_code_t err);
static esp_err_t start_http_server(void);
static void stop_dns_server(void);
static esp_err_t start_dns_server(void);
static void dns_server_task(void *arg);
static void portal_stop_task(void *arg);
static void ensure_scan_mutex(void);
static esp_err_t refresh_scan_results(void);
static bool scan_results_have_ssid(const portal_scan_result_t *results, size_t count, const char *ssid);
static int compare_scan_records_by_rssi_desc(const void *lhs, const void *rhs);
static bool auth_mode_requires_password(wifi_auth_mode_t authmode);
static const char *auth_mode_label(wifi_auth_mode_t authmode);
static char *build_scan_list_html(void);
static char *build_wifi_setup_page(void);
static char *build_config_page_local(void);
static void invalidate_html_cache(void);

static void invalidate_html_cache(void)
{
    if (s_cached_html_page) {
        free(s_cached_html_page);
        s_cached_html_page = NULL;
    }
    s_html_cache_valid = false;
}

static void ensure_scan_mutex(void)
{
    if (!s_scan_mutex) {
        s_scan_mutex = xSemaphoreCreateMutex();
    }
}

static int compare_scan_records_by_rssi_desc(const void *lhs, const void *rhs)
{
    const wifi_ap_record_t *a = (const wifi_ap_record_t *)lhs;
    const wifi_ap_record_t *b = (const wifi_ap_record_t *)rhs;
    return (int)b->rssi - (int)a->rssi;
}

static bool auth_mode_requires_password(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
#ifdef WIFI_AUTH_OWE
    case WIFI_AUTH_OWE:
#endif
        return false;
    default:
        return true;
    }
}

static const char *auth_mode_label(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "Open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "Enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "Secure";
    }
}

static bool scan_results_have_ssid(const portal_scan_result_t *results, size_t count, const char *ssid)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(results[i].ssid, ssid) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t refresh_scan_results(void)
{
    wifi_scan_config_t scan_cfg = {0};
    wifi_ap_record_t *records = NULL;
    portal_scan_result_t staged[MAX_PORTAL_SCAN_RESULTS] = {0};
    uint16_t ap_num = 0;
    size_t staged_count = 0;
    esp_err_t err = ESP_FAIL;

    ensure_scan_mutex();
    if (!s_scan_mutex) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.show_hidden = false;

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_scan_mutex);
        return err;
    }

    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Get AP count failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_scan_mutex);
        return err;
    }

    if (ap_num == 0) {
        memset(s_scan_results, 0, sizeof(s_scan_results));
        s_scan_result_count = 0;
        xSemaphoreGive(s_scan_mutex);
        return ESP_OK;
    }

    if (ap_num > MAX_PORTAL_SCAN_RECORDS) {
        ap_num = MAX_PORTAL_SCAN_RECORDS;
    }

    records = calloc(ap_num, sizeof(*records));
    if (!records) {
        xSemaphoreGive(s_scan_mutex);
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&ap_num, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Get AP records failed: %s", esp_err_to_name(err));
        free(records);
        xSemaphoreGive(s_scan_mutex);
        return err;
    }

    qsort(records, ap_num, sizeof(*records), compare_scan_records_by_rssi_desc);

    for (uint16_t i = 0; i < ap_num && staged_count < MAX_PORTAL_SCAN_RESULTS; ++i) {
        const char *ssid = (const char *)records[i].ssid;
        if (!ssid || ssid[0] == '\0') {
            continue;
        }
        if (scan_results_have_ssid(staged, staged_count, ssid)) {
            continue;
        }
        strncpy(staged[staged_count].ssid, ssid, sizeof(staged[staged_count].ssid) - 1);
        staged[staged_count].ssid[sizeof(staged[staged_count].ssid) - 1] = '\0';
        staged[staged_count].rssi = records[i].rssi;
        staged[staged_count].authmode = records[i].authmode;
        staged_count++;
    }

    memset(s_scan_results, 0, sizeof(s_scan_results));
    memcpy(s_scan_results, staged, staged_count * sizeof(staged[0]));
    s_scan_result_count = staged_count;

    free(records);
    xSemaphoreGive(s_scan_mutex);
    return ESP_OK;
}

static char *build_scan_list_html(void)
{
    char *html;
    size_t cap;
    size_t pos = 0;

    ensure_scan_mutex();
    if (!s_scan_mutex) {
        return NULL;
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    if (s_scan_result_count == 0) {
        static const char *kEmpty = "<div class='scan-empty'>No networks found. Refresh or enter SSID manually.</div>";
        html = strdup(kEmpty);
        xSemaphoreGive(s_scan_mutex);
        return html;
    }

    cap = 256 + s_scan_result_count * 512;
    html = malloc(cap);
    if (!html) {
        xSemaphoreGive(s_scan_mutex);
        return NULL;
    }
    html[0] = '\0';

    for (size_t i = 0; i < s_scan_result_count; ++i) {
        char *esc_ssid = html_escape_dup(s_scan_results[i].ssid);
        const char *auth_label = auth_mode_label(s_scan_results[i].authmode);
        bool needs_password = auth_mode_requires_password(s_scan_results[i].authmode);
        const char *tag_class = needs_password ? "wifi-tag" : "wifi-tag wifi-tag-open";

        if (!esc_ssid) {
            free(html);
            xSemaphoreGive(s_scan_mutex);
            return NULL;
        }

        pos += (size_t)snprintf(
            html + pos, cap - pos,
            "<button class='wifi-item' type='button' data-ssid='%s' data-secure='%d' onclick='selectNetwork(this)'>"
            "<span class='wifi-name'>%s</span>"
            "<span class='wifi-meta'><span class='%s'>%s</span><span>%d dBm</span></span>"
            "</button>",
            esc_ssid, needs_password ? 1 : 0, esc_ssid, tag_class, auth_label, (int)s_scan_results[i].rssi);

        free(esc_ssid);

        if (pos >= cap) {
            break;
        }
    }

    xSemaphoreGive(s_scan_mutex);
    return html;
}

static char *build_wifi_setup_page(void)
{
    char wifi_ssid[33] = {0};
    char wifi_pass[65] = {0};
    char *esc_ssid = NULL;
    char *esc_pass = NULL;
    char *esc_url = NULL;
    char *esc_ap_ssid = NULL;
    char *esc_ap_pass = NULL;
    char *scan_html = NULL;
    char *html = NULL;
    size_t cap;

    load_wifi_credentials_from_nvs(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));

    if (s_scan_result_count == 0) {
        refresh_scan_results();
    }

    esc_ssid = html_escape_dup(wifi_ssid);
    esc_pass = html_escape_dup(wifi_pass);
    esc_url = html_escape_dup(s_ap_url);
    esc_ap_ssid = html_escape_dup(AP_SSID);
    esc_ap_pass = html_escape_dup(AP_PASSWORD);
    scan_html = build_scan_list_html();

    if (!esc_ssid || !esc_pass || !esc_url || !esc_ap_ssid || !esc_ap_pass || !scan_html) {
        goto cleanup;
    }

    cap = strlen(kWifiSetupPageTemplate) + strlen(esc_ap_ssid) + strlen(esc_ap_pass) + strlen(esc_url) +
          strlen(scan_html) + strlen(esc_ssid) + strlen(esc_pass) + 128;
    html = malloc(cap);
    if (!html) {
        goto cleanup;
    }

    snprintf(html, cap, kWifiSetupPageTemplate, esc_ap_ssid, esc_ap_pass, esc_url, scan_html, esc_ssid, esc_pass);

cleanup:
    free(scan_html);
    free(esc_ap_pass);
    free(esc_ap_ssid);
    free(esc_url);
    free(esc_pass);
    free(esc_ssid);
    return html;
}

static char *build_config_page_local(void)
{
    char weather_key[MAX_API_KEY_LEN] = {0};
    char calendar_key[2048] = {0};
    char *esc_weather = NULL;
    char *esc_calendar = NULL;
    char *esc_ssid = NULL;
    char *esc_ip = NULL;
    char *html = NULL;
    size_t cap;
    const char *display_ip = s_local_url;
    
    if (strncmp(display_ip, "http://", 7) == 0) {
        display_ip += 7;
    }

    settings_config_load_weather_key(weather_key, sizeof(weather_key));
    settings_config_load_calendar_key(calendar_key, sizeof(calendar_key));

    esc_weather = html_escape_dup(weather_key);
    esc_calendar = html_escape_dup(calendar_key);
    esc_ssid = html_escape_dup(s_connected_ssid);
    esc_ip = html_escape_dup(display_ip);

    if (!esc_weather || !esc_calendar || !esc_ssid || !esc_ip) {
        goto cleanup;
    }

    cap = strlen(kConfigPageTemplate) + strlen(esc_ssid) + strlen(esc_ip) + 
          strlen(esc_weather) + strlen(esc_calendar) + 256;
    html = malloc(cap);
    if (!html) {
        goto cleanup;
    }

    snprintf(html, cap, kConfigPageTemplate, esc_ssid, esc_ip, esc_weather, esc_calendar);

cleanup:
    free(esc_weather);
    free(esc_calendar);
    free(esc_ssid);
    free(esc_ip);
    return html;
}

static char *get_cached_html_page(void)
{
    if (s_html_cache_valid && s_cached_html_page) {
        return strdup(s_cached_html_page);
    }

    char *html = NULL;
    
    if (s_access_mode == CONFIG_PORTAL_ACCESS_AP) {
        html = build_wifi_setup_page();
    } else if (s_access_mode == CONFIG_PORTAL_ACCESS_LOCAL) {
        html = build_config_page_local();
    }

    if (html) {
        if (s_cached_html_page) {
            free(s_cached_html_page);
        }
        s_cached_html_page = strdup(html);
        s_html_cache_valid = true;
    }

    return html;
}

static char *html_escape_dup(const char *src)
{
    if (!src) {
        src = "";
    }

    size_t len = strlen(src);
    size_t cap = len * 6 + 1;
    char *out = malloc(cap);
    size_t pos = 0;

    if (!out) {
        return NULL;
    }

    for (size_t i = 0; i < len; ++i) {
        const char *rep = NULL;
        switch (src[i]) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }

        if (rep) {
            size_t rep_len = strlen(rep);
            memcpy(out + pos, rep, rep_len);
            pos += rep_len;
        } else {
            out[pos++] = src[i];
        }
    }

    out[pos] = '\0';
    return out;
}

static void url_decode_inplace(char *value)
{
    char *src = value;
    char *dst = value;

    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }

        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    size_t key_len;
    const char *cursor;

    if (!body || !key || !out || out_size == 0) {
        return false;
    }

    key_len = strlen(key);
    cursor = body;

    while (*cursor) {
        const char *pair_end = strchr(cursor, '&');
        const char *value_sep = strchr(cursor, '=');
        size_t value_len;

        if (!pair_end) {
            pair_end = cursor + strlen(cursor);
        }

        if (!value_sep || value_sep > pair_end) {
            cursor = (*pair_end == '&') ? pair_end + 1 : pair_end;
            continue;
        }

        if ((size_t)(value_sep - cursor) == key_len && strncmp(cursor, key, key_len) == 0) {
            value_len = (size_t)(pair_end - value_sep - 1);
            if (value_len >= out_size) {
                value_len = out_size - 1;
            }
            memcpy(out, value_sep + 1, value_len);
            out[value_len] = '\0';
            url_decode_inplace(out);
            return true;
        }

        cursor = (*pair_end == '&') ? pair_end + 1 : pair_end;
    }

    out[0] = '\0';
    return false;
}

static esp_err_t handle_root_get(httpd_req_t *req)
{
    char *html = get_cached_html_page();

    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build page");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ESP_OK;
}

static esp_err_t handle_favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_save_post(httpd_req_t *req)
{
    char *body = NULL;
    char weather_key[MAX_API_KEY_LEN] = {0};
    char calendar_key[2048] = {0};
    char wifi_ssid[33] = {0};
    char wifi_pass[65] = {0};
    int remaining;
    int offset = 0;
    bool data_changed = false;

    if (req->content_len <= 0 || req->content_len > MAX_FORM_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
        return ESP_FAIL;
    }

    body = malloc((size_t)req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + offset, remaining);
        if (received <= 0) {
            free(body);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
                return ESP_ERR_TIMEOUT;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        offset += received;
        remaining -= received;
    }
    body[offset] = '\0';

    if (s_access_mode == CONFIG_PORTAL_ACCESS_AP) {
        form_get_value(body, "wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
        form_get_value(body, "wifi_pass", wifi_pass, sizeof(wifi_pass));
        
        if (strlen(wifi_ssid) > 0) {
            save_wifi_credentials_to_nvs(wifi_ssid, wifi_pass);
            strlcpy(s_current_wifi_ssid, wifi_ssid, sizeof(s_current_wifi_ssid));
            strlcpy(s_current_wifi_pass, wifi_pass, sizeof(s_current_wifi_pass));
            data_changed = true;
            
            app_settings_uart_wifi_cred_data_t payload;
            memset(&payload, 0, sizeof(payload));
            snprintf(payload.ssid, sizeof(payload.ssid), "%s", wifi_ssid);
            int n = snprintf(payload.password, sizeof(payload.password), "%s", wifi_pass);
            if (n > 0) {
                payload.len = n;
                esp_event_post(APP_SETTINGS_UART_EVENTS, APP_SETTGINS_UART_EVENT_RECIEVED_WIFI_CRED, 
                            &payload, sizeof(payload), 0);
            }
        }
    } else {
        form_get_value(body, "weather_key", weather_key, sizeof(weather_key));
        form_get_value(body, "calendar_key", calendar_key, sizeof(calendar_key));

        settings_config_save_weather_key(weather_key);
        settings_config_save_calendar_key(calendar_key);
        data_changed = true;

        if (strlen(weather_key) > 0) {
            app_settings_uart_weather_cred_data_t payload;
            memset(&payload, 0, sizeof(payload));
            int n = snprintf(payload.key, sizeof(payload.key), "%s", weather_key);
            if (n > 0) {
                payload.len = n;
                esp_event_post(APP_SETTINGS_UART_EVENTS, APP_SETTGINS_UART_EVENT_RECIEVED_WEATHER_CRED, 
                            &payload, sizeof(payload), 0);
            }
        }

        if (strlen(calendar_key) > 0) {
            app_settings_uart_calendar_cred_data_t payload;
            memset(&payload, 0, sizeof(payload));
            int n = snprintf(payload.key, sizeof(payload.key), "%s", calendar_key);
            if (n > 0) {
                payload.len = n;
                esp_event_post(APP_SETTINGS_UART_EVENTS, APP_SETTGINS_UART_EVENT_RECIEVED_CALENDAR_CRED, 
                            &payload, sizeof(payload), 0);
            }
        }
    }

    free(body);

    if (data_changed) {
        invalidate_html_cache();
        s_data_saved = true;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, kSuccessPage);

    if (s_runtime_mode) {
        xTaskCreate(portal_stop_task, "portal_stop", 4096, NULL, 10, NULL);
    }

    return ESP_OK;
}

static esp_err_t handle_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void clear_arp_cache(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            struct netif *netif = esp_netif_get_netif_impl(ap_netif);
            if (netif) {
                etharp_cleanup_netif(netif);
            }
        }
    }
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t generate_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t fwlink = {
        .uri = "/fwlink",
        .method = HTTP_GET,
        .handler = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t hotspot = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t connectivity = {
        .uri = "/connectivity-check.html",
        .method = HTTP_GET,
        .handler = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = handle_favicon_get,
        .user_ctx = NULL,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = handle_save_post,
        .user_ctx = NULL,
    };

    config.stack_size = 8192;
    config.lru_purge_enable = false;
    config.server_port = HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 20;

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "httpd_start failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &root), TAG, "register root failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &generate_204), TAG, "register generate_204 failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &fwlink), TAG, "register fwlink failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &hotspot), TAG, "register hotspot failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &connectivity), TAG, "register connectivity failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &favicon), TAG, "register favicon failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &save), TAG, "register save failed");
    httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND, handle_not_found);

    return ESP_OK;
}

static void stop_dns_server(void)
{
    s_dns_running = false;

    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, SHUT_RDWR);
        close(s_dns_sock);
        s_dns_sock = -1;
    }
}

static size_t dns_skip_question_name(const uint8_t *packet, size_t packet_len, size_t offset)
{
    while (offset < packet_len) {
        uint8_t len = packet[offset];
        if (len == 0) {
            return offset + 1;
        }
        if ((len & 0xC0) == 0xC0) {
            return offset + 2;
        }
        offset += (size_t)len + 1;
    }
    return packet_len;
}

static void dns_server_task(void *arg)
{
    uint8_t rx[512];
    uint8_t tx[512];
    struct sockaddr_in bind_addr = {0};
    uint32_t ap_ip_addr = inet_addr(s_ap_url + strlen("http://"));

    (void)arg;

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(DNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_dns_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(s_dns_sock);
        s_dns_sock = -1;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS redirect server started on port %d", DNS_PORT);

    while (s_dns_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        ssize_t rx_len = recvfrom(s_dns_sock, rx, sizeof(rx), 0, (struct sockaddr *)&client_addr, &client_len);

        if (rx_len <= 0) {
            continue;
        }

        if (rx_len < 12) {
            continue;
        }

        size_t qname_end = dns_skip_question_name(rx, (size_t)rx_len, 12);
        if (qname_end + 4 > (size_t)rx_len) {
            continue;
        }

        uint16_t qtype = (uint16_t)((rx[qname_end] << 8) | rx[qname_end + 1]);
        size_t question_len = qname_end + 4 - 12;
        size_t tx_len = 0;
        bool answer_a = (qtype == 1 || qtype == 255);

        memcpy(tx, rx, 2);
        tx[2] = 0x81;
        tx[3] = 0x80;
        tx[4] = 0x00;
        tx[5] = 0x01;
        tx[6] = 0x00;
        tx[7] = answer_a ? 0x01 : 0x00;
        tx[8] = 0x00;
        tx[9] = 0x00;
        tx[10] = 0x00;
        tx[11] = 0x00;
        tx_len = 12;

        memcpy(tx + tx_len, rx + 12, question_len);
        tx_len += question_len;

        if (answer_a) {
            tx[tx_len++] = 0xC0;
            tx[tx_len++] = 0x0C;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x01;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x01;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x3C;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x04;
            memcpy(tx + tx_len, &ap_ip_addr, 4);
            tx_len += 4;
        }

        sendto(s_dns_sock, tx, tx_len, 0, (struct sockaddr *)&client_addr, client_len);
    }

    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }

    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_dns_server(void)
{
    if (s_dns_task) {
        return ESP_OK;
    }

    s_dns_running = true;
    if (xTaskCreate(dns_server_task, "cfg_dns", 4096, NULL, 5, &s_dns_task) != pdPASS) {
        s_dns_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void reset_dhcp_server(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_dhcp_status_t status;
        if (esp_netif_dhcps_get_status(ap_netif, &status) == ESP_OK) {
            if (status == ESP_NETIF_DHCP_STARTED) {
                esp_netif_dhcps_stop(ap_netif);
                vTaskDelay(pdMS_TO_TICKS(200));
                
                esp_netif_ip_info_t ip_info;
                IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
                IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
                IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
                esp_netif_set_ip_info(ap_netif, &ip_info);
                esp_netif_dhcps_start(ap_netif);
            }
        }
    }
}

static void disconnect_all_stations(void)
{
    wifi_config_t ap_config;
    
    if (esp_wifi_get_config(WIFI_IF_AP, &ap_config) == ESP_OK) {
        ap_config.ap.max_connection = 0;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        vTaskDelay(pdMS_TO_TICKS(100));
        ap_config.ap.max_connection = 4;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
}

static void portal_stop_task(void *arg)
{
    (void)arg;
    
    if (s_runtime_mode) {
        config_portal_stop_runtime();
    } else {
        config_portal_stop();
    }
    
    app_settings_uart_enable_data_t payload = {.enable = false};
    esp_event_post(APP_SETTINGS_UART_EVENTS, APP_SETTINGS_UART_EVENT_ENABLE_REQ, 
                   &payload, sizeof(payload), 0);
    
    vTaskDelete(NULL);
}

static esp_err_t start_ap_mode(void)
{
    esp_err_t err;

    s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "Failed to create AP netif");
            s_portal_starting = false;
            return ESP_FAIL;
        }
    }

    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "STA netif not available");
        s_portal_starting = false;
        return ESP_FAIL;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set storage failed: %s", esp_err_to_name(err));
        s_portal_starting = false;
        return err;
    }

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, AP_PASSWORD, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len = strlen(AP_SSID);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set mode failed: %s", esp_err_to_name(err));
        s_portal_starting = false;
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set AP config failed: %s", esp_err_to_name(err));
        s_portal_starting = false;
        return err;
    }

    clear_arp_cache();
    reset_dhcp_server();

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable power save: %s", esp_err_to_name(err));
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(s_ap_url, sizeof(s_ap_url), "http://" IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(s_ap_url, AP_DEFAULT_URL, sizeof(s_ap_url) - 1);
        s_ap_url[sizeof(s_ap_url) - 1] = '\0';
    }

    /* Запускаем сканирование асинхронно — результаты придут позже */
    refresh_scan_results();

    /* Убираем vTaskDelay(1000) — HTTP сервер готов сразу */
    if (!s_portal_starting) {
        ESP_LOGI(TAG, "Portal start cancelled");
        return ESP_OK;
    }

    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        s_portal_starting = false;
        return err;
    }

    err = start_dns_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DNS server start failed: %s", esp_err_to_name(err));
        stop_dns_server();
        httpd_stop(s_http_server);
        s_http_server = NULL;
        s_portal_starting = false;
        return err;
    }

    s_portal_running = true;
    s_portal_starting = false;
    s_access_mode = CONFIG_PORTAL_ACCESS_AP;
    ESP_LOGI(TAG, "Config portal started in AP mode: SSID=%s URL=%s", AP_SSID, s_ap_url);
    return ESP_OK;
}

static esp_err_t start_local_mode(void)
{
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    esp_err_t err;

    if (!s_portal_starting) {
        ESP_LOGI(TAG, "Portal start cancelled before local mode");
        return ESP_OK;
    }

    if (!sta_netif) {
        ESP_LOGE(TAG, "STA netif not available");
        s_portal_starting = false;
        return ESP_FAIL;
    }

    if (esp_netif_get_ip_info(sta_netif, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP info");
        s_portal_starting = false;
        return ESP_FAIL;
    }

    snprintf(s_local_url, sizeof(s_local_url), "http://" IPSTR, IP2STR(&ip_info.ip));

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strlcpy(s_connected_ssid, (const char *)ap_info.ssid, sizeof(s_connected_ssid));
    } else {
        s_connected_ssid[0] = '\0';
    }

    if (!s_portal_starting) {
        ESP_LOGI(TAG, "Portal start cancelled during local mode setup");
        return ESP_OK;
    }

    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        s_portal_starting = false;
        return err;
    }

    s_portal_running = true;
    s_portal_starting = false;
    s_runtime_mode = true;
    s_access_mode = CONFIG_PORTAL_ACCESS_LOCAL;
    ESP_LOGI(TAG, "Config portal started in local mode: URL=%s SSID=%s", 
            s_local_url, s_connected_ssid);
    return ESP_OK;
}

esp_err_t config_portal_start(void)
{
    return config_portal_start_runtime();
}

esp_err_t config_portal_start_runtime(void)
{
    if (s_portal_running) {
        ESP_LOGW(TAG, "Portal already running");
        return ESP_OK;
    }
    
    if (s_portal_starting) {
        ESP_LOGW(TAG, "Portal already starting");
        return ESP_OK;
    }

    invalidate_html_cache();
    s_portal_starting = true;

    if (wifi_ctrl_is_connected()) {
        return start_local_mode();
    } else {
        s_runtime_mode = true;
        return start_ap_mode();
    }
}

void config_portal_stop(void)
{
    if (!s_portal_running && !s_portal_starting) {
        return;
    }

    s_portal_starting = false;

    if (!s_portal_running) {
        ESP_LOGI(TAG, "Portal start cancelled");
        return;
    }

    invalidate_html_cache();
    ESP_LOGI(TAG, "Stopping config portal");

    stop_dns_server();

    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    if (s_access_mode == CONFIG_PORTAL_ACCESS_AP) {
        disconnect_all_stations();
        
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch back to STA mode: %s", esp_err_to_name(err));
        }
    }

    s_portal_running = false;
    s_runtime_mode = false;
    s_access_mode = CONFIG_PORTAL_ACCESS_NONE;
    s_connected_ssid[0] = '\0';
    s_data_saved = false;

    esp_event_post(APP_SETTINGS_UART_EVENTS, APP_SETTINGS_UART_EVENT_PORTAL_STOPPED,
                   NULL, 0, 0);
}

static void portal_stop_async_task(void *arg)
{
    config_portal_stop();
    vTaskDelete(NULL);
}

void config_portal_stop_async(void)
{
    if (!s_portal_running && !s_portal_starting) {
        return;
    }
    
    s_portal_starting = false;
    
    if (!s_portal_running) {
        ESP_LOGI(TAG, "Portal start cancelled");
        return;
    }
    
    xTaskCreate(portal_stop_async_task, "portal_stop_async", 4096, NULL, 5, NULL);
}

esp_err_t config_portal_stop_runtime(void)
{
    config_portal_stop();
    return ESP_OK;
}

bool config_portal_is_running(void)
{
    return s_portal_running;
}

bool config_portal_is_runtime_mode(void)
{
    return s_runtime_mode;
}

config_portal_access_mode_t config_portal_get_access_mode(void)
{
    return s_access_mode;
}

const char *config_portal_get_ap_ssid(void)
{
    return AP_SSID;
}

const char *config_portal_get_ap_password(void)
{
    return AP_PASSWORD;
}

const char *config_portal_get_ap_url(void)
{
    return s_ap_url;
}

const char *config_portal_get_local_url(void)
{
    if (s_access_mode == CONFIG_PORTAL_ACCESS_LOCAL && s_local_url[0] != '\0') {
        return s_local_url;
    }
    return s_ap_url;
}

const char *config_portal_get_connected_ssid(void)
{
    if (s_access_mode == CONFIG_PORTAL_ACCESS_LOCAL && s_connected_ssid[0] != '\0') {
        return s_connected_ssid;
    }
    return NULL;
}

esp_err_t config_portal_init(void)
{
    return ESP_OK;
}

void config_portal_deinit(void)
{
    config_portal_stop();
}
