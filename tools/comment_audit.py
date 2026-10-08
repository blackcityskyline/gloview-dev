#!/usr/bin/env python3
"""Comment audit for the gloview sources.

Budget: comment LINES must stay under 8% of all LINES in src/. Line count, not
bytes — a two-line invariant and a twenty-line essay cost the same budget, which
is the point: the budget is about how much of the file you have to read, not
about how many characters it took to say it.

    python3 tools/comment_audit.py            # per-file table + total
    python3 tools/comment_audit.py --check F  # one file: lines, budget, over by
    python3 tools/comment_audit.py --strip F  # print F with comments removed
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cscan import comment_lines  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
BUDGET = 0.08

strip = None  # cscan owns the scanner; nothing here needs the stripped form


def all_lines():
    yield from sorted(SRC.rglob("*.cpp"))
    yield from sorted(SRC.rglob("*.hpp"))


def count_lines(text):
    """(comment_lines, code_lines) — blank lines count as neither."""
    com = len(comment_lines(text))
    total = text.count("\n") + (0 if text.endswith("\n") else 1)
    blank = sum(1 for ln in text.split("\n") if not ln.strip())
    return com, total - blank - com


def report_line(text):
    com, code = count_lines(text)
    total = com + code
    return (
        f"{com:5d} comment / {total:5d} total lines "
        f"({com / max(1, total) * 100:5.1f}%, budget {int(BUDGET * total)}); "
        f"{max(0, com - int(BUDGET * total))} over\n"
    )


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        for f in args:
            sys.stdout.write(f"{f}: {report_line(Path(f).read_text())}")
        return 0
    if len(sys.argv) == 3 and sys.argv[1] == "--strip":
        from cscan import strip_comments

        sys.stdout.write(strip_comments(Path(args[0]).read_text()))
        return 0

    rows, total_lines = [], 0
    for f in all_lines():
        com, code = count_lines(f.read_text())
        total = com + code
        total_lines += total
        rows.append((com / max(1, total), com, int(BUDGET * total), f))
    rows.sort(reverse=True)
    tc = sum(r[1] for r in rows)

    print(f"{'ratio':>7} {'lines':>7} {'budget':>7}  file")
    for ratio, com, bud, f in rows:
        flag = "  <-- over 8%" if ratio > BUDGET else ""
        print(f"{ratio * 100:6.1f}% {com:7d} {bud:7d}  {f.relative_to(ROOT)}{flag}")

    pct = tc / max(1, total_lines) * 100
    over = [f for r, _, _, f in rows if r > BUDGET]
    print(
        f"\nTOTAL {tc} comment lines / {total_lines} lines = {pct:.1f}% "
        f"(budget {int(BUDGET * total_lines)}); "
        f"{len(over)}/{len(rows)} files individually over"
    )
    if over:
        print("files over: " + ", ".join(str(f.relative_to(ROOT)) for f in over))
    if pct > BUDGET:
        print(f"OVER 8% by {tc - int(BUDGET * total_lines)} lines")
        return 1
    print(
        f"within the 8% budget, {int(BUDGET * total_lines) - tc} lines of slack"
    )
    return 0


def over_files():
    """Files individually above 8% — the tail to squeeze if the total is tight."""
    for f in all_lines():
        com, code = count_lines(f.read_text())
        total = com + code
        if total and com / total > BUDGET:
            print(f"{com / total * 100:5.1f}%  {f}")


if __name__ == "__main__":
    sys.exit(main())
