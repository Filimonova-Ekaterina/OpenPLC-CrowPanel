#include "system_settings.h"

#include <stdint.h>

#include "bsp/display.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings_config.h"
#include "sleep_mode.h"

static const char* TAG = "system_settings";
static esp_codec_dev_handle_t s_speaker_codec;
static TaskHandle_t s_restore_task;

typedef struct
{
    const char* label;
    uint32_t milliseconds;
} sleep_option_t;

static const sleep_option_t SLEEP_OPTIONS[] = {
    {"1m", 1U * 60U * 1000U},
    {"3m", 3U * 60U * 1000U},
    {"5m", 5U * 60U * 1000U},
    {"10m", 10U * 60U * 1000U},
    {"30m", 30U * 60U * 1000U},
    {"1h", 60U * 60U * 1000U},
};

static esp_err_t apply_volume(uint8_t volume)
{
    if (s_speaker_codec == NULL) {
        s_speaker_codec = bsp_audio_codec_speaker_init();
    }
    if (s_speaker_codec == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_codec_dev_set_out_vol(s_speaker_codec, volume) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t system_settings_apply_saved(void)
{
    uint8_t brightness = settings_config_load_brightness();
    uint8_t volume = settings_config_load_audio_volume();
    uint32_t sleep_timeout_ms = settings_config_load_sleep_timeout_ms();

    sleep_update_normal_brightness(brightness);
    sleep_update_idle_timeout(sleep_timeout_ms);
    esp_err_t display_result = bsp_display_brightness_set(brightness);
    esp_err_t audio_result = apply_volume(volume);
    ESP_LOGI(TAG, "Restored: brightness=%u%% volume=%u%% sleep=%ums",
             (unsigned)brightness, (unsigned)volume, (unsigned)sleep_timeout_ms);
    return display_result != ESP_OK ? display_result : audio_result;
}

static void restore_task(void* argument)
{
    (void)argument;
    esp_err_t result = system_settings_apply_saved();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not apply every saved setting: %s", esp_err_to_name(result));
    }
    s_restore_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t system_settings_restore_async(void)
{
    if (s_restore_task != NULL) {
        return ESP_OK;
    }
    BaseType_t created = xTaskCreate(restore_task, "system_restore", 6144, NULL, 3, &s_restore_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static lv_obj_t* create_card(lv_obj_t* parent, const char* title, lv_coord_t height)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), height);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 0, 0);
    return card;
}

static lv_obj_t* add_slider(lv_obj_t* card, int minimum, int maximum, int value,
                            lv_event_cb_t callback)
{
    lv_obj_t* value_label = lv_label_create(card);
    lv_label_set_text_fmt(value_label, "%d%%", value);
    lv_obj_set_width(value_label, 70);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(value_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* slider = lv_slider_create(card);
    lv_slider_set_range(slider, minimum, maximum);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_size(slider, lv_pct(58), 22);
    lv_obj_align_to(slider, value_label, LV_ALIGN_OUT_LEFT_MID, -24, 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A96FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_ALL, value_label);
    return slider;
}

static void brightness_event(lv_event_t* event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t* slider = lv_event_get_target(event);
    int value = lv_slider_get_value(slider);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text_fmt(lv_event_get_user_data(event), "%d%%", value);
        sleep_update_normal_brightness(value);
        (void)bsp_display_brightness_set((uint8_t)value);
    } else if (code == LV_EVENT_RELEASED) {
        (void)settings_config_save_brightness((uint8_t)value);
    }
}

static void volume_event(lv_event_t* event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t* slider = lv_event_get_target(event);
    int value = lv_slider_get_value(slider);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text_fmt(lv_event_get_user_data(event), "%d%%", value);
        (void)apply_volume((uint8_t)value);
    } else if (code == LV_EVENT_RELEASED) {
        (void)settings_config_save_audio_volume((uint8_t)value);
    }
}

static void set_sleep_button_selected(lv_obj_t* button, bool selected)
{
    lv_obj_set_style_bg_color(button, selected ? lv_color_hex(0x184F73) : lv_color_hex(0x111820), LV_PART_MAIN);
    lv_obj_set_style_border_color(button, selected ? lv_color_hex(0x6BC1FF) : lv_color_hex(0x555555), LV_PART_MAIN);
}

static void sleep_option_event(lv_event_t* event)
{
    lv_obj_t* selected_button = lv_event_get_target(event);
    lv_obj_t* button_row = lv_event_get_user_data(event);
    uint32_t selected_timeout = (uint32_t)(uintptr_t)lv_obj_get_user_data(selected_button);
    if (settings_config_save_sleep_timeout_ms(selected_timeout) != ESP_OK) {
        return;
    }
    uint32_t child_count = lv_obj_get_child_cnt(button_row);
    for (uint32_t index = 0; index < child_count; ++index) {
        lv_obj_t* button = lv_obj_get_child(button_row, index);
        uint32_t timeout = (uint32_t)(uintptr_t)lv_obj_get_user_data(button);
        set_sleep_button_selected(button, timeout == selected_timeout);
    }
}

static void create_sleep_card(lv_obj_t* parent)
{
    lv_obj_t* card = create_card(parent, "Sleep after", 150);
    lv_obj_t* button_row = lv_obj_create(card);
    lv_obj_remove_style_all(button_row);
    lv_obj_set_size(button_row, lv_pct(68), 100);
    lv_obj_align(button_row, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_layout(button_row, LV_LAYOUT_GRID);
    static lv_coord_t columns[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rows[] = {44, 44, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(button_row, columns, rows);
    lv_obj_set_style_pad_column(button_row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(button_row, 10, LV_PART_MAIN);

    uint32_t saved_timeout = settings_config_load_sleep_timeout_ms();
    for (uint32_t index = 0; index < sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]); ++index) {
        lv_obj_t* button = lv_btn_create(button_row);
        lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
        lv_obj_set_user_data(button, (void*)(uintptr_t)SLEEP_OPTIONS[index].milliseconds);
        set_sleep_button_selected(button, SLEEP_OPTIONS[index].milliseconds == saved_timeout);
        lv_obj_set_grid_cell(button, LV_GRID_ALIGN_STRETCH, index % 3, 1,
                             LV_GRID_ALIGN_STRETCH, index / 3, 1);
        lv_obj_add_event_cb(button, sleep_option_event, LV_EVENT_CLICKED, button_row);
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, SLEEP_OPTIONS[index].label);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_center(label);
    }
}

lv_obj_t* system_settings_create(lv_obj_t* parent)
{
    if (parent == NULL) return NULL;
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 15, LV_PART_MAIN);
    lv_obj_set_style_pad_row(root, 14, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* brightness_card = create_card(root, "Brightness", 105);
    add_slider(brightness_card, 1, 100, settings_config_load_brightness(), brightness_event);
    lv_obj_t* volume_card = create_card(root, "Volume", 105);
    add_slider(volume_card, 0, 100, settings_config_load_audio_volume(), volume_event);
    create_sleep_card(root);
    return root;
}
