#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    INPUT_EVENT_NONE = 0,
    INPUT_EVENT_POWER_SHORT,
    INPUT_EVENT_POWER_LONG,
    INPUT_EVENT_UP_SHORT,
    INPUT_EVENT_DOWN_SHORT,
    INPUT_EVENT_LEFT_SHORT,
    INPUT_EVENT_RIGHT_SHORT,
    INPUT_EVENT_OK_SHORT,
    INPUT_EVENT_ENCODER_CW,
    INPUT_EVENT_ENCODER_CCW,
    INPUT_EVENT_ENCODER_CLICK,
    INPUT_EVENT_ENCODER_LONG,
    INPUT_EVENT_MAX
} input_event_t;

typedef struct {
    input_event_t event;
    uint32_t arg;
} input_event_data_t;

esp_err_t input_init(void);
