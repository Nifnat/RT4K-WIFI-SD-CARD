@echo off
:startBurn

set EsptoolPath=..\win64\esptool.exe
set MergedBinary=RT4K-complete.bin

set BaseArgs=--chip esp32 --baud 921600
set SetupArgs=--before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size detect

echo Flashing merged binary...
echo %EsptoolPath% %BaseArgs% %SetupArgs% 0x0 %MergedBinary%
%EsptoolPath% %BaseArgs% %SetupArgs% 0x0 %MergedBinary%

pause
::goto startBurn