# GloView
[![license](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://github.com/fedsfarm/gloview/blob/main/LICENSE) [![#main:feds.farm](https://escape.feds.farm/feds.png)](https://escape.feds.farm/#main:feds.farm)

https://github.com/user-attachments/assets/0a3d812a-eae0-4ca5-8698-7a006e540857

A better macOS Mission Control-style overview plugin for Hyprland

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

Closed → opens straight into the all-workspaces view with the grid itself reordered into MRU
(most-recently-used) order — not the usual spatial layout — and the cursor already on the
*previously* focused window (that first invocation already counts as "one tab", and always
lands there regardless of whether it was `gloview:alttab` or `gloview:alttabback` that
opened it, same as a real alt-tab's first press). Already open → tapping the same bound key
again (while still holding the modifier) advances the cycle — Hyprland re-invokes the
dispatcher on every physical press, exactly like holding Alt and tapping Tab; cycling all the
way around returns to the window you started on, same as a real alt-tab. Set `alt_tab_modifier`
to whatever modifier is in that bind (`alt` by default) so releasing it can commit the
selection — see the config table.

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
| `duration` | int (ms) | `360` | Open/close animation length |
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
| `alt_tab_mode` | `smart` \| `linear` | `smart` | `smart`: first hop lands on the most-recently-focused window (Hyprland's own system-wide focus history — not just windows gloview itself focused), then walks back through recency — `linear`: simple fixed circular order |
| `exit_on_click` | bool (0/1) | `1` | Click on empty space dismisses the overview |
| `exit_on_switch` | bool (0/1) | `0` | Dismiss when the live workspace changes underneath (e.g. a keybind) |
| `show_all_workspaces` | bool (0/1) | `0` | Main area shows every window on the monitor (expo), not just the displayed workspace. Toggle live with `gloview:allworkspaces`, the `key_all_workspaces` key, or the strip's "All" card |
| `strip_empty_mode` | `show` \| `neighbors` \| `hide` | `show` | `show`: every numeric workspace up to the highest one in use (at least 1-10) gets a strip card, even ones that were never created — `neighbors`: only occupied workspaces plus the displayed one's immediate numeric neighbors (reveals a run of empties one hop at a time as you navigate into it) — `hide`: only occupied workspaces, no empty ones at all. `show`/`neighbors` cards for a workspace that doesn't exist yet are created lazily on click/drop, same as `+` but at that specific number. |
| `show_special` | bool (0/1) | `0` | Include the special (scratchpad) workspace as a strip card |
| `strip_all_card` | bool (0/1) | `0` | Show a leading "All workspaces" card on the strip that toggles the expo view |
| `drag_to_swap` | bool (0/1) | `1` | Grid mode: dropping a preview onto another swaps the two windows' places — works across workspaces too in the all-workspaces (expo) view |
| `switch_on_drop` | bool (0/1) | `0` | Dropping a window on a card also follows it to that workspace |
| — | — | — | Dragging a preview (grid or strip) onto a workspace **card** with the left button moves it there; the **right** button swaps it with that workspace's last-focused window instead |
| `switch_on_new_workspace` | bool (0/1) | `1` | Clicking `+` follows the display to the new workspace |
| `new_workspace_mode` | `fill` \| `linear` | `fill` | `fill`: `+` takes the lowest free workspace id (backfills a gap left by a closed one) — `linear`: always appends past the highest existing id, never backfills |
| `close_button_color` | color/role | `"0xe6e23b3b"` | `✕` close-button fill — per-window, and per-workspace (closes every window on it) |
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

### Color scheme import

Every `color/role` option above takes ONE value that's either a manual hex color or a
scheme-role keyword — no separate `<name>_source` key anymore:

| Value | Meaning |
|---|---|
| `"0xAARRGGBB"` / `"0xRRGGBB"` / `"#AARRGGBB"` / `"#RRGGBB"` / bare hex | Manual color, unchanged from before (now written as a **string**) |
| `"primary"` | Hyprland's live `general:col.active_border` |
| `"secondary"` | Hyprland's live `general:col.inactive_border` |
| `"error"` / `"danger"` | Hyprland's live `group:col.border_locked_active` |
| `"group_active"` / `"group_inactive"` | Hyprland's live `group:col.border_active` / `border_inactive` |

Role keywords read Hyprland's OWN config back through Hyprland's own config system, so it
doesn't matter which tool set it — Noctalia, matugen, pywal, wallust, or a value hand-written
into `hyprland.conf`/`hyprland.lua` all work the same way, since any variable substitution has
already happened by the time gloview reads it back. Only the RGB comes from the scheme; alpha
comes from the option's own default (e.g. `strip_band_color` stays faint even sourced from a
bright accent) — override it per-key with `"role:AA"` (2 hex digits), e.g. `"primary:cc"`.

Only these five roles are recognized — not an arbitrary Hyprland config path — because handing
an unknown path straight to Hyprland's config lookup crashes the whole compositor, not just the
plugin. Anything that isn't one of the five keywords above is parsed as a manual hex color
instead (a typo like `"primry"` just falls back to that option's own default).

```lua
hover_border = "primary",
select_border = "secondary",
close_button_color = "error", -- only meaningful if your scheme actually themes group:col.border_locked_active
```

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
                alt_tab_mode                  = "smart",

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
        alt_tab_mode = smart

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
