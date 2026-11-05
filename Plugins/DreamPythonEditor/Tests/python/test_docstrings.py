# SPDX-License-Identifier: Apache-2.0
"""The shared docstring parser (TDD §7.4.4): T-STUB-007 goldens, T-STUB-008 fuzz, T-STUB-022 imports.

The goldens are hand-built in the shapes PythonScriptPlugin 5.8 writes (PyGenUtil.cpp: BuildFunctionDocString,
PythonizeTooltip, AppendSourceInformationDocString, FGeneratedWrappedPropertyDoc::AppendDocString), with ``\\r\\n``
line ends as on Windows. The 500 docstrings captured from the G3 fixture (§7.4.4) need a running editor and are not
part of this suite.
"""

import ast
import os
import random
import sys
import unittest

from dream_editor._impl import docstrings as ds

CRLF = "\r\n"

METHOD = CRLF.join([
    "x.set_actor_location(new_location, sweep, teleport) -> HitResult or None",
    "Move the actor instantly to the specified location.",
    "",
    "Args:",
    "    new_location (Vector): The new location to teleport the Actor to.",
    "    sweep (bool): Whether we sweep to the destination location, triggering overlaps along the way and stopping",
    "        short of the target if blocked by something.",
    "    teleport (bool): Whether we teleport the physics state.",
    "",
    "Returns:",
    "    HitResult or None: The hit result, when sweeping.",
])

STATIC = CRLF.join([
    "X.load_asset(asset_path) -> Object",
    "Load an asset. If the asset is not loaded yet, it will be loaded.",
    "",
    "Args:",
    "    asset_path (str): Asset path of the asset to load",
    "",
    "Returns:",
    "    Object: Found or loaded asset.",
])

TUPLE = CRLF.join([
    "x.get_actor_bounds(only_colliding_components, include_from_child_actors=False) -> (origin=Vector, box_extent=Vector)",
    "Returns the bounding box of all components that make up this Actor.",
    "",
    "Args:",
    "    only_colliding_components (bool): If true, will only return the bounding box for components with collision.",
    "    include_from_child_actors (bool): If true then recurse in to ChildActor components.",
    "",
    "Returns:",
    "    tuple:",
    "",
    "    origin (Vector): Set to the center of the actor in world space",
    "",
    "    box_extent (Vector): Set to half the actor's size in 3d space",
])

BOOL_TUPLE = "X.line_trace_single(world_context_object, start, end) -> (out_hit=HitResult) or None" + CRLF + \
    "Does a collision trace along the given line and returns the first blocking hit encountered."

UNNAMED_FIRST = "X.get_float_value(curve, time) -> (float, out_found=bool)" + CRLF + "Reads a curve."

DEFAULTS = "x.spawn(location=Vector(x=0.0, y=0.0, z=0.0), label='a, b', count=[1, 2]) -> None" + CRLF + "Spawns."

NOARGS = "x.get_actor_label() -> str" + CRLF + "Returns this actor's current label."

RENAMED = "deprecated: 'k2_set_actor_location' was renamed to 'set_actor_location'."

DEPRECATED_METHOD = CRLF.join([
    "X.get_all_level_actors() -> Array[Actor]",
    "Find all loaded Actors in the world editor. Exclude actor that are pending kill, in PIE, PreviewEditor, ...",
    "deprecated: The Editor Scripting Utilities Plugin is deprecated - Use the function in Editor Actor Utilities Subsystem",
])

PROPERTY_RW = "(Array[Name]): [Read-Write] Array of tags that can be used for grouping and categorizing."
PROPERTY_RO = "(type(Actor)): [Read-Only] The class to spawn." + CRLF + "deprecated: Property 'spawn_class' is deprecated."

CLASS = CRLF.join([
    "Actor is the base class for an Object that can be placed or spawned in a level.",
    "Actors may contain a collection of ActorComponents.",
    "",
    "**C++ Source:**",
    "",
    "- **Module**: Engine",
    "- **File**: Actor.h",
    "",
    "**Editor Properties:** (see get_editor_property/set_editor_property)",
    "",
    "- ``can_be_damaged`` (bool): [Read-Write] Whether this actor can take damage.",
    "- ``initial_life_span`` (float): [Read-Write] How long this Actor lives before dying, 0=forever.",
    "  Note this is the INITIAL value and should not be modified once play has begun.",
    "- ``root_component`` (SceneComponent): [Read-Only] The component that defines the transform.",
    "- ``spawn_classes`` (Array[type(Actor)]): [Read-Write] Classes.",
])

