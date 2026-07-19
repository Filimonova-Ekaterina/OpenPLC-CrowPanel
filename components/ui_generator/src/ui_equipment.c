#include "ui_equipment.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "ui_theme.h"

#define EQUIPMENT_NUMERIC_SCALE       100.0
#define EQUIPMENT_WRITE_HOLD_MS       2000
#define EQUIPMENT_IDENTITY_WIDTH      220
#define EQUIPMENT_METRICS_PER_ROW     4
#define EQUIPMENT_CONTROLS_PER_ROW    4
#define EQUIPMENT_METRIC_CELL_WIDTH   238
#define EQUIPMENT_METRIC_CELL_HEIGHT  78
#define EQUIPMENT_CONTROL_CELL_HEIGHT 96

typedef enum
{
    EQUIPMENT_BINDING_VALUE,
    EQUIPMENT_BINDING_BOOLEAN_SWITCH,
    EQUIPMENT_BINDING_NUMERIC_SLIDER,
} equipment_binding_type_t;

typedef struct
{
    struct ui_equipment_view* view;
    size_t tag_index;
    equipment_binding_type_t type;
    lv_obj_t* value_label;
    lv_obj_t* visual_object;
    bool updating_from_model;
    bool displayed_boolean_valid;
    bool displayed_boolean_value;
    bool boolean_write_pending;
    bool pending_boolean_value;
    uint32_t pending_write_started;
} equipment_tag_binding_t;

typedef struct
{
    size_t equipment_index;
    size_t status_tag_index;
    size_t alarm_tag_index;
    lv_obj_t* icon_badge;
    lv_obj_t* state_badge;
    lv_obj_t* state_label;
    bool displayed_valid;
    bool displayed_running;
    bool displayed_alarm;
} equipment_state_binding_t;

struct ui_equipment_view
{
    data_model_t* data_model;
    opcua_client_t* opcua_client;
    ui_equipment_page_mode_t mode;
    equipment_tag_binding_t* tag_bindings;
    size_t tag_binding_capacity;
    size_t tag_binding_count;
    equipment_state_binding_t* state_bindings;
    size_t state_binding_capacity;
    size_t state_binding_count;
};

static const char* TAG = "ui_equipment";

static void create_equipment_card(ui_equipment_view_t* view, lv_obj_t* page, const data_model_equipment_t* equipment);
static void create_controls_card(ui_equipment_view_t* view, lv_obj_t* page, const data_model_equipment_t* equipment);
static void create_equipment_header(ui_equipment_view_t* view, lv_obj_t* card, const data_model_equipment_t* equipment);
static lv_obj_t* create_identity(ui_equipment_view_t* view, lv_obj_t* card, const data_model_equipment_t* equipment,
                                 lv_coord_t card_height);
static void create_metric(ui_equipment_view_t* view, lv_obj_t* parent, const data_model_tag_t* tag);
static void create_control(ui_equipment_view_t* view, lv_obj_t* parent, const data_model_tag_t* tag, lv_coord_t width);
static equipment_tag_binding_t* allocate_tag_binding(ui_equipment_view_t* view);
static equipment_state_binding_t* allocate_state_binding(ui_equipment_view_t* view);
static void update_tag_binding(equipment_tag_binding_t* binding, const data_model_tag_t* tag);
static void boolean_switch_event(lv_event_t* event);
static void numeric_slider_event(lv_event_t* event);
static int32_t scale_numeric(double value);
static void numeric_range(const data_model_tag_t* tag, double* minimum, double* maximum);
static bool is_status_role(const data_model_tag_t* tag);
static void create_equipment_monogram(char* destination, size_t destination_size, const char* human_name);

