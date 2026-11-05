# SPDX-License-Identifier: Apache-2.0
"""Rig Picker: click controls on a 2D layout instead of hunting for them in the viewport.

* Works for any Control Rig out of the box: "Auto Layout" projects the controls' rest positions onto a front or side
  view and colours them like their shapes.
* Click selects, Shift+click adds, Ctrl+click toggles, drag a box to marquee select, double-click selects the control
  and everything under it. The picker follows the rig selection made in the viewport.
* Edit mode: drag buttons around, right-click to resize, recolour, reshape, delete or add the selected controls,
  then Save. Pickers are saved per Control Rig asset in ``Saved/AnimToolkit/Pickers`` (studio pickers are read from
  ``ANIMTK_PICKER_PATH`` first).
"""

import math
import os

from .. import core, ops, seq, ui
from .. import rig as rig_mod
from ..pose import side_of

CANVAS = (400.0, 600.0)
MARGIN = 30.0


# ------------------------------------------------------------------------------------------------------------- layout

def auto_layout(points, colors=None, view="front", canvas=CANVAS, size=None):
    """Projects rig-space control positions onto a 2D picker.

    ``points``: {name: (x, y, z)} rest positions (UE axes: Z up). ``view``: "front" puts the axis with the larger
    spread horizontally (characters may face +X or +Y), "side" the other one. Returns picker items with no two items
    overlapping (a few relaxation passes push apart buttons that land on each other).
    """
    colors = colors or {}
    names = [n for n in points]
    if not names:
        return []
    xs = [points[n][0] for n in names]
    ys = [points[n][1] for n in names]
    zs = [points[n][2] for n in names]
    spread_x = max(xs) - min(xs)
    spread_y = max(ys) - min(ys)
    wide_axis = 0 if spread_x >= spread_y else 1
    axis = wide_axis if view == "front" else 1 - wide_axis
    horizontal = [points[n][axis] for n in names]

    h_min, h_max = min(horizontal), max(horizontal)
    v_min, v_max = min(zs), max(zs)
    h_span = max(h_max - h_min, 1e-3)
    v_span = max(v_max - v_min, 1e-3)
    usable_w, usable_h = canvas[0] - 2 * MARGIN, canvas[1] - 2 * MARGIN
    scale = min(usable_w / h_span, usable_h / v_span)
    offset_x = (canvas[0] - h_span * scale) * 0.5
    offset_y = (canvas[1] - v_span * scale) * 0.5

    if size is None:
        size = max(10.0, min(22.0, 360.0 / math.sqrt(len(names))))

    items = []
    for name, h, v in zip(names, horizontal, zs):
        items.append({
            "id": name,
            "x": round(offset_x + (h - h_min) * scale, 2),
            "y": round(canvas[1] - (offset_y + (v - v_min) * scale), 2),
            "w": size,
            "h": size,
            "color": colors.get(name, [0.8, 0.8, 0.2, 1.0]),
            "shape": "circle" if _looks_fk(name) else "rect",
            "label": "",
        })
    # The character faces the viewer, so its left belongs on screen right. Which world direction is "left" depends on
    # how the rig was built, so read it from the control names and flip when needed.
    lefts = [i["x"] for i in items if side_of(i["id"]) == "left"]
    rights = [i["x"] for i in items if side_of(i["id"]) == "right"]
    if lefts and rights and sum(lefts) / len(lefts) < sum(rights) / len(rights):
        for item in items:
            item["x"] = round(canvas[0] - item["x"], 2)
    relax(items, canvas)
    return items


def _looks_fk(name):
    lowered = name.lower()
    return "fk" in lowered or "_rot" in lowered


