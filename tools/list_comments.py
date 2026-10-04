#!/usr/bin/env python3
"""List every comment block in a file, with line numbers and byte size.

The working view for a comment refactor: you edit comments, never code, so you
need the comments and their positions, not the whole file.

    python3 tools/list_comments.py src/foo.cpp
    python3 tools/list_comments.py src/foo.cpp --sizes   # per-block budget
"""

import sys
from pathlib import Path


def blocks(text):
    """Yield (start_line, end_line, text) for each comment block."""
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if text.startswith("//", i):
            start_line = line
            j = i
            while j < n:
                nl = text.find("\n", j)
                j = n if nl < 0 else nl
                if text[j - 2 : j] != "\\":  # not a line continuation
                    break
                j += 1
            yield start_line, line, text[i:j]
            line += text.count("\n", i, j)
            i = j
            continue
        if text.startswith("/*", i):
            start_line = line
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            yield start_line, line, text[i:j]
            line += text.count("\n", i, j)
            i = j
            continue
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            op = text.find("(", i + 2, i + 19)
            if op != -1 and text[op] == "(":
                delim = text[i + 2 : op]
                close = ")" + delim + '"'
                k = text.find(close, op + 1)
                i = n if k == -1 else k + len(close)
                continue
        if c in "\"'":
            q, j = c, i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == q:
                    j += 1
                    break
                if text[j] == "\n":
                    line += 1
                    break
                j += 1
            i = j
            continue
        i += 1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    sizes = "--sizes" in sys.argv
    for path in args:
        text = Path(path).read_text()
        total_com = sum(len(t) for _, _, t in blocks(text))
        code = len(text) - total_com
        budget = int(0.08 / 0.92 * code)
        print(f"== {path}: {total_com} comment bytes, budget {budget}")
        for start, end, t in blocks(text):
            head = t.splitlines()[0][:60]
            mark = "!!" if len(t) > budget * 0.25 else "  "
            print(f"{mark} {start:5d}-{end:<5d} {len(t):5d}  {head}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
