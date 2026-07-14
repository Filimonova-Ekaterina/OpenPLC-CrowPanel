#include "openplc_settings.h"

#include <stdlib.h>
#include <string.h>

#include "settings_config.h"

typedef struct
{
    lv_obj_t* endpoint_textarea;
    lv_obj_t* status_label;
    openplc_endpoint_apply_callback_t apply_callback;
    void* callback_user_data;
} openplc_settings_context_t;

static void save_endpoint_event(lv_event_t* event)
{
    openplc_settings_context_t* context = lv_event_get_user_data(event);
    const char* endpoint_url            = lv_textarea_get_text(context->endpoint_textarea);
    if (endpoint_url == NULL || strncmp(endpoint_url, "opc.tcp://", 10) != 0) {
        lv_label_set_text(context->status_label, "URL must start with opc.tcp://");
        lv_obj_set_style_text_color(context->status_label, lv_color_hex(0xE63946), LV_PART_MAIN);
        return;
    }

    esp_err_t result = settings_config_save_opcua_endpoint(endpoint_url);
    if (result == ESP_OK && context->apply_callback != NULL) {
        result = context->apply_callback(endpoint_url, context->callback_user_data);
    }
    if (result == ESP_OK) {
        lv_label_set_text(context->status_label, "Saved. OPC UA client is reconnecting...");
        lv_obj_set_style_text_color(context->status_label, lv_color_hex(0x38B000), LV_PART_MAIN);
    } else {
        lv_label_set_text_fmt(context->status_label, "Cannot apply URL: %s", esp_err_to_name(result));
        lv_obj_set_style_text_color(context->status_label, lv_color_hex(0xE63946), LV_PART_MAIN);
    }
}

static void release_context_event(lv_event_t* event)
{
    free(lv_event_get_user_data(event));
}

lv_obj_t* openplc_settings_create(lv_obj_t* parent, const char* endpoint_url,
                                  openplc_endpoint_apply_callback_t apply_callback, void* user_data)
{
    if (parent == NULL || endpoint_url == NULL) {
        return NULL;
    }

    openplc_settings_context_t* context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->apply_callback     = apply_callback;
    context->callback_user_data = user_data;

    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 16, LV_PART_MAIN);
    lv_obj_add_event_cb(root, release_context_event, LV_EVENT_DELETE, context);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "OpenPLC OPC UA server");
    lv_obj_set_style_text_color(title, lv_color_hex(0xD8E8F2), LV_PART_MAIN);

    lv_obj_t* description = lv_label_create(root);
    lv_label_set_text(description, "Enter the complete OPC UA endpoint URL used by this panel.");
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(description, lv_pct(100));
    lv_obj_set_style_text_color(description, lv_color_hex(0x8FAAB8), LV_PART_MAIN);

    context->endpoint_textarea = lv_textarea_create(root);
    lv_textarea_set_one_line(context->endpoint_textarea, true);
    lv_textarea_set_text(context->endpoint_textarea, endpoint_url);
    lv_obj_set_width(context->endpoint_textarea, lv_pct(100));

    lv_obj_t* save_button = lv_btn_create(root);
    lv_obj_set_size(save_button, 180, 48);
    lv_obj_add_event_cb(save_button, save_endpoint_event, LV_EVENT_CLICKED, context);
    lv_obj_t* save_label = lv_label_create(save_button);
    lv_label_set_text(save_label, "Save and connect");
    lv_obj_center(save_label);

    context->status_label = lv_label_create(root);
    lv_label_set_text(context->status_label, "");
    lv_obj_set_width(context->status_label, lv_pct(100));

    lv_obj_t* keyboard = lv_keyboard_create(root);
    lv_obj_set_width(keyboard, lv_pct(100));
    lv_obj_set_flex_grow(keyboard, 1);
    lv_keyboard_set_textarea(keyboard, context->endpoint_textarea);
    return root;
}
