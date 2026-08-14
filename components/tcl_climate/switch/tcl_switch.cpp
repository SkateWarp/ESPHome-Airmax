#include "tcl_switch.h"

#include "esphome/core/preferences.h"
#include "../tcl_state_policy.h"

namespace esphome::tcl_climate {

void TclSwitch::write_state(const bool state) {
  const bool changed =
      tcl_switch_state_changed(this->has_state(), this->state, state);
  this->parent_->queue_switch_change(this->type_, state);
  this->publish_state(state);

  // These control switches change rarely, and their values must survive an
  // immediate power loss. Flush only a real persistent change; the platform
  // preferences backend still compares against flash before writing.
  if (changed && (this->restore_mode & switch_::RESTORE_MODE_PERSISTENT_MASK) &&
      global_preferences != nullptr)
    global_preferences->sync();
}

}  // namespace esphome::tcl_climate
