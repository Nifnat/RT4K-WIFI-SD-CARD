#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include "esp_err.h"

#define NET_HOSTNAME           "RT4KSD"
#define NET_AP_SSID            "RT4K-SD-WIFI"
#define NET_AP_IP              "192.168.4.1"
#define NET_AP_GW              "192.168.4.1"
#define NET_AP_NETMASK         "255.255.255.0"
#define NET_CONNECT_TIMEOUT_MS 30000

typedef enum {
    NET_STATUS_DISCONNECTED = 1,
    NET_STATUS_CONNECTING   = 2,
    NET_STATUS_CONNECTED    = 3,
} net_status_t;

/**
 * Initialize networking (event loop, netif, WiFi driver).
 * Does not connect — call network_start() after.
 */
esp_err_t network_init(void);

/**
 * Start WiFi — loads config and connects, or falls back to SoftAP.
 */
void network_start(void);

/**
 * Connect to a specific AP (called from web UI).
 * Runs asynchronously via event loop.
 * Returns true if connection attempt started.
 */
bool network_connect(const char *ssid, const char *password);

/**
 * Start SoftAP mode.
 */
void network_start_softap(void);

/**
 * Trigger a WiFi scan. Results available after scan completes.
 */
void network_start_scan(void);

/**
 * Get WiFi scan results as JSON string.
 * Writes to buf, up to buf_len bytes. Returns number of bytes written.
 */
int network_get_scan_results_json(char *buf, size_t buf_len);

/**
 * Get current network status.
 */
net_status_t network_get_status(void);

/**
 * Get current IP address as string. Returns empty string if not connected.
 */
void network_get_ip_str(char *buf, size_t buf_len);

/**
 * Returns true if currently in STA mode.
 */
bool network_is_sta_mode(void);

#endif /* NETWORK_H */
