#include "ui_generator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "navigation.h"
#include "openplc_settings.h"
#include "wifi_menu.h"

#define UI_REFRESH_PERIOD_MS  250
#define UI_NAVIGATION_HEIGHT  58
#define UI_NUMERIC_SCALE      100.0
#define UI_OVERVIEW_TAG_LIMIT 8

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
} generated_widget_binding_t;

struct ui_generator
{
    lv_obj_t* root;
    lv_obj_t* status_indicator;
    lv_obj_t* status_label;
    lv_obj_t* content_host;
    navigation_t* navigation;
    lv_timer_t* refresh_timer;
    data_model_t* data_model;
    opcua_client_t* opcua_client;
    generated_widget_binding_t* bindings;
    size_t binding_capacity;
    size_t binding_count;
    uint32_t observed_structure_generation;
    uint32_t observed_value_generation;
};

static const char* TAG = "ui_generator";

static void refresh_timer_callback(lv_timer_t* timer);
static void rebuild_interface(ui_generator_t* generator);
static lv_obj_t* create_equipment_page(ui_generator_t* generator, const data_model_equipment_t* equipment);
static void create_tag_widget(ui_generator_t* generator, lv_obj_t* parent, const data_model_tag_t* tag);
static void update_widget(generated_widget_binding_t* binding, const data_model_tag_t* tag);
static void boolean_switch_event(lv_event_t* event);
static void numeric_slider_event(lv_event_t* event);
static void open_wifi_settings_event(lv_event_t* event);
static void close_wifi_settings_event(lv_event_t* event);
static esp_err_t apply_opcua_endpoint(const char* endpoint_url, void* user_data);
static void style_page_as_grid(lv_obj_t* page);
static generated_widget_binding_t* allocate_binding(ui_generator_t* generator);
static void numeric_range(const data_model_tag_t* tag, double* minimum, double* maximum);

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
    lv_obj_set_style_bg_color(generator->root, lv_color_hex(0x0B1218), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(generator->root, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* status_bar = lv_obj_create(generator->root);
    lv_obj_set_width(status_bar, lv_pct(100));
    lv_obj_set_height(status_bar, 52);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x17232D), LV_PART_MAIN);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(status_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(status_bar, open_wifi_settings_event, LV_EVENT_CLICKED, generator);

    generator->status_indicator = lv_obj_create(status_bar);
    lv_obj_remove_style_all(generator->status_indicator);
    lv_obj_set_size(generator->status_indicator, 16, 16);
    lv_obj_set_style_radius(generator->status_indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(generator->status_indicator, LV_OPA_COVER, LV_PART_MAIN);

    generator->status_label = lv_label_create(status_bar);
    lv_label_set_text(generator->status_label, "Starting OPC UA client...");
    lv_obj_set_style_text_color(generator->status_label, lv_color_hex(0xD8E8F2), LV_PART_MAIN);
    lv_label_set_long_mode(generator->status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(generator->status_label, 1);

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
    lv_label_set_text(generator->status_label, status_text);
    opcua_client_state_t state = opcua_client_get_state(generator->opcua_client);
    lv_color_t status_color    = lv_color_hex(0xF0A202);
    if (state == OPCUA_CLIENT_CONNECTED) {
        status_color = lv_color_hex(0x38B000);
    } else if (state == OPCUA_CLIENT_CONNECTION_ERROR || state == OPCUA_CLIENT_BROWSE_ERROR) {
        status_color = lv_color_hex(0xE63946);
    }
    lv_obj_set_style_bg_color(generator->status_indicator, status_color, LV_PART_MAIN);

    uint32_t value_generation = data_model_value_generation(generator->data_model);
    if (value_generation == generator->observed_value_generation) {
        return;
    }
    generator->observed_value_generation = value_generation;
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
    free(generator->bindings);
    generator->bindings         = NULL;
    generator->binding_count    = 0;
    generator->binding_capacity = data_model_tag_count(generator->data_model) * 3 + 1;
    generator->bindings         = calloc(generator->binding_capacity, sizeof(*generator->bindings));

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
    lv_obj_t* overview_card = lv_obj_create(overview_page);
    lv_obj_set_size(overview_card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(overview_card, lv_color_hex(0x17232D), LV_PART_MAIN);
    lv_obj_set_style_border_width(overview_card, 0, LV_PART_MAIN);
    lv_obj_t* overview_title = lv_label_create(overview_card);
    lv_label_set_text_fmt(overview_title,
                          "%u equipment objects\n%u live tags\nInterface generated from OPC UA Address Space",
                          (unsigned)data_model_equipment_count(generator->data_model),
                          (unsigned)data_model_tag_count(generator->data_model));
    lv_obj_set_style_text_color(overview_title, lv_color_hex(0xD8E8F2), LV_PART_MAIN);

    size_t discovered_tag_count = data_model_tag_count(generator->data_model);
    size_t overview_tag_count   = 0;
    for (size_t tag_index = 0; tag_index < discovered_tag_count && overview_tag_count < UI_OVERVIEW_TAG_LIMIT;
         ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) && tag.readable &&
            tag.data_type != DATA_MODEL_TYPE_UNKNOWN) {
            create_tag_widget(generator, overview_page, &tag);
            overview_tag_count++;
        }
    }

    size_t equipment_count = data_model_equipment_count(generator->data_model);
    for (size_t equipment_index = 0; equipment_index < equipment_count; ++equipment_index) {
        data_model_equipment_t equipment;
        if (data_model_get_equipment(generator->data_model, equipment_index, &equipment)) {
            create_equipment_page(generator, &equipment);
        }
    }

    lv_obj_t* controls_page = navigation_add_page(generator->navigation, "Controls");
    style_page_as_grid(controls_page);
    size_t writable_count = 0;
    size_t tag_count      = discovered_tag_count;
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) && tag.writable) {
            create_tag_widget(generator, controls_page, &tag);
            writable_count++;
        }
    }
    if (writable_count == 0) {
        lv_obj_t* empty_label = lv_label_create(controls_page);
        lv_label_set_text(empty_label, "No writable OPC UA variables discovered");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x8FAAB8), LV_PART_MAIN);
    }

    lv_obj_t* trends_page  = navigation_add_page(generator->navigation, "Trends");
    lv_obj_t* trends_label = lv_label_create(trends_page);
    lv_label_set_text(trends_label, "Numeric tags discovered. Trend history is reserved for the next phase.");
    lv_label_set_long_mode(trends_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(trends_label, lv_pct(100));
    lv_obj_set_style_text_color(trends_label, lv_color_hex(0xD8E8F2), LV_PART_MAIN);

    lv_obj_t* alarms_page  = navigation_add_page(generator->navigation, "Alarms");
    lv_obj_t* alarms_label = lv_label_create(alarms_page);
    lv_label_set_text(alarms_label, "Alarm events require OPC UA event-field discovery.\n"
                                    "Boolean alarm variables remain visible on equipment pages.");
    lv_obj_set_style_text_color(alarms_label, lv_color_hex(0xD8E8F2), LV_PART_MAIN);

    lv_obj_t* settings_page   = navigation_add_page(generator->navigation, "Settings");
    lv_obj_t* settings_button = lv_btn_create(settings_page);
    lv_obj_set_size(settings_button, 260, 64);
    lv_obj_center(settings_button);
    lv_obj_add_event_cb(settings_button, open_wifi_settings_event, LV_EVENT_CLICKED, generator);
    lv_obj_t* settings_label = lv_label_create(settings_button);
    lv_label_set_text(settings_label, "Open Settings");
    lv_obj_center(settings_label);

    ESP_LOGI(TAG, "Generated UI for %u equipment objects and %u tags", (unsigned)equipment_count, (unsigned)tag_count);
}

