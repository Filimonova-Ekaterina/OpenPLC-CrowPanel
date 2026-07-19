#include "ui_generator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "navigation.h"
#include "openplc_settings.h"
#include "system_settings.h"
#include "trends.h"
#include "ui_equipment.h"
#include "ui_overview.h"
#include "ui_theme.h"
#include "wifi_menu.h"
#include "wifi_widget.h"

#define UI_REFRESH_PERIOD_MS   250
#define UI_NAVIGATION_HEIGHT   76
#define UI_PAGE_COUNT          5
#define SETTINGS_SECTION_COUNT 3

enum
{
    SETTINGS_SECTION_SYSTEM = 0,
    SETTINGS_SECTION_WIFI,
    SETTINGS_SECTION_OPENPLC,
};

struct ui_generator
{
    lv_obj_t* root;
    lv_obj_t* status_indicator;
    lv_obj_t* status_label;
    lv_obj_t* settings_button;
    lv_obj_t* settings_overlay;
    lv_obj_t* settings_pages[SETTINGS_SECTION_COUNT];
    lv_obj_t* settings_section_buttons[SETTINGS_SECTION_COUNT];
    lv_obj_t* content_host;
    lv_obj_t* alarms_host;
    wifi_widget_t* wifi_widget;
    navigation_t* navigation;
    lv_timer_t* refresh_timer;
    data_model_t* data_model;
    opcua_client_t* opcua_client;
    ui_overview_t* overview;
    ui_equipment_view_t* equipment_view;
    ui_equipment_view_t* controls_view;
    uint32_t observed_structure_generation;
    uint32_t observed_value_generation;
    uint32_t observed_alarm_generation;
    bool status_display_valid;
    opcua_client_state_t displayed_client_state;
    char displayed_status_text[128];
};

static const char* TAG = "ui_generator";
extern const lv_img_dsc_t settings_icon;

static void refresh_timer_callback(lv_timer_t* timer);
static void rebuild_interface(ui_generator_t* generator);
static void rebuild_alarm_list(ui_generator_t* generator);
static void create_alarm_clear_state(lv_obj_t* parent);
static void open_settings_event(lv_event_t* event);
static void close_settings_event(lv_event_t* event);
static void settings_overlay_deleted_event(lv_event_t* event);
static void settings_section_event(lv_event_t* event);
static void show_settings_section(ui_generator_t* generator, unsigned selected_section);
static lv_obj_t* create_settings_section_button(lv_obj_t* parent, const char* title);
static esp_err_t apply_opcua_endpoint(const char* endpoint_url, void* user_data);

