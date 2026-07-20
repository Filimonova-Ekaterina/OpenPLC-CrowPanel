#include "wifi_connectivity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"

#include <string.h>

#include "wifi_ctrl.h"

static const char* TAG = "wifi_connectivity";

#define DEFAULT_CHECK_INTERVAL_MS 60000
#define PING_TIMEOUT_MS           5000
#define RSSI_WEAK_THRESHOLD       -80

typedef struct
{
    wifi_connectivity_status_t current_status;
    const char* detail;
    bool initialized;
    uint32_t check_interval_ms;
    uint32_t last_check_time;
    bool is_checking;
    bool is_running;
    bool active_ping_enabled;

    SemaphoreHandle_t ping_semaphore;
    SemaphoreHandle_t ping_mutex;
    esp_ping_handle_t ping_handle;
    bool ping_active;
    uint32_t ping_replies;

    wifi_connectivity_callback_t status_callback;
    void* status_callback_ctx;

    TaskHandle_t check_task_handle;
} connectivity_ctx_t;

static connectivity_ctx_t s_ctx = {0};

static void on_ping_success(esp_ping_handle_t hdl, void* args);
static void on_ping_timeout(esp_ping_handle_t hdl, void* args);
static void on_ping_end(esp_ping_handle_t hdl, void* args);
static bool perform_ping_test(const char* target_ip);
static void cleanup_ping(void);
static bool has_ip_address(void);
static void update_status(wifi_connectivity_status_t new_status, const char* detail);
static const char* status_to_str(wifi_connectivity_status_t status);
static void connectivity_task(void* pv);

bool wifi_connectivity_init(void)
{
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.current_status    = WIFI_CONNECTIVITY_UNKNOWN;
    s_ctx.check_interval_ms = DEFAULT_CHECK_INTERVAL_MS;
    s_ctx.active_ping_enabled = false;

    s_ctx.ping_semaphore = xSemaphoreCreateBinary();
    s_ctx.ping_mutex     = xSemaphoreCreateMutex();

    if (! s_ctx.ping_semaphore || ! s_ctx.ping_mutex) {
        ESP_LOGE(TAG, "Failed to create semaphores");
        if (s_ctx.ping_semaphore)
            vSemaphoreDelete(s_ctx.ping_semaphore);
        if (s_ctx.ping_mutex)
            vSemaphoreDelete(s_ctx.ping_mutex);
        return false;
    }

    xTaskCreate(connectivity_task, "wifi_conn_task", 4096, NULL, 1, &s_ctx.check_task_handle);
    if (! s_ctx.check_task_handle) {
        ESP_LOGE(TAG, "Failed to create connectivity task");
        vSemaphoreDelete(s_ctx.ping_semaphore);
        vSemaphoreDelete(s_ctx.ping_mutex);
        return false;
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "WiFi connectivity checker initialized");
    return true;
}

void wifi_connectivity_deinit(void)
{
    if (! s_ctx.initialized) {
        return;
    }

    wifi_connectivity_stop();

    if (s_ctx.check_task_handle) {
        vTaskDelete(s_ctx.check_task_handle);
        s_ctx.check_task_handle = NULL;
    }

    if (s_ctx.ping_mutex) {
        xSemaphoreTake(s_ctx.ping_mutex, portMAX_DELAY);
        cleanup_ping();
        xSemaphoreGive(s_ctx.ping_mutex);
        vSemaphoreDelete(s_ctx.ping_mutex);
        s_ctx.ping_mutex = NULL;
    }

    if (s_ctx.ping_semaphore) {
        vSemaphoreDelete(s_ctx.ping_semaphore);
        s_ctx.ping_semaphore = NULL;
    }

    s_ctx.status_callback     = NULL;
    s_ctx.status_callback_ctx = NULL;
    s_ctx.initialized         = false;

    ESP_LOGI(TAG, "WiFi connectivity checker deinitialized");
}

bool wifi_connectivity_start(void)
{
    if (! s_ctx.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return false;
    }

    if (s_ctx.is_running) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    s_ctx.is_running = true;

    xTaskNotifyGive(s_ctx.check_task_handle);

    ESP_LOGI(TAG, "WiFi connectivity checking started (interval: %lu ms)", s_ctx.check_interval_ms);
    return true;
}

