#include "tcl_protocol.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace esphome::tcl_climate {

namespace {

constexpr uint8_t TCL_HEADER = 0xBB;
constexpr uint8_t TCL_STATUS_COMMAND = 0x04;
constexpr uint8_t TCL_COMMAND_RESPONSE = 0x03;

constexpr std::array<uint8_t, TCL_CONTROL_FRAME_MAX_SIZE> TCL_CONTROL_TEMPLATE_35{
    0xBB, 0x00, 0x01, 0x03, 0x1D, 0x00, 0x00, 0x64, 0x03, 0xF3, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// 31-byte family documented by ElectriQ and Pioneer implementations.
constexpr std::array<uint8_t, TCL_CONTROL_FRAME_MAX_SIZE> TCL_CONTROL_TEMPLATE_31{
    0xBB, 0x00, 0x01, 0x03, 0x19, 0x01, 0x00,
};

// Capture-derived TYJW2/ROVSUN extended layout. Extra features remain profile-specific.
constexpr std::array<uint8_t, TCL_CONTROL_FRAME_MAX_SIZE> TCL_CONTROL_TEMPLATE_TYJW2{
    0xBB, 0x00, 0x01, 0x03, 0x1D, 0x00, 0x00, 0x60, 0x03, 0x5A, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x83, 0x9D,
};

// 38-byte command layout used by I-am-nightingale/tclac.
constexpr std::array<uint8_t, TCL_CONTROL_FRAME_MAX_SIZE> TCL_CONTROL_TEMPLATE_TCLAC{
    0xBB, 0x00, 0x01, 0x03, 0x20, 0x03, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

bool profile_is_31_byte(const TclProtocolProfile profile) {
  return profile == TclProtocolProfile::PROFILE_ELECTRIQ_31 ||
         profile == TclProtocolProfile::PROFILE_PIONEER_31;
}

bool profile_is_tyjw2(const TclProtocolProfile profile) {
  return profile == TclProtocolProfile::PROFILE_TYJW2_35;
}

bool profile_is_tclac(const TclProtocolProfile profile) {
  return profile == TclProtocolProfile::PROFILE_TCLAC_38;
}

size_t profile_control_size(const TclProtocolProfile profile) {
  if (profile_is_tclac(profile))
    return TCL_CONTROL_FRAME_38_SIZE;
  return profile_is_31_byte(profile) ? TCL_CONTROL_FRAME_31_SIZE : TCL_CONTROL_FRAME_35_SIZE;
}

const std::array<uint8_t, TCL_CONTROL_FRAME_MAX_SIZE> &profile_control_template(
    const TclProtocolProfile profile) {
  if (profile_is_31_byte(profile))
    return TCL_CONTROL_TEMPLATE_31;
  if (profile_is_tyjw2(profile))
    return TCL_CONTROL_TEMPLATE_TYJW2;
  if (profile_is_tclac(profile))
    return TCL_CONTROL_TEMPLATE_TCLAC;
  return TCL_CONTROL_TEMPLATE_35;
}

bool map_status_mode_to_control(const uint8_t status_mode, uint8_t &control_mode) {
  switch (status_mode) {
    case 0x01:  // Cool
      control_mode = 0x03;
      return true;
    case 0x02:  // Fan only
      control_mode = 0x07;
      return true;
    case 0x03:  // Dry
      control_mode = 0x02;
      return true;
    case 0x04:  // Heat (optional on this TCL family)
      control_mode = 0x01;
      return true;
    case 0x05:  // Auto
      control_mode = 0x08;
      return true;
    default:
      return false;
  }
}

bool map_status_fan_to_control(const uint8_t status_fan, const TclProtocolProfile profile,
                               uint8_t &control_fan) {
  if (profile_is_tclac(profile)) {
    switch (status_fan) {
      case 0x00:
        control_fan = 0x00;
        return true;
      case 0x01:
        control_fan = 0x01;
        return true;
      case 0x02:
        control_fan = 0x03;
        return true;
      case 0x03:
        control_fan = 0x05;
        return true;
      case 0x04:
        control_fan = 0x06;
        return true;
      case 0x05:
        control_fan = 0x07;
        return true;
      default:
        return false;
    }
  }

  switch (status_fan) {
    case 0x00:
      control_fan = 0x00;
      return true;
    case 0x01:
      control_fan = 0x02;
      return true;
    case 0x02:
      control_fan = 0x03;
      return true;
    case 0x03:
      control_fan = 0x05;
      return true;
    case 0x04:
      if (profile_is_tyjw2(profile)) {
        control_fan = 0x06;
        return true;
      }
      return false;
    case 0x05:
      if (profile_is_tyjw2(profile)) {
        control_fan = 0x07;
        return true;
      }
      return false;
    default:
      return false;
  }
}

}  // namespace

const std::array<uint8_t, TCL_STATUS_REQUEST_SIZE> TCL_STATUS_REQUEST{
    0xBB, 0x00, 0x01, 0x04, 0x02, 0x01, 0x00, 0xBD,
};

TclFrameParserResult TclFrameParser::feed(const uint8_t byte) {
  if (this->position_ == 0) {
    if (byte != TCL_HEADER)
      return TclFrameParserResult::NONE;
    this->buffer_[this->position_++] = byte;
    return TclFrameParserResult::NONE;
  }

  if (this->position_ >= this->buffer_.size()) {
    this->reset();
    return TclFrameParserResult::INVALID_LENGTH;
  }

  this->buffer_[this->position_++] = byte;

  if (this->position_ == 5) {
    this->expected_size_ = static_cast<size_t>(this->buffer_[4]) + 6U;
    if (this->expected_size_ < TCL_STATUS_REQUEST_SIZE ||
        this->expected_size_ > this->buffer_.size()) {
      this->reset();
      return TclFrameParserResult::INVALID_LENGTH;
    }
  }

  if (this->expected_size_ != 0 && this->position_ == this->expected_size_)
    return TclFrameParserResult::FRAME_READY;

  return TclFrameParserResult::NONE;
}

void TclFrameParser::reset() {
  this->position_ = 0;
  this->expected_size_ = 0;
}

uint8_t tcl_xor_checksum(const uint8_t *data, const size_t length) {
  uint8_t checksum = 0;
  for (size_t i = 0; i < length; i++)
    checksum ^= data[i];
  return checksum;
}

const char *tcl_protocol_profile_name(const TclProtocolProfile profile) {
  switch (profile) {
    case TclProtocolProfile::PROFILE_TCL_35:
      return "TCL 35 bytes";
    case TclProtocolProfile::PROFILE_ELECTRIQ_31:
      return "ElectriQ 31 bytes";
    case TclProtocolProfile::PROFILE_PIONEER_31:
      return "Pioneer 31 bytes";
    case TclProtocolProfile::PROFILE_TYJW2_35:
      return "TYJW2 extendido 35 bytes";
    case TclProtocolProfile::PROFILE_TCLAC_38:
      return "tclac 38 bytes";
  }
  return "desconocido";
}

float tcl_protocol_target_step(const TclProtocolProfile profile) {
  return profile_is_tyjw2(profile) ? 0.5f : 1.0f;
}

bool tcl_supported_status_frame_size(const size_t length) {
  return length == TCL_STATUS_FRAME_61_SIZE || length == TCL_STATUS_FRAME_65_SIZE ||
         length == TCL_STATUS_FRAME_68_SIZE;
}

bool tcl_validate_status_frame(const uint8_t *data, const size_t length,
                               TclProtocolProfile profile, const bool accept_command_response) {
  if (data == nullptr || !tcl_supported_status_frame_size(length))
    return false;
  if (data[0] != TCL_HEADER || data[1] != 0x01 || data[2] != 0x00 ||
      data[5] != 0x04 || data[6] != 0x00)
    return false;
  const bool profile_accepts_command_response =
      profile != TclProtocolProfile::PROFILE_TCL_35;
  if (data[3] != TCL_STATUS_COMMAND &&
      !(data[3] == TCL_COMMAND_RESPONSE &&
        (profile_accepts_command_response || accept_command_response)))
    return false;
  if (static_cast<size_t>(data[4]) + 6U != length)
    return false;
  return tcl_xor_checksum(data, length - 1) == data[length - 1];
}

bool tcl_decode_status_frame(const uint8_t *data, const size_t length, TclProtocolState &state,
                             TclProtocolProfile profile, const bool accept_command_response) {
  if (!tcl_validate_status_frame(data, length, profile, accept_command_response))
    return false;

  TclProtocolState decoded{};
  decoded.mode = data[7] & 0x0F;
  decoded.power = (data[7] & 0x10) != 0;
  decoded.display = (data[7] & 0x20) != 0;
  decoded.eco = (data[7] & 0x40) != 0;
  decoded.turbo = (data[7] & 0x80) != 0;

  decoded.target_temperature = static_cast<float>((data[8] & 0x0F) + 16U);
  if (profile_is_tyjw2(profile) && (data[9] & 0x01) != 0)
    decoded.target_temperature += 0.5f;
  decoded.fan = profile_is_tyjw2(profile)
                    ? static_cast<uint8_t>((data[8] >> 4U) & 0x0F)
                    : static_cast<uint8_t>((data[8] >> 4U) & 0x07);
  decoded.health = (data[9] & 0x04) != 0;
  decoded.anti_mildew = profile_is_tyjw2(profile) && (data[9] & 0x08) != 0;
  decoded.horizontal_swing = (data[10] & 0x20) != 0;
  decoded.vertical_swing = (data[10] & 0x40) != 0;

  const uint16_t raw_temperature =
      static_cast<uint16_t>((static_cast<uint16_t>(data[17]) << 8U) | data[18]);
  decoded.current_temperature =
      ((static_cast<float>(raw_temperature) / 374.0f) - 32.0f) / 1.8f;

  decoded.sleep = (data[19] & 0x01) != 0;
  decoded.deep_sleep_bit = (data[19] & 0x80) != 0;
  decoded.mute = (data[33] & 0x80) != 0;
  decoded.fan_speed = data[34];
  decoded.pipe_out_temperature = data[35];
  decoded.pipe_in_temperature = data[36];
  decoded.compressor_current = static_cast<float>(data[39]) / 10.0f;
  decoded.compressor_state = data[40];
  decoded.fault = data[44];
  decoded.supply_voltage = data[45];
  decoded.outside_motor = data[46];
  // The extended TYJW2 captures use byte 50 differently; do not invent a filter bit there.
  decoded.clean_filter =
      !profile_is_tyjw2(profile) && !profile_is_tclac(profile) && (data[50] & 0x02) != 0;
  decoded.vertical_vane_position = data[51];
  decoded.horizontal_vane_position = data[52];

  state = decoded;
  return true;
}

bool tcl_build_control_frame(const TclProtocolState &state, TclProtocolProfile profile,
                             TclControlFrame &frame) {
  uint8_t control_mode = 0;
  uint8_t control_fan = 0;
  frame.bytes = profile_control_template(profile);
  frame.size = profile_control_size(profile);
  auto &bytes = frame.bytes;

  if (profile_is_tclac(profile) && !state.power) {
    // tclac encodes OFF with a zero mode nibble.
    control_mode = 0;
  } else if (!map_status_mode_to_control(state.mode, control_mode)) {
    // Some units report mode 0 while OFF. The original 35-byte component
    // retained its template's Cool nibble for non-power changes in that state.
    if (!state.power)
      control_mode = static_cast<uint8_t>(bytes[8] & 0x0FU);
    else
      return false;
  }
  if (!map_status_fan_to_control(state.fan, profile, control_fan)) {
    if (!state.power)
      control_fan = 0;
    else
      return false;
  }

  // Byte 7: power, timers forced off, beep, display and eco.
  bytes[7] &= static_cast<uint8_t>(~0xFCU);
  if (state.power)
    bytes[7] |= 0x04;
  if (state.beep)
    bytes[7] |= 0x20;
  if (state.display && (!profile_is_tclac(profile) || state.power))
    bytes[7] |= 0x40;
  if (state.eco)
    bytes[7] |= 0x80;

  // Byte 8: mode, health, turbo and mute.
  bytes[8] = control_mode;
  if (state.health)
    bytes[8] |= 0x10;
  if (profile_is_tyjw2(profile) && state.anti_mildew)
    bytes[8] |= 0x20;
  if (state.turbo)
    bytes[8] |= 0x40;
  if (state.mute)
    bytes[8] |= 0x80;

  const float step = tcl_protocol_target_step(profile);
  const float maximum = step == 0.5f ? 31.5f : 31.0f;
  const float bounded = std::max(16.0f, std::min(maximum, state.target_temperature));
  const float quantized = std::round(bounded / step) * step;
  const uint8_t whole_target = static_cast<uint8_t>(std::floor(quantized));
  const bool half_degree = step == 0.5f && quantized - whole_target >= 0.25f;
  bytes[9] =
      static_cast<uint8_t>((bytes[9] & 0xF0U) | ((31U - whole_target) & 0x0FU));

  // Byte 10: fan in bits 0..2, vertical swing command in bits 3..5.
  bytes[10] &= static_cast<uint8_t>(~0x3FU);
  bytes[10] |= control_fan;
  if (state.vertical_swing)
    bytes[10] |= 0x38;

  if (profile_is_tyjw2(profile) || profile_is_tclac(profile)) {
    // These extended layouts separate movement and vane position.
    bytes[11] &= static_cast<uint8_t>(profile_is_tyjw2(profile) ? ~0x0CU : ~0x08U);
    if (profile_is_tyjw2(profile) && half_degree)
      bytes[11] |= 0x04;
    if (state.horizontal_swing)
      bytes[11] |= 0x08;

    uint8_t vertical_position = 0x08;
    uint8_t horizontal_position = 0x08;
    if (profile_is_tyjw2(profile)) {
      // Echo capture-derived direction codes for unrelated changes. When a
      // user toggles swing, keep the reported fixed position or choose the
      // documented full-range direction rather than normalizing to center.
      const uint8_t vertical_raw =
          static_cast<uint8_t>(state.vertical_vane_position & 0x1FU);
      const uint8_t horizontal_raw =
          static_cast<uint8_t>(state.horizontal_vane_position & 0x3FU);
      const bool vertical_direction_valid =
          vertical_raw == 0x08 || vertical_raw == 0x10 || vertical_raw == 0x18;
      const bool horizontal_direction_valid =
          horizontal_raw == 0x08 || horizontal_raw == 0x10 ||
          horizontal_raw == 0x18 || horizontal_raw == 0x20;
      const uint8_t vertical_fixed =
          (vertical_raw & 0x07U) <= 0x05 ? static_cast<uint8_t>(vertical_raw & 0x07U) : 0;
      const uint8_t horizontal_fixed =
          (horizontal_raw & 0x07U) <= 0x05 ? static_cast<uint8_t>(horizontal_raw & 0x07U) : 0;
      vertical_position =
          state.vertical_swing
              ? static_cast<uint8_t>(vertical_direction_valid ? vertical_raw : 0x08)
              : vertical_fixed;
      horizontal_position =
          state.horizontal_swing
              ? static_cast<uint8_t>(horizontal_direction_valid ? horizontal_raw : 0x08)
              : horizontal_fixed;
    }
    bytes[32] = static_cast<uint8_t>((bytes[32] & 0xE0U) | vertical_position);
    if (profile_is_tyjw2(profile)) {
      bytes[33] =
          static_cast<uint8_t>((bytes[33] & 0x40U) | 0x80U | horizontal_position);
    } else {
      bytes[33] =
          static_cast<uint8_t>((bytes[33] & 0xC0U) | horizontal_position);
    }

    if (profile_is_tyjw2(profile)) {
      for (const auto &[index, mask] :
           std::array<std::pair<uint8_t, uint8_t>, 3>{{{12, 0x04}, {30, 0x60}, {31, 0x01}}}) {
        if (state.eco)
          bytes[index] |= mask;
        else
          bytes[index] &= static_cast<uint8_t>(~mask);
      }
    }
  } else {
    // Preserve the proven 35-byte layout by default. Pioneer is the explicit
    // three-bit horizontal-swing variant.
    const uint8_t horizontal_mask =
        profile == TclProtocolProfile::PROFILE_PIONEER_31 ? 0x38 : 0x08;
    bytes[14] &= static_cast<uint8_t>(~horizontal_mask);
    if (profile != TclProtocolProfile::PROFILE_PIONEER_31)
      bytes[14] &= static_cast<uint8_t>(~0x20U);  // half degree remains disabled
    if (state.horizontal_swing)
      bytes[14] |= horizontal_mask;
  }

  bytes[19] &= static_cast<uint8_t>(~0x01U);
  if (state.sleep)
    bytes[19] |= 0x01;

  bytes[frame.size - 1] = tcl_xor_checksum(bytes.data(), frame.size - 1);
  return true;
}

}  // namespace esphome::tcl_climate