static lv_obj_t* create_equipment_page(ui_generator_t* generator, const data_model_equipment_t* equipment)
{
    char short_title[24];
    snprintf(short_title, sizeof(short_title), "%s",
             equipment->display_name[0] != '\0' ? equipment->display_name : equipment->browse_name);
    lv_obj_t* page = navigation_add_page(generator->navigation, short_title);
    style_page_as_grid(page);

    size_t matching_tags = 0;
    size_t tag_count     = data_model_tag_count(generator->data_model);
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) && tag.equipment_index == equipment->index) {
            create_tag_widget(generator, page, &tag);
            matching_tags++;
        }
    }
    if (matching_tags == 0) {
        lv_obj_t* empty_label = lv_label_create(page);
        lv_label_set_text(empty_label, "Object contains child equipment; select a child tab");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x8FAAB8), LV_PART_MAIN);
    }
    return page;
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

    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, 250, 138);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x17232D), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x28404F), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);

    lv_obj_t* name_label = lv_label_create(card);
    lv_label_set_text(name_label, tag->display_name);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, lv_pct(100));
    lv_obj_set_style_text_color(name_label, lv_color_hex(0xD8E8F2), LV_PART_MAIN);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 0, 0);

    binding->value_label = lv_label_create(card);
    lv_obj_set_style_text_color(binding->value_label, lv_color_hex(0x7FDBFF), LV_PART_MAIN);
    lv_obj_align(binding->value_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (tag->data_type == DATA_MODEL_TYPE_BOOLEAN) {
        if (tag->writable) {
            binding->widget_type   = GENERATED_WIDGET_BOOLEAN_SWITCH;
            binding->visual_object = lv_switch_create(card);
            lv_obj_align(binding->visual_object, LV_ALIGN_RIGHT_MID, 0, 4);
            lv_obj_add_event_cb(binding->visual_object, boolean_switch_event, LV_EVENT_VALUE_CHANGED, binding);
        } else {
            binding->widget_type   = GENERATED_WIDGET_BOOLEAN_INDICATOR;
            binding->visual_object = lv_obj_create(card);
            lv_obj_remove_style_all(binding->visual_object);
            lv_obj_set_size(binding->visual_object, 42, 42);
            lv_obj_set_style_radius(binding->visual_object, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(binding->visual_object, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_align(binding->visual_object, LV_ALIGN_RIGHT_MID, -8, 4);
        }
    } else if (tag->data_type == DATA_MODEL_TYPE_FLOAT || tag->data_type == DATA_MODEL_TYPE_DOUBLE ||
               tag->data_type == DATA_MODEL_TYPE_INTEGER) {
        double minimum;
        double maximum;
        numeric_range(tag, &minimum, &maximum);
        if (tag->writable) {
            binding->widget_type   = GENERATED_WIDGET_NUMERIC_SLIDER;
            binding->visual_object = lv_slider_create(card);
            lv_slider_set_range(binding->visual_object, (int32_t)lround(minimum * UI_NUMERIC_SCALE),
                                (int32_t)lround(maximum * UI_NUMERIC_SCALE));
            lv_obj_add_event_cb(binding->visual_object, numeric_slider_event, LV_EVENT_RELEASED, binding);
        } else {
            binding->widget_type   = GENERATED_WIDGET_NUMERIC_BAR;
            binding->visual_object = lv_bar_create(card);
            lv_bar_set_range(binding->visual_object, (int32_t)lround(minimum * UI_NUMERIC_SCALE),
                             (int32_t)lround(maximum * UI_NUMERIC_SCALE));
        }
        lv_obj_set_size(binding->visual_object, 210, 18);
        lv_obj_align(binding->visual_object, LV_ALIGN_CENTER, 0, 7);
    } else {
        binding->widget_type = GENERATED_WIDGET_TEXT;
    }
    update_widget(binding, tag);
}

static void update_widget(generated_widget_binding_t* binding, const data_model_tag_t* tag)
{
    binding->updating_from_model = true;
    if (! tag->value_valid) {
        lv_label_set_text(binding->value_label, "No value");
        binding->updating_from_model = false;
        return;
    }

    switch (tag->data_type) {
    case DATA_MODEL_TYPE_BOOLEAN:
        lv_label_set_text(binding->value_label, tag->value.boolean_value ? "ON" : "OFF");
        if (binding->widget_type == GENERATED_WIDGET_BOOLEAN_INDICATOR) {
            lv_obj_set_style_bg_color(binding->visual_object,
                                      tag->value.boolean_value ? lv_color_hex(0x38B000) : lv_color_hex(0xE63946),
                                      LV_PART_MAIN);
        } else if (binding->widget_type == GENERATED_WIDGET_BOOLEAN_SWITCH) {
            if (tag->value.boolean_value) {
                lv_obj_add_state(binding->visual_object, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(binding->visual_object, LV_STATE_CHECKED);
            }
        }
        break;
    case DATA_MODEL_TYPE_INTEGER:
        lv_label_set_text_fmt(binding->value_label, "%lld %s", (long long)tag->value.integer_value,
                              tag->engineering_unit);
        if (binding->visual_object != NULL) {
            int32_t scaled_value = (int32_t)(tag->value.integer_value * UI_NUMERIC_SCALE);
            if (binding->widget_type == GENERATED_WIDGET_NUMERIC_BAR) {
                lv_bar_set_value(binding->visual_object, scaled_value, LV_ANIM_ON);
            } else {
                lv_slider_set_value(binding->visual_object, scaled_value, LV_ANIM_OFF);
            }
        }
        break;
    case DATA_MODEL_TYPE_FLOAT:
    case DATA_MODEL_TYPE_DOUBLE: {
        lv_label_set_text_fmt(binding->value_label, "%.2f %s", tag->value.numeric_value, tag->engineering_unit);
        int32_t scaled_value = (int32_t)lround(tag->value.numeric_value * UI_NUMERIC_SCALE);
        if (binding->widget_type == GENERATED_WIDGET_NUMERIC_BAR) {
            lv_bar_set_value(binding->visual_object, scaled_value, LV_ANIM_ON);
        } else if (binding->widget_type == GENERATED_WIDGET_NUMERIC_SLIDER) {
            lv_slider_set_value(binding->visual_object, scaled_value, LV_ANIM_OFF);
        }
        break;
    }
    case DATA_MODEL_TYPE_STRING:
        lv_label_set_text(binding->value_label, tag->value.string_value);
        break;
    default:
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
    esp_err_t result = opcua_client_write_boolean(binding->generator->opcua_client, binding->tag_index, value);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot queue Boolean write: %s", esp_err_to_name(result));
    }
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

static void open_wifi_settings_event(lv_event_t* event)
{
    ui_generator_t* generator = lv_event_get_user_data(event);
    lv_obj_t* overlay         = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* header = lv_obj_create(overlay);
    lv_obj_set_size(header, lv_pct(100), 56);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x17232D), LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t* close_button = lv_btn_create(header);
    lv_obj_set_size(close_button, 120, 48);
    lv_obj_align(close_button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(close_button, close_wifi_settings_event, LV_EVENT_CLICKED, overlay);
    lv_obj_t* close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, "Back");
    lv_obj_center(close_label);

    lv_obj_t* tabview = lv_tabview_create(overlay, LV_DIR_TOP, 48);
    lv_obj_set_width(tabview, lv_pct(100));
    lv_obj_set_flex_grow(tabview, 1);
    lv_obj_t* wifi_page    = lv_tabview_add_tab(tabview, "Wi-Fi");
    lv_obj_t* openplc_page = lv_tabview_add_tab(tabview, "OpenPLC");

    wifi_menu_create(wifi_page, lv_layer_top());
    wifi_menu_activate();

    char endpoint_url[OPCUA_CLIENT_ENDPOINT_LENGTH];
    opcua_client_get_endpoint(generator->opcua_client, endpoint_url, sizeof(endpoint_url));
    openplc_settings_create(openplc_page, endpoint_url, apply_opcua_endpoint, generator);
}

static void close_wifi_settings_event(lv_event_t* event)
{
    lv_obj_t* overlay = lv_event_get_user_data(event);
    wifi_menu_deactivate();
    if (overlay != NULL && lv_obj_is_valid(overlay)) {
        lv_obj_del(overlay);
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
    lv_obj_set_style_pad_row(page, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, 12, LV_PART_MAIN);
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
