#!/usr/bin/env python3
"""Comment audit for the gloview sources.

Counts, per file, how many bytes live in comments vs in code, and how many
comment lines there are. Used to hold the "comments stay under 8% of the file"
budget and to prove a refactor touched comments only.

    python3 tools/comment_audit.py            # table
    python3 tools/comment_audit.py --strip F  # print F with comments removed

--strip is also the safety net for comment-only refactors: strip the file before
and after, and the two must be byte-identical, or code moved too.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cscan import comment_text, strip_comments  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

strip = strip_comments


def sources():
    yield from sorted(SRC.rglob("*.cpp"))
    yield from sorted(SRC.rglob("*.hpp"))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--strip":
        sys.stdout.write(strip(Path(sys.argv[2]).read_text()))
        return 0
    if len(sys.argv) == 3 and sys.argv[2] == "--check":
        # usage: --check FILE...  -> "<ratio>% comments (budget N bytes) FILE"
        for f in sys.argv[1:]:
            t = Path(f).read_text()
            com = comment_text(t)
            print(f"{budget_line(t, com)}{f}")
        return 0

    rows = []
    for f in sources():
        t = f.read_text()
        com = comment_text(t)
        rows.append((comment_ratio(t, com), len(com), budget(t, com), f))
    rows.sort(reverse=True)
    total_com = sum(r[1] for r in rows)
    total_all = sum(len(f.read_text()) for _, _, _, f in rows)
    print(f"{'ratio':>7} {'bytes':>8} {'budget':>8}  file")
    for cr, size, bud, f in rows:
        flag = "  <-- over 8%" if cr > 0.08 else ""
        print(f"{cr * 100:6.1f}% {size:8d} {bud:8d}  {f.relative_to(ROOT)}{flag}")
    print(
        f"\nTOTAL {total_com / max(1, total_all) * 100:.1f}% comments "
        f"({total_com} of {total_all} bytes) across {len(rows)} files"
    )
    over = [f for r, _, _, f in rows if r > 0.08]
    if over:
        print(f"OVER 8% BUDGET: {len(over)} file(s)")
        return 1
    print("every file within the 8% budget")
    return 0


def comment_ratio(total, com):
    return len(com) / max(1, len(total))


def budget(total, com):
    """Bytes of comment allowed: comments must be <= 8% of the FINAL file.

    Shrinking comments shrinks the file too, so solve c <= 0.08 * (code + c)
    for c: c <= 0.08/0.92 * code.
    """
    code = len(total) - len(com)
    return int(0.08 / 0.92 * code)


def budget_line(total, com):
    return (
        f"{comment_ratio(total, com) * 100:5.1f}% comments "
        f"(budget {budget(total, com)} bytes) "
    )


if __name__ == "__main__":
    sys.exit(main())
