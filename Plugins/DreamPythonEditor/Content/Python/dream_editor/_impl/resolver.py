# SPDX-License-Identifier: Apache-2.0
"""The live resolver (TDD §7.5, D6, INT-02, RV-21, RV-23): completion from live objects, running no user or engine code.

Game thread, idle only: C++ calls these helpers from FDreamDeferral idle ticks (§12.1.3, INV-6), never from a Slate
handler and never while a run is active (D14(5)). Every public function returns its documented shape and never raises;
an internal failure is reported in the result's ``"error"`` string.

Safety policy (§7.5.2). Members come only from MRO dicts, read through the C slot readers of ``type``
(``type.__dict__['__mro__']``, ``['__dict__']``, ``['__name__']``...). Kinds come from ``type(attr)`` identity. Nothing is
ever called on a user value: no ``dir()``, ``getattr``, ``hasattr``, ``isinstance``, ``str``/``repr``, descriptor
``__get__``, ``__getattr__``, property or unreal getset. ``inspect.signature`` is never used. ``__doc__`` is read only
from the ``_DOC_SAFE_TYPES`` slots. The instance ``__dict__`` is read only when the type's ``'__dict__'`` entry is the
standard getset slot. Literal subscripts only on exact ``list``, ``tuple``, ``dict``, and on exact ``unreal.Array`` /
``unreal.Map`` whose element property ``_dream.container_element_kind`` reports as hard (RV-21); otherwise the step
resolves at type level. ``get_editor_property`` reads only hard object references (``_dream.hard_object_props``, N1).
Call steps resolve at type level (a class gives its instance, a method its documented return type).

Residual risk, accepted (§7.5.2): a dict lookup with a literal key can call a stored key's ``__eq__`` on a hash
collision.

Deferred (M2): hover(req), signature(req), definition(req) and saferepr (§7.7); ``/Game`` asset-path completion
(§7.5.4, FAssetPathCache).
"""

import builtins
import collections
import inspect
import re
import sys
import time
import types

from . import namespaces
from . import native

__all__ = ["complete", "session_names", "prewarm", "unreal_names", "set_frame", "clear_frame"]

# Clocks: module attributes so tier 2 can patch them.
_clock = time.perf_counter_ns
_wall = time.monotonic

_DEFAULT_BUDGET_NS = 5_000_000          # §7.5.3 deadline
_RESERVE_NS = 300_000                   # the walk stops this much before the deadline: building the result follows
_PREWARM_BUDGET_NS = 2_000_000          # RV-23 idle slices (proposed)
_LISTING_FLOOR_NS = 4_000_000           # a member listing started inside a prewarm slice may finish in this much
_BREAKER_NS = 20_000_000                # 4x the budget (proposed)
_GEP_TTL_S = 10.0                       # get_editor_property values expire (proposed)
_GEP_CACHE_MAX = 256                    # get_editor_property values kept (each holds its owner and value strongly)
_CHECK_EVERY = 256
_MAX_ROOT_EXPORT = 10_000
_RESULT_CACHE_MAX = 512
_MAX_CHAIN = 4096                       # longer chains answer at once with incomplete (the deadline holds, T-RES-007)
_MEMBER_CACHE_MAX = 512
_HEAPTYPE = 1 << 9
_SOFT_KINDS = frozenset({"SoftObjectProperty", "SoftClassProperty", "LazyObjectProperty"})
_EDITOR_PROPS_HEADER = "**Editor Properties:**"
_EP_RE = re.compile(r"- ``([^`]+)`` \((.+?)\): +\[(Read-Write|Read-Only)\]")   # UE writes two spaces (PythonizeTooltip)
_MISSING = object()

# C slot readers of built-in types: reading them never runs user code.
_T_DICT = type.__dict__["__dict__"]
_T_MRO = type.__dict__["__mro__"]
_T_NAME = type.__dict__["__name__"]
_T_QUALNAME = type.__dict__["__qualname__"]
_T_FLAGS = type.__dict__["__flags__"]
_T_DOC = type.__dict__["__doc__"]
_MOD_DICT = types.ModuleType.__dict__["__dict__"]
_FUNC_ANN = types.FunctionType.__dict__["__annotations__"]
_FUNC_DOC = types.FunctionType.__dict__["__doc__"]
_SM_FUNC = staticmethod.__dict__["__func__"]
_CM_FUNC = classmethod.__dict__["__func__"]
_OBJ_GETATTR = object.__getattribute__

_GETSET = types.GetSetDescriptorType
_MEMBER = types.MemberDescriptorType
_STD_METHOD_TYPES = (types.MethodDescriptorType, types.BuiltinFunctionType, types.WrapperDescriptorType,
                     types.ClassMethodDescriptorType)
_DOC_SLOTS = tuple((t, _T_DICT.__get__(t)["__doc__"]) for t in (_GETSET, _MEMBER) + _STD_METHOD_TYPES)
_SCALARS = (int, float, str, bool, bytes, type(None))
# Plain built-in value types, keyed by id (a dict keyed by a type would hash it, which a user metaclass can override).
_PLAIN = {id(t): t.__name__ for t in (int, float, complex, str, bool, bytes, bytearray, type(None), list, dict, tuple,
                                      set, frozenset)}

# The types whose __doc__ the resolver reads (§7.5.2): the C descriptor types, the unreal descriptor types found at
# run time (methodwithclosure_descriptor, classmethodwithclosure_descriptor, constant_value, _EnumEntry), and these.
_DOC_SAFE_TYPES = (_GETSET, _MEMBER) + _STD_METHOD_TYPES + (type, types.FunctionType, property, classmethod, staticmethod)

_UNREAL_KIND_NAMES = {
    "methodwithclosure_descriptor": "methodwithclosure",
    "classmethodwithclosure_descriptor": "classmethodwithclosure",
    "builtin_function_or_method_with_closure": "functionwithclosure",
    "constant_value": "constant_value",
    "_EnumEntry": "enum_entry",
}
_DISCOVERY_CLASSES = ("Object", "Actor", "EnumBase", "SystemLibrary", "EditorAssetLibrary", "EditorLevelLibrary",
                      "MathLibrary", "Vector", "LinearColor", "Name")
