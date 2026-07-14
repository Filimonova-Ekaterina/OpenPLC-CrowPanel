#include "credentials.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

#define NVS_WIFI_CFG_NAMESPACE "wifi_cfg"
#define NVS_KEY_WIFI_SSID      "wifi_ssid"
#define NVS_KEY_WIFI_PASS      "wifi_pass"
#define MAX_PENDING_SSID_LEN   33

static const char* TAG = "credentials";
static char s_pending_ssid[MAX_PENDING_SSID_LEN] = {0};

esp_err_t nvs_storage_init()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS partition...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        ESP_LOGI(TAG, "NVS initialized");
    }
    ESP_ERROR_CHECK(ret);
    return ret;
}

esp_err_t save_wifi_credentials_to_nvs(const char* ssid, const char* password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_CFG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    esp_err_t final_err = ESP_OK;
    
    if (ssid && strlen(ssid) > 0) {
        err = nvs_set_blob(nvs_handle, NVS_KEY_WIFI_SSID, ssid, strlen(ssid) + 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(err));
            final_err = err;
        } else {
            ESP_LOGI(TAG, "Saved SSID to NVS: %s (length: %d)", ssid, (int)strlen(ssid));
        }
    } else {
        ESP_LOGW(TAG, "SSID is empty, not saving");
        final_err = ESP_ERR_INVALID_ARG;
    }

    if (password && strlen(password) > 0) {
        err = nvs_set_blob(nvs_handle, NVS_KEY_WIFI_PASS, password, strlen(password) + 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(err));
            final_err = err;
        } else {
            ESP_LOGI(TAG, "Saved password to NVS (length: %d)", (int)strlen(password));
        }
    } else {
        ESP_LOGW(TAG, "Password is empty, removing from NVS");
        nvs_erase_key(nvs_handle, NVS_KEY_WIFI_PASS);
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    return final_err;
}

esp_err_t load_wifi_credentials_from_nvs(char* ssid, size_t ssid_len, char* password, size_t password_len)
{
    if (!ssid || !password) {
        ESP_LOGE(TAG, "Invalid pointers");
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_CFG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved WiFi credentials found");
        return ESP_FAIL;
    }

    bool ssid_found = false;
    bool pass_found = false;
    
    size_t required_size = 0;
    err = nvs_get_blob(nvs_handle, NVS_KEY_WIFI_SSID, NULL, &required_size);
    if (err == ESP_OK && required_size > 0 && required_size <= ssid_len) {
        err = nvs_get_blob(nvs_handle, NVS_KEY_WIFI_SSID, ssid, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded WiFi SSID from NVS: %s (length: %d)", ssid, (int)strlen(ssid));
            ssid_found = true;
        } else {
            ESP_LOGE(TAG, "Failed to load SSID: %s", esp_err_to_name(err));
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "SSID not found in NVS");
        ssid[0] = '\0';
    } else if (required_size > ssid_len) {
        ESP_LOGW(TAG, "SSID too long (%d > %d)", (int)required_size, (int)ssid_len);
        ssid[0] = '\0';
    }

    required_size = 0;
    err = nvs_get_blob(nvs_handle, NVS_KEY_WIFI_PASS, NULL, &required_size);
    if (err == ESP_OK && required_size > 0 && required_size <= password_len) {
        err = nvs_get_blob(nvs_handle, NVS_KEY_WIFI_PASS, password, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded WiFi password from NVS (length: %d)", (int)strlen(password));
            pass_found = true;
        } else {
            ESP_LOGE(TAG, "Failed to load password: %s", esp_err_to_name(err));
            password[0] = '\0';
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Password not found in NVS");
        password[0] = '\0';
    } else if (required_size > password_len) {
        ESP_LOGW(TAG, "Password too long (%d > %d), clearing", (int)required_size, (int)password_len);
        password[0] = '\0';
    } else {
        password[0] = '\0';
    }

    nvs_close(nvs_handle);
    
    if (ssid_found) {
        if (!pass_found) {
            ESP_LOGW(TAG, "SSID found but no password for it");
        }
        return ESP_OK;
    }
    
    return ESP_FAIL;
}

void credentials_set_pending_ssid(const char* ssid)
{
    if (ssid && strlen(ssid) > 0) {
        strncpy(s_pending_ssid, ssid, MAX_PENDING_SSID_LEN - 1);
        s_pending_ssid[MAX_PENDING_SSID_LEN - 1] = '\0';
        ESP_LOGI(TAG, "Pending SSID set to: %s", s_pending_ssid);
    } else {
        s_pending_ssid[0] = '\0';
        ESP_LOGW(TAG, "Attempted to set empty pending SSID");
    }
}

const char* credentials_get_pending_ssid(void)
{
    if (strlen(s_pending_ssid) > 0) {
        return s_pending_ssid;
    }
    return NULL;
}

void credentials_clear_pending_ssid(void)
{
    s_pending_ssid[0] = '\0';
    ESP_LOGD(TAG, "Pending SSID cleared");
}