#include "time_sync.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

#define MINIMUM_VALID_UNIX_TIME 1704067200LL /* 2024-01-01 00:00:00 UTC */

static const char* TAG = "time_sync";
static bool s_initialized;

static void synchronization_callback(struct timeval* current_time)
{
    time_t seconds = current_time->tv_sec;
    struct tm utc_time = {0};
    gmtime_r(&seconds, &utc_time);
    ESP_LOGI(TAG, "Clock synchronized: %04d-%02d-%02d %02d:%02d:%02d UTC",
             utc_time.tm_year + 1900, utc_time.tm_mon + 1, utc_time.tm_mday,
             utc_time.tm_hour, utc_time.tm_min, utc_time.tm_sec);
}

esp_err_t time_sync_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    setenv("TZ", "UTC0", 1);
    tzset();

    esp_sntp_config_t configuration = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    configuration.sync_cb = synchronization_callback;
    configuration.smooth_sync = false;
    esp_err_t result = esp_netif_sntp_init(&configuration);
    if (result == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(TAG, "SNTP started; waiting for a valid wall clock");
    }
    return result;
}

bool time_sync_is_valid(void)
{
    time_t current_time = 0;
    time(&current_time);
    return (int64_t)current_time >= MINIMUM_VALID_UNIX_TIME;
}
