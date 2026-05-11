#!/usr/bin/env bash
set -euo pipefail

OUTPUT_DIR="${1:-client_files}"

mkdir -p "$OUTPUT_DIR"

generate_file() {
  local name="$1"
  local size_mb="$2"
  local path="$OUTPUT_DIR/$name"

  if command -v mkfile >/dev/null 2>&1; then
    mkfile "${size_mb}m" "$path"
  else
    dd if=/dev/urandom of="$path" bs=1M count="$size_mb" status=progress
  fi

  echo "Generated $path (${size_mb} MB)"
}

generate_file "file_50mb.bin" 50
generate_file "file_150mb.bin" 150
generate_file "file_200mb.bin" 200

