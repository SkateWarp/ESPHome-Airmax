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

// Residential split systems are normally connected to 100/120/127 V or
// 220/230/240 V mains.  Keep deliberately broad commissioning margins while
// rejecting values that cannot represent a powered residential appliance.
// The TCL status field is only one byte, so readings above 255 V arrive modulo
// 256 (for example, raw 7 represents 263 V).
static constexpr uint16_t TCL_SUPPLY_VOLTAGE_MIN = 90;
static constexpr uint16_t TCL_SUPPLY_VOLTAGE_MAX = 285;
static constexpr uint16_t TCL_SUPPLY_VOLTAGE_ZERO_WRAP_MAX_DELTA = 40;

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
  // Last accepted mains reading.  A rejected raw byte leaves an existing
  // accepted value intact instead of publishing a fabricated diagnostic.
  uint16_t supply_voltage{0};
  bool supply_voltage_valid{false};
  uint8_t outside_motor{0};
  bool clean_filter{false};
  uint8_t vertical_vane_position{0};
  uint8_t horizontal_vane_position{0};
};

struct TclControlFrame {
  std::array<uint8_t, TCL_CONTROL_FRAME_MAX_SIZE> bytes{};
  size_t size{0};
};

// The response envelope is not byte-identical across otherwise compatible
// TCL indoor units. The frame length and model-dependent envelope bytes are
// learned from repeated checksum-valid heartbeat replies, then locked for the
// rest of the boot so unrelated or changing UART traffic cannot enable control.
struct TclStatusSignature {
  uint8_t byte_1{0};
  uint8_t byte_2{0};
  uint8_t byte_5{0};
  uint8_t byte_6{0};
  uint8_t frame_size{0};

  bool is_set() const { return this->frame_size != 0; }
  bool matches(const uint8_t *data, size_t length) const {
    return data != nullptr && this->is_set() && length > 6 &&
           length == this->frame_size &&
           data[1] == this->byte_1 && data[2] == this->byte_2 &&
           data[5] == this->byte_5 && data[6] == this->byte_6;
  }
  bool operator==(const TclStatusSignature &other) const {
    return this->byte_1 == other.byte_1 && this->byte_2 == other.byte_2 &&
           this->byte_5 == other.byte_5 && this->byte_6 == other.byte_6 &&
           this->frame_size == other.frame_size;
  }
};

class TclStatusSignatureDetector {
 public:
  static constexpr uint8_t REQUIRED_CONFIRMATIONS = 2;

  bool observe(const TclStatusSignature &observed) {
    if (!observed.is_set())
      return false;
    if (this->active_.is_set())
      return this->active_ == observed;
    if (this->candidate_ == observed) {
      this->candidate_count_++;
    } else {
      this->candidate_ = observed;
      this->candidate_count_ = 1;
    }
    if (this->candidate_count_ >= REQUIRED_CONFIRMATIONS)
      this->active_ = this->candidate_;
    return this->active_.is_set();
  }

  bool locked() const { return this->active_.is_set(); }
  bool matches(const uint8_t *data, size_t length) const {
    return this->active_.matches(data, length);
  }
  uint8_t candidate_count() const { return this->candidate_count_; }
  const TclStatusSignature &signature() const { return this->active_; }

 protected:
  TclStatusSignature candidate_{};
  TclStatusSignature active_{};
  uint8_t candidate_count_{0};
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
const char *tcl_fan_speed_text(uint8_t fan_speed);
void tcl_format_fault_text(uint8_t fault, char *output, size_t output_size);
void tcl_format_profile_text(TclProtocolProfile profile, size_t status_frame_size,
                             char *output, size_t output_size);
float tcl_protocol_target_step(TclProtocolProfile profile);
bool tcl_supported_status_frame_size(size_t length);
bool tcl_normalize_supply_voltage(uint8_t raw, uint16_t &normalized,
                                  const uint16_t *last_valid = nullptr);
bool tcl_extract_status_signature(const uint8_t *data, size_t length,
                                  TclStatusSignature &signature,
                                  TclProtocolProfile profile = TclProtocolProfile::PROFILE_TCL_35,
                                  bool accept_command_response = false);
bool tcl_validate_status_frame(const uint8_t *data, size_t length,
                               TclProtocolProfile profile = TclProtocolProfile::PROFILE_TCL_35,
                               bool accept_command_response = false,
                               const TclStatusSignature *signature = nullptr);
bool tcl_decode_status_frame(const uint8_t *data, size_t length, TclProtocolState &state,
                             TclProtocolProfile profile = TclProtocolProfile::PROFILE_TCL_35,
                             bool accept_command_response = false,
                             const TclStatusSignature *signature = nullptr);
bool tcl_status_state_is_plausible(
    const TclProtocolState &state,
    TclProtocolProfile profile = TclProtocolProfile::PROFILE_TCL_35);
bool tcl_build_control_frame(const TclProtocolState &state, TclProtocolProfile profile,
                             TclControlFrame &frame);

extern const std::array<uint8_t, TCL_STATUS_REQUEST_SIZE> TCL_STATUS_REQUEST;

}  // namespace esphome::tcl_climate
