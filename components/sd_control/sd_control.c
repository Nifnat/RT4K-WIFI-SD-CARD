#include "sd_control.h"

#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
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
#define SDMMC_FREQ_KHZ                 5000   /* 5 MHz — plenty for WiFi-limited throughput */
#define SDSPI_FREQ_KHZ                 5000   /* 5 MHz for SPI — saves power on marginal supplies */

/* State */
static volatile int64_t s_blockout_time_us = 0;
static bool s_we_took_bus = false;
static bool s_access_enabled = false;
static bool s_esp_exclusive = false;   /* ESP-only: ignores CS_SENSE blockout */
static int  s_hold_count = 0;         /* >0: relinquish skips unmount */

/* Mutex to prevent concurrent SD operations from multiple HTTP handlers */
static SemaphoreHandle_t s_sd_mutex = NULL;

/* Card handle for mount/unmount */
static sdmmc_card_t *s_card = NULL;
static bool s_spi_bus_initialized = false;

/* Last successful mount mode: 1 = SDMMC 1-bit, -1 = SPI, 0 = not yet tried.
 * SPI fallback is treated as transient (RT4K may have been on the bus),
 * so we always re-attempt SDMMC on the next mount. */
static int s_sdmmc_width = 0;

/* Probe negotiation log — human-readable record of last probe attempt */
#define PROBE_LOG_SIZE 384
static char s_probe_log[PROBE_LOG_SIZE] = "no probe yet";

static void probe_log_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_probe_log, PROBE_LOG_SIZE, fmt, ap);
    va_end(ap);
}

static void probe_log_cat(const char *fmt, ...)
{
    size_t cur = strlen(s_probe_log);
    if (cur >= PROBE_LOG_SIZE - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_probe_log + cur, PROBE_LOG_SIZE - cur, fmt, ap);
    va_end(ap);
}

