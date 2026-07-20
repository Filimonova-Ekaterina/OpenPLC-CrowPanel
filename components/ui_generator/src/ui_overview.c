#include "ui_overview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_theme.h"

#define OVERVIEW_PRIORITY_SIGNAL_COUNT 3
#define OVERVIEW_NODE_RING_SIZE        52
#define OVERVIEW_NODE_WRAPPER_WIDTH    976
#define OVERVIEW_NODE_EDGE_PADDING     38
#define OVERVIEW_NODES_PER_ROW         7
#define OVERVIEW_NODE_ROW_HEIGHT       104

typedef struct
{
    size_t equipment_index;
    data_model_entity_kind_t entity_kind;
    size_t status_tag_index;
    size_t alarm_tag_index;
    lv_obj_t* ring;
    lv_obj_t* core;
    lv_obj_t* state_label;
    bool displayed_valid;
    bool displayed_running;
    bool displayed_alarm;
} overview_equipment_binding_t;

typedef struct
{
    size_t tag_index;
    lv_obj_t* value_label;
} overview_signal_binding_t;

typedef struct
{
    const char* roles[3];
} overview_signal_slot_t;

static const overview_signal_slot_t OVERVIEW_SIGNAL_SLOTS[OVERVIEW_PRIORITY_SIGNAL_COUNT] = {
    {.roles = {"pressure", "temperature", NULL}},
    {.roles = {"demand", "flow", NULL}},
    {.roles = {"power", "motor_current", "runtime"}},
};

struct ui_overview
{
    data_model_t* data_model;
    overview_equipment_binding_t* equipment_bindings;
    size_t equipment_binding_capacity;
    size_t equipment_binding_count;
    overview_signal_binding_t signal_bindings[OVERVIEW_PRIORITY_SIGNAL_COUNT];
    size_t signal_binding_count;
    lv_obj_t* state_value;
    lv_obj_t* state_detail;
    lv_obj_t* state_indicator;
    lv_obj_t* running_value;
    lv_obj_t* alarm_value;
};

static lv_obj_t* create_summary_card(lv_obj_t* parent, const char* caption, lv_color_t accent);
static void create_hierarchy_cards(ui_overview_t* overview, lv_obj_t* page);
static bool create_hierarchy_card(ui_overview_t* overview, lv_obj_t* page, const data_model_equipment_t* root,
                                  bool flat_fallback);
static void create_equipment_node(ui_overview_t* overview, lv_obj_t* wrapper, const data_model_equipment_t* equipment,
                                  lv_coord_t x, lv_coord_t y);
static void create_priority_signals(ui_overview_t* overview, lv_obj_t* page);
static bool tag_is_numeric_measurement(const data_model_tag_t* tag);
static bool select_signal_for_slot(const data_model_t* model, const overview_signal_slot_t* slot,
                                   size_t* tag_index_out);
static bool find_shared_signal_scope(const data_model_t* model, const size_t* tag_indices, size_t tag_count,
                                     data_model_equipment_t* scope_out);
static void animate_flow_dash(lv_obj_t* dash, lv_coord_t start_x, lv_coord_t end_x, uint32_t delay);
static void set_flow_dash_x(void* object, int32_t x);

