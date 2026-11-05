# SPDX-License-Identifier: Apache-2.0
"""The Stage 1 snapshot as compact indexes for the rules (TDD §7.3.3 "Unreal data", §7.4.2, RT-13).

The snapshot lives in ``<Project>/Intermediate/Dream/Stubs/<key>/snapshot``: ``manifest.json``, ``modules/*.jsonl`` and
the ``.complete`` marker, written last. It is usable only when the manifest parses, has format ``dream.stubsnap`` and
schema 1, says ``complete``, the marker exists and, when the server was given ``--key``, the key matches. Otherwise
SUM101-SUM103 are off and SUM900 says why. Everything is loaded lazily, on the first analysis that needs it.

Indexes: the top-level ``unreal`` names; per type its kind, bases (followed by name) and own editor properties; per
class the members and their kinds (raw records, parsed on first use); deprecation markers; parsed return types; the
reverse indexes SUM102 needs for an unknown class (where a name is edit-only, which names are attributes anywhere).
"""

import json
import os
import threading

from .docs import docstrings

FORMAT = "dream.stubsnap"
SCHEMA = 1

OK = "ok"
BUILDING = "building"
UNAVAILABLE = "unavailable"

#: Names every Python type has: never reported missing on an ``unreal`` class (SUM101 level 2).
_TYPE_ATTRIBUTES = frozenset(dir(type)) | frozenset(dir(object))


class MemberInfo:
    __slots__ = ("name", "kind", "doc", "_property", "_method", "_parsed")

    def __init__(self, name, kind, doc):
        self.name = name
        self.kind = kind
        self.doc = doc or ""
        self._parsed = False
        self._property = None
        self._method = None

    def _parse(self):
        if self._parsed:
            return
        self._parsed = True
        if self.kind == "getset":
            self._property = docstrings.parse_property_doc(self.doc)
        elif self.kind in ("method", "classmethod"):
            self._method = docstrings.parse_method_doc(self.doc)
        elif self.kind == "glue":
            glue = docstrings.parse_glue_doc(self.doc)
            if glue is not None:
                self._method = docstrings.MethodDoc(False, glue.name, glue.params, glue.returns, (), False, glue.text,
                                                    "", None)

    @property
    def property_doc(self):
        self._parse()
        return self._property

    @property
    def method_doc(self):
        self._parse()
        return self._method

    @property
    def returns(self):
        """The return type text of a method or glue member, or None."""
        method = self.method_doc
        if method is None or method.return_items:
            return None
        return method.returns

    @property
    def deprecated(self):
        """``(message, new_name or None)`` when the member's doc marks it deprecated, else None."""
        message = docstrings.deprecation_message(self.doc)
        if message is None:
            return None
        renamed = docstrings.parse_deprecated(self.doc)
        return message, (renamed[1] if renamed else None)


class TypeInfo:
    __slots__ = ("name", "kind", "bases", "uobject", "doc", "eprops", "_raw_members", "_members")

    def __init__(self, record):
        self.name = record.get("name", "")
        self.kind = record.get("kind", "glue")
        self.bases = tuple(b for b in record.get("bases") or () if isinstance(b, str))
        self.uobject = bool(record.get("uobject"))
        self.doc = record.get("doc") or ""
        self.eprops = {}
        for entry in record.get("eprops") or ():
            if isinstance(entry, list) and len(entry) >= 3 and isinstance(entry[0], str):
                self.eprops[entry[0]] = (entry[1] if isinstance(entry[1], str) else "", entry[2] == "ro")
        self._raw_members = record.get("members") or ()
        self._members = None

    @property
    def members(self):
        """Own members by name (MemberInfo), built on first use."""
        if self._members is None:
            members = {}
            for raw in self._raw_members:
                if isinstance(raw, dict) and isinstance(raw.get("n"), str):
                    members[raw["n"]] = MemberInfo(raw["n"], raw.get("k", "other"), raw.get("doc"))
            self._members = members
            self._raw_members = ()
        return self._members

    @property
    def deprecated(self):
        message = docstrings.deprecation_message(self.doc)
        if message is None:
            return None
        renamed = docstrings.parse_deprecated(self.doc)
        return message, (renamed[1] if renamed else None)


