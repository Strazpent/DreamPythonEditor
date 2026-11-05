# SPDX-License-Identifier: Apache-2.0
"""Shared tier-2 fixtures for the lang tests: a fake ``unreal`` with real-shaped docstrings, a writer that does with
``stubs.step()``'s output what C++ (``FDreamStubSnapshot``) does, an in-process dream-check analysis, and a framed
JSON-RPC client for the dream-check subprocess.

The docstrings follow the shapes PythonScriptPlugin generates (``PyGenUtil.cpp``: BuildFunctionDocString,
PythonizeTooltip, AppendSourceInformationDocString, FGeneratedWrappedPropertyDoc::AppendDocString), with ``\\r\\n``
line ends as on Windows. The texts themselves are written for these tests.
"""

import hashlib
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.normpath(os.path.join(HERE, "..", ".."))
TOOLS_PYTHON = os.path.join(PLUGIN, "Tools", "Python")
LAUNCHER = os.path.join(TOOLS_PYTHON, "dream_launch.py")
FIXTURES = os.path.join(HERE, "check_fixtures")
SNAPSHOT_KEY = "0123456789abcdef"
ENGINE = {"version": "5.8.4-0", "python": "3.11.8"}

if TOOLS_PYTHON not in sys.path:
    sys.path.insert(0, TOOLS_PYTHON)

from dream_tools.check import analysis as check_analysis  # noqa: E402
from dream_tools.check.snapshot import SnapshotIndex  # noqa: E402

from dream_editor._impl import stubs  # noqa: E402

CRLF = "\r\n"


# ---- fake descriptors (slot-only: no getter of theirs can run without being counted) -------------------------------

def _fake_descriptor_type(name):
    """A slot-only type named like an engine or CPython descriptor type: its doc lives in a ``__doc__`` slot (read
    through the slot's member descriptor, as the walker reads C docs) and its ``__get__`` counts and fails."""
    def __init__(self, doc):
        self.__doc__ = doc

    def __get__(self, obj, owner=None):
        klass.calls += 1
        raise AssertionError("the getter of a %s ran" % name)

    klass = type(name, (), {"__slots__": ("__doc__",), "calls": 0, "__init__": __init__, "__get__": __get__,
                            "__module__": __name__})
    return klass


getset_descriptor = _fake_descriptor_type("getset_descriptor")
methodwithclosure_descriptor = _fake_descriptor_type("methodwithclosure_descriptor")
classmethodwithclosure_descriptor = _fake_descriptor_type("classmethodwithclosure_descriptor")
constant_value = _fake_descriptor_type("constant_value")
_EnumEntry = _fake_descriptor_type("_EnumEntry")
builtin_function_or_method = _fake_descriptor_type("builtin_function_or_method")

FAKE_DESCRIPTORS = (getset_descriptor, methodwithclosure_descriptor, classmethodwithclosure_descriptor, constant_value,
                    _EnumEntry, builtin_function_or_method)


def reset_counters():
    for klass in FAKE_DESCRIPTORS:
        klass.calls = 0
    _CountingProperty.calls = 0
    _CountingModule.getattr_calls = 0


def counter_total():
    return sum(k.calls for k in FAKE_DESCRIPTORS) + _CountingProperty.calls + _CountingModule.getattr_calls


class _CountingProperty:
    calls = 0

    @staticmethod
    def getter(self):
        _CountingProperty.calls += 1
        raise AssertionError("a property getter ran")


class _CountingModule:
    getattr_calls = 0

    @staticmethod
    def module_getattr(name):
        """A PEP 562 module ``__getattr__``: the walker must never call it."""
        _CountingModule.getattr_calls += 1
        raise AttributeError(name)


# ---- docstrings ----------------------------------------------------------------------------------------------------

def prop_doc(type_text, rw, text):
    return "(%s): [%s] %s" % (type_text, rw, text)


def method_doc(static, name, params, ret, tooltip, args=(), returns=None):
    lines = ["%s.%s(%s) -> %s" % ("X" if static else "x", name, ", ".join(params), ret), tooltip]
    if args:
        lines.append("")
        lines.append("Args:")
        lines.extend("    %s (%s): %s" % arg for arg in args)
    if returns is not None:
        lines.append("")
        lines.append("Returns:")
        lines.append("    " + returns)
    return CRLF.join(lines)


