# SPDX-License-Identifier: Apache-2.0
"""The in-editor debugger engine: ``dream_editor._impl.debugger`` (TDD §8.1-§8.5, §8.8; DBG-01..DBG-06, DBG-08).

(debug, PO-06 F5) A bdb engine for Debug runs only, served to the Paused window through one DAP-shaped entry,
``handle_request(request_json) -> response_json``. The runner calls ``begin_run(cfg)`` after its swaps and before user
code, ``on_uncaught(exc)`` in its handler after ``user_code_done`` and ``end_run(run_id)`` before it restores the swaps
(§8.1.5). The pause itself is ``_dream.pause(run_id, stop_json)``: C++ opens the Paused window's blocking modal loop with
the GIL released, answers the window's requests from clean points by calling ``handle_request`` and returns the resume
command (§8.3).

Zero cost when not debugging (DBG-06, T-DBG-002): this module is imported only by a Debug run, and a Debug run installs
a trace function only while a verified breakpoint exists, stepping is active or ``breakpoint()`` was called. The trace is
a call filter (``SettraceBackend``, §8.1.3): code with no breakpoint line and no reason to step gets no local tracer, so
CPython raises no line events in it.

Reading variables runs no user code (§8.8, VAR-01): types are compared by identity, names and namespaces are read through
``type``'s and ``object``'s own descriptors, containers (subclasses included) through the built-in types' C methods.

Works under plain CPython with the null ``_dream`` of ``native`` (tier 2), whose ``pause`` calls a test handler.
"""

import bdb
import json
import linecache
import os
import sys
import threading
import time
import types

from . import native

__all__ = ["DebugSession", "DreamBdb", "SettraceBackend", "BreakpointTable", "begin_run", "end_run", "on_uncaught",
           "breakpointhook", "handle_request", "active_session", "saferepr", "executable_lines",
           "REFUSAL_NONE", "REFUSAL_TEXT"]

# The game thread is the thread that imports this module: a Debug run's first statement, on the game thread (§8.4 rule 2).
GAME_THREAD_IDENT = threading.get_ident()

# EDreamPauseRefusal (Source/DreamEditor/Private/Debug/DreamDebugger.h, TDD §8.1.5), in order.
REFUSAL_NONE = 0
REFUSAL_NOT_DEBUG_RUN = 1
REFUSAL_NOT_GAME_THREAD = 2
REFUSAL_SLATE_TICKING = 3
REFUSAL_ENGINE_CALLBACK = 4
REFUSAL_ENGINE_BUSY = 5
REFUSAL_PIE_TRANSITION = 6
REFUSAL_UNATTENDED = 7
REFUSAL_ALREADY_PAUSED = 8
REFUSAL_WINDOW_FAILED = 9
REFUSAL_POLICY_DISABLED = 10
REFUSAL_INSIDE_PUMP = 11

# The §8.4 notice texts ("Breakpoint at importer.py:88 skipped: <text>").
REFUSAL_TEXT = {
    REFUSAL_NOT_DEBUG_RUN: "this is not a Debug run",
    REFUSAL_NOT_GAME_THREAD: "on thread '{thread}': 1.0 pauses only on the game thread",
    REFUSAL_SLATE_TICKING: "inside a Slate tick",
    REFUSAL_ENGINE_CALLBACK: "inside a Python Slate or ticker callback",
    REFUSAL_ENGINE_BUSY: "the engine is saving, collecting garbage or loading",
    REFUSAL_PIE_TRANSITION: "PIE is starting or stopping",
    REFUSAL_UNATTENDED: "unattended session",
    REFUSAL_ALREADY_PAUSED: "the debugger is already paused",
    REFUSAL_WINDOW_FAILED: "the Paused window could not open",
    REFUSAL_POLICY_DISABLED: "the debugger is disabled by policy",
    REFUSAL_INSIDE_PUMP: "inside an editor update during the run",
}

_PAGE = 100                     # D30: children in pages of 100
_VALUE_LIMIT = 200              # characters of a value text (§8.8)
_BUDGET_NS = 5_000_000          # 5 ms of game-thread time per variables request (§8.8, proposed)
_MOVE_LIMIT = 5                 # §8.2.2: a breakpoint moves to the next executable line within 5 lines
_SAFEREPR_ITEMS = 20
_SAFEREPR_DEPTH = 2
_INT_REPR_BITS = 13000          # repr of a larger int raises ValueError (3.11's 4300-digit limit)

_MISSING = object()

_IMPL_DIR = os.path.normcase(os.path.dirname(os.path.abspath(__file__)))
_PACKAGE_DIR = os.path.normcase(os.path.dirname(_IMPL_DIR))
_RUNNER_FILE = os.path.normcase(os.path.join(os.path.dirname(os.path.abspath(__file__)), "runner.py"))
_LIB_DIR = os.path.normcase(os.path.join(sys.base_prefix, "Lib")) + os.sep
_EXACT_PRIMITIVES = (int, float, bool, type(None), str, bytes)
_EXACT_CONTAINERS = (list, tuple, dict, set, frozenset)
_FUNCTION_TYPES = (types.FunctionType, types.BuiltinFunctionType, types.MethodType)

_NAME_OF_TYPE = type.__dict__["__name__"]
_QUALNAME_OF_TYPE = type.__dict__["__qualname__"]
_TYPE_DICT = type.__dict__["__dict__"]
_TYPE_MRO = type.__dict__["__mro__"]
_MODULE_DICT = types.ModuleType.__dict__["__dict__"]
_OBJECT_REPR = object.__repr__
_GETSET_TYPE = type(types.FunctionType.__dict__["__code__"])
_MEMBER_TYPE = type(types.FunctionType.__dict__["__globals__"])


# ---- safe type helpers (§8.3.6, review q1 m1) ---------------------------------------------------------------------

def _is_one_of(cls, classes):
    """``cls in classes`` by identity: ``in`` would call a user metaclass's ``__eq__``."""
    for candidate in classes:
        if cls is candidate:
            return True
    return False


