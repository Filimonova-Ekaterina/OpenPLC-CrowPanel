#include "openplc_settings.h"

#include <stdlib.h>
#include <string.h>

#include "openplc_config_portal.h"
#include "settings_config.h"
#include "settings_events.h"
#include "uart_handler.h"
#include "wifi_ctrl.h"

typedef struct
{
    lv_obj_t* root;
    lv_obj_t* endpoint_label;
    lv_obj_t* endpoint_status;
    lv_obj_t* portal_info_label;
    lv_obj_t* portal_qr_frame;
    lv_obj_t* portal_qr;
    lv_obj_t* dialog;
    lv_obj_t* dialog_textarea;
    lv_obj_t* dialog_status;
    lv_obj_t* dialog_keyboard;
    lv_timer_t* portal_timer;
    esp_event_handler_instance_t endpoint_event_handler;
    openplc_endpoint_apply_callback_t apply_callback;
    void* callback_user_data;
    char current_endpoint[SETTINGS_OPCUA_ENDPOINT_LENGTH];
    bool active;
} openplc_settings_context_t;

typedef struct
{
    openplc_settings_context_t* context;
    char endpoint[SETTINGS_OPCUA_ENDPOINT_LENGTH];
} endpoint_async_update_t;

static openplc_settings_context_t* s_context;
static lv_style_t s_status_default_style;
static lv_style_t s_status_configured_style;
static bool s_styles_initialized;

static void initialize_styles(void)
{
    if (s_styles_initialized) return;

    lv_style_init(&s_status_default_style);
    lv_style_set_bg_color(&s_status_default_style, lv_color_hex(0x0D6EFD));
    lv_style_set_bg_opa(&s_status_default_style, LV_OPA_COVER);
    lv_style_set_radius(&s_status_default_style, 12);
    lv_style_set_pad_hor(&s_status_default_style, 12);
    lv_style_set_pad_ver(&s_status_default_style, 6);
    lv_style_set_text_color(&s_status_default_style, lv_color_white());
    lv_style_set_text_font(&s_status_default_style, &lv_font_montserrat_14);

    lv_style_init(&s_status_configured_style);
    lv_style_set_bg_color(&s_status_configured_style, lv_color_hex(0x198754));
    lv_style_set_bg_opa(&s_status_configured_style, LV_OPA_COVER);
    lv_style_set_radius(&s_status_configured_style, 12);
    lv_style_set_pad_hor(&s_status_configured_style, 12);
    lv_style_set_pad_ver(&s_status_configured_style, 6);
    lv_style_set_text_color(&s_status_configured_style, lv_color_white());
    lv_style_set_text_font(&s_status_configured_style, &lv_font_montserrat_14);
    s_styles_initialized = true;
}

static void update_endpoint_card(openplc_settings_context_t* context)
{
    lv_label_set_text(context->endpoint_label, context->current_endpoint);
    lv_obj_remove_style(context->endpoint_status, &s_status_default_style, LV_PART_MAIN);
    lv_obj_remove_style(context->endpoint_status, &s_status_configured_style, LV_PART_MAIN);
    if (settings_config_has_saved_opcua_endpoint()) {
        lv_label_set_text(context->endpoint_status, "CONFIGURED");
        lv_obj_add_style(context->endpoint_status, &s_status_configured_style, LV_PART_MAIN);
    } else {
        lv_label_set_text(context->endpoint_status, "DEFAULT");
        lv_obj_add_style(context->endpoint_status, &s_status_default_style, LV_PART_MAIN);
    }
}