PLUGIN_CLASS = CRLF.join([
    "Utility class to do most of the common functionalities with assets.",
    "deprecated: The Editor Scripting Utilities Plugin is deprecated",
    "",
    "**C++ Source:**",
    "",
    "- **Plugin**: EditorScriptingUtilities",
    "- **Module**: EditorScriptingUtilities",
    "- **File**: EditorAssetLibrary.h",
    "",
])

ASSET_CLASS = CRLF.join([
    "",
    "**Asset Source:**",
    "",
    "- **Package**: /Game/Blueprints/BP_Spinner",
    "",
    "**Editor Properties:** (see get_editor_property/set_editor_property)",
    "",
    "- ``speed`` (float): [Read-Write] Speed",
])

GLUE = [
    ("cast(cls: Type[_T], object: object) -> _T -- cast the given object to this Unreal object type or raise an "
     "exception if the cast is not possible", ("cast", ["object"], "_T")),
    ("static_class(cls) -> Class -- get the Unreal class of this type", ("static_class", [], "Class")),
    ("index(self, value: _ElemType, start: int = 0, stop: int = -1) -> int -- get the index of the first matching "
     "value in this Unreal array", ("index", ["value", "start", "stop"], "int")),
    ("get_editor_property(self, name: Union[Name, str]) -> object -- get the value of any property visible to the "
     "editor", ("get_editor_property", ["name"], "object")),
]


