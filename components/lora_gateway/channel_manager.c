/**
 * @file channel_manager.c
 * @brief Dual Radio Channel Management
 *
 * Manages RX and TX radios independently:
 * - Radio 0: Continuous RX
 * - Radio 1: TX on demand
 */

#include <string.h>
#include "lora_gateway.h"
#include "gateway_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "ch_manager";

// Channel manager state
typedef struct {
    sx1276_handle_t rx_radio;
    sx1276_handle_t tx_radio;

    // TX queue
    QueueHandle_t tx_queue;

    // Tasks
    TaskHandle_t tx_task_handle;

    // State
    bool running;
    bool tx_busy;

    // Channel hopping
    bool hopping_enabled;
    uint32_t hop_interval_ms;
    uint8_t current_channel;
    TimerHandle_t hop_timer;

    // Synchronization
    SemaphoreHandle_t tx_mutex;

} channel_manager_t;

static channel_manager_t s_cm = {0};

// Forward declarations
static void tx_task(void *arg);
static void hop_timer_callback(TimerHandle_t timer);
static void rx_callback(sx1276_rx_packet_t *packet, void *user_data);
static void tx_done_callback(bool success, void *user_data);

esp_err_t channel_manager_init(sx1276_handle_t rx_handle, sx1276_handle_t tx_handle)
{
    if (!rx_handle || !tx_handle) {
        ESP_LOGE(TAG, "Invalid radio handles");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing Channel Manager...");

    memset(&s_cm, 0, sizeof(channel_manager_t));

    s_cm.rx_radio = rx_handle;
    s_cm.tx_radio = tx_handle;

    // Create TX queue
    s_cm.tx_queue = xQueueCreate(GATEWAY_TX_QUEUE_SIZE, sizeof(lora_tx_packet_t));
    if (!s_cm.tx_queue) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return ESP_ERR_NO_MEM;
    }

    // Create TX mutex
    s_cm.tx_mutex = xSemaphoreCreateMutex();
    if (!s_cm.tx_mutex) {
        ESP_LOGE(TAG, "Failed to create TX mutex");
        vQueueDelete(s_cm.tx_queue);
        return ESP_ERR_NO_MEM;
    }

    // Create hop timer (disabled by default)
    s_cm.hop_timer = xTimerCreate("ch_hop",
                                   pdMS_TO_TICKS(1000),
                                   pdTRUE,
                                   NULL,
                                   hop_timer_callback);

    ESP_LOGI(TAG, "Channel Manager initialized");
    return ESP_OK;
}

esp_err_t channel_manager_start(void)
{
    if (s_cm.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting Channel Manager...");

    // Create TX task
    BaseType_t ret = xTaskCreatePinnedToCore(tx_task,
                                              "cm_tx_task",
                                              4096,
                                              NULL,
                                              9,
                                              &s_cm.tx_task_handle,
                                              1);  // Core 1
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TX task");
        return ESP_FAIL;
    }

    // Start RX on radio 0
    esp_err_t err = sx1276_start_rx(s_cm.rx_radio, rx_callback, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start RX: %s", esp_err_to_name(err));
        vTaskDelete(s_cm.tx_task_handle);
        return err;
    }

    // Put TX radio in standby
    sx1276_set_mode(s_cm.tx_radio, SX1276_MODE_STANDBY);

    // Start hopping timer if enabled
    if (s_cm.hopping_enabled && s_cm.hop_timer) {
        xTimerStart(s_cm.hop_timer, 0);
    }

    s_cm.running = true;
    ESP_LOGI(TAG, "Channel Manager started (RX continuous, TX standby)");

    return ESP_OK;
}