static esp_err_t save_and_apply_endpoint(openplc_settings_context_t* context, const char* endpoint)
{
    if (endpoint == NULL || strncmp(endpoint, "opc.tcp://", 10) != 0 ||
        strlen(endpoint) >= sizeof(context->current_endpoint)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = settings_config_save_opcua_endpoint(endpoint);
    if (result == ESP_OK && context->apply_callback != NULL) {
        result = context->apply_callback(endpoint, context->callback_user_data);
    }
    if (result == ESP_OK) {
        strlcpy(context->current_endpoint, endpoint, sizeof(context->current_endpoint));
        update_endpoint_card(context);
    }
    return result;
}

static void close_dialog(openplc_settings_context_t* context)
{
    if (context->dialog != NULL && lv_obj_is_valid(context->dialog)) {
        lv_obj_del(context->dialog);
    }
    context->dialog          = NULL;
    context->dialog_textarea = NULL;
    context->dialog_status   = NULL;
    context->dialog_keyboard = NULL;
}

static void dialog_cancel_event(lv_event_t* event)
{
    close_dialog(lv_event_get_user_data(event));
}

static void dialog_save_event(lv_event_t* event)
{
    openplc_settings_context_t* context = lv_event_get_user_data(event);
    const char* endpoint = lv_textarea_get_text(context->dialog_textarea);
    esp_err_t result = save_and_apply_endpoint(context, endpoint);
    if (result == ESP_OK) {
        close_dialog(context);
    } else {
        lv_label_set_text(context->dialog_status, "Endpoint must start with opc.tcp://");
        lv_obj_set_style_text_color(context->dialog_status, lv_color_hex(0xE63946), LV_PART_MAIN);
    }
}

static void dialog_keyboard_event(lv_event_t* event)
{
    openplc_settings_context_t* context = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) dialog_save_event(event);
    else if (code == LV_EVENT_CANCEL) close_dialog(context);
}