def class_doc(tooltip, module, file, eprops=(), plugin=None, deprecated=None):
    text = tooltip
    if deprecated:
        text += CRLF + "deprecated: " + deprecated
    text += CRLF + CRLF + "**C++ Source:**" + CRLF
    if plugin:
        text += CRLF + "- **Plugin**: " + plugin
    text += CRLF + "- **Module**: " + module + CRLF + "- **File**: " + file + CRLF
    if eprops:
        text += CRLF + "**Editor Properties:** (see get_editor_property/set_editor_property)" + CRLF
        for name, type_text, rw, tip in eprops:
            text += CRLF + "- ``%s`` (%s): [%s] %s" % (name, type_text, rw, tip.replace("\n", CRLF + "  "))
    return text


ACTOR_EPROPS = [
    ("can_be_damaged", "bool", "Read-Write", "Whether this actor can take damage."),
    ("initial_life_span", "float", "Read-Write", "How long this Actor lives before dying, 0=forever.\n"
                                                    "Note this is the INITIAL value and should not be modified once play has begun."),
    ("tags", "Array[Name]", "Read-Write", "Array of tags that can be used for grouping and categorizing."),
]


def _members(**members):
    return members


def build_fake_unreal():
    """{module name: module} for the fake ``unreal``: ``_unreal_core``, ``_unreal_engine``,
    ``_unreal_editorscriptingutilities``, ``_unreal_unrealed``, ``_unreal_python`` and ``unreal`` itself."""
    def make(name, bases, doc, members=None):
        namespace = {"__doc__": doc, "__module__": "builtins"}
        namespace.update(members or {})
        return type(name, bases, namespace)

    _ObjectBase = make("_ObjectBase", (), "Base type for all UE Object types.", {
        "cast": _builtin_with_doc("cast(cls: Type[_T], object: object) -> _T -- cast the given object to this Unreal "
                                  "object type or raise an exception if the cast is not possible"),
        "get_default_object": _builtin_with_doc("get_default_object(cls: Type[_T]) -> _T -- get the Unreal class "
                                                "default object (CDO) of this type"),
    })
    StructBase = make("StructBase", (), "Base type for all UE struct types.")
    EnumBase = make("EnumBase", (), "Base type for all UE enum types.")

    Object = make("Object", (_ObjectBase,), class_doc("The base class of all UE objects.", "CoreUObject", "Object.h"), {
        "get_name": methodwithclosure_descriptor(method_doc(False, "get_name", [], "str",
                                                            "Returns the name of this object (with no path information)",
                                                            returns="str: Name of the object.")),
        "get_outer": methodwithclosure_descriptor(method_doc(False, "get_outer", [], "Object",
                                                             "Returns the outer object of this object")),
        "static_class": len,        # a builtin_function_or_method: kind glue
    })
    Vector = make("Vector", (StructBase,), class_doc("A vector in 3-D space composed of components (X, Y, Z).",
                                                      "CoreUObject", "NoExportTypes.h", [
                                                          ("x", "double", "Read-Write", "X"),
                                                          ("y", "double", "Read-Write", "Y"),
                                                          ("z", "double", "Read-Write", "Z")]), {
        "x": getset_descriptor(prop_doc("double", "Read-Write", "X")),
        "y": getset_descriptor(prop_doc("double", "Read-Write", "Y")),
        "z": getset_descriptor(prop_doc("double", "Read-Write", "Z")),
        "ZERO": constant_value("Vector(x=0.0, y=0.0, z=0.0)"),
    })
    core = {"Object": Object, "Vector": Vector}

    ActorComponent = make("ActorComponent", (Object,), class_doc(
        "ActorComponent is the base class for components that define reusable behavior.", "Engine", "ActorComponent.h"), {
        "destroy_component": methodwithclosure_descriptor(method_doc(False, "destroy_component", ["object"], "None",
                                                                     "Unregister the component and destroy it.")),
    })
    SceneComponent = make("SceneComponent", (ActorComponent,), class_doc(
        "A SceneComponent has a transform and supports attachment.", "Engine", "SceneComponent.h", [
            ("relative_location", "Vector", "Read-Write", "Location of the component relative to its parent")]), {
        "relative_location": getset_descriptor(prop_doc("Vector", "Read-Only", "Location relative to its parent")),
        "set_world_location": methodwithclosure_descriptor(method_doc(
            False, "set_world_location", ["new_location", "sweep", "teleport"], "HitResult",
            "Put this component at the specified location in world space.")),
        "attach_to_component": methodwithclosure_descriptor(method_doc(
            False, "attach_to_component", ["parent", "socket_name"], "bool", "Attach this component to another.")),
    })
    StaticMeshComponent = make("StaticMeshComponent", (SceneComponent,), class_doc(
        "StaticMeshComponent is used to create an instance of a UStaticMesh.", "Engine", "StaticMeshComponent.h", [
            ("relative_location", "Vector", "Read-Write", "Location of the component relative to its parent"),
            ("static_mesh", "StaticMesh", "Read-Write", "The static mesh that this component uses to render")]), {
        "set_static_mesh": methodwithclosure_descriptor(method_doc(
            False, "set_static_mesh", ["new_mesh"], "bool", "Change the StaticMesh used by this instance.",
            args=[("new_mesh", "StaticMesh", "")], returns="bool:")),
        "get_static_mesh": methodwithclosure_descriptor(method_doc(False, "get_static_mesh", [], "StaticMesh",
                                                                   "Get the StaticMesh used by this instance.")),
    })
    Actor = make("Actor", (Object,), class_doc(
        "Actor is the base class for an Object that can be placed or spawned in a level.", "Engine", "Actor.h",
        ACTOR_EPROPS), {
        "tags": getset_descriptor(prop_doc("Array[Name]", "Read-Write",
                                           "Array of tags that can be used for grouping and categorizing.")),
        "can_be_damaged": getset_descriptor(prop_doc("bool", "Read-Write", "Whether this actor can take damage.")),
        "root_component": getset_descriptor(prop_doc("SceneComponent", "Read-Only",
                                                     "The component that defines the transform of this Actor.")),
        "set_actor_location": methodwithclosure_descriptor(method_doc(
            False, "set_actor_location", ["new_location", "sweep", "teleport"], "HitResult or None",
            "Move the actor instantly to the specified location.",
            args=[("new_location", "Vector", "The new location to teleport the Actor to."),
                  ("sweep", "bool", "Whether we sweep to the destination location."),
                  ("teleport", "bool", "Whether we teleport the physics state.")],
            returns="HitResult or None:")),
        "k2_set_actor_location": methodwithclosure_descriptor(
            "deprecated: 'k2_set_actor_location' was renamed to 'set_actor_location'."),
        "get_actor_label": methodwithclosure_descriptor(method_doc(False, "get_actor_label", [], "str",
                                                                   "Returns this actor's current label.")),
        "set_actor_label": methodwithclosure_descriptor(method_doc(False, "set_actor_label",
                                                                   ["new_actor_label", "mark_dirty=True"], "None",
                                                                   "Assigns a new label to this actor.")),
        "get_components_by_class": methodwithclosure_descriptor(method_doc(
            False, "get_components_by_class", ["component_class"], "Array[ActorComponent]",
            "Gets all the components that inherit from the given class.")),
        "destroy_actor": methodwithclosure_descriptor(method_doc(False, "destroy_actor", [], "None",
                                                                 "Destroy the actor.")),
        "modify": methodwithclosure_descriptor(method_doc(False, "modify", ["always_mark_dirty=True"], "bool",
                                                          "Note that the object will be modified.")),
        "hidden_flag": property(_CountingProperty.getter, doc="A Python property with a counting getter."),
    })
    StaticMeshActor = make("StaticMeshActor", (Actor,), class_doc(
        "StaticMeshActor is an instance of a UStaticMesh in the world.", "Engine", "StaticMeshActor.h",
        ACTOR_EPROPS + [("static_mesh_component", "StaticMeshComponent", "Read-Only", "The mesh component.")]), {
        "static_mesh_component": getset_descriptor(prop_doc("StaticMeshComponent", "Read-Only", "The mesh component.")),
    })
    StaticMesh = make("StaticMesh", (Object,), class_doc(
        "A StaticMesh is a piece of geometry that consists of a static set of polygons.", "Engine", "StaticMesh.h", [
            ("light_map_resolution", "int32", "Read-Write", "The light map resolution."),
            ("lod_group", "Name", "Read-Write", "The LOD group to use for this mesh."),
            ("light_map_coordinate_index", "int32", "Read-Write", "The UV channel of the light map.")]), {
        "light_map_resolution": getset_descriptor(prop_doc("int32", "Read-Only", "The light map resolution.")),
        "light_map_coordinate_index": getset_descriptor(prop_doc("int32", "Read-Write", "The UV channel.")),
        "get_num_lods": methodwithclosure_descriptor(method_doc(False, "get_num_lods", [], "int32",
                                                                "Returns the number of LODs used by the mesh.")),
    })
    Texture = make("Texture", (Object,), class_doc("Base class of textures.", "Engine", "Texture.h", [
        ("lod_bias", "int32", "Read-Write", "A bias to the index of the top mip level to use."),
        ("lod_group", "TextureGroup", "Read-Write", "Texture group this texture belongs to.")]), {
        "lod_bias": getset_descriptor(prop_doc("int32", "Read-Write", "A bias to the index of the top mip level.")),
        "lod_group": getset_descriptor(prop_doc("TextureGroup", "Read-Write", "Texture group.")),
    })
    Texture2D = make("Texture2D", (Texture,), class_doc("A 2-D texture.", "Engine", "Texture2D.h"))
    BlueprintFunctionLibrary = make("BlueprintFunctionLibrary", (Object,),
                                    class_doc("Base class for function libraries.", "Engine", "BlueprintFunctionLibrary.h"))
    Paths = make("Paths", (BlueprintFunctionLibrary,), class_doc("Path helpers for retrieving game dir, engine dir.",
                                                                 "Engine", "BlueprintPathsLibrary.h"), {
        "project_content_dir": classmethodwithclosure_descriptor(method_doc(
            True, "project_content_dir", [], "str", "Returns the content directory of the current game.")),
        "project_saved_dir": classmethodwithclosure_descriptor(method_doc(
            True, "project_saved_dir", [], "str", "Returns the saved directory of the current game.")),
    })
    SystemLibrary = make("SystemLibrary", (BlueprintFunctionLibrary,),
                         class_doc("Kismet System Library.", "Engine", "KismetSystemLibrary.h"), {
        "get_project_content_directory": classmethodwithclosure_descriptor(method_doc(
            True, "get_project_content_directory", [], "str", "Get the content directory of the current project.")),
    })
    CollisionChannel = make("CollisionChannel", (EnumBase,), class_doc("Enum indicating different type of objects.",
                                                                       "Engine", "EngineTypes.h"), {
        "ECC_WORLD_STATIC": _EnumEntry("WorldStatic"),
        "ECC_PAWN": _EnumEntry("Pawn"),
    })
    engine = {
        "ActorComponent": ActorComponent, "SceneComponent": SceneComponent, "StaticMeshComponent": StaticMeshComponent,
        "Actor": Actor, "StaticMeshActor": StaticMeshActor, "StaticMesh": StaticMesh, "Texture": Texture,
        "Texture2D": Texture2D, "BlueprintFunctionLibrary": BlueprintFunctionLibrary, "Paths": Paths,
        "SystemLibrary": SystemLibrary, "CollisionChannel": CollisionChannel,
    }

    EditorAssetLibrary = make("EditorAssetLibrary", (BlueprintFunctionLibrary,), class_doc(
        "Utility class to do most of the common functionalities with assets.", "EditorScriptingUtilities",
        "EditorAssetLibrary.h", plugin="EditorScriptingUtilities"), {
        "load_asset": classmethodwithclosure_descriptor(method_doc(
            True, "load_asset", ["asset_path"], "Object", "Load an asset.",
            args=[("asset_path", "str", "Asset path of the asset to load")], returns="Object: The asset loaded.")),
        "rename_asset": classmethodwithclosure_descriptor(method_doc(
            True, "rename_asset", ["source_asset_path", "destination_asset_path"], "bool", "Rename an asset.")),
        "does_asset_exist": classmethodwithclosure_descriptor(method_doc(
            True, "does_asset_exist", ["asset_path"], "bool", "Check if the asset exists.")),
        "list_assets": classmethodwithclosure_descriptor(method_doc(
            True, "list_assets", ["directory_path", "recursive=True", "include_folder=False"], "Array[str]",
            "Return the list of all the assets found in the directory path.")),
    })
    EditorLevelLibrary = make("EditorLevelLibrary", (BlueprintFunctionLibrary,), class_doc(
        "Utility class to do most of the common functionalities in the World Editor.", "EditorScriptingUtilities",
        "EditorLevelLibrary.h", plugin="EditorScriptingUtilities",
        deprecated="The Editor Scripting Utilities Plugin is deprecated - Use the function in Level Editor Subsystem"), {
        "get_all_level_actors": classmethodwithclosure_descriptor(method_doc(
            True, "get_all_level_actors", [], "Array[Actor]", "Find all loaded Actors in the world editor.")),
    })
    editorscriptingutilities = {"EditorAssetLibrary": EditorAssetLibrary, "EditorLevelLibrary": EditorLevelLibrary}

    EditorSubsystem = make("EditorSubsystem", (Object,), class_doc("Base class for auto instanced editor subsystems.",
                                                                   "EditorSubsystem", "EditorSubsystem.h"))
    EditorActorSubsystem = make("EditorActorSubsystem", (EditorSubsystem,), class_doc(
        "Editor actor utilities.", "UnrealEd", "EditorActorSubsystem.h"), {
        "get_all_level_actors": methodwithclosure_descriptor(method_doc(
            False, "get_all_level_actors", [], "Array[Actor]", "Find all loaded Actors in the world editor.")),
        "get_selected_level_actors": methodwithclosure_descriptor(method_doc(
            False, "get_selected_level_actors", [], "Array[Actor]", "Find all selected Actors.")),
        "destroy_actor": methodwithclosure_descriptor(method_doc(False, "destroy_actor", ["actor_to_destroy"], "bool",
                                                                 "Destroy the actor from the world editor.")),
        "set_actor_selection_state": methodwithclosure_descriptor(method_doc(
            False, "set_actor_selection_state", ["actor", "should_be_selected"], "None", "Set the selection state.")),
    })
    LevelUtils = make("LevelUtils", (BlueprintFunctionLibrary,), class_doc("Level helpers.", "UnrealEd", "LevelUtils.h"))
    EditorLevelUtils = make("EditorLevelUtils", (LevelUtils,), "deprecated: 'EditorLevelUtils' was renamed to 'LevelUtils'.")
    unrealed = {"EditorSubsystem": EditorSubsystem, "EditorActorSubsystem": EditorActorSubsystem,
                "LevelUtils": LevelUtils, "EditorLevelUtils": EditorLevelUtils}

    PyTestStruct = make("PyTestStruct", (StructBase,), class_doc("Test struct.", "PythonScriptPlugin", "PyTest.h"))
    PyTestObject = make("PyTestObject", (Object,), class_doc("Test object.", "PythonScriptPlugin", "PyTest.h"))
    PythonResourceOwner = make("PythonResourceOwner", (Object,), class_doc("Owner.", "PythonScriptPlugin", "PyTest.h"))
    python = {"PyTestStruct": PyTestStruct, "PyTestObject": PyTestObject, "PythonResourceOwner": PythonResourceOwner}

    ScopedEditorTransaction = make("ScopedEditorTransaction", (), "Context manager that opens an editor transaction.")
    BP_Spinner_C = make("BP_Spinner_C", (Actor,), "A Blueprint-generated class.")

    modules = {}
    for name, entries in (("core", core), ("engine", engine), ("editorscriptingutilities", editorscriptingutilities),
                          ("unrealed", unrealed), ("python", python)):
        module = types.ModuleType("_unreal_" + name)
        module.__dict__.update(entries)
        modules["_unreal_" + name] = module

    unreal = types.ModuleType("unreal")
    unreal.__dict__["__getattr__"] = _CountingModule.module_getattr
    for module in modules.values():
        for key, value in module.__dict__.items():
            if not key.startswith("__"):
                unreal.__dict__[key] = value
    unreal.__dict__.update({
        "_ObjectBase": _ObjectBase, "StructBase": StructBase, "EnumBase": EnumBase,
        "ScopedEditorTransaction": ScopedEditorTransaction, "BP_Spinner_C": BP_Spinner_C,
        "log": print,
        "log_warning": print,
        "get_editor_subsystem": _builtin_with_doc(
            "get_editor_subsystem(subsystem: Type[_EditorSubsystemType]) -> _EditorSubsystemType -- get an editor subsystem"),
        "load_asset": _builtin_with_doc(
            "load_asset(name: str, outer: Optional[Object] = None, type: Optional[Class] = None) -> Optional[Object] "
            "-- load an asset"),
        "get_level_actors_old": _builtin_with_doc(
            "get_level_actors_old() -> Array[Actor] -- get every actor of the level" + CRLF
            + "deprecated: 'get_level_actors_old' was renamed to 'get_level_actors'."),
        "DREAM_TEST_DATA": 3,
        "DREAM_TEST_NAN": float("nan"),
        "sys": sys,
    })
    modules["unreal"] = unreal
    return modules


