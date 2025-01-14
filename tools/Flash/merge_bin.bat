@echo off
set BinaryDir=binary-4M
set EsptoolPath=win64\esptool.exe

:: Set the output file name for the merged binary
set MergedBinary=binary-merged\RT4K-complete.bin

echo Creating merged binary: %MergedBinary%

%EsptoolPath% --chip esp32 merge_bin -o %MergedBinary% ^
    0x1000 %BinaryDir%\bootloader.bin ^
    0xe000 %BinaryDir%\boot_app0.bin ^
    0x8000 %BinaryDir%\partitions.bin ^
    0x10000 %BinaryDir%\RT4K-WIFI-SD-CARD.ino.bin ^
    0x210000 %BinaryDir%\RT4K-WIFI-SD-CARD.filesystem.bin

echo Merged binary created successfully!
pause