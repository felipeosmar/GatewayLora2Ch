/**
 * @file lora_gateway.c
 * @brief LoRa Gateway Core Implementation
 */

#include <string.h>
#include "lora_gateway.h"
#include "gateway_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"

static const char *TAG = "lora_gw";

// Gateway state
typedef struct {
    // Radio handles
    sx1276_handle_t rx_radio;
    sx1276_handle_t tx_radio;

    // Configuration
    gateway_config_t config;

    // RX packet queue
    QueueHandle_t rx_queue;

    // Statistics
    gateway_stats_t stats;

    // State
    bool initialized;
    bool running;
    uint32_t start_time;

    // Tasks
    TaskHandle_t rx_process_task;

} gateway_state_t;

static gateway_state_t s_gw = {0};

// Forward declarations
static void rx_process_task(void *arg);
static esp_err_t init_spi_bus(spi_host_device_t host);

// Called from channel_manager when packet received
void lora_gateway_rx_handler(const lora_rx_packet_t *packet)
{
    if (!s_gw.running || !packet) {
        return;
    }

    // Update stats
    s_gw.stats.rx_total++;
    if (packet->crc_ok) {
        s_gw.stats.rx_ok++;
    } else {
        s_gw.stats.rx_bad++;
    }
    s_gw.stats.last_rx_time = esp_timer_get_time();

    // Queue for processing
    if (s_gw.rx_queue) {
        if (xQueueSendFromISR(s_gw.rx_queue, packet, NULL) != pdTRUE) {
            ESP_LOGW(TAG, "RX queue full");
        }
    }
}

