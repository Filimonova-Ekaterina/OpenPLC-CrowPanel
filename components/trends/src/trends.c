#include "trends.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRENDS_MAX_CHARTS       16
#define TRENDS_POINT_COUNT      60
#define TRENDS_SAMPLE_PERIOD_MS 1000

#define TRENDS_COLOR_SURFACE        0x151515
#define TRENDS_COLOR_BORDER         0x303030
#define TRENDS_COLOR_TEXT_PRIMARY   0xF4F4F4
#define TRENDS_COLOR_TEXT_SECONDARY 0xA0A0A0
#define TRENDS_COLOR_ACCENT         0x2A96FF
#define TRENDS_COLOR_ACCENT_SOFT    0x6BC1FF

typedef struct
{
    size_t tag_index;
    lv_obj_t* chart;
    lv_chart_series_t* series;
    lv_obj_t* value_label;
    double samples[TRENDS_POINT_COUNT];
    bool sample_valid[TRENDS_POINT_COUNT];
} trend_binding_t;

typedef struct
{
    lv_obj_t* root;
    lv_timer_t* sample_timer;
    data_model_t* data_model;
    trend_binding_t bindings[TRENDS_MAX_CHARTS];
    size_t binding_count;
} trends_context_t;

static void sample_timer_callback(lv_timer_t* timer);
static void root_deleted_event(lv_event_t* event);
static bool tag_numeric_value(const data_model_tag_t* tag, double* value_out);
static void sample_binding(trends_context_t* context, trend_binding_t* binding);
static void update_chart_points(trend_binding_t* binding, const data_model_tag_t* tag);
static void format_current_value(char* destination, size_t destination_size,
                                 const data_model_tag_t* tag, double value);

/** Return true for data types that can be represented as a continuous trend. */
static bool tag_is_numeric(const data_model_tag_t* tag)
{
    return tag != NULL && tag->readable &&
           (tag->data_type == DATA_MODEL_TYPE_INTEGER || tag->data_type == DATA_MODEL_TYPE_FLOAT ||
            tag->data_type == DATA_MODEL_TYPE_DOUBLE);
}

