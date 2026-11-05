# SPDX-License-Identifier: Apache-2.0
"""Static source checks of TDD section 2.5, run with tier 1 (section 14.1) on the engine's python.exe -I -B.

Scans (each prints its violations as "path:line: message" and a one-line summary):
  spdx         every Dream Python Editor source file starts with the SPDX Apache-2.0 header, and none carries Epic's copyright
               line (rule 2, T-ARCH-003)
  python-h     Python.h is included only by Source/DreamEditor/Private/Python/DreamPythonInclude.h, and any header
               that reaches it is included only from Private/Python, Private/Compat or Private/Run/RunSupervisor*
               (rule 6, T-ARCH-007), or from the test-only module's Private/Python fixture folder
  dreamcore     DreamCore outside Private/UE includes only the C++ standard library and dream/... headers
               (rule 1, T-ARCH-002)
  content      no test_*.py under Content/, no init_unreal.py in the packaged plugin (anything outside Tests/),
               nothing under Content/Python/Lib (rules 3, 4 and 12, T-ARCH-004)
  entry        ExecPythonCommandEx only in Private/Python/DreamPythonEntry.cpp, exactly the two literal templates of
               section 3.6 (_run and _boot); no PyRun_*, PyEval_EvalCode*, Py_CompileString*, PyImport_ExecCodeModule*
               outside the reviewed call sites in ENTRY_EXEMPT (rule 5, T-RUN-020)
  compat       ->cframe, ->gilstate_counter, _Py* names and Python version-macro comparisons only in Private/Compat;
               sys.version_info only in Content/Python/dream_editor/_impl/compat.py (rule 7)
  banned       none of the rule 8 APIs in the shipped modules: FTSTicker::GetCoreTicker().Tick(, EnterDebuggingMode,
               PyErr_SetInterrupt, FInteractiveProcess, FMonitoredProcess, WritePipe with an FString, the Unattended
               command flag; in Python: debugpy wait_for_client, debugpy.configure(, pip (rule 8, T-CAN-013)
  ticker       raw AddTicker, RegisterActiveTimer and OnEndFrame.Add only in FDreamDeferral, FDreamRunService, or right
               after DREAM_TIMER_EXEMPT(paint-only...) (rule 9)
  buildcs      parsed Build.cs dependencies: DreamCore depends on Core only; DreamEditor never on DreamMCP; DreamMCP on no
               Experimental module (rule 10)
  ufunction    no UFUNCTION( in Source/DreamEditor (rule 11)
  manifest     every file under Binaries/ThirdParty and Content/Python/_dream_vendor is listed in its manifest with a
               matching SHA-256 and an allowed licence; none of the RT-11 pruned debugpy patterns is present (rule 12,
               T-ARCH-008)
  loctext      user-visible text is LOCTEXT / NSLOCTEXT in namespace "Dream": no FText::FromString, AsCultureInvariant
               or INVTEXT on a literal that holds a letter, every LOCTEXT file defines and undefines LOCTEXT_NAMESPACE
               "Dream", and one key never carries two source texts (T-GAP-001, A11Y-06, A49)
  trace        the section 14.7 traceability check over docs/TDD.md and docs/SPEC.md (T-GAP-008, Tests/ci/dream_trace.py)
  testnames    every automation test name in Source/DreamEditorTests is ASCII: each string literal in the name argument of
               Describe, It, LatentIt and their x forms (a plain literal, TEXT(...), FString::Printf(TEXT(...)), a ?:
               between literals), and the name literal of BEGIN_DEFINE_SPEC and IMPLEMENT_*_AUTOMATION_TEST. The framework
               takes the narrow literal through the ANSI code page, so a '\u00a7' became U+FFC2 U+FFA7 glyph warnings on
               whichever test ran next. Not followed: a name held in a variable built elsewhere (LatentIt(Name, ...) in
               FastChain, KeysG5, RunNoChange, RunWindowFocus, SyntaxError); its literals are not checked

The shipped-module rules (5, 7, 8, 9, 11) scan Source/DreamEditor, Source/DreamCore and Source/DreamMCP. The test-only
module Source/DreamEditorTests is repository only (TDD 2.2; release descriptors leave it out) and holds the 14.2 fixtures
that the rules exist to keep out of the product (TickCoreTicker ticks the core ticker, SleepNative and the _dream_test
module call CPython directly), so only rules 2 and 6 apply to it: its Python fixture files live in
Source/DreamEditorTests/Private/Python and include CPython only through DreamPythonInclude.h (tests B-note).

Usage: python.exe -I -B dream_ci_scans.py [scan ...] [--plugin <dir>] [--host-scripts <dir>] [--docs <dir>]
       [--list-milestone <M1..>]
Exit code 0 when every selected scan is clean, 1 when one has violations, 2 for a usage error.
"""

import argparse
import hashlib
import json
import os
import re
import sys
import tokenize

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script folder off sys.path
import dream_trace  # noqa: E402

PLUGIN_DEFAULT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HOST_SCRIPTS_DEFAULT = os.path.abspath(os.path.join(PLUGIN_DEFAULT, "..", "..", "DreamHost", "Scripts"))
DOCS_DEFAULT = os.path.abspath(os.path.join(PLUGIN_DEFAULT, "..", "..", "docs"))

# Build products and caches: never sources.
SKIP_DIRS = {"Intermediate", "Binaries", "Saved", "DerivedDataCache", "__pycache__", ".git", ".vs", ".idea", "out"}

C_FAMILY = {".h", ".hpp", ".inl", ".c", ".cc", ".cpp", ".cs"}
HASH_COMMENT = {".py", ".ps1", ".sh", ".cmake"}
BATCH = {".bat", ".cmd"}
SPDX_TEXT = "SPDX-License-Identifier: Apache-2.0"
# Split so this file does not match itself.
EPIC_COPYRIGHT = "Copyright " + "Epic Games"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

