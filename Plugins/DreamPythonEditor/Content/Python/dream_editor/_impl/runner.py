# SPDX-License-Identifier: Apache-2.0
"""The runner: ``dream_editor._impl.runner`` (TDD §5.3, RUN-06..RUN-08, RUN-11, RUN-16, RUN-18, RUN-27, RT-06, RT-14).

``prepare(run_id, request)`` runs in Starting (§5.1.2 step 2) as a C-API helper call: it compiles and caches, and never
executes user code (D14(5)). ``run(run_id)`` runs inside ``_run``, entered from the one-shot OnEndFrame handler through
``ExecPythonCommandEx``: swaps, the user-code window, statement mode or one ``exec``, classification, restore, report.

Works under plain CPython with the null ``_dream`` of ``native`` (tier 2) and under ``-run=PythonScript`` (tier 5).
"""

import ast
import collections
import itertools
import linecache
import os
import sys
import threading
import time
import types

from . import aliases
from . import autoprint
from . import compat
from . import namespaces
from . import native
from . import streams
from . import task

__all__ = ["prepare", "discard", "run", "report_internal_error", "classify_input", "execute_for_test", "RunResult",
           "format_exception", "on_buffer_saved", "active_run_id", "shutdown", "STATEMENT_ROUTES"]

# Routes whose auto-print, and so statement mode, is On in a Session run (§5.2.1). Bridge is 1.1.
STATEMENT_ROUTES = frozenset({"selection", "line", "cell", "buffer", "repl", "console", "bridge"})
# Routes whose prepare refreshes the selection aliases under the U19 rule (§5.2.1 Aliases column): an API run of a tab
# ("as the target"); Run File and an API run of a file get none. Shelf and snippet runs (fresh bindings) are M4.
ALIAS_ROUTES = frozenset({"selection", "line", "cell", "buffer", "repl", "console", "api", "bridge"})
_PROMPT_ROUTES = frozenset({"repl", "console"})

_MAX_PREPARED = 16              # Starting is one run at a time; this only bounds requests C++ never discarded
_PROMPT_LINECACHE_KEEP = 500    # linecache entries of REPL and console inputs: the last 500 runs (§6.5, RUN-12)
_CHAIN_LIMIT = 16               # __context__ / __cause__ links walked for "ours" (§5.3.9, proposed)
_DETAIL_LIMIT = 200             # characters of an error detail, "…" included (ruling R1)

_MISSING = object()

_IMPL_DIR = os.path.normcase(os.path.dirname(os.path.abspath(__file__)))
_PACKAGE_DIR = os.path.dirname(_IMPL_DIR)


class _Prepared:
    """One prepared request (``_PREPARED[run_id]``)."""

    __slots__ = ("run_id", "run_number", "route", "filename", "file_path", "namespace", "ns", "kind", "code",
                 "statements", "first_line_by_code", "setup_annotations", "autoprint", "run_file", "argv",
                 "first_line", "early_cancel", "debug", "debug_config")


_PREPARED = collections.OrderedDict()

# The run between entry and report (one at a time, D14(4)).
_active = {"run_id": 0, "run_number": 0, "reported": False, "swaps": None}

# linecache entries this module wrote: filename -> the exact tuple (identity checks, §5.3.2 "Entry lifetime").
_linecache_ours = {}
_prompt_files = collections.deque()


def active_run_id():
    """The run inside ``_run`` now, or 0."""
    return _active["run_id"]


# ---- linecache (§5.3.2) ------------------------------------------------------------------------------------------

def _is_pseudo(filename):
    return filename.startswith("<") and filename.endswith(">")


def _source_lines(text):
    # Split on LF only, as the compiler counts lines (str.splitlines would also split on form feeds and U+2028).
    if not text:
        return []
    parts = text.split("\n")
    if parts[-1] == "":
        parts.pop()
    return [part + "\n" for part in parts]


def _register_linecache(filename, text):
    entry = (len(text), None, _source_lines(text), filename)    # mtime None: checkcache() never evicts it
    linecache.cache[filename] = entry
    _linecache_ours[filename] = entry
    return entry


def _forget_linecache(filename):
    entry = _linecache_ours.pop(filename, None)
    if entry is not None and linecache.cache.get(filename) is entry:
        del linecache.cache[filename]


def on_buffer_saved(path):
    """A dirty buffer was saved: drop its linecache entry if it is still the tuple we wrote (§5.3.2)."""
    if isinstance(path, str) and not _is_pseudo(path):
        _forget_linecache(path)


def _forget_pseudo_entries():
    # Reset Session ends the lifetime of pseudo-file entries (§5.3.2).
    for filename in [name for name in _linecache_ours if _is_pseudo(name)]:
        _forget_linecache(filename)
    _prompt_files.clear()


namespaces.add_reset_hook(_forget_pseudo_entries)


def _register_source(req):
    whole = req["linecache_text"]
    if whole is None:
        # Only the run's text: pad it so its lines keep their buffer numbers (first_line > 1).
        # (run) PO-06 F2: a fragment's first line is padded to its column, so code positions match the cached line.
        whole = "\n" * (req["first_line"] - 1) + " " * req["first_column"] + req["text"]
    filename = req["filename"]
    if _is_pseudo(filename) or req["dirty"]:
        _register_linecache(filename, whole)
        if req["route"] in _PROMPT_ROUTES and _is_pseudo(filename):
            if filename in _prompt_files:
                _prompt_files.remove(filename)
            _prompt_files.append(filename)
            while len(_prompt_files) > _PROMPT_LINECACHE_KEEP:
                _forget_linecache(_prompt_files.popleft())
    else:
        # A clean saved file: linecache reads the disk, which equals the buffer; drop a dirty-time entry of ours.
        _forget_linecache(filename)
    return whole


# ---- requests ------------------------------------------------------------------------------------------------------

