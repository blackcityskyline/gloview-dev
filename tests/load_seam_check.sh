#!/usr/bin/env bash
# The load seam: gloview.so is dlopen'd INTO the Hyprland binary, so every symbol it
# imports must already exist there. A single missing one kills the plugin before
# PLUGIN_INIT runs -- which looks like a silent config error, not a link error, and
# cost a full day once (mangled Lua C API: _Z14luaL_checktypeP9lua_Stateii).
#
# ld cannot check this: the .so has no DT_NEEDED on Hyprland, so it reports every
# Hyprland import as undefined. So compare the two export tables ourselves.
#
# 77 = skip (no Hyprland installed). Nothing else should ever be special-cased.
set -uo pipefail

SO="${1:-build/gloview.so}"
HYPR="${2:-/usr/bin/Hyprland}"

if [ ! -f "$SO" ]; then
  echo "FAIL: $SO not built"
  exit 1
fi
if [ ! -e "$HYPR" ]; then
  echo "SKIP: $HYPR not found -- cannot verify the load seam"
  exit 77
fi

# `ldd -r` is the source of the candidate list, not nm: it already resolved
# everything the DT_NEEDED libraries cover (libc, libEGL, libhyprutils...) and
# reports only what is left, i.e. what Hyprland's own binary must provide. Reading
# nm's full undefined list instead drags in @GLIBC_2.2.5-versioned libc symbols
# and fails on imports that have always resolved fine.
needs=$(mktemp)
exports=$(mktemp)
trap 'rm -f "$needs" "$exports"' EXIT

ldd -r "$SO" 2>&1 | sed -n 's/.*undefined symbol: //p' | awk '{ print $1 }' | sort -u >"$needs"
nm -D --defined-only "$HYPR" | awk '$2 ~ /^[TWVRDBGi]$/ { print $3 }' | sort -u >"$exports"

missing=$(comm -23 "$needs" "$exports")

if [ -n "$missing" ]; then
  echo "FAIL: $(echo "$missing" | wc -l) symbol(s) gloview.so imports but Hyprland does not export."
  echo "      dlopen will fail on these, before PLUGIN_INIT, and it will read as a"
  echo "      config error rather than a link error:"
  echo "$missing" | sed 's/^/        /'
  echo "      Check: Lua headers wrapped in extern \"C\"? Hyprland ABI moved since the pin?"
  exit 1
fi

echo "ok: $(wc -l <"$needs") symbol(s) imported from Hyprland itself, all exported"