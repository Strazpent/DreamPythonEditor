# SPDX-License-Identifier: Apache-2.0
"""The script shelf: tabs of buttons that run Python, like Maya's shelves.

* A button runs inline Python (``command``) or a ``.py`` file (``script``) as one undo step.
* Drop ``.py`` files from Explorer onto a shelf to add them; drop Content Browser assets to add "open asset" buttons.
* Right-click a button to edit, move, duplicate or delete it; right-click the tab row for shelf commands.
* Your shelves live in ``<Project>/Saved/AnimToolkit/shelves.json`` (seeded from ``Resources/Shelves``). Studio shelves
  are read from every folder in ``ANIMTK_SHELF_PATH`` (``*.json``, one shelf per file) and are read-only.

Scripts add buttons too::

    import animtk.shelf as shelf
    shelf.add_button("Animation", "Key", command="import animtk.ops as ops; ops.key_selected()", icon="Icons.Plus")
"""

import copy
import glob
import os
import runpy
import sys
import traceback

from . import core, ui

unreal = core.unreal

VERSION = 1


# ------------------------------------------------------------------------------------------------------------- model

class ShelfStore(object):
    """Loads and saves shelves. Pure Python apart from the paths, so it is unit tested headless."""

    def __init__(self, user_file=None, default_file=None, studio_dirs=None):
        self.user_file = user_file or core.user_dir("shelves.json")
        self.default_file = default_file or core.resource("Shelves", "default_shelves.json")
        self.studio_dirs = studio_dirs if studio_dirs is not None else core.search_path("ANIMTK_SHELF_PATH")
        self.data = {"version": VERSION, "active": None, "shelves": []}
        self.studio = []
        self.load()

    # Loading ---------------------------------------------------------------------------------------------------------
    def load(self):
        data = core.read_json(self.user_file)
        if data is None:
            data = core.read_json(self.default_file, {"version": VERSION, "shelves": []})
            data = copy.deepcopy(data)
        data.setdefault("version", VERSION)
        data.setdefault("shelves", [])
        for shelf in data["shelves"]:
            shelf.setdefault("buttons", [])
        self.data = data
        self.studio = []
        for folder in self.studio_dirs:
            for path in sorted(glob.glob(os.path.join(folder, "*.json"))):
                shelf = core.read_json(path)
                if isinstance(shelf, dict) and shelf.get("name"):
                    shelf.setdefault("buttons", [])
                    shelf["read_only"] = True
                    shelf["source"] = path
                    self.studio.append(shelf)
        if not self.data.get("active") or self.find(self.data["active"]) is None:
            names = self.names()
            self.data["active"] = names[0] if names else None

    def save(self):
        core.write_json(self.user_file, self.data)

    # Queries ---------------------------------------------------------------------------------------------------------
    def shelves(self):
        return list(self.data["shelves"]) + list(self.studio)

    def names(self):
        return [s["name"] for s in self.shelves()]

    def find(self, name):
        for shelf in self.shelves():
            if shelf["name"] == name:
                return shelf
        return None

    def require(self, name, writable=True):
        shelf = self.find(name)
        if shelf is None:
            raise core.AnimTKError("There is no shelf called {}".format(name))
        if writable and shelf.get("read_only"):
            raise core.AnimTKError("{} is a studio shelf (read-only); copy it to change it".format(name))
        return shelf

    @property
    def active(self):
        return self.data.get("active")

    @active.setter
    def active(self, name):
        self.data["active"] = name
        self.save()

    # Shelves ---------------------------------------------------------------------------------------------------------
    def add_shelf(self, name):
        name = self.unique_name(name)
        self.data["shelves"].append({"name": name, "buttons": []})
        self.data["active"] = name
        self.save()
        return name

    def unique_name(self, name):
        base, index, existing = name, 2, set(self.names())
        while name in existing:
            name = "{} {}".format(base, index)
            index += 1
        return name

    def rename_shelf(self, old, new):
        shelf = self.require(old)
        if new != old:
            new = self.unique_name(new)
        shelf["name"] = new
        if self.data.get("active") == old:
            self.data["active"] = new
        self.save()
        return new

    def delete_shelf(self, name):
        shelf = self.require(name)
        self.data["shelves"].remove(shelf)
        if self.data.get("active") == name:
            names = self.names()
            self.data["active"] = names[0] if names else None
        self.save()

    def copy_shelf(self, name):
        shelf = self.require(name, writable=False)
        clone = copy.deepcopy(shelf)
        clone.pop("read_only", None)
        clone.pop("source", None)
        clone["name"] = self.unique_name(shelf["name"] + " copy")
        self.data["shelves"].append(clone)
        self.data["active"] = clone["name"]
        self.save()
        return clone["name"]

    def move_shelf(self, name, offset):
        shelves = self.data["shelves"]
        shelf = self.require(name)
        index = shelves.index(shelf)
        target = max(0, min(len(shelves) - 1, index + offset))
        shelves.insert(target, shelves.pop(index))
        self.save()

    # Buttons ---------------------------------------------------------------------------------------------------------
    def add_button(self, shelf_name, button, index=None):
        shelf = self.require(shelf_name)
        button = normalize_button(button)
        if index is None:
            shelf["buttons"].append(button)
        else:
            shelf["buttons"].insert(index, button)
        self.save()
        return button

    def update_button(self, shelf_name, index, button):
        shelf = self.require(shelf_name)
        shelf["buttons"][index] = normalize_button(button)
        self.save()

    def delete_button(self, shelf_name, index):
        shelf = self.require(shelf_name)
        del shelf["buttons"][index]
        self.save()

    def move_button(self, shelf_name, index, offset):
        buttons = self.require(shelf_name)["buttons"]
        target = max(0, min(len(buttons) - 1, index + offset))
        buttons.insert(target, buttons.pop(index))
        self.save()

    def duplicate_button(self, shelf_name, index):
        buttons = self.require(shelf_name)["buttons"]
        buttons.insert(index + 1, copy.deepcopy(buttons[index]))
        self.save()


