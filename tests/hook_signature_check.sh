#!/usr/bin/env bash
# Hooks are found by demangled-signature substring (AGENTS.md rule 4), which means a
# signature that does not match the binary makes findFunctionsByName return nothing and
# initialize() fail. The failure is silent-ish: hyprpm's plugin.load swallows it, so the
# symptom is "my config did not take" -- the same shape as a config error, which is how a
# missing extern "C" cost a day.
#
# nm -DC prints exactly what findFunctionsByName sees, so the check is a lookup, not a
# guess. Run after a Hyprland update, before blaming the config.
set -uo pipefail

SO="${1:-build/gloview.so}"
HYPR="${2:-/usr/bin/Hyprland}"
SRC="${3:-src/session/session.cpp}"

if [ ! -e "$HYPR" ]; then
  echo "SKIP: $HYPR not found"
  exit 77
fi
if [ ! -f "$SO" ] || [ ! -f "$SRC" ]; then
  echo "FAIL: need both the built .so and $SRC"
  exit 1
fi

python3 - "$SO" "$HYPR" "$SRC" <<'PY'
import re, subprocess, sys

so, hypr, src = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src).read()

# Every hooked name, and the signature substring its lookup matches on. Adjacent string
# literals are concatenated in the source, so strip whitespace before comparing.
names = re.findall(r'findFunctionsByName\(m_handle,\s*"([^"]+)"\)', text)

# A signature is written as several adjacent string literals, which C++ concatenates.
# Matching a single literal would check only the first fragment -- the one I actually
# got wrong -- and pass while the hook could not be found.
groups = re.findall(r'((?:"(?:[^"\\]|\\.)*"\s*)+)', text)
sigs = []
for g in groups:
    joined = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', g))
    if '(' not in joined:
        continue
    joined = ''.join(joined.split())
    if any(joined.startswith(n + '(') for n in
           ("shouldRenderWindow", "damageSurface", "setPointerFocus",
            "setKeyboardFocus")):
        sigs.append(joined)

if not sigs:
    print("FAIL: no hook signatures found in", src)
    sys.exit(1)

out = subprocess.run(["nm", "-DC", hypr], capture_output=True, text=True).stdout
# nm -DC lines are "<addr> T <mangled?>" -- the demangled form after a single ' T ',
# not after a run of spaces, which the output never contains.
exported = [l.split(' T ', 1)[-1].strip() for l in out.splitlines() if ' T ' in l]
norm = lambda s: ''.join(s.split())

bad = 0
for sig in sigs:
    name = sig.split('(')[0]
    # Substring, not prefix: the hook looks up mt.demangled.find(sig), and the symbol
    # carries a class qualifier in front (Render::IHyprRenderer::, CSeatManager::).
    overloads = [s for s in exported if (name + '(') in norm(s)]
    if not overloads:
        print(f"MISS {name}: no such symbol exported by the compositor at all")
        bad += 1
    elif not any(sig in norm(o) for o in overloads):
        print(f"MISS {name}: {len(overloads)} overload(s) exist, none matches the "
              f"signature the hook looks for")
        for o in overloads:
            print(f"       have: {o}")
        print(f"       want: {sig}")
        bad += 1
    else:
        print(f"OK   {name}")

if bad:
    print(f"\nFAIL: {bad} hook(s) would not be found -- initialize() returns false and "
          f"the plugin never loads.")
    sys.exit(1)
print(f"\nOK: {len(sigs)} hook signatures resolve against {hypr}")
PY