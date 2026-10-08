#include "log.hpp"

#include <cstdio>

#include <hyprland/src/debug/log/Logger.hpp>

#include "../config/config.hpp"

namespace gloview::debug {

bool enabled() { return cfg::debug.logs != 0; }

void dbg(const std::string &msg) {
  // free lines: a dead config path logs nothing
  static int bootstrap = 200000;
  const int gate = cfg::debug.logs;
  if (gate != 0)
    bootstrap = -1;
  else if (bootstrap <= 0 || --bootstrap < 0)
    return;
  if (Log::logger)
    Log::logger->log(Log::INFO, "[gloview] {}", msg);
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

}
