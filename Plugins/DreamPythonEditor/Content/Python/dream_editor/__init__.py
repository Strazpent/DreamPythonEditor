# SPDX-License-Identifier: Apache-2.0
"""Dream Python Editor public API (TDD §12.2, D29), M1 subset.

Run entry and lifecycle hooks for DreamEditor (C++) are private: ``_boot(globals(), generation)`` (§3.4),
``_run(run_id)`` (§5.3.1, RT-15) and ``_shutdown(generation)`` (§3.8). The entry expression is
``__import__('dream_editor')._run(<id>)``, which needs no name in the Session, so ``del dream_editor``, rebinding it or
``globals().clear()`` cannot break later runs; nothing here writes ``dream_editor`` into the Session.

``_dream`` is bound lazily (``_impl.native``), so a studio ``init_unreal.py`` that imports this package before
DreamEditor created the native module still works; editor-only calls raise until then.
"""

import os as _os
import sys as _sys

from ._version import ABI_VERSION as _ABI_VERSION
from ._version import __version__


class ScriptCancelled(KeyboardInterrupt):
    """Raised in the running script when the user presses Stop (D2).

    A ``KeyboardInterrupt``, so ``except Exception`` does not swallow it and asyncio and unittest re-raise it (E5).
    Each delivery creates a new instance with ``run_number`` (the run number #N; 0 from the alternate channel),
    ``source`` ("Stop button", "Esc", "Shift+F5", ...; "async" by default) and ``delivery`` (1 for the first) set.
    """

    run_number = 0
    source = "async"
    delivery = 0


from ._impl import native as _native  # noqa: E402 - ScriptCancelled must exist before the internals import

# §12.2 ABI check at import (the null module of plain CPython passes; _boot checks again once _dream exists).
_native.check_abi(_ABI_VERSION)

from ._impl import task as _task  # noqa: E402

# §5.7.1: register ScriptCancelled at import (and again at _boot, once the real _dream exists), so a fresh import of the
# package keeps C++ deliveries and the runner's "ours" check on the same class.
_task.set_cancel_class(ScriptCancelled)
try:
    _native.get().register_cancel_class(ScriptCancelled)
except RuntimeError:
    pass    # imported off the game thread; _boot registers it on the game thread

# M1 subset of §12.2. Deferred (M3): task (§5.10.4) and revert_module_state (§9.2).
__all__ = ["ScriptCancelled", "open", "new_tab", "clear_output", "run", "suppress", "progress", "check_cancel",
           "sleep"]


def __dir__():
    """``dir(dream_editor)`` is ``__all__`` plus the dunders (T-API-007)."""
    names = set(__all__)
    names.update(name for name in globals() if name.startswith("__") and name.endswith("__"))
    return sorted(names)


def _require_game_thread(name):
    # §11.7 thread check; C++ checks again with the identity captured at _dream creation.
    if not _task.on_game_thread():
        raise RuntimeError(f"dream_editor.{name}() must be called on the game thread")


def _index_or_zero(value, name):
    if value is None:
        return 0
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an int or None, not {type(value).__name__}")
    if value < 0:
        raise ValueError(f"{name} must be 1-based (or None), not {value}")
    return value


def _text_path(path, name):
    path = _os.fspath(path)
    if isinstance(path, bytes):
        path = _os.fsdecode(path)
    if not path:
        raise ValueError(f"dream_editor.{name}(): the path is empty")
    return path


def open(path, line=None, column=None):  # noqa: A001 - the §12.2 name
    """Open or focus a file in a Dream tab and place the caret. Returns the tab ID. GT only."""
    _require_game_thread("open")
    return _native.get().open_tab(_text_path(path, "open"), _index_or_zero(line, "line"),
                                  _index_or_zero(column, "column"))


def new_tab(text="", name=None):
    """Create a scratch tab. Returns the tab ID. GT only."""
    _require_game_thread("new_tab")
    if not isinstance(text, str):
        raise TypeError(f"text must be str, not {type(text).__name__}")
    if name is not None and not isinstance(name, str):
        raise TypeError(f"name must be str or None, not {type(name).__name__}")
    return _native.get().new_tab(text, name or "")


def clear_output():
    """Clear the Dream output view. GT only; works during a run."""
    _require_game_thread("clear_output")
    _native.get().clear_output()


def run(target, *, namespace=None, transaction=None):
    """Queue a run of a tab (ID or name) or a file; return its number. RuntimeError inside a run. GT only."""
    _require_game_thread("run")
    if namespace is not None and namespace not in ("session", "isolated"):
        raise ValueError(f"namespace must be 'session', 'isolated' or None, not {namespace!r}")
    if transaction is not None and not isinstance(transaction, bool):
        raise TypeError(f"transaction must be a bool or None, not {type(transaction).__name__}")
    dream = _native.get()
    if dream.current_run_id():
        raise RuntimeError("dream_editor.run() cannot start a run inside a Dream run: one run at a time")
    import json

    spec = {"target": _text_path(target, "run"), "namespace": namespace, "transaction": transaction}
    return dream.enqueue_run(json.dumps(spec))


