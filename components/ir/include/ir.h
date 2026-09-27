#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "settings.h"

typedef enum {
    IR_ACTION_NONE = 0,
    IR_ACTION_VOL_UP,
    IR_ACTION_VOL_DOWN,
    IR_ACTION_MUTE_TOGGLE,
    IR_ACTION_SRC_NEXT,
    IR_ACTION_SRC_PREV,
    IR_ACTION_POWER_TOGGLE,
    IR_ACTION_MAX
} ir_action_t;

typedef struct {
    uint32_t frames_ok;
    uint32_t manchester_errors;
    uint32_t crc_errors;
    uint32_t drops;
    uint32_t rx_blocks;
    float latency_ema_ms;
} ir_stats_t;

esp_err_t ir_init(void);
void ir_get_stats(ir_stats_t *stats);
esp_err_t ir_get_rmt_err(void);
void ir_dump_last_block(void);
esp_err_t ir_map_remove(uint8_t system, uint8_t command);
esp_err_t ir_map_clear(void);
const char* ir_action_to_string(ir_action_t action);
ir_action_t ir_action_from_string(const char *str);
void ir_dev_register_commands(void);