_DISCOVERY_SCAN_MAX = 200
_DISCOVERY_VALUES_MAX = 4000
_BUILTIN_TYPES = {"str": str, "int": int, "float": float, "bool": bool, "bytes": bytes, "list": list, "dict": dict,
                  "tuple": tuple, "set": set, "object": object}

# ---- State -----------------------------------------------------------------------------------------------------

# Unreal descriptor types found in the current unreal module: [(type, kind)]. The discovery is a generator advanced under
# the active call's deadline (§7.5.3), so a slow first walk spreads over several calls instead of overrunning one.
_disc = {"module": None, "kinds": [], "walk": None, "done": True}
_active = {"deadline": None, "stepped": False}  # the running public helper's deadline (None: no limit); a chunk walked
_gep_for = {"module": None, "fn": None}     # _GEP captured once per unreal module (§7.5.2), never looked up per call
_test_kinds = []                            # [(type, kind)] registered by tier 2
_test_doc_readers = []                      # [(type, reader)]
_frames = {}                                # "frame:<id>" roots (M3 debugger hook): id -> (locals, globals)
_broken = set()                             # circuit-broken root type names (session-long)
_gen = {"generation": None, "epoch": None}
_result_cache = collections.OrderedDict()   # key -> (expiry or None, result)
_member_cache = collections.OrderedDict()   # (token, generation) -> (type_name, stable, items)
_gep_cache = collections.OrderedDict()      # (id(obj), name) -> (obj, read_time, value); oldest first, bounded
_hard_cache = {}                            # type name -> frozenset
_prewarm_state = {"ns": None, "keys": None, "dicts": None, "exported": set()}
_unreal_pass = {"module": None, "keys": None}


class _BadRequest(Exception):
    pass


class _TooDeep(Exception):
    pass


# ---- Slot helpers ----------------------------------------------------------------------------------------------

def _mro(t):
    return _T_MRO.__get__(t)


def _cdict(cls):
    return _T_DICT.__get__(cls)


def _tname(t):
    try:
        name = _T_NAME.__get__(t)
    except Exception:  # noqa: BLE001 - never raises out of the resolver
        return "?"
    return name if type(name) is str else "?"


def _qualname(t):
    try:
        name = _T_QUALNAME.__get__(t)
    except Exception:  # noqa: BLE001
        return _tname(t)
    return name if type(name) is str else _tname(t)


def _is_heap(t):
    return bool(_T_FLAGS.__get__(t) & _HEAPTYPE)


def _in_mro(t, cls):
    for base in _mro(t):
        if base is cls:
            return True
    return False


def _is_type(v):
    return _in_mro(type(v), type)


def _lookup(t, name):
    """The first MRO dict entry for name, or _MISSING (getattr_static on the type, without the metatype)."""
    for cls in _mro(t):
        entry = _cdict(cls).get(name, _MISSING)
        if entry is not _MISSING:
            return entry
    return _MISSING


def _norm(text):
    return text.replace("\r\n", "\n") if type(text) is str else None


def _module_name(m):
    name = _MOD_DICT.__get__(m).get("__name__", None)
    return name if type(name) is str else "?"


def _unreal():
    m = sys.modules.get("unreal")
    return m if type(m) is types.ModuleType else None


def _udict():
    m = _unreal()
    return _MOD_DICT.__get__(m) if m is not None else None


def _udict_type(name):
    d = _udict()
    if d is None:
        return None
    v = dict.get(d, name, None)
    return v if v is not None and _is_type(v) else None


def _dream_fn(name):
    """A _dream function, or None (hard_object_props and container_element_kind may not exist in M1)."""
    dream = native.get()
    if type(dream) is types.ModuleType:
        return _MOD_DICT.__get__(dream).get(name, None)
    return getattr(dream, name, None)    # NullDream: Dream's own class, not user code


# ---- Kinds -----------------------------------------------------------------------------------------------------

def _discover(udict, found):
    """The unreal descriptor types, by name, among the static (non-heap) types reachable from a few classes, appended to
    found. A generator: it yields every _CHECK_EVERY entries so _discover_step can stop at the deadline (§7.5.3)."""
    wanted = dict(_UNREAL_KIND_NAMES)
    enum_base = dict.get(udict, "EnumBase", None)
    if enum_base is not None and _is_type(enum_base):
        meta = type(enum_base)
        if _tname(meta) == "_EnumType" and not _is_heap(meta):
            found.append((meta, "enum_meta"))
    values = list(udict.values())
    count = 0
    for value in values[:_DISCOVERY_VALUES_MAX]:       # module-level functions are bound closures
        count += 1
        if count % _CHECK_EVERY == 0:
            yield
        tp = type(value)
        if _tname(tp) == "builtin_function_or_method_with_closure" and not _is_heap(tp):
            found.append((tp, wanted.pop("builtin_function_or_method_with_closure")))
            break
    candidates = [dict.get(udict, name, None) for name in _DISCOVERY_CLASSES] + values
    scanned = 0
    for cls in candidates:
        count += 1
        if count % _CHECK_EVERY == 0:
            yield
        if not wanted or scanned >= _DISCOVERY_SCAN_MAX:
            break
        if cls is None or not _is_type(cls):
            continue
        scanned += 1
        for entry in list(_cdict(cls).values()):
            count += 1
            if count % _CHECK_EVERY == 0:
                yield
            tp = type(entry)
            name = _tname(tp)
            kind = wanted.get(name)
            if kind is not None and not _is_heap(tp):
                found.append((tp, kind))
                del wanted[name]
                if not wanted:
                    break