def suppress(*, results=None, info=None, warnings=None, errors=None, echo=None):
    """Hide or show output kinds, like Maya's scriptEditorInfo -suppress* flags.

    For every keyword, echo included, True suppresses (hides) that kind, False shows it and None leaves it as it is,
    so ``suppress(echo=True)`` turns the Echo input toggle (OUT-12) off. Returns the previous state, same polarity.
    GT only.
    """
    _require_game_thread("suppress")
    flags = {}
    for key, value in (("results", results), ("info", info), ("warnings", warnings), ("errors", errors),
                       ("echo", echo)):
        if value is None:
            continue
        if not isinstance(value, bool):
            raise TypeError(f"suppress(): {key} must be a bool or None, not {type(value).__name__}")
        flags[key] = value
    return dict(_native.get().suppress(flags))


def progress(i, n=None, msg=""):
    """Report i of n items done (U12); n None is an indeterminate bar. Any thread; a no-op outside a run."""
    _task.progress(i, n, msg)


def check_cancel():
    """Raise ScriptCancelled if Stop was requested for this run (game thread); else return whether it was.

    Off the game thread, and on it where Stop cannot be delivered (inside an import hook or a module body run by
    ``import``, while the editor saves, collects garbage or loads, or inside a native callback), it returns True instead
    of raising, so the caller can return early (§5.10.2).
    """
    return _task.check_cancel()


def sleep(seconds):
    """Sleep in slices of at most 50 ms; raise ScriptCancelled on Stop (D2). Any thread."""
    _task.sleep(seconds)


# ---- C++ entry points (§3.4, §3.6, §5.3.1, §12.1.3) -------------------------------------------------------------

# The plugin descriptor next to Content/Python: "this plugin" for the package check of §2.7 (ENV-07).
_PLUGIN_DESCRIPTOR = "DreamPythonEditor.uplugin"


def _foreign_package_dir():
    """None when this package is the plugin's own copy (<plugin>/Content/Python/dream_editor), else its folder.

    A copy found earlier on ``sys.path`` (a project's Content/Python, a studio folder) has no DreamPythonEditor.uplugin
    three folders up, so boot refuses it and names its path (§2.7 "Our package", ENV-07, T-ARCH-017).
    """
    package_dir = _os.path.dirname(_os.path.abspath(__file__))
    plugin_dir = _os.path.dirname(_os.path.dirname(_os.path.dirname(package_dir)))
    if _os.path.isfile(_os.path.join(plugin_dir, _PLUGIN_DESCRIPTOR)):
        return None
    return package_dir


def _boot_refused(reason):
    """Writes why boot failed to stderr, which reaches LogPython in the editor, and returns False (ruling R3)."""
    stream = _sys.stderr if _sys.stderr is not None else _sys.__stderr__
    if stream is not None:
        try:
            stream.write(f"Dream Python Editor did not start: {reason}\n")
        except Exception:  # noqa: BLE001 - the False return still reaches FDreamPython::Boot
            pass
    return False


def _boot(console_globals, generation=0):
    """Boot (§3.4): ABI check, package check, register ScriptCancelled, record the game thread, capture the Session.

    Returns exactly ``True`` on success, which FDreamPython::Boot reads as the EvaluateStatement repr "True" (slice A
    ruling R3). An ABI mismatch raises ``ImportError`` with the reinstall text (§12.2, T-API-005); any other failure
    (a shadowing package, an import or capture error) is written to stderr and returns ``False``.
    """
    _native.check_abi(_ABI_VERSION)
    try:
        foreign = _foreign_package_dir()
        if foreign is not None:
            return _boot_refused(f"another dream_editor package shadows Dream's own on sys.path: {foreign}")
        from ._impl import namespaces
        from ._impl import runner  # noqa: F401 - the runner core imports at boot, not at the first run

        dream = _native.get()
        dream.register_cancel_class(ScriptCancelled)
        _task.set_cancel_class(ScriptCancelled)
        _task.set_game_thread()
        namespaces.capture_session(console_globals, generation)
    except Exception as exc:  # noqa: BLE001 - R3: every failure but the ABI mismatch is a False return
        import traceback

        return _boot_refused(f"{type(exc).__name__}: {exc}\n{traceback.format_exc().rstrip()}")
    return True


def _run(run_id):
    """The one run entry (§5.3.1). Never raises (INV-4), so RunString always processes reinstancing (E9)."""
    try:
        from ._impl import runner

        runner.run(run_id)
    except BaseException:  # noqa: BLE001 - last resort; only reached on a runner bug
        try:
            _native.get().user_code_done(run_id)
            from ._impl import runner

            runner.report_internal_error(run_id)
        except BaseException:  # noqa: BLE001
            pass


def _shutdown(generation=0):
    """OnPythonShutdown (§3.8), best effort: restore any swap, forget prepared requests and the last-run namespace."""
    try:
        from ._impl import namespaces
        from ._impl import runner
        from ._impl import streams

        streams.emergency_restore()
        runner.shutdown()
        namespaces.release_last_run()
    except BaseException:  # noqa: BLE001
        pass
