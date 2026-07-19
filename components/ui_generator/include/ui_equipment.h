#pragma once

/**
 * @file ui_equipment.h
 * @brief Compact equipment and command cards generated from Data Model metadata.
 */

#include "data_model.h"
#include "lvgl.h"
#include "opcua_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_equipment_view ui_equipment_view_t;

typedef enum
{
    UI_EQUIPMENT_PAGE_DETAILS,
    UI_EQUIPMENT_PAGE_CONTROLS,
} ui_equipment_page_mode_t;

/** Create either the integrated Equipment list or the command-focused Controls list. */
ui_equipment_view_t* ui_equipment_create(lv_obj_t* page, data_model_t* data_model, opcua_client_t* opcua_client,
                                         ui_equipment_page_mode_t mode);

/** Refresh all generated values, states, and controls. */
void ui_equipment_update(ui_equipment_view_t* view);

/** Release binding memory before the containing page is rebuilt. */
void ui_equipment_destroy(ui_equipment_view_t* view);

#ifdef __cplusplus
}
#endif
