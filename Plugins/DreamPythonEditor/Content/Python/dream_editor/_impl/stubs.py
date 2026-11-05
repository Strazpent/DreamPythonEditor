# SPDX-License-Identifier: Apache-2.0
"""Stage 1 of the ``unreal`` stubs: the read-only snapshot walker (TDD §7.4.2, RT-13, D5).

Runs on the game thread in idle slices that C++ (``FDreamStubSnapshot``) drives from an FDreamDeferral ticker: at most
one ``step`` per frame, never while a run is active, a modal is open or within 300 ms of a Dream Python Editor keystroke. It does no
file I/O: every slice returns its output and C++ writes the files on a background task.

What is walked: every ``sys.modules['_unreal_<m>']`` (grouped by that module, never by ``__module__``, which reports
``builtins``), then the ``unreal`` dict itself (module functions, data and glue types found in no ``_unreal_*`` module;
types there with ``Object`` in their MRO are Blueprint-generated and skipped). How it is read: only dict reads and
allow-listed C slots. Type dicts through ``type.__dict__['__dict__'].__get__(cls)``, bases and MRO the same way, the
member kind by ``type(value)``. ``__doc__`` is read only from objects whose type is in ``_DOC_SAFE_TYPES``, through
the descriptor in that type's own dict. Getters, properties, ``constant_value``, ``_EnumEntry`` and module
``__getattr__`` are never called. ``PyTest*`` types are filtered out and counted.

Interface (C++ calls these through FDreamPython's snapshot helpers):

* ``start(key, engine, done)``: begin or resume a walk. ``done`` lists the manifest entries of modules an earlier,
  interrupted walk already wrote (C++ keeps them in ``snapshot/progress.json``); they are skipped and copied into the
  manifest.
* ``step(budget_ns=4_000_000) -> list`` of ``(kind, module, data)``: kind 0 appends one JSONL line (no newline) to
  ``modules/<module>.jsonl``; kind 1 means that module is finished and ``data`` is its manifest entry as JSON; kind 2
  means the walk is complete and ``data`` is ``manifest.json`` (C++ writes it, then ``snapshot/.complete``).
* ``is_active()``, ``stop()`` (Python shutdown, §3.8 step 3).
"""

import hashlib
import json
import math
import sys
import time
import types

from . import docstrings

__all__ = ["FORMAT", "SCHEMA", "UNREAL_MODULE", "start", "step", "is_active", "stop"]

FORMAT = "dream.stubsnap"
SCHEMA = 1
#: The pseudo-module that holds the ``unreal`` dict's own names.
UNREAL_MODULE = "__unreal__"
_MODULE_PREFIX = "_unreal_"
#: Return this long before the budget ends (§7.4.2 "returns 0.5 ms early").
_EARLY_NS = 500_000
#: Check the clock after every this many members of one type.
_MEMBER_BATCH = 64
#: Check the clock after every this many module-dict entries while planning.
_PLAN_BATCH = 256

# Slot accessors: C getsets read directly, so no metaclass or module __getattribute__/__getattr__ ever runs.
_TYPE_DICT = type.__dict__["__dict__"].__get__
_TYPE_BASES = type.__dict__["__bases__"].__get__
_TYPE_MRO = type.__dict__["__mro__"].__get__
_TYPE_NAME = type.__dict__["__name__"].__get__
_MODULE_DICT = types.ModuleType.__dict__["__dict__"].__get__

# CPython's own descriptor types, matched by identity.
_GETSET = types.GetSetDescriptorType
_MEMBER = types.MemberDescriptorType
_METHOD_DESCRIPTOR = types.MethodDescriptorType
_BUILTIN = types.BuiltinFunctionType
_CLASSMETHOD_DESCRIPTOR = types.ClassMethodDescriptorType
_WRAPPER = types.WrapperDescriptorType
_METHOD_WRAPPER = types.MethodWrapperType

# The engine's C types (PyMethodWithClosure.cpp, PyConstant.cpp, PyWrapperEnum.cpp), matched by type name because
# they cannot be imported; CPython's descriptor types by identity first, then by name too (the tier-2 fakes are
# slot-only Python types with those names, so no getter of theirs can run either).
_KINDS_BY_NAME = {
    "methodwithclosure_descriptor": "method",
    "classmethodwithclosure_descriptor": "classmethod",
    "constant_value": "constant",
    "_EnumEntry": "enum_entry",
    "getset_descriptor": "getset",
    "member_descriptor": "member",
    "method_descriptor": "glue",
    "builtin_function_or_method": "glue",
    "classmethod_descriptor": "glue",
    "wrapper_descriptor": "glue",
}
_SCALARS = (bool, int, float, str, type(None))

