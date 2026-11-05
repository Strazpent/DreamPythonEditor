# SPDX-License-Identifier: Apache-2.0
"""Cooperative API (TDD §5.10.1-§5.10.3, M1): progress, check_cancel and the two sliced sleeps.

``sliced_sleep`` replaces ``time.sleep`` for a run (§5.3.5). On the game thread during a run it sleeps in slices of at
most 50 ms by calling the pre-run ``time.sleep``, so pending calls, pumps and Stop run between slices; it never raises
``ScriptCancelled`` itself, so a ``finally`` block that sleeps during cleanup is not interrupted again (RT-06). On
other threads, or outside a run, it calls the original.

``sleep`` is the public ``dream_editor.sleep`` (§12.2): slices of at most 50 ms on any thread, and an explicit Stop check
between slices that raises ``ScriptCancelled``.

``check_cancel`` follows the §5.10.2 table: on the game thread it raises only where a delivery may land; when a deferral
condition holds it returns True instead, so the caller can return early (A19). The conditions are the Python import
machinery on the stack (an import hook or a module body run by ``import``, found here) and the §5.4.3 C++ conditions
(saving a package, GC, PostLoad, native re-entry, outside the user-code window, inside a Dream Python Editor pump), which
``_dream.check_cancel(True)`` evaluates.

Deferred (M3): the ``@task`` decorator, ``Task`` handles and ``_tasks.step`` (§5.10.4).
"""

import math
import operator
import sys
import threading
import time

from . import native

__all__ = ["SLICE_SECONDS", "sliced_sleep", "sleep", "progress", "check_cancel", "new_cancelled",
           "set_game_thread", "on_game_thread", "begin_run", "end_run", "validate_seconds", "set_cancel_class",
           "cancel_class"]

SLICE_SECONDS = 0.05            # below the 100 ms pump period at 10 Hz (§5.10.3)

_perf_counter = time.perf_counter
_get_ident = threading.get_ident


def _unwrap(fn):
    while getattr(fn, "__dream_shim__", False):
        fn = fn.__wrapped__
    return fn


_ORIGINAL_SLEEP = _unwrap(time.sleep)

# Largest sleep the C function accepts: _PyTime_t holds nanoseconds in an int64.
_MAX_SECONDS = (2 ** 63 - 1) / 1e9

_state = {
    "game_ident": None,         # threading.get_ident() of the game thread, recorded at _boot
    "run_id": 0,                # the active run (0 between runs)
    "run_number": 0,
    "pre_run_sleep": None,      # the time.sleep the run's swap replaced
}


def set_game_thread(ident=None):
    """Record the game thread (``_boot`` runs on it). Before boot the main thread stands in."""
    _state["game_ident"] = _get_ident() if ident is None else ident


def on_game_thread():
    ident = _state["game_ident"]
    if ident is None:
        ident = threading.main_thread().ident
    return _get_ident() == ident


def begin_run(run_id, run_number, pre_run_sleep):
    _state["run_id"] = run_id
    _state["run_number"] = run_number
    _state["pre_run_sleep"] = _unwrap(pre_run_sleep) if callable(pre_run_sleep) else None


def end_run():
    _state["run_id"] = 0
    _state["pre_run_sleep"] = None


def active_run_number():
    return _state["run_number"] if _state["run_id"] else 0


def validate_seconds(seconds):
    """Validate a sleep length as ``time.sleep`` does on 3.11; returns it as a number of seconds."""
    if isinstance(seconds, float):
        value = float(seconds)
        if math.isnan(value):
            raise ValueError("Invalid value NaN (not a number)")
    else:
        value = operator.index(seconds)     # the TypeError text matches time.sleep's
    if value < 0:
        raise ValueError("sleep length must be non-negative")
    if value > _MAX_SECONDS:
        _ORIGINAL_SLEEP(value)              # raises the engine Python's own OverflowError without sleeping
        raise OverflowError("timestamp too large to convert to C _PyTime_t")
    return value


