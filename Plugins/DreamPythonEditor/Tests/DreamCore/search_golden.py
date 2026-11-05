# SPDX-License-Identifier: Apache-2.0
# Golden data for T-CORE-017 (TDD §4.4): Python `re` results from the engine's interpreter for dream::SearchEngine.
# build.bat runs this on the engine's python.exe -I -B with an output path. Each row is
#   pattern<TAB>flags<TAB>subject<TAB>template<TAB>spans<TAB>replaced
# with the three texts as hex UTF-16LE, flags "i" (IGNORECASE) or "-", spans "start-end,..." in UTF-16 units of the
# re.finditer matches (always MULTILINE, as §4.4 compiles), "E" when the pattern does not compile, and the re.sub result
# as hex UTF-16LE ("X" when the template is refused, "-" when there is no template). Test_Search_AgainstPythonGolden
# reads the file named by DREAM_SEARCH_GOLDEN.
import re
import sys

PATTERNS = [
    r"abc", r"a.c", r"a*", r"a+?", r"(ab)+", r"x*", r"^", r"$", r"^\s*$", r"\bfoo\b", r"\Bo", r"\w+", r"\W+",
    r"\d+", r"\D", r"\s+", r"\S+", r"[a-c]+", r"[^a-c\n]+", r"[\w.]+@[\w.]+", r"colou?r", r"(\w+)\s(\w+)",
    r"(?P<word>\w+)-(?P=word)", r"(a)(b)?c", r"(a|ab)(c|bcd)(d*)", r"a{2}", r"a{2,}", r"a{,2}", r"a{1,3}?",
    r"a{,}", r"x{", r"{1}", r"\d{2,4}", r"(?=a)\w", r"(?!a)\w+", r"(?<=@)\w+", r"(?<!x)y", r"(?<=ab|cd)e",
    r"\Aabc", r"abc\Z", r"(?i)ABC", r"(?s)a.b", r"(?x) a  b # comment", r"(?i:A)b", r"a(?-i:B)", r"(?>a+)b",
    r"a++b", r"a*+a", r"\x41", r"é", r"\U0001F600", r"\0", r"\101", r"\t", r"[\]]", r"[]a]", r"[a-]",
    r"[-a]", r"[\d-]", r"\.", r"\$\(", r"é+", r"straße", r"STRASSE", r"k", r"ſ", r"µ", r"ΣΑΣ", r"K",
    r"(\w)\1", r"(?i)(\w)\1", r"((a)|b)+", r"(a*)*", r"(a*)+", r"(a|)*b", r"\b", r"(?<=\d)(?=(\d\d\d)+$)",
    r"[^\W\d]+", r"[\s\S]", r"(?m)^\w", r"^.*$", r"\n", r"a\nb", r"[^a]+", r"(?:x|y)+z",
    # errors
    r"(", r")", r"a)", r"[", r"[a", r"*", r"a**", r"a{3,2}", r"\q", r"(?P<1>a)", r"(?P<a>x)(?P<a>y)",
    r"(?P=nope)", r"\2(a)", r"(a\1)", r"(?<=a+)b", r"(?<a>x)", r"x(?i)", r"\N{DASH}", r"(?(1)a|b)", r"[z-a]",
    r"\x4", r"(?L)a", r"(?au)a", r"+a", r"?", r"(?",
]

SUBJECTS = [
    "", "abc", "aabcbc abc\nabcabc", "xx foo foobar foo_ foo\nfoo.bar", "colour color colouur",
    "john smith\nmary jones", "ab-ab cd-ce ee-ee", "aaa ab abc abbc", "1234567 12 123\n99999",
    "a@b.c x@y.z @ @@", "xy yy zy\nxyz", "abe cde xe abcde", "ABC abc AbC\n\taBc", "a\nb\na b",
    "é ée Ééeé", "straße STRASSE Straße", "k K K kelvin", "s S ſ", "µ μ Μ",
    "σας ΣΑΣ", "\U0001F600x\U0001F600", "aa bb ab cdd", "\t \n\n  \n",
    "x{ {1} x{1}", "$( a.b", "xxyz xyz yyz", "abcd abcbcd", "aaaa", "ab\x00cd", "]a-]",
]

TEMPLATES = [None, "<\\g<0>>", "[\\1]", "\\2 \\1", "\\g<word>!", "-", "", "\\n", "x\\\\y", "\\q"]

