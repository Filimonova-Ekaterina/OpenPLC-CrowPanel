#include "opcua_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "opcua_open62541.h"
#include "time_sync.h"
#include "wifi_ctrl.h"

#define OPCUA_WRITE_QUEUE_LENGTH 12
#define OPCUA_STATUS_LENGTH 128
#define OPCUA_EVENT_FIELD_COUNT 4

typedef enum
{
    WRITE_REQUEST_BOOLEAN,
    WRITE_REQUEST_NUMBER,
} write_request_type_t;

typedef struct
{
    size_t tag_index;
    write_request_type_t type;
    bool boolean_value;
    double numeric_value;
} write_request_t;

struct opcua_client
{
    opcua_client_config_t configuration;
    char endpoint_url[OPCUA_CLIENT_ENDPOINT_LENGTH];
    data_model_t* data_model;
    UA_Client* ua_client;
    UA_NodeId* tag_node_ids;
    UA_NodeId* equipment_node_ids;
    UA_UInt16* tag_builtin_type_indices;
    size_t mapped_tag_count;
    size_t mapped_equipment_count;
    UA_UInt32 subscription_id;
    QueueHandle_t write_queue;
    SemaphoreHandle_t status_mutex;
    SemaphoreHandle_t configuration_mutex;
    TaskHandle_t task_handle;
    volatile bool stop_requested;
    volatile bool pause_requested;
    uint32_t pause_depth;
    volatile bool reconnect_requested;
    opcua_client_state_t state;
    char status[OPCUA_STATUS_LENGTH];
};

static const char* TAG = "opcua_client";

static void client_task(void* argument);
static bool network_is_ready(void);
static void wait_for_retry(opcua_client_t* context);
static void set_status(opcua_client_t* context, opcua_client_state_t state, const char* details);
static UA_StatusCode discover_address_space(opcua_client_t* context);
static UA_StatusCode browse_children(opcua_client_t* context, const UA_NodeId* parent_node_id,
                                     size_t parent_equipment_index, unsigned depth);
static UA_StatusCode process_child_references(opcua_client_t* context, const UA_BrowseResult* browse_result,
                                              size_t parent_equipment_index, unsigned depth);
static void release_continuation_point(opcua_client_t* context, UA_ByteString* continuation_point);
static UA_StatusCode add_variable(opcua_client_t* context, const UA_ReferenceDescription* reference,
                                  size_t equipment_index, unsigned depth);
static void read_object_metadata(opcua_client_t* context, const UA_NodeId* object_node_id,
                                 data_model_equipment_t* equipment);
static void read_variable_metadata(opcua_client_t* context, const UA_NodeId* variable_node_id, data_model_tag_t* tag);
static void create_subscriptions(opcua_client_t* context);
static void create_event_subscriptions(opcua_client_t* context);
static void process_write_requests(opcua_client_t* context);
static void clear_tag_node_ids(opcua_client_t* context);
static data_model_type_t map_data_type(const UA_NodeId* data_type_id);
static UA_UInt16 map_builtin_type_index(const UA_NodeId* data_type_id);
static bool variant_to_model_value(const UA_Variant* variant, data_model_type_t data_type,
                                   data_model_value_t* value_out);
static void node_id_to_text(const UA_NodeId* node_id, char* buffer, size_t buffer_size);
static void ua_string_to_text(const UA_String* source, char* buffer, size_t buffer_size);
static void event_notification_callback(UA_Client* client, UA_UInt32 subscription_id, void* subscription_context,
                                        UA_UInt32 monitored_item_id, void* monitored_item_context,
                                        size_t event_field_count, UA_Variant* event_fields);
static void configure_event_field(UA_SimpleAttributeOperand* operand, const char* browse_name);

static bool is_ns0_numeric_node(const UA_NodeId* node_id, UA_UInt32 identifier)
{
    return node_id != NULL && node_id->namespaceIndex == 0 && node_id->identifierType == UA_NODEIDTYPE_NUMERIC &&
           node_id->identifier.numeric == identifier;
}

static data_model_entity_kind_t parse_entity_kind(const char* value)
{
    if (value == NULL) {
        return DATA_MODEL_ENTITY_UNKNOWN;
    }
    if (strcmp(value, "System") == 0) {
        return DATA_MODEL_ENTITY_SYSTEM;
    }
    if (strcmp(value, "Process") == 0) {
        return DATA_MODEL_ENTITY_PROCESS;
    }
    if (strcmp(value, "ActiveEquipment") == 0) {
        return DATA_MODEL_ENTITY_ACTIVE_EQUIPMENT;
    }
    if (strcmp(value, "PassiveEquipment") == 0) {
        return DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT;
    }
    if (strcmp(value, "Group") == 0) {
        return DATA_MODEL_ENTITY_GROUP;
    }
    return DATA_MODEL_ENTITY_UNKNOWN;
}

/** Compare full OPC UA NodeIds so aliases in the address-space graph are mapped only once. */
static size_t find_mapped_node_id(const UA_NodeId* mapped_ids, size_t mapped_count, const UA_NodeId* candidate)
{
    for (size_t index = 0; index < mapped_count; ++index) {
        if (UA_NodeId_equal(&mapped_ids[index], candidate)) {
            return index;
        }
    }
    return DATA_MODEL_INVALID_INDEX;
}

esp_err_t opcua_client_create(const opcua_client_config_t* configuration, data_model_t* data_model,
                              opcua_client_t** client_out)
{
    if (configuration == NULL || configuration->endpoint_url == NULL || data_model == NULL || client_out == NULL ||
        configuration->maximum_equipment_objects == 0 || configuration->maximum_tags == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    opcua_client_t* context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return ESP_ERR_NO_MEM;
    }

    context->tag_node_ids = calloc(configuration->maximum_tags, sizeof(*context->tag_node_ids));
    context->equipment_node_ids =
        calloc(configuration->maximum_equipment_objects, sizeof(*context->equipment_node_ids));
    context->tag_builtin_type_indices =
        malloc(configuration->maximum_tags * sizeof(*context->tag_builtin_type_indices));
    context->write_queue         = xQueueCreate(OPCUA_WRITE_QUEUE_LENGTH, sizeof(write_request_t));
    context->status_mutex        = xSemaphoreCreateMutex();
    context->configuration_mutex = xSemaphoreCreateMutex();
    if (strlen(configuration->endpoint_url) >= sizeof(context->endpoint_url) || context->tag_node_ids == NULL ||
        context->equipment_node_ids == NULL ||
        context->tag_builtin_type_indices == NULL || context->write_queue == NULL || context->status_mutex == NULL ||
        context->configuration_mutex == NULL) {
        free(context->tag_node_ids);
        free(context->equipment_node_ids);
        free(context->tag_builtin_type_indices);
        if (context->write_queue != NULL) {
            vQueueDelete(context->write_queue);
        }
        if (context->status_mutex != NULL) {
            vSemaphoreDelete(context->status_mutex);
        }
        if (context->configuration_mutex != NULL) {
            vSemaphoreDelete(context->configuration_mutex);
        }
        free(context);
        return strlen(configuration->endpoint_url) >= OPCUA_CLIENT_ENDPOINT_LENGTH ? ESP_ERR_INVALID_SIZE
                                                                                   : ESP_ERR_NO_MEM;
    }

    for (size_t index = 0; index < configuration->maximum_tags; ++index) {
        context->tag_builtin_type_indices[index] = UINT16_MAX;
    }

    snprintf(context->endpoint_url, sizeof(context->endpoint_url), "%s", configuration->endpoint_url);
    context->configuration              = *configuration;
    context->configuration.endpoint_url = context->endpoint_url;
    context->data_model                 = data_model;
    set_status(context, OPCUA_CLIENT_STOPPED, "Client is stopped");
    *client_out = context;
    return ESP_OK;
}

