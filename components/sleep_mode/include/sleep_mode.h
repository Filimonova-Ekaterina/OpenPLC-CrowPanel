#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SLEEP_STATE_AWAKE = 0,
    SLEEP_STATE_SLEEPING
} sleep_state_t;

typedef enum
{
    SLEEP_EVT_ENTER = 0,
    SLEEP_EVT_WAKE
} sleep_evt_t;

typedef void (*sleep_event_cb_t)(sleep_evt_t evt, void* ctx);

// --- Brightness driver callbacks ---
typedef esp_err_t (*sleep_brightness_init_fn)(void* ctx);
typedef esp_err_t (*sleep_brightness_set_fn)(void* ctx, uint8_t pct);

// --- Optional esp_event integration ---
ESP_EVENT_DECLARE_BASE(SLEEP_EVENT);

typedef enum
{
    SLEEP_EVENT_ENTER = 0,
    SLEEP_EVENT_WAKE  = 1,
} sleep_event_id_t;

typedef struct
{
    uint32_t idle_timeout_ms; // e.g. 60000

    int normal_brightness_pct; // e.g. 100
    int sleep_brightness_pct;  // e.g. 0

    /* Delay turning brightness back on after WAKE to avoid showing stale UI frames. */
    uint32_t wake_brightness_delay_ms;

    // brightness driver
    sleep_brightness_init_fn brightness_init;
    sleep_brightness_set_fn brightness_set;
    void* brightness_ctx;
} sleep_cfg_t;

esp_err_t sleep_init(const sleep_cfg_t* cfg);
esp_err_t sleep_deinit(void);

void sleep_set_event_cb(sleep_event_cb_t cb, void* ctx);

/**
 * Notify about user activity (touch/button).
 * - If AWAKE: resets idle timer.
 * - If SLEEPING: turns backlight on, switches to AWAKE, emits WAKE, resets timer.
 */
void sleep_notify_activity(void);

void sleep_force_sleep(void);
void sleep_force_wake(void);
void sleep_update_normal_brightness(int brightness_pct);
void sleep_update_idle_timeout(uint32_t idle_timeout_ms);

sleep_state_t sleep_get_state(void);
uint32_t sleep_get_idle_timeout_ms(void);
int sleep_get_normal_brightness_pct(void);
int sleep_get_sleep_brightness_pct(void);

#ifdef __cplusplus
}
#endif
