#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "tcl_protocol.h"

namespace esphome::tcl_climate {

enum class TclSwitchType : uint8_t {
  DISPLAY_CONTROL,
  BEEP_CONTROL,
  HEALTH_CONTROL,
};

class TclClimate final : public climate::Climate,
                         public PollingComponent,
                         public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  void set_supports_heat(bool value) { this->supports_heat_ = value; }
  void set_supports_horizontal_swing(bool value) { this->supports_horizontal_swing_ = value; }
  void set_protocol_profile(TclProtocolProfile value) {
    this->configured_profile_ = value;
    this->active_profile_ = value;
  }
  void set_status_frame_size(uint8_t value) {
    this->configured_status_frame_size_ = value;
    this->active_status_frame_size_ = value;
  }
  void set_restore_state(bool value) { this->restore_state_enabled_ = value; }
  void set_deep_sleep_active_low(bool value) { this->deep_sleep_active_low_ = value; }
  void set_status_timeout(uint32_t value) { this->status_timeout_ms_ = value; }
  void set_inter_byte_timeout(uint32_t value) { this->inter_byte_timeout_ms_ = value; }
  void set_temperature_samples(uint8_t value) { this->temperature_window_size_ = value; }

  void set_display_switch(switch_::Switch *value) { this->display_switch_ = value; }
  void set_beep_switch(switch_::Switch *value) { this->beep_switch_ = value; }
  void set_health_switch(switch_::Switch *value) { this->health_switch_ = value; }

  void set_current_sensor(sensor::Sensor *value) { this->current_sensor_ = value; }
  void set_supply_voltage_sensor(sensor::Sensor *value) { this->supply_voltage_sensor_ = value; }
  void set_pipe_in_temperature_sensor(sensor::Sensor *value) { this->pipe_in_temperature_sensor_ = value; }
  void set_pipe_out_temperature_sensor(sensor::Sensor *value) { this->pipe_out_temperature_sensor_ = value; }
  void set_outside_motor_sensor(sensor::Sensor *value) { this->outside_motor_sensor_ = value; }
  void set_fan_speed_raw_sensor(sensor::Sensor *value) { this->fan_speed_raw_sensor_ = value; }
  void set_compressor_state_sensor(sensor::Sensor *value) { this->compressor_state_sensor_ = value; }
  void set_vertical_vane_position_sensor(sensor::Sensor *value) {
    this->vertical_vane_position_sensor_ = value;
  }
  void set_horizontal_vane_position_sensor(sensor::Sensor *value) {
    this->horizontal_vane_position_sensor_ = value;
  }

  void set_fan_speed_text_sensor(text_sensor::TextSensor *value) { this->fan_speed_text_sensor_ = value; }
  void set_fault_text_sensor(text_sensor::TextSensor *value) { this->fault_text_sensor_ = value; }
  void set_protocol_profile_text_sensor(text_sensor::TextSensor *value) {
    this->protocol_profile_text_sensor_ = value;
  }

  void set_deep_sleep_binary_sensor(binary_sensor::BinarySensor *value) {
    this->deep_sleep_binary_sensor_ = value;
  }
  void set_clean_filter_binary_sensor(binary_sensor::BinarySensor *value) {
    this->clean_filter_binary_sensor_ = value;
  }

  void queue_switch_change(TclSwitchType type, bool state);

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  enum PendingField : uint32_t {
    PENDING_POWER = 1U << 0,
    PENDING_MODE = 1U << 1,
    PENDING_TARGET = 1U << 2,
    PENDING_FAN = 1U << 3,
    PENDING_DISPLAY = 1U << 4,
    PENDING_ECO = 1U << 5,
    PENDING_TURBO = 1U << 6,
    PENDING_HEALTH = 1U << 7,
    PENDING_HORIZONTAL_SWING = 1U << 8,
    PENDING_VERTICAL_SWING = 1U << 9,
    PENDING_SLEEP = 1U << 10,
    PENDING_MUTE = 1U << 11,
    PENDING_BEEP = 1U << 12,
  };

