# GloView
[![license](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://github.com/fedsfarm/gloview/blob/main/LICENSE) [![#main:feds.farm](https://escape.feds.farm/feds.png)](https://escape.feds.farm/#main:feds.farm)

https://github.com/user-attachments/assets/0a3d812a-eae0-4ca5-8698-7a006e540857

A better macOS Mission Control-style overview plugin for Hyprland

## Architecture

```
src/
├── main.cpp              plugin entry: registers the config schema, actions, Lua API
├── overview.hpp          Session: owns the three stores (Model / Clocks / Pixels)
├── session/              lifecycle: open/close/deactivate, shouldRenderWindow + damageSurface hooks, animation pump
├── build/                Model construction: windows -> tiles/strip, reflow, sync
├── input/                pointer events -> Model (drag FSM), keyboard, alt-tab
├── actions/              commands: drop/swap/workspace/focus (dispatcher + hyprctl + Lua from one table)
├── render/               the painter (one pass element, fixed z-slots), backdrop blur cache,
│                         window-content leaf (immediate surface drawing), tile/strip views, fx
├── anim/                 the animation registry: enum Id, config keys, defaults, shape
│                         params, Lua rules. Also where the Lua C API is bridged (extern "C")
├── clock/                time domain: Tween, per-effect clocks, curve registry (native + Lua)
├── pixel/                caches: labels, close glyph, entry snapshots
├── config/               the typed option schema, grouped by domain; live handles
├── view/                 the view-state shared by input and render
├── debug/                gated log channel (/tmp/gloview.log)
├── model/                the Model data structures (Tile/StripItem/Drag/...)
├── layout.{hpp,cpp}      pure geometry: no Hyprland, no GL (the only ctest domain)
├── blur.{hpp,cpp}        self-contained dual-Kawase + gaussian, independent of decoration:blur
└── cursor.{hpp,cpp}      HW/SW cursor drawn on top (a consequence of RENDER_LAST_MOMENT)

Layering rule: arrows point one way — main wires the domains; domains
(session/build/input/actions/render) read the primitives (model/config/clock/
anim/pixel/layout/gl_util); the painter only reads Model+Clocks+Pixels and mutates
nothing. Whatever sits ON TOP of content draws AFTER it.
```

## Install

Via hyprpm:

```sh
hyprpm add https://github.com/fedsfarm/gloview
hyprpm enable gloview
```

### Arch (AUR)

```sh
yay -S gloview
```

### Nixos

```nix
inputs.gloview = { url = "github:fedsfarm/gloview"; inputs.hyprland.follows = "hyprland"; };
```
```nix
wayland.windowManager.hyprland = {
  enable = true;
  plugins = [ inputs.gloview.packages.${pkgs.system}.gloview ];
  settings.bind = [ "SUPER, TAB, gloview:toggle" ];
};
```

## Manual build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Produces `build/gloview.so`. The ABI must match the running Hyprland exactly —
build against the same headers, or a version skew gives a `.so` that crashes on
load. While iterating, `cmake --build build --target reload` rebuilds and
hot-reloads into the running Hyprland.

## Usage

Dispatchers: `gloview:toggle`, `gloview:open`, `gloview:close`, `gloview:desktop`,
`gloview:allworkspaces` (the all-workspaces "expo" view — opens into it if closed),
`gloview:alttab` / `gloview:alttabback` (alt-tab cycling — see `alt_tab_*` below).
Or `hyprctl gloview` / `gloviewclose` / `gloviewdesktop` / `gloviewall` / `gloviewalttab` / `gloviewalttabback`

Lua:

```lua
hl.bind("SUPER + TAB", hl.plugin.gloview.toggle)
hl.bind("SUPER + SHIFT + TAB", hl.plugin.gloview.desktop)
hl.bind("SUPER + CTRL + TAB", hl.plugin.gloview.allworkspaces)
```

```ini
bind = SUPER, TAB, gloview:toggle
bind = SUPER SHIFT, TAB, gloview:desktop
bind = SUPER CTRL, TAB, gloview:allworkspaces
```

### Alt-Tab

`gloview:alttab` / `gloview:alttabback` are meant to be bound to a single modifier+key
combo, the same way you'd normally bind a real alt-tab — bind the SAME key you might
otherwise give `gloview:allworkspaces`:

```ini
bind = SUPER, TAB, gloview:alttab
bind = SUPER SHIFT, TAB, gloview:alttabback
```

Closed → opens straight into the all-workspaces view with the selection on the first tile.
Already open → tapping the same bound key again (while still holding the modifier) advances
the cycle — Hyprland re-invokes the dispatcher on every physical press, exactly like holding
Alt and tapping Tab. Tiles keep their normal spatial grid order; cycling just walks the
selection through them. Set `alt_tab_modifier` to whatever modifier is in that bind (`alt`
by default) so releasing it can commit the selection — see the config table.

## Config

All keys live under `plugin:gloview:*`. Colors are `0xAARRGGBB` integers.

- **`rows`** (default) — macOS-like: previews keep their aspect ratio and are packed into balanced rows, with the row count chosen to make the previews as large as possible. Reads spatially like the real desktop.
- **`grid`** — uniform cells, one preview centered in each.
- **`natural`** — keeps each window's real on-screen position, uniformly scaling the whole arrangement to fit.
- **`carousel`** — a switch, not a layout: when on it lays the main area out as one overlapped horizontal run of cards with the selected window centered and its neighbours peeking out on both sides, on top of whatever `layout` says. It applies in the **all-workspaces view only** — a single workspace has nothing to arrange against — and with it off the layout runs untouched. It is only a different arrangement of the main area: the strip, workspace switching, the all↔one flip, alt-tab, drag-and-drop and canvas mode all keep working. See [Carousel mode](#carousel-mode).

| Option | Type | Default | Description |
|---|---|---|---|
| `layout` | `rows` \| `grid` \| `natural` | `rows` | Main-area preview layout engine |
| `gap` | int (px) | `34` | Min spacing between window previews |
| `padding` | int (px) | `80` | Left/right outer margin of the preview area |
| `padding_top` | int (px) | `40` | Extra gap between the strip and the previews |
| `padding_bottom` | int (px) | `70` | Bottom outer margin |
| `max_scale` | float | `1.0` | Never enlarge a preview past real size × this |
| `carousel` | int | `0` | Lay the main area out as a card run; all-workspaces view only, over any `layout` |
| `carousel_cards` | int | `5` | `carousel` only: how many cards the run shows (any count, even included) |
| `carousel_card_width` | int (px) | `0` | `carousel` only: narrow the standard box; 0 keeps the fullscreen box |
| `carousel_scale` | float | `1.0` | `carousel` only: zoom the whole run on top of the automatic fit (above 1 it deliberately overflows) |
| `carousel_tone_near` | float | `0.125` | `carousel` only: dim applied to the nearest unfocused card |
| `carousel_tone_far` | float | `0.25` | `carousel` only: dim applied to the farthest unfocused card |
| `carousel_depth_taper` | float | `0.8` | `carousel` only: how much each step deeper multiplies the dim by

### Animations

One master switch (`animations_enabled`, 0 = the whole plugin is instant) and
a set of LEAVES — one per ACTION. Every leaf reads four options:

| field | type | meaning |
|---|---|---|
| `<leaf>_enabled` | 0/1 | kill-switch for that one motion |
| `<leaf>_ms` | int (ms) | absolute duration; defaults to the leaf's built-in base, so existing configs are unchanged |
| `<leaf>_speed` | float | multiplier over the duration above — `2.0` is twice as fast, `0.5` twice as slow (floor: one frame) |
| `<leaf>_curve` | name | any registry curve: native (`linear`/`easeout`/`easeinout`/`back`) or registered from Lua |

Motions with selectable TYPES also read a `<thing>_style` key (listed below).

The imperative twin of these keys is the Lua API
`gloview.animation{ leaf = "...", enabled = bool, speed = n, ms = n, curve = "...", style = "..." }`
— whatever it sets wins over the static keys; unset fields fall through.

| Leaf | base ms | default curve | What it drives |
|---|---|---|---|
| `open` | 500 | easeout | entry reveal (chrome + tiles) |
| `close` | 500 | easeout | exit collapse |
| `glide` | 300 | easeout | tile movement slot ↔ slot (drops, syncs, close-home) |
| `appear` | 250 | easeout | staggered entry of brand-new tiles |
| `card` | 250 | back | "+" card pop-in |
| `pulse` | 180 | back | success ring after swaps/moves |
| `strip_step` | 200 | easeinout | animated strip scroll per workspace step |
| `ws_enter` | 250 | easeout | incoming tiles' timing on a workspace switch (style: `ws_enter_style`) |
| `ws_exit` | 250 | easeout | outgoing ghosts on a workspace switch (style: `ws_exit_style`) |
| `expo_in` | 250 | easeout | one→all spread during the all↔one flip (`gloview:allworkspaces`) |
| `expo_out` | 250 | easeout | all→one collapse during the flip |
| `jump_in` | 180 | easeout | one→all flip started from the keyboard (`jump_style`) |
| `jump_out` | 220 | easeout | all→one flip started from the keyboard |
| `swap_main` | 320 | easeinout | the swap INITIATOR's flight |
| `swap_partner` | 320 | easeinout | the swap PARTNER's flight |
| `drop` | 400 | easeout | the landing of a preview released into a slot |
| `swap_fly` | 400 | easeout | the fly-between-slots of a swap |
| `lift` | 400 | easeout | the raise of a preview at pickup (see `lift_scale`, `lift_rise`) |
| `carousel_step` | 300 | easeout | the carousel run's selection step |
| `new_card` | 250 | easeout | a workspace card that appeared on its own |

`drop`, `swap_fly` and `lift` used to be one `drag` leaf, which meant a single
`_speed` moved all three at once and none could be tuned apart.

Shape parameters — not timings, but "how it looks", so they are plain floats
rather than per-leaf keys:

| Key | default | meaning |
|---|---|---|
| `appear_stagger` | `0.08` | fraction of `appear` each tile waits before starting |
| `appear_stagger_per_tile` | `0.015` | extra stagger per tile index |
| `carousel_step_collapse` | `0.5` | fraction of the step over which unfocused cards collapse |
| `pulse_ring_scale` | `0.10` | ring growth per pulse frame |
| `lift_scale` | `0.07` | how much a lifted preview grows |
| `lift_rise` | `5.0` | px a lifted preview rises |

Style keys:

| Key | values | default |
|---|---|---|
| `ws_enter_style` | `pop` \| `slide` \| `slidevert` \| `fade` | `slide` |
| `ws_exit_style` | `slide` \| `slidevert` \| `fade` | `slide` |
| `grid_swap_style` | `horizontal` \| `slidevert` \| `fade` \| `pop` | `horizontal` |
| `strip_swap_style` | same set | `horizontal` |
| `jump_style` | `instant` \| `horizontal` \| `slidevert` \| `fade` \| `pop` | `instant` |

**Custom curves (Lua).** Register a named curve once (e.g. next to the config
in `plugins/gloview.lua`) — either as a function or as a cubic bezier:

```lua
-- function form: receives t in 0..1, returns shaped progress (overshoot OK)
gloview.curve("snap", function(t) return 1 - (1 - t)^3 end)

-- bezier form (CSS-style control points), both spellings work:
gloview.curve("fluent_decel", { type = "bezier", points = { {0, 0.2}, {0.4, 1} } })
gloview.curve("easeOutCubic", { 0.33, 1, 0.68, 1 })

-- then use it anywhere:
gloview.animation({ leaf = "glide", curve = "fluent_decel" })
```

| `preview_round` | int (px) | `12` | Corner radius for every window-shaped preview — main grid tiles AND strip card previews (with their borders/shadows) — clamped down automatically on the smaller strip thumbnails |
| `preview_round_power` | float | `2.0` | Corner curve exponent (`2` = circular, higher = squarer "squircle"), applied consistently to the same set of elements as `preview_round` |
| `strip_preview_round` | — | — | **Deprecated**, no longer read — strip window previews now share `preview_round`/`preview_round_power` |
| `blur` | float `0`..`1` | `1.0` | Backdrop + strip blur strength (`0` = off; fractions allowed) |
| `blur_passes` | int `1`..`16` | `3` | Backdrop blur gaussian iterations (more = softer) |
| `blur_size` | int (px) | `8` | Backdrop blur radius in screen pixels |
| `blur_resolution` | int `1`..`32` | `4` | Backdrop blur buffer = `1/N` monitor resolution (lower = sharper/cleaner, slightly more GPU; the blur is computed once per overview open, so the cost is negligible) |
| `anchor` | `top` \| `bottom` \| `left` \| `right` | `top` | Edge the workspace strip attaches to |
| `strip_offset` | int (px) | `0` | Inset from the anchored edge (0 = flush, no gap) |
| `strip_height` | int (px) | `150` | Strip band thickness, label included |
| `strip_margin` | int (px) | `22` | Padding around the strip |
| `strip_gap` | int (px) | `18` | Spacing between workspace cards |
| `strip_card_round` | int (px) | `10` | Workspace card corner radius |
| `backdrop_color` | color/role | `"0x73070a10"` | Dim + blur fill over the desktop |
| `strip_band_color` | color/role | `"0x24ffffff"` | Band behind the cards |
| `strip_band_opacity` | int `0`..`100` | `70` | Opacity of the strip band, in percent. This is the band's ONLY alpha source — `strip_band_color` supplies the RGB. Below ~60 the band reads at two different densities depending on whether a blurred or a sharp backdrop sits behind it (the backdrop is sharp on an empty landing workspace, blurred when windows are present) |
| `strip_card_color` | color/role | `"0x3a0e131c"` | Inactive workspace card fill |
| `strip_active_color` | color/role | `"0x4d1c2c44"` | Active workspace card fill |
| `strip_active_border` | color/role | `"0xf0ffffff"` | Active card outline |
| `strip_hover_border` | color/role | `"0x80ffffff"` | Hovered card outline |
| `strip_plus_color` | color/role | `"0xd0eef4ff"` | The "+" glyph |
| `strip_all_color` | color/role | `"0xd0eef4ff"` | The "All workspaces" 2×2 glyph — own key, independent of `strip_plus_color` |
| `shadow_color` | color/role | `"0x70000000"` | Window preview drop shadow |
| `hover_border` | color/role | `"0xf0ffffff"` | Hovered window preview outline — a real border stroke, drawn only around the tile, never a fill over it |
| `select_border` | color/role | `"0xf066ccff"` | Keyboard-selected preview outline (Alt-Tab's cursor) — same real-stroke rendering as `hover_border` |
| `select_border_size` | int (px) | `3` | Keyboard-selected preview outline thickness — `0` disables it entirely |
| `hover_border_size` | int (px) | `3` | Hovered/focused preview outline thickness — `0` disables it entirely |
| `show_focus_border` | bool (0/1) | `1` | Whether hover/selection draw `hover_border`/`select_border` at all — see "Border modes" below |
| `show_border` | bool (0/1) | `0` | An always-on base ring on every tile, independent of hover/selection — see "Border modes" below |
| `border_color` | color/role | `"0x50ffffff"` | `show_border`'s ring color |
| `border_size` | int (px) | `2` | `show_border`'s ring thickness — `0` disables it |
| `focus_follows_mouse` | bool (0/1) | `1` | Keyboard selection tracks the hovered preview |
| `scroll_switches_workspace` | bool (0/1) | `1` | Wheel over the main area steps prev/next workspace. A carousel run pages on its own instead, see `carousel_scroll_speed` |
| `carousel_scroll_speed` | float | `1.0` | `carousel` only: wheel sensitivity — notches × speed = cards moved, so `1` is one card per notch and `0.5` one card every two. Fractions carry over instead of stalling. Independent of `scroll_switches_workspace`: paging the run is not a workspace switch |
| `passthrough_keys` | bool (0/1) | `1` | Let keys the overview doesn't use reach Hyprland (keybinds keep working) |
| `key_close` | key names | `escape` | Keys that dismiss |
| `key_next_workspace` | key names | `tab` | Cycle the displayed workspace forward (wraps); `""` to disable. Held modifiers match exactly, so e.g. a `SUPER+Tab` toggle bind still passes through and closes |
| `key_prev_workspace` | key names | `shift+tab` | Cycle the displayed workspace backward (`mod+key` combos supported: `shift`/`ctrl`/`alt`/`super`) |
| `key_activate` | key names | `enter` | Keys that focus the selected preview |
| `key_close_window` | key names | `d` | Keys that close the selected preview's window (overview stays open); `""` to disable |
| `key_left` / `key_right` / `key_up` / `key_down` | key names | `left` / `right` / `up` / `down` | Move the keyboard selection (e.g. set `h`/`l`/`k`/`j` for vim nav) |
| `key_desktop` | key names | `shift` | Flip canvas↔grid |
| `key_all_workspaces` | key names | `a` | Toggle the all-workspaces (expo) view; `""` to disable |
| `key_workspace` | key names | `1,2,3,4,5,6,7,8,9,0` | Key at position N switches DIRECTLY to workspace N+1 (so `0` is always workspace 10) — creates it first if needed, independent of what's currently on the strip |
| `key_workspace_mode` | `switch` \| `jump` | `switch` | `switch`: a digit changes the displayed workspace and the overview stays open (Ctrl+digit is a no-op) — `jump`: a digit switches AND immediately closes the overview; hold Ctrl+digit for the old stay-open behavior |
| `alt_tab_modifier` | `alt` \| `ctrl` \| `shift` \| `super` | `alt` | Which modifier's release commits the Alt-Tab selection (match whatever you bound `gloview:alttab` with) |
| `alt_tab_commit_on_release` | bool (0/1) | `1` | Releasing `alt_tab_modifier` focuses the selection & closes, like a normal alt-tab — off: releasing does nothing, confirm with `key_activate`/click instead |
| `exit_on_click` | bool (0/1) | `1` | Click on empty space dismisses the overview |
| `exit_on_switch` | bool (0/1) | `0` | Dismiss when the live workspace changes underneath (e.g. a keybind) |
| `show_all_workspaces` | bool (0/1) | `0` | Main area shows every window on the monitor (expo), not just the displayed workspace. Toggle live with `gloview:allworkspaces`, the `key_all_workspaces` key, or the strip's "All" card |
| `strip_wallpaper` | bool (0/1) | `1` | Empty workspace cards show a cover-fit wallpaper thumbnail (the same source the backdrop uses) instead of a flat body |
| `strip_empty_mode` | `show` \| `neighbors` \| `hide` | `show` | `show`: every numeric workspace up to the highest one in use (at least 1-10) gets a strip card, even ones that were never created — `neighbors`: only occupied workspaces plus the displayed one's immediate numeric neighbors (reveals a run of empties one hop at a time as you navigate into it) — `hide`: only occupied workspaces, no empty ones at all. `show`/`neighbors` cards for a workspace that doesn't exist yet are created lazily on click/drop, same as `+` but at that specific number. |
| `show_special` | bool (0/1) | `0` | Include the special (scratchpad) workspace as a strip card |
| `alttab_advances` | bool (0/1) | `0` | Alt-tab: the entry is aimed at the NEXT card, so the run travels to one target from its first frame and lands on it — one motion, taking exactly the entry's own time. `0` opens on the focused window and a second press steps |
| `strip_all_card` | bool (0/1) | `0` | Show a leading "All workspaces" card on the strip that toggles the expo view |
| `drag_to_swap` | bool (0/1) | `1` | Grid mode: dropping a preview onto another swaps the two windows' places — works across workspaces too in the all-workspaces (expo) view |
| `switch_on_drop` | bool (0/1) | `0` | Dropping a window on a card also follows it to that workspace |
| — | — | — | Dragging a preview (grid or strip) onto a workspace **card** with the left button moves it there; the **right** button swaps it with that workspace's last-focused window instead |
| `switch_on_new_workspace` | bool (0/1) | `1` | Clicking `+` follows the display to the new workspace |
| `hold_lift_ms` | int (ms) | `400` | Hold a dragged preview still this long before it lifts and starts scaling — separates a hold from a drag |
| `tile_layout` | `none` \| `dwindle` | `none` | `dwindle`: after a swap, re-run Hyprland's dwindle layout on the target workspace so neighbours fill the freed space; `none` leaves the layout alone |
| `new_workspace_mode` | `fill` \| `linear` | `fill` | `fill`: `+` takes the lowest free workspace id (backfills a gap left by a closed one) — `linear`: always appends past the highest existing id, never backfills |
| `close_button_color` | color/role | `"0xe6e23b3b"` | `✕` close-button fill — per-window, and per-workspace (closes every window on it) |
| `close_glyph_color` | color | `"0xffffffff"` | the `✕` glyph itself, drawn on top of `close_button_color` |
| `label_color` | color | `"0xf2ffffff"` | window/workspace name text under tiles and strip cards |
| `title_pill_color` | color | `"0xcc11151c"` | pill behind a tile's title label in desktop/canvas mode |
| `lift_ms` | int | `400` | the raise when a tile is picked up |
| `swap_fly_ms` | int | `400` | the flight between slots on a swap |
| `drop_ms` | int | `400` | window landing after a drop |
| `carousel_count_pill_color` | color | `"0xcc11151c"` | pill behind the carousel counter (`1 / 4`) |
| `appear_stagger` | float | `0.08` | entry ladder: how far the tile entry is spread across the window (as a fraction) |
| `appear_stagger_per_tile` | float | `0.015` | extra spread per tile, capped by `appear_stagger` |
| `carousel_step_collapse` | float | `0.5` | fraction of the exit spent shrinking the unfocused carousel cards |
| `pulse_ring_scale` | float | `0.10` | how far the post-drop pulse ring reaches past the tile edge |
| `lift_scale` | float | `0.07` | how much a picked-up tile grows |
| `lift_rise` | float | `5.0` | how far a picked-up tile rises, px |
| `kill_zone` | int | `1` | closing a window by dragging it to the screen edge; `0` makes the edge a no-op |
| `kill_zone_size` | float | `40` | thickness of that edge zone, px |
| `kill_zone_falloff` | float | `150` | how far from the zone its glow reaches, px |
| `kill_zone_reach` | int (px) | `160` | how far from the screen edge the drop target reaches |
| `click_window_ms` | int | `400` | window in which two clicks count as a double-click (`close_trigger=doubleclick`) |
| `strip_label_band` | float | `26` | height of the label strip under a strip card |
| `strip_label_extra` | float | `24` | how far a strip card widens to fit its label |
| `strip_slide_frac` | float | `0.55` | fraction of the band thickness the slide-out covers |
| `strip_offset_max` | float | `0.42` | hard ceiling on `strip_offset`, as a fraction of the cross axis |
| `shadow_radius` | float | `6` | tile shadow blur at rest |
| `shadow_radius_lift` | float | `14` | tile shadow blur while dragged |
| `shadow_alpha` | float | `0.18` | tile shadow opacity multiplier |
| `title_pad_x` | float | `14` | horizontal padding inside the title pill |
| `title_pad_y` | float | `6` | vertical padding inside the title pill |
| `title_gap` | float | `10` | gap between a tile and its title pill |
| `title_margin` | float | `6` | screen margin kept around the title pill |
| `close_glyph_fill` | float | `0.62` | fraction of the close button the glyph fills |
| `drop_hint_color` | color | `"0x38ffffff"` | drop-zone highlight flash on a workspace card while dragging |
| `close_button_visibility` | `shift` \| `always` | `shift` | `shift`: close buttons only show in desktop/canvas mode (`key_desktop`) — `always`: show them on every tile and strip card all the time |
| `close_button_icon` | string | `✕` | Glyph drawn in the close buttons |
| `close_button_size` | float | `1.0` | Scale multiplier over the computed base button size |
| `drag_size` | float (0.15–1.0) | `0.55` | Drag-phase preview size as a fraction of its source slot — applies to grid tiles and strip thumbs alike; `1.0` keeps the full-size preview under the cursor |
| `drag_offset_x` | float (px) | `46.0` | Drag preview offset from the pointer, x — keeps the drop zones visible under it |
| `drag_offset_y` | float (px) | `64.0` | Drag preview offset from the pointer, y |
| `swap_lift` | float (px) | `12.0` | vertical lift of a `slidevert` swap |
| `close_button_position` | `top-right` \| `top-left` \| `bottom-right` \| `bottom-left` | `top-right` | Corner the close button sits in |
| `close_trigger` | `button` \| `doubleclick` | `button` | `button` (default): the "✕" close button — `doubleclick`: double-click/double-tap directly on a tile closes it instead (the "✕" is hidden); `key_close_window` and the strip card's whole-workspace "✕"/middle-click are unaffected either way |
| `hide_top_layers` | bool (0/1) | `0` | Fade out Top layer surfaces (bars, e.g. Waybar) while open |
| `hide_overlay_layers` | bool (0/1) | `0` | Fade out Overlay layer surfaces (popups/notifications) while open |
| `above_namespaces` | string | `""` | Comma/space list of layer namespaces to draw *above* the overview (trailing `*` glob; a namespace containing `aboveoverview` always qualifies) |
| `preview_mode` | `live` \| `snapshot` | `live` | `live` (default): each tile renders its window's actual surface every frame — video/animations keep running, at the cost of GPU per visible preview — `snapshot`: tiles show a frozen texture captured at open time (no per-frame surface rendering; cheaper GPU, but content is static under the overlay) |
| `cursor_mode` | `auto` \| `software` | `auto` | `auto` (default): prefers Hyprland's hardware cursor (KMS cursor plane) when the driver exposes one — the cursor plane is composited above the framebuffer, so it stays visible over the dim backdrop with zero framebuffer writes, zero erase cycles, and zero GPU cost per mouse move — `software`: forces gloview to draw the cursor itself, first erasing the previous position with a fully-opaque backdrop-colored rect (use only if you observe stale cursor artifacts with hardware mode) |
| `debug_logs` | bool (0/1) | `0` | Verbose `[gloview]` logging |

`top`/`bottom` give a horizontal strip, `left`/`right` a vertical one. `anchor`
supersedes the older `bar_position` (top/bottom only); set `anchor` and it wins.

### Carousel mode

`carousel = 1` rearranges the MAIN AREA into a window switcher run: one row of
overlapped fixed-width cards with the selected window centered. It is a switch
laid over `layout`, not a layout of its own: `layout` keeps saying `rows`,
`grid` or `natural` and decides everything the run does not. The workspace strip
stays exactly where it is, and workspace switching, the all↔one flip, alt-tab,
drag-and-drop and canvas mode all keep working.

**It only applies in the all-workspaces view.** A run exists to arrange many
windows against each other; a single workspace's handful of tiles has nothing to
arrange against, so toggling all↔one switches the arrangement too, not just the
window set. With `carousel = 0` the layout runs untouched in both views.

```
            2 / 7                          counter (n / total)
┌───┐┌─┐┌───┴───┐┌─┐┌───┐
│   ││ ││       ││ ││   │                  the selected card is centered;
└───┘└─┘└───────┘└─┘└───┘                  neighbours overlap its edges
```

- **One standard box for every card.** Every card is the same rect — a
  fullscreen window's box, to the pixel, in width and height — whatever window
  holds it. A window smaller than the box is fitted whole inside it; neither the
  box nor the window is deformed. Because the box never depends on the window,
  the run's geometry cannot shift with the selection: only the content moves.
- **The focused card sits on the centre of the FREE SPACE, not of the screen.**
  The strip's band is part of the layout's padding on whichever edge it is
  anchored to, so the centre follows it: a top or bottom strip leaves the centre
  where it is, a left or right one shifts it.
- **The run tapers and overlaps.** The focused card is the standard box; each
  step out is a fraction of it (100%, 76%, 56%, then 8% smaller per step), and
  the step to a neighbour is 81.6% of *that neighbour's own width*, so every card
  is tucked in by the same share and the taper changes the size but not how
  tight the run looks. Because the focused card is placed absolutely rather
  than by centring the run, the run need not be symmetric about it.
- **The ladder is symmetric top and bottom.** Cards are centred on the focused
  card vertically, not lined up by their top edges — a run that only steps
  downwards reads as cards hanging off one.
- **On close the run leaves as a deck.** The focused card flies home and grows
  into the real window, staying whole for the handoff; every other card shrinks
  to its own centre and is gone well before it starts growing. They cannot fly
  home: a maximized window, and every window on a workspace that is not the one
  on screen, all share one geometry, so their returns are the same rectangle and
  the whole run lands on top of the focused card, which is drawn last. That is
  why the exit read as no exit at all — it was drawn, exactly under the landing,
  for the whole close.
- **The landing is the same motion everywhere.** The focused tile is never
  staggered and never given a private lerp: it rides the ordinary
  `natural → target` lerp on the close curve, identically in the carousel and in
  one-workspace mode. Both of those used to exist separately, which is why the
  same window landed differently depending on the view it came from.
- **Preview windows are always opaque.** Every preview goes through one render
  path that pins the surface alpha to 1, so the desktop's own `active_opacity` /
  `inactive_opacity` rules never make a preview see-through; depth is carried by
  size and overlap instead.
- **The title pill never outgrows its card.** A title wider than the tile under
  it is shortened to fit with a trailing `…`; the pill re-renders at the largest
  prefix that fits and is cached, so it costs one render per card, not per frame.
- **A drop zone at the opposite edge closes.** A card dragged from the run onto
  the 50px band along the edge opposite the strip's anchor closes that window, in
  the carousel and in any plain layout alike. The band spans the **whole monitor
  width** — a narrow target in the middle of a wide edge is something to aim at,
  and the glow reads as a panel rather than as light.
  It is invisible at rest. While a card is in flight the edge glows in proportion
  to how close the cursor is — full inside the band, nothing ~150px away,
  smoothstepped in between — so it guides the drop instead of appearing only once
  you are already on top of it. The glow runs soft at the band's inner edge to
  densest at the screen edge, then bleeds 150px further into the screen.
- **Only the centred card is framed.** The rest carry no ring, and the ring is
  one fixed size everywhere rather than scaled down to suit a smaller card.
- **Card count is yours.** `carousel_cards` sets how many cards the run shows —
  any number, even included (with an even count the centred card sits one slot
  off the exact middle). Windows beyond the count are **not drawn**; the run
  never shows a card that is not in it. The whole run scales down by one factor
  if it does not fit, so nothing is ever cut off. `carousel_card_width` narrows
  the standard box (0 = the fullscreen box).
- **The chrome is the plugin's own**, exactly as in every other layout, because
  it is literally the same code: a soft drop shadow, the frost underlay while the
  backdrop is still fading, a title pill under the **centred** card (and the
  hovered one), and a bare `n / total` counter above the run. Nothing is filled
  in behind a window — a card is the window plus its frame, and where they
  differ the backdrop shows.
- **Depth.** Cards one step out are dimmed slightly, cards two steps out more so
  (`0.125` / `0.25` of the shadow colour), thinning further past that so a deep
  run doesn't go black. Cards paint far → centred, so the centred one and its ring
  sit on top.

| Option | Type | Default | Description |
|---|---|---|---|
| `carousel` | int | `0` | Lay the main area out as a card run instead of `layout`; all-workspaces view only |
| `carousel_on_open` | bool (0/1) | `1` | `carousel` only: whether the run is on when the all-workspaces view is opened from outside. `key_carousel` ignores this |
| `key_carousel` | key | `c` | From one-workspace: switch to all-workspaces **with** the run on. From all-workspaces: flip the run only, no view change |
| `carousel_cards` | int | `5` | How many cards the run shows (any count, even included) |
| `carousel_card_width` | int (px) | `0` | Narrow the standard box; 0 keeps the fullscreen box |
| `carousel_scale` | float | `1.0` | Zoom the whole run on top of the automatic fit; above 1 it deliberately overflows |
| `carousel_scroll_speed` | float | `1.0` | Wheel sensitivity: notches × speed = cards moved. `1` is one card per notch, `0.5` one every two, `3` three per notch |

- **The run has its own toggle, and it is asymmetric.** `key_carousel` (`c` by
  default) does not behave like the `a` binding in both directions: from
  one-workspace it switches to all-workspaces *and* puts the run on, the same as
  `a` plus the carousel; from all-workspaces it only flips the run, never the
  view. Both directions go through the same full rebuild the `a` binding uses —
  a bare relayout leaves the carousel era's boxes behind and the grid comes out
  scattered. There is deliberately no path from all back to one — that is what `a` is
  for. `carousel_on_open` decides the state when the view is opened from outside,
  and the key ignores it.
- **Click to bring a card to centre, click again to leave.** A click on a card
  that is not the focused one moves the run to it and stays in the overview; only
  the focused card commits on click. So an off-centre card costs two clicks — one
  to bring it in, one to leave on it — and the centred one costs one.
- **Navigation.** Because the slots are built around the selection, moving it
  rotates the run (all cards glide to their new depths on the usual `glide`
  clock, without the per-tile stagger — it is one band, not a queue).
  - Left/Right step the centered card, circularly; up/down do nothing.
  - Tab/Shift-Tab, the digit keys and the `gloview:allworkspaces` toggle keep
    their normal meaning: **workspaces**, not cards.
  - Alt-Tab still cycles the selection; Enter or a click focuses the selected
    window and dismisses.
  - `✕` on a card closes that window.
- **Hover** does not move the selection — the slots are built around it, so
  following the mouse would re-lay-out the run on every crossing. Hover swaps
  the ring.

Left out of the source design: app icons and its per-card open/close scale-slide
(the plugin's own tile glide covers the motion). Colors, border sizes, corner
rounding and durations all come from the usual `look.*` / `colors.*` / `anim.*`
options.

Canvas mode still overrides the carousel (it is a different engine entirely and
keeps its existing precedence over `layout`), so `Shift` while in carousel mode
parks the previews at their real on-screen positions; `Shift` again returns to
the carousel.

### Border modes

`show_border` and `show_focus_border` are independent toggles, not a fixed set of modes — combine
them however you like:

| `show_border` | `show_focus_border` | Result |
|---|---|---|
| `0` | `0` | No borders at all |
| `0` | `1` (default) | Borders only on hover/keyboard-selection — the original look |
| `1` | `0` | A constant ring on every tile that never changes |
| `1` | `1` | A constant ring that visibly changes color/thickness on focus, since `hover_border`/`select_border` draw right over it |

`border_color`/`border_size` style the always-on ring; `hover_border`/`select_border` and their
`_size` options style the focus ring, same as before.

### Colors

Every color option takes ONE string: a hex literal in any accepted form, or a
palette-resolved hex produced Lua-side from your theme module — gloview itself
has zero coupling to any scheme engine:

| Value | Meaning |
|---|---|
| `"0xAARRGGBB"` / `"0xRRGGBB"` / `"#AARRGGBB"` / `"#RRGGBB"` / bare hex | Manual color (written as a **string**) |
| `col("primary")` etc. | Resolved by YOUR Lua config via `require(...)` — see below |

Scheme integration is fully optional and lives in the config:

```lua
-- any Lua module that exposes color roles as hex strings
local ok_c, c = pcall(require, "my-colors")
local function col(key, fallback)
    if ok_c and type(c[key]) == "string" then return c[key] end
    return fallback
end
local function with_alpha(color_str, alpha_hex)
    local hex = string.match(color_str, "#?(%x%x%x%x%x%x)")
    if not hex then return color_str end
    return "#" .. alpha_hex .. hex
end

hover_border        = with_alpha(col("primary", "ffffff"), "f0"),
close_button_color  = with_alpha(col("error",   "e23b3b"), "e6"),
strip_band_color    = with_alpha(col("primary", "ffffff"), "24"),
```

A value that doesn't parse falls through to that option's documented default,
so a missing module or a typo degrades gracefully instead of going black. If
your module rewrites itself on a theme change, re-evaluating `hl.config{}`
applies the new colors live.

### Lua

```lua
    hl.config({
        plugin = {
            gloview = {
                layout         = "rows",
                gap            = 34,
                padding        = 80,
                padding_top    = 40,
                padding_bottom = 70,
                max_scale      = 1.0,
                preview_round       = 12,
                preview_round_power = 2.0,
                blur           = 1,

                anchor           = "top",
                strip_offset     = 0,
                strip_height     = 150,
                strip_margin     = 22,
                strip_gap        = 18,
                strip_card_round = 10,

                focus_follows_mouse       = 1,
                scroll_switches_workspace = 1,
                passthrough_keys          = 1,
                exit_on_click             = 1,
                exit_on_switch            = 0,

                key_close     = "escape",
                key_next_workspace = "tab",
                key_prev_workspace = "shift+tab",
                key_activate  = "enter",
                key_close_window = "d",
                key_left      = "left",
                key_right     = "right",
                key_up        = "up",
                key_down      = "down",
                key_desktop   = "shift",
                key_all_workspaces = "a",
                key_workspace = "1,2,3,4,5,6,7,8,9,0",
                key_workspace_mode = "switch",
                alt_tab_modifier              = "alt",
                alt_tab_commit_on_release     = 1,

                show_all_workspaces     = 0,
                strip_empty_mode        = "show",
                show_special            = 0,
                strip_all_card          = 1,
                drag_to_swap            = 1,
                switch_on_drop          = 0,
                switch_on_new_workspace = 1,
                new_workspace_mode      = "fill",

                hide_top_layers     = 0,
                hide_overlay_layers = 0,
                above_namespaces    = "",
                debug_logs = 0,

                select_border_size  = 3,
                select_border       = "0xf066ccff",
                hover_border_size   = 3,
                show_focus_border   = 1,
                show_border         = 0,
                border_color        = "0x50ffffff",
                border_size         = 2,
                close_button_color    = "0xe6e23b3b",
                close_button_visibility = "shift",
                close_button_icon       = "✕",
                close_button_size       = 1.0,
                close_button_position   = "top-right",
                close_trigger           = "button",
                backdrop_color      = "0x73070a10",
                strip_band_color    = "0x24ffffff",
                strip_card_color    = "0x3a0e131c",
                strip_active_color  = "0x4d1c2c44",
                strip_active_border = "0xf0ffffff",
                strip_hover_border  = "0x80ffffff",
                strip_plus_color    = "0xd0eef4ff",
                strip_all_color     = "0xd0eef4ff",
                shadow_color        = "0x70000000",
                hover_border        = "0xf0ffffff",
            },
        },
    })
```

### hyprland.conf

```ini
plugin {
    gloview {
        layout = rows
        gap = 34
        padding = 80
        padding_top = 40
        padding_bottom = 70
        max_scale = 1.0
        preview_round = 12
        preview_round_power = 2.0
        blur = 1

        anchor = top
        strip_offset = 0
        strip_height = 150
        strip_margin = 22
        strip_gap = 18
        strip_card_round = 10

        focus_follows_mouse       = 1
        scroll_switches_workspace = 1
        passthrough_keys          = 1
        exit_on_click             = 1
        exit_on_switch            = 0

        key_close     = escape
        key_next_workspace = tab
        key_prev_workspace = shift+tab
        key_activate  = enter
        key_close_window = d
        key_left      = left
        key_right     = right
        key_up        = up
        key_down      = down
        key_desktop   = shift
        key_all_workspaces = a
        key_workspace = 1,2,3,4,5,6,7,8,9,0
        key_workspace_mode = switch
        alt_tab_modifier = alt
        alt_tab_commit_on_release = 1

        show_all_workspaces     = 0
        strip_empty_mode        = show
        show_special            = 0
        strip_all_card          = 0
        drag_to_swap            = 1
        switch_on_drop          = 0
        switch_on_new_workspace = 1
        new_workspace_mode      = fill

        hide_top_layers     = 0
        hide_overlay_layers = 0
        above_namespaces    =
        debug_logs = 0

        select_border_size  = 3
        select_border       = 0xf066ccff
        hover_border_size   = 3
        show_focus_border   = 1
        show_border         = 0
        border_color        = 0x50ffffff
        border_size         = 2
        close_button_color    = 0xe6e23b3b
        close_button_visibility = shift
        close_button_icon       = ✕
        close_button_size       = 1.0
        close_button_position   = top-right
        close_trigger           = button
        backdrop_color      = 0x73070a10
        strip_band_color    = 0x24ffffff
        strip_card_color    = 0x3a0e131c
        strip_active_color  = 0x4d1c2c44
        strip_active_border = 0xf0ffffff
        strip_hover_border  = 0x80ffffff
        strip_plus_color    = 0xd0eef4ff
        strip_all_color     = 0xd0eef4ff
        shadow_color        = 0x70000000
        hover_border        = 0xf0ffffff
    }
}
```

## Donate

#### BTC:
`bc1p2xkwf9elq8wgajtq2cc6zthuh4k998tgnk6365cnjqgal7mpd09q4jtfq8`  
#### ETH:
`0xBD636eBD3a6b9F046930101657459E90DA370e81`  
#### XMR:
`42uxSBp4aMyTAsPCMGEwHvJyGpemr1c7kdjtFsD5tnEsU7XsnYMjseyXBzLWHkruSWFGbQWagsh31bBRdU7vDNUBAzm1Mo4`  
