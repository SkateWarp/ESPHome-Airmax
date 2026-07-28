#include "tcl_climate.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "esphome/core/log.h"

namespace esphome::tcl_climate {

namespace {

static const char *const TAG = "tcl_climate";

void publish_sensor_if_changed(sensor::Sensor *entity, const float value, const float epsilon = 0.001f) {
  if (entity == nullptr)
    return;
  if (!entity->has_state() || std::isnan(entity->state) || std::fabs(entity->state - value) > epsilon)
    entity->publish_state(value);
}

void publish_binary_if_changed(binary_sensor::BinarySensor *entity, const bool value) {
  if (entity != nullptr && (!entity->has_state() || entity->state != value))
    entity->publish_state(value);
}

void publish_text_if_changed(text_sensor::TextSensor *entity, const char *value) {
  if (entity != nullptr && (!entity->has_state() || entity->state != value))
    entity->publish_state(value);
}

void publish_switch_if_changed(switch_::Switch *entity, const bool value) {
  if (entity != nullptr && (!entity->has_state() || entity->state != value))
    entity->publish_state(value);
}

}  // namespace

void TclClimate::setup() {
  const float unavailable = std::numeric_limits<float>::quiet_NaN();
  this->current_temperature = unavailable;
  this->target_temperature = unavailable;
  this->status_set_warning();

  // Child switches are not Components themselves. Restore their configured state here,
  // after code generation has attached every child to this climate instance.
  this->restore_switch_(this->display_switch_, TclSwitchType::DISPLAY_CONTROL);
  this->restore_switch_(this->beep_switch_, TclSwitchType::BEEP_CONTROL);
  this->restore_switch_(this->health_switch_, TclSwitchType::HEALTH_CONTROL);

  if (this->restore_state_enabled_) {
    auto restored = this->restore_state_();
    if (restored.has_value()) {
      // control() only queues the compact restored state. Transmission remains
      // locked until a fresh, validated appliance status has been received.
      restored->to_call(this).perform();
      ESP_LOGI(TAG, "Queued persisted climate state for safe restore");
    } else {
      ESP_LOGI(TAG, "State restore enabled, but no persisted climate state exists yet");
    }
  }
  this->publish_profile_state_();
}

void TclClimate::dump_config() {
  LOG_CLIMATE("", "TCL UART Climate", this);
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  Protocol family: TCL UART / XOR");
  ESP_LOGCONFIG(TAG, "  Configured protocol profile: %s",
                tcl_protocol_profile_name(this->configured_profile_));
  if (this->configured_status_frame_size_ == 0) {
    if (this->active_status_frame_size_ == 0)
      ESP_LOGCONFIG(TAG, "  Status frame length: auto (waiting for first valid status)");
    else
      ESP_LOGCONFIG(TAG, "  Status frame length: auto (locked to %u bytes)",
                    this->active_status_frame_size_);
  } else {
    ESP_LOGCONFIG(TAG, "  Status frame length: %u bytes",
                  this->configured_status_frame_size_);
  }
  ESP_LOGCONFIG(TAG, "  Heat support: %s", YESNO(this->supports_heat_));
  ESP_LOGCONFIG(TAG, "  Horizontal swing support: %s", YESNO(this->supports_horizontal_swing_));
  ESP_LOGCONFIG(TAG, "  Restore climate state after power loss: %s",
                YESNO(this->restore_state_enabled_));
  ESP_LOGCONFIG(TAG, "  Temperature moving-average samples: %u", this->temperature_window_size_);
  ESP_LOGCONFIG(TAG, "  Status timeout: %" PRIu32 " ms", this->status_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Inter-byte timeout: %" PRIu32 " ms", this->inter_byte_timeout_ms_);
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_EVEN, 8);
}

climate::ClimateTraits TclClimate::traits() {
  climate::ClimateTraits traits;
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                           climate::CLIMATE_SUPPORTS_ACTION);

  traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
  traits.add_supported_mode(climate::CLIMATE_MODE_COOL);
  traits.add_supported_mode(climate::CLIMATE_MODE_FAN_ONLY);
  traits.add_supported_mode(climate::CLIMATE_MODE_DRY);
  traits.add_supported_mode(climate::CLIMATE_MODE_AUTO);
  if (this->supports_heat_)
    traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);

  traits.add_supported_fan_mode(climate::CLIMATE_FAN_AUTO);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_QUIET);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_LOW);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_MEDIUM);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_HIGH);
  if (this->configured_profile_ == TclProtocolProfile::PROFILE_TCLAC_38) {
    traits.add_supported_fan_mode(climate::CLIMATE_FAN_MIDDLE);
    traits.add_supported_fan_mode(climate::CLIMATE_FAN_FOCUS);
    traits.add_supported_fan_mode(climate::CLIMATE_FAN_DIFFUSE);
  }

  traits.add_supported_swing_mode(climate::CLIMATE_SWING_OFF);
  traits.add_supported_swing_mode(climate::CLIMATE_SWING_VERTICAL);
  if (this->supports_horizontal_swing_) {
    traits.add_supported_swing_mode(climate::CLIMATE_SWING_HORIZONTAL);
    traits.add_supported_swing_mode(climate::CLIMATE_SWING_BOTH);
  }

  traits.add_supported_preset(climate::CLIMATE_PRESET_NONE);
  traits.add_supported_preset(climate::CLIMATE_PRESET_ECO);
  traits.add_supported_preset(climate::CLIMATE_PRESET_SLEEP);
  if (this->configured_profile_ == TclProtocolProfile::PROFILE_TCLAC_38) {
    if (this->health_switch_ == nullptr)
      traits.add_supported_preset(climate::CLIMATE_PRESET_COMFORT);
  } else {
    traits.add_supported_preset(climate::CLIMATE_PRESET_BOOST);
  }
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(
      tcl_protocol_target_step(this->configured_profile_) == 0.5f ? 31.5f : 31.0f);
  traits.set_visual_target_temperature_step(tcl_protocol_target_step(this->configured_profile_));
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void TclClimate::loop() {
  bool received_byte = false;
  uint8_t byte = 0;
  while (this->available() > 0 && this->read_byte(&byte)) {
    received_byte = true;
    this->last_rx_byte_ms_ = millis();
    this->process_rx_byte_(byte);
  }

  uint32_t now = millis();
  if (!received_byte && this->parser_.has_partial_frame() &&
      now - this->last_rx_byte_ms_ >= this->inter_byte_timeout_ms_) {
    this->parser_.reset();
    this->invalidate_status_("Incomplete UART frame timed out");
  }

  if (this->has_valid_status_ && !this->status_timed_out_ &&
      now - this->last_status_ms_ >= this->status_timeout_ms_) {
    this->status_timed_out_ = true;
    this->status_set_warning();
    this->current_temperature = std::numeric_limits<float>::quiet_NaN();
    this->publish_state();
    ESP_LOGW(TAG, "TCL status is stale; climate control is locked until a valid frame arrives");
  }
}

void TclClimate::update() {
  if (!this->bus_is_quiet_()) {
    ESP_LOGVV(TAG, "TX deferred while the TCL UART bus is active");
    return;
  }

  const uint32_t now = millis();
  if (this->awaiting_response_ &&
      now - this->last_tx_ms_ < RESPONSE_WINDOW_MS) {
    ESP_LOGVV(TAG, "TX deferred during the TCL response window");
    return;
  }
  this->awaiting_response_ = false;

  // A control frame must be followed by a new, validated appliance state
  // before another queued change may be built on top of it.
  if (this->awaiting_command_status_) {
    this->send_status_request_();
    return;
  }

  if (!this->send_pending_command_())
    this->send_status_request_();
}

void TclClimate::process_rx_byte_(const uint8_t byte) {
  const TclFrameParserResult result = this->parser_.feed(byte);
  if (result == TclFrameParserResult::INVALID_LENGTH) {
    this->invalidate_status_("UART frame declared an invalid length");
    return;
  }
  if (result != TclFrameParserResult::FRAME_READY)
    return;

  this->handle_frame_(this->parser_.data(), this->parser_.size());
  this->parser_.reset();
}

