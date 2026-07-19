#include "esp_log.h"

#include "message_box.h"
#include "password_dialog.h"
#include "settings_events.h"

typedef struct
{
    lv_obj_t* parent;
    lv_obj_t* overlay;
    lv_obj_t* cont;
    lv_obj_t* ta;
    lv_obj_t* keyboard;
} password_dialog_t;

static const char* TAG = "Password dialog";
static password_dialog_t* s_active_dialog = NULL;

static void textarea_event_handler(lv_event_t* e);
static void keyboard_event_cb(lv_event_t* e);
static void cancel_btn_event_cb(lv_event_t* e);
static void confirm_btn_event_cb(lv_event_t* e);
static void password_dialog_close(password_dialog_t* dialog);
static void password_dialog_close_deferred(void* arg);

void password_dialog_create(lv_obj_t* parent, const char* msg)
{
    password_dialog_t* dialog = malloc(sizeof(password_dialog_t));
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

    lv_obj_t* keyboard = lv_keyboard_create(overlay);
    lv_obj_set_size(keyboard, lv_pct(100), lv_pct(50));
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x333333), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);

    lv_obj_t* cont = lv_obj_create(overlay);
    lv_obj_set_size(cont, lv_pct(70), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 20, 0);
    lv_obj_set_align(cont, LV_ALIGN_TOP_MID);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 2, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(cont, 16, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text_fmt(title, "%s", msg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_bottom(title, 10, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t* ta = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "Password");
    lv_obj_set_width(ta, lv_pct(80));

    dialog->parent   = parent;
    dialog->cont     = cont;
    dialog->overlay  = overlay;
    dialog->ta       = ta;
    dialog->keyboard = keyboard;
    s_active_dialog  = dialog;

    lv_obj_add_event_cb(ta, textarea_event_handler, LV_EVENT_ALL, dialog);

    lv_obj_add_state(ta, LV_STATE_FOCUSED);

    lv_obj_t* btn_cont = lv_obj_create(cont);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(btn_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_cont, 0, 0);
    lv_obj_set_style_pad_top(btn_cont, 15, 0);

    lv_obj_t* cancel_btn   = lv_btn_create(btn_cont);
    lv_obj_t* cancel_label = lv_label_create(cancel_btn);
    lv_obj_center(cancel_label);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_width(cancel_btn, 100);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, dialog);

    lv_obj_t* confirm_btn   = lv_btn_create(btn_cont);
    lv_obj_t* confirm_label = lv_label_create(confirm_btn);
    lv_obj_center(confirm_label);
    lv_label_set_text(confirm_label, "Confirm");
    lv_obj_set_width(confirm_btn, 100);
    lv_obj_set_style_bg_opa(confirm_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(confirm_btn, 2, 0);
    lv_obj_set_style_border_color(confirm_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_color(confirm_label, lv_color_white(), 0);
    lv_obj_add_event_cb(confirm_btn, confirm_btn_event_cb, LV_EVENT_CLICKED, dialog);

    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_ALL, dialog);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(keyboard, ta);
}

static void textarea_event_handler(lv_event_t* e)
{
    lv_event_code_t code      = lv_event_get_code(e);
    lv_obj_t* ta              = lv_event_get_target(e);
    password_dialog_t* dialog = lv_event_get_user_data(e);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        if (dialog->keyboard) {
            lv_obj_clear_flag(dialog->keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(dialog->cont);
            lv_obj_move_foreground(dialog->keyboard);
        }
    }
}

static void send_connect_request(const char* password)
{
    app_settings_wifi_connect_data_t payload;
    int n = snprintf(payload.password, sizeof(payload.password), "%s", password);
    if (n > 0) {
        payload.len = n;
        esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_CONNECT_REQ, &payload, sizeof(payload), 0);
    }
}

static void keyboard_event_cb(lv_event_t* e)
{
    lv_event_code_t code      = lv_event_get_code(e);
    password_dialog_t* dialog = lv_event_get_user_data(e);

    if (code == LV_EVENT_READY) {
        const char* pwd = lv_textarea_get_text(dialog->ta);
        ESP_LOGD(TAG, "Entered password: %s", pwd);
        if (pwd[0] != '\0') {
            send_connect_request(pwd);
        } else {
            message_box_create(dialog->parent, "No password provided");
        }

        password_dialog_close(dialog);
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(dialog->keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cancel_btn_event_cb(lv_event_t* e)
{
    lv_obj_t* btn             = lv_event_get_target(e);
    password_dialog_t* dialog = lv_event_get_user_data(e);

    if (dialog) {
        ESP_LOGD(TAG, "Cancelled password input");
        password_dialog_close(dialog);
    }
}

static void confirm_btn_event_cb(lv_event_t* e)
{
    lv_obj_t* btn             = lv_event_get_target(e);
    password_dialog_t* dialog = lv_event_get_user_data(e);

    if (dialog) {
        const char* pwd = lv_textarea_get_text(dialog->ta);
        ESP_LOGD(TAG, "Confirmed password: %s", pwd);
        if (pwd[0] != '\0') {
            send_connect_request(pwd);
        } else {
            message_box_create(dialog->parent, "No password provided");
        }

        password_dialog_close(dialog);
    }
}

static void password_dialog_close(password_dialog_t* dialog)
{
    if (!dialog) return;
    if (s_active_dialog == dialog) s_active_dialog = NULL;

    if (dialog->ta) {
        lv_obj_remove_event_cb(dialog->ta, textarea_event_handler);
    }
    if (dialog->keyboard) {
        lv_keyboard_set_textarea(dialog->keyboard, NULL);
        lv_obj_remove_event_cb(dialog->keyboard, keyboard_event_cb);
    }
    if (dialog->overlay && lv_obj_is_valid(dialog->overlay)) {
        lv_obj_del_async(dialog->overlay);
    }
    free(dialog);
}

static void password_dialog_close_deferred(void* arg)
{
    password_dialog_close((password_dialog_t*)arg);
}

void password_dialog_close_active(void)
{
    if (s_active_dialog) {
        password_dialog_t* dialog = s_active_dialog;
        s_active_dialog = NULL;
        lv_async_call(password_dialog_close_deferred, dialog);
    }
}
