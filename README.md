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

## Update firmware

Reflash:
See above

OTA:
In the settings menu upload the correct bin and the ESP will perform an OTA.

There are 2 possible bins to upload, rt4k-wifi-sd-card.bin for the code and storage.bin for the SPIFFS partition (static files for the web UI)
Depending on what's been updated you might only have to flash one of these but it's best to flash both.

## Access SD WIFI PRO (SWP)
1. Access the softAP RT4K-SD-WIFI
2. Go to the settings page
3. Scan for Wifi or enter SSID manually and then enter password
4. Hit connect, connection details will be saved to the NVS and should stay even through updates


Below is broken - might be fixed soon TM
1. Add a text file to the root of the SD card named `config.txt`
2. Format it like below:
```
SSID=
PASSWORD=
```
3. On boot of the ESP you can check the serial port to see what IP your router has assigned to it or use `[http://rt4ksd.local/](http://rt4ksdcard.local/)` 

## Building 
Building the code should work via the Arduino IDE although I have had problems with it when using ESP32 v3.x

### libs:
Now using standard libs from the IDF component register
- esp_http_server 
- esp_common
- log
- spiffs
- sd_control
- network
- app_update


### Command for building via CLI
```bash
idf.py build
```

### Building the SPIFFS bin:
idf build now handles this via the below in cMakeLists
```bash
spiffs_create_partition_image(storage data FLASH_IN_PROJECT)
```

Optional-Merged:  
`tools\Flash\merge_bin.bat` is a batch file to merge all of the seperate bins to one bin for flashing (SH file coming soon TM)

From there you can run the `install-all-4M custom-spiifs.bat` flashing the sepertate bins or Optional-Merged: run the `flash-merged.X` where X is either bat or sh depending on your OS