def _discover_step():
    """Advances the discovery for the current unreal module under the active deadline; True once it is complete."""
    m = _unreal()
    if m is not _disc["module"]:
        found = []
        _disc.update(module=m, kinds=found, walk=_discover(_MOD_DICT.__get__(m), found) if m is not None else None,
                     done=m is None)
    if _disc["done"]:
        return True
    deadline = _active["deadline"]
    walk = _disc["walk"]
    try:
        while True:
            # One chunk per public call even past the deadline (so the walk always progresses), never more.
            if deadline is not None and _active["stepped"] and _clock() > deadline:
                return False
            next(walk)
            _active["stepped"] = True
    except StopIteration:
        _disc.update(walk=None, done=True)
        return True


def _kinds_ready():
    """The kinds used so far are final: no result that depends on them may be cached before this holds."""
    m = _unreal()
    return m is None or (m is _disc["module"] and _disc["done"])


def _kind_of(tp):
    for known, kind in _test_kinds:
        if known is tp:
            return kind
    _discover_step()
    for known, kind in _disc["kinds"]:
        if known is tp:
            return kind
    return None


def _classify(e):
    te = type(e)
    if te is _GETSET:
        return "getset"
    if te is _MEMBER:
        return "member"
    if te is property:
        return "property"
    if te is types.ModuleType:
        return "module"
    if te is types.FunctionType or te is staticmethod or te is classmethod:
        return "method"
    for tp in _STD_METHOD_TYPES:
        if te is tp:
            return "method"
    kind = _kind_of(te)
    if kind == "methodwithclosure" or kind == "classmethodwithclosure" or kind == "functionwithclosure":
        return "method"
    if kind == "constant_value":
        return "constant"
    if kind == "enum_entry":
        return "enum_entry"
    if kind == "property-getset":
        return "getset"
    if _is_type(e):
        return "class"
    if _lookup(te, "__get__") is not _MISSING:
        return "opaque"
    return "value"


def _is_data_descr(e):
    te = type(e)
    return _lookup(te, "__set__") is not _MISSING or _lookup(te, "__delete__") is not _MISSING


# ---- Docs ------------------------------------------------------------------------------------------------------

def _class_doc(cls):
    if _is_heap(cls):
        return _norm(_cdict(cls).get("__doc__", None))     # a heap type's __doc__ getter could call a descriptor
    return _norm(_T_DOC.__get__(cls))


def _doc_of(e):
    te = type(e)
    try:
        for tp, slot in _DOC_SLOTS:
            if te is tp:
                return _norm(slot.__get__(e))
        if te is types.FunctionType:
            return _norm(_FUNC_DOC.__get__(e))
        for tp, reader in _test_doc_readers:
            if te is tp:
                return _norm(reader(e))
        if _kind_of(te) is not None and not _is_heap(te):
            slot = _cdict(te).get("__doc__", None)
            if type(slot) is _GETSET or type(slot) is _MEMBER:
                return _norm(slot.__get__(e))
            return None
        if _is_type(e):
            return _class_doc(e)
    except Exception:  # noqa: BLE001 - a doc we cannot read is no doc
        return None
    return None


def _prop_type_text(doc):
    """'(Type): [Read-Write] text' -> 'Type'."""
    if not doc or doc[0] != "(":
        return None
    depth = 0
    for i, ch in enumerate(doc[:400]):
        if ch == "(" or ch == "[":
            depth += 1
        elif ch == ")" or ch == "]":
            depth -= 1
            if depth == 0:
                return doc[1:i].strip() or None
    return None


def _return_text(doc):
    """The return type text: the first line's ' -> Ret' (method and glue forms), else the 'Returns:' block."""
    if not doc:
        return None
    first = doc.split("\n", 1)[0].split(" -- ", 1)[0]
    at = first.rfind(") -> ")
    if at >= 0:
        text = first[at + 5:].strip()
        if text:
            return text
    at = doc.find("\nReturns:\n")
    if at >= 0:
        line = doc[at + 10:].split("\n", 1)[0].strip()
        text = line.split(":", 1)[0].strip()
        if text:
            return text
    return None


def _parse_editor_props(doc):
    if not doc:
        return []
    at = doc.find(_EDITOR_PROPS_HEADER)
    if at < 0:
        return []
    out = []
    for line in doc[at:].split("\n")[1:]:
        if line.startswith("- ``"):
            m = _EP_RE.match(line)
            if m is not None:
                out.append((m.group(1), m.group(2).strip(), m.group(3)))
    return out


def _editor_props(t, deadline):
    """Own and inherited Editor Properties from the class docs, union over the MRO (first occurrence wins)."""
    seen = set()
    out = []
    for cls in _mro(t):
        if _clock() > deadline:
            return out, False
        for name, text, rw in _parse_editor_props(_class_doc(cls)):
            if name not in seen:
                seen.add(name)
                out.append((name, text, rw))
    return out, True


# ---- Type text -------------------------------------------------------------------------------------------------

def _named_type(name):
    builtin = _BUILTIN_TYPES.get(name)
    if builtin is not None:
        return builtin
    cls = _udict_type(name)
    if cls is not None:
        return cls
    if name == "Name" or name == "Text":
        return str
    return None


def _split_top(text):
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
    parts.append(text[start:].strip())
    return parts


def _state_from_text(text, depth=0):
    """A walker state from a doc type text: ('inst', cls, container decl) or ('val', cls, None) for type(X)."""
    if not text or depth > 8:
        return None
    t = text.strip()
    if t.startswith("unreal."):
        t = t[7:]
    if t.endswith(" or None"):
        t = t[:-8].strip()
    if t.startswith("Optional[") and t.endswith("]"):
        return _state_from_text(t[9:-1], depth + 1)
    if t.startswith("type(") and t.endswith(")"):
        cls = _named_type(t[5:-1].strip())
        return ("val", cls, None) if cls is not None else None
    for head in ("Array", "Set", "Map"):
        if t.startswith(head + "[") and t.endswith("]"):
            return ("inst", _named_type(head), (head, t[len(head) + 1:-1]))
    if t.startswith("("):
        return None     # an output tuple (a=T, b=U)
    cls = _named_type(t)
    return ("inst", cls, None) if cls is not None else None


def _elem_state(decl):
    head, inner = decl
    if head == "Map":
        parts = _split_top(inner)
        return _state_from_text(parts[1]) if len(parts) == 2 else None
    return _state_from_text(inner)


