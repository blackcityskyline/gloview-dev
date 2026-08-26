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
├── anim/                 clocks (Tween + leaf resolution) and the curve registry (native + Lua curves)
├── config/               the typed option schema, grouped by domain; live handles
├── debug/                gated log channel (/tmp/gloview.log)
└── model/                the Model data structures (Tile/StripItem/Drag/...)

Layering rule: arrows point one way — main wires the domains; domains
(session/build/input/actions/render/anim) read the primitives (model/config/
layout/gl_util); the painter only reads Model+Clocks+Pixels and mutates
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

| Option | Type | Default | Description |
|---|---|---|---|
| `layout` | `rows` \| `grid` \| `natural` | `rows` | Main-area preview layout engine |
| `gap` | int (px) | `34` | Min spacing between window previews |
| `padding` | int (px) | `80` | Left/right outer margin of the preview area |
| `padding_top` | int (px) | `40` | Extra gap between the strip and the previews |
| `padding_bottom` | int (px) | `70` | Bottom outer margin |
| `max_scale` | float | `1.0` | Never enlarge a preview past real size × this |
| `duration` | int (ms) | `360` | Legacy shared animation length — followed by every `<leaf>_ms` left at `-1` |

### Animations

One master switch plus a registry of leaves, mirroring Hyprland's own
animation model. Every leaf expands to three options: `<leaf>_enabled` (0/1),
`<leaf>_ms` (-1 = follow `duration`; otherwise its own length), and
`<leaf>_curve` (`linear` / `easeout` / `easeinout` / `back`, or the name of
any custom curve registered from Lua — see below). Setting
`animations_enabled = 0` makes the ENTIRE plugin instant — open/close, glides,
pulses, everything — through a single choke point.