def _normalise_newlines(text):
    if "\r" in text:
        text = text.replace("\r\n", "\n").replace("\r", "\n")
    return text


def _normalise_request(run_id, request):
    if not isinstance(request, dict):
        raise TypeError("prepare(run_id, request): request must be a dict")
    text = request.get("text")
    if not isinstance(text, str):
        raise TypeError("prepare(): request['text'] must be str")
    route = request.get("route") or "buffer"
    namespace = request.get("namespace") or "session"
    if namespace not in ("session", "isolated"):
        raise ValueError(f"prepare(): unknown namespace {namespace!r}")
    first_line = request.get("first_line", 1)
    if isinstance(first_line, bool) or not isinstance(first_line, int):
        raise TypeError("prepare(): request['first_line'] must be an int")
    first_column = request.get("first_column", 0) or 0
    if isinstance(first_column, bool) or not isinstance(first_column, int):
        raise TypeError("prepare(): request['first_column'] must be an int")
    run_number = request.get("run_number", 0) or 0
    default_mode = namespace == "session" and route in STATEMENT_ROUTES
    statement_mode = bool(request.get("statement_mode", default_mode))
    autoprint_on = bool(request.get("autoprint", statement_mode))
    if namespace == "isolated":
        # §5.2.1: the Isolated choice turns auto-print, and so statement mode, off.
        statement_mode = autoprint_on = False
    filename = request.get("filename") or f"<dream:Run#{run_number}>"
    linecache_text = request.get("linecache_text")
    if linecache_text is not None:
        if not isinstance(linecache_text, str):
            raise TypeError("prepare(): request['linecache_text'] must be str or None")
        linecache_text = _normalise_newlines(linecache_text)
    argv = request.get("argv") or []
    return {
        "run_id": run_id,
        "run_number": int(run_number),
        "route": route,
        "text": _normalise_newlines(text),
        "first_line": max(1, first_line),
        # (run) PO-06 F2: the UTF-16 column of first_line where a selection fragment's text starts (0: the line start).
        "first_column": max(0, first_column),
        "filename": filename,
        "linecache_text": linecache_text,
        "file_path": request.get("file_path") or None,
        "dirty": bool(request.get("dirty", False)),
        "namespace": namespace,
        "autoprint": autoprint_on,
        "statement_mode": statement_mode,
        "argv": [str(arg) for arg in argv],
        # (slice A choice) Run File semantics (sys.path entry, argv) for route run_file, or an API run of a file that
        # sets "run_file": True.
        "run_file": bool(request.get("run_file", route == "run_file")),
        "tab_title": request.get("tab_title") or "",
        # §5.3.8: C++ sends False when the user setting turns the aliases off; Run File semantics never get them.
        "aliases": bool(request.get("aliases", route in ALIAS_ROUTES))
        and route in ALIAS_ROUTES and not request.get("run_file", route == "run_file"),
        # (debug, PO-06 F5) A Debug run (F5, §8 Terms) and its DebugConfig (§8.1.5) as C++ built it: JSON text or a dict.
        "debug": bool(request.get("debug", False)),
        "debug_config": request.get("debug_config"),
    }


# ---- prepare: compile rules (§5.3.2) ---------------------------------------------------------------------------

class _PrepareError(Exception):
    """A compile failure with the line delta that maps the reported line to a buffer line."""

    def __init__(self, error, delta, phase):
        super().__init__(error)
        self.error = error
        self.delta = delta
        self.phase = phase          # "parse": CPython reports characters; "compile": UTF-8 bytes (from the AST)


def _first_code_line(lines):
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            return index
    return None


def _parse(text, filename, first_line):
    """RUN-06 / RT-14: parse with exact buffer lines; an indented first line goes through the ``if True:`` wrapper."""
    lines = text.split("\n")
    first_code = _first_code_line(lines)
    if first_code is None or lines[first_code][:1] not in (" ", "\t"):
        try:
            tree = ast.parse(text, filename, "exec")
        except (SyntaxError, ValueError, OverflowError, RecursionError) as exc:
            raise _PrepareError(exc, first_line - 1, "parse") from None
        ast.increment_lineno(tree, first_line - 1)
        return tree
    wrapped = "if True:\n" + text
    try:
        tree = ast.parse(wrapped, filename, "exec")
    except (SyntaxError, ValueError, OverflowError, RecursionError) as exc:
        raise _PrepareError(exc, first_line - 2, "parse") from None
    head = tree.body[0]
    if head.orelse:
        # A column-0 'else' or 'elif' bound to the wrapper's 'if True:'; in the buffer it matches nothing.
        wrapped_lines = wrapped.split("\n")
        line_no = head.body[-1].end_lineno + 1
        for candidate in range(line_no, len(wrapped_lines) + 1):
            if wrapped_lines[candidate - 1].startswith("el"):
                line_no = candidate
                break
        source_line = wrapped_lines[line_no - 1] if line_no <= len(wrapped_lines) else ""
        error = SyntaxError("invalid syntax", (filename, line_no, 1, source_line + "\n", line_no, 5))
        raise _PrepareError(error, first_line - 2, "parse")
    # Statements after the indented block that start at column 0 stay in the run, in order.
    module = ast.Module(body=head.body + tree.body[1:], type_ignores=tree.type_ignores)
    ast.increment_lineno(module, first_line - 2)
    return module


def _is_bare_string(stmt):
    return isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Constant) and isinstance(stmt.value.value, str)


def _future_flags(module):
    """The OR of the compiler flags of the module's leading ``from __future__`` imports (§5.3.2)."""
    flags = 0
    body = module.body
    start = 1 if body and _is_bare_string(body[0]) else 0
    for stmt in body[start:]:
        if not (isinstance(stmt, ast.ImportFrom) and stmt.module == "__future__" and not stmt.level):
            break
        import __future__   # only buffers with future imports pay for it

        for alias in stmt.names:
            feature = getattr(__future__, alias.name, None)
            if feature is not None:
                flags |= feature.compiler_flag
    return flags


