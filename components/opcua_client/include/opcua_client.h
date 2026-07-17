#pragma once

/**
 * @file opcua_client.h
 * @brief Background OPC UA discovery, subscription, reconnect, and write API.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "data_model.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPCUA_CLIENT_ENDPOINT_LENGTH 256

typedef enum
{
    OPCUA_CLIENT_STOPPED = 0,
    OPCUA_CLIENT_PAUSED,
    OPCUA_CLIENT_WAITING_FOR_WIFI,
    OPCUA_CLIENT_WAITING_FOR_TIME,
    OPCUA_CLIENT_CONNECTING,
    OPCUA_CLIENT_BROWSING,
    OPCUA_CLIENT_CONNECTED,
    OPCUA_CLIENT_CONNECTION_ERROR,
    OPCUA_CLIENT_BROWSE_ERROR,
} opcua_client_state_t;

typedef struct
{
    const char* endpoint_url;
    uint32_t connection_timeout_ms;
    uint32_t reconnect_delay_ms;
    uint32_t subscription_interval_ms;
    uint32_t task_stack_size;
    unsigned task_priority;
    size_t maximum_equipment_objects;
    size_t maximum_tags;
    unsigned maximum_browse_depth;
} opcua_client_config_t;

typedef struct opcua_client opcua_client_t;

/** Allocate the client. Network activity starts only after opcua_client_start. */
esp_err_t opcua_client_create(const opcua_client_config_t* configuration, data_model_t* data_model,
                              opcua_client_t** client_out);

/** Start the background connection and reconnect task. */
esp_err_t opcua_client_start(opcua_client_t* client);

/** Request a clean background-task stop. */
void opcua_client_stop(opcua_client_t* client);

/** Suspend all OPC UA network activity while a configuration screen is open. */
void opcua_client_pause(opcua_client_t* client);

/** Resume connection attempts after leaving configuration screens. */
void opcua_client_resume(opcua_client_t* client);

/** Queue a Boolean write so LVGL never blocks on network I/O. */
esp_err_t opcua_client_write_boolean(opcua_client_t* client, size_t tag_index, bool value);

/** Queue a numeric write so LVGL never blocks on network I/O. */
esp_err_t opcua_client_write_number(opcua_client_t* client, size_t tag_index, double value);

/** Update the endpoint and request an asynchronous reconnect. */
esp_err_t opcua_client_set_endpoint(opcua_client_t* client, const char* endpoint_url);

/** Copy the currently configured endpoint. */
void opcua_client_get_endpoint(const opcua_client_t* client, char* buffer, size_t buffer_size);

opcua_client_state_t opcua_client_get_state(const opcua_client_t* client);

/** Copy a short user-facing status string into the supplied buffer. */
void opcua_client_get_status(const opcua_client_t* client, char* buffer, size_t buffer_size);

const char* opcua_client_state_name(opcua_client_state_t state);

#ifdef __cplusplus
}
#endif
