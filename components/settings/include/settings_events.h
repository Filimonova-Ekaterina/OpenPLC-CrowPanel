#pragma once

#include "esp_event.h"
#include "esp_wifi.h"

ESP_EVENT_DECLARE_BASE(APP_SETTINGS_WIFI_EVENTS);
ESP_EVENT_DECLARE_BASE(APP_SETTINGS_STORAGE_EVENTS);
ESP_EVENT_DECLARE_BASE(APP_SETTINGS_UART_EVENTS);
ESP_EVENT_DECLARE_BASE(APP_SETTINGS_SYSTEM_EVENTS);
ESP_EVENT_DECLARE_BASE(APP_SETTINGS_HA_EVENTS);

typedef enum
{
    APP_SETTINGS_HA_EVENT_ENABLED,
    APP_SETTINGS_HA_EVENT_DISABLED,
} app_settings_ha_event_t;

typedef enum
{
    APP_SETTINGS_WIFI_EVENT_WIFI_ENABLE_REQ,
    APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ,
    APP_SETTINGS_WIFI_EVENT_SET_NETWORK_REQ,
    APP_SETTINGS_WIFI_EVENT_CONNECT_REQ,
    APP_SETTINGS_WIFI_EVENT_DISCONNECT_REQ,
    APP_SETTINGS_WIFI_EVENT_CONNECTIVITY_STATUS_CHANGED,
    APP_SETTINGS_WIFI_EVENT_SCAN_STATE_CHANGED,
    APP_SETTINGS_WIFI_EVENT_PASSWORD_REQUESTED,
    APP_SETTINGS_WIFI_EVENT_CONNECT_RESULT,
    APP_SETTINGS_WIFI_EVENT_CONNECTING,
} app_settings_wifi_event_t;

typedef enum
{
    APP_SETTINGS_STORAGE_EVENT_CLEAR_REQ,
    APP_SETTINGS_STORAGE_EVENT_CLEARED,
} app_settings_storage_event_t;

typedef enum
{
    APP_SETTINGS_UART_EVENT_ENABLE_REQ,
    APP_SETTGINS_UART_EVENT_RECIEVED_WIFI_CRED,
    APP_SETTGINS_UART_EVENT_RECIEVED_WEATHER_CRED,
    APP_SETTGINS_UART_EVENT_RECIEVED_CALENDAR_CRED,
    APP_SETTINGS_UART_EVENT_PORTAL_STOPPED,
    APP_SETTGINS_UART_EVENT_RECIEVED_HA_CRED,
} app_settings_uart_event_t;

typedef enum
{
    APP_SETTINGS_TIME_EVENT_SYNCED,
    APP_SETTINGS_TIMEZONE_EVENT_LOCATION_AVAILABLE,
} app_settings_system_event_t;

typedef struct
{
    bool enable;
} app_settings_wifi_enable_data_t;

typedef struct
{
    bool enable;
} app_settings_wifi_scan_enable_data_t;

typedef struct
{
    bool enable;
} app_settings_uart_enable_data_t;

typedef struct
{
    wifi_ap_record_t network;
} app_settings_wifi_set_network_data_t;

typedef struct
{
    char password[64];
    uint8_t len;
} app_settings_wifi_connect_data_t;

typedef struct
{
    char ssid[32];
    uint8_t len;
} app_settings_wifi_password_requested_data_t;

typedef struct
{
    int status;
    char details[64];
    uint8_t len;
} app_settings_wifi_connectivity_status_data_t;

typedef struct
{
    bool is_enabled;
} app_settings_wifi_scan_state_data_t;

typedef struct
{
    char password[64];
    char ssid[33];
    uint8_t len;
} app_settings_uart_wifi_cred_data_t;

typedef struct
{
    char key[1024];
    uint8_t len;
} app_settings_uart_weather_cred_data_t;

typedef struct
{
    char key[2048];
    uint8_t len;
} app_settings_uart_calendar_cred_data_t;

typedef struct
{
    int error;
    char details[64];
    uint8_t len;
} app_settings_wifi_connect_result_data_t;

typedef struct
{
    char url[256];
    char login[65];
    char password[65];
    uint8_t url_len;
    uint8_t login_len;
    uint8_t password_len;
    uint16_t ws_port;
} app_settings_uart_ha_cred_data_t;

static inline const char* app_settings_wifi_event_to_string(app_settings_wifi_event_t event)
{
    switch (event) {
    case APP_SETTINGS_WIFI_EVENT_WIFI_ENABLE_REQ:
        return "APP_SETTINGS_WIFI_EVENT_WIFI_ENABLE_REQ";
    case APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ:
        return "APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ";
    case APP_SETTINGS_WIFI_EVENT_SET_NETWORK_REQ:
        return "APP_SETTINGS_WIFI_EVENT_SET_NETWORK_REQ";
    case APP_SETTINGS_WIFI_EVENT_CONNECT_REQ:
        return "APP_SETTINGS_WIFI_EVENT_CONNECT_REQ";
    case APP_SETTINGS_WIFI_EVENT_DISCONNECT_REQ:
        return "APP_SETTINGS_WIFI_EVENT_DISCONNECT_REQ";
    case APP_SETTINGS_WIFI_EVENT_CONNECTIVITY_STATUS_CHANGED:
        return "APP_SETTINGS_WIFI_EVENT_CONNECTIVITY_STATUS_CHANGED";
    case APP_SETTINGS_WIFI_EVENT_SCAN_STATE_CHANGED:
        return "APP_SETTINGS_WIFI_EVENT_SCAN_STATE_CHANGED";
    case APP_SETTINGS_WIFI_EVENT_PASSWORD_REQUESTED:
        return "APP_SETTINGS_WIFI_EVENT_PASSWORD_REQUESTED";
    case APP_SETTINGS_WIFI_EVENT_CONNECT_RESULT:
        return "APP_SETTINGS_WIFI_EVENT_CONNECT_RESULT";
    case APP_SETTINGS_WIFI_EVENT_CONNECTING:
        return "APP_SETTINGS_WIFI_EVENT_CONNECTING";
    default:
        return "UNKNOWN_WIFI_EVENT";
    }
}

static inline const char* app_settings_storage_event_to_string(app_settings_storage_event_t event)
{
    switch (event) {
    case APP_SETTINGS_STORAGE_EVENT_CLEAR_REQ:
        return "APP_SETTINGS_STORAGE_EVENT_CLEAR_REQ";
    case APP_SETTINGS_STORAGE_EVENT_CLEARED:
        return "APP_SETTINGS_STORAGE_EVENT_CLEARED";
    default:
        return "UNKNOWN_STORAGE_EVENT";
    }
}

static inline const char* app_settings_uart_event_to_string(app_settings_uart_event_t event)
{
    switch (event) {
    case APP_SETTINGS_UART_EVENT_ENABLE_REQ:
        return "APP_SETTINGS_UART_EVENT_ENABLE_REQ";
    case APP_SETTGINS_UART_EVENT_RECIEVED_WIFI_CRED:
        return "APP_SETTINGS_UART_EVENT_RECEIVED_WIFI_CRED";
    case APP_SETTGINS_UART_EVENT_RECIEVED_WEATHER_CRED:
        return "APP_SETTINGS_UART_EVENT_RECEIVED_WEATHER_CRED";
    case APP_SETTGINS_UART_EVENT_RECIEVED_CALENDAR_CRED:
        return "APP_SETTINGS_UART_EVENT_RECEIVED_CALENDAR_CRED";
    default:
        return "UNKNOWN_UART_EVENT";
    }
}