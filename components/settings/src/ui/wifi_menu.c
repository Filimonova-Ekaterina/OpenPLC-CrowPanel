#include "esp_log.h"
#include "config_portal.h"
#include "message_box.h"
#include "password_dialog.h"
#include "settings_events.h"
#include "wifi_menu.h"
#include "wifi_connectivity.h"
#include "wifi_ctrl.h"
#include "credentials.h"
#include "settings_config.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WIFI_MAX_NETWORKS 32

typedef struct wifi_qr_block_t wifi_qr_block_t;

static void qr_update_timer_cb(lv_timer_t* timer);
static void display_scan_data(void* arg);
static void update_cred_status(wifi_qr_block_t* qr_block);
static void update_qr_display(wifi_qr_block_t* qr_block);
static void start_portal_if_needed(void);
static void on_wifi_event(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void on_wifi_internal_event(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void on_uart_wifi_cred_event(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void on_connecting(void* arg);
static void on_disconnected(void* arg);
static void on_connectivity_update(void* arg);
static void _password_dialog_create(void* arg);
static void _message_box_create(void* arg);
static void load_current_wifi_password(char* password, size_t password_len);
static bool wifi_menu_active = false;
static SemaphoreHandle_t s_wifi_menu_mutex = NULL;

typedef struct
{
    lv_obj_t* cont;
    lv_obj_t* list;
    lv_obj_t* scanning_label;
} scan_area_t;

typedef struct
{
    lv_obj_t* cont;
    lv_obj_t* ssid_label;
    lv_obj_t* pass_label;
    lv_obj_t* show_pass_btn;
    lv_obj_t* show_pass_label;
    lv_obj_t* status_label;
    lv_obj_t* disconnect_btn;
    bool pass_visible;
    char current_pass[65];
} current_connection_t;

typedef struct wifi_qr_block_t
{
    lv_obj_t* cont;
    lv_obj_t* left_side;
    lv_obj_t* right_side;
    lv_obj_t* wifi_qr_col;
    lv_obj_t* url_qr_col;
    lv_obj_t* wifi_qr_frame;
    lv_obj_t* url_qr_frame;
    lv_obj_t* wifi_label;
    lv_obj_t* url_label;
    lv_obj_t* wifi_info_label;
    lv_obj_t* wifi_qr_obj;
    lv_obj_t* url_qr_obj;
    lv_obj_t* cred_status_label;
    lv_timer_t* update_timer;
    lv_obj_t* scan_info_label; 
} wifi_qr_block_t;

typedef struct
{
    lv_obj_t* cont;
    lv_obj_t* top_layer;
    current_connection_t current_connection;
    scan_area_t scan_area;
    wifi_qr_block_t qr_block;

    wifi_ap_record_t ap_records_pool[WIFI_MAX_NETWORKS];
    size_t ap_records_count;
    bool scan_in_progress;
    bool scan_done;
    bool has_cached_results;
    bool portal_running;
    esp_event_handler_instance_t wifi_event_handler;
    esp_event_handler_instance_t app_wifi_event_handler;
    esp_event_handler_instance_t uart_event_handler;
} wifi_menu_t;

static wifi_menu_t s_wifi_menu = {0};
static const char* TAG = "WiFi menu";

static lv_style_t style_status_ok;
static lv_style_t style_status_err;
static bool styles_initialized = false;

static void load_current_wifi_password(char* password, size_t password_len)
{
    if (!password || password_len == 0) return;

    password[0] = '\0';
    wifi_config_t active_config = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &active_config) == ESP_OK) {
        strlcpy(password, (const char*)active_config.sta.password, password_len);
        return;
    }

    char saved_ssid[33] = {0};
    load_wifi_credentials_from_nvs(saved_ssid, sizeof(saved_ssid), password, password_len);
}

static void init_styles(void)
{
    if (styles_initialized) return;
    
    lv_style_init(&style_status_ok);
    lv_style_set_bg_color(&style_status_ok, lv_color_hex(0x198754));
    lv_style_set_bg_opa(&style_status_ok, LV_OPA_COVER);
    lv_style_set_radius(&style_status_ok, 12);
    lv_style_set_pad_hor(&style_status_ok, 12);
    lv_style_set_pad_ver(&style_status_ok, 6);
    lv_style_set_text_color(&style_status_ok, lv_color_white());
    lv_style_set_text_font(&style_status_ok, &lv_font_montserrat_14);
    
    lv_style_init(&style_status_err);
    lv_style_set_bg_color(&style_status_err, lv_palette_darken(LV_PALETTE_GREY, 3));
    lv_style_set_bg_opa(&style_status_err, LV_OPA_COVER);
    lv_style_set_radius(&style_status_err, 12);
    lv_style_set_pad_hor(&style_status_err, 12);
    lv_style_set_pad_ver(&style_status_err, 6);
    lv_style_set_text_color(&style_status_err, lv_color_white());
    lv_style_set_text_font(&style_status_err, &lv_font_montserrat_14);
    
    styles_initialized = true;
}

static void start_portal_if_needed(void)
{
    if (!wifi_menu_active) return;
    if (!s_wifi_menu.scan_done) return;
    if (wifi_ctrl_is_connected()) return;
    
    if (s_wifi_menu.portal_running) {
        config_portal_stop();
        s_wifi_menu.portal_running = false;
    }
    
    config_portal_start();
    s_wifi_menu.portal_running = true;
    
    if (s_wifi_menu.qr_block.update_timer) {
        lv_timer_del(s_wifi_menu.qr_block.update_timer);
    }
    s_wifi_menu.qr_block.update_timer = lv_timer_create(qr_update_timer_cb, 750, &s_wifi_menu.qr_block);
}

void wifi_menu_activate(void)
{
    if (!s_wifi_menu_mutex) return;
    
    xSemaphoreTake(s_wifi_menu_mutex, portMAX_DELAY);
    
    if (wifi_menu_active) {
        xSemaphoreGive(s_wifi_menu_mutex);
        return;
    }
    
    wifi_menu_active = true;
    ESP_LOGI(TAG, "WiFi menu activated");
    
    /* Перерегистрируем обработчики событий */
    if (!s_wifi_menu.app_wifi_event_handler) {
        esp_event_handler_instance_register(APP_SETTINGS_WIFI_EVENTS, ESP_EVENT_ANY_ID, on_wifi_event, NULL, 
                                           &s_wifi_menu.app_wifi_event_handler);
    }
    if (!s_wifi_menu.wifi_event_handler) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_internal_event, NULL,
                                           &s_wifi_menu.wifi_event_handler);
    }
    if (!s_wifi_menu.uart_event_handler) {
        esp_event_handler_instance_register(APP_SETTINGS_UART_EVENTS, APP_SETTGINS_UART_EVENT_RECIEVED_WIFI_CRED,
                                            on_uart_wifi_cred_event, NULL, &s_wifi_menu.uart_event_handler);
    }
    
    if (wifi_ctrl_is_connected()) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            char ssid[33];
            snprintf(ssid, sizeof(ssid), "%s", ap_info.ssid);
            if (s_wifi_menu.current_connection.ssid_label && lv_obj_is_valid(s_wifi_menu.current_connection.ssid_label)) {
                lv_label_set_text_fmt(s_wifi_menu.current_connection.ssid_label, "Connected: %s", ssid);
            }
            
            if (s_wifi_menu.current_connection.current_pass[0] == '\0') {
                char wifi_pass[65] = {0};
                load_current_wifi_password(wifi_pass, sizeof(wifi_pass));
                strlcpy(s_wifi_menu.current_connection.current_pass, wifi_pass, 65);
            }
            
            if (s_wifi_menu.current_connection.pass_label && lv_obj_is_valid(s_wifi_menu.current_connection.pass_label)) {
                if (strlen(s_wifi_menu.current_connection.current_pass) > 0) {
                    size_t len = strlen(s_wifi_menu.current_connection.current_pass);
                    char masked[65] = {0};
                    for (size_t i = 0; i < len && i < 60; i++) masked[i] = '*';
                    masked[len > 60 ? 60 : len] = '\0';
                    lv_label_set_text_fmt(s_wifi_menu.current_connection.pass_label, "Password: %s", masked);
                } else {
                    lv_label_set_text(s_wifi_menu.current_connection.pass_label, "Password: Open");
                }
                lv_obj_clear_flag(s_wifi_menu.current_connection.pass_label, LV_OBJ_FLAG_HIDDEN);
            }
            if (s_wifi_menu.current_connection.show_pass_btn) lv_obj_clear_flag(s_wifi_menu.current_connection.show_pass_btn, LV_OBJ_FLAG_HIDDEN);
            if (s_wifi_menu.current_connection.disconnect_btn) lv_obj_clear_flag(s_wifi_menu.current_connection.disconnect_btn, LV_OBJ_FLAG_HIDDEN);
            
            const char* details = wifi_connectivity_get_details();
            if (s_wifi_menu.current_connection.status_label && lv_obj_is_valid(s_wifi_menu.current_connection.status_label)) {
                if (details && strlen(details) > 0) {
                    lv_label_set_text_fmt(s_wifi_menu.current_connection.status_label, "Status: %s", details);
                } else {
                    lv_label_set_text(s_wifi_menu.current_connection.status_label, "");
                }
            }
            
            update_cred_status(&s_wifi_menu.qr_block);
            update_qr_display(&s_wifi_menu.qr_block);
        }
    } else {
        if (s_wifi_menu.current_connection.ssid_label && lv_obj_is_valid(s_wifi_menu.current_connection.ssid_label)) {
            lv_label_set_text(s_wifi_menu.current_connection.ssid_label, "Not connected");
        }
        if (s_wifi_menu.current_connection.pass_label) lv_obj_add_flag(s_wifi_menu.current_connection.pass_label, LV_OBJ_FLAG_HIDDEN);
        if (s_wifi_menu.current_connection.show_pass_btn) lv_obj_add_flag(s_wifi_menu.current_connection.show_pass_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_wifi_menu.current_connection.disconnect_btn) lv_obj_add_flag(s_wifi_menu.current_connection.disconnect_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_wifi_menu.current_connection.status_label && lv_obj_is_valid(s_wifi_menu.current_connection.status_label)) {
            lv_label_set_text(s_wifi_menu.current_connection.status_label, "");
        }
    }
    
    if (!s_wifi_menu.has_cached_results) {
        s_wifi_menu.scan_in_progress = false;
        s_wifi_menu.scan_done = false;
        s_wifi_menu.scan_in_progress = true;
        
        if (s_wifi_menu.scan_area.list && lv_obj_is_valid(s_wifi_menu.scan_area.list)) {
            lv_obj_clean(s_wifi_menu.scan_area.list);
        }
        s_wifi_menu.ap_records_count = 0;
        
        if (s_wifi_menu.scan_area.scanning_label) {
            lv_obj_clear_flag(s_wifi_menu.scan_area.scanning_label, LV_OBJ_FLAG_HIDDEN);
        }
        
        app_settings_wifi_scan_enable_data_t payload_disable = {.enable = false};
        esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ, 
                       &payload_disable, sizeof(payload_disable), 0);
        
        app_settings_wifi_scan_enable_data_t payload = {.enable = true};
        esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ, 
                       &payload, sizeof(payload), 0);
    } else {
        lv_async_call(display_scan_data, NULL);
    }
    
    xSemaphoreGive(s_wifi_menu_mutex);
}

