# SPDX-License-Identifier: Apache-2.0
"""Character Loader: pick a character, press Load, animate. Like a rig referencer in Maya.

Loading a character:
  1. uses the open Level Sequence (or creates and opens ``/Game/Cinematics/AnimTK_Scratch``),
  2. spawns the character's skeletal mesh as an actor with a namespace label ("Manny:1"),
  3. adds it to the sequence (possessable, or spawnable when asked),
  4. adds a Control Rig track with the character's Control Rig, ready to key.

Characters come from JSON registries, first match wins: every folder in ``ANIMTK_CHARACTER_PATH`` (studio),
``Saved/AnimToolkit/characters.json`` (yours: Add, Scan and drag-and-drop write here) and the plugin's
``Resources/Characters/characters.json`` (examples for the UE5 mannequins). An entry::

    {"name": "Manny", "rig": "/Game/Characters/Mannequins/Rigs/CR_Mannequin_Body",
     "mesh": "/Game/Characters/Mannequins/Meshes/SKM_Manny", "thumbnail": "", "category": "Mannequins"}

"Scene" lists what the loader put in the level (actor tag ``AnimTK.Character``) with Select, Reload Rig, Swap Rig
and Remove.
"""

import glob
import os

from .. import core, seq, ui
from .. import rig as rig_mod

unreal = core.unreal

TAG = "AnimTK.Character"
RIG_TAG_PREFIX = "AnimTK.Rig="


# ------------------------------------------------------------------------------------------------------------- registry

def registry_files():
    files = []
    for folder in core.search_path("ANIMTK_CHARACTER_PATH"):
        files.extend(sorted(glob.glob(os.path.join(folder, "*.json"))))
    files.append(core.user_dir("characters.json"))
    files.append(core.resource("Characters", "characters.json"))
    return files


def load_characters(files=None):
    """Merged character list; an earlier file wins for a name that appears twice."""
    seen = {}
    order = []
    for path in files or registry_files():
        data = core.read_json(path, {}) or {}
        for entry in data.get("characters", []):
            name = entry.get("name")
            if not name or name in seen or not entry.get("rig"):
                continue
            entry = dict(entry)
            entry["source"] = path
            seen[name] = entry
            order.append(name)
    return [seen[n] for n in order]


def save_user_character(entry):
    path = core.user_dir("characters.json")
    data = core.read_json(path, {}) or {}
    characters = [c for c in data.get("characters", []) if c.get("name") != entry["name"]]
    clean = dict((k, v) for k, v in entry.items() if k != "source")
    characters.append(clean)
    data["characters"] = characters
    core.write_json(path, data)


def remove_user_character(name):
    path = core.user_dir("characters.json")
    data = core.read_json(path, {}) or {}
    data["characters"] = [c for c in data.get("characters", []) if c.get("name") != name]
    core.write_json(path, data)


def character_from_rig(rig_path):
    """An entry built from a Control Rig asset: its preview mesh becomes the character's mesh."""
    rig_asset = core.load_asset(rig_path)
    mesh = unreal.ControlRigBlueprintLibrary.get_preview_mesh(rig_asset)
    if mesh is None:
        raise core.AnimTKError("{} has no preview mesh; set one in the Control Rig editor or add the character by hand".format(rig_path))
    name = rig_path.rsplit("/", 1)[-1].split(".")[0]
    for prefix in ("CR_", "CTRL_", "Rig_"):
        if name.startswith(prefix):
            name = name[len(prefix):]
    return {"name": name, "rig": rig_path.split(".")[0], "mesh": mesh.get_path_name().split(".")[0], "category": "Scanned"}