void TclClimate::handle_frame_(const uint8_t *data, const size_t length) {
  if (!tcl_supported_status_frame_size(length))
    return;
  if (this->active_status_frame_size_ != 0 &&
      length != this->active_status_frame_size_) {
    ESP_LOGW(TAG, "Ignoring %u-byte status after locking to %u bytes", length,
             this->active_status_frame_size_);
    return;
  }

  // The original, proven 35-byte component intentionally ignored command
  // responses (type 0x03). Keep doing so without poisoning the last clean
  // status: some indoor units echo such a response after a control command.
  if (this->active_profile_ == TclProtocolProfile::PROFILE_TCL_35 &&
      data[3] == 0x03) {
    if (tcl_validate_status_frame(data, length, this->active_profile_, true))
      ESP_LOGVV(TAG, "Ignored valid TCL command response");
    else
      ESP_LOGW(TAG, "Ignored malformed TCL command response");
    return;
  }

  TclProtocolState decoded{};
  if (!tcl_decode_status_frame(data, length, decoded, this->active_profile_)) {
    const uint8_t calculated = tcl_xor_checksum(data, length - 1);
    ESP_LOGW(TAG, "Rejected TCL status for profile %s: received checksum 0x%02X, "
                  "calculated 0x%02X",
             tcl_protocol_profile_name(this->active_profile_), data[length - 1], calculated);
    this->invalidate_status_("Rejected malformed or incompatible TCL status");
    return;
  }

  if (this->active_status_frame_size_ == 0) {
    this->active_status_frame_size_ = static_cast<uint8_t>(length);
    ESP_LOGI(TAG, "Automatically locked TCL status length to %u bytes", length);
    this->publish_profile_state_();
  }

  // The status message has no beep state. Preserve the local policy used for future commands.
  decoded.beep = this->state_.beep;

  const bool tclac_profile =
      this->active_profile_ == TclProtocolProfile::PROFILE_TCLAC_38;
  if (tclac_profile && decoded.power && this->deferred_fields_ != 0 &&
      !(this->awaiting_command_status_ && this->awaiting_deferred_fields_ != 0)) {
    // The unit was turned on outside this component. Queue the preferences
    // selected while it was OFF before the fresh ON status overwrites state_.
    const uint32_t deferred_without_newer_request =
        this->deferred_fields_ & ~this->pending_fields_;
    this->apply_fields_(this->requested_state_, this->state_,
                        deferred_without_newer_request);
    this->pending_fields_ |= this->deferred_fields_;
    ESP_LOGD(TAG, "Queued deferred TCLAC OFF-state preferences after external power-on");
  }

  if (tclac_profile && !decoded.power) {
    // tclac does not expose these controls in an OFF response. Keep their
    // last policy internally so a later ON command does not erase it.
    if (this->has_valid_status_) {
      decoded.mode = this->state_.mode;
      decoded.display = this->state_.display;
      decoded.fan = this->state_.fan;
      decoded.mute = this->state_.mute;
      decoded.turbo = this->state_.turbo;  // FAN_DIFFUSE in this profile
      decoded.eco = this->state_.eco;
      decoded.health = this->state_.health;
      decoded.sleep = this->state_.sleep;
      decoded.vertical_swing = this->state_.vertical_swing;
      decoded.horizontal_swing = this->state_.horizontal_swing;
    } else {
      decoded.display = false;
      decoded.fan = 0x00;
      decoded.mute = false;
      decoded.turbo = false;
      decoded.eco = false;
      decoded.health = false;
      decoded.sleep = false;
      decoded.vertical_swing = false;
      decoded.horizontal_swing = false;
    }
  }

  decoded.current_temperature = this->add_temperature_sample_(decoded.current_temperature);
  bool command_rejected = false;
  if (this->awaiting_command_status_) {
    if (this->status_confirms_command_(decoded)) {
      const bool retained_local_policy =
          this->awaiting_command_observable_fields_ != this->awaiting_command_fields_;
      this->deferred_fields_ &= ~this->awaiting_deferred_fields_;
      this->awaiting_deferred_fields_ = 0;
      this->awaiting_command_status_ = false;
      this->awaiting_command_fields_ = 0;
      this->awaiting_command_observable_fields_ = 0;
      this->command_confirmation_misses_ = 0;
      if (retained_local_policy) {
        ESP_LOGD(TAG, "Observable TCL fields confirmed; write-only or OFF-state "
                      "preferences remain local until the appliance reports them");
      } else {
        ESP_LOGD(TAG, "TCL control command confirmed by appliance state");
      }
    } else if (++this->command_confirmation_misses_ >=
               MAX_COMMAND_CONFIRMATION_MISSES) {
      // If the appliance is still OFF, these fields remain unobservable and
      // must stay deferred for a future ON attempt. Once it is ON, a mismatch
      // is an actual rejection and the reported appliance state wins.
      if (!(tclac_profile && !decoded.power))
        this->deferred_fields_ &= ~this->awaiting_deferred_fields_;
      this->awaiting_deferred_fields_ = 0;
      this->awaiting_command_status_ = false;
      this->awaiting_command_fields_ = 0;
      this->awaiting_command_observable_fields_ = 0;
      this->command_confirmation_misses_ = 0;
      command_rejected = true;
      ESP_LOGW(TAG, "TCL control was not confirmed after %u clean status frames; "
                    "continuing from the appliance state",
               MAX_COMMAND_CONFIRMATION_MISSES);
    } else {
      ESP_LOGD(TAG, "TCL status has not reflected the last control yet (%u/%u)",
               this->command_confirmation_misses_,
               MAX_COMMAND_CONFIRMATION_MISSES);
    }
  }

  this->state_ = decoded;
  this->has_valid_status_ = true;
  this->status_timed_out_ = false;
  this->awaiting_response_ = false;
  this->last_status_ms_ = millis();
  if (command_rejected)
    this->status_set_warning();
  else
    this->status_clear_warning();

  ESP_LOGVV(TAG,
            "RX status: power=%s mode=0x%02X target=%.1f fan=0x%02X current=%.2f checksum=0x%02X",
            ONOFF(this->state_.power), this->state_.mode, this->state_.target_temperature,
            this->state_.fan, this->state_.current_temperature, data[length - 1]);
  this->publish_protocol_state_();
}

void TclClimate::invalidate_status_(const char *reason) {
  const bool had_valid_status = this->has_valid_status_;
  this->has_valid_status_ = false;
  this->status_timed_out_ = true;
  this->status_set_warning();
  if (had_valid_status) {
    // Keep the last command state (including target) so persistence is not
    // overwritten with NaN merely because communication was interrupted.
    this->current_temperature = std::numeric_limits<float>::quiet_NaN();
    this->publish_state();
  }
  ESP_LOGW(TAG, "%s; climate control is locked until a clean status arrives", reason);
}

void TclClimate::send_status_request_() {
  this->write_array(TCL_STATUS_REQUEST);
  this->last_tx_ms_ = millis();
  this->awaiting_response_ = true;
  ESP_LOGVV(TAG, "TX status request");
}