# C++20 standard library headers and the C compatibility headers (ISO/IEC 14882:2020 [headers]).
STD_HEADERS = set("""
algorithm any array atomic barrier bit bitset charconv chrono codecvt compare complex concepts condition_variable
coroutine deque exception execution expected filesystem format forward_list fstream functional future
initializer_list iomanip ios iosfwd iostream istream iterator latch limits list locale map memory memory_resource
mutex new numbers numeric optional ostream queue random ranges ratio regex scoped_allocator semaphore set
shared_mutex source_location span sstream stack stdexcept stop_token streambuf string string_view syncstream
system_error thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector
version
cassert cctype cerrno cfenv cfloat cinttypes climits clocale cmath csetjmp csignal cstdarg cstddef cstdint cstdio
cstdlib cstring ctime cuchar cwchar cwctype
""".split())
# Compiler intrinsics: not engine headers, and the only non-standard headers a checksum may need.
INTRINSIC_HEADERS = {"intrin.h", "immintrin.h", "nmmintrin.h", "smmintrin.h", "emmintrin.h", "arm_neon.h", "arm_acle.h"}


def rel(path, root):
    return os.path.relpath(path, root).replace(os.sep, "/")


def walk_files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            yield os.path.join(dirpath, name)


def read_lines(path):
    with open(path, "r", encoding="utf-8-sig", errors="replace") as handle:
        return handle.read().splitlines()


def is_source(path):
    name = os.path.basename(path)
    ext = os.path.splitext(name)[1].lower()
    return ext in C_FAMILY or ext in HASH_COMMENT or ext in BATCH or name == "CMakeLists.txt"


def spdx_ok(path, lines):
    name = os.path.basename(path)
    ext = os.path.splitext(name)[1].lower()
    head = [line.strip() for line in lines[:3]]
    if ext in C_FAMILY:
        return bool(head) and head[0] == "// " + SPDX_TEXT
    if ext in BATCH:
        # "@echo off" may come first.
        return any(re.fullmatch(r"(?i)(rem|::)\s+" + re.escape(SPDX_TEXT), line) for line in head)
    if ext in HASH_COMMENT or name == "CMakeLists.txt":
        # A shebang or an encoding line may come first.
        return any(line == "# " + SPDX_TEXT for line in head)
    return True


def scan_spdx(plugin, host_scripts):
    violations = []
    count = 0
    roots = [plugin] + ([host_scripts] if os.path.isdir(host_scripts) else [])
    base = os.path.dirname(os.path.dirname(plugin))
    for root in roots:
        for path in walk_files(root):
            if not is_source(path):
                continue
            count += 1
            lines = read_lines(path)
            if not spdx_ok(path, lines):
                violations.append("%s:1: missing the SPDX header (%s)" % (rel(path, base), SPDX_TEXT))
            for number, line in enumerate(lines, 1):
                if EPIC_COPYRIGHT in line:
                    violations.append("%s:%d: Epic copyright text (rule 2: no Epic source copied)" % (rel(path, base), number))
    return count, violations


def resolve_include(including, target, search_dirs):
    candidates = [os.path.join(os.path.dirname(including), target)] + [os.path.join(d, target) for d in search_dirs]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return os.path.normcase(os.path.abspath(candidate))
    return None


