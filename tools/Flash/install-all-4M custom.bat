@echo off
:startBurn

echo. 
echo.     ┃                今天是 %date%  %time%               ┃
echo. 

set BinaryDir=binary-4M
set EsptoolPath=win64\esptool.exe

set BaseArgs=--chip esp32 --baud 921600
set SetupArgs=--before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size detect

set Bootloader=0x1000 %BinaryDir%\bootloader.bin
set Bootapp=0xe000 %BinaryDir%\boot_app0.bin
set Partitions=0x8000 %BinaryDir%\partitions.bin
set Firmware=0x10000 %BinaryDir%\SdWiFiBrowser.ino.bin
set LocalFS=0x210000 %BinaryDir%\spiffs.bin

echo %EsptoolPath% %BaseArgs% %SetupArgs% %Bootloader% %Bootapp% %Firmware% %Partitions%
%EsptoolPath% %BaseArgs% %SetupArgs% %Bootloader% %Bootapp% %Firmware% %Partitions%

echo %EsptoolPath% %BaseArgs% %SetupArgs% %LocalFS%
%EsptoolPath% %BaseArgs% %SetupArgs% %LocalFS%

pause
::goto startBurn
