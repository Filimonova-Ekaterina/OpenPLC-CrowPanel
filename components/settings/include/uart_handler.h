#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    void* ctx;
    bool (*on_recv_wifi_password)(void* ctx, const char* ssid, const char* password);
    bool (*on_recv_weather_api_key)(void* ctx, const char* key);
    bool (*on_recv_calendar_api_key)(void* ctx, const char* key);
    bool (*on_recv_ha_creds)(void* ctx, const char* url, const char* login, const char* password);
} uart_handler_callbacks_t;

/**
 * @brief Initialize the UART handler.
 */
void uart_password_handler_init(const uart_handler_callbacks_t* callbacks);

/**
 * @brief Deinitialize the UART handler completely.
 */
void uart_password_handler_deinit(void);

/**
 * @brief Start the UART listening task.
 */
void uart_password_handler_start(void);

/**
 * @brief Stop the UART listening task.
 */
void uart_password_handler_stop(void);

/**
 * @brief Check if the UART handler is currently running.
 */
bool uart_password_handler_is_active(void);

const char* uart_password_handler_get_ssid(void);

/**
 * @brief Get the received WiFi password.
 * @return Pointer to internal buffer or NULL if not received.
 */
const char* uart_password_handler_get_password(void);

/**
 * @brief Get the received Weather API key.
 * @return Pointer to internal buffer or NULL if not received.
 */
const char* uart_password_handler_get_weather_key(void);

/**
 * @brief Get the received Calendar API key.
 * @return Pointer to internal buffer or NULL if not received.
 */
const char* uart_password_handler_get_calendar_key(void);

/**
 * @brief Check if both API keys have been received.
 */
bool uart_password_handler_keys_received(void);

/**
 * @brief Clear the stored WiFi password and reset flag.
 */
void uart_password_handler_clear(void);

/**
 * @brief Check if a new password was received since last check.
 * @return true if new password was available, false otherwise.
 *         Resets the internal flag if true.
 */
bool uart_password_handler_has_new_password(void);

/**
 * @brief Set whether UART was auto-started from saved config
 */
void uart_password_handler_set_auto_started(bool auto_started);

/**
 * @brief Check if UART was auto-started from saved config
 */
bool uart_password_handler_was_auto_started(void);

/**
 * @brief Save current UART state to NVS
 */
void uart_password_handler_save_state(void);

/**
 * @brief Load and apply saved UART state from NVS
 */
void uart_password_handler_load_state(void);

bool uart_password_handler_ha_creds_received(void);
const char* uart_password_handler_get_ha_url(void);
const char* uart_password_handler_get_ha_login(void);
const char* uart_password_handler_get_ha_password(void);
uint16_t uart_password_handler_get_ha_ws_port(void);

#ifdef __cplusplus
}
#endif
