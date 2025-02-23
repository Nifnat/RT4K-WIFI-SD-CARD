# RT4K WIFI SD CARD Beta Firmware

Beta firmware for SD WIFI PRO for use with a Retrotink 4K

## Based on:
- SdWiFiBrowser @ https://github.com/tiredboffin/SdWiFiBrowser
- Modeline calc from @ https://github.com/Guspaz/rt4k_esp32

## Features:
- A web interface that mostly works:
    - File browser - Now with directories!!!
    - Uploader - now lets you upload to directories!!!
    - Built-in file editor, it needs work but it's ok
    - File renaming for renaming profiles
    - Some semblance of stability - I've had no crashes at all in my own testing and certainly no lockups requiring a reboot of the ESP32. Large (10MB+) filetransfers or editing will likely take ages and possibly fail but so far have not resulted in lockups. This is not a guarantee though.
- Port of Modeline calc with ability to one-click write the selected modeline to one of the four custom slots
    - Now also lets you read current custom modelines. 
- A cool new way to make an RT4K update fail - Latest updates to this firmware should avoid locking the SD card to the ESP32 on boot if valid WiFi creds are stored in EEPROM, but this is not a guarantee.
    - I have had no issues uploading firmware updates and then installing them off the SD-WIFI card, but I would still recommend using an actual SD card for this.

## Flash Binary firmware to SD WIFI PRO (SWP)

Windows (preferred):
Run `tools\Flash\binary-merged\flash-merged.bat` with SD-WIFI-PRO plugged in with dip switches set to 01 (off on)

Linux (preferred):
Run `tools\Flash\binary-merged\flash-merged.sh` with SD-WIFI-PRO plugged in with dip switches set to 01 (off on)

Backup flashing if the merged bin fails:
Run `tools\Flash\install-all-4M custom-spiifs.bat` with SD-WIFI-PRO plugged in with dip switches set to 01 (off on)

## Access SD WIFI PRO (SWP)
1. Add a text file to the root of the SD card named `config.txt`
2. Format it like below:
```
SSID=
PASSWORD=
```
3. On boot of the ESP you can check the serial port to see what IP your router has assigned to it ~~or use `http://rt4ksd.local/` if mDNS decides to work for once~~ - mDNS broke - who would have guessed?

## Building 
Building the code should work via the Arduino IDE although I have had problems with it when using ESP32 v3.x

### External Libs:
- ArduinoJson 7.3.0 @ Should be available through Arduino CLI/IDE
- Async TCP 3.3.2 @ https://github.com/mathieucarbou/AsyncTCP
- ESP Async WebServer 3.6.0 @ https://github.com/mathieucarbou/ESPAsyncWebServer
- ESPAsyncTCP 1.2.4 @ https://github.com/dvarrel/ESPAsyncTCP

### All libs:
- FS 3.1.1
- EEPROM 3.1.1
- WiFi 3.1.1
- Networking 3.1.1
- Async TCP 3.3.2
- ESP Async WebServer 3.6.0
- SPIFFS 3.1.1
- Ticker 3.1.1
- ArduinoOTA 3.1.1
- Update 3.1.1
- ArduinoJson 7.3.0
- SPI 3.1.1
- SD 3.1.1
- ESPmDNS 3.1.1
- SD_MMC 3.1.1

### Command for building via CLI
```bash
arduino-cli compile --fqbn esp32:esp32:pico32:PartitionScheme=no_ota --output-dir build
```

### Building the SPIFFS bin:
I've only tested this in Linux, so who knows:
```bash
/home/<user>/.arduino15/packages/esp32/tools/mkspiffs/0.2.3/mkspiffs -c data --page 256 --block 4096 --size 0x1E0000 tools/Flash/binary-4M/RT4K-WIFI-SD-CARD.filesystem.bin
```

Copy `RT4K-WIFI-SD-CARD.ino.bin` into `tools/Flash/binary-XM` (likely using the 4M version)

Optional-Merged:  
`tools\Flash\merge_bin.bat` is a batch file to merge all of the seperate bins to one bin for flashing (SH file coming soon TM)

From there you can run the `install-all-4M custom-spiifs.bat` flashing the sepertate bins or Optional-Merged: run the `flash-merged.X` where X is either bat or sh depending on your OS