# The types whose __doc__ is read (§7.4.2): the C descriptor types above, plus type, function, property,
# classmethod and staticmethod. Engine C types are added by name in _doc_safe.
_DOC_SAFE_TYPES = frozenset({
    _GETSET, _MEMBER, _METHOD_DESCRIPTOR, _BUILTIN, _CLASSMETHOD_DESCRIPTOR, _WRAPPER,
    type, types.FunctionType, property, classmethod, staticmethod,
})

# Wrapper base types (PyWrapperBase.cpp and friends): the kind of a type comes from its MRO.
_KIND_BY_BASE = (
    ("_ObjectBase", "class"), ("Object", "class"), ("StructBase", "struct"), ("EnumBase", "enum"),
    ("DelegateBase", "delegate"), ("MulticastDelegateBase", "delegate"),
)

# N6, inverted: the C++ module behind the lower-cased Python module name when no class doc names it.
_CPP_MODULE_FALLBACK = {"core": "CoreUObject", "slate": "SlateCore", "editor": "UnrealEd", "python": "PythonScriptPlugin"}

_state = None


def _type_name(cls):
    try:
        return _TYPE_NAME(cls)
    except Exception:  # noqa: BLE001
        return "?"


def _is_type(value):
    return issubclass(type(value), type)


def _own_dict_entry(cls, name):
    """The raw entry ``name`` of ``cls``'s own dict (no descriptor call), or None."""
    try:
        return _TYPE_DICT(cls).get(name)
    except Exception:  # noqa: BLE001
        return None


def _doc_safe(value_type):
    return value_type in _DOC_SAFE_TYPES or _type_name(value_type) in _KINDS_BY_NAME


def _read_doc(value):
    """``value.__doc__`` when its type is allow-listed: through the ``__doc__`` descriptor found in the MRO dicts of
    ``type(value)``, called only when it is a C getset or member descriptor. None otherwise."""
    value_type = type(value)
    if value_type is type or issubclass(value_type, type):
        doc = _own_dict_entry(value, "__doc__")
        return doc if isinstance(doc, str) else None
    if not _doc_safe(value_type):
        return None
    try:
        for klass in _TYPE_MRO(value_type):
            entry = _TYPE_DICT(klass).get("__doc__")
            if entry is None:
                continue
            entry_type = type(entry)
            if entry_type is _GETSET or entry_type is _MEMBER:
                doc = entry.__get__(value, value_type)
                return doc if isinstance(doc, str) else None
            return entry if isinstance(entry, str) else None
    except Exception:  # noqa: BLE001
        return None
    return None


def _member_kind(value):
    """(kind, extra) for a member value, decided by ``type(value)`` alone."""
    value_type = type(value)
    if value_type is _GETSET:
        return "getset", None
    if value_type in _SCALARS:
        return "data", value
    by_name = _KINDS_BY_NAME.get(_type_name(value_type))
    if by_name is not None:
        return by_name, None
    if value_type is _METHOD_DESCRIPTOR or value_type is _BUILTIN or value_type is _CLASSMETHOD_DESCRIPTOR \
            or value_type is _WRAPPER or value_type is _METHOD_WRAPPER:
        return "glue", None
    if value_type is _MEMBER:
        return "member", None
    if value_type in _SCALARS:
        return "data", value
    return "other", _type_name(value_type)


def _kind_of_type(cls):
    try:
        names = [_type_name(klass) for klass in _TYPE_MRO(cls)]
    except Exception:  # noqa: BLE001
        return "glue", False
    uobject = "Object" in names
    for base_name, kind in _KIND_BY_BASE:
        if base_name in names:
            return kind, uobject
    return "glue", uobject


def _json_value(value):
    """A scalar as JSON can hold it: a non-finite float becomes its repr (strict JSON has no NaN or Infinity)."""
    if isinstance(value, float) and not math.isfinite(value):
        return repr(value)
    return value


def _dumps(record):
    return json.dumps(record, ensure_ascii=True, separators=(",", ":")).encode("ascii")


def _now():
    return time.perf_counter_ns()


