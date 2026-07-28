#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "../tcl_climate.h"

namespace esphome::tcl_climate {

class TclSwitch final : public switch_::Switch, public Parented<TclClimate> {
 public:
  explicit TclSwitch(TclSwitchType type) : type_(type) {}

 protected:
  void write_state(bool state) override;

  TclSwitchType type_;
};

}  // namespace esphome::tcl_climate
