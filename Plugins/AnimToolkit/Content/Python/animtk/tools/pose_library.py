# SPDX-License-Identifier: Apache-2.0
"""Pose / Animation Library: save poses and short clips of Control Rig controls, then apply, blend or mirror them.

* Library folders hold one ``.json`` per pose or clip (plus a ``.png`` thumbnail grabbed from the viewport).
  Your library is ``Saved/AnimToolkit/PoseLibrary``; add studio libraries with ``ANIMTK_POSE_LIBRARY_PATH``.
* Save Pose stores the selected controls (or the whole rig) at the playhead; Save Anim stores the work range
  (the Sequencer selection range, else the playback range).
* Double-click applies. Drag the blend slider to fade from the current pose into the selected pose (one undo step);
  "Mirror" applies the opposite side, "Selected only" limits it to the selected controls.
"""

import os
import shutil
import time

from .. import core, pose, seq, ui
from .. import rig as rig_mod

unreal = core.unreal


def library_roots():
    roots = [("My Library", core.user_dir("PoseLibrary"))]
    for folder in core.search_path("ANIMTK_POSE_LIBRARY_PATH"):
        if os.path.isdir(folder):
            roots.append((os.path.basename(folder.rstrip("\\/")) or folder, folder))
    return roots


def list_folders(root):
    folders = [""]
    for dirpath, dirnames, _files in os.walk(root):
        dirnames.sort()
        for name in dirnames:
            folders.append(os.path.relpath(os.path.join(dirpath, name), root).replace("\\", "/"))
    return folders


def list_entries(folder):
    entries = []
    if not os.path.isdir(folder):
        return entries
    for name in sorted(os.listdir(folder), key=str.lower):
        if not name.lower().endswith(".json"):
            continue
        path = os.path.join(folder, name)
        data = core.read_json(path, {}) or {}
        entries.append({
            "path": path,
            "name": data.get("name") or os.path.splitext(name)[0],
            "type": data.get("type", "pose"),
            "length": data.get("length", 1),
            "count": len(data.get("controls", {})),
            "thumbnail": os.path.join(folder, data["thumbnail"]) if data.get("thumbnail") else "",
            "rig": data.get("rig", ""),
        })
    return entries


CAPTURE_SPACING = 1.5   # the engine keeps only the last of several screenshot requests made close together
_capture_queue = []
_capture_state = {"tick": None, "last": 0.0}


def _pump_captures():
    """Runs on the editor tick, so each screenshot request lands in its own frame."""
    if not _capture_queue:
        core.remove_tick(_capture_state["tick"])
        _capture_state["tick"] = None
        return
    if time.time() - _capture_state["last"] < CAPTURE_SPACING:
        return
    path = _capture_queue.pop(0)
    _capture_state["last"] = time.time()
    try:
        unreal.AutomationLibrary.take_high_res_screenshot(256, 256, path, None, False, False)
    except Exception as exc:  # noqa: BLE001 - thumbnails are a nicety
        core.warn("No thumbnail: {}".format(exc))


def capture_seconds_left():
    """Roughly how long until every queued thumbnail is written."""
    return len(_capture_queue) * CAPTURE_SPACING + 1.0


def capture_thumbnail(json_path):
    """Queues a viewport grab into a thumbnail next to ``json_path``; returns the file name (written shortly after).
    Saving several poses quickly, even from a script, still gives each one its thumbnail."""
    stem = os.path.splitext(os.path.basename(json_path))[0]
    # A new name per capture: Slate caches loose images by file name.
    file_name = "{}_{}.png".format(stem, int(time.time() * 1000))
    _capture_queue.append(os.path.join(os.path.dirname(json_path), file_name))
    if _capture_state["tick"] is None:
        _capture_state["tick"] = core.add_tick(_pump_captures, 0.1)
    return file_name