def sliced_sleep(seconds):
    value = validate_seconds(seconds)
    original = _state["pre_run_sleep"] or _ORIGINAL_SLEEP
    if not _state["run_id"] or not on_game_thread():
        return original(seconds)
    deadline = _perf_counter() + value
    remaining = value
    while True:
        original(remaining if remaining < SLICE_SECONDS else SLICE_SECONDS)
        remaining = deadline - _perf_counter()
        if remaining <= 0:
            return None


sliced_sleep.__name__ = "sleep"
sliced_sleep.__qualname__ = "sleep"
sliced_sleep.__doc__ = _ORIGINAL_SLEEP.__doc__
sliced_sleep.__wrapped__ = _ORIGINAL_SLEEP
sliced_sleep.__dream_shim__ = True


_cancel = {"cls": None}


def set_cancel_class(cls):
    """The ``ScriptCancelled`` class the package registered with ``_dream`` (at import and at ``_boot``, §5.7.1)."""
    _cancel["cls"] = cls


def cancel_class():
    """The registered ``ScriptCancelled``; held here so classification never depends on ``sys.modules``."""
    cls = _cancel["cls"]
    if cls is None:
        cls = getattr(sys.modules.get("dream_editor"), "ScriptCancelled", None)
    return cls


def new_cancelled():
    """A ``ScriptCancelled`` raised from Python (check_cancel, sleep): it carries the active run's number (§5.7.1)."""
    exc = (cancel_class() or KeyboardInterrupt)()
    try:
        exc.run_number = active_run_number()
    except AttributeError:
        pass
    return exc


# co_filename of the frozen import machinery on 3.11 (importlib._bootstrap and _bootstrap_external).
_IMPORT_FILENAMES = frozenset({"<frozen importlib._bootstrap>", "<frozen importlib._bootstrap_external>"})
_IMPORT_WALK_LIMIT = 64          # frames looked at, as the §5.4.7 frame walk


def _inside_import(frame):
    """True when the import machinery is on the stack: an import hook, or a module body that ``import`` is running."""
    depth = 0
    while frame is not None and depth < _IMPORT_WALK_LIMIT:
        if frame.f_code.co_filename in _IMPORT_FILENAMES:
            return True
        frame = frame.f_back
        depth += 1
    return False


def _deliverable(dream):
    """Stop is pending: may ScriptCancelled be raised here? Off the game thread always (the caller decides, §5.10.2);
    on it only when no deferral condition holds. A True from ``check_cancel(True)`` counts as a delivery in C++."""
    if not on_game_thread():
        return True
    if _inside_import(sys._getframe(2)):
        return False
    return bool(dream.check_cancel(True))


def check_cancel():
    """§5.10.2: raises ScriptCancelled on the game thread when Stop is pending and a delivery may land there; returns True
    when a deferral condition holds (inside an import hook, native re-entry, ...), and the bool on other threads.

    ``_dream.check_cancel()`` never raises (A19). Off the game thread the result is returned and never raised.
    """
    dream = native.get()
    if not dream.check_cancel():
        return False
    if not on_game_thread():
        return True
    if _deliverable(dream):
        raise new_cancelled()
    return True


def sleep(seconds):
    value = validate_seconds(seconds)
    dream = native.get()
    deadline = _perf_counter() + value
    while True:
        # Any thread raises on Stop (§12.2); on the game thread not while a deferral condition holds (§5.10.2), where the
        # sleep goes on and a later check (or the pending call, §5.7.3) delivers.
        if dream.check_cancel() and _deliverable(dream):
            raise new_cancelled()
        remaining = deadline - _perf_counter()
        if remaining <= 0:
            return None
        _ORIGINAL_SLEEP(remaining if remaining < SLICE_SECONDS else SLICE_SECONDS)


def progress(i, n=None, msg=""):
    """§5.10.1: report i of n items done; n None is an indeterminate bar (U12). Any thread; a no-op outside a run."""
    i = operator.index(i)
    if n is not None:
        n = operator.index(n)
    if not isinstance(msg, str):
        msg = str(msg)
    native.get().progress(i, n, msg)
