/**
 * @file network_manager.h
 * @brief Network Manager - WiFi and Ethernet with failover
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Network interface type
 */
typedef enum {
    NET_IF_NONE = 0,
    NET_IF_WIFI,
    NET_IF_ETHERNET
} net_interface_t;

/**
 * @brief Network status
 */
typedef enum {
    NET_STATUS_DISCONNECTED = 0,
    NET_STATUS_CONNECTING,
    NET_STATUS_CONNECTED,
    NET_STATUS_FAILED
} net_status_t;

/**
 * @brief Network event callback
 */
typedef void (*net_event_callback_t)(net_interface_t interface, net_status_t status, void *user_data);

/**
 * @brief Network manager configuration
 */
typedef struct {
    bool wifi_enabled;
    bool ethernet_enabled;
    bool auto_failover;
    net_interface_t preferred_interface;
    net_event_callback_t event_callback;
    void *callback_user_data;
} net_manager_config_t;

/**
 * @brief Initialize network manager
 *
 * @param config Network manager configuration
 * @return ESP_OK on success
 */
esp_err_t net_manager_init(const net_manager_config_t *config);

/**
 * @brief Start network connections
 *
 * @return ESP_OK on success
 */
esp_err_t net_manager_start(void);

/**
 * @brief Stop network connections
 *
 * @return ESP_OK on success
 */
esp_err_t net_manager_stop(void);

/**
 * @brief Get current active interface
 *
 * @return Active interface type
 */
net_interface_t net_manager_get_active_interface(void);

/**
 * @brief Get network status for an interface
 *
 * @param interface Interface to query
 * @return Network status
 */
net_status_t net_manager_get_status(net_interface_t interface);

/**
 * @brief Check if network is connected
 *
 * @return true if any interface is connected
 */
bool net_manager_is_connected(void);

/**
 * @brief Get IP address of active interface
 *
 * @param ip_info Output IP information
 * @return ESP_OK on success
 */
esp_err_t net_manager_get_ip_info(esp_netif_ip_info_t *ip_info);

/**
 * @brief Get active netif handle
 *
 * @return esp_netif handle or NULL
 */
esp_netif_t *net_manager_get_netif(void);

/**
 * @brief Force switch to specific interface
 *
 * @param interface Interface to switch to
 * @return ESP_OK on success
 */
esp_err_t net_manager_switch_interface(net_interface_t interface);

/**
 * @brief WiFi initialization
 */
esp_err_t wifi_handler_init(void);
esp_err_t wifi_handler_start(void);
esp_err_t wifi_handler_stop(void);
net_status_t wifi_handler_get_status(void);
esp_netif_t *wifi_handler_get_netif(void);

/**
 * @brief Ethernet initialization (W5500)
 */
esp_err_t ethernet_handler_init(void);
esp_err_t ethernet_handler_start(void);
esp_err_t ethernet_handler_stop(void);
net_status_t ethernet_handler_get_status(void);
esp_netif_t *ethernet_handler_get_netif(void);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_MANAGER_H
