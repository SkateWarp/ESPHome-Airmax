#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

assert_restore_mode() {
  local generated_file="$1"
  local switch_id="$2"
  local expected_mode="$3"
  local expected_line="${switch_id}->set_restore_mode(switch_::${expected_mode});"

  if ! grep -Fq "$expected_line" "$generated_file"; then
    echo "Missing generated restore mode: $expected_line" >&2
    return 1
  fi
}

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

  esp32_main="$project_dir/tests/.esphome/build/tcl-climate-esp32-test/src/main.cpp"
  assert_restore_mode "$esp32_main" test_default_display SWITCH_RESTORE_DEFAULT_ON
  assert_restore_mode "$esp32_main" test_default_beep SWITCH_RESTORE_DEFAULT_OFF
  assert_restore_mode "$esp32_main" test_default_health SWITCH_RESTORE_DEFAULT_ON
else
  echo "ESPHome is not installed; skipped schema validation and firmware compilation."
fi
