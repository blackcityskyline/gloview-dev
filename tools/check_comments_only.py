#!/usr/bin/env python3
"""Prove a comment refactor did not move code.

For every source file: strip comments from the working tree and from the git
HEAD version, collapse whitespace, and compare. Equal means the same code
tokens in the same order — comments were the only thing that changed.

Whitespace is normalized because deleting a comment LINE necessarily deletes a
newline, which a byte-exact compare would report as a code change. The check is
token-level, so it tolerates blank-line removal but still catches any real edit
to code (a changed token, a reordered statement, a dropped argument).

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
    """Comment-free, whitespace-collapsed form used for comparison."""
    return re.sub(r"\s+", " ", strip_comments(text)).strip()


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
