#include "sd_control.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pins.h"

static const char *TAG = "sd_ctrl";

#define SPI_BLOCKOUT_BOOTUP_PERIOD_S   10
#define SPI_BLOCKOUT_PERIOD_MS         5000
#define SDSPI_MAX_TRANSFER_SIZE        (8 * 1024)

/* State */
static volatile int64_t s_blockout_time_us = 0;
static bool s_we_took_bus = false;
static bool s_access_enabled = false;

/* Mutex to prevent concurrent SD operations from multiple HTTP handlers */
static SemaphoreHandle_t s_sd_mutex = NULL;

/* SPI bus and card handles for mount/unmount */
static sdmmc_card_t *s_card = NULL;
static bool s_spi_bus_initialized = false;

/* ISR: Fires when RT4K asserts/deasserts CS on the SD card.
 * Note: This only works if RT4K uses 4-bit SD mode (DAT3 toggles).
 * In 1-bit mode DAT3 is unused, so this ISR won't fire — the manual
 * access toggle in the web UI is the primary gating mechanism. */
static void IRAM_ATTR cs_sense_isr(void *arg)
{
    if (!s_we_took_bus) {
        s_blockout_time_us = esp_timer_get_time() + (SPI_BLOCKOUT_PERIOD_MS * 1000LL);
    } else {
        s_blockout_time_us = esp_timer_get_time();
    }
}

esp_err_t sd_control_init(void)
{
    s_sd_mutex = xSemaphoreCreateMutex();
    if (!s_sd_mutex) {
        ESP_LOGE(TAG, "Failed to create SD mutex");
        return ESP_FAIL;
    }

    /* Configure SD switch pin (mux control) */
    gpio_config_t sw_cfg = {
        .pin_bit_mask = (1ULL << PIN_SD_SWITCH),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sw_cfg);

    /* Configure SD power pin */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << PIN_SD_POWER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);

    /* Configure CS_SENSE input with interrupt on any edge */
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << PIN_CS_SENSE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&cs_cfg);

    /* Install GPIO ISR service and attach handler */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_CS_SENSE, cs_sense_isr, NULL);

    /* Default: mux to RT4K, power on, access disabled */
    gpio_set_level(PIN_SD_SWITCH, 1);
    gpio_set_level(PIN_SD_POWER, 0);

    /* Set initial blockout time */
    s_blockout_time_us = esp_timer_get_time() + (SPI_BLOCKOUT_PERIOD_MS * 1000LL);

    ESP_LOGI(TAG, "SD access disabled by default — enable via web UI");

    return ESP_OK;
}

void sd_control_take(void)
{
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    if (s_we_took_bus) {
        xSemaphoreGive(s_sd_mutex);
        return;
    }

    s_we_took_bus = true;

    /* Switch mux to ESP32 */
    gpio_set_level(PIN_SD_SWITCH, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Initialize SPI bus if not already done */
    if (!s_spi_bus_initialized) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = PIN_SD_MOSI,
            .miso_io_num = PIN_SD_MISO,
            .sclk_io_num = PIN_SD_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SDSPI_MAX_TRANSFER_SIZE,
        };
        esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
            s_we_took_bus = false;
            gpio_set_level(PIN_SD_SWITCH, 1);
            xSemaphoreGive(s_sd_mutex);
            return;
        }
        s_spi_bus_initialized = true;
    }

    /* Mount SD card via SPI with FATFS */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 4 * 1024,
    };

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.host_id = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                             &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        /* Retry up to 5 times like the Arduino version */
        for (int i = 0; i < 4 && err != ESP_OK; i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                           &mount_cfg, &s_card);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD mount failed after retries: %s", esp_err_to_name(err));
            s_we_took_bus = false;
            gpio_set_level(PIN_SD_SWITCH, 1);
            xSemaphoreGive(s_sd_mutex);
            return;
        }
    }

    ESP_LOGD(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    xSemaphoreGive(s_sd_mutex);
}

void sd_control_relinquish(void)
{
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    if (!s_we_took_bus) {
        xSemaphoreGive(s_sd_mutex);
        return;
    }

    /* Unmount SD card */
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }

    /* Free SPI bus */
    if (s_spi_bus_initialized) {
        spi_bus_free(SPI2_HOST);
        s_spi_bus_initialized = false;
    }

    /* Set SD data pins to input with pullup (release bus cleanly) */
    static const int sd_pins[] = {
        PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3,
        PIN_SD_CLK, PIN_SD_CMD
    };
    for (int i = 0; i < sizeof(sd_pins) / sizeof(sd_pins[0]); i++) {
        gpio_reset_pin(sd_pins[i]);
        gpio_set_direction(sd_pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(sd_pins[i], GPIO_PULLUP_ONLY);
    }

    /* Switch mux back to RT4K */
    gpio_set_level(PIN_SD_SWITCH, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    s_we_took_bus = false;

    ESP_LOGD(TAG, "SD bus relinquished");
    xSemaphoreGive(s_sd_mutex);
}

int sd_control_can_take(void)
{
    /* Primary gate: access must be enabled via web UI */
    if (!s_access_enabled) {
        return -1;
    }

    /* If we already hold the bus, allow (re-entrant from same enable session) */
    if (s_we_took_bus) return 0;

    /* Secondary check: CS_SENSE blockout (works if RT4K uses 4-bit mode) */
    int64_t now = esp_timer_get_time();
    if (now < s_blockout_time_us) {
        ESP_LOGD(TAG, "Blocked: %" PRId64 " us remaining",
                 s_blockout_time_us - now);
        return -1;
    }
    return 0;
}

bool sd_control_we_have_control(void)
{
    return s_we_took_bus;
}

esp_err_t sd_control_set_access(bool enabled)
{
    if (enabled == s_access_enabled) {
        return ESP_OK;
    }

    if (enabled) {
        /* Check CS_SENSE blockout before enabling */
        int64_t now = esp_timer_get_time();
        if (now < s_blockout_time_us) {
            ESP_LOGW(TAG, "Cannot enable: RT4K may be using SD card");
            return ESP_ERR_INVALID_STATE;
        }

        s_access_enabled = true;
        ESP_LOGI(TAG, "SD access enabled — per-request bus sharing active");
    } else {
        s_access_enabled = false;
        sd_control_relinquish();
        ESP_LOGI(TAG, "SD access disabled — bus released to RT4K");
    }

    return ESP_OK;
}

bool sd_control_is_access_enabled(void)
{
    return s_access_enabled;
}
