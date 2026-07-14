#include "navigation.h"

#include <stdlib.h>

struct navigation
{
    lv_obj_t* tabview;
    lv_coord_t bottom_bar_height;
};

navigation_t* navigation_create(lv_obj_t* parent, lv_coord_t bottom_bar_height)
{
    if (parent == NULL) {
        return NULL;
    }
    navigation_t* navigation = calloc(1, sizeof(*navigation));
    if (navigation == NULL) {
        return NULL;
    }

    navigation->bottom_bar_height = bottom_bar_height;
    navigation->tabview           = lv_tabview_create(parent, LV_DIR_BOTTOM, bottom_bar_height);
    lv_obj_set_size(navigation->tabview, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(navigation->tabview, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t* tab_buttons = lv_tabview_get_tab_btns(navigation->tabview);
    lv_obj_set_style_bg_color(tab_buttons, lv_color_hex(0x17232D), LV_PART_MAIN);
    lv_obj_set_style_text_color(tab_buttons, lv_color_hex(0xD8E8F2), LV_PART_ITEMS);
    return navigation;
}

lv_obj_t* navigation_add_page(navigation_t* navigation, const char* title)
{
    if (navigation == NULL || navigation->tabview == NULL || title == NULL) {
        return NULL;
    }
    lv_obj_t* page = lv_tabview_add_tab(navigation->tabview, title);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 12, LV_PART_MAIN);
    return page;
}

void navigation_clear(navigation_t* navigation)
{
    if (navigation == NULL || navigation->tabview == NULL) {
        return;
    }
    lv_obj_t* parent = lv_obj_get_parent(navigation->tabview);
    lv_obj_del(navigation->tabview);
    navigation->tabview = lv_tabview_create(parent, LV_DIR_BOTTOM, navigation->bottom_bar_height);
    lv_obj_set_size(navigation->tabview, lv_pct(100), lv_pct(100));
}

lv_obj_t* navigation_root(navigation_t* navigation)
{
    return navigation != NULL ? navigation->tabview : NULL;
}

void navigation_release(navigation_t* navigation)
{
    free(navigation);
}
