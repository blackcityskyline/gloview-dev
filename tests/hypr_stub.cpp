// Link-time stub for the two Hyprland log symbols curves.cpp calls.
//
// CLogger's constructor and log() live in the Hyprland binary, not in any library, so a
// headless link of curves.cpp needs them defined exactly once somewhere. Defining them
// here is the whole trick -- the registry itself needs no compositor, and nothing here
// reaches into Hyprland state.
//
// Kept deliberately tiny: only the two symbols the linker named. If a future change makes
// curves.cpp use another, the link fails loudly here rather than quietly at runtime.

#include <hyprutils/cli/Logger.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

namespace Log {

CLogger::CLogger() = default;

void CLogger::log(Hyprutils::CLI::eLogLevel, const std::string_view &) {
  // swallow: a warning about a bad curve name is not the test's business
}

} // namespace Log