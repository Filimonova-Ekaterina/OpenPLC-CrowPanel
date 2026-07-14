#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONFIG_PORTAL_ACCESS_NONE = 0,
    CONFIG_PORTAL_ACCESS_AP,
    CONFIG_PORTAL_ACCESS_LOCAL
} config_portal_access_mode_t;

esp_err_t config_portal_init(void);
void config_portal_deinit(void);
esp_err_t config_portal_start(void);
esp_err_t config_portal_start_runtime(void);
void config_portal_stop(void);
void config_portal_stop_async(void);
esp_err_t config_portal_stop_runtime(void);
bool config_portal_is_running(void);
bool config_portal_is_runtime_mode(void);
config_portal_access_mode_t config_portal_get_access_mode(void);
const char* config_portal_get_ap_ssid(void);
const char* config_portal_get_ap_password(void);
const char* config_portal_get_ap_url(void);
const char* config_portal_get_local_url(void);
const char* config_portal_get_connected_ssid(void);

#ifdef __cplusplus
}
#endif