  void process_rx_byte_(uint8_t byte);
  void handle_frame_(const uint8_t *data, size_t length);
  void invalidate_status_(const char *reason);
  bool send_pending_command_();
  void send_status_request_();
  void apply_pending_fields_(TclProtocolState &state) const;
  void apply_fields_(TclProtocolState &target, const TclProtocolState &source,
                     uint32_t fields) const;
  void publish_protocol_state_();
  void publish_profile_state_();
  float add_temperature_sample_(float value);
  bool status_is_fresh_() const;
  bool bus_is_quiet_();
  bool status_confirms_command_(const TclProtocolState &state) const;
  void restore_switch_(switch_::Switch *entity, TclSwitchType type);

  TclFrameParser parser_{};
  TclProtocolState state_{};
  TclProtocolState requested_state_{};
  TclProtocolState awaiting_command_state_{};
  uint32_t pending_fields_{0};
  uint32_t awaiting_command_fields_{0};
  uint32_t awaiting_command_observable_fields_{0};
  uint32_t deferred_fields_{0};
  uint32_t awaiting_deferred_fields_{0};
  uint32_t pending_off_reset_fields_{0};

  bool supports_heat_{false};
  bool supports_horizontal_swing_{false};
  bool deep_sleep_active_low_{true};
  bool restore_state_enabled_{false};
  bool has_valid_status_{false};
  bool status_timed_out_{false};
  TclProtocolProfile configured_profile_{TclProtocolProfile::PROFILE_TCL_35};
  TclProtocolProfile active_profile_{TclProtocolProfile::PROFILE_TCL_35};
  uint8_t configured_status_frame_size_{0};
  uint8_t active_status_frame_size_{0};
  uint32_t status_timeout_ms_{5000};
  uint32_t inter_byte_timeout_ms_{50};
  uint32_t last_status_ms_{0};
  uint32_t last_rx_byte_ms_{0};
  uint32_t last_tx_ms_{0};
  bool awaiting_response_{false};
  bool awaiting_command_status_{false};
  uint8_t command_confirmation_misses_{0};

  static constexpr uint32_t RESPONSE_WINDOW_MS = 400;
  static constexpr uint8_t MAX_COMMAND_CONFIRMATION_MISSES = 3;
  static constexpr uint32_t TCLAC_OFF_UNOBSERVABLE_FIELDS =
      PENDING_MODE | PENDING_FAN | PENDING_DISPLAY | PENDING_ECO |
      PENDING_TURBO | PENDING_HEALTH | PENDING_HORIZONTAL_SWING |
      PENDING_VERTICAL_SWING | PENDING_SLEEP | PENDING_MUTE;
  static constexpr uint32_t LEGACY_OFF_RESET_FIELDS =
      PENDING_FAN | PENDING_ECO | PENDING_TURBO | PENDING_SLEEP;

  static constexpr size_t MAX_TEMPERATURE_SAMPLES = 20;
  std::array<float, MAX_TEMPERATURE_SAMPLES> temperature_samples_{};
  uint8_t temperature_window_size_{10};
  uint8_t temperature_sample_count_{0};
  uint8_t temperature_sample_index_{0};
  float temperature_sample_sum_{0.0f};

  switch_::Switch *display_switch_{nullptr};
  switch_::Switch *beep_switch_{nullptr};
  switch_::Switch *health_switch_{nullptr};

  sensor::Sensor *current_sensor_{nullptr};
  sensor::Sensor *supply_voltage_sensor_{nullptr};
  sensor::Sensor *pipe_in_temperature_sensor_{nullptr};
  sensor::Sensor *pipe_out_temperature_sensor_{nullptr};
  sensor::Sensor *outside_motor_sensor_{nullptr};
  sensor::Sensor *fan_speed_raw_sensor_{nullptr};
  sensor::Sensor *compressor_state_sensor_{nullptr};
  sensor::Sensor *vertical_vane_position_sensor_{nullptr};
  sensor::Sensor *horizontal_vane_position_sensor_{nullptr};

  text_sensor::TextSensor *fan_speed_text_sensor_{nullptr};
  text_sensor::TextSensor *fault_text_sensor_{nullptr};
  text_sensor::TextSensor *protocol_profile_text_sensor_{nullptr};
  binary_sensor::BinarySensor *deep_sleep_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *clean_filter_binary_sensor_{nullptr};
};

}  // namespace esphome::tcl_climate
