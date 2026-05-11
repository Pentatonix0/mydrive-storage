#!/usr/bin/env bash
set -euo pipefail

OUTPUT_DIR="${1:-client_files}"

mkdir -p "$OUTPUT_DIR"

path="$OUTPUT_DIR/file_1500mb.bin"

if command -v mkfile >/dev/null 2>&1; then
  mkfile 1500m "$path"
else
  dd if=/dev/urandom of="$path" bs=1M count=1500 status=progress
fi

echo "Generated $path (1500 MB)"

