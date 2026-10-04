#pragma once

// Narrow host doubles for compiling the real TclClimate/TclSwitch code. In
// particular Switch publication deliberately does NOT set EntityBase has_state,
// matching ESPHome 2026.9.0 and reproducing the production regression.
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
template<typename T> using optional = std::optional<T>;
inline uint32_t test_millis = 1000;
inline uint32_t millis() { return test_millis; }
template<typename... Args> void test_log(Args &&...) {}
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  void status_set_warning() {}
  void status_clear_warning() {}
};
class PollingComponent : public Component {
 public:
  virtual void update() {}
};
template<typename T> class Parented {
 public:
  void set_parent(T *parent) { parent_ = parent; }
 protected:
  T *parent_{nullptr};
};
class ESPPreferences {
 public:
  unsigned sync_count{0};
  bool sync() { ++sync_count; return true; }
};
inline ESPPreferences *global_preferences = nullptr;

namespace switch_ {
constexpr uint8_t RESTORE_MODE_PERSISTENT_MASK = 2;
constexpr uint8_t RESTORE_MODE_INVERTED_MASK = 4;
constexpr uint8_t RESTORE_MODE_DISABLED_MASK = 8;
constexpr uint8_t SWITCH_ALWAYS_OFF = 0;
constexpr uint8_t SWITCH_ALWAYS_ON = 1;
constexpr uint8_t SWITCH_RESTORE_DEFAULT_OFF = 2;
constexpr uint8_t SWITCH_RESTORE_DEFAULT_ON = 3;
constexpr uint8_t SWITCH_RESTORE_INVERTED_DEFAULT_OFF = 6;
constexpr uint8_t SWITCH_RESTORE_INVERTED_DEFAULT_ON = 7;
constexpr uint8_t SWITCH_RESTORE_DISABLED = 8;
class Switch {
 public:
  virtual ~Switch() = default;
  bool state{false};
  uint8_t restore_mode{SWITCH_RESTORE_DEFAULT_OFF};
  optional<bool> persisted{};
  optional<bool> published{};
  unsigned publications{0};
  std::function<void(bool)> on_state{};
  bool has_state() const { return false; }
  void publish_state(bool value) {
    if (published.has_value() && *published == value) return;
    state = value;
    published = value;
    ++publications;
    if (restore_mode & RESTORE_MODE_PERSISTENT_MASK) persisted = value;
    if (on_state) on_state(value);
  }
  optional<bool> get_initial_state_with_restore_mode() const {
    if (restore_mode & RESTORE_MODE_DISABLED_MASK) return {};
    if ((restore_mode & RESTORE_MODE_PERSISTENT_MASK) && persisted.has_value())
      return (restore_mode & RESTORE_MODE_INVERTED_MASK) ? !*persisted : *persisted;
    return (restore_mode & 1) != 0;
  }
  void turn_on() { write_state(true); }
  void turn_off() { write_state(false); }
 protected:
  virtual void write_state(bool value) = 0;
};
}
namespace sensor {
class Sensor {
 public:
  float state{NAN};
  bool has_state() const { return !std::isnan(state); }
  void publish_state(float value) { state = value; }
};
}
namespace binary_sensor {
class BinarySensor {
 public:
  bool state{false}, known{false};
  bool has_state() const { return known; }
  void publish_state(bool value) { state = value; known = true; }
};
}
namespace text_sensor {
class TextSensor {
 public:
  std::string state;
  bool known{false};
  bool has_state() const { return known; }
  void publish_state(const char *value) { state = value; known = true; }
};
}
namespace uart {
constexpr uint8_t UART_CONFIG_PARITY_EVEN = 2;
class UARTDevice {
 public:
  std::vector<std::vector<uint8_t>> sent_frames;
  int available() { return 0; }
  bool read_byte(uint8_t *) { return false; }
  void write_array(const uint8_t *bytes, size_t size) { sent_frames.emplace_back(bytes, bytes + size); }
  template<size_t N> void write_array(const std::array<uint8_t, N> &bytes) { write_array(bytes.data(), N); }
  void check_uart_settings(int, int, int, int) {}
};
}
namespace climate {
enum ClimateMode { CLIMATE_MODE_OFF, CLIMATE_MODE_COOL, CLIMATE_MODE_HEAT, CLIMATE_MODE_FAN_ONLY,
                   CLIMATE_MODE_DRY, CLIMATE_MODE_AUTO };
enum ClimateAction { CLIMATE_ACTION_OFF, CLIMATE_ACTION_COOLING, CLIMATE_ACTION_HEATING,
                     CLIMATE_ACTION_FAN, CLIMATE_ACTION_DRYING, CLIMATE_ACTION_IDLE };
enum ClimateFanMode { CLIMATE_FAN_AUTO, CLIMATE_FAN_QUIET, CLIMATE_FAN_LOW, CLIMATE_FAN_MEDIUM,
                      CLIMATE_FAN_HIGH, CLIMATE_FAN_MIDDLE, CLIMATE_FAN_FOCUS, CLIMATE_FAN_DIFFUSE };
enum ClimatePreset { CLIMATE_PRESET_NONE, CLIMATE_PRESET_ECO, CLIMATE_PRESET_SLEEP,
                     CLIMATE_PRESET_BOOST, CLIMATE_PRESET_COMFORT };
enum ClimateSwingMode { CLIMATE_SWING_OFF, CLIMATE_SWING_VERTICAL, CLIMATE_SWING_HORIZONTAL,
                        CLIMATE_SWING_BOTH };
constexpr int CLIMATE_SUPPORTS_CURRENT_TEMPERATURE = 1, CLIMATE_SUPPORTS_ACTION = 2;
class ClimateTraits {
 public:
  void add_feature_flags(int) {}
  void add_supported_mode(ClimateMode) {}
  void add_supported_fan_mode(ClimateFanMode) {}
  void add_supported_preset(ClimatePreset) {}
  void add_supported_swing_mode(ClimateSwingMode) {}
  void set_visual_min_temperature(float) {}
  void set_visual_max_temperature(float) {}
  void set_visual_target_temperature_step(float) {}
  void set_visual_current_temperature_step(float) {}
};
class Climate;
class ClimateCall {
 public:
  explicit ClimateCall(Climate *parent = nullptr) : parent_(parent) {}
  optional<ClimateMode> mode;
  optional<ClimateFanMode> fan;
  optional<ClimatePreset> preset;
  optional<ClimateSwingMode> swing;
  optional<float> target;
  optional<ClimateMode> get_mode() const { return mode; }
  optional<ClimateFanMode> get_fan_mode() const { return fan; }
  optional<ClimatePreset> get_preset() const { return preset; }
  optional<ClimateSwingMode> get_swing_mode() const { return swing; }
  optional<float> get_target_temperature() const { return target; }
  void perform();
 private:
  Climate *parent_;
};
struct ClimateRestoreState {
  bool uses_custom_fan_mode{false}, uses_custom_preset{false};
  ClimateFanMode fan_mode{CLIMATE_FAN_AUTO};
  ClimatePreset preset{CLIMATE_PRESET_NONE};
  ClimateCall to_call(Climate *parent) const { return ClimateCall(parent); }
};
class Climate {
 public:
  virtual ~Climate() = default;
  ClimateMode mode{CLIMATE_MODE_OFF};
  ClimateAction action{CLIMATE_ACTION_OFF};
  ClimateSwingMode swing_mode{CLIMATE_SWING_OFF};
  optional<ClimateFanMode> fan_mode;
  optional<ClimatePreset> preset;
  float target_temperature{NAN}, current_temperature{NAN};
  unsigned publications{0};
  virtual ClimateTraits traits() = 0;
  virtual void control(const ClimateCall &) = 0;
  optional<ClimateRestoreState> restore_state_() { return {}; }
  void publish_state() { ++publications; }
};
inline void ClimateCall::perform() { if (parent_ != nullptr) parent_->control(*this); }
}
}
#define ESP_LOGD(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGI(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGW(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGVV(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGCONFIG(...) ::esphome::test_log(__VA_ARGS__)
#define LOG_CLIMATE(...) ::esphome::test_log(__VA_ARGS__)
#define LOG_UPDATE_INTERVAL(...) ::esphome::test_log(__VA_ARGS__)
#define YESNO(value) ((value) ? "yes" : "no")
#define ONOFF(value) ((value) ? "on" : "off")
