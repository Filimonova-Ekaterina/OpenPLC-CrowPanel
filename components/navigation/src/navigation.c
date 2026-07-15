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
    lv_obj_set_style_bg_color(navigation->tabview, lv_color_black(), LV_PART_MAIN);

    lv_obj_t* tab_buttons = lv_tabview_get_tab_btns(navigation->tabview);
    lv_obj_set_style_bg_color(tab_buttons, lv_color_hex(0x181818), LV_PART_MAIN);
    lv_obj_set_style_text_color(tab_buttons, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(tab_buttons, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(tab_buttons, lv_color_hex(0x111111), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(tab_buttons, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(tab_buttons, lv_color_hex(0x4A4A4A), LV_PART_ITEMS);
    lv_obj_set_style_border_width(tab_buttons, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_opa(tab_buttons, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_side(tab_buttons, LV_BORDER_SIDE_FULL, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(tab_buttons, lv_color_hex(0x184F73), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(tab_buttons, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tab_buttons, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tab_buttons, lv_color_hex(0x6BC1FF), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(tab_buttons, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(tab_buttons, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(tab_buttons, 14, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(tab_buttons, 8, LV_PART_ITEMS);
    return navigation;
}

lv_obj_t* navigation_add_page(navigation_t* navigation, const char* title)
{
    if (navigation == NULL || navigation->tabview == NULL || title == NULL) {
        return NULL;
    }
    lv_obj_t* page = lv_tabview_add_tab(navigation->tabview, title);
    lv_obj_set_style_bg_color(page, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 18, LV_PART_MAIN);
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
