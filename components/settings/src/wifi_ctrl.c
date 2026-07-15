#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>
#include "settings_events.h"
#include "wifi.h"
#include "wifi_connectivity.h"
#include "wifi_ctrl.h"
#include "settings_config.h"
#include "credentials.h"
#include "config_portal.h"

#define WIFI_MAX_NETWORKS 32
#define WIFI_SSID_BUFFER_LENGTH 33
#define WIFI_RECONNECT_DELAY_MS 2000

typedef struct
{
    bool scanning;
    bool interface_started;
    bool connected;
    bool initialized;
    bool pending_save;
    bool scan_requested;
    uint32_t last_scan_time;
    uint32_t scan_cooldown_ms;

    char network_ssid[WIFI_SSID_BUFFER_LENGTH];
    char network_password[64];

    bool pending_connect;
    bool pending_connect_from_uart_credentials;
    bool uart_password_ready;
    char uart_network_ssid[WIFI_SSID_BUFFER_LENGTH];
    char uart_network_password[64];

    SemaphoreHandle_t mutex;
    TaskHandle_t scan_task_handle;
    uint32_t scan_interval_ms;
    bool auto_start_wifi;
    bool auto_connect_from_saved;
    bool reconnect_suppressed;
    bool ip_lost;
    
    TimerHandle_t save_timer;
    TimerHandle_t reconnect_timer;
    uint32_t last_save_hash;
} wifi_mgr_t;

static wifi_mgr_t s_mgr = {0};
static const char* TAG  = "WiFi ctrl";

static void scanner_task(void* pv);
static void connect_to_network(const char* ssid, const char* password);
static bool set_scanning(bool scanning);
static void save_state_timer_cb(TimerHandle_t xTimer);
static void reconnect_timer_cb(TimerHandle_t xTimer);
static uint32_t calculate_state_hash(void);

static size_t safe_strcpy(char *dest, const char *src, size_t dest_size)
{
    if (!dest || !src || dest_size == 0) return 0;
    
    size_t src_len = strlen(src);
    size_t copy_len = (src_len < dest_size - 1) ? src_len : dest_size - 1;
    
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
    
    return copy_len;
}

static uint32_t calculate_state_hash(void)
{
    uint32_t hash = 0;
    hash ^= (s_mgr.interface_started ? 0x1234 : 0x5678);
    hash ^= (s_mgr.connected ? 0x9ABC : 0xDEF0);
    
    if (s_mgr.connected && strlen(s_mgr.network_ssid) > 0) {
        for (size_t i = 0; i < strlen(s_mgr.network_ssid); i++) {
            hash ^= (s_mgr.network_ssid[i] << ((i % 4) * 8));
        }
    }
    
    return hash;
}

static void save_state_timer_cb(TimerHandle_t xTimer)
{
    if (!s_mgr.pending_save) {
        return;
    }
    
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    
    uint32_t current_hash = calculate_state_hash();
    if (current_hash != s_mgr.last_save_hash) {
        bool wifi_was_enabled = s_mgr.interface_started;
        bool wifi_was_connected = s_mgr.connected;
        
        xSemaphoreGiveRecursive(s_mgr.mutex);
        
        settings_config_save_wifi_enabled(wifi_was_enabled);
        
        if (wifi_was_connected && strlen(s_mgr.network_ssid) > 0) {
            save_wifi_credentials_to_nvs(s_mgr.network_ssid, s_mgr.network_password);
        }
        
        xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
        s_mgr.last_save_hash = current_hash;
        ESP_LOGI(TAG, "WiFi state saved: enabled=%d, connected=%d", wifi_was_enabled, wifi_was_connected);
    }
    
    s_mgr.pending_save = false;
    xSemaphoreGiveRecursive(s_mgr.mutex);
}

/* Reconnect outside the event callback. A short delay also gives DHCP a
 * chance to recover on its own after a transient LOST_IP notification. */
