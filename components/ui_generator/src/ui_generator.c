#include "ui_generator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "navigation.h"
#include "openplc_settings.h"
#include "system_settings.h"
#include "trends.h"
#include "wifi_widget.h"
#include "wifi_menu.h"

#define UI_REFRESH_PERIOD_MS  250
#define UI_NAVIGATION_HEIGHT  76
#define UI_NUMERIC_SCALE      100.0
#define UI_HUMAN_NAME_LENGTH  128
#define UI_BOOLEAN_WRITE_HOLD_MS 2000
#define SETTINGS_SECTION_COUNT 3

#define UI_COLOR_BACKGROUND       0x080808
#define UI_COLOR_SURFACE          0x151515
#define UI_COLOR_SURFACE_RAISED   0x1B1B1B
#define UI_COLOR_BORDER           0x303030
#define UI_COLOR_CONTROL_BORDER   0x484848
#define UI_COLOR_TEXT_PRIMARY     0xF4F4F4
#define UI_COLOR_TEXT_LABEL       0xD0D0D0
#define UI_COLOR_TEXT_SECONDARY   0xA0A0A0
#define UI_COLOR_ACCENT           0x2A96FF
#define UI_COLOR_ACCENT_SOFT      0x6BC1FF
#define UI_COLOR_SUCCESS          0x38B000
#define UI_COLOR_WARNING          0xF0A202
#define UI_COLOR_DANGER           0xE63946
#define UI_COLOR_INACTIVE         0x666666

enum
{
    SETTINGS_SECTION_SYSTEM = 0,
    SETTINGS_SECTION_WIFI,
    SETTINGS_SECTION_OPENPLC,
};

typedef enum
{
    GENERATED_WIDGET_BOOLEAN_INDICATOR,
    GENERATED_WIDGET_BOOLEAN_SWITCH,
    GENERATED_WIDGET_NUMERIC_BAR,
    GENERATED_WIDGET_NUMERIC_SLIDER,
    GENERATED_WIDGET_TEXT,
} generated_widget_type_t;

typedef struct
{
    struct ui_generator* generator;
    size_t tag_index;
    generated_widget_type_t widget_type;
    lv_obj_t* value_label;
    lv_obj_t* visual_object;
    bool updating_from_model;
    bool boolean_write_pending;
    bool pending_boolean_value;
    uint32_t pending_write_started;
} generated_widget_binding_t;

typedef struct
{
    size_t status_tag_index;
    size_t alarm_tag_index;
    lv_obj_t* state_beacon;
    lv_obj_t* state_indicator;
    lv_obj_t* state_label;
    bool displayed_state_valid;
    bool displayed_running;
    bool displayed_alarm;
} overview_equipment_binding_t;

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
    wifi_widget_t* wifi_widget;
    navigation_t* navigation;
    lv_timer_t* refresh_timer;
    data_model_t* data_model;
    opcua_client_t* opcua_client;
    generated_widget_binding_t* bindings;
    size_t binding_capacity;
    size_t binding_count;
    overview_equipment_binding_t* overview_equipment;
    size_t overview_equipment_count;
    lv_obj_t* overview_running_value;
    lv_obj_t* overview_alarm_value;
    lv_obj_t* overview_state_value;
    lv_obj_t* overview_state_detail;
    lv_obj_t* overview_state_indicator;
    lv_obj_t* alarms_host;
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
static void create_overview(ui_generator_t* generator, lv_obj_t* page);
static void update_overview(ui_generator_t* generator);
static void rebuild_alarm_list(ui_generator_t* generator);
static void create_equipment_section(ui_generator_t* generator, lv_obj_t* page,
                                     const data_model_equipment_t* equipment);
static size_t create_controls_section(ui_generator_t* generator, lv_obj_t* page,
                                      const data_model_equipment_t* equipment);
static void create_tag_widget(ui_generator_t* generator, lv_obj_t* parent, const data_model_tag_t* tag);
static void update_widget(generated_widget_binding_t* binding, const data_model_tag_t* tag);
static void boolean_switch_event(lv_event_t* event);
static void numeric_slider_event(lv_event_t* event);
static void open_settings_event(lv_event_t* event);
static void close_settings_event(lv_event_t* event);
static void settings_overlay_deleted_event(lv_event_t* event);
static void settings_section_event(lv_event_t* event);
static void show_settings_section(ui_generator_t* generator, unsigned section);
static lv_obj_t* create_settings_section_button(lv_obj_t* parent, const char* title);
static esp_err_t apply_opcua_endpoint(const char* endpoint_url, void* user_data);
static void style_page_as_grid(lv_obj_t* page);
static generated_widget_binding_t* allocate_binding(ui_generator_t* generator);
static void numeric_range(const data_model_tag_t* tag, double* minimum, double* maximum);
static void set_numeric_label(lv_obj_t* label, double value, const char* engineering_unit);
static int32_t scale_numeric_for_widget(double value);
static void copy_display_unit(char* destination, size_t destination_size, const char* source);
static bool find_equipment_tag_by_role(const data_model_t* model, size_t equipment_index, const char* semantic_role,
                                       data_model_tag_t* tag_out);
static lv_obj_t* create_summary_card(lv_obj_t* parent, const char* caption, const char* value, lv_color_t accent);
static void create_section_heading(lv_obj_t* parent, const char* title, const char* detail);
static void create_empty_state(lv_obj_t* parent, const char* title, const char* detail, lv_color_t accent);
static void create_alarm_clear_state(lv_obj_t* parent);
static void style_content_card(lv_obj_t* card, lv_color_t background, lv_coord_t radius);
static lv_color_t boolean_state_color(const data_model_tag_t* tag, bool value);
static void set_label_text_if_changed(lv_obj_t* label, const char* text);
static void copy_equipment_group_name(char* destination, size_t destination_size,
                                      const data_model_equipment_t* equipment);
