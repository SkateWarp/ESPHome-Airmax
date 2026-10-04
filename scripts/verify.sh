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

assert_generated_line() {
  local generated_file="$1"
  local expected_line="$2"

  if ! grep -Fq "$expected_line" "$generated_file"; then
    echo "Missing generated line: $expected_line" >&2
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

g++ \
  -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -I "$project_dir/tests/host_stubs" \
  "$project_dir/components/tcl_climate/tcl_protocol.cpp" \
  "$project_dir/components/tcl_climate/tcl_climate.cpp" \
  "$project_dir/components/tcl_climate/switch/tcl_switch.cpp" \
  "$project_dir/tests/test_tcl_switch_lifecycle.cpp" \
  -o "$build_dir/test_tcl_switch_lifecycle"

"$build_dir/test_tcl_switch_lifecycle"

# ESPHome copies the root component files but omits the switch/ platform files
# when no TCL switch entities are configured. Check that shape separately.
mkdir "$build_dir/climate_only"
cp "$project_dir/components/tcl_climate/"*.h "$build_dir/climate_only/"
cp "$project_dir/components/tcl_climate/tcl_climate.cpp" "$build_dir/climate_only/"
g++ \
  -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsyntax-only \
  -I "$project_dir/tests/host_stubs" \
  "$build_dir/climate_only/tcl_climate.cpp"
echo "Climate-only component header regression passed."

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

  esp32c3_main="$project_dir/tests/.esphome/build/tcl-climate-esp32c3-test/src/main.cpp"
  assert_generated_line "$esp32c3_main" \
    "test_climate->set_health_ignore_appliance_state(true);"
  assert_generated_line "$esp32c3_main" \
    "test_climate->set_display_ignore_appliance_state(false);"
else
  echo "ESPHome is not installed; skipped schema validation and firmware compilation."
fi
