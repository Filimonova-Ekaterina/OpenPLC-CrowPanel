#include "ui_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const lv_point_t PRESSURE_NEEDLE_POINTS[] = {{0, 8}, {8, 0}};

void ui_theme_style_page(lv_obj_t* page)
{
    if (page == NULL) {
        return;
    }
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_COLOR_ACCENT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(page, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(page, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(page, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
}

void ui_theme_style_card(lv_obj_t* card, lv_color_t background, lv_coord_t radius)
{
    if (card == NULL) {
        return;
    }
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, background, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, radius, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
}

lv_obj_t* ui_theme_create_heading(lv_obj_t* parent, const char* title, const char* detail)
{
    lv_obj_t* heading = lv_obj_create(parent);
    lv_obj_remove_style_all(heading);
    lv_obj_set_size(heading, lv_pct(100), 52);
    lv_obj_clear_flag(heading, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* accent = lv_obj_create(heading);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 5, 30);
    lv_obj_set_style_radius(accent, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(accent, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t* title_label = lv_label_create(heading);
    lv_label_set_text(title_label, title != NULL ? title : "");
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title_label, lv_pct(68));
    lv_obj_set_style_text_color(title_label, lv_color_hex(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_26, LV_PART_MAIN);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 24, 0);

    if (detail != NULL && detail[0] != '\0') {
        lv_obj_t* detail_label = lv_label_create(heading);
        lv_label_set_text(detail_label, detail);
        lv_obj_set_style_text_color(detail_label, lv_color_hex(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
        lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(detail_label, LV_ALIGN_RIGHT_MID, 0, 1);
    }
    return heading;
}

void ui_theme_create_empty_state(lv_obj_t* parent, const char* title, const char* detail, lv_color_t accent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), 124);
    ui_theme_style_card(card, lv_color_hex(UI_COLOR_SURFACE), 20);
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

void ui_theme_humanize_name(char* destination, size_t destination_size, const char* source)
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
    unsigned char previous   = 0;
    while (source[source_index] != '\0' && destination_index + 1 < destination_size) {
        unsigned char current  = (unsigned char)source[source_index];
        unsigned char next     = (unsigned char)source[source_index + 1];
        bool current_is_upper  = current >= 'A' && current <= 'Z';
        bool current_is_lower  = current >= 'a' && current <= 'z';
        bool current_is_digit  = current >= '0' && current <= '9';
        bool previous_is_upper = previous >= 'A' && previous <= 'Z';
        bool previous_is_lower = previous >= 'a' && previous <= 'z';
        bool previous_is_digit = previous >= '0' && previous <= '9';
        bool next_is_lower     = next >= 'a' && next <= 'z';

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
        previous                         = current;
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

void ui_theme_copy_display_unit(char* destination, size_t destination_size, const char* source)
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
        unsigned char first  = (unsigned char)source[source_index];
        unsigned char second = (unsigned char)source[source_index + 1];
        if (first == 0xC2 && (second == 0xB2 || second == 0xB3)) {
            destination[destination_index++] = second == 0xB2 ? '2' : '3';
            source_index += 2;
        } else {
            destination[destination_index++] = source[source_index++];
        }
    }
    destination[destination_index] = '\0';
}

void ui_theme_format_tag_value(char* destination, size_t destination_size, const data_model_tag_t* tag)
{
    if (destination == NULL || destination_size == 0 || tag == NULL) {
        return;
    }
    if (! tag->value_valid) {
        snprintf(destination, destination_size, "No value");
        return;
    }
    if (tag->data_type == DATA_MODEL_TYPE_BOOLEAN) {
        snprintf(destination, destination_size, "%s", tag->value.boolean_value ? "ON" : "OFF");
        return;
    }
    if (tag->data_type == DATA_MODEL_TYPE_STRING) {
        snprintf(destination, destination_size, "%.63s", tag->value.string_value);
        return;
    }

    double value =
        tag->data_type == DATA_MODEL_TYPE_INTEGER ? (double)tag->value.integer_value : tag->value.numeric_value;
    if (! isfinite(value)) {
        snprintf(destination, destination_size, "%s", isnan(value) ? "NaN" : "Out of range");
        return;
    }
    char unit[DATA_MODEL_UNIT_LENGTH];
    ui_theme_copy_display_unit(unit, sizeof(unit), tag->engineering_unit);
    const char* separator = unit[0] != '\0' ? " " : "";
    if (tag->data_type == DATA_MODEL_TYPE_INTEGER) {
        snprintf(destination, destination_size, "%lld%s%.20s", (long long)tag->value.integer_value, separator, unit);
    } else if (fabs(value) >= 1000.0) {
        snprintf(destination, destination_size, "%.0f%s%.20s", value, separator, unit);
    } else if (fabs(value) >= 100.0) {
        snprintf(destination, destination_size, "%.1f%s%.20s", value, separator, unit);
    } else {
        snprintf(destination, destination_size, "%.2f%s%.20s", value, separator, unit);
    }
}

bool ui_theme_find_tag_by_role(const data_model_t* model, size_t equipment_index, const char* semantic_role,
                               data_model_tag_t* tag_out)
{
    if (model == NULL || semantic_role == NULL || tag_out == NULL) {
        return false;
    }
    size_t tag_count = data_model_tag_count(model);
    for (size_t index = 0; index < tag_count; ++index) {
        data_model_tag_t tag;
        if (data_model_get_tag(model, index, &tag) && tag.equipment_index == equipment_index &&
            strcmp(tag.semantic_role, semantic_role) == 0) {
            *tag_out = tag;
            return true;
        }
    }
    return false;
}

const char* ui_theme_tag_icon(const data_model_tag_t* tag)
{
    if (tag == NULL) {
        return LV_SYMBOL_BARS;
    }
    const char* role = tag->semantic_role;
    if (strcmp(role, "temperature") == 0)
        return "T";
    if (strcmp(role, "pressure") == 0)
        return "P";
    if (strcmp(role, "motor_current") == 0)
        return LV_SYMBOL_CHARGE;
    if (strcmp(role, "runtime") == 0)
        return LV_SYMBOL_LOOP;
    if (strcmp(role, "flow") == 0 || strcmp(role, "demand") == 0)
        return LV_SYMBOL_TINT;
    if (strcmp(role, "power") == 0)
        return LV_SYMBOL_POWER;
    if (strcmp(role, "operating_status") == 0 || strcmp(role, "operating_command") == 0)
        return LV_SYMBOL_PLAY;
    if (strcmp(role, "alarm_status") == 0 || strcmp(role, "fault_command") == 0)
        return LV_SYMBOL_WARNING;
    if (strcmp(role, "automatic_mode") == 0)
        return LV_SYMBOL_REFRESH;
    return tag->data_type == DATA_MODEL_TYPE_BOOLEAN ? LV_SYMBOL_OK : LV_SYMBOL_BARS;
}

void ui_theme_create_tag_icon(lv_obj_t* parent, const data_model_tag_t* tag, lv_coord_t size)
{
    if (parent == NULL || tag == NULL) {
        return;
    }
    const char* role = tag->semantic_role;
    lv_color_t color = lv_color_hex(UI_COLOR_ACCENT_SOFT);
    if (strcmp(role, "temperature") == 0) {
        lv_obj_t* stem = lv_obj_create(parent);
        lv_obj_remove_style_all(stem);
        lv_obj_set_size(stem, size / 5, size / 2);
        lv_obj_set_style_radius(stem, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(stem, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(stem, LV_ALIGN_CENTER, 0, -size / 8);

        lv_obj_t* bulb = lv_obj_create(parent);
        lv_obj_remove_style_all(bulb);
        lv_obj_set_size(bulb, size / 2, size / 2);
        lv_obj_set_style_radius(bulb, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bulb, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bulb, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(bulb, LV_ALIGN_CENTER, 0, size / 5);
        return;
    }
    if (strcmp(role, "demand") == 0) {
        const lv_coord_t heights[] = {size / 3, size / 2, (lv_coord_t)(size * 3 / 4)};
        lv_coord_t group_height    = heights[2];
        for (size_t index = 0; index < 3; ++index) {
            lv_obj_t* bar = lv_obj_create(parent);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, size / 5, heights[index]);
            lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, color, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_align(bar, LV_ALIGN_CENTER, (lv_coord_t)((int)index - 1) * (size / 3),
                         (group_height - heights[index]) / 2);
        }
        return;
    }
    if (strcmp(role, "pressure") == 0) {
        lv_obj_t* gauge = lv_arc_create(parent);
        lv_obj_remove_style_all(gauge);
        lv_obj_set_size(gauge, size, size);
        lv_arc_set_rotation(gauge, 135);
        lv_arc_set_bg_angles(gauge, 0, 270);
        lv_arc_set_range(gauge, 0, 100);
        lv_arc_set_value(gauge, 68);
        lv_obj_set_style_arc_width(gauge, size >= 30 ? 4 : 3, LV_PART_MAIN);
        lv_obj_set_style_arc_color(gauge, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(gauge, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_width(gauge, size >= 30 ? 4 : 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(gauge, color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(gauge, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(gauge, 0, LV_PART_KNOB);
        lv_obj_clear_flag(gauge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(gauge);

        lv_obj_t* needle = lv_line_create(parent);
        lv_line_set_points(needle, PRESSURE_NEEDLE_POINTS,
                           sizeof(PRESSURE_NEEDLE_POINTS) / sizeof(PRESSURE_NEEDLE_POINTS[0]));
        lv_obj_set_style_line_width(needle, size >= 24 ? 3 : 2, LV_PART_MAIN);
        lv_obj_set_style_line_color(needle, color, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(needle, true, LV_PART_MAIN);
        lv_obj_align(needle, LV_ALIGN_CENTER, 4, -4);

        lv_obj_t* hub = lv_obj_create(parent);
        lv_obj_remove_style_all(hub);
        lv_obj_set_size(hub, size >= 24 ? 5 : 4, size >= 24 ? 5 : 4);
        lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(hub, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_center(hub);
        return;
    }

    lv_obj_t* icon = lv_label_create(parent);
    lv_label_set_text(icon, ui_theme_tag_icon(tag));
    lv_obj_set_style_text_color(icon, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(icon, size >= 30 ? &lv_font_montserrat_18 : &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(icon);
}

lv_color_t ui_theme_boolean_color(const data_model_tag_t* tag, bool value)
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

void ui_theme_set_label_text(lv_obj_t* label, const char* text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char* current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}