| Leaf | Default ms | Default curve | What it drives |
|---|---|---|---|
| `open` | via duration | easeout | entry reveal (chrome + tiles) |
| `close` | via duration | easeout | exit collapse |
| `reflow` | via duration | easeout | tile glide on drops/swaps/sync/close-home |
| `new_card` | via duration | back | "+" card pop-in |
| `swap_pulse` | 180 | back | success ring after swaps/moves |
| `strip_step` | 200 | easeinout | animated strip scroll per workspace step |
| `populate` | 250 | easeout | staggered tile population + ghost fade-out fallback (window close etc.) |
| `drop` | 320 | easeinout | drag/swap landings: the dragged preview flies from the release point into its new slot; strip thumbs fly between slots on swaps |
| `swap_main` | 320 | easeinout | the swap INITIATOR's flight (overrides `drop` for it) |
| `swap_partner` | 320 | easeinout | the swap PARTNER's flight |
| `expo_in` | 250 | easeout | one→all spread: tile glide + newcomers during the all↔one flip (`gloview:allworkspaces`) |
| `expo_out` | 250 | easeout | all→one collapse: tile glide + dispersing ghosts during the flip |
| `ws_in` | 250 | easeout | incoming tiles' timing on a workspace switch (styles via `ws_enter_anim`; the flip above uses `expo_*` instead) |
| `ws_out` | 250 | easeout | outgoing ghosts' timing on a workspace switch (styles via `ws_exit_anim`) |
| `drag_lift` | 150 | easeout | the dragged preview's lift ramp when a grid-tile drag picks up (scale 0.7→1 + alpha); disabled → instant |
| `grid_swap_anim` | `horizontal` \| `slidevert` \| `fade` \| `pop` | `horizontal` | how grid tiles choreograph a swap: travel toward each other / exit up + enter from the top / fade out + in / scale-pop in place |
| `strip_swap_anim` | same set | `horizontal` | the same styles for strip card thumbnails (slidevert always uses the strip's top edge) |
| `ws_enter_anim` | `pop` \| `slide` \| `slidevert` \| `fade` | `slide` | how the incoming workspace's tiles appear on a ws switch: scale-pop from the slot center / slide in from the direction side (by ws id order) / drop from the top edge / alpha fade |
| `ws_exit_anim` | `slide` \| `slidevert` \| `fade` | `slide` | how the outgoing workspace's tiles leave: slide out to the opposite side / exit through the top edge / fade+shrink in place |

**Custom curves (Lua).** Register a named curve once (e.g. next to the config
in `plugins/gloview.lua`) and use its name in any `<leaf>_curve`:

```lua
hl.plugin.gloview.curve("snap", function(t)
    return 1 - (1 - t) * (1 - t) * (1 - t) * (1 - t) -- quartic out
end)
-- then: populate_curve = "snap"
```

The function receives `t` in `0..1` and returns the shaped progress;
overshoot beyond `1` is allowed (that is what `back` does). Errors and
non-number results fall back to `easeout` and are logged once.

```lua
animations_enabled = 1,
-- populate_ms = 250, populate_curve = "easeout", populate_enabled = 1, ...
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
| `scroll_switches_workspace` | bool (0/1) | `1` | Wheel over the main area steps prev/next workspace |
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
| `strip_all_card` | bool (0/1) | `0` | Show a leading "All workspaces" card on the strip that toggles the expo view |
| `drag_to_swap` | bool (0/1) | `1` | Grid mode: dropping a preview onto another swaps the two windows' places — works across workspaces too in the all-workspaces (expo) view |
| `switch_on_drop` | bool (0/1) | `0` | Dropping a window on a card also follows it to that workspace |
| — | — | — | Dragging a preview (grid or strip) onto a workspace **card** with the left button moves it there; the **right** button swaps it with that workspace's last-focused window instead |
| `switch_on_new_workspace` | bool (0/1) | `1` | Clicking `+` follows the display to the new workspace |
| `new_workspace_mode` | `fill` \| `linear` | `fill` | `fill`: `+` takes the lowest free workspace id (backfills a gap left by a closed one) — `linear`: always appends past the highest existing id, never backfills |
| `close_button_color` | color/role | `"0xe6e23b3b"` | `✕` close-button fill — per-window, and per-workspace (closes every window on it) |
| `close_glyph_color` | color | `"0xffffffff"` | the `✕` glyph itself, drawn on top of `close_button_color` |
| `label_color` | color | `"0xf2ffffff"` | window/workspace name text under tiles and strip cards |
| `title_pill_color` | color | `"0xcc11151c"` | pill behind a tile's title label in desktop/canvas mode |
| `backing_color` | color | `"0xff14181f"` | translucent-tile safety backing — only its RGB is used, the renderer applies its own low alpha |
| `drop_hint_color` | color | `"0x38ffffff"` | drop-zone highlight flash on a workspace card while dragging |
| `close_button_visibility` | `shift` \| `always` | `shift` | `shift`: close buttons only show in desktop/canvas mode (`key_desktop`) — `always`: show them on every tile and strip card all the time |
| `close_button_icon` | string | `✕` | Glyph drawn in the close buttons |
| `close_button_size` | float | `1.0` | Scale multiplier over the computed base button size |
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
palette-resolved hex produced Lua-side from your theme module (the hyprbars
pattern) — gloview itself has zero coupling to any scheme engine:

| Value | Meaning |
|---|---|
| `"0xAARRGGBB"` / `"0xRRGGBB"` / `"#AARRGGBB"` / `"#RRGGBB"` / bare hex | Manual color (written as a **string**) |
| `col("primary")` etc. | Resolved by YOUR Lua config via `require(...)` — see below |

Scheme integration is fully optional and lives in the config:

```lua
-- noctalia module — same one hyprbars uses; any key it exposes works
local ok_c, c = pcall(require, "noctalia.noctalia-colors-extended")
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
so a missing module or a typo degrades gracefully instead of going black.
Noctalia rewrites its module on theme change; re-evaluating `hl.config{}`
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
                duration       = 200,
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
        duration = 200
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
