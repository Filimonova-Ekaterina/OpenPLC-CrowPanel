#pragma once

/**
 * @file wifi_widget.h
 * @brief Standalone LVGL Wi-Fi status indicator for the persistent HMI header.
 */

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wifi_widget wifi_widget_t;

/** Create a non-clickable Wi-Fi indicator which refreshes itself periodically. */
esp_err_t wifi_widget_create(lv_obj_t* parent, wifi_widget_t** widget_out);

#ifdef __cplusplus
}
#endif