static void reconnect_timer_cb(TimerHandle_t xTimer)
{
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);

    bool recovery_allowed = s_mgr.initialized && s_mgr.interface_started &&
                            !s_mgr.reconnect_suppressed &&
                            !config_portal_is_running();
    bool force_reassociate = recovery_allowed && s_mgr.connected && s_mgr.ip_lost;
    bool reconnect_station = recovery_allowed && !s_mgr.connected &&
                             s_mgr.network_ssid[0] != '\0';

    if (force_reassociate) {
        s_mgr.ip_lost = false;
    }
    xSemaphoreGiveRecursive(s_mgr.mutex);

    esp_err_t result = ESP_OK;
    if (force_reassociate) {
        ESP_LOGW(TAG, "IP was not restored; reassociating WiFi station");
        result = esp_wifi_disconnect();
    } else if (reconnect_station) {
        ESP_LOGI(TAG, "Reconnecting to saved WiFi network");
        result = esp_wifi_connect();
    } else {
        return;
    }

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "WiFi recovery request failed: %s", esp_err_to_name(result));
        xTimerStart(s_mgr.reconnect_timer, 0);
    }
}

void wifi_ctrl_schedule_state_save(void)
{
    if (!s_mgr.initialized || !s_mgr.save_timer) {
        return;
    }
    
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    
    uint32_t current_hash = calculate_state_hash();
    if (current_hash == s_mgr.last_save_hash) {
        xSemaphoreGiveRecursive(s_mgr.mutex);
        return;
    }
    
    s_mgr.pending_save = true;
    xSemaphoreGiveRecursive(s_mgr.mutex);
    
    xTimerStop(s_mgr.save_timer, 0);
    xTimerStart(s_mgr.save_timer, 0);
}

void wifi_ctrl_force_state_save(void)
{
    if (!s_mgr.initialized) {
        return;
    }
    
    if (s_mgr.save_timer) {
        xTimerStop(s_mgr.save_timer, 0);
    }
    
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    
    bool wifi_was_enabled = s_mgr.interface_started;
    bool wifi_was_connected = s_mgr.connected;
    char ssid_copy[33] = {0};
    char pass_copy[65] = {0};
    
    if (wifi_was_connected) {
        safe_strcpy(ssid_copy, s_mgr.network_ssid, sizeof(ssid_copy));
        safe_strcpy(pass_copy, s_mgr.network_password, sizeof(pass_copy));
    }
    
    uint32_t current_hash = calculate_state_hash();
    s_mgr.last_save_hash = current_hash;
    s_mgr.pending_save = false;
    
    xSemaphoreGiveRecursive(s_mgr.mutex);
    
    settings_config_save_wifi_enabled(wifi_was_enabled);
    
    if (wifi_was_connected && strlen(ssid_copy) > 0) {
        save_wifi_credentials_to_nvs(ssid_copy, pass_copy);
    }
    
    ESP_LOGI(TAG, "WiFi state force saved: enabled=%d, connected=%d", wifi_was_enabled, wifi_was_connected);
}

static void post_connect_error(int error, const char* details)
{
    app_settings_wifi_connect_result_data_t payload = {0};
    payload.error                                   = error;
    if (details == NULL) {
        payload.len        = 0;
        payload.details[0] = '\0';
    } else {
        int written = snprintf(payload.details, sizeof(payload.details), "%s", details);

        if (written < 0) {
            payload.len        = 0;
            payload.details[0] = '\0';
        } else {
            payload.len = written;
        }
    }
    esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_CONNECT_RESULT, &payload, sizeof(payload), 0);
}