void wifi_connectivity_stop(void)
{
    if (! s_ctx.initialized || ! s_ctx.is_running) {
        return;
    }

    ESP_LOGI(TAG, "Stopping connectivity checker...");
    s_ctx.is_running = false;

    if (s_ctx.ping_mutex) {
        if (xSemaphoreTake(s_ctx.ping_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            cleanup_ping();
            xSemaphoreGive(s_ctx.ping_mutex);
        } else {
            ESP_LOGE(TAG, "Failed to take ping mutex during stop");
        }
    }

    if (s_ctx.check_task_handle) {
        xTaskNotifyGive(s_ctx.check_task_handle);
    }

    ESP_LOGI(TAG, "WiFi connectivity checking stopped");
}

void wifi_connectivity_set_check_interval(uint32_t interval_ms)
{
    if (! s_ctx.initialized) {
        return;
    }
    s_ctx.check_interval_ms = (interval_ms > 0) ? interval_ms : DEFAULT_CHECK_INTERVAL_MS;
    ESP_LOGI(TAG, "Check interval set to: %lu ms", s_ctx.check_interval_ms);
}

static bool has_ip_address(void)
{
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (! sta_netif) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta_netif, &ip_info) != ESP_OK) {
        return false;
    }

    return (ip_info.ip.addr != 0);
}

static void on_ping_success(esp_ping_handle_t hdl, void* args)
{
    uint16_t seqno;
    uint32_t elapsed;
    ip_addr_t target;

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));

    ESP_LOGD(TAG, "Ping reply from %s seq=%d time=%dms", inet_ntoa(target.u_addr.ip4), seqno, elapsed);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void* args)
{
    uint16_t seqno;
    ip_addr_t target;

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));

    ESP_LOGD(TAG, "Ping timeout from %s seq=%d", inet_ntoa(target.u_addr.ip4), seqno);
}

static void on_ping_end(esp_ping_handle_t hdl, void* args)
{
    uint32_t transmitted;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &s_ctx.ping_replies, sizeof(s_ctx.ping_replies));

    ESP_LOGI(TAG, "Ping complete: %lu sent, %lu received", transmitted, s_ctx.ping_replies);
    xSemaphoreGive(s_ctx.ping_semaphore);
}

static void cleanup_ping(void)
{
    if (s_ctx.ping_active && s_ctx.ping_handle) {
        ESP_LOGD(TAG, "Cleaning up ping session");
        esp_ping_stop(s_ctx.ping_handle);
        esp_ping_delete_session(s_ctx.ping_handle);
        s_ctx.ping_handle = NULL;
        s_ctx.ping_active = false;
        xSemaphoreGive(s_ctx.ping_semaphore);
    }
}

