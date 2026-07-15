#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "esp_log.h"

#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char* TAG = "wifi";

void wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi");

    ESP_ERROR_CHECK(esp_netif_init());

    esp_event_loop_create_default();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Display sleep only turns off the LCD. Keep the station radio awake so
     * ESP-Hosted does not drop the link while the HMI is idle. */
    esp_err_t power_save_result = esp_wifi_set_ps(WIFI_PS_NONE);
    if (power_save_result != ESP_OK) {
        ESP_LOGW(TAG, "Unable to disable WiFi power save: %s",
                 esp_err_to_name(power_save_result));
    }
}

void wifi_deinit(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
}

bool wifi_connect(const char* ssid, const char* password)
{
    if (! ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return false;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);

    if (password && strlen(password) > 0) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config.sta.threshold.rssi     = -80;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
        return false;
    }
    if (esp_wifi_connect() != ESP_OK) {
        return false;
    }

    return true;
}

bool wifi_disconnect()
{
    ESP_LOGI(TAG, "Disconnecting from AP");
    if (esp_wifi_disconnect() != ESP_OK) {
        return false;
    }
    return true;
}
