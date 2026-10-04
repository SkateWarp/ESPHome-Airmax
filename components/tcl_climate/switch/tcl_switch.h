#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "../tcl_climate.h"

namespace esphome::tcl_climate {

class TclSwitch final : public switch_::Switch, public Parented<TclClimate> {
 public:
  explicit TclSwitch(TclSwitchType type) : type_(type) {}

  // Switch::publish_state() does not establish EntityBase::has_state() on all
  // supported ESPHome versions. Track a real publication ourselves instead
  // of treating the default false value as an initialized user preference.
  bool has_published_state() const { return this->state_published_; }
  void publish_control_state(bool state) {
    // Callbacks must observe the initialized state during the publication.
    this->state_published_ = true;
    this->publish_state(state);
  }

 protected:
  void write_state(bool state) override;

  TclSwitchType type_;
  bool state_published_{false};
};

}  // namespace esphome::tcl_climate
