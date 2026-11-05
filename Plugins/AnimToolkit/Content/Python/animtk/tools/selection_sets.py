# SPDX-License-Identifier: Apache-2.0
"""Selection Sets: one-click buttons that select groups of controls, across rigs.

* Click selects the set, Shift+click adds it to the selection, Ctrl+click removes it, Alt+click keys it.
* Right-click a set to add or remove the current selection, recolour, rename, mirror, key or delete it.
* Sets remember the rig they came from by name and fall back to any rig that has the same controls, so a set made on
  one character works on another with the same rig. Stored in ``Saved/AnimToolkit/selection_sets.json``.

UE 5.8 also has native selection sets (``ControlRigSequencerLibrary.get_selection_sets()``); this tool keeps its own
JSON so it is easy to read, diff and share, and doubles as an example of a small, complete tool.
"""

import uuid

from .. import core, ui
from .. import rig as rig_mod
from ..pose import mirror_name

COLORS = [("Grey", "#5D6D7E"), ("Red", "#C0392B"), ("Orange", "#D35400"), ("Yellow", "#B7950B"),
          ("Green", "#229954"), ("Teal", "#138D75"), ("Blue", "#2471A3"), ("Purple", "#7D3C98"), ("Pink", "#C2185B")]


# ------------------------------------------------------------------------------------------------------------- model

class SetStore(object):
    def __init__(self, path=None):
        self.path = path or core.user_dir("selection_sets.json")
        data = core.read_json(self.path, {}) or {}
        self.sets = data.get("sets", [])

    def save(self):
        core.write_json(self.path, {"version": 1, "sets": self.sets})

    def find(self, set_id):
        for item in self.sets:
            if item["id"] == set_id:
                return item
        raise core.AnimTKError("That selection set no longer exists")

    def create(self, name, members, color=None):
        item = {"id": uuid.uuid4().hex[:12], "name": name, "color": color or COLORS[(len(self.sets) + 1) % len(COLORS)][1],
                "members": members}
        self.sets.append(item)
        self.save()
        return item

    def delete(self, set_id):
        self.sets.remove(self.find(set_id))
        self.save()

    def move(self, set_id, offset):
        item = self.find(set_id)
        index = self.sets.index(item)
        target = max(0, min(len(self.sets) - 1, index + offset))
        self.sets.insert(target, self.sets.pop(index))
        self.save()


def members_from_selection():
    return [{"rig": r.name, "control": name} for r, name in rig_mod.selected_controls()]


def resolve(members, rigs):
    """[(rig, [controls])] for a set's members: the rig with the stored name when present, else every rig that has
    the control."""
    by_name = dict((r.name, r) for r in rigs)
    controls_cache = {}

    def has(rig, control):
        if rig.name not in controls_cache:
            controls_cache[rig.name] = set(rig.controls())
        return control in controls_cache[rig.name]

    result = {}
    order = []
    for member in members:
        rig = by_name.get(member.get("rig"))
        targets = [rig] if rig is not None and has(rig, member["control"]) else [r for r in rigs if has(r, member["control"])]
        for target in targets:
            if target.name not in result:
                result[target.name] = (target, [])
                order.append(target.name)
            if member["control"] not in result[target.name][1]:
                result[target.name][1].append(member["control"])
    return [result[name] for name in order]


# ------------------------------------------------------------------------------------------------------------- tool