bool TclClimate::send_pending_command_() {
  if (this->pending_fields_ == 0 || !this->status_is_fresh_())
    return false;

  TclProtocolState command_state = this->state_;
  this->apply_pending_fields_(command_state);

  // These three controls are represented by child entities in the original component.
  // Their current UI state remains the source of truth when they are configured.
  if (!(this->pending_fields_ & PENDING_DISPLAY) &&
      this->display_switch_ != nullptr && this->display_switch_->has_state())
    command_state.display = this->display_switch_->state;
  if (!(this->pending_fields_ & PENDING_BEEP) &&
      this->beep_switch_ != nullptr && this->beep_switch_->has_state())
    command_state.beep = this->beep_switch_->state;
  if (!(this->pending_fields_ & PENDING_HEALTH) &&
      this->health_switch_ != nullptr && this->health_switch_->has_state())
    command_state.health = this->health_switch_->state;

  TclControlFrame frame{};
  if (!tcl_build_control_frame(command_state, this->active_profile_, frame)) {
    ESP_LOGW(TAG, "Control remains queued: current TCL mode or fan code is not safely mappable");
    return false;
  }

  this->write_array(frame.bytes.data(), frame.size);
  this->awaiting_command_state_ = command_state;
  uint32_t command_fields = this->pending_fields_;
  this->awaiting_deferred_fields_ = 0;
  if (this->active_profile_ == TclProtocolProfile::PROFILE_TCLAC_38) {
    if (command_state.power) {
      // Preferences selected while OFF are encoded by every ON frame. Include
      // them in this command's confirmation even if POWER/MODE was the only
      // new ClimateCall.
      this->awaiting_deferred_fields_ = this->deferred_fields_;
      command_fields |= this->deferred_fields_;
      if (this->awaiting_deferred_fields_ != 0)
        command_fields |= PENDING_POWER;
    } else {
      this->deferred_fields_ |=
          this->pending_fields_ & TCLAC_OFF_UNOBSERVABLE_FIELDS;
    }
  }
  this->awaiting_command_fields_ = command_fields;
  this->awaiting_command_observable_fields_ =
      command_fields & ~static_cast<uint32_t>(PENDING_BEEP);
  if (this->active_profile_ == TclProtocolProfile::PROFILE_TCLAC_38 &&
      !command_state.power) {
    // tclac deliberately does not expose these values in an OFF status. Some
    // (notably display) are not transmitted while OFF; the rest are retained
    // as local preferences for the next ON command.
    this->awaiting_command_observable_fields_ &=
        ~TCLAC_OFF_UNOBSERVABLE_FIELDS;
  }
  this->state_ = command_state;
  this->pending_fields_ = 0;
  this->pending_off_reset_fields_ = 0;
  this->last_tx_ms_ = millis();
  this->awaiting_response_ = true;
  this->awaiting_command_status_ = true;
  this->command_confirmation_misses_ = 0;
  ESP_LOGD(TAG, "TX combined control command (%u bytes, profile %s, checksum 0x%02X)",
           frame.size, tcl_protocol_profile_name(this->active_profile_),
           frame.bytes[frame.size - 1]);
  return true;
}

bool TclClimate::status_is_fresh_() const {
  return this->has_valid_status_ && !this->status_timed_out_ &&
         !this->awaiting_command_status_ &&
         millis() - this->last_status_ms_ < this->status_timeout_ms_;
}

bool TclClimate::bus_is_quiet_() {
  // update() can run before loop() has drained the hardware FIFO.
  if (this->available() > 0 || this->parser_.has_partial_frame())
    return false;
  return this->last_rx_byte_ms_ == 0 ||
         millis() - this->last_rx_byte_ms_ >= this->inter_byte_timeout_ms_;
}

bool TclClimate::status_confirms_command_(const TclProtocolState &state) const {
  const uint32_t fields = this->awaiting_command_observable_fields_;
  const auto &expected = this->awaiting_command_state_;
  if ((fields & PENDING_POWER) && state.power != expected.power)
    return false;
  if ((fields & PENDING_MODE) && state.mode != expected.mode)
    return false;
  if ((fields & PENDING_TARGET) &&
      std::fabs(state.target_temperature - expected.target_temperature) > 0.26f)
    return false;
  if ((fields & PENDING_FAN) && state.fan != expected.fan)
    return false;
  if ((fields & PENDING_DISPLAY) && state.display != expected.display)
    return false;
  if ((fields & PENDING_ECO) && state.eco != expected.eco)
    return false;
  if ((fields & PENDING_TURBO) && state.turbo != expected.turbo)
    return false;
  if ((fields & PENDING_HEALTH) && state.health != expected.health)
    return false;
  if ((fields & PENDING_HORIZONTAL_SWING) &&
      state.horizontal_swing != expected.horizontal_swing)
    return false;
  if ((fields & PENDING_VERTICAL_SWING) &&
      state.vertical_swing != expected.vertical_swing)
    return false;
  if ((fields & PENDING_SLEEP) && state.sleep != expected.sleep)
    return false;
  if ((fields & PENDING_MUTE) && state.mute != expected.mute)
    return false;
  // Beep is a write-only policy on the known status frames.
  return true;
}

void TclClimate::queue_switch_change(const TclSwitchType type, const bool state) {
  switch (type) {
    case TclSwitchType::DISPLAY_CONTROL:
      this->requested_state_.display = state;
      this->pending_fields_ |= PENDING_DISPLAY;
      break;
    case TclSwitchType::BEEP_CONTROL:
      this->requested_state_.beep = state;
      this->pending_fields_ |= PENDING_BEEP;
      break;
    case TclSwitchType::HEALTH_CONTROL:
      this->requested_state_.health = state;
      this->pending_fields_ |= PENDING_HEALTH;
      break;
  }
}