static void on_wifi_event(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started");
        s_mgr.interface_started = true;
        s_mgr.reconnect_suppressed = false;
        
        if (s_mgr.auto_connect_from_saved && strlen(s_mgr.network_ssid) > 0 && strlen(s_mgr.network_password) > 0) {
            ESP_LOGI(TAG, "Auto-connecting to: %s", s_mgr.network_ssid);
            s_mgr.pending_connect = true;
            credentials_set_pending_ssid(s_mgr.network_ssid);
            xSemaphoreGiveRecursive(s_mgr.mutex);
            wifi_ctrl_try_stop_scanner();
            wifi_connect(s_mgr.network_ssid, s_mgr.network_password);
        } else {
            xSemaphoreGiveRecursive(s_mgr.mutex);
        }
        wifi_ctrl_schedule_state_save();
        break;
    case WIFI_EVENT_STA_STOP:
        ESP_LOGI(TAG, "STA stopped");
        if (s_mgr.reconnect_timer) {
            xTimerStop(s_mgr.reconnect_timer, 0);
        }
        s_mgr.interface_started                     = false;
        s_mgr.connected                             = false;
        s_mgr.ip_lost                               = false;
        s_mgr.pending_connect_from_uart_credentials = false;
        s_mgr.network_ssid[0]                       = '\0';
        s_mgr.network_password[0]                   = '\0';
        set_scanning(false);
        wifi_connectivity_stop();
        xSemaphoreGiveRecursive(s_mgr.mutex);
        wifi_ctrl_schedule_state_save();
        break;
    case WIFI_EVENT_STA_CONNECTED: {
        s_mgr.connected = true;
        s_mgr.ip_lost = false;
        if (s_mgr.reconnect_timer) {
            xTimerStop(s_mgr.reconnect_timer, 0);
        }
        wifi_event_sta_connected_t* event = (wifi_event_sta_connected_t*)event_data;
        ESP_LOGI(TAG, "Connected to AP: %s", event->ssid);
        
        safe_strcpy(s_mgr.network_ssid, (char*)event->ssid, sizeof(s_mgr.network_ssid));
        
        wifi_config_t wifi_config;
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK) {
            if (strlen((char*)wifi_config.sta.password) > 0) {
                safe_strcpy(s_mgr.network_password, (char*)wifi_config.sta.password, 
                        sizeof(s_mgr.network_password));
                ESP_LOGI(TAG, "Saved password from active config (length: %d)", 
                        (int)strlen(s_mgr.network_password));
            }
        }
        
        if (s_mgr.pending_connect_from_uart_credentials) {
            char saved_ssid[33] = {0};
            char saved_pass[65] = {0};
            if (load_wifi_credentials_from_nvs(saved_ssid, sizeof(saved_ssid), 
                                            saved_pass, sizeof(saved_pass)) == ESP_OK) {
                safe_strcpy(s_mgr.uart_network_ssid, saved_ssid, sizeof(s_mgr.uart_network_ssid));
                safe_strcpy(s_mgr.uart_network_password, saved_pass, sizeof(s_mgr.uart_network_password));
                ESP_LOGI(TAG, "Updated uart cache from NVS: ssid=%s", s_mgr.uart_network_ssid);
            }
            s_mgr.pending_connect_from_uart_credentials = false;
        }
        
        wifi_connectivity_start();
        xSemaphoreGiveRecursive(s_mgr.mutex);
        wifi_ctrl_schedule_state_save(); 
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)event_data;
        
        char target_ssid[33] = {0};
        const char* pending_ssid = credentials_get_pending_ssid();
        
        if (strlen((char*)event->ssid) > 0) {
            safe_strcpy(target_ssid, (char*)event->ssid, sizeof(target_ssid));
        } else if (s_mgr.pending_connect && pending_ssid) {
            safe_strcpy(target_ssid, pending_ssid, sizeof(target_ssid));
        } else if (strlen(s_mgr.network_ssid) > 0) {
            safe_strcpy(target_ssid, s_mgr.network_ssid, sizeof(target_ssid));
        } else if (s_mgr.pending_connect_from_uart_credentials && strlen(s_mgr.uart_network_ssid) > 0) {
            safe_strcpy(target_ssid, s_mgr.uart_network_ssid, sizeof(target_ssid));
        }

        bool was_pending_connect = s_mgr.pending_connect;
        bool was_pending_from_uart = s_mgr.pending_connect_from_uart_credentials;
        
        s_mgr.connected                             = false;
        s_mgr.ip_lost                               = false;
        s_mgr.pending_connect_from_uart_credentials = false;
        
        ESP_LOGI(TAG, "Disconnected from AP: %s, reason: %d", target_ssid, event->reason);
        
        if (!config_portal_is_running()) {
            char buffer[64] = {0};
            switch (event->reason) {
            case WIFI_REASON_ASSOC_LEAVE:
            case WIFI_REASON_UNSPECIFIED:
                snprintf(buffer, sizeof(buffer), "Disconnected from %s", target_ssid);
                post_connect_error(event->reason, (const char*)buffer);
                break;
            case WIFI_REASON_AUTH_EXPIRE:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_NO_AP_FOUND:
            case WIFI_REASON_CONNECTION_FAIL:
            default:
                if (was_pending_connect || was_pending_from_uart) {
                    snprintf(buffer, sizeof(buffer), "Failed to connect to %s", target_ssid);
                } else if (!was_pending_from_uart) {
                    snprintf(buffer, sizeof(buffer), "Failed to auto-connect to %s", target_ssid);
                }
                if (buffer[0] != '\0') {
                    post_connect_error(event->reason, (const char*)buffer);
                }
                break;
            }
        }
        
        bool should_reconnect = s_mgr.interface_started &&
                                !s_mgr.reconnect_suppressed &&
                                !config_portal_is_running() &&
                                target_ssid[0] != '\0';

        s_mgr.pending_connect = false;
        if (should_reconnect) {
            safe_strcpy(s_mgr.network_ssid, target_ssid, sizeof(s_mgr.network_ssid));
        } else {
            s_mgr.network_ssid[0]     = '\0';
            s_mgr.network_password[0] = '\0';
        }
        credentials_clear_pending_ssid();

        wifi_connectivity_stop();
        xSemaphoreGiveRecursive(s_mgr.mutex);
        if (should_reconnect && s_mgr.reconnect_timer) {
            ESP_LOGI(TAG, "WiFi reconnect scheduled in %d ms", WIFI_RECONNECT_DELAY_MS);
            xTimerStart(s_mgr.reconnect_timer, 0);
        }
        wifi_ctrl_schedule_state_save();
        break;
    }
    case WIFI_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "Scan completed");
        xSemaphoreGiveRecursive(s_mgr.mutex);
        break;
    default:
        xSemaphoreGiveRecursive(s_mgr.mutex);
        break;
    }
}

