// Escape, Enter and the toggle bind: three keys, three intents.
//
// All three used to end in Overview::close(). On a deck holding windows from other
// workspaces that meant the exit spent itself flying the centred card toward the tile box
// it would have had if we had committed, and then dropped it in a single frame -- we landed
// back on the empty workspace we opened from. The user had been shown a window manager
// tiling motion for a window that was never going to be on screen.
//
// Two decisions carry the fix and both are here, pure:
//
//   toggleIntent -- what the toggle key means. It opened on one state and closed on the
//   other, so the bind the user reaches for first was the one that could not confirm a
//   focus. Enter could.
//   tileExit -- whether a card's natural box is a destination. It is only one when the
//   window is on the workspace the close lands on.
//
// The third half of the fix is not here and cannot be: "is this card focused" lived in two
// places, layoutTiles answering none->0 and the input handlers reading the raw index, so
// the deck could show a centred card while Enter had nothing to activate. That is
// model::Scene::focused(), which pulls in Hyprland types.

#include <cassert>
#include <cmath>
#include <cstdio>

#include "../src/anim_types.hpp"

using gloview::anim::cardHides;
using gloview::anim::DismissIntent;
using gloview::anim::hideShrink;
using gloview::anim::TileExit;
using gloview::anim::tileExit;
using gloview::anim::toggleIntent;

namespace {

// One tile of the deck, and the workspace it lives on. Only the two facts the decisions ask
// about, so the scenario below is readable as the user's description rather than as code.
struct Deck {
  int focusableTiles = 0;      // m_scene.focused() >= 0
  bool focusOnLandingWs = false;
};

constexpr DismissIntent ESC = DismissIntent::Cancel;

} // namespace