/** Apply the same quiet surface treatment used by the generated HMI. */
static void style_card(lv_obj_t* card)
{
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(TRENDS_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(TRENDS_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
}

/** Add the full-width title row and keep its count aligned with other pages. */
static void create_heading(lv_obj_t* parent, size_t visible_count, size_t total_count)
{
    lv_obj_t* heading = lv_obj_create(parent);
    lv_obj_remove_style_all(heading);
    lv_obj_set_size(heading, lv_pct(100), 48);
    lv_obj_clear_flag(heading, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* accent = lv_obj_create(heading);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 5, 28);
    lv_obj_set_style_radius(accent, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(accent, lv_color_hex(TRENDS_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t* title = lv_label_create(heading);
    lv_label_set_text(title, "Live trends");
    lv_obj_set_style_text_color(title, lv_color_hex(TRENDS_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    char detail_text[48];
    if (visible_count < total_count) {
        snprintf(detail_text, sizeof(detail_text), "%u of %u signals",
                 (unsigned)visible_count, (unsigned)total_count);
    } else {
        snprintf(detail_text, sizeof(detail_text), "%u signals", (unsigned)total_count);
    }
    lv_obj_t* detail = lv_label_create(heading);
    lv_label_set_text(detail, detail_text);
    lv_obj_set_style_text_color(detail, lv_color_hex(TRENDS_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(detail, LV_ALIGN_RIGHT_MID, 0, 1);
}

/** Add a neutral empty state when the server exposes no numeric values. */
static void create_empty_state(lv_obj_t* parent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), 124);
    style_card(card);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);

    lv_obj_t* indicator = lv_obj_create(card);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(indicator, 20, 20);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(TRENDS_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(indicator, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "No numeric signals");
    lv_obj_set_style_text_color(title, lv_color_hex(TRENDS_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 40, -16);

    lv_obj_t* detail = lv_label_create(card);
    lv_label_set_text(detail, "Charts appear automatically for readable numeric OPC UA variables");
    lv_obj_set_style_text_color(detail, lv_color_hex(TRENDS_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(detail, LV_ALIGN_LEFT_MID, 40, 18);
}

/** Create one self-contained chart card for a discovered tag. */
static void create_chart_card(trends_context_t* context, const data_model_tag_t* tag)
{
    trend_binding_t* binding = &context->bindings[context->binding_count++];
    binding->tag_index = tag->index;

    lv_obj_t* card = lv_obj_create(context->root);
    lv_obj_set_size(card, 468, 218);
    style_card(card);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);

    data_model_equipment_t equipment;
    const char* equipment_name = "Unassigned equipment";
    if (data_model_get_equipment(context->data_model, tag->equipment_index, &equipment)) {
        equipment_name = equipment.display_name[0] != '\0' ? equipment.display_name : equipment.browse_name;
    }
    lv_obj_t* equipment_label = lv_label_create(card);
    lv_label_set_text(equipment_label, equipment_name);
    lv_label_set_long_mode(equipment_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(equipment_label, 260);
    lv_obj_set_style_text_color(equipment_label, lv_color_hex(TRENDS_COLOR_ACCENT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_font(equipment_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(equipment_label, LV_ALIGN_TOP_LEFT, 0, 0);

    const char* tag_name = tag->display_name[0] != '\0' ? tag->display_name : tag->browse_name;
    lv_obj_t* name_label = lv_label_create(card);
    lv_label_set_text(name_label, tag_name);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, 260);
    lv_obj_set_style_text_color(name_label, lv_color_hex(TRENDS_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 0, 22);

    binding->value_label = lv_label_create(card);
    lv_label_set_text(binding->value_label, "No value");
    lv_obj_set_width(binding->value_label, 140);
    lv_obj_set_style_text_align(binding->value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(binding->value_label, lv_color_hex(TRENDS_COLOR_ACCENT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_font(binding->value_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(binding->value_label, LV_ALIGN_TOP_RIGHT, 0, 20);

    binding->chart = lv_chart_create(card);
    lv_obj_set_size(binding->chart, 432, 126);
    lv_obj_align(binding->chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_chart_set_type(binding->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(binding->chart, TRENDS_POINT_COUNT);
    lv_chart_set_range(binding->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(binding->chart, 4, 6);
    lv_obj_set_style_bg_opa(binding->chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(binding->chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(binding->chart, lv_color_hex(TRENDS_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_width(binding->chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_color(binding->chart, lv_color_hex(TRENDS_COLOR_ACCENT), LV_PART_ITEMS);
    lv_obj_set_style_line_width(binding->chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(binding->chart, 0, LV_PART_INDICATOR);
    binding->series = lv_chart_add_series(binding->chart, lv_color_hex(TRENDS_COLOR_ACCENT),
                                           LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(binding->chart, binding->series, LV_CHART_POINT_NONE);
}

lv_obj_t* trends_create(lv_obj_t* parent, data_model_t* data_model)
{
    if (parent == NULL || data_model == NULL) {
        return NULL;
    }
    trends_context_t* context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->data_model = data_model;
    context->root = lv_obj_create(parent);
    if (context->root == NULL) {
        free(context);
        return NULL;
    }
    lv_obj_remove_style_all(context->root);
    lv_obj_set_size(context->root, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(context->root, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(context->root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(context->root, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_column(context->root, 16, LV_PART_MAIN);
    lv_obj_clear_flag(context->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(context->root, root_deleted_event, LV_EVENT_DELETE, context);

    size_t numeric_count = 0;
    size_t tag_count = data_model_tag_count(data_model);
    for (size_t tag_index = 0; tag_index < tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (! data_model_get_tag(data_model, tag_index, &tag) || ! tag_is_numeric(&tag)) {
            continue;
        }
        numeric_count++;
        if (context->binding_count < TRENDS_MAX_CHARTS) {
            create_chart_card(context, &tag);
        }
    }

    /* Move the heading before cards after counts are known. */
    create_heading(context->root, context->binding_count, numeric_count);
    lv_obj_move_to_index(lv_obj_get_child(context->root, context->binding_count), 0);

    if (context->binding_count == 0) {
        create_empty_state(context->root);
        return context->root;
    }

    for (size_t index = 0; index < context->binding_count; ++index) {
        sample_binding(context, &context->bindings[index]);
    }
    context->sample_timer = lv_timer_create(sample_timer_callback, TRENDS_SAMPLE_PERIOD_MS, context);
    return context->root;
}

static void sample_timer_callback(lv_timer_t* timer)
{
    trends_context_t* context = timer->user_data;
    for (size_t index = 0; index < context->binding_count; ++index) {
        sample_binding(context, &context->bindings[index]);
    }
}

static void root_deleted_event(lv_event_t* event)
{
    trends_context_t* context = lv_event_get_user_data(event);
    if (context->sample_timer != NULL) {
        lv_timer_del(context->sample_timer);
        context->sample_timer = NULL;
    }
    free(context);
}

static bool tag_numeric_value(const data_model_tag_t* tag, double* value_out)
{
    if (tag == NULL || value_out == NULL || ! tag->value_valid || ! tag_is_numeric(tag)) {
        return false;
    }
    double value = tag->data_type == DATA_MODEL_TYPE_INTEGER
                       ? (double)tag->value.integer_value
                       : tag->value.numeric_value;
    if (! isfinite(value)) {
        return false;
    }
    *value_out = value;
    return true;
}

static void sample_binding(trends_context_t* context, trend_binding_t* binding)
{
    data_model_tag_t tag;
    if (! data_model_get_tag(context->data_model, binding->tag_index, &tag)) {
        return;
    }

    memmove(&binding->samples[0], &binding->samples[1],
            (TRENDS_POINT_COUNT - 1) * sizeof(binding->samples[0]));
    memmove(&binding->sample_valid[0], &binding->sample_valid[1],
            (TRENDS_POINT_COUNT - 1) * sizeof(binding->sample_valid[0]));

    double value = 0.0;
    bool valid = tag_numeric_value(&tag, &value);
    binding->samples[TRENDS_POINT_COUNT - 1] = value;
    binding->sample_valid[TRENDS_POINT_COUNT - 1] = valid;
    if (valid) {
        char value_text[64];
        format_current_value(value_text, sizeof(value_text), &tag, value);
        lv_label_set_text(binding->value_label, value_text);
        lv_obj_set_style_text_color(binding->value_label, lv_color_hex(TRENDS_COLOR_ACCENT_SOFT), LV_PART_MAIN);
    } else {
        lv_label_set_text(binding->value_label, "No value");
        lv_obj_set_style_text_color(binding->value_label, lv_color_hex(TRENDS_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    }
    update_chart_points(binding, &tag);
}

static void update_chart_points(trend_binding_t* binding, const data_model_tag_t* tag)
{
    double minimum = 0.0;
    double maximum = 0.0;
    bool range_valid = tag->has_minimum && tag->has_maximum && tag->maximum > tag->minimum;
    if (range_valid) {
        minimum = tag->minimum;
        maximum = tag->maximum;
    } else {
        for (size_t index = 0; index < TRENDS_POINT_COUNT; ++index) {
            if (! binding->sample_valid[index]) {
                continue;
            }
            double sample = binding->samples[index];
            if (! range_valid) {
                minimum = sample;
                maximum = sample;
                range_valid = true;
            } else {
                if (sample < minimum) minimum = sample;
                if (sample > maximum) maximum = sample;
            }
        }
    }
    if (! range_valid) {
        lv_chart_set_all_value(binding->chart, binding->series, LV_CHART_POINT_NONE);
        return;
    }
    if (maximum <= minimum) {
        double padding = fmax(fabs(minimum) * 0.1, 1.0);
        minimum -= padding;
        maximum += padding;
    } else if (! (tag->has_minimum && tag->has_maximum)) {
        double padding = (maximum - minimum) * 0.1;
        minimum -= padding;
        maximum += padding;
    }

    double span = maximum - minimum;
    for (size_t index = 0; index < TRENDS_POINT_COUNT; ++index) {
        lv_coord_t chart_value = LV_CHART_POINT_NONE;
        if (binding->sample_valid[index]) {
            double normalized = (binding->samples[index] - minimum) * 100.0 / span;
            if (normalized < 0.0) normalized = 0.0;
            if (normalized > 100.0) normalized = 100.0;
            chart_value = (lv_coord_t)lround(normalized);
        }
        lv_chart_set_value_by_id(binding->chart, binding->series, (uint16_t)index, chart_value);
    }
    lv_chart_refresh(binding->chart);
}

static void format_current_value(char* destination, size_t destination_size,
                                 const data_model_tag_t* tag, double value)
{
    const char* unit = tag->engineering_unit;
    const char* separator = unit[0] != '\0' ? " " : "";
    if (tag->data_type == DATA_MODEL_TYPE_INTEGER) {
        snprintf(destination, destination_size, "%lld%s%.20s",
                 (long long)tag->value.integer_value, separator, unit);
    } else if (fabs(value) >= 1000.0) {
        snprintf(destination, destination_size, "%.0f%s%.20s", value, separator, unit);
    } else if (fabs(value) >= 100.0) {
        snprintf(destination, destination_size, "%.1f%s%.20s", value, separator, unit);
    } else {
        snprintf(destination, destination_size, "%.2f%s%.20s", value, separator, unit);
    }
}
