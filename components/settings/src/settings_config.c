#include "settings_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sleep_mode.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "settings_config";

#define DEFAULT_SLEEP_TIMEOUT_MS (3U * 60U * 1000U)

static bool is_valid_sleep_timeout_ms(uint32_t timeout_ms)
{
    static const uint32_t allowed[] = {
        1U * 60U * 1000U, 3U * 60U * 1000U, 5U * 60U * 1000U, 10U * 60U * 1000U, 30U * 60U * 1000U, 60U * 60U * 1000U,
    };

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (timeout_ms == allowed[i]) {
            return true;
        }
    }

    return false;
}

esp_err_t settings_config_init(void)
{
    return ESP_OK;
}

esp_err_t settings_config_save_audio_volume(uint8_t volume)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    if (volume > 100U) {
        volume = 100U;
    }

    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY_AUDIO_VOLUME, volume);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Audio volume saved: %u", (unsigned int)volume);
    }
    return err;
}

uint8_t settings_config_load_audio_volume(void)
{
    nvs_handle_t nvs_handle;
    uint8_t volume = 80;

    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        (void)nvs_get_u8(nvs_handle, NVS_KEY_AUDIO_VOLUME, &volume);
        nvs_close(nvs_handle);
    }

    return volume <= 100U ? volume : 80U;
}

esp_err_t settings_config_save_sleep_timeout_ms(uint32_t timeout_ms)
{
    if (! is_valid_sleep_timeout_ms(timeout_ms)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u32(nvs_handle, NVS_KEY_SLEEP_TIMEOUT_MS, timeout_ms);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        sleep_update_idle_timeout(timeout_ms);
        ESP_LOGI(TAG, "Sleep timeout saved: %ums", (unsigned)timeout_ms);
    }
    return err;
}

uint32_t settings_config_load_sleep_timeout_ms(void)
{
    nvs_handle_t nvs_handle;
    uint32_t timeout_ms = DEFAULT_SLEEP_TIMEOUT_MS;

    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        (void)nvs_get_u32(nvs_handle, NVS_KEY_SLEEP_TIMEOUT_MS, &timeout_ms);
        nvs_close(nvs_handle);
    }

    if (! is_valid_sleep_timeout_ms(timeout_ms)) {
        timeout_ms = DEFAULT_SLEEP_TIMEOUT_MS;
    }

    return timeout_ms;
}

esp_err_t settings_config_save_wifi_enabled(bool enabled)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY_WIFI_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "WiFi enabled state saved: %d", enabled);
    return err;
}

bool settings_config_load_wifi_enabled(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No saved WiFi enabled state found");
        return false;
    }

    uint8_t value = 0;
    err           = nvs_get_u8(nvs_handle, NVS_KEY_WIFI_ENABLED, &value);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi enabled state loaded: %d", value ? true : false);
        return value ? true : false;
    }

    return false;
}

esp_err_t settings_config_save_uart_enabled(bool enabled)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY_UART_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "UART enabled state saved: %d", enabled);
    return err;
}

bool settings_config_load_uart_enabled(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No saved UART enabled state found");
        return false;
    }

    uint8_t value = 0;
    err           = nvs_get_u8(nvs_handle, NVS_KEY_UART_ENABLED, &value);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UART enabled state loaded: %d", value ? true : false);
        return value ? true : false;
    }

    return false;
}

esp_err_t settings_config_save_weather_key(const char* key)
{
    if (! key)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, NVS_KEY_WEATHER_KEY, key, strlen(key) + 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Weather API key saved");
    return err;
}

esp_err_t settings_config_load_weather_key(char* key, size_t len)
{
    if (! key || len == 0)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = 0;
    err                  = nvs_get_blob(nvs_handle, NVS_KEY_WEATHER_KEY, NULL, &required_size);
    if (err == ESP_OK && required_size > 0 && required_size <= len) {
        err = nvs_get_blob(nvs_handle, NVS_KEY_WEATHER_KEY, key, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Weather API key loaded");
        }
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t settings_config_save_calendar_key(const char* key)
{
    if (! key)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, NVS_KEY_CALENDAR_KEY, key, strlen(key) + 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Calendar API key saved");
    return err;
}

esp_err_t settings_config_load_calendar_key(char* key, size_t len)
{
    if (! key || len == 0)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = 0;
    err                  = nvs_get_blob(nvs_handle, NVS_KEY_CALENDAR_KEY, NULL, &required_size);
    if (err == ESP_OK && required_size > 0 && required_size <= len) {
        err = nvs_get_blob(nvs_handle, NVS_KEY_CALENDAR_KEY, key, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Calendar API key loaded");
        }
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t settings_config_clear_all(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "All configuration cleared");
    return err;
}

esp_err_t settings_config_save_brightness(uint8_t brightness)
{
    if (brightness < 1U || brightness > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY_BRIGHTNESS, brightness);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        sleep_update_normal_brightness(brightness);
        ESP_LOGI(TAG, "Brightness saved: %d", brightness);
    }
    return err;
}

uint8_t settings_config_load_brightness(void)
{
    nvs_handle_t nvs_handle;
    uint8_t brightness = 80;

    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        (void)nvs_get_u8(nvs_handle, NVS_KEY_BRIGHTNESS, &brightness);
        nvs_close(nvs_handle);
    }

    return brightness >= 1U && brightness <= 100U ? brightness : 80U;
}

esp_err_t settings_config_save_ha_enabled(bool enabled)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY_HA_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "HA enabled state saved: %d", enabled);
    return err;
}