static void connectivity_check_async(void* arg)
{
    wifi_connectivity_check_now();
    vTaskDelete(NULL);
}

static void on_ip_event(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        s_mgr.ip_lost = false;
        if (s_mgr.reconnect_timer) {
            xTimerStop(s_mgr.reconnect_timer, 0);
        }
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreGiveRecursive(s_mgr.mutex);
        xTaskCreate(connectivity_check_async, "connectivity_check_async", 4096, NULL, 2, NULL);
        break;
    }
    case IP_EVENT_STA_LOST_IP:
        ESP_LOGW(TAG, "Lost IP address");
        s_mgr.ip_lost = true;
        xSemaphoreGiveRecursive(s_mgr.mutex);
        if (s_mgr.reconnect_timer && !s_mgr.reconnect_suppressed) {
            xTimerStart(s_mgr.reconnect_timer, 0);
        }
        break;
    default:
        xSemaphoreGiveRecursive(s_mgr.mutex);
        break;
    }
}

static void on_ui_request(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    switch (event_id) {
    case APP_SETTINGS_WIFI_EVENT_WIFI_ENABLE_REQ:
        if (event_data) {
            bool enabled = *(bool*)event_data;
            if (enabled) {
                s_mgr.reconnect_suppressed = false;
                if (! s_mgr.interface_started) {
                    esp_wifi_start();
                }
            } else {
                s_mgr.reconnect_suppressed = true;
                if (s_mgr.reconnect_timer) {
                    xTimerStop(s_mgr.reconnect_timer, 0);
                }
                if (s_mgr.interface_started) {
                    set_scanning(false);
                    esp_wifi_stop();
                }
            }
        }
        xSemaphoreGiveRecursive(s_mgr.mutex);
        break;
    case APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ:
        if (event_data) {
            bool enabled = *(bool*)event_data;
            set_scanning(enabled);
        }
        xSemaphoreGiveRecursive(s_mgr.mutex);
        break;
    case APP_SETTINGS_WIFI_EVENT_SET_NETWORK_REQ:
        if (event_data) {
            wifi_ap_record_t* network = (wifi_ap_record_t*)event_data;
            safe_strcpy(s_mgr.network_ssid, (char*)network->ssid, sizeof(s_mgr.network_ssid));
            ESP_LOGI(TAG, "Selected network: ssid=%s", s_mgr.network_ssid);

            app_settings_wifi_password_requested_data_t payload = {0};
            int n = snprintf(payload.ssid, sizeof(payload.ssid), "%s", (char*)network->ssid);
            if (n > 0) {
                payload.len = n;
                esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_PASSWORD_REQUESTED, &payload,
                            sizeof(payload), 0);
            }
        }
        xSemaphoreGiveRecursive(s_mgr.mutex);
        break;
    case APP_SETTINGS_WIFI_EVENT_CONNECT_REQ:
        if (! s_mgr.connected && s_mgr.interface_started) {
            s_mgr.reconnect_suppressed = false;
            if (event_data) {
                app_settings_wifi_connect_data_t* payload = (app_settings_wifi_connect_data_t*)event_data;
                
                safe_strcpy(s_mgr.network_password, payload->password, sizeof(s_mgr.network_password));
                
                if (strlen(s_mgr.network_ssid) > 0 && strlen(s_mgr.network_password) > 0) {
                    s_mgr.pending_connect = true;
                    credentials_set_pending_ssid(s_mgr.network_ssid);
                    
                    wifi_ctrl_try_stop_scanner();
                    esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_CONNECTING, NULL, 0, 0);
                    xSemaphoreGiveRecursive(s_mgr.mutex);
                    connect_to_network((const char*)s_mgr.network_ssid, s_mgr.network_password);
                } else {
                    xSemaphoreGiveRecursive(s_mgr.mutex);
                }
            } else {
                xSemaphoreGiveRecursive(s_mgr.mutex);
            }
        } else {
            ESP_LOGW(TAG, "Already connected to network");
            char buffer[64] = {0};
            snprintf(buffer, sizeof(buffer), "Already connected to network");
            post_connect_error(0, (const char*)buffer);
            xSemaphoreGiveRecursive(s_mgr.mutex);
        }
        break;
    case APP_SETTINGS_WIFI_EVENT_DISCONNECT_REQ:
        if (s_mgr.connected) {
            s_mgr.reconnect_suppressed = true;
            if (s_mgr.reconnect_timer) {
                xTimerStop(s_mgr.reconnect_timer, 0);
            }
            wifi_disconnect();
        }
        xSemaphoreGiveRecursive(s_mgr.mutex);
        break;
    default:
        xSemaphoreGiveRecursive(s_mgr.mutex);
        break;
    }
}

