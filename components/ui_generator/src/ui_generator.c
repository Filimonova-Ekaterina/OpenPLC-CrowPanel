#include "ui_generator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "navigation.h"
#include "openplc_settings.h"
#include "system_settings.h"
#include "wifi_widget.h"
#include "wifi_menu.h"

#define UI_REFRESH_PERIOD_MS  250
#define UI_NAVIGATION_HEIGHT  76
#define UI_NUMERIC_SCALE      100.0
#define UI_OVERVIEW_TAG_LIMIT 6
#define UI_BOOLEAN_WRITE_HOLD_MS 2000
#define SETTINGS_SECTION_COUNT 3

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
    uint32_t observed_structure_generation;
    uint32_t observed_value_generation;
};

static const char* TAG = "ui_generator";
extern const lv_img_dsc_t settings_icon;

static void refresh_timer_callback(lv_timer_t* timer);
static void rebuild_interface(ui_generator_t* generator);
static void create_equipment_section(ui_generator_t* generator, lv_obj_t* page,
                                     const data_model_equipment_t* equipment);
static void create_tag_widget(ui_generator_t* generator, lv_obj_t* parent, const data_model_tag_t* tag,
                              bool include_equipment_name);
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
    lv_obj_set_style_bg_color(generator->root, lv_color_hex(0x080808), LV_PART_MAIN);
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
    lv_obj_set_style_bg_color(overview_card, lv_color_hex(0x181818), LV_PART_MAIN);
    lv_obj_set_style_border_color(overview_card, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_border_width(overview_card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(overview_card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overview_card, 20, LV_PART_MAIN);
    lv_obj_t* overview_title = lv_label_create(overview_card);
    lv_label_set_text_fmt(overview_title,
                          "%u equipment objects\n%u live tags\nInterface generated from OPC UA Address Space",
                          (unsigned)data_model_equipment_count(generator->data_model),
                          (unsigned)data_model_tag_count(generator->data_model));
    lv_obj_set_style_text_color(overview_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(overview_title, &lv_font_montserrat_22, LV_PART_MAIN);

    size_t discovered_tag_count = data_model_tag_count(generator->data_model);
    size_t overview_tag_count   = 0;
    for (size_t tag_index = 0; tag_index < discovered_tag_count && overview_tag_count < UI_OVERVIEW_TAG_LIMIT;
         ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) && tag.readable &&
            tag.data_type != DATA_MODEL_TYPE_UNKNOWN) {
            create_tag_widget(generator, overview_page, &tag, true);
            overview_tag_count++;
        }
    }

    lv_obj_t* equipment_page = navigation_add_page(generator->navigation, "Equipment");
    style_page_as_grid(equipment_page);
    size_t equipment_count = data_model_equipment_count(generator->data_model);
    for (size_t equipment_index = 0; equipment_index < equipment_count; ++equipment_index) {
        data_model_equipment_t equipment;
        if (data_model_get_equipment(generator->data_model, equipment_index, &equipment)) {
            create_equipment_section(generator, equipment_page, &equipment);
        }
    }

    lv_obj_t* controls_page = navigation_add_page(generator->navigation, "Controls");
    style_page_as_grid(controls_page);
    size_t writable_count = 0;
    size_t tag_count      = discovered_tag_count;
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) && tag.writable) {
            create_tag_widget(generator, controls_page, &tag, true);
            writable_count++;
        }
    }
    if (writable_count == 0) {
        lv_obj_t* empty_label = lv_label_create(controls_page);
        lv_label_set_text(empty_label, "No writable OPC UA variables discovered");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
        lv_obj_set_style_text_font(empty_label, &lv_font_montserrat_20, LV_PART_MAIN);
    }

    lv_obj_t* alarms_page  = navigation_add_page(generator->navigation, "Alarms");
    lv_obj_t* alarms_label = lv_label_create(alarms_page);
    lv_label_set_text(alarms_label, "Alarm events require OPC UA event-field discovery.\n"
                                    "Boolean alarm variables remain visible on equipment pages.");
    lv_obj_set_style_text_color(alarms_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(alarms_label, &lv_font_montserrat_20, LV_PART_MAIN);

    ESP_LOGI(TAG, "Generated UI for %u equipment objects and %u tags", (unsigned)equipment_count, (unsigned)tag_count);
}