def normalize_button(button):
    """Fills defaults and checks that a button runs something."""
    result = {
        "label": str(button.get("label") or "Script"),
        "icon": button.get("icon") or "",
        "tooltip": button.get("tooltip") or "",
        "command": button.get("command") or "",
        "script": button.get("script") or "",
        "color": button.get("color") or None,
    }
    if not result["command"] and not result["script"]:
        raise core.AnimTKError("A shelf button needs a command or a script")
    return result


def button_from_file(path):
    """A button that runs a dropped .py file (label from the file name, tooltip from its docstring's first line)."""
    label = os.path.splitext(os.path.basename(path))[0].replace("_", " ").strip().title()
    tooltip = path
    try:
        with open(path, "r", encoding="utf-8") as handle:
            head = handle.read(2000)
        for quote in ('"""', "'''"):
            if quote in head:
                doc = head.split(quote, 2)[1].strip().splitlines()
                if doc:
                    tooltip = doc[0]
                break
    except OSError:
        pass
    return {"label": label, "script": os.path.normpath(path), "tooltip": tooltip}


def button_from_asset(object_path):
    name = object_path.rsplit("/", 1)[-1].split(".")[0]
    return {
        "label": name,
        "command": "import animtk.core as c; c.open_asset({!r})".format(object_path),
        "tooltip": "Open " + object_path,
        "icon": "Icons.FolderOpen",
    }


# ------------------------------------------------------------------------------------------------------------- running

def resolve_script(path):
    """Absolute paths as they are; relative ones against Saved/AnimToolkit/Scripts, then Resources/ShelfScripts."""
    path = os.path.expandvars(os.path.expanduser(path))
    if os.path.isabs(path):
        return path
    for base in (core.user_dir("Scripts"), core.resource("ShelfScripts")):
        candidate = os.path.join(base, path)
        if os.path.isfile(candidate):
            return candidate
    return path