class PoseLibraryTool(ui.Tool):
    panel_id = "animtk_pose_library"
    title = "Pose Library"

    def __init__(self):
        super(PoseLibraryTool, self).__init__()
        self.root_index = 0
        self.folder = ""
        self.filter = ""
        self.selected_path = None
        self.drag_base = None
        self.drag_rig = None
        self.interaction = core.Interaction("Blend pose")

    # Paths -----------------------------------------------------------------------------------------------------------
    def root(self):
        roots = library_roots()
        self.root_index = min(self.root_index, len(roots) - 1)
        return roots[self.root_index][1]

    def folder_path(self):
        return os.path.join(self.root(), self.folder) if self.folder else self.root()

    def entries(self):
        result = list_entries(self.folder_path())
        if self.filter:
            needle = self.filter.lower()
            result = [e for e in result if needle in e["name"].lower()]
        return result

    def _tile_items(self):
        items = []
        for entry in self.entries():
            label = entry["name"] + (" ({}f)".format(entry["length"]) if entry["type"] == "anim" else "")
            items.append({
                "id": entry["path"],
                "label": label,
                "icon": entry["thumbnail"],
                "color": [0.25, 0.35, 0.55, 1] if entry["type"] == "anim" else [0.3, 0.3, 0.3, 1],
                "tooltip": "{}\n{} controls, {}\n{}".format(entry["name"], entry["count"], entry["rig"].rsplit("/", 1)[-1], entry["path"]),
            })
        return items

    # Build -----------------------------------------------------------------------------------------------------------
    def build(self):
        roots = library_roots()
        folders = list_folders(self.root())
        if self.folder not in folders:
            self.folder = ""
        folder_items = [{"id": f, "label": f or "(top)"} for f in folders]
        tile_menu = [ui.menu_item("Apply", "apply"), ui.menu_item("Apply Mirrored", "apply_mirror"),
                     ui.menu_item("Select Its Controls", "select"), ui.menu_separator(),
                     ui.menu_item("Update Thumbnail", "thumb"), ui.menu_item("Overwrite With Current Pose", "overwrite"),
                     ui.menu_item("Rename...", "rename"), ui.menu_item("Show In Explorer", "explore"),
                     ui.menu_separator(), ui.menu_item("Delete", "delete")]
        return ui.VBox(
            ui.HBox(
                ui.Combo([name for name, _p in roots], value=roots[self.root_index][0], id="root", on_changed=self._root_changed),
                ui.TextField(self.filter, hint="Search", id="search", on_changed=self._search),
            ),
            ui.Splitter(
                ui.VBox(
                    ui.ListView(folder_items, multi=False, id="folders", selected=[self.folder], on_selection=self._folder_changed,
                                menu=[ui.menu_item("New Folder...", "new"), ui.menu_item("Delete Folder", "delete")], on_menu=self._folder_menu),
                    size=0.3,
                ),
                ui.TileView(self._tile_items(), tile_size=80, id="tiles", selected=[self.selected_path] if self.selected_path else [],
                            on_selection=self._tile_selected, on_double_click=lambda e: self._apply(e.item), menu=tile_menu, on_menu=self._tile_menu,
                            size=0.7),
                stretch=1,
            ),
            ui.HBox(
                ui.Label("Blend", min_width=40),
                ui.Slider(0.0, 0.0, 1.0, id="blend", on_begin=self._blend_begin, on_changed=self._blend, on_end=self._blend_end,
                          tooltip="Drag to fade from the current pose into the selected pose"),
            ),
            ui.HBox(
                ui.CheckBox("Selected only", value=False, id="selected_only"),
                ui.CheckBox("Mirror", value=False, id="mirror"),
                ui.CheckBox("Key", value=True, id="key"),
                ui.Spacer(stretch=1),
            ),
            ui.HBox(
                ui.TextField("", hint="Name for a new pose", id="name", stretch=1),
                ui.Button("Save Pose", color="#2E86C1", on_clicked=lambda: self._save("pose")),
                ui.Button("Save Anim", tooltip="Save the work range as a clip", on_clicked=lambda: self._save("anim")),
                ui.Button("Apply", on_clicked=lambda: self._apply(self.selected_path)),
            ),
        )

    def refresh_tiles(self):
        self.set("tiles", "items", self._tile_items())

    # Navigation ------------------------------------------------------------------------------------------------------
    def _root_changed(self, event):
        self.root_index = event.index or 0
        self.folder = ""
        self.rebuild()

    def _search(self, event):
        self.filter = event.value or ""
        self.refresh_tiles()

    def _folder_changed(self, event):
        self.folder = (event.items or [""])[0]
        self.refresh_tiles()

    def _folder_menu(self, event):
        if event.action == "new":
            def create(name):
                os.makedirs(os.path.join(self.folder_path(), core.safe_filename(name)), exist_ok=True)
                self.rebuild()
            ui.prompt("New Folder", "Folder name (inside {}):".format(self.folder or "the top level"), create)
        elif event.action == "delete" and self.folder:
            path = self.folder_path()
            if ui.confirm("Delete Folder", "Delete {} and every pose in it?".format(path)):
                shutil.rmtree(path)
                self.folder = ""
                self.rebuild()

    def _tile_selected(self, event):
        self.selected_path = (event.items or [None])[0]

    # Saving ----------------------------------------------------------------------------------------------------------
    def _save(self, kind):
        name = (self.get("name") or "").strip()
        if not name:
            raise core.AnimTKError("Type a name for the pose first")
        rig = rig_mod.require_rig()
        controls = rig.selected() if self.get("selected_only") else None
        if kind == "anim":
            start, end = seq.work_range()
            clip = pose.capture(rig, controls, start, end, name=name, kind="anim")
        else:
            clip = pose.capture(rig, controls, name=name, kind="pose")
        path = os.path.join(self.folder_path(), core.safe_filename(name) + ".json")
        if os.path.exists(path) and not ui.confirm("Pose Library", "Overwrite {}?".format(name)):
            return
        clip["thumbnail"] = capture_thumbnail(path)
        core.write_json(path, clip)
        self.selected_path = path
        core.notify("Saved {} ({} controls)".format(name, len(clip["controls"])), "success")
        self.refresh_tiles()
        core.call_later(self.refresh_tiles, capture_seconds_left() + 1.0)   # written a moment later

    # Applying --------------------------------------------------------------------------------------------------------
    def _load(self, path):
        if not path:
            raise core.AnimTKError("Select a pose first")
        clip = core.read_json(path)
        if not clip:
            raise core.AnimTKError("Could not read {}".format(path))
        return clip

    def _target(self, clip):
        rig = rig_mod.require_rig()
        if not pose.is_compatible(clip, rig):
            core.notify("{} was saved from another rig; applying the controls that match".format(clip.get("name")), "warning")
        limit = rig.selected() if self.get("selected_only") else None
        return rig, limit

    def _apply(self, path, mirror=None):
        clip = self._load(path)
        rig, limit = self._target(clip)
        mirror = self.get("mirror") if mirror is None else mirror
        with core.undo("Apply {}".format(clip.get("name", "pose"))):
            count = pose.apply(rig, clip, controls=limit, mirror=mirror, key=bool(self.get("key")))
        core.notify("Applied {} to {} controls".format(clip.get("name"), count))

    def _blend_begin(self, event):
        clip = self._load(self.selected_path)
        if clip.get("type") == "anim":
            raise core.AnimTKError("Blending works on poses; apply clips with Apply")
        rig, limit = self._target(clip)
        self.drag_rig = rig
        self.drag_clip = pose.mirror_clip(clip) if self.get("mirror") else clip
        self.drag_limit = limit
        self.interaction.begin()
        self.drag_base = pose.capture(rig, pose.controls_of(self.drag_clip))

    def _blend(self, event):
        if self.drag_base is None:
            return
        pose.apply(self.drag_rig, self.drag_clip, controls=self.drag_limit, weight=float(event.value), base=self.drag_base, key=False)

    def _blend_end(self, event):
        if self.drag_base is not None:
            if self.get("key"):
                pose.apply(self.drag_rig, self.drag_clip, controls=self.drag_limit, weight=float(event.value), base=self.drag_base, key=True)
            self.interaction.end()
        self.drag_base = None
        self.set("blend", "value", 0.0)

    # Tile menu -------------------------------------------------------------------------------------------------------
    def _tile_menu(self, event):
        path = event.item
        if not path:
            return
        action = event.action
        if action == "apply":
            self._apply(path)
        elif action == "apply_mirror":
            self._apply(path, mirror=True)
        elif action == "select":
            clip = self._load(path)
            rig = rig_mod.require_rig()
            rig.select([c for c in pose.controls_of(clip) if rig.has_control(c)], "replace")
        elif action == "thumb":
            clip = self._load(path)
            old = clip.get("thumbnail")
            clip["thumbnail"] = capture_thumbnail(path)
            core.write_json(path, clip)
            if old and os.path.isfile(os.path.join(os.path.dirname(path), old)):
                core.call_later(lambda: os.remove(os.path.join(os.path.dirname(path), old)), 3.0)
            core.call_later(self.refresh_tiles, capture_seconds_left() + 1.0)
        elif action == "overwrite":
            clip = self._load(path)
            rig = rig_mod.require_rig()
            if ui.confirm("Pose Library", "Overwrite {} with the current pose?".format(clip.get("name"))):
                new = pose.capture(rig, pose.controls_of(clip), name=clip.get("name", ""), kind="pose")
                new["thumbnail"] = clip.get("thumbnail", "")
                core.write_json(path, new)
        elif action == "rename":
            clip = self._load(path)

            def rename(new_name):
                clip["name"] = new_name
                new_path = os.path.join(os.path.dirname(path), core.safe_filename(new_name) + ".json")
                core.write_json(new_path, clip)
                if os.path.normcase(new_path) != os.path.normcase(path):
                    os.remove(path)
                self.selected_path = new_path
                self.refresh_tiles()
            ui.prompt("Rename Pose", "New name:", rename, clip.get("name", ""))
        elif action == "explore":
            from ..shelf import open_folder
            open_folder(os.path.dirname(path))
        elif action == "delete":
            clip = core.read_json(path, {}) or {}
            if ui.confirm("Pose Library", "Delete {}?".format(clip.get("name", path))):
                os.remove(path)
                thumb = clip.get("thumbnail")
                if thumb and os.path.isfile(os.path.join(os.path.dirname(path), thumb)):
                    os.remove(os.path.join(os.path.dirname(path), thumb))
                self.selected_path = None
                self.refresh_tiles()


def show():
    return PoseLibraryTool.show()