def scan_project(root="/Game"):
    """Every Control Rig asset under ``root`` that has a preview mesh."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    class_path = unreal.TopLevelAssetPath("/Script/ControlRigDeveloper", "ControlRigBlueprint")
    found = []
    for data in registry.get_assets_by_class(class_path, True) or []:
        package = str(data.package_name)
        if not package.startswith(root):
            continue
        try:
            found.append(character_from_rig(package))
        except core.AnimTKError:
            continue
    return found


# ------------------------------------------------------------------------------------------------------------- scene

def loaded_characters():
    """[(actor, character name, rig path)] for actors the loader put in the level."""
    result = []
    for actor in core.actor_subsystem().get_all_level_actors():
        tags = [str(t) for t in actor.tags]
        if TAG not in tags:
            continue
        rig_path = next((t[len(RIG_TAG_PREFIX):] for t in tags if t.startswith(RIG_TAG_PREFIX)), "")
        result.append((actor, actor.get_actor_label(), rig_path))
    return result


def next_namespace(name):
    taken = set(label for _a, label, _r in loaded_characters())
    index = 1
    while "{}:{}".format(name, index) in taken:
        index += 1
    return "{}:{}".format(name, index)


def load_character(entry, namespace=None, spawnable=False, location=None):
    """Spawns ``entry`` and gives it a Control Rig track in the open (or scratch) sequence. Returns (actor, rig)."""
    namespace = namespace or next_namespace(entry["name"])
    mesh = core.load_asset(entry["mesh"]) if entry.get("mesh") else None
    rig_asset = core.load_asset(entry["rig"])
    if mesh is None:
        mesh = unreal.ControlRigBlueprintLibrary.get_preview_mesh(rig_asset)
    if mesh is None:
        raise core.AnimTKError("{} has no mesh and its rig has no preview mesh".format(entry["name"]))

    sequence = seq.ensure_sequence()
    world = core.editor_world()
    if location is None:
        location = unreal.Vector(150.0 * len(loaded_characters()), 0.0, 0.0)

    with core.undo("Load {}".format(namespace)):
        actor = core.actor_subsystem().spawn_actor_from_object(mesh, location, unreal.Rotator(0, 0, 0))
        if actor is None:
            raise core.AnimTKError("Could not spawn {}".format(entry["mesh"]))
        actor.set_actor_label(namespace)
        tags = list(actor.tags)
        tags += [unreal.Name(TAG), unreal.Name(RIG_TAG_PREFIX + entry["rig"])]
        actor.tags = tags

        binding = seq.add_actor(actor, spawnable=spawnable)
        rig_class = rig_asset.generated_class()
        track = unreal.ControlRigSequencerLibrary.find_or_create_control_rig_track(world, sequence, rig_class, binding)
        if track is None:
            raise core.AnimTKError("Sequencer did not add a Control Rig track for {}".format(entry["rig"]))
    seq.refresh()
    core.select_actors([actor])
    core.notify("Loaded {} ({})".format(namespace, entry["rig"].rsplit("/", 1)[-1]), "success")
    return actor, rig_mod.find_rig(namespace, sequence)


def rig_tracks_for(actor):
    sequence = seq.current()
    if sequence is None:
        return None, []
    binding = seq.binding_for_actor(actor, sequence)
    if binding is None:
        return None, []
    return binding, [t for t in binding.get_tracks() if isinstance(t, unreal.MovieSceneControlRigParameterTrack)]


def swap_rig(actor, rig_path):
    """Replaces the actor's Control Rig track with one running ``rig_path`` (keys of the old track are dropped)."""
    binding, tracks = rig_tracks_for(actor)
    if binding is None:
        raise core.AnimTKError("{} is not in the open sequence".format(actor.get_actor_label()))
    rig_asset = core.load_asset(rig_path)
    with core.undo("Swap rig on {}".format(actor.get_actor_label())):
        for track in tracks:
            binding.remove_track(track)
        unreal.ControlRigSequencerLibrary.find_or_create_control_rig_track(core.editor_world(), seq.require(), rig_asset.generated_class(), binding)
        actor.tags = [t for t in actor.tags if not str(t).startswith(RIG_TAG_PREFIX)] + [unreal.Name(RIG_TAG_PREFIX + rig_path)]
    seq.refresh()


def remove_character(actor):
    binding, _tracks = rig_tracks_for(actor)
    with core.undo("Remove {}".format(actor.get_actor_label())):
        if binding is not None:
            binding.remove()
        core.actor_subsystem().destroy_actor(actor)
    seq.refresh()


# ------------------------------------------------------------------------------------------------------------- tool