def _statement_first_line(stmt):
    # On 3.11 a decorated def or class reports its 'def' line; its first line is the first decorator's.
    first = stmt.lineno
    for decorator in getattr(stmt, "decorator_list", ()):
        first = min(first, decorator.lineno)
    return first


def _buffer_line(req, line):
    """The text of a 1-based buffer line, from the whole buffer when C++ sent it, else from the run's text."""
    whole = req["linecache_text"]
    if whole is not None:
        lines = whole.split("\n")
        index = line - 1
    else:
        lines = (" " * req["first_column"] + req["text"]).split("\n")    # (run) the fragment at its column
        index = line - req["first_line"]
    if 0 <= index < len(lines):
        return lines[index]
    return None


_BLANKS = " \t\f"


def _fragment_layout(req):
    """(run, PO-06 F2, q1-m1) How a selection fragment compiles: ``(text, delta_chars, delta_bytes)``.

    C++ sends ``first_column`` (UTF-16 units) when the selection starts after code on its first line. The fragment's
    first line is given its buffer line's indentation in place of the text before it (and its own leading blanks), so a
    fragment that continues into an indented body lines up with the lines after it, and the RT-14 wrapper removes the
    common indentation. ``delta_*`` is how far the first line's code sits further right in the buffer than in ``text``:
    characters for parser offsets, UTF-8 bytes for AST columns. Without ``first_column``: the text, 0, 0.
    """
    text = req["text"]
    units = req["first_column"]
    if units <= 0:
        return text, 0, 0
    line = _buffer_line(req, req["first_line"]) or ""
    chars = 0
    seen = 0
    while chars < len(line) and seen < units:
        seen += 2 if ord(line[chars]) > 0xFFFF else 1
        chars += 1
    first, newline, rest = text.partition("\n")
    stripped = first.lstrip(_BLANKS)
    code_start = min(chars + len(first) - len(stripped), len(line))
    indent = line[:len(line) - len(line.lstrip(_BLANKS))]
    if len(indent) > code_start:
        indent = indent[:code_start]
    delta_chars = code_start - len(indent)
    delta_bytes = len(line[:code_start].encode("utf-8", "surrogatepass")) - len(indent.encode("utf-8", "surrogatepass"))
    return indent + stripped + newline + rest, delta_chars, delta_bytes


def _shift_first_line(module, req):
    """(run, PO-06 F2) Moves the AST positions on the fragment's first line to buffer columns (UTF-8 bytes), so code
    objects carry buffer columns: traceback carets, linecache lines and compile errors line up with the buffer."""
    delta = _fragment_layout(req)[2]
    if delta <= 0:
        return
    line = req["first_line"]
    for node in ast.walk(module):
        if getattr(node, "lineno", None) == line and getattr(node, "col_offset", None) is not None:
            node.col_offset += delta
        if getattr(node, "end_lineno", None) == line and getattr(node, "end_col_offset", None) is not None:
            node.end_col_offset += delta


def _utf8_column(text, chars):
    """A 0-based character column as a 0-based UTF-8 byte column of ``text``."""
    return len(text[:chars].encode("utf-8", "surrogatepass"))


def _error_dict(failure, req):
    """The prepare error in buffer coordinates (§5.3.2). Offsets are 1-based UTF-8 columns (FDreamSyntaxError).

    CPython reports parser errors in characters and compiler errors (from the AST) in UTF-8 bytes; both become bytes.
    """
    exc = failure.error
    text_lines = req["text"].split("\n")
    last_line = req["first_line"] + max(len(text_lines), 1) - 1
    if isinstance(exc, SyntaxError):
        msg = exc.msg if exc.msg is not None else str(exc)
        reported = exc.lineno
    else:
        msg = str(exc) or type(exc).__name__
        reported = None
    if reported is None or reported <= 0:
        line = req["first_line"]
        first_code = _first_code_line(text_lines)
        if first_code is not None:
            line += first_code
    else:
        line = reported + failure.delta if failure.phase == "parse" else reported
    line = min(max(line, req["first_line"]), last_line)
    source = _buffer_line(req, line)
    if source is None:
        source = (getattr(exc, "text", None) or "").rstrip("\r\n")

    in_chars = failure.phase == "parse"
    unit_length = len(source) if in_chars else len(source.encode("utf-8", "surrogatepass"))
    offset = getattr(exc, "offset", None)
    defaulted = not offset or offset < 0
    if defaulted:
        offset = 1
    # (run, PO-06 F2) Parser offsets count from the start of the compiled text line; on a fragment's first line its
    # code sits delta columns further right in the buffer (_fragment_layout). Compile errors come from the AST, which
    # _shift_first_line already moved to buffer columns.
    shift = 0
    if line == req["first_line"] and (in_chars or defaulted):
        _text, shift_chars, shift_bytes = _fragment_layout(req)
        shift = shift_chars if in_chars else shift_bytes
    offset += shift
    end_offset = getattr(exc, "end_offset", None)
    end_line = getattr(exc, "end_lineno", None)
    if end_line is not None and reported is not None and end_line != reported:
        end_offset = unit_length + 1        # the error runs past this line: mark to its end
    elif end_offset in (None, 0):
        end_offset = offset
    else:
        end_offset += shift
    if end_offset <= offset:
        end_offset = offset + 1
    if in_chars:
        offset = _utf8_column(source, offset - 1) + 1
        end_offset = _utf8_column(source, end_offset - 1) + 1
    return {
        "msg": msg,
        "lineno": line,
        "offset": offset,
        "end_offset": max(end_offset, offset + 1),
        "text": source,
        "type": type(exc).__name__,     # (slice A addition) "SyntaxError", "IndentationError", "TabError", ...
    }