# ---- Namespaces ------------------------------------------------------------------------------------------------

def _roots(namespace):
    """The namespace's own dicts, in lookup order (exact dicts only)."""
    if type(namespace) is not str:
        raise _BadRequest("namespace")
    found = []
    if namespace == "session":
        found = [namespaces.session()]
    elif namespace.startswith("isolated:"):
        found = [namespaces.last_run()]
    elif namespace.startswith("frame:"):
        entry = _frames.get(namespace[6:])
        found = list(entry) if entry is not None else []
    return [d for d in found if type(d) is dict]


def _find_root(name, dicts):
    for d in dicts:
        value = dict.get(d, name, _MISSING)
        if value is not _MISSING:
            return value, True
    value = dict.get(_MOD_DICT.__get__(builtins), name, _MISSING)
    if value is not _MISSING:
        return value, False
    if name == "unreal":
        m = _unreal()
        if m is not None:
            return m, False
    return _MISSING, False


def _root_type(root):
    t = type(root)
    if t is types.ModuleType:
        return "module " + _module_name(root)
    if _is_type(root):
        return _tname(root)
    return _tname(t)


def set_frame(frame_id, f_locals, f_globals):
    """Registers a paused frame's roots for namespace "frame:<id>" (the M3 debugger hook; never raises)."""
    try:
        if type(frame_id) is int or type(frame_id) is str:
            _frames["%s" % frame_id] = (f_locals, f_globals)
    except Exception:  # noqa: BLE001
        pass


def clear_frame(frame_id):
    try:
        if type(frame_id) is int or type(frame_id) is str:
            _frames.pop("%s" % frame_id, None)
    except Exception:  # noqa: BLE001
        pass


# ---- The walk --------------------------------------------------------------------------------------------------

class _Ctx:
    __slots__ = ("deadline", "generation", "gep_time")

    def __init__(self, deadline, generation):
        self.deadline = deadline
        self.generation = generation
        self.gep_time = None


def _instance_dict(v, t):
    if type(_lookup(t, "__dict__")) is not _GETSET:
        return None
    try:
        d = _OBJ_GETATTR(v, "__dict__")
    except Exception:  # noqa: BLE001
        return None
    return d if type(d) is dict else None


def _hard_props(type_name):
    hit = _hard_cache.get(type_name)
    if hit is not None:
        return hit
    props = frozenset()
    fn = _dream_fn("hard_object_props")
    if fn is not None:
        try:
            result = fn(type_name)
        except Exception:  # noqa: BLE001 - every failure means "do not read" (N1)
            result = None
        if type(result) is frozenset or type(result) is set:
            props = frozenset(x for x in result if type(x) is str)
    _hard_cache[type_name] = props
    return props


def _capture_gep():
    """_GEP = inspect.getattr_static(unreal.Object, 'get_editor_property') (§7.5.2), taken once per unreal module: at
    import when the engine module is there, else at the first need. Later rebinding in the live module is never seen."""
    m = _unreal()
    if m is _gep_for["module"]:
        return _gep_for["fn"]
    obj_cls = _udict_type("Object")
    fn = inspect.getattr_static(obj_cls, "get_editor_property", None) if obj_cls is not None else None
    _gep_for.update(module=m, fn=fn)
    return fn


def _gep_fn():
    return _capture_gep()


def _gep_allowed(owner, name, cls, doc):
    obj_cls = _udict_type("Object")
    if cls is None or obj_cls is None or not _in_mro(cls, obj_cls):
        return False
    if doc and "deprecated" in doc.lower():
        return False
    return name.replace("_", "").lower() in _hard_props(_tname(owner))


def _expire_gep(now):
    """Drops the get_editor_property values older than _GEP_TTL_S (insertion order is read order, so the expired ones are
    first). Run on every lookup and at the start of every public helper, so an expired owner is never held past the
    next resolver call (the prewarm after each run included)."""
    while _gep_cache:
        oldest = next(iter(_gep_cache.values()))
        if now - oldest[1] < _GEP_TTL_S:
            return
        _gep_cache.popitem(last=False)


def _gep_read(v, name, ctx):
    key = (id(v), name)
    now = _wall()
    _expire_gep(now)
    hit = _gep_cache.get(key)
    if hit is not None and hit[0] is v and now - hit[1] < _GEP_TTL_S:
        ctx.gep_time = hit[1] if ctx.gep_time is None else min(ctx.gep_time, hit[1])
        return hit[2]
    gep = _gep_fn()
    if gep is None:
        return _MISSING
    try:
        value = gep(v, name)
    except Exception:  # noqa: BLE001
        return _MISSING
    _gep_cache[key] = (v, now, value)
    _gep_cache.move_to_end(key)
    while len(_gep_cache) > _GEP_CACHE_MAX:     # the overflow goes oldest first
        _gep_cache.popitem(last=False)
    ctx.gep_time = now if ctx.gep_time is None else min(ctx.gep_time, now)
    return value


def _getset_state(owner, name, e, v, ctx):
    doc = _doc_of(e)
    state = _state_from_text(_prop_type_text(doc))
    if state is None:
        return None
    if v is not _MISSING and state[0] == "inst" and state[2] is None and _gep_allowed(owner, name, state[1], doc):
        value = _gep_read(v, name, ctx)
        if value is not _MISSING:
            return ("val", value, None)
    return state


def _instance_attr(t, name, v, ctx):
    e = _lookup(t, name)
    code = _classify(e) if e is not _MISSING else None
    if code == "getset":
        return _getset_state(t, name, e, v, ctx)
    if code == "member" or code == "property":
        return None
    if code == "opaque" and _is_data_descr(e):
        return None
    if v is not _MISSING:
        d = _instance_dict(v, t)
        if d is not None:
            value = dict.get(d, name, _MISSING)
            if value is not _MISSING:
                return ("val", value, None)
    if e is _MISSING or code == "opaque":
        return None
    if code == "enum_entry":
        return ("inst", t, None)
    if code == "constant":
        return _state_from_text(_prop_type_text(_doc_of(e)))
    return ("val", e, None)