static void endpoint_card_event(lv_event_t* event)
{
    openplc_settings_context_t* context = lv_event_get_user_data(event);
    if (context->dialog != NULL) return;

    context->dialog = lv_obj_create(lv_obj_get_screen(context->root));
    lv_obj_set_size(context->dialog, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(context->dialog, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(context->dialog, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(context->dialog, 0, LV_PART_MAIN);
    lv_obj_clear_flag(context->dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(context->dialog);
    lv_obj_set_size(panel, lv_pct(78), lv_pct(46));
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "OPC UA server endpoint");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);

    context->dialog_textarea = lv_textarea_create(panel);
    lv_textarea_set_one_line(context->dialog_textarea, true);
    lv_textarea_set_text(context->dialog_textarea, context->current_endpoint);
    lv_obj_set_size(context->dialog_textarea, lv_pct(100), 62);
    lv_obj_set_style_text_font(context->dialog_textarea, &lv_font_montserrat_20, LV_PART_MAIN);

    context->dialog_status = lv_label_create(panel);
    lv_label_set_text(context->dialog_status, "");
    lv_obj_set_style_text_font(context->dialog_status, &lv_font_montserrat_16, LV_PART_MAIN);

    lv_obj_t* button_row = lv_obj_create(panel);
    lv_obj_remove_style_all(button_row);
    lv_obj_set_size(button_row, lv_pct(100), 58);
    lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(button_row, 12, LV_PART_MAIN);

    lv_obj_t* cancel_button = lv_btn_create(button_row);
    lv_obj_set_size(cancel_button, 130, 54);
    lv_obj_set_style_bg_opa(cancel_button, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cancel_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(cancel_button, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_button, dialog_cancel_event, LV_EVENT_CLICKED, context);
    lv_obj_t* cancel_label = lv_label_create(cancel_button);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);

    lv_obj_t* save_button = lv_btn_create(button_row);
    lv_obj_set_size(save_button, 130, 54);
    lv_obj_set_style_bg_color(save_button, lv_color_hex(0x2A96FF), LV_PART_MAIN);
    lv_obj_add_event_cb(save_button, dialog_save_event, LV_EVENT_CLICKED, context);
    lv_obj_t* save_label = lv_label_create(save_button);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);

    /* Match the Wi-Fi settings: the keyboard is a separate bottom sheet,
     * not a child embedded inside the edit dialog. */
    context->dialog_keyboard = lv_keyboard_create(context->dialog);
    lv_obj_set_size(context->dialog_keyboard, lv_pct(100), lv_pct(50));
    lv_obj_align(context->dialog_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(context->dialog_keyboard, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(context->dialog_keyboard, lv_color_hex(0x333333), LV_PART_ITEMS);
    lv_obj_set_style_text_color(context->dialog_keyboard, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(context->dialog_keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_textarea(context->dialog_keyboard, context->dialog_textarea);
    lv_obj_add_event_cb(context->dialog_keyboard, dialog_keyboard_event, LV_EVENT_ALL, context);
}

static lv_obj_t* create_endpoint_card(openplc_settings_context_t* context, lv_obj_t* parent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), 105);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 30, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, endpoint_card_event, LV_EVENT_CLICKED, context);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "Server URL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 5);

    context->endpoint_label = lv_label_create(card);
    lv_obj_set_width(context->endpoint_label, lv_pct(78));
    lv_label_set_long_mode(context->endpoint_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(context->endpoint_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(context->endpoint_label, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
    lv_obj_align(context->endpoint_label, LV_ALIGN_BOTTOM_LEFT, 0, -4);

    context->endpoint_status = lv_label_create(card);
    lv_obj_align(context->endpoint_status, LV_ALIGN_RIGHT_MID, 0, 0);
    update_endpoint_card(context);
    return card;
}

static lv_obj_t* create_portal_card(openplc_settings_context_t* context, lv_obj_t* parent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), 165);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "Setup via QR portal");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 0);

    lv_obj_t* description = lv_label_create(card);
    lv_label_set_text(description, "Scan the QR code to open the local\nOpenPLC setup page on your phone.");
    lv_obj_set_style_text_font(description, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(description, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
    lv_obj_align(description, LV_ALIGN_LEFT_MID, 12, 14);

    context->portal_info_label = lv_label_create(card);
    lv_obj_set_width(context->portal_info_label, 220);
    lv_label_set_long_mode(context->portal_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(context->portal_info_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(context->portal_info_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(context->portal_info_label, lv_color_white(), LV_PART_MAIN);

    context->portal_qr_frame = lv_obj_create(card);
    lv_obj_set_size(context->portal_qr_frame, 125, 125);
    lv_obj_set_style_bg_color(context->portal_qr_frame, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_radius(context->portal_qr_frame, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(context->portal_qr_frame, 6, LV_PART_MAIN);
    lv_obj_align(context->portal_qr_frame, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_align_to(context->portal_info_label, context->portal_qr_frame,
                    LV_ALIGN_OUT_LEFT_MID, -18, 0);
#if LV_USE_QRCODE
    context->portal_qr = lv_qrcode_create(context->portal_qr_frame, 108, lv_color_black(), lv_color_white());
    lv_obj_center(context->portal_qr);
#endif
    return card;
}

static lv_obj_t* create_uart_card(lv_obj_t* parent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), 120);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 30, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "Setup via UART / PC app");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t* description = lv_label_create(card);
    lv_label_set_text(description,
        "Send: {\"type\":\"openplc_config\",\"endpoint\":\"opc.tcp://192.168.1.10:4840\"}");
    lv_obj_set_width(description, lv_pct(100));
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(description, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(description, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
    lv_obj_align(description, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    return card;
}

static void refresh_portal(openplc_settings_context_t* context)
{
    if (! context->active) return;
    bool wifi_connected = wifi_ctrl_is_connected();
    if (! wifi_connected && openplc_config_portal_is_running()) {
        openplc_config_portal_stop();
    } else if (! openplc_config_portal_is_running() && wifi_connected) {
        (void)openplc_config_portal_start();
    }

    const char* local_url = openplc_config_portal_get_local_url();
    const char* ssid = openplc_config_portal_get_connected_ssid();
    if (local_url != NULL) {
        lv_label_set_text_fmt(context->portal_info_label, "Connected to\n%s\n\nSetup Page\n%s",
                              ssid != NULL ? ssid : "Wi-Fi", local_url + (strncmp(local_url, "http://", 7) == 0 ? 7 : 0));
        lv_obj_clear_flag(context->portal_qr_frame, LV_OBJ_FLAG_HIDDEN);
#if LV_USE_QRCODE
        lv_qrcode_update(context->portal_qr, local_url, strlen(local_url));
#endif
    } else {
        lv_label_set_text(context->portal_info_label,
                          wifi_connected ? "Starting setup portal..." : "Connect to Wi-Fi first");
        lv_obj_add_flag(context->portal_qr_frame, LV_OBJ_FLAG_HIDDEN);
    }
    /* The label height changes between one and five lines. Re-align after
     * updating the text so its visual centre stays level with the QR code. */
    lv_obj_update_layout(context->portal_info_label);
    lv_obj_align_to(context->portal_info_label, context->portal_qr_frame,
                    LV_ALIGN_OUT_LEFT_MID, -18, 0);
}

static void portal_timer_callback(lv_timer_t* timer)
{
    refresh_portal(timer->user_data);
}

static void apply_received_endpoint_async(void* argument)
{
    endpoint_async_update_t* update = argument;
    if (s_context == update->context) {
        esp_err_t result = save_and_apply_endpoint(update->context, update->endpoint);
        if (result != ESP_OK) {
            lv_label_set_text(update->context->endpoint_status, "ERROR");
        }
    }
    free(update);
}

static void endpoint_event_handler(void* argument, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    (void)event_base;
    if (event_id != APP_SETTINGS_UART_EVENT_RECEIVED_OPCUA_ENDPOINT || event_data == NULL) return;
    app_settings_uart_opcua_endpoint_data_t* payload = event_data;
    endpoint_async_update_t* update = calloc(1, sizeof(*update));
    if (update == NULL) return;
    update->context = argument;
    strlcpy(update->endpoint, payload->endpoint, sizeof(update->endpoint));
    lv_async_call(apply_received_endpoint_async, update);
}

static void release_context_event(lv_event_t* event)
{
    openplc_settings_context_t* context = lv_event_get_user_data(event);
    openplc_settings_deactivate();
    if (context->portal_timer != NULL) lv_timer_del(context->portal_timer);
    if (context->endpoint_event_handler != NULL) {
        esp_event_handler_instance_unregister(APP_SETTINGS_UART_EVENTS,
                                              APP_SETTINGS_UART_EVENT_RECEIVED_OPCUA_ENDPOINT,
                                              context->endpoint_event_handler);
    }
    close_dialog(context);
    if (s_context == context) s_context = NULL;
    free(context);
}

lv_obj_t* openplc_settings_create(lv_obj_t* parent, const char* endpoint_url,
                                  openplc_endpoint_apply_callback_t apply_callback, void* user_data)
{
    if (parent == NULL || endpoint_url == NULL) return NULL;
    initialize_styles();

    openplc_settings_context_t* context = calloc(1, sizeof(*context));
    if (context == NULL) return NULL;
    context->apply_callback = apply_callback;
    context->callback_user_data = user_data;
    strlcpy(context->current_endpoint, endpoint_url, sizeof(context->current_endpoint));
    s_context = context;

    context->root = lv_obj_create(parent);
    lv_obj_remove_style_all(context->root);
    lv_obj_set_size(context->root, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(context->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(context->root, 15, LV_PART_MAIN);
    lv_obj_set_style_pad_row(context->root, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(context->root, release_context_event, LV_EVENT_DELETE, context);

    create_endpoint_card(context, context->root);
    create_portal_card(context, context->root);
    create_uart_card(context->root);

    context->portal_timer = lv_timer_create(portal_timer_callback, 750, context);
    esp_event_handler_instance_register(APP_SETTINGS_UART_EVENTS,
                                        APP_SETTINGS_UART_EVENT_RECEIVED_OPCUA_ENDPOINT,
                                        endpoint_event_handler, context, &context->endpoint_event_handler);
    return context->root;
}

void openplc_settings_activate(void)
{
    if (s_context == NULL || s_context->active) return;
    s_context->active = true;
    if (! uart_password_handler_is_initialized()) uart_password_handler_init(NULL);
    uart_password_handler_start();
    (void)openplc_config_portal_start();
    refresh_portal(s_context);
}

void openplc_settings_deactivate(void)
{
    if (s_context == NULL || ! s_context->active) return;
    s_context->active = false;
    openplc_config_portal_stop();
    uart_password_handler_stop();
}