def relax(items, canvas=CANVAS, iterations=40):
    """Pushes overlapping items apart (in place)."""
    for _ in range(iterations):
        moved = False
        for i in range(len(items)):
            a = items[i]
            for j in range(i + 1, len(items)):
                b = items[j]
                min_dx = (a["w"] + b["w"]) * 0.5 + 1.0
                min_dy = (a["h"] + b["h"]) * 0.5 + 1.0
                dx = b["x"] - a["x"]
                dy = b["y"] - a["y"]
                if abs(dx) >= min_dx or abs(dy) >= min_dy:
                    continue
                push_x = (min_dx - abs(dx)) * 0.5
                push_y = (min_dy - abs(dy)) * 0.5
                if push_x < push_y:
                    sign = 1.0 if dx > 0 or (dx == 0 and j % 2) else -1.0
                    a["x"] -= push_x * sign
                    b["x"] += push_x * sign
                else:
                    sign = 1.0 if dy > 0 or (dy == 0 and j % 2) else -1.0
                    a["y"] -= push_y * sign
                    b["y"] += push_y * sign
                moved = True
        for item in items:
            item["x"] = min(max(item["x"], item["w"] * 0.5), canvas[0] - item["w"] * 0.5)
            item["y"] = min(max(item["y"], item["h"] * 0.5), canvas[1] - item["h"] * 0.5)
        if not moved:
            break
    return items


# ------------------------------------------------------------------------------------------------------------- storage

def picker_path(asset_path, for_writing=False):
    name = core.safe_filename(asset_path.rsplit("/", 1)[-1] or "rig") + ".json"
    if not for_writing:
        for folder in core.search_path("ANIMTK_PICKER_PATH"):
            candidate = os.path.join(folder, name)
            if os.path.isfile(candidate):
                return candidate
    return core.user_dir("Pickers", name)


def load_picker(asset_path):
    return core.read_json(picker_path(asset_path))


def save_picker(asset_path, data):
    path = picker_path(asset_path, for_writing=True)
    core.write_json(path, data)
    return path


def generate(rig, view="front"):
    names = rig.controls()
    points, colors = {}, {}
    for name in names:
        try:
            points[name] = rig.initial_global_position(name)
            colors[name] = rig.color(name)
        except Exception as exc:  # noqa: BLE001
            core.warn("Picker skipped {}: {}".format(name, exc))
    return {"rig": rig.asset_path, "view": view, "canvas_size": list(CANVAS), "background": "",
            "items": auto_layout(points, colors, view)}


# ------------------------------------------------------------------------------------------------------------- tool

PALETTE = [("Yellow", [0.9, 0.8, 0.1, 1]), ("Red", [0.85, 0.15, 0.1, 1]), ("Blue", [0.1, 0.4, 0.9, 1]),
           ("Green", [0.15, 0.7, 0.2, 1]), ("Orange", [0.95, 0.45, 0.05, 1]), ("Purple", [0.55, 0.2, 0.8, 1]),
           ("Cyan", [0.1, 0.75, 0.85, 1]), ("White", [0.9, 0.9, 0.9, 1])]


