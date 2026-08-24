#pragma once

// The debug channel (REFACTORING.md D1). One gated sink, two consumers:
// the per-frame trace in render/painter.cpp and scattered state probes.
// Hyprland's own log routing varies by session init (journald unit names,
// stdout redirection) — the plain file is always where you expect it.

#include <string>

namespace gloview::debug {

// plugin:gloview:debug_logs != 0
bool enabled();

// Gate-checked log line: "[gate=N] msg" appended to /tmp/gloview.log
// (truncated per plugin load) and mirrored to Hyprland's log at INFO.
// Includes a bootstrap window — the first lines after a plugin load are
// written REGARDLESS of the flag, stamped with the gate value this instance
// reads: a dead config path then shows either the frames or the gate lying,
// never silence.
void dbg(const std::string &msg);

} // namespace gloview::debug
