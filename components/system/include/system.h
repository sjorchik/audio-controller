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

/* ────────────────────────────────────────────────────────────────
 * Payload для SYSTEM_EVENT_INPUT
 * Перенесено сюди для уникнення циклічних залежностей між
 * компонентами system та input (input викликає system_post,
 * а system має знати структуру для evmon).
 * ──────────────────────────────────────────────────────────────── */

typedef enum {
    INPUT_SRC_NONE = 0,
    INPUT_SRC_BTN_POWER,
    INPUT_SRC_BTN_UP,
    INPUT_SRC_BTN_DOWN,
    INPUT_SRC_BTN_LEFT,
    INPUT_SRC_BTN_RIGHT,
    INPUT_SRC_BTN_OK,
    INPUT_SRC_ENC_BTN,
    INPUT_SRC_ENC_STEP,
    INPUT_SRC_MAX
} input_source_t;

typedef enum {
    INPUT_ACTION_NONE = 0,
    INPUT_ACTION_SHORT,
    INPUT_ACTION_LONG,
    INPUT_ACTION_REPEAT,
    INPUT_ACTION_STEP,
} input_action_t;

typedef struct {
    input_source_t source;
    input_action_t action;
    int8_t arg; // Для STEP: +1 або -1. Для інших: 0.
    uint32_t timestamp_ms;
} input_event_t;

esp_err_t system_init(void);
esp_err_t system_post(system_event_id_t event_id, const void *data, size_t data_size);