esp_err_t opcua_client_start(opcua_client_t* client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (client->task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    client->stop_requested = false;
    client->pause_requested = false;
    client->pause_depth = 0;
    BaseType_t created     = xTaskCreate(client_task, "opcua_client", client->configuration.task_stack_size, client,
                                         client->configuration.task_priority, &client->task_handle);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void opcua_client_stop(opcua_client_t* client)
{
    if (client != NULL) {
        client->stop_requested = true;
    }
}

void opcua_client_pause(opcua_client_t* client)
{
    if (client != NULL) {
        xSemaphoreTake(client->configuration_mutex, portMAX_DELAY);
        client->pause_depth++;
        client->pause_requested = true;
        client->reconnect_requested = true;
        uint32_t pause_depth = client->pause_depth;
        xSemaphoreGive(client->configuration_mutex);
        ESP_LOGI(TAG, "Network activity paused (depth=%" PRIu32 ")", pause_depth);
    }
}

void opcua_client_resume(opcua_client_t* client)
{
    if (client != NULL) {
        xSemaphoreTake(client->configuration_mutex, portMAX_DELAY);
        if (client->pause_depth > 0) {
            client->pause_depth--;
        }
        client->pause_requested = client->pause_depth > 0;
        uint32_t pause_depth = client->pause_depth;
        xSemaphoreGive(client->configuration_mutex);
        ESP_LOGI(TAG, "Network pause released (depth=%" PRIu32 ")", pause_depth);
    }
}

esp_err_t opcua_client_write_boolean(opcua_client_t* client, size_t tag_index, bool value)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    write_request_t request = {
        .tag_index     = tag_index,
        .type          = WRITE_REQUEST_BOOLEAN,
        .boolean_value = value,
    };
    return xQueueSend(client->write_queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t opcua_client_write_number(opcua_client_t* client, size_t tag_index, double value)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    write_request_t request = {
        .tag_index     = tag_index,
        .type          = WRITE_REQUEST_NUMBER,
        .numeric_value = value,
    };
    return xQueueSend(client->write_queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t opcua_client_set_endpoint(opcua_client_t* client, const char* endpoint_url)
{
    if (client == NULL || endpoint_url == NULL || strncmp(endpoint_url, "opc.tcp://", 10) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(endpoint_url) >= sizeof(client->endpoint_url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(client->configuration_mutex, portMAX_DELAY);
    snprintf(client->endpoint_url, sizeof(client->endpoint_url), "%s", endpoint_url);
    client->configuration.endpoint_url = client->endpoint_url;
    client->reconnect_requested        = true;
    xSemaphoreGive(client->configuration_mutex);
    ESP_LOGI(TAG, "Endpoint updated; reconnect requested");
    return ESP_OK;
}

void opcua_client_get_endpoint(const opcua_client_t* client, char* buffer, size_t buffer_size)
{
    if (client == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }
    xSemaphoreTake(client->configuration_mutex, portMAX_DELAY);
    snprintf(buffer, buffer_size, "%s", client->endpoint_url);
    xSemaphoreGive(client->configuration_mutex);
}

opcua_client_state_t opcua_client_get_state(const opcua_client_t* client)
{
    if (client == NULL || client->status_mutex == NULL) {
        return OPCUA_CLIENT_STOPPED;
    }
    xSemaphoreTake(client->status_mutex, portMAX_DELAY);
    opcua_client_state_t state = client->state;
    xSemaphoreGive(client->status_mutex);
    return state;
}

void opcua_client_get_status(const opcua_client_t* client, char* buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    if (client == NULL || client->status_mutex == NULL) {
        snprintf(buffer, buffer_size, "OPC UA client unavailable");
        return;
    }
    xSemaphoreTake(client->status_mutex, portMAX_DELAY);
    snprintf(buffer, buffer_size, "%s", client->status);
    xSemaphoreGive(client->status_mutex);
}

const char* opcua_client_state_name(opcua_client_state_t state)
{
    switch (state) {
    case OPCUA_CLIENT_WAITING_FOR_WIFI:
        return "WAITING_FOR_WIFI";
    case OPCUA_CLIENT_PAUSED:
        return "PAUSED";
    case OPCUA_CLIENT_WAITING_FOR_TIME:
        return "WAITING_FOR_TIME";
    case OPCUA_CLIENT_CONNECTING:
        return "CONNECTING";
    case OPCUA_CLIENT_BROWSING:
        return "BROWSING";
    case OPCUA_CLIENT_CONNECTED:
        return "CONNECTED";
    case OPCUA_CLIENT_CONNECTION_ERROR:
        return "CONNECTION_ERROR";
    case OPCUA_CLIENT_BROWSE_ERROR:
        return "BROWSE_ERROR";
    default:
        return "STOPPED";
    }
}

static void client_task(void* argument)
{
    opcua_client_t* context = argument;
    char endpoint_url[OPCUA_CLIENT_ENDPOINT_LENGTH];

    while (! context->stop_requested) {
        while (context->pause_requested && ! context->stop_requested) {
            set_status(context, OPCUA_CLIENT_PAUSED, "Paused while settings are open");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (context->stop_requested) {
            break;
        }
        while (! network_is_ready() && ! context->stop_requested) {
            if (context->pause_requested) {
                break;
            }
            set_status(context, OPCUA_CLIENT_WAITING_FOR_WIFI, "Waiting for Wi-Fi and an IP address");
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (context->stop_requested) {
            break;
        }
        if (context->pause_requested) {
            continue;
        }

        while (!time_sync_is_valid() && !context->stop_requested && !context->pause_requested) {
            set_status(context, OPCUA_CLIENT_WAITING_FOR_TIME, "Waiting for SNTP clock synchronization");
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (context->stop_requested) {
            break;
        }
        if (context->pause_requested) {
            continue;
        }

        xSemaphoreTake(context->configuration_mutex, portMAX_DELAY);
        snprintf(endpoint_url, sizeof(endpoint_url), "%s", context->endpoint_url);
        context->reconnect_requested = false;
        xSemaphoreGive(context->configuration_mutex);
        set_status(context, OPCUA_CLIENT_CONNECTING, endpoint_url);
        context->ua_client = UA_Client_new();
        if (context->ua_client == NULL) {
            set_status(context, OPCUA_CLIENT_CONNECTION_ERROR, "Cannot allocate open62541 client");
            wait_for_retry(context);
            continue;
        }

        UA_ClientConfig* client_configuration = UA_Client_getConfig(context->ua_client);
        UA_ClientConfig_setDefault(client_configuration);
        client_configuration->timeout = context->configuration.connection_timeout_ms;

        UA_StatusCode status = UA_Client_connect(context->ua_client, endpoint_url);
        if (status != UA_STATUSCODE_GOOD) {
            char details[96];
            snprintf(details, sizeof(details), "Connect failed: %s", UA_StatusCode_name(status));
            ESP_LOGE(TAG, "%s", details);
            set_status(context, OPCUA_CLIENT_CONNECTION_ERROR, details);
            UA_Client_delete(context->ua_client);
            context->ua_client = NULL;
            wait_for_retry(context);
            continue;
        }

        set_status(context, OPCUA_CLIENT_BROWSING, "Reading OPC UA Address Space");
        status = discover_address_space(context);
        if (status != UA_STATUSCODE_GOOD) {
            char details[96];
            snprintf(details, sizeof(details), "Browse failed: %s", UA_StatusCode_name(status));
            ESP_LOGE(TAG, "%s", details);
            set_status(context, OPCUA_CLIENT_BROWSE_ERROR, details);
            UA_Client_disconnect(context->ua_client);
            UA_Client_delete(context->ua_client);
            context->ua_client = NULL;
            wait_for_retry(context);
            continue;
        }

        create_subscriptions(context);
        char connected_details[96];
        snprintf(connected_details, sizeof(connected_details), "Connected: %u objects, %u tags",
                 (unsigned)data_model_object_count(context->data_model),
                 (unsigned)data_model_tag_count(context->data_model));
        set_status(context, OPCUA_CLIENT_CONNECTED, connected_details);
        ESP_LOGI(TAG, "%s", connected_details);

        while (! context->stop_requested && ! context->pause_requested &&
               ! context->reconnect_requested && network_is_ready()) {
            process_write_requests(context);
            status = UA_Client_run_iterate(context->ua_client, 100);
            if (status != UA_STATUSCODE_GOOD) {
                ESP_LOGW(TAG, "Connection iteration failed: %s", UA_StatusCode_name(status));
                break;
            }
            UA_SecureChannelState channel_state;
            UA_SessionState session_state;
            UA_StatusCode connect_status;
            UA_Client_getState(context->ua_client, &channel_state, &session_state, &connect_status);
            if (channel_state != UA_SECURECHANNELSTATE_OPEN || session_state != UA_SESSIONSTATE_ACTIVATED ||
                connect_status != UA_STATUSCODE_GOOD) {
                ESP_LOGW(TAG, "OPC UA connection state changed: channel=%d session=%d status=%s",
                         (int)channel_state, (int)session_state, UA_StatusCode_name(connect_status));
                status = connect_status != UA_STATUSCODE_GOOD ? connect_status : UA_STATUSCODE_BADCONNECTIONCLOSED;
                break;
            }
        }

        if (context->pause_requested) {
            set_status(context, OPCUA_CLIENT_PAUSED, "Paused while settings are open");
        } else if (context->reconnect_requested) {
            set_status(context, OPCUA_CLIENT_CONNECTING, "Endpoint changed; reconnecting");
        } else {
            set_status(context, OPCUA_CLIENT_CONNECTION_ERROR, "Connection lost; retrying");
        }
        if ((context->pause_requested || context->reconnect_requested) && network_is_ready()) {
            UA_Client_disconnect(context->ua_client);
        } else {
            /* Do not wait for a synchronous CloseSession on an already broken TCP path. */
            UA_Client_disconnectAsync(context->ua_client);
        }
        UA_Client_delete(context->ua_client);
        context->ua_client       = NULL;
        context->subscription_id = 0;
        wait_for_retry(context);
    }

    clear_tag_node_ids(context);
    set_status(context, OPCUA_CLIENT_STOPPED, "Client stopped");
    context->task_handle = NULL;
    vTaskDelete(NULL);
}

static void wait_for_retry(opcua_client_t* context)
{
    uint32_t waited_ms = 0;
    while (waited_ms < context->configuration.reconnect_delay_ms &&
           !context->stop_requested && !context->pause_requested) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited_ms += 100;
    }
}

static bool network_is_ready(void)
{
    if (! wifi_ctrl_is_connected() || ! wifi_ctrl_has_ip_address()) {
        return false;
    }
    esp_netif_t* station_interface          = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t address_information = {0};
    return station_interface != NULL && esp_netif_get_ip_info(station_interface, &address_information) == ESP_OK &&
           address_information.ip.addr != 0;
}

static void set_status(opcua_client_t* context, opcua_client_state_t state, const char* details)
{
    if (context->status_mutex == NULL) {
        return;
    }
    xSemaphoreTake(context->status_mutex, portMAX_DELAY);
    context->state = state;
    const char* state_text  = opcua_client_state_name(state);
    const char* detail_text = details != NULL ? details : "";
    size_t write_offset     = 0;

    /* Copy bounded fragments manually because an endpoint URL can be longer
     * than the short status text shown by the UI. */
    while (*state_text != '\0' && write_offset + 1 < sizeof(context->status)) {
        context->status[write_offset++] = *state_text++;
    }
    if (write_offset + 1 < sizeof(context->status)) {
        context->status[write_offset++] = ':';
    }
    if (write_offset + 1 < sizeof(context->status)) {
        context->status[write_offset++] = ' ';
    }
    while (*detail_text != '\0' && write_offset + 1 < sizeof(context->status)) {
        context->status[write_offset++] = *detail_text++;
    }
    context->status[write_offset] = '\0';
    xSemaphoreGive(context->status_mutex);
}

static UA_StatusCode discover_address_space(opcua_client_t* context)
{
    data_model_begin_structure_update(context->data_model);
    data_model_clear(context->data_model);
    clear_tag_node_ids(context);
    UA_NodeId objects_folder = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_StatusCode result     = browse_children(context, &objects_folder, DATA_MODEL_INVALID_INDEX, 0);
    if (result != UA_STATUSCODE_GOOD) {
        data_model_clear(context->data_model);
        clear_tag_node_ids(context);
    }
    data_model_end_structure_update(context->data_model);
    return result;
}

static UA_StatusCode browse_children(opcua_client_t* context, const UA_NodeId* parent_node_id,
                                     size_t parent_equipment_index, unsigned depth)
{
    if (depth > context->configuration.maximum_browse_depth) {
        ESP_LOGW(TAG, "Maximum Browse depth reached");
        return UA_STATUSCODE_GOOD;
    }

    UA_BrowseRequest request;
    UA_BrowseRequest_init(&request);
    request.requestedMaxReferencesPerNode = 0;
    request.nodesToBrowse                 = UA_BrowseDescription_new();
    if (request.nodesToBrowse == NULL) {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }
    request.nodesToBrowseSize = 1;
    UA_NodeId_copy(parent_node_id, &request.nodesToBrowse[0].nodeId);
    request.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    request.nodesToBrowse[0].referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
    request.nodesToBrowse[0].includeSubtypes = true;
    request.nodesToBrowse[0].nodeClassMask   = UA_NODECLASS_OBJECT | UA_NODECLASS_VARIABLE;
    request.nodesToBrowse[0].resultMask      = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse response = UA_Client_Service_browse(context->ua_client, request);
    UA_BrowseRequest_clear(&request);
    if (response.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        UA_StatusCode result = response.responseHeader.serviceResult;
        UA_BrowseResponse_clear(&response);
        return result;
    }

    UA_StatusCode result             = response.resultsSize == 1
                                           ? process_child_references(context, &response.results[0], parent_equipment_index, depth)
                                           : UA_STATUSCODE_BADUNEXPECTEDERROR;
    UA_ByteString continuation_point = {0, NULL};
    if (response.resultsSize == 1 && response.results[0].continuationPoint.length > 0) {
        UA_StatusCode copy_result = UA_ByteString_copy(&response.results[0].continuationPoint, &continuation_point);
        if (copy_result != UA_STATUSCODE_GOOD) {
            result = copy_result;
        }
    }
    UA_BrowseResponse_clear(&response);

    while (result == UA_STATUSCODE_GOOD && continuation_point.length > 0) {
        UA_BrowseNextRequest next_request;
        UA_BrowseNextRequest_init(&next_request);
        next_request.continuationPointsSize = 1;
        next_request.continuationPoints     = &continuation_point;

        UA_BrowseNextResponse next_response = UA_Client_Service_browseNext(context->ua_client, next_request);
        next_request.continuationPoints     = NULL;
        next_request.continuationPointsSize = 0;
        UA_ByteString_clear(&continuation_point);

        if (next_response.responseHeader.serviceResult != UA_STATUSCODE_GOOD || next_response.resultsSize != 1) {
            result = next_response.responseHeader.serviceResult != UA_STATUSCODE_GOOD
                         ? next_response.responseHeader.serviceResult
                         : UA_STATUSCODE_BADUNEXPECTEDERROR;
            UA_BrowseNextResponse_clear(&next_response);
            break;
        }

        result = process_child_references(context, &next_response.results[0], parent_equipment_index, depth);
        if (next_response.results[0].continuationPoint.length > 0) {
            UA_StatusCode copy_result =
                UA_ByteString_copy(&next_response.results[0].continuationPoint, &continuation_point);
            if (copy_result != UA_STATUSCODE_GOOD) {
                result = copy_result;
            }
        }
        UA_BrowseNextResponse_clear(&next_response);
    }

    if (continuation_point.length > 0) {
        release_continuation_point(context, &continuation_point);
    }
    return result;
}

static UA_StatusCode process_child_references(opcua_client_t* context, const UA_BrowseResult* browse_result,
                                              size_t parent_equipment_index, unsigned depth)
{
    if (browse_result->statusCode != UA_STATUSCODE_GOOD) {
        return browse_result->statusCode;
    }

    for (size_t reference_index = 0; reference_index < browse_result->referencesSize; ++reference_index) {
        const UA_ReferenceDescription* reference = &browse_result->references[reference_index];
        const UA_NodeId* child_node_id           = &reference->nodeId.nodeId;
        if (reference->nodeId.serverIndex != 0) {
            continue;
        }

        if (child_node_id->namespaceIndex == 0) {
            bool is_standard_server = child_node_id->identifierType == UA_NODEIDTYPE_NUMERIC &&
                                      child_node_id->identifier.numeric == UA_NS0ID_SERVER;
            if (reference->nodeClass == UA_NODECLASS_OBJECT && ! is_standard_server) {
                UA_StatusCode result = browse_children(context, child_node_id, parent_equipment_index, depth + 1);
                if (result != UA_STATUSCODE_GOOD) {
                    return result;
                }
            }
            continue;
        }

        UA_StatusCode result = UA_STATUSCODE_GOOD;
        if (reference->nodeClass == UA_NODECLASS_OBJECT) {
            if (find_mapped_node_id(context->equipment_node_ids, context->mapped_equipment_count, child_node_id) !=
                DATA_MODEL_INVALID_INDEX) {
                continue;
            }
            data_model_equipment_t equipment = {
                .parent_index = parent_equipment_index,
            };
            node_id_to_text(child_node_id, equipment.node_id, sizeof(equipment.node_id));
            node_id_to_text(&reference->typeDefinition.nodeId, equipment.type_definition_id,
                            sizeof(equipment.type_definition_id));
            ua_string_to_text(&reference->browseName.name, equipment.browse_name, sizeof(equipment.browse_name));
            ua_string_to_text(&reference->displayName.text, equipment.display_name, sizeof(equipment.display_name));
            if (equipment.display_name[0] == '\0') {
                snprintf(equipment.display_name, sizeof(equipment.display_name), "%s", equipment.browse_name);
            }
            if (parent_equipment_index == DATA_MODEL_INVALID_INDEX) {
                equipment.reference_kind = DATA_MODEL_REFERENCE_ROOT;
            } else if (is_ns0_numeric_node(&reference->referenceTypeId, UA_NS0ID_ORGANIZES)) {
                equipment.reference_kind = DATA_MODEL_REFERENCE_ORGANIZES;
            } else if (is_ns0_numeric_node(&reference->referenceTypeId, UA_NS0ID_HASCOMPONENT) ||
                       is_ns0_numeric_node(&reference->referenceTypeId, UA_NS0ID_HASORDEREDCOMPONENT)) {
                equipment.reference_kind = DATA_MODEL_REFERENCE_COMPONENT;
            }
            if (is_ns0_numeric_node(&reference->typeDefinition.nodeId, UA_NS0ID_FOLDERTYPE)) {
                equipment.entity_kind          = DATA_MODEL_ENTITY_GROUP;
                equipment.entity_kind_explicit = true;
            }
            read_object_metadata(context, child_node_id, &equipment);

            size_t equipment_index = DATA_MODEL_INVALID_INDEX;
            esp_err_t add_result   = data_model_add_equipment(context->data_model, &equipment, &equipment_index);
            if (add_result != ESP_OK || equipment_index >= context->configuration.maximum_equipment_objects) {
                return UA_STATUSCODE_BADOUTOFMEMORY;
            }
            UA_NodeId_copy(child_node_id, &context->equipment_node_ids[equipment_index]);
            context->mapped_equipment_count = equipment_index + 1;
            ESP_LOGI(TAG, "%*s[Object] %s (%s) | %s", (int)(depth * 2), "", equipment.display_name,
                     equipment.node_id, data_model_entity_kind_name(equipment.entity_kind));
            result = browse_children(context, child_node_id, equipment_index, depth + 1);
        } else if (reference->nodeClass == UA_NODECLASS_VARIABLE &&
                   parent_equipment_index != DATA_MODEL_INVALID_INDEX &&
                   ! is_ns0_numeric_node(&reference->referenceTypeId, UA_NS0ID_HASPROPERTY)) {
            result = add_variable(context, reference, parent_equipment_index, depth);
        }

        if (result != UA_STATUSCODE_GOOD) {
            return result;
        }
    }
    return UA_STATUSCODE_GOOD;
}

static void release_continuation_point(opcua_client_t* context, UA_ByteString* continuation_point)
{
    UA_BrowseNextRequest request;
    UA_BrowseNextRequest_init(&request);
    request.releaseContinuationPoints = true;
    request.continuationPointsSize    = 1;
    request.continuationPoints        = continuation_point;
    UA_BrowseNextResponse response    = UA_Client_Service_browseNext(context->ua_client, request);
    request.continuationPoints        = NULL;
    request.continuationPointsSize    = 0;
    UA_BrowseNextResponse_clear(&response);
    UA_ByteString_clear(continuation_point);
}

static void read_object_metadata(opcua_client_t* context, const UA_NodeId* object_node_id,
                                 data_model_equipment_t* equipment)
{
    UA_BrowseRequest request;
    UA_BrowseRequest_init(&request);
    request.nodesToBrowse = UA_BrowseDescription_new();
    if (request.nodesToBrowse == NULL) {
        return;
    }
    request.nodesToBrowseSize = 1;
    UA_NodeId_copy(object_node_id, &request.nodesToBrowse[0].nodeId);
    request.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    request.nodesToBrowse[0].referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY);
    request.nodesToBrowse[0].includeSubtypes = true;
    request.nodesToBrowse[0].nodeClassMask   = UA_NODECLASS_VARIABLE;
    request.nodesToBrowse[0].resultMask      = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse response = UA_Client_Service_browse(context->ua_client, request);
    UA_BrowseRequest_clear(&request);
    if (response.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        UA_BrowseResponse_clear(&response);
        return;
    }

    for (size_t result_index = 0; result_index < response.resultsSize; ++result_index) {
        UA_BrowseResult* browse_result = &response.results[result_index];
        for (size_t reference_index = 0; reference_index < browse_result->referencesSize; ++reference_index) {
            UA_ReferenceDescription* property = &browse_result->references[reference_index];
            char property_name[DATA_MODEL_NAME_LENGTH];
            ua_string_to_text(&property->browseName.name, property_name, sizeof(property_name));
            if (strcmp(property_name, "EntityKind") != 0) {
                continue;
            }

            UA_Variant property_value;
            UA_Variant_init(&property_value);
            if (UA_Client_readValueAttribute(context->ua_client, property->nodeId.nodeId, &property_value) ==
                    UA_STATUSCODE_GOOD &&
                UA_Variant_hasScalarType(&property_value, &UA_TYPES[UA_TYPES_STRING])) {
                char entity_kind[DATA_MODEL_NAME_LENGTH];
                ua_string_to_text((UA_String*)property_value.data, entity_kind, sizeof(entity_kind));
                data_model_entity_kind_t parsed_kind = parse_entity_kind(entity_kind);
                if (parsed_kind != DATA_MODEL_ENTITY_UNKNOWN) {
                    equipment->entity_kind          = parsed_kind;
                    equipment->entity_kind_explicit = true;
                }
            }
            UA_Variant_clear(&property_value);
        }
    }
    UA_BrowseResponse_clear(&response);
}

static UA_StatusCode add_variable(opcua_client_t* context, const UA_ReferenceDescription* reference,
                                  size_t equipment_index, unsigned depth)
{
    const UA_NodeId* node_id = &reference->nodeId.nodeId;
    if (find_mapped_node_id(context->tag_node_ids, context->mapped_tag_count, node_id) != DATA_MODEL_INVALID_INDEX) {
        return UA_STATUSCODE_GOOD;
    }
    UA_NodeId data_type_id   = UA_NODEID_NULL;
    UA_Byte access_level     = 0;
    UA_Variant current_value;
    UA_Variant_init(&current_value);

    UA_StatusCode result = UA_Client_readDataTypeAttribute(context->ua_client, *node_id, &data_type_id);
    if (result != UA_STATUSCODE_GOOD) {
        return result;
    }
    /* UserAccessLevel reflects rights of the active OPC UA session, not only node capabilities. */
    result = UA_Client_readUserAccessLevelAttribute(context->ua_client, *node_id, &access_level);
    if (result != UA_STATUSCODE_GOOD) {
        UA_NodeId_clear(&data_type_id);
        return result;
    }

    UA_UInt16 builtin_type_index = map_builtin_type_index(&data_type_id);
    data_model_tag_t tag         = {
                .equipment_index = equipment_index,
                .data_type       = map_data_type(&data_type_id),
                .readable        = (access_level & UA_ACCESSLEVELMASK_READ) != 0,
                .writable        = (access_level & UA_ACCESSLEVELMASK_WRITE) != 0,
    };
    UA_NodeId_clear(&data_type_id);
    node_id_to_text(node_id, tag.node_id, sizeof(tag.node_id));
    ua_string_to_text(&reference->browseName.name, tag.browse_name, sizeof(tag.browse_name));
    ua_string_to_text(&reference->displayName.text, tag.display_name, sizeof(tag.display_name));
    if (tag.display_name[0] == '\0') {
        snprintf(tag.display_name, sizeof(tag.display_name), "%s", tag.browse_name);
    }

    if (tag.readable) {
        result = UA_Client_readValueAttribute(context->ua_client, *node_id, &current_value);
        if (result == UA_STATUSCODE_GOOD) {
            tag.value_valid = variant_to_model_value(&current_value, tag.data_type, &tag.value);
        }
    }
    read_variable_metadata(context, node_id, &tag);

    size_t tag_index     = DATA_MODEL_INVALID_INDEX;
    esp_err_t add_result = data_model_add_tag(context->data_model, &tag, &tag_index);
    if (add_result != ESP_OK || tag_index >= context->configuration.maximum_tags) {
        UA_Variant_clear(&current_value);
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }
    UA_NodeId_copy(node_id, &context->tag_node_ids[tag_index]);
    context->tag_builtin_type_indices[tag_index] = builtin_type_index;
    context->mapped_tag_count                    = tag_index + 1;

    ESP_LOGI(TAG, "%*s[Variable] %s | %s | %s%s | %s", (int)(depth * 2), "", tag.display_name, tag.node_id,
             tag.readable ? "R" : "-", tag.writable ? "W" : "-", data_model_type_name(tag.data_type));
    UA_Variant_clear(&current_value);
    return UA_STATUSCODE_GOOD;
}

static void read_variable_metadata(opcua_client_t* context, const UA_NodeId* variable_node_id, data_model_tag_t* tag)
{
    UA_BrowseRequest request;
    UA_BrowseRequest_init(&request);
    request.nodesToBrowse = UA_BrowseDescription_new();
    if (request.nodesToBrowse == NULL) {
        return;
    }
    request.nodesToBrowseSize = 1;
    UA_NodeId_copy(variable_node_id, &request.nodesToBrowse[0].nodeId);
    request.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    request.nodesToBrowse[0].referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY);
    request.nodesToBrowse[0].includeSubtypes = true;
    request.nodesToBrowse[0].nodeClassMask   = UA_NODECLASS_VARIABLE;
    request.nodesToBrowse[0].resultMask      = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse response = UA_Client_Service_browse(context->ua_client, request);
    UA_BrowseRequest_clear(&request);
    if (response.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        UA_BrowseResponse_clear(&response);
        return;
    }

    for (size_t result_index = 0; result_index < response.resultsSize; ++result_index) {
        UA_BrowseResult* browse_result = &response.results[result_index];
        for (size_t reference_index = 0; reference_index < browse_result->referencesSize; ++reference_index) {
            UA_ReferenceDescription* reference = &browse_result->references[reference_index];
            char property_name[DATA_MODEL_NAME_LENGTH];
            ua_string_to_text(&reference->browseName.name, property_name, sizeof(property_name));

            UA_Variant property_value;
            UA_Variant_init(&property_value);
            if (UA_Client_readValueAttribute(context->ua_client, reference->nodeId.nodeId, &property_value) !=
                UA_STATUSCODE_GOOD) {
                continue;
            }

            if ((strcmp(property_name, "EngineeringUnit") == 0 || strcmp(property_name, "EngineeringUnits") == 0) &&
                UA_Variant_hasScalarType(&property_value, &UA_TYPES[UA_TYPES_STRING])) {
                ua_string_to_text((UA_String*)property_value.data, tag->engineering_unit,
                                  sizeof(tag->engineering_unit));
            } else if (strcmp(property_name, "SemanticRole") == 0 &&
                       UA_Variant_hasScalarType(&property_value, &UA_TYPES[UA_TYPES_STRING])) {
                ua_string_to_text((UA_String*)property_value.data, tag->semantic_role,
                                  sizeof(tag->semantic_role));
            } else if (strcmp(property_name, "Minimum") == 0) {
                data_model_value_t value;
                if (variant_to_model_value(&property_value, DATA_MODEL_TYPE_FLOAT, &value) ||
                    variant_to_model_value(&property_value, DATA_MODEL_TYPE_DOUBLE, &value)) {
                    tag->minimum     = value.numeric_value;
                    tag->has_minimum = true;
                }
            } else if (strcmp(property_name, "Maximum") == 0) {
                data_model_value_t value;
                if (variant_to_model_value(&property_value, DATA_MODEL_TYPE_FLOAT, &value) ||
                    variant_to_model_value(&property_value, DATA_MODEL_TYPE_DOUBLE, &value)) {
                    tag->maximum     = value.numeric_value;
                    tag->has_maximum = true;
                }
            } else if (strcmp(property_name, "EURange") == 0 &&
                       UA_Variant_hasScalarType(&property_value, &UA_TYPES[UA_TYPES_RANGE])) {
                UA_Range* range  = property_value.data;
                tag->minimum     = range->low;
                tag->maximum     = range->high;
                tag->has_minimum = true;
                tag->has_maximum = true;
            }
            UA_Variant_clear(&property_value);
        }
    }
    UA_BrowseResponse_clear(&response);
}

static void data_change_callback(UA_Client* client, UA_UInt32 subscription_id, void* subscription_context,
                                 UA_UInt32 monitored_item_id, void* monitored_item_context, UA_DataValue* value)
{
    (void)client;
    (void)subscription_id;
    (void)monitored_item_id;
    opcua_client_t* context = subscription_context;
    size_t tag_index        = (size_t)(uintptr_t)monitored_item_context - 1;
    data_model_tag_t tag;
    data_model_value_t model_value = {0};
    if (value == NULL || ! value->hasValue || ! data_model_get_tag(context->data_model, tag_index, &tag)) {
        return;
    }
    bool valid = variant_to_model_value(&value->value, tag.data_type, &model_value);
    data_model_update_value(context->data_model, tag_index, &model_value, valid);
}

static void create_subscriptions(opcua_client_t* context)
{
    UA_CreateSubscriptionRequest request = UA_CreateSubscriptionRequest_default();
    request.requestedPublishingInterval  = context->configuration.subscription_interval_ms;
    UA_CreateSubscriptionResponse response =
        UA_Client_Subscriptions_create(context->ua_client, request, context, NULL, NULL);
    if (response.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        ESP_LOGW(TAG, "Subscription creation failed: %s; values remain at initial Read",
                 UA_StatusCode_name(response.responseHeader.serviceResult));
        UA_CreateSubscriptionResponse_clear(&response);
        return;
    }
    context->subscription_id = response.subscriptionId;
    UA_CreateSubscriptionResponse_clear(&response);

    for (size_t tag_index = 0; tag_index < context->mapped_tag_count; ++tag_index) {
        data_model_tag_t tag;
        if (! data_model_get_tag(context->data_model, tag_index, &tag) || ! tag.readable) {
            continue;
        }
        UA_MonitoredItemCreateRequest item = UA_MonitoredItemCreateRequest_default(context->tag_node_ids[tag_index]);
        item.requestedParameters.samplingInterval = context->configuration.subscription_interval_ms;
        UA_MonitoredItemCreateResult item_result  = UA_Client_MonitoredItems_createDataChange(
            context->ua_client, context->subscription_id, UA_TIMESTAMPSTORETURN_BOTH, item,
            (void*)(uintptr_t)(tag_index + 1), data_change_callback, NULL);
        if (item_result.statusCode != UA_STATUSCODE_GOOD) {
            ESP_LOGW(TAG, "Cannot monitor %s: %s", tag.node_id, UA_StatusCode_name(item_result.statusCode));
        }
        UA_MonitoredItemCreateResult_clear(&item_result);
    }
    create_event_subscriptions(context);
}

static void create_event_subscriptions(opcua_client_t* context)
{
    for (size_t equipment_index = 0; equipment_index < context->mapped_equipment_count; ++equipment_index) {
        UA_Byte event_notifier = 0;
        UA_StatusCode read_status = UA_Client_readEventNotifierAttribute(
            context->ua_client, context->equipment_node_ids[equipment_index], &event_notifier);
        if (read_status != UA_STATUSCODE_GOOD || (event_notifier & UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT) == 0) {
            continue;
        }

        UA_EventFilter filter;
        UA_EventFilter_init(&filter);
        filter.selectClausesSize = OPCUA_EVENT_FIELD_COUNT;
        filter.selectClauses = UA_Array_new(filter.selectClausesSize, &UA_TYPES[UA_TYPES_SIMPLEATTRIBUTEOPERAND]);
        if (filter.selectClauses == NULL) {
            ESP_LOGW(TAG, "Cannot allocate OPC UA event filter");
            return;
        }
        configure_event_field(&filter.selectClauses[0], "SourceNode");
        configure_event_field(&filter.selectClauses[1], "SourceName");
        configure_event_field(&filter.selectClauses[2], "Message");
        configure_event_field(&filter.selectClauses[3], "Severity");

        UA_MonitoredItemCreateRequest item =
            UA_MonitoredItemCreateRequest_default(context->equipment_node_ids[equipment_index]);
        item.itemToMonitor.attributeId = UA_ATTRIBUTEID_EVENTNOTIFIER;
        item.requestedParameters.queueSize = 16;
        item.requestedParameters.discardOldest = true;
        item.requestedParameters.filter.encoding = UA_EXTENSIONOBJECT_DECODED;
        item.requestedParameters.filter.content.decoded.type = &UA_TYPES[UA_TYPES_EVENTFILTER];
        item.requestedParameters.filter.content.decoded.data = &filter;

        UA_MonitoredItemCreateResult result = UA_Client_MonitoredItems_createEvent(
            context->ua_client, context->subscription_id, UA_TIMESTAMPSTORETURN_BOTH, item, NULL,
            event_notification_callback, NULL);
        if (result.statusCode != UA_STATUSCODE_GOOD) {
            ESP_LOGW(TAG, "Cannot monitor events for equipment %u: %s", (unsigned)equipment_index,
                     UA_StatusCode_name(result.statusCode));
        }
        UA_MonitoredItemCreateResult_clear(&result);
        UA_EventFilter_clear(&filter);
    }
}

static void configure_event_field(UA_SimpleAttributeOperand* operand, const char* browse_name)
{
    UA_SimpleAttributeOperand_init(operand);
    operand->typeDefinitionId = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEEVENTTYPE);
    operand->attributeId = UA_ATTRIBUTEID_VALUE;
    operand->browsePathSize = 1;
    operand->browsePath = UA_QualifiedName_new();
    if (operand->browsePath != NULL) {
        operand->browsePath[0] = UA_QUALIFIEDNAME_ALLOC(0, browse_name);
    }
}

static void event_notification_callback(UA_Client* client, UA_UInt32 subscription_id, void* subscription_context,
                                        UA_UInt32 monitored_item_id, void* monitored_item_context,
                                        size_t event_field_count, UA_Variant* event_fields)
{
    (void)client;
    (void)subscription_id;
    (void)monitored_item_id;
    (void)monitored_item_context;
    opcua_client_t* context = subscription_context;
    if (context == NULL || event_fields == NULL || event_field_count < OPCUA_EVENT_FIELD_COUNT ||
        !UA_Variant_hasScalarType(&event_fields[0], &UA_TYPES[UA_TYPES_NODEID]) ||
        !UA_Variant_hasScalarType(&event_fields[1], &UA_TYPES[UA_TYPES_STRING]) ||
        !UA_Variant_hasScalarType(&event_fields[2], &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]) ||
        !UA_Variant_hasScalarType(&event_fields[3], &UA_TYPES[UA_TYPES_UINT16])) {
        ESP_LOGW(TAG, "Ignored malformed OPC UA event notification");
        return;
    }

    data_model_alarm_t alarm = {0};
    node_id_to_text((UA_NodeId*)event_fields[0].data, alarm.source_node_id, sizeof(alarm.source_node_id));
    ua_string_to_text((UA_String*)event_fields[1].data, alarm.source_name, sizeof(alarm.source_name));
    UA_LocalizedText* localized_message = event_fields[2].data;
    char message[DATA_MODEL_ALARM_REASON_LENGTH];
    ua_string_to_text(&localized_message->text, message, sizeof(message));
    alarm.severity = *(UA_UInt16*)event_fields[3].data;

    const char* code_start = strchr(message, '[');
    const char* code_end = code_start != NULL ? strchr(code_start + 1, ']') : NULL;
    if (code_start == NULL || code_end == NULL || code_end == code_start + 1) {
        ESP_LOGW(TAG, "Ignored event without alarm code: %s", message);
        return;
    }
    size_t code_length = (size_t)(code_end - code_start - 1);
    if (code_length >= sizeof(alarm.alarm_code)) {
        code_length = sizeof(alarm.alarm_code) - 1;
    }
    memcpy(alarm.alarm_code, code_start + 1, code_length);
    alarm.alarm_code[code_length] = '\0';
    alarm.active = strstr(code_end, " ACTIVE:") != NULL;
    bool cleared = strstr(code_end, " CLEARED:") != NULL;
    if (!alarm.active && !cleared) {
        ESP_LOGW(TAG, "Ignored event without ACTIVE/CLEARED state: %s", message);
        return;
    }

    const char* reason = strstr(code_end, ":");
    reason = reason != NULL ? reason + 1 : code_end + 1;
    while (*reason == ' ') {
        reason++;
    }
    size_t reason_length = strnlen(reason, sizeof(alarm.reason) - 1);
    memcpy(alarm.reason, reason, reason_length);
    alarm.reason[reason_length] = '\0';
    esp_err_t update_result = data_model_update_alarm(context->data_model, &alarm);
    if (update_result == ESP_OK) {
        ESP_LOGI(TAG, "Alarm %s: %s / %s", alarm.active ? "ACTIVE" : "CLEARED", alarm.source_name,
                 alarm.alarm_code);
    } else {
        ESP_LOGW(TAG, "Cannot store alarm event: %s", esp_err_to_name(update_result));
    }
}

static void process_write_requests(opcua_client_t* context)
{
    write_request_t request;
    while (xQueueReceive(context->write_queue, &request, 0) == pdTRUE) {
        if (request.tag_index >= context->mapped_tag_count) {
            ESP_LOGE(TAG, "Write rejected: tag index %u is invalid", (unsigned)request.tag_index);
            continue;
        }

        data_model_tag_t tag;
        if (! data_model_get_tag(context->data_model, request.tag_index, &tag) || ! tag.writable) {
            ESP_LOGE(TAG, "Write rejected: tag is not writable");
            continue;
        }

        UA_Variant value;
        UA_Variant_init(&value);
        UA_Boolean boolean_value     = request.boolean_value;
        UA_SByte signed_byte_value   = (UA_SByte)request.numeric_value;
        UA_Byte byte_value           = (UA_Byte)request.numeric_value;
        UA_Int16 int16_value         = (UA_Int16)request.numeric_value;
        UA_UInt16 uint16_value       = (UA_UInt16)request.numeric_value;
        UA_Int32 int32_value         = (UA_Int32)request.numeric_value;
        UA_UInt32 uint32_value       = (UA_UInt32)request.numeric_value;
        UA_Int64 int64_value         = (UA_Int64)request.numeric_value;
        UA_UInt64 uint64_value       = (UA_UInt64)request.numeric_value;
        UA_Float float_value         = (UA_Float)request.numeric_value;
        UA_Double double_value       = request.numeric_value;
        UA_UInt16 builtin_type_index = context->tag_builtin_type_indices[request.tag_index];

        if (request.type == WRITE_REQUEST_BOOLEAN && builtin_type_index == UA_TYPES_BOOLEAN) {
            UA_Variant_setScalar(&value, &boolean_value, &UA_TYPES[UA_TYPES_BOOLEAN]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_SBYTE) {
            UA_Variant_setScalar(&value, &signed_byte_value, &UA_TYPES[UA_TYPES_SBYTE]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_BYTE) {
            UA_Variant_setScalar(&value, &byte_value, &UA_TYPES[UA_TYPES_BYTE]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_INT16) {
            UA_Variant_setScalar(&value, &int16_value, &UA_TYPES[UA_TYPES_INT16]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_UINT16) {
            UA_Variant_setScalar(&value, &uint16_value, &UA_TYPES[UA_TYPES_UINT16]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_INT32) {
            UA_Variant_setScalar(&value, &int32_value, &UA_TYPES[UA_TYPES_INT32]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_UINT32) {
            UA_Variant_setScalar(&value, &uint32_value, &UA_TYPES[UA_TYPES_UINT32]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_INT64) {
            UA_Variant_setScalar(&value, &int64_value, &UA_TYPES[UA_TYPES_INT64]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_UINT64) {
            UA_Variant_setScalar(&value, &uint64_value, &UA_TYPES[UA_TYPES_UINT64]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_FLOAT) {
            UA_Variant_setScalar(&value, &float_value, &UA_TYPES[UA_TYPES_FLOAT]);
        } else if (request.type == WRITE_REQUEST_NUMBER && builtin_type_index == UA_TYPES_DOUBLE) {
            UA_Variant_setScalar(&value, &double_value, &UA_TYPES[UA_TYPES_DOUBLE]);
        } else {
            ESP_LOGE(TAG, "Write type does not match %s", tag.display_name);
            continue;
        }

        UA_StatusCode status =
            UA_Client_writeValueAttribute(context->ua_client, context->tag_node_ids[request.tag_index], &value);
        if (status == UA_STATUSCODE_GOOD) {
            ESP_LOGI(TAG, "Write completed: %s", tag.display_name);
        } else {
            ESP_LOGE(TAG, "Write failed for %s: %s", tag.display_name, UA_StatusCode_name(status));
        }
    }
}

static void clear_tag_node_ids(opcua_client_t* context)
{
    for (size_t index = 0; index < context->mapped_tag_count; ++index) {
        UA_NodeId_clear(&context->tag_node_ids[index]);
        context->tag_builtin_type_indices[index] = UINT16_MAX;
    }
    context->mapped_tag_count = 0;
    for (size_t index = 0; index < context->mapped_equipment_count; ++index) {
        UA_NodeId_clear(&context->equipment_node_ids[index]);
    }
    context->mapped_equipment_count = 0;
}

static data_model_type_t map_data_type(const UA_NodeId* data_type_id)
{
    switch (map_builtin_type_index(data_type_id)) {
    case UA_TYPES_BOOLEAN:
        return DATA_MODEL_TYPE_BOOLEAN;
    case UA_TYPES_FLOAT:
        return DATA_MODEL_TYPE_FLOAT;
    case UA_TYPES_DOUBLE:
        return DATA_MODEL_TYPE_DOUBLE;
    case UA_TYPES_STRING:
        return DATA_MODEL_TYPE_STRING;
    case UA_TYPES_SBYTE:
    case UA_TYPES_BYTE:
    case UA_TYPES_INT16:
    case UA_TYPES_UINT16:
    case UA_TYPES_INT32:
    case UA_TYPES_UINT32:
    case UA_TYPES_INT64:
    case UA_TYPES_UINT64:
        return DATA_MODEL_TYPE_INTEGER;
    default:
        return DATA_MODEL_TYPE_UNKNOWN;
    }
}

static UA_UInt16 map_builtin_type_index(const UA_NodeId* data_type_id)
{
    static const UA_UInt16 supported_types[] = {
        UA_TYPES_BOOLEAN, UA_TYPES_SBYTE, UA_TYPES_BYTE,   UA_TYPES_INT16, UA_TYPES_UINT16, UA_TYPES_INT32,
        UA_TYPES_UINT32,  UA_TYPES_INT64, UA_TYPES_UINT64, UA_TYPES_FLOAT, UA_TYPES_DOUBLE, UA_TYPES_STRING,
    };
    for (size_t index = 0; index < sizeof(supported_types) / sizeof(supported_types[0]); ++index) {
        UA_UInt16 type_index = supported_types[index];
        if (UA_NodeId_equal(data_type_id, &UA_TYPES[type_index].typeId)) {
            return type_index;
        }
    }
    return UINT16_MAX;
}

static bool variant_to_model_value(const UA_Variant* variant, data_model_type_t data_type,
                                   data_model_value_t* value_out)
{
    if (variant == NULL || value_out == NULL || ! UA_Variant_isScalar(variant) || variant->data == NULL) {
        return false;
    }
    memset(value_out, 0, sizeof(*value_out));

    switch (data_type) {
    case DATA_MODEL_TYPE_BOOLEAN:
        if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_BOOLEAN])) {
            value_out->boolean_value = *(UA_Boolean*)variant->data;
            return true;
        }
        break;
    case DATA_MODEL_TYPE_FLOAT:
        if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_FLOAT])) {
            value_out->numeric_value = *(UA_Float*)variant->data;
            return true;
        }
        break;
    case DATA_MODEL_TYPE_DOUBLE:
        if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_DOUBLE])) {
            value_out->numeric_value = *(UA_Double*)variant->data;
            return true;
        }
        break;
    case DATA_MODEL_TYPE_STRING:
        if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_STRING])) {
            ua_string_to_text((UA_String*)variant->data, value_out->string_value, sizeof(value_out->string_value));
            return true;
        }
        break;
    case DATA_MODEL_TYPE_INTEGER:
        if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_SBYTE])) {
            value_out->integer_value = *(UA_SByte*)variant->data;
        } else if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_BYTE])) {
            value_out->integer_value = *(UA_Byte*)variant->data;
        } else if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_INT16])) {
            value_out->integer_value = *(UA_Int16*)variant->data;
        } else if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_UINT16])) {
            value_out->integer_value = *(UA_UInt16*)variant->data;
        } else if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_INT32])) {
            value_out->integer_value = *(UA_Int32*)variant->data;
        } else if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_UINT32])) {
            value_out->integer_value = *(UA_UInt32*)variant->data;
        } else if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_INT64])) {
            value_out->integer_value = *(UA_Int64*)variant->data;
        } else if (UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_UINT64])) {
            value_out->integer_value = (int64_t)*(UA_UInt64*)variant->data;
        } else {
            return false;
        }
        return true;
    default:
        break;
    }
    return false;
}

