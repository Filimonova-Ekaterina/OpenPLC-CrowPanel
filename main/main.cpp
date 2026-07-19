#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "c6_fw_update.h"
#include "data_model.h"
#include "idle_monitor.h"
#include "opcua_client.h"
#include "settings_config.h"
#include "sleep_mode.h"
#include "system_settings.h"
#include "time_sync.h"
#include "ui_generator.h"
#include "wifi_ctrl.h"

static const char* TAG = "main";

static data_model_t* s_data_model;
static opcua_client_t* s_opcua_client;
static ui_generator_t* s_ui_generator;
static char s_opcua_endpoint[OPCUA_CLIENT_ENDPOINT_LENGTH];
static lv_obj_t* s_startup_overlay;
static lv_obj_t* s_startup_stage_label;
static lv_obj_t* s_startup_detail_label;
static lv_obj_t* s_startup_progress_bar;
static lv_obj_t* s_startup_progress_label;

/** Pause only OPC UA traffic during display sleep; keep Wi-Fi associated. */
static void handle_sleep_event(void* argument, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_data;
    if (event_id == SLEEP_EVENT_ENTER) {
        opcua_client_pause(s_opcua_client);
    } else if (event_id == SLEEP_EVENT_WAKE) {
        opcua_client_resume(s_opcua_client);
    }
}

/** Initialize NVS without silently erasing user Wi-Fi settings. */
static esp_err_t initialize_nonvolatile_storage(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS requires maintenance (%s); saved settings were not erased", esp_err_to_name(result));
    }
    return result;
}

/** Start LVGL with the panel backlight kept off until the first frame is ready. */
static esp_err_t initialize_display(void)
{
    bsp_display_cfg_t display_configuration = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = BSP_LCD_H_RES * 60,
        .double_buffer = true,
        .flags =
            {
                .buff_dma    = false,
                .buff_spiram = true,
                .sw_rotate   = false,
            },
    };

    if (bsp_display_start_with_config(&display_configuration) == nullptr) {
        ESP_LOGE(TAG, "Display initialization failed");
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(bsp_display_backlight_off(), TAG, "Cannot keep backlight off during startup");
    return ESP_OK;
}

