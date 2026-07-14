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
#include "ui_generator.h"
#include "wifi_ctrl.h"

static const char* TAG = "main";

static data_model_t* s_data_model;
static opcua_client_t* s_opcua_client;
static ui_generator_t* s_ui_generator;
static char s_opcua_endpoint[OPCUA_CLIENT_ENDPOINT_LENGTH];

/** Initialize NVS without silently erasing user Wi-Fi settings. */
static esp_err_t initialize_nonvolatile_storage(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS requires maintenance (%s); saved settings were not erased", esp_err_to_name(result));
    }
    return result;
}

/** Start the panel and the LVGL service owned by the board support package. */
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
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "Backlight initialization failed");
    return ESP_OK;
}

/** Create the transport-independent model, generated UI, and OPC UA task. */
static esp_err_t initialize_hmi_pipeline(void)
{
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
        .maximum_tags             = CONFIG_OPCUA_MAX_TAGS,
        .maximum_browse_depth     = CONFIG_OPCUA_MAX_BROWSE_DEPTH,
    };
    ESP_RETURN_ON_ERROR(opcua_client_create(&client_configuration, s_data_model, &s_opcua_client), TAG,
                        "Cannot create OPC UA client");

    if (! bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Cannot lock LVGL to create generated UI");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ui_result = ui_generator_create(lv_scr_act(), s_data_model, s_opcua_client, &s_ui_generator);
    bsp_display_unlock();
    ESP_RETURN_ON_ERROR(ui_result, TAG, "Cannot create generated UI");

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

    esp_err_t event_loop_result = esp_event_loop_create_default();
    if (event_loop_result != ESP_OK && event_loop_result != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(event_loop_result);
    }

    esp_err_t firmware_update_result = c6_fw_update_check_and_apply();
    if (firmware_update_result != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi module firmware check failed: %s", esp_err_to_name(firmware_update_result));
    }

    /* Existing settings component owns Wi-Fi credentials, scanning, and reconnect. */
    wifi_ctrl_init();
    ESP_ERROR_CHECK(idle_monitor_init());

    ESP_ERROR_CHECK(initialize_hmi_pipeline());
    ESP_LOGI(TAG, "HMI initialized; OPC UA endpoint: %s", s_opcua_endpoint);
}
