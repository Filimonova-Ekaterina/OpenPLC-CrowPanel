#include "wifi_widget.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_log.h"
#include "wifi_connectivity.h"
#include "wifi_ctrl.h"

#define WIFI_WIDGET_REFRESH_PERIOD_MS 10000

extern const lv_img_dsc_t wifi_off;
extern const lv_img_dsc_t wifi_no_internet;
extern const lv_img_dsc_t wifi_low;
extern const lv_img_dsc_t wifi_medium;
extern const lv_img_dsc_t wifi_medium_high;
extern const lv_img_dsc_t wifi_high;

typedef enum
{
    WIFI_ICON_OFF,
    WIFI_ICON_NO_INTERNET,
    WIFI_ICON_LOW,
    WIFI_ICON_MEDIUM,
    WIFI_ICON_MEDIUM_HIGH,
    WIFI_ICON_HIGH,
} wifi_icon_type_t;

struct wifi_widget
{
    lv_obj_t* container;
    lv_obj_t* icon;
    lv_timer_t* refresh_timer;
    wifi_icon_type_t displayed_icon;
};

static const char* TAG = "wifi_widget";

static wifi_icon_type_t select_icon(bool connected, bool has_internet, int rssi)
{
    if (! connected) {
        return WIFI_ICON_OFF;
    }
    if (! has_internet) {
        return WIFI_ICON_NO_INTERNET;
    }
    if (rssi >= -45) {
        return WIFI_ICON_HIGH;
    }
    if (rssi >= -55) {
        return WIFI_ICON_MEDIUM_HIGH;
    }
    if (rssi >= -75) {
        return WIFI_ICON_MEDIUM;
    }
    return WIFI_ICON_LOW;
}

static const lv_img_dsc_t* icon_source(wifi_icon_type_t icon)
{
    switch (icon) {
    case WIFI_ICON_NO_INTERNET:
        return &wifi_no_internet;
    case WIFI_ICON_LOW:
        return &wifi_low;
    case WIFI_ICON_MEDIUM:
        return &wifi_medium;
    case WIFI_ICON_MEDIUM_HIGH:
        return &wifi_medium_high;
    case WIFI_ICON_HIGH:
        return &wifi_high;
    case WIFI_ICON_OFF:
    default:
        return &wifi_off;
    }
}

static void refresh_widget(wifi_widget_t* widget)
{
    bool connected = wifi_ctrl_is_connected();
    int rssi       = connected ? wifi_connectivity_get_rssi() : -100;
    bool internet  = connected && wifi_connectivity_has_internet();
    wifi_icon_type_t requested_icon = select_icon(connected, internet, rssi);
    if (requested_icon == widget->displayed_icon) {
        return;
    }

    widget->displayed_icon = requested_icon;
    lv_img_set_src(widget->icon, icon_source(requested_icon));
    ESP_LOGI(TAG, "Status changed: connected=%d internet=%d rssi=%d icon=%d", connected, internet, rssi,
             requested_icon);
}

static void refresh_timer_callback(lv_timer_t* timer)
{
    refresh_widget(timer->user_data);
}

static void release_widget_callback(lv_event_t* event)
{
    wifi_widget_t* widget = lv_event_get_user_data(event);
    if (widget->refresh_timer != NULL) {
        lv_timer_del(widget->refresh_timer);
        widget->refresh_timer = NULL;
    }
    free(widget);
}

esp_err_t wifi_widget_create(lv_obj_t* parent, wifi_widget_t** widget_out)
{
    if (parent == NULL || widget_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_widget_t* widget = calloc(1, sizeof(*widget));
    if (widget == NULL) {
        return ESP_ERR_NO_MEM;
    }
    widget->displayed_icon = WIFI_ICON_OFF;
    widget->container      = lv_obj_create(parent);
    lv_obj_remove_style_all(widget->container);
    lv_obj_set_size(widget->container, 60, 56);
    lv_obj_clear_flag(widget->container, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(widget->container, release_widget_callback, LV_EVENT_DELETE, widget);

    widget->icon = lv_img_create(widget->container);
    lv_img_set_src(widget->icon, &wifi_off);
    lv_obj_center(widget->icon);

    widget->refresh_timer = lv_timer_create(refresh_timer_callback, WIFI_WIDGET_REFRESH_PERIOD_MS, widget);
    if (widget->refresh_timer == NULL) {
        lv_obj_del(widget->container);
        return ESP_ERR_NO_MEM;
    }
    refresh_widget(widget);
    *widget_out = widget;
    return ESP_OK;
}