def _compile_statements(module, filename, flags):
    statements = []
    first_line_by_code = {}
    with streams.silenced_warning_display():
        for index, stmt in enumerate(module.body):
            first = _statement_first_line(stmt)
            last = getattr(stmt, "end_lineno", None) or stmt.lineno
            if index >= 1 and _is_bare_string(stmt):
                # Compiled alone it would be a module docstring and rebind __doc__; echoed, not executed (§5.3.2).
                code = None
            else:
                code = compile(ast.Module(body=[stmt], type_ignores=[]), filename, "exec", flags=flags,
                               dont_inherit=True)
                first_line_by_code[id(code)] = first
            statements.append((first, last, code))
    return statements, first_line_by_code


def prepare(run_id, request):
    """Helper (§12.1.3): compile and cache one request. Returns ``{'ok': True, 'statements': [...] | None}`` or
    ``{'ok': False, 'error': {...}}``. Executes no user code."""
    if isinstance(run_id, bool) or not isinstance(run_id, int):
        raise TypeError("prepare(run_id, request): run_id must be an int")
    req = _normalise_request(run_id, request)
    _PREPARED.pop(run_id, None)
    filename = req["filename"]
    _register_source(req)
    # §5.3.8 (U19): the aliases are refreshed while the interpreter is idle, before any user code; C++ bumps
    # FDreamPython::GetSessionEpoch when a name was written ("aliases_written").
    aliases_written = 0
    if req["aliases"]:
        try:
            aliases_written = aliases.refresh(req["run_number"])
        except Exception:  # noqa: BLE001 - an alias problem never fails the prepare
            aliases_written = 0

    if req["namespace"] == "session":
        ns = namespaces.session()
        if ns is None:
            raise RuntimeError("the Session namespace is not captured: dream_editor._boot has not run")
    else:
        ns = namespaces.new_isolated(req["file_path"])

    try:
        module = _parse(_fragment_layout(req)[0], filename, req["first_line"])     # (run) q1-m1: a fragment's layout
        _shift_first_line(module, req)      # (run) PO-06 F2: buffer columns for a selection fragment
        if req["autoprint"]:
            autoprint.rewrite_last_expression(module)
        try:
            # Exactly as one exec: every compile-time error ends the run not executed before anything runs (RUN-16).
            whole = compile(module, filename, "exec", dont_inherit=True)
        except (SyntaxError, ValueError, OverflowError, RecursionError) as exc:
            raise _PrepareError(exc, 0, "compile") from None
        statements = first_line_by_code = None
        if req["statement_mode"]:
            try:
                statements, first_line_by_code = _compile_statements(module, filename, _future_flags(module))
            except (SyntaxError, ValueError, OverflowError, RecursionError) as exc:
                raise _PrepareError(exc, 0, "compile") from None    # not expected after the whole-module compile
    except _PrepareError as failure:
        return {"ok": False, "error": _error_dict(failure, req), "aliases_written": aliases_written}

    prep = _Prepared()
    prep.run_id = run_id
    prep.run_number = req["run_number"]
    prep.route = req["route"]
    prep.filename = filename
    prep.file_path = req["file_path"]
    prep.namespace = req["namespace"]
    prep.ns = ns
    prep.kind = "statements" if statements is not None else "exec"
    prep.code = whole if statements is None else None
    prep.statements = statements
    prep.first_line_by_code = first_line_by_code or {id(whole): req["first_line"]}
    prep.setup_annotations = statements is not None and compat.has_setup_annotations(whole)
    prep.autoprint = req["autoprint"]
    prep.run_file = req["run_file"]
    prep.argv = req["argv"]
    prep.first_line = req["first_line"]
    prep.early_cancel = None
    prep.debug = req["debug"]                   # (debug)
    prep.debug_config = req["debug_config"]     # (debug)
    _PREPARED[run_id] = prep
    while len(_PREPARED) > _MAX_PREPARED:
        _PREPARED.popitem(last=False)
    if statements is None:
        return {"ok": True, "statements": None, "aliases_written": aliases_written}
    return {"ok": True, "statements": [[first, last] for first, last, _code in statements],
            "aliases_written": aliases_written}


def discard(run_id):
    """Helper (§12.1.3): Starting was aborted after a successful prepare."""
    _PREPARED.pop(run_id, None)


# ---- the run swaps (§5.3.4, §5.3.5) ------------------------------------------------------------------------------

