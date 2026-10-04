#include "esphome/test_stubs.h"
#include <cstdlib>
#include <iostream>

// Exercise the production state machine without adding test hooks to firmware.
#define protected public
#include "../components/tcl_climate/tcl_climate.h"
#include "../components/tcl_climate/switch/tcl_switch.h"
#undef protected

using namespace esphome;
using namespace esphome::tcl_climate;

namespace {
void require(bool value, const char *message) {
  if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

std::array<uint8_t, 61> status(bool power, bool health, bool display) {
  std::array<uint8_t, 61> frame{};
  frame[0] = 0xBB; frame[1] = 0x01; frame[3] = 0x04;
  frame[4] = 55; frame[5] = 0x04;
  frame[7] = 0x01 | (power ? 0x10 : 0) | (display ? 0x20 : 0);
  frame[8] = 8;  // Auto fan, 24 C.
  frame[9] = health ? 0x04 : 0;
  frame[17] = 0x49; frame[18] = 0x0C; frame[45] = 230;
  frame.back() = tcl_xor_checksum(frame.data(), frame.size() - 1);
  return frame;
}

struct Fixture {
  TclClimate climate;
  TclSwitch health{TclSwitchType::HEALTH_CONTROL};
  TclSwitch display{TclSwitchType::DISPLAY_CONTROL};
  TclSwitch beep{TclSwitchType::BEEP_CONTROL};
  TclSwitch restore{TclSwitchType::RESTORE_STATE_CONTROL};
  ESPPreferences preferences;

  explicit Fixture(TclProtocolProfile profile, bool restore_values = true,
                   bool ignore_appliance = true) {
    global_preferences = &preferences;
    climate.set_protocol_profile(profile);
    climate.set_health_switch(&health); climate.set_display_switch(&display);
    climate.set_beep_switch(&beep); climate.set_restore_state_switch(&restore);
    health.set_parent(&climate); display.set_parent(&climate);
    beep.set_parent(&climate); restore.set_parent(&climate);
    climate.set_health_ignore_appliance_state(ignore_appliance);
    climate.set_display_ignore_appliance_state(ignore_appliance);
    if (restore_values) {
      health.restore_mode = switch_::SWITCH_RESTORE_DEFAULT_ON;
      display.restore_mode = switch_::SWITCH_RESTORE_DEFAULT_ON;
      beep.restore_mode = switch_::SWITCH_RESTORE_DEFAULT_OFF;
      beep.persisted = true;  // Exercise a restored Beep ON policy too.
      restore.restore_mode = switch_::SWITCH_RESTORE_DEFAULT_ON;
      restore.persisted = false;
    } else {
      health.restore_mode = display.restore_mode = beep.restore_mode =
          restore.restore_mode = switch_::SWITCH_RESTORE_DISABLED;
    }
    climate.setup();
  }
  ~Fixture() { global_preferences = nullptr; }
  void rx(bool power, bool health_value, bool display_value) {
    test_millis += 100;
    climate.awaiting_response_ = true;
    const auto frame = status(power, health_value, display_value);
    climate.handle_frame_(frame.data(), frame.size());
  }
  void establish_on() {
    rx(true, true, true); rx(true, true, true);
    require(climate.has_valid_status_, "two real status publications lock the signature");
    require(climate.send_pending_command_(), "restored preferences produce an initial command");
    rx(true, true, true);
    require(climate.pending_fields_ == 0 && climate.deferred_fields_ == 0,
            "initial restored preferences finish before the regression cycle");
  }
  void on() {
    climate::ClimateCall call(&climate);
    call.mode = climate::CLIMATE_MODE_COOL;
    call.perform();
    require(climate.send_pending_command_(), "ON request produces a command");
  }
};

void assert_command(const std::vector<uint8_t> &frame, bool health, bool display, bool beep) {
  require((frame[7] & 0x04) != 0, "control frame requests power ON");
  require(((frame[8] & 0x10) != 0) == health, "control frame carries the known Health preference");
  require(((frame[7] & 0x40) != 0) == display, "control frame carries the known Display preference");
  require(((frame[7] & 0x20) != 0) == beep, "control frame carries the intended Beep policy");
  require(frame.back() == tcl_xor_checksum(frame.data(), frame.size() - 1),
          "generated control checksum is valid");
}

void test_power_cycle(TclProtocolProfile profile) {
  Fixture f(profile);
  require(f.health.has_published_state() && f.display.has_published_state() &&
              f.beep.has_published_state() && f.restore.has_published_state(),
          "all four restored switches have explicit known state");
  require(!f.health.has_state(), "faithful base Switch publication must NOT magically set has_state");
  require(f.health.state && f.display.state && f.beep.state && !f.restore.state,
          "restored switch values are published");
  require(!f.climate.restore_state_runtime_enabled_, "restore gate follows its persisted false value");
  f.establish_on();
  f.rx(false, false, false);
  require(f.health.state && f.display.state, "ignored OFF status preserves published preferences");
  require((f.climate.deferred_fields_ & (TclClimate::PENDING_HEALTH | TclClimate::PENDING_DISPLAY)) ==
              (TclClimate::PENDING_HEALTH | TclClimate::PENDING_DISPLAY),
          "known switches become sticky on the real OFF edge");
  f.on();
  assert_command(f.climate.sent_frames.back(), true, true, true);
  f.rx(true, false, false);
  require((f.climate.pending_fields_ & TclClimate::PENDING_HEALTH) != 0,
          "fresh ON status queues Health for phase B even if its reported bit is false");
  require(f.climate.send_pending_command_(), "phase B emits a separate command");
  assert_command(f.climate.sent_frames.back(), true, true, false);
  require(f.beep.state && f.climate.state_.beep, "silent internal phase B does not erase Beep preference");
  f.rx(true, false, false);
  require(f.health.state && f.display.state, "ignore-appliance policy remains intact after confirmation");

  const auto syncs = f.preferences.sync_count;
  f.health.turn_on();
  require((f.climate.pending_fields_ & TclClimate::PENDING_HEALTH) != 0,
          "an explicit same-state ON still queues a reassertion");
  require(f.preferences.sync_count == syncs, "same-state reassertion does not sync preferences");
  f.health.turn_off();
  require(f.preferences.sync_count == syncs + 1, "real user change syncs once");
}

void test_unknown_and_publication_paths(TclProtocolProfile profile) {
  Fixture f(profile, false, true);
  require(!f.health.has_published_state() && !f.display.has_published_state() &&
              !f.beep.has_published_state() && !f.restore.has_published_state(),
          "RESTORE_DISABLED is unknown, not an invented false preference");
  f.rx(false, false, false); f.rx(false, false, false);
  require(!f.health.has_published_state() && !f.display.has_published_state(),
          "ignored UART state cannot initialize an unknown preference");
  // A known protocol state must not be overwritten by an unpublished child's
  // default false. This separately guards against indiscriminately removing
  // every has-state check instead of tracking real publications.
  f.climate.state_.health = true;
  f.climate.state_.display = true;
  f.climate.state_.beep = true;
  f.on();
  assert_command(f.climate.sent_frames.back(), true, true, true);
  f.rx(true, true, true);
  require(f.climate.deferred_fields_ == 0, "unknown switches do not become sticky on ON");
  bool callback_known = false;
  f.health.on_state = [&](bool value) { callback_known = value && f.health.has_published_state(); };
  f.health.turn_on();
  require(callback_known && f.health.has_published_state(), "explicit publication is known inside callbacks");
  f.restore.turn_off();
  require(f.restore.has_published_state() && !f.climate.restore_state_runtime_enabled_,
          "explicit restore gate publication initializes its state");

  Fixture followed(profile, false, false);
  followed.health.restore_mode = switch_::SWITCH_RESTORE_DEFAULT_ON;
  followed.rx(true, false, false); followed.rx(true, false, false);
  require(followed.health.has_published_state() && !followed.health.state &&
              followed.display.has_published_state() && !followed.display.state,
          "accepted UART publications initialize unknown Health and Display, including false");
  const auto syncs = followed.preferences.sync_count;
  followed.rx(true, false, false); followed.rx(true, false, false);
  require(followed.preferences.sync_count == syncs, "identical UART reports are deduplicated after publication");
  followed.rx(true, true, true);
  require(followed.health.state && followed.display.state,
          "non-ignored UART changes still update the switches");
}

void test_restore_mode_matrix() {
  struct Case { uint8_t mode; optional<bool> persisted; optional<bool> expected; };
  const Case cases[] = {
      {switch_::SWITCH_RESTORE_DISABLED, true, {}},
      {switch_::SWITCH_ALWAYS_OFF, true, false},
      {switch_::SWITCH_ALWAYS_ON, false, true},
      {switch_::SWITCH_RESTORE_DEFAULT_OFF, {}, false},
      {switch_::SWITCH_RESTORE_DEFAULT_ON, {}, true},
      {switch_::SWITCH_RESTORE_DEFAULT_OFF, true, true},
      {switch_::SWITCH_RESTORE_DEFAULT_ON, false, false},
      {switch_::SWITCH_RESTORE_INVERTED_DEFAULT_OFF, true, false},
      {switch_::SWITCH_RESTORE_INVERTED_DEFAULT_ON, false, true},
      {switch_::SWITCH_RESTORE_INVERTED_DEFAULT_OFF, {}, false},
      {switch_::SWITCH_RESTORE_INVERTED_DEFAULT_ON, {}, true},
  };
  for (const auto &test : cases) {
    TclClimate climate;
    TclSwitch health(TclSwitchType::HEALTH_CONTROL);
    health.set_parent(&climate);
    climate.set_health_switch(&health);
    health.restore_mode = test.mode;
    health.persisted = test.persisted;
    climate.setup();
    require(health.has_published_state() == test.expected.has_value(),
            "restore modes only establish known state when an initial value exists");
    if (test.expected.has_value())
      require(health.state == *test.expected, "restore mode resolves its native ESPHome value");
    require(((climate.pending_fields_ & TclClimate::PENDING_HEALTH) != 0) == test.expected.has_value(),
            "only a resolved restore value queues a Health preference");
  }
}
}

int main() {
  test_restore_mode_matrix();
  for (const auto profile : {TclProtocolProfile::PROFILE_TCL_35, TclProtocolProfile::PROFILE_TCLAC_38}) {
    test_power_cycle(profile);
    test_unknown_and_publication_paths(profile);
  }
  std::cout << "Switch lifecycle regressions passed (TCL35 and TCLAC38).\n";
}
