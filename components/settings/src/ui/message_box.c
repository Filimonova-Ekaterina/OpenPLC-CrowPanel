#include "esp_log.h"

#include "message_box.h"

typedef struct
{
    lv_obj_t* overlay;
    lv_obj_t* cont;
} message_box_t;

static const char* TAG = "Message box";

static void confirm_btn_event_cb(lv_event_t* e);
static void message_box_close(message_box_t* dialog);

void message_box_create(lv_obj_t* parent, const char* msg)
{
    message_box_t* dialog = malloc(sizeof(message_box_t));
    if (! dialog) {
        return;
    }

    lv_obj_t* overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_top(overlay, 30, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cont = lv_obj_create(overlay);
    lv_obj_set_size(cont, lv_pct(70), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 20, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 2, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(cont, 16, 0);
    lv_obj_set_align(cont, LV_ALIGN_CENTER);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text_fmt(title, "%s", msg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_pad_bottom(title, 10, 0);

    dialog->cont    = cont;
    dialog->overlay = overlay;

    lv_obj_t* btn_cont = lv_obj_create(cont);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(btn_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_cont, 0, 0);
    lv_obj_set_style_pad_top(btn_cont, 15, 0);

    lv_obj_t* confirm_btn   = lv_btn_create(btn_cont);
    lv_obj_t* confirm_label = lv_label_create(confirm_btn);
    lv_obj_center(confirm_label);
    lv_label_set_text(confirm_label, "Ok");
    lv_obj_set_style_bg_opa(confirm_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(confirm_btn, 2, 0);
    lv_obj_set_style_border_color(confirm_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_color(confirm_label, lv_color_white(), 0);
    lv_obj_set_width(confirm_btn, 100);
    lv_obj_add_event_cb(confirm_btn, confirm_btn_event_cb, LV_EVENT_CLICKED, dialog);
}

static void confirm_btn_event_cb(lv_event_t* e)
{
    lv_obj_t* btn         = lv_event_get_target(e);
    message_box_t* dialog = lv_event_get_user_data(e);

    if (dialog) {
        message_box_close(dialog);
    }
}

static void message_box_close(message_box_t* dialog)
{
    if (!dialog) return;
    if (dialog->overlay && lv_obj_is_valid(dialog->overlay)) {
        lv_obj_del_async(dialog->overlay);
    }
    free(dialog);
}