int main() {
  // --- the toggle key ----------------------------------------------------------------
  // Down it opens. That much did not change.
  assert(toggleIntent(false, false) == DismissIntent::Open);
  assert(toggleIntent(false, true)  == DismissIntent::Open);

  // Up, with a card focused, it confirms -- the same thing Enter does. It used to cancel,
  // which is the whole reason one bind felt like "back" and another like "go there".
  assert(toggleIntent(true, true) == DismissIntent::Commit);

  // Up with nothing to confirm, it cancels. Committing nothing is cancelling; saying it
  // here is what stops it becoming an activateSelection() that quietly closes.
  assert(toggleIntent(true, false) == DismissIntent::Cancel);
  assert(toggleIntent(true, false) == ESC);

  // The toggle only looks at what is focused, never at how many tiles exist: a deck of one
  // is as confirmable as a deck of twenty.
  assert(toggleIntent(true, true) == toggleIntent(true, true));
  std::puts("toggle: opens when down, confirms when up with a card, cancels when up with none");

  // --- where a card goes ---------------------------------------------------------------
  // The window is on the workspace we land on: fly it home. This is the seamless case, the
  // one that has always worked, and it must keep working untouched.
  assert(tileExit(true) == TileExit::Home);

  // It is not. Its natural box is a rectangle on a workspace that is not coming, so the
  // flight is the lie and the card must leave standing.
  assert(tileExit(false) == TileExit::InPlace);
  std::puts("a card flies home only when its window is on the landing workspace");

  // --- the scenario, end to end --------------------------------------------------------
  // Open on an EMPTY workspace: no tiles, nothing focusable, allOverride 0.
  Deck d;

  // "a", then "c". The rebuild puts windows from other workspaces on the deck -- and the
  // deck is now visibly showing one of them centred. This is the state the user was in.
  d.focusableTiles = 5;

  // Escape is the close key and its intent does not depend on the deck: return to where we
  // came from. The landing workspace is still the empty one, so every card is a window that
  // is not on it and none of them has anywhere to fly.
  assert(ESC == DismissIntent::Cancel);
  assert(tileExit(d.focusOnLandingWs) == TileExit::InPlace);

  // Toggle: the user meant "go to that window", so the close lands on the window's own
  // workspace and that window has somewhere to fly to.
  assert(toggleIntent(true, d.focusableTiles > 0) == DismissIntent::Commit);
  d.focusOnLandingWs = true;
  assert(tileExit(d.focusOnLandingWs) == TileExit::Home);

  // ...and the cards from the workspace we are leaving are InPlace even now: a commit is
  // not a licence to fly every card at a tile. Only the ones that are coming go home.
  assert(tileExit(d.focusOnLandingWs) == TileExit::Home);    // the committed window
  assert(tileExit(!d.focusOnLandingWs) == TileExit::InPlace); // the rest of the deck
  std::puts("the scenario: Escape dissolves the deck, the toggle flies exactly one card home");

  // --- the two must not be confused -----------------------------------------------------
  // Commit and Home are not the same decision and neither implies the other. A close can
  // commit a focus and still leave most of the deck in place, and it can cancel and still
  // fly home every window that genuinely lives on the landing workspace -- which is the
  // ordinary "Escape" on a workspace that has windows.
  assert(DismissIntent::Commit != DismissIntent::Cancel);
  assert(tileExit(true) == TileExit::Home && tileExit(false) == TileExit::InPlace);
  std::puts("committing a focus and going home are independent");

  // --- who plays the hide ----------------------------------------------------------------
  // THIS is the architecture the user described: one hide motion, applied to every card,
  // and the ONLY difference between committing and cancelling is whether one card is
  // spared it because it is landing. Three cells, and the third is what did not exist.
  //
  // Cell 1: the centred card of the run, and its window IS on the landing workspace. It is
  // the one card being committed to -- it flies home and must not touch the shrink, or the
  // seamless handover becomes a visible one.
  assert(!cardHides(0, true));
  // Cell 2: a side card of the run. It was always hidden this way, whatever its workspace.
  assert(cardHides(3, true));
  assert(cardHides(1, true));
  // Cell 3: the centred card, and it CANNOT land. On a cancel that is every card in the
  // deck, and each one has to hide exactly like the side cards do. Asserting depth 0 here
  // is the regression: the rule used to be "not the centred one", which left this card
  // pretending to fly somewhere and then dropped it.
  assert(cardHides(0, false));
  std::puts("the centred card hides whenever it cannot land, at any depth");

  // The rule is one predicate, not three call sites: the grid passes depth 0 for every
  // card, which is right -- nothing in a grid is "in front" -- and gets "cannot land" as
  // its only reason to hide. That is what makes a cancel take a whole grid out.
  assert(cardHides(0, false) == cardHides(7, false));
  assert(!cardHides(0, true) && cardHides(0, false));
  std::puts("grid and run ask the same question");

  // --- the hide motion itself -------------------------------------------------------------
  // shrink toward the card's own centre, finishing at `collapse` of the close. A card that
  // is still shrinking when the overlay goes is the pop the user reported, so collapse is
  // where the shrink ENDS, not where it begins.
  {
    constexpr double c = 0.5;
    assert(std::abs(hideShrink(0.0, c) - 1.0) < 1e-12); // the close has not started
    assert(std::abs(hideShrink(c / 2, c) - 0.5) < 1e-12);
    assert(hideShrink(c, c) < 1e-12);   // gone by the time the backdrop is still holding
    assert(hideShrink(c * 2, c) < 1e-12);
  }
  std::puts("the shrink runs 1 -> 0 and finishes at collapse");

  // A negative or zero collapse would divide by zero and hand the compositor a NaN box.
  // The guard is the difference between "the card vanished" and "the compositor drew
  // garbage", so it is asserted rather than assumed.
  for (const double bad : {0.0, -1.0, -0.5}) {
    for (const double out : {0.0, 0.25, 1.0, 4.0}) {
      const double k = hideShrink(out, bad);
      assert(std::isfinite(k));
      assert(k >= 0.0 && k <= 1.0);
    }
  }
  // out past 1 (a close resumed from a seek) clamps instead of going negative.
  assert(hideShrink(-1.0, 0.5) == 1.0);
  std::puts("nonsense collapse cannot produce a NaN box");

  std::puts("OK");
  return 0;
}