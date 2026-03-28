#ifndef PINS_H
#define PINS_H

/* CS_SENSE: Detect when the RT4K (other master) uses the SD card */
#define PIN_CS_SENSE        32

/* SD_SWITCH_PIN: Controls the SD bus mux (LOW = ESP32, HIGH = RT4K) */
#define PIN_SD_SWITCH       26

/* SD_POWER_PIN: Optional SD power control */
#define PIN_SD_POWER        27

/* SPI SD card pins */
#define PIN_SD_CS           13
#define PIN_SD_MISO          2
#define PIN_SD_MOSI         15
#define PIN_SD_SCLK         14

/* SD MMC pin aliases (used when releasing bus - set to input pullup) */
#define PIN_SD_CMD          15
#define PIN_SD_CLK          14
#define PIN_SD_D0            2
#define PIN_SD_D1            4
#define PIN_SD_D2           12
#define PIN_SD_D3           13

#endif /* PINS_H */
