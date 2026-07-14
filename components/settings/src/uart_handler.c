#include "uart_handler.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char* UART_TAG = "UART_HANDLER";

#define UART_NUM         UART_NUM_0
#define UART_RX_PIN      38
#define UART_TX_PIN      37
#define UART_BAUD_RATE   115200
#define UART_BUF_SIZE    2048
#define UART_QUEUE_SIZE  20
#define MAX_PASSWORD_LEN 64
#define MAX_KEY_LEN      256
#define MAX_SSID_LEN     33
#define JSON_BUF_SIZE    1024
#define MAX_HA_URL_LEN   256
#define MAX_HA_LOGIN_LEN 65

#define UART_CMD_STOP  (1UL << 0)
#define UART_CMD_START (1UL << 1)
#define UART_CMD_EXIT  (1UL << 2)

static uart_handler_callbacks_t s_callbacks = {0};
static char wifi_password[MAX_PASSWORD_LEN] = {0};
static char wifi_ssid[MAX_SSID_LEN] = {0};
static char weather_api_key[MAX_KEY_LEN] = {0};
static char calendar_api_key[MAX_KEY_LEN] = {0};

static bool password_received = false;
static bool weather_key_received = false;
static bool calendar_key_received = false;
static bool keys_received = false;
static bool ssid_received = false;

static QueueHandle_t uart_queue = NULL;
static TaskHandle_t uart_task_handle = NULL;
static SemaphoreHandle_t s_data_mutex = NULL;
static uint8_t* s_rx_buffer = NULL;

static bool uart_initialized = false;
static bool uart_running = false;
static bool uart_auto_started = false;

static char ha_url[MAX_HA_URL_LEN] = {0};
static char ha_login[MAX_HA_LOGIN_LEN] = {0};
static char ha_password[MAX_PASSWORD_LEN] = {0};
static uint16_t ha_ws_port = 3000;
static bool ha_creds_received = false;

static void uart_event_task(void* arg);
static bool parse_wifi_password_json(const char* json_str);
static bool parse_api_keys_json(const char* json_str);
static bool parse_ha_creds_json(const char* json_str);
static uint32_t calculate_crc32(const char* data);
static void trim_newline(char* s);
static void uart_send_response(const char* response);
static void notify_keys_received(void);

static void trim_newline(char* s)
{
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

static uint32_t calculate_crc32(const char* data)
{
    if (!data) return 0;
    return esp_crc32_le(0, (const uint8_t*)data, strlen(data));
}

static void uart_send_response(const char* response)
{
    if (!uart_initialized || !uart_running) return;

    char full_response[128];
    int len = snprintf(full_response, sizeof(full_response), "%s\n", response);
    if (len > 0) {
        uart_write_bytes(UART_NUM, full_response, len);
        ESP_LOGI(UART_TAG, "TX: %s", response);
    }
}

static void notify_keys_received(void)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool weather_ready = weather_key_received;
    bool calendar_ready = calendar_key_received;
    xSemaphoreGive(s_data_mutex);

    if (weather_ready && calendar_ready) {
        bool weather_key_set_res = false;
        bool calendar_key_set_res = false;

        if (s_callbacks.on_recv_weather_api_key) {
            weather_key_set_res = s_callbacks.on_recv_weather_api_key(s_callbacks.ctx, weather_api_key);
        }
        if (s_callbacks.on_recv_calendar_api_key) {
            calendar_key_set_res = s_callbacks.on_recv_calendar_api_key(s_callbacks.ctx, calendar_api_key);
        }

        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        keys_received = true;
        xSemaphoreGive(s_data_mutex);

        if (weather_key_set_res && calendar_key_set_res) {
            ESP_LOGI(UART_TAG, "API keys successfully passed to main");
            uart_send_response("{\"status\":\"OK\",\"message\":\"API keys received and stored\"}");
        } else {
            ESP_LOGE(UART_TAG, "Failed to pass API keys to main");
            uart_send_response("{\"status\":\"ERROR\",\"message\":\"Failed to store API keys\"}");
        }
    }
}

