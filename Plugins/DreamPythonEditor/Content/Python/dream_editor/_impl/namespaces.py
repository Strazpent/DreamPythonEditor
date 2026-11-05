# SPDX-License-Identifier: Apache-2.0
"""Session and Isolated namespaces, the last-run namespace and Reset Session (TDD §5.3.3, D8, E4, RT-15).

Session: UE's console globals dict, captured by ``_boot(globals(), generation)`` once per Python generation, with a
snapshot of its keys and values for Reset Session. Isolated: a fresh dict per run, kept as the last-run namespace
until the next run starts, Reset Session, a map change (``aliases.on_map_change``) or Python shutdown (the D3 release
rule).
"""

import builtins

__all__ = ["capture_session", "session", "session_generation", "new_isolated", "last_run", "set_last_run",
           "release_last_run", "add_reset_hook", "reset_session", "PROTECTED_KEY"]

# Reset Session never removes or restores this key (RT-15): the entry expression does not depend on it.
PROTECTED_KEY = "dream_editor"

_MISSING = object()

_state = {
    "session": None,        # the captured console dict
    "initial": None,        # {key: value} at first capture
    "generation": None,
    "last_run": None,
}

# Hooks other modules register to run inside Reset Session (the runner forgets its pseudo-file linecache entries).
_reset_hooks = []


def capture_session(console_globals, generation=0):
    """Capture the Session (§3.4 boot). A new generation, or a different dict, captures again (D16)."""
    if not isinstance(console_globals, dict):
        raise TypeError("the Session must be a dict (the console globals)")
    if (_state["session"] is console_globals and _state["generation"] == generation
            and _state["initial"] is not None):
        return console_globals
    _state["session"] = console_globals
    _state["initial"] = dict(console_globals)
    _state["generation"] = generation
    _state["last_run"] = None
    return console_globals


def session():
    """The captured Session dict, or None before ``_boot``."""
    return _state["session"]


def session_generation():
    return _state["generation"]


def new_isolated(file_path=None):
    """A fresh Isolated dict (§5.3.3): ``__name__`` '__main__', ``__file__`` for a file, the builtins, no doc or spec."""
    ns = {
        "__name__": "__main__",
        "__builtins__": builtins,
        "__doc__": None,
        "__package__": None,
        "__spec__": None,
    }
    if file_path:
        ns["__file__"] = file_path
    return ns


def last_run():
    """The last Isolated run's namespace (Variables view, post-mortem), or None once released."""
    return _state["last_run"]


def set_last_run(ns):
    _state["last_run"] = ns


def release_last_run():
    """Drop the reference only: functions defined there keep their globals (a registered callback still works)."""
    _state["last_run"] = None


def add_reset_hook(hook):
    if hook not in _reset_hooks:
        _reset_hooks.append(hook)


def reset_session():
    """Helper (§12.1.3, slice A choice A15): Reset Session. Returns the removed keys.

    Deletes every key that was not present at first capture, restores the captured value of every key that was, skips
    ``dream_editor`` entirely and keeps the dict object (RUN-10). Refused while a run is active (C++ refuses first).
    """
    from . import runner    # local: runner imports this module

    if runner.active_run_id():
        raise RuntimeError("Reset Session is not available while a run is active")
    ns = _state["session"]
    if ns is None:
        return []
    initial = _state["initial"] or {}
    removed = [key for key in list(ns) if key not in initial and key != PROTECTED_KEY]
    for key in removed:
        ns.pop(key, None)
    for key, value in initial.items():
        if key == PROTECTED_KEY:
            continue
        if ns.get(key, _MISSING) is not value:
            ns[key] = value
    # The aliases left with the other new keys; the alias module's reset hook clears its ownership table (U19, §5.3.8).
    release_last_run()
    for hook in list(_reset_hooks):
        try:
            hook()
        except Exception:  # noqa: BLE001 - a helper must finish the reset
            pass
    return removed