esp_err_t ui_generator_create(lv_obj_t* parent, data_model_t* data_model, opcua_client_t* opcua_client,
                              ui_generator_t** generator_out)
{
    if (parent == NULL || data_model == NULL || opcua_client == NULL || generator_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ui_generator_t* generator = calloc(1, sizeof(*generator));
    if (generator == NULL) {
        return ESP_ERR_NO_MEM;
    }
    generator->data_model   = data_model;
    generator->opcua_client = opcua_client;

    generator->root = lv_obj_create(parent);
    lv_obj_remove_style_all(generator->root);
    lv_obj_set_size(generator->root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(generator->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(generator->root, lv_color_hex(UI_COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(generator->root, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* status_bar = lv_obj_create(generator->root);
    lv_obj_set_width(status_bar, lv_pct(100));
    lv_obj_set_height(status_bar, 76);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x11171C), LV_PART_MAIN);
    lv_obj_set_style_pad_left(status_bar, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_right(status_bar, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_top(status_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(status_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(status_bar, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    generator->status_indicator = lv_obj_create(status_bar);
    lv_obj_remove_style_all(generator->status_indicator);
    lv_obj_set_size(generator->status_indicator, 20, 20);
    lv_obj_set_style_radius(generator->status_indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(generator->status_indicator, LV_OPA_COVER, LV_PART_MAIN);

    generator->status_label = lv_label_create(status_bar);
    lv_label_set_text(generator->status_label, "Starting OPC UA client...");
    lv_obj_set_style_text_color(generator->status_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(generator->status_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_long_mode(generator->status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(generator->status_label, 1);

    esp_err_t wifi_result = wifi_widget_create(status_bar, &generator->wifi_widget);
    if (wifi_result != ESP_OK) {
        ESP_LOGW(TAG, "Cannot create Wi-Fi indicator: %s", esp_err_to_name(wifi_result));
    }

    generator->settings_button = lv_btn_create(status_bar);
    lv_obj_set_size(generator->settings_button, 58, 54);
    lv_obj_set_style_radius(generator->settings_button, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(generator->settings_button, lv_color_hex(0x20282E), LV_PART_MAIN);
    lv_obj_set_style_border_width(generator->settings_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(generator->settings_button, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(generator->settings_button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(generator->settings_button, open_settings_event, LV_EVENT_CLICKED, generator);
    lv_obj_t* settings_icon_image = lv_img_create(generator->settings_button);
    lv_img_set_src(settings_icon_image, &settings_icon);
    lv_obj_center(settings_icon_image);

    generator->content_host = lv_obj_create(generator->root);
    lv_obj_remove_style_all(generator->content_host);
    lv_obj_set_width(generator->content_host, lv_pct(100));
    lv_obj_set_flex_grow(generator->content_host, 1);

    generator->observed_structure_generation = UINT32_MAX;
    rebuild_interface(generator);
    generator->refresh_timer = lv_timer_create(refresh_timer_callback, UI_REFRESH_PERIOD_MS, generator);
    if (generator->refresh_timer == NULL) {
        ui_overview_destroy(generator->overview);
        ui_equipment_destroy(generator->equipment_view);
        ui_equipment_destroy(generator->controls_view);
        free(generator);
        return ESP_ERR_NO_MEM;
    }
    *generator_out = generator;
    return ESP_OK;
}

static void refresh_timer_callback(lv_timer_t* timer)
{
    ui_generator_t* generator     = timer->user_data;
    uint32_t structure_generation = data_model_structure_generation(generator->data_model);
    if (structure_generation != generator->observed_structure_generation) {
        rebuild_interface(generator);
    }

    opcua_client_state_t state = opcua_client_get_state(generator->opcua_client);
    char status_text[128];
    if (state == OPCUA_CLIENT_CONNECTED) {
        snprintf(status_text, sizeof(status_text), "CONNECTED  |  %u objects  |  %u tags",
                 (unsigned)data_model_equipment_count(generator->data_model),
                 (unsigned)data_model_tag_count(generator->data_model));
    } else {
        opcua_client_get_status(generator->opcua_client, status_text, sizeof(status_text));
    }
    if (strcmp(generator->displayed_status_text, status_text) != 0) {
        lv_label_set_text(generator->status_label, status_text);
        snprintf(generator->displayed_status_text, sizeof(generator->displayed_status_text), "%s", status_text);
    }
    if (! generator->status_display_valid || state != generator->displayed_client_state) {
        lv_color_t color = lv_color_hex(UI_COLOR_WARNING);
        if (state == OPCUA_CLIENT_CONNECTED) {
            color = lv_color_hex(UI_COLOR_SUCCESS);
        } else if (state == OPCUA_CLIENT_CONNECTION_ERROR || state == OPCUA_CLIENT_BROWSE_ERROR) {
            color = lv_color_hex(UI_COLOR_DANGER);
        }
        lv_obj_set_style_bg_color(generator->status_indicator, color, LV_PART_MAIN);
        generator->displayed_client_state = state;
        generator->status_display_valid   = true;
    }

    uint32_t alarm_generation = data_model_alarm_generation(generator->data_model);
    if (alarm_generation != generator->observed_alarm_generation) {
        generator->observed_alarm_generation = alarm_generation;
        rebuild_alarm_list(generator);
        ui_overview_update(generator->overview);
    }

    uint32_t value_generation = data_model_value_generation(generator->data_model);
    if (value_generation == generator->observed_value_generation) {
        return;
    }
    generator->observed_value_generation = value_generation;
    ui_overview_update(generator->overview);
    ui_equipment_update(generator->equipment_view);
    ui_equipment_update(generator->controls_view);
}

static void rebuild_interface(ui_generator_t* generator)
{
    uint16_t selected_tab = 0;
    if (generator->navigation != NULL && navigation_root(generator->navigation) != NULL) {
        selected_tab = lv_tabview_get_tab_act(navigation_root(generator->navigation));
    }

    generator->observed_structure_generation = data_model_structure_generation(generator->data_model);
    generator->observed_value_generation     = UINT32_MAX;
    generator->observed_alarm_generation     = UINT32_MAX;

    ui_overview_destroy(generator->overview);
    ui_equipment_destroy(generator->equipment_view);
    ui_equipment_destroy(generator->controls_view);
    generator->overview       = NULL;
    generator->equipment_view = NULL;
    generator->controls_view  = NULL;
    generator->alarms_host    = NULL;

    navigation_release(generator->navigation);
    generator->navigation = NULL;
    lv_obj_clean(generator->content_host);
    generator->navigation = navigation_create(generator->content_host, UI_NAVIGATION_HEIGHT);
    if (generator->navigation == NULL) {
        ESP_LOGE(TAG, "Cannot allocate navigation");
        return;
    }

    lv_obj_t* overview_page = navigation_add_page(generator->navigation, LV_SYMBOL_HOME "  Overview");
    ui_theme_style_page(overview_page);
    generator->overview = ui_overview_create(overview_page, generator->data_model);
    if (generator->overview == NULL) {
        ui_theme_create_empty_state(overview_page, "Cannot create Overview", "Not enough memory for generated bindings",
                                    lv_color_hex(UI_COLOR_WARNING));
    }

    lv_obj_t* equipment_page = navigation_add_page(generator->navigation, LV_SYMBOL_LIST "  Equipment");
    ui_theme_style_page(equipment_page);
    generator->equipment_view =
        ui_equipment_create(equipment_page, generator->data_model, generator->opcua_client, UI_EQUIPMENT_PAGE_DETAILS);
    if (generator->equipment_view == NULL) {
        ui_theme_create_empty_state(equipment_page, "Cannot create Equipment",
                                    "Not enough memory for generated bindings", lv_color_hex(UI_COLOR_WARNING));
    }

    lv_obj_t* trends_page = navigation_add_page(generator->navigation, LV_SYMBOL_CHARGE "  Trends");
    ui_theme_style_page(trends_page);
    if (trends_create(trends_page, generator->data_model) == NULL) {
        ui_theme_create_empty_state(trends_page, "Cannot create trend charts", "Not enough memory for the Trends view",
                                    lv_color_hex(UI_COLOR_WARNING));
    }

    lv_obj_t* controls_page = navigation_add_page(generator->navigation, LV_SYMBOL_EDIT "  Controls");
    ui_theme_style_page(controls_page);
    generator->controls_view =
        ui_equipment_create(controls_page, generator->data_model, generator->opcua_client, UI_EQUIPMENT_PAGE_CONTROLS);
    if (generator->controls_view == NULL) {
        ui_theme_create_empty_state(controls_page, "Cannot create Controls", "Not enough memory for generated bindings",
                                    lv_color_hex(UI_COLOR_WARNING));
    }

    lv_obj_t* alarms_page = navigation_add_page(generator->navigation, LV_SYMBOL_WARNING "  Alarms");
    ui_theme_style_page(alarms_page);
    generator->alarms_host = alarms_page;
    rebuild_alarm_list(generator);

    if (selected_tab < UI_PAGE_COUNT) {
        lv_tabview_set_act(navigation_root(generator->navigation), selected_tab, LV_ANIM_OFF);
    }

    ESP_LOGI(TAG, "Generated UI for %u equipment objects and %u tags",
             (unsigned)data_model_equipment_count(generator->data_model),
             (unsigned)data_model_tag_count(generator->data_model));
}

static void rebuild_alarm_list(ui_generator_t* generator)
{
    if (generator == NULL || generator->alarms_host == NULL) {
        return;
    }
    lv_obj_clean(generator->alarms_host);
    size_t equipment_count = data_model_equipment_count(generator->data_model);
    if (equipment_count == 0) {
        ui_theme_create_heading(generator->alarms_host, "Alarm center", "OPC UA discovery");
        ui_theme_create_empty_state(generator->alarms_host, "Waiting for equipment",
                                    "Alarm monitoring starts when equipment is discovered",
                                    lv_color_hex(UI_COLOR_ACCENT));
        return;
    }

    size_t active_alarm_count = data_model_active_alarm_count(generator->data_model);
    char count_text[48];
    snprintf(count_text, sizeof(count_text), "%u active", (unsigned)active_alarm_count);
    ui_theme_create_heading(generator->alarms_host, "Alarm center", count_text);
    if (active_alarm_count == 0) {
        create_alarm_clear_state(generator->alarms_host);
        return;
    }

    for (size_t alarm_index = 0; alarm_index < active_alarm_count; ++alarm_index) {
        data_model_alarm_t alarm;
        if (! data_model_get_active_alarm(generator->data_model, alarm_index, &alarm)) {
            continue;
        }
        lv_color_t alarm_color = alarm.severity >= 800 ? lv_color_hex(UI_COLOR_DANGER) : lv_color_hex(UI_COLOR_WARNING);
        lv_obj_t* card         = lv_obj_create(generator->alarms_host);
        lv_obj_set_size(card, lv_pct(100), 118);
        ui_theme_style_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
        lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);

        lv_obj_t* badge = lv_obj_create(card);
        lv_obj_set_size(badge, 58, 58);
        ui_theme_style_card(badge, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(badge, alarm_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(badge, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_border_color(badge, alarm_color, LV_PART_MAIN);
        lv_obj_set_style_border_width(badge, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
        lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t* symbol = lv_label_create(badge);
        lv_label_set_text(symbol, "!");
        lv_obj_set_style_text_color(symbol, alarm_color, LV_PART_MAIN);
        lv_obj_set_style_text_font(symbol, &lv_font_montserrat_30, LV_PART_MAIN);
        lv_obj_center(symbol);

        lv_obj_t* severity = lv_label_create(card);
        lv_label_set_text(severity, alarm.severity >= 800 ? "CRITICAL" : "WARNING");
        lv_obj_set_style_text_color(severity, alarm_color, LV_PART_MAIN);
        lv_obj_set_style_text_font(severity, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(severity, LV_ALIGN_TOP_RIGHT, 0, 2);

        char source_name[UI_HUMAN_NAME_LENGTH];
        ui_theme_humanize_name(source_name, sizeof(source_name), alarm.source_name);
        lv_obj_t* source = lv_label_create(card);
        lv_label_set_text(source, source_name);
        lv_label_set_long_mode(source, LV_LABEL_LONG_DOT);
        lv_obj_set_width(source, lv_pct(58));
        lv_obj_set_style_text_color(source, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(source, &lv_font_montserrat_22, LV_PART_MAIN);
        lv_obj_align(source, LV_ALIGN_TOP_LEFT, 78, 0);

        lv_obj_t* reason = lv_label_create(card);
        lv_label_set_text(reason, alarm.reason);
        lv_label_set_long_mode(reason, LV_LABEL_LONG_DOT);
        lv_obj_set_width(reason, lv_pct(78));
        lv_obj_set_style_text_color(reason, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(reason, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_align(reason, LV_ALIGN_TOP_LEFT, 78, 34);

        char details[80];
        snprintf(details, sizeof(details), "%.47s  |  Severity %u", alarm.alarm_code, (unsigned)alarm.severity);
        lv_obj_t* details_label = lv_label_create(card);
        lv_label_set_text(details_label, details);
        lv_obj_set_style_text_color(details_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(details_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(details_label, LV_ALIGN_BOTTOM_LEFT, 78, 0);
    }
}

static void create_alarm_clear_state(lv_obj_t* parent)
{
    lv_color_t healthy = lv_color_hex(UI_COLOR_SUCCESS);
    lv_obj_t* card     = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), 118);
    ui_theme_style_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);

    lv_obj_t* badge = lv_obj_create(card);
    lv_obj_set_size(badge, 58, 58);
    ui_theme_style_card(badge, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(badge, healthy, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_color(badge, healthy, LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* symbol = lv_label_create(badge);
    lv_label_set_text(symbol, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(symbol, healthy, LV_PART_MAIN);
    lv_obj_set_style_text_font(symbol, &lv_font_montserrat_30, LV_PART_MAIN);
    lv_obj_center(symbol);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "All equipment is normal");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 78, -15);
    lv_obj_t* detail = lv_label_create(card);
    lv_label_set_text(detail, "No active alarms reported by OPC UA");
    lv_obj_set_style_text_color(detail, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(detail, LV_ALIGN_LEFT_MID, 78, 17);
}

/* Settings UI below intentionally keeps the existing layout and behavior. */
static lv_obj_t* create_settings_section_button(lv_obj_t* parent, const char* title)
{
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_size(button, lv_pct(100), 72);
    lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x111820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

static void open_settings_event(lv_event_t* event)
{
    ui_generator_t* generator = lv_event_get_user_data(event);
    if (generator->settings_overlay != NULL && lv_obj_is_valid(generator->settings_overlay)) {
        lv_obj_move_foreground(generator->settings_overlay);
        return;
    }
    opcua_client_pause(generator->opcua_client);

    generator->settings_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(generator->settings_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(generator->settings_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(generator->settings_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(generator->settings_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(generator->settings_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(generator->settings_overlay, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_column(generator->settings_overlay, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(generator->settings_overlay, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(generator->settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(generator->settings_overlay, settings_overlay_deleted_event, LV_EVENT_DELETE, generator);

    lv_obj_t* sidebar = lv_obj_create(generator->settings_overlay);
    lv_obj_set_size(sidebar, 280, lv_pct(100));
    lv_obj_set_style_bg_color(sidebar, lv_color_hex(0x181818), LV_PART_MAIN);
    lv_obj_set_style_border_width(sidebar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sidebar, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sidebar, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_row(sidebar, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);

    generator->settings_section_buttons[SETTINGS_SECTION_SYSTEM]  = create_settings_section_button(sidebar, "System");
    generator->settings_section_buttons[SETTINGS_SECTION_WIFI]    = create_settings_section_button(sidebar, "Wi-Fi");
    generator->settings_section_buttons[SETTINGS_SECTION_OPENPLC] = create_settings_section_button(sidebar, "OpenPLC");
    for (unsigned section = 0; section < SETTINGS_SECTION_COUNT; ++section) {
        lv_obj_add_event_cb(generator->settings_section_buttons[section], settings_section_event, LV_EVENT_CLICKED,
                            generator);
    }

    lv_obj_t* spacer = lv_obj_create(sidebar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t* back_button = lv_btn_create(sidebar);
    lv_obj_set_size(back_button, lv_pct(100), 68);
    lv_obj_set_style_radius(back_button, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_border_width(back_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(back_button, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back_button, close_settings_event, LV_EVENT_CLICKED, generator);
    lv_obj_t* back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(back_label);

    lv_obj_t* content = lv_obj_create(generator->settings_overlay);
    lv_obj_set_height(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x181818), LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 12, LV_PART_MAIN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    for (unsigned section = 0; section < SETTINGS_SECTION_COUNT; ++section) {
        generator->settings_pages[section] = lv_obj_create(content);
        lv_obj_set_size(generator->settings_pages[section], lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_opa(generator->settings_pages[section], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(generator->settings_pages[section], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(generator->settings_pages[section], 0, LV_PART_MAIN);
    }

    system_settings_create(generator->settings_pages[SETTINGS_SECTION_SYSTEM]);
    wifi_menu_create(generator->settings_pages[SETTINGS_SECTION_WIFI], lv_layer_top());
    char endpoint_url[OPCUA_CLIENT_ENDPOINT_LENGTH];
    opcua_client_get_endpoint(generator->opcua_client, endpoint_url, sizeof(endpoint_url));
    openplc_settings_create(generator->settings_pages[SETTINGS_SECTION_OPENPLC], endpoint_url, apply_opcua_endpoint,
                            generator);
    show_settings_section(generator, SETTINGS_SECTION_SYSTEM);
}

static void close_settings_event(lv_event_t* event)
{
    ui_generator_t* generator = lv_event_get_user_data(event);
    if (generator->settings_overlay != NULL && lv_obj_is_valid(generator->settings_overlay)) {
        lv_obj_del(generator->settings_overlay);
    }
}

static void settings_overlay_deleted_event(lv_event_t* event)
{
    ui_generator_t* generator = lv_event_get_user_data(event);
    wifi_menu_deactivate();
    openplc_settings_deactivate();
    opcua_client_resume(generator->opcua_client);
    generator->settings_overlay = NULL;
    for (unsigned section = 0; section < SETTINGS_SECTION_COUNT; ++section) {
        generator->settings_pages[section]           = NULL;
        generator->settings_section_buttons[section] = NULL;
    }
}

static void settings_section_event(lv_event_t* event)
{
    ui_generator_t* generator = lv_event_get_user_data(event);
    lv_obj_t* selected_button = lv_event_get_target(event);
    for (unsigned section = 0; section < SETTINGS_SECTION_COUNT; ++section) {
        if (selected_button == generator->settings_section_buttons[section]) {
            show_settings_section(generator, section);
            return;
        }
    }
}

static void show_settings_section(ui_generator_t* generator, unsigned selected_section)
{
    for (unsigned section = 0; section < SETTINGS_SECTION_COUNT; ++section) {
        bool selected = section == selected_section;
        if (selected) {
            lv_obj_clear_flag(generator->settings_pages[section], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(generator->settings_pages[section], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_border_color(generator->settings_section_buttons[section],
                                      selected ? lv_color_hex(0x6BC1FF) : lv_color_hex(0x555555), LV_PART_MAIN);
        lv_obj_set_style_bg_color(generator->settings_section_buttons[section],
                                  selected ? lv_color_hex(0x184F73) : lv_color_hex(0x111820), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(generator->settings_section_buttons[section], LV_OPA_COVER, LV_PART_MAIN);
    }
    if (selected_section == SETTINGS_SECTION_WIFI) {
        openplc_settings_deactivate();
        wifi_menu_activate();
    } else if (selected_section == SETTINGS_SECTION_OPENPLC) {
        wifi_menu_deactivate();
        openplc_settings_activate();
    } else {
        wifi_menu_deactivate();
        openplc_settings_deactivate();
    }
}

static esp_err_t apply_opcua_endpoint(const char* endpoint_url, void* user_data)
{
    ui_generator_t* generator = user_data;
    return opcua_client_set_endpoint(generator->opcua_client, endpoint_url);
}
