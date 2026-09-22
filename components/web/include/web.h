#pragma once

#include "esp_err.h"

typedef enum {
    WEB_STATE_STOPPED = 0,
    WEB_STATE_STARTING,
    WEB_STATE_RUNNING,
    WEB_STATE_ERROR
} web_state_t;

esp_err_t web_init(void);
esp_err_t web_start(void);
esp_err_t web_stop(void);
web_state_t web_get_state(void);
