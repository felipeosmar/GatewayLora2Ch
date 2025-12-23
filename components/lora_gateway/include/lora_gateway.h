/**
 * @file lora_gateway.h
 * @brief LoRa Gateway Core - Dual Radio Management
 */

#ifndef LORA_GATEWAY_H
#define LORA_GATEWAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lora_packet.h"
#include "sx1276.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_RX_QUEUE_SIZE   32
#define GATEWAY_TX_QUEUE_SIZE   16

/**
 * @brief Gateway radio role
 */
typedef enum {
    RADIO_ROLE_RX = 0,      // Dedicated RX radio
    RADIO_ROLE_TX,          // Dedicated TX radio
    RADIO_ROLE_BOTH         // Single radio for both (not recommended)
} radio_role_t;

/**
 * @brief Callback for received packets
 */
typedef void (*gw_rx_callback_t)(const lora_rx_packet_t *packet, void *user_data);

/**
 * @brief Callback for TX complete
 */
typedef void (*gw_tx_callback_t)(bool success, void *user_data);

/**
 * @brief Gateway configuration
 */
typedef struct {
    // Radio configuration
    struct {
        sx1276_pins_t pins;
        sx1276_config_t config;
        radio_role_t role;
    } radio[2];

    // SPI host (shared between radios)
    spi_host_device_t spi_host;

    // Callbacks
    gw_rx_callback_t rx_callback;
    void *rx_user_data;
    gw_tx_callback_t tx_callback;
    void *tx_user_data;

} gateway_config_t;

/**
 * @brief Initialize LoRa gateway
 *
 * @param config Gateway configuration
 * @return ESP_OK on success
 */
esp_err_t lora_gateway_init(const gateway_config_t *config);

/**
 * @brief Deinitialize gateway
 *
 * @return ESP_OK on success
 */
esp_err_t lora_gateway_deinit(void);

/**
 * @brief Start gateway operation
 *
 * @return ESP_OK on success
 */
esp_err_t lora_gateway_start(void);

/**
 * @brief Stop gateway operation
 *
 * @return ESP_OK on success
 */
esp_err_t lora_gateway_stop(void);

/**
 * @brief Queue a packet for transmission
 *
 * @param packet Packet to transmit
 * @return ESP_OK if queued successfully
 */
esp_err_t lora_gateway_send(const lora_tx_packet_t *packet);

/**
 * @brief Get gateway statistics
 *
 * @param stats Output statistics
 * @return ESP_OK on success
 */
esp_err_t lora_gateway_get_stats(gateway_stats_t *stats);

/**
 * @brief Reset gateway statistics
 */
void lora_gateway_reset_stats(void);

/**
 * @brief Set RX frequency
 *
 * @param frequency Frequency in Hz
 * @return ESP_OK on success
 */
esp_err_t lora_gateway_set_rx_frequency(uint32_t frequency);

/**
 * @brief Set RX parameters
 *
 * @param sf Spreading factor (7-12)
 * @param bw Bandwidth
 * @return ESP_OK on success
 */
esp_err_t lora_gateway_set_rx_params(uint8_t sf, uint8_t bw);

/**
 * @brief Get current timestamp (for packet forwarder)
 *
 * @return Timestamp in microseconds
 */
uint32_t lora_gateway_get_timestamp(void);

/**
 * @brief Check if gateway is running
 *
 * @return true if running
 */
bool lora_gateway_is_running(void);

// Channel Manager API

/**
 * @brief Initialize channel manager
 *
 * @param rx_handle RX radio handle
 * @param tx_handle TX radio handle
 * @return ESP_OK on success
 */
esp_err_t channel_manager_init(sx1276_handle_t rx_handle, sx1276_handle_t tx_handle);

/**
 * @brief Start channel manager
 *
 * @return ESP_OK on success
 */
esp_err_t channel_manager_start(void);

/**
 * @brief Stop channel manager
 *
 * @return ESP_OK on success
 */
esp_err_t channel_manager_stop(void);

/**
 * @brief Schedule downlink transmission
 *
 * @param packet Packet to transmit
 * @return ESP_OK if scheduled
 */
esp_err_t channel_manager_schedule_tx(const lora_tx_packet_t *packet);

/**
 * @brief Set channel hopping mode
 *
 * @param enabled Enable channel hopping
 * @param interval_ms Hop interval in ms
 * @return ESP_OK on success
 */
esp_err_t channel_manager_set_hopping(bool enabled, uint32_t interval_ms);

#ifdef __cplusplus
}
#endif

#endif // LORA_GATEWAY_H
