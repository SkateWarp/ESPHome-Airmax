#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>

#include "../components/tcl_climate/tcl_protocol.h"
#include "../components/tcl_climate/tcl_state_policy.h"

using esphome::tcl_climate::TCL_STATUS_FRAME_61_SIZE;
using esphome::tcl_climate::TCL_STATUS_FRAME_65_SIZE;
using esphome::tcl_climate::TCL_STATUS_FRAME_68_SIZE;
using esphome::tcl_climate::TCL_STATUS_REQUEST;
using esphome::tcl_climate::TclControlFrame;
using esphome::tcl_climate::TclFrameParser;
using esphome::tcl_climate::TclFrameParserResult;
using esphome::tcl_climate::TclProtocolProfile;
using esphome::tcl_climate::TclProtocolState;
using esphome::tcl_climate::TclStatusSignature;
using esphome::tcl_climate::TclStatusSignatureDetector;
using esphome::tcl_climate::tcl_build_control_frame;
using esphome::tcl_climate::tcl_capture_off_deferred_fields;
using esphome::tcl_climate::tcl_command_confirmation_fields;
using esphome::tcl_climate::tcl_decode_status_frame;
using esphome::tcl_climate::tcl_effective_off_epoch;
using esphome::tcl_climate::tcl_observable_command_fields;
using esphome::tcl_climate::tcl_extract_status_signature;
using esphome::tcl_climate::tcl_fan_speed_text;
using esphome::tcl_climate::tcl_format_fault_text;
using esphome::tcl_climate::tcl_format_profile_text;
using esphome::tcl_climate::tcl_normalize_supply_voltage;
using esphome::tcl_climate::tcl_phase_b_interrupted_by_power_off;
using esphome::tcl_climate::tcl_should_queue_deferred_fields_after_power_on;
using esphome::tcl_climate::tcl_should_suppress_internal_deferred_beep;
using esphome::tcl_climate::tcl_sticky_fields_for_power_off;
using esphome::tcl_climate::tcl_sticky_fields_for_power_on;
using esphome::tcl_climate::tcl_status_state_is_plausible;
using esphome::tcl_climate::tcl_supported_status_frame_size;
using esphome::tcl_climate::tcl_protocol_profile_name;
using esphome::tcl_climate::tcl_switch_state_changed;
using esphome::tcl_climate::tcl_tclac_phase_a_safe_fields;
using esphome::tcl_climate::tcl_uart_state_can_replace_switch_intent;
using esphome::tcl_climate::tcl_validate_status_frame;
using esphome::tcl_climate::tcl_visible_deferred_fields;
using esphome::tcl_climate::tcl_visible_intent_layers;
using esphome::tcl_climate::tcl_xor_checksum;

