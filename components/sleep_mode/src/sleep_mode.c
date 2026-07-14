#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_event.h"
#include "esp_log.h"

#include "sleep_mode.h"

ESP_EVENT_DEFINE_BASE(SLEEP_EVENT);

static const char* TAG = "sleep_mode";

typedef struct
{
    bool inited;

    sleep_cfg_t cfg;
    sleep_state_t state;

    TimerHandle_t idle_timer;
    TimerHandle_t wake_brightness_timer;
    SemaphoreHandle_t mu;

    // fallback callback
    sleep_event_cb_t cb;
    void* cb_ctx;
} sleep_ctx_t;

static sleep_ctx_t g = {0};

static portMUX_TYPE g_cb_mux = portMUX_INITIALIZER_UNLOCKED;

static void idle_timer_cb(TimerHandle_t xTimer);
static void wake_brightness_timer_cb(TimerHandle_t xTimer);

static void restart_idle_timer_locked(void)
{
    if (! g.idle_timer)
        return;

    // one-shot timer; reset starts it if stopped
    (void)xTimerReset(g.idle_timer, 0);
}

static void do_emit_evt(sleep_evt_t evt, sleep_event_cb_t cb, void* cb_ctx)
{
    int32_t id = (evt == SLEEP_EVT_ENTER) ? SLEEP_EVENT_ENTER : SLEEP_EVENT_WAKE;
    (void)esp_event_post(SLEEP_EVENT, id, NULL, 0, 0);

    if (cb)
        cb(evt, cb_ctx);
}

void sleep_update_normal_brightness(int brightness_pct)
{
    if (!g.mu) return;
    
    xSemaphoreTake(g.mu, portMAX_DELAY);
    if (g.inited) {
        g.cfg.normal_brightness_pct = brightness_pct;
        ESP_LOGI(TAG, "Updated normal brightness to %d%%", brightness_pct);
    }
    xSemaphoreGive(g.mu);
}

void sleep_update_idle_timeout(uint32_t idle_timeout_ms)
{
    if (idle_timeout_ms == 0 || !g.mu) return;

    xSemaphoreTake(g.mu, portMAX_DELAY);
    if (g.inited) {
        g.cfg.idle_timeout_ms = idle_timeout_ms;
        if (g.idle_timer && g.state == SLEEP_STATE_AWAKE) {
            (void)xTimerChangePeriod(g.idle_timer, pdMS_TO_TICKS(idle_timeout_ms), 0);
            restart_idle_timer_locked();
        }
        ESP_LOGI(TAG, "Updated idle timeout to %ums", (unsigned)idle_timeout_ms);
    }
    xSemaphoreGive(g.mu);
}

static void enter_sleep_internal(bool from_timer)
{
    if (! g.mu)
        return;

    sleep_event_cb_t cb = NULL;
    void* cb_ctx        = NULL;
    bool do_evt         = false;

    xSemaphoreTake(g.mu, portMAX_DELAY);

    if (g.inited && g.state == SLEEP_STATE_AWAKE) {
        /* Cancel pending wake brightness, if any. */
        if (g.wake_brightness_timer) {
            (void)xTimerStop(g.wake_brightness_timer, 0);
        }

        // backlight to sleep level
        if (g.cfg.brightness_set) {
            (void)g.cfg.brightness_set(g.cfg.brightness_ctx, (uint8_t)g.cfg.sleep_brightness_pct);
        }

        g.state = SLEEP_STATE_SLEEPING;

        cb     = g.cb;
        cb_ctx = g.cb_ctx;
        do_evt = true;
    }

    xSemaphoreGive(g.mu);

    if (do_evt) {
        ESP_LOGI(TAG, "ENTER (reason=%s)", from_timer ? "idle" : "force");
        do_emit_evt(SLEEP_EVT_ENTER, cb, cb_ctx);
    }
}

static void wake_internal(bool from_activity)
{
    if (! g.mu)
        return;

    sleep_event_cb_t cb = NULL;
    void* cb_ctx        = NULL;
    bool do_evt         = false;

    xSemaphoreTake(g.mu, portMAX_DELAY);

    if (g.inited && g.state == SLEEP_STATE_SLEEPING) {
        g.state = SLEEP_STATE_AWAKE;

        cb     = g.cb;
        cb_ctx = g.cb_ctx;
        do_evt = true;

        restart_idle_timer_locked();

        if (g.cfg.wake_brightness_delay_ms == 0) {
            if (g.cfg.brightness_set) {
                (void)g.cfg.brightness_set(g.cfg.brightness_ctx, (uint8_t)g.cfg.normal_brightness_pct);
            }
        } else if (g.wake_brightness_timer) {
            (void)xTimerChangePeriod(g.wake_brightness_timer, pdMS_TO_TICKS(g.cfg.wake_brightness_delay_ms), 0);
            (void)xTimerStart(g.wake_brightness_timer, 0);
        }
    } else if (g.inited && g.state == SLEEP_STATE_AWAKE) {
        restart_idle_timer_locked();
    }

    xSemaphoreGive(g.mu);

    if (do_evt) {
        ESP_LOGI(TAG, "WAKE (reason=%s)", from_activity ? "activity" : "force");
        do_emit_evt(SLEEP_EVT_WAKE, cb, cb_ctx);
    }
}

static void idle_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    enter_sleep_internal(true);
}

static void wake_brightness_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    if (! g.mu)
        return;

    xSemaphoreTake(g.mu, portMAX_DELAY);

    if (g.inited && g.state == SLEEP_STATE_AWAKE && g.cfg.brightness_set) {
        (void)g.cfg.brightness_set(g.cfg.brightness_ctx, (uint8_t)g.cfg.normal_brightness_pct);
    }

    xSemaphoreGive(g.mu);
}

