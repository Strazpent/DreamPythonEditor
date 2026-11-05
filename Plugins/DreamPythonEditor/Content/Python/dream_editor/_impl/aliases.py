# SPDX-License-Identifier: Apache-2.0
"""Selection aliases ``sel``, ``sel_assets``, ``world`` and ``subsys`` (TDD §5.3.8, §12.1.3, U19, UEI-03, D3).

``refresh(run_number)`` writes the aliases into the Session while the interpreter is idle: from ``runner.prepare`` in
Starting (§5.1.2 step 2) and from an idle tick after an editor selection change (FDreamPython, §3.7 "Alias refresh").
A name is written only when the key is absent, or still holds the object Dream Python Editor wrote last time (an ``is`` check against
``LAST_WRITTEN``). Any other value makes the name user-owned for the session: Dream Python Editor never touches it again, and the
ownership table keeps the run number at which Dream Python Editor found it rebound ("sel: your value since Run #N", VAR-02). Reset
Session clears the table and ``LAST_WRITTEN`` (a reset hook, §5.3.3).

``on_map_change()`` releases what Dream Python Editor wrote, so no alias keeps an old world alive, and the last-run namespace with it
(the D3 release rule, §3.8 UObject rules, T-LIFE-014). The next refresh writes fresh values.

What each alias holds (U19, U20):
- ``sel``: ``unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_selected_level_actors()``;
- ``sel_assets``: ``unreal.EditorUtilityLibrary.get_selected_asset_data()`` (asset data of the Content Browser);
- ``world``: ``unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()``;
- ``subsys``: ``subsys(cls)``, the editor subsystem of ``cls``, else its engine subsystem.

The values are read through the ``unreal`` module only when it is already loaded (the editor loads it at Python start);
this module never imports it, so plain CPython (tier 2 without a fake ``unreal``) gets no aliases.
"""

import sys
import time

from . import namespaces

__all__ = ["NAMES", "LAST_WRITTEN", "refresh", "on_map_change", "owned_since", "is_owned", "refreshed_at",
           "reset_for_test"]

NAMES = ("sel", "sel_assets", "world", "subsys")

# name -> the object Dream wrote last (the U19 identity check).
LAST_WRITTEN = {}

_MISSING = object()

# name -> the run number at which Dream found the name rebound (0: before any run, or by a foreign console).
_owned = {}

_state = {
    "refreshed_at": None,       # time.time() of the last refresh that read the editor ("selection as of <time>")
    "last_run_number": 0,       # the run whose prepare refreshed last: the one that may have rebound a name since
}


def subsys(cls):
    """The editor subsystem of class ``cls``, else its engine subsystem (U19 ``subsys(cls)``)."""
    unreal = sys.modules.get("unreal")
    if unreal is None:
        raise RuntimeError("subsys() needs the unreal module")
    getter = getattr(unreal, "get_editor_subsystem", None)
    result = getter(cls) if getter is not None else None
    if result is None:
        engine_getter = getattr(unreal, "get_engine_subsystem", None)
        if engine_getter is not None:
            result = engine_getter(cls)
    return result


def _read_sel(unreal):
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    return subsystem.get_selected_level_actors() if subsystem is not None else []


def _read_sel_assets(unreal):
    library = getattr(unreal, "EditorUtilityLibrary", None)
    if library is None:
        return []
    return library.get_selected_asset_data()


def _read_world(unreal):
    subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    return subsystem.get_editor_world() if subsystem is not None else None


def _read_subsys(_unreal):
    return subsys


_READERS = {
    "sel": _read_sel,
    "sel_assets": _read_sel_assets,
    "world": _read_world,
    "subsys": _read_subsys,
}


def _same(old, new):
    """True when ``new`` holds what ``old`` holds, so the alias keeps its object and nothing is rewritten.

    Element comparisons are between unreal wrappers (objects compare by pointer, structs natively), never user objects:
    ``old`` is an object Dream Python Editor itself wrote.
    """
    if old is new:
        return True
    if type(old) is not type(new):
        return False
    try:
        if isinstance(old, (str, bytes, int, float)) or old is None:
            return old == new
        try:
            old_items = list(old)
            new_items = list(new)
        except TypeError:
            return bool(old == new)
        if len(old_items) != len(new_items):
            return False
        return all(a is b or bool(a == b) for a, b in zip(old_items, new_items))
    except Exception:  # noqa: BLE001 - a failed comparison rewrites the alias
        return False


def refresh(run_number=0):
    """Helper (§12.1.3): write the aliases into the Session under the U19 rule. Returns how many names were written.

    ``run_number`` is the run being prepared (0 for an idle refresh after a selection change). Never raises for an
    editor read that fails: that alias keeps its value.
    """
    ns = namespaces.session()
    unreal = sys.modules.get("unreal")
    if ns is None or unreal is None:
        return 0
    written = 0
    for name in NAMES:
        if name in _owned:
            continue
        current = ns.get(name, _MISSING)
        ours = LAST_WRITTEN.get(name, _MISSING)
        if current is not _MISSING and current is not ours:
            # Rebound by user code (a run, the REPL, the Output Log console, Ctrl+Shift+V's line, U20): user-owned.
            _owned[name] = _state["last_run_number"]
            LAST_WRITTEN.pop(name, None)
            continue
        try:
            value = _READERS[name](unreal)
        except Exception:  # noqa: BLE001 - an editor read must never fail the prepare
            continue
        if current is not _MISSING and _same(current, value):
            continue
        ns[name] = value
        LAST_WRITTEN[name] = value
        written += 1
    _state["refreshed_at"] = time.time()
    if run_number:
        _state["last_run_number"] = int(run_number)
    return written


def on_map_change():
    """Helper (§12.1.3): a map change released the old world. Drops every alias that still holds Dream's object, and the
    last-run namespace (D3). User-owned names stay the user's. Returns how many names were released."""
    ns = namespaces.session()
    released = 0
    for name, value in list(LAST_WRITTEN.items()):
        if name == "subsys":
            continue        # a function: it holds no world
        if ns is not None and ns.get(name, _MISSING) is value:
            del ns[name]
            released += 1
        del LAST_WRITTEN[name]
    namespaces.release_last_run()
    return released


def owned_since(name):
    """The run number at which ``name`` became user-owned, or None while Dream Python Editor still refreshes it (VAR-02)."""
    return _owned.get(name)


def is_owned(name):
    return name in _owned


def refreshed_at():
    """``time.time()`` of the last refresh that read the editor, or None (hover: "selection as of <time>")."""
    return _state["refreshed_at"]


def _reset():
    # Reset Session (§5.3.3): the aliases left the Session with every other new key; the table starts over.
    _owned.clear()
    LAST_WRITTEN.clear()
    _state["last_run_number"] = 0


namespaces.add_reset_hook(_reset)


def reset_for_test():
    """Tier 2: forget the table, as a new Python generation does."""
    _reset()
    _state["refreshed_at"] = None
