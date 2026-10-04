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

uint32_t attach_switch(TclClimate &climate, TclSwitch &entity, TclSwitchType type) {
  entity.set_parent(&climate);
  switch (type) {
    case TclSwitchType::DISPLAY_CONTROL:
      climate.set_display_switch(&entity);
      return TclClimate::PENDING_DISPLAY;
    case TclSwitchType::BEEP_CONTROL:
      climate.set_beep_switch(&entity);
      return TclClimate::PENDING_BEEP;
    case TclSwitchType::HEALTH_CONTROL:
      climate.set_health_switch(&entity);
      return TclClimate::PENDING_HEALTH;
    case TclSwitchType::RESTORE_STATE_CONTROL:
      climate.set_restore_state_switch(&entity);
      return 0;  // Local next-boot policy, not an appliance command.
  }
  std::abort();
}

void test_all_switch_writes(TclProtocolProfile profile) {
  for (const auto type : {TclSwitchType::DISPLAY_CONTROL, TclSwitchType::BEEP_CONTROL,
                          TclSwitchType::HEALTH_CONTROL, TclSwitchType::RESTORE_STATE_CONTROL}) {
    TclClimate climate;
    TclSwitch entity(type);
    ESPPreferences preferences;
    global_preferences = &preferences;
    climate.set_protocol_profile(profile);
    const auto field = attach_switch(climate, entity, type);
    entity.restore_mode = switch_::SWITCH_RESTORE_DEFAULT_OFF;
    climate.setup();
    require(entity.has_published_state() && !entity.state,
            "every configured switch initializes its own persisted OFF state");
    unsigned expected_syncs = 0;
    bool previous = false;
    for (const bool requested : {true, true, false, false}) {
      climate.pending_fields_ = 0;
      bool callback_known = false;
      entity.on_state = [&](bool value) {
        callback_known = entity.has_published_state() && value == requested;
      };
      if (requested) entity.turn_on(); else entity.turn_off();
      expected_syncs += requested != previous;
      require(callback_known == (requested != previous),
              "changed states are known inside callbacks; duplicate publications are suppressed");
      require(entity.has_published_state() && entity.state == requested,
              "all switches keep the requested known state, including repeated commands");
      require(entity.persisted == optional<bool>(requested),
              "every switch saves the requested preference");
      require(preferences.sync_count == expected_syncs,
              "all switches sync changes but not repeated same-state commands");
      require(climate.pending_fields_ == field,
              "repeated switch commands reassert only their own field; restore is local");
      if (type == TclSwitchType::RESTORE_STATE_CONTROL)
        require(climate.restore_state_runtime_enabled_ == requested,
                "restore switch changes its next-boot gate in both directions");
      previous = requested;
    }
    global_preferences = nullptr;
  }
}

void test_all_first_publications(TclProtocolProfile profile) {
  for (const auto type : {TclSwitchType::DISPLAY_CONTROL, TclSwitchType::BEEP_CONTROL,
                          TclSwitchType::HEALTH_CONTROL, TclSwitchType::RESTORE_STATE_CONTROL}) {
    for (const bool requested : {false, true}) {
      TclClimate climate;
      TclSwitch entity(type);
      ESPPreferences preferences;
      global_preferences = &preferences;
      climate.set_protocol_profile(profile);
      const auto field = attach_switch(climate, entity, type);
      entity.restore_mode = switch_::SWITCH_RESTORE_DISABLED;
      climate.setup();
      require(!entity.has_published_state(), "disabled restore leaves every switch unknown");
      // Isolate first publication from restore initialization, including an
      // explicit false that equals the inherited, unpublished default value.
      entity.restore_mode = switch_::SWITCH_RESTORE_DEFAULT_OFF;
      bool callback_known = false;
      entity.on_state = [&](bool value) {
        callback_known = entity.has_published_state() && value == requested;
      };
      if (requested) entity.turn_on(); else entity.turn_off();
      require(callback_known && entity.state == requested,
              "first true or false publication is known before every switch callback");
      require(entity.persisted == optional<bool>(requested) && preferences.sync_count == 1,
              "first explicit preference is saved and synced for every switch");
      require(climate.pending_fields_ == field,
              "first preference queues its own field, except local restore policy");
      global_preferences = nullptr;
    }
  }
}

