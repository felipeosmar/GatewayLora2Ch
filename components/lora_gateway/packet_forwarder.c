/**
 * @file packet_forwarder.c
 * @brief Semtech UDP Packet Forwarder Implementation
 *
 * Implements the Semtech UDP protocol for LoRaWAN gateways:
 * - PUSH_DATA (0x00): Gateway -> Server (uplink data)
 * - PUSH_ACK  (0x01): Server -> Gateway (acknowledge)
 * - PULL_DATA (0x02): Gateway -> Server (keepalive/poll)
 * - PULL_RESP (0x03): Server -> Gateway (downlink data)
 * - PULL_ACK  (0x04): Server -> Gateway (acknowledge)
 * - TX_ACK    (0x05): Gateway -> Server (TX confirm)
 */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "packet_forwarder.h"
#include "lora_gateway.h"
#include "network_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "cJSON.h"

static const char *TAG = "pkt_fwd";

// Protocol identifiers
#define PROTOCOL_VERSION        2
#define PKT_PUSH_DATA           0x00
#define PKT_PUSH_ACK            0x01
#define PKT_PULL_DATA           0x02
#define PKT_PULL_RESP           0x03
#define PKT_PULL_ACK            0x04
#define PKT_TX_ACK              0x05

// Buffer sizes
#define UDP_BUFFER_SIZE         2048
#define JSON_BUFFER_SIZE        1024
#define MAX_UPLINK_BATCH        8

// Packet forwarder state
typedef struct {
    pkt_fwd_config_t config;

    // Socket
    int sock;
    struct sockaddr_in server_addr;

    // Token management
    uint16_t push_token;
    uint16_t pull_token;

    // Tasks and timers
    TaskHandle_t rx_task;
    TaskHandle_t tx_task;
    TimerHandle_t keepalive_timer;
    TimerHandle_t stat_timer;

    // Uplink queue
    QueueHandle_t uplink_queue;

    // Statistics
    forwarder_status_t status;
    uint32_t push_sent;
    uint32_t pull_sent;
    int64_t last_pull_ack;

    // State
    bool initialized;
    bool running;

} pkt_fwd_state_t;

static pkt_fwd_state_t s_pf = {0};

// Forward declarations
static void rx_task(void *arg);
static void tx_task(void *arg);
static void keepalive_callback(TimerHandle_t timer);
static void stat_callback(TimerHandle_t timer);
static esp_err_t send_push_data(const lora_rx_packet_t *packets, int count);
static esp_err_t send_pull_data(void);
static esp_err_t send_tx_ack(uint16_t token, const char *error);
static void handle_pull_resp(const uint8_t *data, int len);
static void encode_base64(const uint8_t *data, int len, char *output);
static int decode_base64(const char *input, uint8_t *output, int max_len);
static const char *get_datr_string(uint8_t sf, uint8_t bw);
static const char *get_codr_string(uint8_t cr);

esp_err_t pkt_fwd_init(const pkt_fwd_config_t *config)
{
    if (s_pf.initialized) {
        return ESP_OK;
    }

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing Packet Forwarder...");
    ESP_LOGI(TAG, "Server: %s:%d", config->server_host, config->server_port);

    memset(&s_pf, 0, sizeof(pkt_fwd_state_t));
    memcpy(&s_pf.config, config, sizeof(pkt_fwd_config_t));
    s_pf.sock = -1;

    // Create uplink queue
    s_pf.uplink_queue = xQueueCreate(32, sizeof(lora_rx_packet_t));
    if (!s_pf.uplink_queue) {
        ESP_LOGE(TAG, "Failed to create uplink queue");
        return ESP_ERR_NO_MEM;
    }

    // Create keepalive timer
    s_pf.keepalive_timer = xTimerCreate("pf_keepalive",
                                         pdMS_TO_TICKS(config->keepalive_interval_ms),
                                         pdTRUE,
                                         NULL,
                                         keepalive_callback);

    // Create statistics timer
    s_pf.stat_timer = xTimerCreate("pf_stat",
                                    pdMS_TO_TICKS(config->stat_interval_ms),
                                    pdTRUE,
                                    NULL,
                                    stat_callback);

    s_pf.initialized = true;
    ESP_LOGI(TAG, "Packet Forwarder initialized");

    return ESP_OK;
}

