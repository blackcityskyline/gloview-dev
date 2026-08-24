#include "log.hpp"

#include <cstdio>

#include <hyprland/src/debug/log/Logger.hpp>

#include "../config/config.hpp"

namespace gloview::debug {

bool enabled() { return cfg::debug.logs != 0; }

void dbg(const std::string &msg) {
  // Bootstrap window: the first lines after a plugin load are written
  // REGARDLESS of the flag, stamped with what THIS instance reads for it.
  // Exists because a dead config path silently produced an empty log while
  // hyprctl getoption insisted the flag was 1 — with the bootstrap we either
  // see the frames or see the gate value lying, never silence.
  static int bootstrap = 200000;
  const int gate = cfg::debug.logs;
  if (gate != 0)
    bootstrap = -1; // flag works — stop spending the free lines
  else if (bootstrap <= 0 || --bootstrap < 0)
    return;
  if (Log::logger)
    Log::logger->log(Log::INFO, "[gloview] {}", msg);
  static FILE *f = fopen("/tmp/gloview.log", "w"); // truncated per plugin load
  if (f) {
    fprintf(f, "[gate=%d] %s\n", gate, msg.c_str());
    fflush(f);
  }
}

} // namespace gloview::debug
