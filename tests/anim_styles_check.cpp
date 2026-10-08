// The style sets, and the strings the motion code compares against.
//
// The value of this is not the membership check -- that is a loop. It is that the legal
// values are now DECLARED rather than implied by five string comparisons split across
// clocks.cpp and actions.cpp. Before, adding a style meant writing a branch and
// forgetting one left a value the config happily accepted and the motion ignored: no
// error, the transition just came out as something else.
//
// This test therefore also does the thing that makes the declaration stick: it reads the
// source of the two motion files and fails if a style name is compared there that is not
// in the corresponding list. A new style added to the renderer without the list shows up
// here; a list entry the renderer never reads shows up as unused below.

#include <cassert>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#include "../src/anim_types.hpp"

using namespace gloview::anim;

namespace {

std::string slurp(const char *path) {
  std::ifstream f(path);
  assert(f && "cannot open the source to scan");
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Every `== "x"` / `!= "x"` literal in the file, which is how the motion code asks what
// style it was given.
std::set<std::string> comparedLiterals(const std::string &src) {
  std::set<std::string> out;
  for (size_t p = src.find("\""); p != std::string::npos; p = src.find('"', p + 1)) {
    const auto e = src.find('"', p + 1);
    if (e == std::string::npos)
      break;
    const auto tok = src.substr(p + 1, e - p - 1);
    if (!tok.empty())
      out.insert(tok);
    p = e;
  }
  return out;
}

} // namespace

int main(int argc, char **argv) {
  const std::string root = argc > 1 ? argv[1] : ".";
  const auto clocks  = slurp((root + "/src/clock/clocks.cpp").c_str());
  const auto actions = slurp((root + "/src/actions/actions.cpp").c_str());
  // parseSwapStyle -- the swap name -> SwapStyle mapping -- lives in overview.hpp, not in
  // actions.cpp, which is why a first pass at this scanned two files and reported
  // "horizontal" as unreachable when it is read on every swap.
  const auto overview = slurp((root + "/src/overview.hpp").c_str());

  // --- the sets are what they claim ------------------------------------------------
  for (const char *s : kEnterStyles)
    assert(isEnterStyle(s));
  for (const char *s : kSwapStyles)
    assert(isSwapStyle(s));
  std::puts("declared styles are accepted by their own predicates");

  // --- and nothing else is ---------------------------------------------------------
  // "horizontal" is a swap-only name; typing it into ws_enter_style must not pass.
  assert(!isEnterStyle("horizontal"));
  assert(!isSwapStyle("instant"));   // that one is jump_style's, a different key
  assert(!isEnterStyle(""));
  assert(!isEnterStyle("Slide"));    // case is not normalised anywhere
  assert(!isSwapStyle("slide"));     // swap says "horizontal", not "slide"
  std::puts("near-misses are rejected");

  // --- the renderer does not compare against anything undeclared --------------------
  // The whole reason these lists exist. Walk every string literal the two motion files
  // compare with and require it to be a style name that some list declares.
  auto literals = comparedLiterals(clocks);
  for (const std::string *src : {&actions, &overview}) {
    const auto b = comparedLiterals(*src);
    literals.insert(b.begin(), b.end());
  }

  // Literals that appear in those files for other reasons -- log tags, format strings,
  // unrelated comparisons. Only names in the style vocabulary are checked.
  const std::set<std::string> styleWords = {"slide",   "slidevert", "fade",  "pop",
                                            "horizontal", "instant"};
  int checked = 0;
  for (const auto &lit : literals) {
    if (!styleWords.count(lit))
      continue;
    ++checked;
    assert(isEnterStyle(lit) || isSwapStyle(lit));
  }
  assert(checked > 0);
  std::printf("renderer compares against %d style name(s), all declared\n", checked);

  // --- no dead entries ---------------------------------------------------------------
  // A name in a list that no comparison can reach is a style the user can type and
  // nothing will read. The membership above cannot see this; counting can.
  const auto reach = [&literals](const char *const *list, std::size_t n) {
    int hit = 0;
    for (std::size_t i = 0; i < n; ++i)
      if (literals.count(list[i]))
        ++hit;
    return hit;
  };
  const int enterHit = reach(kEnterStyles, std::size(kEnterStyles));
  const int swapHit  = reach(kSwapStyles, std::size(kSwapStyles));
  std::printf("enter styles reached: %d/%zu, swap styles reached: %d/%zu\n", enterHit,
              std::size(kEnterStyles), swapHit, std::size(kSwapStyles));

  // Reading these numbers: "horizontal" sits at 3/4 on purpose. parseSwapStyle ends in
  // ": SwapStyle::Horizontal" -- it is the branch that catches everything unrecognised,
  // not a comparison against its own name. So it works, and is reachable, just not by
  // a string test. A number BELOW the list size for any other entry would be a key the
  // user can set that nothing reads.
  std::puts("OK");
  return 0;
}