def scan_python_h(plugin):
    source = os.path.join(plugin, "Source")
    editor_private = os.path.join(source, "DreamEditor", "Private")
    search_dirs = [
        editor_private,
        os.path.join(source, "DreamCore", "Public"),
        os.path.join(source, "DreamCore", "Private"),
        os.path.join(source, "DreamEditorTests", "Private"),
    ]
    the_include = os.path.normcase(os.path.join(editor_private, "Python", "DreamPythonInclude.h"))
    allowed_dirs = [os.path.normcase(os.path.join(editor_private, d)) + os.sep for d in ("Python", "Compat")]
    # The test-only module's 14.2 fixtures (_dream_test, SleepNative, FillPendingCallQueue): repository only (see above).
    allowed_dirs.append(os.path.normcase(os.path.join(source, "DreamEditorTests", "Private", "Python")) + os.sep)
    run_dir = os.path.normcase(os.path.join(editor_private, "Run")) + os.sep

    def is_allowed(path):
        path = os.path.normcase(path)
        if any(path.startswith(d) for d in allowed_dirs):
            return True
        return path.startswith(run_dir) and os.path.basename(path).lower().startswith("runsupervisor")

    # normcase'd path -> the path as found, for messages.
    files = {os.path.normcase(os.path.abspath(p)): p for p in walk_files(source)
             if os.path.splitext(p)[1].lower() in {".h", ".hpp", ".inl", ".cpp", ".c", ".cc"}}
    includes = {}
    violations = []
    for path, shown in files.items():
        entries = []
        for number, line in enumerate(read_lines(path), 1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            target = match.group(2)
            if os.path.basename(target.replace("\\", "/")).lower() == "python.h" and path != the_include:
                violations.append("%s:%d: includes %s directly; use Python/DreamPythonInclude.h" % (rel(shown, plugin), number, target))
            entries.append((number, target, resolve_include(path, target, search_dirs)))
        includes[path] = entries

    # Headers that reach Python.h: DreamPythonInclude.h and, transitively, every header that includes one of them.
    tainted = {the_include}
    changed = True
    while changed:
        changed = False
        for path, entries in includes.items():
            if path in tainted or not path.endswith((".h", ".hpp", ".inl")):
                continue
            if any(resolved in tainted for _, _, resolved in entries):
                tainted.add(path)
                changed = True

    for path, entries in includes.items():
        if is_allowed(path):
            continue
        for number, target, resolved in entries:
            if resolved in tainted:
                violations.append("%s:%d: includes %s, which reaches Python.h; only Private/Python, Private/Compat and "
                                  "Private/Run/RunSupervisor* may (rule 6)" % (rel(files[path], plugin), number, target))
    return len(files), violations


def scan_dreamcore(plugin):
    core = os.path.join(plugin, "Source", "DreamCore")
    adapter = os.path.normcase(os.path.join(core, "Private", "UE")) + os.sep
    core_root = os.path.normcase(core) + os.sep
    search_dirs = [os.path.join(core, "Public"), os.path.join(core, "Private")]
    violations = []
    count = 0
    for path in walk_files(core):
        if os.path.splitext(path)[1].lower() not in {".h", ".hpp", ".inl", ".cpp", ".c", ".cc"}:
            continue
        if os.path.normcase(os.path.abspath(path)).startswith(adapter):
            continue
        count += 1
        for number, line in enumerate(read_lines(path), 1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            delimiter, target = match.group(1), match.group(2).replace("\\", "/")
            if target.startswith("dream/"):
                resolved = resolve_include(path, target, search_dirs)
                if resolved and resolved.startswith(core_root) and not resolved.startswith(adapter):
                    continue
                violations.append("%s:%d: %s does not resolve to DreamCore/Public/dream" % (rel(path, plugin), number, target))
                continue
            if delimiter == "<" and (target in STD_HEADERS or target.lower() in INTRINSIC_HEADERS):
                continue
            if delimiter == '"':
                resolved = resolve_include(path, target, search_dirs)
                if resolved and resolved.startswith(core_root) and not resolved.startswith(adapter):
                    continue
            violations.append("%s:%d: includes %s%s%s; DreamCore outside Private/UE may include only the standard library "
                              "and dream/... (rule 1)" % (rel(path, plugin), number, delimiter, target, ">" if delimiter == "<" else '"'))
    return count, violations


def scan_content(plugin):
    violations = []
    count = 0
    content = os.path.join(plugin, "Content")
    lib = os.path.normcase(os.path.join(content, "Python", "Lib")) + os.sep
    tests = os.path.normcase(os.path.join(plugin, "Tests")) + os.sep
    for path in walk_files(plugin):
        count += 1
        norm = os.path.normcase(os.path.abspath(path))
        name = os.path.basename(path)
        in_content = norm.startswith(os.path.normcase(content) + os.sep)
        if in_content and re.fullmatch(r"test_.*\.py", name, re.IGNORECASE):
            violations.append("%s: test_*.py under Content/ (rule 3: PythonAutomationTest would run it)" % rel(path, plugin))
        if name.lower() == "init_unreal.py" and not norm.startswith(tests):
            violations.append("%s: init_unreal.py in the plugin (rule 4)" % rel(path, plugin))
        if norm.startswith(lib):
            violations.append("%s: a file under Content/Python/Lib (rule 12: Dream Python Editor ships nothing there)" % rel(path, plugin))
    return count, violations


# ---- C++ text without comments (and, for most rules, without string contents) -----------------------------------------

CPP_EXTS = {".h", ".hpp", ".inl", ".cpp", ".c", ".cc"}
RAW_STRING_RE = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\v\f\n]{0,16})\(')
SHIPPED_MODULES = ("DreamEditor", "DreamCore", "DreamMCP")


def strip_cpp(text, keep_strings):
    """C++ source with comments blanked (newlines kept, so line numbers hold); string and character literal contents are
    blanked too unless keep_strings. Raw strings, escapes and digit separators (1'000) are handled."""
    out = []
    i, n = 0, len(text)

    def blank(segment):
        return "".join("\n" if ch == "\n" else " " for ch in segment)

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(blank(text[i:j]))
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(blank(text[i:j]))
            i = j
            continue
        prev = text[i - 1] if i > 0 else ""
        if c in "uULR" and not (prev.isalnum() or prev == "_"):
            match = RAW_STRING_RE.match(text, i)
            if match:
                close = ")" + match.group(1) + '"'
                end = text.find(close, match.end())
                end = n if end < 0 else end + len(close)
                body = text[match.end():end - len(close)] if end <= n else ""
                out.append(text[i:match.end()] + (body if keep_strings else blank(body)) + close)
                i = end
                continue
        if c == '"' or (c == "'" and not (prev.isalnum() or prev == "_")):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j, n - 1)
            body = text[i + 1:j]
            out.append(c + (body if keep_strings else blank(body)) + (text[j] if j < n else ""))
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def cpp_files(plugin, modules=SHIPPED_MODULES):
    for module in modules:
        root = os.path.join(plugin, "Source", module)
        if not os.path.isdir(root):
            continue
        for path in walk_files(root):
            if os.path.splitext(path)[1].lower() in CPP_EXTS:
                yield path


def read_text(path):
    with open(path, "r", encoding="utf-8-sig", errors="replace") as handle:
        return handle.read()


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def call_arguments(text, open_paren):
    """The top-level argument strings of the call whose '(' is at open_paren (in comment-stripped text)."""
    depth, start, args = 0, open_paren + 1, []
    for index in range(open_paren, len(text)):
        ch = text[index]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:index].strip())
                return [a for a in args if a]
        elif ch == "," and depth == 1:
            args.append(text[start:index].strip())
            start = index + 1
    return args


# ---- Rule 5: one entry call ------------------------------------------------------------------------------------------

ENTRY_FILE = "DreamEditor/Private/Python/DreamPythonEntry.cpp"
ENTRY_TEMPLATES = ("__import__('dream_editor')._run(", "__import__('dream_editor')._boot(globals(), ")
ENTRY_BANNED_RE = re.compile(r"\b(PyRun_\w+|PyEval_EvalCode\w*|Py_CompileString\w*|PyImport_ExecCodeModule\w*)\s*\(")
# Reviewed call sites that run no user code through a second entry: (file, called API, required first-argument prefix or
# None, the C++ function the call must sit in or None, reason). Each entry admits exactly one call site. Reported on the
# slice-B board for the architect (tests B-note).
ENTRY_EXEMPT = [
    ("DreamEditor/Private/Python/DreamNativeModule.cpp", "PyEval_EvalCode", None, "RunStatement",
     "run_statement step 4 evaluates the statement's prepared code object: TDD 5.3.7 statement mode prescribes "
     "PyEval_EvalCode(code, ns, ns) inside the one entry _run"),
    ("DreamEditor/Private/Compat/DreamPyCompat.cpp", "PyRun_StringFlags", '"None"', None,
     "ClearAsyncExc evaluates the literal None so the pending async exception is delivered at a safe point (SUP-01, "
     "T-CAN-009); no user code"),
]
# A function definition: a return type, the name, the parameter list and the opening brace on the next line(s); the
# keywords that take a parenthesised head are not names.
FUNCTION_DEF_RE = re.compile(r"\n[ \t]*(?:static[ \t]+|inline[ \t]+)*[\w:<>]+[ \t*&]+[*&]?([A-Za-z_][\w:]*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:override\s*)?\{")
NOT_FUNCTION_NAMES = {"if", "for", "while", "switch", "catch", "return", "sizeof"}


