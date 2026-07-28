#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

g++ \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Werror \
  "$project_dir/components/tcl_climate/tcl_protocol.cpp" \
  "$project_dir/tests/test_tcl_protocol.cpp" \
  -o "$build_dir/test_tcl_protocol"

"$build_dir/test_tcl_protocol"

esphome_bin="${ESPHOME_BIN:-esphome}"
if command -v "$esphome_bin" >/dev/null 2>&1; then
  "$esphome_bin" config "$project_dir/tests/test_host.yaml"
  "$esphome_bin" config "$project_dir/tests/test_esp8266.yaml"
  "$esphome_bin" config "$project_dir/tests/test_esp32.yaml"
  "$esphome_bin" config "$project_dir/tests/test_esp32c3.yaml"
  "$esphome_bin" compile "$project_dir/tests/test_esp8266.yaml"
  "$esphome_bin" compile "$project_dir/tests/test_esp32.yaml"
  "$esphome_bin" compile "$project_dir/tests/test_esp32c3.yaml"
else
  echo "ESPHome is not installed; skipped schema validation and firmware compilation."
fi
