#ifndef RT4K_CONFIG_H
#define RT4K_CONFIG_H

#include <stdbool.h>

#define WIFI_SSID_MAX_LEN   32
#define WIFI_PASS_MAX_LEN   64
#define CONFIG_FILE_PATH    "/sdcard/config.txt"

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN];
    char password[WIFI_PASS_MAX_LEN];
    bool valid;
} rt4k_wifi_config_t;

/**
 * Load WiFi config. Tries NVS first, then SD card config.txt fallback.
 * Returns true if valid config was loaded.
 */
bool rt4k_config_load(rt4k_wifi_config_t *cfg);

/**
 * Save WiFi credentials to NVS.
 */
bool rt4k_config_save(const char *ssid, const char *password);

/**
 * Clear stored WiFi credentials from NVS.
 */
void rt4k_config_clear(void);

#endif /* RT4K_CONFIG_H */
