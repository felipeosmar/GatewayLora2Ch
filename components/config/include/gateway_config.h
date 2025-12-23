/**
 * @file gateway_config.h
 * @brief Gateway Configuration Management
 */

#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_EUI_SIZE        8
#define GATEWAY_MAX_CHANNELS    8
#define WIFI_SSID_MAX_LEN       32
#define WIFI_PASS_MAX_LEN       64
#define SERVER_HOST_MAX_LEN     64

/**
 * @brief AU915 frequency plan sub-bands
 */
typedef enum {
    AU915_SB1 = 0,  // Channels 0-7:   915.2 - 916.6 MHz
    AU915_SB2,      // Channels 8-15:  916.8 - 918.2 MHz (TTN default)
    AU915_SB3,      // Channels 16-23: 918.4 - 919.8 MHz
    AU915_SB4,      // Channels 24-31: 920.0 - 921.4 MHz
    AU915_SB5,      // Channels 32-39: 921.6 - 923.0 MHz
    AU915_SB6,      // Channels 40-47: 923.2 - 924.6 MHz
    AU915_SB7,      // Channels 48-55: 924.8 - 926.2 MHz
    AU915_SB8,      // Channels 56-63: 926.4 - 927.8 MHz
    AU915_SB_MAX
} au915_subband_t;

/**
 * @brief Channel configuration
 */
typedef struct {
    uint32_t frequency;     // Frequency in Hz
    uint8_t sf_min;         // Minimum spreading factor
    uint8_t sf_max;         // Maximum spreading factor
    uint8_t bw;             // Bandwidth (0=125, 1=250, 2=500)
    bool enabled;           // Channel enabled
} gw_channel_config_t;

/**
 * @brief LoRa radio configuration
 */
typedef struct {
    au915_subband_t subband;                    // Active sub-band
    gw_channel_config_t channels[GATEWAY_MAX_CHANNELS];
    uint8_t rx_sf;                              // Default RX spreading factor
    uint8_t rx_bw;                              // Default RX bandwidth
    int8_t tx_power;                            // TX power in dBm
    uint8_t sync_word;                          // Sync word (0x34 for public)
} gw_lora_config_t;

/**
 * @brief WiFi configuration
 */
typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASS_MAX_LEN + 1];
    bool enabled;
    uint8_t max_retry;
} gw_wifi_config_t;

/**
 * @brief Ethernet configuration
 */
typedef struct {
    bool enabled;
    bool dhcp;
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
} gw_ethernet_config_t;

/**
 * @brief Server configuration
 */
typedef struct {
    char host[SERVER_HOST_MAX_LEN + 1];
    uint16_t port;
    uint32_t keepalive_interval;    // PULL_DATA interval in ms
    uint32_t stat_interval;         // Statistics interval in ms
} gw_server_config_t;

/**
 * @brief Complete gateway configuration
 */
typedef struct {
    uint8_t gateway_eui[GATEWAY_EUI_SIZE];
    gw_lora_config_t lora;
    gw_wifi_config_t wifi;
    gw_ethernet_config_t ethernet;
    gw_server_config_t server;
    uint32_t config_version;
} gateway_config_t;

/**
 * @brief Initialize configuration system
 *
 * @return ESP_OK on success
 */
esp_err_t gw_config_init(void);

/**
 * @brief Load configuration from NVS
 *
 * @param config Output configuration
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no saved config
 */
esp_err_t gw_config_load(gateway_config_t *config);

/**
 * @brief Save configuration to NVS
 *
 * @param config Configuration to save
 * @return ESP_OK on success
 */
esp_err_t gw_config_save(const gateway_config_t *config);

/**
 * @brief Reset configuration to defaults
 *
 * @param config Output configuration with defaults
 */
void gw_config_defaults(gateway_config_t *config);

/**
 * @brief Get current configuration (read-only)
 *
 * @return Pointer to current configuration
 */
const gateway_config_t *gw_config_get(void);

/**
 * @brief Update configuration
 *
 * @param config New configuration
 * @param save_to_nvs Also save to NVS
 * @return ESP_OK on success
 */
esp_err_t gw_config_update(const gateway_config_t *config, bool save_to_nvs);

/**
 * @brief Get Gateway EUI as string (16 hex chars)
 *
 * @param buffer Output buffer (at least 17 bytes)
 */
void gw_config_get_eui_string(char *buffer);

/**
 * @brief Set Gateway EUI from string
 *
 * @param eui_string 16 character hex string
 * @return ESP_OK on success
 */
esp_err_t gw_config_set_eui_string(const char *eui_string);

/**
 * @brief Get uplink frequency for a channel
 *
 * @param channel Channel number (0-7 within sub-band)
 * @return Frequency in Hz
 */
uint32_t gw_config_get_uplink_freq(uint8_t channel);

/**
 * @brief Get downlink frequency for RX1
 *
 * @param uplink_freq Uplink frequency
 * @return Downlink frequency in Hz
 */
uint32_t gw_config_get_downlink_freq(uint32_t uplink_freq);

/**
 * @brief Get all AU915 sub-band 2 frequencies (TTN default)
 *
 * @param frequencies Array of 8 frequencies
 */
void gw_config_get_subband_frequencies(au915_subband_t subband, uint32_t *frequencies);

#ifdef __cplusplus
}
#endif

#endif // GATEWAY_CONFIG_H