def _type_is(value, classes):
    """``isinstance`` by the exact type's MRO only: never looks up a user ``__class__``."""
    cls = type(value)
    if not isinstance(classes, tuple):
        classes = (classes,)
    for candidate in classes:
        if type.__subclasscheck__(candidate, cls):
            return True
    return False


def _class_dict(cls):
    """A class's own namespace through type's descriptor, never a metaclass ``__getattribute__``."""
    try:
        found = _TYPE_DICT.__get__(cls)
        return found if found is not None else {}
    except Exception:  # noqa: BLE001
        return {}


def _safe_mro(cls):
    try:
        mro = _TYPE_MRO.__get__(cls)
        return mro if type(mro) is tuple else (cls,)
    except Exception:  # noqa: BLE001
        return (cls,)


def _safe_type_name(value):
    """The type's name from type's own descriptor: never a metaclass property."""
    try:
        return _NAME_OF_TYPE.__get__(type(value))
    except Exception:  # noqa: BLE001
        return "object"


def _safe_qualname(cls):
    try:
        return _QUALNAME_OF_TYPE.__get__(cls)
    except Exception:  # noqa: BLE001
        return "object"


def _primitive_repr(value, limit):
    """repr of an exact primitive, bounded; a huge int shows its size instead of raising (review q1 m2)."""
    if type(value) is int and value.bit_length() > _INT_REPR_BITS:
        return f"<int, {value.bit_length()} bits>"
    try:
        text = repr(value)
    except Exception as exc:  # noqa: BLE001
        return f"<{_safe_type_name(value)}: {type(exc).__name__}>"
    return text if len(text) <= limit else text[:limit - 1] + "…"


def _container_base(value):
    """The built-in container type a value is or subclasses (its C methods are used, never overrides), or None."""
    cls = type(value)
    for base in _EXACT_CONTAINERS:
        if cls is base or type.__subclasscheck__(base, cls):
            return base
    return None


# ---- paths and frames --------------------------------------------------------------------------------------------

def _is_pseudo(filename):
    return filename.startswith("<") and filename.endswith(">")


_canonic_cache = {}


def canonic(filename):
    """The breakpoint key of a filename (§8.2.1): ``<dream:…>`` names unchanged, real paths case-normalised real paths."""
    if not isinstance(filename, str):
        return ""
    hit = _canonic_cache.get(filename)
    if hit is None:
        hit = filename if _is_pseudo(filename) else os.path.normcase(os.path.realpath(filename))
        if len(_canonic_cache) < 8192:
            _canonic_cache[filename] = hit
    return hit


_dream_file_cache = {}


def _is_dream_file(filename):
    """Frames of the dream_editor package (the runner, ``_run``, the public helpers) are never user frames (§8.1.6)."""
    hit = _dream_file_cache.get(filename)
    if hit is None:
        if _is_pseudo(filename):
            hit = filename.startswith("<frozen ")
        else:
            directory = os.path.normcase(os.path.dirname(os.path.abspath(filename)))
            hit = directory == _PACKAGE_DIR or directory.startswith(_PACKAGE_DIR + os.sep)
        if len(_dream_file_cache) < 8192:
            _dream_file_cache[filename] = hit
    return hit


_library_cache = {}


def _is_library_file(filename):
    """just_my_code (§8.1.6): the engine's Lib directory, every site-packages and the plugin's vendor folder."""
    hit = _library_cache.get(filename)
    if hit is None:
        if _is_pseudo(filename):
            hit = False
        else:
            path = os.path.normcase(os.path.abspath(filename))
            hit = (path.startswith(_LIB_DIR) or (os.sep + "site-packages" + os.sep) in path
                   or (os.sep + "_dream_vendor" + os.sep) in path)
        if len(_library_cache) < 8192:
            _library_cache[filename] = hit
    return hit


def _short_name(filename):
    if _is_pseudo(filename):
        return filename
    return os.path.basename(filename)


def _unreal_types():
    """``(Object, StructBase)`` of the loaded ``unreal`` module, or Nones: never imports it (tier 2 has none)."""
    unreal = sys.modules.get("unreal")
    if unreal is None:
        return None, None
    try:
        unreal_dict = _MODULE_DICT.__get__(unreal)
    except Exception:  # noqa: BLE001
        return None, None
    obj = unreal_dict.get("Object")
    struct = unreal_dict.get("StructBase")
    return (obj if isinstance(obj, type) else None), (struct if isinstance(struct, type) else None)


def _is_unreal_object(value):
    obj_type, _struct = _unreal_types()
    return obj_type is not None and type.__subclasscheck__(obj_type, type(value))


def _is_unreal_struct(value):
    _obj, struct_type = _unreal_types()
    return struct_type is not None and type.__subclasscheck__(struct_type, type(value))


def _unreal_path(value):
    obj_type, _struct = _unreal_types()
    try:
        # Reflection reads through the class's own method (PyWrapperObject.cpp: get_path_name), never a user override.
        return str(_class_dict(obj_type)["get_path_name"](value)) if obj_type is not None else ""
    except Exception:  # noqa: BLE001
        return ""


# ---- saferepr (§8.3.6, RV-21) ------------------------------------------------------------------------------------