def _builtin_with_doc(doc):
    """A slot-only callable stand-in for a C glue function (kind glue by name), with its own doc."""
    return builtin_function_or_method(doc)



# ---- the C++ side of the snapshot ------------------------------------------------------------------------------------

class SnapshotWriter:
    """Writes ``stubs.step()`` output as FDreamStubSnapshot does: lines kept per module until the module is done, then
    ``modules/<m>.jsonl``; ``progress.json`` lists finished entries; ``manifest.json`` then ``.complete`` last."""

    def __init__(self, folder):
        self.folder = folder
        self.pending = {}
        self.entries = []
        self.max_step_ns = 0
        self.steps = 0
        os.makedirs(os.path.join(folder, "modules"), exist_ok=True)

    def apply(self, output):
        for kind, module, data in output:
            if kind == 0:
                self.pending.setdefault(module, []).append(data)
            elif kind == 1:
                entry = json.loads(data.decode("utf-8"))
                body = b"".join(line + b"\n" for line in self.pending.pop(module, []))
                if hashlib.sha256(body).hexdigest() != entry["sha256"]:
                    raise AssertionError("sha256 of %s does not match its entry" % module)
                path = os.path.join(self.folder, "modules", "%s.jsonl" % module)
                with open(path + ".tmp", "wb") as handle:
                    handle.write(body)
                os.replace(path + ".tmp", path)
                self.entries.append(entry)
                with open(os.path.join(self.folder, "progress.json"), "w", encoding="utf-8") as handle:
                    json.dump(self.entries, handle)
            elif kind == 2:
                with open(os.path.join(self.folder, "manifest.json"), "wb") as handle:
                    handle.write(data)
                with open(os.path.join(self.folder, ".complete"), "wb"):
                    pass

    def run(self, key=SNAPSHOT_KEY, modules=None, done=None, budget_ns=4_000_000, max_steps=None, stop_after_modules=None):
        """Steps a walk; True when it completed. max_steps or stop_after_modules interrupt it (the walk is then
        forgotten, as at an editor exit, and its finished modules stay in progress.json)."""
        stubs.start(key, ENGINE, done if done is not None else [], _modules=modules)
        count = 0
        while stubs.is_active():
            interrupted = (max_steps is not None and count >= max_steps) or \
                (stop_after_modules is not None and len(self.entries) >= stop_after_modules)
            if interrupted:
                stubs.stop()
                return False
            began = time.perf_counter_ns()
            output = stubs.step(budget_ns)
            elapsed = time.perf_counter_ns() - began
            self.max_step_ns = max(self.max_step_ns, elapsed)
            self.apply(output)
            count += 1
            self.steps += 1
        return True


