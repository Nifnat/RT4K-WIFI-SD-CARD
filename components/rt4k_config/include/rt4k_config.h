#ifndef RT4K_CONFIG_H
#define RT4K_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define WIFI_SSID_MAX_LEN   32
#define WIFI_PASS_MAX_LEN   64
#define OTA_PASS_HASH_LEN   65  /* 64 hex chars + null */
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

/**
 * Get OTA password from NVS. Returns true if a password is set.
 */
bool rt4k_config_get_ota_password(char *buf, size_t buf_len);

/**
 * Set OTA password in NVS. Pass empty string to clear.
 */
bool rt4k_config_set_ota_password(const char *password);

/**
 * Check plaintext password against stored SHA-256 hash.
 */
bool rt4k_config_check_ota_password(const char *password);

#endif /* RT4K_CONFIG_H */
