#include "power.h"
#include "esp_log.h"

static const char *TAG = "power";
static power_state_t s_state = POWER_STATE_BOOT;

esp_err_t power_init(void)
{
    ESP_LOGI(TAG, "power_init: stub");
    s_state = POWER_STATE_BOOT;
    return ESP_OK;
}

esp_err_t power_set_state(power_state_t state)
{
    ESP_LOGI(TAG, "power_set_state: %d -> %d", (int)s_state, (int)state);
    s_state = state;
    return ESP_OK;
}

power_state_t power_get_state(void)
{
    return s_state;
}
