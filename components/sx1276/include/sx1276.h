/**
 * @file sx1276.h
 * @brief SX1276 LoRa Module Driver for ESP-IDF
 *
 * Supports multiple instances for dual-radio gateway configuration.
 */

#ifndef SX1276_H
#define SX1276_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX1276_MAX_PACKET_SIZE      255
#define SX1276_FIFO_SIZE            256

/**
 * @brief SX1276 bandwidth settings
 */
typedef enum {
    SX1276_BW_7_8_KHZ = 0,
    SX1276_BW_10_4_KHZ,
    SX1276_BW_15_6_KHZ,
    SX1276_BW_20_8_KHZ,
    SX1276_BW_31_25_KHZ,
    SX1276_BW_41_7_KHZ,
    SX1276_BW_62_5_KHZ,
    SX1276_BW_125_KHZ,
    SX1276_BW_250_KHZ,
    SX1276_BW_500_KHZ
} sx1276_bandwidth_t;

/**
 * @brief SX1276 coding rate settings
 */
typedef enum {
    SX1276_CR_4_5 = 1,
    SX1276_CR_4_6 = 2,
    SX1276_CR_4_7 = 3,
    SX1276_CR_4_8 = 4
} sx1276_coding_rate_t;

/**
 * @brief SX1276 spreading factor settings
 */
typedef enum {
    SX1276_SF_6 = 6,
    SX1276_SF_7 = 7,
    SX1276_SF_8 = 8,
    SX1276_SF_9 = 9,
    SX1276_SF_10 = 10,
    SX1276_SF_11 = 11,
    SX1276_SF_12 = 12
} sx1276_spreading_factor_t;

/**
 * @brief SX1276 operating modes
 */
typedef enum {
    SX1276_MODE_SLEEP = 0,
    SX1276_MODE_STANDBY,
    SX1276_MODE_FSTX,
    SX1276_MODE_TX,
    SX1276_MODE_FSRX,
    SX1276_MODE_RX_CONTINUOUS,
    SX1276_MODE_RX_SINGLE,
    SX1276_MODE_CAD
} sx1276_mode_t;

/**
 * @brief Received packet information
 */
typedef struct {
    uint8_t data[SX1276_MAX_PACKET_SIZE];
    uint8_t length;
    int16_t rssi;
    int8_t snr;
    uint32_t frequency;
    uint32_t timestamp;
    uint8_t sf;
    uint8_t bw;
    uint8_t cr;
    bool crc_ok;
} sx1276_rx_packet_t;

/**
 * @brief TX packet structure
 */
typedef struct {
    uint8_t data[SX1276_MAX_PACKET_SIZE];
    uint8_t length;
    uint32_t frequency;
    int8_t power;
    uint8_t sf;
    uint8_t bw;
    uint8_t cr;
    bool invert_iq;
    uint32_t tx_delay_us;  // Delay before TX (for precise timing)
} sx1276_tx_packet_t;

/**
 * @brief Callback for received packets
 */
typedef void (*sx1276_rx_callback_t)(sx1276_rx_packet_t *packet, void *user_data);

/**
 * @brief Callback for TX complete
 */
typedef void (*sx1276_tx_callback_t)(bool success, void *user_data);

/**
 * @brief SX1276 pin configuration
 */
typedef struct {
    gpio_num_t cs;      // Chip select (NSS)
    gpio_num_t reset;   // Reset pin
    gpio_num_t dio0;    // DIO0 (RX Done / TX Done)
    gpio_num_t dio1;    // DIO1 (RX Timeout / FHSS)
    gpio_num_t dio2;    // DIO2 (FHSS)
} sx1276_pins_t;

/**
 * @brief SX1276 radio configuration
 */
typedef struct {
    uint32_t frequency;                 // Frequency in Hz
    sx1276_spreading_factor_t sf;       // Spreading factor
    sx1276_bandwidth_t bw;              // Bandwidth
    sx1276_coding_rate_t cr;            // Coding rate
    int8_t tx_power;                    // TX power in dBm
    uint8_t sync_word;                  // Sync word (0x34 for LoRaWAN public)
    uint16_t preamble_length;           // Preamble length
    bool crc_on;                        // Enable CRC
    bool implicit_header;               // Use implicit header mode
    bool invert_iq_rx;                  // Invert IQ for RX
    bool invert_iq_tx;                  // Invert IQ for TX
} sx1276_config_t;

/**
 * @brief SX1276 device handle
 */
typedef struct sx1276_dev_s *sx1276_handle_t;

/**
 * @brief Initialize SX1276 device
 *
 * @param spi_host SPI host to use (e.g., SPI2_HOST)
 * @param pins Pin configuration
 * @param config Radio configuration
 * @param handle Output handle
 * @return ESP_OK on success
 */
esp_err_t sx1276_init(spi_host_device_t spi_host, const sx1276_pins_t *pins,
                      const sx1276_config_t *config, sx1276_handle_t *handle);

/**
 * @brief Deinitialize SX1276 device
 *
 * @param handle Device handle
 * @return ESP_OK on success
 */
esp_err_t sx1276_deinit(sx1276_handle_t handle);

/**
 * @brief Set operating mode
 *
 * @param handle Device handle
 * @param mode Operating mode
 * @return ESP_OK on success
 */
