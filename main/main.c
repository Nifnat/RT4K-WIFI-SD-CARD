#include <stdio.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "sd_control.h"
#include "network.h"
#include "web_server.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "RT4K WiFi SD Card - ESP-IDF");

    /* Initialize NVS (required for WiFi and config storage) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize SPIFFS for web UI static files */
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    ret = esp_vfs_spiffs_register(&spiffs_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("storage", &total, &used);
        ESP_LOGI(TAG, "SPIFFS: %d/%d bytes used", used, total);
    }

    /* Initialize SD card bus control (GPIO mux, CS_SENSE ISR, bootup wait) */
    ESP_ERROR_CHECK(sd_control_init());

    /* Initialize and start WiFi */
    ESP_ERROR_CHECK(network_init());
    network_start();

    /* Start HTTP web server */
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "System ready");
}
