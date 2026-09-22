#include "system.h"

esp_err_t system_init(void)
{
    return ESP_OK;
}

esp_err_t system_post(system_event_id_t event_id, const void *data, size_t data_size)
{
    (void)event_id;
    (void)data;
    (void)data_size;
    return ESP_ERR_NOT_SUPPORTED;
}