def saferepr(value, deadline_ns=None, limit=_VALUE_LIMIT):
    """A bounded repr that runs no user code: it recurses only into exact list, tuple, dict, set and frozenset (depth 2,
    20 items per level), calls ``repr`` only on exact int, float, bool, None, str and bytes, shows an unreal object as its
    class and path, and any other value as ``<TypeName>``. At most ``limit`` characters."""
    if deadline_ns is None:
        deadline_ns = time.perf_counter_ns() + _BUDGET_NS
    parts = []
    size = [0]

    def emit(text):
        parts.append(text)
        size[0] += len(text)

    def walk(item, depth):
        if size[0] > limit or time.perf_counter_ns() > deadline_ns:
            emit("…")
            return
        cls = type(item)
        if _is_one_of(cls, _EXACT_PRIMITIVES):
            emit(_primitive_repr(item, limit))
            return
        if _is_one_of(cls, _EXACT_CONTAINERS):
            if depth >= _SAFEREPR_DEPTH:
                emit(f"{_safe_type_name(item)} ({len(item)})")
                return
            if cls is dict:
                emit("{")
                for index, (key, val) in enumerate(item.items()):
                    if index >= _SAFEREPR_ITEMS:
                        emit(", …")
                        break
                    if index:
                        emit(", ")
                    walk(key, depth + 1)
                    emit(": ")
                    walk(val, depth + 1)
                emit("}")
                return
            if cls is list:
                opener, closer = "[", "]"
            elif cls is tuple:
                opener, closer = "(", ")"
            elif cls is set:
                if not item:
                    emit("set()")
                    return
                opener, closer = "{", "}"
            else:
                opener, closer = "frozenset({", "})"
            emit(opener)
            for index, element in enumerate(item):
                if index >= _SAFEREPR_ITEMS:
                    emit(", …")
                    break
                if index:
                    emit(", ")
                walk(element, depth + 1)
            if cls is tuple and len(item) == 1:
                emit(",")
            emit(closer)
            return
        if _is_unreal_object(item):
            emit(f"<{_safe_type_name(item)} '{_unreal_path(item)}'>")
            return
        emit(f"<{_safe_type_name(item)}>")

    walk(value, 0)
    text = "".join(parts)
    if len(text) > limit:
        text = text[:limit - 1] + "…"
    return text


# ---- breakpoints (§8.2) ------------------------------------------------------------------------------------------

def executable_lines(text, filename):
    """The executable lines of a source (``co_lines`` of every nested code object, §8.2.2), or raises SyntaxError."""
    code = compile(text, filename, "exec", dont_inherit=True)
    lines = set()
    pending = [code]
    while pending:
        current = pending.pop()
        for _start, _end, line in current.co_lines():
            if line:
                lines.add(line)
        for const in current.co_consts:
            if isinstance(const, types.CodeType):
                pending.append(const)
    return lines


class BreakpointTable:
    """Breakpoints by source key (§8.2.1), verified against the source's current text (§8.2.2)."""

    def __init__(self):
        self._requested = {}    # canonical key -> [spec, ...]
        self._names = {}        # canonical key -> the key as C++ sent it (linecache name)
        self._verified = {}     # canonical key -> {line: spec}
        self.results = {}       # spec id -> {"id", "verified", "line", "message"}
        self.generation = 0

    def __bool__(self):
        return any(self._verified.values())

    def load(self, specs):
        grouped = {}
        for spec in specs or ():
            if isinstance(spec, dict) and isinstance(spec.get("source_key"), str):
                grouped.setdefault(spec["source_key"], []).append(spec)
        for key, items in grouped.items():
            self.set_source(key, items)

    def set_source(self, source_key, specs):
        """Replaces every breakpoint of one source; returns the verification results in order (DAP setBreakpoints)."""
        key = canonic(source_key)
        self._names[key] = source_key
        clean = []
        for index, spec in enumerate(specs or ()):
            line = spec.get("line")
            if isinstance(line, bool) or not isinstance(line, int) or line <= 0:
                continue
            clean.append({
                "id": spec.get("id") if isinstance(spec.get("id"), int) else (self.generation + 1) * 10000 + index,
                "line": line,
                "enabled": spec.get("enabled", True) is not False,
                "condition": spec.get("condition") or None,
            })
        self._requested[key] = clean
        results = self._verify(key)
        self.generation += 1
        return results

    def _verify(self, key):
        specs = self._requested.get(key, [])
        name = self._names.get(key, key)
        lines = None
        reason = ""
        try:
            text = "".join(linecache.getlines(name))
            if text:
                lines = executable_lines(text, name)
        except (SyntaxError, ValueError, OverflowError, RecursionError) as exc:
            # (debug choice) A selection or line run of a buffer that does not compile as a whole still stops: its lines
            # are kept as given (the run's own compile decides), with the reason in the result.
            reason = f"Not checked: {type(exc).__name__}"
            lines = None
        verified = {}
        results = []
        for spec in specs:
            result = {"id": spec["id"], "verified": True, "line": spec["line"], "message": reason}
            if not spec["enabled"]:
                result["verified"] = False
                result["message"] = "Disabled"
            elif lines is not None:
                target = next((candidate for candidate in range(spec["line"], spec["line"] + _MOVE_LIMIT + 1)
                               if candidate in lines), None)
                if target is None:
                    result["verified"] = False
                    result["message"] = "No code on this line"
                else:
                    result["line"] = target
            if result["verified"]:
                verified.setdefault(result["line"], spec)
            results.append(result)
            self.results[spec["id"]] = result
        self._verified[key] = verified
        return results

    def lines_in(self, key):
        return self._verified.get(key) or {}

    def spec_at(self, filename, line):
        lines = self._verified.get(canonic(filename))
        return lines.get(line) if lines else None

    def code_lines(self, code):
        """The verified breakpoint lines of one code object's own instructions (the call filter, §8.1.3)."""
        lines = self._verified.get(canonic(code.co_filename))
        if not lines:
            return frozenset()
        return frozenset(line for _start, _end, line in code.co_lines() if line and line in lines)

    def keys(self):
        return list(self._names.values())


# ---- the variable store (§8.8, D30) --------------------------------------------------------------------------------

class _VariableStore:
    """DAP ``variablesReference`` handles, valid for one stop epoch."""

    def __init__(self):
        self._refs = {}
        self._next = 1

    def add(self, provider):
        ref = self._next
        self._next += 1
        self._refs[ref] = provider
        return ref

    def get(self, ref):
        return self._refs.get(ref)

    def clear(self):
        self._refs.clear()


def _module_name(module):
    try:
        name = _MODULE_DICT.__get__(module).get("__name__")
        return name if type(name) is str else "?"
    except Exception:  # noqa: BLE001
        return "?"


def _function_name(function):
    for attr in ("__qualname__", "__name__"):
        try:
            name = object.__getattribute__(function, attr)
            if type(name) is str:
                return name
        except Exception:  # noqa: BLE001
            continue
    return "?"