def _class_attr(cls, name):
    e = _lookup(cls, name)
    if e is _MISSING:
        return None
    code = _classify(e)
    if code == "enum_entry":
        return ("inst", cls, None)
    if code == "constant":
        return _state_from_text(_prop_type_text(_doc_of(e)))
    if code == "opaque":
        return None
    return ("val", e, None)


def _attr_state(state, name, ctx):
    mode, obj, _decl = state
    if mode == "val":
        tv = type(obj)
        if tv is types.ModuleType:
            value = _MOD_DICT.__get__(obj).get(name, _MISSING)
            return ("val", value, None) if value is not _MISSING else None
        if _is_type(obj):
            return _class_attr(obj, name)
        return _instance_attr(tv, name, obj, ctx)
    if mode == "inst" and obj is not None:
        return _instance_attr(obj, name, _MISSING, ctx)
    return None


def _call_state(state):
    mode, obj, _decl = state
    if mode != "val":
        return None
    v = obj
    if _is_type(v):
        return ("inst", v, None)
    tv = type(v)
    if tv is staticmethod:
        v = _SM_FUNC.__get__(v)
        tv = type(v)
    elif tv is classmethod:
        v = _CM_FUNC.__get__(v)
        tv = type(v)
    if tv is types.FunctionType:
        ann = _FUNC_ANN.__get__(v)
        ret = dict.get(ann, "return", None) if type(ann) is dict else None
        if ret is not None and _is_type(ret):
            return ("inst", ret, None)
        return _state_from_text(ret) if type(ret) is str else None
    if _classify(v) == "method":
        return _state_from_text(_return_text(_doc_of(v)))
    return None


def _element_kind(v):
    fn = _dream_fn("container_element_kind")
    if fn is None:
        return None
    try:
        kind = fn(v)
    except Exception:  # noqa: BLE001 - a failed call means "do not read" (RV-21)
        return None
    return kind if type(kind) is str and kind not in _SOFT_KINDS else None


def _sub_state(state, key):
    if type(key) is not int and type(key) is not str:
        return None
    mode, obj, decl = state
    if mode == "val":
        tv = type(obj)
        if tv is list or tv is tuple:
            if type(key) is not int:
                return None
            try:
                return ("val", tv.__getitem__(obj, key), None)
            except IndexError:
                return None
        if tv is dict:
            value = dict.get(obj, key, _MISSING)
            return ("val", value, None) if value is not _MISSING else None
        array, mapping = _udict_type("Array"), _udict_type("Map")
        if (array is not None and tv is array) or (mapping is not None and tv is mapping):
            if _element_kind(obj) is not None and (tv is not array or type(key) is int):
                getitem = _lookup(tv, "__getitem__")
                if getitem is not _MISSING:
                    try:
                        return ("val", getitem(obj, key), None)
                    except Exception:  # noqa: BLE001
                        return None
            return _elem_state(decl) if decl is not None else None
        return None
    if mode == "inst" and decl is not None:
        return _elem_state(decl)
    return None


# ---- Items and listings ----------------------------------------------------------------------------------------

def _class_detail(cls):
    if _kind_of(type(cls)) == "enum_meta":
        return "enum"
    struct = _udict_type("StructBase")
    if struct is not None and _in_mro(cls, struct):
        return "struct"
    return "class"


def _item(name, e, in_class, enum_listing=False):
    sort = ("1" if enum_listing else "")
    plain = _PLAIN.get(id(type(e)))
    if plain is not None:
        return (name, "variable", plain, None, sort)
    code = _classify(e)
    if code == "getset":
        return (name, "property", _prop_type_text(_doc_of(e)) or "", None, sort)
    if code == "member":
        return (name, "field", "", None, sort)
    if code == "method":
        te = type(e)
        function = te is types.FunctionType or te is types.BuiltinFunctionType or _kind_of(te) == "functionwithclosure"
        kind = "function" if not in_class and function else "method"
        ret = _return_text(_doc_of(e)) if te is not types.FunctionType else None
        return (name, kind, ("-> " + ret) if ret else "", None, sort)
    if code == "constant":
        return (name, "constant", "", None, sort)
    if code == "enum_entry":
        doc = _doc_of(e) or ""
        return (name, "enum_member", doc.split("\n", 1)[0][:80], None, "0" if enum_listing else sort)
    if code == "property":
        return (name, "property", "", None, sort)
    if code == "class":
        return (name, "class", _class_detail(e), None, sort)
    if code == "module":
        return (name, "module", "module", None, sort)
    if code == "opaque":
        return (name, "variable", "", None, sort)      # unknown descriptor: name only, no doc, no type (T-RES-018)
    return (name, "variable", _tname(type(e)), None, sort)


def _target(state):
    mode, obj, _decl = state
    if mode == "val":
        tv = type(obj)
        if tv is types.ModuleType:
            return ("M", obj, _MISSING)
        if _is_type(obj):
            return ("C", obj, _MISSING)
        return ("I", tv, obj)
    if mode == "inst" and obj is not None:
        return ("I", obj, _MISSING)
    return None


def _token(tag, t, v):
    if tag == "M":
        name = _module_name(t)
        return "M:%x:%s" % (id(t), name), name
    token = "%s:%x:%s" % (tag, id(t), _qualname(t))
    if tag == "I" and v is not _MISSING:
        d = _instance_dict(v, t)
        if d:
            token += "@%x" % id(v)
    return token, _tname(t)


def _is_stable(tag, t, token):
    if tag == "M":
        return t is _unreal()
    if "@" in token:
        return False
    if not _is_heap(t):
        return True
    d = _udict()
    return d is not None and dict.get(d, _tname(t), None) is t


