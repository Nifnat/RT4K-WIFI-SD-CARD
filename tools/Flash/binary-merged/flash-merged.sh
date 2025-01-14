#!/bin/bash

MERGED_BINARY="RT4K-complete.bin"

BASE_ARGS="--chip esp32 --baud 921600"
SETUP_ARGS="--before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size detect"

echo "Flashing merged binary..."
echo "esptool $BASE_ARGS $SETUP_ARGS 0x0 $MERGED_BINARY"
esptool.py $BASE_ARGS $SETUP_ARGS 0x0 $MERGED_BINARY