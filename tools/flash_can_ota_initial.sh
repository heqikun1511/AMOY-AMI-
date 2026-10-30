#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 /dev/ttyACM0" >&2
    exit 2
fi

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="$1"

"$project_dir/tools/build_can_ota.sh"

python -m esptool --chip esp32p4 --port "$port" --baud 460800 \
    --before default-reset --after hard-reset write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 16MB \
    0x2000 "$project_dir/recovery/build/bootloader/bootloader.bin" \
    0x8000 "$project_dir/recovery/build/partition_table/partition-table.bin" \
    0xD000 "$project_dir/recovery/build/ota_data_initial.bin" \
    0x10000 "$project_dir/recovery/build/can_recovery.bin" \
    0x190000 "$project_dir/build/lvgl_transplant.bin"

echo "Initial CAN OTA layout installed. Future dashboard updates use tools/can_flash.py."
