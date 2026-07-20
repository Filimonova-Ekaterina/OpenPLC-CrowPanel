#include "data_model.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct data_model
{
    data_model_equipment_t* equipment;
    data_model_tag_t* tags;
    data_model_alarm_t alarms[DATA_MODEL_MAX_ALARMS];
    size_t equipment_capacity;
    size_t tag_capacity;
    size_t equipment_count;
    size_t tag_count;
    size_t alarm_count;
    uint32_t structure_generation;
    uint32_t value_generation;
    uint32_t alarm_generation;
    bool structure_update_active;
    bool structure_dirty;
    SemaphoreHandle_t mutex;
};

static void mark_structure_changed(data_model_t* model)
{
    model->structure_dirty = true;
    if (! model->structure_update_active) {
        model->structure_generation++;
        model->structure_dirty = false;
    }
}

static bool tag_is_numeric(const data_model_tag_t* tag)
{
    return tag->data_type == DATA_MODEL_TYPE_INTEGER || tag->data_type == DATA_MODEL_TYPE_FLOAT ||
           tag->data_type == DATA_MODEL_TYPE_DOUBLE;
}

/** Infer only when the server did not publish an explicit semantic kind. */
static void classify_entities_locked(data_model_t* model)
{
    for (size_t equipment_index = 0; equipment_index < model->equipment_count; ++equipment_index) {
        data_model_equipment_t* equipment = &model->equipment[equipment_index];
        if (equipment->entity_kind_explicit) {
            continue;
        }

        bool has_children         = false;
        bool has_tags             = false;
        bool has_measurements     = false;
        bool has_commands         = false;
        bool has_operating_status = false;
        bool has_process_context  = false;
        for (size_t child_index = 0; child_index < model->equipment_count; ++child_index) {
            if (model->equipment[child_index].parent_index == equipment_index) {
                has_children = true;
                break;
            }
        }
        for (size_t tag_index = 0; tag_index < model->tag_count; ++tag_index) {
            const data_model_tag_t* tag = &model->tags[tag_index];
            if (tag->equipment_index != equipment_index) {
                continue;
            }
            has_tags = true;
            if (tag->writable) {
                has_commands = true;
            }
            if (strcmp(tag->semantic_role, "operating_status") == 0) {
                has_operating_status = true;
            }
            if (strcmp(tag->semantic_role, "demand") == 0 || strcmp(tag->semantic_role, "production_total") == 0 ||
                strcmp(tag->semantic_role, "consumption_total") == 0) {
                has_process_context = true;
            }
            if (tag->readable && ! tag->writable && tag_is_numeric(tag)) {
                has_measurements = true;
            }
        }

        if (has_children) {
            equipment->entity_kind = DATA_MODEL_ENTITY_SYSTEM;
        } else if (has_process_context) {
            equipment->entity_kind = DATA_MODEL_ENTITY_PROCESS;
        } else if (has_commands || has_operating_status) {
            equipment->entity_kind = DATA_MODEL_ENTITY_ACTIVE_EQUIPMENT;
        } else if (has_measurements || has_tags) {
            equipment->entity_kind = DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT;
        } else {
            equipment->entity_kind = DATA_MODEL_ENTITY_GROUP;
        }
    }
}

static bool model_lock(const data_model_t* model)
{
    return model != NULL && model->mutex != NULL && xSemaphoreTake(model->mutex, portMAX_DELAY) == pdTRUE;
}

static void model_unlock(const data_model_t* model)
{
    xSemaphoreGive(model->mutex);
}

