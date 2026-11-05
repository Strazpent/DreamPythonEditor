# SPDX-License-Identifier: Apache-2.0
"""Headless tests for the parts of animtk that do not need the editor (no ``unreal`` module).

Run:  python Plugins/AnimToolkit/Tests/python/test_animtk.py
"""

import json
import os
import py_compile
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(PLUGIN, "Content", "Python"))

from animtk import core, pose, rig, ui  # noqa: E402
from animtk import shelf as shelf_mod  # noqa: E402
from animtk.tools import character_loader, picker, selection_sets  # noqa: E402


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="animtk_test_")
        os.environ["ANIMTK_USER_DIR"] = os.path.join(self.tmp, "user")

    def tearDown(self):
        os.environ.pop("ANIMTK_USER_DIR", None)
        shutil.rmtree(self.tmp, ignore_errors=True)


# ----------------------------------------------------------------------------------------------------------------- ui

class UiSpecTests(unittest.TestCase):
    def test_spec_assigns_ids_only_to_widgets_with_handlers(self):
        root = ui.VBox(ui.Label("Title"), ui.Button("Go", on_clicked=lambda e: None), ui.Slider(id="amount"))
        spec = root.to_spec(ui._Ids())
        self.assertEqual(spec["type"], "vbox")
        label, button, slider = spec["children"]
        self.assertNotIn("id", label)
        self.assertEqual(button["id"], "button_1")
        self.assertEqual(slider["id"], "amount")
        self.assertNotIn("on_clicked", button)
        json.dumps(spec)  # must be JSON serialisable

    def test_none_props_are_dropped_and_tuples_become_lists(self):
        spec = ui.Button("x", icon=None, color=(1, 0, 0)).to_spec(ui._Ids())
        self.assertNotIn("icon", spec)
        self.assertEqual(spec["color"], [1, 0, 0])

    def test_children_lists_are_flattened(self):
        spec = ui.HBox([ui.Label("a"), [ui.Label("b"), None]], ui.Label("c")).to_spec(ui._Ids())
        self.assertEqual([c["text"] for c in spec["children"]], ["a", "b", "c"])

    def test_combo_defaults_to_first_option(self):
        self.assertEqual(ui.Combo(["One", "Two"]).props["value"], "One")

    def test_menu_helpers(self):
        entries = [ui.menu_header("H"), ui.menu_item("Run"), ui.menu_separator(), ui.submenu("More", [ui.menu_item("A", "a", checked=True)])]
        self.assertEqual(entries[1], {"label": "Run", "action": "Run"})
        self.assertTrue(entries[2]["separator"])
        self.assertTrue(entries[3]["submenu"][0]["checked"])

    def test_event_reads_missing_keys_as_none(self):
        event = ui.Event("w", "clicked", {"shift": True}, None)
        self.assertTrue(event.shift)
        self.assertIsNone(event.ctrl)

    def test_handlers_with_and_without_argument(self):
        seen = []
        ui._call(lambda: seen.append("none"), ui.Event("w", "e", {}, None))
        ui._call(lambda e: seen.append(e.name), ui.Event("w", "e", {}, None))
        self.assertEqual(seen, ["none", "e"])

    def test_panel_needs_the_plugin(self):
        with self.assertRaises(core.AnimTKError):
            ui.Panel("x", "X")


# ----------------------------------------------------------------------------------------------------------------- pose

