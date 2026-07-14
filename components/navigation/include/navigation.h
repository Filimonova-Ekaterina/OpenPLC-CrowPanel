#pragma once

/**
 * @file navigation.h
 * @brief Small bottom-tab navigation primitive used by generated HMI screens.
 */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct navigation navigation_t;

/** Create navigation owned by the supplied LVGL parent. */
navigation_t* navigation_create(lv_obj_t* parent, lv_coord_t bottom_bar_height);

/** Add a page and a corresponding bottom navigation item. */
lv_obj_t* navigation_add_page(navigation_t* navigation, const char* title);

/** Remove all generated pages while preserving the navigation root. */
void navigation_clear(navigation_t* navigation);

/** Return the LVGL tabview root for layout customization. */
lv_obj_t* navigation_root(navigation_t* navigation);

/** Release the small wrapper after its LVGL parent has deleted the tabview. */
void navigation_release(navigation_t* navigation);

#ifdef __cplusplus
}
#endif
