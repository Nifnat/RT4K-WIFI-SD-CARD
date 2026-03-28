#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

/**
 * Start the HTTP web server on port 80.
 * Registers all URI handlers for file management, WiFi config, etc.
 * SPIFFS must be mounted before calling this.
 */
esp_err_t web_server_start(void);

/**
 * Stop the HTTP web server.
 */
void web_server_stop(void);

#endif /* WEB_SERVER_H */
