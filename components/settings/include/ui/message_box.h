#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void message_box_create(lv_obj_t* parent, const char* msg);

#ifdef __cplusplus
}
#endif
