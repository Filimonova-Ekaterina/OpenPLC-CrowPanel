#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_storage_init();
// not needed, wifi stack saves/loads by default
esp_err_t save_wifi_credentials_to_nvs(const char* ssid, const char* password);
esp_err_t load_wifi_credentials_from_nvs(char* ssid, size_t ssid_len, char* password, size_t password_len);

void credentials_set_pending_ssid(const char* ssid);
const char* credentials_get_pending_ssid(void);
void credentials_clear_pending_ssid(void);

#ifdef __cplusplus
}
#endif
