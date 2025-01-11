Beta of firmware for SD WIFI PRO for use with a Retrotink 4K

Based of:
SdWiFiBrowser @ https://github.com/tiredboffin/SdWiFiBrowser
Modeline calc from @ https://github.com/Guspaz/rt4k_esp32

Features:
A web interface that mostly works:
	File browser - Now with directories!!!
	Uploader - now lets you upload to directories!!!
	Built in file editor, it needs work but it's ok
	Some semblance of stability 
	
Port of Modeline calc with ability to one click write the selectled modeline to one of the four custom slots


### Flash Binary firmware to SD WIFI PRO(SWP)
Run \tools\Flash\install-all-4M custom-spiifs.bat with SD-WIFI-PRO plugged in with dip switches set to 01 (off on)

### Access SD WIFI PRO(SWP)
Add a text file to the root of the SD card named config.txt
Format it like below
SSID=
PASSWORD=

On boot of the ESP you can check the serial port to see what IP your router has assigned to it or use http://rt4ksd.local/ if mDNS decides to work for once

### Functions


### Building 
Building the code should work via the arduino IDE although I have had problems with it when using ESP32 v3.x

External Libs:
ArduinoJson         7.3.0 @ Should be available through arduino CLI/IDE
Async TCP           3.3.2 @ https://github.com/mathieucarbou/AsyncTCP
ESP Async WebServer 3.6.0 @ https://github.com/mathieucarbou/ESPAsyncWebServer
ESPAsyncTCP         1.2.4 @ https://github.com/dvarrel/ESPAsyncTCP

All libs:
FS                  3.1.1
EEPROM              3.1.1
WiFi                3.1.1
Networking          3.1.1
Async TCP           3.3.2
ESP Async WebServer 3.6.0
SPIFFS              3.1.1
Ticker              3.1.1
ArduinoOTA          3.1.1
Update              3.1.1
ArduinoJson         7.3.0
SPI                 3.1.1
SD                  3.1.1
ESPmDNS             3.1.1
SD_MMC              3.1.1


Command for building via CLI
arduino-cli compile --fqbn esp32:esp32:pico32:PartitionScheme=no_ota --output-dir build

Building the spiffs bin:
I've only tested this in linux so who knows
/home/<user>/.arduino15/packages/esp32/tools/mkspiffs/0.2.3/mkspiffs -c data --page 256 --block 4096 --size 0x1E0000 tools/Flash/binary-4M/RT4K-WIFI-SD-CARD.filesystem.bin
Copy RT4K-WIFI-SD-CARD.ino.bin into tools/Flash/binary-XM (likely using the 4M version)

    