void wifi_menu_deactivate(void)
{
    if (!s_wifi_menu_mutex) return;
    
    xSemaphoreTake(s_wifi_menu_mutex, portMAX_DELAY);
    
    if (!wifi_menu_active) {
        xSemaphoreGive(s_wifi_menu_mutex);
        return;
    }
    
    wifi_menu_active = false;
    ESP_LOGI(TAG, "WiFi menu deactivated");

    lv_async_call_cancel(display_scan_data, NULL);
    lv_async_call_cancel(on_connecting, NULL);
    lv_async_call_cancel(on_disconnected, NULL);
    lv_async_call_cancel(on_connectivity_update, NULL);
    lv_async_call_cancel(_password_dialog_create, NULL);
    lv_async_call_cancel(_message_box_create, NULL);
    
    s_wifi_menu.scan_in_progress = false;
    s_wifi_menu.scan_done = false;
    s_wifi_menu.has_cached_results = false;
    
    if (s_wifi_menu.qr_block.update_timer) {
        lv_timer_del(s_wifi_menu.qr_block.update_timer);
        s_wifi_menu.qr_block.update_timer = NULL;
    }
    
    if (s_wifi_menu.app_wifi_event_handler) {
        esp_event_handler_instance_unregister(APP_SETTINGS_WIFI_EVENTS, ESP_EVENT_ANY_ID, 
                                             s_wifi_menu.app_wifi_event_handler);
        s_wifi_menu.app_wifi_event_handler = NULL;
    }
    if (s_wifi_menu.wifi_event_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                             s_wifi_menu.wifi_event_handler);
        s_wifi_menu.wifi_event_handler = NULL;
    }
    if (s_wifi_menu.uart_event_handler) {
        esp_event_handler_instance_unregister(APP_SETTINGS_UART_EVENTS, 
                                             APP_SETTGINS_UART_EVENT_RECIEVED_WIFI_CRED,
                                             s_wifi_menu.uart_event_handler);
        s_wifi_menu.uart_event_handler = NULL;
    }

    app_settings_wifi_scan_enable_data_t payload = {.enable = false};
    esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ,
                   &payload, sizeof(payload), 0);

    if (s_wifi_menu.portal_running || config_portal_is_running()) {
        /* Release the Wi-Fi portal HTTP server before another settings
         * section starts its own server on port 80. */
        config_portal_stop();
        s_wifi_menu.portal_running = false;
    }
    
    xSemaphoreGive(s_wifi_menu_mutex);
}