esp_err_t lora_gateway_init(const gateway_config_t *config)
{
    if (s_gw.initialized) {
        ESP_LOGW(TAG, "Gateway already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing LoRa Gateway...");

    memset(&s_gw, 0, sizeof(gateway_state_t));
    memcpy(&s_gw.config, config, sizeof(gateway_config_t));

    // Initialize SPI bus
    esp_err_t ret = init_spi_bus(config->spi_host);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return ret;
    }

    // Initialize RX radio (Radio 0)
    ESP_LOGI(TAG, "Initializing RX radio...");
    ret = sx1276_init(config->spi_host,
                      &config->radio[0].pins,
                      &config->radio[0].config,
                      &s_gw.rx_radio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RX radio init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize TX radio (Radio 1)
    ESP_LOGI(TAG, "Initializing TX radio...");
    ret = sx1276_init(config->spi_host,
                      &config->radio[1].pins,
                      &config->radio[1].config,
                      &s_gw.tx_radio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX radio init failed: %s", esp_err_to_name(ret));
        sx1276_deinit(s_gw.rx_radio);
        return ret;
    }

    // Initialize channel manager
    ret = channel_manager_init(s_gw.rx_radio, s_gw.tx_radio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Channel manager init failed");
        sx1276_deinit(s_gw.rx_radio);
        sx1276_deinit(s_gw.tx_radio);
        return ret;
    }

    // Create RX queue
    s_gw.rx_queue = xQueueCreate(GATEWAY_RX_QUEUE_SIZE, sizeof(lora_rx_packet_t));
    if (!s_gw.rx_queue) {
        ESP_LOGE(TAG, "Failed to create RX queue");
        return ESP_ERR_NO_MEM;
    }

    s_gw.initialized = true;
    ESP_LOGI(TAG, "LoRa Gateway initialized");

    // Log radio versions
    ESP_LOGI(TAG, "RX Radio version: 0x%02X", sx1276_get_version(s_gw.rx_radio));
    ESP_LOGI(TAG, "TX Radio version: 0x%02X", sx1276_get_version(s_gw.tx_radio));

    return ESP_OK;
}

esp_err_t lora_gateway_deinit(void)
{
    if (!s_gw.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing gateway...");

    lora_gateway_stop();

    if (s_gw.rx_queue) {
        vQueueDelete(s_gw.rx_queue);
    }

    channel_manager_stop();
    sx1276_deinit(s_gw.rx_radio);
    sx1276_deinit(s_gw.tx_radio);

    s_gw.initialized = false;
    ESP_LOGI(TAG, "Gateway deinitialized");

    return ESP_OK;
}

esp_err_t lora_gateway_start(void)
{
    if (!s_gw.initialized) {
        ESP_LOGE(TAG, "Gateway not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_gw.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting LoRa Gateway...");

    // Create RX processing task
    BaseType_t ret = xTaskCreatePinnedToCore(rx_process_task,
                                              "gw_rx_task",
                                              4096,
                                              NULL,
                                              10,
                                              &s_gw.rx_process_task,
                                              1);  // Core 1
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX task");
        return ESP_FAIL;
    }

    // Start channel manager
    esp_err_t err = channel_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start channel manager");
        vTaskDelete(s_gw.rx_process_task);
        return err;
    }

    s_gw.running = true;
    s_gw.start_time = esp_timer_get_time() / 1000000;

    ESP_LOGI(TAG, "LoRa Gateway started");
    return ESP_OK;
}

esp_err_t lora_gateway_stop(void)
{
    if (!s_gw.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping LoRa Gateway...");

    s_gw.running = false;

    channel_manager_stop();

    if (s_gw.rx_process_task) {
        vTaskDelete(s_gw.rx_process_task);
        s_gw.rx_process_task = NULL;
    }

    ESP_LOGI(TAG, "LoRa Gateway stopped");
    return ESP_OK;
}

esp_err_t lora_gateway_send(const lora_tx_packet_t *packet)
{
    if (!s_gw.running || !packet) {
        return ESP_ERR_INVALID_STATE;
    }

    s_gw.stats.tx_total++;

    esp_err_t ret = channel_manager_schedule_tx(packet);
    if (ret == ESP_OK) {
        s_gw.stats.last_tx_time = esp_timer_get_time();
    } else {
        s_gw.stats.tx_fail++;
    }

    return ret;
}

esp_err_t lora_gateway_get_stats(gateway_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(stats, &s_gw.stats, sizeof(gateway_stats_t));
    stats->uptime = (esp_timer_get_time() / 1000000) - s_gw.start_time;

    return ESP_OK;
}

void lora_gateway_reset_stats(void)
{
    uint32_t start_time = s_gw.start_time;
    memset(&s_gw.stats, 0, sizeof(gateway_stats_t));
    s_gw.start_time = start_time;
}

esp_err_t lora_gateway_set_rx_frequency(uint32_t frequency)
{
    if (!s_gw.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return sx1276_set_frequency(s_gw.rx_radio, frequency);
}

esp_err_t lora_gateway_set_rx_params(uint8_t sf, uint8_t bw)
{
    if (!s_gw.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = sx1276_set_spreading_factor(s_gw.rx_radio, sf);
    if (ret != ESP_OK) {
        return ret;
    }

    return sx1276_set_bandwidth(s_gw.rx_radio, bw);
}

uint32_t lora_gateway_get_timestamp(void)
{
    return (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
}

bool lora_gateway_is_running(void)
{
    return s_gw.running;
}

// Internal: RX processing task
static void rx_process_task(void *arg)
{
    lora_rx_packet_t packet;

    ESP_LOGI(TAG, "RX processing task started");

    while (s_gw.running) {
        if (xQueueReceive(s_gw.rx_queue, &packet, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Log received packet
            ESP_LOGI(TAG, "RX: %d bytes, RSSI=%d, SNR=%.1f, CRC=%s",
                     packet.payload_size,
                     packet.rssi,
                     packet.snr,
                     packet.crc_ok ? "OK" : "ERR");

            // Call user callback if set
            if (s_gw.config.rx_callback && packet.crc_ok) {
                s_gw.config.rx_callback(&packet, s_gw.config.rx_user_data);
            }
        }
    }

    ESP_LOGI(TAG, "RX processing task stopped");
    vTaskDelete(NULL);
}

// Internal: Initialize SPI bus
static esp_err_t init_spi_bus(spi_host_device_t host)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_SPI_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };

    esp_err_t ret = spi_bus_initialize(host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE means bus already initialized (OK)
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPI bus initialized (MOSI=%d, MISO=%d, SCK=%d)",
             CONFIG_SPI_MOSI_GPIO, CONFIG_SPI_MISO_GPIO, CONFIG_SPI_SCK_GPIO);

    return ESP_OK;
}