class PoseTests(unittest.TestCase):
    def test_lerp_kinds(self):
        self.assertAlmostEqual(pose.lerp_value("float", 0.0, 10.0, 0.25), 2.5)
        self.assertEqual(pose.lerp_value("integer", 0, 3, 0.5), 2)
        self.assertEqual(pose.lerp_value("bool", False, True, 0.4), False)
        self.assertEqual(pose.lerp_value("bool", False, True, 0.6), True)
        self.assertEqual(pose.lerp_value("position", [0, 0, 0], [2, 4, 6], 0.5), [1, 2, 3])

    def test_rotations_take_the_short_way(self):
        self.assertAlmostEqual(pose.lerp_value("rotator", [170, 0, 0], [-170, 0, 0], 0.5)[0], 180.0)
        value = pose.lerp_value("euler_transform", {"t": [0, 0, 0], "r": [0, 350, 0], "s": [1, 1, 1]},
                                {"t": [10, 0, 0], "r": [0, 10, 0], "s": [3, 1, 1]}, 0.5)
        self.assertEqual(value["t"][0], 5)
        self.assertAlmostEqual(value["r"][1], 360.0)
        self.assertEqual(value["s"][0], 2)

    def test_mirror_names(self):
        cases = {
            "hand_l_ctrl": "hand_r_ctrl",
            "foot_r_ik_ctrl": "foot_l_ik_ctrl",
            "thumb_01_l": "thumb_01_r",
            "l_arm": "r_arm",
            "LeftHand": "RightHand",
            "Arm_L": "Arm_R",
            "spine_01_ctrl": None,
            "body_ctrl": None,
            "pelvis_low": None,
        }
        self.assertEqual(pose.side_of("hand_l_ctrl"), "left")
        self.assertEqual(pose.side_of("Arm_R"), "right")
        self.assertIsNone(pose.side_of("spine_01_ctrl"))
        for name, expected in cases.items():
            self.assertEqual(pose.mirror_name(name), expected, name)

    def test_mirror_clip_swaps_sides_and_flips_centre(self):
        clip = {"type": "pose", "controls": {
            "hand_l_ctrl": {"kind": "position", "values": [[1, 2, 3]]},
            "spine_ctrl": {"kind": "euler_transform", "values": [{"t": [5, 1, 1], "r": [10, 20, 30], "s": [1, 1, 1]}]},
        }}
        mirrored = pose.mirror_clip(clip)
        self.assertEqual(mirrored["controls"]["hand_r_ctrl"]["values"], [[1, 2, 3]])
        self.assertNotIn("hand_l_ctrl", mirrored["controls"])
        spine = mirrored["controls"]["spine_ctrl"]["values"][0]
        self.assertEqual(spine["t"], [-5, 1, 1])
        self.assertEqual(spine["r"], [10, -20, -30])
        self.assertEqual(clip["controls"]["spine_ctrl"]["values"][0]["t"], [5, 1, 1], "the input is not modified")


# ----------------------------------------------------------------------------------------------------------------- rig

class _V(object):
    def __init__(self, **kw):
        self.__dict__.update(kw)


class RigValueTests(unittest.TestCase):
    def test_to_python_with_duck_typed_values(self):
        self.assertEqual(rig.to_python("position", _V(x=1, y=2, z=3)), [1.0, 2.0, 3.0])
        self.assertEqual(rig.to_python("rotator", _V(roll=1, pitch=2, yaw=3)), [1.0, 2.0, 3.0])
        euler = _V(location=_V(x=1, y=0, z=0), rotation=_V(roll=0, pitch=90, yaw=0), scale=_V(x=1, y=1, z=1))
        self.assertEqual(rig.to_python("euler_transform", euler), {"t": [1.0, 0.0, 0.0], "r": [0.0, 90.0, 0.0], "s": [1.0, 1.0, 1.0]})
        self.assertEqual(rig.to_python("integer", 3.0), 3)
        with self.assertRaises(core.AnimTKError):
            rig.to_python("nonsense", 1)

    def test_every_kind_has_accessors(self):
        for kind in rig.KINDS:
            self.assertIn(kind, rig._ACCESSORS)

    def test_by_rig_groups_in_order(self):
        a, b = _V(name="A"), _V(name="B")
        grouped = rig.by_rig([(a, "x"), (b, "y"), (a, "z")])
        self.assertEqual([(r.name, names) for r, names in grouped], [("A", ["x", "z"]), ("B", ["y"])])


# ----------------------------------------------------------------------------------------------------------------- shelf