static bool parse_ha_creds_json(const char* json_str)
{
    if (!json_str || json_str[0] == '\0') {
        ESP_LOGE(UART_TAG, "Empty JSON string for HA creds");
        return false;
    }

    char* json_copy = strdup(json_str);
    if (!json_copy) {
        ESP_LOGE(UART_TAG, "Failed to allocate memory for JSON copy");
        return false;
    }
    trim_newline(json_copy);

    cJSON* root = cJSON_Parse(json_copy);
    if (!root) {
        ESP_LOGE(UART_TAG, "Failed to parse JSON");
        free(json_copy);
        return false;
    }

    cJSON* type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_item) || strcmp(type_item->valuestring, "ha_creds") != 0) {
        ESP_LOGE(UART_TAG, "Invalid message type for HA creds");
        cJSON_Delete(root);
        free(json_copy);
        return false;
    }

    cJSON* url_item = cJSON_GetObjectItem(root, "ha_url");
    cJSON* login_item = cJSON_GetObjectItem(root, "ha_login");
    cJSON* password_item = cJSON_GetObjectItem(root, "ha_password");
    cJSON* ws_port_item = cJSON_GetObjectItem(root, "ha_ws_port");

    const char* url = cJSON_IsString(url_item) ? url_item->valuestring : "";
    const char* login = cJSON_IsString(login_item) ? login_item->valuestring : "";
    const char* password = cJSON_IsString(password_item) ? password_item->valuestring : "";
    uint16_t ws_port = 3000;
    if (cJSON_IsNumber(ws_port_item)) {
        int port = ws_port_item->valueint;
        if (port >= 1 && port <= 65535) {
            ws_port = (uint16_t)port;
        }
    }

    cJSON* crc_item = cJSON_GetObjectItem(root, "crc32");
    if (cJSON_IsString(crc_item)) {
        cJSON* copy = cJSON_Duplicate(root, 1);
        if (copy) {
            cJSON_DeleteItemFromObject(copy, "crc32");
            char* pure_json = cJSON_PrintUnformatted(copy);
            if (pure_json) {
                uint32_t calc_crc = calculate_crc32(pure_json);
                char calc_str[9];
                snprintf(calc_str, sizeof(calc_str), "%08lX", (unsigned long)calc_crc);
                if (strcasecmp(calc_str, crc_item->valuestring) != 0) {
                    ESP_LOGW(UART_TAG, "CRC mismatch for HA creds");
                } else {
                    ESP_LOGI(UART_TAG, "CRC OK for HA creds");
                }
                free(pure_json);
            }
            cJSON_Delete(copy);
        }
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    strncpy(ha_url, url, MAX_HA_URL_LEN - 1);
    ha_url[MAX_HA_URL_LEN - 1] = '\0';
    
    strncpy(ha_login, login, MAX_HA_LOGIN_LEN - 1);
    ha_login[MAX_HA_LOGIN_LEN - 1] = '\0';
    
    strncpy(ha_password, password, MAX_PASSWORD_LEN - 1);
    ha_password[MAX_PASSWORD_LEN - 1] = '\0';
    
    ha_ws_port = ws_port;
    ha_creds_received = true;
    xSemaphoreGive(s_data_mutex);

    ESP_LOGI(UART_TAG, "HA credentials received via UART (ws_port=%u)", ws_port);
    uart_send_response("{\"status\":\"OK\",\"message\":\"HA credentials received\"}");

    if (s_callbacks.on_recv_ha_creds) {
        s_callbacks.on_recv_ha_creds(s_callbacks.ctx, url, login, password);
    }

    cJSON_Delete(root);
    free(json_copy);
    return true;
}