esp_err_t data_model_create(size_t equipment_capacity, size_t tag_capacity, data_model_t** model_out)
{
    if (model_out == NULL || equipment_capacity == 0 || tag_capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    data_model_t* model = calloc(1, sizeof(*model));
    if (model == NULL) {
        return ESP_ERR_NO_MEM;
    }

    model->equipment = calloc(equipment_capacity, sizeof(*model->equipment));
    model->tags      = calloc(tag_capacity, sizeof(*model->tags));
    model->mutex     = xSemaphoreCreateMutex();
    if (model->equipment == NULL || model->tags == NULL || model->mutex == NULL) {
        data_model_destroy(model);
        return ESP_ERR_NO_MEM;
    }

    model->equipment_capacity   = equipment_capacity;
    model->tag_capacity         = tag_capacity;
    model->structure_generation = 1;
    *model_out                  = model;
    return ESP_OK;
}

void data_model_destroy(data_model_t* model)
{
    if (model == NULL) {
        return;
    }
    if (model->mutex != NULL) {
        vSemaphoreDelete(model->mutex);
    }
    free(model->equipment);
    free(model->tags);
    free(model);
}

void data_model_clear(data_model_t* model)
{
    if (! model_lock(model)) {
        return;
    }
    memset(model->equipment, 0, model->equipment_capacity * sizeof(*model->equipment));
    memset(model->tags, 0, model->tag_capacity * sizeof(*model->tags));
    model->equipment_count = 0;
    model->tag_count       = 0;
    memset(model->alarms, 0, sizeof(model->alarms));
    model->alarm_count = 0;
    mark_structure_changed(model);
    model->value_generation++;
    model->alarm_generation++;
    model_unlock(model);
}

void data_model_begin_structure_update(data_model_t* model)
{
    if (! model_lock(model)) {
        return;
    }
    model->structure_update_active = true;
    model->structure_dirty         = false;
    model_unlock(model);
}

void data_model_end_structure_update(data_model_t* model)
{
    if (! model_lock(model)) {
        return;
    }
    if (model->structure_update_active && model->structure_dirty) {
        classify_entities_locked(model);
        model->structure_generation++;
    }
    model->structure_update_active = false;
    model->structure_dirty         = false;
    model_unlock(model);
}

esp_err_t data_model_add_equipment(data_model_t* model, const data_model_equipment_t* equipment, size_t* index_out)
{
    if (equipment == NULL || ! model_lock(model)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (model->equipment_count >= model->equipment_capacity) {
        model_unlock(model);
        return ESP_ERR_NO_MEM;
    }

    size_t index                  = model->equipment_count++;
    model->equipment[index]       = *equipment;
    model->equipment[index].index = index;
    mark_structure_changed(model);
    if (index_out != NULL) {
        *index_out = index;
    }
    model_unlock(model);
    return ESP_OK;
}

esp_err_t data_model_add_tag(data_model_t* model, const data_model_tag_t* tag, size_t* index_out)
{
    if (tag == NULL || ! model_lock(model)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (model->tag_count >= model->tag_capacity) {
        model_unlock(model);
        return ESP_ERR_NO_MEM;
    }

    size_t index             = model->tag_count++;
    model->tags[index]       = *tag;
    model->tags[index].index = index;
    mark_structure_changed(model);
    if (index_out != NULL) {
        *index_out = index;
    }
    model_unlock(model);
    return ESP_OK;
}

esp_err_t data_model_update_value(data_model_t* model, size_t tag_index, const data_model_value_t* value,
                                  bool value_valid)
{
    if (value == NULL || ! model_lock(model)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tag_index >= model->tag_count) {
        model_unlock(model);
        return ESP_ERR_NOT_FOUND;
    }

    model->tags[tag_index].value       = *value;
    model->tags[tag_index].value_valid = value_valid;
    model->value_generation++;
    model_unlock(model);
    return ESP_OK;
}

bool data_model_get_equipment(const data_model_t* model, size_t equipment_index, data_model_equipment_t* equipment_out)
{
    if (equipment_out == NULL || ! model_lock(model)) {
        return false;
    }
    bool found = equipment_index < model->equipment_count;
    if (found) {
        *equipment_out = model->equipment[equipment_index];
    }
    model_unlock(model);
    return found;
}

bool data_model_get_tag(const data_model_t* model, size_t tag_index, data_model_tag_t* tag_out)
{
    if (tag_out == NULL || ! model_lock(model)) {
        return false;
    }
    bool found = tag_index < model->tag_count;
    if (found) {
        *tag_out = model->tags[tag_index];
    }
    model_unlock(model);
    return found;
}

esp_err_t data_model_update_alarm(data_model_t* model, const data_model_alarm_t* alarm)
{
    if (alarm == NULL || alarm->source_node_id[0] == '\0' || alarm->alarm_code[0] == '\0' || !model_lock(model)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t alarm_index = DATA_MODEL_INVALID_INDEX;
    for (size_t index = 0; index < model->alarm_count; ++index) {
        if (strcmp(model->alarms[index].source_node_id, alarm->source_node_id) == 0 &&
            strcmp(model->alarms[index].alarm_code, alarm->alarm_code) == 0) {
            alarm_index = index;
            break;
        }
    }
    if (alarm_index == DATA_MODEL_INVALID_INDEX) {
        if (!alarm->active) {
            model_unlock(model);
            return ESP_OK;
        }
        if (model->alarm_count >= DATA_MODEL_MAX_ALARMS) {
            model_unlock(model);
            return ESP_ERR_NO_MEM;
        }
        alarm_index = model->alarm_count++;
    }

    model->alarms[alarm_index] = *alarm;
    model->alarm_generation++;
    model_unlock(model);
    return ESP_OK;
}

void data_model_clear_alarms(data_model_t* model)
{
    if (!model_lock(model)) {
        return;
    }
    memset(model->alarms, 0, sizeof(model->alarms));
    model->alarm_count = 0;
    model->alarm_generation++;
    model_unlock(model);
}

bool data_model_get_active_alarm(const data_model_t* model, size_t active_index, data_model_alarm_t* alarm_out)
{
    if (alarm_out == NULL || !model_lock(model)) {
        return false;
    }
    size_t current_active_index = 0;
    bool found = false;
    for (size_t index = 0; index < model->alarm_count; ++index) {
        if (!model->alarms[index].active) {
            continue;
        }
        if (current_active_index++ == active_index) {
            *alarm_out = model->alarms[index];
            found = true;
            break;
        }
    }
    model_unlock(model);
    return found;
}

size_t data_model_active_alarm_count(const data_model_t* model)
{
    if (!model_lock(model)) {
        return 0;
    }
    size_t active_count = 0;
    for (size_t index = 0; index < model->alarm_count; ++index) {
        if (model->alarms[index].active) {
            active_count++;
        }
    }
    model_unlock(model);
    return active_count;
}

size_t data_model_object_count(const data_model_t* model)
{
    if (! model_lock(model)) {
        return 0;
    }
    size_t count = model->equipment_count;
    model_unlock(model);
    return count;
}

size_t data_model_equipment_count(const data_model_t* model)
{
    return data_model_object_count(model);
}

static size_t count_entity_kind(const data_model_t* model, data_model_entity_kind_t entity_kind)
{
    if (! model_lock(model)) {
        return 0;
    }
    size_t count = 0;
    for (size_t index = 0; index < model->equipment_count; ++index) {
        if (model->equipment[index].entity_kind == entity_kind) {
            ++count;
        }
    }
    model_unlock(model);
    return count;
}

size_t data_model_asset_count(const data_model_t* model)
{
    if (! model_lock(model)) {
        return 0;
    }
    size_t count = 0;
    for (size_t index = 0; index < model->equipment_count; ++index) {
        if (data_model_entity_is_equipment(model->equipment[index].entity_kind)) {
            ++count;
        }
    }
    model_unlock(model);
    return count;
}

size_t data_model_active_equipment_count(const data_model_t* model)
{
    return count_entity_kind(model, DATA_MODEL_ENTITY_ACTIVE_EQUIPMENT);
}

size_t data_model_passive_equipment_count(const data_model_t* model)
{
    return count_entity_kind(model, DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT);
}

size_t data_model_tag_count(const data_model_t* model)
{
    if (! model_lock(model)) {
        return 0;
    }
    size_t count = model->tag_count;
    model_unlock(model);
    return count;
}

uint32_t data_model_structure_generation(const data_model_t* model)
{
    if (! model_lock(model)) {
        return 0;
    }
    uint32_t generation = model->structure_generation;
    model_unlock(model);
    return generation;
}

uint32_t data_model_value_generation(const data_model_t* model)
{
    if (! model_lock(model)) {
        return 0;
    }
    uint32_t generation = model->value_generation;
    model_unlock(model);
    return generation;
}

uint32_t data_model_alarm_generation(const data_model_t* model)
{
    if (!model_lock(model)) {
        return 0;
    }
    uint32_t generation = model->alarm_generation;
    model_unlock(model);
    return generation;
}

const char* data_model_type_name(data_model_type_t data_type)
{
    switch (data_type) {
    case DATA_MODEL_TYPE_BOOLEAN:
        return "BOOL";
    case DATA_MODEL_TYPE_INTEGER:
        return "INT";
    case DATA_MODEL_TYPE_FLOAT:
        return "FLOAT";
    case DATA_MODEL_TYPE_DOUBLE:
        return "DOUBLE";
    case DATA_MODEL_TYPE_STRING:
        return "STRING";
    default:
        return "UNKNOWN";
    }
}

bool data_model_entity_is_equipment(data_model_entity_kind_t entity_kind)
{
    return entity_kind == DATA_MODEL_ENTITY_ACTIVE_EQUIPMENT ||
           entity_kind == DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT;
}

const char* data_model_entity_kind_name(data_model_entity_kind_t entity_kind)
{
    switch (entity_kind) {
    case DATA_MODEL_ENTITY_SYSTEM:
        return "SYSTEM";
    case DATA_MODEL_ENTITY_PROCESS:
        return "PROCESS";
    case DATA_MODEL_ENTITY_ACTIVE_EQUIPMENT:
        return "ACTIVE_EQUIPMENT";
    case DATA_MODEL_ENTITY_PASSIVE_EQUIPMENT:
        return "PASSIVE_EQUIPMENT";
    case DATA_MODEL_ENTITY_GROUP:
        return "GROUP";
    default:
        return "UNKNOWN";
    }
}