class _RunSwaps:
    """The run's slots: the §6.1 stream slots, time.sleep, __dream_display__, dunders, sys.path and sys.argv."""

    def __init__(self, prep, dream):
        self.prep = prep
        self.dream = dream
        self.stream_swap = streams.StreamSwap(prep.run_id, prep.run_number, dream)
        self.run_swap = streams.Swap(prep.run_number)
        self.file_swap = streams.Swap(prep.run_number)
        self.saved_dunders = None
        self.path_entry = None
        self.done = False

    def install(self):
        prep = self.prep
        dream = self.dream
        streams.register_active(self)
        self.stream_swap.install()
        session = namespaces.session()
        autoprint.set_context(dream, prep.run_id, session if session is not None else prep.ns)
        pre_run_sleep = getattr(time, "sleep", None)
        self.run_swap.put(streams.AttrSlot(time, "sleep", "time.sleep"), task.sliced_sleep)
        task.begin_run(prep.run_id, prep.run_number, pre_run_sleep)
        if prep.autoprint:
            self.run_swap.put(streams.KeySlot(prep.ns, autoprint.DISPLAY_NAME),
                              autoprint.make_display(dream, prep.run_id, prep.ns))
        if prep.namespace == "session":
            ns = prep.ns
            self.saved_dunders = (ns.get("__name__", _MISSING), ns.get("__file__", _MISSING))
            ns["__name__"] = "__main__"
            if prep.file_path:
                ns["__file__"] = prep.file_path
            else:
                ns.pop("__file__", None)        # untitled buffers, REPL and console: no __file__ (D8)
        # (debug, PO-06 F5) §8.2.4: a run-scoped sys.breakpointhook for every Dream run; only a Debug run imports the
        # debugger (DBG-06: no cost when not debugging).
        if prep.debug:
            from . import debugger
            hook = debugger.breakpointhook
        else:
            hook = _make_breakpoint_notice(dream, prep.run_id)
        self.run_swap.put(streams.AttrSlot(sys, "breakpointhook", "sys.breakpointhook"), hook)
        if prep.run_file and prep.file_path:
            entry = os.path.dirname(prep.file_path)
            sys.path.insert(0, entry)
            self.path_entry = entry             # removed by identity, never by value (§5.3.4)
            self.file_swap.put(streams.AttrSlot(sys, "argv", "sys.argv"), [prep.file_path, *prep.argv])

    def restore(self):
        """Reverse order of install; every step in its own try (§5.3.5). Idempotent."""
        if self.done:
            return []
        self.done = True
        notes = []
        try:
            notes.extend(self.file_swap.restore())
        except Exception:  # noqa: BLE001
            pass
        if self.path_entry is not None:
            try:
                path = sys.path
                for index, item in enumerate(path):
                    if item is self.path_entry:
                        del path[index]
                        break
            except Exception:  # noqa: BLE001
                pass
            self.path_entry = None
        if self.saved_dunders is not None:
            try:
                ns = self.prep.ns
                saved_name, saved_file = self.saved_dunders
                ns.pop("__file__", None)            # pop, never del: the key may be gone already (E4)
                if saved_file is not _MISSING:
                    ns["__file__"] = saved_file
                ns.pop("__name__", None)
                if saved_name is not _MISSING:
                    ns["__name__"] = saved_name
            except Exception:  # noqa: BLE001
                pass
            self.saved_dunders = None
        try:
            notes.extend(self.run_swap.restore())
        except Exception:  # noqa: BLE001
            pass
        task.end_run()
        try:
            notes.extend(self.stream_swap.restore())
        except Exception:  # noqa: BLE001
            pass
        streams.unregister_active(self)
        return notes


def _make_breakpoint_notice(dream, run_id):
    """(debug) §8.2.4: ``breakpoint()`` outside a Debug run writes one notice per run and returns."""
    said = []

    def breakpointhook(*args, **kwargs):
        if not said:
            said.append(True)
            dream.write(dream.STREAM_DEBUGGER, run_id,
                       "breakpoint() ignored: this is not a Debug run (press F5 to debug)\n")
        return None

    return breakpointhook


# ---- run (§5.3.7) ------------------------------------------------------------------------------------------------

def _execute(prep, dream):
    if prep.kind == "statements":
        if prep.setup_annotations:
            # What SETUP_ANNOTATIONS does at the start of one exec (§5.3.2).
            prep.ns.setdefault("__annotations__", {})
        run_statement = dream.run_statement
        run_id = prep.run_id
        ns = prep.ns
        for index, (_first, _last, code) in enumerate(prep.statements):
            # C: Stop check, started = index + 1, echo, eval, returned = index + 1 (§5.3.7).
            run_statement(run_id, index, code, ns)
    else:
        exec(prep.code, prep.ns)  # noqa: S102 - one exec: Run File, Isolated runs, an API run of a file
    # Deferred (M3): the kinds "task_step" (§5.10.4), "reload" (§9) and "entry_point" (§9.4).


def _thread_snapshot():
    return {thread.ident for thread in threading.enumerate()}


def _label_threads(dream, threads):
    for thread in threads:
        native_id = getattr(thread, "native_id", None)
        if native_id is not None:
            try:
                dream.thread_label(native_id, thread.name)
            except Exception:  # noqa: BLE001 - labels are cosmetic
                pass


def _thread_notes(dream, before):
    current = threading.current_thread()
    # A _DummyThread is a foreign native thread that touched threading during the run; the run did not start it.
    alive = [thread for thread in threading.enumerate()
             if thread.ident not in before and thread is not current and thread.is_alive()
             and type(thread).__name__ != "_DummyThread"]
    if not alive:
        return []
    _label_threads(dream, alive)
    names = ", ".join(thread.name + (" (daemon)" if thread.daemon else "") for thread in alive)
    if len(alive) == 1:
        return [f"1 thread started by this run is still running: {names}. Dream Python Editor does not stop it."]
    return [f"{len(alive)} threads started by this run are still running: {names}. Dream Python Editor does not stop them."]


def run(run_id):
    """Run a prepared request (§5.3.7). Reports the outcome exactly once; ``_run`` catches anything that escapes."""
    _active["reported"] = False
    dream = native.get()
    prep = _PREPARED.pop(run_id, None)
    if prep is None:
        _active["reported"] = True
        dream.report_outcome(run_id, native.OUTCOME_NOT_EXECUTED, "request expired", None)
        return
    _active.update(run_id=run_id, run_number=prep.run_number)
    try:
        _run_prepared(prep, dream)
    finally:
        _active.update(run_id=0, run_number=0, swaps=None)