static bool parse_api_keys_json(const char* json_str)
{
    if (!json_str || json_str[0] == '\0') {
        ESP_LOGE(UART_TAG, "Empty JSON string for API keys");
        return false;
    }

    char* json_copy = strdup(json_str);
    if (!json_copy) {
        ESP_LOGE(UART_TAG, "Failed to allocate memory for JSON copy");
        return false;
    }
    trim_newline(json_copy);

    cJSON* root = cJSON_Parse(json_copy);
    if (!root) {
        ESP_LOGE(UART_TAG, "Failed to parse JSON");
        free(json_copy);
        return false;
    }

    cJSON* type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_item) || strcmp(type_item->valuestring, "api_keys") != 0) {
        ESP_LOGE(UART_TAG, "Invalid message type for API keys");
        cJSON_Delete(root);
        free(json_copy);
        return false;
    }

    cJSON* weather_key_item = cJSON_GetObjectItem(root, "weather_key");
    cJSON* calendar_key_item = cJSON_GetObjectItem(root, "calendar_key");

    if (!cJSON_IsString(weather_key_item) || !cJSON_IsString(calendar_key_item)) {
        ESP_LOGE(UART_TAG, "Missing or invalid key fields");
        cJSON_Delete(root);
        free(json_copy);
        return false;
    }

    const char* weather_key = weather_key_item->valuestring;
    const char* calendar_key = calendar_key_item->valuestring;
    size_t weather_len = strlen(weather_key);
    size_t calendar_len = strlen(calendar_key);

    if (weather_len == 0 || calendar_len == 0 || weather_len >= MAX_KEY_LEN || calendar_len >= MAX_KEY_LEN) {
        ESP_LOGE(UART_TAG, "Invalid API key length");
        cJSON_Delete(root);
        free(json_copy);
        return false;
    }

    cJSON* crc_item = cJSON_GetObjectItem(root, "crc32");
    if (cJSON_IsString(crc_item)) {
        cJSON* copy = cJSON_Duplicate(root, 1);
        if (copy) {
            cJSON_DeleteItemFromObject(copy, "crc32");
            char* pure_json = cJSON_PrintUnformatted(copy);
            if (pure_json) {
                uint32_t calc_crc = calculate_crc32(pure_json);
                char calc_str[9];
                snprintf(calc_str, sizeof(calc_str), "%08lX", (unsigned long)calc_crc);
                if (strcasecmp(calc_str, crc_item->valuestring) != 0) {
                    ESP_LOGW(UART_TAG, "CRC mismatch");
                } else {
                    ESP_LOGI(UART_TAG, "CRC OK for API keys");
                }
                free(pure_json);
            }
            cJSON_Delete(copy);
        }
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    strncpy(weather_api_key, weather_key, MAX_KEY_LEN - 1);
    weather_api_key[MAX_KEY_LEN - 1] = '\0';
    weather_key_received = true;

    strncpy(calendar_api_key, calendar_key, MAX_KEY_LEN - 1);
    calendar_api_key[MAX_KEY_LEN - 1] = '\0';
    calendar_key_received = true;
    xSemaphoreGive(s_data_mutex);

    ESP_LOGI(UART_TAG, "API keys received");
    cJSON_Delete(root);
    free(json_copy);

    notify_keys_received();
    return true;
}