void test_all_control_combinations(TclProtocolProfile profile) {
  // Exercise independent ON and OFF choices, not only all switches ON together.
  for (unsigned combination = 0; combination < 8; ++combination) {
    const bool health = (combination & 1) != 0;
    const bool display = (combination & 2) != 0;
    const bool beep = (combination & 4) != 0;
    Fixture f(profile);
    f.establish_on();
    if (health) f.health.turn_on(); else f.health.turn_off();
    if (display) f.display.turn_on(); else f.display.turn_off();
    if (beep) f.beep.turn_on(); else f.beep.turn_off();
    require(f.climate.send_pending_command_(), "explicit control choices produce a command");
    assert_command(f.climate.sent_frames.back(), health, display, beep);
    f.rx(true, health, display);
    require(f.climate.pending_fields_ == 0, "settle the explicit choices before power-off");
    f.restore.turn_on();
    f.restore.turn_off();
    require(f.climate.pending_fields_ == 0 && f.health.state == health &&
                f.display.state == display && f.beep.state == beep,
            "next-boot restore policy emits no command and preserves independent preferences");

    f.rx(false, false, false);
    require(f.health.state == health && f.display.state == display && f.beep.state == beep,
            "OFF status preserves each independent control preference");
    f.on();
    assert_command(f.climate.sent_frames.back(), health, display, beep);
    f.rx(true, !health, !display);
    require(f.climate.send_pending_command_(), "ON feedback triggers the sticky phase B");
    assert_command(f.climate.sent_frames.back(), health, display, false);
    f.rx(true, !health, !display);
    require(f.health.state == health && f.display.state == display && f.beep.state == beep,
            "ignored contradictory feedback and silent phase B never erase preferences");

    // The internal silent phase must not mute the next user-issued command.
    f.on();
    assert_command(f.climate.sent_frames.back(), health, display, beep);
  }
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
  for (const auto type : {TclSwitchType::DISPLAY_CONTROL, TclSwitchType::BEEP_CONTROL,
                          TclSwitchType::HEALTH_CONTROL, TclSwitchType::RESTORE_STATE_CONTROL}) {
    for (const auto &test : cases) {
      TclClimate climate;
      TclSwitch entity(type);
      const auto field = attach_switch(climate, entity, type);
      entity.restore_mode = test.mode;
      entity.persisted = test.persisted;
      climate.setup();
      require(entity.has_published_state() == test.expected.has_value(),
              "all switch restore modes establish known state only for resolved initial values");
      if (test.expected.has_value()) {
        require(entity.state == *test.expected, "each switch resolves its native ESPHome restore value");
        if (type == TclSwitchType::RESTORE_STATE_CONTROL)
          require(climate.restore_state_runtime_enabled_ == *test.expected,
                  "restored next-boot policy remains local and matches the saved value");
      }
      require(climate.pending_fields_ == (test.expected.has_value() ? field : 0),
              "only resolved appliance-control preferences queue their own field");
    }
  }
}
}

int main() {
  test_restore_mode_matrix();
  for (const auto profile : {TclProtocolProfile::PROFILE_TCL_35, TclProtocolProfile::PROFILE_TCLAC_38}) {
    test_power_cycle(profile);
    test_unknown_and_publication_paths(profile);
    test_all_switch_writes(profile);
    test_all_first_publications(profile);
    test_all_control_combinations(profile);
  }
  std::cout << "Switch lifecycle regressions passed (TCL35 and TCLAC38).\n";
}
