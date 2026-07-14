#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t* wifi_menu_create(lv_obj_t* parent, lv_obj_t* top_layer);
void wifi_menu_activate(void);
void wifi_menu_deactivate(void);

#ifdef __cplusplus
}
#endif