class _Module:
    __slots__ = ("name", "cpp_module", "items", "count", "index", "lines", "sha", "snap_ns", "header_done",
                 "is_unreal")

    def __init__(self, name, items, is_unreal=False):
        self.name = name
        self.cpp_module = None
        self.items = items              # [(key, value)] of the records to emit
        self.count = len(items)
        self.index = 0
        self.lines = 0
        self.sha = hashlib.sha256()
        self.snap_ns = 0
        self.header_done = False
        self.is_unreal = is_unreal


class _Walk:
    """One walk: planning (module dicts read into item lists, largest first), then types and members in slices."""

    def __init__(self, key, engine, done, modules):
        self.key = str(key)
        self.engine = dict(engine or {})
        self.done = [dict(entry) for entry in (done or []) if isinstance(entry, dict) and entry.get("name")]
        self.done_names = {entry["name"] for entry in self.done}
        self.sys_modules = modules if modules is not None else sys.modules
        self.planned = False
        self.plan_cursor = None
        self.queue = []                 # _Module, largest first, __unreal__ last
        self.current = None
        self.member_cursor = None       # (type record, member items, index) of a type split across slices
        self.entries = []               # manifest entries of modules finished in this walk
        self.pytest_types = 0
        self.type_module = {}           # id(type) -> module name, for base_modules
        self.eprop_cache = {}           # id(type) -> frozenset of editor-property names (first-base comparison)
        self.slices = 0
        self.overruns = 0               # slices longer than their budget (§7.11 "overrun counted")
        self.max_slice_ns = 0
        self.total_ns = 0
        self.finished = False
        self.slice_begin = 0
        self.budget_ns = 4_000_000

    # ---- planning ----------------------------------------------------------------------------------------------

    def _plan(self, deadline):
        """Reads every _unreal_* module dict, then the unreal dict, into item lists (a generator resumed slice by
        slice, so a module of 50,000 types never holds one slice past its budget). True when planning is complete."""
        if self.plan_cursor is None:
            self.plan_cursor = self._plan_steps()
        for _ in self.plan_cursor:
            if _now() >= deadline:
                return False
        self.plan_cursor = None
        return True

    def _plan_steps(self):
        names = []
        try:
            for name, module in list(self.sys_modules.items()):
                if isinstance(name, str) and name.startswith(_MODULE_PREFIX) and type(module) is types.ModuleType:
                    names.append((name, module))
        except Exception:  # noqa: BLE001
            names = []
        names.sort(key=lambda pair: pair[0])
        planned = []
        for full_name, module in names:
            short = full_name[len(_MODULE_PREFIX):]
            try:
                entries = list(_MODULE_DICT(module).items())
            except Exception:  # noqa: BLE001
                entries = []
            items = []
            for number, (key, value) in enumerate(entries):
                if number % _PLAN_BATCH == _PLAN_BATCH - 1:
                    yield
                if not isinstance(key, str) or not _is_type(value):
                    continue
                self.type_module.setdefault(id(value), short)
                if key.startswith("PyTest") or _type_name(value).startswith("PyTest"):
                    if short not in self.done_names:
                        self.pytest_types += 1
                    continue
                items.append((key, value))       # dict order: deterministic for one engine build
            if short not in self.done_names:
                planned.append(_Module(short, items))
            yield

        # The unreal dict: functions, data and glue types found in no _unreal_* module.
        unreal_items = []
        if UNREAL_MODULE not in self.done_names:
            unreal = None
            try:
                unreal = self.sys_modules.get("unreal")
            except Exception:  # noqa: BLE001
                unreal = None
            if type(unreal) is types.ModuleType:
                try:
                    entries = list(_MODULE_DICT(unreal).items())
                except Exception:  # noqa: BLE001
                    entries = []
                for number, (key, value) in enumerate(entries):
                    if number % _PLAN_BATCH == _PLAN_BATCH - 1:
                        yield
                    if not isinstance(key, str) or key.startswith("__"):
                        continue
                    if _is_type(value):
                        if id(value) in self.type_module:
                            continue
                        if key.startswith("PyTest") or _type_name(value).startswith("PyTest"):
                            self.pytest_types += 1
                            continue
                        _, uobject = _kind_of_type(value)
                        if uobject:
                            continue    # Blueprint-generated: live completion covers it
                        self.type_module.setdefault(id(value), UNREAL_MODULE)
                        unreal_items.append((key, value))
                    elif type(value) is types.ModuleType:
                        continue
                    else:
                        unreal_items.append((key, value))
        planned.sort(key=lambda module: (-module.count, module.name))
        self.queue = planned
        if UNREAL_MODULE not in self.done_names:
            self.queue.append(_Module(UNREAL_MODULE, unreal_items, is_unreal=True))
        self.planned = True

    # ---- records ------------------------------------------------------------------------------------------------

    def _eprop_names(self, cls):
        cached = self.eprop_cache.get(id(cls))
        if cached is not None:
            return cached
        parsed = docstrings.parse_class_doc(_own_dict_entry(cls, "__doc__"))
        names = frozenset(p.name for p in parsed.editor_properties) if parsed else frozenset()
        self.eprop_cache[id(cls)] = names
        return names

    def _type_header(self, key, cls):
        kind, uobject = _kind_of_type(cls)
        try:
            bases = list(_TYPE_BASES(cls))
        except Exception:  # noqa: BLE001
            bases = []
        doc = _own_dict_entry(cls, "__doc__")
        doc = doc if isinstance(doc, str) else ""
        parsed = docstrings.parse_class_doc(doc)
        inherited = self._eprop_names(bases[0]) if bases else frozenset()
        eprops = []
        if parsed is not None:
            for prop in parsed.editor_properties:
                if prop.name in inherited:
                    continue
                eprops.append([prop.name, prop.type, "ro" if prop.read_only else "rw", prop.tooltip])
        return {
            "rec": "type",
            "name": key,
            "kind": kind,
            "bases": [_type_name(base) for base in bases],
            "base_modules": [self.type_module.get(id(base), "builtins") for base in bases],
            "uobject": uobject,
            "doc": docstrings.strip_editor_properties(doc),
            "eprops": eprops,
            "members": [],
        }

    @staticmethod
    def _member_record(name, value):
        kind, extra = _member_kind(value)
        record = {"n": name, "k": kind}
        if kind == "data":
            record["t"] = _type_name(type(value))
            record["v"] = _json_value(extra)
        elif kind == "other":
            record["t"] = extra
            if _doc_safe(type(value)):
                doc = _read_doc(value)
                if doc:
                    record["doc"] = docstrings.normalize(doc)
            return record
        if kind not in ("data", "member"):
            doc = _read_doc(value)
            if doc:
                record["doc"] = docstrings.normalize(doc)
        return record

    def _emit(self, module, out, record):
        line = _dumps(record)
        module.sha.update(line)
        module.sha.update(b"\n")
        module.lines += 1
        out.append((0, module.name, line))

    def _module_cpp(self, module):
        if module.is_unreal:
            return ""
        for _, value in module.items[:16]:
            parsed = docstrings.parse_class_doc(_own_dict_entry(value, "__doc__"))
            if parsed is not None and parsed.source is not None and parsed.source.module:
                return parsed.source.module
        return _CPP_MODULE_FALLBACK.get(module.name, module.name[:1].upper() + module.name[1:])

    def _finish_module(self, module, out):
        entry = {
            "name": module.name,
            "cpp_module": module.cpp_module or "",
            "file": "modules/%s.jsonl" % module.name,
            "types": module.count,
            "sha256": module.sha.hexdigest(),
            "snap_ms": round(module.snap_ns / 1e6, 3),
        }
        self.entries.append(entry)
        out.append((1, module.name, json.dumps(entry, separators=(",", ":")).encode("ascii")))

    def _manifest(self):
        current = _now() - self.slice_begin      # the slice that finishes the walk counts too
        modules = [entry for entry in self.done if entry.get("name") != UNREAL_MODULE]
        modules += [entry for entry in self.entries if entry.get("name") != UNREAL_MODULE]
        manifest = {
            "format": FORMAT,
            "schema": SCHEMA,
            "key": self.key,
            "engine": self.engine,
            "complete": True,
            "modules": modules,
            "unreal_module": "modules/%s.jsonl" % UNREAL_MODULE,
            "filtered": {"pytest_types": self.pytest_types},
            "stats": {
                "slices": self.slices + 1,
                "max_slice_us": int(max(self.max_slice_ns, current) // 1000),
                "total_idle_ms": int((self.total_ns + current) // 1_000_000),
                "overruns": self.overruns + (1 if current > self.budget_ns else 0),
            },
        }
        return json.dumps(manifest, indent=1, separators=(",", ": ")).encode("ascii")

    # ---- slices -------------------------------------------------------------------------------------------------

    def _walk_members(self, module, out, deadline):
        """Continues the type in member_cursor. True when the type is complete."""
        record, items, index = self.member_cursor
        while index < len(items):
            stop = min(index + _MEMBER_BATCH, len(items))
            while index < stop:
                name, value = items[index]
                index += 1
                if not isinstance(name, str) or (name.startswith("__") and name.endswith("__")):
                    continue
                record["members"].append(self._member_record(name, value))
            if index < len(items) and _now() >= deadline:
                self.member_cursor = (record, items, index)
                return False
        self._emit(module, out, record)
        self.member_cursor = None
        return True

    def _unreal_record(self, key, value):
        if _is_type(value):
            return None
        kind, extra = _member_kind(value)
        if kind == "data":
            return {"rec": "data", "name": key, "t": _type_name(type(value)), "v": _json_value(extra)}
        if kind in ("glue", "method", "classmethod") or type(value) is types.FunctionType:
            record = {"rec": "func", "name": key}
            doc = _read_doc(value)
            if doc:
                record["doc"] = docstrings.normalize(doc)
            return record
        record = {"rec": "data", "name": key, "t": _type_name(type(value))}
        return record

    def step(self, budget_ns):
        begin = _now()
        self.slice_begin = begin
        self.budget_ns = int(budget_ns)
        deadline = begin + max(0, int(budget_ns) - _EARLY_NS)
        out = []
        try:
            if not self.planned and not self._plan(deadline):
                return out
            while not self.finished:
                if self.current is None:
                    if not self.queue:
                        out.append((2, "", self._manifest()))
                        self.finished = True
                        break
                    self.current = self.queue.pop(0)
                module = self.current
                module_begin = _now()
                if not module.header_done:
                    module.cpp_module = self._module_cpp(module)
                    count = module.count
                    if module.is_unreal:
                        count = sum(1 for _, value in module.items if _is_type(value))
                        module.count = count
                    self._emit(module, out, {"rec": "module", "name": module.name, "cpp_module": module.cpp_module,
                                             "count": count})
                    module.header_done = True
                complete = True
                if self.member_cursor is not None:
                    complete = self._walk_members(module, out, deadline)
                while complete and module.index < len(module.items):
                    if _now() >= deadline:
                        complete = False
                        break
                    key, value = module.items[module.index]
                    module.index += 1
                    if _is_type(value):
                        record = self._type_header(key, value)
                        try:
                            members = list(_TYPE_DICT(value).items())
                        except Exception:  # noqa: BLE001
                            members = []
                        self.member_cursor = (record, members, 0)
                        complete = self._walk_members(module, out, deadline)
                    else:
                        record = self._unreal_record(key, value)
                        if record is not None:
                            self._emit(module, out, record)
                module.snap_ns += _now() - module_begin
                if not complete or module.index < len(module.items):
                    break
                self._finish_module(module, out)
                self.current = None
                if _now() >= deadline:
                    break
            return out
        finally:
            elapsed = _now() - begin
            if elapsed > self.budget_ns:
                self.overruns += 1
            self.slices += 1
            self.total_ns += elapsed
            if elapsed > self.max_slice_ns:
                self.max_slice_ns = elapsed


def start(key, engine, done, _modules=None):
    """Begins (or restarts, resuming past ``done``) a walk. ``_modules`` replaces ``sys.modules`` in tests."""
    global _state
    _state = _Walk(key, engine, done, _modules)


def step(budget_ns=4_000_000):
    """One idle slice; see the module doc for the output tuples. ``[]`` when no walk is active."""
    global _state
    walk = _state
    if walk is None or walk.finished:
        return []
    out = walk.step(budget_ns)
    if walk.finished:
        _state = None
    return out


def is_active():
    """True while a walk has output left to produce."""
    return _state is not None and not _state.finished


def stop():
    """Forgets the walk (Python shutdown, §3.8 step 3, or a new key)."""
    global _state
    _state = None


def stats():
    """The running walk's counters (tests and the status line): slices, max slice in µs, idle time in ms."""
    walk = _state
    if walk is None:
        return None
    return {"slices": walk.slices, "max_slice_us": walk.max_slice_ns // 1000, "total_idle_ms": walk.total_ns // 1_000_000,
            "overruns": walk.overruns}
