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
 * Returns 0 if ok, -1 if blocked (RT4K is using the bus).
 */
int sd_control_can_take(void);

/**
 * Returns true if we currently hold the SD bus.
 */
bool sd_control_we_have_control(void);

#endif /* SD_CONTROL_H */