# Corner rows from review r1 (core-formats): astral look-behind, IGNORECASE classes and backreferences, the Kelvin sign,
# dotless i, final sigma, (?a) folding, atomic groups, \B on an empty text, templates.
EXTRA_PATTERNS = [
    "(?<=\U0001F600)x", r"(?<!\w)\w", r"\b\w+\b", r"(a|b)*?c", r"(?:a|ab)*c", r"(a?)+?b", r"(\w+)\s+\1",
    r"[^\W_]+", r"(?i)[a-z]+", r"(?i)[^k]", r"(?i)ss", r"x*", r".", r"(?a)\w+", r"\d", r"(?s).+",
    "(?x)a b # c\n d", r"$", r"\Z", r"|", r"a{,3}?", r"(a)|b", r"(?P<x>a)(?P=x)", r"[\x00-\x7f]+",
    r"(?<=a)(?=b)", r"(?=(a+))a*b\1", r"^(?:(a)|b)+$", r"(a*)*b", r"(a*)+$", r"(?:a*?)*", r"\s",
    r"(?i)σ", "(?i)İ", r"(?<=ab)c|(?<=x)y", r"a(?=b)|ab", r"(?>a|ab)c", r"(?#comment)a", r"(?i:a)b",
    r"[[]", r"[a-\d]", r"\W", r"(?i)\w+", r"(?:(?i)a)", r"(?i)(?P<n>K)(?P=n)", r"(a)(?<=(a))", r"(?<=(a))b",
    r"\B", r"(?=a)*", r"(?:a|)*?", r"(a){0}b", r"(?i)K", r"(?i)[K]", r"[\U0001F600-\U0001F64F]+",
    r"é{2}", r"(?i)ǅ", r"(.)(?!\1)", r"^$", r"(?<=\n)a", r"a$\n?", r"\w(?=\s|$)",
    # (?a) with IGNORECASE folds ASCII only; IGNORECASE backreferences compare lower() only.
    r"(?ai)k", r"(?ai)[k]", r"(?ai)i", r"(?ai)[^k]+", r"(?i)(σ)\1", r"(?i)(.)\1", r"(?ai)(.)\1",
]

EXTRA_SUBJECTS = [
    "", "\U0001F600x x\U0001F600", "\U0001D400\U0001D401c d", "aabac abc ac", "the the cat cat sat",
    "ǅǄǆ K K ſ k", "K k K", "ß ss SS", "\U0001F600\U0001F600", "é abc", "١٢٣ 123", "a\nb\n",
    "ab abab b", "aaab aab", "\x1c\x1d\x1e\x1f\x85\xa0 x", "Σσς", "İiIı", "abc xy", "abcd", "[a", "a-1",
    "aAAa", "kKKKk", "\U0001F601\U0001F64F!", "éé éÉ", "\n\na\n", "abba aab", "a \n b", "aa a",
    "σς Σσ ΣΣ ςς", "Kk KK kK",
]

EXTRA_TEMPLATES = [None, "\\g<0>\\g<0>", "\\0", "[\\1]", "\\g<1>0", "x"]


def u16(s, i):
    return len(s[:i].encode("utf-16-le", "surrogatepass")) // 2


def hexs(s):
    return s.encode("utf-16-le", "surrogatepass").hex()


def main():
    out = open(sys.argv[1], "w", encoding="ascii", newline="\n")
    rows = 0
    for patterns, subjects, templates in ((PATTERNS, SUBJECTS, TEMPLATES), (EXTRA_PATTERNS, EXTRA_SUBJECTS, EXTRA_TEMPLATES)):
        for pat in patterns:
            for flags in ("-", "i"):
                f = re.M | (re.I if flags == "i" else 0)
                try:
                    rx = re.compile(pat, f)
                except (re.error, OverflowError, ValueError):
                    out.write("%s\t%s\t%s\t%s\tE\t-\n" % (hexs(pat), flags, hexs(""), hexs("")))
                    rows += 1
                    continue
                for subj in subjects:
                    spans = ",".join("%d-%d" % (u16(subj, m.start()), u16(subj, m.end())) for m in rx.finditer(subj))
                    for tpl in templates:
                        if tpl is None:
                            rep = "-"
                        else:
                            try:
                                rep = hexs(rx.sub(tpl, subj))
                            except (re.error, IndexError):
                                rep = "X"
                        out.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (hexs(pat), flags, hexs(subj), "-" if tpl is None else hexs(tpl),
                                                              spans if spans else ".", rep))
                        rows += 1
    out.close()
    print("rows", rows)


main()
