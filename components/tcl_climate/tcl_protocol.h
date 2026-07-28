#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::tcl_climate {

static constexpr size_t TCL_STATUS_FRAME_61_SIZE = 61;
static constexpr size_t TCL_STATUS_FRAME_65_SIZE = 65;
static constexpr size_t TCL_STATUS_FRAME_68_SIZE = 68;
static constexpr size_t TCL_STATUS_FRAME_MAX_SIZE = 68;
static constexpr size_t TCL_CONTROL_FRAME_MAX_SIZE = 38;
static constexpr size_t TCL_CONTROL_FRAME_35_SIZE = 35;
static constexpr size_t TCL_CONTROL_FRAME_31_SIZE = 31;
static constexpr size_t TCL_CONTROL_FRAME_38_SIZE = 38;
static constexpr size_t TCL_STATUS_REQUEST_SIZE = 8;
static constexpr size_t TCL_MAX_FRAME_SIZE = 96;

enum class TclProtocolProfile : uint8_t {
  PROFILE_TCL_35,
  PROFILE_ELECTRIQ_31,
  PROFILE_PIONEER_31,
  PROFILE_TYJW2_35,
  PROFILE_TCLAC_38,
};

struct TclProtocolState {
  bool power{false};
  uint8_t mode{0};
  float target_temperature{22.0f};
  uint8_t fan{0};
  bool display{false};
  bool eco{false};
  bool turbo{false};
  bool health{false};
  bool horizontal_swing{false};
  bool vertical_swing{false};
  bool sleep{false};
  bool deep_sleep_bit{false};
  bool mute{false};
  bool beep{false};
  bool anti_mildew{false};

  float current_temperature{0.0f};
  uint8_t fan_speed{0};
  uint8_t pipe_out_temperature{0};
  uint8_t pipe_in_temperature{0};
  float compressor_current{0.0f};
  uint8_t compressor_state{0};
  uint8_t fault{0};
  uint8_t supply_voltage{0};
  uint8_t outside_motor{0};
  bool clean_filter{false};
  uint8_t vertical_vane_position{0};
  uint8_t horizontal_vane_position{0};
};

struct TclControlFrame {
  std::array<uint8_t, TCL_CONTROL_FRAME_MAX_SIZE> bytes{};
  size_t size{0};
};

enum class TclFrameParserResult : uint8_t {
  NONE,
  FRAME_READY,
  INVALID_LENGTH,
};

class TclFrameParser {
 public:
  TclFrameParserResult feed(uint8_t byte);
  void reset();

  const uint8_t *data() const { return this->buffer_.data(); }
  size_t size() const { return this->position_; }
  bool has_partial_frame() const { return this->position_ != 0; }

 protected:
  std::array<uint8_t, TCL_MAX_FRAME_SIZE> buffer_{};
  size_t position_{0};
  size_t expected_size_{0};
};

uint8_t tcl_xor_checksum(const uint8_t *data, size_t length);
const char *tcl_protocol_profile_name(TclProtocolProfile profile);
float tcl_protocol_target_step(TclProtocolProfile profile);
bool tcl_supported_status_frame_size(size_t length);
bool tcl_validate_status_frame(const uint8_t *data, size_t length,
                               TclProtocolProfile profile = TclProtocolProfile::PROFILE_TCL_35,
                               bool accept_command_response = false);
bool tcl_decode_status_frame(const uint8_t *data, size_t length, TclProtocolState &state,
                             TclProtocolProfile profile = TclProtocolProfile::PROFILE_TCL_35,
                             bool accept_command_response = false);
bool tcl_build_control_frame(const TclProtocolState &state, TclProtocolProfile profile,
                             TclControlFrame &frame);

extern const std::array<uint8_t, TCL_STATUS_REQUEST_SIZE> TCL_STATUS_REQUEST;

}  // namespace esphome::tcl_climate
