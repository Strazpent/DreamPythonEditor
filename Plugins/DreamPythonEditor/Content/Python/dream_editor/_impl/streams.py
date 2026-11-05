# SPDX-License-Identifier: Apache-2.0
"""The run swaps (TDD §5.3.5, §6.1): install for one run, restore with compare-and-restore.

``StreamSwap`` owns the interpreter-global output slots: ``sys.stdout``, ``sys.stderr``, ``sys.displayhook``, the
``unreal.log*`` functions in the ``unreal`` module dict and ``warnings.showwarning``. The runner adds its own slots
(``time.sleep``, ``__dream_display__``, ``sys.argv``) through the same ``Swap`` machinery.

Compare-and-restore (D19, D25): at the end, a slot that still holds our object, or was deleted, gets its pre-run value
back; a slot the user replaced keeps the user's value and yields one footer note. Each restore step has its own
``try/except Exception``, so one failure never skips the others. Runs never nest (D14(4)), so the saved value is
always the pre-run value.
"""

import sys
import warnings

from . import compat
from . import native

__all__ = ["StreamSwap", "Swap", "AttrSlot", "KeySlot", "showwarning", "silenced_warning_display",
           "emergency_restore", "describe", "register_active", "unregister_active"]

_MISSING = object()

# Every object with a restore() between its install and the end of its restore (each Swap, and the runner's run
# swaps); emergency_restore() walks it (§5.2.4).
_ACTIVE = []


def register_active(obj):
    if obj not in _ACTIVE:
        _ACTIVE.append(obj)


def unregister_active(obj):
    try:
        _ACTIVE.remove(obj)
    except ValueError:
        pass

# The run that the module-level showwarning writes to: the last installed run. After the run a stale reference still
# writes there, and C++ tags the line background (§6.1 stale stream references).
_state = {"run_id": 0, "dream": None}


def describe(obj):
    """A short dotted name for a replacement, for footer notes: 'colorama.ansitowin32.StreamWrapper'."""
    named = isinstance(obj, type) or (callable(obj) and hasattr(obj, "__qualname__"))
    target = obj if named else type(obj)
    module = getattr(target, "__module__", None)
    name = getattr(target, "__qualname__", None) or getattr(target, "__name__", None) or repr(target)
    if module and module not in ("builtins", "__main__"):
        return f"{module}.{name}"
    return name


class AttrSlot:
    """An attribute of a module or object (``sys.stdout``, ``unreal.log``): a module attribute is its dict entry."""

    __slots__ = ("target", "name", "label")

    def __init__(self, target, name, label=None):
        self.target = target
        self.name = name
        self.label = label or f"{getattr(target, '__name__', type(target).__name__)}.{name}"

    def read(self):
        return getattr(self.target, self.name, _MISSING)

    def write(self, value):
        setattr(self.target, self.name, value)

    def remove(self):
        try:
            delattr(self.target, self.name)
        except AttributeError:
            pass


class KeySlot:
    """A key of a namespace dict (``ns['__dream_display__']``)."""

    __slots__ = ("mapping", "key", "label")

    def __init__(self, mapping, key, label=None):
        self.mapping = mapping
        self.key = key
        self.label = label or key

    def read(self):
        return self.mapping.get(self.key, _MISSING)

    def write(self, value):
        self.mapping[self.key] = value

    def remove(self):
        self.mapping.pop(self.key, None)


class Swap:
    """A group of slots installed together for one run and restored in reverse order."""

    def __init__(self, run_number=0):
        self.run_number = run_number
        self._entries = []          # [slot, saved value or _MISSING, our object]
        self._registered = False

    def put(self, slot, ours):
        """Install ``ours`` in ``slot``, remembering the pre-run value (absent slots are remembered as absent)."""
        if not self._registered:
            register_active(self)
            self._registered = True
        saved = slot.read()
        slot.write(ours)
        self._entries.append([slot, saved, ours])
        return ours

    def restore(self):
        """Compare-and-restore every slot; returns the footer notes for slots the user replaced."""
        notes = []
        while self._entries:
            slot, saved, ours = self._entries.pop()
            try:
                current = slot.read()
                if current is ours or current is _MISSING:
                    if saved is _MISSING:
                        slot.remove()
                    else:
                        slot.write(saved)
                else:
                    notes.append(f"{slot.label} was replaced during run #{self.run_number} by {describe(current)}; "
                                 "Dream Python Editor left it in place.")
            except Exception as exc:  # noqa: BLE001 - one failure must not skip the other slots (§5.3.5)
                notes.append(f"Dream Python Editor could not restore {slot.label}: {type(exc).__name__}: {exc}")
        self._unregister()
        return notes

    def _unregister(self):
        if self._registered:
            unregister_active(self)
            self._registered = False