def _list_members(tag, t, v, deadline):
    """(items, complete) for a module ('M'), a class object ('C') or instances of a type ('I')."""
    items = []
    count = 0
    if tag == "M":
        d = _MOD_DICT.__get__(t)
        try:
            for name, value in d.items():
                count += 1
                if count % _CHECK_EVERY == 0 and _clock() > deadline:
                    return items, False
                if type(name) is str:
                    items.append(_item(name, value, False))
        except RuntimeError:        # changed by another thread: incomplete, as for the deadline
            return items, False
        return items, _kinds_ready()
    enum_listing = tag == "C" and _kind_of(type(t)) == "enum_meta"
    seen = set()
    for cls in _mro(t):
        d = _cdict(cls)
        for name in list(d):
            count += 1
            if count % _CHECK_EVERY == 0 and _clock() > deadline:
                return items, False
            if type(name) is not str or name in seen:
                continue
            seen.add(name)
            items.append(_item(name, d.get(name, None), True, enum_listing))
    if tag == "I" and v is not _MISSING:
        d = _instance_dict(v, t)
        for name in list(d) if d else ():
            count += 1
            if count % _CHECK_EVERY == 0 and _clock() > deadline:
                return items, False
            if type(name) is str and name not in seen:
                seen.add(name)
                items.append(_item(name, dict.get(d, name, None), False))
    if tag == "I":
        obj_cls, struct = _udict_type("Object"), _udict_type("StructBase")
        if (obj_cls is not None and _in_mro(t, obj_cls)) or (struct is not None and _in_mro(t, struct)):
            props, complete = _editor_props(t, deadline)
            for name, text, rw in props:
                if name not in seen:     # an edit-only editor property: no Python attribute (INT-02)
                    seen.add(name)
                    items.append((name, "editor_property", "%s [%s]" % (text, rw), 'get_editor_property("%s")' % name, ""))
            if not complete:
                return items, False
    return items, _kinds_ready()


def _members(tag, t, v, generation, deadline):
    """(token, type_name, stable, items, complete), through the member cache."""
    token, type_name = _token(tag, t, v)
    key = (token, generation)
    hit = _member_cache.get(key)
    if hit is not None:
        _member_cache.move_to_end(key)
        return token, hit[0], hit[1], hit[2], True
    items, complete = _list_members(tag, t, v, deadline)
    stable = _is_stable(tag, t, token)
    if complete:
        _member_cache[key] = (type_name, stable, items)
        if len(_member_cache) > _MEMBER_CACHE_MAX:
            _member_cache.popitem(last=False)
    return token, type_name, stable, items, complete


def _list_names(dicts, deadline):
    items, seen, count = [], set(), 0
    for d in dicts:
        # Iterated in place, not copied (a copy of 100,000 keys alone costs about 1.5 ms); a change by another thread
        # between two checks raises RuntimeError, which ends the listing as incomplete.
        try:
            for name, value in d.items():
                count += 1
                if count % _CHECK_EVERY == 0 and _clock() > deadline:
                    return items, False
                if type(name) is str and name not in seen:
                    seen.add(name)
                    items.append(_item(name, value, False))
        except RuntimeError:
            return items, False
    m = _unreal()
    if m is not None and "unreal" not in seen:
        items.append(("unreal", "module", "module", None, ""))
    return items, _kinds_ready()


# ---- Generations and caches ------------------------------------------------------------------------------------

def _sync_generation(generation, epoch):
    if _gen["generation"] != generation:
        _gen["generation"], _gen["epoch"] = generation, epoch
        _result_cache.clear()
        _member_cache.clear()
        _gep_cache.clear()
        _hard_cache.clear()
    elif _gen["epoch"] != epoch:
        _gen["epoch"] = epoch
        _result_cache.clear()       # roots are re-resolved; member lists are kept (U19, T-RES-027)


def _int(value, default=0):
    return value if type(value) is int else default