class SnapshotIndex:
    """One snapshot directory. Thread-safe loading; queries after ``status() == OK``."""

    def __init__(self, path=None, key=None):
        self._lock = threading.Lock()
        self.reset(path, key)

    def reset(self, path, key=None):
        with getattr(self, "_lock", threading.Lock()):
            self.path = path
            self.key = key
            self._status = None
            self._loaded = False
            self.types = {}
            self.funcs = {}             # module function name -> doc
            self.data = set()
            self._edit_only = None      # prop -> sorted classes where it is an editor property but not an attribute
            self._attribute_names = None
            self._mro_cache = {}
            self._deprecated_names = None
            self._manifest = None
            self.load_error = None

    # ---- validity -----------------------------------------------------------------------------------------------

    def status(self):
        """OK, BUILDING (the folder exists but is incomplete) or UNAVAILABLE. Cached until reset()."""
        if self._status is None:
            self._status = self._check()
        return self._status

    def _check(self):
        path = self.path
        if not path or not os.path.isdir(path):
            return UNAVAILABLE
        manifest_path = os.path.join(path, "manifest.json")
        try:
            with open(manifest_path, "r", encoding="utf-8") as handle:
                manifest = json.load(handle)
        except FileNotFoundError:
            return BUILDING
        except (OSError, ValueError):
            return UNAVAILABLE
        if not isinstance(manifest, dict) or manifest.get("format") != FORMAT or manifest.get("schema") != SCHEMA:
            return UNAVAILABLE
        if self.key and manifest.get("key") != self.key:
            return UNAVAILABLE
        if manifest.get("complete") is not True or not os.path.isfile(os.path.join(path, ".complete")):
            return BUILDING
        self._manifest = manifest
        return OK

    def usable(self):
        if self.status() != OK:
            return False
        self._ensure_loaded()
        return self._loaded and self.load_error is None

    # ---- loading ------------------------------------------------------------------------------------------------

    def _ensure_loaded(self):
        if self._loaded:
            return
        with self._lock:
            if self._loaded:
                return
            try:
                files = [entry.get("file") for entry in self._manifest.get("modules") or () if isinstance(entry, dict)]
                files.append(self._manifest.get("unreal_module") or "modules/__unreal__.jsonl")
                for relative in files:
                    if isinstance(relative, str):
                        self._load_file(os.path.join(self.path, relative))
            except (OSError, ValueError) as error:
                self.load_error = "%s: %s" % (type(error).__name__, error)
            self._loaded = True

    def _load_file(self, path):
        if not os.path.isfile(path):
            return
        with open(path, "r", encoding="utf-8") as handle:
            for raw in handle:
                raw = raw.strip()
                if not raw:
                    continue
                record = json.loads(raw)
                if not isinstance(record, dict):
                    continue
                kind = record.get("rec")
                name = record.get("name")
                if not isinstance(name, str):
                    continue
                if kind == "type":
                    self.types.setdefault(name, TypeInfo(record))
                elif kind == "func":
                    self.funcs.setdefault(name, record.get("doc") or "")
                elif kind == "data":
                    self.data.add(name)

    # ---- queries ------------------------------------------------------------------------------------------------

    def has_name(self, name):
        return name in self.types or name in self.funcs or name in self.data

    def top_names(self):
        return list(self.types) + list(self.funcs) + list(self.data)

    def type_info(self, name):
        return self.types.get(name)

    def is_class(self, name):
        return name in self.types

    def mro(self, name):
        """(names in base order, complete) where complete is False when a base is not in the snapshot."""
        cached = self._mro_cache.get(name)
        if cached is not None:
            return cached
        order = []
        complete = True
        seen = set()
        stack = [name]
        while stack:
            current = stack.pop(0)
            if current in seen:
                continue
            seen.add(current)
            info = self.types.get(current)
            if info is None:
                if current not in ("object", "type"):
                    complete = False
                continue
            order.append(current)
            stack.extend(info.bases)
        result = (order, complete)
        self._mro_cache[name] = result
        return result

    def is_subclass(self, name, base):
        order, _ = self.mro(name)
        return base in order

    def member(self, class_name, member):
        """The MemberInfo of ``member`` on ``class_name`` or its bases, or None."""
        order, _ = self.mro(class_name)
        for current in order:
            found = self.types[current].members.get(member)
            if found is not None:
                return found
        return None

    def has_member(self, class_name, member):
        """True, False, or None when the answer is unknown (an unresolved base)."""
        if member in _TYPE_ATTRIBUTES or member.startswith("__"):
            return True
        order, complete = self.mro(class_name)
        for current in order:
            if member in self.types[current].members:
                return True
        return False if complete else None

    def member_names(self, class_name):
        order, _ = self.mro(class_name)
        names = []
        for current in order:
            names.extend(self.types[current].members)
        return names

    def eprops(self, class_name):
        """Own and inherited editor properties: name -> (type text, read_only)."""
        order, _ = self.mro(class_name)
        merged = {}
        for current in reversed(order):
            merged.update(self.types[current].eprops)
        return merged

    def property_type(self, class_name, prop):
        """The type text of an attribute or editor property, or None."""
        member = self.member(class_name, prop)
        if member is not None and member.kind == "getset" and member.property_doc is not None:
            return member.property_doc.type
        entry = self.eprops(class_name).get(prop)
        return entry[0] if entry else None

    def method_returns(self, class_name, method):
        member = self.member(class_name, method)
        return member.returns if member is not None else None

    def func_returns(self, name):
        doc = self.funcs.get(name)
        if not doc:
            return None
        glue = docstrings.parse_glue_doc(doc)
        if glue is not None:
            return glue.returns
        method = docstrings.parse_method_doc(doc)
        if method is not None and not method.return_items:
            return method.returns
        return None

    def deprecated_type(self, name):
        info = self.types.get(name)
        return info.deprecated if info is not None else None

    def deprecated_func(self, name):
        """``(message, new_name or None)`` when a module-level function's doc marks it deprecated (SUM103), else None."""
        doc = self.funcs.get(name)
        if not doc:
            return None
        message = docstrings.deprecation_message(doc)
        if message is None:
            return None
        renamed = docstrings.parse_deprecated(doc)
        return message, (renamed[1] if renamed else None)

    def deprecated_member(self, class_name, member):
        found = self.member(class_name, member)
        return found.deprecated if found is not None else None

    def deprecated_member_names(self):
        """Every member name some class marks deprecated (a cheap filter before inference, SUM103)."""
        if self._deprecated_names is None:
            names = set()
            for info in self.types.values():
                for name, member in info.members.items():
                    if "deprecated:" in member.doc:
                        names.add(name)
            self._deprecated_names = names
        return self._deprecated_names

    def _build_reverse(self):
        if self._edit_only is not None:
            return
        edit_only = {}
        attributes = set()
        for name, info in self.types.items():
            for member in info.members:
                attributes.add(member)
        for name, info in self.types.items():
            if not info.eprops:
                continue
            for prop in info.eprops:
                found = self.member(name, prop)
                if found is None or found.kind != "getset":
                    edit_only.setdefault(prop, set()).add(name)
        self._edit_only = {prop: sorted(classes) for prop, classes in edit_only.items()}
        self._attribute_names = attributes

    def edit_only_classes(self, prop):
        """Classes whose editor property ``prop`` is not a Python attribute (sorted)."""
        self._build_reverse()
        return self._edit_only.get(prop, [])

    def is_attribute_anywhere(self, prop):
        self._build_reverse()
        return prop in self._attribute_names
