@echo off
set BuildDir=..\..\build
set BinaryDir=binary-4M
set EsptoolPath=win64\esptool.exe
set MergedBinary=binary-merged\RT4K-complete.bin

:: Copy build outputs to binary-4M
echo Copying build outputs to %BinaryDir%...
copy /Y "%BuildDir%\bootloader\bootloader.bin"            "%BinaryDir%\bootloader.bin"
copy /Y "%BuildDir%\partition_table\partition-table.bin"   "%BinaryDir%\partition-table.bin"
copy /Y "%BuildDir%\ota_data_initial.bin"                  "%BinaryDir%\ota_data_initial.bin"
copy /Y "%BuildDir%\rt4k-wifi-sd-card.bin"                 "%BinaryDir%\rt4k-wifi-sd-card.bin"
copy /Y "%BuildDir%\storage.bin"                           "%BinaryDir%\storage.bin"

:: Create merged binary
echo Creating merged binary: %MergedBinary%

%EsptoolPath% --chip esp32 merge_bin --flash_mode dio --flash_freq 80m --flash_size 4MB -o %MergedBinary% ^
    0x1000  %BinaryDir%\bootloader.bin ^
    0x8000  %BinaryDir%\partition-table.bin ^
    0xF000  %BinaryDir%\ota_data_initial.bin ^
    0x20000 %BinaryDir%\rt4k-wifi-sd-card.bin ^
    0x2C0000 %BinaryDir%\storage.bin

echo Merged binary created successfully!
pause