class StreamSwap(Swap):
    """The output slots of §6.1 for one run: writers, displayhook, unreal.log shims and showwarning."""

    def __init__(self, run_id, run_number=0, dream=None):
        super().__init__(run_number)
        self.run_id = run_id
        self.dream = dream if dream is not None else native.get()

    def install(self):
        """Called by the runner after prepare and before user_code_begin (§5.3.5)."""
        from . import autoprint     # local: autoprint imports nothing back, but keeps this module's import light

        dream = self.dream
        _state["run_id"] = self.run_id
        _state["dream"] = dream
        self.put(AttrSlot(sys, "stdout", "sys.stdout"), dream.Writer(dream.STREAM_STDOUT, self.run_id))
        self.put(AttrSlot(sys, "stderr", "sys.stderr"), dream.Writer(dream.STREAM_STDERR, self.run_id))
        self.put(AttrSlot(sys, "displayhook", "sys.displayhook"), autoprint.displayhook)
        # RT-07: only when the unreal module is already loaded; the runner never imports it.
        unreal = sys.modules.get("unreal")
        if unreal is not None:
            for name, shim in (("log", dream.shim_log), ("log_warning", dream.shim_log_warning),
                               ("log_error", dream.shim_log_error)):
                if hasattr(unreal, name):
                    self.put(AttrSlot(unreal, name, f"unreal.{name}"), shim)
        self.put(AttrSlot(warnings, "showwarning", "warnings.showwarning"), showwarning)


def showwarning(message, category, filename, lineno, file=None, line=None):
    """The run's ``warnings.showwarning`` (§5.3.5): honours ``file=`` as the stdlib does, else one warning line.

    The engine's own ``_redirect_warning`` calls ``log_warning`` from ``_unreal_core`` directly, which would bypass the
    RT-07 shims and grow ``LogOutput``; this writes through the batch path instead.
    """
    text = warnings.formatwarning(message, category, filename, lineno, line)
    if file is not None:
        try:
            file.write(text)
        except OSError:
            pass
        return
    dream = _state["dream"] or native.get()
    dream.write(dream.STREAM_WARNING, _state["run_id"], text)


def _silent_showwarning(message, category, filename, lineno, file=None, line=None):
    return None


def _silent_showwarnmsg_impl(msg):
    return None


_SILENT = {"showwarning": _silent_showwarning, "_showwarnmsg_impl": _silent_showwarnmsg_impl}


class silenced_warning_display:
    """Silence warning display for the per-statement compiles of statement mode (§5.3.2).

    Sets ``warnings.showwarning`` and ``warnings._showwarnmsg_impl`` (the two slots ``catch_warnings(record=True)``
    swaps) to no-ops and restores them by compare-and-restore. ``warnings.filters`` is never touched:
    ``catch_warnings()`` would call ``_filters_mutated()`` and reset every once-per-location registry.
    """

    __slots__ = ("_saved",)

    def __enter__(self):
        self._saved = []
        for name, value in compat.warning_display_slots():
            self._saved.append((name, value))
            setattr(warnings, name, _SILENT[name])
        return self

    def __exit__(self, exc_type, exc, tb):
        for name, value in self._saved:
            try:
                if getattr(warnings, name, _MISSING) is _SILENT[name]:
                    setattr(warnings, name, value)
            except Exception:  # noqa: BLE001 - restoring the other slot still matters
                pass
        self._saved = []
        return False


def emergency_restore():
    """Helper (§5.2.4, §12.1.3): after ``ExecPythonCommandEx`` returned false, restore every slot still active.

    Compare-and-restore as at a normal end, without notes. Idempotent; never raises.
    """
    while _ACTIVE:
        swap = _ACTIVE[-1]
        try:
            swap.restore()
        except Exception:  # noqa: BLE001 - a helper must not raise
            pass
        unregister_active(swap)
