#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    IR_COMMAND_UNKNOWN = 0,
    IR_COMMAND_POWER,
    IR_COMMAND_VOLUME_UP,
    IR_COMMAND_VOLUME_DOWN,
    IR_COMMAND_SOURCE,
    IR_COMMAND_MUTE,
    IR_COMMAND_MAX
} ir_command_t;

typedef struct {
    ir_command_t command;
    uint32_t raw;
} ir_event_data_t;

esp_err_t ir_init(void);
