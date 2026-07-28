#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "../components/tcl_climate/tcl_protocol.h"

using esphome::tcl_climate::TCL_STATUS_FRAME_61_SIZE;
using esphome::tcl_climate::TCL_STATUS_FRAME_65_SIZE;
using esphome::tcl_climate::TCL_STATUS_FRAME_68_SIZE;
using esphome::tcl_climate::TCL_STATUS_REQUEST;
using esphome::tcl_climate::TclControlFrame;
using esphome::tcl_climate::TclFrameParser;
using esphome::tcl_climate::TclFrameParserResult;
using esphome::tcl_climate::TclProtocolProfile;
using esphome::tcl_climate::TclProtocolState;
using esphome::tcl_climate::tcl_build_control_frame;
using esphome::tcl_climate::tcl_decode_status_frame;
using esphome::tcl_climate::tcl_supported_status_frame_size;
using esphome::tcl_climate::tcl_validate_status_frame;
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
  const std::array<uint8_t, 8> expected_request{
      0xBB, 0x00, 0x01, 0x04, 0x02, 0x01, 0x00, 0xBD,
  };
  require(TCL_STATUS_REQUEST == expected_request, "golden 8-byte status request");
  require(tcl_xor_checksum(TCL_STATUS_REQUEST.data(), TCL_STATUS_REQUEST.size() - 1) == 0xBD,
          "golden status-request checksum");
  require(tcl_xor_checksum(nullptr, 0) == 0, "empty checksum");

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
  require(tcl_decode_status_frame(status.data(), status.size(), state), "decode valid status");
  require(state.power && state.mode == 0x01, "power and cool mode");
  require(std::fabs(state.target_temperature - 22.0f) < 0.001f, "target temperature");
  require(state.fan == 0x02, "fan code");
  require(state.display && state.eco && state.turbo && state.health, "feature bits");
  require(state.horizontal_swing && state.vertical_swing, "swing bits");
  require(state.sleep && state.deep_sleep_bit && state.mute, "sleep/mute bits");
  require(std::fabs(state.current_temperature - 10.0f) < 0.001f,
          "floating-point temperature formula");
  require(std::fabs(state.compressor_current - 1.2f) < 0.001f, "compressor current");
  require(state.fan_speed == 118, "raw fan speed");
  require(state.pipe_out_temperature == 50 && state.pipe_in_temperature == 60,
          "raw pipe temperatures");
  require(state.compressor_state == 0x8A, "compressor state");
  require(state.fault == 0xAB && state.supply_voltage == 230 &&
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

  auto command_response = status;
  command_response[3] = 0x03;
  command_response.back() =
      tcl_xor_checksum(command_response.data(), command_response.size() - 1);
  require(!tcl_validate_status_frame(command_response.data(), command_response.size(), TCL_35),
          "default profile preserves 0x04-only status behavior");
  require(tcl_validate_status_frame(command_response.data(), command_response.size(), TCLAC_38),
          "tclac profile accepts command response 0x03");

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