_SNAPSHOT_DIR = None


def fixture_snapshot():
    """A complete snapshot of the fake ``unreal`` (built once per test run, in a temporary folder)."""
    global _SNAPSHOT_DIR
    if _SNAPSHOT_DIR is None:
        folder = os.path.join(tempfile.mkdtemp(prefix="dream-snapshot-"), "snapshot")
        SnapshotWriter(folder).run(modules=build_fake_unreal())
        _SNAPSHOT_DIR = folder
    return _SNAPSHOT_DIR


def fixture_index():
    return SnapshotIndex(fixture_snapshot(), SNAPSHOT_KEY)


# ---- analysis ---------------------------------------------------------------------------------------------------------

def read_fixture(name):
    with open(os.path.join(FIXTURES, name), "r", encoding="utf-8") as handle:
        return handle.read()


def analyse(text, config=None, index=True):
    if index is True:
        index = fixture_index()
    return check_analysis.analyze(text, config, index)


def codes(diagnostics, prefix="SUM"):
    return [d.code for d in diagnostics if d.code.startswith(prefix)]


def lines_of(diagnostics, code):
    return [d.start[0] + 1 for d in diagnostics if d.code == code]


def apply_edits(text, edits):
    """Applies (start, end, new text) edits in (line, byte column) positions, highest first."""
    lines = text.split("\n")

    def offset(position):
        line, byte_col = position
        prefix = "\n".join(lines[:line])
        start = len(prefix) + (1 if line > 0 else 0)
        char = len(lines[line].encode("utf-8")[:byte_col].decode("utf-8", "ignore"))
        return start + char

    result = text
    for start, end, new_text in sorted(edits, key=lambda e: e[0], reverse=True):
        a, b = offset(start), offset(end)
        result = result[:a] + new_text + result[b:]
    return result


