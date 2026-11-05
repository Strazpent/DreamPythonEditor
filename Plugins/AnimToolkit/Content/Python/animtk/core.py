# SPDX-License-Identifier: Apache-2.0
"""Editor plumbing every tool needs: logging, toasts, undo, paths, JSON files, ticking.

Nothing here knows about animation. ``animtk.seq`` and ``animtk.rig`` build on it.
"""

import contextlib
import json
import os
import time
import traceback

try:
    import unreal
except ImportError:  # headless tests import the pure parts without the editor
    unreal = None


class AnimTKError(RuntimeError):
    """An error meant for the animator: tools show the message instead of a traceback."""


# ----------------------------------------------------------------------------------------------------------- logging

def log(message):
    if unreal:
        unreal.log("[AnimToolkit] {}".format(message))
    else:
        print("[AnimToolkit] {}".format(message))


def warn(message):
    if unreal:
        unreal.log_warning("[AnimToolkit] {}".format(message))
    else:
        print("[AnimToolkit] WARNING: {}".format(message))


def error(message):
    if unreal:
        unreal.log_error("[AnimToolkit] {}".format(message))
    else:
        print("[AnimToolkit] ERROR: {}".format(message))


def notify(message, kind="info", seconds=3.0):
    """Editor toast. ``kind`` is "info", "success", "warning" or "error"."""
    {"warning": warn, "error": error}.get(kind, log)(message)
    if unreal and hasattr(unreal, "AnimTKLibrary"):
        unreal.AnimTKLibrary.notify(str(message), kind, float(seconds))


def safe_call(fn, *args, **kwargs):
    """Calls ``fn`` and reports any exception instead of raising: UI callbacks must never throw into Slate."""
    try:
        return fn(*args, **kwargs)
    except AnimTKError as exc:
        notify(str(exc), "warning")
    except Exception:  # noqa: BLE001 - a tool bug must not break the editor
        error("{} failed:\n{}".format(getattr(fn, "__qualname__", fn), traceback.format_exc()))
        notify("{} failed, see the Output Log".format(getattr(fn, "__name__", "tool")), "error")
    return None


# ----------------------------------------------------------------------------------------------------------- input

def modifiers():
    """The modifier keys held right now, as a set of "shift", "ctrl", "alt"."""
    if not unreal or not hasattr(unreal, "AnimTKLibrary"):
        return set()
    text = unreal.AnimTKLibrary.get_modifier_keys()
    return set(part for part in text.split(",") if part)


def clipboard_copy(text):
    if unreal and hasattr(unreal, "AnimTKLibrary"):
        unreal.AnimTKLibrary.clipboard_copy(str(text))


def clipboard_paste():
    if unreal and hasattr(unreal, "AnimTKLibrary"):
        return unreal.AnimTKLibrary.clipboard_paste()
    return ""


# ----------------------------------------------------------------------------------------------------------- undo

@contextlib.contextmanager
def undo(description="Anim Toolkit"):
    """One undo step for everything inside the block::

        with core.undo("Mirror pose"):
            ...
    """
    if unreal is None:
        yield
        return
    with unreal.ScopedEditorTransaction(description):
        yield


class Interaction(object):
    """An undo step that spans many events, for slider drags: ``begin()`` on press, ``end()`` on release."""

    def __init__(self, description):
        self.description = description
        self.active = False

    def begin(self):
        if self.active or unreal is None:
            return
        unreal.SystemLibrary.begin_transaction("AnimToolkit", self.description, None)
        self.active = True

    def end(self):
        if not self.active:
            return
        self.active = False
        unreal.SystemLibrary.end_transaction()


# ----------------------------------------------------------------------------------------------------------- editor

def editor_world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def actor_subsystem():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def selected_actors():
    return list(actor_subsystem().get_selected_level_actors())


def select_actors(actors):
    actor_subsystem().set_selected_level_actors(list(actors))


