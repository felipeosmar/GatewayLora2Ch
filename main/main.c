/**
 * @file main.c
 * @brief Gateway LoRaWAN 2 Canais - Main Application
 *
 * ESP32-based dual-channel LoRaWAN gateway using two SX1276 modules
 * for simultaneous RX and TX operation.
 *
 * Features:
 * - Dual SX1276 radios (RX continuous + TX on demand)
 * - AU915 frequency plan support (configurable sub-band)
 * - WiFi + Ethernet connectivity with failover
 * - Semtech UDP packet forwarder protocol
 * - NVS configuration storage
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "gateway_config.h"
#include "network_manager.h"
#include "lora_gateway.h"
#include "packet_forwarder.h"

static const char *TAG = "main";

// Forward declarations
static void rx_packet_handler(const lora_rx_packet_t *packet, void *user_data);
static void network_event_handler(net_interface_t interface, net_status_t status, void *user_data);
static void print_gateway_info(void);
static void status_task(void *arg);

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Gateway LoRaWAN 2 Canais");
    ESP_LOGI(TAG, "  ESP32 + Dual SX1276");
    ESP_LOGI(TAG, "========================================");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize configuration
    ESP_LOGI(TAG, "Loading configuration...");
    ESP_ERROR_CHECK(gw_config_init());

    const gateway_config_t *config = gw_config_get();
    print_gateway_info();

    // Initialize network manager
    ESP_LOGI(TAG, "Initializing network...");
    net_manager_config_t net_config = {
        .wifi_enabled = config->wifi.enabled,
        .ethernet_enabled = config->ethernet.enabled,
        .auto_failover = true,
        .preferred_interface = NET_IF_WIFI,
        .event_callback = network_event_handler,
        .callback_user_data = NULL,
    };
    ESP_ERROR_CHECK(net_manager_init(&net_config));

    // Start network
    ESP_ERROR_CHECK(net_manager_start());

    // Wait for network connection
    ESP_LOGI(TAG, "Waiting for network connection...");
    int timeout = 0;
    while (!net_manager_is_connected() && timeout < 30) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        timeout++;
        ESP_LOGI(TAG, "Connecting... (%d s)", timeout);
    }

    if (!net_manager_is_connected()) {
        ESP_LOGW(TAG, "Network not connected, continuing anyway...");
    } else {
        esp_netif_ip_info_t ip_info;
        net_manager_get_ip_info(&ip_info);
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&ip_info.ip));
    }

    // Initialize LoRa Gateway
    ESP_LOGI(TAG, "Initializing LoRa Gateway...");

    gateway_config_t gw_config = {
        .spi_host = SPI2_HOST,
        .rx_callback = rx_packet_handler,
        .rx_user_data = NULL,
        .tx_callback = NULL,
        .tx_user_data = NULL,
    };

    // RX Radio (Radio 0) configuration
    gw_config.radio[0].pins = (sx1276_pins_t){
        .cs = CONFIG_SX1276_RX_CS_GPIO,
        .reset = CONFIG_SX1276_RX_RST_GPIO,
        .dio0 = CONFIG_SX1276_RX_DIO0_GPIO,
        .dio1 = CONFIG_SX1276_RX_DIO1_GPIO,
        .dio2 = CONFIG_SX1276_RX_DIO2_GPIO,
    };
    gw_config.radio[0].config = (sx1276_config_t){
        .frequency = config->lora.channels[0].frequency,
        .sf = config->lora.rx_sf,
        .bw = config->lora.rx_bw,
        .cr = SX1276_CR_4_5,
        .tx_power = config->lora.tx_power,
        .sync_word = config->lora.sync_word,
        .preamble_length = 8,
        .crc_on = true,
        .implicit_header = false,
        .invert_iq_rx = false,  // Uplink: no inversion
        .invert_iq_tx = true,   // Downlink: inverted
    };
    gw_config.radio[0].role = RADIO_ROLE_RX;

    // TX Radio (Radio 1) configuration
    gw_config.radio[1].pins = (sx1276_pins_t){
        .cs = CONFIG_SX1276_TX_CS_GPIO,
        .reset = CONFIG_SX1276_TX_RST_GPIO,
        .dio0 = CONFIG_SX1276_TX_DIO0_GPIO,
        .dio1 = CONFIG_SX1276_TX_DIO1_GPIO,
        .dio2 = CONFIG_SX1276_TX_DIO2_GPIO,
    };
    gw_config.radio[1].config = (sx1276_config_t){
        .frequency = 923300000,  // Default downlink frequency
        .sf = SX1276_SF_12,      // Default for RX2
        .bw = SX1276_BW_500_KHZ,
        .cr = SX1276_CR_4_5,
        .tx_power = config->lora.tx_power,
        .sync_word = config->lora.sync_word,
        .preamble_length = 8,
        .crc_on = true,
        .implicit_header = false,
        .invert_iq_rx = false,
        .invert_iq_tx = true,   // Downlink: inverted
    };
    gw_config.radio[1].role = RADIO_ROLE_TX;

    ret = lora_gateway_init(&gw_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LoRa Gateway init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Check SX1276 connections!");
        // Continue to allow debugging
    } else {
        // Start gateway
        ESP_ERROR_CHECK(lora_gateway_start());
        ESP_LOGI(TAG, "LoRa Gateway started");
    }

    // Initialize Packet Forwarder
    ESP_LOGI(TAG, "Initializing Packet Forwarder...");
    pkt_fwd_config_t pf_config = {0};
    strncpy(pf_config.server_host, config->server.host, sizeof(pf_config.server_host) - 1);
    pf_config.server_port = config->server.port;
    memcpy(pf_config.gateway_eui, config->gateway_eui, 8);
    pf_config.keepalive_interval_ms = config->server.keepalive_interval;
    pf_config.stat_interval_ms = config->server.stat_interval;

    ret = pkt_fwd_init(&pf_config);
    if (ret == ESP_OK && net_manager_is_connected()) {
        pkt_fwd_start();
        ESP_LOGI(TAG, "Packet Forwarder started");
    }

    // Create status monitoring task
    xTaskCreate(status_task, "status_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Gateway Ready!");
    ESP_LOGI(TAG, "========================================");

    // Main loop - keep alive and monitor
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        // Check if network reconnected
        if (net_manager_is_connected() && !pkt_fwd_is_connected()) {
            ESP_LOGI(TAG, "Network reconnected, restarting packet forwarder...");
            pkt_fwd_start();
        }
    }
}

// Callback for received LoRa packets
static void rx_packet_handler(const lora_rx_packet_t *packet, void *user_data)
{
    if (!packet) {
        return;
    }

    ESP_LOGI(TAG, "RX Packet: %d bytes, RSSI=%d dBm, SNR=%.1f dB",
             packet->payload_size, packet->rssi, packet->snr);

    // Log first few bytes as hex
    char hex_str[64];
    int hex_len = (packet->payload_size > 16) ? 16 : packet->payload_size;
    for (int i = 0; i < hex_len; i++) {
        sprintf(&hex_str[i * 3], "%02X ", packet->payload[i]);
    }
    ESP_LOGI(TAG, "Data: %s%s", hex_str, (packet->payload_size > 16) ? "..." : "");

    // Forward to packet forwarder
    if (pkt_fwd_is_connected()) {
        pkt_fwd_send_uplink(packet);
    }
}

// Callback for network events
static void network_event_handler(net_interface_t interface, net_status_t status, void *user_data)
{
    const char *if_name = (interface == NET_IF_WIFI) ? "WiFi" : "Ethernet";

    switch (status) {
        case NET_STATUS_CONNECTED:
            ESP_LOGI(TAG, "%s connected", if_name);
            break;
        case NET_STATUS_DISCONNECTED:
            ESP_LOGW(TAG, "%s disconnected", if_name);
            break;
        case NET_STATUS_CONNECTING:
            ESP_LOGI(TAG, "%s connecting...", if_name);
            break;
        case NET_STATUS_FAILED:
            ESP_LOGE(TAG, "%s connection failed", if_name);
            break;
    }
}

// Print gateway information
static void print_gateway_info(void)
{
    const gateway_config_t *config = gw_config_get();

    char eui_str[17];
    gw_config_get_eui_string(eui_str);

    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "Gateway EUI: %s", eui_str);
    ESP_LOGI(TAG, "Server: %s:%d", config->server.host, config->server.port);
    ESP_LOGI(TAG, "Sub-band: %d", config->lora.subband + 1);
    ESP_LOGI(TAG, "Channels:");

    for (int i = 0; i < GATEWAY_MAX_CHANNELS; i++) {
        if (config->lora.channels[i].enabled) {
            ESP_LOGI(TAG, "  CH%d: %.2f MHz",
                     i, config->lora.channels[i].frequency / 1e6);
        }
    }

    ESP_LOGI(TAG, "WiFi SSID: %s", config->wifi.ssid);
    ESP_LOGI(TAG, "----------------------------------------");
}

// Status monitoring task
static void status_task(void *arg)
{
    gateway_stats_t stats;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));  // Every minute

        if (lora_gateway_is_running()) {
            lora_gateway_get_stats(&stats);

            ESP_LOGI(TAG, "=== Gateway Status ===");
            ESP_LOGI(TAG, "Uptime: %lu s", stats.uptime);
            ESP_LOGI(TAG, "RX: total=%lu, ok=%lu, bad=%lu",
                     stats.rx_total, stats.rx_ok, stats.rx_bad);
            ESP_LOGI(TAG, "TX: total=%lu, ok=%lu, fail=%lu",
                     stats.tx_total, stats.tx_ok, stats.tx_fail);
            ESP_LOGI(TAG, "Network: %s",
                     net_manager_is_connected() ? "Connected" : "Disconnected");
            ESP_LOGI(TAG, "Server: %s",
                     pkt_fwd_is_connected() ? "Connected" : "Disconnected");

            // Print heap info
            ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
        }
    }
}