static void on_wifi_connectivity_changed(void* user_ctx, wifi_connectivity_status_t status, void* event_data)
{
    app_settings_wifi_connectivity_status_data_t payload = {0};
    const char* details                                  = wifi_connectivity_get_details();
    payload.status                                       = status;
    if (details == NULL) {
        payload.len        = 0;
        payload.details[0] = '\0';
    } else {
        int written = snprintf(payload.details, sizeof(payload.details), "%s", details);

        if (written < 0) {
            payload.len        = 0;
            payload.details[0] = '\0';
        } else {
            payload.len = written;
        }
    }
    esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_CONNECTIVITY_STATUS_CHANGED, &payload,
                   sizeof(payload), 0);
}

static void on_uart_event(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    switch (event_id) {
    case APP_SETTGINS_UART_EVENT_RECIEVED_WIFI_CRED: {
        if (event_data) {
            app_settings_uart_wifi_cred_data_t* payload = (app_settings_uart_wifi_cred_data_t*)event_data;
            
            xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
            
            safe_strcpy(s_mgr.uart_network_password, payload->password, sizeof(s_mgr.uart_network_password));
            
            if (strlen(payload->ssid) > 0) {
                safe_strcpy(s_mgr.uart_network_ssid, payload->ssid, sizeof(s_mgr.uart_network_ssid));
            }
            
            s_mgr.uart_password_ready = true;
            
            ESP_LOGI(TAG, "WiFi credentials received via UART: ssid=%s", 
                     strlen(payload->ssid) > 0 ? payload->ssid : "will use saved");
            
            const char* ssid_to_use = strlen(payload->ssid) > 0 ? payload->ssid : NULL;
            
            if (!ssid_to_use) {
                char saved_ssid[32] = {0};
                char saved_pass[64] = {0};
                if (load_wifi_credentials_from_nvs(saved_ssid, sizeof(saved_ssid), 
                                                saved_pass, sizeof(saved_pass)) == ESP_OK) {
                    safe_strcpy(s_mgr.uart_network_ssid, saved_ssid, sizeof(s_mgr.uart_network_ssid));
                    ssid_to_use = s_mgr.uart_network_ssid;
                }
            }
            
            if (!ssid_to_use || strlen(ssid_to_use) == 0) {
                ESP_LOGW(TAG, "No SSID to connect to");
                xSemaphoreGiveRecursive(s_mgr.mutex);
                break;
            }
            
            bool was_connected = s_mgr.connected;
            xSemaphoreGiveRecursive(s_mgr.mutex);
            
            if (was_connected) {
                ESP_LOGI(TAG, "Disconnecting from current network");
                xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
                s_mgr.reconnect_suppressed = true;
                xSemaphoreGiveRecursive(s_mgr.mutex);
                wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            
            xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
            
            wifi_config_t wifi_config = {0};
            esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            
            s_mgr.pending_connect = true;
            s_mgr.pending_connect_from_uart_credentials = true;
            s_mgr.reconnect_suppressed = false;
            credentials_set_pending_ssid(ssid_to_use);
            
            xSemaphoreGiveRecursive(s_mgr.mutex);
            
            connect_to_network(ssid_to_use, payload->password);
        }
    } break;
    default:
        break;
    }
}

static void scanner_task(void* pv)
{
    ESP_LOGI(TAG, "Scan task started");

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        bool should_scan = false;
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
        
        if (s_mgr.interface_started && s_mgr.scan_requested) {
            if ((current_time - s_mgr.last_scan_time) >= s_mgr.scan_cooldown_ms) {
                should_scan = true;
                s_mgr.scanning = true;
            }
        }
        
        xSemaphoreGiveRecursive(s_mgr.mutex);

        if (!should_scan) {
            continue;
        }

        ESP_LOGI(TAG, "Starting WiFi scan");
        
        wifi_scan_config_t scan_conf = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time = {.active = {.min = 100, .max = 300}}
        };

        esp_err_t ret = esp_wifi_scan_start(&scan_conf, false);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "WiFi scan started");
            
            xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
            s_mgr.last_scan_time = current_time;
            xSemaphoreGiveRecursive(s_mgr.mutex);
            
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        } else {
            ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
        s_mgr.scanning = false;
        
        if (s_mgr.scan_requested) {
            xSemaphoreGiveRecursive(s_mgr.mutex);
            xTaskNotifyGive(s_mgr.scan_task_handle);
        } else {
            xSemaphoreGiveRecursive(s_mgr.mutex);
        }
    }
}