def run_button(button):
    """Runs a shelf button: inline code or a script file, in a fresh namespace, as one undo step."""
    label = button.get("label") or "Shelf button"
    namespace = {"__name__": "__main__", "unreal": unreal}
    try:
        import animtk
        namespace["animtk"] = animtk
    except ImportError:
        pass
    with core.undo(label):
        try:
            if button.get("script"):
                path = resolve_script(button["script"])
                if not os.path.isfile(path):
                    raise core.AnimTKError("Script not found: {}".format(path))
                old_argv, old_path = sys.argv, list(sys.path)
                sys.argv = [path]
                sys.path.insert(0, os.path.dirname(path))
                try:
                    runpy.run_path(path, init_globals=namespace, run_name="__main__")
                finally:
                    sys.argv, sys.path[:] = old_argv, old_path
            else:
                exec(compile(button.get("command", ""), "<shelf: {}>".format(label), "exec"), namespace)  # noqa: S102 - this is the point of a shelf
        except core.AnimTKError as exc:
            core.notify(str(exc), "warning")
        except Exception:  # noqa: BLE001
            core.error("Shelf button '{}' failed:\n{}".format(label, traceback.format_exc()))
            core.notify("'{}' failed, see the Output Log".format(label), "error")


# ------------------------------------------------------------------------------------------------------------- API

_store = None


def store():
    global _store
    if _store is None:
        _store = ShelfStore()
    return _store


def add_button(shelf_name, label, command="", script="", icon="", tooltip="", color=None):
    """Adds a button (creating the shelf when needed) and refreshes an open shelf panel."""
    s = store()
    if s.find(shelf_name) is None:
        s.add_shelf(shelf_name)
    s.add_button(shelf_name, {"label": label, "command": command, "script": script, "icon": icon, "tooltip": tooltip, "color": color})
    ShelfTool.refresh()


def add_shelf(name):
    name = store().add_shelf(name)
    ShelfTool.refresh()
    return name


# ------------------------------------------------------------------------------------------------------------- UI

COLORS = [
    ("Default", None),
    ("Red", "#C0392B"), ("Orange", "#D35400"), ("Yellow", "#B7950B"), ("Green", "#229954"),
    ("Teal", "#138D75"), ("Blue", "#2471A3"), ("Purple", "#7D3C98"), ("Grey", "#566573"),
]