def _value_text(value):
    cls = type(value)
    if _is_one_of(cls, _EXACT_PRIMITIVES):
        return _primitive_repr(value, _VALUE_LIMIT)
    if _is_one_of(cls, _EXACT_CONTAINERS):
        return f"{_safe_type_name(value)} ({len(value)})"
    if _is_unreal_object(value):
        return f"{_safe_type_name(value)} '{_unreal_path(value)}'"
    if _is_unreal_struct(value):
        return f"<{_safe_type_name(value)}>"
    if cls is types.ModuleType:
        return f"<module {_module_name(value)}>"
    if _is_one_of(cls, _FUNCTION_TYPES):
        return f"<{_safe_type_name(value)} {_function_name(value)}>"
    if _type_is(value, type):
        return f"<class {_safe_qualname(value)}>"
    try:
        # §8.8: object.__repr__, never the user's __repr__.
        return _OBJECT_REPR(value)
    except Exception:  # noqa: BLE001
        return f"<{_safe_type_name(value)}>"


def _instance_dict(value):
    """The instance ``__dict__`` through object's own lookup, or None (never a user ``__getattr__``)."""
    try:
        cls = type(value)
        for base in _safe_mro(cls):
            descriptor = _class_dict(base).get("__dict__")
            if descriptor is not None:
                if type(descriptor) is _GETSET_TYPE:
                    found = descriptor.__get__(value, cls)
                    return found if type(found) is dict else None
                return None
    except Exception:  # noqa: BLE001
        return None
    return None


def _has_children(value):
    cls = type(value)
    if _is_one_of(cls, _EXACT_PRIMITIVES):
        return False
    base = _container_base(value)
    if base is not None:
        return base.__len__(value) > 0
    if _is_unreal_object(value):
        return True
    if _is_unreal_struct(value) or _is_one_of(cls, _FUNCTION_TYPES):
        return False
    if cls is types.ModuleType:
        return True
    if _type_is(value, type):
        return False
    if _instance_dict(value):
        return True
    for klass in _safe_mro(cls):
        if klass is object:
            continue
        for name, item in _class_dict(klass).items():
            if type(name) is str and not name.startswith("__") and _type_is(item, (property, _GETSET_TYPE, _MEMBER_TYPE)):
                return True
    return False


# ---- the tracer (§8.1.3, §8.5) -------------------------------------------------------------------------------------

class SettraceBackend:
    """The 3.11 trace backend (§8.1.3): ``sys.settrace`` with a call filter, compare-and-restore on uninstall."""

    def __init__(self, bdb_engine):
        self._bdb = bdb_engine
        self._installed = False
        self._previous = None
        self._fn = bdb_engine.on_call      # one bound method, so gettrace() can be compared by identity

    def install(self, session=None):
        if self._installed:
            return
        self._previous = sys.gettrace()
        sys.settrace(self._fn)
        self._installed = True

    def uninstall(self):
        """Compare-and-restore (D25): the previous tracer only when the slot still holds ours. True when it did."""
        if not self._installed:
            return True
        self._installed = False
        current = sys.gettrace()
        if current is self._fn or current is None:
            # None: CPython dropped ours when a trace callback raised (Stop at a line stop); the previous one comes back.
            sys.settrace(self._previous)
            self._previous = None
            return True
        self._previous = None
        return False

    def invalidate_code_interest(self):
        self._bdb.interest.clear()

    def set_stepping(self, mode, frame):
        pass

    def owns_slot(self):
        return self._installed and sys.gettrace() is self._fn

    @property
    def installed(self):
        return self._installed


class DreamBdb(bdb.Bdb):
    """bdb with Dream's frame rules (§8.1.6), the per-code call filter (§8.1.3) and no stop on return events (§8.5)."""

    def __init__(self, session):
        super().__init__(skip=None)
        self.session = session
        self.interest = {}          # code -> bool: a verified breakpoint in its own lines
        self._user_codes = {}       # code -> bool
        self.botframe = None
        self.stopframe = None
        self.returnframe = None
        self.stoplineno = -1
        self.quitting = False
        self.last_return = _MISSING
        self.backend = SettraceBackend(self)

    # -- frame rules --

    def is_user_code(self, code):
        hit = self._user_codes.get(code)
        if hit is None:
            filename = code.co_filename
            hit = not _is_dream_file(filename) and not (self.session.just_my_code and _is_library_file(filename))
            if len(self._user_codes) < 65536:
                self._user_codes[code] = hit
        return hit

    def code_interest(self, code):
        hit = self.interest.get(code)
        if hit is None:
            hit = bool(self.session.table.code_lines(code))
            self.interest[code] = hit
        return hit

    def stop_here(self, frame):
        if not self.is_user_code(frame.f_code):
            return False
        if frame is self.stopframe:
            if self.stoplineno == -1:
                return False
            return frame.f_lineno >= self.stoplineno
        return not self.stopframe

    def break_here(self, frame):
        return self.session.breakpoint_hit(frame) is not None

    def break_anywhere(self, frame):
        return self.code_interest(frame.f_code)

    def is_stepping(self):
        return self.stopframe is None or self.stoplineno != -1

    # -- the global trace: a call filter (§8.1.3) --

    def on_call(self, frame, event, arg):
        if event != "call":
            return None
        code = frame.f_code
        if self.stopframe is None and self.is_user_code(code):
            return self.trace_dispatch(frame, event, arg)
        if self.code_interest(code):
            return self.trace_dispatch(frame, event, arg)
        return None

    # -- dispatch --

    def dispatch_line(self, frame):
        hit = self.session.breakpoint_hit(frame)
        if self.stop_here(frame) or hit is not None:
            self.session.on_line(frame, hit)
        return self.trace_dispatch

    def dispatch_return(self, frame, arg):
        # §8.5: no stop on return events; Step out shows the value in Special. A frame being stepped over hands the step
        # to its caller (bdb's own rule), so a statement frame's return goes to the next statement's first line (RUN-27).
        if frame is self.returnframe or frame is self.stopframe:
            self.last_return = (frame.f_code.co_name, arg)
            if self.stopframe is frame and self.stoplineno != -1:
                self._set_stopinfo(None, None)
        if self.stopframe is None or frame is self.returnframe:
            self._trace_caller(frame)       # stepping goes on in the caller (bdb.set_step's issue #13183 rule)
        return self.trace_dispatch

    def dispatch_exception(self, frame, arg):
        return self.trace_dispatch      # raised-exception filters are Later (DBG-05); uncaught stops need no trace

    def user_call(self, frame, argument_list):
        pass

    def user_line(self, frame):
        pass

    def user_return(self, frame, return_value):
        pass

    def user_exception(self, frame, exc_info):
        pass

    # -- resume commands (§8.5) --

    def _trace_caller(self, frame):
        caller = frame.f_back
        if caller is not None and caller.f_trace is None and self.is_user_code(caller.f_code):
            caller.f_trace = self.trace_dispatch

    def resume_continue(self):
        self._set_stopinfo(self.botframe, None, -1)

    def resume_next(self, frame):
        self._set_stopinfo(frame, None)

    def resume_step(self):
        self._set_stopinfo(None, None)

    def resume_return(self, frame):
        self.last_return = _MISSING
        self._set_stopinfo(frame.f_back, frame)


