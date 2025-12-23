/**
 * @file sx1276.c
 * @brief SX1276 LoRa Module Driver Implementation
 */

#include <string.h>
#include "sx1276.h"
#include "sx1276_regs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "sx1276";

/**
 * @brief Internal device structure
 */
struct sx1276_dev_s {
    spi_device_handle_t spi;
    sx1276_pins_t pins;
    sx1276_config_t config;
    SemaphoreHandle_t mutex;

    // Callbacks
    sx1276_rx_callback_t rx_callback;
    void *rx_user_data;
    sx1276_tx_callback_t tx_callback;
    void *tx_user_data;

    // State
    sx1276_mode_t current_mode;
    bool is_transmitting;
};

// Forward declarations
static void IRAM_ATTR dio0_isr_handler(void *arg);
static esp_err_t sx1276_write_reg(sx1276_handle_t handle, uint8_t reg, uint8_t value);
static uint8_t sx1276_read_reg(sx1276_handle_t handle, uint8_t reg);
static esp_err_t sx1276_write_fifo(sx1276_handle_t handle, const uint8_t *data, uint8_t len);
static esp_err_t sx1276_read_fifo(sx1276_handle_t handle, uint8_t *data, uint8_t len);
static void sx1276_reset(sx1276_handle_t handle);

