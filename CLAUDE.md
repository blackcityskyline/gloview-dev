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
[1] DUAL-KAWASE DOWN-CHAIN (blur.cpp:486-499)
  |-- Downsample level 1: W/2  x H/2
  |-- Downsample level 2: W/4  x H/4
  \-- Downsample level N: W/2^N x H/2^N  (= m_fbA)
       |
       v
[2] SEPARABLE GAUSSIAN at bottom (blur.cpp:501-507)
  |-- H pass: m_fbA -> m_fbB
  \-- V pass: m_fbB -> m_fbA  (repeated `m_passes` times)
       |
       v
[3] DUAL-KAWASE UP-CHAIN (blur.cpp:509-528)
  |-- Upsample level N-1: m_downFBs[N-2]
  \-- Upsample level 1:   dst framebuffer (full-res)
       |
       v
[4] DIM COMPOSITE (blur.cpp:530-546)
  \-- src-over blend of backdrop_color
```

### Config Keys (registered in main.cpp)
```
plugin:gloview:blur              = 1.0   (float 0..1)   -- backdrop blur strength (0=off)
plugin:gloview:blur_passes       = 3     (int 1..16)     -- gaussian iterations at pyramid bottom
plugin:gloview:blur_size         = 8     (int 1..200)    -- gaussian radius in screen pixels
plugin:gloview:blur_resolution   = 4     (int 1..32)     -- blur buffer = 1/N monitor resolution
```

### How Blur Transitions Work

The blur is **NOT independently animated** — it fades as part of the single `m_progress` animation curve:

1. **Open:** `m_progress` goes 0→1 via `easeOutCubic`. Blur alpha = `e * blurStrength()` (overview_render.cpp:817). First frame sets `m_blurDirty = true` (overview_core.cpp:712), blur is computed once, subsequent frames blit cached result.

2. **Close:** `m_progress` goes 1→0. Same `eased()` factor fades blur out. No re-blur during close — cached FBO drawn at progressively lower alpha.

3. **Workspace switch:** Blur cache is NOT invalidated unless source texture changes (e.g. fullscreen mpv). This prevents brightness shifts.

4. **Reflow (drag-drop):** `m_progress` stays pinned at 1.0 so blur stays settled.

### Blur Cache System

- `m_blurCacheFB` — persistent FBO holding blurred backdrop
- `m_blurDirty` — flag triggering re-blur (set on open, source change)
- `clearBlurCache()` resets both: `m_blurDirty = true; m_blurCacheFB.reset();`
- Cache invalidation tracks source identity via `m_cachedBackdropWs` / `m_cachedBackdropMpv`

## Files to Focus On

### Core Blur Implementation
- **`src/blur.hpp`** (122 lines) — `CBlurFilter` class declaration, all GL state, comments explaining the pyramid architecture
- **`src/blur.cpp`** (551 lines) — All 5 GLSL shaders (embedded), GL program compilation, `prepare()`, `render()` with the full 4-stage pipeline

### Blur Integration in Render Pipeline
- **`src/overview_render.cpp`** lines 805-925 — `renderBackdrop()`: the main call site that invokes `CBlurFilter`, manages blur cache FBO, draws blurred result
- **`src/overview_render.cpp`** line 476 — `eased()` function driving the animation curve
- **`src/overview_render.cpp`** lines 437-440 — `needsLiveBlur()` telling Hyprland renderer to refresh live-blur

### Blur Configuration & Lifecycle
- **`src/overview_core.cpp`** lines 500-518 — `blurEnabled()`, `blurStrength()`, `blurPasses()`, `blurSize()`, `blurResolution()` accessors
- **`src/overview_core.cpp`** lines 703-714 — `open()` sets `m_blurDirty = true` and `clearBlurCache()`
- **`src/overview_core.cpp`** lines 716-753 — `close()` starts close animation, no blur re-computation
- **`src/overview_core.cpp`** lines 967-972 — `clearBlurCache()` implementation

### Blur State in Overview
- **`src/overview.hpp`** lines 387-416 — Blur cache members: `m_blurCacheFB`, `m_blurDirty`, `m_blurFilter`, `m_cachedBackdropWs`, `m_cachedBackdropMpv`

### Config Registration
- **`src/main.cpp`** lines ~139-142 — Blur config key registration

## Key Constraints

- Blur must be called with an active monitor render (`m_renderData.pMonitor` set) and projection `RPT_EXPORT`
- The filter switches viewport/fbSize for intermediate FBOs and leaves the final viewport at `(W,H)`
- `resolution` (pyramid depth) is the PRIMARY blur-strength control, not just anti-aliasing
- Each Kawase down+up step is a real box/tent filter; more levels = more compound blur
- The gaussian stage runs at the BOTTOM of the pyramid (small buffer), so 9 taps stay cheap regardless of output resolution
- Blend state is fully captured and restored in `CBlurFilter::render()` to avoid corrupting downstream draw calls

## Open bug (deferred): black flash on overview open

Frame-traced, solitary/DS ruled out; prime suspect = swapchain buffer-age
(CDamageRing 3-deep ring) + async explicit-sync commits (this box runs
render:new_render_scheduling=true). `m_forceFullFrames=3` in open() did NOT
fix it. Full hypothesis board: CANDIDATES.md. Next step when resumed:
epoch-stamped trace (system_clock alongside steady_clock) + wf-recorder PTS
correlation protocol (claude_review.md history, commit ea8891e..).
