/**
 * @file lora_packet.h
 * @brief LoRa Packet Structures for Gateway
 */

#ifndef LORA_PACKET_H
#define LORA_PACKET_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_MAX_PAYLOAD_SIZE   255
#define LORA_EUI_SIZE           8

/**
 * @brief LoRa modulation parameters
 */
typedef struct {
    uint32_t frequency;     // Frequency in Hz
    uint8_t bandwidth;      // 0=125kHz, 1=250kHz, 2=500kHz
    uint8_t spreading_factor;  // 7-12
    uint8_t coding_rate;    // 1=4/5, 2=4/6, 3=4/7, 4=4/8
    bool invert_polarity;   // IQ inversion
} lora_modulation_t;

/**
 * @brief Received uplink packet
 */
typedef struct {
    // Payload
    uint8_t payload[LORA_MAX_PAYLOAD_SIZE];
    uint8_t payload_size;

    // Modulation info
    lora_modulation_t modulation;

    // Reception quality
    int16_t rssi;           // RSSI in dBm
    float snr;              // SNR in dB
    bool crc_ok;            // CRC status

    // Timing
    uint32_t timestamp;     // Internal timestamp (microseconds)
    uint32_t tmst;          // Gateway timestamp for packet forwarder

    // Channel info
    uint8_t rf_chain;       // RF chain (0 or 1)
    uint8_t if_chain;       // IF channel

} lora_rx_packet_t;

/**
 * @brief Downlink packet to transmit
 */
typedef struct {
    // Payload
    uint8_t payload[LORA_MAX_PAYLOAD_SIZE];
    uint8_t payload_size;

    // Modulation settings
    lora_modulation_t modulation;

    // TX parameters
    int8_t tx_power;        // TX power in dBm
    bool immediate;         // TX immediately or use timestamp
    uint32_t tx_timestamp;  // Timestamp for TX (if not immediate)

    // For Class B/C
    uint8_t rf_chain;       // RF chain to use

} lora_tx_packet_t;

/**
 * @brief Gateway statistics
 */
typedef struct {
    // Uplink stats
    uint32_t rx_total;      // Total packets received
    uint32_t rx_ok;         // Packets with valid CRC
    uint32_t rx_bad;        // Packets with CRC error
    uint32_t rx_forwarded;  // Packets forwarded to server

    // Downlink stats
    uint32_t tx_total;      // Total TX requests
    uint32_t tx_ok;         // Successful transmissions
    uint32_t tx_fail;       // Failed transmissions
    uint32_t tx_collision;  // TX aborted (collision)

    // Timing
    uint32_t uptime;        // Gateway uptime in seconds
    int64_t last_rx_time;   // Last RX timestamp
    int64_t last_tx_time;   // Last TX timestamp

} gateway_stats_t;

/**
 * @brief Packet forwarder status
 */
typedef struct {
    bool connected;         // Connected to server
    uint32_t push_ack;      // PUSH_ACK count
    uint32_t pull_ack;      // PULL_ACK count
    int32_t latency_ms;     // Average latency in ms
} forwarder_status_t;

#ifdef __cplusplus
}
#endif

#endif // LORA_PACKET_H