def enclosing_function(code, pos):
    """The name of the last function definition that starts before pos (comments and strings already stripped)."""
    name = None
    for match in FUNCTION_DEF_RE.finditer(code, 0, pos):
        if match.group(1) not in NOT_FUNCTION_NAMES:
            name = match.group(1).split("::")[-1]
    return name


def module_relative(path, plugin):
    return rel(path, os.path.join(plugin, "Source"))


def scan_entry(plugin):
    violations, count = [], 0
    entry_calls = 0
    exempt_uses = {}
    for path in cpp_files(plugin):
        count += 1
        shown = module_relative(path, plugin)
        raw = read_text(path)
        code = strip_cpp(raw, keep_strings=False)
        with_strings = strip_cpp(raw, keep_strings=True)
        for match in re.finditer(r"\bExecPythonCommand(?:Ex)?\s*\(", code):
            if shown != ENTRY_FILE:
                violations.append("%s:%d: ExecPythonCommand outside %s (rule 5, D14)" % (shown, line_of(code, match.start()), ENTRY_FILE))
            elif "Ex" in match.group(0):
                entry_calls += 1
        for match in ENTRY_BANNED_RE.finditer(code):
            name = match.group(1)
            first = call_arguments(with_strings, match.end() - 1)
            first_arg = first[0] if first else ""
            allowed = False
            for index, (f, fn, prefix, function, _) in enumerate(ENTRY_EXEMPT):
                if shown == f and name == fn and (prefix is None or first_arg.startswith(prefix)) \
                        and (function is None or enclosing_function(code, match.start()) == function):
                    exempt_uses[index] = exempt_uses.get(index, 0) + 1
                    allowed = exempt_uses[index] == 1     # one reviewed call site per entry
            if not allowed:
                violations.append("%s:%d: %s: user code may enter only through _run (rule 5, D14, T-RUN-020)" % (shown, line_of(code, match.start()), name))
        if shown == ENTRY_FILE:
            for template in ENTRY_TEMPLATES:
                if template not in with_strings:
                    violations.append("%s:1: the entry template %s is missing (section 3.6 INV-3)" % (shown, template))
            templates = re.findall(r"__import__\('dream_editor'\)\.(\w+)\(", with_strings)
            for name in templates:
                if name not in ("_run", "_boot"):
                    violations.append("%s:1: a third entry template __import__('dream_editor').%s (rule 5: two literal templates)" % (shown, name))
    if entry_calls != 2:
        violations.append("%s:1: %d ExecPythonCommandEx calls; rule 5 allows exactly two (EnterRun, Boot)" % (ENTRY_FILE, entry_calls))
    return count, violations


# ---- Rule 7: compat confinement --------------------------------------------------------------------------------------

COMPAT_CPP_RE = re.compile(r"->\s*cframe\b|->\s*gilstate_counter\b|\b_Py[A-Za-z]\w*|\bPY_(?:VERSION_HEX|MAJOR_VERSION|MINOR_VERSION|MICRO_VERSION|RELEASE_LEVEL|RELEASE_SERIAL)\b")


def python_files(root):
    if not os.path.isdir(root):
        return
    for path in walk_files(root):
        if path.endswith(".py"):
            yield path


def python_code_tokens(path):
    """(type, string, line) of every token that is code: comments and string literals (docstrings included) dropped."""
    with open(path, "rb") as handle:
        try:
            tokens = list(tokenize.tokenize(handle.readline))
        except (tokenize.TokenError, SyntaxError, IndentationError):
            return None
    return [(t.type, t.string, t.start[0]) for t in tokens if t.type not in (tokenize.COMMENT, tokenize.STRING, tokenize.NL,
                                                                           tokenize.NEWLINE, tokenize.ENCODING)]


def scan_compat(plugin):
    violations, count = [], 0
    for path in cpp_files(plugin):
        shown = module_relative(path, plugin)
        count += 1
        if shown.startswith("DreamEditor/Private/Compat/"):
            continue
        code = strip_cpp(read_text(path), keep_strings=False)
        for match in COMPAT_CPP_RE.finditer(code):
            violations.append("%s:%d: %s outside Private/Compat (rule 7, D15)" % (shown, line_of(code, match.start()), match.group(0).strip()))
    content = os.path.join(plugin, "Content", "Python")
    allowed = os.path.normcase(os.path.join(content, "dream_editor", "_impl", "compat.py"))
    for path in python_files(content):
        count += 1
        if os.path.normcase(path) == allowed:
            continue
        tokens = python_code_tokens(path)
        if tokens is None:
            violations.append("%s:1: does not tokenize" % rel(path, plugin))
            continue
        for index, (kind, text, line) in enumerate(tokens):
            if text != "version_info":
                continue
            dotted = index >= 2 and tokens[index - 1][1] == "." and tokens[index - 2][1] == "sys"
            imported = any(t[1] == "import" for t in tokens[max(0, index - 4):index]) and any(t[1] == "sys" for t in tokens[max(0, index - 4):index])
            if dotted or imported:
                violations.append("%s:%d: sys.version_info outside dream_editor/_impl/compat.py (rule 7)" % (rel(path, plugin), line))
    return count, violations


# ---- Rule 8: banned APIs ---------------------------------------------------------------------------------------------