ui_equipment_view_t* ui_equipment_create(lv_obj_t* page, data_model_t* data_model, opcua_client_t* opcua_client,
                                         ui_equipment_page_mode_t mode)
{
    if (page == NULL || data_model == NULL || opcua_client == NULL) {
        return NULL;
    }
    ui_equipment_view_t* view = calloc(1, sizeof(*view));
    if (view == NULL) {
        return NULL;
    }
    view->data_model             = data_model;
    view->opcua_client           = opcua_client;
    view->mode                   = mode;
    view->tag_binding_capacity   = data_model_tag_count(data_model) + 1;
    view->state_binding_capacity = data_model_equipment_count(data_model) + 1;
    view->tag_bindings           = calloc(view->tag_binding_capacity, sizeof(*view->tag_bindings));
    view->state_bindings         = calloc(view->state_binding_capacity, sizeof(*view->state_bindings));
    if (view->tag_bindings == NULL || view->state_bindings == NULL) {
        ui_equipment_destroy(view);
        return NULL;
    }

    size_t equipment_count = data_model_equipment_count(data_model);
    size_t writable_total  = 0;
    for (size_t tag_index = 0; tag_index < data_model_tag_count(data_model); ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(data_model, tag_index, &tag) && tag.writable) {
            writable_total++;
        }
    }

    char detail[64];
    if (mode == UI_EQUIPMENT_PAGE_DETAILS) {
        snprintf(detail, sizeof(detail), "%u discovered  |  live", (unsigned)equipment_count);
        ui_theme_create_heading(page, "Equipment", detail);
    } else {
        snprintf(detail, sizeof(detail), "%u writable variables", (unsigned)writable_total);
        ui_theme_create_heading(page, "Controls", detail);
    }

    if (equipment_count == 0) {
        ui_theme_create_empty_state(page, "Waiting for equipment", "Cards appear automatically after OPC UA discovery",
                                    lv_color_hex(UI_COLOR_ACCENT));
        return view;
    }

    size_t created_cards = 0;
    for (size_t equipment_index = 0; equipment_index < equipment_count; ++equipment_index) {
        data_model_equipment_t equipment;
        if (! data_model_get_equipment(data_model, equipment_index, &equipment)) {
            continue;
        }
        if (mode == UI_EQUIPMENT_PAGE_DETAILS) {
            create_equipment_card(view, page, &equipment);
            created_cards++;
        } else {
            size_t before = view->tag_binding_count;
            create_controls_card(view, page, &equipment);
            if (view->tag_binding_count > before) {
                created_cards++;
            }
        }
    }
    if (mode == UI_EQUIPMENT_PAGE_CONTROLS && created_cards == 0) {
        ui_theme_create_empty_state(page, "No writable variables",
                                    "Controls appear automatically when OPC UA write access is discovered",
                                    lv_color_hex(UI_COLOR_ACCENT));
    }
    ui_equipment_update(view);
    return view;
}

static void create_equipment_card(ui_equipment_view_t* view, lv_obj_t* page, const data_model_equipment_t* equipment)
{
    size_t tag_count     = data_model_tag_count(view->data_model);
    size_t metric_count  = 0;
    size_t control_count = 0;
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (! data_model_get_tag(view->data_model, tag_index, &tag) || tag.equipment_index != equipment->index) {
            continue;
        }
        if (tag.writable) {
            control_count++;
        } else if (tag.readable && ! is_status_role(&tag)) {
            metric_count++;
        }
    }

    size_t metric_rows         = (metric_count + EQUIPMENT_METRICS_PER_ROW - 1) / EQUIPMENT_METRICS_PER_ROW;
    size_t control_rows        = (control_count + EQUIPMENT_CONTROLS_PER_ROW - 1) / EQUIPMENT_CONTROLS_PER_ROW;
    lv_coord_t metrics_height  = (lv_coord_t)(metric_rows * 86);
    lv_coord_t controls_height = (lv_coord_t)(control_rows * 104);
    lv_coord_t section_gap     = metric_count > 0 && control_count > 0 ? 8 : 0;
    lv_coord_t card_height     = 74 + metrics_height + section_gap + controls_height;

    lv_obj_t* card = lv_obj_create(page);
    lv_obj_set_size(card, lv_pct(100), card_height);
    ui_theme_style_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    create_equipment_header(view, card, equipment);

    lv_obj_t* metrics = lv_obj_create(card);
    lv_obj_remove_style_all(metrics);
    lv_obj_set_size(metrics, lv_pct(100), metrics_height);
    lv_obj_set_pos(metrics, 0, 50);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(metrics, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(metrics, 8, LV_PART_MAIN);
    lv_obj_clear_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);

    if (control_count > 0) {
        lv_obj_t* controls = lv_obj_create(card);
        lv_obj_remove_style_all(controls);
        lv_obj_set_size(controls, lv_pct(100), controls_height);
        lv_obj_set_pos(controls, 0, 50 + metrics_height + section_gap);
        lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(controls, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_column(controls, 8, LV_PART_MAIN);
        lv_obj_clear_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

        for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
            data_model_tag_t tag;
            if (data_model_get_tag(view->data_model, tag_index, &tag) && tag.equipment_index == equipment->index &&
                tag.writable) {
                create_control(view, controls, &tag, 230);
            }
        }
    }

    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(view->data_model, tag_index, &tag) && tag.equipment_index == equipment->index &&
            tag.readable && ! tag.writable && ! is_status_role(&tag)) {
            create_metric(view, metrics, &tag);
        }
    }
}