class ShelfTool(ui.Tool):
    panel_id = "animtk_shelf"
    title = "Anim Shelf"

    def __init__(self):
        super(ShelfTool, self).__init__()
        self.store = store()

    @classmethod
    def refresh(cls):
        inst = cls.__dict__.get("_instance")
        if inst is not None and inst.panel.is_open():
            inst.rebuild()

    # Build -----------------------------------------------------------------------------------------------------------
    def build(self):
        s = self.store
        tabs = []
        for shelf in s.shelves():
            name = shelf["name"]
            tabs.append(ui.Toggle(
                name + (" (studio)" if shelf.get("read_only") else ""),
                value=name == s.active,
                on_changed=lambda e, n=name: self._activate(n),
                menu=self._shelf_menu(shelf),
                on_menu=lambda e: self._shelf_action(e.action),
                tooltip=shelf.get("source", "Right-click for shelf commands"),
            ))
        tabs.append(ui.Button("+", flat=True, tooltip="New shelf", on_clicked=self._new_shelf))

        active = s.find(s.active) if s.active else None
        buttons = []
        if active is not None:
            for index, button in enumerate(active["buttons"]):
                buttons.append(self._button_widget(active, index, button))
        if not buttons:
            buttons.append(ui.Label("Drop .py files here, or right-click > New Button", style="small", color=[0.6, 0.6, 0.6, 1]))

        return ui.VBox(
            ui.HBox(*tabs, ui.Spacer(stretch=1),
                    ui.Button("", icon="Icons.Plus", flat=True, tooltip="New button", on_clicked=lambda: self._edit(None)),
                    ui.Button("", icon="Icons.Refresh", flat=True, tooltip="Reload shelves from disk", on_clicked=self._reload),
                    padding=0),
            ui.Separator(padding=0),
            ui.Scroll(
                ui.Wrap(*buttons, id="buttons", spacing=3, accept_drop=True, on_drop=self._drop,
                        menu=[ui.menu_item("New Button...", "new"), ui.menu_item("Paste Button From Clipboard", "paste"),
                              ui.menu_separator(), ui.menu_item("Open Shelf Folder", "folder")],
                        on_menu=self._wrap_menu, min_height=60),
                stretch=1,
            ),
        )

    def _button_widget(self, shelf, index, button):
        read_only = shelf.get("read_only")
        has_icon = bool(button.get("icon"))
        menu = [ui.menu_item("Run", "run"), ui.menu_item("Copy Command To Clipboard", "copy")]
        if button.get("script"):
            menu.append(ui.menu_item("Open Script", "open"))
        if not read_only:
            menu += [ui.menu_separator(), ui.menu_item("Edit...", "edit"), ui.menu_item("Duplicate", "duplicate"),
                     ui.menu_item("Move Left", "left"), ui.menu_item("Move Right", "right"),
                     ui.submenu("Color", [ui.menu_item(n, "color:" + (c or "")) for n, c in COLORS]),
                     ui.menu_separator(), ui.menu_item("Delete", "delete")]
        return ui.Button(
            button.get("label", "") if (not has_icon or len(button.get("label", "")) <= 10) else "",
            icon=button.get("icon") or None,
            icon_above=has_icon,
            color=button.get("color"),
            tooltip=button.get("tooltip") or button.get("script") or button.get("command", "")[:200],
            min_width=48,
            min_height=40 if has_icon else None,
            style="small",
            on_clicked=lambda e, b=button: self._run(b),
            menu=menu,
            on_menu=lambda e, n=shelf["name"], i=index: self._button_menu(n, i, e.action),
        )

    def _shelf_menu(self, shelf):
        if shelf.get("read_only"):
            return [ui.menu_header(shelf["name"]), ui.menu_item("Copy To My Shelves", "copy_shelf:" + shelf["name"])]
        name = shelf["name"]
        return [ui.menu_header(name),
                ui.menu_item("Rename...", "rename:" + name),
                ui.menu_item("Move Left", "shelf_left:" + name), ui.menu_item("Move Right", "shelf_right:" + name),
                ui.menu_separator(), ui.menu_item("New Shelf...", "new_shelf"),
                ui.menu_separator(), ui.menu_item("Delete Shelf", "delete_shelf:" + name)]

    # Events ----------------------------------------------------------------------------------------------------------
    def _activate(self, name):
        self.store.active = name
        self.rebuild()

    def _run(self, button):
        run_button(button)

    def _reload(self):
        self.store.load()
        self.rebuild()
        core.notify("Shelves reloaded")

    def _new_shelf(self):
        ui.prompt("New Shelf", "Shelf name:", lambda name: (self.store.add_shelf(name), self.rebuild()), "Shelf")

    def _shelf_action(self, action):
        verb, _, name = action.partition(":")
        s = self.store
        if verb == "new_shelf":
            self._new_shelf()
            return
        if verb == "rename":
            ui.prompt("Rename Shelf", "New name for {}:".format(name), lambda new: (s.rename_shelf(name, new), self.rebuild()), name)
            return
        if verb == "delete_shelf":
            if ui.confirm("Delete Shelf", "Delete the shelf '{}' and its buttons?".format(name)):
                s.delete_shelf(name)
        elif verb == "shelf_left":
            s.move_shelf(name, -1)
        elif verb == "shelf_right":
            s.move_shelf(name, 1)
        elif verb == "copy_shelf":
            s.copy_shelf(name)
        self.rebuild()

    def _button_menu(self, shelf_name, index, action):
        s = self.store
        button = s.require(shelf_name, writable=False)["buttons"][index]
        if action == "run":
            self._run(button)
        elif action == "copy":
            text = button.get("command") or "exec(open({!r}).read())".format(button.get("script"))
            core.clipboard_copy(text)
            core.notify("Copied")
        elif action == "open":
            open_script(resolve_script(button["script"]))
        elif action == "edit":
            self._edit(index)
        elif action == "duplicate":
            s.duplicate_button(shelf_name, index)
        elif action == "left":
            s.move_button(shelf_name, index, -1)
        elif action == "right":
            s.move_button(shelf_name, index, 1)
        elif action.startswith("color:"):
            updated = dict(button)
            updated["color"] = action.split(":", 1)[1] or None
            s.update_button(shelf_name, index, updated)
        elif action == "delete":
            if ui.confirm("Delete Button", "Delete '{}'?".format(button.get("label"))):
                s.delete_button(shelf_name, index)
        self.rebuild()

    def _wrap_menu(self, event):
        if event.action == "new":
            self._edit(None)
        elif event.action == "paste":
            code = core.clipboard_paste().strip()
            if not code:
                raise core.AnimTKError("The clipboard is empty")
            first = code.splitlines()[0].lstrip("# ").strip()[:16] or "Script"
            self.store.add_button(self.store.active, {"label": first, "command": code})
            self.rebuild()
        elif event.action == "folder":
            open_folder(os.path.dirname(self.store.user_file))

    def _drop(self, event):
        s = self.store
        if not s.active:
            s.add_shelf("Shelf")
        added = 0
        for path in event.files or []:
            if path.lower().endswith(".py"):
                s.add_button(s.active, button_from_file(path))
                added += 1
        for asset in event.assets or []:
            s.add_button(s.active, button_from_asset(asset))
            added += 1
        if not added:
            core.notify("Drop .py files or Content Browser assets on the shelf", "warning")
        self.rebuild()

    def _edit(self, index):
        ButtonEditor.edit(self.store.active, index)


