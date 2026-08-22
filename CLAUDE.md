# GloView — task prompt: two visual bugs

You are working on gloview, a Hyprland 0.56.2 plugin (.so, C++23/GLES 3.2).
Read AGENTS.md first — every rule there is binding (crash = whole session down;
hot-reload via `cmake --build build --target reload`; comments document
verified Hyprland internals only).

This document defines exactly two tasks. Do not refactor anything else.

---

## Bug A — black flash on overview OPEN

### Symptom
1–3 frames right after the bind: the screen goes BLACK except one window which
renders crisp and live (that crisp window is actually gloview's tile preview —
previews draw opaque at their natural boxes from frame 1). Then the entry
animation plays normally. Currently ~2 frames @60fps.

### Verified facts (do not re-litigate)
- NOT simplify()/damage-discard: `debug:pass=1` (forces infinite damage,
  disables occlusion culling) does not fix it.
- Independent of wallpaper source: identical with noctalia layer, swaybg, and
  NO wallpaper at all. With `misc:background_color=ff0000` runtime-set the
  flash stayed black (a red frame appeared exactly once, right after eval,
  before the plugin's reloadConfig re-read the file value rgb(000000)).
- `misc:disable_hyprland_logo=true` on this system ⇒ Hyprland's
  renderBackground() is a full no-op normally (`PRENDERTEX=false` and
  `m_backgroundOpacity` not animated ⇒ neither clear nor texture queued);
  the visible desktop background comes ONLY from BACKGROUND/BOTTOM layer
  surfaces (noctalia/swaybg).
- Therefore the flash frames are commits where (a) bg layer surfaces did not
  reach the framebuffer AND (b) the target buffer held zeros (fresh/unrendered),
  while windows/tile-previews DID draw.
- The event-loop animation pump (rearmanim/ensureAnimPump, ticks strictly
  between frames) reduced this from ~30 frames to 2–3 but did not eliminate it.
- Hypothesis board lives in CANDIDATES.md — C1/C2 (simplify family) are
  REFUTED there; read it before proposing anything.

### Where to dig (pinned Hyprland source: /tmp/opencode/HLsrc, v0.56.2)
`IHyprRenderer::renderMonitor()` flow: early-return gate on
`needsFrame/forceFullFrames/m_damage.hasChanged()` → direct-scanout attempt
(`canAttemptDirectScanoutFast()` / `attemptDirectScanout()`) → `beginRender`
→ solitary-client branch (`m_solitaryClient` renders ONLY that window,
skipping renderAllClientsForWorkspace entirely) → renderWorkspace (background
→ layers → windows) → RENDER_LAST_MOMENT (gloview appends its pass elements)
→ endRender → `m_renderPass.render(damage)` → commit.

Prime suspects, in order:
1. **Direct scanout engage/leave**: the damage storm at open() may let
   `attemptDirectScanout()` present client buffers for 1–2 frames, then
   `handleDSleave()` transitions back to composition — check what the
   composited framebuffer contains right after DS leave, and whether our
   shouldRenderWindow-hook/hide state makes a single-window workspace look
   scanout-eligible for a moment.
2. **Solitary-client branch**: same trigger shape — if `m_solitaryClient` is
   set for a frame, background/layers are skipped wholesale while our overlay
   still queues (RENDER_LAST_MOMENT fires regardless). Check what sets/clears
   it and whether gloview's window-hiding influences it.
3. **Who schedules the first commits**: log every scheduleFrame source +
   damage region size for ~5 frames around open(); compare against presented
   frames captured with wf-recorder (frame-extract workflow: record to /tmp,
   extract PNGs, inspect YAVG per frame — see git history for the exact
   commands used previously).

### Instrumentation rules
- Plugin side: use Overview::dbg() (debug_logs=1 → /tmp/gloview.log).
- Compositor side: DO NOT patch Hyprland; reason from the pinned source +
  logs. If a decisive experiment REQUIRES a compositor change, propose it in
  the final report instead of applying it.
- One experiment = one commit (build green, no warnings), so any result can
  be bisected.

### Definition of done (Bug A)
Toggle produces zero dark frames on entry in all three wallpaper scenarios
(noctalia / swaybg / none), verified frame-by-frame on extracted PNGs, across
10 consecutive toggles including after idle periods. No GPU-load regression
while idle (the hkDamageSurface suppression must keep working).

---

## Bug B — blur → sharp → blur on overview CLOSE

### Symptom
During close the backdrop reads as: blurred (our backdrop crossfade) → SHARP
(middle of the glide) → blurred again at the end. The middle-sharp phase is
wrong; the desktop should never appear sharper than its resting state at any
point of the transition.

### Mechanism to verify first (high confidence)
- Our backdrop is a cached blur blitted with fading alpha over live currentFB;
  as it fades, the sharp desktop shows through BY DESIGN.
- Translucent windows on the desktop carry Hyprland's PER-WINDOW decoration
  blur (blur-behind). While the overview is up, real windows are hidden and
  our preview surfaces are queued WITHOUT per-surface blur (see
  renderWindowLive's data.blur comment). When close completes and real
  windows return, their decoration blur snaps back ⇒ sharp phase sits between
  our fading backdrop blur and the windows' own blur.
- Check renderWindowLive (overview_render.cpp) for how data.blur /
  needsLiveBlur interact with our phases, and whether enabling the previews'
  blur-behind to MATCH the real windows' setting removes the sharp phase.

### Candidate fixes (evaluate, pick ONE, measure GPU cost)
1. Queue preview surfaces with the same blur-behind the real window has, so
   mid-transition pixels already match the end state (preferred if cheap —
   reuses Hyprland's live-blur machinery our Back/Mid phases already feed via
   needsLiveBlur()).
2. Hold a low-alpha floor of our cached backdrop blur until the handoff frame
   (m_pendingDeactivate), then cut — simplest, but check it doesn't read as a
   dim step (the entry-side equivalent was fixed once; see the (1-e) backing
   alpha comment in drawPreviewTile).
3. Crossfade OUR blur into per-window blur by fading the backdrop slower than
   the tiles fly (curve change only) — last resort, changes feel globally.

### Definition of done (Bug B)
Frame-extracted close sequence shows monotonically non-increasing sharpness
(no local maximum mid-glide) for: plain toggle, expo close, alt-tab commit,
close-during-open. Idle GPU load unchanged. The entry look must remain
exactly as-is (entry was tuned separately; don't touch its curves).

---

## Working rules for both bugs
- Start each session by reading REFACTORING.md (current state) and CANDIDATES.md.
- Never use currentFB as a blur/backdrop source (solitary-client fast path) —
  backdropSource()/renderBackdropSource() only.
- Coordinates: monitor-local logical px through pxb()/pxr() before any GL call.
- Config only via cfg* helpers; colors via cfgColor().
- If an experiment disproves a CANDIDATES.md item, update that file in the
  same commit.
