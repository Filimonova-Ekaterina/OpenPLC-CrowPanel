#pragma once

/**
 * @file trends.h
 * @brief Generic short-history charts generated from numeric Data Model tags.
 */

#include "data_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create a self-owned Trends view. Its timer and memory are released with the root object. */
lv_obj_t* trends_create(lv_obj_t* parent, data_model_t* data_model);

#ifdef __cplusplus
}
#endif