# ---- the subprocess client ----------------------------------------------------------------------------------------

def encode(message):
    body = json.dumps(message).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


class ServerProcess:
    """dream-check as the editor runs it, with framed JSON over pipes and a reader thread (every wait has a timeout).
    Server ``workspace/configuration`` requests are answered with ``config`` (a dict, or None for a null answer)."""

    def __init__(self, data=None, key=None, parent_pid=None, config=None, extra_args=()):
        args = [sys.executable, "-I", LAUNCHER, "check"]
        if data:
            args += ["--data", data]
        if key:
            args += ["--key", key]
        if parent_pid:
            args += ["--parent-pid", str(parent_pid)]
        args += list(extra_args)
        self.config = config
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.messages = queue.Queue()
        self.stderr_lines = []
        self.next_id = 1
        self.notifications = []
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._read_err, daemon=True).start()

    def _read(self):
        stream = self.process.stdout
        while True:
            header = b""
            while not header.endswith(b"\r\n\r\n"):
                char = stream.read(1)
                if not char:
                    self.messages.put(None)
                    return
                header += char
            length = int(header.split(b":")[1].strip())
            body = stream.read(length)
            message = json.loads(body.decode("utf-8"))
            if message.get("method") == "workspace/configuration" and "id" in message:
                items = message["params"]["items"]
                answer = None if self.config is None else [dict(self.config) for _ in items]
                self.send({"jsonrpc": "2.0", "id": message["id"], "result": answer})
                continue
            self.messages.put(message)

    def _read_err(self):
        for line in self.process.stderr:
            self.stderr_lines.append(line.decode("utf-8", "replace").rstrip())

    def send(self, message):
        message.setdefault("jsonrpc", "2.0")
        try:
            self.process.stdin.write(encode(message))
            self.process.stdin.flush()
        except OSError:
            pass

    def notify(self, method, params):
        self.send({"method": method, "params": params})

    def request(self, method, params, timeout=10.0):
        request_id = self.next_id
        self.next_id += 1
        self.send({"id": request_id, "method": method, "params": params})
        return self.wait(lambda m: m.get("id") == request_id and "method" not in m, timeout)

    def wait(self, predicate, timeout=10.0):
        deadline = time.monotonic() + timeout
        while True:
            for message in self.notifications:
                if predicate(message):
                    self.notifications.remove(message)
                    return message
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError("timed out waiting for a message; stderr:\n" + "\n".join(self.stderr_lines[-20:]))
            try:
                message = self.messages.get(timeout=remaining)
            except queue.Empty:
                continue
            if message is None:
                raise AssertionError("the server exited; stderr:\n" + "\n".join(self.stderr_lines[-20:]))
            if predicate(message):
                return message
            self.notifications.append(message)

    def wait_diagnostics(self, uri, version=None, timeout=10.0):
        def match(message):
            if message.get("method") != "textDocument/publishDiagnostics":
                return False
            params = message["params"]
            return params["uri"] == uri and (version is None or params.get("version") == version)
        return self.wait(match, timeout)["params"]

    def initialize(self, encodings=("utf-8", "utf-16"), process_id=None):
        params = {"processId": process_id if process_id is not None else os.getpid(), "rootUri": None,
                  "capabilities": {"general": {"positionEncodings": list(encodings)}}, "initializationOptions": {}}
        response = self.request("initialize", params)
        self.notify("initialized", {})
        return response["result"]

    def open(self, uri, text, version=1):
        self.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "python", "version": version,
                                                               "text": text}})

    def close(self, timeout=5.0):
        """shutdown then exit; returns the exit code."""
        try:
            if self.process.poll() is None:
                self.request("shutdown", None, timeout)
                self.notify("exit", None)
            return self.process.wait(timeout)
        finally:
            self.kill()

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                pass
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            try:
                stream.close()
            except OSError:
                pass


def file_uri(name):
    return "file:///C:/Project/Scripts/%s" % name