def _deadline(req, start, default_budget):
    budget = req.get("budget_ns")
    if type(budget) is not int or budget <= 0:
        absolute = req.get("deadline_ns")
        if budget is None and type(absolute) is int:
            budget = absolute - start
        else:
            budget = default_budget
    return start + budget - min(_RESERVE_NS, max(0, budget) // 10)


def _str_set(value):
    if type(value) in (list, tuple, set, frozenset):
        return {x for x in value if type(x) is str}
    return set()


def _parse_chain(chain):
    if type(chain) is not list and type(chain) is not tuple or not chain:
        raise _BadRequest("chain")
    if len(chain) > _MAX_CHAIN:
        raise _TooDeep()
    steps = []
    for element in chain:
        if (type(element) is not list and type(element) is not tuple) or len(element) != 2:
            raise _BadRequest("chain element")
        kind, value = element[0], element[1]
        if type(kind) is not str:
            raise _BadRequest("chain kind")
        steps.append((kind, value))
    if steps[-1][0] != "prefix":
        raise _BadRequest("chain must end with prefix")
    for kind, value in steps[:-1]:
        if kind in ("name", "attr") and type(value) is not str:
            raise _BadRequest("chain name")
        if kind == "sub" and type(value) is not int and type(value) is not str:
            raise _BadRequest("chain subscript")
        if kind not in ("name", "attr", "sub", "call"):
            raise _BadRequest("chain kind")
    return steps[:-1]


def _empty_result():
    return {"token": None, "type_name": "", "root_type": "", "items": [], "incomplete": False, "root_bound": False,
            "stable": False, "members_cached": False, "elapsed_us": 0, "breaker": "", "error": ""}


_CACHED_KEYS = ("token", "type_name", "root_type", "items", "root_bound", "stable")


# ---- Public helpers --------------------------------------------------------------------------------------------

def complete(req):
    """Live completion (§7.5.6). See the module docstring and ARCHITECTURE B-notes for the request and result keys."""
    start = _clock()
    out = _empty_result()
    try:
        _expire_gep(_wall())
        if type(req) is not dict:
            raise _BadRequest("request")
        _active.update(deadline=_deadline(req, start, _DEFAULT_BUDGET_NS), stepped=False)
        _discover_step()                # every call walks the discovery on, also one that classifies nothing new
        _complete(req, start, out)
        if not _kinds_ready():
            out["incomplete"] = True    # the discovery is still walking: the next keystroke asks again
    except _TooDeep:
        out["items"], out["incomplete"] = [], True
    except _BadRequest as exc:
        out["error"] = "bad request: %s" % (exc.args[0] if exc.args and type(exc.args[0]) is str else "")
        out["items"] = []
    except Exception as exc:  # noqa: BLE001 - the resolver never raises (§7.5.6)
        out["error"] = "internal: " + _tname(type(exc))
        out["items"] = []
    finally:
        _active.update(deadline=None, stepped=False)
    elapsed = _clock() - start
    out["elapsed_us"] = max(0, elapsed // 1000)
    root_type = out["root_type"]
    if elapsed > _BREAKER_NS and root_type and root_type not in _broken:
        _broken.add(root_type)
        out["breaker"] = root_type
    return out


def _complete(req, start, out):
    context = req.get("context", "member")
    if type(context) is not str:
        raise _BadRequest("context")
    steps = _parse_chain(req.get("chain"))
    namespace = req.get("namespace", "session")
    generation, epoch = _int(req.get("generation", 0)), _int(req.get("epoch", 0))
    deadline = _deadline(req, start, _DEFAULT_BUDGET_NS)
    want = _str_set(req.get("want_members_for"))
    func, index = req.get("func"), _int(req.get("index", 0))
    if type(func) is not str:
        func = ""
    _sync_generation(generation, epoch)
    if type(namespace) is not str:
        raise _BadRequest("namespace")
    key = (generation, epoch, namespace, context, tuple(steps), func, index)
    try:
        hash(key)
    except TypeError:
        raise _BadRequest("chain") from None
    hit = _result_cache.get(key)
    if hit is not None and (hit[0] is None or _wall() < hit[0]):
        _result_cache.move_to_end(key)
        for name in _CACHED_KEYS:
            out[name] = hit[1][name]
        out["items"] = list(out["items"])
        if out["token"] is not None and out["token"] in want:
            out["items"], out["members_cached"] = [], True
        return
    ctx = _Ctx(deadline, generation)

    if context == "name":
        if steps:
            raise _BadRequest("name context takes only a prefix")
        items, done = _list_names(_roots(namespace), deadline)
        out["items"] = items
        out["incomplete"] = not done
    elif context == "member" or context == "string_arg":
        if not steps or steps[0][0] != "name":
            raise _BadRequest("chain root")
        root, bound = _find_root(steps[0][1], _roots(namespace))
        out["root_bound"] = bound
        if root is _MISSING:
            _store(key, out, ctx)
            return
        out["root_type"] = _root_type(root)
        if type(root) is types.ModuleType and len(steps) > 1 and steps[1][0] == "attr":
            # A module is a namespace, not a type: the breaker holds for the member the chain reaches through it
            # ("module unreal.EditorAssetLibrary"), so one slow class never turns off live results for all of unreal.
            out["root_type"] = "%s.%s" % (out["root_type"], steps[1][1])
        if out["root_type"] in _broken:
            out["error"] = "breaker"
            return
        state = ("val", root, None)
        for kind, value in steps[1:]:
            if _clock() > deadline:
                out["incomplete"] = True
                return
            if kind == "attr":
                state = _attr_state(state, value, ctx)
            elif kind == "sub":
                state = _sub_state(state, value)
            else:
                state = _call_state(state)
            if state is None:
                _store(key, out, ctx)
                return
        if context == "member":
            _complete_members(state, ctx, want, out)
        else:
            _complete_string_arg(state, func, index, ctx, out)
    else:
        raise _BadRequest("context")
    if not out["incomplete"] and not out["members_cached"]:
        _store(key, out, ctx)


def _complete_members(state, ctx, want, out):
    target = _target(state)
    if target is None:
        return
    tag, t, v = target
    token, type_name = _token(tag, t, v)
    out["token"], out["type_name"] = token, type_name
    if token in want:
        out["members_cached"] = True
        out["stable"] = _is_stable(tag, t, token)
        return
    token, type_name, stable, items, done = _members(tag, t, v, ctx.generation, ctx.deadline)
    out["items"], out["stable"], out["incomplete"] = items, stable, not done


def _complete_string_arg(state, func, index, ctx, out):
    if func not in ("get_editor_property", "set_editor_property") or index != 0:
        return
    mode, obj, _decl = state
    if mode == "val":
        if type(obj) is types.ModuleType:
            return
        t = obj if _is_type(obj) else type(obj)
    elif obj is not None:
        t = obj
    else:
        return
    props, done = _editor_props(t, ctx.deadline)
    out["type_name"] = _tname(t)
    out["items"] = [(name, "editor_property", "%s [%s]" % (text, rw), None, "") for name, text, rw in props]
    out["incomplete"] = not done
    out["stable"] = _is_stable("I", t, "")


def _store(key, out, ctx):
    if not _kinds_ready():
        return                  # resolved with a partial discovery: the next call resolves it again
    expiry = None if ctx.gep_time is None else ctx.gep_time + _GEP_TTL_S
    _result_cache[key] = (expiry, {name: out[name] for name in _CACHED_KEYS})
    if len(_result_cache) > _RESULT_CACHE_MAX:
        _result_cache.popitem(last=False)


def session_names(namespace):
    """The str keys of the namespace's own dicts (not builtins): the Problems filter and the C++ cache (U13, RV-23)."""
    try:
        _expire_gep(_wall())
        roots = _roots(namespace)
        if len(roots) == 1:
            # One dict (the Session): its keys are unique already; this keeps 10,000 names well inside a 2 ms slice.
            return tuple([name for name in list(roots[0]) if type(name) is str])
        names, seen = [], set()
        for d in roots:
            for name in list(d):
                if type(name) is str and name not in seen:
                    seen.add(name)
                    names.append(name)
        return tuple(names)
    except Exception:  # noqa: BLE001
        return ()


def prewarm(req):
    """One idle slice of the post-run copy into the C++ cache (RV-23): roots, their kinds and tokens, member lists."""
    out = {"roots": [], "members": [], "cursor": -1, "count": 0, "incomplete": False, "error": ""}
    try:
        _expire_gep(_wall())
        if type(req) is not dict:
            raise _BadRequest("request")
        start = _clock()
        namespace = req.get("namespace", "session")
        if type(namespace) is not str:
            raise _BadRequest("namespace")
        cursor = max(0, _int(req.get("cursor", 0)))
        generation, epoch = _int(req.get("generation", 0)), _int(req.get("epoch", 0))
        _sync_generation(generation, epoch)
        deadline = _deadline(req, start, _PREWARM_BUDGET_NS)
        _active.update(deadline=deadline, stepped=False)
        if not _discover_step():
            out["cursor"], out["incomplete"] = cursor, True     # kinds first; the pass resumes at the same cursor
            return out
        known = _str_set(req.get("known_tokens"))
        state = _prewarm_state
        if cursor == 0 or state["ns"] != namespace or state["keys"] is None:
            dicts = _roots(namespace)
            keys, seen = [], set()
            for d in dicts:
                for name in list(d):
                    if type(name) is str and name not in seen:
                        seen.add(name)
                        keys.append(name)
            state.update(ns=namespace, keys=keys, dicts=dicts, exported=set())
        keys, dicts, exported = state["keys"], state["dicts"], state["exported"]
        out["count"] = len(keys)
        export_roots = len(keys) <= _MAX_ROOT_EXPORT
        i = min(cursor, len(keys))
        while i < len(keys):
            name = keys[i]
            i += 1
            value = _MISSING
            for d in dicts:
                value = dict.get(d, name, _MISSING)
                if value is not _MISSING:
                    break
            if value is not _MISSING:
                _prewarm_one(name, value, export_roots, known, exported, generation, out)
            if _clock() > deadline:
                break
        out["cursor"] = i if i < len(keys) else -1
        out["incomplete"] = out["cursor"] != -1
        if out["cursor"] == -1:
            state.update(ns=None, keys=None, dicts=None, exported=set())
    except _BadRequest:
        out["error"] = "bad request"
    except Exception as exc:  # noqa: BLE001
        out["error"] = "internal: " + _tname(type(exc))
    finally:
        _active.update(deadline=None, stepped=False)
    return out


def _prewarm_one(name, value, export_roots, known, exported, generation, out):
    target = _target(("val", value, None))
    if target is None:
        return
    tag, t, v = target
    token, type_name = _token(tag, t, v)
    if export_roots:
        out["roots"].append((name, _item(name, value, False)[1], token, type_name))
    if token in known or token in exported:
        return
    floor = _clock() + _LISTING_FLOOR_NS
    token, type_name, stable, items, done = _members(tag, t, v, generation, floor)
    if done:
        exported.add(token)
        out["members"].append([token, type_name, stable, items])


def unreal_names(req):
    """One idle slice of the unreal module's top-level names for FUnrealNameIndex (§7.5.3)."""
    out = {"items": [], "cursor": -1, "count": 0, "error": ""}
    try:
        _expire_gep(_wall())
        if type(req) is not dict:
            raise _BadRequest("request")
        start = _clock()
        m = _unreal()
        if m is None:
            return out
        d = _MOD_DICT.__get__(m)
        cursor = max(0, _int(req.get("cursor", 0)))
        deadline = _deadline(req, start, _PREWARM_BUDGET_NS)
        _active.update(deadline=deadline, stepped=False)
        if not _discover_step():
            out["cursor"] = cursor          # kinds first (they classify the names); the next slice resumes here
            return out
        if cursor == 0 or _unreal_pass["module"] is not m or _unreal_pass["keys"] is None:
            _unreal_pass["module"] = m
            _unreal_pass["keys"] = [name for name in list(d) if type(name) is str]
        keys = _unreal_pass["keys"]
        out["count"] = len(keys)
        i = min(cursor, len(keys))
        while i < len(keys):
            name = keys[i]
            i += 1
            value = dict.get(d, name, _MISSING)
            if value is not _MISSING:
                item = _item(name, value, False)
                if item[1] == "variable" and type(value) in _SCALARS:
                    item = (name, "constant", item[2], None, "")
                out["items"].append(item)
            if i % 64 == 0 and _clock() > deadline:
                break
        out["cursor"] = i if i < len(keys) else -1
        if out["cursor"] == -1:
            _unreal_pass["keys"] = None
    except _BadRequest:
        out["error"] = "bad request"
    except Exception as exc:  # noqa: BLE001
        out["error"] = "internal: " + _tname(type(exc))
    finally:
        _active.update(deadline=None, stepped=False)
    return out


# ---- Test hooks (tier 2) ---------------------------------------------------------------------------------------

def _test_register_kind(tp, kind, doc_reader=None):
    """Registers a fake unreal descriptor type (kinds: methodwithclosure, classmethodwithclosure, functionwithclosure,
    constant_value, enum_entry, enum_meta, property-getset); doc_reader(obj) returns its __doc__ (test kinds only)."""
    _test_kinds[:] = [(t, k) for t, k in _test_kinds if t is not tp]
    _test_kinds.append((tp, kind))
    if doc_reader is not None:
        _test_doc_readers[:] = [(t, r) for t, r in _test_doc_readers if t is not tp]
        _test_doc_readers.append((tp, doc_reader))


def _test_reset():
    _test_kinds.clear()
    _test_doc_readers.clear()
    _disc.update(module=None, kinds=[], walk=None, done=True)
    _gep_for.update(module=None, fn=None)
    _active.update(deadline=None, stepped=False)
    _frames.clear()
    _broken.clear()
    _gen.update(generation=None, epoch=None)
    _result_cache.clear()
    _member_cache.clear()
    _gep_cache.clear()
    _hard_cache.clear()
    _prewarm_state.update(ns=None, keys=None, dicts=None, exported=set())
    _unreal_pass.update(module=None, keys=None)


try:
    _capture_gep()      # §7.5.2: at import, before any later code can rebind unreal.Object.get_editor_property
except Exception:  # noqa: BLE001 - the resolver never raises; the first need captures it then
    pass
