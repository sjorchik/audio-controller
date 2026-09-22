#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SYSTEM_EVENT_INVALID = 0,
    SYSTEM_EVENT_POWER_STATE_CHANGED,
    SYSTEM_EVENT_SOURCE_CHANGED,
    SYSTEM_EVENT_VOLUME_CHANGED,
    SYSTEM_EVENT_INPUT,
    SYSTEM_EVENT_IR,
    SYSTEM_EVENT_MAX
} system_event_id_t;

esp_err_t system_init(void);
esp_err_t system_post(system_event_id_t event_id, const void *data, size_t data_size);