class ShelfTests(TempDirCase):
    def store(self, studio_dirs=()):
        return shelf_mod.ShelfStore(user_file=os.path.join(self.tmp, "shelves.json"),
                                    default_file=os.path.join(PLUGIN, "Resources", "Shelves", "default_shelves.json"),
                                    studio_dirs=list(studio_dirs))

    def test_seeded_from_defaults_and_saved(self):
        s = self.store()
        self.assertIn("Animation", s.names())
        self.assertEqual(s.active, "Animation")
        s.add_button("Animation", {"label": "Mine", "command": "print(1)"})
        reloaded = self.store()
        self.assertEqual(reloaded.find("Animation")["buttons"][-1]["label"], "Mine")

    def test_shelf_operations(self):
        s = self.store()
        name = s.add_shelf("Animation")
        self.assertEqual(name, "Animation 2")
        self.assertEqual(s.active, "Animation 2")
        s.rename_shelf("Animation 2", "Mine")
        s.add_button("Mine", {"label": "a", "command": "1"})
        s.add_button("Mine", {"label": "b", "command": "2"})
        s.move_button("Mine", 1, -1)
        self.assertEqual([b["label"] for b in s.find("Mine")["buttons"]], ["b", "a"])
        s.duplicate_button("Mine", 0)
        self.assertEqual(len(s.find("Mine")["buttons"]), 3)
        s.delete_button("Mine", 0)
        s.delete_shelf("Mine")
        self.assertIsNone(s.find("Mine"))

    def test_studio_shelves_are_read_only(self):
        studio = os.path.join(self.tmp, "studio")
        os.makedirs(studio)
        with open(os.path.join(studio, "team.json"), "w") as handle:
            json.dump({"name": "Team", "buttons": [{"label": "T", "command": "pass"}]}, handle)
        s = self.store([studio])
        self.assertTrue(s.find("Team")["read_only"])
        with self.assertRaises(core.AnimTKError):
            s.add_button("Team", {"label": "x", "command": "1"})
        copy_name = s.copy_shelf("Team")
        self.assertFalse(s.find(copy_name).get("read_only"))

    def test_buttons_need_something_to_run(self):
        with self.assertRaises(core.AnimTKError):
            shelf_mod.normalize_button({"label": "empty"})

    def test_button_from_file_uses_the_docstring(self):
        path = os.path.join(self.tmp, "key_all_the_things.py")
        with open(path, "w") as handle:
            handle.write('"""Keys every control.\n\nMore text."""\nprint(1)\n')
        button = shelf_mod.button_from_file(path)
        self.assertEqual(button["label"], "Key All The Things")
        self.assertEqual(button["tooltip"], "Keys every control.")

    def test_default_shelf_scripts_exist(self):
        s = self.store()
        for shelf in s.shelves():
            for button in shelf["buttons"]:
                shelf_mod.normalize_button(button)
                if button.get("script"):
                    self.assertTrue(os.path.isfile(shelf_mod.resolve_script(button["script"])), button["script"])
                if button.get("command"):
                    compile(button["command"], button["label"], "exec")

    def test_run_button_executes_inline_code_and_scripts(self):
        marker = os.path.join(self.tmp, "ran.txt")
        shelf_mod.run_button({"label": "inline", "command": "with open({!r}, 'w') as f: f.write('inline')".format(marker)})
        with open(marker) as handle:
            self.assertEqual(handle.read(), "inline")
        script = os.path.join(self.tmp, "script.py")
        with open(script, "w") as handle:
            handle.write("import sys\nopen({!r}, 'w').write(__name__ + ' ' + sys.argv[0])\n".format(marker))
        shelf_mod.run_button({"label": "script", "script": script})
        with open(marker) as handle:
            self.assertEqual(handle.read(), "__main__ " + script)


# ----------------------------------------------------------------------------------------------------------------- picker

class PickerTests(unittest.TestCase):
    def points(self):
        # A T-pose facing +Y: arms spread along X.
        return {"root": (0, 0, 0), "pelvis": (0, 0, 100), "head": (0, 0, 170), "hand_l": (80, 0, 140), "hand_r": (-80, 0, 140),
                "foot_l": (15, 5, 5), "foot_r": (-15, 5, 5), "pelvis_fk": (0, 0, 101)}

    def test_items_stay_on_the_canvas_and_do_not_overlap(self):
        items = picker.auto_layout(self.points(), view="front")
        self.assertEqual(len(items), 8)
        for item in items:
            self.assertTrue(0 <= item["x"] <= picker.CANVAS[0])
            self.assertTrue(0 <= item["y"] <= picker.CANVAS[1])
        for i, a in enumerate(items):
            for b in items[i + 1:]:
                overlap_x = abs(a["x"] - b["x"]) < (a["w"] + b["w"]) * 0.5
                overlap_y = abs(a["y"] - b["y"]) < (a["h"] + b["h"]) * 0.5
                self.assertFalse(overlap_x and overlap_y, (a["id"], b["id"]))

    def test_front_view_mirrors_like_facing_the_viewer(self):
        items = dict((i["id"], i) for i in picker.auto_layout(self.points(), view="front"))
        self.assertGreater(items["hand_l"]["x"], items["hand_r"]["x"], "the character's left is on screen right")
        self.assertLess(items["head"]["y"], items["pelvis"]["y"], "up is up")
        self.assertEqual(items["pelvis_fk"]["shape"], "circle")

    def test_orientation_follows_the_names_not_the_axis(self):
        flipped = dict((k, (-v[0], v[1], v[2])) for k, v in self.points().items())
        items = dict((i["id"], i) for i in picker.auto_layout(flipped, view="front"))
        self.assertGreater(items["hand_l"]["x"], items["hand_r"]["x"])

    def test_side_view_uses_the_other_axis(self):
        items = dict((i["id"], i) for i in picker.auto_layout(self.points(), view="side"))
        self.assertAlmostEqual(items["hand_l"]["x"], items["hand_r"]["x"], delta=25)

    def test_empty_rig(self):
        self.assertEqual(picker.auto_layout({}), [])