static void node_id_to_text(const UA_NodeId* node_id, char* buffer, size_t buffer_size)
{
    if (node_id == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }
    switch (node_id->identifierType) {
    case UA_NODEIDTYPE_NUMERIC:
        snprintf(buffer, buffer_size, "ns=%u;i=%" PRIu32, node_id->namespaceIndex, node_id->identifier.numeric);
        break;
    case UA_NODEIDTYPE_STRING: {
        char identifier[DATA_MODEL_NODE_ID_LENGTH] = {0};
        ua_string_to_text(&node_id->identifier.string, identifier, sizeof(identifier));
        snprintf(buffer, buffer_size, "ns=%u;s=%s", node_id->namespaceIndex, identifier);
        break;
    }
    case UA_NODEIDTYPE_GUID:
        snprintf(
            buffer, buffer_size, "ns=%u;g=%08" PRIx32 "-%04" PRIx16 "-%04" PRIx16 "-%02x%02x-%02x%02x%02x%02x%02x%02x",
            node_id->namespaceIndex, node_id->identifier.guid.data1, node_id->identifier.guid.data2,
            node_id->identifier.guid.data3, node_id->identifier.guid.data4[0], node_id->identifier.guid.data4[1],
            node_id->identifier.guid.data4[2], node_id->identifier.guid.data4[3], node_id->identifier.guid.data4[4],
            node_id->identifier.guid.data4[5], node_id->identifier.guid.data4[6], node_id->identifier.guid.data4[7]);
        break;
    case UA_NODEIDTYPE_BYTESTRING: {
        int written = snprintf(buffer, buffer_size, "ns=%u;b=", node_id->namespaceIndex);
        if (written < 0 || (size_t)written >= buffer_size) {
            break;
        }
        size_t offset                   = (size_t)written;
        const UA_ByteString* identifier = &node_id->identifier.byteString;
        for (size_t index = 0; index < identifier->length && offset + 2 < buffer_size; ++index) {
            written = snprintf(buffer + offset, buffer_size - offset, "%02x", identifier->data[index]);
            if (written != 2) {
                break;
            }
            offset += 2;
        }
        break;
    }
    default:
        snprintf(buffer, buffer_size, "ns=%u;opaque", node_id->namespaceIndex);
        break;
    }
}

static void ua_string_to_text(const UA_String* source, char* buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    if (source == NULL || source->data == NULL || source->length == 0) {
        buffer[0] = '\0';
        return;
    }
    size_t copy_length = source->length < buffer_size - 1 ? source->length : buffer_size - 1;
    memcpy(buffer, source->data, copy_length);
    buffer[copy_length] = '\0';
}
