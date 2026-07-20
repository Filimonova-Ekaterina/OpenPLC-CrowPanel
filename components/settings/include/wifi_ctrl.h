#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void wifi_ctrl_init();
void wifi_ctrl_deinit();
void wifi_ctrl_try_stop_scanner();
bool wifi_ctrl_is_connected();

/** Return true only after the station has received a current DHCP address. */
bool wifi_ctrl_has_ip_address(void);
/**
 * @brief Save current WiFi state to NVS
 */
void wifi_ctrl_save_state(void);

/**
 * @brief Load and apply saved WiFi state from NVS
 */
void wifi_ctrl_load_state(void);

/**
 * @brief Check if WiFi was auto-started from saved config
 */
bool wifi_ctrl_was_auto_started(void);

/**
 * @brief Check if there are saved credentials for auto-connect
 */
bool wifi_ctrl_has_saved_credentials(void);

void wifi_ctrl_try_stop_scanner_async(void);

#ifdef __cplusplus
}
#endif
