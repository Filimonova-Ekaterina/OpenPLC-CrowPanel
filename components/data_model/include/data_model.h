#pragma once

/**
 * @file data_model.h
 * @brief Thread-safe, transport-independent OPC UA equipment model.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DATA_MODEL_INVALID_INDEX       SIZE_MAX
#define DATA_MODEL_NAME_LENGTH         64
#define DATA_MODEL_NODE_ID_LENGTH      128
#define DATA_MODEL_STRING_VALUE_LENGTH 96
#define DATA_MODEL_UNIT_LENGTH         24
#define DATA_MODEL_SEMANTIC_ROLE_LENGTH 32
#define DATA_MODEL_ALARM_CODE_LENGTH   48
#define DATA_MODEL_ALARM_REASON_LENGTH 128
#define DATA_MODEL_MAX_ALARMS          32

typedef enum
{
    DATA_MODEL_TYPE_UNKNOWN = 0,
    DATA_MODEL_TYPE_BOOLEAN,
    DATA_MODEL_TYPE_INTEGER,
    DATA_MODEL_TYPE_FLOAT,
    DATA_MODEL_TYPE_DOUBLE,
    DATA_MODEL_TYPE_STRING,
} data_model_type_t;

typedef struct
{
    bool boolean_value;
    int64_t integer_value;
    double numeric_value;
    char string_value[DATA_MODEL_STRING_VALUE_LENGTH];
} data_model_value_t;

typedef struct
{
    size_t index;
    size_t parent_index;
    char node_id[DATA_MODEL_NODE_ID_LENGTH];
    char browse_name[DATA_MODEL_NAME_LENGTH];
    char display_name[DATA_MODEL_NAME_LENGTH];
} data_model_equipment_t;

typedef struct
{
    size_t index;
    size_t equipment_index;
    char node_id[DATA_MODEL_NODE_ID_LENGTH];
    char browse_name[DATA_MODEL_NAME_LENGTH];
    char display_name[DATA_MODEL_NAME_LENGTH];
    char engineering_unit[DATA_MODEL_UNIT_LENGTH];
    char semantic_role[DATA_MODEL_SEMANTIC_ROLE_LENGTH];
    data_model_type_t data_type;
    bool readable;
    bool writable;
    bool has_minimum;
    bool has_maximum;
    double minimum;
    double maximum;
    bool value_valid;
    data_model_value_t value;
} data_model_tag_t;

typedef struct
{
    char source_name[DATA_MODEL_NAME_LENGTH];
    char alarm_code[DATA_MODEL_ALARM_CODE_LENGTH];
    char reason[DATA_MODEL_ALARM_REASON_LENGTH];
    uint16_t severity;
    bool active;
} data_model_alarm_t;

typedef struct data_model data_model_t;

/** Create a model with bounded capacities suitable for embedded memory. */
esp_err_t data_model_create(size_t equipment_capacity, size_t tag_capacity, data_model_t** model_out);

/** Release a model and all resources owned by it. */
void data_model_destroy(data_model_t* model);

/** Remove the discovered hierarchy before a new Browse operation. */
void data_model_clear(data_model_t* model);

/** Batch Browse mutations so the UI rebuilds only after discovery completes. */
void data_model_begin_structure_update(data_model_t* model);

/** Publish a completed batch of discovered objects and tags to readers. */
void data_model_end_structure_update(data_model_t* model);

/** Add one discovered OPC UA Object node. */
esp_err_t data_model_add_equipment(data_model_t* model, const data_model_equipment_t* equipment, size_t* index_out);

/** Add one discovered OPC UA Variable node. */
esp_err_t data_model_add_tag(data_model_t* model, const data_model_tag_t* tag, size_t* index_out);

/** Update the current value of one tag from Read or Subscription. */
esp_err_t data_model_update_value(data_model_t* model, size_t tag_index, const data_model_value_t* value,
                                  bool value_valid);

/** Copy one equipment entry while holding the model lock internally. */
bool data_model_get_equipment(const data_model_t* model, size_t equipment_index, data_model_equipment_t* equipment_out);

/** Copy one tag entry while holding the model lock internally. */
bool data_model_get_tag(const data_model_t* model, size_t tag_index, data_model_tag_t* tag_out);

/** Add, update, or clear an alarm identified by its source and alarm code. */
esp_err_t data_model_update_alarm(data_model_t* model, const data_model_alarm_t* alarm);

/** Clear alarms when the OPC UA session or discovered structure is replaced. */
void data_model_clear_alarms(data_model_t* model);

/** Return active alarms only; active_index is dense even if retained slots are inactive. */
bool data_model_get_active_alarm(const data_model_t* model, size_t active_index, data_model_alarm_t* alarm_out);
size_t data_model_active_alarm_count(const data_model_t* model);

size_t data_model_equipment_count(const data_model_t* model);
size_t data_model_tag_count(const data_model_t* model);
uint32_t data_model_structure_generation(const data_model_t* model);
uint32_t data_model_value_generation(const data_model_t* model);
uint32_t data_model_alarm_generation(const data_model_t* model);

/** Human-readable data type name for logs and diagnostics. */
const char* data_model_type_name(data_model_type_t data_type);

#ifdef __cplusplus
}
#endif