class PickerTool(ui.Tool):
    panel_id = "animtk_picker"
    title = "Rig Picker"
    tick_interval = 0.2

    def __init__(self):
        super(PickerTool, self).__init__()
        self.rig_name = None
        self.data = None
        self.edit = False
        self.dirty = False
        self._last_selection = None

    # Data ------------------------------------------------------------------------------------------------------------
    def current_rig(self):
        if self.rig_name:
            found = rig_mod.find_rig(self.rig_name)
            if found is not None:
                return found
        rigs = rig_mod.active_rigs()
        if rigs:
            self.rig_name = rigs[0].name
            return rigs[0]
        return None

    def load_for(self, rig):
        self.data = None
        if rig is None:
            return
        self.data = load_picker(rig.asset_path) or generate(rig)
        self.dirty = False

    # Build -----------------------------------------------------------------------------------------------------------
    def build(self):
        rig = self.current_rig()
        if self.data is None or (rig is not None and self.data.get("rig") != rig.asset_path):
            self.load_for(rig)
        rig_names = [r.name for r in rig_mod.rigs()] or ["(no Control Rig in the open sequence)"]
        data = self.data or {"items": [], "canvas_size": list(CANVAS)}
        anim_menu = [ui.menu_item("Key Selected", "key"), ui.menu_item("Reset Selected", "reset"),
                     ui.menu_item("Select Children", "children"), ui.menu_item("Select Mirror", "mirror_select"),
                     ui.menu_separator(), ui.menu_item("Edit Layout", "edit")]
        edit_menu = [ui.menu_header("Item"),
                     ui.submenu("Color", [ui.menu_item(n, "color:{}".format(i)) for i, (n, _c) in enumerate(PALETTE)]),
                     ui.menu_item("Bigger", "bigger"), ui.menu_item("Smaller", "smaller"),
                     ui.menu_item("Circle", "circle"), ui.menu_item("Rectangle", "rect"),
                     ui.menu_item("Label From Name", "label"), ui.menu_item("Clear Label", "nolabel"),
                     ui.menu_item("Delete", "delete"),
                     ui.menu_header("Picker"),
                     ui.menu_item("Add Selected Controls", "add_selected"),
                     ui.menu_item("Mirror Layout Left/Right", "mirror_layout"),
                     ui.menu_item("Done Editing", "edit")]
        return ui.VBox(
            ui.HBox(
                ui.Combo(rig_names, value=self.rig_name if self.rig_name in rig_names else rig_names[0], id="rig", on_changed=self._rig_changed),
                ui.Button("", icon="Icons.Refresh", flat=True, tooltip="Refresh the rig list", on_clicked=self._refresh),
            ),
            ui.HBox(
                ui.Button("Key", tooltip="Key the selected controls", on_clicked=lambda: ops.key_selected()),
                ui.Button("Reset", tooltip="Reset the selected controls", on_clicked=lambda: ops.reset_selected()),
                ui.Button("All", tooltip="Select every control", on_clicked=self._select_all),
                ui.Button("Mirror", tooltip="Select the mirrored controls", on_clicked=lambda: ops.select_mirror()),
                ui.Spacer(stretch=1),
                ui.Toggle("Edit", value=self.edit, id="edit", tooltip="Edit the layout", on_changed=self._toggle_edit),
            ),
            ui.Canvas(
                data.get("items", []),
                canvas_size=data.get("canvas_size", list(CANVAS)),
                background=data.get("background") or None,
                id="canvas",
                edit=self.edit,
                snap=5 if self.edit else 0,
                menu=edit_menu if self.edit else anim_menu,
                stretch=1,
                on_click=self._click,
                on_marquee=self._marquee,
                on_double_click=self._double_click,
                on_moved=self._moved,
                on_menu=self._menu,
            ),
            ui.HBox(
                ui.Label("", id="status", style="small", stretch=1),
                ui.Button("Auto Front", visible=self.edit, tooltip="Rebuild the layout from the rig (front view)", on_clicked=lambda: self._auto("front")),
                ui.Button("Auto Side", visible=self.edit, tooltip="Rebuild the layout from the rig (side view)", on_clicked=lambda: self._auto("side")),
                ui.Button("Save", visible=self.edit, color="#2E86C1", on_clicked=self._save),
            ),
        )

    # Sync ------------------------------------------------------------------------------------------------------------
    def tick(self):
        if self.edit or seq.current() is None:
            return
        rig = self.current_rig()
        if rig is None:
            return
        selection = rig.selected()
        if selection != self._last_selection:
            self._last_selection = selection
            self.set("canvas", "selected", selection)
            self._status(selection)

    def _status(self, selection):
        if not selection:
            text = ""
        elif len(selection) <= 3:
            text = ", ".join(selection)
        else:
            text = "{} controls".format(len(selection))
        self.set("status", "text", text)

    # Anim mode events ------------------------------------------------------------------------------------------------
    def _rig(self):
        rig = self.current_rig()
        if rig is None:
            raise core.AnimTKError("No Control Rig in the open sequence")
        return rig

    def _mode(self, event):
        if event.ctrl:
            return "toggle"
        if event.shift:
            return "add"
        return "replace"

    def _click(self, event):
        if self.edit:
            self.selected_item = event.item
            return
        rig = self._rig()
        if not event.item:
            if not event.shift and not event.ctrl:
                rig.clear_selection()
            return
        rig.select([event.item], self._mode(event))

    def _marquee(self, event):
        if self.edit:
            return
        rig = self._rig()
        mode = "toggle" if event.ctrl else ("add" if event.shift else "replace")
        rig.select(event.items or [], mode)

    def _double_click(self, event):
        if self.edit or not event.item:
            return
        rig = self._rig()
        rig.select([event.item] + rig.children(event.item), "add" if event.shift else "replace")

    def _select_all(self):
        ops.select_all(self._rig())

    def _rig_changed(self, event):
        self.rig_name = event.value
        self.data = None
        self.rebuild()

    def _refresh(self):
        self.rig_name = None
        self.data = None
        self.rebuild()

    # Edit mode -------------------------------------------------------------------------------------------------------
    def _toggle_edit(self, event=None):
        if self.edit and self.dirty and not ui.confirm("Rig Picker", "Keep the layout changes without saving?"):
            self.set("edit", "value", True)
            return
        self.edit = not self.edit
        if not self.edit:
            self._last_selection = None
        self.rebuild()

    def _moved(self, event):
        by_id = dict((i["id"], i) for i in self.data["items"])
        for moved in event.items or []:
            item = by_id.get(moved["id"])
            if item is not None:
                item["x"], item["y"] = moved["x"], moved["y"]
        self.dirty = True

    def _menu(self, event):
        action = event.action or ""
        targets = list(event.items or [])
        if event.item and event.item not in targets:
            targets = [event.item]
        if not self.edit:
            {"key": ops.key_selected, "reset": ops.reset_selected, "children": lambda: ops.select_children(self._rig()),
             "mirror_select": ops.select_mirror, "edit": self._toggle_edit}.get(action, lambda: None)()
            return
        if action == "edit":
            self._toggle_edit()
            return
        items = self.data["items"]
        by_id = dict((i["id"], i) for i in items)
        if action == "add_selected":
            rig = self._rig()
            x, y = self.data.get("canvas_size", CANVAS)[0] * 0.5, 20.0
            for name in rig.selected():
                if name not in by_id:
                    items.append({"id": name, "x": x, "y": y, "w": 16, "h": 16, "color": rig.color(name), "shape": "rect", "label": ""})
                    y += 20
            relax(items, self.data.get("canvas_size", CANVAS))
        elif action == "mirror_layout":
            self._mirror_layout()
        else:
            for item_id in targets:
                item = by_id.get(item_id)
                if item is None:
                    continue
                if action.startswith("color:"):
                    item["color"] = PALETTE[int(action.split(":")[1])][1]
                elif action == "bigger":
                    item["w"], item["h"] = item["w"] * 1.25, item["h"] * 1.25
                elif action == "smaller":
                    item["w"], item["h"] = max(6, item["w"] * 0.8), max(6, item["h"] * 0.8)
                elif action in ("circle", "rect"):
                    item["shape"] = action
                elif action == "label":
                    item["label"] = _short_label(item_id)
                elif action == "nolabel":
                    item["label"] = ""
                elif action == "delete":
                    items.remove(item)
        self.dirty = True
        self.set("canvas", "items", items)

    def _mirror_layout(self):
        """Places every right-side item at the mirror position of its left-side twin."""
        from ..pose import mirror_name
        width = self.data.get("canvas_size", CANVAS)[0]
        by_id = dict((i["id"], i) for i in self.data["items"])
        for item in self.data["items"]:
            other = mirror_name(item["id"])
            if other and other in by_id and item["x"] > width * 0.5:
                twin = by_id[other]
                twin["x"], twin["y"] = width - item["x"], item["y"]
                twin["w"], twin["h"] = item["w"], item["h"]

    def _auto(self, view):
        if self.data and self.data.get("items") and not ui.confirm("Rig Picker", "Replace the layout with an automatic {} view?".format(view)):
            return
        self.data = generate(self._rig(), view)
        self.dirty = True
        self.rebuild()

    def _save(self):
        path = save_picker(self.data["rig"], self.data)
        self.dirty = False
        core.notify("Picker saved to {}".format(path), "success")


def _short_label(name):
    base = name.replace("_ctrl", "").replace("_CTRL", "")
    parts = [p for p in base.split("_") if p]
    return "".join(p[0] for p in parts[:3]).upper() if len(base) > 4 else base.upper()


def show():
    return PickerTool.show()