# ---- the session --------------------------------------------------------------------------------------------------

class _Stop:
    __slots__ = ("reason", "description", "frames", "can_step", "exception")

    def __init__(self, reason, description, frames, can_step, exception=None):
        self.reason = reason
        self.description = description
        self.frames = frames            # [(frame, line)], innermost first, user frames only
        self.can_step = can_step
        self.exception = exception


class DebugSession:
    """One Debug run (§8.1.1): its breakpoints, its bdb engine, the current stop and the variable handles."""

    def __init__(self, cfg):
        cfg = cfg or {}
        self.run_id = int(cfg.get("run_id") or 0)
        self.break_on_uncaught = cfg.get("break_on_uncaught", True) is not False
        self.just_my_code = cfg.get("just_my_code", True) is not False
        self.stop_on_entry = bool(cfg.get("stop_on_entry", False))
        self.table = BreakpointTable()
        self.table.load(cfg.get("breakpoints"))
        self.bdb = DreamBdb(self)
        self.stop = None
        self.epoch = 0
        self.store = _VariableStore()
        self.pending_reason = None
        self.notices = set()
        self.stops = 0
        self.ended = False

    # -- trace slot --

    def needs_trace(self):
        return bool(self.table) or self.bdb.is_stepping()

    def install(self):
        self.bdb.backend.install(self)

    def uninstall(self):
        return self.bdb.backend.uninstall()

    def _trace_stack(self, frames):
        """Gives every user frame of a stop a local tracer, so stepping and later breakpoints reach them (§8.5)."""
        dispatch = self.bdb.trace_dispatch
        for frame, _line in frames:
            if frame.f_trace is None:
                frame.f_trace = dispatch

    def _untrace_stack(self, frames):
        for frame, _line in frames:
            if not self.bdb.code_interest(frame.f_code):
                frame.f_trace = None

    # -- breakpoints --

    def breakpoint_hit(self, frame):
        """The breakpoint spec at the frame's line when it pauses (a condition that is falsy does not), else None."""
        spec = self.table.spec_at(frame.f_code.co_filename, frame.f_lineno)
        if spec is None:
            return None
        condition = spec.get("condition")
        if not condition:
            return spec
        try:
            # §8.2.3: eval on a copy of the locals; an Exception pauses with the reason, ScriptCancelled propagates.
            if eval(condition, frame.f_globals, dict(frame.f_locals)):  # noqa: S307 - the user's own condition
                return spec
            return None
        except Exception as exc:  # noqa: BLE001
            return dict(spec, condition_error=f"Condition raised {type(exc).__name__}: {exc}")

    def set_breakpoints(self, source_key, specs):
        results = self.table.set_source(source_key, specs)
        self.bdb.backend.invalidate_code_interest()
        if self.table and not self.bdb.backend.installed:
            self.install()
        if self.stop is not None:
            self._trace_stack(self.stop.frames)
        return results

    # -- pausing (§8.3, §8.4) --

    def notice(self, frame, line, reason_code, kind="Breakpoint"):
        key = ((frame.f_code.co_filename, line), reason_code)
        if key in self.notices:
            return
        self.notices.add(key)
        text = REFUSAL_TEXT.get(reason_code, "the pause was refused").format(thread=threading.current_thread().name)
        message = f"{kind} at {_short_name(frame.f_code.co_filename)}:{line} skipped: {text}\n"
        try:
            dream = native.get()
            dream.write(dream.STREAM_DEBUGGER, self.run_id, message)
        except Exception:  # noqa: BLE001 - a notice never breaks the run
            pass

    def gate(self, frame, line):
        """``PauseGate.check`` (§8.4): Python's rules first, then ``_dream.pause_context``. The first failing rule wins."""
        dream = native.get()
        if self.ended or self.run_id == 0 or dream.current_run_id() != self.run_id:
            return REFUSAL_NOT_DEBUG_RUN
        if threading.get_ident() != GAME_THREAD_IDENT:
            return REFUSAL_NOT_GAME_THREAD
        if self.stop is not None:
            return REFUSAL_ALREADY_PAUSED
        return int(dream.pause_context(self.run_id))

    def collect_frames(self, frame):
        """User frames from ``frame`` outward, innermost first, stopping at the runner (§8.1.6)."""
        frames = []
        current = frame
        while current is not None and current is not self.bdb.botframe:
            filename = current.f_code.co_filename
            if os.path.normcase(filename) == _RUNNER_FILE:
                break
            if not _is_dream_file(filename):
                frames.append((current, current.f_lineno))
            current = current.f_back
        return frames

    def pause(self, stop, kind="Breakpoint"):
        """Pauses at ``stop`` (frames, reason). Returns the resume command, or None when the pause was refused."""
        frame, line = stop.frames[0]
        code = self.gate(frame, line)
        if code != REFUSAL_NONE:
            if code != REFUSAL_NOT_DEBUG_RUN:
                self.notice(frame, line, code, kind)
            return None
        self.epoch += 1
        self.store.clear()
        self.stop = stop
        self.stops += 1
        payload = {
            "run_id": self.run_id,
            "reason": stop.reason,
            "description": stop.description,
            "source": frame.f_code.co_filename,
            "line": line,
            "canStep": stop.can_step,
            "epoch": self.epoch,
            "stackFrames": self._stack_frames(stop),
        }
        try:
            command, args = native.get().pause(self.run_id, json.dumps(payload))
        finally:
            self.stop = None
            self.store.clear()
        if command == "refused":
            try:
                refusal = int(args)
            except (TypeError, ValueError):
                refusal = REFUSAL_WINDOW_FAILED
            if refusal != REFUSAL_NOT_DEBUG_RUN:
                self.notice(frame, line, refusal, kind)
            return None
        return command

    def _stack_frames(self, stop):
        frames = []
        for index, (frame, line) in enumerate(stop.frames):
            frames.append({"id": index + 1, "name": frame.f_code.co_name, "source": frame.f_code.co_filename,
                           "line": line})
        return frames

    def on_line(self, frame, hit):
        """A line event that stops (§8.5): pause, then apply the resume command."""
        if hit is not None:
            reason = "breakpoint"
            description = hit.get("condition_error", "")
        else:
            reason = self.pending_reason or "step"
            description = ""
        self.pending_reason = None
        frames = self.collect_frames(frame)
        if not frames:
            return
        self._trace_stack(frames)
        stop = _Stop(reason, description, frames, can_step=True)
        command = self.pause(stop)
        self.apply(command, frame, frames)

    def apply(self, command, frame, frames):
        engine = self.bdb
        if command == "next":
            engine.resume_next(frame)
        elif command == "stepIn":
            engine.resume_step()
        elif command == "stepOut":
            engine.resume_return(frame)
        elif command == "terminate":
            # C++ raises ScriptCancelled from _dream.pause at a line stop; this is the fallback when it returned instead.
            from . import task
            raise task.new_cancelled()
        elif command is None:
            # A refused pause keeps the current mode (stepping goes on); the notice is written once per location.
            return
        else:
            engine.resume_continue()
        if not self.needs_trace():
            self._untrace_stack(frames)
            self.uninstall()
        elif not engine.backend.installed:
            self.install()

    def set_trace_at(self, frame):
        """``breakpoint()`` in a Debug run (§8.2.4): install the trace if needed and pause at the caller's next line."""
        dispatch = self.bdb.trace_dispatch
        current = frame
        while current is not None and current is not self.bdb.botframe:
            if os.path.normcase(current.f_code.co_filename) == _RUNNER_FILE:
                break
            if self.bdb.is_user_code(current.f_code) and current.f_trace is None:
                current.f_trace = dispatch
            current = current.f_back
        self.pending_reason = "breakpoint()"
        self.bdb.resume_step()
        self.install()

    def inspect_exception(self, exc):
        """The uncaught-exception stop (§8.2.5): inspection only, on the innermost user frame of the traceback."""
        entries = []
        tb = exc.__traceback__
        while tb is not None:
            if not _is_dream_file(tb.tb_frame.f_code.co_filename):
                entries.append((tb.tb_frame, tb.tb_lineno if tb.tb_lineno and tb.tb_lineno > 0 else tb.tb_frame.f_lineno))
            tb = tb.tb_next
        if not entries:
            return None     # the innermost frame is ours: an internal error never pauses (§8.2.5)
        entries.reverse()
        stop = _Stop("exception", _exception_text(exc), entries, can_step=False, exception=exc)
        return self.pause(stop, kind="Exception stop")

    # -- requests (§8.1.2) --

    def frame_at(self, frame_id):
        if self.stop is None:
            raise _RequestError("not paused")
        index = int(frame_id) - 1
        if not 0 <= index < len(self.stop.frames):
            raise _RequestError(f"no frame {frame_id}")
        return self.stop.frames[index]


