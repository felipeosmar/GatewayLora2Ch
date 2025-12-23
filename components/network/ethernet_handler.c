/**
 * @file ethernet_handler.c
 * @brief Ethernet Handler for W5500 Module
 */

#include <string.h>
#include "network_manager.h"
#include "gateway_config.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth_driver.h"

static const char *TAG = "eth_handler";

static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
static net_status_t s_eth_status = NET_STATUS_DISCONNECTED;
static bool s_initialized = false;

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == ETH_EVENT) {
        switch (event_id) {
            case ETHERNET_EVENT_CONNECTED:
                ESP_LOGI(TAG, "Ethernet link up");
                s_eth_status = NET_STATUS_CONNECTING;
                break;

            case ETHERNET_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "Ethernet link down");
                s_eth_status = NET_STATUS_DISCONNECTED;
                break;

            case ETHERNET_EVENT_START:
                ESP_LOGI(TAG, "Ethernet started");
                break;

            case ETHERNET_EVENT_STOP:
                ESP_LOGI(TAG, "Ethernet stopped");
                s_eth_status = NET_STATUS_DISCONNECTED;
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_ETH_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_eth_status = NET_STATUS_CONNECTED;
        } else if (event_id == IP_EVENT_ETH_LOST_IP) {
            ESP_LOGW(TAG, "Ethernet lost IP");
            s_eth_status = NET_STATUS_CONNECTING;
        }
    }
}

esp_err_t ethernet_handler_init(void)
{
#ifndef CONFIG_W5500_ENABLED
    ESP_LOGI(TAG, "Ethernet (W5500) disabled in config");
    return ESP_OK;
#else
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Ethernet (W5500)...");

    // Configure SPI bus for W5500 (shared with LoRa modules)
    // Note: SPI bus should already be initialized in main

    // W5500 specific configuration
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,  // 20 MHz
        .spics_io_num = CONFIG_W5500_CS_GPIO,
        .queue_size = 20,
    };

    // Configure W5500 ethernet MAC and PHY
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = CONFIG_W5500_INT_GPIO;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = CONFIG_W5500_RST_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC");
        return ESP_FAIL;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "Failed to create W5500 PHY");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install ethernet driver: %s", esp_err_to_name(ret));
        return ret;
    }

    // Generate MAC address from Gateway EUI
    const gateway_config_t *gw_config = gw_config_get();
    uint8_t mac_addr[6];
    mac_addr[0] = gw_config->gateway_eui[0] | 0x02;  // Set locally administered bit
    mac_addr[1] = gw_config->gateway_eui[1];
    mac_addr[2] = gw_config->gateway_eui[2];
    mac_addr[3] = gw_config->gateway_eui[5];
    mac_addr[4] = gw_config->gateway_eui[6];
    mac_addr[5] = gw_config->gateway_eui[7];

    ret = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set MAC address: %s", esp_err_to_name(ret));
    }

    // Create netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "Failed to create ethernet netif");
        return ESP_FAIL;
    }

    // Attach ethernet driver to netif
    ret = esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach ethernet to netif: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers
    ret = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                              &eth_event_handler, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH event handler");
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                              &eth_event_handler, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Ethernet initialized");

    return ESP_OK;
#endif
}

esp_err_t ethernet_handler_start(void)
{
#ifndef CONFIG_W5500_ENABLED
    return ESP_OK;
#else
    if (!s_initialized) {
        esp_err_t ret = ethernet_handler_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    const gateway_config_t *config = gw_config_get();
    if (!config->ethernet.enabled) {
        ESP_LOGI(TAG, "Ethernet disabled in config");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting Ethernet...");

    // Configure DHCP or static IP
    if (!config->ethernet.dhcp) {
        esp_netif_dhcpc_stop(s_eth_netif);

        esp_netif_ip_info_t ip_info = {
            .ip.addr = config->ethernet.ip,
            .netmask.addr = config->ethernet.netmask,
            .gw.addr = config->ethernet.gateway,
        };
        esp_netif_set_ip_info(s_eth_netif, &ip_info);

        if (config->ethernet.dns != 0) {
            esp_netif_dns_info_t dns_info = {
                .ip.u_addr.ip4.addr = config->ethernet.dns,
                .ip.type = ESP_IPADDR_TYPE_V4,
            };
            esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        }
    }

    s_eth_status = NET_STATUS_CONNECTING;

    esp_err_t ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ethernet: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Ethernet started");
    return ESP_OK;
#endif
}

esp_err_t ethernet_handler_stop(void)
{
#ifndef CONFIG_W5500_ENABLED
    return ESP_OK;
#else
    if (!s_initialized || !s_eth_handle) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping Ethernet...");

    esp_err_t ret = esp_eth_stop(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet stop warning: %s", esp_err_to_name(ret));
    }

    s_eth_status = NET_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "Ethernet stopped");

    return ESP_OK;
#endif
}

net_status_t ethernet_handler_get_status(void)
{
    return s_eth_status;
}

esp_netif_t *ethernet_handler_get_netif(void)
{
    return s_eth_netif;
}