esp_err_t channel_manager_stop(void)
{
    if (!s_cm.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping Channel Manager...");

    s_cm.running = false;

    // Stop hopping timer
    if (s_cm.hop_timer) {
        xTimerStop(s_cm.hop_timer, 0);
    }

    // Stop RX
    sx1276_stop_rx(s_cm.rx_radio);

    // Stop TX task
    if (s_cm.tx_task_handle) {
        vTaskDelete(s_cm.tx_task_handle);
        s_cm.tx_task_handle = NULL;
    }

    // Put radios in sleep
    sx1276_set_mode(s_cm.rx_radio, SX1276_MODE_SLEEP);
    sx1276_set_mode(s_cm.tx_radio, SX1276_MODE_SLEEP);

    ESP_LOGI(TAG, "Channel Manager stopped");
    return ESP_OK;
}

esp_err_t channel_manager_schedule_tx(const lora_tx_packet_t *packet)
{
    if (!s_cm.running || !packet) {
        return ESP_ERR_INVALID_STATE;
    }

    // Add to TX queue
    if (xQueueSend(s_cm.tx_queue, packet, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full, packet dropped");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "TX packet queued (freq: %lu, size: %d)",
             packet->modulation.frequency, packet->payload_size);

    return ESP_OK;
}

esp_err_t channel_manager_set_hopping(bool enabled, uint32_t interval_ms)
{
    s_cm.hopping_enabled = enabled;
    s_cm.hop_interval_ms = interval_ms;

    if (s_cm.hop_timer) {
        if (enabled) {
            xTimerChangePeriod(s_cm.hop_timer, pdMS_TO_TICKS(interval_ms), 0);
            if (s_cm.running) {
                xTimerStart(s_cm.hop_timer, 0);
            }
        } else {
            xTimerStop(s_cm.hop_timer, 0);
        }
    }

    ESP_LOGI(TAG, "Channel hopping %s (interval: %lu ms)",
             enabled ? "enabled" : "disabled", interval_ms);

    return ESP_OK;
}

// Internal: TX task
static void tx_task(void *arg)
{
    lora_tx_packet_t packet;
    sx1276_tx_packet_t sx_packet;

    ESP_LOGI(TAG, "TX task started");

    while (s_cm.running) {
        // Wait for packet in queue
        if (xQueueReceive(s_cm.tx_queue, &packet, pdMS_TO_TICKS(100)) == pdTRUE) {
            xSemaphoreTake(s_cm.tx_mutex, portMAX_DELAY);
            s_cm.tx_busy = true;

            // Check timing
            if (!packet.immediate) {
                uint32_t current = lora_gateway_get_timestamp();
                int32_t delay = (int32_t)(packet.tx_timestamp - current);

                if (delay > 0 && delay < 5000000) {  // Max 5 second delay
                    ESP_LOGD(TAG, "TX scheduled in %ld us", delay);
                    // Wait until timestamp
                    while (lora_gateway_get_timestamp() < packet.tx_timestamp) {
                        vTaskDelay(1);
                    }
                } else if (delay < -100000) {
                    // Too late, skip packet
                    ESP_LOGW(TAG, "TX too late by %ld us, skipping", -delay);
                    s_cm.tx_busy = false;
                    xSemaphoreGive(s_cm.tx_mutex);
                    continue;
                }
            }

            // Prepare SX1276 packet
            memcpy(sx_packet.data, packet.payload, packet.payload_size);
            sx_packet.length = packet.payload_size;
            sx_packet.frequency = packet.modulation.frequency;
            sx_packet.power = packet.tx_power;
            sx_packet.sf = packet.modulation.spreading_factor;
            sx_packet.bw = packet.modulation.bandwidth;
            sx_packet.cr = packet.modulation.coding_rate;
            sx_packet.invert_iq = packet.modulation.invert_polarity;
            sx_packet.tx_delay_us = 0;

            ESP_LOGI(TAG, "TX: freq=%lu, SF%d, %d bytes",
                     sx_packet.frequency, sx_packet.sf, sx_packet.length);

            // Transmit
            esp_err_t err = sx1276_transmit(s_cm.tx_radio, &sx_packet,
                                            tx_done_callback, NULL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "TX failed: %s", esp_err_to_name(err));
            }

            // Wait for TX complete (with timeout)
            int timeout = 0;
            while (s_cm.tx_busy && timeout < 5000) {
                vTaskDelay(pdMS_TO_TICKS(1));
                timeout++;
            }

            if (timeout >= 5000) {
                ESP_LOGW(TAG, "TX timeout");
            }

            xSemaphoreGive(s_cm.tx_mutex);
        }
    }

    ESP_LOGI(TAG, "TX task stopped");
    vTaskDelete(NULL);
}

// Internal: RX callback (called from ISR context)
static void rx_callback(sx1276_rx_packet_t *packet, void *user_data)
{
    if (!packet) {
        return;
    }

    // Convert to gateway packet format
    lora_rx_packet_t gw_packet = {0};
    memcpy(gw_packet.payload, packet->data, packet->length);
    gw_packet.payload_size = packet->length;
    gw_packet.modulation.frequency = packet->frequency;
    gw_packet.modulation.spreading_factor = packet->sf;
    gw_packet.modulation.bandwidth = packet->bw;
    gw_packet.modulation.coding_rate = packet->cr;
    gw_packet.rssi = packet->rssi;
    gw_packet.snr = packet->snr;
    gw_packet.crc_ok = packet->crc_ok;
    gw_packet.timestamp = packet->timestamp;
    gw_packet.tmst = lora_gateway_get_timestamp();
    gw_packet.rf_chain = 0;

    // Forward to gateway
    extern void lora_gateway_rx_handler(const lora_rx_packet_t *packet);
    lora_gateway_rx_handler(&gw_packet);
}

// Internal: TX done callback
static void tx_done_callback(bool success, void *user_data)
{
    s_cm.tx_busy = false;

    if (success) {
        ESP_LOGD(TAG, "TX complete");
    } else {
        ESP_LOGW(TAG, "TX failed");
    }
}

// Internal: Channel hopping timer
static void hop_timer_callback(TimerHandle_t timer)
{
    if (!s_cm.running || !s_cm.hopping_enabled) {
        return;
    }

    const gateway_config_t *config = gw_config_get();
    uint8_t num_channels = GATEWAY_MAX_CHANNELS;

    // Move to next channel
    s_cm.current_channel = (s_cm.current_channel + 1) % num_channels;

    // Get frequency for this channel
    uint32_t freq = gw_config_get_uplink_freq(s_cm.current_channel);

    // Update RX radio frequency
    sx1276_set_frequency(s_cm.rx_radio, freq);

    ESP_LOGD(TAG, "Hopped to channel %d (%.2f MHz)",
             s_cm.current_channel, freq / 1e6);
}