/** Draw the first visible frame and turn on the panel at the saved brightness. */
static esp_err_t create_startup_screen(void)
{
    if (! bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    s_startup_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_startup_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_startup_overlay, lv_color_hex(0x080808), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_startup_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_startup_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_startup_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_startup_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_startup_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(s_startup_overlay);
    lv_obj_set_size(panel, 680, 300);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x151515), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 28, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 32, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(panel);

    lv_obj_t* spinner = lv_spinner_create(panel, 900, 85);
    lv_obj_set_size(spinner, 66, 66);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x2A96FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
    lv_obj_align(spinner, LV_ALIGN_LEFT_MID, 8, -4);

    s_startup_stage_label = lv_label_create(panel);
    lv_label_set_text(s_startup_stage_label, "Starting system");
    lv_label_set_long_mode(s_startup_stage_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_startup_stage_label, 500);
    lv_obj_set_style_text_color(s_startup_stage_label, lv_color_hex(0xF4F4F4), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_startup_stage_label, &lv_font_montserrat_30, LV_PART_MAIN);
    lv_obj_align(s_startup_stage_label, LV_ALIGN_LEFT_MID, 104, -28);

    s_startup_detail_label = lv_label_create(panel);
    lv_label_set_text(s_startup_detail_label, "Preparing core services");
    lv_label_set_long_mode(s_startup_detail_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_startup_detail_label, 500);
    lv_obj_set_style_text_color(s_startup_detail_label, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_startup_detail_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(s_startup_detail_label, LV_ALIGN_LEFT_MID, 104, 18);

    s_startup_progress_bar = lv_bar_create(panel);
    lv_obj_set_size(s_startup_progress_bar, 536, 14);
    lv_bar_set_range(s_startup_progress_bar, 0, 100);
    lv_bar_set_value(s_startup_progress_bar, 8, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_startup_progress_bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_startup_progress_bar, lv_color_hex(0x2A96FF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_startup_progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_startup_progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_align(s_startup_progress_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_startup_progress_label = lv_label_create(panel);
    lv_label_set_text(s_startup_progress_label, "8%");
    lv_obj_set_width(s_startup_progress_label, 60);
    lv_obj_set_style_text_align(s_startup_progress_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_startup_progress_label, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_startup_progress_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(s_startup_progress_label, LV_ALIGN_BOTTOM_RIGHT, 0, 2);

    lv_refr_now(NULL);
    bsp_display_unlock();

    int startup_brightness = settings_config_load_brightness();
    esp_err_t brightness_result = bsp_display_brightness_set(startup_brightness);
    if (brightness_result != ESP_OK) {
        return bsp_display_backlight_on();
    }
    return ESP_OK;
}

/** Update startup progress synchronously before beginning the next blocking stage. */
static void update_startup_screen(int progress, const char* stage, const char* detail)
{
    if (s_startup_overlay == nullptr || ! bsp_display_lock(1000)) {
        return;
    }
    lv_obj_move_foreground(s_startup_overlay);
    lv_label_set_text(s_startup_stage_label, stage);
    lv_label_set_text(s_startup_detail_label, detail);
    lv_bar_set_value(s_startup_progress_bar, progress, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_startup_progress_label, "%d%%", progress);
    lv_refr_now(NULL);
    bsp_display_unlock();
}

/** Reveal the generated HMI only after every required startup service is ready. */
static void close_startup_screen(void)
{
    if (s_startup_overlay == nullptr || ! bsp_display_lock(1000)) {
        return;
    }
    lv_obj_del(s_startup_overlay);
    s_startup_overlay = nullptr;
    s_startup_stage_label = nullptr;
    s_startup_detail_label = nullptr;
    s_startup_progress_bar = nullptr;
    s_startup_progress_label = nullptr;
    lv_refr_now(NULL);
    bsp_display_unlock();
}

/** Create the transport-independent model, generated UI, and OPC UA task. */
static esp_err_t initialize_hmi_pipeline(void)
{
    update_startup_screen(62, "Preparing industrial data", "Creating the OPC UA data model");
    ESP_RETURN_ON_ERROR(data_model_create(CONFIG_OPCUA_MAX_EQUIPMENT_OBJECTS, CONFIG_OPCUA_MAX_TAGS, &s_data_model),
                        TAG, "Cannot create Data Model");

    esp_err_t endpoint_result =
        settings_config_load_opcua_endpoint(s_opcua_endpoint, sizeof(s_opcua_endpoint), CONFIG_OPCUA_SERVER_ENDPOINT);
    if (endpoint_result != ESP_OK) {
        ESP_LOGW(TAG, "Using default OPC UA endpoint: %s", esp_err_to_name(endpoint_result));
    }

    opcua_client_config_t client_configuration = {
        .endpoint_url             = s_opcua_endpoint,
        .connection_timeout_ms    = CONFIG_OPCUA_CONNECTION_TIMEOUT_MS,
        .reconnect_delay_ms       = CONFIG_OPCUA_RECONNECT_DELAY_MS,
        .subscription_interval_ms = CONFIG_OPCUA_SUBSCRIPTION_INTERVAL_MS,
        .task_stack_size          = CONFIG_OPCUA_TASK_STACK_SIZE,
        .task_priority            = CONFIG_OPCUA_TASK_PRIORITY,
        .maximum_equipment_objects = CONFIG_OPCUA_MAX_EQUIPMENT_OBJECTS,
        .maximum_tags             = CONFIG_OPCUA_MAX_TAGS,
        .maximum_browse_depth     = CONFIG_OPCUA_MAX_BROWSE_DEPTH,
    };
    ESP_RETURN_ON_ERROR(opcua_client_create(&client_configuration, s_data_model, &s_opcua_client), TAG,
                        "Cannot create OPC UA client");

    update_startup_screen(76, "Building the interface", "Creating pages, controls, and trend charts");
    if (! bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Cannot lock LVGL to create generated UI");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ui_result = ui_generator_create(lv_scr_act(), s_data_model, s_opcua_client, &s_ui_generator);
    if (ui_result == ESP_OK) {
        /* Render synchronously before sleep_init restores the saved brightness. */
        lv_refr_now(NULL);
    }
    bsp_display_unlock();
    ESP_RETURN_ON_ERROR(ui_result, TAG, "Cannot create generated UI");

    update_startup_screen(88, "Starting live monitoring", "Connecting background data services");
    ESP_RETURN_ON_ERROR(opcua_client_start(s_opcua_client), TAG, "Cannot start OPC UA client task");
    return ESP_OK;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting auto-discovery industrial HMI");
    heap_caps_malloc_extmem_enable(2048);

    ESP_ERROR_CHECK(initialize_nonvolatile_storage());
    ESP_ERROR_CHECK(settings_config_init());
    ESP_ERROR_CHECK(initialize_display());
    ESP_ERROR_CHECK(create_startup_screen());

    update_startup_screen(16, "Preparing system services", "Starting internal event handling");
    esp_err_t event_loop_result = esp_event_loop_create_default();
    if (event_loop_result != ESP_OK && event_loop_result != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(event_loop_result);
    }

    update_startup_screen(28, "Preparing communication", "Checking the wireless controller");
    esp_err_t firmware_update_result = c6_fw_update_check_and_apply();
    if (firmware_update_result != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi module firmware check failed: %s", esp_err_to_name(firmware_update_result));
    }

    /* Existing settings component owns Wi-Fi credentials, scanning, and reconnect. */
    update_startup_screen(42, "Starting network services", "Loading saved Wi-Fi settings");
    wifi_ctrl_init();
    update_startup_screen(54, "Synchronizing system time", "Preparing reliable timestamps");
    ESP_ERROR_CHECK(time_sync_init());
    ESP_ERROR_CHECK(initialize_hmi_pipeline());
    update_startup_screen(94, "Applying system settings", "Starting display and idle monitoring");
    ESP_ERROR_CHECK(idle_monitor_init());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(SLEEP_EVENT, ESP_EVENT_ANY_ID,
                                                        handle_sleep_event, NULL, NULL));
    esp_err_t system_settings_result = system_settings_restore_async();
    if (system_settings_result != ESP_OK) {
        ESP_LOGW(TAG, "Saved system settings restore could not be scheduled: %s",
                 esp_err_to_name(system_settings_result));
    }
    update_startup_screen(100, "System ready", "Opening the live interface");
    close_startup_screen();
    ESP_LOGI(TAG, "HMI initialized; OPC UA endpoint: %s", s_opcua_endpoint);
}
