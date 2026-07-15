#pragma once

/**
 * @file openplc_config_portal.h
 * @brief Local HTTP portal used to configure the OPC UA endpoint.
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t openplc_config_portal_start(void);
void openplc_config_portal_stop(void);
bool openplc_config_portal_is_running(void);
const char* openplc_config_portal_get_local_url(void);
const char* openplc_config_portal_get_connected_ssid(void);

#ifdef __cplusplus
}
#endif
