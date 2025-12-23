/**
 * @file packet_forwarder.h
 * @brief Semtech UDP Packet Forwarder Protocol
 */

#ifndef PACKET_FORWARDER_H
#define PACKET_FORWARDER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lora_packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Packet forwarder configuration
 */
typedef struct {
    char server_host[64];
    uint16_t server_port;
    uint8_t gateway_eui[8];
    uint32_t keepalive_interval_ms;
    uint32_t stat_interval_ms;
} pkt_fwd_config_t;

/**
 * @brief Initialize packet forwarder
 *
 * @param config Configuration
 * @return ESP_OK on success
 */
esp_err_t pkt_fwd_init(const pkt_fwd_config_t *config);

/**
 * @brief Start packet forwarder
 *
 * @return ESP_OK on success
 */
esp_err_t pkt_fwd_start(void);

/**
 * @brief Stop packet forwarder
 *
 * @return ESP_OK on success
 */
esp_err_t pkt_fwd_stop(void);

/**
 * @brief Send uplink packet to server
 *
 * @param packet Received packet
 * @return ESP_OK on success
 */
esp_err_t pkt_fwd_send_uplink(const lora_rx_packet_t *packet);

/**
 * @brief Get forwarder status
 *
 * @param status Output status
 * @return ESP_OK on success
 */
esp_err_t pkt_fwd_get_status(forwarder_status_t *status);

/**
 * @brief Check if connected to server
 *
 * @return true if connected
 */
bool pkt_fwd_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // PACKET_FORWARDER_H