def _exception_text(exc):
    name = _safe_qualname(type(exc))
    try:
        text = str(exc)
    except BaseException:  # noqa: BLE001
        text = ""
    first = text.splitlines()[0].rstrip() if text.strip() else ""
    detail = f"{name}: {first}" if first else name
    return detail if len(detail) <= _VALUE_LIMIT else detail[:_VALUE_LIMIT - 1] + "…"


# ---- module state and the runner's calls (§8.1.5) ---------------------------------------------------------------

_session = None


def active_session():
    return _session


def _parse_config(cfg):
    if isinstance(cfg, str):
        try:
            cfg = json.loads(cfg) if cfg else {}
        except ValueError:
            cfg = {}
    return dict(cfg) if isinstance(cfg, dict) else {}


def begin_run(cfg, run_id=None):
    """After stream setup, before user code (§8.1.5). Installs the trace only when a breakpoint verified (DBG-06)."""
    global _session
    if _session is not None:
        end_run(_session.run_id)
    cfg = _parse_config(cfg)
    if run_id is not None:
        cfg["run_id"] = run_id
    session = DebugSession(cfg)
    session.bdb.botframe = sys._getframe(1)
    if session.stop_on_entry:
        session.pending_reason = "entry"
        session.bdb.resume_step()
        session.install()
    elif session.table:
        session.bdb.resume_continue()
        session.install()
    else:
        session.bdb.resume_continue()
    _session = session
    return session


def end_run(run_id):
    """Compare-and-restore every hook (§8.1.5); a replaced tracer is left in place with one notice."""
    global _session
    session = _session
    if session is None or (run_id and session.run_id != run_id):
        return
    _session = None
    session.ended = True
    if not session.uninstall():
        try:
            dream = native.get()
            dream.write(dream.STREAM_NOTICE, session.run_id,
                       "sys.settrace was replaced during the Debug run; Dream Python Editor left the new tracer in place.\n")
        except Exception:  # noqa: BLE001
            pass
    session.store.clear()


