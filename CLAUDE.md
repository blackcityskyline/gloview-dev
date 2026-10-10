# GloView - Claude Code Context

## Project Summary

GloView is a macOS Mission Control-style overview plugin for the Hyprland Wayland compositor. It's a C++23 shared library (`gloview.so`) loaded at runtime via `hyprpm`. When activated (e.g. `SUPER+TAB`), it draws a compositor-side overlay showing live window previews over a blurred backdrop, with a workspace strip at a configurable screen edge.

**Key facts:**
- Language: C++23, built with CMake
- Output: `build/gloview.so`
- Dependencies: Hyprland headers, GLESv2 (raw OpenGL ES 3.2), Lua
- All blur shaders are embedded in C++ source as GLSL raw string literals (no separate .glsl files)

## Blur System Architecture

The blur is a **self-contained GL pipeline** that bypasses Hyprland's global `decoration:blur:*` settings. It uses a **dual-Kawase pyramid** for primary blur strength with an optional **separable 9-tap gaussian** at the pyramid bottom.

### Pipeline Flow
```
Source texture (full-res monitor, W x H)
  |
  v
[1] DOWN-CHAIN, real 2x per step (blur.cpp:377-393)
  |-- level 1: W/2 x H/2 ... level N: W/2^N x H/2^N (= m_fbA)
  \-- m_levels == 0 short-circuits to a straight blit into m_fbA
       |
       v
[2] SEPARABLE GAUSSIAN at the bottom (blur.cpp:395-399)
  |-- H pass: m_fbA -> m_fbB
  \-- V pass: m_fbB -> m_fbA  (repeated m_passes times)
       |
       v
[3] UP-CHAIN (blur.cpp:405-415)
  \-- levels N-1..1 -> dst framebuffer (full-res)

No dim in the filter (blur.cpp:420): renderBackdrop() draws backdrop_color as its own
rect so the cache stays reusable and the fade lives in one place.
```

### How Blur Transitions Work

The blur is **NOT independently animated** — its alpha rides the single
`m_progress` clock of the overview (`Overview::blurAlpha()`, overview.hpp:201).
There is no separate blur tween: fading the blit is the whole animation.

1. **Open:** the overview is *parked* first. `m_pendingOpen` makes `paint()` call
   `primeBackdrop()` and draw nothing (render/painter.cpp:186) until the backdrop
   composite is actually there. Limit: 1-2 parked frames.
2. **Close:** same `blurAlpha()` factor fades the blit out. No re-blur.
3. **Workspace switch:** the cache is not invalidated unless the source identity
   changes.
4. **Reflow (drag-drop):** `m_progress` stays pinned at 1.0, so the blur stays settled.

### Blur Cache System

`render::BlurCache` (render/backdrop.hpp:7) — `fb`, `srcId`, and the recipe
(`passes`/`sizePx`/`resolution`/`strength`).

- **`matches()` is checked EVERY frame, outside any dirty-guard.** That is the only
  thing that catches a source swap (wallpaper <-> video <-> window). Moving it inside a
  dirty-guard means the cache never invalidates.
- **The cache is pure blur:** no dim, no fade. `blur.cpp:420` says so explicitly. The
  dim is a separate rect in `renderBackdrop()` on the same `blurAlpha()` clock, so the
  fade and the dim cannot drift apart.
- **The backdrop composite does NOT expire.** It used to carry a 3-second TTL and a
  `hideLayers()` call in `open()`, which is what produced the black first frame: the
  composite timed out and was re-derived from a wallpaper texture mid-recreation, i.e.
  black. Now it lives until the source texture genuinely changes. A private scratch
  framebuffer (`m_backdropScratchFB`) absorbs a failed attempt so a black try cannot
  overwrite a good composite.
- `invalidate()` keeps the framebuffer, `drop()` releases it.

## Files to Focus On

### Core Blur Implementation
- **`src/blur.hpp`** (58 lines) — `CBlurFilter` declaration, GL state
- **`src/blur.cpp`** (434 lines) — GLSL shaders embedded, program compilation,
  `prepare()`, `render()`. Pyramid depth from `blur_resolution`, clamped by `kMaxLevels`.

### Blur Integration in Render Pipeline
- **`src/render/backdrop.cpp`** (439 lines) — `renderBackdropSource()` (27),
  `primeBackdrop()` (180), `renderBackdrop()` (218): source capture, the parked-open
  prime, cache hit/miss, dim, blit
- **`src/render/painter.cpp:185`** — `paint()`, the real draw order
- **`src/overview.hpp:201,244`** — `blurAlpha()`, `m_blur` (`render::BlurCache`)

### Blur Configuration & Lifecycle
- **`src/config/config.cpp:132-134,173,212`** — key registration:
  `blur_passes`, `blur_size`, `blur_resolution` (ints), `blur` (float),
  `backdrop_color` (colour)
- **`src/overview.hpp:331-334`** — accessors

### Config Keys
```
plugin:gloview:blur              = 1.0   (float 0..1)  -- backdrop blur strength (0=off)
plugin:gloview:blur_passes       = 3     (int 1..16)   -- gaussian iterations at pyramid bottom
plugin:gloview:blur_size         = 8     (int 1..200)  -- gaussian radius in screen pixels
plugin:gloview:blur_resolution   = 4     (int 1..32)   -- pyramid depth: buffer = 1/N
plugin:gloview:backdrop_color    = 0x73070a10         -- the dim, drawn separately
```

## Key Constraints

- Blur must be called with an active monitor render (`m_renderData.pMonitor` set) and projection `RPT_EXPORT`
- The filter switches viewport/fbSize for intermediate FBOs and leaves the final viewport at `(W,H)`
- `resolution` (pyramid depth) is the PRIMARY blur-strength control, not just anti-aliasing
- Each Kawase down+up step is a real box/tent filter; more levels = more compound blur
- The gaussian stage runs at the BOTTOM of the pyramid (small buffer), so 9 taps stay cheap regardless of output resolution
- Blend state is fully captured and restored in `CBlurFilter::render()` to avoid corrupting downstream draw calls
- **Blur strength radius lives in the bottom buffer's texels** (`blur.cpp:374-375`): `size * strength / 2^levels`, not the raw config value

## Fixed: black flash on overview open (2026-10-07)

Cause was **not** swapchain buffer-age, not solitary-client, not blend state:
the backdrop composite carried a 3-second expiry and was re-derived from a
wallpaper texture that was mid-recreation (i.e. black), and `hideLayers()` dimmed
the layers before the plugin had drawn its backdrop. Fixed by making the composite
permanent, comparing it against the source texture every frame, absorbing failed
attempts in a scratch framebuffer, and parking the overview until the composite is
really there. Details and the five dead ends are in REFACTORING.md.

## Agent skills

Publishing, fetching or triaging work: `docs/agents/issue-tracker.md` (GitHub Issues via `gh`).
Triage vocabulary: `docs/agents/triage-labels.md`.
Domain glossary and ADRs: `docs/agents/domain.md` (single-context; `/domain-modeling` creates them lazily).
