#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_lcd_touch.h"
#include "esp_log.h"

#include "bsp/display.h"

#include "idle_monitor.h"
#include "sleep_mode.h"
#include "settings_config.h"
#include "system_settings.h"

// defined in bsp component
extern esp_lcd_touch_handle_t tp;

static SemaphoreHandle_t touch_activity_sem = NULL;
static TimerHandle_t touch_debounce_timer   = NULL;

static const char* TAG = "idle monitor";

static void sleep_event_callback(sleep_evt_t event, void* context)
{
    (void)context;
    if (event == SLEEP_EVT_WAKE) {
        esp_err_t result = system_settings_restore_async();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Cannot schedule system settings restore after wake: %s", esp_err_to_name(result));
        }
    }
}

static esp_err_t sleep_brightness_set_fcnc(void* ctx, uint8_t pct)
{
    return bsp_display_brightness_set(pct);
}

static void touch_debounce_callback(TimerHandle_t xTimer)
{
    xSemaphoreGive(touch_activity_sem);
}

static void touch_isr_callback(esp_lcd_touch_handle_t tp)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTimerResetFromISR(touch_debounce_timer, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void idle_monitor_task(void* pv)
{
    while (1) {
        if (xSemaphoreTake(touch_activity_sem, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Activity notify (source=%s)", "touch");
            sleep_notify_activity();
        }
    }
}

esp_err_t idle_monitor_init()
{
    static bool init_once = false;
    if (init_once) {
        return ESP_OK;
    }

    uint8_t saved_brightness = settings_config_load_brightness();
    ESP_LOGI(TAG, "Loading saved brightness: %d%%", saved_brightness);
    
    sleep_cfg_t sleep_cfg = {
        .idle_timeout_ms       = settings_config_load_sleep_timeout_ms(),
        .normal_brightness_pct = saved_brightness,
        .sleep_brightness_pct  = 0,
        .brightness_init       = NULL,
        .brightness_set        = sleep_brightness_set_fcnc,
        .brightness_ctx        = NULL,
    };

    if (sleep_init(&sleep_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init sleep mode");
        return ESP_FAIL;
    }
    sleep_set_event_cb(sleep_event_callback, NULL);

    touch_activity_sem = xSemaphoreCreateBinary();
    if (! touch_activity_sem) {
        ESP_LOGE(TAG, "Failed to create touch semaphore");
        return ESP_FAIL;
    }

    touch_debounce_timer = xTimerCreate("TouchDebounce",
                                        pdMS_TO_TICKS(100), // Debounce period
                                        pdFALSE,            // One-shot
                                        NULL, touch_debounce_callback);

    if (xTaskCreate(idle_monitor_task, "idle_monitor_task", 2048, NULL, tskIDLE_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create idle_monitor_task");
        vSemaphoreDelete(touch_activity_sem);
        return ESP_FAIL;
    }

    if (esp_lcd_touch_register_interrupt_callback(tp, touch_isr_callback) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set touch callback");
        return ESP_FAIL;
    }

    init_once = true;
    return ESP_OK;
}