# ----------------------------------------------------------------------------------------------------------------- sets

class SelectionSetTests(TempDirCase):
    class FakeRig(object):
        def __init__(self, name, controls):
            self.name = name
            self._controls = controls

        def controls(self):
            return list(self._controls)

    def test_resolve_prefers_the_named_rig_then_falls_back(self):
        manny = self.FakeRig("Manny:1", ["hand_l", "hand_r"])
        quinn = self.FakeRig("Quinn:1", ["hand_l"])
        members = [{"rig": "Manny:1", "control": "hand_l"}, {"rig": "Gone", "control": "hand_l"}, {"rig": "Gone", "control": "nope"}]
        result = selection_sets.resolve(members, [manny, quinn])
        self.assertEqual([(r.name, c) for r, c in result], [("Manny:1", ["hand_l"]), ("Quinn:1", ["hand_l"])])

    def test_store_round_trip(self):
        store = selection_sets.SetStore(os.path.join(self.tmp, "sets.json"))
        item = store.create("Arms", [{"rig": "A", "control": "hand_l"}])
        store.create("Legs", [])
        store.move(item["id"], 1)
        again = selection_sets.SetStore(os.path.join(self.tmp, "sets.json"))
        self.assertEqual([s["name"] for s in again.sets], ["Legs", "Arms"])


# ----------------------------------------------------------------------------------------------------------------- characters

class CharacterRegistryTests(TempDirCase):
    def test_earlier_files_win_and_entries_need_a_rig(self):
        first = os.path.join(self.tmp, "a.json")
        second = os.path.join(self.tmp, "b.json")
        core.write_json(first, {"characters": [{"name": "Manny", "rig": "/Game/A"}, {"name": "NoRig"}]})
        core.write_json(second, {"characters": [{"name": "Manny", "rig": "/Game/B"}, {"name": "Quinn", "rig": "/Game/Q"}]})
        result = character_loader.load_characters([first, second])
        self.assertEqual([(c["name"], c["rig"]) for c in result], [("Manny", "/Game/A"), ("Quinn", "/Game/Q")])

    def test_shipped_registry_is_valid(self):
        result = character_loader.load_characters([os.path.join(PLUGIN, "Resources", "Characters", "characters.json")])
        self.assertTrue(result)
        for entry in result:
            self.assertTrue(entry["rig"].startswith("/Game/"))


# ----------------------------------------------------------------------------------------------------------------- files

class CompileTests(unittest.TestCase):
    def test_every_python_file_compiles(self):
        for folder in ("Content", "Resources", "Tests"):
            for dirpath, _dirs, files in os.walk(os.path.join(PLUGIN, folder)):
                for name in files:
                    if name.endswith(".py"):
                        py_compile.compile(os.path.join(dirpath, name), doraise=True, cfile=os.path.join(tempfile.gettempdir(), "animtk_compile.pyc"))

    def test_every_tool_module_imports(self):
        import importlib
        from animtk.tools import ALL
        for module_name, class_name, _label, _tip in ALL:
            module = importlib.import_module(module_name)
            cls = getattr(module, class_name)
            self.assertTrue(cls.panel_id)
            self.assertIn(module_name, cls.factory_command())


if __name__ == "__main__":
    unittest.main(verbosity=2)