static bool parse_wifi_password_json(const char* json_str)
{
    if (!json_str || json_str[0] == '\0') {
        ESP_LOGE(UART_TAG, "Empty JSON string");
        return false;
    }

    char* json_copy = strdup(json_str);
    if (!json_copy) {
        ESP_LOGE(UART_TAG, "Failed to allocate memory for JSON copy");
        return false;
    }
    trim_newline(json_copy);

    cJSON* root = cJSON_Parse(json_copy);
    if (!root) {
        ESP_LOGE(UART_TAG, "Failed to parse JSON");
        free(json_copy);
        return false;
    }

    cJSON* type_item = cJSON_GetObjectItem(root, "type");
    const char* type_str = (cJSON_IsString(type_item)) ? type_item->valuestring : "";

    bool is_wifi_config = (strcmp(type_str, "wifi_password") == 0) || 
                          (strcmp(type_str, "wifi_config") == 0);

    if (!is_wifi_config) {
        ESP_LOGE(UART_TAG, "Invalid message type: %s", type_str);
        cJSON_Delete(root);
        free(json_copy);
        return false;
    }

    cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
    const char* ssid = "";
    if (cJSON_IsString(ssid_item)) {
        ssid = ssid_item->valuestring;
        size_t ssid_len = strlen(ssid);
        
        if (ssid_len > 0 && ssid_len < MAX_SSID_LEN) {
            xSemaphoreTake(s_data_mutex, portMAX_DELAY);
            strlcpy(wifi_ssid, ssid, MAX_SSID_LEN);
            ssid_received = true;
            xSemaphoreGive(s_data_mutex);
            ESP_LOGI(UART_TAG, "WiFi SSID received: %s", wifi_ssid);
        }
    }

    cJSON* password_item = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(password_item)) {
        ESP_LOGE(UART_TAG, "Missing or invalid password field");
        cJSON_Delete(root);
        free(json_copy);
        return false;
    }

    const char* password = password_item->valuestring;
    size_t pass_len = strlen(password);

    if (pass_len == 0 || pass_len >= MAX_PASSWORD_LEN) {
        ESP_LOGE(UART_TAG, "Invalid password length");
        cJSON_Delete(root);
        free(json_copy);
        return false;
    }

    cJSON* crc_item = cJSON_GetObjectItem(root, "crc32");
    if (cJSON_IsString(crc_item)) {
        cJSON* copy = cJSON_Duplicate(root, 1);
        if (copy) {
            cJSON_DeleteItemFromObject(copy, "crc32");
            char* pure_json = cJSON_PrintUnformatted(copy);
            if (pure_json) {
                uint32_t calc_crc = calculate_crc32(pure_json);
                char calc_str[9];
                snprintf(calc_str, sizeof(calc_str), "%08lX", (unsigned long)calc_crc);
                if (strcasecmp(calc_str, crc_item->valuestring) != 0) {
                    ESP_LOGW(UART_TAG, "CRC mismatch");
                } else {
                    ESP_LOGI(UART_TAG, "CRC OK");
                }
                free(pure_json);
            }
            cJSON_Delete(copy);
        }
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    strlcpy(wifi_password, password, MAX_PASSWORD_LEN);
    password_received = true;
    xSemaphoreGive(s_data_mutex);

    ESP_LOGI(UART_TAG, "WiFi credentials received via UART (SSID: %s)", 
             ssid_received ? wifi_ssid : "not provided");
    uart_send_response("{\"status\":\"OK\",\"message\":\"WiFi credentials received\"}");

    if (s_callbacks.on_recv_wifi_password) {
        s_callbacks.on_recv_wifi_password(s_callbacks.ctx, 
                                          ssid_received ? wifi_ssid : NULL,
                                          password);
    }

    cJSON_Delete(root);
    free(json_copy);
    return true;
}