void TclClimate::control(const climate::ClimateCall &call) {
  const bool tclac_profile =
      this->active_profile_ == TclProtocolProfile::PROFILE_TCLAC_38;
  const bool explicit_fan = call.get_fan_mode().has_value();
  bool requested_off = false;
  bool requested_working_mode = false;

  if (const auto requested_mode = call.get_mode(); requested_mode.has_value()) {
    switch (*requested_mode) {
      case climate::CLIMATE_MODE_OFF:
        requested_off = true;
        this->requested_state_.power = false;
        this->pending_fields_ |= PENDING_POWER;
        if (!tclac_profile) {
          this->requested_state_.fan = 0x00;
          this->requested_state_.sleep = false;
          this->requested_state_.turbo = false;
          this->requested_state_.eco = false;
          this->pending_fields_ |=
              PENDING_FAN | PENDING_SLEEP | PENDING_TURBO | PENDING_ECO;
          this->pending_off_reset_fields_ |= LEGACY_OFF_RESET_FIELDS;
        }
        break;
      case climate::CLIMATE_MODE_COOL:
        requested_working_mode = true;
        this->requested_state_.power = true;
        this->requested_state_.mode = 0x01;
        this->pending_fields_ |= PENDING_POWER | PENDING_MODE;
        break;
      case climate::CLIMATE_MODE_FAN_ONLY:
        requested_working_mode = true;
        this->requested_state_.power = true;
        this->requested_state_.mode = 0x02;
        this->pending_fields_ |= PENDING_POWER | PENDING_MODE;
        break;
      case climate::CLIMATE_MODE_DRY:
        requested_working_mode = true;
        this->requested_state_.power = true;
        this->requested_state_.mode = 0x03;
        this->pending_fields_ |= PENDING_POWER | PENDING_MODE;
        break;
      case climate::CLIMATE_MODE_HEAT:
        if (this->supports_heat_) {
          requested_working_mode = true;
          this->requested_state_.power = true;
          this->requested_state_.mode = 0x04;
          this->pending_fields_ |= PENDING_POWER | PENDING_MODE;
        }
        break;
      case climate::CLIMATE_MODE_AUTO:
        requested_working_mode = true;
        this->requested_state_.power = true;
        this->requested_state_.mode = 0x05;
        this->pending_fields_ |= PENDING_POWER | PENDING_MODE;
        break;
      default:
        ESP_LOGW(TAG, "Ignoring unsupported climate mode");
        break;
    }
    if (requested_working_mode && !tclac_profile) {
      // OFF carries legacy reset side effects. If it is superseded before
      // transmission, remove only those synthetic changes; later explicit
      // fan/preset calls have already removed their bits from this mask.
      this->pending_fields_ &= ~this->pending_off_reset_fields_;
      this->pending_off_reset_fields_ = 0;
    }
  }

  if (const auto requested_target = call.get_target_temperature(); requested_target.has_value()) {
    const float step = tcl_protocol_target_step(this->active_profile_);
    const float maximum = step == 0.5f ? 31.5f : 31.0f;
    const float bounded = std::max(16.0f, std::min(maximum, *requested_target));
    this->requested_state_.target_temperature = std::round(bounded / step) * step;
    this->pending_fields_ |= PENDING_TARGET;
  }

  if (const auto requested_swing = call.get_swing_mode(); requested_swing.has_value()) {
    switch (*requested_swing) {
      case climate::CLIMATE_SWING_OFF:
        this->requested_state_.vertical_swing = false;
        this->pending_fields_ |= PENDING_VERTICAL_SWING;
        if (this->supports_horizontal_swing_) {
          this->requested_state_.horizontal_swing = false;
          this->pending_fields_ |= PENDING_HORIZONTAL_SWING;
        }
        break;
      case climate::CLIMATE_SWING_VERTICAL:
        this->requested_state_.vertical_swing = true;
        this->pending_fields_ |= PENDING_VERTICAL_SWING;
        if (this->supports_horizontal_swing_) {
          this->requested_state_.horizontal_swing = false;
          this->pending_fields_ |= PENDING_HORIZONTAL_SWING;
        }
        break;
      case climate::CLIMATE_SWING_HORIZONTAL:
        if (this->supports_horizontal_swing_) {
          this->requested_state_.vertical_swing = false;
          this->requested_state_.horizontal_swing = true;
          this->pending_fields_ |= PENDING_VERTICAL_SWING | PENDING_HORIZONTAL_SWING;
        }
        break;
      case climate::CLIMATE_SWING_BOTH:
        if (this->supports_horizontal_swing_) {
          this->requested_state_.vertical_swing = true;
          this->requested_state_.horizontal_swing = true;
          this->pending_fields_ |= PENDING_VERTICAL_SWING | PENDING_HORIZONTAL_SWING;
        }
        break;
    }
  }

  if (const auto requested_fan = call.get_fan_mode(); requested_fan.has_value()) {
    bool supported = true;
    uint8_t fan = 0x00;
    bool mute = false;
    bool turbo = false;
    switch (*requested_fan) {
      case climate::CLIMATE_FAN_QUIET:
        fan = tclac_profile ? 0x00 : 0x01;
        mute = true;
        break;
      case climate::CLIMATE_FAN_AUTO:
        fan = 0x00;
        break;
      case climate::CLIMATE_FAN_LOW:
        fan = 0x01;
        break;
      case climate::CLIMATE_FAN_MEDIUM:
        fan = 0x02;
        break;
      case climate::CLIMATE_FAN_HIGH:
        fan = tclac_profile ? 0x05 : 0x03;
        break;
      case climate::CLIMATE_FAN_MIDDLE:
        if (tclac_profile)
          fan = 0x04;
        else
          supported = false;
        break;
      case climate::CLIMATE_FAN_FOCUS:
        if (tclac_profile)
          fan = 0x03;
        else
          supported = false;
        break;
      case climate::CLIMATE_FAN_DIFFUSE:
        if (tclac_profile) {
          fan = 0x00;
          turbo = true;
        } else {
          supported = false;
        }
        break;
      default:
        supported = false;
        break;
    }
    if (supported) {
      this->requested_state_.fan = fan;
      this->requested_state_.turbo = turbo;
      this->requested_state_.mute = mute;
      this->pending_fields_ |= PENDING_FAN | PENDING_TURBO | PENDING_MUTE;
      this->pending_off_reset_fields_ &= ~(PENDING_FAN | PENDING_TURBO);
    } else {
      ESP_LOGW(TAG, "Ignoring unsupported fan mode");
    }
  }

  // Preset is intentionally independent from fan mode so a combined ClimateCall is not lost.
  if (const auto requested_preset = call.get_preset(); requested_preset.has_value()) {
    bool supported = true;
    uint32_t explicit_preset_fields = 0;
    switch (*requested_preset) {
      case climate::CLIMATE_PRESET_SLEEP:
        this->requested_state_.eco = false;
        this->requested_state_.sleep = true;
        this->pending_fields_ |= PENDING_ECO | PENDING_SLEEP;
        explicit_preset_fields |= PENDING_ECO | PENDING_SLEEP;
        if (tclac_profile) {
          if (this->health_switch_ == nullptr) {
            this->requested_state_.health = false;
            this->pending_fields_ |= PENDING_HEALTH;
          }
        } else {
          this->requested_state_.turbo = false;
          this->pending_fields_ |= PENDING_TURBO;
          explicit_preset_fields |= PENDING_TURBO;
        }
        break;
      case climate::CLIMATE_PRESET_NONE:
        this->requested_state_.sleep = false;
        this->requested_state_.eco = false;
        this->pending_fields_ |= PENDING_SLEEP | PENDING_ECO;
        explicit_preset_fields |= PENDING_SLEEP | PENDING_ECO;
        if (tclac_profile) {
          if (this->health_switch_ == nullptr) {
            this->requested_state_.health = false;
            this->pending_fields_ |= PENDING_HEALTH;
          }
        } else {
          this->requested_state_.turbo = false;
          this->pending_fields_ |= PENDING_TURBO;
          explicit_preset_fields |= PENDING_TURBO;
          if (!explicit_fan) {
            this->requested_state_.fan = 0x00;
            this->pending_fields_ |= PENDING_FAN;
            explicit_preset_fields |= PENDING_FAN;
          }
        }
        break;
      case climate::CLIMATE_PRESET_ECO:
        this->requested_state_.sleep = false;
        this->requested_state_.eco = true;
        this->pending_fields_ |= PENDING_SLEEP | PENDING_ECO;
        explicit_preset_fields |= PENDING_SLEEP | PENDING_ECO;
        if (tclac_profile) {
          if (this->health_switch_ == nullptr) {
            this->requested_state_.health = false;
            this->pending_fields_ |= PENDING_HEALTH;
          }
        } else {
          this->requested_state_.turbo = false;
          this->pending_fields_ |= PENDING_TURBO;
          explicit_preset_fields |= PENDING_TURBO;
          if (!explicit_fan) {
            this->requested_state_.fan = 0x00;
            this->pending_fields_ |= PENDING_FAN;
            explicit_preset_fields |= PENDING_FAN;
          }
        }
        break;
      case climate::CLIMATE_PRESET_BOOST:
        if (tclac_profile) {
          supported = false;
        } else {
          this->requested_state_.fan = 0x03;
          this->requested_state_.sleep = false;
          this->requested_state_.turbo = true;
          this->requested_state_.eco = false;
          this->pending_fields_ |= PENDING_FAN | PENDING_SLEEP | PENDING_TURBO | PENDING_ECO;
          explicit_preset_fields |=
              PENDING_FAN | PENDING_SLEEP | PENDING_TURBO | PENDING_ECO;
        }
        break;
      case climate::CLIMATE_PRESET_COMFORT:
        if (tclac_profile && this->health_switch_ == nullptr) {
          this->requested_state_.eco = false;
          this->requested_state_.sleep = false;
          this->requested_state_.health = true;
          this->pending_fields_ |= PENDING_ECO | PENDING_SLEEP | PENDING_HEALTH;
        } else {
          supported = false;
        }
        break;
      default:
        supported = false;
        break;
    }
    if (supported) {
      this->pending_off_reset_fields_ &= ~explicit_preset_fields;
      if (!tclac_profile &&
          (!explicit_fan || *requested_preset == climate::CLIMATE_PRESET_BOOST)) {
        this->requested_state_.mute = false;
        this->pending_fields_ |= PENDING_MUTE;
      }
    } else {
      ESP_LOGW(TAG, "Ignoring unsupported preset");
    }
  }

  // Climate restore calls contain mode, fan and preset together. For the
  // proven legacy profile OFF must win over those later fields; tclac keeps
  // fan/preset internally while encoding an explicit zero OFF mode.
  if (requested_off && !tclac_profile) {
    this->requested_state_.power = false;
    this->requested_state_.fan = 0x00;
    this->requested_state_.sleep = false;
    this->requested_state_.turbo = false;
    this->requested_state_.eco = false;
    this->pending_fields_ |= PENDING_POWER | PENDING_FAN | PENDING_SLEEP |
                             PENDING_TURBO | PENDING_ECO;
    this->pending_off_reset_fields_ |= LEGACY_OFF_RESET_FIELDS;
  }
}

