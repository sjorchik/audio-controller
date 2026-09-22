#pragma once

#include "esp_err.h"

typedef enum {
    POWER_STATE_BOOT = 0,
    POWER_STATE_RUN,
    POWER_STATE_STANDBY,
    POWER_STATE_ERROR
} power_state_t;

esp_err_t power_init(void);
esp_err_t power_set_state(power_state_t state);
power_state_t power_get_state(void);