esp_err_t sx1276_init(spi_host_device_t spi_host, const sx1276_pins_t *pins,
                      const sx1276_config_t *config, sx1276_handle_t *handle)
{
    if (!pins || !config || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct sx1276_dev_s *dev = calloc(1, sizeof(struct sx1276_dev_s));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }

    dev->pins = *pins;
    dev->config = *config;
    dev->mutex = xSemaphoreCreateMutex();
    if (!dev->mutex) {
        free(dev);
        return ESP_ERR_NO_MEM;
    }

    // Configure CS pin
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pins->cs),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(pins->cs, 1);

    // Configure Reset pin
    io_conf.pin_bit_mask = (1ULL << pins->reset);
    gpio_config(&io_conf);

    // Configure DIO0 as input with interrupt
    io_conf.pin_bit_mask = (1ULL << pins->dio0);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&io_conf);

    // Configure DIO1 as input
    if (pins->dio1 != GPIO_NUM_NC) {
        io_conf.pin_bit_mask = (1ULL << pins->dio1);
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
    }

    // Configure DIO2 as input
    if (pins->dio2 != GPIO_NUM_NC) {
        io_conf.pin_bit_mask = (1ULL << pins->dio2);
        gpio_config(&io_conf);
    }

    // Configure SPI device
    spi_device_interface_config_t spi_cfg = {
        .clock_speed_hz = 8 * 1000 * 1000,  // 8 MHz
        .mode = 0,
        .spics_io_num = -1,  // Manual CS control
        .queue_size = 1,
        .flags = 0,
    };

    esp_err_t ret = spi_bus_add_device(spi_host, &spi_cfg, &dev->spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        vSemaphoreDelete(dev->mutex);
        free(dev);
        return ret;
    }

    // Hardware reset
    sx1276_reset(dev);

    // Check chip version
    uint8_t version = sx1276_read_reg(dev, REG_VERSION);
    if (version != SX1276_VERSION) {
        ESP_LOGE(TAG, "Invalid chip version: 0x%02X (expected 0x%02X)", version, SX1276_VERSION);
        spi_bus_remove_device(dev->spi);
        vSemaphoreDelete(dev->mutex);
        free(dev);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SX1276 detected, version: 0x%02X", version);

    // Set sleep mode to configure
    sx1276_write_reg(dev, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Set LoRa mode
    sx1276_write_reg(dev, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Apply configuration
    ret = sx1276_apply_config(dev, config);
    if (ret != ESP_OK) {
        spi_bus_remove_device(dev->spi);
        vSemaphoreDelete(dev->mutex);
        free(dev);
        return ret;
    }

    // Install ISR handler
    gpio_install_isr_service(0);
    gpio_isr_handler_add(pins->dio0, dio0_isr_handler, dev);

    dev->current_mode = SX1276_MODE_STANDBY;
    *handle = dev;

    ESP_LOGI(TAG, "SX1276 initialized, freq: %lu Hz, SF%d, BW%d",
             config->frequency, config->sf, config->bw);

    return ESP_OK;
}

esp_err_t sx1276_deinit(sx1276_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_isr_handler_remove(handle->pins.dio0);
    sx1276_set_mode(handle, SX1276_MODE_SLEEP);
    spi_bus_remove_device(handle->spi);
    vSemaphoreDelete(handle->mutex);
    free(handle);

    return ESP_OK;
}

static void sx1276_reset(sx1276_handle_t handle)
{
    gpio_set_level(handle->pins.reset, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(handle->pins.reset, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static esp_err_t sx1276_write_reg(sx1276_handle_t handle, uint8_t reg, uint8_t value)
{
    uint8_t tx_data[2] = {reg | 0x80, value};
    uint8_t rx_data[2];

    spi_transaction_t trans = {
        .length = 16,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    gpio_set_level(handle->pins.cs, 0);
    esp_err_t ret = spi_device_transmit(handle->spi, &trans);
    gpio_set_level(handle->pins.cs, 1);

    return ret;
}

static uint8_t sx1276_read_reg(sx1276_handle_t handle, uint8_t reg)
{
    uint8_t tx_data[2] = {reg & 0x7F, 0x00};
    uint8_t rx_data[2];

    spi_transaction_t trans = {
        .length = 16,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    gpio_set_level(handle->pins.cs, 0);
    spi_device_transmit(handle->spi, &trans);
    gpio_set_level(handle->pins.cs, 1);

    return rx_data[1];
}

static esp_err_t sx1276_write_fifo(sx1276_handle_t handle, const uint8_t *data, uint8_t len)
{
    uint8_t tx_buf[257];
    tx_buf[0] = REG_FIFO | 0x80;
    memcpy(&tx_buf[1], data, len);

    spi_transaction_t trans = {
        .length = (len + 1) * 8,
        .tx_buffer = tx_buf,
    };

    gpio_set_level(handle->pins.cs, 0);
    esp_err_t ret = spi_device_transmit(handle->spi, &trans);
    gpio_set_level(handle->pins.cs, 1);

    return ret;
}

static esp_err_t sx1276_read_fifo(sx1276_handle_t handle, uint8_t *data, uint8_t len)
{
    uint8_t tx_buf[257] = {REG_FIFO & 0x7F};
    uint8_t rx_buf[257];

    spi_transaction_t trans = {
        .length = (len + 1) * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };

    gpio_set_level(handle->pins.cs, 0);
    esp_err_t ret = spi_device_transmit(handle->spi, &trans);
    gpio_set_level(handle->pins.cs, 1);

    memcpy(data, &rx_buf[1], len);
    return ret;
}

esp_err_t sx1276_set_mode(sx1276_handle_t handle, sx1276_mode_t mode)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t op_mode = MODE_LONG_RANGE_MODE;

    switch (mode) {
        case SX1276_MODE_SLEEP:
            op_mode |= MODE_SLEEP;
            break;
        case SX1276_MODE_STANDBY:
            op_mode |= MODE_STDBY;
            break;
        case SX1276_MODE_FSTX:
            op_mode |= MODE_FSTX;
            break;
        case SX1276_MODE_TX:
            op_mode |= MODE_TX;
            break;
        case SX1276_MODE_FSRX:
            op_mode |= MODE_FSRX;
            break;
        case SX1276_MODE_RX_CONTINUOUS:
            op_mode |= MODE_RX_CONTINUOUS;
            break;
        case SX1276_MODE_RX_SINGLE:
            op_mode |= MODE_RX_SINGLE;
            break;
        case SX1276_MODE_CAD:
            op_mode |= MODE_CAD;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    sx1276_write_reg(handle, REG_OP_MODE, op_mode);
    handle->current_mode = mode;
    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

esp_err_t sx1276_get_mode(sx1276_handle_t handle, sx1276_mode_t *mode)
{
    if (!handle || !mode) {
        return ESP_ERR_INVALID_ARG;
    }

    *mode = handle->current_mode;
    return ESP_OK;
}

esp_err_t sx1276_set_frequency(sx1276_handle_t handle, uint32_t frequency)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t frf = ((uint64_t)frequency << 19) / 32000000;

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    sx1276_write_reg(handle, REG_FRF_MSB, (uint8_t)(frf >> 16));
    sx1276_write_reg(handle, REG_FRF_MID, (uint8_t)(frf >> 8));
    sx1276_write_reg(handle, REG_FRF_LSB, (uint8_t)(frf >> 0));
    handle->config.frequency = frequency;
    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

esp_err_t sx1276_set_spreading_factor(sx1276_handle_t handle, sx1276_spreading_factor_t sf)
{
    if (!handle || sf < SX1276_SF_6 || sf > SX1276_SF_12) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    uint8_t config2 = sx1276_read_reg(handle, REG_MODEM_CONFIG_2);
    config2 = (config2 & 0x0F) | (sf << 4);
    sx1276_write_reg(handle, REG_MODEM_CONFIG_2, config2);

    // SF6 requires special settings
    if (sf == SX1276_SF_6) {
        sx1276_write_reg(handle, REG_DETECT_OPTIMIZE, DETECT_OPTIMIZE_SF6);
        sx1276_write_reg(handle, REG_DETECTION_THRESHOLD, DETECTION_THRESHOLD_SF6);
    } else {
        sx1276_write_reg(handle, REG_DETECT_OPTIMIZE, DETECT_OPTIMIZE_SF7_12);
        sx1276_write_reg(handle, REG_DETECTION_THRESHOLD, DETECTION_THRESHOLD_SF7_12);
    }

    // Enable LDR optimization for SF11/SF12 and low bandwidth
    uint8_t config3 = sx1276_read_reg(handle, REG_MODEM_CONFIG_3);
    if (sf >= SX1276_SF_11 && handle->config.bw <= SX1276_BW_125_KHZ) {
        config3 |= 0x08;  // LowDataRateOptimize
    } else {
        config3 &= ~0x08;
    }
    sx1276_write_reg(handle, REG_MODEM_CONFIG_3, config3);

    handle->config.sf = sf;
    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

esp_err_t sx1276_set_bandwidth(sx1276_handle_t handle, sx1276_bandwidth_t bw)
{
    if (!handle || bw > SX1276_BW_500_KHZ) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    uint8_t config1 = sx1276_read_reg(handle, REG_MODEM_CONFIG_1);
    config1 = (config1 & 0x0F) | (bw << 4);
    sx1276_write_reg(handle, REG_MODEM_CONFIG_1, config1);

    handle->config.bw = bw;
    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

esp_err_t sx1276_set_coding_rate(sx1276_handle_t handle, sx1276_coding_rate_t cr)
{
    if (!handle || cr < SX1276_CR_4_5 || cr > SX1276_CR_4_8) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    uint8_t config1 = sx1276_read_reg(handle, REG_MODEM_CONFIG_1);
    config1 = (config1 & 0xF1) | (cr << 1);
    sx1276_write_reg(handle, REG_MODEM_CONFIG_1, config1);

    handle->config.cr = cr;
    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

esp_err_t sx1276_set_tx_power(sx1276_handle_t handle, int8_t power)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    if (power > 17) {
        // Use PA_BOOST with +20dBm capability
        sx1276_write_reg(handle, REG_PA_DAC, 0x87);  // Enable +20dBm
        power = (power > 20) ? 20 : power;
        sx1276_write_reg(handle, REG_PA_CONFIG, PA_BOOST | (power - 5));
    } else if (power > 14) {
        sx1276_write_reg(handle, REG_PA_DAC, 0x84);  // Default
        sx1276_write_reg(handle, REG_PA_CONFIG, PA_BOOST | (power - 2));
    } else {
        sx1276_write_reg(handle, REG_PA_DAC, 0x84);
        power = (power < 2) ? 2 : power;
        sx1276_write_reg(handle, REG_PA_CONFIG, PA_BOOST | (power - 2));
    }

    // Set OCP
    sx1276_write_reg(handle, REG_OCP, 0x2B);  // 100mA

    handle->config.tx_power = power;
    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

esp_err_t sx1276_set_sync_word(sx1276_handle_t handle, uint8_t sync_word)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    sx1276_write_reg(handle, REG_SYNC_WORD, sync_word);
    handle->config.sync_word = sync_word;
    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

esp_err_t sx1276_set_invert_iq(sx1276_handle_t handle, bool invert_rx, bool invert_tx)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    uint8_t val = sx1276_read_reg(handle, REG_INVERT_IQ);
    if (invert_rx) {
        val |= 0x40;
    } else {
        val &= ~0x40;
    }
    if (invert_tx) {
        val |= 0x01;
    } else {
        val &= ~0x01;
    }
    sx1276_write_reg(handle, REG_INVERT_IQ, val);

    // Also set INVERT_IQ_2 register
    sx1276_write_reg(handle, REG_INVERT_IQ_2, (invert_rx || invert_tx) ? 0x19 : 0x1D);

    handle->config.invert_iq_rx = invert_rx;
    handle->config.invert_iq_tx = invert_tx;
    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

esp_err_t sx1276_apply_config(sx1276_handle_t handle, const sx1276_config_t *config)
{
    if (!handle || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Put in standby mode for configuration
    sx1276_set_mode(handle, SX1276_MODE_STANDBY);

    // Set frequency
    sx1276_set_frequency(handle, config->frequency);

    // Set modulation parameters
    sx1276_set_bandwidth(handle, config->bw);
    sx1276_set_spreading_factor(handle, config->sf);
    sx1276_set_coding_rate(handle, config->cr);

    // Set TX power
    sx1276_set_tx_power(handle, config->tx_power);

    // Set sync word
    sx1276_set_sync_word(handle, config->sync_word);

    // Set preamble length
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    sx1276_write_reg(handle, REG_PREAMBLE_MSB, (config->preamble_length >> 8) & 0xFF);
    sx1276_write_reg(handle, REG_PREAMBLE_LSB, config->preamble_length & 0xFF);

    // Set CRC and implicit header
    uint8_t config1 = sx1276_read_reg(handle, REG_MODEM_CONFIG_1);
    if (config->implicit_header) {
        config1 |= MODEM_CONFIG1_IMPLICIT_HEADER;
    } else {
        config1 &= ~MODEM_CONFIG1_IMPLICIT_HEADER;
    }
    sx1276_write_reg(handle, REG_MODEM_CONFIG_1, config1);

    uint8_t config2 = sx1276_read_reg(handle, REG_MODEM_CONFIG_2);
    if (config->crc_on) {
        config2 |= MODEM_CONFIG2_RX_CRC;
    } else {
        config2 &= ~MODEM_CONFIG2_RX_CRC;
    }
    sx1276_write_reg(handle, REG_MODEM_CONFIG_2, config2);

    // Set FIFO base addresses
    sx1276_write_reg(handle, REG_FIFO_TX_BASE_ADDR, 0x00);
    sx1276_write_reg(handle, REG_FIFO_RX_BASE_ADDR, 0x00);

    // Enable AGC auto
    uint8_t config3 = sx1276_read_reg(handle, REG_MODEM_CONFIG_3);
    config3 |= 0x04;  // AGC auto on
    sx1276_write_reg(handle, REG_MODEM_CONFIG_3, config3);

    // Set LNA gain
    sx1276_write_reg(handle, REG_LNA, 0x23);  // LNA gain max, LNA boost on

    xSemaphoreGive(handle->mutex);

    // Set IQ inversion
    sx1276_set_invert_iq(handle, config->invert_iq_rx, config->invert_iq_tx);

    handle->config = *config;

    return ESP_OK;
}

esp_err_t sx1276_start_rx(sx1276_handle_t handle, sx1276_rx_callback_t callback, void *user_data)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    handle->rx_callback = callback;
    handle->rx_user_data = user_data;

    // Clear IRQ flags
    sx1276_write_reg(handle, REG_IRQ_FLAGS, 0xFF);

    // Set DIO0 to RxDone
    sx1276_write_reg(handle, REG_DIO_MAPPING_1, DIO0_RX_DONE);

    // Set FIFO address
    sx1276_write_reg(handle, REG_FIFO_ADDR_PTR, 0x00);

    xSemaphoreGive(handle->mutex);

    // Start continuous RX
    return sx1276_set_mode(handle, SX1276_MODE_RX_CONTINUOUS);
}

esp_err_t sx1276_stop_rx(sx1276_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    handle->rx_callback = NULL;
    return sx1276_set_mode(handle, SX1276_MODE_STANDBY);
}

esp_err_t sx1276_transmit(sx1276_handle_t handle, const sx1276_tx_packet_t *packet,
                          sx1276_tx_callback_t callback, void *user_data)
{
    if (!handle || !packet || packet->length > SX1276_MAX_PACKET_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    // Go to standby mode
    sx1276_write_reg(handle, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);

    // Apply TX-specific settings if provided
    if (packet->frequency > 0) {
        uint64_t frf = ((uint64_t)packet->frequency << 19) / 32000000;
        sx1276_write_reg(handle, REG_FRF_MSB, (uint8_t)(frf >> 16));
        sx1276_write_reg(handle, REG_FRF_MID, (uint8_t)(frf >> 8));
        sx1276_write_reg(handle, REG_FRF_LSB, (uint8_t)(frf >> 0));
    }

    // Set IQ inversion for TX
    if (packet->invert_iq) {
        sx1276_write_reg(handle, REG_INVERT_IQ, 0x01 | 0x40);
        sx1276_write_reg(handle, REG_INVERT_IQ_2, 0x19);
    }

    // Clear IRQ flags
    sx1276_write_reg(handle, REG_IRQ_FLAGS, 0xFF);

    // Set DIO0 to TxDone
    sx1276_write_reg(handle, REG_DIO_MAPPING_1, DIO0_TX_DONE);

    // Set FIFO address
    sx1276_write_reg(handle, REG_FIFO_ADDR_PTR, 0x00);
    sx1276_write_reg(handle, REG_FIFO_TX_BASE_ADDR, 0x00);

    // Write payload to FIFO
    sx1276_write_fifo(handle, packet->data, packet->length);

    // Set payload length
    sx1276_write_reg(handle, REG_PAYLOAD_LENGTH, packet->length);

    handle->tx_callback = callback;
    handle->tx_user_data = user_data;
    handle->is_transmitting = true;

    // Delay if specified
    if (packet->tx_delay_us > 0) {
        xSemaphoreGive(handle->mutex);
        esp_rom_delay_us(packet->tx_delay_us);
        xSemaphoreTake(handle->mutex, portMAX_DELAY);
    }

    // Start transmission
    sx1276_write_reg(handle, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);
    handle->current_mode = SX1276_MODE_TX;

    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

static void IRAM_ATTR dio0_isr_handler(void *arg)
{
    sx1276_handle_t handle = (sx1276_handle_t)arg;
    if (!handle) {
        return;
    }

    BaseType_t higher_priority_task_woken = pdFALSE;

    uint8_t irq_flags = sx1276_read_reg(handle, REG_IRQ_FLAGS);

    if (irq_flags & IRQ_RX_DONE) {
        // Handle RX Done
        if (handle->rx_callback) {
            sx1276_rx_packet_t packet = {0};

            // Get payload length
            packet.length = sx1276_read_reg(handle, REG_RX_NB_BYTES);

            // Set FIFO address to current RX address
            uint8_t fifo_addr = sx1276_read_reg(handle, REG_FIFO_RX_CURRENT_ADDR);
            sx1276_write_reg(handle, REG_FIFO_ADDR_PTR, fifo_addr);

            // Read payload
            sx1276_read_fifo(handle, packet.data, packet.length);

            // Get RSSI and SNR
            packet.rssi = sx1276_read_reg(handle, REG_PKT_RSSI_VALUE) - 157;
            packet.snr = (int8_t)sx1276_read_reg(handle, REG_PKT_SNR_VALUE) / 4;

            // Check CRC
            packet.crc_ok = !(irq_flags & IRQ_PAYLOAD_CRC_ERROR);

            // Get timestamp
            packet.timestamp = esp_timer_get_time();

            // Store config info
            packet.frequency = handle->config.frequency;
            packet.sf = handle->config.sf;
            packet.bw = handle->config.bw;
            packet.cr = handle->config.cr;

            handle->rx_callback(&packet, handle->rx_user_data);
        }

        // Clear RX Done flag and restart RX if in continuous mode
        sx1276_write_reg(handle, REG_IRQ_FLAGS, IRQ_RX_DONE | IRQ_PAYLOAD_CRC_ERROR);
    }

    if (irq_flags & IRQ_TX_DONE) {
        // Handle TX Done
        handle->is_transmitting = false;

        if (handle->tx_callback) {
            handle->tx_callback(true, handle->tx_user_data);
        }

        // Clear TX Done flag
        sx1276_write_reg(handle, REG_IRQ_FLAGS, IRQ_TX_DONE);

        // Go back to standby
        sx1276_write_reg(handle, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
        handle->current_mode = SX1276_MODE_STANDBY;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
}

int16_t sx1276_get_packet_rssi(sx1276_handle_t handle)
{
    if (!handle) {
        return 0;
    }
    return sx1276_read_reg(handle, REG_PKT_RSSI_VALUE) - 157;
}

int8_t sx1276_get_packet_snr(sx1276_handle_t handle)
{
    if (!handle) {
        return 0;
    }
    return (int8_t)sx1276_read_reg(handle, REG_PKT_SNR_VALUE) / 4;
}

int16_t sx1276_get_rssi(sx1276_handle_t handle)
{
    if (!handle) {
        return 0;
    }
    return sx1276_read_reg(handle, REG_RSSI_VALUE) - 157;
}

esp_err_t sx1276_channel_free(sx1276_handle_t handle, bool *is_free)
{
    if (!handle || !is_free) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    // Clear IRQ flags
    sx1276_write_reg(handle, REG_IRQ_FLAGS, 0xFF);

    // Start CAD
    sx1276_write_reg(handle, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_CAD);

    // Wait for CAD done
    uint32_t start = esp_timer_get_time();
    while (!(sx1276_read_reg(handle, REG_IRQ_FLAGS) & IRQ_CAD_DONE)) {
        if ((esp_timer_get_time() - start) > 100000) {  // 100ms timeout
            xSemaphoreGive(handle->mutex);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    *is_free = !(sx1276_read_reg(handle, REG_IRQ_FLAGS) & IRQ_CAD_DETECTED);

    // Clear flags and go to standby
    sx1276_write_reg(handle, REG_IRQ_FLAGS, IRQ_CAD_DONE | IRQ_CAD_DETECTED);
    sx1276_write_reg(handle, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);

    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

uint8_t sx1276_get_version(sx1276_handle_t handle)
{
    if (!handle) {
        return 0;
    }
    return sx1276_read_reg(handle, REG_VERSION);
}