def _is_cancel(exc):
    from . import task
    cls = task.cancel_class()
    return cls is not None and isinstance(exc, cls)


def on_uncaught(exc):
    """In the runner's handler after ``user_code_done`` (RT-06): the uncaught-exception stop (§8.2.5, DBG-05).

    Returns True when the user pressed Stop at the stop (the run ends cancelled), else False (Continue: it ends error)."""
    session = _session
    if session is None or not session.break_on_uncaught or session.ended:
        return False
    if _is_cancel(exc):
        return False
    if isinstance(exc, SystemExit) and (exc.code is None or exc.code == 0):
        return False
    session.uninstall()     # §8.2.5: on_uncaught is no trace callback, so the trace goes first (§8.3.2)
    command = session.inspect_exception(exc)
    return command == "terminate"


def breakpointhook(*args, **kwargs):
    """The run-scoped ``sys.breakpointhook`` of a Debug run (§8.2.4)."""
    session = _session
    if session is None or session.ended:
        return None
    session.set_trace_at(sys._getframe(1))
    return None


# ---- handle_request (§8.1.2) --------------------------------------------------------------------------------------

class _RequestError(Exception):
    pass


def _node(store, name, value, kind="local"):
    node = {
        "name": name,
        "type": _safe_type_name(value),
        "value": _value_text(value),
        "variablesReference": 0,
        "kind": kind,
        "badges": [],
    }
    base = _container_base(value)
    if base is not None:
        node["size"] = base.__len__(value)
    if _is_unreal_object(value):
        node["badges"].append("unreal")
    if _has_children(value):
        node["variablesReference"] = store.add(("value", value))
    return node


def _leaf(name, value_text, type_name="", kind="property", badges=None):
    return {"name": name, "type": type_name, "value": value_text, "variablesReference": 0, "kind": kind,
            "badges": list(badges or [])}


def _more(store, provider, start):
    return {"name": "…more", "type": "", "value": "", "kind": "special", "badges": [],
            "variablesReference": store.add(("from", provider, start))}


def _dict_children(store, mapping, start, count, deadline, kind, hide_dunder=False):
    """Children of a dict (subclasses through dict's own items), paged, with a "…more" node at the budget or page end."""
    nodes = []
    shown = 0
    for key, value in list(dict.items(mapping)):
        if hide_dunder and type(key) is str and key.startswith("__") and key.endswith("__"):
            continue
        if shown < start:
            shown += 1
            continue
        if len(nodes) >= count or time.perf_counter_ns() > deadline:
            nodes.append(_more(store, ("dict", mapping, kind, hide_dunder), shown))
            return nodes
        name = key if type(key) is str else saferepr(key, deadline)
        nodes.append(_node(store, name, value, kind))
        shown += 1
    return nodes


def _children(store, provider, start, count, deadline):
    tag = provider[0]
    if tag == "from":
        return _children(store, provider[1], provider[2] + start, count, deadline)
    if tag == "scope_locals":
        frame = provider[1]
        if frame.f_locals is frame.f_globals:
            # A module-level frame (a statement of a Session run): its locals are the namespace (D8).
            return _dict_children(store, frame.f_globals, start, count, deadline, "local", hide_dunder=True)
        local_map = frame.f_locals
        if type(local_map) is not dict:
            # A class body's __prepare__ mapping: iterating it could run user code (review q1 m1).
            return [_leaf("(locals)", f"<{_safe_type_name(local_map)}: not read>", "", "special")]
        return _dict_children(store, dict.copy(local_map), start, count, deadline, "local")
    if tag == "scope_globals":
        frame = provider[1]
        return _dict_children(store, frame.f_globals, start, count, deadline, "global", hide_dunder=True)
    if tag == "scope_special":
        return [_node(store, name, value, "special") for name, value in provider[1]]
    if tag == "dict":
        return _dict_children(store, provider[1], start, count, deadline, provider[2], provider[3])
    if tag == "value":
        return _value_children(store, provider[1], start, count, deadline)
    if tag == "range":
        _tag, value, low, high = provider
        return _value_children(store, value, low + start, min(count, high - low - start), deadline, page=False)
    return []


def _value_children(store, value, start, count, deadline, page=True):
    base = _container_base(value)
    if base is list or base is tuple:
        size = base.__len__(value)
        getitem = base.__getitem__
        if page and size > _PAGE and start == 0:
            return [{"name": f"[{low}…{min(low + _PAGE, size) - 1}]", "type": "", "value": "", "kind": "item",
                     "badges": [], "variablesReference": store.add(("range", value, low, min(low + _PAGE, size)))}
                    for low in range(0, size, _PAGE)]
        nodes = []
        for index in range(start, min(size, start + count)):
            if time.perf_counter_ns() > deadline:
                nodes.append(_more(store, ("value", value), index))
                break
            nodes.append(_node(store, f"[{index}]", getitem(value, index), "item"))
        return nodes
    if base is dict:
        return _dict_children(store, value, start, count, deadline, "item")
    if base is set or base is frozenset:
        nodes = []
        for index, element in enumerate(base.__iter__(value)):
            if index < start:
                continue
            if len(nodes) >= count or time.perf_counter_ns() > deadline:
                nodes.append(_more(store, ("value", value), index))
                break
            nodes.append(_node(store, f"<{index}>", element, "item"))
        return nodes
    if type(value) is types.ModuleType:
        try:
            module_dict = _MODULE_DICT.__get__(value)
        except Exception:  # noqa: BLE001
            return []
        return _dict_children(store, module_dict, start, count, deadline, "attr", hide_dunder=True)
    if _is_unreal_object(value):
        return _unreal_children(value)
    return _instance_children(store, value, start, count, deadline)


