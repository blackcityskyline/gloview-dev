#!/usr/bin/env python3
"""Single-pass C++ scanner: the one place that decides what a comment is.

Shared by comment_audit.py, list_comments.py and check_comments_only.py so all
three agree.

Why a hand-rolled scanner rather than regexes: an apostrophe inside a COMMENT
("the plugin's own") looks exactly like the start of a char literal, and a naive
regex pass then swallows real code as a string and leaves comments unstripped.
Only a left-to-right state machine knows which context it is in.

Handles // and /* */, "..." and '...' with escapes, and raw strings
R"delim(...)delim" with any delimiter up to 16 chars.
"""

DELIM_MAX = 16


def _scan(text):
    """Walk the file once. Yields (start, end, start_line, is_comment).

    Non-comment spans are yielded with is_comment=False so one walk serves both
    "find the comments" and "rebuild the file without them" — two walks would be
    two chances to disagree about what a comment is.
    """
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue

        # Raw string first: its contents may contain // and /*.
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            op = text.find("(", i + 2, i + 2 + DELIM_MAX + 1)
            if op != -1 and text[op] == "(":
                delim = text[i + 2 : op]
                close = ")" + delim + '"'
                k = text.find(close, op + 1)
                k = n if k == -1 else k + len(close)
                line += text.count("\n", i, k)
                yield (i, k, line, False)
                i = k
                continue

        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = i
            while j < n:  # a trailing backslash continues the comment
                nl = text.find("\n", j)
                j = n if nl < 0 else nl
                if text[j - 2 : j] != "\\":
                    break
                j += 1
            yield (i, j, line, True)
            line += text.count("\n", i, j)
            i = j
            continue

        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            yield (i, j, line, True)
            line += text.count("\n", i, j)
            i = j
            continue

        if c in "\"'":
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                if text[j] == "\n":  # unterminated: it was never a literal
                    line += 1
                    j += 1
                    break
                j += 1
            yield (i, j, line, False)
            i = j
            continue

        i += 1


def comment_blocks(text):
    """(start_line, end_line, text) per comment block, lines 1-based."""
    for start, end, line, is_com in _scan(text):
        if is_com:
            yield line, line + text[start:end].count("\n"), text[start:end]


def comment_lines(text):
    """1-based line numbers of every line carrying comment text.

    A trailing comment counts its line, which is the honest unit for a line
    budget: a one-line invariant costs one line whether it is 40 or 400 chars.
    """
    out = []
    for start, end, _ in comment_blocks(text):
        out.extend(range(start, end + 1))
    return out


def comment_text(text):
    """Every comment's text, newline-separated."""
    return "\n".join(t for _, _, t in comment_blocks(text))


def strip_comments(text):
    """The file with comments removed and string literals untouched.

    A removed comment leaves its newlines behind so the code that follows keeps
    its line numbers.
    """
    out, prev = [], 0
    for start, end, _, is_com in _scan(text):
        if not is_com:
            continue
        out.append(text[prev:start])
        nl = text[start:end].count("\n")
        if nl:
            out.append("\n" * nl)
        prev = end
    out.append(text[prev:])
    return "".join(out)
