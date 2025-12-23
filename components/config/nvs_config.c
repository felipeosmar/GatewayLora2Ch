/**
 * @file nvs_config.c
 * @brief NVS Configuration Storage Implementation
 */

#include <string.h>
#include "gateway_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "nvs_config";
static const char *NVS_NAMESPACE = "gw_config";
static const char *NVS_KEY_CONFIG = "config_blob";

esp_err_t gw_config_load(gateway_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Open NVS namespace
    nvs_handle_t nvs_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    // Read configuration blob
    size_t required_size = sizeof(gateway_config_t);
    ret = nvs_get_blob(nvs_handle, NVS_KEY_CONFIG, config, &required_size);
    nvs_close(nvs_handle);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Config not found in NVS: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    // Validate config version
    if (config->config_version == 0) {
        ESP_LOGW(TAG, "Invalid config version");
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "Configuration loaded from NVS (version %lu)", config->config_version);
    return ESP_OK;
}

esp_err_t gw_config_save(const gateway_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize NVS if needed
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Open NVS namespace
    nvs_handle_t nvs_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Write configuration blob
    ret = nvs_set_blob(nvs_handle, NVS_KEY_CONFIG, config, sizeof(gateway_config_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // Commit changes
    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Configuration saved to NVS");
    return ESP_OK;
}
