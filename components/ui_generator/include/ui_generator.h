#pragma once

/**
 * @file ui_generator.h
 * @brief LVGL interface generated exclusively from the discovered Data Model.
 */

#include "data_model.h"
#include "esp_err.h"
#include "lvgl.h"
#include "opcua_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_generator ui_generator_t;

/** Build the UI root and start periodic model-to-widget synchronization. */
esp_err_t ui_generator_create(lv_obj_t* parent, data_model_t* data_model, opcua_client_t* opcua_client,
                              ui_generator_t** generator_out);

#ifdef __cplusplus
}
#endif