static void create_controls_card(ui_equipment_view_t* view, lv_obj_t* page, const data_model_equipment_t* equipment)
{
    size_t tag_count      = data_model_tag_count(view->data_model);
    size_t writable_count = 0;
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(view->data_model, tag_index, &tag) && tag.equipment_index == equipment->index &&
            tag.writable) {
            writable_count++;
        }
    }
    if (writable_count == 0) {
        return;
    }
    size_t rows            = (writable_count + 2) / 3;
    lv_coord_t card_height = (lv_coord_t)(rows * 106 + 32);
    if (card_height < 144)
        card_height = 144;

    lv_obj_t* card = lv_obj_create(page);
    lv_obj_set_size(card, lv_pct(100), card_height);
    ui_theme_style_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    create_identity(view, card, equipment, card_height);

    lv_obj_t* controls = lv_obj_create(card);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, 734, card_height - 24);
    lv_obj_set_pos(controls, EQUIPMENT_IDENTITY_WIDTH + 14, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(controls, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(controls, 10, LV_PART_MAIN);
    lv_obj_clear_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(view->data_model, tag_index, &tag) && tag.equipment_index == equipment->index &&
            tag.writable) {
            create_control(view, controls, &tag, 232);
        }
    }
}

/** Create a compact equipment header so the data grid can use the full card width. */
static void create_equipment_header(ui_equipment_view_t* view, lv_obj_t* card, const data_model_equipment_t* equipment)
{
    equipment_state_binding_t* state = allocate_state_binding(view);
    if (state != NULL) {
        state->equipment_index  = equipment->index;
        state->status_tag_index = DATA_MODEL_INVALID_INDEX;
        state->alarm_tag_index  = DATA_MODEL_INVALID_INDEX;
        data_model_tag_t role_tag;
        if (ui_theme_find_tag_by_role(view->data_model, equipment->index, "operating_status", &role_tag)) {
            state->status_tag_index = role_tag.index;
        }
        if (ui_theme_find_tag_by_role(view->data_model, equipment->index, "alarm_status", &role_tag)) {
            state->alarm_tag_index = role_tag.index;
        }
    }

    lv_obj_t* icon_badge = lv_obj_create(card);
    lv_obj_set_size(icon_badge, 42, 42);
    ui_theme_style_card(icon_badge, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_color(icon_badge, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(icon_badge, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon_badge, 0, LV_PART_MAIN);
    lv_obj_set_pos(icon_badge, 0, 0);
    if (state != NULL) {
        state->icon_badge = icon_badge;
    }

    const char* source_name = equipment->display_name[0] != '\0' ? equipment->display_name : equipment->browse_name;
    char human_name[UI_HUMAN_NAME_LENGTH];
    ui_theme_humanize_name(human_name, sizeof(human_name), source_name);
    char monogram[5];
    create_equipment_monogram(monogram, sizeof(monogram), human_name);
    lv_obj_t* icon = lv_label_create(icon_badge);
    lv_label_set_text(icon, monogram);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ACCENT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(icon);

    lv_obj_t* name = lv_label_create(card);
    lv_label_set_text(name, human_name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 650);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 56, 9);

    lv_obj_t* state_badge = lv_obj_create(card);
    lv_obj_set_size(state_badge, 112, 32);
    ui_theme_style_card(state_badge, lv_color_hex(UI_COLOR_SURFACE_RAISED), 9);
    lv_obj_set_style_pad_all(state_badge, 0, LV_PART_MAIN);
    lv_obj_align(state_badge, LV_ALIGN_TOP_RIGHT, 0, 5);
    lv_obj_t* state_label = lv_label_create(state_badge);
    lv_label_set_text(state_label, "AVAILABLE");
    lv_obj_set_style_text_color(state_label, lv_color_hex(UI_COLOR_INACTIVE), LV_PART_MAIN);
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(state_label);
    if (state != NULL) {
        state->state_badge = state_badge;
        state->state_label = state_label;
    }
}

static lv_obj_t* create_identity(ui_equipment_view_t* view, lv_obj_t* card, const data_model_equipment_t* equipment,
                                 lv_coord_t card_height)
{
    lv_obj_t* identity = lv_obj_create(card);
    lv_obj_remove_style_all(identity);
    lv_obj_set_size(identity, EQUIPMENT_IDENTITY_WIDTH, card_height - 24);
    lv_obj_set_pos(identity, 0, 0);
    lv_obj_clear_flag(identity, LV_OBJ_FLAG_SCROLLABLE);

    equipment_state_binding_t* state = allocate_state_binding(view);
    if (state != NULL) {
        state->equipment_index  = equipment->index;
        state->status_tag_index = DATA_MODEL_INVALID_INDEX;
        state->alarm_tag_index  = DATA_MODEL_INVALID_INDEX;
        data_model_tag_t role_tag;
        if (ui_theme_find_tag_by_role(view->data_model, equipment->index, "operating_status", &role_tag)) {
            state->status_tag_index = role_tag.index;
        }
        if (ui_theme_find_tag_by_role(view->data_model, equipment->index, "alarm_status", &role_tag)) {
            state->alarm_tag_index = role_tag.index;
        }
    }

    lv_obj_t* icon_badge = lv_obj_create(identity);
    lv_obj_set_size(icon_badge, 56, 56);
    ui_theme_style_card(icon_badge, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_color(icon_badge, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(icon_badge, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon_badge, 0, LV_PART_MAIN);
    lv_obj_align(icon_badge, LV_ALIGN_LEFT_MID, 4, 0);
    if (state != NULL)
        state->icon_badge = icon_badge;

    const char* source_name = equipment->display_name[0] != '\0' ? equipment->display_name : equipment->browse_name;
    char human_name[UI_HUMAN_NAME_LENGTH];
    ui_theme_humanize_name(human_name, sizeof(human_name), source_name);
    char monogram[5];
    create_equipment_monogram(monogram, sizeof(monogram), human_name);
    lv_obj_t* icon = lv_label_create(icon_badge);
    lv_label_set_text(icon, monogram);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ACCENT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(icon);

    lv_obj_t* name = lv_label_create(identity);
    lv_label_set_text(name, human_name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 145);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 72, -23);

    lv_obj_t* state_badge = lv_obj_create(identity);
    lv_obj_set_size(state_badge, 100, 30);
    ui_theme_style_card(state_badge, lv_color_hex(UI_COLOR_SURFACE_RAISED), 8);
    lv_obj_set_style_pad_all(state_badge, 0, LV_PART_MAIN);
    lv_obj_align(state_badge, LV_ALIGN_LEFT_MID, 72, 18);
    lv_obj_t* state_label = lv_label_create(state_badge);
    lv_label_set_text(state_label, "AVAILABLE");
    lv_obj_set_style_text_color(state_label, lv_color_hex(UI_COLOR_INACTIVE), LV_PART_MAIN);
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(state_label);
    if (state != NULL) {
        state->state_badge = state_badge;
        state->state_label = state_label;
    }

    lv_obj_t* divider = lv_obj_create(card);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 1, card_height - 34);
    lv_obj_set_style_bg_color(divider, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(divider, EQUIPMENT_IDENTITY_WIDTH, 5);
    return identity;
}

static void create_metric(ui_equipment_view_t* view, lv_obj_t* parent, const data_model_tag_t* tag)
{
    equipment_tag_binding_t* binding = allocate_tag_binding(view);
    if (binding == NULL) {
        return;
    }
    binding->view      = view;
    binding->tag_index = tag->index;
    binding->type      = EQUIPMENT_BINDING_VALUE;

    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, EQUIPMENT_METRIC_CELL_WIDTH, EQUIPMENT_METRIC_CELL_HEIGHT);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cell, 8, LV_PART_MAIN);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    const char* source_name = tag->display_name[0] != '\0' ? tag->display_name : tag->browse_name;
    char human_name[UI_HUMAN_NAME_LENGTH];
    ui_theme_humanize_name(human_name, sizeof(human_name), source_name);
    lv_coord_t identity_width =
        lv_txt_get_width(human_name, (uint32_t)strlen(human_name), &lv_font_montserrat_12, 0, LV_TEXT_FLAG_NONE) + 6;
    if (identity_width < 64)
        identity_width = 64;
    if (identity_width > 98)
        identity_width = 98;

    lv_obj_t* identity = lv_obj_create(cell);
    lv_obj_remove_style_all(identity);
    lv_obj_set_size(identity, identity_width, EQUIPMENT_METRIC_CELL_HEIGHT);
    lv_obj_clear_flag(identity, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon_badge = lv_obj_create(identity);
    lv_obj_set_size(icon_badge, 34, 34);
    ui_theme_style_card(icon_badge, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_color(icon_badge, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon_badge, 0, LV_PART_MAIN);
    lv_obj_align(icon_badge, LV_ALIGN_TOP_MID, 0, 0);
    ui_theme_create_tag_icon(icon_badge, tag, 20);

    lv_obj_t* name = lv_label_create(identity);
    lv_label_set_text(name, human_name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, identity_width);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 42);

    binding->value_label = lv_label_create(cell);
    lv_label_set_text(binding->value_label, "No value");
    lv_label_set_long_mode(binding->value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(binding->value_label, 1);
    lv_obj_set_flex_grow(binding->value_label, 1);
    lv_obj_set_style_text_align(binding->value_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(binding->value_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_translate_y(binding->value_label, -4, LV_PART_MAIN);
}

static void create_control(ui_equipment_view_t* view, lv_obj_t* parent, const data_model_tag_t* tag, lv_coord_t width)
{
    equipment_tag_binding_t* binding = allocate_tag_binding(view);
    if (binding == NULL) {
        return;
    }
    binding->view      = view;
    binding->tag_index = tag->index;

    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_set_size(cell, width, EQUIPMENT_CONTROL_CELL_HEIGHT);
    ui_theme_style_card(cell, lv_color_hex(UI_COLOR_SURFACE_RAISED), 14);
    lv_obj_set_style_pad_all(cell, 10, LV_PART_MAIN);

    const char* source_name = tag->display_name[0] != '\0' ? tag->display_name : tag->browse_name;
    char human_name[UI_HUMAN_NAME_LENGTH];
    ui_theme_humanize_name(human_name, sizeof(human_name), source_name);
    lv_obj_t* name = lv_label_create(cell);
    lv_label_set_text(name, human_name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(name, width - 20, 36);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT_LABEL), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

    binding->value_label = lv_label_create(cell);
    lv_label_set_text(binding->value_label, "OFF");
    lv_label_set_long_mode(binding->value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(binding->value_label, width - 98);
    lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_ACCENT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        binding->value_label, view->mode == UI_EQUIPMENT_PAGE_DETAILS ? &lv_font_montserrat_20 : &lv_font_montserrat_16,
        LV_PART_MAIN);
    lv_obj_align(binding->value_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (tag->data_type == DATA_MODEL_TYPE_BOOLEAN) {
        binding->type          = EQUIPMENT_BINDING_BOOLEAN_SWITCH;
        binding->visual_object = lv_switch_create(cell);
        lv_obj_set_size(binding->visual_object, 68, 34);
        lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(binding->visual_object, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_ACCENT),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_KNOB);
        lv_obj_set_style_pad_all(binding->visual_object, 2, LV_PART_KNOB);
        lv_obj_align(binding->visual_object, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_add_event_cb(binding->visual_object, boolean_switch_event, LV_EVENT_VALUE_CHANGED, binding);
    } else if (tag->data_type == DATA_MODEL_TYPE_INTEGER || tag->data_type == DATA_MODEL_TYPE_FLOAT ||
               tag->data_type == DATA_MODEL_TYPE_DOUBLE) {
        binding->type          = EQUIPMENT_BINDING_NUMERIC_SLIDER;
        binding->visual_object = lv_slider_create(cell);
        double minimum;
        double maximum;
        numeric_range(tag, &minimum, &maximum);
        lv_slider_set_range(binding->visual_object, scale_numeric(minimum), scale_numeric(maximum));
        lv_obj_set_size(binding->visual_object, 76, 16);
        lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(binding->visual_object, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_KNOB);
        lv_obj_set_style_pad_all(binding->visual_object, 5, LV_PART_KNOB);
        lv_obj_align(binding->visual_object, LV_ALIGN_BOTTOM_RIGHT, 0, -3);
        lv_obj_add_event_cb(binding->visual_object, numeric_slider_event, LV_EVENT_RELEASED, binding);
    } else {
        binding->type          = EQUIPMENT_BINDING_VALUE;
        binding->visual_object = NULL;
    }
}

static equipment_tag_binding_t* allocate_tag_binding(ui_equipment_view_t* view)
{
    if (view == NULL || view->tag_bindings == NULL || view->tag_binding_count >= view->tag_binding_capacity) {
        return NULL;
    }
    return &view->tag_bindings[view->tag_binding_count++];
}

static equipment_state_binding_t* allocate_state_binding(ui_equipment_view_t* view)
{
    if (view == NULL || view->state_bindings == NULL || view->state_binding_count >= view->state_binding_capacity) {
        return NULL;
    }
    return &view->state_bindings[view->state_binding_count++];
}

void ui_equipment_update(ui_equipment_view_t* view)
{
    if (view == NULL) {
        return;
    }
    for (size_t index = 0; index < view->state_binding_count; ++index) {
        equipment_state_binding_t* state = &view->state_bindings[index];
        data_model_tag_t status_tag;
        bool running = state->status_tag_index != DATA_MODEL_INVALID_INDEX &&
                       data_model_get_tag(view->data_model, state->status_tag_index, &status_tag) &&
                       status_tag.value_valid && status_tag.value.boolean_value;
        data_model_tag_t alarm_tag;
        bool alarm = state->alarm_tag_index != DATA_MODEL_INVALID_INDEX &&
                     data_model_get_tag(view->data_model, state->alarm_tag_index, &alarm_tag) &&
                     alarm_tag.value_valid && alarm_tag.value.boolean_value;
        if (! state->displayed_valid || state->displayed_running != running || state->displayed_alarm != alarm) {
            const char* text =
                alarm ? "ALARM"
                      : (running ? "RUNNING"
                                 : (state->status_tag_index != DATA_MODEL_INVALID_INDEX ? "READY" : "AVAILABLE"));
            lv_color_t color = alarm ? lv_color_hex(UI_COLOR_DANGER)
                                     : (running ? lv_color_hex(UI_COLOR_SUCCESS) : lv_color_hex(UI_COLOR_INACTIVE));
            ui_theme_set_label_text(state->state_label, text);
            lv_obj_set_style_text_color(state->state_label, color, LV_PART_MAIN);
            lv_obj_set_style_border_color(state->state_badge, color, LV_PART_MAIN);
            lv_obj_set_style_border_opa(state->state_badge, running || alarm ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN);
            lv_obj_set_style_border_color(state->icon_badge, color, LV_PART_MAIN);
            state->displayed_running = running;
            state->displayed_alarm   = alarm;
            state->displayed_valid   = true;
        }
    }
    for (size_t index = 0; index < view->tag_binding_count; ++index) {
        data_model_tag_t tag;
        if (data_model_get_tag(view->data_model, view->tag_bindings[index].tag_index, &tag)) {
            update_tag_binding(&view->tag_bindings[index], &tag);
        }
    }
}

static void update_tag_binding(equipment_tag_binding_t* binding, const data_model_tag_t* tag)
{
    binding->updating_from_model = true;
    if (! tag->value_valid) {
        ui_theme_set_label_text(binding->value_label, "No value");
        lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
        binding->displayed_boolean_valid = false;
        binding->updating_from_model     = false;
        return;
    }

    if (binding->type == EQUIPMENT_BINDING_BOOLEAN_SWITCH) {
        bool value = tag->value.boolean_value;
        if (strcmp(tag->semantic_role, "operating_command") == 0) {
            data_model_tag_t status;
            if (ui_theme_find_tag_by_role(binding->view->data_model, tag->equipment_index, "operating_status",
                                          &status) &&
                status.value_valid) {
                value = status.value.boolean_value;
            }
        }
        if (binding->boolean_write_pending) {
            if (value == binding->pending_boolean_value) {
                binding->boolean_write_pending = false;
            } else if (lv_tick_elaps(binding->pending_write_started) < EQUIPMENT_WRITE_HOLD_MS) {
                binding->updating_from_model = false;
                return;
            } else {
                binding->boolean_write_pending = false;
            }
        }
        if (value) {
            lv_obj_add_state(binding->visual_object, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(binding->visual_object, LV_STATE_CHECKED);
        }
        ui_theme_set_label_text(binding->value_label, value ? "ON" : "OFF");
        lv_obj_set_style_text_color(binding->value_label, ui_theme_boolean_color(tag, value), LV_PART_MAIN);
        binding->displayed_boolean_valid = true;
        binding->displayed_boolean_value = value;
    } else {
        char value[64];
        ui_theme_format_tag_value(value, sizeof(value), tag);
        ui_theme_set_label_text(binding->value_label, value);
        lv_color_t value_color = tag->data_type == DATA_MODEL_TYPE_BOOLEAN
                                     ? ui_theme_boolean_color(tag, tag->value.boolean_value)
                                     : lv_color_hex(UI_COLOR_TEXT_PRIMARY);
        lv_obj_set_style_text_color(binding->value_label, value_color, LV_PART_MAIN);
        if (binding->type == EQUIPMENT_BINDING_NUMERIC_SLIDER && binding->visual_object != NULL) {
            double numeric =
                tag->data_type == DATA_MODEL_TYPE_INTEGER ? (double)tag->value.integer_value : tag->value.numeric_value;
            lv_slider_set_value(binding->visual_object, scale_numeric(numeric), LV_ANIM_OFF);
        }
    }
    binding->updating_from_model = false;
}

static void boolean_switch_event(lv_event_t* event)
{
    equipment_tag_binding_t* binding = lv_event_get_user_data(event);
    if (binding == NULL || binding->updating_from_model) {
        return;
    }
    bool value = lv_obj_has_state(binding->visual_object, LV_STATE_CHECKED);
    data_model_tag_t command;
    bool has_command = data_model_get_tag(binding->view->data_model, binding->tag_index, &command);
    if (has_command && strcmp(command.semantic_role, "operating_command") == 0) {
        data_model_tag_t automatic_mode;
        if (ui_theme_find_tag_by_role(binding->view->data_model, command.equipment_index, "automatic_mode",
                                      &automatic_mode) &&
            automatic_mode.value_valid && automatic_mode.value.boolean_value) {
            esp_err_t mode_result =
                opcua_client_write_boolean(binding->view->opcua_client, automatic_mode.index, false);
            if (mode_result != ESP_OK) {
                ESP_LOGE(TAG, "Cannot switch equipment to manual mode: %s", esp_err_to_name(mode_result));
                update_tag_binding(binding, &command);
                return;
            }
        }
    }
    esp_err_t result = opcua_client_write_boolean(binding->view->opcua_client, binding->tag_index, value);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot queue Boolean write: %s", esp_err_to_name(result));
        if (has_command)
            update_tag_binding(binding, &command);
        return;
    }
    binding->boolean_write_pending = true;
    binding->pending_boolean_value = value;
    binding->pending_write_started = lv_tick_get();
    ui_theme_set_label_text(binding->value_label, value ? "ON" : "OFF");
    lv_obj_set_style_text_color(binding->value_label, ui_theme_boolean_color(has_command ? &command : NULL, value),
                                LV_PART_MAIN);
}

static void numeric_slider_event(lv_event_t* event)
{
    equipment_tag_binding_t* binding = lv_event_get_user_data(event);
    if (binding == NULL || binding->updating_from_model) {
        return;
    }
    double value     = lv_slider_get_value(binding->visual_object) / EQUIPMENT_NUMERIC_SCALE;
    esp_err_t result = opcua_client_write_number(binding->view->opcua_client, binding->tag_index, value);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot queue numeric write: %s", esp_err_to_name(result));
    }
}

static int32_t scale_numeric(double value)
{
    double scaled = value * EQUIPMENT_NUMERIC_SCALE;
    if (! isfinite(scaled))
        return scaled < 0.0 ? INT32_MIN : INT32_MAX;
    if (scaled <= (double)INT32_MIN)
        return INT32_MIN;
    if (scaled >= (double)INT32_MAX)
        return INT32_MAX;
    return (int32_t)lround(scaled);
}

static void numeric_range(const data_model_tag_t* tag, double* minimum, double* maximum)
{
    *minimum = tag->has_minimum ? tag->minimum : 0.0;
    *maximum = tag->has_maximum ? tag->maximum : 100.0;
    if (*maximum <= *minimum)
        *maximum = *minimum + 100.0;
}

static bool is_status_role(const data_model_tag_t* tag)
{
    return tag != NULL &&
           (strcmp(tag->semantic_role, "operating_status") == 0 || strcmp(tag->semantic_role, "alarm_status") == 0);
}

/** Build a short, equipment-agnostic identifier such as C1 or AR from the discovered name. */
static void create_equipment_monogram(char* destination, size_t destination_size, const char* human_name)
{
    if (destination == NULL || destination_size < 2) {
        return;
    }
    destination[0] = '#';
    destination[1] = '\0';
    if (human_name == NULL || human_name[0] == '\0') {
        return;
    }

    size_t output_index = 0;
    for (size_t index = 0; human_name[index] != '\0'; ++index) {
        char current = human_name[index];
        if ((current >= 'A' && current <= 'Z') || (current >= 'a' && current <= 'z')) {
            destination[output_index++] = current >= 'a' && current <= 'z' ? (char)(current - ('a' - 'A')) : current;
            break;
        }
    }

    bool digit_found = false;
    for (size_t index = 0; human_name[index] != '\0' && output_index + 1 < destination_size; ++index) {
        if (human_name[index] >= '0' && human_name[index] <= '9') {
            destination[output_index++] = human_name[index];
            digit_found                 = true;
            if (output_index >= 3) {
                break;
            }
        } else if (digit_found) {
            break;
        }
    }

    if (! digit_found && output_index + 1 < destination_size) {
        for (size_t index = 1; human_name[index] != '\0'; ++index) {
            char current     = human_name[index];
            bool starts_word = human_name[index - 1] == ' ' &&
                               ((current >= 'A' && current <= 'Z') || (current >= 'a' && current <= 'z'));
            if (starts_word) {
                destination[output_index++] =
                    current >= 'a' && current <= 'z' ? (char)(current - ('a' - 'A')) : current;
                break;
            }
        }
    }
    if (output_index == 0) {
        destination[output_index++] = '#';
    }
    destination[output_index] = '\0';
}

void ui_equipment_destroy(ui_equipment_view_t* view)
{
    if (view == NULL) {
        return;
    }
    free(view->tag_bindings);
    free(view->state_bindings);
    free(view);
}