static void create_equipment_section(ui_generator_t* generator, lv_obj_t* page,
                                     const data_model_equipment_t* equipment)
{
    const char* equipment_name = equipment->display_name[0] != '\0' ? equipment->display_name : equipment->browse_name;
    lv_obj_t* section_title    = lv_label_create(page);
    lv_label_set_text(section_title, equipment_name);
    lv_obj_set_width(section_title, lv_pct(100));
    lv_obj_set_style_text_color(section_title, lv_color_hex(0x6BC1FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(section_title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_pad_top(section_title, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(section_title, 2, LV_PART_MAIN);

    size_t matching_tags = 0;
    size_t tag_count     = data_model_tag_count(generator->data_model);
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(generator->data_model, tag_index, &tag) && tag.equipment_index == equipment->index) {
            create_tag_widget(generator, page, &tag, false);
            matching_tags++;
        }
    }
    if (matching_tags == 0) {
        lv_obj_t* empty_label = lv_label_create(page);
        lv_label_set_text(empty_label, "No variables on this object");
        lv_obj_set_width(empty_label, lv_pct(100));
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
        lv_obj_set_style_text_font(empty_label, &lv_font_montserrat_20, LV_PART_MAIN);
    }
}

static void create_tag_widget(ui_generator_t* generator, lv_obj_t* parent, const data_model_tag_t* tag,
                              bool include_equipment_name)
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
    lv_obj_set_size(card, 310, is_boolean ? 132 : 180);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x181818), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);

    lv_obj_t* name_label = lv_label_create(card);
    const char* tag_name = tag->display_name[0] != '\0' ? tag->display_name : tag->browse_name;
    char widget_title[(DATA_MODEL_NAME_LENGTH * 2) + 4];
    data_model_equipment_t equipment;
    if (include_equipment_name &&
        data_model_get_equipment(generator->data_model, tag->equipment_index, &equipment)) {
        const char* equipment_name =
            equipment.display_name[0] != '\0' ? equipment.display_name : equipment.browse_name;
        snprintf(widget_title, sizeof(widget_title), "%.63s / %.63s", equipment_name, tag_name);
        lv_label_set_text(name_label, widget_title);
    } else {
        lv_label_set_text(name_label, tag_name);
    }
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(name_label, lv_pct(100));
    lv_obj_set_style_text_color(name_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 0, 0);

    binding->value_label = lv_label_create(card);
    lv_obj_set_style_text_color(binding->value_label, lv_color_hex(0x2A96FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(binding->value_label,
                               is_boolean ? &lv_font_montserrat_22 : &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(binding->value_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (is_boolean) {
        if (tag->writable) {
            binding->widget_type   = GENERATED_WIDGET_BOOLEAN_SWITCH;
            binding->visual_object = lv_switch_create(card);
            lv_obj_set_size(binding->visual_object, 68, 38);
            lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(0x2A96FF),
                                      LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_align(binding->visual_object, LV_ALIGN_BOTTOM_RIGHT, 0, 2);
            lv_obj_add_event_cb(binding->visual_object, boolean_switch_event, LV_EVENT_VALUE_CHANGED, binding);
        } else {
            binding->widget_type   = GENERATED_WIDGET_BOOLEAN_INDICATOR;
            binding->visual_object = lv_obj_create(card);
            lv_obj_remove_style_all(binding->visual_object);
            lv_obj_set_size(binding->visual_object, 30, 30);
            lv_obj_set_style_radius(binding->visual_object, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(binding->visual_object, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_align(binding->visual_object, LV_ALIGN_BOTTOM_RIGHT, -4, 4);
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
        lv_obj_set_size(binding->visual_object, 270, 24);
        lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(0x2A96FF), LV_PART_INDICATOR);
        lv_obj_align(binding->visual_object, LV_ALIGN_CENTER, 0, 7);
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
        binding->updating_from_model = false;
        return;
    }

    switch (tag->data_type) {
    case DATA_MODEL_TYPE_BOOLEAN:
        if (binding->boolean_write_pending) {
            if (tag->value.boolean_value == binding->pending_boolean_value) {
                binding->boolean_write_pending = false;
            } else if (lv_tick_elaps(binding->pending_write_started) < UI_BOOLEAN_WRITE_HOLD_MS) {
                binding->updating_from_model = false;
                return;
            } else {
                binding->boolean_write_pending = false;
            }
        }
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
        {
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
                lv_bar_set_value(binding->visual_object, scaled_value, LV_ANIM_ON);
            } else {
                lv_slider_set_value(binding->visual_object, scaled_value, LV_ANIM_OFF);
            }
        }
        break;
    case DATA_MODEL_TYPE_FLOAT:
    case DATA_MODEL_TYPE_DOUBLE: {
        set_numeric_label(binding->value_label, tag->value.numeric_value, tag->engineering_unit);
        if (!isfinite(tag->value.numeric_value)) {
            break;
        }
        int32_t scaled_value = scale_numeric_for_widget(tag->value.numeric_value);
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
    lv_obj_set_style_pad_row(page, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, 18, LV_PART_MAIN);
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