static bool set_scanning(bool scanning)
{
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    
    if (scanning) {
        if (!s_mgr.interface_started) {
            ESP_LOGW(TAG, "Interface is not started yet");
            s_mgr.scan_requested = true;
            xSemaphoreGiveRecursive(s_mgr.mutex);
            return false;
        }
        if (s_mgr.scan_task_handle == NULL) {
            ESP_LOGE(TAG, "Scan task not initialized");
            xSemaphoreGiveRecursive(s_mgr.mutex);
            return false;
        }
        
        s_mgr.scan_requested = true;
        
        if (s_mgr.scanning) {
            ESP_LOGW(TAG, "Already scanning, request queued");
            xSemaphoreGiveRecursive(s_mgr.mutex);
            return true;
        }
        
        xSemaphoreGiveRecursive(s_mgr.mutex);

        ESP_LOGI(TAG, "Enable scanning");
        if (xTaskNotifyGive(s_mgr.scan_task_handle) == pdFAIL) {
            ESP_LOGE(TAG, "Failed to notify scan task");
            return false;
        }
        return true;
    } else {
        bool was_scanning = s_mgr.scanning;
        s_mgr.scan_requested = false;
        
        if (was_scanning) {
            ESP_LOGI(TAG, "Disable scanning");
            xSemaphoreGiveRecursive(s_mgr.mutex);
            esp_wifi_scan_stop();
            
            int timeout = 10;
            while (timeout-- > 0) {
                xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
                bool still_scanning = s_mgr.scanning;
                xSemaphoreGiveRecursive(s_mgr.mutex);
                if (!still_scanning) break;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        } else {
            xSemaphoreGiveRecursive(s_mgr.mutex);
        }
        return was_scanning;
    }
}

static void stop_scanner_async(void *arg)
{
    bool was_scanning = set_scanning(false);
    
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    bool is_scanning = s_mgr.scanning;
    xSemaphoreGiveRecursive(s_mgr.mutex);
    
    if (was_scanning) {
        app_settings_wifi_scan_state_data_t payload = {.is_enabled = is_scanning};
        esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SCAN_STATE_CHANGED, &payload, sizeof(payload), 0);
    }
    
    vTaskDelete(NULL);
}