BANNED_CPP = [
    (re.compile(r"GetCoreTicker\s*\(\s*\)\s*\.\s*Tick\s*\("), "FTSTicker::GetCoreTicker().Tick( (RT-01: the pump never ticks the core ticker)"),
    (re.compile(r"\bEnterDebuggingMode\b"), "EnterDebuggingMode (D3)"),
    (re.compile(r"\bPyErr_SetInterrupt\w*"), "PyErr_SetInterrupt (D2: Stop is ScriptCancelled through the pending call)"),
    (re.compile(r"\bFInteractiveProcess\b"), "FInteractiveProcess (D4: language servers use FLspTransport)"),
    (re.compile(r"\bFMonitoredProcess\b"), "FMonitoredProcess (D4)"),
    (re.compile(r"\bEPythonCommandFlags\s*::\s*Unattended\b"), "EPythonCommandFlags::Unattended (D14: runs stay attended)"),
]
def scan_banned(plugin):
    violations, count = [], 0
    for path in cpp_files(plugin):
        count += 1
        shown = module_relative(path, plugin)
        code = strip_cpp(read_text(path), keep_strings=False)
        for pattern, what in BANNED_CPP:
            for match in pattern.finditer(code):
                violations.append("%s:%d: %s (rule 8)" % (shown, line_of(code, match.start()), what))
        # WritePipe has an FString overload (FString, optional FString* written) next to the byte one (Data, Length,
        # optional int32*): two arguments, or a third that is not a length, is the FString overload.
        for match in re.finditer(r"\bWritePipe\s*\(", code):
            args = call_arguments(code, match.end() - 1)
            if len(args) == 2 or (len(args) == 3 and (args[2] == "nullptr" or args[2].startswith("&"))):
                violations.append("%s:%d: WritePipe with an FString (rule 8, D4: framed bytes only)" % (shown, line_of(code, match.start())))
    for root in (os.path.join(plugin, "Content", "Python"), os.path.join(plugin, "Tools", "Python")):
        for path in python_files(root):
            count += 1
            tokens = python_code_tokens(path)
            if tokens is None:
                continue
            words = [(text, line) for _, text, line in tokens]
            for index, (text, line) in enumerate(words):
                if text == "wait_for_client":
                    violations.append("%s:%d: debugpy wait_for_client (rule 8, D3: the editor never blocks on a client)" % (rel(path, plugin), line))
                if text == "configure" and index >= 2 and words[index - 1][0] == "." and words[index - 2][0] == "debugpy":
                    violations.append("%s:%d: debugpy.configure( (rule 8, RT-11)" % (rel(path, plugin), line))
                # pip: importing it (D3: never pip-install into the engine interpreter).
                if text == "pip" and index > 0 and words[index - 1][0] in ("import", "from"):
                    violations.append("%s:%d: imports pip (rule 8)" % (rel(path, plugin), line))
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                for number, line in enumerate(handle, 1):
                    if re.search(r"""["']-m["']\s*,\s*["']pip["']|-m pip\b|\bpip install\b""", line) and not line.lstrip().startswith("#"):
                        violations.append("%s:%d: runs pip (rule 8)" % (rel(path, plugin), number))
    return count, violations


# ---- Rule 9: ticker work gated ---------------------------------------------------------------------------------------

TICKER_RE = re.compile(r"(?<![\w:])(?:FDreamDeferral\s*::\s*)?AddTicker\s*\(|\bRegisterActiveTimer\s*\(|\bOnEndFrame\s*(?:\(\s*\))?\s*\.\s*Add\w*\s*\(")
TICKER_FILES = {"DreamEditor/Private/Run/DreamDeferral.cpp", "DreamEditor/Private/Run/DreamDeferral.h", "DreamEditor/Private/Run/DreamRunService.cpp"}
EXEMPT_RE = re.compile(r"\bDREAM_TIMER_EXEMPT\s*\(\s*paint-only")


def scan_ticker(plugin):
    violations, count = [], 0
    for path in cpp_files(plugin):
        count += 1
        shown = module_relative(path, plugin)
        if shown in TICKER_FILES:
            continue
        raw = read_text(path)
        code = strip_cpp(raw, keep_strings=False)
        raw_lines = raw.splitlines()
        for match in TICKER_RE.finditer(code):
            text = match.group(0)
            if text.startswith("FDreamDeferral"):
                continue
            # Member functions named AddTicker on another object are still raw tickers; FDreamDeferral:: is the gate.
            before = code[max(0, match.start() - 40):match.start()]
            if re.search(r"FDreamDeferral\s*::\s*$", before):
                continue
            line = line_of(code, match.start())
            # The exemption marker sits on one of the three lines above the call (it is a macro statement).
            window = [raw_lines[i] for i in range(max(0, line - 4), line) if i < len(raw_lines)]
            if any(EXEMPT_RE.search(w) for w in window):
                continue
            violations.append("%s:%d: raw %s outside FDreamDeferral and FDreamRunService without DREAM_TIMER_EXEMPT(paint-only) "
                              "(rule 9, RT-01)" % (shown, line, text.split("(")[0].strip()))
    return count, violations


# ---- Rule 10: module direction ---------------------------------------------------------------------------------------

DEP_LIST_RE = re.compile(r"(Public|Private)DependencyModuleNames\s*\.\s*(Add|AddRange)\s*\(")
DYN_LIST_RE = re.compile(r"DynamicallyLoadedModuleNames\s*\.\s*(Add|AddRange)\s*\(")


def build_cs_dependencies(path):
    code = strip_cpp(read_text(path), keep_strings=True)
    deps = []
    for pattern in (DEP_LIST_RE, DYN_LIST_RE):
        for match in pattern.finditer(code):
            args = call_arguments(code, match.end() - 1)
            for arg in args:
                deps.extend(re.findall(r'"([A-Za-z0-9_]+)"', arg))
    return deps


def experimental_modules(engine):
    """Names of the engine modules under an Experimental folder (Source/*/Experimental, Plugins/Experimental)."""
    names = set()
    for base in (os.path.join(engine, "Engine", "Plugins", "Experimental"), os.path.join(engine, "Engine", "Source")):
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and d not in ("ThirdParty",)]
            in_experimental = "Experimental" in dirpath.replace("\\", "/").split("/") or base.endswith("Experimental")
            if not in_experimental:
                continue
            for name in filenames:
                if name.endswith(".Build.cs"):
                    names.add(name[:-len(".Build.cs")])
    return names


