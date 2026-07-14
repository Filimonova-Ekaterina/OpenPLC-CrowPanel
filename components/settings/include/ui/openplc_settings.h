#pragma once

/**
 * @file openplc_settings.h
 * @brief OpenPLC OPC UA endpoint editor used by the generated Settings screen.
 */

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*openplc_endpoint_apply_callback_t)(const char* endpoint_url, void* user_data);

/** Create an endpoint editor which persists the URL and invokes the apply callback. */
lv_obj_t* openplc_settings_create(lv_obj_t* parent, const char* endpoint_url,
                                  openplc_endpoint_apply_callback_t apply_callback, void* user_data);

#ifdef __cplusplus
}
#endif
