#!/usr/bin/env bash
set -euo pipefail

OUTPUT_DIR="client_files"
COUNT=""

usage() {
  echo "Usage: $0 --n COUNT [--dir OUTPUT_DIR]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --n)
      if [[ $# -lt 2 ]]; then
        usage
        exit 1
      fi
      COUNT="$2"
      shift 2
      ;;
    --dir)
      if [[ $# -lt 2 ]]; then
        usage
        exit 1
      fi
      OUTPUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$COUNT" || ! "$COUNT" =~ ^[0-9]+$ || "$COUNT" -lt 1 ]]; then
  echo "--n must be a positive integer" >&2
  usage
  exit 1
fi

mkdir -p "$OUTPUT_DIR"

if [[ "$(uname -s)" == "Darwin" ]]; then
  BLOCK_SIZE="1m"
else
  BLOCK_SIZE="1M"
fi

for ((i = 1; i <= COUNT; ++i)); do
  size_mb=$((105 + RANDOM % 101))
  name=$(printf "random_%03d_%03dmb.bin" "$i" "$size_mb")
  path="$OUTPUT_DIR/$name"

  dd if=/dev/urandom of="$path" bs="$BLOCK_SIZE" count="$size_mb"
  echo "Generated $path (${size_mb} MB)"
done