static lv_obj_tree_walk_res_t clear_clickable_children(struct _lv_obj_t* obj, void* user_data)
{
    if (obj == (lv_obj_t*)user_data) return LV_OBJ_TREE_WALK_NEXT;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return LV_OBJ_TREE_WALK_NEXT;
}

static void network_item_event_cb(lv_event_t* e)
{
    lv_obj_t* btn = lv_event_get_target(e);
    wifi_ap_record_t* network = (wifi_ap_record_t*)lv_obj_get_user_data(btn);
    ESP_LOGI(TAG, "Selected: ssid=%s", network->ssid);
    app_settings_wifi_set_network_data_t payload = {0};
    memcpy(&payload.network, network, sizeof(wifi_ap_record_t));
    esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SET_NETWORK_REQ, &payload, sizeof(payload), 0);
}

static lv_obj_t* create_list_item(lv_obj_t* parent, wifi_ap_record_t* network, bool is_connected)
{
    lv_obj_t* list_btn = lv_list_add_btn(parent, NULL, "");
    lv_obj_set_style_bg_color(list_btn, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_bg_opa(list_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(list_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_border_opa(list_btn, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(list_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_top(list_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(list_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(list_btn, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_right(list_btn, 20, LV_PART_MAIN);

    static lv_coord_t grid_col_dsc[] = {LV_GRID_FR(2), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t grid_row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(list_btn, grid_col_dsc, grid_row_dsc);

    lv_obj_t* ssid_label = lv_label_create(list_btn);
    if (is_connected) {
        lv_label_set_text_fmt(ssid_label, "%s (Connected)", network->ssid);
    } else {
        lv_label_set_text_fmt(ssid_label, "%s", network->ssid);
    }
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ssid_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_grid_cell(ssid_label, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t* conn_rssi_label = lv_label_create(list_btn);
    lv_label_set_text_fmt(conn_rssi_label, "%d dBm", network->rssi);
    lv_obj_set_style_text_font(conn_rssi_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(conn_rssi_label,lv_palette_darken(LV_PALETTE_GREY, 1), LV_PART_MAIN);
    lv_obj_set_grid_cell(conn_rssi_label, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 0, 2);

    lv_obj_t* conn_sec_label = lv_label_create(list_btn);
    lv_label_set_text_fmt(conn_sec_label, "%s", network->authmode != WIFI_AUTH_OPEN ? "secure" : "");
    lv_obj_set_style_text_font(conn_sec_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(conn_sec_label, lv_palette_darken(LV_PALETTE_GREY, 1), LV_PART_MAIN);
    lv_obj_set_grid_cell(conn_sec_label, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    if (!is_connected) {
        lv_obj_set_user_data(list_btn, (void*)network);
        lv_obj_add_event_cb(list_btn, network_item_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_tree_walk(list_btn, clear_clickable_children, list_btn);
    } else {
        lv_obj_clear_flag(list_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(list_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
        lv_obj_set_style_bg_opa(list_btn, LV_OPA_10,0);
        lv_obj_set_style_border_color(list_btn, lv_color_hex(0x2A96FF), 0);
        lv_obj_set_style_border_opa(list_btn, LV_OPA_COVER, 0);
    }

    return list_btn;
}

static void toggle_password_visibility(lv_event_t* e)
{
    current_connection_t* conn = &s_wifi_menu.current_connection;
    conn->pass_visible = !conn->pass_visible;
    
    if (conn->pass_visible) {
        lv_label_set_text_fmt(conn->pass_label, "Password: %s", conn->current_pass);
        lv_label_set_text(conn->show_pass_label, "Hide");
    } else {
        size_t len = strlen(conn->current_pass);
        char masked[65] = {0};
        for (size_t i = 0; i < len && i < 60; i++) masked[i] = '*';
        masked[len > 60 ? 60 : len] = '\0';
        if (len > 0) {
            lv_label_set_text_fmt(conn->pass_label, "Password: %s", masked);
        } else {
            lv_label_set_text(conn->pass_label, "Password: Open");
        }
        lv_label_set_text(conn->show_pass_label, "Show");
    }
}

static void disconnect_btn_event_cb(lv_event_t* e)
{
    s_wifi_menu.scan_in_progress = false;
    app_settings_wifi_scan_enable_data_t payload_disable = {.enable = false};
    esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ, 
                   &payload_disable, sizeof(payload_disable), 0);
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_DISCONNECT_REQ, NULL, 0, 0);
}

static current_connection_t current_connection_area_create(lv_obj_t* parent)
{
    init_styles();
    
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(cont, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(cont, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(cont, 25, LV_PART_MAIN);
    lv_obj_set_style_pad_right(cont, 25, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cont, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 2, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_opa(cont, LV_OPA_60, 0);
    lv_obj_set_style_radius(cont, 20, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 4, 0);
    
    lv_obj_t* row1 = lv_obj_create(cont);
    lv_obj_remove_style_all(row1);
    lv_obj_set_size(row1, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row1, 20, 0);
    
    lv_obj_t* ssid_label = lv_label_create(row1);
    lv_label_set_text(ssid_label, "Not connected");
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(ssid_label, lv_color_white(), LV_PART_MAIN);
    
    lv_obj_t* pass_label = lv_label_create(row1);
    lv_label_set_text(pass_label, "");
    lv_obj_set_style_text_font(pass_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(pass_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_flag(pass_label, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* spacer1 = lv_obj_create(row1);
    lv_obj_remove_style_all(spacer1);
    lv_obj_set_flex_grow(spacer1, 1);
    lv_obj_set_height(spacer1, 1);
    
    lv_obj_t* show_pass_btn = lv_btn_create(row1);
    lv_obj_set_size(show_pass_btn, 50, 28);
    lv_obj_set_style_radius(show_pass_btn, 6, 0);
    lv_obj_set_style_bg_color(show_pass_btn, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_bg_opa(show_pass_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(show_pass_btn, 2, 0);
    lv_obj_set_style_border_color(show_pass_btn, lv_color_hex(0x555555), 0);
    lv_obj_add_flag(show_pass_btn, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* show_pass_label = lv_label_create(show_pass_btn);
    lv_label_set_text(show_pass_label, "Show");
    lv_obj_center(show_pass_label);
    lv_obj_set_style_text_font(show_pass_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_border_opa(show_pass_btn, LV_OPA_80, 0);
    lv_obj_set_style_text_color(show_pass_label, lv_color_white(), 0);
    lv_obj_add_event_cb(show_pass_btn, toggle_password_visibility, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* row2 = lv_obj_create(cont);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    lv_obj_t* status_label = lv_label_create(row2);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_palette_darken(LV_PALETTE_GREY, 1), LV_PART_MAIN);
    
    lv_obj_t* spacer2 = lv_obj_create(row2);
    lv_obj_remove_style_all(spacer2);
    lv_obj_set_flex_grow(spacer2, 1);
    lv_obj_set_height(spacer2, 1);
    
    lv_obj_t* disconnect_btn = lv_btn_create(row2);
    lv_obj_set_size(disconnect_btn, 105, 32);
    lv_obj_set_style_radius(disconnect_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(disconnect_btn, lv_color_hex(0xDC3545), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(disconnect_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(disconnect_btn, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(disconnect_btn, lv_color_hex(0xDC3545), LV_PART_MAIN);
    lv_obj_set_style_border_opa(disconnect_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* btn_label = lv_label_create(disconnect_btn);
    lv_label_set_text(btn_label, "Disconnect");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(disconnect_btn, disconnect_btn_event_cb, LV_EVENT_CLICKED, NULL);

    current_connection_t cur_con = {
        .cont = cont,
        .ssid_label = ssid_label,
        .pass_label = pass_label,
        .show_pass_btn = show_pass_btn,
        .show_pass_label = show_pass_label,
        .status_label = status_label,
        .disconnect_btn = disconnect_btn,
        .pass_visible = false,
        .current_pass = {0}
    };
    return cur_con;
}

static scan_area_t scan_area_create(lv_obj_t* parent)
{
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), 260);
    lv_obj_set_style_pad_top(cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_left(cont, 15, LV_PART_MAIN);
    lv_obj_set_style_pad_right(cont, 15, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cont, lv_palette_darken(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 2, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_opa(cont, LV_OPA_60, 0);
    lv_obj_set_style_radius(cont, 20, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text(title, "Available Networks");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(title, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_left(title, 10, 0);

    lv_obj_t* scanning_label = lv_label_create(cont);
    lv_label_set_text(scanning_label, "Scanning...");
    lv_obj_set_style_text_font(scanning_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(scanning_label, lv_palette_darken(LV_PALETTE_GREY, 1), LV_PART_MAIN);
    lv_obj_set_style_pad_left(scanning_label, 10, 0);

    lv_obj_t* list = lv_list_create(cont);
    lv_obj_set_size(list, lv_pct(100), 215);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_bg_color(list, lv_palette_darken(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_0, 0);
    lv_obj_set_style_radius(list, 20, 0);

    scan_area_t scan_area = {.cont = cont, .list = list, .scanning_label = scanning_label};
    return scan_area;
}

static void update_cred_status(wifi_qr_block_t* qr_block)
{
    char wifi_ssid[33] = {0};
    char wifi_pass[65] = {0};
    load_wifi_credentials_from_nvs(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));
    
    if (qr_block->cred_status_label) {
        if (strlen(wifi_ssid) > 0) {
            lv_obj_remove_style(qr_block->cred_status_label, &style_status_err, 0);
            lv_obj_add_style(qr_block->cred_status_label, &style_status_ok, 0);
            lv_label_set_text(qr_block->cred_status_label, "CONFIGURED");
        } else {
            lv_obj_remove_style(qr_block->cred_status_label, &style_status_ok, 0);
            lv_obj_add_style(qr_block->cred_status_label, &style_status_err, 0);
            lv_label_set_text(qr_block->cred_status_label, "MISSING");
        }
    }
}

static void update_qr_display(wifi_qr_block_t* qr_block)
{
    bool wifi_connected = wifi_ctrl_is_connected();
    bool portal_running = config_portal_is_running();
    
    if (!qr_block->right_side) return;
    
    if (wifi_connected) {
        if (qr_block->wifi_qr_col) lv_obj_add_flag(qr_block->wifi_qr_col, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->url_qr_col)  lv_obj_add_flag(qr_block->url_qr_col, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->scan_info_label) lv_obj_add_flag(qr_block->scan_info_label, LV_OBJ_FLAG_HIDDEN);
        
        if (qr_block->wifi_info_label) {
            lv_obj_clear_flag(qr_block->wifi_info_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(qr_block->wifi_info_label,
                "Already connected.\nSelect another network from the list above "
                "or disconnect\nto send new credentials.");
        }
        update_cred_status(qr_block);
        
    } else if (!wifi_connected && portal_running) {
        if (qr_block->scan_info_label) lv_obj_add_flag(qr_block->scan_info_label, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->wifi_info_label) lv_obj_add_flag(qr_block->wifi_info_label, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->wifi_qr_col)     lv_obj_clear_flag(qr_block->wifi_qr_col, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->wifi_qr_frame)   lv_obj_clear_flag(qr_block->wifi_qr_frame, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->wifi_label) {
            lv_obj_clear_flag(qr_block->wifi_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(qr_block->wifi_label, "1. Connect to\n%s", config_portal_get_ap_ssid());
        }
        if (qr_block->url_qr_col)     lv_obj_clear_flag(qr_block->url_qr_col, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->url_qr_frame)   lv_obj_clear_flag(qr_block->url_qr_frame, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->url_label) {
            lv_obj_clear_flag(qr_block->url_label, LV_OBJ_FLAG_HIDDEN);
            const char* ap_url = config_portal_get_ap_url();
            const char* host = ap_url;
            if (strncmp(host, "http://", 7) == 0) host += 7;
            lv_label_set_text_fmt(qr_block->url_label, "2. Open setup\n%s", host);
        }
        if (qr_block->wifi_qr_obj) {
            char wifi_payload[128];
            snprintf(wifi_payload, sizeof(wifi_payload), "WIFI:T:WPA;S:%s;P:%s;;", 
                     config_portal_get_ap_ssid(), config_portal_get_ap_password());
            lv_qrcode_update(qr_block->wifi_qr_obj, wifi_payload, strlen(wifi_payload));
        }
        if (qr_block->url_qr_obj) {
            const char* ap_url = config_portal_get_ap_url();
            if (ap_url && ap_url[0]) {
                lv_qrcode_update(qr_block->url_qr_obj, ap_url, strlen(ap_url));
            }
        }
    } else {
        if (qr_block->wifi_qr_col)  lv_obj_add_flag(qr_block->wifi_qr_col, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->url_qr_col)   lv_obj_add_flag(qr_block->url_qr_col, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->wifi_info_label) lv_obj_add_flag(qr_block->wifi_info_label, LV_OBJ_FLAG_HIDDEN);
        if (qr_block->scan_info_label) {
            lv_obj_clear_flag(qr_block->scan_info_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void qr_update_timer_cb(lv_timer_t* timer)
{
    wifi_qr_block_t* qr_block = (wifi_qr_block_t*)timer->user_data;
    if (!config_portal_is_running()) {
        lv_timer_del(timer);
        qr_block->update_timer = NULL;
        return;
    }
    
    config_portal_access_mode_t mode = config_portal_get_access_mode();
    if (mode == CONFIG_PORTAL_ACCESS_LOCAL) {
        const char* url = config_portal_get_local_url();
        if (url && !strstr(url, "0.0.0.0")) {
            update_qr_display(qr_block);
            lv_timer_del(timer);
            qr_block->update_timer = NULL;
        }
    } else if (mode == CONFIG_PORTAL_ACCESS_AP) {
        update_qr_display(qr_block);
        lv_timer_del(timer);
        qr_block->update_timer = NULL;
    }
}

static wifi_qr_block_t wifi_qr_block_create(lv_obj_t* parent)
{
    init_styles();
    
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), 165);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* left_side = lv_obj_create(card);
    lv_obj_remove_style_all(left_side);
    lv_obj_set_pos(left_side, 0, 0);
    lv_obj_set_size(left_side, 305, lv_pct(100));
    lv_obj_set_flex_flow(left_side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_side, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(left_side, 25, 0);
    lv_obj_set_style_pad_top(left_side, 12, 0);
    lv_obj_set_style_pad_bottom(left_side, 8, 0);
    lv_obj_clear_flag(left_side, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* title = lv_label_create(left_side);
    lv_label_set_text(title, "Setup via mobile");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_pad_bottom(title, 6, 0);
    
    lv_obj_t* desc = lv_label_create(left_side);
    lv_label_set_text(desc, "Scan QR code to open setup page,\nenter credentials and send to\nsave into device memory.");
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(desc, lv_palette_darken(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_pad_bottom(desc, 8, 0);
    
    lv_obj_t* cred_status = lv_label_create(left_side);
    lv_label_set_text(cred_status, "MISSING");
    lv_obj_set_style_text_font(cred_status, &lv_font_montserrat_14, 0);
    lv_obj_add_style(cred_status, &style_status_err, 0);
    
    lv_obj_t* right_side = lv_obj_create(card);
    lv_obj_remove_style_all(right_side);
    lv_obj_set_pos(right_side, 325, 0);
    lv_obj_set_size(right_side, 285, lv_pct(100));
    lv_obj_set_flex_flow(right_side, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_side, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(right_side, 0, 0);
    lv_obj_set_style_pad_column(right_side, 8, 0);
    lv_obj_clear_flag(right_side, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* wifi_info_label = lv_label_create(right_side);
    lv_obj_set_style_text_font(wifi_info_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(wifi_info_label, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_label_set_long_mode(wifi_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wifi_info_label, 270);
    lv_obj_set_style_text_align(wifi_info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(wifi_info_label, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* scan_info_label = lv_label_create(right_side);
    lv_obj_set_style_text_font(scan_info_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(scan_info_label, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_label_set_long_mode(scan_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(scan_info_label, 270);
    lv_obj_set_style_text_align(scan_info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(scan_info_label, "Wait for the network scan to complete.");
    lv_obj_add_flag(scan_info_label, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* wifi_qr_col = lv_obj_create(right_side);
    lv_obj_remove_style_all(wifi_qr_col);
    lv_obj_set_size(wifi_qr_col, 125, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wifi_qr_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_qr_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wifi_qr_col, 8, 0);
    
    lv_obj_t* wifi_label = lv_label_create(wifi_qr_col);
    lv_label_set_text(wifi_label, "");
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_label, lv_color_white(), 0);
    lv_label_set_long_mode(wifi_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wifi_label, 120);
    lv_obj_set_style_text_align(wifi_label, LV_TEXT_ALIGN_CENTER, 0);
    
    lv_obj_t* wifi_qr_frame = lv_obj_create(wifi_qr_col);
    lv_obj_set_size(wifi_qr_frame, 120, 120);
    lv_obj_set_style_radius(wifi_qr_frame, 16, 0);
    lv_obj_set_style_bg_color(wifi_qr_frame, lv_color_white(), 0);
    lv_obj_set_style_pad_all(wifi_qr_frame, 5, 0);
    lv_obj_clear_flag(wifi_qr_frame, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* url_qr_col = lv_obj_create(right_side);
    lv_obj_remove_style_all(url_qr_col);
    lv_obj_set_size(url_qr_col, 125, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(url_qr_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(url_qr_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(url_qr_col, 0, 0);
    
    lv_obj_t* url_label = lv_label_create(url_qr_col);
    lv_label_set_text(url_label, "");
    lv_obj_set_style_text_font(url_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(url_label, lv_color_white(), 0);
    lv_label_set_long_mode(url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(url_label, 120);
    lv_obj_set_style_text_align(url_label, LV_TEXT_ALIGN_CENTER, 0);
    
    lv_obj_t* url_qr_frame = lv_obj_create(url_qr_col);
    lv_obj_set_size(url_qr_frame, 120, 120);
    lv_obj_set_style_radius(url_qr_frame, 16, 0);
    lv_obj_set_style_bg_color(url_qr_frame, lv_color_white(), 0);
    lv_obj_set_style_pad_all(url_qr_frame, 5, 0);
    lv_obj_clear_flag(url_qr_frame, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* wifi_qr_obj = NULL;
    lv_obj_t* url_qr_obj = NULL;
    
#if LV_USE_QRCODE
    char wifi_payload[128];
    snprintf(wifi_payload, sizeof(wifi_payload), "WIFI:T:WPA;S:%s;P:%s;;", 
             config_portal_get_ap_ssid(), config_portal_get_ap_password());
    
    wifi_qr_obj = lv_qrcode_create(wifi_qr_frame, 105, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
    if (wifi_qr_obj) {
        lv_qrcode_update(wifi_qr_obj, wifi_payload, strlen(wifi_payload));
        lv_obj_center(wifi_qr_obj);
    }
    
    url_qr_obj = lv_qrcode_create(url_qr_frame, 105, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
    if (url_qr_obj) {
        lv_qrcode_update(url_qr_obj, "http://192.168.4.1", strlen("http://192.168.4.1"));
        lv_obj_center(url_qr_obj);
    }
#else
    lv_obj_t* wifi_placeholder = lv_label_create(wifi_qr_frame);
    lv_label_set_text(wifi_placeholder, "QR\nN/A");
    lv_obj_center(wifi_placeholder);
    
    lv_obj_t* url_placeholder = lv_label_create(url_qr_frame);
    lv_label_set_text(url_placeholder, "QR\nN/A");
    lv_obj_center(url_placeholder);
#endif
    
    wifi_qr_block_t qr_block = {
        .cont = card,
        .left_side = left_side,
        .right_side = right_side,
        .wifi_qr_col = wifi_qr_col,
        .url_qr_col = url_qr_col,
        .wifi_qr_frame = wifi_qr_frame,
        .url_qr_frame = url_qr_frame,
        .wifi_label = wifi_label,
        .url_label = url_label,
        .wifi_info_label = wifi_info_label,
        .wifi_qr_obj = wifi_qr_obj,
        .url_qr_obj = url_qr_obj,
        .cred_status_label = cred_status,
        .update_timer = NULL,
        .scan_info_label = scan_info_label
    };
    
    update_cred_status(&qr_block);
    update_qr_display(&qr_block);
    
    return qr_block;
}

static void display_scan_data(void* arg)
{
    (void)arg;
    if (!wifi_menu_active) return;
    if (!s_wifi_menu.scan_area.list || !lv_obj_is_valid(s_wifi_menu.scan_area.list)) return;
    
    if (s_wifi_menu.scan_area.scanning_label && lv_obj_is_valid(s_wifi_menu.scan_area.scanning_label)) {
        lv_obj_add_flag(s_wifi_menu.scan_area.scanning_label, LV_OBJ_FLAG_HIDDEN);
    }
    
    s_wifi_menu.scan_done = true;
    s_wifi_menu.has_cached_results = true;
    lv_obj_clean(s_wifi_menu.scan_area.list);
    
    wifi_ap_record_t current_ap;
    bool is_connected = wifi_ctrl_is_connected() && (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK);
    
    if (s_wifi_menu.ap_records_count == 0) {
        lv_obj_t* no_networks_label = lv_label_create(s_wifi_menu.scan_area.list);
        lv_label_set_text(no_networks_label, "No networks found");
        lv_obj_set_style_text_font(no_networks_label, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(no_networks_label, lv_palette_darken(LV_PALETTE_GREY, 2), LV_PART_MAIN);
        lv_obj_set_style_pad_all(no_networks_label, 10, LV_PART_MAIN);
    } else {
        for (int i = 0; i < s_wifi_menu.ap_records_count; i++) {
            wifi_ap_record_t* network = &s_wifi_menu.ap_records_pool[i];
            bool connected = false;
            if (is_connected && strcmp((const char*)network->ssid, (const char*)current_ap.ssid) == 0) {
                connected = true;
            }
            create_list_item(s_wifi_menu.scan_area.list, network, connected);
        }
    }
    
    start_portal_if_needed();
}

static void on_connected(void* arg)
{
    if (!wifi_menu_active) return;
    const char* ssid = (const char*)arg;
    
    lv_label_set_text_fmt(s_wifi_menu.current_connection.ssid_label, "Connected: %s", ssid);
    
    char wifi_pass[65] = {0};
    load_current_wifi_password(wifi_pass, sizeof(wifi_pass));
    strlcpy(s_wifi_menu.current_connection.current_pass, wifi_pass, 65);
    
    if (strlen(wifi_pass) > 0) {
        size_t len = strlen(wifi_pass);
        char masked[65] = {0};
        for (size_t i = 0; i < len && i < 60; i++) masked[i] = '*';
        masked[len > 60 ? 60 : len] = '\0';
        lv_label_set_text_fmt(s_wifi_menu.current_connection.pass_label, "Password: %s", masked);
    } else {
        lv_label_set_text(s_wifi_menu.current_connection.pass_label, "Password: Open");
    }
    
    lv_obj_clear_flag(s_wifi_menu.current_connection.pass_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_menu.current_connection.show_pass_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_menu.current_connection.disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    
    if (config_portal_is_running()) {
        config_portal_stop();
    }
    s_wifi_menu.portal_running = false;
    
    if (s_wifi_menu.qr_block.update_timer) {
        lv_timer_del(s_wifi_menu.qr_block.update_timer);
        s_wifi_menu.qr_block.update_timer = NULL;
    }
    
    update_cred_status(&s_wifi_menu.qr_block);
    update_qr_display(&s_wifi_menu.qr_block);
}

static void on_connecting(void* arg)
{
    if (!wifi_menu_active) return;
    (void)arg;
    lv_label_set_text(s_wifi_menu.current_connection.ssid_label, "Connecting...");
    lv_obj_add_flag(s_wifi_menu.current_connection.pass_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_menu.current_connection.show_pass_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_menu.current_connection.disconnect_btn, LV_OBJ_FLAG_HIDDEN);
}

static void on_disconnected(void* arg)
{
    (void)arg;
    if (!wifi_menu_active) return;
    lv_label_set_text(s_wifi_menu.current_connection.ssid_label, "Not connected");
    lv_label_set_text(s_wifi_menu.current_connection.status_label, "");
    lv_obj_add_flag(s_wifi_menu.current_connection.pass_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_menu.current_connection.show_pass_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_menu.current_connection.disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    
    update_cred_status(&s_wifi_menu.qr_block);
    
    if (s_wifi_menu.portal_running) {
        config_portal_stop();
        s_wifi_menu.portal_running = false;
    }
    update_qr_display(&s_wifi_menu.qr_block);
    
    s_wifi_menu.scan_in_progress = false;
    app_settings_wifi_scan_enable_data_t payload_disable = {.enable = false};
    esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ, 
                   &payload_disable, sizeof(payload_disable), 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    
    s_wifi_menu.scan_done = false;
    s_wifi_menu.scan_in_progress = true;
    s_wifi_menu.ap_records_count = 0;
    lv_obj_clean(s_wifi_menu.scan_area.list);
    
    if (s_wifi_menu.scan_area.scanning_label) {
        lv_obj_clear_flag(s_wifi_menu.scan_area.scanning_label, LV_OBJ_FLAG_HIDDEN);
    }
    
    app_settings_wifi_scan_enable_data_t payload = {.enable = true};
    esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ, 
                   &payload, sizeof(payload), 0);
}

static void on_connectivity_update(void* arg)
{
    if (!wifi_menu_active) return;
    app_settings_wifi_connectivity_status_data_t* payload = (app_settings_wifi_connectivity_status_data_t*)arg;
    if (payload->len) {
        lv_label_set_text_fmt(s_wifi_menu.current_connection.status_label, "Status: %s", payload->details);
    } else {
        lv_label_set_text(s_wifi_menu.current_connection.status_label, "");
    }
    
    if (wifi_ctrl_is_connected()) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            lv_label_set_text_fmt(s_wifi_menu.current_connection.ssid_label, "Connected: %s", ap_info.ssid);
            
            if (lv_obj_has_flag(s_wifi_menu.current_connection.pass_label, LV_OBJ_FLAG_HIDDEN)) {
                char wifi_pass[65] = {0};
                load_current_wifi_password(wifi_pass, sizeof(wifi_pass));
                strlcpy(s_wifi_menu.current_connection.current_pass, wifi_pass, 65);
                
                if (strlen(wifi_pass) > 0) {
                    size_t len = strlen(wifi_pass);
                    char masked[65] = {0};
                    for (size_t i = 0; i < len && i < 60; i++) masked[i] = '*';
                    masked[len > 60 ? 60 : len] = '\0';
                    lv_label_set_text_fmt(s_wifi_menu.current_connection.pass_label, "Password: %s", masked);
                } else {
                    lv_label_set_text(s_wifi_menu.current_connection.pass_label, "Password: Open");
                }
                
                lv_obj_clear_flag(s_wifi_menu.current_connection.pass_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_wifi_menu.current_connection.show_pass_btn, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_wifi_menu.current_connection.disconnect_btn, LV_OBJ_FLAG_HIDDEN);
            }
            
            update_cred_status(&s_wifi_menu.qr_block);
            update_qr_display(&s_wifi_menu.qr_block);
        }
    } else {
        lv_label_set_text(s_wifi_menu.current_connection.ssid_label, "Not connected");
        lv_obj_add_flag(s_wifi_menu.current_connection.pass_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_menu.current_connection.show_pass_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_menu.current_connection.disconnect_btn, LV_OBJ_FLAG_HIDDEN);
        
        update_cred_status(&s_wifi_menu.qr_block);
        update_qr_display(&s_wifi_menu.qr_block);
    }
}

static void _password_dialog_create(void* arg)
{
    if (!wifi_menu_active) return;
    if (!s_wifi_menu.top_layer || !lv_obj_is_valid(s_wifi_menu.top_layer)) return;
    const char* ssid = (const char*)arg;
    password_dialog_create(s_wifi_menu.top_layer, ssid);
}

static void _message_box_create(void* arg)
{
    if (!wifi_menu_active) return;
    if (!s_wifi_menu.top_layer || !lv_obj_is_valid(s_wifi_menu.top_layer)) return;
    const char* msg = (const char*)arg;
    message_box_create(s_wifi_menu.top_layer, msg);
}

static void on_wifi_event(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    (void)event_handler_arg;
    (void)event_base;
    if (!wifi_menu_active) return;
    static char buffer[64] = {0};
    switch (event_id) {
    case APP_SETTINGS_WIFI_EVENT_CONNECTIVITY_STATUS_CHANGED:
        if (event_data) {
            static app_settings_wifi_connectivity_status_data_t payload = {0};
            memcpy(&payload, event_data, sizeof(payload));
            lv_async_call(on_connectivity_update, &payload);
        }
        break;
    case APP_SETTINGS_WIFI_EVENT_PASSWORD_REQUESTED:
        if (event_data) {
            app_settings_wifi_password_requested_data_t* payload =
                (app_settings_wifi_password_requested_data_t*)event_data;
            snprintf(buffer, sizeof(buffer), "%s", payload->ssid);
            lv_async_call(_password_dialog_create, buffer);
        }
        break;
    case APP_SETTINGS_WIFI_EVENT_CONNECT_RESULT:
        if (event_data) {
            app_settings_wifi_connect_result_data_t* payload = (app_settings_wifi_connect_result_data_t*)event_data;
            snprintf(buffer, sizeof(buffer), "%s", payload->details);
            lv_async_call(_message_box_create, buffer);
        }
        break;
    case APP_SETTINGS_WIFI_EVENT_CONNECTING:
        lv_async_call(on_connecting, NULL);
        break;
    default:
        break;
    }
}

static void on_wifi_internal_event(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id,
                                   void* event_data)
{
    (void)event_handler_arg;
    (void)event_base;
    if (!wifi_menu_active) return;
    switch (event_id) {
    case WIFI_EVENT_STA_CONNECTED:
        if (event_data) {
            wifi_event_sta_connected_t* event = (wifi_event_sta_connected_t*)event_data;
            static char ssid[33];
            snprintf(ssid, sizeof(ssid), "%s", event->ssid);
            lv_async_call_cancel(on_connecting, NULL);
            lv_async_call(on_connected, ssid);
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        lv_async_call_cancel(on_connecting, NULL);
        lv_async_call(on_disconnected, NULL);
        break;
    case WIFI_EVENT_SCAN_DONE: {
        if (!s_wifi_menu.scan_in_progress) break;
        
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        uint16_t max_aps = ap_count > WIFI_MAX_NETWORKS ? WIFI_MAX_NETWORKS : ap_count;
        
        if (max_aps > 0) {
            uint16_t number = max_aps;
            if (esp_wifi_scan_get_ap_records(&number, s_wifi_menu.ap_records_pool) == ESP_OK) {
                s_wifi_menu.ap_records_count = number;
            }
        } else {
            s_wifi_menu.ap_records_count = 0;
        }
        
        s_wifi_menu.scan_in_progress = false;
        app_settings_wifi_scan_enable_data_t payload = {.enable = false};
        esp_event_post(APP_SETTINGS_WIFI_EVENTS, APP_SETTINGS_WIFI_EVENT_SCAN_ENABLE_REQ, 
                       &payload, sizeof(payload), 0);
        
        lv_async_call(display_scan_data, NULL);
        break;
    }
    default:
        break;
    }
}

static void on_uart_wifi_cred_event(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    (void)arg;
    (void)event_base;
    if (!wifi_menu_active) return;
    if (event_id == APP_SETTGINS_UART_EVENT_RECIEVED_WIFI_CRED) {
        app_settings_uart_wifi_cred_data_t* data = (app_settings_uart_wifi_cred_data_t*)event_data;
        
        strlcpy(s_wifi_menu.current_connection.current_pass, data->password, 65);
        
        if (s_wifi_menu.current_connection.pass_label) {
            if (data->len > 0) {
                char masked[65] = {0};
                for (int i = 0; i < data->len && i < 60; i++) masked[i] = '*';
                masked[data->len > 60 ? 60 : data->len] = '\0';
                lv_label_set_text_fmt(s_wifi_menu.current_connection.pass_label, "Password: %s", masked);
                lv_obj_clear_flag(s_wifi_menu.current_connection.pass_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_wifi_menu.current_connection.show_pass_btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
        
        update_cred_status(&s_wifi_menu.qr_block);
        update_qr_display(&s_wifi_menu.qr_block);
    }
}

lv_obj_t* wifi_menu_create(lv_obj_t* parent, lv_obj_t* top_layer)
{
    if (!s_wifi_menu_mutex) {
        s_wifi_menu_mutex = xSemaphoreCreateMutex();
    }
    
    init_styles();
    
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(cont, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    current_connection_t cur_con = current_connection_area_create(cont);
    scan_area_t scan_area = scan_area_create(cont);
    wifi_qr_block_t qr_block = wifi_qr_block_create(cont);

    s_wifi_menu.cont = cont;
    s_wifi_menu.top_layer = top_layer;
    s_wifi_menu.current_connection = cur_con;
    s_wifi_menu.scan_area = scan_area;
    s_wifi_menu.qr_block = qr_block;
    s_wifi_menu.scan_in_progress = false;
    s_wifi_menu.scan_done = false;
    s_wifi_menu.has_cached_results = false;
    s_wifi_menu.portal_running = false;

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(APP_SETTINGS_WIFI_EVENTS, ESP_EVENT_ANY_ID, on_wifi_event, NULL, 
                                           &s_wifi_menu.app_wifi_event_handler));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_internal_event, NULL,
                                           &s_wifi_menu.wifi_event_handler));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(APP_SETTINGS_UART_EVENTS, APP_SETTGINS_UART_EVENT_RECIEVED_WIFI_CRED,
                                            on_uart_wifi_cred_event, NULL, &s_wifi_menu.uart_event_handler));
    
    return cont;
}
