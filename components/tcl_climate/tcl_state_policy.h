#pragma once

#include <cstdint>

namespace esphome::tcl_climate {

inline constexpr bool tcl_switch_state_changed(
    const bool has_state, const bool current_state, const bool next_state) {
  return !has_state || current_state != next_state;
}

inline constexpr bool tcl_uart_state_can_replace_switch_intent(
    const uint32_t pending_fields, const uint32_t awaiting_command_fields,
    const uint32_t switch_field) {
  return ((pending_fields | awaiting_command_fields) & switch_field) == 0;
}

}  // namespace esphome::tcl_climate
