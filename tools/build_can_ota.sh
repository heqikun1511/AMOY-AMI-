#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

idf.py -C "$project_dir" build
IDF_TARGET=esp32p4 idf.py -C "$project_dir/recovery" build

echo "Main image:     $project_dir/build/lvgl_transplant.bin"
echo "Recovery image: $project_dir/recovery/build/can_recovery.bin"