namespace {

constexpr TclProtocolProfile TCL_35 = TclProtocolProfile::PROFILE_TCL_35;
constexpr TclProtocolProfile ELECTRIQ_31 = TclProtocolProfile::PROFILE_ELECTRIQ_31;
constexpr TclProtocolProfile PIONEER_31 = TclProtocolProfile::PROFILE_PIONEER_31;
constexpr TclProtocolProfile TYJW2_35 = TclProtocolProfile::PROFILE_TYJW2_35;
constexpr TclProtocolProfile TCLAC_38 = TclProtocolProfile::PROFILE_TCLAC_38;

void require(const bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

template<size_t N> std::array<uint8_t, N> make_status_frame() {
  static_assert(N == TCL_STATUS_FRAME_61_SIZE || N == TCL_STATUS_FRAME_65_SIZE ||
                N == TCL_STATUS_FRAME_68_SIZE);
  std::array<uint8_t, N> frame{};
  frame[0] = 0xBB;
  frame[1] = 0x01;
  frame[2] = 0x00;
  frame[3] = 0x04;
  frame[4] = static_cast<uint8_t>(N - 6);
  frame[5] = 0x04;
  frame[6] = 0x00;

  frame[7] = 0xF1;   // Cool + power + display + eco + turbo
  frame[8] = 0x26;   // Medium fan, 22 °C
  frame[9] = 0x04;   // Health
  frame[10] = 0x60;  // Horizontal + vertical swing
  frame[17] = 0x49;
  frame[18] = 0x0C;  // 18700 -> 10.0 °C
  frame[19] = 0x81;  // Sleep and deep-sleep protocol bit
  frame[33] = 0x80;  // Mute
  frame[34] = 118;
  frame[35] = 50;
  frame[36] = 60;
  frame[39] = 12;
  frame[40] = 0x8A;
  frame[44] = 0xAB;
  frame[45] = 230;
  frame[46] = 7;
  frame[50] = 0x02;
  frame[51] = 3;
  frame[52] = 4;
  frame.back() = tcl_xor_checksum(frame.data(), frame.size() - 1);
  return frame;
}

TclProtocolState make_control_state() {
  TclProtocolState state{};
  state.power = true;
  state.mode = 0x01;
  state.target_temperature = 22.0f;
  state.fan = 0x02;
  state.display = true;
  state.eco = true;
  state.turbo = true;
  state.health = true;
  state.horizontal_swing = true;
  state.vertical_swing = true;
  state.sleep = true;
  state.mute = true;
  state.beep = true;
  state.vertical_vane_position = 3;
  state.horizontal_vane_position = 3;
  return state;
}

template<size_t N>
bool control_equals(const TclControlFrame &actual, const std::array<uint8_t, N> &expected) {
  return actual.size == N &&
         std::equal(expected.begin(), expected.end(), actual.bytes.begin());
}

}  // namespace

int main() {
  require(tcl_switch_state_changed(false, false, false),
          "the first published switch value is a real state");
  require(!tcl_switch_state_changed(true, true, true),
          "an identical switch heartbeat is deduplicated");
  require(tcl_switch_state_changed(true, true, false),
          "a changed switch value is persisted");

  constexpr uint32_t pending_power = 1U << 0;
  constexpr uint32_t pending_mode = 1U << 1;
  constexpr uint32_t pending_target = 1U << 2;
  constexpr uint32_t pending_fan = 1U << 3;
  constexpr uint32_t pending_display = 1U << 4;
  constexpr uint32_t pending_eco = 1U << 5;
  constexpr uint32_t pending_turbo = 1U << 6;
  constexpr uint32_t pending_health = 1U << 7;
  constexpr uint32_t pending_horizontal_swing = 1U << 8;
  constexpr uint32_t pending_vertical_swing = 1U << 9;
  constexpr uint32_t pending_sleep = 1U << 10;
  constexpr uint32_t pending_mute = 1U << 11;
  constexpr uint32_t pending_beep = 1U << 12;
  constexpr uint32_t off_deferable_fields =
      pending_target | pending_fan | pending_display | pending_eco |
      pending_turbo | pending_health | pending_horizontal_swing |
      pending_vertical_swing | pending_sleep | pending_mute;
  constexpr uint32_t phase_a_fields =
      pending_power | pending_mode | pending_beep;
  constexpr uint32_t sticky_switch_fields = pending_display | pending_health;
  require(tcl_uart_state_can_replace_switch_intent(
              0, 0, 0, pending_display, false),
          "idle UART status may refresh a switch");
  require(!tcl_uart_state_can_replace_switch_intent(
              pending_display, 0, 0, pending_display, false),
          "queued display intent blocks stale UART publication");
  require(!tcl_uart_state_can_replace_switch_intent(
              0, pending_health, 0, pending_health, false),
          "unconfirmed health intent blocks stale UART publication");
  require(tcl_uart_state_can_replace_switch_intent(
              pending_beep, 0, 0, pending_display, false),
          "an unrelated write-only beep intent does not block display status");
  require(!tcl_uart_state_can_replace_switch_intent(
              0, 0, 0, pending_health, true),
          "an authoritative local health policy ignores idle UART status");
  require(tcl_observable_command_fields(
              pending_power | pending_health | pending_beep,
              pending_beep, pending_health) == pending_power,
          "ignored appliance state is treated as local-only for confirmation");

  const uint32_t coalesced_requested =
      pending_power | pending_mode | pending_health;
  const uint32_t coalesced_deferred = tcl_capture_off_deferred_fields(
      true, coalesced_requested, 0, off_deferable_fields);
  require(coalesced_deferred == pending_health,
          "health intent is captured from confirmed OFF before a coalesced ON TX");
  require(tcl_command_confirmation_fields(
              true, coalesced_requested, phase_a_fields) ==
              (pending_power | pending_mode),
          "coalesced power-on phase A confirms only power and mode");
  require(tcl_capture_off_deferred_fields(
              false, pending_health, 0, off_deferable_fields) == 0,
          "a health change received while confirmed ON remains one-phase");

  const uint32_t all_explicit_options = off_deferable_fields | pending_beep;
  const uint32_t captured_explicit_options =
      tcl_capture_off_deferred_fields(true, all_explicit_options, 0,
                                      off_deferable_fields);
  require(captured_explicit_options == off_deferable_fields,
          "target, fan, preset, swing, display, and health selected while OFF defer");
  require((captured_explicit_options & pending_beep) == 0,
          "write-only beep policy is never deferred or confirmed");
  const uint32_t synthetic_legacy_off_reset =
      pending_fan | pending_eco | pending_turbo | pending_sleep;
  require(tcl_capture_off_deferred_fields(
              true, synthetic_legacy_off_reset, synthetic_legacy_off_reset,
              off_deferable_fields) == 0,
          "synthetic legacy OFF resets do not resurrect an old preset or fan");

  require(tcl_effective_off_epoch(false, 0, true, 0, true,
                                  pending_power),
          "a confirmed OFF state opens the preference-defer epoch");
  const bool pending_off_epoch = tcl_effective_off_epoch(
      true, pending_power | pending_target, false, 0, true, pending_power);
  require(pending_off_epoch,
          "a queued OFF opens the epoch before UART confirmation");
  require(tcl_capture_off_deferred_fields(
              pending_off_epoch, pending_target | pending_fan, 0,
              off_deferable_fields) == (pending_target | pending_fan),
          "target and fan selected in the queued-OFF race remain deferred");
  const bool awaiting_off_epoch = tcl_effective_off_epoch(
      true, pending_health, true, pending_power, false, pending_power);
  require(awaiting_off_epoch,
          "an in-flight OFF keeps the epoch open for a later switch request");
  require(tcl_capture_off_deferred_fields(
              awaiting_off_epoch, pending_health, 0,
              off_deferable_fields) == pending_health,
          "health selected while OFF confirmation is in flight remains deferred");
  require(!tcl_effective_off_epoch(true, pending_power, true, 0, true,
                                   pending_power),
          "an unsent OFF superseded by ON does not create a synthetic OFF epoch");

  const uint32_t sticky_after_power_on = tcl_sticky_fields_for_power_on(
      false, true, sticky_switch_fields);
  require(sticky_after_power_on == sticky_switch_fields,
          "display and health always get phase B on each confirmed OFF-to-ON edge");
  require(tcl_sticky_fields_for_power_on(true, true, sticky_switch_fields) == 0,
          "repeated ON status does not create another sticky phase B");
  const uint32_t sticky_after_power_off = tcl_sticky_fields_for_power_off(
      true, false, sticky_switch_fields);
  require(sticky_after_power_off == sticky_switch_fields,
          "falling power captures display and health before OFF feedback publishes");
  require(tcl_sticky_fields_for_power_off(false, false, sticky_switch_fields) == 0,
          "repeated OFF status does not create another sticky capture");
  const bool reported_off_display = false;
  const bool requested_sticky_display = true;
  const bool visible_display =
      (tcl_visible_deferred_fields(sticky_after_power_off,
                                   off_deferable_fields) & pending_display) != 0
          ? requested_sticky_display
          : reported_off_display;
  require(visible_display,
          "ON-to-OFF display=false feedback cannot erase a sticky ON intent");
  require(!tcl_uart_state_can_replace_switch_intent(
              0, 0, sticky_after_power_on, pending_health, false),
          "an OFF-state deferred preference blocks stale UART publication");
  require(!tcl_should_queue_deferred_fields_after_power_on(
              true, sticky_after_power_on, true, false),
          "phase B waits until power-on phase A is confirmed");
  require(tcl_should_queue_deferred_fields_after_power_on(
              true, sticky_after_power_on, false, false),
          "confirmed power-on queues sticky preferences as phase B");
  require(!tcl_should_queue_deferred_fields_after_power_on(
              false, sticky_after_power_on, false, false),
          "deferred preferences remain dormant while the appliance is OFF");
  require(tcl_command_confirmation_fields(
              false, sticky_after_power_on, phase_a_fields) ==
              sticky_after_power_on,
          "phase B confirms the deferred display and health fields themselves");
  const uint32_t phase_b_confirmation =
      tcl_command_confirmation_fields(false, sticky_after_power_on,
                                      phase_a_fields) |
      pending_power;
  require((phase_b_confirmation & pending_power) != 0,
          "phase B requires the appliance to remain powered ON");
  require(tcl_phase_b_interrupted_by_power_off(false,
                                               sticky_after_power_on),
          "an OFF report aborts phase B without confirming false features");
  require(!tcl_phase_b_interrupted_by_power_off(true,
                                                sticky_after_power_on),
          "an ON report may confirm phase-B preferences normally");
  require(tcl_should_suppress_internal_deferred_beep(
              sticky_after_power_on, sticky_after_power_on,
              sticky_after_power_on,
              pending_beep),
          "internal phase B suppresses a second audible beep");
  require(!tcl_should_suppress_internal_deferred_beep(
              0, coalesced_requested, coalesced_deferred,
              pending_beep),
          "phase A keeps the configured beep policy");
  require(!tcl_should_suppress_internal_deferred_beep(
              sticky_after_power_on, sticky_after_power_on | pending_beep,
              sticky_after_power_on, pending_beep),
          "an explicit beep request is not suppressed by phase B");
  require(!tcl_should_suppress_internal_deferred_beep(
              sticky_after_power_on, sticky_after_power_on | pending_power,
              sticky_after_power_on, pending_beep),
          "a command with any non-deferred field is not treated as internal phase B");

  constexpr uint32_t tclac_phase_a_safe_fields =
      pending_fan | pending_eco | pending_turbo | pending_horizontal_swing |
      pending_vertical_swing | pending_sleep | pending_mute;
  require(tcl_tclac_phase_a_safe_fields(
              true, true, tclac_phase_a_safe_fields) ==
              tclac_phase_a_safe_fields,
          "tclac phase A neutralizes stale fan, preset, and swing fields");
  require((tclac_phase_a_safe_fields & pending_target) == 0,
          "tclac phase A does not neutralize the selected target");
  const uint32_t tclac_no_child_phase_a_safe_fields =
      tclac_phase_a_safe_fields | pending_display | pending_health;
  require((tcl_tclac_phase_a_safe_fields(
               true, true, tclac_no_child_phase_a_safe_fields) &
           (pending_display | pending_health)) ==
              (pending_display | pending_health),
          "tclac phase A neutralizes stale display/health without child switches");
  require(tcl_tclac_phase_a_safe_fields(
              false, true, tclac_phase_a_safe_fields) == 0 &&
              tcl_tclac_phase_a_safe_fields(
                  true, false, tclac_phase_a_safe_fields) == 0,
          "safe-field neutralization is exclusive to tclac phase A");

  const auto stale_one_layers = tcl_visible_intent_layers(
      0, pending_target | pending_fan, 0, off_deferable_fields);
  require(stale_one_layers.awaiting_fields ==
              (pending_target | pending_fan),
          "the first stale UART report keeps in-flight target and fan visible");
  const auto stale_two_layers = tcl_visible_intent_layers(
      0, pending_target | pending_fan, pending_health,
      off_deferable_fields);
  require(stale_two_layers.awaiting_fields ==
              (pending_target | pending_fan) &&
              stale_two_layers.deferred_fields == pending_health,
          "the second stale report keeps awaiting and deferred intentions visible");
  const auto rejected_three_layers = tcl_visible_intent_layers(
      0, 0, 0, off_deferable_fields);
  require(rejected_three_layers.awaiting_fields == 0 &&
              rejected_three_layers.deferred_fields == 0 &&
              rejected_three_layers.pending_fields == 0,
          "after the third rejection no overlay hides the UART appliance state");
  const auto newest_pending_layers = tcl_visible_intent_layers(
      pending_target, pending_target, pending_target,
      off_deferable_fields);
  require(newest_pending_layers.pending_fields == pending_target,
          "a newer queued target is the last visible overlay layer");

  const std::array<uint8_t, 8> expected_request{
      0xBB, 0x00, 0x01, 0x04, 0x02, 0x01, 0x00, 0xBD,
  };
  require(TCL_STATUS_REQUEST == expected_request, "golden 8-byte status request");
  require(tcl_xor_checksum(TCL_STATUS_REQUEST.data(), TCL_STATUS_REQUEST.size() - 1) == 0xBD,
          "golden status-request checksum");
  require(tcl_xor_checksum(nullptr, 0) == 0, "empty checksum");

  require(std::strcmp(tcl_protocol_profile_name(TCL_35), "TCL 35 bytes") == 0 &&
              std::strcmp(tcl_protocol_profile_name(ELECTRIQ_31),
                          "ElectriQ 31 bytes") == 0 &&
              std::strcmp(tcl_protocol_profile_name(PIONEER_31),
                          "Pioneer 31 bytes") == 0 &&
              std::strcmp(tcl_protocol_profile_name(TYJW2_35),
                          "TYJW2 extended 35 bytes") == 0 &&
              std::strcmp(tcl_protocol_profile_name(TCLAC_38),
                          "tclac 38 bytes") == 0,
          "protocol profile names are English");
  require(std::strcmp(tcl_protocol_profile_name(
                          static_cast<TclProtocolProfile>(0xFF)),
                      "unknown") == 0,
          "unknown protocol profile is English");

  const std::array<uint8_t, 8> fan_speed_boundaries{0, 1, 85, 86, 98, 99, 117, 118};
  const std::array<const char *, 8> expected_fan_speed_text{
      "OFF", "LOW", "LOW", "MEDIUM", "MEDIUM", "HIGH", "HIGH", "TURBO"};
  for (size_t i = 0; i < fan_speed_boundaries.size(); i++)
    require(std::strcmp(tcl_fan_speed_text(fan_speed_boundaries[i]),
                        expected_fan_speed_text[i]) == 0,
            "fan-speed diagnostic text is English at every boundary");

  char fault_text[16]{};
  tcl_format_fault_text(0, fault_text, sizeof(fault_text));
  require(std::strcmp(fault_text, "NO FAULTS") == 0,
          "zero fault diagnostic is English");
  tcl_format_fault_text(0x01, fault_text, sizeof(fault_text));
  require(std::strcmp(fault_text, "FAULT 01") == 0,
          "fault diagnostic keeps a leading zero");
  tcl_format_fault_text(0xAB, fault_text, sizeof(fault_text));
  require(std::strcmp(fault_text, "FAULT AB") == 0,
          "fault diagnostic uses uppercase hexadecimal");
  tcl_format_fault_text(0xFF, fault_text, sizeof(fault_text));
  require(std::strcmp(fault_text, "FAULT FF") == 0,
          "maximum fault diagnostic is formatted safely");
  char tiny_fault_text[5]{};
  tcl_format_fault_text(0xAB, tiny_fault_text, sizeof(tiny_fault_text));
  require(std::strcmp(tiny_fault_text, "FAUL") == 0,
          "fault diagnostic is safely truncated");
  tcl_format_fault_text(0xAB, nullptr, 0);

  char profile_text[64]{};
  tcl_format_profile_text(TCL_35, 0, profile_text, sizeof(profile_text));
  require(std::strcmp(profile_text, "TX TCL 35 bytes / RX AUTO (waiting)") == 0,
          "auto response profile reports waiting in English");
  for (const size_t frame_size : {size_t{61}, size_t{65}, size_t{68}}) {
    tcl_format_profile_text(TCLAC_38, frame_size, profile_text, sizeof(profile_text));
    const std::string expected = "TX tclac 38 bytes / RX " + std::to_string(frame_size);
    require(std::strcmp(profile_text, expected.c_str()) == 0,
            "locked response profile reports its frame size");
  }
  char tiny_profile_text[4]{};
  tcl_format_profile_text(TCL_35, 0, tiny_profile_text, sizeof(tiny_profile_text));
  require(std::strcmp(tiny_profile_text, "TX ") == 0,
          "profile diagnostic is safely truncated");
  tcl_format_profile_text(TCL_35, 0, nullptr, 0);

  require(tcl_supported_status_frame_size(61) && tcl_supported_status_frame_size(65) &&
              tcl_supported_status_frame_size(68),
          "61/65/68-byte status lengths supported");
  require(!tcl_supported_status_frame_size(60) && !tcl_supported_status_frame_size(69),
          "unknown status lengths rejected");

  auto status = make_status_frame<TCL_STATUS_FRAME_61_SIZE>();
  require(tcl_validate_status_frame(status.data(), status.size()), "valid 61-byte status");
  require(!tcl_validate_status_frame(nullptr, status.size()), "null status rejected");
  require(!tcl_validate_status_frame(status.data(), status.size() - 1), "short status rejected");

  TclProtocolState state{};
  state.beep = true;
  require(tcl_decode_status_frame(status.data(), status.size(), state), "decode valid status");
  require(state.power && state.mode == 0x01, "power and cool mode");
  require(std::fabs(state.target_temperature - 22.0f) < 0.001f, "target temperature");
  require(state.fan == 0x02, "fan code");
  require(state.display && state.eco && state.turbo && state.health, "feature bits");
  require(!state.beep, "status decoder does not invent a write-only beep value");
  require(state.horizontal_swing && state.vertical_swing, "swing bits");
  require(state.sleep && state.deep_sleep_bit && state.mute, "sleep/mute bits");
  require(std::fabs(state.current_temperature - 10.0f) < 0.001f,
          "floating-point temperature formula");
  require(std::fabs(state.compressor_current - 1.2f) < 0.001f, "compressor current");
  require(state.fan_speed == 118, "raw fan speed");
  require(state.pipe_out_temperature == 50 && state.pipe_in_temperature == 60,
          "raw pipe temperatures");
  require(state.compressor_state == 0x8A, "compressor state");
  require(state.fault == 0xAB && state.supply_voltage_valid &&
              state.supply_voltage == 230 &&
              state.outside_motor == 7,
          "diagnostics");
  require(state.clean_filter && state.vertical_vane_position == 3 &&
              state.horizontal_vane_position == 4,
          "optional diagnostics");

  const auto status65 = make_status_frame<TCL_STATUS_FRAME_65_SIZE>();
  const auto status68 = make_status_frame<TCL_STATUS_FRAME_68_SIZE>();
  TclProtocolState extended_state{};
  require(tcl_decode_status_frame(status65.data(), status65.size(), extended_state),
          "decode 65-byte common prefix");
  require(tcl_decode_status_frame(status68.data(), status68.size(), extended_state),
          "decode 68-byte common prefix");
  require(extended_state.fan_speed == 118 && extended_state.fault == 0xAB,
          "extended frames keep common offsets");

  uint16_t normalized_voltage = 0;
  require(tcl_normalize_supply_voltage(230, normalized_voltage) &&
              normalized_voltage == 230,
          "ordinary 230 V sample remains direct");
  require(tcl_normalize_supply_voltage(90, normalized_voltage) &&
              normalized_voltage == 90,
          "direct voltage accepts the documented lower boundary");
  require(tcl_normalize_supply_voltage(7, normalized_voltage) &&
              normalized_voltage == 263,
          "one-byte voltage rollover maps 7 to 263 V");
  require(tcl_normalize_supply_voltage(29, normalized_voltage) &&
              normalized_voltage == 285,
          "wrapped voltage accepts the documented upper boundary");
  require(!tcl_normalize_supply_voltage(30, normalized_voltage),
          "wrapped voltage above the documented boundary is rejected");
  require(!tcl_normalize_supply_voltage(50, normalized_voltage),
          "raw and wrapped values outside the residential range are rejected");
  require(!tcl_normalize_supply_voltage(0, normalized_voltage),
          "zero without history is rejected as an ambiguous sentinel");
  const uint16_t previous_high_voltage = 230;
  require(tcl_normalize_supply_voltage(0, normalized_voltage,
                                       &previous_high_voltage) &&
              normalized_voltage == 256,
          "zero wraps to 256 only with close high-voltage continuity");
  const uint16_t previous_low_voltage = 120;
  require(!tcl_normalize_supply_voltage(0, normalized_voltage,
                                        &previous_low_voltage),
          "zero does not jump a low-voltage installation to 256 V");

  auto wrapped_voltage = status;
  wrapped_voltage[45] = 7;
  wrapped_voltage.back() =
      tcl_xor_checksum(wrapped_voltage.data(), wrapped_voltage.size() - 1);
  require(tcl_decode_status_frame(wrapped_voltage.data(), wrapped_voltage.size(), state) &&
              state.supply_voltage_valid && state.supply_voltage == 263,
          "decoder carries a 230 to 263 V rollover transition");

  auto invalid_voltage = status;
  invalid_voltage[45] = 50;
  invalid_voltage.back() =
      tcl_xor_checksum(invalid_voltage.data(), invalid_voltage.size() - 1);
  require(tcl_decode_status_frame(invalid_voltage.data(), invalid_voltage.size(), state) &&
              state.supply_voltage_valid && state.supply_voltage == 263,
          "invalid voltage sample preserves the last accepted reading");

  TclProtocolState fresh_invalid_voltage{};
  require(tcl_decode_status_frame(invalid_voltage.data(), invalid_voltage.size(),
                                  fresh_invalid_voltage) &&
              !fresh_invalid_voltage.supply_voltage_valid &&
              fresh_invalid_voltage.supply_voltage == 0,
          "invalid first voltage sample is not invented or published");

  require(tcl_decode_status_frame(status.data(), status.size(), state) &&
              state.supply_voltage_valid && state.supply_voltage == 230,
          "valid voltage resumes after a rejected transition");

  auto command_response = status;
  command_response[3] = 0x03;
  command_response.back() =
      tcl_xor_checksum(command_response.data(), command_response.size() - 1);
  require(!tcl_validate_status_frame(command_response.data(), command_response.size(), TCL_35),
          "default profile preserves 0x04-only status behavior");
  require(!tcl_validate_status_frame(command_response.data(), command_response.size(), ELECTRIQ_31) &&
              !tcl_validate_status_frame(command_response.data(), command_response.size(), PIONEER_31),
          "31-byte profiles do not decode command responses as heartbeat status");
  require(tcl_validate_status_frame(command_response.data(), command_response.size(),
                                    ELECTRIQ_31, true),
          "an explicit command-response check remains available");
  require(tcl_validate_status_frame(command_response.data(), command_response.size(), TYJW2_35),
          "TYJW2 captures use type 0x03 for status");
  require(tcl_validate_status_frame(command_response.data(), command_response.size(), TCLAC_38),
          "tclac profile accepts command response 0x03");

  auto alternate_envelope = status;
  alternate_envelope[1] = 0x12;
  alternate_envelope[2] = 0x34;
  alternate_envelope[5] = 0x03;
  alternate_envelope[6] = 0x56;
  alternate_envelope.back() =
      tcl_xor_checksum(alternate_envelope.data(), alternate_envelope.size() - 1);
  require(!tcl_validate_status_frame(alternate_envelope.data(), alternate_envelope.size()),
          "alternate envelope is not accepted without startup learning");
  TclStatusSignature alternate_signature{};
  require(tcl_extract_status_signature(alternate_envelope.data(), alternate_envelope.size(),
                                       alternate_signature, TCL_35),
          "checksum-valid heartbeat response yields an alternate signature");
  require(alternate_signature.frame_size == 61 && alternate_signature.byte_1 == 0x12 &&
              alternate_signature.byte_2 == 0x34 && alternate_signature.byte_5 == 0x03 &&
              alternate_signature.byte_6 == 0x56,
          "all model-dependent envelope bytes are learned");
  require(tcl_validate_status_frame(alternate_envelope.data(), alternate_envelope.size(),
                                    TCL_35, false, &alternate_signature),
          "alternate envelope validates only against its learned signature");
  TclProtocolState alternate_state{};
  require(tcl_decode_status_frame(alternate_envelope.data(), alternate_envelope.size(),
                                  alternate_state, TCL_35, false, &alternate_signature),
          "alternate envelope decodes with its learned signature");

  TclStatusSignatureDetector signature_detector;
  require(!signature_detector.observe(alternate_signature) && !signature_detector.locked(),
          "one heartbeat response does not lock a signature");
  auto alternate_payload = alternate_envelope;
  alternate_payload[7] ^= 0x10;
  alternate_payload.back() =
      tcl_xor_checksum(alternate_payload.data(), alternate_payload.size() - 1);
  TclStatusSignature alternate_payload_signature{};
  require(tcl_extract_status_signature(alternate_payload.data(), alternate_payload.size(),
                                       alternate_payload_signature, TCL_35),
          "second heartbeat payload has a valid signature");
  require(signature_detector.observe(alternate_payload_signature) &&
              signature_detector.locked() &&
              signature_detector.matches(alternate_payload.data(), alternate_payload.size()),
          "two different payloads with the same signature lock detection");
  require(!signature_detector.matches(status.data(), status.size()),
          "a different signature is rejected after locking");

  auto alternate65 = status65;
  alternate65[5] = 0x03;
  alternate65.back() = tcl_xor_checksum(alternate65.data(), alternate65.size() - 1);
  TclStatusSignature alternate65_signature{};
  require(tcl_extract_status_signature(alternate65.data(), alternate65.size(),
                                       alternate65_signature, TCL_35) &&
              alternate65_signature.frame_size == 65,
          "65-byte alternate signature learns");
  auto alternate68 = status68;
  alternate68[5] = 0x03;
  alternate68.back() = tcl_xor_checksum(alternate68.data(), alternate68.size() - 1);
  TclStatusSignature alternate68_signature{};
  require(tcl_extract_status_signature(alternate68.data(), alternate68.size(),
                                       alternate68_signature, TCL_35) &&
              alternate68_signature.frame_size == 68,
          "68-byte alternate signature learns");

  TclStatusSignatureDetector changing_detector;
  require(!changing_detector.observe(alternate_signature),
          "first alternate signature remains a candidate");
  TclStatusSignature canonical_signature{};
  require(tcl_extract_status_signature(status.data(), status.size(), canonical_signature, TCL_35),
          "canonical signature extracts");
  require(!changing_detector.observe(canonical_signature) &&
              changing_detector.candidate_count() == 1,
          "a changed pre-lock signature restarts confirmation");
  require(changing_detector.observe(canonical_signature) && changing_detector.locked(),
          "the replacement signature locks after its own second confirmation");

  auto learning_command_response = alternate_envelope;
  learning_command_response[3] = 0x03;
  learning_command_response.back() =
      tcl_xor_checksum(learning_command_response.data(), learning_command_response.size() - 1);
  TclStatusSignature ignored_learning_signature{};
  require(!tcl_extract_status_signature(learning_command_response.data(),
                                        learning_command_response.size(),
                                        ignored_learning_signature, TCL_35),
          "TCL 35 command response cannot train heartbeat detection");
  require(!tcl_extract_status_signature(learning_command_response.data(),
                                        learning_command_response.size(),
                                        ignored_learning_signature, ELECTRIQ_31),
          "ElectriQ command response cannot train heartbeat detection");
  require(tcl_extract_status_signature(learning_command_response.data(),
                                       learning_command_response.size(),
                                       ignored_learning_signature, TYJW2_35),
          "TYJW2 type 0x03 status can train its explicit profile");

  TclProtocolState plausible_state{};
  require(tcl_decode_status_frame(status.data(), status.size(), plausible_state),
          "plausibility fixture decodes");
  require(tcl_status_state_is_plausible(plausible_state, TCL_35),
          "known mode and fan are plausible");
  plausible_state.mode = 0x06;
  require(!tcl_status_state_is_plausible(plausible_state, TCL_35),
          "unknown powered mode cannot unlock control");
  plausible_state.mode = 0x01;
  plausible_state.fan = 0x05;
  require(!tcl_status_state_is_plausible(plausible_state, TCL_35) &&
              tcl_status_state_is_plausible(plausible_state, TCLAC_38),
          "fan plausibility follows the selected TX profile");
  plausible_state.power = false;
  plausible_state.mode = 0x00;
  require(tcl_status_state_is_plausible(plausible_state, TCL_35),
          "reserved fan state while OFF cannot prevent signature detection");

  auto precise_temperature = status;
  precise_temperature[17] = 0x4A;
  precise_temperature[18] = 0x38;  // 19000, deliberately not divisible by 374
  precise_temperature.back() =
      tcl_xor_checksum(precise_temperature.data(), precise_temperature.size() - 1);
  TclProtocolState precise_state{};
  require(tcl_decode_status_frame(precise_temperature.data(), precise_temperature.size(),
                                  precise_state),
          "decode non-divisible raw temperature");
  const float expected_temperature = ((19000.0f / 374.0f) - 32.0f) / 1.8f;
  require(std::fabs(precise_state.current_temperature - expected_temperature) < 0.0001f,
          "temperature conversion keeps fractional precision");

  auto half_degree_status = status;
  half_degree_status[9] |= 0x09;
  half_degree_status.back() =
      tcl_xor_checksum(half_degree_status.data(), half_degree_status.size() - 1);
  require(tcl_decode_status_frame(half_degree_status.data(), half_degree_status.size(),
                                  extended_state, TYJW2_35),
          "TYJW2 half-degree status");
  require(std::fabs(extended_state.target_temperature - 22.5f) < 0.001f,
          "TYJW2 half-degree decode");
  require(extended_state.anti_mildew, "TYJW2 anti-mildew state preserved");

  state = make_control_state();
  TclControlFrame control{};
  require(tcl_build_control_frame(state, TCL_35, control), "build proven TCL 35 frame");
  const std::array<uint8_t, 35> expected_control{
      0xBB, 0x00, 0x01, 0x03, 0x1D, 0x00, 0x00, 0xE4, 0xD3, 0xF9, 0x3B, 0x00,
      0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58,
  };
  require(control_equals(control, expected_control), "golden 35-byte user control frame");
  require((control.bytes[7] & 0x40) != 0,
          "TCL 35 control frame carries the display preference");
  require((control.bytes[7] & 0x20) != 0,
          "TCL 35 control frame carries the write-only beep policy");
  require((control.bytes[8] & 0x10) != 0,
          "TCL 35 control frame carries the health preference");

  const std::array<uint8_t, 5> status_modes{0x01, 0x02, 0x03, 0x04, 0x05};
  const std::array<uint8_t, 5> control_modes{0x03, 0x07, 0x02, 0x01, 0x08};
  for (size_t i = 0; i < status_modes.size(); i++) {
    state = make_control_state();
    state.mode = status_modes[i];
    require(tcl_build_control_frame(state, TCL_35, control), "known mode maps");
    require((control.bytes[8] & 0x0F) == control_modes[i], "mode mapping");
  }

  const std::array<uint8_t, 4> status_fans{0x00, 0x01, 0x02, 0x03};
  const std::array<uint8_t, 4> control_fans{0x00, 0x02, 0x03, 0x05};
  for (size_t i = 0; i < status_fans.size(); i++) {
    state = make_control_state();
    state.fan = status_fans[i];
    require(tcl_build_control_frame(state, TCL_35, control), "known fan maps");
    require((control.bytes[10] & 0x07) == control_fans[i], "original fan mapping");
  }

  state = make_control_state();
  state.fan = 0x07;
  require(!tcl_build_control_frame(state, TCL_35, control), "unknown fan fails closed");
  state = make_control_state();
  state.mode = 0x00;
  require(!tcl_build_control_frame(state, TCL_35, control), "unknown mode fails closed");

  state = make_control_state();
  state.power = false;
  state.display = false;
  state.eco = false;
  state.turbo = false;
  state.health = false;
  state.horizontal_swing = false;
  state.vertical_swing = false;
  state.sleep = false;
  state.mute = false;
  state.beep = false;
  require(tcl_build_control_frame(state, TCL_35, control), "build all-flags-off frame");
  require((control.bytes[7] & 0xFC) == 0, "power/timers/beep/display/eco cleared");
  require((control.bytes[8] & 0xD0) == 0, "health/turbo/mute cleared");
  require((control.bytes[10] & 0x38) == 0 && (control.bytes[14] & 0x28) == 0 &&
              (control.bytes[19] & 0x01) == 0,
          "swing/half-degree/sleep cleared");

  state.target_temperature = 0.0f;
  require(tcl_build_control_frame(state, TCL_35, control), "low target clamps");
  require((control.bytes[9] & 0x0F) == 0x0F, "target clamps to 16 C");
  state.target_temperature = 255.0f;
  require(tcl_build_control_frame(state, TCL_35, control), "high target clamps");
  require((control.bytes[9] & 0x0F) == 0x00, "target clamps to 31 C");

  TclProtocolState electriq{};
  electriq.power = true;
  electriq.mode = 0x05;
  electriq.target_temperature = 16.0f;
  electriq.fan = 0x00;
  electriq.beep = true;
  require(tcl_build_control_frame(electriq, ELECTRIQ_31, control),
          "build ElectriQ 31-byte frame");
  const std::array<uint8_t, 31> expected_electriq{
      0xBB, 0x00, 0x01, 0x03, 0x19, 0x01, 0x00, 0x24, 0x08, 0x0F, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82,
  };
  require(control_equals(control, expected_electriq), "golden ElectriQ on frame");

  electriq.horizontal_swing = true;
  require(tcl_build_control_frame(electriq, ELECTRIQ_31, control), "ElectriQ H swing");
  require(control.bytes[14] == 0x08, "ElectriQ single-bit H swing");
  require(tcl_build_control_frame(electriq, PIONEER_31, control), "Pioneer H swing");
  require(control.bytes[14] == 0x38, "Pioneer three-bit H swing");

  TclProtocolState tyjw2{};
  tyjw2.mode = 0x01;
  tyjw2.target_temperature = 21.0f;
  tyjw2.fan = 0x00;
  tyjw2.beep = true;
  tyjw2.display = true;
  tyjw2.vertical_vane_position = 3;
  tyjw2.horizontal_vane_position = 3;
  require(tcl_build_control_frame(tyjw2, TYJW2_35, control), "build TYJW2 safe frame");
  const std::array<uint8_t, 35> expected_tyjw2{
      0xBB, 0x00, 0x01, 0x03, 0x1D, 0x00, 0x00, 0x60, 0x03, 0x5A, 0x00, 0x00,
      0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x83, 0x9D,
  };
  require(control_equals(control, expected_tyjw2), "golden TYJW2 safe frame");
  tyjw2.target_temperature = 22.5f;
  tyjw2.anti_mildew = true;
  require(tcl_build_control_frame(tyjw2, TYJW2_35, control), "TYJW2 half-degree command");
  require((control.bytes[11] & 0x04) != 0, "TYJW2 half-degree TX bit");
  require((control.bytes[8] & 0x20) != 0, "TYJW2 anti-mildew TX bit");
  tyjw2.vertical_swing = true;
  tyjw2.horizontal_swing = true;
  tyjw2.vertical_vane_position = 0x10;
  tyjw2.horizontal_vane_position = 0x20;
  require(tcl_build_control_frame(tyjw2, TYJW2_35, control),
          "TYJW2 captured vane directions");
  require((control.bytes[32] & 0x1F) == 0x10 &&
              (control.bytes[33] & 0x3F) == 0x20,
          "TYJW2 vane direction codes are not normalized");
  tyjw2.vertical_vane_position = 3;
  tyjw2.horizontal_vane_position = 0x0D;
  require(tcl_build_control_frame(tyjw2, TYJW2_35, control),
          "TYJW2 swing from fixed/unknown positions");
  require((control.bytes[32] & 0x1F) == 0x08 &&
              (control.bytes[33] & 0x3F) == 0x08,
          "TYJW2 uses a documented default swing direction");

  TclProtocolState tclac{};
  tclac.power = true;
  tclac.mode = 0x01;
  tclac.target_temperature = 22.0f;
  tclac.fan = 0x02;
  tclac.beep = true;
  tclac.display = true;
  tclac.vertical_vane_position = 3;
  tclac.horizontal_vane_position = 3;
  require(tcl_build_control_frame(tclac, TCLAC_38, control), "build tclac 38-byte frame");
  const std::array<uint8_t, 38> expected_tclac{
      0xBB, 0x00, 0x01, 0x03, 0x20, 0x03, 0x01, 0x64, 0x03, 0x09,
      0x03, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0xF7,
  };
  require(control_equals(control, expected_tclac), "golden tclac 38-byte frame");

  TclProtocolState tclac_boost = tclac;
  tclac_boost.fan = 0x00;
  tclac_boost.turbo = true;
  tclac_boost.mute = false;
  require(tcl_build_control_frame(tclac_boost, TCLAC_38, control),
          "build tclac Boost frame");
  require((control.bytes[8] & 0x40) != 0,
          "tclac Boost keeps the proven Turbo command bit");
  require((control.bytes[8] & 0x80) == 0,
          "tclac Boost clears the unrelated Mute command bit");
  require((control.bytes[10] & 0x07) == 0,
          "tclac Boost keeps the underlying automatic fan command");

  TclProtocolState tclac_phase_a{};
  tclac_phase_a.power = true;
  tclac_phase_a.mode = 0x01;
  tclac_phase_a.target_temperature = 24.0f;
  tclac_phase_a.display = true;
  tclac_phase_a.health = true;
  tclac_phase_a.beep = true;
  require(tcl_build_control_frame(tclac_phase_a, TCLAC_38, control),
          "build safe tclac power-on phase A");
  require((control.bytes[7] & 0x64) == 0x64 &&
              (control.bytes[8] & 0x10) != 0,
          "tclac phase A may carry sticky child policies and one audible beep");
  require((control.bytes[7] & 0x80) == 0 &&
              (control.bytes[8] & 0xC0) == 0 &&
              (control.bytes[10] & 0x3F) == 0 &&
              (control.bytes[11] & 0x08) == 0 &&
              (control.bytes[19] & 0x01) == 0,
          "tclac phase A does not revive stale fan, preset, or swing features");

  TclProtocolState tclac_phase_b = tclac_phase_a;
  tclac_phase_b.beep = false;
  tclac_phase_b.fan = 0x02;
  tclac_phase_b.sleep = true;
  tclac_phase_b.vertical_swing = true;
  tclac_phase_b.horizontal_swing = true;
  require(tcl_build_control_frame(tclac_phase_b, TCLAC_38, control),
          "build tclac preference phase B");
  require((control.bytes[7] & 0x20) == 0 &&
              (control.bytes[10] & 0x3F) != 0 &&
              (control.bytes[11] & 0x08) != 0 &&
              (control.bytes[19] & 0x01) != 0,
          "tclac phase B reapplies deferred options without a second beep");

  const std::array<uint8_t, 6> tclac_status_fans{0, 1, 2, 3, 4, 5};
  const std::array<uint8_t, 6> tclac_control_fans{0, 1, 3, 5, 6, 7};
  for (size_t i = 0; i < tclac_status_fans.size(); i++) {
    tclac.fan = tclac_status_fans[i];
    require(tcl_build_control_frame(tclac, TCLAC_38, control), "known tclac fan maps");
    require((control.bytes[10] & 0x07) == tclac_control_fans[i],
            "tclac fan mapping");
  }
  tclac.power = false;
  tclac.mode = 0;
  tclac.display = true;
  require(tcl_build_control_frame(tclac, TCLAC_38, control), "tclac explicit OFF frame");
  require((control.bytes[7] & 0x44) == 0 && (control.bytes[8] & 0x0F) == 0,
          "tclac OFF clears power, display, and mode");
  require(control.bytes[32] == 0x08 && control.bytes[33] == 0x08,
          "tclac safe default vane directions");

  auto corrupted = status;
  corrupted[17] ^= 0x01;
  require(!tcl_validate_status_frame(corrupted.data(), corrupted.size()),
          "bad checksum rejected");
  require(!tcl_decode_status_frame(corrupted.data(), corrupted.size(), state),
          "decoder rejects bad checksum");

  auto wrong_header = status;
  wrong_header[0] = 0xBA;
  wrong_header.back() = tcl_xor_checksum(wrong_header.data(), wrong_header.size() - 1);
  require(!tcl_validate_status_frame(wrong_header.data(), wrong_header.size()),
          "wrong header rejected");
  auto wrong_direction = status;
  wrong_direction[1] = 0x00;
  wrong_direction.back() =
      tcl_xor_checksum(wrong_direction.data(), wrong_direction.size() - 1);
  require(!tcl_validate_status_frame(wrong_direction.data(), wrong_direction.size()),
          "wrong direction rejected");
  auto wrong_discriminator = status;
  wrong_discriminator[5] = 0x01;
  wrong_discriminator.back() =
      tcl_xor_checksum(wrong_discriminator.data(), wrong_discriminator.size() - 1);
  require(!tcl_validate_status_frame(wrong_discriminator.data(), wrong_discriminator.size()),
          "wrong discriminator rejected");
  auto wrong_length = status;
  wrong_length[4] = 0x36;
  wrong_length.back() = tcl_xor_checksum(wrong_length.data(), wrong_length.size() - 1);
  require(!tcl_validate_status_frame(wrong_length.data(), wrong_length.size()),
          "wrong declared length rejected");

  TclFrameParser parser;
  require(parser.feed(0x00) == TclFrameParserResult::NONE, "noise ignored before header");
  for (size_t i = 0; i < status68.size(); i++) {
    const auto result = parser.feed(status68[i]);
    if (i + 1 == status68.size())
      require(result == TclFrameParserResult::FRAME_READY, "68-byte frame completion");
    else
      require(result == TclFrameParserResult::NONE, "no early frame completion");
  }
  require(parser.size() == TCL_STATUS_FRAME_68_SIZE, "parser variable frame length");
  parser.reset();

  for (size_t i = 0; i < TCL_STATUS_REQUEST.size(); i++) {
    const auto result = parser.feed(TCL_STATUS_REQUEST[i]);
    if (i + 1 == TCL_STATUS_REQUEST.size())
      require(result == TclFrameParserResult::FRAME_READY, "parser accepts 8-byte frame");
    else
      require(result == TclFrameParserResult::NONE, "no early 8-byte frame completion");
  }
  parser.reset();

  require(parser.feed(0xBB) == TclFrameParserResult::NONE, "invalid-length header");
  require(parser.feed(0x00) == TclFrameParserResult::NONE, "invalid-length byte 1");
  require(parser.feed(0x00) == TclFrameParserResult::NONE, "invalid-length byte 2");
  require(parser.feed(0x04) == TclFrameParserResult::NONE, "invalid-length command");
  require(parser.feed(0xFF) == TclFrameParserResult::INVALID_LENGTH,
          "oversized declared length rejected");

  std::cout << "All TCL protocol tests passed\n";
  return 0;
}
