#pragma once

/**
 * @file ui_overview.h
 * @brief Data-driven system overview generated from OPC UA hierarchy and semantics.
 */

#include "data_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_overview ui_overview_t;

/** Create the Overview page and its model bindings. */
ui_overview_t* ui_overview_create(lv_obj_t* page, data_model_t* data_model);

/** Refresh live state and priority signal values. */
void ui_overview_update(ui_overview_t* overview);

/** Release binding memory before the containing LVGL page is rebuilt. */
void ui_overview_destroy(ui_overview_t* overview);

#ifdef __cplusplus
}
#endif
