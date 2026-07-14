#ifndef WIFI_CONNECTIVITY_H
#define WIFI_CONNECTIVITY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    WIFI_CONNECTIVITY_UNKNOWN,        // Not checked yet
    WIFI_CONNECTIVITY_NO_INTERNET,    // Connected to AP but no internet
    WIFI_CONNECTIVITY_OK,             // Internet available
    WIFI_CONNECTIVITY_WEAK_SIGNAL,    // Internet available but weak signal
    WIFI_CONNECTIVITY_ROUTER_PROBLEM, // Have IP but can't reach gateway
    WIFI_CONNECTIVITY_ISP_PROBLEM     // Can reach gateway but not internet
} wifi_connectivity_status_t;

typedef void (*wifi_connectivity_callback_t)(void* user_ctx, wifi_connectivity_status_t status, void* event_data);

bool wifi_connectivity_init(void);
void wifi_connectivity_deinit(void);
bool wifi_connectivity_start(void);
void wifi_connectivity_stop(void);
void wifi_connectivity_set_check_interval(uint32_t interval_ms);
wifi_connectivity_status_t wifi_connectivity_check_now(void);
wifi_connectivity_status_t wifi_connectivity_get_status(void);
int wifi_connectivity_get_rssi(void);
const char* wifi_connectivity_get_details(void);
bool wifi_connectivity_has_internet(void);
void wifi_connectivity_register_callback(wifi_connectivity_callback_t callback, void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONNECTIVITY_H */
