#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*c6_fw_update_status_callback_t)(const char* status_text);

void c6_fw_update_set_status_callback(c6_fw_update_status_callback_t callback);
esp_err_t c6_fw_update_check_and_apply(void);

#ifdef __cplusplus
}
#endif