static void copy_humanized_name(char* destination, size_t destination_size, const char* source);

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
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x181818), LV_PART_MAIN);
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
    lv_obj_set_style_text_color(generator->status_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(generator->status_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_long_mode(generator->status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(generator->status_label, 1);

    esp_err_t wifi_widget_result = wifi_widget_create(status_bar, &generator->wifi_widget);
    if (wifi_widget_result != ESP_OK) {
        ESP_LOGW(TAG, "Cannot create Wi-Fi indicator: %s", esp_err_to_name(wifi_widget_result));
    }

    generator->settings_button = lv_btn_create(status_bar);
    lv_obj_set_size(generator->settings_button, 58, 54);
    lv_obj_set_style_radius(generator->settings_button, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(generator->settings_button, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_border_width(generator->settings_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(generator->settings_button, lv_color_hex(0x2A96FF), LV_PART_MAIN);
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

    char status_text[128];
    opcua_client_get_status(generator->opcua_client, status_text, sizeof(status_text));
    if (strcmp(generator->displayed_status_text, status_text) != 0) {
        lv_label_set_text(generator->status_label, status_text);
        snprintf(generator->displayed_status_text, sizeof(generator->displayed_status_text), "%s", status_text);
    }
    opcua_client_state_t state = opcua_client_get_state(generator->opcua_client);
    if (! generator->status_display_valid || state != generator->displayed_client_state) {
        lv_color_t status_color = lv_color_hex(0xF0A202);
        if (state == OPCUA_CLIENT_CONNECTED) {
            status_color = lv_color_hex(0x38B000);
        } else if (state == OPCUA_CLIENT_CONNECTION_ERROR || state == OPCUA_CLIENT_BROWSE_ERROR) {
            status_color = lv_color_hex(0xE63946);
        }
        lv_obj_set_style_bg_color(generator->status_indicator, status_color, LV_PART_MAIN);
        generator->displayed_client_state = state;
        generator->status_display_valid = true;
    }

    uint32_t alarm_generation = data_model_alarm_generation(generator->data_model);
    if (alarm_generation != generator->observed_alarm_generation) {
        generator->observed_alarm_generation = alarm_generation;
        rebuild_alarm_list(generator);
        update_overview(generator);
    }

    uint32_t value_generation = data_model_value_generation(generator->data_model);
    if (value_generation == generator->observed_value_generation) {
        return;
    }
    generator->observed_value_generation = value_generation;
    update_overview(generator);
    for (size_t binding_index = 0; binding_index < generator->binding_count; ++binding_index) {
        generated_widget_binding_t* binding = &generator->bindings[binding_index];
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, binding->tag_index, &tag)) {
            update_widget(binding, &tag);
        }
    }
}

static void rebuild_interface(ui_generator_t* generator)
{
    generator->observed_structure_generation = data_model_structure_generation(generator->data_model);
    generator->observed_value_generation     = UINT32_MAX;
    generator->observed_alarm_generation     = UINT32_MAX;
    free(generator->bindings);
    free(generator->overview_equipment);
    generator->bindings         = NULL;
    generator->binding_count    = 0;
    generator->binding_capacity = data_model_tag_count(generator->data_model) * 3 + 1;
    generator->bindings         = calloc(generator->binding_capacity, sizeof(*generator->bindings));
    generator->overview_equipment = NULL;
    generator->overview_equipment_count = 0;
    generator->overview_running_value = NULL;
    generator->overview_alarm_value = NULL;
    generator->overview_state_value = NULL;
    generator->overview_state_detail = NULL;
    generator->overview_state_indicator = NULL;
    generator->alarms_host = NULL;

    navigation_release(generator->navigation);
    generator->navigation = NULL;
    lv_obj_clean(generator->content_host);
    generator->navigation = navigation_create(generator->content_host, UI_NAVIGATION_HEIGHT);
    if (generator->navigation == NULL) {
        ESP_LOGE(TAG, "Cannot allocate navigation");
        return;
    }

    lv_obj_t* overview_page = navigation_add_page(generator->navigation, "Overview");
    style_page_as_grid(overview_page);
    create_overview(generator, overview_page);

    size_t discovered_tag_count = data_model_tag_count(generator->data_model);
    lv_obj_t* equipment_page = navigation_add_page(generator->navigation, "Equipment");
    style_page_as_grid(equipment_page);
    size_t equipment_count = data_model_equipment_count(generator->data_model);
    for (size_t equipment_index = 0; equipment_index < equipment_count; ++equipment_index) {
        data_model_equipment_t equipment;
        if (data_model_get_equipment(generator->data_model, equipment_index, &equipment)) {
            create_equipment_section(generator, equipment_page, &equipment);
        }
    }

    lv_obj_t* trends_page = navigation_add_page(generator->navigation, "Trends");
    style_page_as_grid(trends_page);
    if (trends_create(trends_page, generator->data_model) == NULL) {
        create_section_heading(trends_page, "Live trends", "History unavailable");
        create_empty_state(trends_page, "Cannot create trend charts",
                           "Not enough memory for the Trends view", lv_color_hex(UI_COLOR_WARNING));
    }

    lv_obj_t* controls_page = navigation_add_page(generator->navigation, "Controls");
    style_page_as_grid(controls_page);
    size_t writable_count = 0;
    size_t tag_count      = discovered_tag_count;
    for (size_t equipment_index = 0; equipment_index < equipment_count; ++equipment_index) {
        data_model_equipment_t equipment;
        if (data_model_get_equipment(generator->data_model, equipment_index, &equipment)) {
            writable_count += create_controls_section(generator, controls_page, &equipment);
        }
    }
    if (writable_count == 0) {
        create_section_heading(controls_page, "Controls", "OPC UA write access");
        create_empty_state(controls_page, "No writable variables",
                           "Controls appear automatically when write access is discovered",
                           lv_color_hex(UI_COLOR_ACCENT));
    }

    lv_obj_t* alarms_page  = navigation_add_page(generator->navigation, "Alarms");
    style_page_as_grid(alarms_page);
    generator->alarms_host = alarms_page;
    rebuild_alarm_list(generator);

    ESP_LOGI(TAG, "Generated UI for %u equipment objects and %u tags", (unsigned)equipment_count, (unsigned)tag_count);
}

static void create_overview(ui_generator_t* generator, lv_obj_t* page)
{
    size_t equipment_count = data_model_equipment_count(generator->data_model);
    size_t tag_count = data_model_tag_count(generator->data_model);

    /* The command-center row establishes a clear reading order: overall state
     * first, then the two operational counts that need regular attention. */
    lv_obj_t* command_row = lv_obj_create(page);
    lv_obj_remove_style_all(command_row);
    lv_obj_set_size(command_row, lv_pct(97), 126);
    lv_obj_set_flex_flow(command_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(command_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(command_row, 16, LV_PART_MAIN);
    lv_obj_clear_flag(command_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* state_card = lv_obj_create(command_row);
    style_content_card(state_card, lv_color_hex(UI_COLOR_SURFACE_RAISED), 20);
    lv_obj_set_height(state_card, lv_pct(100));
    lv_obj_set_flex_grow(state_card, 1);
    lv_obj_set_style_pad_left(state_card, 24, LV_PART_MAIN);

    lv_obj_t* eyebrow = lv_label_create(state_card);
    lv_label_set_text(eyebrow, "SYSTEM OVERVIEW");
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(UI_COLOR_ACCENT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_font(eyebrow, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(eyebrow, LV_ALIGN_TOP_LEFT, 34, 0);

    generator->overview_state_indicator = lv_obj_create(state_card);
    lv_obj_remove_style_all(generator->overview_state_indicator);
    lv_obj_set_size(generator->overview_state_indicator, 18, 18);
    lv_obj_set_style_radius(generator->overview_state_indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(generator->overview_state_indicator, lv_color_hex(UI_COLOR_INACTIVE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(generator->overview_state_indicator, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(generator->overview_state_indicator, LV_ALIGN_TOP_LEFT, 0, 1);

    generator->overview_state_value = lv_label_create(state_card);
    lv_label_set_text(generator->overview_state_value, "Discovering equipment");
    lv_obj_set_width(generator->overview_state_value, lv_pct(100));
    lv_label_set_long_mode(generator->overview_state_value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(generator->overview_state_value, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(generator->overview_state_value, &lv_font_montserrat_34, LV_PART_MAIN);
    lv_obj_align(generator->overview_state_value, LV_ALIGN_LEFT_MID, 0, 2);

    generator->overview_state_detail = lv_label_create(state_card);
    lv_label_set_text_fmt(generator->overview_state_detail, "%u objects  |  %u live tags",
                          (unsigned)equipment_count, (unsigned)tag_count);
    lv_obj_set_style_text_color(generator->overview_state_detail, lv_color_hex(UI_COLOR_TEXT_SECONDARY),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(generator->overview_state_detail, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(generator->overview_state_detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    generator->overview_running_value =
        create_summary_card(command_row, "RUNNING", "0", lv_color_hex(UI_COLOR_ACCENT));
    generator->overview_alarm_value =
        create_summary_card(command_row, "ACTIVE ALARMS", "0", lv_color_hex(UI_COLOR_DANGER));

    create_section_heading(page, "Equipment composition", "Discovered from OPC UA");
    for (size_t equipment_index = 0; equipment_index < equipment_count; ++equipment_index) {
        data_model_equipment_t equipment;
        if (!data_model_get_equipment(generator->data_model, equipment_index, &equipment)) {
            continue;
        }
        char group_name[UI_HUMAN_NAME_LENGTH];
        copy_equipment_group_name(group_name, sizeof(group_name), &equipment);
        bool group_already_added = false;
        for (size_t prior_index = 0; prior_index < equipment_index; ++prior_index) {
            data_model_equipment_t prior_equipment;
            char prior_group_name[UI_HUMAN_NAME_LENGTH];
            if (data_model_get_equipment(generator->data_model, prior_index, &prior_equipment)) {
                copy_equipment_group_name(prior_group_name, sizeof(prior_group_name), &prior_equipment);
                if (strcmp(group_name, prior_group_name) == 0) {
                    group_already_added = true;
                    break;
                }
            }
        }
        if (group_already_added) {
            continue;
        }

        size_t group_count = 0;
        for (size_t candidate_index = equipment_index; candidate_index < equipment_count; ++candidate_index) {
            data_model_equipment_t candidate;
            char candidate_group_name[UI_HUMAN_NAME_LENGTH];
            if (data_model_get_equipment(generator->data_model, candidate_index, &candidate)) {
                copy_equipment_group_name(candidate_group_name, sizeof(candidate_group_name), &candidate);
                if (strcmp(group_name, candidate_group_name) == 0) {
                    group_count++;
                }
            }
        }

        lv_obj_t* group_card = lv_obj_create(page);
        lv_obj_remove_style_all(group_card);
        lv_obj_set_size(group_card, 310, 70);
        lv_obj_clear_flag(group_card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* group_label = lv_label_create(group_card);
        lv_label_set_text(group_label, group_name);
        lv_label_set_long_mode(group_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(group_label, 242);
        lv_obj_set_style_text_color(group_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(group_label, &lv_font_montserrat_22, LV_PART_MAIN);
        lv_obj_align(group_label, LV_ALIGN_LEFT_MID, 64, 0);

        lv_obj_t* count_cell = lv_obj_create(group_card);
        lv_obj_set_size(count_cell, 48, 48);
        lv_obj_clear_flag(count_cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(count_cell, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(count_cell, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(count_cell, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_border_width(count_cell, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(count_cell, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(count_cell, 0, LV_PART_MAIN);
        lv_obj_align(count_cell, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t* count_label = lv_label_create(count_cell);
        char count_text[16];
        snprintf(count_text, sizeof(count_text), "%u", (unsigned)group_count);
        lv_label_set_text(count_label, count_text);
        lv_obj_set_style_text_color(count_label, lv_color_hex(UI_COLOR_ACCENT_SOFT), LV_PART_MAIN);
        lv_obj_set_style_text_font(count_label, &lv_font_montserrat_22, LV_PART_MAIN);
        lv_obj_center(count_label);
    }

    create_section_heading(page, "Live equipment status", "Semantic status signals");

    generator->overview_equipment = calloc(equipment_count, sizeof(*generator->overview_equipment));
    if (generator->overview_equipment == NULL && equipment_count > 0) {
        ESP_LOGW(TAG, "Cannot allocate overview equipment bindings");
        return;
    }
    for (size_t equipment_index = 0; equipment_index < equipment_count; ++equipment_index) {
        data_model_equipment_t equipment;
        data_model_tag_t status_tag;
        if (!data_model_get_equipment(generator->data_model, equipment_index, &equipment) ||
            !find_equipment_tag_by_role(generator->data_model, equipment_index, "operating_status", &status_tag)) {
            continue;
        }

        overview_equipment_binding_t* binding =
            &generator->overview_equipment[generator->overview_equipment_count++];
        binding->status_tag_index = status_tag.index;
        binding->alarm_tag_index = DATA_MODEL_INVALID_INDEX;
        data_model_tag_t alarm_tag;
        if (find_equipment_tag_by_role(generator->data_model, equipment_index, "alarm_status", &alarm_tag)) {
            binding->alarm_tag_index = alarm_tag.index;
        }

        lv_obj_t* status_card = lv_obj_create(page);
        lv_obj_remove_style_all(status_card);
        lv_obj_set_size(status_card, 228, 84);
        lv_obj_clear_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* status_beacon = lv_obj_create(status_card);
        lv_obj_set_size(status_beacon, 54, 54);
        lv_obj_clear_flag(status_beacon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(status_beacon, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(status_beacon, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(status_beacon, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(status_beacon, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(status_beacon, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(status_beacon, 0, LV_PART_MAIN);
        lv_obj_align(status_beacon, LV_ALIGN_LEFT_MID, 0, 0);
        binding->state_beacon = status_beacon;

        const char* equipment_name =
            equipment.display_name[0] != '\0' ? equipment.display_name : equipment.browse_name;
        char human_equipment_name[UI_HUMAN_NAME_LENGTH];
        copy_humanized_name(human_equipment_name, sizeof(human_equipment_name), equipment_name);
        lv_obj_t* equipment_label = lv_label_create(status_card);
        lv_label_set_text(equipment_label, human_equipment_name);
        lv_label_set_long_mode(equipment_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(equipment_label, 158);
        lv_obj_set_style_text_color(equipment_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(equipment_label, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_align(equipment_label, LV_ALIGN_LEFT_MID, 68, -14);

        binding->state_indicator = lv_obj_create(status_beacon);
        lv_obj_remove_style_all(binding->state_indicator);
        lv_obj_set_size(binding->state_indicator, 18, 18);
        lv_obj_set_style_radius(binding->state_indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(binding->state_indicator, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_center(binding->state_indicator);

        binding->state_label = lv_label_create(status_card);
        lv_obj_set_style_text_font(binding->state_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(binding->state_label, LV_ALIGN_LEFT_MID, 68, 16);
    }
    if (generator->overview_equipment_count == 0) {
        create_empty_state(page, "No semantic status signals",
                           "Equipment is available on the Equipment page", lv_color_hex(UI_COLOR_ACCENT));
    }
    update_overview(generator);
}

static lv_obj_t* create_summary_card(lv_obj_t* parent, const char* caption, const char* value, lv_color_t accent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, 190, lv_pct(100));
    style_content_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);

    lv_obj_t* accent_line = lv_obj_create(card);
    lv_obj_remove_style_all(accent_line);
    lv_obj_set_size(accent_line, 40, 5);
    lv_obj_set_style_radius(accent_line, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(accent_line, accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent_line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(accent_line, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* caption_label = lv_label_create(card);
    lv_label_set_text(caption_label, caption);
    lv_obj_set_width(caption_label, lv_pct(100));
    lv_obj_set_style_text_align(caption_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_color(caption_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(caption_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(caption_label, LV_ALIGN_TOP_LEFT, 0, 16);

    lv_obj_t* value_label = lv_label_create(card);
    lv_label_set_text(value_label, value);
    lv_obj_set_width(value_label, lv_pct(100));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_color(value_label, accent, LV_PART_MAIN);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_align(value_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return value_label;
}

static void update_overview(ui_generator_t* generator)
{
    if (generator == NULL || generator->overview_running_value == NULL || generator->overview_alarm_value == NULL ||
        generator->overview_state_value == NULL || generator->overview_state_detail == NULL ||
        generator->overview_state_indicator == NULL) {
        return;
    }
    size_t running_count = 0;
    size_t alarm_status_count = 0;
    for (size_t index = 0; index < generator->overview_equipment_count; ++index) {
        overview_equipment_binding_t* binding = &generator->overview_equipment[index];
        data_model_tag_t status_tag;
        bool running = data_model_get_tag(generator->data_model, binding->status_tag_index, &status_tag) &&
                       status_tag.value_valid && status_tag.value.boolean_value;
        bool alarm_active = false;
        data_model_tag_t alarm_tag;
        if (binding->alarm_tag_index != DATA_MODEL_INVALID_INDEX &&
            data_model_get_tag(generator->data_model, binding->alarm_tag_index, &alarm_tag)) {
            alarm_active = alarm_tag.value_valid && alarm_tag.value.boolean_value;
        }
        running_count += running ? 1U : 0U;
        alarm_status_count += alarm_active ? 1U : 0U;

        lv_color_t state_color = alarm_active ? lv_color_hex(UI_COLOR_DANGER)
                                               : (running ? lv_color_hex(UI_COLOR_SUCCESS)
                                                          : lv_color_hex(UI_COLOR_INACTIVE));
        const char* state_text = alarm_active ? "ALARM" : (running ? "RUNNING" : "STOPPED");
        if (! binding->displayed_state_valid || running != binding->displayed_running ||
            alarm_active != binding->displayed_alarm) {
            lv_obj_set_style_bg_color(binding->state_indicator, state_color, LV_PART_MAIN);
            lv_obj_set_style_border_color(binding->state_beacon, state_color, LV_PART_MAIN);
            lv_obj_set_style_border_opa(binding->state_beacon, LV_OPA_60, LV_PART_MAIN);
            lv_label_set_text(binding->state_label, state_text);
            lv_obj_set_style_text_color(binding->state_label, state_color, LV_PART_MAIN);
            binding->displayed_running = running;
            binding->displayed_alarm = alarm_active;
            binding->displayed_state_valid = true;
        }
    }

    size_t active_alarm_count = data_model_active_alarm_count(generator->data_model);
    if (alarm_status_count > active_alarm_count) {
        active_alarm_count = alarm_status_count;
    }
    char count_text[16];
    snprintf(count_text, sizeof(count_text), "%u", (unsigned)running_count);
    set_label_text_if_changed(generator->overview_running_value, count_text);
    snprintf(count_text, sizeof(count_text), "%u", (unsigned)active_alarm_count);
    set_label_text_if_changed(generator->overview_alarm_value, count_text);

    const char* overview_state;
    lv_color_t overview_color;
    if (active_alarm_count > 0) {
        overview_state = "Attention required";
        overview_color = lv_color_hex(UI_COLOR_DANGER);
    } else if (generator->overview_equipment_count == 0) {
        overview_state = "Equipment discovered";
        overview_color = lv_color_hex(UI_COLOR_ACCENT);
    } else if (running_count > 0) {
        overview_state = "System operational";
        overview_color = lv_color_hex(UI_COLOR_SUCCESS);
    } else {
        overview_state = "Equipment ready";
        overview_color = lv_color_hex(UI_COLOR_INACTIVE);
    }
    set_label_text_if_changed(generator->overview_state_value, overview_state);
    char state_detail[64];
    snprintf(state_detail, sizeof(state_detail), "%u of %u active  |  %u alarms",
             (unsigned)running_count, (unsigned)generator->overview_equipment_count,
             (unsigned)active_alarm_count);
    set_label_text_if_changed(generator->overview_state_detail, state_detail);
    lv_obj_set_style_bg_color(generator->overview_state_indicator, overview_color, LV_PART_MAIN);
}

static void rebuild_alarm_list(ui_generator_t* generator)
{
    if (generator == NULL || generator->alarms_host == NULL) {
        return;
    }
    lv_obj_clean(generator->alarms_host);
    size_t active_alarm_count = data_model_active_alarm_count(generator->data_model);
    char alarm_count_text[48];
    snprintf(alarm_count_text, sizeof(alarm_count_text), "%u active",
             (unsigned)active_alarm_count);
    create_section_heading(generator->alarms_host, "Alarm center", alarm_count_text);

    if (active_alarm_count == 0) {
        create_alarm_clear_state(generator->alarms_host);
        return;
    }

    for (size_t alarm_index = 0; alarm_index < active_alarm_count; ++alarm_index) {
        data_model_alarm_t alarm;
        if (!data_model_get_active_alarm(generator->data_model, alarm_index, &alarm)) {
            continue;
        }
        lv_color_t alarm_color =
            alarm.severity >= 800 ? lv_color_hex(UI_COLOR_DANGER) : lv_color_hex(UI_COLOR_WARNING);
        lv_obj_t* alarm_card = lv_obj_create(generator->alarms_host);
        lv_obj_set_size(alarm_card, lv_pct(97), 118);
        style_content_card(alarm_card, lv_color_hex(UI_COLOR_SURFACE), 20);
        lv_obj_set_style_pad_all(alarm_card, 18, LV_PART_MAIN);

        lv_obj_t* severity_badge = lv_obj_create(alarm_card);
        lv_obj_set_size(severity_badge, 58, 58);
        lv_obj_clear_flag(severity_badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(severity_badge, alarm_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(severity_badge, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_border_color(severity_badge, alarm_color, LV_PART_MAIN);
        lv_obj_set_style_border_width(severity_badge, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(severity_badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(severity_badge, 0, LV_PART_MAIN);
        lv_obj_align(severity_badge, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* severity_symbol = lv_label_create(severity_badge);
        lv_label_set_text(severity_symbol, "!");
        lv_obj_set_style_text_color(severity_symbol, alarm_color, LV_PART_MAIN);
        lv_obj_set_style_text_font(severity_symbol, &lv_font_montserrat_30, LV_PART_MAIN);
        lv_obj_center(severity_symbol);

        lv_obj_t* severity_label = lv_label_create(alarm_card);
        lv_label_set_text(severity_label, alarm.severity >= 800 ? "CRITICAL" : "WARNING");
        lv_obj_set_style_text_color(severity_label, alarm_color, LV_PART_MAIN);
        lv_obj_set_style_text_font(severity_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(severity_label, LV_ALIGN_TOP_RIGHT, 0, 2);

        lv_obj_t* source_label = lv_label_create(alarm_card);
        char human_source_name[UI_HUMAN_NAME_LENGTH];
        copy_humanized_name(human_source_name, sizeof(human_source_name), alarm.source_name);
        lv_label_set_text(source_label, human_source_name);
        lv_label_set_long_mode(source_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(source_label, lv_pct(58));
        lv_obj_set_style_text_color(source_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(source_label, &lv_font_montserrat_22, LV_PART_MAIN);
        lv_obj_align(source_label, LV_ALIGN_TOP_LEFT, 78, 0);

        lv_obj_t* reason_label = lv_label_create(alarm_card);
        lv_label_set_text(reason_label, alarm.reason);
        lv_label_set_long_mode(reason_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(reason_label, lv_pct(78));
        lv_obj_set_style_text_color(reason_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(reason_label, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_align(reason_label, LV_ALIGN_TOP_LEFT, 78, 34);

        char details[80];
        snprintf(details, sizeof(details), "%.47s  |  Severity %u", alarm.alarm_code, (unsigned)alarm.severity);
        lv_obj_t* details_label = lv_label_create(alarm_card);
        lv_label_set_text(details_label, details);
        lv_obj_set_style_text_color(details_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(details_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(details_label, LV_ALIGN_BOTTOM_LEFT, 78, 0);
    }
}

static void create_equipment_section(ui_generator_t* generator, lv_obj_t* page,
                                     const data_model_equipment_t* equipment)
{
    size_t matching_tags = 0;
    size_t tag_count     = data_model_tag_count(generator->data_model);
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) &&
            tag.equipment_index == equipment->index) {
            matching_tags++;
        }
    }

    const char* equipment_name = equipment->display_name[0] != '\0' ? equipment->display_name : equipment->browse_name;
    char human_equipment_name[UI_HUMAN_NAME_LENGTH];
    copy_humanized_name(human_equipment_name, sizeof(human_equipment_name), equipment_name);
    char section_detail[48];
    snprintf(section_detail, sizeof(section_detail), "%u variables", (unsigned)matching_tags);
    create_section_heading(page, human_equipment_name, section_detail);

    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) && tag.equipment_index == equipment->index) {
            create_tag_widget(generator, page, &tag);
        }
    }
    if (matching_tags == 0) {
        create_empty_state(page, "No readable variables", "The OPC UA object contains no supported values",
                           lv_color_hex(UI_COLOR_ACCENT));
    }
}

static size_t create_controls_section(ui_generator_t* generator, lv_obj_t* page,
                                      const data_model_equipment_t* equipment)
{
    size_t tag_count = data_model_tag_count(generator->data_model);
    size_t writable_count = 0;
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) && tag.writable &&
            tag.equipment_index == equipment->index) {
            writable_count++;
        }
    }
    if (writable_count == 0) {
        return 0;
    }

    const char* equipment_name =
        equipment->display_name[0] != '\0' ? equipment->display_name : equipment->browse_name;
    char human_equipment_name[UI_HUMAN_NAME_LENGTH];
    copy_humanized_name(human_equipment_name, sizeof(human_equipment_name), equipment_name);
    char section_detail[48];
    snprintf(section_detail, sizeof(section_detail), "%u controls", (unsigned)writable_count);
    create_section_heading(page, human_equipment_name, section_detail);

    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) && tag.writable &&
            tag.equipment_index == equipment->index) {
            create_tag_widget(generator, page, &tag);
        }
    }
    return writable_count;
}

static void create_tag_widget(ui_generator_t* generator, lv_obj_t* parent, const data_model_tag_t* tag)
{
    generated_widget_binding_t* binding = allocate_binding(generator);
    if (binding == NULL) {
        ESP_LOGW(TAG, "Widget capacity exhausted");
        return;
    }
    binding->generator = generator;
    binding->tag_index = tag->index;

    bool is_boolean = tag->data_type == DATA_MODEL_TYPE_BOOLEAN;
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, is_boolean ? 228 : 310, is_boolean ? 112 : 156);
    style_content_card(card,
                       lv_color_hex(tag->writable ? UI_COLOR_SURFACE_RAISED : UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
    if (tag->writable) {
        lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_border_opa(card, LV_OPA_40, LV_PART_MAIN);
    }

    lv_obj_t* name_label = lv_label_create(card);
    const char* tag_name = tag->display_name[0] != '\0' ? tag->display_name : tag->browse_name;
    char human_tag_name[UI_HUMAN_NAME_LENGTH];
    copy_humanized_name(human_tag_name, sizeof(human_tag_name), tag_name);
    lv_label_set_text(name_label, human_tag_name);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, lv_pct(100));
    lv_obj_set_style_text_color(name_label, lv_color_hex(UI_COLOR_TEXT_LABEL), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 0, 0);

    binding->value_label = lv_label_create(card);
    lv_obj_set_width(binding->value_label, is_boolean ? 112 : 202);
    lv_label_set_long_mode(binding->value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_ACCENT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_font(binding->value_label,
                               is_boolean ? &lv_font_montserrat_22 : &lv_font_montserrat_30, LV_PART_MAIN);
    lv_obj_align(binding->value_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (is_boolean) {
        if (tag->writable) {
            binding->widget_type   = GENERATED_WIDGET_BOOLEAN_SWITCH;
            binding->visual_object = lv_switch_create(card);
            lv_obj_set_size(binding->visual_object, 72, 36);
            lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(binding->visual_object, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(binding->visual_object, lv_color_hex(UI_COLOR_CONTROL_BORDER),
                                          LV_PART_MAIN);
            lv_obj_set_style_border_width(binding->visual_object, 1, LV_PART_MAIN);
            lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_ACCENT),
                                      LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_KNOB);
            lv_obj_set_style_pad_all(binding->visual_object, 2, LV_PART_KNOB);
            lv_obj_align(binding->visual_object, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
            lv_obj_add_event_cb(binding->visual_object, boolean_switch_event, LV_EVENT_VALUE_CHANGED, binding);
        } else {
            binding->widget_type   = GENERATED_WIDGET_BOOLEAN_INDICATOR;
            binding->visual_object = lv_obj_create(card);
            lv_obj_remove_style_all(binding->visual_object);
            lv_obj_set_size(binding->visual_object, 18, 18);
            lv_obj_set_style_radius(binding->visual_object, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(binding->visual_object, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_align(binding->visual_object, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
        }
    } else if (tag->data_type == DATA_MODEL_TYPE_FLOAT || tag->data_type == DATA_MODEL_TYPE_DOUBLE ||
               tag->data_type == DATA_MODEL_TYPE_INTEGER) {
        double minimum;
        double maximum;
        numeric_range(tag, &minimum, &maximum);
        if (tag->writable) {
            binding->widget_type   = GENERATED_WIDGET_NUMERIC_SLIDER;
            binding->visual_object = lv_slider_create(card);
            lv_slider_set_range(binding->visual_object, scale_numeric_for_widget(minimum),
                                scale_numeric_for_widget(maximum));
            lv_obj_add_event_cb(binding->visual_object, numeric_slider_event, LV_EVENT_RELEASED, binding);
        } else {
            binding->widget_type   = GENERATED_WIDGET_NUMERIC_BAR;
            binding->visual_object = lv_bar_create(card);
            lv_bar_set_range(binding->visual_object, scale_numeric_for_widget(minimum),
                             scale_numeric_for_widget(maximum));
        }
        lv_obj_set_size(binding->visual_object, 276, tag->writable ? 22 : 16);
        lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_radius(binding->visual_object, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_radius(binding->visual_object, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        if (tag->writable) {
            lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_KNOB);
            lv_obj_set_style_pad_all(binding->visual_object, 6, LV_PART_KNOB);
        }
        lv_obj_align(binding->visual_object, LV_ALIGN_CENTER, 0, 11);
    } else {
        binding->widget_type = GENERATED_WIDGET_TEXT;
    }
    update_widget(binding, tag);
}

static void update_widget(generated_widget_binding_t* binding, const data_model_tag_t* tag)
{
    if (binding == NULL || tag == NULL || binding->value_label == NULL) {
        ESP_LOGE(TAG, "Cannot update an invalid widget binding");
        return;
    }

    binding->updating_from_model = true;
    if (! tag->value_valid) {
        lv_label_set_text(binding->value_label, "No value");
        lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
        binding->updating_from_model = false;
        return;
    }

    switch (tag->data_type) {
    case DATA_MODEL_TYPE_BOOLEAN: {
        bool displayed_value = tag->value.boolean_value;
        if (strcmp(tag->semantic_role, "operating_command") == 0) {
            data_model_tag_t operating_status;
            if (find_equipment_tag_by_role(binding->generator->data_model, tag->equipment_index,
                                           "operating_status", &operating_status) && operating_status.value_valid) {
                displayed_value = operating_status.value.boolean_value;
            }
        }
        if (binding->boolean_write_pending) {
            if (displayed_value == binding->pending_boolean_value) {
                binding->boolean_write_pending = false;
            } else if (lv_tick_elaps(binding->pending_write_started) < UI_BOOLEAN_WRITE_HOLD_MS) {
                binding->updating_from_model = false;
                return;
            } else {
                binding->boolean_write_pending = false;
            }
        }
        lv_label_set_text(binding->value_label, displayed_value ? "ON" : "OFF");
        lv_color_t state_color = boolean_state_color(tag, displayed_value);
        lv_obj_set_style_text_color(binding->value_label, state_color, LV_PART_MAIN);
        if (binding->widget_type == GENERATED_WIDGET_BOOLEAN_INDICATOR) {
            lv_obj_set_style_bg_color(binding->visual_object, state_color, LV_PART_MAIN);
        } else if (binding->widget_type == GENERATED_WIDGET_BOOLEAN_SWITCH) {
            if (displayed_value) {
                lv_obj_add_state(binding->visual_object, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(binding->visual_object, LV_STATE_CHECKED);
            }
        }
        break;
    }
    case DATA_MODEL_TYPE_INTEGER:
        {
            lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_ACCENT_SOFT), LV_PART_MAIN);
            char value_text[64];
            char display_unit[DATA_MODEL_UNIT_LENGTH];
            copy_display_unit(display_unit, sizeof(display_unit), tag->engineering_unit);
            snprintf(value_text, sizeof(value_text), "%lld%s%.23s", (long long)tag->value.integer_value,
                     display_unit[0] != '\0' ? " " : "", display_unit);
            lv_label_set_text(binding->value_label, value_text);
        }
        if (binding->visual_object != NULL) {
            int32_t scaled_value = scale_numeric_for_widget((double)tag->value.integer_value);
            if (binding->widget_type == GENERATED_WIDGET_NUMERIC_BAR) {
                lv_bar_set_value(binding->visual_object, scaled_value, LV_ANIM_OFF);
            } else {
                lv_slider_set_value(binding->visual_object, scaled_value, LV_ANIM_OFF);
            }
        }
        break;
    case DATA_MODEL_TYPE_FLOAT:
    case DATA_MODEL_TYPE_DOUBLE: {
        lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_ACCENT_SOFT), LV_PART_MAIN);
        set_numeric_label(binding->value_label, tag->value.numeric_value, tag->engineering_unit);
        if (!isfinite(tag->value.numeric_value)) {
            break;
        }
        int32_t scaled_value = scale_numeric_for_widget(tag->value.numeric_value);
        if (binding->widget_type == GENERATED_WIDGET_NUMERIC_BAR) {
            lv_bar_set_value(binding->visual_object, scaled_value, LV_ANIM_OFF);
        } else if (binding->widget_type == GENERATED_WIDGET_NUMERIC_SLIDER) {
            lv_slider_set_value(binding->visual_object, scaled_value, LV_ANIM_OFF);
        }
        break;
    }
    case DATA_MODEL_TYPE_STRING:
        lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_label_set_text(binding->value_label, tag->value.string_value);
        break;
    default:
        lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
        lv_label_set_text(binding->value_label, "Unsupported OPC UA type");
        break;
    }
    binding->updating_from_model = false;
}

static void boolean_switch_event(lv_event_t* event)
{
    generated_widget_binding_t* binding = lv_event_get_user_data(event);
    if (binding == NULL || binding->updating_from_model) {
        return;
    }
    bool value       = lv_obj_has_state(binding->visual_object, LV_STATE_CHECKED);
    data_model_tag_t command_tag;
    if (data_model_get_tag(binding->generator->data_model, binding->tag_index, &command_tag) &&
        strcmp(command_tag.semantic_role, "operating_command") == 0) {
        data_model_tag_t automatic_mode;
        if (find_equipment_tag_by_role(binding->generator->data_model, command_tag.equipment_index,
                                       "automatic_mode", &automatic_mode) && automatic_mode.value_valid &&
            automatic_mode.value.boolean_value) {
            esp_err_t mode_result =
                opcua_client_write_boolean(binding->generator->opcua_client, automatic_mode.index, false);
            if (mode_result != ESP_OK) {
                ESP_LOGE(TAG, "Cannot switch equipment to manual mode: %s", esp_err_to_name(mode_result));
                update_widget(binding, &command_tag);
                return;
            }
        }
    }
    esp_err_t result = opcua_client_write_boolean(binding->generator->opcua_client, binding->tag_index, value);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot queue Boolean write: %s", esp_err_to_name(result));
        binding->boolean_write_pending = false;
        data_model_tag_t tag;
        if (data_model_get_tag(binding->generator->data_model, binding->tag_index, &tag)) {
            update_widget(binding, &tag);
        }
        return;
    }
    binding->boolean_write_pending = true;
    binding->pending_boolean_value = value;
    binding->pending_write_started = lv_tick_get();
    lv_label_set_text(binding->value_label, value ? "ON" : "OFF");
}

static void numeric_slider_event(lv_event_t* event)
{
    generated_widget_binding_t* binding = lv_event_get_user_data(event);
    if (binding == NULL || binding->updating_from_model) {
        return;
    }
    double value     = lv_slider_get_value(binding->visual_object) / UI_NUMERIC_SCALE;
    esp_err_t result = opcua_client_write_number(binding->generator->opcua_client, binding->tag_index, value);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot queue numeric write: %s", esp_err_to_name(result));
    }
}

static lv_obj_t* create_settings_section_button(lv_obj_t* parent, const char* title)
{
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_size(button, lv_pct(100), 72);
    lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x111820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(0x666666), LV_PART_MAIN);
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

    /* As in the former HA lifecycle, configuration owns the network while
     * settings are open. This prevents OPC UA reconnect attempts from
     * competing with the Wi-Fi and OpenPLC HTTP portals. */
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

    generator->settings_section_buttons[SETTINGS_SECTION_SYSTEM] =
        create_settings_section_button(sidebar, "System");
    generator->settings_section_buttons[SETTINGS_SECTION_WIFI] =
        create_settings_section_button(sidebar, "Wi-Fi");
    generator->settings_section_buttons[SETTINGS_SECTION_OPENPLC] =
        create_settings_section_button(sidebar, "OpenPLC");
    for (unsigned section = 0; section < SETTINGS_SECTION_COUNT; ++section) {
        lv_obj_add_event_cb(generator->settings_section_buttons[section], settings_section_event,
                            LV_EVENT_CLICKED, generator);
    }

    lv_obj_t* sidebar_spacer = lv_obj_create(sidebar);
    lv_obj_remove_style_all(sidebar_spacer);
    lv_obj_set_width(sidebar_spacer, 1);
    lv_obj_set_flex_grow(sidebar_spacer, 1);

    lv_obj_t* back_button = lv_btn_create(sidebar);
    lv_obj_set_size(back_button, lv_pct(100), 68);
    lv_obj_set_style_radius(back_button, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_border_width(back_button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(back_button, lv_color_white(), LV_PART_MAIN);
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
    openplc_settings_create(generator->settings_pages[SETTINGS_SECTION_OPENPLC], endpoint_url,
                            apply_opcua_endpoint, generator);
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
                                      selected ? lv_color_hex(0x6BC1FF) : lv_color_hex(0x4A4A4A), LV_PART_MAIN);
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

static void style_page_as_grid(lv_obj_t* page)
{
    if (page == NULL) {
        return;
    }
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_COLOR_ACCENT), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(page, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
}

/** Apply the shared quiet surface treatment used outside the Settings UI. */
static void style_content_card(lv_obj_t* card, lv_color_t background, lv_coord_t radius)
{
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, background, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, radius, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
}

/** Create a full-width section marker that keeps generated groups easy to scan. */
static void create_section_heading(lv_obj_t* parent, const char* title, const char* detail)
{
    lv_obj_t* heading = lv_obj_create(parent);
    lv_obj_remove_style_all(heading);
    lv_obj_set_size(heading, lv_pct(97), 48);
    lv_obj_clear_flag(heading, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* accent = lv_obj_create(heading);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 5, 28);
    lv_obj_set_style_radius(accent, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(accent, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t* title_label = lv_label_create(heading);
    lv_label_set_text(title_label, title != NULL ? title : "");
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title_label, lv_pct(70));
    lv_obj_set_style_text_color(title_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 24, 0);

    if (detail != NULL && detail[0] != '\0') {
        lv_obj_t* detail_label = lv_label_create(heading);
        lv_label_set_text(detail_label, detail);
        lv_obj_set_style_text_color(detail_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(detail_label, LV_ALIGN_RIGHT_MID, 0, 1);
    }
}

/** Create a calm empty state without using a large semantic color field. */
static void create_empty_state(lv_obj_t* parent, const char* title, const char* detail, lv_color_t accent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(97), 124);
    style_content_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);

    lv_obj_t* indicator = lv_obj_create(card);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(indicator, 20, 20);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(indicator, accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(indicator, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* title_label = lv_label_create(card);
    lv_label_set_text(title_label, title != NULL ? title : "");
    lv_obj_set_style_text_color(title_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 40, -16);

    lv_obj_t* detail_label = lv_label_create(card);
    lv_label_set_text(detail_label, detail != NULL ? detail : "");
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(detail_label, lv_pct(82));
    lv_obj_set_style_text_color(detail_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(detail_label, LV_ALIGN_LEFT_MID, 40, 18);
}

/** Match the active-alarm visual language while clearly showing a healthy state. */
static void create_alarm_clear_state(lv_obj_t* parent)
{
    lv_color_t healthy_color = lv_color_hex(UI_COLOR_SUCCESS);
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(97), 118);
    style_content_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);

    lv_obj_t* status_badge = lv_obj_create(card);
    lv_obj_set_size(status_badge, 58, 58);
    lv_obj_clear_flag(status_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(status_badge, healthy_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_badge, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_color(status_badge, healthy_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_badge, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(status_badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_badge, 0, LV_PART_MAIN);
    lv_obj_align(status_badge, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* status_symbol = lv_label_create(status_badge);
    lv_label_set_text(status_symbol, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(status_symbol, healthy_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_symbol, &lv_font_montserrat_30, LV_PART_MAIN);
    lv_obj_center(status_symbol);

    lv_obj_t* title_label = lv_label_create(card);
    lv_label_set_text(title_label, "All equipment is normal");
    lv_obj_set_style_text_color(title_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 78, -15);

    lv_obj_t* detail_label = lv_label_create(card);
    lv_label_set_text(detail_label, "No active alarms reported by OPC UA");
    lv_obj_set_style_text_color(detail_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(detail_label, LV_ALIGN_LEFT_MID, 78, 17);
}

/** Map Boolean semantics to color without relying on equipment or tag names. */
static lv_color_t boolean_state_color(const data_model_tag_t* tag, bool value)
{
    if (! value) {
        return lv_color_hex(UI_COLOR_INACTIVE);
    }
    if (tag != NULL && strcmp(tag->semantic_role, "alarm_status") == 0) {
        return lv_color_hex(UI_COLOR_DANGER);
    }
    if (tag != NULL && strcmp(tag->semantic_role, "operating_status") == 0) {
        return lv_color_hex(UI_COLOR_SUCCESS);
    }
    return lv_color_hex(UI_COLOR_ACCENT);
}

/** Avoid invalidating labels when a periodic refresh produces identical text. */
static void set_label_text_if_changed(lv_obj_t* label, const char* text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char* current_text = lv_label_get_text(label);
    if (current_text == NULL || strcmp(current_text, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static generated_widget_binding_t* allocate_binding(ui_generator_t* generator)
{
    if (generator->bindings == NULL || generator->binding_count >= generator->binding_capacity) {
        return NULL;
    }
    return &generator->bindings[generator->binding_count++];
}

static void numeric_range(const data_model_tag_t* tag, double* minimum, double* maximum)
{
    *minimum = tag->has_minimum ? tag->minimum : 0.0;
    *maximum = tag->has_maximum ? tag->maximum : 100.0;
    if (*maximum <= *minimum) {
        *maximum = *minimum + 100.0;
    }
}

/**
 * Format a numeric OPC UA value without passing a double through LVGL's printf.
 * LV_SPRINTF_USE_FLOAT is disabled in this firmware, so using %f in
 * lv_label_set_text_fmt would leave the double argument unconsumed and corrupt
 * the following vararg.
 */
static void set_numeric_label(lv_obj_t* label, double value, const char* engineering_unit)
{
    if (!isfinite(value)) {
        lv_label_set_text(label, isnan(value) ? "NaN" : (value < 0.0 ? "-Infinity" : "Infinity"));
        return;
    }

    const double maximum_display_value = 9000000000000000.0;
    double magnitude                   = fabs(value);
    if (magnitude > maximum_display_value) {
        lv_label_set_text(label, "Out of display range");
        return;
    }

    unsigned long long scaled_value = (unsigned long long)llround(magnitude * UI_NUMERIC_SCALE);
    unsigned long long whole_value  = scaled_value / 100ULL;
    unsigned long long fraction     = scaled_value % 100ULL;
    char display_unit[DATA_MODEL_UNIT_LENGTH];
    copy_display_unit(display_unit, sizeof(display_unit), engineering_unit);
    const char* unit = display_unit;
    char value_text[64];
    snprintf(value_text, sizeof(value_text), "%s%llu.%02llu%s%.23s", signbit(value) ? "-" : "", whole_value,
             fraction, unit[0] != '\0' ? " " : "", unit);
    lv_label_set_text(label, value_text);
}

/** Keep unexpected sensor values inside LVGL's signed 32-bit widget range. */
static int32_t scale_numeric_for_widget(double value)
{
    double scaled_value = value * UI_NUMERIC_SCALE;
    if (!isfinite(scaled_value)) {
        return scaled_value < 0.0 ? INT32_MIN : INT32_MAX;
    }
    if (scaled_value <= (double)INT32_MIN) {
        return INT32_MIN;
    }
    if (scaled_value >= (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)lround(scaled_value);
}

/** Replace common superscript unit glyphs that are absent from the bundled Montserrat font. */
static void copy_display_unit(char* destination, size_t destination_size, const char* source)
{
    if (destination == NULL || destination_size == 0) {
        return;
    }
    destination[0] = '\0';
    if (source == NULL) {
        return;
    }

    size_t source_index      = 0;
    size_t destination_index = 0;
    while (source[source_index] != '\0' && destination_index + 1 < destination_size) {
        unsigned char first_byte  = (unsigned char)source[source_index];
        unsigned char second_byte = (unsigned char)source[source_index + 1];
        if (first_byte == 0xC2 && (second_byte == 0xB2 || second_byte == 0xB3)) {
            destination[destination_index++] = second_byte == 0xB2 ? '2' : '3';
            source_index += 2;
            continue;
        }
        destination[destination_index++] = source[source_index++];
    }
    destination[destination_index] = '\0';
}

static bool find_equipment_tag_by_role(const data_model_t* model, size_t equipment_index, const char* semantic_role,
                                       data_model_tag_t* tag_out)
{
    if (model == NULL || semantic_role == NULL || tag_out == NULL) {
        return false;
    }
    size_t tag_count = data_model_tag_count(model);
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(model, tag_index, &tag) && tag.equipment_index == equipment_index &&
            strcmp(tag.semantic_role, semantic_role) == 0) {
            *tag_out = tag;
            return true;
        }
    }
    return false;
}

static void copy_equipment_group_name(char* destination, size_t destination_size,
                                      const data_model_equipment_t* equipment)
{
    if (destination == NULL || destination_size == 0 || equipment == NULL) {
        return;
    }
    const char* source = equipment->display_name[0] != '\0' ? equipment->display_name : equipment->browse_name;
    copy_humanized_name(destination, destination_size, source);
    size_t destination_length = strnlen(destination, destination_size - 1);
    while (destination_length > 0 && destination[destination_length - 1] >= '0' &&
           destination[destination_length - 1] <= '9') {
        destination_length--;
    }
    while (destination_length > 0 && destination[destination_length - 1] == ' ') {
        destination_length--;
    }
    if (destination_length > 0) {
        destination[destination_length] = '\0';
    } else {
        copy_humanized_name(destination, destination_size, source);
    }
}

/** Make OPC UA BrowseNames readable without changing the discovered Data Model. */
static void copy_humanized_name(char* destination, size_t destination_size, const char* source)
{
    if (destination == NULL || destination_size == 0) {
        return;
    }
    destination[0] = '\0';
    if (source == NULL) {
        return;
    }

    size_t source_index = 0;
    size_t destination_index = 0;
    unsigned char previous = 0;
    while (source[source_index] != '\0' && destination_index + 1 < destination_size) {
        unsigned char current = (unsigned char)source[source_index];
        unsigned char next = (unsigned char)source[source_index + 1];
        bool current_is_upper = current >= 'A' && current <= 'Z';
        bool current_is_lower = current >= 'a' && current <= 'z';
        bool current_is_digit = current >= '0' && current <= '9';
        bool previous_is_upper = previous >= 'A' && previous <= 'Z';
        bool previous_is_lower = previous >= 'a' && previous <= 'z';
        bool previous_is_digit = previous >= '0' && previous <= '9';
        bool next_is_lower = next >= 'a' && next <= 'z';

        if (current == '_' || current == '-') {
            if (destination_index > 0 && destination[destination_index - 1] != ' ') {
                destination[destination_index++] = ' ';
            }
            previous = current;
            source_index++;
            continue;
        }

        bool needs_separator = destination_index > 0 && destination[destination_index - 1] != ' ' &&
                               ((current_is_upper && (previous_is_lower || (previous_is_upper && next_is_lower))) ||
                                (current_is_digit && (previous_is_lower || previous_is_upper)) ||
                                ((current_is_lower || current_is_upper) && previous_is_digit));
        if (needs_separator && destination_index + 1 < destination_size) {
            destination[destination_index++] = ' ';
        }
        destination[destination_index++] = (char)current;
        previous = current;
        source_index++;
    }
    while (destination_index > 0 && destination[destination_index - 1] == ' ') {
        destination_index--;
    }
    destination[destination_index] = '\0';
    if (destination[0] >= 'a' && destination[0] <= 'z') {
        destination[0] = (char)(destination[0] - ('a' - 'A'));
    }
}