def scan_buildcs(plugin):
    violations = []
    source = os.path.join(plugin, "Source")
    files = {}
    for module in os.listdir(source) if os.path.isdir(source) else []:
        path = os.path.join(source, module, module + ".Build.cs")
        if os.path.isfile(path):
            files[module] = path
    for module, path in sorted(files.items()):
        deps = build_cs_dependencies(path)
        shown = rel(path, plugin)
        if module == "DreamCore":
            for dep in deps:
                if dep != "Core":
                    violations.append("%s:1: DreamCore depends on %s; DreamCore -> Core only (rule 10, D7)" % (shown, dep))
        if module == "DreamEditor" and "DreamMCP" in deps:
            violations.append("%s:1: DreamEditor depends on DreamMCP (rule 10, D11)" % shown)
        if module == "DreamMCP":
            engine = os.environ.get("UE_ROOT", r"C:\Program Files\Epic Games\UE_5.8")
            experimental = experimental_modules(engine)
            for dep in deps:
                if dep in experimental:
                    violations.append("%s:1: DreamMCP depends on the Experimental module %s (rule 10, D11)" % (shown, dep))
    if "DreamCore" not in files:
        violations.append("Source/DreamCore/DreamCore.Build.cs: missing")
    return len(files), violations


# ---- Rule 11: no UFUNCTION -------------------------------------------------------------------------------------------

def scan_ufunction(plugin):
    violations, count = [], 0
    for path in cpp_files(plugin, modules=("DreamEditor",)):
        count += 1
        code = strip_cpp(read_text(path), keep_strings=False)
        for match in re.finditer(r"\bUFUNCTION\s*\(", code):
            violations.append("%s:%d: UFUNCTION in DreamEditor (rule 11, D29: no Blueprint or reflected API in 1.0)" % (module_relative(path, plugin), line_of(code, match.start())))
    return count, violations


# ---- Rule 12: manifests and licences ---------------------------------------------------------------------------------

# D12's permissive allow-list plus the character-data licences (TDD 11.10, OI-48) and EPL-1.0 for the unmodified pydevd
# inside _dream_vendor/debugpy only (RT-11). SPDX identifiers.
ALLOWED_LICENCES = {"Apache-2.0", "MIT", "BSD-2-Clause", "BSD-3-Clause", "ISC", "Zlib", "OFL-1.1", "PSF-2.0",
                    "Python-2.0", "Unicode-3.0", "Unicode-DFS-2016", "BSL-1.0", "CC0-1.0", "0BSD"}
EPL_ONLY_UNDER = "Content/Python/_dream_vendor/debugpy/"
PRUNED_PATTERNS = [
    (re.compile(r"(^|/)inject_dll_[^/]*$"), "inject_dll_*"),
    (re.compile(r"(^|/)run_code_on_dllmain_[^/]*$"), "run_code_on_dllmain_*"),
    (re.compile(r"(^|/)[^/]*(x86|_x86|win32)[^/]*\.(dll|exe|pyd|so)$", re.IGNORECASE), "x86 files"),
    (re.compile(r"\.pdb$", re.IGNORECASE), ".pdb files"),
    (re.compile(r"(^|/)winappdbg(/|$)"), "winappdbg"),
    (re.compile(r"\.pyd$", re.IGNORECASE), "non-cp311 .pyd"),
]
MANIFESTS = ["Resources/Manifest.json", "Binaries/ThirdParty/DreamTools/manifest.json"]


def load_manifest_entries(plugin):
    """path (plugin-relative, '/') -> entry dict, from every manifest that exists. A manifest is a JSON list of entries,
    or an object with a "files" list; each entry has path, sha256, licence (or license) and optionally size."""
    entries, problems = {}, []
    for name in MANIFESTS:
        path = os.path.join(plugin, name)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8") as handle:
                doc = json.load(handle)
        except (OSError, ValueError) as error:
            problems.append("%s:1: unreadable manifest (%s)" % (name, error))
            continue
        items = doc.get("files", []) if isinstance(doc, dict) else doc
        base = os.path.dirname(name)
        for item in items if isinstance(items, list) else []:
            if not isinstance(item, dict) or "path" not in item:
                problems.append("%s:1: an entry without a path" % name)
                continue
            listed = item["path"].replace("\\", "/")
            # Paths in the DreamTools manifest are relative to its folder; Resources/Manifest.json lists plugin paths.
            key = listed if name.startswith("Resources/") else (base + "/" + listed)
            entries[key] = item
    return entries, problems


def scan_manifest(plugin):
    entries, violations = load_manifest_entries(plugin)
    count = 0
    for top in ("Binaries/ThirdParty", "Content/Python/_dream_vendor"):
        root = os.path.join(plugin, top)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames if d != "__pycache__")
            for name in sorted(filenames):
                path = os.path.join(dirpath, name)
                shown = rel(path, plugin)
                count += 1
                if shown.endswith("DreamTools/manifest.json"):
                    continue
                for pattern, what in PRUNED_PATTERNS:
                    if what == "non-cp311 .pyd":
                        if shown.startswith(EPL_ONLY_UNDER) and pattern.search(shown) and "cp311" not in name:
                            violations.append("%s: %s must be pruned (RT-11)" % (shown, what))
                    elif shown.startswith(EPL_ONLY_UNDER) and pattern.search(shown):
                        violations.append("%s: %s must be pruned (RT-11)" % (shown, what))
                entry = entries.get(shown)
                if entry is None:
                    violations.append("%s: not listed in %s (rule 12, D4)" % (shown, " or ".join(MANIFESTS)))
                    continue
                with open(path, "rb") as handle:
                    digest = hashlib.sha256(handle.read()).hexdigest()
                if str(entry.get("sha256", "")).lower() != digest:
                    violations.append("%s: SHA-256 %s does not match the manifest (rule 12)" % (shown, digest))
                licence = entry.get("licence", entry.get("license", ""))
                ok = licence in ALLOWED_LICENCES or (licence == "EPL-1.0" and shown.startswith(EPL_ONLY_UNDER))
                if not ok:
                    violations.append("%s: licence %r is not on the allow-list (rule 12, D12, MAINT-05)" % (shown, licence))
    return count, violations