static void uart_event_task(void* arg)
{
    ESP_LOGI(UART_TAG, "UART event task started");
    size_t buffered = 0;
    bool listening = false;

    while (1) {
        uart_event_t event;
        BaseType_t queue_result = xQueueReceive(uart_queue, &event, pdMS_TO_TICKS(100));

        uint32_t notification_value = 0;
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &notification_value, 0) == pdTRUE) {
            if (notification_value & UART_CMD_EXIT) {
                ESP_LOGI(UART_TAG, "Exit command received, task terminating");
                break;
            }
            if (notification_value & UART_CMD_STOP) {
                ESP_LOGI(UART_TAG, "Stop command received");
                listening = false;
                uart_running = false;
                continue;
            }
            if (notification_value & UART_CMD_START) {
                ESP_LOGI(UART_TAG, "Start command received");
                listening = true;
                uart_running = true;
                vTaskDelay(200 / portTICK_PERIOD_MS);
                uart_flush(UART_NUM);
                uart_send_response("UART_READY");
                continue;
            }
        }

        if (!listening) {
            continue;
        }

        if (queue_result != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case UART_DATA: {
            int len = uart_read_bytes(UART_NUM, &s_rx_buffer[buffered], event.size, pdMS_TO_TICKS(100));
            if (len > 0) {
                buffered += len;
            }

            if (buffered >= JSON_BUF_SIZE - 1) {
                ESP_LOGW(UART_TAG, "Buffer overflow, clearing");
                uart_send_response("ERROR: Buffer overflow");
                buffered = 0;
                memset(s_rx_buffer, 0, JSON_BUF_SIZE);
                break;
            }

            if (event.timeout_flag) {
                if (buffered > 0) {
                    s_rx_buffer[buffered] = '\0';
                    ESP_LOGI(UART_TAG, "Received %d bytes", buffered);

                    char* json_start = (char*)s_rx_buffer;
                    bool json_found = false;

                    while (*json_start &&
                           ((unsigned char)*json_start < 32 && *json_start != '\n' && *json_start != '\r')) {
                        json_start++;
                    }

                    if (*json_start == '{') {
                        json_found = true;
                    } else {
                        const char* patterns[] = {"{\"type\":\"wifi_password\"", "{\"type\":\"wifi_config\"",
                                                  "{\"type\":\"api_keys\"", "{\"type\":\"ha_creds\"",
                                                  "{\"password\"", NULL};
                        for (int i = 0; patterns[i] != NULL; i++) {
                            char* possible_json = strstr(json_start, patterns[i]);
                            if (possible_json) {
                                json_start = possible_json;
                                json_found = true;
                                break;
                            }
                        }
                    }

                    if (json_found && strlen(json_start) > 10) {
                        ESP_LOGI(UART_TAG, "Processing JSON: %s", json_start);

                        bool success = false;
                        if (strstr(json_start, "\"type\":\"api_keys\"") != NULL) {
                            success = parse_api_keys_json(json_start);
                            if (success)
                                uart_send_response("{\"status\":\"OK\",\"message\":\"API keys received\"}");
                        } else if (strstr(json_start, "\"type\":\"wifi_password\"") != NULL ||
                                   strstr(json_start, "\"type\":\"wifi_config\"") != NULL) {
                            success = parse_wifi_password_json(json_start);
                        } else if (strstr(json_start, "\"type\":\"ha_creds\"") != NULL) {
                            success = parse_ha_creds_json(json_start);
                        } else {
                            if (strstr(json_start, "\"password\"") != NULL) {
                                success = parse_wifi_password_json(json_start);
                            }
                        }

                        if (!success) {
                            ESP_LOGE(UART_TAG, "Failed to parse JSON");
                            uart_send_response("{\"status\":\"ERROR\",\"message\":\"Invalid format\"}");
                        }
                    } else {
                        ESP_LOGW(UART_TAG, "No valid JSON found in buffer");
                    }
                }
                buffered = 0;
                memset(s_rx_buffer, 0, JSON_BUF_SIZE);
            }
            break;
        }
        case UART_FIFO_OVF:
            ESP_LOGW(UART_TAG, "UART FIFO overflow");
            uart_flush_input(UART_NUM);
            buffered = 0;
            memset(s_rx_buffer, 0, JSON_BUF_SIZE);
            break;
        case UART_BUFFER_FULL:
            ESP_LOGW(UART_TAG, "UART buffer full");
            uart_flush_input(UART_NUM);
            buffered = 0;
            memset(s_rx_buffer, 0, JSON_BUF_SIZE);
            break;
        default:
            break;
        }
    }

    vTaskDelete(NULL);
}

