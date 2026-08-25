#pragma once

#include <cstdint>

namespace esphome::tcl_climate {

inline constexpr bool tcl_switch_state_changed(
    const bool has_state, const bool current_state, const bool next_state) {
  return !has_state || current_state != next_state;
}

inline constexpr bool tcl_uart_state_can_replace_switch_intent(
    const uint32_t pending_fields, const uint32_t awaiting_command_fields,
    const uint32_t deferred_fields, const uint32_t switch_field) {
  return ((pending_fields | awaiting_command_fields | deferred_fields) &
          switch_field) == 0;
}

inline constexpr uint32_t tcl_visible_deferred_fields(
    const uint32_t deferred_fields, const uint32_t visible_option_fields) {
  return deferred_fields & visible_option_fields;
}

struct TclVisibleIntentLayers {
  uint32_t awaiting_fields{0};
  uint32_t deferred_fields{0};
  uint32_t pending_fields{0};
};

inline constexpr TclVisibleIntentLayers tcl_visible_intent_layers(
    const uint32_t pending_fields, const uint32_t awaiting_fields,
    const uint32_t deferred_fields, const uint32_t visible_option_fields) {
  return {
      awaiting_fields & visible_option_fields,
      deferred_fields & visible_option_fields,
      pending_fields & visible_option_fields,
  };
}

inline constexpr uint32_t tcl_capture_off_deferred_fields(
    const bool off_epoch_active, const uint32_t requested_fields,
    const uint32_t synthetic_off_reset_fields,
    const uint32_t deferable_fields) {
  if (!off_epoch_active)
    return 0;
  return requested_fields & ~synthetic_off_reset_fields & deferable_fields;
}

inline constexpr bool tcl_effective_off_epoch(
    const bool last_confirmed_power, const uint32_t pending_fields,
    const bool requested_power, const uint32_t awaiting_fields,
    const bool awaiting_power, const uint32_t power_field) {
  if (!last_confirmed_power)
    return true;
  if ((pending_fields & power_field) != 0 && !requested_power)
    return true;
  return (awaiting_fields & power_field) != 0 && !awaiting_power;
}

inline constexpr uint32_t tcl_sticky_fields_for_power_on(
    const bool previous_confirmed_power, const bool reported_power,
    const uint32_t configured_sticky_fields) {
  return !previous_confirmed_power && reported_power
             ? configured_sticky_fields
             : 0;
}

inline constexpr uint32_t tcl_sticky_fields_for_power_off(
    const bool previous_confirmed_power, const bool reported_power,
    const uint32_t configured_sticky_fields) {
  return previous_confirmed_power && !reported_power
             ? configured_sticky_fields
             : 0;
}

inline constexpr uint32_t tcl_command_confirmation_fields(
    const bool power_on_phase_a, const uint32_t requested_fields,
    const uint32_t phase_a_fields) {
  if (power_on_phase_a)
    return requested_fields & phase_a_fields;
  return requested_fields;
}

inline constexpr bool tcl_phase_b_interrupted_by_power_off(
    const bool reported_power, const uint32_t awaiting_deferred_fields) {
  return !reported_power && awaiting_deferred_fields != 0;
}

inline constexpr bool tcl_should_queue_deferred_fields_after_power_on(
    const bool reported_power, const uint32_t deferred_fields,
    const bool awaiting_command_status, const bool command_rejected) {
  return reported_power && deferred_fields != 0 &&
         !awaiting_command_status && !command_rejected;
}

inline constexpr bool tcl_should_suppress_internal_deferred_beep(
    const uint32_t phase_b_fields, const uint32_t requested_fields,
    const uint32_t deferred_fields, const uint32_t beep_field) {
  const uint32_t non_beep_fields = requested_fields & ~beep_field;
  return phase_b_fields != 0 && non_beep_fields != 0 &&
         (non_beep_fields & ~deferred_fields) == 0 &&
         (requested_fields & beep_field) == 0;
}

inline constexpr uint32_t tcl_tclac_phase_a_safe_fields(
    const bool tclac_profile, const bool power_on_phase_a,
    const uint32_t safe_fields) {
  return tclac_profile && power_on_phase_a ? safe_fields : 0;
}

}  // namespace esphome::tcl_climate
