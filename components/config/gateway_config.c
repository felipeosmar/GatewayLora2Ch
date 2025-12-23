/**
 * @file gateway_config.c
 * @brief Gateway Configuration Implementation
 */

#include <string.h>
#include <stdio.h>
#include "gateway_config.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "gw_config";

// Current active configuration
static gateway_config_t s_config;
static bool s_initialized = false;

// AU915 frequency definitions
#define AU915_FREQ_START_UP     915200000   // First uplink channel
#define AU915_FREQ_STEP_UP      200000      // 200 kHz spacing
#define AU915_FREQ_START_DOWN   923300000   // First downlink channel
#define AU915_FREQ_STEP_DOWN    600000      // 600 kHz spacing

esp_err_t gw_config_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // Try to load from NVS
    esp_err_t ret = gw_config_load(&s_config);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No saved config found, using defaults");
        gw_config_defaults(&s_config);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Configuration initialized");

    return ESP_OK;
}

void gw_config_defaults(gateway_config_t *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(gateway_config_t));

    // Generate Gateway EUI from MAC address
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    config->gateway_eui[0] = mac[0];
    config->gateway_eui[1] = mac[1];
    config->gateway_eui[2] = mac[2];
    config->gateway_eui[3] = 0xFF;
    config->gateway_eui[4] = 0xFE;
    config->gateway_eui[5] = mac[3];
    config->gateway_eui[6] = mac[4];
    config->gateway_eui[7] = mac[5];

    // LoRa defaults - AU915 Sub-band 2 (TTN)
    config->lora.subband = AU915_SB2;
    config->lora.rx_sf = 7;
    config->lora.rx_bw = 0;  // 125 kHz
    config->lora.tx_power = 14;
    config->lora.sync_word = 0x34;  // LoRaWAN public

    // Configure channels for sub-band 2
    for (int i = 0; i < GATEWAY_MAX_CHANNELS; i++) {
        config->lora.channels[i].frequency = AU915_FREQ_START_UP + ((8 + i) * AU915_FREQ_STEP_UP);
        config->lora.channels[i].sf_min = 7;
        config->lora.channels[i].sf_max = 10;
        config->lora.channels[i].bw = 0;  // 125 kHz
        config->lora.channels[i].enabled = true;
    }

    // WiFi defaults
    config->wifi.enabled = true;
    strncpy(config->wifi.ssid, CONFIG_WIFI_SSID, WIFI_SSID_MAX_LEN);
    strncpy(config->wifi.password, CONFIG_WIFI_PASSWORD, WIFI_PASS_MAX_LEN);
    config->wifi.max_retry = CONFIG_WIFI_MAX_RETRY;

    // Ethernet defaults
#ifdef CONFIG_W5500_ENABLED
    config->ethernet.enabled = true;
#else
    config->ethernet.enabled = false;
#endif
    config->ethernet.dhcp = true;

    // Server defaults
    strncpy(config->server.host, CONFIG_LORAWAN_SERVER_HOST, SERVER_HOST_MAX_LEN);
    config->server.port = CONFIG_LORAWAN_SERVER_PORT;
    config->server.keepalive_interval = 10000;  // 10 seconds
    config->server.stat_interval = 30000;       // 30 seconds

    config->config_version = 1;
}

const gateway_config_t *gw_config_get(void)
{
    if (!s_initialized) {
        gw_config_init();
    }
    return &s_config;
}

esp_err_t gw_config_update(const gateway_config_t *config, bool save_to_nvs)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(gateway_config_t));

    if (save_to_nvs) {
        return gw_config_save(config);
    }

    return ESP_OK;
}

void gw_config_get_eui_string(char *buffer)
{
    if (!buffer) {
        return;
    }

    const gateway_config_t *config = gw_config_get();
    for (int i = 0; i < GATEWAY_EUI_SIZE; i++) {
        sprintf(&buffer[i * 2], "%02X", config->gateway_eui[i]);
    }
    buffer[16] = '\0';
}

esp_err_t gw_config_set_eui_string(const char *eui_string)
{
    if (!eui_string || strlen(eui_string) != 16) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t eui[GATEWAY_EUI_SIZE];
    for (int i = 0; i < GATEWAY_EUI_SIZE; i++) {
        char hex[3] = {eui_string[i * 2], eui_string[i * 2 + 1], '\0'};
        eui[i] = (uint8_t)strtol(hex, NULL, 16);
    }

    memcpy(s_config.gateway_eui, eui, GATEWAY_EUI_SIZE);
    return ESP_OK;
}

uint32_t gw_config_get_uplink_freq(uint8_t channel)
{
    const gateway_config_t *config = gw_config_get();

    if (channel >= GATEWAY_MAX_CHANNELS) {
        channel = 0;
    }

    // Calculate frequency based on sub-band and channel
    uint8_t absolute_channel = (config->lora.subband * 8) + channel;
    return AU915_FREQ_START_UP + (absolute_channel * AU915_FREQ_STEP_UP);
}

uint32_t gw_config_get_downlink_freq(uint32_t uplink_freq)
{
    // AU915 downlink mapping:
    // Uplink channels 0-7 -> Downlink channel 0 (923.3 MHz)
    // Uplink channels 8-15 -> Downlink channel 1 (923.9 MHz)
    // etc.

    uint32_t channel = (uplink_freq - AU915_FREQ_START_UP) / AU915_FREQ_STEP_UP;
    uint32_t downlink_channel = channel / 8;

    if (downlink_channel > 7) {
        downlink_channel = 7;
    }

    return AU915_FREQ_START_DOWN + (downlink_channel * AU915_FREQ_STEP_DOWN);
}

void gw_config_get_subband_frequencies(au915_subband_t subband, uint32_t *frequencies)
{
    if (!frequencies || subband >= AU915_SB_MAX) {
        return;
    }

    uint8_t base_channel = subband * 8;
    for (int i = 0; i < 8; i++) {
        frequencies[i] = AU915_FREQ_START_UP + ((base_channel + i) * AU915_FREQ_STEP_UP);
    }
}
