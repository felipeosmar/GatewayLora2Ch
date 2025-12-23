/**
 * @file network_manager.c
 * @brief Network Manager with WiFi/Ethernet Failover
 */

#include <string.h>
#include "network_manager.h"
#include "gateway_config.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "net_manager";

#define NET_MONITOR_INTERVAL_MS     5000
#define NET_FAILOVER_DELAY_MS       3000

static net_manager_config_t s_config;
static net_interface_t s_active_interface = NET_IF_NONE;
static bool s_initialized = false;
static TimerHandle_t s_monitor_timer = NULL;

static void net_monitor_callback(TimerHandle_t timer);
static void perform_failover(void);

esp_err_t net_manager_init(const net_manager_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Network Manager...");

    // Initialize TCP/IP stack
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init netif: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create default event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    if (config) {
        memcpy(&s_config, config, sizeof(net_manager_config_t));
    } else {
        // Default configuration
        s_config.wifi_enabled = true;
        s_config.ethernet_enabled = true;
        s_config.auto_failover = true;
        s_config.preferred_interface = NET_IF_WIFI;
        s_config.event_callback = NULL;
    }

    // Initialize WiFi if enabled
    if (s_config.wifi_enabled) {
        ret = wifi_handler_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
            s_config.wifi_enabled = false;
        }
    }

    // Initialize Ethernet if enabled
    if (s_config.ethernet_enabled) {
        ret = ethernet_handler_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(ret));
            s_config.ethernet_enabled = false;
        }
    }

    // Create monitor timer
    if (s_config.auto_failover) {
        s_monitor_timer = xTimerCreate("net_monitor",
                                       pdMS_TO_TICKS(NET_MONITOR_INTERVAL_MS),
                                       pdTRUE,
                                       NULL,
                                       net_monitor_callback);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Network Manager initialized (WiFi: %s, Ethernet: %s)",
             s_config.wifi_enabled ? "enabled" : "disabled",
             s_config.ethernet_enabled ? "enabled" : "disabled");

    return ESP_OK;
}

esp_err_t net_manager_start(void)
{
    if (!s_initialized) {
        esp_err_t ret = net_manager_init(NULL);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    ESP_LOGI(TAG, "Starting Network Manager...");

    // Start preferred interface first
    if (s_config.preferred_interface == NET_IF_ETHERNET && s_config.ethernet_enabled) {
        ethernet_handler_start();
        s_active_interface = NET_IF_ETHERNET;
    } else if (s_config.wifi_enabled) {
        wifi_handler_start();
        s_active_interface = NET_IF_WIFI;
    } else if (s_config.ethernet_enabled) {
        ethernet_handler_start();
        s_active_interface = NET_IF_ETHERNET;
    }

    // Start secondary interface as backup
    if (s_config.auto_failover) {
        if (s_active_interface == NET_IF_WIFI && s_config.ethernet_enabled) {
            ethernet_handler_start();
        } else if (s_active_interface == NET_IF_ETHERNET && s_config.wifi_enabled) {
            wifi_handler_start();
        }
    }

    // Start monitor timer
    if (s_monitor_timer) {
        xTimerStart(s_monitor_timer, 0);
    }

    ESP_LOGI(TAG, "Network Manager started, active interface: %s",
             s_active_interface == NET_IF_WIFI ? "WiFi" :
             s_active_interface == NET_IF_ETHERNET ? "Ethernet" : "None");

    return ESP_OK;
}

esp_err_t net_manager_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping Network Manager...");

    if (s_monitor_timer) {
        xTimerStop(s_monitor_timer, 0);
    }

    wifi_handler_stop();
    ethernet_handler_stop();

    s_active_interface = NET_IF_NONE;

    ESP_LOGI(TAG, "Network Manager stopped");
    return ESP_OK;
}

net_interface_t net_manager_get_active_interface(void)
{
    return s_active_interface;
}

net_status_t net_manager_get_status(net_interface_t interface)
{
    switch (interface) {
        case NET_IF_WIFI:
            return wifi_handler_get_status();
        case NET_IF_ETHERNET:
            return ethernet_handler_get_status();
        default:
            return NET_STATUS_DISCONNECTED;
    }
}

bool net_manager_is_connected(void)
{
    return (wifi_handler_get_status() == NET_STATUS_CONNECTED) ||
           (ethernet_handler_get_status() == NET_STATUS_CONNECTED);
}

esp_err_t net_manager_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    if (!ip_info) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *netif = net_manager_get_netif();
    if (!netif) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_netif_get_ip_info(netif, ip_info);
}

esp_netif_t *net_manager_get_netif(void)
{
    // Return the netif of the connected interface
    if (wifi_handler_get_status() == NET_STATUS_CONNECTED) {
        return wifi_handler_get_netif();
    }
    if (ethernet_handler_get_status() == NET_STATUS_CONNECTED) {
        return ethernet_handler_get_netif();
    }

    // Return the active interface's netif even if not connected
    switch (s_active_interface) {
        case NET_IF_WIFI:
            return wifi_handler_get_netif();
        case NET_IF_ETHERNET:
            return ethernet_handler_get_netif();
        default:
            return NULL;
    }
}

esp_err_t net_manager_switch_interface(net_interface_t interface)
{
    if (interface == s_active_interface) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Switching to interface: %s",
             interface == NET_IF_WIFI ? "WiFi" : "Ethernet");

    s_active_interface = interface;

    // Notify callback
    if (s_config.event_callback) {
        net_status_t status = net_manager_get_status(interface);
        s_config.event_callback(interface, status, s_config.callback_user_data);
    }

    return ESP_OK;
}

static void net_monitor_callback(TimerHandle_t timer)
{
    if (!s_config.auto_failover) {
        return;
    }

    net_status_t wifi_status = wifi_handler_get_status();
    net_status_t eth_status = ethernet_handler_get_status();

    // Check if active interface is still connected
    net_status_t active_status = (s_active_interface == NET_IF_WIFI) ? wifi_status : eth_status;

    if (active_status == NET_STATUS_CONNECTED) {
        // Active interface is connected, check if we should switch back to preferred
        if (s_config.preferred_interface != s_active_interface) {
            net_status_t preferred_status = (s_config.preferred_interface == NET_IF_WIFI) ?
                                            wifi_status : eth_status;
            if (preferred_status == NET_STATUS_CONNECTED) {
                ESP_LOGI(TAG, "Preferred interface connected, switching back");
                net_manager_switch_interface(s_config.preferred_interface);
            }
        }
        return;
    }

    // Active interface is not connected, try failover
    perform_failover();
}

static void perform_failover(void)
{
    net_status_t wifi_status = wifi_handler_get_status();
    net_status_t eth_status = ethernet_handler_get_status();

    if (s_active_interface == NET_IF_WIFI && eth_status == NET_STATUS_CONNECTED) {
        ESP_LOGI(TAG, "Failover: WiFi -> Ethernet");
        net_manager_switch_interface(NET_IF_ETHERNET);
    } else if (s_active_interface == NET_IF_ETHERNET && wifi_status == NET_STATUS_CONNECTED) {
        ESP_LOGI(TAG, "Failover: Ethernet -> WiFi");
        net_manager_switch_interface(NET_IF_WIFI);
    } else {
        ESP_LOGW(TAG, "No connected interface available for failover");
    }
}