def _run_prepared(prep, dream):
    run_id = prep.run_id
    namespaces.release_last_run()               # the last-run namespace lives until the next run starts (§5.3.3)
    swaps = _RunSwaps(prep, dream)
    _active["swaps"] = swaps
    outcome = None
    threads_before = frozenset()
    debug = None
    try:
        swaps.install()
        threads_before = _thread_snapshot()
        alive = list(threading.enumerate())
        _label_threads(dream, alive)
        dream.capture_begin(run_id, [t.native_id for t in alive if getattr(t, "native_id", None) is not None])
        if prep.debug:
            # (debug) §8.1.5: after stream setup, before user code.
            from . import debugger as debug
            debug.begin_run(prep.debug_config, run_id=run_id)
        try:
            if not dream.user_code_begin(run_id):
                # Stop came before user code: the runner raises ScriptCancelled itself (DreamNativeModule.h).
                prep.early_cancel = task.new_cancelled()
                raise prep.early_cancel
            _execute(prep, dream)
            dream.user_code_done(run_id)
            outcome = _ok(prep, dream)
        except BaseException as exc:
            dream.user_code_done(run_id)         # FIRST statement of the handler (RT-06)
            stopped = False
            if debug is not None:
                # (debug) §8.2.5, DBG-05: the uncaught-exception stop, after user_code_done; Stop there ends cancelled.
                try:
                    stopped = debug.on_uncaught(exc)
                except Exception as failure:  # noqa: BLE001 - the user's outcome must survive a debugger problem
                    dream.write(dream.STREAM_NOTICE, run_id,
                               f"Dream Python Editor could not pause at the exception: {type(failure).__name__}: {failure}\n")
            outcome = _classify(prep, dream, exc)
            if stopped and outcome[0] == native.OUTCOME_ERROR:
                outcome = (native.OUTCOME_CANCELLED, "stopped at the exception", outcome[2])
            try:
                _after_failure(prep, dream, exc, outcome)
            except Exception as failure:  # noqa: BLE001 - the user's outcome must survive a formatting problem
                dream.write(dream.STREAM_NOTICE, run_id,
                           f"Dream Python Editor could not write the traceback: {type(failure).__name__}: {failure}\n")
    finally:
        if debug is not None:
            debug.end_run(run_id)               # (debug) compare-and-restore the trace before the swaps
        notes = swaps.restore()
    if prep.namespace == "isolated":
        namespaces.set_last_run(prep.ns)
    notes.extend(_thread_notes(dream, threads_before))
    for note in notes:
        dream.write(dream.STREAM_NOTICE, run_id, note + "\n")    # footer notes (slice A choice A23)
    _report(dream, run_id, outcome)
    dream.capture_end(run_id)


def _report(dream, run_id, outcome):
    code, detail, location = outcome
    _active["reported"] = True
    dream.report_outcome(run_id, code, detail, location)


def _ok(prep, dream):
    if dream.stop_requested(prep.run_id):
        return (native.OUTCOME_CANCELLED, "the script handled Stop and returned", None)
    return (native.OUTCOME_OK, "", None)


def _is_ours(exc, run_number):
    cls = task.cancel_class()
    if cls is None or not isinstance(exc, cls):
        return False
    return getattr(exc, "run_number", 0) in (0, run_number)


def _chain_contains_ours(exc, run_number):
    """"Ours" on ``exc`` or in its __context__ / __cause__ chain, up to 16 links (§5.3.9, §5.7.1)."""
    pending = [(exc, 0)]
    seen = set()
    while pending:
        current, depth = pending.pop()
        if current is None or id(current) in seen:
            continue
        seen.add(id(current))
        if _is_ours(current, run_number):
            return True
        if depth < _CHAIN_LIMIT:
            pending.append((current.__cause__, depth + 1))
            pending.append((current.__context__, depth + 1))
    return False


def _type_name(cls):
    module = getattr(cls, "__module__", None)
    name = getattr(cls, "__qualname__", cls.__name__)
    if module and module not in ("builtins", "__main__"):
        return f"{module}.{name}"
    return name


def _error_detail(exc):
    """The error detail of §5.3.9 (slice A ruling R1): ``"<Type>: <first line of str(exc)>"``, or ``"<Type>"`` when
    that line is empty, cut to 200 characters with "…". It is also the runtime Problems message; C++ takes the text
    before the first ": " where it needs the bare type. The line stays in ``location``."""
    name = _type_name(type(exc))
    try:
        text = str(exc)
    except BaseException:  # noqa: BLE001 - a user __str__ must not break classification; CPython's traceback text
        text = "<exception str() failed>"
    lines = text.splitlines()
    first = lines[0].rstrip() if lines else ""
    detail = f"{name}: {first}" if first else name
    if len(detail) > _DETAIL_LIMIT:
        detail = detail[:_DETAIL_LIMIT - 1] + "\N{HORIZONTAL ELLIPSIS}"
    return detail


def _is_dream_file(filename, _cache={}):  # noqa: B006 - a per-process cache of path tests
    hit = _cache.get(filename)
    if hit is None:
        if _is_pseudo(filename):
            hit = (False, False)
        else:
            directory = os.path.normcase(os.path.dirname(os.path.abspath(filename)))
            in_impl = directory == _IMPL_DIR or directory.startswith(_IMPL_DIR + os.sep)
            in_package = in_impl or directory == os.path.normcase(_PACKAGE_DIR)
            hit = (in_impl, in_package)
        if len(_cache) < 4096:
            _cache[filename] = hit
    return hit


def _frame_line(prep, frame, lineno):
    if lineno is None or lineno <= 0:
        # A Stop at a statement's first instruction (RESUME, line 0) is shown at that statement's first line.
        return prep.first_line_by_code.get(id(frame.f_code), prep.first_line) if prep is not None else None
    return lineno


def _location(prep, exc):
    """(filename, line) of the innermost frame of the run's file, else of the innermost non-Dream frame, else None."""
    innermost_user = None
    innermost_other = None
    tb = exc.__traceback__
    while tb is not None:
        frame = tb.tb_frame
        filename = frame.f_code.co_filename
        if filename == prep.filename:
            innermost_user = (filename, _frame_line(prep, frame, tb.tb_lineno))
        elif not _is_dream_file(filename)[1]:
            innermost_other = (filename, tb.tb_lineno if tb.tb_lineno and tb.tb_lineno > 0 else 1)
        tb = tb.tb_next
    if innermost_user is not None and innermost_user[1]:
        return innermost_user, True
    return innermost_other, False


