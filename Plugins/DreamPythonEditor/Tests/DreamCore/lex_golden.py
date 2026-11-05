# SPDX-License-Identifier: Apache-2.0
# Golden data for T-LEX-001 (TDD §4.3): token boundaries of the engine interpreter's 3.11 tokenize on its own
# Lib/**/*.py. build.bat runs this on the engine's python.exe -I -B with the Lib root and an output path. For every file
# that compiles and tokenizes it writes "F<TAB>path", then for each non-empty line of at most 20,000 UTF-16 units
#   line<TAB>boundaries<TAB>merged
# (0-based line; UTF-16 columns). boundaries: where PyLexer must start a token: column 0, every token start and every
# token end before the line end (whitespace starts there). merged: "s-e;..." spans whose inner boundaries PyLexer may
# add and tokenize does not have: STRING tokens (f-string and escape sub-tokens merged), COMMENT tokens, and a
# backslash join with the blanks before it. Names that tokenize.py splits at a combining mark or variation selector are
# joined first (the compiler takes them whole; §4.3.1 identifiers follow str.isidentifier). Lex_AgainstTokenizeGolden
# reads the file named by DREAM_LEX_GOLDEN. The first line ("# lex_golden ...") is a stamp: when the output already starts
# with it the file is reused, so only the first full tier-1 run in an output folder pays the minute this takes.
import hashlib
import io
import os
import sys
import tokenize

SKIP = {tokenize.NEWLINE, tokenize.NL, tokenize.INDENT, tokenize.DEDENT, tokenize.ENDMARKER, tokenize.ENCODING}
LONG = 20000


_U16_CACHE = {}


def u16(s, i):
    # Code point index -> UTF-16 index; ASCII lines (almost all) map to themselves.
    if s.isascii():
        return i
    table = _U16_CACHE.get(s)
    if table is None:
        table = [0]
        for ch in s:
            table.append(table[-1] + (2 if ord(ch) > 0xFFFF else 1))
        if len(_U16_CACHE) > 4096:
            _U16_CACHE.clear()
        _U16_CACHE[s] = table
    return table[i]


def merge_identifiers(toks):
    # The pure-Python tokenize module matches names with w+, so it splits an identifier at a combining mark or a
    # variation selector (XID_Continue, not w); the compiler (and PyLexer, §4.3.1) takes the whole identifier.
    out = []
    for t in toks:
        if (out and out[-1].type == tokenize.NAME and t.type in (tokenize.NAME, tokenize.ERRORTOKEN, tokenize.NUMBER)
                and out[-1].end == t.start and (out[-1].string + t.string).isidentifier()):
            p = out[-1]
            out[-1] = tokenize.TokenInfo(tokenize.NAME, p.string + t.string, p.start, t.end, p.line)
        else:
            out.append(t)
    return out


def stamp(root):
    # The first line of the output names this script's hash, the interpreter and the corpus root; a file that already
    # starts with the same line is reused (the corpus is the read-only engine install, so it cannot change under it).
    with open(os.path.abspath(__file__), "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()[:16]
    return "# lex_golden %s python %s root %s" % (digest, sys.version.split()[0], os.path.abspath(root))


def main():
    root = sys.argv[1]
    first = stamp(root)
    try:
        with open(sys.argv[2], "r", encoding="utf-8") as f:
            if f.readline().rstrip("\n") == first:
                print("cached")
                return
    except OSError:
        pass
    tmp = sys.argv[2] + ".tmp"
    out = open(tmp, "w", encoding="utf-8", newline="\n")
    out.write(first + "\n")
    files = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            if not name.endswith(".py"):
                continue
            path = os.path.join(dirpath, name)
            try:
                with open(path, "rb") as f:
                    data = f.read()
                compile(data, path, "exec")
                toks = list(tokenize.tokenize(io.BytesIO(data).readline))
                encoding, _ = tokenize.detect_encoding(io.BytesIO(data).readline)
                text = data.decode(encoding)
            except Exception:
                continue
            if "\r" in text.replace("\r\n", "\n"):
                continue   # a lone CR splits lines for the model but not for tokenize
            if text.startswith("﻿"):
                text = text[1:]
            lines = text.replace("\r\n", "\n").split("\n")
            toks = merge_identifiers(toks)
            bounds = [set() for _ in lines]
            merged = [[] for _ in lines]
            covered = [[] for _ in lines]   # spans of real tokens, to find backslash joins
            for t in toks:
                if t.type in SKIP:
                    continue
                (sl, sc), (el, ec) = t.start, t.end
                sl -= 1
                el -= 1
                if sl < 0 or el >= len(lines):
                    continue
                line = lines[sl]
                bounds[sl].add(u16(line, sc))
                if sl == el:
                    e16 = u16(line, ec)
                    if e16 < u16(line, len(line)):
                        bounds[sl].add(e16)
                    if t.type in (tokenize.STRING, tokenize.COMMENT):
                        merged[sl].append((u16(line, sc), e16))
                    covered[sl].append((sc, ec))
                else:
                    merged[sl].append((u16(line, sc), u16(line, len(line))))
                    covered[sl].append((sc, len(line)))
                    for k in range(sl + 1, el):
                        merged[k].append((0, u16(lines[k], len(lines[k]))))
                        covered[k].append((0, len(lines[k])))
                    last = lines[el]
                    e16 = u16(last, ec)
                    merged[el].append((0, e16))
                    covered[el].append((0, ec))
                    if e16 < u16(last, len(last)):
                        bounds[el].add(e16)
            files += 1
            out.write("F\t%s\n" % path)
            for i, line in enumerate(lines):
                if not line:
                    continue
                n16 = u16(line, len(line))
                if n16 > LONG:
                    continue
                if line.endswith("\\"):
                    end = max([e for s, e in covered[i]], default=0)
                    if end <= len(line) - 1 and not any(s <= len(line) - 1 < e for s, e in covered[i]):
                        merged[i].append((u16(line, end), n16))
                b = sorted(x for x in bounds[i] | {0} if x < n16)
                m = ";".join("%d-%d" % se for se in sorted(merged[i]))
                out.write("%d\t%s\t%s\n" % (i, ",".join(map(str, b)), m if m else "."))
    out.close()
    os.replace(tmp, sys.argv[2])
    print("files", files)


main()