void TclClimate::apply_pending_fields_(TclProtocolState &state) const {
  this->apply_fields_(state, this->requested_state_, this->pending_fields_);
}

void TclClimate::apply_fields_(TclProtocolState &target,
                               const TclProtocolState &source,
                               const uint32_t fields) const {
  if (fields & PENDING_POWER)
    target.power = source.power;
  if (fields & PENDING_MODE)
    target.mode = source.mode;
  if (fields & PENDING_TARGET)
    target.target_temperature = source.target_temperature;
  if (fields & PENDING_FAN)
    target.fan = source.fan;
  if (fields & PENDING_DISPLAY)
    target.display = source.display;
  if (fields & PENDING_ECO)
    target.eco = source.eco;
  if (fields & PENDING_TURBO)
    target.turbo = source.turbo;
  if (fields & PENDING_HEALTH)
    target.health = source.health;
  if (fields & PENDING_HORIZONTAL_SWING)
    target.horizontal_swing = source.horizontal_swing;
  if (fields & PENDING_VERTICAL_SWING)
    target.vertical_swing = source.vertical_swing;
  if (fields & PENDING_SLEEP)
    target.sleep = source.sleep;
  if (fields & PENDING_MUTE)
    target.mute = source.mute;
  if (fields & PENDING_BEEP)
    target.beep = source.beep;
}