ui_overview_t* ui_overview_create(lv_obj_t* page, data_model_t* data_model)
{
    if (page == NULL || data_model == NULL) {
        return NULL;
    }
    ui_overview_t* overview = calloc(1, sizeof(*overview));
    if (overview == NULL) {
        return NULL;
    }
    overview->data_model                 = data_model;
    overview->equipment_binding_capacity = data_model_object_count(data_model);
    if (overview->equipment_binding_capacity > 0) {
        overview->equipment_bindings =
            calloc(overview->equipment_binding_capacity, sizeof(*overview->equipment_bindings));
        if (overview->equipment_bindings == NULL) {
            free(overview);
            return NULL;
        }
    }

    lv_obj_t* command_row = lv_obj_create(page);
    lv_obj_remove_style_all(command_row);
    lv_obj_set_size(command_row, lv_pct(100), 110);
    lv_obj_set_flex_flow(command_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(command_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(command_row, 14, LV_PART_MAIN);
    lv_obj_clear_flag(command_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* state_card = lv_obj_create(command_row);
    ui_theme_style_card(state_card, lv_color_hex(UI_COLOR_SURFACE_RAISED), 20);
    lv_obj_set_height(state_card, lv_pct(100));
    lv_obj_set_flex_grow(state_card, 1);
    lv_obj_set_style_pad_all(state_card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_left(state_card, 22, LV_PART_MAIN);

    overview->state_value = lv_label_create(state_card);
    lv_label_set_text(overview->state_value, "Discovering equipment");
    lv_label_set_long_mode(overview->state_value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(overview->state_value, lv_pct(100));
    lv_obj_set_style_text_color(overview->state_value, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(overview->state_value, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_align(overview->state_value, LV_ALIGN_TOP_LEFT, 0, 3);

    overview->state_detail = lv_label_create(state_card);
    lv_label_set_text(overview->state_detail, "Waiting for live OPC UA status");
    lv_label_set_long_mode(overview->state_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(overview->state_detail, lv_pct(100));
    lv_obj_set_style_text_color(overview->state_detail, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(overview->state_detail, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(overview->state_detail, LV_ALIGN_BOTTOM_LEFT, 0, -1);

    overview->running_value = create_summary_card(command_row, "RUNNING", lv_color_hex(UI_COLOR_SUCCESS));
    overview->alarm_value   = create_summary_card(command_row, "ALARMS", lv_color_hex(UI_COLOR_DANGER));

    create_hierarchy_cards(overview, page);
    create_priority_signals(overview, page);
    ui_overview_update(overview);
    return overview;
}

static lv_obj_t* create_summary_card(lv_obj_t* parent, const char* caption, lv_color_t accent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, 176, lv_pct(100));
    ui_theme_style_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);

    lv_obj_t* dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 13, 13);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 0, 2);

    lv_obj_t* caption_label = lv_label_create(card);
    lv_label_set_text(caption_label, caption);
    lv_obj_set_style_text_color(caption_label, lv_color_hex(UI_COLOR_TEXT_LABEL), LV_PART_MAIN);
    lv_obj_set_style_text_font(caption_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(caption_label, LV_ALIGN_TOP_LEFT, 24, 0);

    lv_obj_t* value_label = lv_label_create(card);
    lv_label_set_text(value_label, "0");
    lv_obj_set_style_text_color(value_label, accent, LV_PART_MAIN);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_38, LV_PART_MAIN);
    lv_obj_align(value_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return value_label;
}

static void create_hierarchy_cards(ui_overview_t* overview, lv_obj_t* page)
{
    size_t equipment_count = data_model_object_count(overview->data_model);
    if (equipment_count == 0) {
        ui_theme_create_heading(page, "Equipment status", "Classified from OPC UA semantics");
        ui_theme_create_empty_state(page, "Waiting for equipment",
                                    "The hierarchy appears automatically after discovery",
                                    lv_color_hex(UI_COLOR_ACCENT));
        return;
    }

    size_t card_count = 0;
    for (size_t index = 0; index < equipment_count; ++index) {
        data_model_equipment_t equipment;
        if (! data_model_get_equipment(overview->data_model, index, &equipment)) {
            continue;
        }
        if (equipment.parent_index == DATA_MODEL_INVALID_INDEX || equipment.parent_index >= equipment_count) {
            if (create_hierarchy_card(overview, page, &equipment, false)) {
                card_count++;
            }
        }
    }
    if (card_count == 0) {
        data_model_equipment_t first;
        if (data_model_get_equipment(overview->data_model, 0, &first)) {
            if (create_hierarchy_card(overview, page, &first, true)) {
                card_count++;
            }
        }
    }
    if (card_count == 0) {
        ui_theme_create_heading(page, "Equipment status", "Classified from OPC UA semantics");
        ui_theme_create_empty_state(page, "No physical assets discovered",
                                    "System and process objects remain available as signal sources",
                                    lv_color_hex(UI_COLOR_ACCENT));
    }
}

static bool equipment_belongs_to_scope(const data_model_t* model, const data_model_equipment_t* equipment,
                                       const data_model_equipment_t* root, bool flat_fallback)
{
    if (! data_model_entity_is_equipment(equipment->entity_kind)) {
        return false;
    }
    if (flat_fallback || equipment->index == root->index) {
        return true;
    }

    size_t parent_index  = equipment->parent_index;
    size_t object_count  = data_model_object_count(model);
    for (size_t guard = 0; guard < object_count && parent_index != DATA_MODEL_INVALID_INDEX; ++guard) {
        if (parent_index == root->index) {
            return true;
        }
        data_model_equipment_t parent;
        if (! data_model_get_equipment(model, parent_index, &parent) || parent.parent_index == parent.index) {
            break;
        }
        parent_index = parent.parent_index;
    }
    return false;
}

static bool create_hierarchy_card(ui_overview_t* overview, lv_obj_t* page, const data_model_equipment_t* root,
                                  bool flat_fallback)
{
    size_t equipment_count = data_model_object_count(overview->data_model);
    size_t active_count    = 0;
    size_t passive_count   = 0;
    for (size_t index = 0; index < equipment_count; ++index) {
        data_model_equipment_t child;
        if (! data_model_get_equipment(overview->data_model, index, &child) ||
            ! equipment_belongs_to_scope(overview->data_model, &child, root, flat_fallback)) {
            continue;
        }
        if (child.entity_kind == DATA_MODEL_ENTITY_ACTIVE_EQUIPMENT)
            active_count++;
        else
            passive_count++;
    }
    size_t child_count = active_count + passive_count;
    if (child_count == 0) {
        return false;
    }
    size_t node_rows             = (child_count + OVERVIEW_NODES_PER_ROW - 1) / OVERVIEW_NODES_PER_ROW;
    lv_coord_t hierarchy_height  = (lv_coord_t)(106 + node_rows * OVERVIEW_NODE_ROW_HEIGHT);
    lv_coord_t hierarchy_content = (lv_coord_t)(node_rows * OVERVIEW_NODE_ROW_HEIGHT);

    lv_obj_t* card = lv_obj_create(page);
    lv_obj_set_size(card, lv_pct(100), hierarchy_height);
    ui_theme_style_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);

    lv_obj_t* accent = lv_obj_create(card);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 5, 27);
    lv_obj_set_style_radius(accent, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(accent, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(accent, LV_ALIGN_TOP_LEFT, 2, 0);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "Equipment status");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 0);

    lv_obj_t* detail = lv_label_create(card);
    char equipment_summary[64];
    snprintf(equipment_summary, sizeof(equipment_summary), "%u active  |  %u passive", (unsigned)active_count,
             (unsigned)passive_count);
    lv_label_set_text(detail, equipment_summary);
    lv_obj_set_style_text_color(detail, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(detail, LV_ALIGN_TOP_RIGHT, 0, 3);

    const char* root_name = root->display_name[0] != '\0' ? root->display_name : root->browse_name;
    char human_root_name[UI_HUMAN_NAME_LENGTH];
    ui_theme_humanize_name(human_root_name, sizeof(human_root_name), root_name);
    lv_obj_t* root_pill = lv_obj_create(card);
    lv_obj_set_size(root_pill, 330, 38);
    ui_theme_style_card(root_pill, lv_color_hex(UI_COLOR_SURFACE_RAISED), 19);
    lv_obj_set_style_border_color(root_pill, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_align(root_pill, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_t* root_label = lv_label_create(root_pill);
    lv_label_set_text(root_label, human_root_name);
    lv_label_set_long_mode(root_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(root_label, 285);
    lv_obj_set_style_text_align(root_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(root_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(root_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_center(root_label);

    lv_obj_t* viewport = lv_obj_create(card);
    lv_obj_remove_style_all(viewport);
    lv_obj_set_size(viewport, lv_pct(100), hierarchy_content);
    lv_obj_align(viewport, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t wrapper_width = OVERVIEW_NODE_WRAPPER_WIDTH;
    lv_obj_t* wrapper        = lv_obj_create(viewport);
    lv_obj_remove_style_all(wrapper);
    lv_obj_set_size(wrapper, wrapper_width, hierarchy_content);
    lv_obj_clear_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t row = 0; row < node_rows; ++row) {
        size_t remaining = child_count - row * OVERVIEW_NODES_PER_ROW;
        size_t columns   = remaining < OVERVIEW_NODES_PER_ROW ? remaining : OVERVIEW_NODES_PER_ROW;
        lv_coord_t connector_x =
            columns > 1 ? OVERVIEW_NODE_EDGE_PADDING + OVERVIEW_NODE_RING_SIZE / 2 : OVERVIEW_NODE_WRAPPER_WIDTH / 2;
        lv_coord_t connector_width =
            columns > 1 ? OVERVIEW_NODE_WRAPPER_WIDTH - OVERVIEW_NODE_RING_SIZE - 2 * OVERVIEW_NODE_EDGE_PADDING : 3;
        lv_coord_t connector_y = (lv_coord_t)(row * OVERVIEW_NODE_ROW_HEIGHT + 57);
        lv_obj_t* connector    = lv_obj_create(wrapper);
        lv_obj_remove_style_all(connector);
        lv_obj_set_size(connector, connector_width, 3);
        lv_obj_set_style_radius(connector, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(connector, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(connector, LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_pos(connector, connector_x, connector_y);

        if (columns > 1) {
            for (unsigned dash_index = 0; dash_index < 2; ++dash_index) {
                lv_obj_t* dash = lv_obj_create(wrapper);
                lv_obj_remove_style_all(dash);
                lv_obj_set_size(dash, 18, 5);
                lv_obj_set_style_radius(dash, LV_RADIUS_CIRCLE, LV_PART_MAIN);
                lv_obj_set_style_bg_color(dash, lv_color_hex(UI_COLOR_ACCENT_SOFT), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(dash, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_y(dash, connector_y - 1);
                animate_flow_dash(dash, connector_x, connector_x + connector_width - 18,
                                  (uint32_t)(row * 400U + dash_index * 1100U));
            }
        }
    }

    size_t visible_index = 0;
    const data_model_entity_kind_t display_order[] = {DATA_MODEL_ENTITY_ACTIVE_EQUIPMENT,
                                                       DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT};
    for (size_t kind_index = 0; kind_index < sizeof(display_order) / sizeof(display_order[0]); ++kind_index) {
        for (size_t index = 0; index < equipment_count; ++index) {
            data_model_equipment_t child;
            if (! data_model_get_equipment(overview->data_model, index, &child) ||
                child.entity_kind != display_order[kind_index] ||
                ! equipment_belongs_to_scope(overview->data_model, &child, root, flat_fallback)) {
                continue;
            }
            size_t row       = visible_index / OVERVIEW_NODES_PER_ROW;
            size_t column    = visible_index % OVERVIEW_NODES_PER_ROW;
            size_t remaining = child_count - row * OVERVIEW_NODES_PER_ROW;
            size_t columns   = remaining < OVERVIEW_NODES_PER_ROW ? remaining : OVERVIEW_NODES_PER_ROW;
            lv_coord_t x =
                columns > 1
                    ? (lv_coord_t)(OVERVIEW_NODE_EDGE_PADDING +
                                   column * (OVERVIEW_NODE_WRAPPER_WIDTH - OVERVIEW_NODE_RING_SIZE -
                                             2 * OVERVIEW_NODE_EDGE_PADDING) /
                                       (columns - 1))
                    : (OVERVIEW_NODE_WRAPPER_WIDTH - OVERVIEW_NODE_RING_SIZE) / 2;
            create_equipment_node(overview, wrapper, &child, x, (lv_coord_t)(row * OVERVIEW_NODE_ROW_HEIGHT));
            visible_index++;
        }
    }
    return true;
}

static void animate_flow_dash(lv_obj_t* dash, lv_coord_t start_x, lv_coord_t end_x, uint32_t delay)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, dash);
    lv_anim_set_values(&animation, start_x, end_x);
    lv_anim_set_time(&animation, 3200);
    lv_anim_set_delay(&animation, delay);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&animation, set_flow_dash_x);
    lv_anim_start(&animation);
}

static void set_flow_dash_x(void* object, int32_t x)
{
    lv_obj_set_x((lv_obj_t*)object, (lv_coord_t)x);
}

static void create_equipment_node(ui_overview_t* overview, lv_obj_t* wrapper, const data_model_equipment_t* equipment,
                                  lv_coord_t x, lv_coord_t y)
{
    if (overview->equipment_binding_count >= overview->equipment_binding_capacity) {
        return;
    }
    overview_equipment_binding_t* binding = &overview->equipment_bindings[overview->equipment_binding_count++];
    binding->equipment_index              = equipment->index;
    binding->entity_kind                  = equipment->entity_kind;
    binding->status_tag_index             = DATA_MODEL_INVALID_INDEX;
    binding->alarm_tag_index              = DATA_MODEL_INVALID_INDEX;

    data_model_tag_t tag;
    if (ui_theme_find_tag_by_role(overview->data_model, equipment->index, "operating_status", &tag)) {
        binding->status_tag_index = tag.index;
    }
    if (ui_theme_find_tag_by_role(overview->data_model, equipment->index, "alarm_status", &tag)) {
        binding->alarm_tag_index = tag.index;
    }

    const char* name = equipment->display_name[0] != '\0' ? equipment->display_name : equipment->browse_name;
    char human_name[UI_HUMAN_NAME_LENGTH];
    ui_theme_humanize_name(human_name, sizeof(human_name), name);
    lv_obj_t* name_label = lv_label_create(wrapper);
    lv_label_set_text(name_label, human_name);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, 128);
    lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_label, lv_color_hex(UI_COLOR_TEXT_LABEL), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_coord_t name_x = x + OVERVIEW_NODE_RING_SIZE / 2 - 64;
    if (name_x < 0)
        name_x = 0;
    if (name_x > OVERVIEW_NODE_WRAPPER_WIDTH - 128)
        name_x = OVERVIEW_NODE_WRAPPER_WIDTH - 128;
    lv_obj_set_pos(name_label, name_x, y);

    binding->ring = lv_obj_create(wrapper);
    lv_obj_set_size(binding->ring, OVERVIEW_NODE_RING_SIZE, OVERVIEW_NODE_RING_SIZE);
    lv_obj_set_pos(binding->ring, x, y + 31);
    lv_obj_clear_flag(binding->ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(binding->ring, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(binding->ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(binding->ring,
                                  lv_color_hex(equipment->entity_kind == DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT
                                                   ? UI_COLOR_ACCENT_SOFT
                                                   : UI_COLOR_INACTIVE),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(binding->ring, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(binding->ring,
                            equipment->entity_kind == DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT ? 14 : LV_RADIUS_CIRCLE,
                            LV_PART_MAIN);
    lv_obj_set_style_pad_all(binding->ring, 0, LV_PART_MAIN);

    binding->core = lv_obj_create(binding->ring);
    lv_obj_remove_style_all(binding->core);
    bool passive = equipment->entity_kind == DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT;
    lv_obj_set_size(binding->core, passive ? 22 : 19, passive ? 5 : 19);
    lv_obj_set_style_radius(binding->core, passive ? 3 : LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(binding->core, lv_color_hex(passive ? UI_COLOR_ACCENT_SOFT : UI_COLOR_INACTIVE),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(binding->core, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_center(binding->core);

    binding->state_label = lv_label_create(wrapper);
    lv_label_set_text(binding->state_label, passive ? "MONITORED" : "READY");
    lv_obj_set_width(binding->state_label, 104);
    lv_obj_set_style_text_align(binding->state_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(binding->state_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_coord_t state_x = x + OVERVIEW_NODE_RING_SIZE / 2 - 52;
    if (state_x < 0)
        state_x = 0;
    if (state_x > OVERVIEW_NODE_WRAPPER_WIDTH - 104)
        state_x = OVERVIEW_NODE_WRAPPER_WIDTH - 104;
    lv_obj_set_pos(binding->state_label, state_x, y + 84);
}

static bool tag_is_numeric_measurement(const data_model_tag_t* tag)
{
    return tag != NULL && tag->readable && ! tag->writable &&
           (tag->data_type == DATA_MODEL_TYPE_INTEGER || tag->data_type == DATA_MODEL_TYPE_FLOAT ||
            tag->data_type == DATA_MODEL_TYPE_DOUBLE);
}

static bool role_belongs_to_overview_slots(const char* role)
{
    for (size_t slot_index = 0; slot_index < OVERVIEW_PRIORITY_SIGNAL_COUNT; ++slot_index) {
        for (size_t role_index = 0; role_index < 3; ++role_index) {
            const char* slot_role = OVERVIEW_SIGNAL_SLOTS[slot_index].roles[role_index];
            if (slot_role != NULL && strcmp(role, slot_role) == 0) {
                return true;
            }
        }
    }
    return false;
}

static size_t count_equipment_overview_signals(const data_model_t* model, size_t equipment_index)
{
    size_t signal_count = 0;
    size_t tag_count    = data_model_tag_count(model);
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (data_model_get_tag(model, tag_index, &tag) && tag.equipment_index == equipment_index &&
            tag_is_numeric_measurement(&tag) && role_belongs_to_overview_slots(tag.semantic_role)) {
            ++signal_count;
        }
    }
    return signal_count;
}

static size_t equipment_hierarchy_depth(const data_model_t* model, size_t equipment_index)
{
    size_t depth           = 0;
    size_t equipment_count = data_model_object_count(model);
    for (size_t guard = 0; guard < equipment_count; ++guard) {
        data_model_equipment_t equipment = {0};
        if (! data_model_get_equipment(model, equipment_index, &equipment) ||
            equipment.parent_index == DATA_MODEL_INVALID_INDEX || equipment.parent_index == equipment.index) {
            break;
        }
        ++depth;
        equipment_index = equipment.parent_index;
    }
    return depth;
}

/** Select one representative KPI for a semantic slot without relying on node names. */
static bool select_signal_for_slot(const data_model_t* model, const overview_signal_slot_t* slot,
                                   size_t* tag_index_out)
{
    size_t tag_count = data_model_tag_count(model);
    for (size_t role_index = 0; role_index < 3 && slot->roles[role_index] != NULL; ++role_index) {
        bool found           = false;
        size_t best_index    = DATA_MODEL_INVALID_INDEX;
        unsigned best_scope  = 0;
        size_t best_coverage = 0;
        size_t best_depth    = SIZE_MAX;

        for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
            data_model_tag_t tag;
            if (! data_model_get_tag(model, tag_index, &tag) || ! tag_is_numeric_measurement(&tag) ||
                strcmp(tag.semantic_role, slot->roles[role_index]) != 0) {
                continue;
            }
            data_model_equipment_t equipment;
            unsigned scope = 0;
            if (data_model_get_equipment(model, tag.equipment_index, &equipment)) {
                if (equipment.entity_kind == DATA_MODEL_ENTITY_SYSTEM ||
                    equipment.entity_kind == DATA_MODEL_ENTITY_PROCESS) {
                    scope = 2;
                } else if (data_model_entity_is_equipment(equipment.entity_kind)) {
                    scope = 1;
                }
            }
            size_t coverage = count_equipment_overview_signals(model, tag.equipment_index);
            size_t depth    = equipment_hierarchy_depth(model, tag.equipment_index);
            if (! found || scope > best_scope ||
                (scope == best_scope &&
                 (depth < best_depth || (depth == best_depth && coverage > best_coverage)))) {
                found         = true;
                best_index    = tag.index;
                best_scope    = scope;
                best_coverage = coverage;
                best_depth    = depth;
            }
        }
        if (found) {
            *tag_index_out = best_index;
            return true;
        }
    }
    return false;
}

/** Find the deepest OPC UA object that contains every selected signal source. */
static bool find_shared_signal_scope(const data_model_t* model, const size_t* tag_indices, size_t tag_count,
                                     data_model_equipment_t* scope_out)
{
    if (model == NULL || tag_indices == NULL || tag_count == 0 || scope_out == NULL) {
        return false;
    }

    data_model_tag_t first_tag;
    if (! data_model_get_tag(model, tag_indices[0], &first_tag)) {
        return false;
    }

    size_t candidate_index = first_tag.equipment_index;
    size_t equipment_count = data_model_object_count(model);
    for (size_t candidate_guard = 0; candidate_guard < equipment_count; ++candidate_guard) {
        data_model_equipment_t candidate;
        if (! data_model_get_equipment(model, candidate_index, &candidate)) {
            return false;
        }

        bool contains_all = true;
        for (size_t tag_position = 1; tag_position < tag_count && contains_all; ++tag_position) {
            data_model_tag_t tag;
            if (! data_model_get_tag(model, tag_indices[tag_position], &tag)) {
                contains_all = false;
                break;
            }

            size_t source_index = tag.equipment_index;
            bool source_in_scope = false;
            for (size_t source_guard = 0; source_guard < equipment_count; ++source_guard) {
                data_model_equipment_t source;
                if (! data_model_get_equipment(model, source_index, &source)) {
                    break;
                }
                if (source.index == candidate.index) {
                    source_in_scope = true;
                    break;
                }
                if (source.parent_index == DATA_MODEL_INVALID_INDEX || source.parent_index == source.index) {
                    break;
                }
                source_index = source.parent_index;
            }
            contains_all = source_in_scope;
        }

        if (contains_all) {
            *scope_out = candidate;
            return true;
        }
        if (candidate.parent_index == DATA_MODEL_INVALID_INDEX || candidate.parent_index == candidate.index) {
            break;
        }
        candidate_index = candidate.parent_index;
    }
    return false;
}

static void create_priority_signals(ui_overview_t* overview, lv_obj_t* page)
{
    size_t selected_indices[OVERVIEW_PRIORITY_SIGNAL_COUNT];
    size_t selected_count = 0;

    for (size_t slot_index = 0; slot_index < OVERVIEW_PRIORITY_SIGNAL_COUNT; ++slot_index) {
        size_t tag_index;
        if (select_signal_for_slot(overview->data_model, &OVERVIEW_SIGNAL_SLOTS[slot_index], &tag_index)) {
            selected_indices[selected_count] = tag_index;
            ++selected_count;
        }
    }

    if (selected_count == 0) {
        return;
    }
    lv_obj_t* card = lv_obj_create(page);
    lv_obj_set_size(card, lv_pct(100), 172);
    ui_theme_style_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);

    lv_obj_t* accent = lv_obj_create(card);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 5, 27);
    lv_obj_set_style_radius(accent, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(accent, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(accent, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "Live signals");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 0);
    lv_obj_t* detail = lv_label_create(card);
    lv_label_set_text(detail, "Most relevant values from OPC UA");
    lv_obj_set_style_text_color(detail, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(detail, LV_ALIGN_TOP_RIGHT, 0, 4);

    data_model_equipment_t signal_scope;
    if (find_shared_signal_scope(overview->data_model, selected_indices, selected_count, &signal_scope)) {
        const char* scope_name = signal_scope.display_name[0] != '\0' ? signal_scope.display_name : signal_scope.browse_name;
        char human_scope_name[UI_HUMAN_NAME_LENGTH];
        ui_theme_humanize_name(human_scope_name, sizeof(human_scope_name), scope_name);

        lv_obj_t* scope_pill = lv_obj_create(card);
        lv_obj_set_size(scope_pill, 330, 38);
        ui_theme_style_card(scope_pill, lv_color_hex(UI_COLOR_SURFACE_RAISED), 19);
        lv_obj_set_style_border_color(scope_pill, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_align(scope_pill, LV_ALIGN_TOP_MID, 0, 30);

        lv_obj_t* scope_label = lv_label_create(scope_pill);
        lv_label_set_text(scope_label, human_scope_name);
        lv_label_set_long_mode(scope_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(scope_label, 285);
        lv_obj_set_style_text_align(scope_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(scope_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(scope_label, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_center(scope_label);
    }

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 76);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t selected = 0; selected < selected_count; ++selected) {
        data_model_tag_t tag;
        if (! data_model_get_tag(overview->data_model, selected_indices[selected], &tag)) {
            continue;
        }
        overview_signal_binding_t* binding = &overview->signal_bindings[overview->signal_binding_count++];
        binding->tag_index                 = tag.index;

        lv_obj_t* cell = lv_obj_create(row);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 300, 74);
        if (selected > 0) {
            lv_obj_set_style_border_color(cell, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_border_width(cell, 1, LV_PART_MAIN);
            lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
            lv_obj_set_style_pad_left(cell, 18, LV_PART_MAIN);
        }

        lv_obj_t* icon_badge = lv_obj_create(cell);
        lv_obj_set_size(icon_badge, 52, 52);
        ui_theme_style_card(icon_badge, lv_color_hex(UI_COLOR_SURFACE_RAISED), LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_color(icon_badge, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_pad_all(icon_badge, 0, LV_PART_MAIN);
        lv_obj_align(icon_badge, LV_ALIGN_LEFT_MID, 0, 0);
        ui_theme_create_tag_icon(icon_badge, &tag, 26);

        const char* tag_name = tag.display_name[0] != '\0' ? tag.display_name : tag.browse_name;
        char human_name[UI_HUMAN_NAME_LENGTH];
        ui_theme_humanize_name(human_name, sizeof(human_name), tag_name);

        lv_obj_t* name = lv_label_create(cell);
        lv_label_set_text(name, human_name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 220);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT_LABEL), LV_PART_MAIN);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 66, 4);

        binding->value_label = lv_label_create(cell);
        lv_obj_set_width(binding->value_label, 220);
        lv_label_set_long_mode(binding->value_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(binding->value_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(binding->value_label, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_align(binding->value_label, LV_ALIGN_TOP_LEFT, 66, 29);
    }
}

void ui_overview_update(ui_overview_t* overview)
{
    if (overview == NULL) {
        return;
    }
    size_t running_count        = 0;
    size_t semantic_alarm_count = 0;
    for (size_t index = 0; index < overview->equipment_binding_count; ++index) {
        overview_equipment_binding_t* binding = &overview->equipment_bindings[index];
        data_model_tag_t status_tag;
        bool running = binding->status_tag_index != DATA_MODEL_INVALID_INDEX &&
                       data_model_get_tag(overview->data_model, binding->status_tag_index, &status_tag) &&
                       status_tag.value_valid && status_tag.value.boolean_value;
        data_model_tag_t alarm_tag;
        bool alarm = binding->alarm_tag_index != DATA_MODEL_INVALID_INDEX &&
                     data_model_get_tag(overview->data_model, binding->alarm_tag_index, &alarm_tag) &&
                     alarm_tag.value_valid && alarm_tag.value.boolean_value;
        if (binding->entity_kind == DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT) {
            running = false;
        }
        if (! binding->displayed_valid || binding->displayed_running != running || binding->displayed_alarm != alarm) {
            bool passive = binding->entity_kind == DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT;
            lv_color_t color = alarm ? lv_color_hex(UI_COLOR_DANGER)
                                     : (passive ? lv_color_hex(UI_COLOR_ACCENT_SOFT)
                                                : (running ? lv_color_hex(UI_COLOR_SUCCESS)
                                                           : lv_color_hex(UI_COLOR_INACTIVE)));
            lv_obj_set_style_border_color(binding->ring, color, LV_PART_MAIN);
            lv_obj_set_style_border_opa(binding->ring, alarm || running || passive ? LV_OPA_COVER : LV_OPA_70,
                                        LV_PART_MAIN);
            lv_obj_set_style_bg_color(binding->core, color, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(binding->core, LV_OPA_COVER, LV_PART_MAIN);
            if (passive && ! alarm) {
                lv_obj_set_size(binding->core, 22, 5);
                lv_obj_set_style_radius(binding->core, 3, LV_PART_MAIN);
            } else {
                lv_obj_set_size(binding->core, running || alarm ? 19 : 16, running || alarm ? 19 : 16);
                lv_obj_set_style_radius(binding->core, running || alarm ? LV_RADIUS_CIRCLE : 2, LV_PART_MAIN);
            }
            lv_obj_center(binding->core);
            lv_obj_set_style_text_color(binding->state_label, color, LV_PART_MAIN);
            ui_theme_set_label_text(binding->state_label,
                                    alarm ? "ALARM" : (passive ? "MONITORED" : (running ? "RUNNING" : "READY")));
            binding->displayed_running = running;
            binding->displayed_alarm   = alarm;
            binding->displayed_valid   = true;
        }
    }

    size_t equipment_count = data_model_object_count(overview->data_model);
    for (size_t equipment_index = 0; equipment_index < equipment_count; ++equipment_index) {
        data_model_equipment_t equipment;
        if (! data_model_get_equipment(overview->data_model, equipment_index, &equipment)) {
            continue;
        }
        data_model_tag_t status;
        if (equipment.entity_kind == DATA_MODEL_ENTITY_ACTIVE_EQUIPMENT &&
            ui_theme_find_tag_by_role(overview->data_model, equipment_index, "operating_status", &status) &&
            status.value_valid && status.value.boolean_value) {
            running_count++;
        }
        data_model_tag_t alarm;
        if (ui_theme_find_tag_by_role(overview->data_model, equipment_index, "alarm_status", &alarm) &&
            alarm.value_valid && alarm.value.boolean_value) {
            semantic_alarm_count++;
        }
    }

    size_t active_alarm_count = data_model_active_alarm_count(overview->data_model);
    if (semantic_alarm_count > active_alarm_count) {
        active_alarm_count = semantic_alarm_count;
    }
    char count_text[16];
    snprintf(count_text, sizeof(count_text), "%u", (unsigned)running_count);
    ui_theme_set_label_text(overview->running_value, count_text);
    snprintf(count_text, sizeof(count_text), "%u", (unsigned)active_alarm_count);
    ui_theme_set_label_text(overview->alarm_value, count_text);

    const char* state_text;
    const char* state_detail;
    lv_color_t state_color;
    if (active_alarm_count > 0) {
        state_text   = "Attention required";
        state_detail = "An active OPC UA alarm requires operator attention";
        state_color  = lv_color_hex(UI_COLOR_DANGER);
    } else if (data_model_object_count(overview->data_model) == 0) {
        state_text   = "Discovering equipment";
        state_detail = "Waiting for the OPC UA address space";
        state_color  = lv_color_hex(UI_COLOR_ACCENT);
    } else if (running_count > 0) {
        state_text   = "System operational";
        state_detail = "All monitored equipment is responding normally";
        state_color  = lv_color_hex(UI_COLOR_SUCCESS);
    } else if (data_model_active_equipment_count(overview->data_model) > 0) {
        state_text   = "System ready";
        state_detail = "Equipment is connected and standing by";
        state_color  = lv_color_hex(UI_COLOR_INACTIVE);
    } else {
        state_text   = "System monitored";
        state_detail = "Passive equipment and process signals are available";
        state_color  = lv_color_hex(UI_COLOR_ACCENT_SOFT);
    }
    ui_theme_set_label_text(overview->state_value, state_text);
    ui_theme_set_label_text(overview->state_detail, state_detail);
    if (overview->state_indicator != NULL) {
        lv_obj_set_style_bg_color(overview->state_indicator, state_color, LV_PART_MAIN);
    }

    for (size_t index = 0; index < overview->signal_binding_count; ++index) {
        data_model_tag_t tag;
        if (data_model_get_tag(overview->data_model, overview->signal_bindings[index].tag_index, &tag)) {
            char value[64];
            ui_theme_format_tag_value(value, sizeof(value), &tag);
            ui_theme_set_label_text(overview->signal_bindings[index].value_label, value);
            lv_obj_set_style_text_color(overview->signal_bindings[index].value_label,
                                        tag.value_valid ? lv_color_hex(UI_COLOR_TEXT_PRIMARY)
                                                        : lv_color_hex(UI_COLOR_TEXT_SECONDARY),
                                        LV_PART_MAIN);
        }
    }
}

void ui_overview_destroy(ui_overview_t* overview)
{
    if (overview == NULL) {
        return;
    }
    free(overview->equipment_bindings);
    free(overview);
}