class CharacterLoaderTool(ui.Tool):
    panel_id = "animtk_character_loader"
    title = "Character Loader"

    def __init__(self):
        super(CharacterLoaderTool, self).__init__()
        self.characters = []
        self.selected = None
        self.scene_selected = []

    def build(self):
        self.characters = load_characters()
        if self.selected not in [c["name"] for c in self.characters]:
            self.selected = self.characters[0]["name"] if self.characters else None
        entry = self._entry()
        tiles = [{"id": c["name"], "label": c["name"], "icon": c.get("thumbnail", ""),
                  "tooltip": "{}\nRig: {}\nMesh: {}\nFrom: {}".format(c["name"], c["rig"], c.get("mesh", "(rig preview mesh)"), c["source"])}
                 for c in self.characters]
        return ui.VBox(
            ui.Label("Characters", style="title"),
            ui.Label("Double-click to load. Drop Control Rig assets here to add characters.", style="small", color=[0.6, 0.6, 0.6, 1]),
            ui.TileView(tiles, tile_size=72, id="characters", selected=[self.selected] if self.selected else [],
                        accept_drop=True, on_drop=self._drop, on_selection=self._select, on_double_click=lambda e: self._load(),
                        menu=[ui.menu_item("Load", "load"), ui.menu_item("Remove From My List", "forget")], on_menu=self._character_menu,
                        stretch=1),
            ui.Group(
                ui.Label(entry["name"] if entry else "No character selected", bold=True, id="details_name"),
                ui.Label(self._details(entry), style="small", id="details", wrap=True),
                ui.HBox(ui.Label("Namespace", min_width=70),
                        ui.TextField(next_namespace(entry["name"]) if entry else "", id="namespace")),
                ui.HBox(ui.CheckBox("Spawnable", value=False, id="spawnable",
                                    tooltip="Owned by the sequence (spawned when it plays) instead of living in the level"),
                        ui.Spacer(stretch=1),
                        ui.Button("Load Into Scene", color="#2E86C1", on_clicked=self._load, enabled=entry is not None)),
            ),
            ui.HBox(ui.Button("Scan Project", tooltip="Add every Control Rig with a preview mesh", on_clicked=self._scan),
                    ui.Button("Refresh", on_clicked=self.rebuild), ui.Spacer(stretch=1)),
            ui.Separator(),
            ui.Label("In This Level", style="title"),
            ui.ListView(self._scene_items(), id="scene", stretch=0.6, on_selection=self._scene_select,
                        on_double_click=lambda e: self._scene_action("select"),
                        menu=[ui.menu_item("Select Actor", "select"), ui.menu_item("Select All Controls", "controls"),
                              ui.menu_item("Reload Rig", "reload"),
                              ui.submenu("Swap Rig", [ui.menu_item(c["name"], "swap:" + c["rig"]) for c in self.characters]),
                              ui.menu_separator(), ui.menu_item("Remove From Scene", "remove")],
                        on_menu=lambda e: self._scene_action(e.action)),
        )

    # Helpers ---------------------------------------------------------------------------------------------------------
    def _entry(self):
        for c in self.characters:
            if c["name"] == self.selected:
                return c
        return None

    def _details(self, entry):
        if not entry:
            return "Add characters with Scan Project, by dropping Control Rig assets above, or in characters.json."
        status = []
        for key in ("rig", "mesh"):
            path = entry.get(key)
            if path:
                status.append("{}: {}{}".format(key.title(), path, "" if core.asset_exists(path) else "  (MISSING)"))
        return "\n".join(status)

    def _scene_items(self):
        items = []
        for actor, label, rig_path in loaded_characters():
            items.append({"id": actor.get_path_name(), "label": "{}   ({})".format(label, rig_path.rsplit("/", 1)[-1]), "tooltip": rig_path})
        return items

    def _scene_actors(self):
        wanted = set(self.scene_selected)
        return [a for a, _l, _r in loaded_characters() if a.get_path_name() in wanted]

    # Events ----------------------------------------------------------------------------------------------------------
    def _select(self, event):
        self.selected = (event.items or [None])[0]
        entry = self._entry()
        self.set("details_name", "text", entry["name"] if entry else "")
        self.set("details", "text", self._details(entry))
        self.set("namespace", "value", next_namespace(entry["name"]) if entry else "")

    def _load(self):
        entry = self._entry()
        if entry is None:
            raise core.AnimTKError("Select a character first")
        namespace = (self.get("namespace") or "").strip() or None
        load_character(entry, namespace=namespace, spawnable=bool(self.get("spawnable")))
        self.set("scene", "items", self._scene_items())
        self.set("namespace", "value", next_namespace(entry["name"]))

    def _drop(self, event):
        added = 0
        for path in event.assets or []:
            try:
                save_user_character(character_from_rig(path))
                added += 1
            except core.AnimTKError as exc:
                core.notify(str(exc), "warning")
        if added:
            core.notify("Added {} character(s)".format(added), "success")
            self.rebuild()

    def _scan(self):
        found = scan_project()
        known = set(c["name"] for c in self.characters)
        new = [c for c in found if c["name"] not in known]
        for entry in new:
            save_user_character(entry)
        core.notify("Scan found {} rig(s), {} new".format(len(found), len(new)), "success")
        self.rebuild()

    def _character_menu(self, event):
        if event.action == "load":
            self._load()
        elif event.action == "forget" and event.item:
            remove_user_character(event.item)
            self.rebuild()

    def _scene_select(self, event):
        self.scene_selected = list(event.items or [])

    def _scene_action(self, action):
        actors = self._scene_actors()
        if not actors:
            raise core.AnimTKError("Select a character in the list first")
        if action == "select":
            core.select_actors(actors)
        elif action == "controls":
            for actor in actors:
                found = rig_mod.find_rig(actor.get_actor_label())
                if found is not None:
                    found.select(found.controls(), "replace")
        elif action == "reload":
            for actor in actors:
                rig_path = next((str(t)[len(RIG_TAG_PREFIX):] for t in actor.tags if str(t).startswith(RIG_TAG_PREFIX)), "")
                if rig_path and ui.confirm("Reload Rig", "Recreate the Control Rig track of {}? Its keys are removed.".format(actor.get_actor_label())):
                    swap_rig(actor, rig_path)
        elif action.startswith("swap:"):
            for actor in actors:
                if ui.confirm("Swap Rig", "Replace the Control Rig of {}? Its keys are removed.".format(actor.get_actor_label())):
                    swap_rig(actor, action.split(":", 1)[1])
        elif action == "remove":
            if ui.confirm("Remove", "Remove {} character(s) from the level and the sequence?".format(len(actors))):
                for actor in actors:
                    remove_character(actor)
        self.set("scene", "items", self._scene_items())


def show():
    return CharacterLoaderTool.show()