# ---- T-GAP-001: LOCTEXT scan -----------------------------------------------------------------------------------------

LITERAL_TEXT_RE = re.compile(r'\b(FText\s*::\s*FromString|FText\s*::\s*AsCultureInvariant|FText\s*::\s*FromStringView|INVTEXT)\s*\(')
LOCTEXT_RE = re.compile(r'\bLOCTEXT\s*\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"')
NSLOCTEXT_RE = re.compile(r'\bNSLOCTEXT\s*\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"')
NAMESPACE_DEFINE_RE = re.compile(r'^\s*#\s*define\s+LOCTEXT_NAMESPACE\s+"([^"]*)"', re.MULTILINE)
NAMESPACE_UNDEF_RE = re.compile(r'^\s*#\s*undef\s+LOCTEXT_NAMESPACE\b', re.MULTILINE)
STRING_LITERAL_RE = re.compile(r'(?:TEXT\s*\(\s*)?(?:u8|L|u|U)?"((?:[^"\\]|\\.)*)"')


PRINTF_SPEC_RE = re.compile(r"%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L|I64)?[diouxXeEfFgGcspn%]|\{\d*\}")
# Calls whose first literal argument is the text itself; a literal handed to any other call (a path segment, a key)
# is not shown.
TRANSPARENT_CALLS = {"TEXT"}
FORMAT_CALLS = {"FString::Printf", "Printf", "FString", "FString::Format", "FString::Join"}


def shown_literals(expression):
    """The string literals of an FText argument that reach the user: at the top level of the expression (a ternary
    branch, a + concatenation), through TEXT(), or as the format of FString::Printf / FString(...)."""
    shown = []
    for match in re.finditer(r'(?:u8|L|u|U)?"((?:[^"\\]|\\.)*)"', expression):
        stack = []
        for index in range(match.start()):
            ch = expression[index]
            if ch == "(":
                head = re.search(r"([A-Za-z_][\w:]*)\s*$", expression[:index])
                stack.append((head.group(1).replace(" ", "") if head else "", index))
            elif ch == ")" and stack:
                stack.pop()
        while stack and stack[-1][0] in TRANSPARENT_CALLS:
            stack.pop()
        if not stack:
            shown.append(match.group(1))
            continue
        name, open_index = stack[-1]
        if name in FORMAT_CALLS:
            first = expression[open_index + 1:match.start()].strip()
            if first in ("", "TEXT(") or re.fullmatch(r"TEXT\s*\(", first):
                shown.append(PRINTF_SPEC_RE.sub("", match.group(1)))
    return shown


def literal_has_letter(argument):
    """True when a literal the user sees (shown_literals) holds a letter: text a translator would see. A literal of
    symbols only ("×", " · ", "\\n", "[%s]") carries nothing to translate (tests B-note)."""
    for body in shown_literals(argument):
        body = re.sub(r"\\.", "", body)
        if any(ch.isalpha() for ch in body):
            return True
    return False