bool settings_config_load_ha_enabled(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t value = 0;
    err           = nvs_get_u8(nvs_handle, NVS_KEY_HA_ENABLED, &value);
    nvs_close(nvs_handle);

    return (err == ESP_OK) ? (value != 0) : false;
}

esp_err_t settings_config_save_ha_url(const char* url)
{
    if (! url)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, NVS_KEY_HA_URL, url, strlen(url) + 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "HA URL saved");
    return err;
}

esp_err_t settings_config_load_ha_url(char* url, size_t len)
{
    if (! url || len == 0)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = 0;
    err                  = nvs_get_blob(nvs_handle, NVS_KEY_HA_URL, NULL, &required_size);
    if (err == ESP_OK && required_size > 0 && required_size <= len) {
        err = nvs_get_blob(nvs_handle, NVS_KEY_HA_URL, url, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HA URL loaded");
        }
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t settings_config_save_ha_login(const char* login)
{
    if (! login)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, NVS_KEY_HA_LOGIN, login, strlen(login) + 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "HA login saved");
    return err;
}

esp_err_t settings_config_load_ha_login(char* login, size_t len)
{
    if (! login || len == 0)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = 0;
    err                  = nvs_get_blob(nvs_handle, NVS_KEY_HA_LOGIN, NULL, &required_size);
    if (err == ESP_OK && required_size > 0 && required_size <= len) {
        err = nvs_get_blob(nvs_handle, NVS_KEY_HA_LOGIN, login, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HA login loaded");
        }
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t settings_config_save_ha_password(const char* password)
{
    if (! password)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, NVS_KEY_HA_PASSWORD, password, strlen(password) + 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "HA password saved");
    return err;
}

esp_err_t settings_config_load_ha_password(char* password, size_t len)
{
    if (! password || len == 0)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = 0;
    err                  = nvs_get_blob(nvs_handle, NVS_KEY_HA_PASSWORD, NULL, &required_size);
    if (err == ESP_OK && required_size > 0 && required_size <= len) {
        err = nvs_get_blob(nvs_handle, NVS_KEY_HA_PASSWORD, password, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HA password loaded");
        }
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t settings_config_save_ha_ws_port(uint16_t port)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u16(nvs_handle, NVS_KEY_HA_WS_PORT, port);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "HA WS port saved: %u", port);
    return err;
}

uint16_t settings_config_load_ha_ws_port(void)
{
    nvs_handle_t nvs_handle;
    uint16_t port = 3000;

    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_get_u16(nvs_handle, NVS_KEY_HA_WS_PORT, &port);
        nvs_close(nvs_handle);
    }

    if (err != ESP_OK || port == 0) {
        port = 3000;
    }

    ESP_LOGI(TAG, "HA WS port loaded: %u", port);
    return port;
}

esp_err_t settings_config_save_opcua_endpoint(const char* endpoint_url)
{
    if (endpoint_url == NULL || strncmp(endpoint_url, "opc.tcp://", 10) != 0 ||
        strlen(endpoint_url) >= SETTINGS_OPCUA_ENDPOINT_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_str(nvs_handle, NVS_KEY_OPCUA_ENDPOINT, endpoint_url);
    if (result == ESP_OK) {
        result = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return result;
}

esp_err_t settings_config_load_opcua_endpoint(char* endpoint_url, size_t length, const char* default_endpoint_url)
{
    if (endpoint_url == NULL || length == 0 || default_endpoint_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (result == ESP_OK) {
        size_t required_length = length;
        result                 = nvs_get_str(nvs_handle, NVS_KEY_OPCUA_ENDPOINT, endpoint_url, &required_length);
        nvs_close(nvs_handle);
    }
    if (result == ESP_OK && strncmp(endpoint_url, "opc.tcp://", 10) == 0) {
        return ESP_OK;
    }

    snprintf(endpoint_url, length, "%s", default_endpoint_url);
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}

bool settings_config_has_saved_opcua_endpoint(void)
{
    char endpoint_url[SETTINGS_OPCUA_ENDPOINT_LENGTH] = {0};
    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (result != ESP_OK) {
        return false;
    }

    size_t required_length = sizeof(endpoint_url);
    result = nvs_get_str(nvs_handle, NVS_KEY_OPCUA_ENDPOINT, endpoint_url, &required_length);
    nvs_close(nvs_handle);
    return result == ESP_OK && strncmp(endpoint_url, "opc.tcp://", 10) == 0;
}
