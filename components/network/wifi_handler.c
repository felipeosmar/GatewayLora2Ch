/**
 * @file wifi_handler.c
 * @brief WiFi Connection Handler
 */

#include <string.h>
#include "network_manager.h"
#include "gateway_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_handler";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static esp_netif_t *s_wifi_netif = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static net_status_t s_wifi_status = NET_STATUS_DISCONNECTED;
static int s_retry_num = 0;
static bool s_initialized = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    const gateway_config_t *config = gw_config_get();

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi started, connecting...");
                s_wifi_status = NET_STATUS_CONNECTING;
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                if (s_retry_num < config->wifi.max_retry) {
                    s_retry_num++;
                    ESP_LOGI(TAG, "WiFi disconnected, retry %d/%d",
                             s_retry_num, config->wifi.max_retry);
                    s_wifi_status = NET_STATUS_CONNECTING;
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "WiFi connection failed after %d retries", s_retry_num);
                    s_wifi_status = NET_STATUS_FAILED;
                    if (s_wifi_event_group) {
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected to AP");
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_retry_num = 0;
            s_wifi_status = NET_STATUS_CONNECTED;
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            ESP_LOGW(TAG, "WiFi lost IP");
            s_wifi_status = NET_STATUS_CONNECTING;
        }
    }
}

esp_err_t wifi_handler_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WiFi...");

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Create default WiFi station netif
    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (!s_wifi_netif) {
        ESP_LOGE(TAG, "Failed to create WiFi netif");
        return ESP_FAIL;
    }

    // Initialize WiFi with default config
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler");
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler");
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                              &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP lost event handler");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");

    return ESP_OK;
}

esp_err_t wifi_handler_start(void)
{
    if (!s_initialized) {
        esp_err_t ret = wifi_handler_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    const gateway_config_t *config = gw_config_get();

    if (!config->wifi.enabled) {
        ESP_LOGI(TAG, "WiFi disabled in config");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting WiFi, SSID: %s", config->wifi.ssid);

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, config->wifi.ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, config->wifi.password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }

    s_retry_num = 0;
    s_wifi_status = NET_STATUS_CONNECTING;

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi started");
    return ESP_OK;
}

esp_err_t wifi_handler_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping WiFi...");

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "WiFi disconnect warning: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "WiFi stop warning: %s", esp_err_to_name(ret));
    }

    s_wifi_status = NET_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi stopped");

    return ESP_OK;
}

net_status_t wifi_handler_get_status(void)
{
    return s_wifi_status;
}

esp_netif_t *wifi_handler_get_netif(void)
{
    return s_wifi_netif;
}