def load_asset(path):
    """Loads an asset by object or package path; raises AnimTKError when it does not exist."""
    asset = unreal.EditorAssetLibrary.load_asset(path) if path else None
    if asset is None:
        raise AnimTKError("Asset not found: {}".format(path))
    return asset


def asset_exists(path):
    return bool(path) and unreal.EditorAssetLibrary.does_asset_exist(path)


def open_asset(path):
    unreal.get_editor_subsystem(unreal.AssetEditorSubsystem).open_editor_for_assets([load_asset(path)])


# ----------------------------------------------------------------------------------------------------------- paths

def plugin_dir():
    """The AnimToolkit plugin folder (this file is Content/Python/animtk/core.py)."""
    return os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))


def resource(*parts):
    return os.path.join(plugin_dir(), "Resources", *parts)


def user_dir(*parts):
    """Per-project user data: ``ANIMTK_USER_DIR`` or ``<Project>/Saved/AnimToolkit``. Folders are created on demand."""
    base = os.environ.get("ANIMTK_USER_DIR")
    if not base:
        if unreal:
            base = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), "AnimToolkit")
        else:
            base = os.path.join(os.path.expanduser("~"), ".animtk")
    path = os.path.join(base, *parts)
    folder = path if not os.path.splitext(path)[1] else os.path.dirname(path)
    if folder and not os.path.isdir(folder):
        os.makedirs(folder)
    return path


def search_path(env_var, *default_parts):
    """Folders from an os.pathsep separated environment variable followed by the defaults (studio-wide data first)."""
    folders = [p for p in os.environ.get(env_var, "").split(os.pathsep) if p]
    folders.extend(default_parts)
    return [os.path.normpath(f) for f in folders if f]


# ----------------------------------------------------------------------------------------------------------- json

def read_json(path, default=None):
    if not os.path.isfile(path):
        return default
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError) as exc:
        warn("Could not read {}: {}".format(path, exc))
        return default


def write_json(path, data):
    """Writes atomically (temp file + rename), so a crash never leaves half a file."""
    folder = os.path.dirname(path)
    if folder and not os.path.isdir(folder):
        os.makedirs(folder)
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2, sort_keys=False)
    os.replace(temp, path)


def safe_filename(name):
    keep = "".join(c if (c.isalnum() or c in " -_.") else "_" for c in name).strip()
    return keep or "untitled"


# ----------------------------------------------------------------------------------------------------------- ticking

_ticks = {}
_tick_handle = None
_next_tick_id = [0]


def add_tick(fn, interval=0.0):
    """Calls ``fn()`` on the game thread every ``interval`` seconds (0 = every frame). Returns an id for remove_tick."""
    global _tick_handle
    _next_tick_id[0] += 1
    tick_id = _next_tick_id[0]
    _ticks[tick_id] = [fn, float(interval), 0.0]
    if _tick_handle is None and unreal is not None:
        _tick_handle = unreal.register_slate_post_tick_callback(_on_tick)
    return tick_id


def remove_tick(tick_id):
    global _tick_handle
    _ticks.pop(tick_id, None)
    if not _ticks and _tick_handle is not None:
        unreal.unregister_slate_post_tick_callback(_tick_handle)
        _tick_handle = None


def call_later(fn, delay):
    """Runs ``fn()`` once, ``delay`` seconds from now."""
    due = time.time() + delay
    holder = {}

    def _check():
        if time.time() >= due:
            remove_tick(holder["id"])
            safe_call(fn)

    holder["id"] = add_tick(_check, 0.1)
    return holder["id"]


def _on_tick(delta_seconds):
    now = time.time()
    for tick_id, entry in list(_ticks.items()):
        fn, interval, last = entry
        if now - last < interval:
            continue
        entry[2] = now
        try:
            fn()
        except Exception:  # noqa: BLE001
            error("Tick {} failed and was removed:\n{}".format(getattr(fn, "__qualname__", fn), traceback.format_exc()))
            _ticks.pop(tick_id, None)