def loctext_baseline():
    """Known violations reported on the slice-B board for their owners: (file, line text substring) -> reason. A listed
    violation passes until its owner fixes it; then its entry must go (a stale entry fails)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "loctext_baseline.json")
    if not os.path.isfile(path):
        return {}
    with open(path, "r", encoding="utf-8") as handle:
        doc = json.load(handle)
    return {(item["file"], item["contains"]): item["reason"] for item in doc.get("known", [])}


def scan_loctext(plugin):
    violations, count = [], 0
    keys = {}
    baseline = loctext_baseline()
    used_baseline = set()
    for path in cpp_files(plugin, modules=("DreamEditor",)):
        count += 1
        shown = module_relative(path, plugin)
        raw = read_text(path)
        raw_lines = raw.splitlines()
        code = strip_cpp(raw, keep_strings=True)
        for match in LITERAL_TEXT_RE.finditer(code):
            args = call_arguments(code, match.end() - 1)
            if args and literal_has_letter(args[0]):
                line = line_of(code, match.start())
                text = raw_lines[line - 1] if line - 1 < len(raw_lines) else ""
                known = [key for key in baseline if key[0] == shown and key[1] in text]
                if known:
                    used_baseline.update(known)
                    continue
                violations.append("%s:%d: %s on a literal shown to users; use LOCTEXT (T-GAP-001, A11Y-06)"
                                  % (shown, line, re.sub(r"\s+", "", match.group(1))))
        uses_loctext = bool(LOCTEXT_RE.search(code))
        defines = NAMESPACE_DEFINE_RE.findall(code)
        if uses_loctext and not defines:
            violations.append("%s:1: LOCTEXT without a LOCTEXT_NAMESPACE define (T-GAP-001)" % shown)
        for namespace in defines:
            if namespace != "Dream":
                violations.append("%s:1: LOCTEXT_NAMESPACE \"%s\"; Dream's namespace is \"Dream\" (A49)" % (shown, namespace))
        if defines and path.endswith(".cpp") and not NAMESPACE_UNDEF_RE.search(code):
            violations.append("%s:1: LOCTEXT_NAMESPACE is defined but never undefined (unity builds)" % shown)
        for match in LOCTEXT_RE.finditer(code):
            keys.setdefault(("Dream", match.group(1)), []).append((match.group(2), shown, line_of(code, match.start())))
        for match in NSLOCTEXT_RE.finditer(code):
            if match.group(1) != "Dream":
                violations.append("%s:%d: NSLOCTEXT namespace \"%s\"; Dream's namespace is \"Dream\" (A49)" % (shown, line_of(code, match.start()), match.group(1)))
            keys.setdefault((match.group(1), match.group(2)), []).append((match.group(3), shown, line_of(code, match.start())))
    for (namespace, key), uses in sorted(keys.items()):
        texts = {text for text, _, _ in uses}
        if len(texts) > 1:
            first = uses[0]
            for text, shown, line in uses[1:]:
                if text != first[0]:
                    violations.append("%s:%d: key %s.%s has another source text than %s:%d (one key, one text)" % (shown, line, namespace, key, first[1], first[2]))
    for key in sorted(set(baseline) - used_baseline):
        violations.append("Tests/ci/loctext_baseline.json: stale entry for %s (%r): the violation is gone, remove the entry" % key)
    return count, violations


# ---- T-GAP-008: traceability -----------------------------------------------------------------------------------------

# ---- Test names: ASCII only (tests, PO-04 integration follow-up) -------------------------------------------------------

TEST_NAME_CALL = re.compile(r'(?<![\w.>:~])(?:x?Describe|x?It|x?LatentIt|LatentDescribe)\s*\(')
STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
TEST_NAME_SPEC = re.compile(r'\bBEGIN_DEFINE_SPEC\s*\(\s*\w+\s*,\s*"((?:[^"\\\n]|\\.)*)"')
TEST_NAME_IMPLEMENT = re.compile(
    r'\bIMPLEMENT_(?:CUSTOM_)?(?:SIMPLE|COMPLEX)_AUTOMATION_TEST\s*\(\s*\w+\s*,\s*(?:\w+\s*,\s*)?"((?:[^"\\\n]|\\.)*)"')


def first_argument_span(code, open_paren):
    """(start, end) of the first top-level argument of the call whose '(' is at open_paren, in comment-stripped code."""
    depth, index = 0, open_paren
    while index < len(code):
        ch = code[index]
        if ch == '"':
            literal = STRING_LITERAL.match(code, index)
            index = literal.end() if literal else index + 1
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                return open_paren + 1, index
        elif ch == "," and depth == 1:
            return open_paren + 1, index
        index += 1
    return open_paren + 1, len(code)


def scan_testnames(plugin):
    violations, count = [], 0
    for path in cpp_files(plugin, modules=("DreamEditorTests",)):
        count += 1
        text = read_text(path)
        code = strip_cpp(text, keep_strings=True)   # comments blanked, offsets kept
        spans = []
        for match in TEST_NAME_CALL.finditer(code):
            start, end = first_argument_span(code, match.end() - 1)
            spans.extend((literal.start(1), literal.end(1)) for literal in STRING_LITERAL.finditer(code, start, end))
        for pattern in (TEST_NAME_SPEC, TEST_NAME_IMPLEMENT):
            spans.extend((match.start(1), match.end(1)) for match in pattern.finditer(code))
        for begin, finish in spans:
            literal = text[begin:finish]
            bad = sorted(set(ch for ch in literal if ord(ch) > 127))
            if bad:
                violations.append("%s:%d: test name with non-ASCII %s: \"%s\" (the narrow name goes through the ANSI code page; "
                                  "write it in ASCII, for example 'TDD 7.1' for the section sign)"
                                  % (rel(path, plugin), line_of(text, begin), " ".join("U+%04X" % ord(ch) for ch in bad),
                                     literal[:80].encode("ascii", "replace").decode("ascii")))
    return count, violations


def scan_trace(args):
    tdd = os.path.join(args.docs, "TDD.md")
    spec = os.path.join(args.docs, "SPEC.md")
    terms = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lands_terms.json")
    for needed in (tdd, spec, terms):
        if not os.path.isfile(needed):
            return 0, ["%s: missing (T-GAP-008)" % needed]
    summary, violations, tests = dream_trace.check(tdd, spec, terms)
    for line in summary:
        print("trace: " + line)
    if args.list_milestone:
        for line in dream_trace.milestone_listing(tests, args.list_milestone):
            print("trace: %s exit requires %s" % (args.list_milestone, line))
    return len(tests), violations


SCANS = {
    "spdx": lambda args: scan_spdx(args.plugin, args.host_scripts),
    "python-h": lambda args: scan_python_h(args.plugin),
    "dreamcore": lambda args: scan_dreamcore(args.plugin),
    "content": lambda args: scan_content(args.plugin),
    "entry": lambda args: scan_entry(args.plugin),
    "compat": lambda args: scan_compat(args.plugin),
    "banned": lambda args: scan_banned(args.plugin),
    "ticker": lambda args: scan_ticker(args.plugin),
    "buildcs": lambda args: scan_buildcs(args.plugin),
    "ufunction": lambda args: scan_ufunction(args.plugin),
    "manifest": lambda args: scan_manifest(args.plugin),
    "loctext": lambda args: scan_loctext(args.plugin),
    "trace": scan_trace,
    "testnames": lambda args: scan_testnames(args.plugin),
}


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scans", nargs="*", help="scans to run: %s (default: all)" % ", ".join(sorted(SCANS)))
    parser.add_argument("--plugin", default=PLUGIN_DEFAULT, help="the DreamPythonEditor plugin folder")
    parser.add_argument("--host-scripts", default=HOST_SCRIPTS_DEFAULT, help="DreamHost/Scripts, scanned for SPDX headers")
    parser.add_argument("--docs", default=DOCS_DEFAULT, help="the folder holding TDD.md and SPEC.md (trace)")
    parser.add_argument("--list-milestone", default="", choices=[""] + dream_trace.MILESTONES,
                        help="trace: also print every test and split step that milestone's exit requires")
    args = parser.parse_args(argv)
    args.docs = os.path.abspath(args.docs)
    unknown = [name for name in args.scans if name not in SCANS]
    if unknown:
        print("dream_ci_scans: unknown scan %s; known: %s" % (", ".join(unknown), ", ".join(sorted(SCANS))))
        return 2
    args.plugin = os.path.abspath(args.plugin)
    if not os.path.isdir(os.path.join(args.plugin, "Source")):
        print("dream_ci_scans: %s has no Source folder" % args.plugin)
        return 2

    failed = False
    for name in args.scans or sorted(SCANS):
        count, violations = SCANS[name](args)
        for violation in violations:
            print(violation)
        print("scan %-9s %s (%d files)" % (name, "FAILED: %d violations" % len(violations) if violations else "ok", count))
        failed = failed or bool(violations)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
