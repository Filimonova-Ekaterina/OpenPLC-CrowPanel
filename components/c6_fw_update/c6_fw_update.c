#include "c6_fw_update.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const uint8_t network_adapter_esp32c6_bin_start[] asm("_binary_network_adapter_esp32c6_bin_start");
extern const uint8_t network_adapter_esp32c6_bin_end[] asm("_binary_network_adapter_esp32c6_bin_end");

static const char* TAG = "c6_fw_update";
static c6_fw_update_status_callback_t s_status_callback = NULL;

#define C6_FW_UPDATE_CHUNK_SIZE 1400U
#define C6_FW_UPDATE_DESIRED_MAJOR 2U
#define C6_FW_UPDATE_DESIRED_MINOR 11U
#define C6_FW_UPDATE_DESIRED_PATCH 0U
#define C6_FW_UPDATE_EXPECTED_PROJECT_NAME "network_adapter"

typedef struct
{
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} c6_fw_version_t;

void c6_fw_update_set_status_callback(c6_fw_update_status_callback_t callback)
{
    s_status_callback = callback;
}

static void notify_status(const char* status_text)
{
    if (s_status_callback != NULL) {
        s_status_callback(status_text);
    }
}

static bool versions_equal(const esp_hosted_coprocessor_fwver_t* current, const c6_fw_version_t* desired)
{
    return current->major1 == desired->major && current->minor1 == desired->minor && current->patch1 == desired->patch;
}

static bool slave_supports_activate(const esp_hosted_coprocessor_fwver_t* current)
{
    return current->major1 > 2 || (current->major1 == 2 && current->minor1 > 5);
}

static esp_err_t parse_embedded_image(size_t* image_size, char* app_version_text, size_t app_version_text_len,
                                      char* project_name_text, size_t project_name_text_len)
{
    const uint8_t* image = network_adapter_esp32c6_bin_start;
    const size_t embedded_size =
        (size_t)(network_adapter_esp32c6_bin_end - network_adapter_esp32c6_bin_start);
    esp_image_header_t image_header;
    esp_app_desc_t app_desc;
    const size_t app_desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);

    if (embedded_size < sizeof(image_header)) {
        ESP_LOGE(TAG, "Embedded C6 firmware is too small");
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&image_header, image, sizeof(image_header));
    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "Invalid C6 firmware magic: 0x%02x", image_header.magic);
        return ESP_ERR_INVALID_ARG;
    }

    if (app_desc_offset + sizeof(app_desc) > embedded_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(&app_desc, image + app_desc_offset, sizeof(app_desc));
    strlcpy(app_version_text, app_desc.version, app_version_text_len);
    strlcpy(project_name_text, app_desc.project_name, project_name_text_len);

    if (strcmp(project_name_text, C6_FW_UPDATE_EXPECTED_PROJECT_NAME) != 0) {
        ESP_LOGE(TAG, "Embedded C6 image project is '%s', expected '%s'", project_name_text,
                 C6_FW_UPDATE_EXPECTED_PROJECT_NAME);
        return ESP_ERR_INVALID_ARG;
    }

    *image_size = embedded_size;
    return ESP_OK;
}

static esp_err_t write_embedded_image_to_slave(size_t image_size)
{
    const uint8_t* image = network_adapter_esp32c6_bin_start;
    size_t offset        = 0;

    ESP_RETURN_ON_ERROR(esp_hosted_slave_ota_begin(), TAG, "Failed to begin C6 OTA");

    while (offset < image_size) {
        const size_t remaining = image_size - offset;
        const size_t chunk_len = remaining > C6_FW_UPDATE_CHUNK_SIZE ? C6_FW_UPDATE_CHUNK_SIZE : remaining;
        esp_err_t ret = esp_hosted_slave_ota_write((uint8_t*)(image + offset), (uint32_t)chunk_len);
        if (ret != ESP_OK) {
            (void)esp_hosted_slave_ota_end();
            ESP_LOGE(TAG, "Failed to write C6 OTA chunk at offset %u: %s", (unsigned int)offset,
                     esp_err_to_name(ret));
            return ret;
        }
        offset += chunk_len;
    }

    ESP_RETURN_ON_ERROR(esp_hosted_slave_ota_end(), TAG, "Failed to finish C6 OTA");
    return ESP_OK;
}

esp_err_t c6_fw_update_check_and_apply(void)
{
    esp_hosted_coprocessor_fwver_t current = {0};
    const c6_fw_version_t desired = {
        .major = C6_FW_UPDATE_DESIRED_MAJOR,
        .minor = C6_FW_UPDATE_DESIRED_MINOR,
        .patch = C6_FW_UPDATE_DESIRED_PATCH,
    };
    char app_version_text[32] = {0};
    char project_name_text[32] = {0};
    size_t image_size                      = 0;

    ESP_LOGI(TAG, "Checking ESP32-C6 firmware");

    ESP_RETURN_ON_ERROR(parse_embedded_image(&image_size, app_version_text, sizeof(app_version_text),
                                             project_name_text, sizeof(project_name_text)),
                        TAG, "Invalid embedded C6 firmware image");

    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "Failed to initialize ESP-Hosted");
    ESP_RETURN_ON_ERROR(esp_hosted_connect_to_slave(), TAG, "Failed to connect to ESP32-C6");

    esp_err_t ret = esp_hosted_get_coprocessor_fwversion(&current);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not read current C6 firmware version: %s. Trying OTA anyway", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Current C6 firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32, current.major1, current.minor1,
                 current.patch1);
        ESP_LOGI(TAG, "Desired C6 Hosted firmware: %u.%u.%u (image project: %s, app version: %s)",
                 (unsigned int)desired.major, (unsigned int)desired.minor, (unsigned int)desired.patch,
                 project_name_text, app_version_text);

        if (versions_equal(&current, &desired)) {
            ESP_LOGI(TAG, "ESP32-C6 firmware is already up to date");
            return ESP_OK;
        }
    }

    notify_status("Upgrading WiFi module...");
    ESP_LOGW(TAG, "Updating ESP32-C6 firmware to %u.%u.%u (%u bytes)", (unsigned int)desired.major,
             (unsigned int)desired.minor, (unsigned int)desired.patch, (unsigned int)image_size);
    ESP_RETURN_ON_ERROR(write_embedded_image_to_slave(image_size), TAG, "C6 OTA failed");

    if (ret == ESP_OK && slave_supports_activate(&current)) {
        ESP_RETURN_ON_ERROR(esp_hosted_slave_ota_activate(), TAG, "Failed to activate C6 OTA firmware");
    } else {
        ESP_LOGI(TAG, "Skipping explicit C6 OTA activate; current firmware does not report support");
    }

    ESP_LOGW(TAG, "ESP32-C6 firmware update complete. Restarting host to resync");
    notify_status("WiFi module updated\nRestarting...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}