esp_err_t pkt_fwd_start(void)
{
    if (!s_pf.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_pf.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting Packet Forwarder...");

    // Resolve server address
    struct hostent *server = gethostbyname(s_pf.config.server_host);
    if (!server) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", s_pf.config.server_host);
        return ESP_FAIL;
    }

    memset(&s_pf.server_addr, 0, sizeof(s_pf.server_addr));
    s_pf.server_addr.sin_family = AF_INET;
    s_pf.server_addr.sin_port = htons(s_pf.config.server_port);
    memcpy(&s_pf.server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    ESP_LOGI(TAG, "Server resolved: %s", inet_ntoa(s_pf.server_addr.sin_addr));

    // Create UDP socket
    s_pf.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_pf.sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return ESP_FAIL;
    }

    // Set socket timeout for receiving
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(s_pf.sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Create RX task (receives from server)
    xTaskCreatePinnedToCore(rx_task, "pf_rx", 4096, NULL, 7, &s_pf.rx_task, 0);

    // Create TX task (sends to server)
    xTaskCreatePinnedToCore(tx_task, "pf_tx", 8192, NULL, 8, &s_pf.tx_task, 0);

    // Start timers
    xTimerStart(s_pf.keepalive_timer, 0);
    xTimerStart(s_pf.stat_timer, 0);

    s_pf.running = true;

    // Send initial PULL_DATA
    send_pull_data();

    ESP_LOGI(TAG, "Packet Forwarder started");
    return ESP_OK;
}

esp_err_t pkt_fwd_stop(void)
{
    if (!s_pf.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping Packet Forwarder...");

    s_pf.running = false;

    // Stop timers
    xTimerStop(s_pf.keepalive_timer, 0);
    xTimerStop(s_pf.stat_timer, 0);

    // Delete tasks
    if (s_pf.rx_task) {
        vTaskDelete(s_pf.rx_task);
        s_pf.rx_task = NULL;
    }
    if (s_pf.tx_task) {
        vTaskDelete(s_pf.tx_task);
        s_pf.tx_task = NULL;
    }

    // Close socket
    if (s_pf.sock >= 0) {
        close(s_pf.sock);
        s_pf.sock = -1;
    }

    s_pf.status.connected = false;
    ESP_LOGI(TAG, "Packet Forwarder stopped");

    return ESP_OK;
}

esp_err_t pkt_fwd_send_uplink(const lora_rx_packet_t *packet)
{
    if (!s_pf.running || !packet) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_pf.uplink_queue, packet, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Uplink queue full");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t pkt_fwd_get_status(forwarder_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(status, &s_pf.status, sizeof(forwarder_status_t));
    return ESP_OK;
}

bool pkt_fwd_is_connected(void)
{
    return s_pf.status.connected;
}

// Internal: RX task - receives packets from server
static void rx_task(void *arg)
{
    uint8_t buffer[UDP_BUFFER_SIZE];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    ESP_LOGI(TAG, "RX task started");

    while (s_pf.running) {
        int len = recvfrom(s_pf.sock, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&from_addr, &from_len);

        if (len < 4) {
            continue;
        }

        // Validate protocol version
        if (buffer[0] != PROTOCOL_VERSION) {
            ESP_LOGW(TAG, "Invalid protocol version: %d", buffer[0]);
            continue;
        }

        uint16_t token = (buffer[1] << 8) | buffer[2];
        uint8_t type = buffer[3];

        switch (type) {
            case PKT_PUSH_ACK:
                ESP_LOGD(TAG, "PUSH_ACK received (token: %04X)", token);
                s_pf.status.push_ack++;
                break;

            case PKT_PULL_ACK:
                ESP_LOGD(TAG, "PULL_ACK received (token: %04X)", token);
                s_pf.status.pull_ack++;
                s_pf.status.connected = true;
                s_pf.last_pull_ack = esp_timer_get_time();
                break;

            case PKT_PULL_RESP:
                ESP_LOGI(TAG, "PULL_RESP received (%d bytes)", len);
                handle_pull_resp(buffer, len);
                break;

            default:
                ESP_LOGW(TAG, "Unknown packet type: %d", type);
                break;
        }
    }

    ESP_LOGI(TAG, "RX task stopped");
    vTaskDelete(NULL);
}

// Internal: TX task - sends packets to server
static void tx_task(void *arg)
{
    lora_rx_packet_t packets[MAX_UPLINK_BATCH];
    int batch_count = 0;

    ESP_LOGI(TAG, "TX task started");

    while (s_pf.running) {
        batch_count = 0;

        // Collect packets (batch up to MAX_UPLINK_BATCH)
        while (batch_count < MAX_UPLINK_BATCH) {
            TickType_t wait = (batch_count == 0) ? pdMS_TO_TICKS(100) : 0;
            if (xQueueReceive(s_pf.uplink_queue, &packets[batch_count], wait) == pdTRUE) {
                batch_count++;
            } else {
                break;
            }
        }

        // Send batch if we have packets
        if (batch_count > 0) {
            send_push_data(packets, batch_count);
        }
    }

    ESP_LOGI(TAG, "TX task stopped");
    vTaskDelete(NULL);
}

// Internal: Send PUSH_DATA packet
static esp_err_t send_push_data(const lora_rx_packet_t *packets, int count)
{
    if (s_pf.sock < 0 || count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buffer[UDP_BUFFER_SIZE];
    int offset = 0;

    // Header
    buffer[offset++] = PROTOCOL_VERSION;
    s_pf.push_token++;
    buffer[offset++] = (s_pf.push_token >> 8) & 0xFF;
    buffer[offset++] = s_pf.push_token & 0xFF;
    buffer[offset++] = PKT_PUSH_DATA;

    // Gateway EUI
    memcpy(&buffer[offset], s_pf.config.gateway_eui, 8);
    offset += 8;

    // Build JSON payload
    cJSON *root = cJSON_CreateObject();
    cJSON *rxpk = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        const lora_rx_packet_t *pkt = &packets[i];

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "tmst", pkt->tmst);
        cJSON_AddNumberToObject(item, "freq", pkt->modulation.frequency / 1e6);
        cJSON_AddNumberToObject(item, "chan", pkt->rf_chain);
        cJSON_AddNumberToObject(item, "rfch", pkt->rf_chain);
        cJSON_AddStringToObject(item, "stat", pkt->crc_ok ? "OK" : "CRC");
        cJSON_AddStringToObject(item, "modu", "LORA");
        cJSON_AddStringToObject(item, "datr", get_datr_string(pkt->modulation.spreading_factor,
                                                              pkt->modulation.bandwidth));
        cJSON_AddStringToObject(item, "codr", get_codr_string(pkt->modulation.coding_rate));
        cJSON_AddNumberToObject(item, "rssi", pkt->rssi);
        cJSON_AddNumberToObject(item, "lsnr", pkt->snr);
        cJSON_AddNumberToObject(item, "size", pkt->payload_size);

        // Base64 encode payload
        char b64[512];
        encode_base64(pkt->payload, pkt->payload_size, b64);
        cJSON_AddStringToObject(item, "data", b64);

        cJSON_AddItemToArray(rxpk, item);
    }

    cJSON_AddItemToObject(root, "rxpk", rxpk);

    char *json = cJSON_PrintUnformatted(root);
    int json_len = strlen(json);
    cJSON_Delete(root);

    if (offset + json_len >= UDP_BUFFER_SIZE) {
        ESP_LOGE(TAG, "PUSH_DATA too large");
        free(json);
        return ESP_ERR_NO_MEM;
    }

    memcpy(&buffer[offset], json, json_len);
    offset += json_len;
    free(json);

    // Send
    int sent = sendto(s_pf.sock, buffer, offset, 0,
                      (struct sockaddr *)&s_pf.server_addr, sizeof(s_pf.server_addr));
    if (sent != offset) {
        ESP_LOGE(TAG, "PUSH_DATA send failed");
        return ESP_FAIL;
    }

    s_pf.push_sent++;
    ESP_LOGI(TAG, "PUSH_DATA sent (%d packets, %d bytes)", count, offset);

    return ESP_OK;
}

// Internal: Send PULL_DATA packet
static esp_err_t send_pull_data(void)
{
    if (s_pf.sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buffer[12];

    buffer[0] = PROTOCOL_VERSION;
    s_pf.pull_token++;
    buffer[1] = (s_pf.pull_token >> 8) & 0xFF;
    buffer[2] = s_pf.pull_token & 0xFF;
    buffer[3] = PKT_PULL_DATA;
    memcpy(&buffer[4], s_pf.config.gateway_eui, 8);

    int sent = sendto(s_pf.sock, buffer, 12, 0,
                      (struct sockaddr *)&s_pf.server_addr, sizeof(s_pf.server_addr));
    if (sent != 12) {
        ESP_LOGE(TAG, "PULL_DATA send failed");
        return ESP_FAIL;
    }

    s_pf.pull_sent++;
    ESP_LOGD(TAG, "PULL_DATA sent (token: %04X)", s_pf.pull_token);

    return ESP_OK;
}

// Internal: Send TX_ACK packet
static esp_err_t send_tx_ack(uint16_t token, const char *error)
{
    if (s_pf.sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buffer[UDP_BUFFER_SIZE];
    int offset = 0;

    buffer[offset++] = PROTOCOL_VERSION;
    buffer[offset++] = (token >> 8) & 0xFF;
    buffer[offset++] = token & 0xFF;
    buffer[offset++] = PKT_TX_ACK;
    memcpy(&buffer[offset], s_pf.config.gateway_eui, 8);
    offset += 8;

    // Add JSON payload with error if present
    if (error) {
        cJSON *root = cJSON_CreateObject();
        cJSON *txpk_ack = cJSON_CreateObject();
        cJSON_AddStringToObject(txpk_ack, "error", error);
        cJSON_AddItemToObject(root, "txpk_ack", txpk_ack);

        char *json = cJSON_PrintUnformatted(root);
        strcpy((char *)&buffer[offset], json);
        offset += strlen(json);
        free(json);
        cJSON_Delete(root);
    }

    sendto(s_pf.sock, buffer, offset, 0,
           (struct sockaddr *)&s_pf.server_addr, sizeof(s_pf.server_addr));

    ESP_LOGD(TAG, "TX_ACK sent (error: %s)", error ? error : "none");
    return ESP_OK;
}

// Internal: Handle PULL_RESP (downlink)
static void handle_pull_resp(const uint8_t *data, int len)
{
    if (len < 4) {
        return;
    }

    uint16_t token = (data[1] << 8) | data[2];
    const char *json_str = (const char *)&data[4];

    ESP_LOGI(TAG, "PULL_RESP JSON: %s", json_str);

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON in PULL_RESP");
        send_tx_ack(token, "INVALID_JSON");
        return;
    }

    cJSON *txpk = cJSON_GetObjectItem(root, "txpk");
    if (!txpk) {
        ESP_LOGE(TAG, "Missing txpk in PULL_RESP");
        cJSON_Delete(root);
        send_tx_ack(token, "MISSING_TXPK");
        return;
    }

    // Parse downlink parameters
    lora_tx_packet_t tx_pkt = {0};

    cJSON *imme = cJSON_GetObjectItem(txpk, "imme");
    tx_pkt.immediate = imme && cJSON_IsTrue(imme);

    cJSON *tmst = cJSON_GetObjectItem(txpk, "tmst");
    if (tmst && cJSON_IsNumber(tmst)) {
        tx_pkt.tx_timestamp = (uint32_t)tmst->valuedouble;
    }

    cJSON *freq = cJSON_GetObjectItem(txpk, "freq");
    if (freq && cJSON_IsNumber(freq)) {
        tx_pkt.modulation.frequency = (uint32_t)(freq->valuedouble * 1e6);
    }

    cJSON *powe = cJSON_GetObjectItem(txpk, "powe");
    tx_pkt.tx_power = powe ? powe->valueint : 14;

    cJSON *datr = cJSON_GetObjectItem(txpk, "datr");
    if (datr && cJSON_IsString(datr)) {
        // Parse "SF7BW125" format
        int sf, bw;
        if (sscanf(datr->valuestring, "SF%dBW%d", &sf, &bw) == 2) {
            tx_pkt.modulation.spreading_factor = sf;
            tx_pkt.modulation.bandwidth = (bw == 500) ? 2 : (bw == 250) ? 1 : 0;
        }
    }

    cJSON *codr = cJSON_GetObjectItem(txpk, "codr");
    if (codr && cJSON_IsString(codr)) {
        // Parse "4/5" format
        int num, den;
        if (sscanf(codr->valuestring, "%d/%d", &num, &den) == 2) {
            tx_pkt.modulation.coding_rate = den - 4;
        }
    }

    cJSON *ipol = cJSON_GetObjectItem(txpk, "ipol");
    tx_pkt.modulation.invert_polarity = ipol && cJSON_IsTrue(ipol);

    cJSON *data_b64 = cJSON_GetObjectItem(txpk, "data");
    if (data_b64 && cJSON_IsString(data_b64)) {
        tx_pkt.payload_size = decode_base64(data_b64->valuestring,
                                            tx_pkt.payload,
                                            LORA_MAX_PAYLOAD_SIZE);
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "TX request: freq=%.2f MHz, SF%d, %d bytes, %s",
             tx_pkt.modulation.frequency / 1e6,
             tx_pkt.modulation.spreading_factor,
             tx_pkt.payload_size,
             tx_pkt.immediate ? "immediate" : "scheduled");

    // Send to gateway
    esp_err_t ret = lora_gateway_send(&tx_pkt);
    if (ret == ESP_OK) {
        send_tx_ack(token, NULL);
    } else {
        send_tx_ack(token, "TX_FAILED");
    }
}

// Internal: Keepalive timer callback
static void keepalive_callback(TimerHandle_t timer)
{
    if (!s_pf.running) {
        return;
    }

    send_pull_data();

    // Check connection status
    int64_t now = esp_timer_get_time();
    if (now - s_pf.last_pull_ack > 30000000) {  // 30 seconds
        if (s_pf.status.connected) {
            ESP_LOGW(TAG, "Server connection lost");
            s_pf.status.connected = false;
        }
    }
}

// Internal: Statistics timer callback
static void stat_callback(TimerHandle_t timer)
{
    if (!s_pf.running) {
        return;
    }

    // Send gateway statistics
    gateway_stats_t gw_stats;
    lora_gateway_get_stats(&gw_stats);

    uint8_t buffer[UDP_BUFFER_SIZE];
    int offset = 0;

    buffer[offset++] = PROTOCOL_VERSION;
    s_pf.push_token++;
    buffer[offset++] = (s_pf.push_token >> 8) & 0xFF;
    buffer[offset++] = s_pf.push_token & 0xFF;
    buffer[offset++] = PKT_PUSH_DATA;
    memcpy(&buffer[offset], s_pf.config.gateway_eui, 8);
    offset += 8;

    // Build stats JSON
    cJSON *root = cJSON_CreateObject();
    cJSON *stat = cJSON_CreateObject();

    // Get current time
    time_t now;
    time(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S GMT", gmtime(&now));
    cJSON_AddStringToObject(stat, "time", time_str);

    cJSON_AddNumberToObject(stat, "rxnb", gw_stats.rx_total);
    cJSON_AddNumberToObject(stat, "rxok", gw_stats.rx_ok);
    cJSON_AddNumberToObject(stat, "rxfw", gw_stats.rx_forwarded);
    cJSON_AddNumberToObject(stat, "ackr", 100.0);  // ACK ratio
    cJSON_AddNumberToObject(stat, "dwnb", gw_stats.tx_total);
    cJSON_AddNumberToObject(stat, "txnb", gw_stats.tx_ok);

    cJSON_AddItemToObject(root, "stat", stat);

    char *json = cJSON_PrintUnformatted(root);
    strcpy((char *)&buffer[offset], json);
    offset += strlen(json);
    free(json);
    cJSON_Delete(root);

    sendto(s_pf.sock, buffer, offset, 0,
           (struct sockaddr *)&s_pf.server_addr, sizeof(s_pf.server_addr));

    ESP_LOGD(TAG, "Stats sent: rx=%lu, tx=%lu", gw_stats.rx_total, gw_stats.tx_total);
}

// Internal: Base64 encoding
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void encode_base64(const uint8_t *data, int len, char *output)
{
    int i, j;
    for (i = 0, j = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) n |= data[i + 2];

        output[j++] = b64_table[(n >> 18) & 0x3F];
        output[j++] = b64_table[(n >> 12) & 0x3F];
        output[j++] = (i + 1 < len) ? b64_table[(n >> 6) & 0x3F] : '=';
        output[j++] = (i + 2 < len) ? b64_table[n & 0x3F] : '=';
    }
    output[j] = '\0';
}

static int decode_base64(const char *input, uint8_t *output, int max_len)
{
    int len = strlen(input);
    int out_len = 0;

    for (int i = 0; i < len && out_len < max_len; i += 4) {
        uint32_t n = 0;
        for (int j = 0; j < 4 && (i + j) < len; j++) {
            char c = input[i + j];
            uint8_t v = 0;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+') v = 62;
            else if (c == '/') v = 63;
            else if (c == '=') v = 0;
            n = (n << 6) | v;
        }

        if (out_len < max_len) output[out_len++] = (n >> 16) & 0xFF;
        if (out_len < max_len && input[i + 2] != '=') output[out_len++] = (n >> 8) & 0xFF;
        if (out_len < max_len && input[i + 3] != '=') output[out_len++] = n & 0xFF;
    }

    return out_len;
}

static const char *get_datr_string(uint8_t sf, uint8_t bw)
{
    static char datr[16];
    int bw_khz = (bw == 2) ? 500 : (bw == 1) ? 250 : 125;
    snprintf(datr, sizeof(datr), "SF%dBW%d", sf, bw_khz);
    return datr;
}

static const char *get_codr_string(uint8_t cr)
{
    switch (cr) {
        case 1: return "4/5";
        case 2: return "4/6";
        case 3: return "4/7";
        case 4: return "4/8";
        default: return "4/5";
    }
}
