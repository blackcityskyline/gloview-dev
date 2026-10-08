// Which leaf drives the master clock, and whether the backdrop fades with it.
//
// These were three separate expressions of the same two questions, in eased(),
// animDuration() and blurAlpha(). eased() picked a leaf to read a curve from and
// animDuration() picked a leaf to read a duration from -- and nothing checked that the two
// picked the same one. If they ever disagreed, the overview would close along one curve
// while timing itself by another.
//
// eased() and animDuration() now both go through Overview::masterId(), so the choice
// happens once. What this file checks is the choice itself and the rule about the
// backdrop, both of which are the kind of thing that is right in every case someone tries
// and wrong in the one they do not.

#include <cassert>
#include <cstdio>

#include "../src/anim_types.hpp"

using gloview::anim::backdropFades;
using gloview::anim::masterLeaf;
using gloview::anim::MasterLeaf;

int main() {
  // --- the leaf ------------------------------------------------------------------
  // Plain open, plain close, and the jump exit. jumpMode only changes anything once the
  // overview is on its way out: while it is still opening, the opening leaf is correct
  // even under a digit key, because nothing about that is visible yet.
  assert(masterLeaf(false, true)  == MasterLeaf::Open);
  assert(masterLeaf(false, false) == MasterLeaf::Close);
  assert(masterLeaf(true, true)   == MasterLeaf::Open);
  assert(masterLeaf(true, false)  == MasterLeaf::JumpOut);
  std::puts("jumpMode only changes the exit leaf");

  // --- the backdrop ---------------------------------------------------------------
  // With windows present on the landing workspace, the real windows' own blur covers the
  // exit, so ours holds. On an empty workspace there is nothing to cover it and the
  // backdrop must ride the clock, or the exit opens onto a sharp desktop.
  {
    assert(backdropFades(true, true));    // opening: fades regardless
    assert(backdropFades(true, false));   // opening onto occupied: fades, it is arriving
    assert(backdropFades(false, true));   // closing off empty: nothing to cover
    assert(!backdropFades(false, false)); // closing off occupied: holds
  }
  std::puts("backdrop holds on exit only when windows are underneath");

  // The distinction that matters is exit-only. Opening onto an occupied workspace still
  // fades -- the backdrop is arriving, not leaving, and holding it would show a sharp
  // desktop under the rising overview. Symmetric reasoning on both directions would get
  // this wrong, which is why both cells are asserted rather than the rule summarised.
  assert(backdropFades(true, false) != backdropFades(false, false));
  std::puts("the two directions are not symmetric, and are not treated as such");

  std::puts("OK");
  return 0;
}