esp_err_t sx1276_set_mode(sx1276_handle_t handle, sx1276_mode_t mode);

/**
 * @brief Get current operating mode
 *
 * @param handle Device handle
 * @param mode Output mode
 * @return ESP_OK on success
 */
esp_err_t sx1276_get_mode(sx1276_handle_t handle, sx1276_mode_t *mode);

/**
 * @brief Set frequency
 *
 * @param handle Device handle
 * @param frequency Frequency in Hz
 * @return ESP_OK on success
 */
esp_err_t sx1276_set_frequency(sx1276_handle_t handle, uint32_t frequency);

/**
 * @brief Set spreading factor
 *
 * @param handle Device handle
 * @param sf Spreading factor (6-12)
 * @return ESP_OK on success
 */
esp_err_t sx1276_set_spreading_factor(sx1276_handle_t handle, sx1276_spreading_factor_t sf);

/**
 * @brief Set bandwidth
 *
 * @param handle Device handle
 * @param bw Bandwidth
 * @return ESP_OK on success
 */
esp_err_t sx1276_set_bandwidth(sx1276_handle_t handle, sx1276_bandwidth_t bw);

/**
 * @brief Set coding rate
 *
 * @param handle Device handle
 * @param cr Coding rate
 * @return ESP_OK on success
 */
esp_err_t sx1276_set_coding_rate(sx1276_handle_t handle, sx1276_coding_rate_t cr);

/**
 * @brief Set TX power
 *
 * @param handle Device handle
 * @param power Power in dBm (2-20)
 * @return ESP_OK on success
 */
esp_err_t sx1276_set_tx_power(sx1276_handle_t handle, int8_t power);

/**
 * @brief Set sync word
 *
 * @param handle Device handle
 * @param sync_word Sync word (0x34 for LoRaWAN public)
 * @return ESP_OK on success
 */
esp_err_t sx1276_set_sync_word(sx1276_handle_t handle, uint8_t sync_word);

/**
 * @brief Set IQ inversion
 *
 * @param handle Device handle
 * @param invert_rx Invert IQ for RX
 * @param invert_tx Invert IQ for TX
 * @return ESP_OK on success
 */
esp_err_t sx1276_set_invert_iq(sx1276_handle_t handle, bool invert_rx, bool invert_tx);

/**
 * @brief Start continuous RX mode
 *
 * @param handle Device handle
 * @param callback Callback for received packets (called from ISR context)
 * @param user_data User data passed to callback
 * @return ESP_OK on success
 */
esp_err_t sx1276_start_rx(sx1276_handle_t handle, sx1276_rx_callback_t callback, void *user_data);

/**
 * @brief Stop RX mode
 *
 * @param handle Device handle
 * @return ESP_OK on success
 */
esp_err_t sx1276_stop_rx(sx1276_handle_t handle);

/**
 * @brief Transmit packet
 *
 * @param handle Device handle
 * @param packet Packet to transmit
 * @param callback Callback when TX complete (can be NULL)
 * @param user_data User data passed to callback
 * @return ESP_OK on success
 */
esp_err_t sx1276_transmit(sx1276_handle_t handle, const sx1276_tx_packet_t *packet,
                          sx1276_tx_callback_t callback, void *user_data);

/**
 * @brief Get last packet RSSI
 *
 * @param handle Device handle
 * @return RSSI in dBm
 */
int16_t sx1276_get_packet_rssi(sx1276_handle_t handle);

/**
 * @brief Get last packet SNR
 *
 * @param handle Device handle
 * @return SNR in dB (x4)
 */
int8_t sx1276_get_packet_snr(sx1276_handle_t handle);

/**
 * @brief Get current RSSI
 *
 * @param handle Device handle
 * @return RSSI in dBm
 */
int16_t sx1276_get_rssi(sx1276_handle_t handle);

/**
 * @brief Check if channel is free (CAD)
 *
 * @param handle Device handle
 * @param is_free Output: true if channel is free
 * @return ESP_OK on success
 */
esp_err_t sx1276_channel_free(sx1276_handle_t handle, bool *is_free);

/**
 * @brief Get chip version
 *
 * @param handle Device handle
 * @return Chip version (0x12 for SX1276)
 */
uint8_t sx1276_get_version(sx1276_handle_t handle);

/**
 * @brief Apply full configuration
 *
 * @param handle Device handle
 * @param config Configuration to apply
 * @return ESP_OK on success
 */
esp_err_t sx1276_apply_config(sx1276_handle_t handle, const sx1276_config_t *config);

/**
 * @brief Default configuration for AU915
 */
#define SX1276_CONFIG_DEFAULT_AU915() { \
    .frequency = 916800000,             \
    .sf = SX1276_SF_7,                  \
    .bw = SX1276_BW_125_KHZ,            \
    .cr = SX1276_CR_4_5,                \
    .tx_power = 14,                     \
    .sync_word = 0x34,                  \
    .preamble_length = 8,               \
    .crc_on = true,                     \
    .implicit_header = false,           \
    .invert_iq_rx = false,              \
    .invert_iq_tx = true                \
}

#ifdef __cplusplus
}
#endif

#endif // SX1276_H
