#!/usr/bin/env python3
"""Single-pass C++ scanner that removes comments.

Shared by comment_audit.py and check_comments_only.py so both agree on what a
comment is.

Why a hand-rolled scanner rather than regexes: an apostrophe inside a COMMENT
("the plugin's own") looks exactly like the start of a char literal, and a naive
regex pass then swallows real code as a string and leaves comments unstripped.
Only a left-to-right state machine knows which context it is in.

Handles: // and /* */ (nestable-free, as C++ is), "..." and '...' with escapes,
and raw strings R"delim(...)delim" with any delimiter up to 16 chars.
"""

DELIM_MAX = 16


def strip_comments(text, keep_newlines=True):
    """Return `text` with comments removed and string literals untouched."""
    out = []
    i, n = 0, len(text)

    def emit(s):
        out.append(s)

    while i < n:
        c = text[i]

        # --- comments -----------------------------------------------------
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            if keep_newlines:
                emit(text[j : j + 1])  # the newline itself
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            if keep_newlines:
                emit("\n" * text.count("\n", i, j))
            i = j
            continue

        # --- raw string: R"delim( ... )delim" ----------------------------
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            open_paren = text.find("(", i + 2, i + 2 + DELIM_MAX + 1)
            if open_paren != -1 and text[open_paren] == "(":
                delim = text[i + 2 : open_paren]
                close = ')' + delim + '"'
                k = text.find(close, open_paren + 1)
                k = n if k == -1 else k + len(close)
                emit(text[i:k])
                i = k
                continue

        # --- ordinary literals --------------------------------------------
        if c == '"' or c == "'":
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                # An unterminated literal means we were never in one (an
                # apostrophe in code context); don't swallow the file.
                if text[j] == "\n":
                    break
                j += 1
            emit(text[i:j])
            i = j
            continue

        emit(c)
        i += 1

    return "".join(out)


def comment_text(text):
    """Return only the comment text, newline-separated (for size accounting)."""
    parts = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            parts.append(text[i:j])
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            parts.append(text[i:j])
            i = j
            continue
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            open_paren = text.find("(", i + 2, i + 2 + DELIM_MAX + 1)
            if open_paren != -1 and text[open_paren] == "(":
                delim = text[i + 2 : open_paren]
                close = ")" + delim + '"'
                k = text.find(close, open_paren + 1)
                i = n if k == -1 else k + len(close)
                continue
        if c == '"' or c == "'":
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                if text[j] == "\n":
                    break
                j += 1
            i = j
            continue
        i += 1
    return "\n".join(parts)