class SelectionSetsTool(ui.Tool):
    panel_id = "animtk_selection_sets"
    title = "Selection Sets"

    def __init__(self):
        super(SelectionSetsTool, self).__init__()
        self.store = SetStore()

    def build(self):
        buttons = []
        for item in self.store.sets:
            buttons.append(ui.Button(
                item["name"],
                color=item.get("color"),
                text_color=[1, 1, 1, 1],
                min_width=70,
                tooltip="{} controls\nClick select, Shift add, Ctrl remove, Alt key".format(len(item["members"])),
                on_clicked=lambda e, i=item["id"]: self._clicked(i, e),
                menu=self._menu(item),
                on_menu=lambda e, i=item["id"]: self._menu_action(i, e.action),
            ))
        if not buttons:
            buttons.append(ui.Label("Select controls, type a name and press Create.", style="small", color=[0.6, 0.6, 0.6, 1]))
        return ui.VBox(
            ui.HBox(
                ui.TextField("", hint="New set name", id="name", on_committed=lambda e: self._create()),
                ui.Button("Create", color="#2E86C1", tooltip="Make a set from the selected controls", on_clicked=self._create),
            ),
            ui.Scroll(ui.Wrap(*buttons, spacing=3), stretch=1),
        )

    def _menu(self, item):
        return [ui.menu_header(item["name"]),
                ui.menu_item("Add Selection", "add"), ui.menu_item("Remove Selection", "remove"),
                ui.menu_item("Replace With Selection", "replace"),
                ui.menu_separator(),
                ui.menu_item("Key Set", "key"), ui.menu_item("Create Mirrored Set", "mirror"),
                ui.submenu("Color", [ui.menu_item(n, "color:" + c) for n, c in COLORS]),
                ui.menu_item("Rename...", "rename"),
                ui.menu_item("Move Left", "left"), ui.menu_item("Move Right", "right"),
                ui.menu_separator(), ui.menu_item("Delete", "delete")]

    # Actions ---------------------------------------------------------------------------------------------------------
    def _create(self, event=None):
        name = (self.get("name") or "").strip()
        members = members_from_selection()
        if not members:
            raise core.AnimTKError("Select controls first")
        self.store.create(name or "Set {}".format(len(self.store.sets) + 1), members)
        self.set("name", "value", "")
        self.rebuild()

    def _clicked(self, set_id, event):
        item = self.store.find(set_id)
        targets = resolve(item["members"], rig_mod.rigs())
        if not targets:
            raise core.AnimTKError("None of the controls in '{}' exist in the open sequence".format(item["name"]))
        if event.alt:
            with core.undo("Key {}".format(item["name"])):
                for rig, controls in targets:
                    rig.key(controls)
            return
        mode = "remove" if event.ctrl else ("add" if event.shift else "replace")
        if mode == "replace":
            # Clear every rig first so a set on one character deselects the others.
            for rig in rig_mod.rigs():
                rig.clear_selection()
            mode = "add"
        for rig, controls in targets:
            rig.select(controls, mode)

    def _menu_action(self, set_id, action):
        item = self.store.find(set_id)
        if action in ("add", "remove", "replace"):
            selection = members_from_selection()
            if not selection and action != "remove":
                raise core.AnimTKError("Select controls first")
            if action == "replace":
                item["members"] = selection
            elif action == "add":
                known = set((m["rig"], m["control"]) for m in item["members"])
                item["members"] += [m for m in selection if (m["rig"], m["control"]) not in known]
            else:
                drop = set(m["control"] for m in selection)
                item["members"] = [m for m in item["members"] if m["control"] not in drop]
            self.store.save()
        elif action == "key":
            with core.undo("Key {}".format(item["name"])):
                for rig, controls in resolve(item["members"], rig_mod.rigs()):
                    rig.key(controls)
            return
        elif action == "mirror":
            members = [{"rig": m["rig"], "control": mirror_name(m["control"]) or m["control"]} for m in item["members"]]
            name = mirror_name(item["name"]) or item["name"] + " (mirror)"
            self.store.create(name, members, item.get("color"))
        elif action.startswith("color:"):
            item["color"] = action.split(":", 1)[1]
            self.store.save()
        elif action == "rename":
            def rename(new_name):
                item["name"] = new_name
                self.store.save()
                self.rebuild()
            ui.prompt("Rename Set", "New name:", rename, item["name"])
            return
        elif action in ("left", "right"):
            self.store.move(set_id, -1 if action == "left" else 1)
        elif action == "delete":
            if not ui.confirm("Selection Sets", "Delete '{}'?".format(item["name"])):
                return
            self.store.delete(set_id)
        self.rebuild()


def show():
    return SelectionSetsTool.show()
