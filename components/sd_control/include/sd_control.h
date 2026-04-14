#ifndef SD_CONTROL_H
#define SD_CONTROL_H

#include <stdbool.h>
#include "esp_err.h"

/* SD card VFS mount point */
#define SD_MOUNT_POINT "/sdcard"

/**
 * Initialize SD control: configure GPIO mux, CS_SENSE ISR, wait for RT4K.
 */
esp_err_t sd_control_init(void);

/**
 * Take control of the SD bus (switch mux to ESP32, mount FAT).
 */
void sd_control_take(void);

/**
 * Relinquish control of the SD bus (unmount, switch mux to RT4K).
 */
void sd_control_relinquish(void);

/**
 * Check if we can take control.
 * Returns 0 if ok, -1 if blocked (SD access disabled or RT4K using bus).
 */
int sd_control_can_take(void);

/**
 * Returns true if we currently hold the SD bus.
 */
bool sd_control_we_have_control(void);

/**
 * Enable or disable ESP SD card access.
 * When enabled, takes the bus and mounts the card (held until disabled).
 * When disabled, unmounts and releases back to RT4K.
 * Returns ESP_OK on success.
 */
esp_err_t sd_control_set_access(bool enabled);

/**
 * Returns true if SD access is currently enabled.
 */
bool sd_control_is_access_enabled(void);

/**
 * Enable/disable ESP-exclusive SD mode.
 * When on, CS_SENSE blockout is bypassed — the ESP32 always claims the bus.
 */
void sd_control_set_esp_exclusive(bool exclusive);

/**
 * Returns true if ESP-exclusive SD mode is active.
 */
bool sd_control_is_esp_exclusive(void);

/**
 * Get the current SD bus mode string (e.g. "SDMMC 4-bit", "SPI", "not mounted").
 */
const char *sd_control_get_bus_mode(void);

/**
 * Get the probe negotiation log (human-readable results of last probe attempt).
 */
const char *sd_control_get_probe_log(void);

/**
 * Hold the SD bus across multiple requests (increments hold count).
 * While held, sd_control_relinquish() becomes a no-op.
 * The bus must already be taken (call sd_control_take() first).
 */
void sd_control_hold(void);

/**
 * Release a previously acquired hold.
 * The bus is not unmounted until hold count reaches 0 and relinquish is called.
 */
void sd_control_unhold(void);

/**
 * Force re-probe of SD bus mode (unmount, reset cache, remount).
 * Requires SD access to be enabled.
 */
esp_err_t sd_control_reprobe(void);

#endif /* SD_CONTROL_H */
