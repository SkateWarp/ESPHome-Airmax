#include "tcl_switch.h"

namespace esphome::tcl_climate {

void TclSwitch::write_state(const bool state) {
  this->parent_->queue_switch_change(this->type_, state);
  this->publish_state(state);
}

}  // namespace esphome::tcl_climate
