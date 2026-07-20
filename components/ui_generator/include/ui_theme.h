#pragma once

/**
 * @file ui_theme.h
 * @brief Shared visual language and formatting helpers for generated HMI pages.
 */

#include <stdbool.h>
#include <stddef.h>

#include "data_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_COLOR_BACKGROUND     0x080B0E
#define UI_COLOR_SURFACE        0x11171C
#define UI_COLOR_SURFACE_RAISED 0x151D23
#define UI_COLOR_BORDER         0x34414A
#define UI_COLOR_CONTROL_BORDER 0x52616B
#define UI_COLOR_TEXT_PRIMARY   0xF4F7F9
#define UI_COLOR_TEXT_LABEL     0xD5DCE1
#define UI_COLOR_TEXT_SECONDARY 0x97A3AB
#define UI_COLOR_ACCENT         0x2A96FF
#define UI_COLOR_ACCENT_SOFT    0x6BC1FF
#define UI_COLOR_SUCCESS        0x58D000
#define UI_COLOR_WARNING        0xF0A202
#define UI_COLOR_DANGER         0xFF5C35
#define UI_COLOR_INACTIVE       0x737F87

#define UI_HUMAN_NAME_LENGTH 128

/** Apply the standard wrapping layout and scrollbar treatment to a generated page. */
void ui_theme_style_page(lv_obj_t* page);

/** Apply the shared quiet card surface. */
void ui_theme_style_card(lv_obj_t* card, lv_color_t background, lv_coord_t radius);

/** Create a full-width page or section heading. */
lv_obj_t* ui_theme_create_heading(lv_obj_t* parent, const char* title, const char* detail);

/** Create a neutral generated-data empty state. */
void ui_theme_create_empty_state(lv_obj_t* parent, const char* title, const char* detail, lv_color_t accent);

/** Make OPC UA BrowseNames readable without modifying the Data Model. */
void ui_theme_humanize_name(char* destination, size_t destination_size, const char* source);

/** Replace engineering-unit glyphs that are absent from the embedded font. */
void ui_theme_copy_display_unit(char* destination, size_t destination_size, const char* source);

/** Format a tag value without relying on LVGL floating-point printf support. */
void ui_theme_format_tag_value(char* destination, size_t destination_size, const data_model_tag_t* tag);

/** Find the first tag with a semantic role on one discovered equipment object. */
bool ui_theme_find_tag_by_role(const data_model_t* model, size_t equipment_index, const char* semantic_role,
                               data_model_tag_t* tag_out);

/** Return a compact built-in icon selected from protocol semantic metadata. */
const char* ui_theme_tag_icon(const data_model_tag_t* tag);

/** Draw a semantic LVGL icon with a type-based fallback for unknown roles. */
void ui_theme_create_tag_icon(lv_obj_t* parent, const data_model_tag_t* tag, lv_coord_t size);

/** Map Boolean semantics to a restrained state color. */
lv_color_t ui_theme_boolean_color(const data_model_tag_t* tag, bool value);

/** Update a label only when its text changed. */
void ui_theme_set_label_text(lv_obj_t* label, const char* text);

#ifdef __cplusplus
}
#endif