def _unreal_children(value):
    """§8.8 (RV-21): an unreal object's editor properties as export text from C++, never read through Python, so no
    getter runs and no soft reference loads."""
    path = _unreal_path(value)
    nodes = [_leaf("(path)", repr(path), "str", "special")]
    try:
        props = native.get().object_props(path)
    except Exception as exc:  # noqa: BLE001
        nodes.append(_leaf("(properties)", f"unavailable: {type(exc).__name__}", "", "special"))
        return nodes
    for name, type_name, text in props or ():
        nodes.append(_leaf(str(name), str(text)[:_VALUE_LIMIT], str(type_name), "property", ["unreal"]))
    return nodes


def _instance_children(store, value, start, count, deadline):
    """§8.8: a user-class instance by static enumeration of the instance ``__dict__`` and the MRO dicts, never ``dir()``;
    properties and getset descriptors are not evaluated; ``__slots__`` members are read (no code runs). The instance
    attributes page with a "…more" node (review q1 m5); the class members follow on the first page."""
    instance = _instance_dict(value) or {}
    nodes = []
    if instance:
        nodes.extend(_dict_children(store, instance, start, count, deadline, "attr"))
        if nodes and nodes[-1]["name"] == "…more":
            return nodes
    if start > 0:
        return nodes
    seen = set(dict.keys(instance))
    cls = type(value)
    for base in _safe_mro(cls):
        if base is object:
            continue
        for name, item in list(_class_dict(base).items()):
            if type(name) is not str or name in seen or name.startswith("__"):
                continue
            if type(item) is _MEMBER_TYPE:
                seen.add(name)
                try:
                    member = item.__get__(value, cls)
                except AttributeError:
                    nodes.append(_leaf(name, "<unset>", "", "attr"))
                    continue
                nodes.append(_node(store, name, member, "attr"))
            elif _type_is(item, (property, _GETSET_TYPE)):
                seen.add(name)
                nodes.append(_leaf(name, "property: not evaluated", "", "property", ["property: not evaluated"]))
    return nodes


def _req_threads(session, args):
    return {"threads": [{"id": 1, "name": "Game thread"}]}


def _req_stack(session, args):
    if session.stop is None:
        raise _RequestError("not paused")
    frames = session._stack_frames(session.stop)
    start = int(args.get("startFrame") or 0)
    levels = int(args.get("levels") or 0) or len(frames)
    return {"stackFrames": frames[start:start + levels], "totalFrames": len(frames)}


def _req_scopes(session, args):
    frame_id = args.get("frameId", 1)
    frame, _line = session.frame_at(frame_id)
    store = session.store
    scopes = [
        {"name": "Locals", "variablesReference": store.add(("scope_locals", frame)), "expensive": False},
    ]
    if frame.f_locals is not frame.f_globals:
        scopes.append({"name": "Globals", "variablesReference": store.add(("scope_globals", frame)), "expensive": True})
    special = []
    if session.stop.exception is not None and frame_id in (1, "1"):
        special.append(("(exception)", session.stop.exception))
    last = session.bdb.last_return
    if last is not _MISSING and frame_id in (1, "1"):
        special.append((f"(return) {last[0]}", last[1]))
    if special:
        scopes.append({"name": "Special", "variablesReference": store.add(("scope_special", special)),
                       "expensive": False})
    return {"scopes": scopes}


def _req_variables(session, args):
    if session.stop is None:
        raise _RequestError("not paused")
    ref = args.get("variablesReference")
    provider = session.store.get(ref) if isinstance(ref, int) else None
    if provider is None:
        raise _RequestError(f"no variables handle {ref} in this stop")
    start = max(0, int(args.get("start") or 0))
    count = int(args.get("count") or _PAGE)
    count = max(1, min(count, _PAGE))
    deadline = time.perf_counter_ns() + _BUDGET_NS
    return {"variables": _children(session.store, provider, start, count, deadline)}


def _req_source(session, args):
    source = args.get("source") or {}
    path = source.get("path") if isinstance(source, dict) else None
    if not isinstance(path, str) or not path:
        raise _RequestError("source needs a path")
    return {"content": "".join(linecache.getlines(path)), "mimeType": "text/x-python"}


def _req_set_breakpoints(session, args):
    source = args.get("source") or {}
    path = source.get("path") if isinstance(source, dict) else None
    if not isinstance(path, str) or not path:
        raise _RequestError("setBreakpoints needs source.path")
    specs = [dict(item, source_key=path) for item in args.get("breakpoints") or () if isinstance(item, dict)]
    results = session.set_breakpoints(path, specs)
    return {"breakpoints": results}


def _req_evaluate(session, args):
    raise _RequestError("Evaluate is not available in this version of the Paused window")


_HANDLERS = {
    "threads": _req_threads,
    "stackTrace": _req_stack,
    "scopes": _req_scopes,
    "variables": _req_variables,
    "source": _req_source,
    "setBreakpoints": _req_set_breakpoints,
    "evaluate": _req_evaluate,
}


def handle_request(request_json):
    """The DAP-shaped entry (§8.1.2): one request as JSON text in, one response as JSON text out. Never raises."""
    seq = 0
    command = None
    try:
        request = json.loads(request_json)
        if not isinstance(request, dict):
            raise _RequestError("the request is not an object")
        seq = request.get("seq") or 0
        command = request.get("command")
        handler = _HANDLERS.get(command)
        if handler is None:
            raise _RequestError(f"unknown command {command!r}")
        session = _session
        if session is None:
            raise _RequestError("no Debug run is active")
        arguments = request.get("arguments") or {}
        if not isinstance(arguments, dict):
            raise _RequestError("arguments must be an object")
        body = handler(session, arguments)
        response = {"seq": 0, "type": "response", "request_seq": seq, "command": command, "success": True,
                    "body": body}
    except _RequestError as exc:
        response = {"seq": 0, "type": "response", "request_seq": seq, "command": command, "success": False,
                    "message": str(exc)}
    except BaseException as exc:  # noqa: BLE001 - shown in the window; never into the modal loop (review q1 m4)
        response = {"seq": 0, "type": "response", "request_seq": seq, "command": command, "success": False,
                    "message": f"internal error: {type(exc).__name__}: {exc}"}
    return json.dumps(response, ensure_ascii=False, default=str)
