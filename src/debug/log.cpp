#include "log.hpp"

#include <cstdio>

#include <hyprland/src/debug/log/Logger.hpp>

#include "../config/config.hpp"

namespace gloview::debug {

bool enabled() { return cfg::debug.logs != 0; }

void dbg(const std::string &msg) {
  // Free lines before the flag starts gating: a dead config path otherwise
  // yields an empty log while the option insists it is 1.
  static int bootstrap = 200000;
  const int gate = cfg::debug.logs;
  if (gate != 0)
    bootstrap = -1; // flag works — stop spending the free lines
  else if (bootstrap <= 0 || --bootstrap < 0)
    return;
  if (Log::logger)
    Log::logger->log(Log::INFO, "[gloview] {}", msg);
  // Append, never truncate: a crash must keep its own evidence across the
  // restarted session's plugin load.
  static FILE *f = nullptr;
  if (!f) {
    f = fopen("/tmp/gloview.log", "a");
    if (f) {
      fprintf(f, "\n=== SESSION %s ===\n",
              getenv("HYPRLAND_INSTANCE_SIGNATURE")
                  ? getenv("HYPRLAND_INSTANCE_SIGNATURE")
                  : "?");
      fflush(f);
    }
  }
  if (f) {
    fprintf(f, "[gate=%d] %s\n", gate, msg.c_str());
    fflush(f);
  }
}

} // namespace gloview::debug