def _exit_code(exc):
    code = exc.code
    if code is None:
        return 0, None
    if isinstance(code, int):
        return int(code), None
    return 1, str(code)     # as CPython: a non-int code is printed to stderr and the exit status is 1


def _classify(prep, dream, exc):
    """Outcome classification (§5.3.9). Returns (outcome, detail, location)."""
    if prep.early_cancel is not None and exc is prep.early_cancel:
        return (native.OUTCOME_CANCELLED, "stopped before user code started", None)
    location, in_run_file = _location(prep, exc)
    if _chain_contains_ours(exc, prep.run_number):
        if not in_run_file:
            # The Stop landed in the runner, or run_statement's Stop check raised it (§5.3.7).
            if prep.kind == "statements":
                _started, returned = dream.statement_counts(prep.run_id)
                total = len(prep.statements)
                if returned >= total:
                    return (native.OUTCOME_CANCELLED, "Stop arrived as the code finished", None)
                return (native.OUTCOME_CANCELLED, f"stopped after statement {returned} of {total}", None)
            return (native.OUTCOME_CANCELLED, "Stop arrived as the code finished", None)
        return (native.OUTCOME_CANCELLED, "", location)
    if isinstance(exc, SystemExit):
        code, _message = _exit_code(exc)
        outcome = native.OUTCOME_OK if code == 0 else native.OUTCOME_ERROR
        return (outcome, f"exited (code {code})", location)
    # Any other BaseException, a foreign KeyboardInterrupt included.
    return (native.OUTCOME_ERROR, _error_detail(exc), location)


def _after_failure(prep, dream, exc, outcome):
    """Error output after user_code_done (§5.3.7): the traceback without runner frames, the SystemExit message."""
    code = outcome[0]
    if isinstance(exc, SystemExit):
        # As CPython: no traceback for SystemExit (RUN-18); a non-int code is printed.
        _code, message = _exit_code(exc)
        if message is not None:
            dream.write(dream.STREAM_STDERR, prep.run_id, message + "\n")
        return
    if code == native.OUTCOME_CANCELLED and _is_ours(exc, prep.run_number):
        return      # the footer says where it landed; ScriptCancelled itself is no error
    if code not in (native.OUTCOME_ERROR, native.OUTCOME_CANCELLED):
        return
    dream.write(dream.STREAM_ERROR, prep.run_id, format_exception(exc, prep))
    # Deferred (M2): PostMortem.retain(run_id, exc) (§8.7); sys.last_* are never set.
    if code == native.OUTCOME_ERROR:
        _call_user_excepthook(prep, dream, exc)


def _call_user_excepthook(prep, dream, exc):
    """A studio's sys.excepthook runs in a second, short user-code window, so Stop works inside it (§5.3.7)."""
    hook = getattr(sys, "excepthook", None)
    if hook is None or hook is sys.__excepthook__ or not callable(hook):
        return
    run_id = prep.run_id
    try:
        if dream.user_code_begin(run_id):
            hook(type(exc), exc, _filtered_traceback(exc.__traceback__, prep))
        dream.user_code_done(run_id)
    except BaseException as hook_exc:  # noqa: BLE001 - the run's outcome stays error
        dream.user_code_done(run_id)
        dream.write(dream.STREAM_NOTICE, run_id,
                   f"sys.excepthook raised {_type_name(type(hook_exc))}: {hook_exc}\n")


def report_internal_error(run_id):
    """Last resort of ``_run`` (§5.3.1): a runner bug. Restores the swaps and reports "Dream Python Editor internal error"."""
    import traceback

    dream = native.get()
    text = traceback.format_exc()
    swaps = _active.get("swaps")
    if swaps is not None:
        try:
            swaps.restore()
        except Exception:  # noqa: BLE001
            pass
    streams.emergency_restore()
    task.end_run()
    try:
        dream.write(dream.STREAM_ERROR, run_id, "Dream Python Editor internal error\n" + text)
    finally:
        already = _active.get("reported")
        _active.update(run_id=0, run_number=0, swaps=None)
        if not already:
            _active["reported"] = True
            dream.report_outcome(run_id, native.OUTCOME_ERROR, "Dream Python Editor internal error", None)
            try:
                dream.capture_end(run_id)
            except Exception:  # noqa: BLE001
                pass


def shutdown():
    """``_shutdown`` (§3.8): forget prepared requests and the active run. Best effort."""
    _PREPARED.clear()
    _active.update(run_id=0, run_number=0, reported=False, swaps=None)


# ---- tracebacks without runner frames (§5.3.7) ----------------------------------------------------------------

def _filtered_traceback(tb, prep):
    """The traceback with every frame under ``dream_editor/_impl`` dropped and line-0 statement frames fixed."""
    entries = []
    changed = False
    current = tb
    while current is not None:
        frame = current.tb_frame
        filename = frame.f_code.co_filename
        if _is_dream_file(filename)[0]:
            changed = True
        else:
            lineno = current.tb_lineno
            if (lineno is None or lineno <= 0) and prep is not None and filename == prep.filename:
                # lasti -1: no column positions, so the frame shows the statement's first line and no carets.
                entries.append((frame, -1, _frame_line(prep, frame, lineno)))
                changed = True
            else:
                entries.append((frame, current.tb_lasti, lineno))
        current = current.tb_next
    if not changed:
        return tb
    rebuilt = None
    for frame, lasti, lineno in reversed(entries):
        rebuilt = types.TracebackType(rebuilt, frame, lasti, lineno)
    return rebuilt


def _exception_chain(exc):
    seen = set()
    order = []
    pending = [exc]
    while pending:
        current = pending.pop()
        if current is None or id(current) in seen:
            continue
        seen.add(id(current))
        order.append(current)
        pending.append(current.__cause__)
        pending.append(current.__context__)
        pending.extend(getattr(current, "exceptions", ()) if isinstance(current, BaseExceptionGroup) else ())
    return order


