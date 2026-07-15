#pragma once

/**
 * @file system_settings.h
 * @brief System settings UI and application of saved runtime values.
 */

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the System settings page. */
lv_obj_t* system_settings_create(lv_obj_t* parent);

/** Load brightness, volume and sleep timeout from NVS and apply them. */
esp_err_t system_settings_apply_saved(void);

/** Apply saved values from a dedicated task with enough stack for NVS/audio. */
esp_err_t system_settings_restore_async(void);

#ifdef __cplusplus
}
#endif