class Goldens(unittest.TestCase):
    """T-STUB-007 (hand-built goldens)."""

    def test_method(self):
        doc = ds.parse_method_doc(METHOD)
        self.assertFalse(doc.static)
        self.assertEqual(doc.name, "set_actor_location")
        self.assertEqual([p.name for p in doc.params], ["new_location", "sweep", "teleport"])
        self.assertEqual([p.type for p in doc.params], ["Vector", "bool", "bool"])
        self.assertTrue(doc.params[1].doc.endswith("short of the target if blocked by something."))
        self.assertEqual(doc.returns, "HitResult")
        self.assertTrue(doc.optional)
        self.assertEqual(doc.return_items, ())
        self.assertEqual(doc.tooltip, "Move the actor instantly to the specified location.")
        self.assertEqual(doc.returns_doc, "HitResult or None: The hit result, when sweeping.")
        self.assertIsNone(doc.deprecated)

    def test_static_and_defaults(self):
        doc = ds.parse_method_doc(STATIC)
        self.assertTrue(doc.static)
        self.assertEqual((doc.name, doc.returns, doc.params[0].type), ("load_asset", "Object", "str"))
        doc = ds.parse_method_doc(DEFAULTS)
        self.assertEqual([(p.name, p.default) for p in doc.params],
                         [("location", "Vector(x=0.0, y=0.0, z=0.0)"), ("label", "'a, b'"), ("count", "[1, 2]")])
        self.assertIsNone(doc.returns)
        self.assertEqual(ds.parse_method_doc(NOARGS).params, ())
        self.assertEqual(ds.parse_method_doc(NOARGS).returns, "str")

    def test_output_tuples(self):
        doc = ds.parse_method_doc(TUPLE)
        self.assertIsNone(doc.returns)
        self.assertEqual(doc.return_items, (("origin", "Vector"), ("box_extent", "Vector")))
        self.assertEqual(doc.params[1].default, "False")
        doc = ds.parse_method_doc(BOOL_TUPLE)
        self.assertEqual(doc.return_items, (("out_hit", "HitResult"),))
        self.assertTrue(doc.optional)
        doc = ds.parse_method_doc(UNNAMED_FIRST)
        self.assertEqual(doc.return_items, ((None, "float"), ("out_found", "bool")))

    def test_deprecation(self):
        self.assertIsNone(ds.parse_method_doc(RENAMED))
        self.assertEqual(ds.parse_deprecated(RENAMED), ("k2_set_actor_location", "set_actor_location"))
        self.assertTrue(ds.is_deprecated(RENAMED))
        doc = ds.parse_method_doc(DEPRECATED_METHOD)
        self.assertTrue(doc.deprecated.startswith("The Editor Scripting Utilities Plugin is deprecated"))
        self.assertEqual(doc.returns, "Array[Actor]")
        self.assertNotIn("deprecated", doc.tooltip)
        self.assertIsNone(ds.parse_deprecated(DEPRECATED_METHOD))
        self.assertFalse(ds.is_deprecated("A tooltip that mentions deprecated: in the middle of a line."))

    def test_properties(self):
        prop = ds.parse_property_doc(PROPERTY_RW)
        self.assertEqual((prop.type, prop.read_only), ("Array[Name]", False))
        self.assertTrue(prop.text.startswith("Array of tags"))
        prop = ds.parse_property_doc(PROPERTY_RO)
        self.assertEqual((prop.type, prop.read_only), ("type(Actor)", True))
        self.assertEqual(prop.deprecated, "Property 'spawn_class' is deprecated.")
        self.assertEqual(prop.text, "The class to spawn.")
        self.assertIsNone(ds.parse_property_doc("Not a property doc."))

    def test_class(self):
        doc = ds.parse_class_doc(CLASS)
        self.assertEqual(doc.tooltip, "Actor is the base class for an Object that can be placed or spawned in a level.\n"
                                      "Actors may contain a collection of ActorComponents.")
        self.assertEqual(doc.source, ds.SourceInfo("cpp", None, "Engine", "Actor.h", None))
        names = [(p.name, p.type, p.read_only) for p in doc.editor_properties]
        self.assertEqual(names, [("can_be_damaged", "bool", False), ("initial_life_span", "float", False),
                                 ("root_component", "SceneComponent", True),
                                 ("spawn_classes", "Array[type(Actor)]", False)])
        self.assertEqual(doc.editor_properties[1].tooltip, "How long this Actor lives before dying, 0=forever.\n"
                                                           "Note this is the INITIAL value and should not be modified "
                                                           "once play has begun.")
        self.assertIsNone(doc.deprecated)
        stripped = ds.strip_editor_properties(CLASS)
        self.assertNotIn("Editor Properties", stripped)
        self.assertTrue(stripped.endswith("- **File**: Actor.h"))
        plugin = ds.parse_class_doc(PLUGIN_CLASS)
        self.assertEqual(plugin.source.plugin, "EditorScriptingUtilities")
        self.assertEqual(plugin.deprecated, "The Editor Scripting Utilities Plugin is deprecated")
        self.assertEqual(plugin.editor_properties, ())
        asset = ds.parse_class_doc(ASSET_CLASS)
        self.assertEqual(asset.source.kind, "asset")
        self.assertEqual(asset.source.package, "/Game/Blueprints/BP_Spinner")
        self.assertEqual([p.name for p in asset.editor_properties], ["speed"])
        self.assertIsNone(ds.parse_class_doc(""))

    def test_glue(self):
        for doc, (name, params, returns) in GLUE:
            parsed = ds.parse_glue_doc(doc)
            self.assertEqual((parsed.name, [p.name for p in parsed.params], parsed.returns), (name, params, returns))
        parsed = ds.parse_glue_doc(GLUE[2][0])
        self.assertEqual([(p.type, p.default) for p in parsed.params], [("_ElemType", None), ("int", "0"), ("int", "-1")])
        self.assertIsNone(ds.parse_glue_doc(METHOD))

    def test_type_text(self):
        T = ds.TypeRef
        self.assertEqual(ds.parse_type_text("Array[Actor]"), T("Array", (T("Actor", ()),)))
        self.assertEqual(ds.parse_type_text("Map[str, int32]"), T("Map", (T("str", ()), T("int32", ()))))
        self.assertEqual(ds.parse_type_text("unreal.Optional[unreal.Texture]"), T("Optional", (T("Texture", ()),)))
        self.assertEqual(ds.parse_type_text("type(Actor)"), T("type", (T("Actor", ()),)))
        self.assertEqual(ds.parse_type_text("'undefined'"), T("'undefined'", ()))
        for bad in ("", "Array[", "Map[str,]", "a b", "Array]Actor[", None, 3):
            self.assertIsNone(ds.parse_type_text(bad), bad)
        self.assertEqual(ds.class_name("StaticMesh"), "StaticMesh")
        self.assertEqual(ds.class_name("Optional[StaticMesh]"), "StaticMesh")
        self.assertIsNone(ds.class_name("int32"))
        self.assertIsNone(ds.class_name("Array[Actor]"))
        self.assertEqual(ds.element_class("Array[Actor]"), "Actor")
        self.assertEqual(ds.element_class("Set[StaticMesh]"), "StaticMesh")
        self.assertEqual(ds.element_class("Optional[Array[Actor]]"), "Actor")
        self.assertIsNone(ds.element_class("Array[str]"))
        self.assertIsNone(ds.element_class("Map[str, Actor]"))
        self.assertEqual(ds.class_of_type("type(Actor)"), "Actor")
        self.assertIsNone(ds.class_of_type("Actor"))

    def test_normalize(self):
        self.assertEqual(ds.normalize("a\r\nb\rc\n"), "a\nb\nc\n")
        self.assertEqual(ds.normalize(None), "")
        self.assertEqual(ds.normalize(b"bytes"), "")


