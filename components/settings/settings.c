#include "settings.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "settings";

#define IR_MAP_NAMESPACE "irmap"
#define IR_MAP_BLOB_KEY  "blob"

typedef struct {
    uint8_t version;
    uint16_t count;
    ir_pair_t pairs[32];
} __attribute__((packed)) ir_map_blob_t;

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t settings_load(settings_t *out_settings)
{
    if (out_settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    out_settings->source = BSP_AUDIO_SOURCE_TV_BOX;
    out_settings->volume_db = 0;
    out_settings->display_backlight_on = true;

    for (int i = 0; i < BSP_AUDIO_SOURCE_MAX; ++i) {
        out_settings->source_trim_db[i] = 0;
    }

    ESP_LOGI(TAG, "settings_load: defaults loaded");
    return ESP_OK;
}

esp_err_t settings_save(const settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t settings_ir_map_load(ir_pair_t *pairs, uint16_t *count) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IR_MAP_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        *count = 0;
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    
    ir_map_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(handle, IR_MAP_BLOB_KEY, &blob, &len);
    nvs_close(handle);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *count = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    
    if (blob.version != 1) return ESP_ERR_INVALID_VERSION;
    
    uint16_t c = blob.count;
    if (c > 32) c = 32;
    
    memcpy(pairs, blob.pairs, c * sizeof(ir_pair_t));
    *count = c;
    return ESP_OK;
}

esp_err_t settings_ir_map_save(const ir_pair_t *pairs, uint16_t count) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IR_MAP_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    ir_map_blob_t blob = {0};
    blob.version = 1;
    blob.count = count;
    if (count > 32) count = 32;
    memcpy(blob.pairs, pairs, count * sizeof(ir_pair_t));
    
    err = nvs_set_blob(handle, IR_MAP_BLOB_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}