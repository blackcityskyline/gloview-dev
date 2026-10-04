#pragma once

// Gated debug sink: /tmp/gloview.log (append, one separator per session) plus
// Hyprland's log at INFO. The plain file is always where you expect it,
// regardless of how the session routed its own logs.

#include <string>

namespace gloview::debug {

// plugin:gloview:debug_logs != 0
bool enabled();

// "[gate=N] msg". The first lines after a load are written even when the flag
// is off (stamped with the value this instance reads), so a dead config path
// shows either the frames or the gate lying — never silence.
void dbg(const std::string &msg);

} // namespace gloview::debug
