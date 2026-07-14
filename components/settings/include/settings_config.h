#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_CONFIG_NAMESPACE           "app_config"
#define NVS_KEY_WIFI_ENABLED           "wifi_enabled"
#define NVS_KEY_UART_ENABLED           "uart_enabled"
#define NVS_KEY_WEATHER_KEY            "weather_key"
#define NVS_KEY_CALENDAR_KEY           "calendar_key"
#define NVS_KEY_BRIGHTNESS             "brightness"
#define NVS_KEY_SLEEP_TIMEOUT_MS       "sleep_to_ms"
#define NVS_KEY_HA_ENABLED             "ha_enabled"
#define NVS_KEY_HA_URL                 "ha_url"
#define NVS_KEY_HA_LOGIN               "ha_login"
#define NVS_KEY_HA_PASSWORD            "ha_password"
#define NVS_KEY_HA_WS_PORT             "ha_ws_port"
#define NVS_KEY_OPCUA_ENDPOINT         "opcua_url"
#define SETTINGS_OPCUA_ENDPOINT_LENGTH 256

typedef struct
{
    bool wifi_enabled;
    bool uart_enabled;
    char weather_api_key[256];
    char calendar_api_key[2048];
} app_config_t;

esp_err_t settings_config_init(void);
esp_err_t settings_config_save_wifi_enabled(bool enabled);
bool settings_config_load_wifi_enabled(void);
esp_err_t settings_config_save_uart_enabled(bool enabled);
bool settings_config_load_uart_enabled(void);
esp_err_t settings_config_save_weather_key(const char* key);
esp_err_t settings_config_load_weather_key(char* key, size_t len);
esp_err_t settings_config_save_calendar_key(const char* key);
esp_err_t settings_config_load_calendar_key(char* key, size_t len);
esp_err_t settings_config_clear_all(void);
esp_err_t settings_config_save_brightness(uint8_t brightness);
uint8_t settings_config_load_brightness(void);
esp_err_t settings_config_save_audio_volume(uint8_t volume);
uint8_t settings_config_load_audio_volume(void);
esp_err_t settings_config_save_sleep_timeout_ms(uint32_t timeout_ms);
uint32_t settings_config_load_sleep_timeout_ms(void);
esp_err_t settings_config_save_ha_enabled(bool enabled);
bool settings_config_load_ha_enabled(void);
esp_err_t settings_config_save_ha_url(const char* url);
esp_err_t settings_config_load_ha_url(char* url, size_t len);
esp_err_t settings_config_save_ha_login(const char* login);
esp_err_t settings_config_load_ha_login(char* login, size_t len);
esp_err_t settings_config_save_ha_password(const char* password);
esp_err_t settings_config_load_ha_password(char* password, size_t len);
esp_err_t settings_config_save_ha_ws_port(uint16_t port);
uint16_t settings_config_load_ha_ws_port(void);
esp_err_t settings_config_save_opcua_endpoint(const char* endpoint_url);
esp_err_t settings_config_load_opcua_endpoint(char* endpoint_url, size_t length, const char* default_endpoint_url);

#ifdef __cplusplus
}
#endif