class Fuzz(unittest.TestCase):
    """T-STUB-008: fuzzed docstrings never raise; failures return None."""

    FUNCTIONS = (ds.parse_method_doc, ds.parse_glue_doc, ds.parse_property_doc, ds.parse_class_doc, ds.parse_deprecated,
                 ds.parse_type_text, ds.class_name, ds.element_class, ds.class_of_type, ds.is_deprecated,
                 ds.deprecation_message, ds.strip_editor_properties, ds.normalize)

    def test_mutations_never_raise(self):
        rng = random.Random(0x5EED)
        corpus = [METHOD, STATIC, TUPLE, BOOL_TUPLE, UNNAMED_FIRST, DEFAULTS, NOARGS, RENAMED, DEPRECATED_METHOD,
                  PROPERTY_RW, PROPERTY_RO, CLASS, PLUGIN_CLASS, ASSET_CLASS] + [g[0] for g in GLUE]
        alphabet = "()[]{},:='\"`-*\\\r\n\t xX.Args:Returns: or None->\x00\ud800\U0001F600"
        for _ in range(4000):
            text = rng.choice(corpus)
            for _ in range(rng.randint(1, 6)):
                if not text:
                    break
                at = rng.randrange(len(text) + 1)
                action = rng.random()
                if action < 0.4:
                    text = text[:at] + rng.choice(alphabet) * rng.randint(1, 3) + text[at:]
                elif action < 0.8:
                    text = text[:at] + text[at + rng.randint(1, 8):]
                else:
                    text = text[:at] + rng.choice(corpus)[: rng.randint(0, 40)] + text[at:]
            for function in self.FUNCTIONS:
                function(text)

    def test_non_strings(self):
        for value in (None, 0, 1.5, b"x.f() -> None", ["x.f() -> None"], object()):
            for function in self.FUNCTIONS:
                result = function(value)
                if function not in (ds.normalize, ds.strip_editor_properties, ds.is_deprecated):
                    self.assertIsNone(result, (function.__name__, value))


class EngineSpacing(unittest.TestCase):
    """T-STUB-007: the exact spacing UE 5.8.4 writes, copied from the engine's own docs (PyGenUtil.cpp PythonizeTooltip
    appends "): " and AppendPropertyPythonReadWriteState " [Read-Write]", so two spaces precede the bracket). A parser
    that wanted one space found no editor property on any of 11,114 real types, and SUM102 never fired (Dream.Language)."""

    PROPERTY = "(Array[StaticMaterial]):  [Read-Write]"
    PROPERTY_TEXT = ("(int32):  [Read-Write] Specifies which mesh LOD to use for complex (per-poly) collision." + CRLF +
                     "Sometimes it can be desirable to use a lower poly representation for collision.")
    CLASS = CRLF.join([
        "A StaticMesh is a piece of geometry that consists of a static set of polygons.",
        "",
        "**C++ Source:**",
        "",
        "- **Module**: Engine",
        "- **File**: StaticMesh.h",
        "",
        "**Editor Properties:** (see get_editor_property/set_editor_property)",
        "",
        "- ``light_map_resolution`` (int32):  [Read-Write] The light map resolution",
        "  used for this mesh.",
        "- ``lod_group`` (Name):  [Read-Write] The LOD group to which this mesh belongs.",
        "- ``nanite_settings`` (MeshNaniteSettings):  [Read-Only] Settings related to Nanite.",
    ])

    def test_property_two_spaces(self):
        prop = ds.parse_property_doc(self.PROPERTY)
        self.assertEqual((prop.type, prop.read_only, prop.text), ("Array[StaticMaterial]", False, ""))
        prop = ds.parse_property_doc(self.PROPERTY_TEXT)
        self.assertEqual(prop.type, "int32")
        self.assertTrue(prop.text.startswith("Specifies which mesh LOD"))

    def test_editor_properties_two_spaces(self):
        doc = ds.parse_class_doc(self.CLASS)
        self.assertEqual([(p.name, p.type, p.read_only) for p in doc.editor_properties],
                         [("light_map_resolution", "int32", False), ("lod_group", "Name", False),
                          ("nanite_settings", "MeshNaniteSettings", True)])
        self.assertEqual(doc.editor_properties[0].tooltip, "The light map resolution\nused for this mesh.")
        self.assertTrue(ds.strip_editor_properties(self.CLASS).endswith("- **File**: StaticMesh.h"))


class Imports(unittest.TestCase):
    """T-STUB-022: docstrings.py imports only the standard library (dream-check loads it by path, §7.3.3)."""

    def test_stdlib_only(self):
        path = os.path.join(os.path.dirname(ds.__file__), "docstrings.py")
        with open(path, "r", encoding="utf-8") as handle:
            tree = ast.parse(handle.read())
        imported = []
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported.extend(alias.name.split(".")[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom):
                self.assertEqual(node.level, 0, "relative import in docstrings.py")
                imported.append(node.module.split(".")[0])
        self.assertTrue(imported)
        for name in imported:
            self.assertIn(name, sys.stdlib_module_names, name)


if __name__ == "__main__":
    unittest.main()