# ------------------------------------------------------------------------------------------------------------- editor

class ButtonEditor(ui.Tool):
    panel_id = "animtk_shelf_button_editor"
    title = "Shelf Button"
    floating = True

    def __init__(self):
        super(ButtonEditor, self).__init__()
        self.shelf_name = None
        self.index = None

    @classmethod
    def edit(cls, shelf_name, index):
        if not shelf_name:
            shelf_name = store().add_shelf("Shelf")
        inst = cls.instance()
        inst.shelf_name = shelf_name
        inst.index = index
        inst.rebuild()
        inst.open(floating=True)

    def build(self):
        button = {"label": "New Button", "command": "", "script": "", "icon": "", "tooltip": ""}
        if self.index is not None:
            button.update(store().require(self.shelf_name, writable=False)["buttons"][self.index])
        mode = "Script file" if button.get("script") else "Python command"
        return ui.VBox(
            ui.Label("Shelf: {}".format(self.shelf_name), style="small"),
            ui.HBox(ui.Label("Label", min_width=60), ui.TextField(button["label"], id="label")),
            ui.HBox(ui.Label("Icon", min_width=60), ui.TextField(button.get("icon", ""), id="icon", hint="Icons.Plus or C:/icons/key.png")),
            ui.HBox(ui.Label("Tooltip", min_width=60), ui.TextField(button.get("tooltip", ""), id="tooltip")),
            ui.HBox(ui.Label("Runs", min_width=60), ui.Combo(["Python command", "Script file"], value=mode, id="mode", on_changed=self._mode)),
            ui.HBox(ui.Label("Script", min_width=60), ui.TextField(button.get("script", ""), id="script", hint="C:/scripts/my_tool.py"),
                    id="script_row", visible=mode == "Script file"),
            ui.TextArea(button.get("command", ""), id="command", code=True, stretch=1, visible=mode == "Python command",
                        hint="import animtk.ops as ops\nops.key_selected()"),
            ui.HBox(
                ui.Button("Test", on_clicked=self._test),
                ui.Spacer(stretch=1),
                ui.Button("Save", color="#2E86C1", on_clicked=self._save),
                ui.Button("Cancel", on_clicked=self.close),
            ),
        )

    def _mode(self, event):
        script = event.value == "Script file"
        self.set("script_row", "visible", script)
        self.set("command", "visible", not script)

    def _collect(self):
        script_mode = self.get("mode") == "Script file"
        return {
            "label": self.get("label"),
            "icon": self.get("icon"),
            "tooltip": self.get("tooltip"),
            "command": "" if script_mode else self.get("command"),
            "script": self.get("script") if script_mode else "",
        }

    def _test(self):
        run_button(normalize_button(self._collect()))

    def _save(self):
        s = store()
        button = self._collect()
        if self.index is None:
            s.add_button(self.shelf_name, button)
        else:
            s.update_button(self.shelf_name, self.index, button)
        self.close()
        ShelfTool.refresh()


# ------------------------------------------------------------------------------------------------------------- helpers

def open_script(path):
    """Opens a script in the Dream Python Editor when it is installed, else with the OS default program."""
    try:
        import dream_editor
        dream_editor.open(path)
        return
    except Exception:  # noqa: BLE001 - Dream is optional
        pass
    open_folder(path)


def open_folder(path):
    if sys.platform.startswith("win"):
        os.startfile(path)  # noqa: S606
    else:
        import subprocess
        subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", path])