static bool perform_ping_test(const char* target_ip)
{
    if (xSemaphoreTake(s_ctx.ping_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take ping mutex");
        return false;
    }

    cleanup_ping();
    vTaskDelay(pdMS_TO_TICKS(50)); // Brief delay for cleanup

    ip_addr_t target_addr;
    target_addr.type            = IPADDR_TYPE_V4;
    target_addr.u_addr.ip4.addr = inet_addr(target_ip);

    if (target_addr.u_addr.ip4.addr == 0 || target_addr.u_addr.ip4.addr == 0xFFFFFFFF) {
        ESP_LOGE(TAG, "Invalid ping target: %s", target_ip);
        xSemaphoreGive(s_ctx.ping_mutex);
        return false;
    }

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr       = target_addr;
    ping_config.count             = 2;
    ping_config.interval_ms       = 1000;
    ping_config.timeout_ms        = 2500;
    ping_config.data_size         = 32;
    ping_config.task_stack_size   = 4096;
    ping_config.task_prio         = 1;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout, .on_ping_end = on_ping_end};

    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &s_ctx.ping_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ping session: %s", esp_err_to_name(err));
        xSemaphoreGive(s_ctx.ping_mutex);
        return false;
    }

    s_ctx.ping_active  = true;
    s_ctx.ping_replies = 0;
    xSemaphoreTake(s_ctx.ping_semaphore, 0); // Clear any pending signal

    err = esp_ping_start(s_ctx.ping_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ping: %s", esp_err_to_name(err));
        cleanup_ping();
        xSemaphoreGive(s_ctx.ping_mutex);
        return false;
    }

    xSemaphoreGive(s_ctx.ping_mutex);

    bool success = false;
    if (xSemaphoreTake(s_ctx.ping_semaphore, pdMS_TO_TICKS(PING_TIMEOUT_MS)) == pdTRUE) {
        success = (s_ctx.ping_replies > 0);
        if (xSemaphoreTake(s_ctx.ping_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            cleanup_ping();
            xSemaphoreGive(s_ctx.ping_mutex);
        }
    } else {
        ESP_LOGW(TAG, "Ping timed out");
        if (xSemaphoreTake(s_ctx.ping_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            cleanup_ping();
            xSemaphoreGive(s_ctx.ping_mutex);
        }
    }

    return success;
}

static void update_status(wifi_connectivity_status_t new_status, const char* detail)
{
    if (new_status != s_ctx.current_status) {
        ESP_LOGI(TAG, "Status: %s -> %s", status_to_str(s_ctx.current_status), status_to_str(new_status));
        s_ctx.current_status = new_status;

        if (detail) {
            s_ctx.detail = detail;
        }

        if (s_ctx.status_callback) {
            s_ctx.status_callback(s_ctx.status_callback_ctx, new_status, NULL);
        }
    }
}

wifi_connectivity_status_t wifi_connectivity_check_now(void)
{
    if (! s_ctx.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return WIFI_CONNECTIVITY_UNKNOWN;
    }

    if (s_ctx.is_checking) {
        ESP_LOGW(TAG, "Check already in progress");
        return s_ctx.current_status;
    }

    // Must be connected to check connectivity
    if (! wifi_ctrl_is_connected()) {
        update_status(WIFI_CONNECTIVITY_NO_INTERNET, "Not connected");
        return WIFI_CONNECTIVITY_NO_INTERNET;
    }

    s_ctx.is_checking     = true;
    s_ctx.last_check_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    ESP_LOGI(TAG, "Starting connectivity check");

    wifi_connectivity_status_t result;
    const char* detail = NULL;

    /* The HMI only requires LAN reachability. Active ICMP probes add avoidable
     * traffic on the ESP32-P4 SDIO Wi-Fi link, so they stay disabled by
     * default. Association, DHCP state, and RSSI are sufficient here. */
    if (! has_ip_address()) {
        result = WIFI_CONNECTIVITY_NO_INTERNET;
        detail = "Connected (waiting for IP)";
    } else {
        bool ping_ok = true;
        if (s_ctx.active_ping_enabled) {
            ping_ok = perform_ping_test("8.8.8.8");
        }

        if (ping_ok) {
            int rssi = wifi_connectivity_get_rssi();
            if (rssi <= RSSI_WEAK_THRESHOLD) {
                result = WIFI_CONNECTIVITY_WEAK_SIGNAL;
                detail = "Connected (weak signal)";
            } else {
                result = WIFI_CONNECTIVITY_OK;
                detail = "Connected with LAN access";
            }
        } else {
            // Ping failed - try gateway to determine if router or ISP problem
            if (has_ip_address()) {
                result = WIFI_CONNECTIVITY_ISP_PROBLEM;
                detail = "ISP problem (IP, but no internet)";
            } else {
                result = WIFI_CONNECTIVITY_ROUTER_PROBLEM;
                detail = "Router problem (no IP)";
            }
        }
    }

    update_status(result, detail);
    s_ctx.is_checking = false;

    return result;
}

wifi_connectivity_status_t wifi_connectivity_get_status(void)
{
    return s_ctx.current_status;
}

int wifi_connectivity_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

const char* wifi_connectivity_get_details(void)
{
    return s_ctx.detail;
}

bool wifi_connectivity_has_internet(void)
{
    return (s_ctx.current_status == WIFI_CONNECTIVITY_OK || s_ctx.current_status == WIFI_CONNECTIVITY_WEAK_SIGNAL);
}

void wifi_connectivity_register_callback(wifi_connectivity_callback_t callback, void* user_ctx)
{
    s_ctx.status_callback     = callback;
    s_ctx.status_callback_ctx = user_ctx;
}

static const char* status_to_str(wifi_connectivity_status_t status)
{
    switch (status) {
    case WIFI_CONNECTIVITY_UNKNOWN:
        return "WIFI_CONNECTIVITY_UNKNOWN";
    case WIFI_CONNECTIVITY_NO_INTERNET:
        return "WIFI_CONNECTIVITY_NO_INTERNET";
    case WIFI_CONNECTIVITY_OK:
        return "WIFI_CONNECTIVITY_OK";
    case WIFI_CONNECTIVITY_WEAK_SIGNAL:
        return "WIFI_CONNECTIVITY_WEAK_SIGNAL";
    case WIFI_CONNECTIVITY_ROUTER_PROBLEM:
        return "WIFI_CONNECTIVITY_ROUTER_PROBLEM";
    case WIFI_CONNECTIVITY_ISP_PROBLEM:
        return "WIFI_CONNECTIVITY_ISP_PROBLEM";
    default:
        return "";
    }
}

static void connectivity_task(void* pv)
{
    while (1) {
        if (! s_ctx.is_running) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        wifi_connectivity_check_now();

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_ctx.check_interval_ms));
    }
}