def format_exception(exc, prep=None):
    """CPython's traceback text, carets included, with runner frames removed from every exception in the chain."""
    import traceback    # only failed runs pay for it (§3.4 boot budget)

    originals = []
    try:
        for current in _exception_chain(exc):
            tb = current.__traceback__
            filtered = _filtered_traceback(tb, prep)
            if filtered is not tb:
                originals.append((current, tb))
                current.__traceback__ = filtered
        report = traceback.TracebackException(type(exc), exc, exc.__traceback__, compact=True)
        return "".join(report.format())
    finally:
        for current, tb in originals:
            current.__traceback__ = tb


# ---- REPL completeness (§6.5) ----------------------------------------------------------------------------------

def _compile_command(source, filename):
    """``codeop.compile_command(source, filename, "exec")`` without ``catch_warnings``.

    codeop wraps its probes in ``catch_warnings()``, which mutates the filters and resets every once-per-location
    registry (§5.3.2); warning display is silenced through the two display slots instead.
    """
    for line in source.split("\n"):
        line = line.strip()
        if line and line[0] != "#":
            break
    else:
        source = "pass"
    probe_flags = compat.PYCF_ALLOW_INCOMPLETE_INPUT | compat.PYCF_DONT_IMPLY_DEDENT
    with streams.silenced_warning_display():
        try:
            compile(source, filename, "exec", probe_flags, dont_inherit=True)
        except SyntaxError:
            try:
                compile(source + "\n", filename, "exec", probe_flags, dont_inherit=True)
                return None
            except SyntaxError as exc:
                if "incomplete input" in str(exc):
                    return None
        return compile(source, filename, "exec", 0, dont_inherit=True)


def classify_input(source, last_logical_indented):
    """Helper (§12.1.3): ``"complete"``, ``"incomplete"`` or ``"invalid"`` for a REPL Enter. Never executes code."""
    if not isinstance(source, str):
        raise TypeError("classify_input(source, last_logical_indented): source must be str")
    if last_logical_indented and not source.endswith("\n"):
        return "incomplete"     # a blank Enter runs an indented block (REPL-01)
    try:
        code = _compile_command(source, "<dream:REPL>")
    except (SyntaxError, OverflowError, ValueError):
        return "invalid"
    except Exception:  # noqa: BLE001 - any other error is reported by the run
        return "complete"
    return "incomplete" if code is None else "complete"


# ---- tiers 2 and 5 -----------------------------------------------------------------------------------------------

class RunResult:
    """What ``execute_for_test`` saw for one request."""

    def __init__(self, run_id, prepared, outcome, detail, location, lines, events, ns):
        self.run_id = run_id
        self.prepared = prepared            # prepare()'s result
        self.outcome = outcome              # native.OUTCOME_*
        self.detail = detail
        self.location = location
        self.lines = lines                  # [(stream, text)] written for this run, in order
        self.events = events                # null _dream events of this run: (kind, payload)
        self.ns = ns

    @property
    def error(self):
        return None if self.prepared.get("ok") else self.prepared.get("error")

    def text(self, *stream_ids):
        return "".join(text for stream, text in self.lines if not stream_ids or stream in stream_ids)

    @property
    def stdout(self):
        return self.text(native.STREAM_STDOUT)

    @property
    def stderr(self):
        return self.text(native.STREAM_STDERR)

    @property
    def errors(self):
        return self.text(native.STREAM_ERROR)

    @property
    def results(self):
        return self.text(native.STREAM_RESULT)

    @property
    def notices(self):
        return self.text(native.STREAM_NOTICE)

    def __repr__(self):
        return f"RunResult(run_id={self.run_id}, outcome={self.outcome}, detail={self.detail!r})"


_test_serial = itertools.count(1)


def execute_for_test(request, dream=None, *, before_run=None):
    """Tiers 2 and 5 (§5.3.11): prepare and run one request against a null ``_dream`` as FDreamRunService would.

    ``before_run(dream, run_id)`` runs after the run is made active and before ``_run`` (to request a Stop first).
    """
    if dream is None:
        dream = native.get()
    previous = native._override
    native.set_for_test(dream)
    try:
        request = dict(request)
        run_id = request.get("run_id") or next(_test_serial)
        request["run_id"] = run_id
        request.setdefault("run_number", run_id & 0xFFFFFFFF)
        cls = task.cancel_class()
        if cls is not None:
            dream.register_cancel_class(cls)
        lines_before = len(getattr(dream, "test_lines", ()))
        events_before = len(getattr(dream, "test_events", ()))
        prepared = prepare(run_id, request)
        ns = None
        if not prepared["ok"]:
            outcome, detail, location = native.OUTCOME_NOT_EXECUTED, prepared["error"]["type"], None
        else:
            ns = _PREPARED[run_id].ns
            if hasattr(dream, "test_begin_run"):
                dream.test_begin_run(run_id, route=request.get("route") or "buffer")
                if prepared["statements"] is not None:
                    dream.test_set_statement_count(run_id, len(prepared["statements"]))
            if before_run is not None:
                before_run(dream, run_id)
            sys.modules["dream_editor"]._run(run_id)
            if hasattr(dream, "test_end_run"):
                dream.test_end_run(run_id)
            outcome, detail, location = getattr(dream, "test_outcomes", {}).get(run_id, (None, None, None))
        lines = [(stream, text) for stream, rid, text, _bg in getattr(dream, "test_lines", ())[lines_before:]
                 if rid == run_id]
        events = [(kind, payload) for kind, rid, payload in getattr(dream, "test_events", ())[events_before:]
                  if rid == run_id]
        return RunResult(run_id, prepared, outcome, detail, location, lines, events, ns)
    finally:
        native._override = previous
