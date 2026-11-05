# SPDX-License-Identifier: Apache-2.0
"""``# noqa`` suppression (TDD §7.3.3 "Suppression", T-DIAG-011).

flake8 semantics: a bare ``# noqa`` silences every code on its line, ``# noqa: SUM102, SUM104`` only those codes. The
comment must be a real comment token (a ``# noqa`` inside a string literal does nothing) and applies to the line a
diagnostic starts on. Policy ``lint.enforced`` (M4) cannot override it, because it is visible in the code.
"""

import io
import re
import tokenize

ALL = "*"

_NOQA_RE = re.compile(r"#\s*noqa(?:\s*:\s*(?P<codes>[A-Z]+[0-9]+(?:[\s,]+[A-Z]+[0-9]+)*))?", re.IGNORECASE)
_CODE_RE = re.compile(r"[A-Z]+[0-9]+", re.IGNORECASE)


def _comments(text):
    """(0-based line, column, comment text) of every comment token; a regex fallback when tokenize fails."""
    found = []
    try:
        for token in tokenize.generate_tokens(io.StringIO(text).readline):
            if token.type == tokenize.COMMENT:
                found.append((token.start[0] - 1, token.start[1], token.string))
        return found, True
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return [(number, line.find("#"), line[line.find("#"):]) for number, line in enumerate(text.split("\n"))
                if "#" in line], False


def noqa_map(text):
    """{0-based line: ALL or a set of upper-case codes} for every line with a ``# noqa`` comment."""
    result = {}
    if "noqa" not in text.lower():
        return result
    comments, _ = _comments(text)
    for line, _, comment in comments:
        match = _NOQA_RE.search(comment)
        if not match:
            continue
        codes = match.group("codes")
        if not codes:
            result[line] = ALL
            continue
        current = result.get(line)
        if current == ALL:
            continue
        current = set() if current is None else current
        current.update(code.upper() for code in _CODE_RE.findall(codes))
        result[line] = current
    return result


def is_suppressed(noqa, line, code):
    entry = noqa.get(line)
    return entry is not None and (entry == ALL or code.upper() in entry)


def unsafe_comment_lines(text):
    """Lines where appending a comment would change the code: the line ends inside a multi-line string, or with a
    backslash continuation."""
    unsafe = set()
    for number, line in enumerate(text.split("\n")):
        if line.rstrip().endswith("\\"):
            unsafe.add(number)
    try:
        for token in tokenize.generate_tokens(io.StringIO(text).readline):
            if token.type == tokenize.STRING and token.end[0] > token.start[0]:
                unsafe.update(range(token.start[0] - 1, token.end[0] - 1))
    except (tokenize.TokenError, IndentationError, SyntaxError):
        pass
    return unsafe


def noqa_edit(line_text, line_number, code):
    """The edit that suppresses ``code`` on a line: (start, end, new text) in (line, byte column) positions."""
    end_col = len(line_text.encode("utf-8", "surrogatepass"))
    comments, _ = _comments(line_text)
    for _, column, comment in comments:
        match = _NOQA_RE.search(comment)
        if match and match.group("codes"):
            # Append to the existing list: "# noqa: SUM101" -> "# noqa: SUM101, SUM102".
            codes_end = column + match.end("codes")
            prefix = line_text[:codes_end]
            byte_col = len(prefix.encode("utf-8", "surrogatepass"))
            return (line_number, byte_col), (line_number, byte_col), ", " + code
    trimmed = line_text.rstrip()
    trimmed_col = len(trimmed.encode("utf-8", "surrogatepass"))
    return (line_number, trimmed_col), (line_number, end_col), "  # noqa: " + code