void wifi_ctrl_try_stop_scanner_async(void)
{
    if (!s_mgr.initialized) return;
    
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    bool is_scanning = s_mgr.scanning || s_mgr.scan_requested;
    xSemaphoreGiveRecursive(s_mgr.mutex);
    
    if (is_scanning) {
        xTaskCreate(stop_scanner_async, "stop_scan_async", 2048, NULL, 5, NULL);
    }
}

void wifi_ctrl_init()
{
    if (s_mgr.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return;
    }

    wifi_init();

    if (! wifi_connectivity_init()) {
        ESP_LOGE(TAG, "Failed to initialize WiFi connectivity manager");
    }
    wifi_connectivity_register_callback(on_wifi_connectivity_changed, NULL);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(APP_SETTINGS_WIFI_EVENTS, ESP_EVENT_ANY_ID, on_ui_request, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        APP_SETTINGS_UART_EVENTS, APP_SETTGINS_UART_EVENT_RECIEVED_WIFI_CRED, on_uart_event, NULL, NULL));

    s_mgr.mutex = xSemaphoreCreateRecursiveMutex();
    if (! s_mgr.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    xTaskCreate(scanner_task, "wifi_scan_task", 4096, NULL, 1, &s_mgr.scan_task_handle);

    if (s_mgr.scan_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create scan task");
        return;
    }

    s_mgr.save_timer = xTimerCreate("wifi_save_timer", pdMS_TO_TICKS(3000), pdFALSE, NULL, save_state_timer_cb);
    s_mgr.reconnect_timer = xTimerCreate("wifi_reconnect_timer", pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS),
                                        pdFALSE, NULL, reconnect_timer_cb);
    
    s_mgr.scanning                              = false;
    s_mgr.scan_requested                        = false;
    s_mgr.interface_started                     = false;
    s_mgr.connected                             = false;
    s_mgr.pending_connect                       = false;
    s_mgr.pending_connect_from_uart_credentials = false;
    s_mgr.uart_password_ready                   = false;
    s_mgr.auto_start_wifi                        = false;
    s_mgr.auto_connect_from_saved                = false;
    s_mgr.reconnect_suppressed                   = false;
    s_mgr.ip_lost                                = false;
    s_mgr.initialized                           = true;
    s_mgr.scan_interval_ms                      = 10000;
    s_mgr.scan_cooldown_ms                      = 3000;
    s_mgr.last_scan_time                        = 0;
    s_mgr.pending_save                          = false;
    s_mgr.last_save_hash                        = 0;

    wifi_ctrl_load_state();

    ESP_LOGI(TAG, "WiFi Controller initialized");
    
    if (s_mgr.auto_start_wifi) {
        bool enable = true;
        esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_WIFI_ENABLE_REQ, 
                    &enable, sizeof(enable), 0);
    }
}