/* ISR: Fires when RT4K asserts/deasserts CS on the SD card.
 * Note: DAT3 is unused in 1-bit SDMMC mode, so this ISR won't fire —
 * the manual access toggle in the web UI is the primary gating mechanism. */
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

    /* Set SD bus signal pins to INPUT + PULLUP at boot. */
    static const int sd_pins[] = {
        PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3,
        PIN_SD_CLK, PIN_SD_CMD
    };
    for (int i = 0; i < (int)(sizeof(sd_pins) / sizeof(sd_pins[0])); i++) {
        gpio_reset_pin(sd_pins[i]);
        gpio_set_direction(sd_pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(sd_pins[i], GPIO_PULLUP_ONLY);
    }

    /* Send CMD0 (GO_IDLE_STATE) to the SD card immediately at boot.
     *
     * SD cards power on in native SD mode where their internal state
     * machine is actively polling for initialisation.  This draws
     * ~25-35 mA at 3.3 V.  Sending CMD0 via SPI transitions the card to
     * SPI-idle mode — a low-power defined waiting state that draws <5 mA.
     * Without this, the power drop only happens after the first user-
     * triggered SD access.
     *
     * This probe also cycles the mux chip through a clean 0 → 1
     * transition, which settles it from the undefined float at boot.
     *
     * One attempt only — CMD0 needs no retries; on failure we still
     * leave the bus and mux in a clean state. */
    {
        gpio_set_level(PIN_SD_SWITCH, 0);   /* mux to ESP32 */
        vTaskDelay(pdMS_TO_TICKS(50));

        spi_bus_config_t boot_bus = {
            .mosi_io_num     = PIN_SD_MOSI,
            .miso_io_num     = PIN_SD_MISO,
            .sclk_io_num     = PIN_SD_SCLK,
            .quadwp_io_num   = -1,
            .quadhd_io_num   = -1,
            .max_transfer_sz = SDSPI_MAX_TRANSFER_SIZE,
        };
        if (spi_bus_initialize(SPI2_HOST, &boot_bus, SPI_DMA_CH_AUTO) == ESP_OK) {
            sdspi_device_config_t boot_slot = SDSPI_DEVICE_CONFIG_DEFAULT();
            boot_slot.gpio_cs = PIN_SD_CS;
            boot_slot.host_id = SPI2_HOST;

            sdmmc_host_t boot_host = SDSPI_HOST_DEFAULT();
            boot_host.slot         = SPI2_HOST;
            boot_host.max_freq_khz = 400;   /* SD-spec minimum; CMD0 works at any freq */

            esp_vfs_fat_sdmmc_mount_config_t boot_mcfg = {
                .format_if_mount_failed = false,
                .max_files              = 1,
                .allocation_unit_size   = 4096,
            };
            sdmmc_card_t *boot_card = NULL;
            esp_err_t boot_err = esp_vfs_fat_sdspi_mount(
                SD_MOUNT_POINT, &boot_host, &boot_slot, &boot_mcfg, &boot_card);

            /* CMD0 was sent regardless of whether the full mount succeeded */
            if (boot_err == ESP_OK && boot_card) {
                esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, boot_card);
                ESP_LOGI(TAG, "Boot CMD0 probe: card mounted and released");
            } else {
                ESP_LOGI(TAG, "Boot CMD0 probe: mount failed (%s) — CMD0 still sent",
                         esp_err_to_name(boot_err));
            }
            spi_bus_free(SPI2_HOST);
        } else {
            ESP_LOGW(TAG, "Boot CMD0 probe: SPI bus init failed");
        }

        /* Reset SD pins and switch mux back to RT4K */
        static const int sd_pins2[] = {
            PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3,
            PIN_SD_CLK, PIN_SD_CMD
        };
        for (int i = 0; i < (int)(sizeof(sd_pins2)/sizeof(sd_pins2[0])); i++) {
            gpio_reset_pin(sd_pins2[i]);
            gpio_set_direction(sd_pins2[i], GPIO_MODE_INPUT);
            gpio_set_pull_mode(sd_pins2[i], GPIO_PULLUP_ONLY);
        }
        gpio_set_level(PIN_SD_SWITCH, 1);   /* back to RT4K */

        /* s_spi_bus_initialized stays false — the probe freed the bus */
    }

    /* Set initial blockout time (after probe so it doesn't expire mid-probe) */
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

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 32 * 1024,
    };

    esp_err_t err = ESP_FAIL;


    /* ── Try SDMMC 1-bit, then SPI fallback ── */
    if (s_sdmmc_width == 1) {
        /* SDMMC previously worked — try it directly (skip full probe) */
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = SDMMC_FREQ_KHZ;
        host.flags = SDMMC_HOST_FLAG_1BIT;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;

        err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                       &mount_cfg, &s_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SDMMC 1-bit remount failed: %s, will re-probe",
                     esp_err_to_name(err));
            s_sdmmc_width = 0; /* Force re-probing */
        }
    }

    // /* Probe path: entered on first mount, after cached-SDMMC failure,
    //  * AND after SPI fallback (s_sdmmc_width == -1) so we always
    //  * re-attempt SDMMC in case the RT4K is no longer on the bus. */
    // if (s_sdmmc_width <= 0 && err != ESP_OK) {
    //     probe_log_set("Probing @ %d kHz: ", SDMMC_FREQ_KHZ);

    //     /* Try 1-bit SDMMC — single attempt; if the RT4K's host
    //      * controller is holding the bus we won't recover with retries. */
    //     sdmmc_host_t host1 = SDMMC_HOST_DEFAULT();
    //     host1.max_freq_khz = SDMMC_FREQ_KHZ;
    //     host1.flags = SDMMC_HOST_FLAG_1BIT;

    //     sdmmc_slot_config_t slot1 = SDMMC_SLOT_CONFIG_DEFAULT();
    //     slot1.width = 1;

    //     err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host1, &slot1,
    //                                    &mount_cfg, &s_card);
    //     if (err == ESP_OK) {
    //         s_sdmmc_width = 1;
    //         probe_log_cat("1-bit OK");
    //         ESP_LOGI(TAG, "SD card mounted via SDMMC 1-bit @ %d kHz", SDMMC_FREQ_KHZ);
    //     } else {
    //         probe_log_cat("1-bit: %s", esp_err_to_name(err));
    //         ESP_LOGW(TAG, "SDMMC 1-bit failed: %s, falling back to SPI", esp_err_to_name(err));
    //     }
    // }


    /* ── SPI fallback ── */
    if (err != ESP_OK) {

        if (!s_spi_bus_initialized) {
            spi_bus_config_t bus_cfg = {
                .mosi_io_num = PIN_SD_MOSI,
                .miso_io_num = PIN_SD_MISO,
                .sclk_io_num = PIN_SD_SCLK,
                .quadwp_io_num = -1,
                .quadhd_io_num = -1,
                .max_transfer_sz = SDSPI_MAX_TRANSFER_SIZE,
            };
            esp_err_t bus_err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
            if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(bus_err));
                s_we_took_bus = false;
                gpio_set_level(PIN_SD_SWITCH, 1);
                xSemaphoreGive(s_sd_mutex);
                return;
            }
            s_spi_bus_initialized = true;
        }

        sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_cfg.gpio_cs = PIN_SD_CS;
        slot_cfg.host_id = SPI2_HOST;

        sdmmc_host_t host_spi = SDSPI_HOST_DEFAULT();
        host_spi.slot = SPI2_HOST;
        host_spi.max_freq_khz = SDSPI_FREQ_KHZ;

        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host_spi, &slot_cfg,
                                       &mount_cfg, &s_card);
        for (int i = 0; i < 4 && err != ESP_OK; i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host_spi, &slot_cfg,
                                           &mount_cfg, &s_card);
        }

        if (err == ESP_OK) {
            s_sdmmc_width = -1;  /* Remember SPI mode */
            probe_log_cat(" | SPI: OK");
            ESP_LOGI(TAG, "SD card mounted via SPI @ %d kHz", SDSPI_FREQ_KHZ);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed after all attempts: %s", esp_err_to_name(err));
        s_we_took_bus = false;
        gpio_set_level(PIN_SD_SWITCH, 1);
        xSemaphoreGive(s_sd_mutex);
        return;
    }

    xSemaphoreGive(s_sd_mutex);
}

