#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void password_dialog_create(lv_obj_t* parent, const char* msg);
void password_dialog_close_active(void);

#ifdef __cplusplus
}
#endif