void uart_password_handler_init(const uart_handler_callbacks_t* callbacks)
{
    if (uart_initialized) {
        ESP_LOGW(UART_TAG, "UART handler already initialized");
        return;
    }

    ESP_LOGI(UART_TAG, "Initializing UART password handler");

    s_data_mutex = xSemaphoreCreateMutex();
    if (!s_data_mutex) {
        ESP_LOGE(UART_TAG, "Failed to create mutex");
        return;
    }

    s_rx_buffer = (uint8_t*)malloc(JSON_BUF_SIZE);
    if (!s_rx_buffer) {
        ESP_LOGE(UART_TAG, "Failed to allocate RX buffer");
        vSemaphoreDelete(s_data_mutex);
        s_data_mutex = NULL;
        return;
    }
    memset(s_rx_buffer, 0, JSON_BUF_SIZE);

    uart_queue = xQueueCreate(UART_QUEUE_SIZE, sizeof(uart_event_t));
    if (!uart_queue) {
        ESP_LOGE(UART_TAG, "Failed to create UART queue");
        free(s_rx_buffer);
        s_rx_buffer = NULL;
        vSemaphoreDelete(s_data_mutex);
        s_data_mutex = NULL;
        return;
    }

    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(UART_TAG, "Failed to configure UART parameters: %s", esp_err_to_name(err));
        vQueueDelete(uart_queue);
        uart_queue = NULL;
        free(s_rx_buffer);
        s_rx_buffer = NULL;
        vSemaphoreDelete(s_data_mutex);
        s_data_mutex = NULL;
        return;
    }

    err = uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(UART_TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        vQueueDelete(uart_queue);
        uart_queue = NULL;
        free(s_rx_buffer);
        s_rx_buffer = NULL;
        vSemaphoreDelete(s_data_mutex);
        s_data_mutex = NULL;
        return;
    }

    err = uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, UART_QUEUE_SIZE, &uart_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(UART_TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        vQueueDelete(uart_queue);
        uart_queue = NULL;
        free(s_rx_buffer);
        s_rx_buffer = NULL;
        vSemaphoreDelete(s_data_mutex);
        s_data_mutex = NULL;
        return;
    }

    if (callbacks) {
        s_callbacks = *callbacks;
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    password_received = false;
    weather_key_received = false;
    calendar_key_received = false;
    keys_received = false;
    ha_creds_received = false;
    ha_ws_port = 3000;
    memset(wifi_password, 0, sizeof(wifi_password));
    memset(weather_api_key, 0, sizeof(weather_api_key));
    memset(calendar_api_key, 0, sizeof(calendar_api_key));
    memset(ha_url, 0, sizeof(ha_url));
    memset(ha_login, 0, sizeof(ha_login));
    memset(ha_password, 0, sizeof(ha_password));
    xSemaphoreGive(s_data_mutex);

    BaseType_t ret = xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 1, &uart_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(UART_TAG, "Failed to create UART task");
        vQueueDelete(uart_queue);
        uart_queue = NULL;
        free(s_rx_buffer);
        s_rx_buffer = NULL;
        vSemaphoreDelete(s_data_mutex);
        s_data_mutex = NULL;
        return;
    }

    uart_initialized = true;
    uart_running = false;
    ESP_LOGI(UART_TAG, "UART handler initialized (stopped)");
}

void uart_password_handler_deinit(void)
{
    if (!uart_initialized) {
        return;
    }

    ESP_LOGI(UART_TAG, "Deinitializing UART handler");

    if (uart_task_handle) {
        xTaskNotify(uart_task_handle, UART_CMD_EXIT, eSetBits);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        uart_task_handle = NULL;
    }

    if (uart_queue) {
        uart_event_t event;
        while (xQueueReceive(uart_queue, &event, 0) == pdTRUE) {
        }
        vQueueDelete(uart_queue);
        uart_queue = NULL;
    }

    uart_driver_delete(UART_NUM);

    if (s_rx_buffer) {
        free(s_rx_buffer);
        s_rx_buffer = NULL;
    }

    if (s_data_mutex) {
        vSemaphoreDelete(s_data_mutex);
        s_data_mutex = NULL;
    }

    uart_initialized = false;
    uart_running = false;
    ESP_LOGI(UART_TAG, "UART handler deinitialized");
}

void uart_password_handler_start(void)
{
    if (!uart_initialized) {
        ESP_LOGE(UART_TAG, "Handler not initialized. Call init first.");
        return;
    }
    if (uart_running) {
        ESP_LOGW(UART_TAG, "Handler already running");
        return;
    }

    ESP_LOGI(UART_TAG, "Starting UART handler...");
    xTaskNotify(uart_task_handle, UART_CMD_START, eSetBits);
    ESP_LOGI(UART_TAG, "UART handler started");
}

