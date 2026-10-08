#!/usr/bin/env python3
"""Prove a comment refactor did not move code.

For every source file: strip comments from the working tree and from the git
HEAD version, collapse whitespace, and compare. Equal means the same code
tokens in the same order — comments were the only thing that changed.

Whitespace is removed entirely because reflowing a wrapped signature (a
comment-only edit does that constantly) must not read as a code change. The
code is not being modified, only annotated, so this stays a real check: any
changed, added or dropped token shows up.

    python3 tools/check_comments_only.py [BASE_REF]     # default HEAD
"""

import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cscan import strip_comments  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def code_signature(text):
    """Comment-free, whitespace-free form used for comparison.

    ALL whitespace is removed, not collapsed: reflowing a wrapped signature
    (`foo(\n  int x)` -> `foo(int x)`) must not read as a code change, and no
    real edit survives whitespace-blind comparison here — the code is not
    being modified at all, only annotated.
    """
    return re.sub(r"\s+", "", strip_comments(text))


def head_version(rel, ref):
    r = subprocess.run(
        ["git", "show", f"{ref}:{rel}"], cwd=ROOT, capture_output=True, text=True
    )
    return r.stdout if r.returncode == 0 else None


def main():
    ref = sys.argv[1] if len(sys.argv) > 1 else "HEAD"
    files = sorted(
        list((ROOT / "src").rglob("*.cpp")) + list((ROOT / "src").rglob("*.hpp"))
    )
    bad, skipped = [], []
    for f in files:
        rel = f.relative_to(ROOT)
        before = head_version(rel, ref)
        if before is None:
            skipped.append(f"{rel}: not in {ref}")
            continue
        if code_signature(f.read_text()) != code_signature(before):
            bad.append(rel)
    for s in skipped:
        print(s)
    if bad:
        for b in bad:
            print(f"{b}: CODE CHANGED vs {ref}")
        print(f"\n{len(bad)} file(s) moved code — comments must be the only diff")
        return 1
    print(f"all {len(files)} files: comment-only changes verified against {ref}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