void sd_control_relinquish(void)
{
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    if (!s_we_took_bus) {
        xSemaphoreGive(s_sd_mutex);
        return;
    }

    if (s_hold_count > 0) {
        xSemaphoreGive(s_sd_mutex);
        return;
    }

    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }

    /* Free SPI bus if we used SPI mode.
     * The unmount above removed the SPI device but didn't free the bus. */
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

    /* Switch mux back to RT4K — unless ESP-exclusive keeps it locked */
    if (!s_esp_exclusive) {
        gpio_set_level(PIN_SD_SWITCH, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_we_took_bus = false;

    ESP_LOGD(TAG, "SD bus relinquished%s",
             s_esp_exclusive ? " (mux held for ESP-exclusive)" : "");
    xSemaphoreGive(s_sd_mutex);
}

void sd_control_hold(void)
{
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    s_hold_count++;
    xSemaphoreGive(s_sd_mutex);
}

void sd_control_unhold(void)
{
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    if (s_hold_count > 0) s_hold_count--;
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

    /* ESP-exclusive mode: skip CS_SENSE blockout entirely */
    if (s_esp_exclusive) return 0;

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

void sd_control_set_esp_exclusive(bool exclusive)
{
    s_esp_exclusive = exclusive;
    ESP_LOGI(TAG, "ESP-exclusive SD mode %s", exclusive ? "ON" : "OFF");

    if (exclusive) {
        /* Immediately lock mux to ESP side so RT4K loses access */
        gpio_set_level(PIN_SD_SWITCH, 0);
    } else if (!s_we_took_bus) {
        /* Release mux back to RT4K (only if we don't currently hold bus) */
        gpio_set_level(PIN_SD_SWITCH, 1);
    }
}

bool sd_control_is_esp_exclusive(void)
{
    return s_esp_exclusive;
}

const char *sd_control_get_bus_mode(void)
{
    if (!s_access_enabled) return "disabled";
    switch (s_sdmmc_width) {
    case 1:  return "SDMMC 1-bit";
    case -1: return "SPI";
    default: return "not yet probed";
    }
}

const char *sd_control_get_probe_log(void)
{
    return s_probe_log;
}

esp_err_t sd_control_reprobe(void)
{
    if (!s_access_enabled) return ESP_ERR_INVALID_STATE;

    /* Unmount if mounted */
    if (s_we_took_bus) {
        sd_control_relinquish();
    }

    /* Reset probe cache to force fresh negotiation */
    s_sdmmc_width = 0;

    /* Re-take with fresh probe */
    sd_control_take();

    return s_we_took_bus ? ESP_OK : ESP_FAIL;
}