float TclClimate::add_temperature_sample_(const float value) {
  const uint8_t window = std::max<uint8_t>(
      1U, std::min<uint8_t>(this->temperature_window_size_, MAX_TEMPERATURE_SAMPLES));

  if (this->temperature_sample_count_ < window) {
    this->temperature_samples_[this->temperature_sample_index_] = value;
    this->temperature_sample_sum_ += value;
    this->temperature_sample_count_++;
  } else {
    this->temperature_sample_sum_ -= this->temperature_samples_[this->temperature_sample_index_];
    this->temperature_samples_[this->temperature_sample_index_] = value;
    this->temperature_sample_sum_ += value;
  }

  this->temperature_sample_index_ =
      static_cast<uint8_t>((this->temperature_sample_index_ + 1U) % window);
  return this->temperature_sample_sum_ / static_cast<float>(this->temperature_sample_count_);
}

void TclClimate::publish_protocol_state_() {
  if (!this->state_.power) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch (this->state_.mode) {
      case 0x01:
        this->mode = climate::CLIMATE_MODE_COOL;
        break;
      case 0x02:
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case 0x03:
        this->mode = climate::CLIMATE_MODE_DRY;
        break;
      case 0x04:
        if (this->supports_heat_)
          this->mode = climate::CLIMATE_MODE_HEAT;
        break;
      case 0x05:
        this->mode = climate::CLIMATE_MODE_AUTO;
        break;
      default:
        ESP_LOGW(TAG, "Unknown TCL status mode 0x%02X", this->state_.mode);
        break;
    }
  }

  if (!this->state_.power) {
    this->action = climate::CLIMATE_ACTION_OFF;
  } else if (this->state_.compressor_state == 0x8A) {
    this->action = climate::CLIMATE_ACTION_COOLING;
  } else if (this->supports_heat_ && this->state_.compressor_state == 0xCA) {
    this->action = climate::CLIMATE_ACTION_HEATING;
  } else if (this->state_.mode == 0x02) {
    this->action = climate::CLIMATE_ACTION_FAN;
  } else if (this->state_.mode == 0x03) {
    this->action = climate::CLIMATE_ACTION_DRYING;
  } else {
    // Includes both known idle codes from the original protocol notes: 0x80 and 0xC0.
    this->action = climate::CLIMATE_ACTION_IDLE;
  }

  const bool tclac_profile =
      this->active_profile_ == TclProtocolProfile::PROFILE_TCLAC_38;
  if (tclac_profile) {
    if (this->state_.eco)
      this->preset = climate::CLIMATE_PRESET_ECO;
    else if (this->health_switch_ == nullptr && this->state_.health)
      this->preset = climate::CLIMATE_PRESET_COMFORT;
    else if (this->state_.sleep)
      this->preset = climate::CLIMATE_PRESET_SLEEP;
    else
      this->preset = climate::CLIMATE_PRESET_NONE;
  } else {
    if (this->state_.turbo)
      this->preset = climate::CLIMATE_PRESET_BOOST;
    else if (this->state_.sleep)
      this->preset = climate::CLIMATE_PRESET_SLEEP;
    else if (this->state_.eco)
      this->preset = climate::CLIMATE_PRESET_ECO;
    else
      this->preset = climate::CLIMATE_PRESET_NONE;
  }

  if (this->state_.mute) {
    this->fan_mode = climate::CLIMATE_FAN_QUIET;
  } else if (tclac_profile && this->state_.turbo) {
    this->fan_mode = climate::CLIMATE_FAN_DIFFUSE;
  } else if (this->state_.fan == 0x00) {
    this->fan_mode = climate::CLIMATE_FAN_AUTO;
  } else if (this->state_.fan == 0x01) {
    this->fan_mode = climate::CLIMATE_FAN_LOW;
  } else if (this->state_.fan == 0x02) {
    this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
  } else if (this->state_.fan == 0x03) {
    this->fan_mode =
        tclac_profile ? climate::CLIMATE_FAN_FOCUS : climate::CLIMATE_FAN_HIGH;
  } else if (tclac_profile && this->state_.fan == 0x04) {
    this->fan_mode = climate::CLIMATE_FAN_MIDDLE;
  } else if (tclac_profile && this->state_.fan == 0x05) {
    this->fan_mode = climate::CLIMATE_FAN_HIGH;
  } else {
    ESP_LOGW(TAG, "Unknown TCL status fan code 0x%02X", this->state_.fan);
  }

  if (this->supports_horizontal_swing_) {
    if (this->state_.vertical_swing && this->state_.horizontal_swing)
      this->swing_mode = climate::CLIMATE_SWING_BOTH;
    else if (this->state_.vertical_swing)
      this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
    else if (this->state_.horizontal_swing)
      this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
    else
      this->swing_mode = climate::CLIMATE_SWING_OFF;
  } else {
    this->swing_mode =
        this->state_.vertical_swing ? climate::CLIMATE_SWING_VERTICAL : climate::CLIMATE_SWING_OFF;
  }

  this->target_temperature = this->state_.target_temperature;
  this->current_temperature = this->state_.current_temperature;

  if (!(this->pending_fields_ & PENDING_DISPLAY) &&
      (!tclac_profile || this->state_.power))
    publish_switch_if_changed(this->display_switch_, this->state_.display);
  if ((this->state_.power || tclac_profile) &&
      !(this->pending_fields_ & PENDING_HEALTH))
    publish_switch_if_changed(this->health_switch_, this->state_.health);

  publish_sensor_if_changed(this->current_sensor_, this->state_.compressor_current, 0.01f);
  publish_sensor_if_changed(this->supply_voltage_sensor_, this->state_.supply_voltage);
  publish_sensor_if_changed(this->pipe_in_temperature_sensor_,
                            static_cast<float>(static_cast<int>(this->state_.pipe_in_temperature) - 32));
  publish_sensor_if_changed(this->pipe_out_temperature_sensor_,
                            static_cast<float>(static_cast<int>(this->state_.pipe_out_temperature) - 32));
  publish_sensor_if_changed(this->outside_motor_sensor_, this->state_.outside_motor);
  publish_sensor_if_changed(this->fan_speed_raw_sensor_, this->state_.fan_speed);
  publish_sensor_if_changed(this->compressor_state_sensor_, this->state_.compressor_state);
  if (!tclac_profile) {
    publish_sensor_if_changed(this->vertical_vane_position_sensor_,
                              this->state_.vertical_vane_position);
    publish_sensor_if_changed(this->horizontal_vane_position_sensor_,
                              this->state_.horizontal_vane_position);
  }

  const char *fan_speed_text = "APAGADO";
  if (this->state_.fan_speed > 117)
    fan_speed_text = "TURBO";
  else if (this->state_.fan_speed >= 99)
    fan_speed_text = "ALTO";
  else if (this->state_.fan_speed >= 86)
    fan_speed_text = "MEDIO";
  else if (this->state_.fan_speed > 0)
    fan_speed_text = "BAJO";
  publish_text_if_changed(this->fan_speed_text_sensor_, fan_speed_text);

  if (this->state_.fault == 0) {
    publish_text_if_changed(this->fault_text_sensor_, "SIN FALLAS");
  } else {
    char fault_text[16];
    std::snprintf(fault_text, sizeof(fault_text), "FALLA %02X", this->state_.fault);
    publish_text_if_changed(this->fault_text_sensor_, fault_text);
  }

  const bool deep_sleep =
      this->deep_sleep_active_low_ ? !this->state_.deep_sleep_bit : this->state_.deep_sleep_bit;
  publish_binary_if_changed(this->deep_sleep_binary_sensor_, deep_sleep);
  publish_binary_if_changed(this->clean_filter_binary_sensor_, this->state_.clean_filter);
  this->publish_state();
}

void TclClimate::publish_profile_state_() {
  if (this->protocol_profile_text_sensor_ == nullptr)
    return;
  char value[64];
  if (this->active_status_frame_size_ == 0) {
    std::snprintf(value, sizeof(value), "TX %s / RX AUTO (esperando)",
                  tcl_protocol_profile_name(this->active_profile_));
  } else {
    std::snprintf(value, sizeof(value), "TX %s / RX %u",
                  tcl_protocol_profile_name(this->active_profile_),
                  this->active_status_frame_size_);
  }
  publish_text_if_changed(this->protocol_profile_text_sensor_, value);
}

void TclClimate::restore_switch_(switch_::Switch *entity, const TclSwitchType type) {
  if (entity == nullptr)
    return;
  const auto initial = entity->get_initial_state_with_restore_mode();
  if (!initial.has_value())
    return;
  this->queue_switch_change(type, *initial);
  entity->publish_state(*initial);
}

}  // namespace esphome::tcl_climate