esp_err_t sleep_init(const sleep_cfg_t* cfg)
{
    if (! cfg)
        return ESP_ERR_INVALID_ARG;
    if (cfg->idle_timeout_ms == 0)
        return ESP_ERR_INVALID_ARG;

    if (cfg->normal_brightness_pct < 0 || cfg->normal_brightness_pct > 100)
        return ESP_ERR_INVALID_ARG;
    if (cfg->sleep_brightness_pct < 0 || cfg->sleep_brightness_pct > 100)
        return ESP_ERR_INVALID_ARG;
    if (! cfg->brightness_set)
        return ESP_ERR_INVALID_ARG;

    if (g.inited)
        return ESP_ERR_INVALID_STATE;

    g.mu = xSemaphoreCreateMutex();
    if (! g.mu)
        return ESP_ERR_NO_MEM;

    xSemaphoreTake(g.mu, portMAX_DELAY);

    g.cfg   = *cfg;
    g.state = SLEEP_STATE_AWAKE;

    g.idle_timer =
        xTimerCreate("sleep_idle", pdMS_TO_TICKS(g.cfg.idle_timeout_ms), pdFALSE /* one-shot */, NULL, idle_timer_cb);
    if (! g.idle_timer) {
        xSemaphoreGive(g.mu);
        vSemaphoreDelete(g.mu);
        g.mu = NULL;
        return ESP_ERR_NO_MEM;
    }

    g.wake_brightness_timer =
        xTimerCreate("sleep_wake_bl", pdMS_TO_TICKS(1), pdFALSE /* one-shot */, NULL, wake_brightness_timer_cb);
    if (! g.wake_brightness_timer) {
        (void)xTimerDelete(g.idle_timer, 0);
        g.idle_timer = NULL;

        xSemaphoreGive(g.mu);
        vSemaphoreDelete(g.mu);
        g.mu = NULL;
        return ESP_ERR_NO_MEM;
    }

    g.inited = true;

    xSemaphoreGive(g.mu);

    if (g.cfg.brightness_init) {
        esp_err_t err = g.cfg.brightness_init(g.cfg.brightness_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "brightness_init failed: %s", esp_err_to_name(err));
            sleep_deinit();
            return err;
        }
    }

    (void)g.cfg.brightness_set(g.cfg.brightness_ctx, (uint8_t)g.cfg.normal_brightness_pct);

    // start timer
    xSemaphoreTake(g.mu, portMAX_DELAY);
    restart_idle_timer_locked();
    xSemaphoreGive(g.mu);

    ESP_LOGI(TAG, "init: idle=%ums normal=%d%% sleep=%d%%", (unsigned)g.cfg.idle_timeout_ms,
             g.cfg.normal_brightness_pct, g.cfg.sleep_brightness_pct);

    return ESP_OK;
}

esp_err_t sleep_deinit(void)
{
    if (! g.inited || ! g.mu)
        return ESP_OK;

    xSemaphoreTake(g.mu, portMAX_DELAY);

    TimerHandle_t t1     = g.idle_timer;
    TimerHandle_t t2     = g.wake_brightness_timer;
    SemaphoreHandle_t mu = g.mu;

    g.idle_timer = NULL;
    g.wake_brightness_timer = NULL;
    g.inited     = false;
    g.cb         = NULL;
    g.cb_ctx     = NULL;

    // crucial: prevent other threads from using mu after we unlock
    g.mu = NULL;

    xSemaphoreGive(mu);

    if (t1) {
        (void)xTimerStop(t1, 0);
        (void)xTimerDelete(t1, 0);
    }
    if (t2) {
        (void)xTimerStop(t2, 0);
        (void)xTimerDelete(t2, 0);
    }

    vSemaphoreDelete(mu);

    return ESP_OK;
}

void sleep_set_event_cb(sleep_event_cb_t cb, void* ctx)
{
    if (! g.mu) {
        portENTER_CRITICAL(&g_cb_mux);
        g.cb     = cb;
        g.cb_ctx = ctx;
        portEXIT_CRITICAL(&g_cb_mux);
        return;
    }

    xSemaphoreTake(g.mu, portMAX_DELAY);
    g.cb     = cb;
    g.cb_ctx = ctx;
    xSemaphoreGive(g.mu);
}

void sleep_notify_activity(void)
{
    // If sleeping -> WAKE, else just reset timer
    wake_internal(true);
}

void sleep_force_sleep(void)
{
    enter_sleep_internal(false);
}

void sleep_force_wake(void)
{
    wake_internal(false);
}

sleep_state_t sleep_get_state(void)
{
    if (! g.mu)
        return SLEEP_STATE_AWAKE;

    xSemaphoreTake(g.mu, portMAX_DELAY);
    sleep_state_t s = g.state;
    xSemaphoreGive(g.mu);
    return s;
}

uint32_t sleep_get_idle_timeout_ms(void)
{
    if (! g.mu)
        return 0;

    xSemaphoreTake(g.mu, portMAX_DELAY);
    uint32_t v = g.cfg.idle_timeout_ms;
    xSemaphoreGive(g.mu);
    return v;
}

int sleep_get_normal_brightness_pct(void)
{
    if (! g.mu)
        return 0;

    xSemaphoreTake(g.mu, portMAX_DELAY);
    int v = g.cfg.normal_brightness_pct;
    xSemaphoreGive(g.mu);
    return v;
}

int sleep_get_sleep_brightness_pct(void)
{
    if (! g.mu)
        return 0;

    xSemaphoreTake(g.mu, portMAX_DELAY);
    int v = g.cfg.sleep_brightness_pct;
    xSemaphoreGive(g.mu);
    return v;
}