void wifi_ctrl_deinit()
{
    if (! s_mgr.initialized) {
        return;
    }

    ESP_LOGI(TAG, "Deinitializing WiFi Controller");

    set_scanning(false);

    if (s_mgr.save_timer) {
        xTimerStop(s_mgr.save_timer, 0);
        xTimerDelete(s_mgr.save_timer, 0);
    }

    if (s_mgr.reconnect_timer) {
        xTimerStop(s_mgr.reconnect_timer, 0);
        xTimerDelete(s_mgr.reconnect_timer, 0);
        s_mgr.reconnect_timer = NULL;
    }

    if (s_mgr.scan_task_handle) {
        vTaskDelete(s_mgr.scan_task_handle);
        s_mgr.scan_task_handle = NULL;
    }

    if (s_mgr.mutex) {
        vSemaphoreDelete(s_mgr.mutex);
    }

    wifi_deinit();

    s_mgr.initialized = false;
}

void wifi_ctrl_try_stop_scanner()
{
    bool stopped = set_scanning(false);
    if (stopped) {
        app_settings_wifi_scan_state_data_t payload = {.is_enabled = s_mgr.scanning};
        esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SCAN_STATE_CHANGED, &payload, sizeof(payload),
                       0);
    }
}

bool wifi_ctrl_is_connected()
{
    if (s_mgr.initialized) {
        return s_mgr.connected;
    }
    return false;
}

static void connect_to_network(const char* ssid, const char* password)
{
    wifi_ctrl_try_stop_scanner();
    wifi_connect(ssid, password);
}

void wifi_ctrl_save_state(void)
{
    wifi_ctrl_force_state_save();
}

void wifi_ctrl_load_state(void)
{
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    
    bool was_enabled = settings_config_load_wifi_enabled();
    
    if (was_enabled) {
        char saved_ssid[33] = {0};
        char saved_pass[65] = {0};
        
        if (load_wifi_credentials_from_nvs(saved_ssid, sizeof(saved_ssid), 
                                           saved_pass, sizeof(saved_pass)) == ESP_OK) {
            ESP_LOGI(TAG, "Found saved WiFi credentials for: %s", saved_ssid);
            
            safe_strcpy(s_mgr.uart_network_ssid, saved_ssid, sizeof(s_mgr.uart_network_ssid));
            safe_strcpy(s_mgr.uart_network_password, saved_pass, sizeof(s_mgr.uart_network_password));
            s_mgr.uart_password_ready = true;
            s_mgr.auto_connect_from_saved = true;
            s_mgr.auto_start_wifi = true;
            
            safe_strcpy(s_mgr.network_ssid, saved_ssid, sizeof(s_mgr.network_ssid));
            safe_strcpy(s_mgr.network_password, saved_pass, sizeof(s_mgr.network_password));
        } else {
            s_mgr.auto_start_wifi = true;
            ESP_LOGI(TAG, "WiFi was enabled but no saved credentials found");
        }
    } else {
        s_mgr.auto_start_wifi = true;
        ESP_LOGI(TAG, "First boot, enabling WiFi");
    }
    
    xSemaphoreGiveRecursive(s_mgr.mutex);
    
    if (s_mgr.auto_start_wifi) {
        bool enable = true;
        esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_WIFI_ENABLE_REQ, 
                      &enable, sizeof(enable), 0);
    }
}

bool wifi_ctrl_was_auto_started(void)
{
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    bool was = s_mgr.auto_start_wifi;
    xSemaphoreGiveRecursive(s_mgr.mutex);
    return was;
}

bool wifi_ctrl_has_saved_credentials(void)
{
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
    bool has = s_mgr.auto_connect_from_saved;
    xSemaphoreGiveRecursive(s_mgr.mutex);
    return has;
}