void uart_password_handler_stop(void)
{
    if (!uart_initialized || !uart_running) {
        return;
    }

    ESP_LOGI(UART_TAG, "Stopping UART handler...");
    xTaskNotify(uart_task_handle, UART_CMD_STOP, eSetBits);
    uart_running = false;
    ESP_LOGI(UART_TAG, "UART handler stopped");
}

bool uart_password_handler_is_active(void)
{
    return uart_initialized && uart_running;
}

bool uart_password_handler_is_initialized(void)
{
    return uart_initialized;
}

const char* uart_password_handler_get_ssid(void)
{
    if (!s_data_mutex) return NULL;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool received = ssid_received;
    xSemaphoreGive(s_data_mutex);

    if (received) {
        return wifi_ssid;
    }
    return NULL;
}

const char* uart_password_handler_get_password(void)
{
    if (!s_data_mutex) return NULL;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool received = password_received;
    xSemaphoreGive(s_data_mutex);

    if (received) {
        return wifi_password;
    }
    return NULL;
}

bool uart_password_handler_keys_received(void)
{
    if (!s_data_mutex) return false;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool received = keys_received;
    xSemaphoreGive(s_data_mutex);
    return received;
}

const char* uart_password_handler_get_weather_key(void)
{
    if (!s_data_mutex) return NULL;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool received = weather_key_received;
    xSemaphoreGive(s_data_mutex);

    if (received) {
        return weather_api_key;
    }
    return NULL;
}

const char* uart_password_handler_get_calendar_key(void)
{
    if (!s_data_mutex) return NULL;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool received = calendar_key_received;
    xSemaphoreGive(s_data_mutex);

    if (received) {
        return calendar_api_key;
    }
    return NULL;
}

void uart_password_handler_clear(void)
{
    if (!s_data_mutex) return;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    memset(wifi_password, 0, sizeof(wifi_password));
    memset(wifi_ssid, 0, sizeof(wifi_ssid));
    password_received = false;
    ssid_received = false;
    xSemaphoreGive(s_data_mutex);
}

bool uart_password_handler_has_new_password(void)
{
    if (!s_data_mutex) return false;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool has_new = password_received;
    if (has_new) {
        password_received = false;
    }
    xSemaphoreGive(s_data_mutex);
    return has_new;
}

bool uart_password_handler_ha_creds_received(void)
{
    if (!s_data_mutex) return false;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool received = ha_creds_received;
    xSemaphoreGive(s_data_mutex);
    return received;
}

const char* uart_password_handler_get_ha_url(void)
{
    if (!s_data_mutex) return NULL;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool received = ha_creds_received;
    xSemaphoreGive(s_data_mutex);

    if (received) {
        return ha_url;
    }
    return NULL;
}

const char* uart_password_handler_get_ha_login(void)
{
    if (!s_data_mutex) return NULL;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool received = ha_creds_received;
    xSemaphoreGive(s_data_mutex);

    if (received) {
        return ha_login;
    }
    return NULL;
}

const char* uart_password_handler_get_ha_password(void)
{
    if (!s_data_mutex) return NULL;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool received = ha_creds_received;
    xSemaphoreGive(s_data_mutex);

    if (received) {
        return ha_password;
    }
    return NULL;
}

uint16_t uart_password_handler_get_ha_ws_port(void)
{
    if (!s_data_mutex) return 3000;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    uint16_t port = ha_ws_port;
    xSemaphoreGive(s_data_mutex);
    return port;
}

void uart_password_handler_set_auto_started(bool auto_started)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    uart_auto_started = auto_started;
    xSemaphoreGive(s_data_mutex);
}

bool uart_password_handler_was_auto_started(void)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    bool was = uart_auto_started;
    xSemaphoreGive(s_data_mutex);
    return was;
}