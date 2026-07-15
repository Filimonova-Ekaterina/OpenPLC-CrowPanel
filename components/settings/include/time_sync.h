#pragma once

/**
 * @file time_sync.h
 * @brief System wall-clock synchronization required by OPC UA timestamps.
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t time_sync_init(void);
bool time_sync_is_valid(void);

#ifdef __cplusplus
}
#endif
