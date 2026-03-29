#include "rt4k_config.h"
#include "sd_control.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "config";

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "password"

/* Validate that a string contains only printable ASCII (32-126) */
static bool validate_ascii(const char *str, size_t max_len)
{
    size_t len = strnlen(str, max_len);
    if (len == 0 || len >= max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (str[i] < 32 || str[i] > 126) {
            return false;
        }
    }
    return true;
}

/* Try to load WiFi config from NVS */
static bool load_from_nvs(rt4k_wifi_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS namespace not found: %s", esp_err_to_name(err));
        return false;
    }

    size_t ssid_len = sizeof(cfg->ssid);
    size_t pass_len = sizeof(cfg->password);

    err = nvs_get_str(handle, NVS_KEY_SSID, cfg->ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS ssid read failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    err = nvs_get_str(handle, NVS_KEY_PASS, cfg->password, &pass_len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS password read failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    nvs_close(handle);

    if (!validate_ascii(cfg->ssid, WIFI_SSID_MAX_LEN) ||
        !validate_ascii(cfg->password, WIFI_PASS_MAX_LEN)) {
        ESP_LOGW(TAG, "NVS data failed validation");
        return false;
    }

    ESP_LOGI(TAG, "Loaded config from NVS - SSID: %s", cfg->ssid);
    return true;
}

/* Parse config.txt from SD card (INI-like: SSID=xxx / PASSWORD=xxx) */
static bool load_from_sd(rt4k_wifi_config_t *cfg)
{
    if (sd_control_can_take() != 0) {
        ESP_LOGW(TAG, "SD card not available for config read");
        return false;
    }

    sd_control_take();

    FILE *f = fopen(CONFIG_FILE_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open %s", CONFIG_FILE_PATH);
        sd_control_relinquish();
        return false;
    }

    bool got_ssid = false;
    bool got_pass = false;
    char line[128];

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing whitespace / newlines */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        /* Trim key */
        while (*key == ' ') key++;
        char *end = key + strlen(key) - 1;
        while (end > key && *end == ' ') *end-- = '\0';

        /* Trim value */
        while (*val == ' ') val++;
        end = val + strlen(val) - 1;
        while (end > val && *end == ' ') *end-- = '\0';

        if (strcasecmp(key, "SSID") == 0 && strlen(val) > 0) {
            strncpy(cfg->ssid, val, WIFI_SSID_MAX_LEN - 1);
            cfg->ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
            got_ssid = true;
            ESP_LOGI(TAG, "SD config: SSID=%s", cfg->ssid);
        } else if (strcasecmp(key, "PASSWORD") == 0 && strlen(val) > 0) {
            strncpy(cfg->password, val, WIFI_PASS_MAX_LEN - 1);
            cfg->password[WIFI_PASS_MAX_LEN - 1] = '\0';
            got_pass = true;
            ESP_LOGI(TAG, "SD config: PASSWORD loaded");
        }
    }

    fclose(f);
    sd_control_relinquish();

    if (!got_ssid || !got_pass) {
        ESP_LOGW(TAG, "SD config incomplete (ssid=%d, pass=%d)", got_ssid, got_pass);
        return false;
    }

    return true;
}

bool rt4k_config_load(rt4k_wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* Try NVS first */
    if (load_from_nvs(cfg)) {
        cfg->valid = true;
        return true;
    }

    /* Fall back to SD card config.txt */
    if (load_from_sd(cfg)) {
        /* Save to NVS for next boot */
        rt4k_config_save(cfg->ssid, cfg->password);
        cfg->valid = true;
        return true;
    }

    cfg->valid = false;
    return false;
}

bool rt4k_config_save(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set ssid failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    err = nvs_set_str(handle, NVS_KEY_PASS, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set password failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Config saved to NVS - SSID: %s", ssid);
    return true;
}

void rt4k_config_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Config cleared");
}
