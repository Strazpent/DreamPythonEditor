# SPDX-License-Identifier: Apache-2.0
"""The live resolver (TDD §7.5, §7.12 T-RES-001..027 tier-2 rows; D6, E1, E2, INT-02, RV-21, RV-23, U19).

A fake ``unreal`` module mimics the engine's shapes: wrapped classes with property getsets ("(Type): [Read-Write] ..."
docs), methodwithclosure / classmethodwithclosure descriptors, constant_value, an _EnumType metaclass with _EnumEntry
descriptors, class docs with the Editor Properties block (CRLF line ends, as PyGenUtil writes them), and Array / Map
containers. Every fake that the resolver must never call counts its calls. ``_dream.hard_object_props`` and
``_dream.container_element_kind`` are faked on the test's NullDream (they are not in the M1 surface).

T-RES-023 (hover on ``sel``, M3) and T-RES-029 (saferepr and hover, §7.7, M2) are not resolver completion work and are
not here.
"""

import builtins
import collections
import inspect
import sys
import time
import types
import unittest
from unittest import mock

from dream_testing import RunnerTestCase

from dream_editor._impl import namespaces
from dream_editor._impl import resolver

COUNTS = collections.Counter()
_VALUES = {}

RESULT_KEYS = {"token", "type_name", "root_type", "items", "incomplete", "root_bound", "stable", "members_cached",
               "elapsed_us", "breaker", "error"}


# ---- Fake unreal descriptor types (registered through resolver._test_register_kind) ---------------------------------

class FakeGetSet:
    """Stands in for a C getset_descriptor: an unreal property. Its getter must never run."""

    def __init__(self, doc):
        self.doc = doc

    def __get__(self, obj, owner=None):
        COUNTS["getset.get"] += 1
        return None

    def __set__(self, obj, value):
        COUNTS["getset.set"] += 1


class methodwithclosure_descriptor:  # noqa: N801 - the engine's type name
    def __init__(self, doc):
        self.doc = doc

    def __get__(self, obj, owner=None):
        COUNTS["method.get"] += 1
        return self

    def __call__(self, *args):
        COUNTS["method.call"] += 1


class classmethodwithclosure_descriptor(methodwithclosure_descriptor):  # noqa: N801
    pass


class builtin_function_or_method_with_closure(methodwithclosure_descriptor):  # noqa: N801
    """A module-level unreal function (a bound closure)."""


class constant_value:  # noqa: N801
    def __init__(self, doc):
        self.doc = doc

    def __get__(self, obj, owner=None):
        COUNTS["constant.get"] += 1
        return 0


class _EnumEntry:
    def __init__(self, doc):
        self.doc = doc

    def __get__(self, obj, owner=None):
        COUNTS["enum.get"] += 1
        return 0


class _EnumType(type):
    pass


def _doc_reader(descriptor):
    return object.__getattribute__(descriptor, "doc")


def _register_fake_kinds():
    resolver._test_register_kind(FakeGetSet, "property-getset", _doc_reader)
    resolver._test_register_kind(methodwithclosure_descriptor, "methodwithclosure", _doc_reader)
    resolver._test_register_kind(classmethodwithclosure_descriptor, "classmethodwithclosure", _doc_reader)
    resolver._test_register_kind(builtin_function_or_method_with_closure, "functionwithclosure", _doc_reader)
    resolver._test_register_kind(constant_value, "constant_value", _doc_reader)
    resolver._test_register_kind(_EnumEntry, "enum_entry", _doc_reader)
    resolver._test_register_kind(_EnumType, "enum_meta")


def _class_doc(tooltip, props):
    lines = [tooltip, "", "**C++ Source:**", "", "- **Module**: Engine", "- **File**: Fake.h", "",
             "**Editor Properties:** (see get_editor_property/set_editor_property)", ""]
    for name, type_text, rw, text in props:
        lines.append("- ``%s`` (%s):  [%s] %s" % (name, type_text, rw, text))     # two spaces, as UE 5.8 writes them
        lines.append("  continued tooltip line")
    return "\r\n".join(lines)


def _fake_unreal():
    m = types.ModuleType("unreal")

    class _ObjectBase:
        def get_editor_property(self, name):
            COUNTS["gep"] += 1
            COUNTS["gep:" + name] += 1
            return _VALUES.get((id(self), name))

    class Object(_ObjectBase):
        __doc__ = _class_doc("Base object.", [])
        get_class = methodwithclosure_descriptor("x.get_class() -> Class\nThe class.")

    class StructBase:
        pass

    class Vector(StructBase):
        __doc__ = _class_doc("A vector.", [("x", "float", "Read-Write", "X.")])
        x = FakeGetSet("(float): [Read-Write] X.")
        y = FakeGetSet("(float): [Read-Write] Y.")
        ZERO = constant_value("(Vector): The zero vector.")
        length = methodwithclosure_descriptor("x.length() -> float\nLength.")

    class StaticMesh(Object):
        __doc__ = _class_doc("A mesh.", [("lod_group", "Name", "Read-Write", "LOD group.")])
        get_bounds = methodwithclosure_descriptor("x.get_bounds() -> BoxSphereBounds\nBounds.")

    class SceneComponent(Object):
        __doc__ = _class_doc("A component.", [("mobility", "ComponentMobility", "Read-Write", "Mobility.")])
        get_world_location = methodwithclosure_descriptor("x.get_world_location() -> Vector\nLocation.")
        relative_location = FakeGetSet("(Vector): [Read-Write] Location.")

    class StaticMeshComponent(SceneComponent):
        __doc__ = _class_doc("A mesh component.", [("static_mesh", "StaticMesh", "Read-Write", "Mesh.")])
        static_mesh = FakeGetSet("(StaticMesh): [Read-Write] Mesh.")
        set_static_mesh = methodwithclosure_descriptor("x.set_static_mesh(new_mesh) -> bool\nSets it.")

    class Actor(Object):
        __doc__ = _class_doc("An actor.", [
            ("root_component", "SceneComponent", "Read-Write", "The root."),
            ("hidden", "bool", "Read-Only", "Hidden."),
        ])
        root_component = FakeGetSet("(SceneComponent): [Read-Write] The root.")
        some_struct = FakeGetSet("(Vector): [Read-Write] A struct.")
        items = FakeGetSet("(Array[Actor]): [Read-Write] A container.")
        old_component = FakeGetSet("(SceneComponent): [Read-Write] deprecated: 'old_component' was renamed to "
                                   "'root_component'.")
        soft_mesh = FakeGetSet("(StaticMesh): [Read-Write] A soft reference.")
        get_actor_location = methodwithclosure_descriptor("x.get_actor_location() -> Vector\nLocation.\r\n\r\n"
                                                          "Returns:\r\n    Vector: the location")

    class StaticMeshActor(Actor):
        __doc__ = _class_doc("A static mesh actor.", [
            ("static_mesh_component", "StaticMeshComponent", "Read-Only", "The component."),
            ("lod_group", "Name", "Read-Write", "Edit only."),
            ("root_component", "SceneComponent", "Read-Write", "The root."),
        ])
        static_mesh_component = FakeGetSet("(StaticMeshComponent): [Read-Only] The component.")

    class EditorActorSubsystem(Object):
        get_selected_level_actors = methodwithclosure_descriptor(
            "x.get_selected_level_actors() -> Array[Actor]\nThe selection.")

    class EditorAssetLibrary(Object):
        save_asset = classmethodwithclosure_descriptor("X.save_asset(asset_to_save, only_if_is_dirty=True) -> bool\nSaves.")
        load_asset = classmethodwithclosure_descriptor("X.load_asset(asset_path) -> Object\nLoads.")

    class EnumBase(metaclass=_EnumType):
        pass

    class CollisionChannel(EnumBase):
        ECC_WORLD_STATIC = _EnumEntry("World static\nmore")
        ECC_PAWN = _EnumEntry("Pawn")
        get_display_name = methodwithclosure_descriptor("x.get_display_name() -> Text\nName.")

    class Array:
        def __init__(self, values):
            _VALUES[(id(self), "values")] = list(values)

        def __getitem__(self, key):
            COUNTS["array.getitem"] += 1
            return _VALUES[(id(self), "values")][key]

        def __iter__(self):
            COUNTS["array.iter"] += 1
            return iter(())

    class Map(Array):
        def __init__(self, values):
            _VALUES[(id(self), "values")] = dict(values)

        def __getitem__(self, key):
            COUNTS["map.getitem"] += 1
            return _VALUES[(id(self), "values")][key]

    class Set(Array):
        pass

    def get_editor_subsystem(cls):
        COUNTS["function.call"] += 1

    for value in (Object, StructBase, Vector, StaticMesh, SceneComponent, StaticMeshComponent, Actor, StaticMeshActor,
                  EditorActorSubsystem, EditorAssetLibrary, EnumBase, CollisionChannel, Array, Map, Set):
        setattr(m, value.__name__, value)
    m.get_editor_subsystem = get_editor_subsystem
    m.get_actor_subsystem = builtin_function_or_method_with_closure(
        "get_actor_subsystem() -> EditorActorSubsystem -- Gets the subsystem")
    m.SOME_INT = 7
    m.sub_module = types.ModuleType("unreal_sub")
    return m


def chain(*steps, prefix=""):
    """chain("actor", ".root_component", "[0]", "()") -> request chain."""
    out = []
    for step in steps:
        if step == "()":
            out.append(["call", None])
        elif step.startswith("."):
            out.append(["attr", step[1:]])
        elif step.startswith("["):
            inner = step[1:-1]
            out.append(["sub", inner[1:-1] if inner.startswith('"') else int(inner)])
        else:
            out.append(["name", step])
    out.append(["prefix", prefix])
    return out


def labels(result):
    return [item[0] for item in result["items"]]


def item(result, label):
    for entry in result["items"]:
        if entry[0] == label:
            return entry
    raise AssertionError("%s not in %s" % (label, labels(result)[:40]))


class ResolverTestCase(RunnerTestCase):
    def setUp(self):
        resolver._test_reset()
        self.addCleanup(resolver._test_reset)
        COUNTS.clear()
        _VALUES.clear()
        self.unreal = _fake_unreal()
        sys.modules["unreal"] = self.unreal
        self.addCleanup(sys.modules.pop, "unreal", None)
        super().setUp()
        _register_fake_kinds()
        self.generation = 1
        self.actor = self.unreal.StaticMeshActor()
        self.session["actor"] = self.actor

    def complete(self, steps, context="member", namespace="session", **extra):
        req = {"chain": steps, "context": context, "namespace": namespace, "generation": self.generation, "epoch": 0}
        req.update(extra)
        result = resolver.complete(req)
        self.assertEqual(set(result), RESULT_KEYS)
        return result

    def members(self, *steps, **extra):
        return self.complete(chain(*steps), **extra)

    def fresh(self):
        self.generation += 1


class Shape(ResolverTestCase):
    def test_result_shape(self):
        result = self.members("actor")
        self.assertEqual(result["error"], "")
        self.assertIsInstance(result["token"], str)
        self.assertTrue(result["token"].startswith("I:"))
        self.assertTrue(result["token"].endswith("StaticMeshActor"))         # f"I:{id:x}:{qualname}"
        self.assertEqual(result["type_name"], "StaticMeshActor")
        self.assertEqual(result["root_type"], "StaticMeshActor")
        self.assertIs(result["root_bound"], True)
        self.assertIs(result["stable"], True)
        self.assertIs(result["incomplete"], False)
        self.assertIs(result["members_cached"], False)
        self.assertIsInstance(result["elapsed_us"], int)
        self.assertEqual(result["breaker"], "")
        for entry in result["items"]:
            self.assertIsInstance(entry, tuple)
            self.assertEqual(len(entry), 5)
            label, kind, detail, insert, sort = entry
            self.assertIsInstance(label, str)
            self.assertIn(kind, {"variable", "function", "method", "class", "module", "property", "field", "constant",
                                 "enum_member", "editor_property"})
            self.assertIsInstance(detail, str)
            self.assertTrue(insert is None or isinstance(insert, str))
            self.assertIsInstance(sort, str)
        self.assertEqual(item(result, "root_component")[1:3], ("property", "SceneComponent"))
        self.assertEqual(item(result, "get_actor_location")[1:3], ("method", "-> Vector"))

    def test_class_and_module_tokens(self):
        cls = self.members("unreal", ".EditorAssetLibrary")
        self.assertTrue(cls["token"].startswith("C:") and cls["token"].endswith("EditorAssetLibrary"))
        self.assertEqual(cls["type_name"], "EditorAssetLibrary")
        self.assertIn("save_asset", labels(cls))
        module = self.members("unreal")
        self.assertTrue(module["token"].startswith("M:") and module["token"].endswith(":unreal"))
        self.assertEqual(module["root_type"], "module unreal")
        self.assertIs(module["root_bound"], False)
        self.assertIn("StaticMeshActor", labels(module))

    def test_want_members_for(self):
        first = self.members("actor")
        again = self.members("actor", want_members_for=[first["token"]])
        self.assertEqual(again["items"], [])
        self.assertIs(again["members_cached"], True)
        self.assertEqual(again["token"], first["token"])
        self.fresh()
        miss = self.members("actor", want_members_for=[first["token"]])
        self.assertIs(miss["members_cached"], True)
        other = self.members("actor", ".root_component", want_members_for=[first["token"]])
        self.assertIs(other["members_cached"], False)
        self.assertTrue(other["items"])

    def test_name_context(self):
        self.session["helper"] = len
        self.session["MyType"] = int
        self.session["data"] = [1]
        self.session["mod"] = types.ModuleType("mod")
        result = self.complete(chain(prefix="he"), context="name")
        self.assertEqual(result["error"], "")
        self.assertIsNone(result["token"])
        self.assertEqual(item(result, "helper")[1], "function")
        self.assertEqual(item(result, "MyType")[1:3], ("class", "class"))
        self.assertEqual(item(result, "data")[1:3], ("variable", "list"))
        self.assertEqual(item(result, "mod")[1], "module")
        self.assertEqual(item(result, "unreal")[1], "module")
        self.assertNotIn("print", labels(result))      # builtins come from C++'s keyword table

    def test_never_raises(self):
        garbage = [None, {}, {"chain": "x"}, {"chain": []}, {"chain": [["prefix", ""]], "context": "member"},
                   {"chain": [["name", 5], ["prefix", ""]]}, {"chain": [["name", "actor"]]},
                   {"chain": chain("actor"), "namespace": 5}, {"chain": chain("actor"), "context": 3},
                   {"chain": chain("actor"), "context": "hover"}, {"chain": [["name", "a"], ["sub", 1.5], ["prefix", ""]]},
                   {"chain": [["name", "actor"], ["call", []], ["prefix", ""]]},
                   {"chain": chain("actor"), "want_members_for": 3, "generation": "x", "budget_ns": "y"},
                   {"chain": chain(prefix="a"), "context": "name", "namespace": "frame:nope"},
                   {"chain": [["name", "actor"], ["prefix", ""]], "context": "string_arg", "func": 7, "index": "0"}]
        for req in garbage:
            with self.subTest(req=req):
                result = resolver.complete(req)
                self.assertEqual(set(result), RESULT_KEYS)
                self.assertIsInstance(result["items"], list)
        self.assertTrue(resolver.complete(None)["error"].startswith("bad request"))
        self.assertTrue(resolver.complete({"chain": chain("actor"), "namespace": 5})["error"].startswith("bad request"))
        with mock.patch.object(namespaces, "session", lambda: ["not", "a", "dict"]):
            result = self.members("actor")
            self.assertEqual((result["error"], result["items"]), ("", []))
            self.assertEqual(resolver.session_names("session"), ())
        self.assertEqual(resolver.session_names(None), ())
        for call in (lambda: resolver.prewarm(None), lambda: resolver.prewarm({"namespace": 3}),
                     lambda: resolver.unreal_names("x")):
            result = call()
            self.assertTrue(result["error"])
        self.assertEqual(set(resolver.prewarm({"namespace": "none"})), {"roots", "members", "cursor", "count",
                                                                         "incomplete", "error"})
        self.assertEqual(set(resolver.unreal_names({})), {"items", "cursor", "count", "error"})


class SafetyPolicy(ResolverTestCase):
    def test_res_001_property_getattr_getattribute_descriptor(self):
        calls = collections.Counter()

        class SideEffect:
            def __get__(self, obj, owner=None):
                calls["descriptor"] += 1
                return 1

        class Trap:
            custom = SideEffect()

            @property
            def prop(self):
                calls["property"] += 1
                return 1

            def __getattr__(self, name):
                calls["getattr"] += 1
                return 1

            def __getattribute__(self, name):
                calls["getattribute"] += 1
                return object.__getattribute__(self, name)

            def method(self):
                return 1

        trap = Trap()
        object.__setattr__(trap, "x", 1)
        calls.clear()
        self.session["trap"] = trap
        result = self.members("trap")
        for name in ("prop", "custom", "x", "method", "__getattr__"):
            self.assertIn(name, labels(result))
        self.assertTrue(result["token"].endswith("@%x" % id(trap)))      # instance attributes make it per-value
        self.assertIs(result["stable"], False)
        for steps in (("trap", ".prop"), ("trap", ".custom"), ("trap", ".missing"), ("trap", ".x")):
            self.members(*steps)
        self.assertEqual(self.members("trap", ".x")["type_name"], "int")
        self.assertEqual(sum(calls.values()), 0, calls)

    def test_res_002_unreal_getset_never_invoked(self):
        result = self.members("actor", ".root_component")
        self.assertEqual(result["type_name"], "SceneComponent")
        self.assertIn("get_world_location", labels(result))
        self.assertIn("relative_location", labels(result))
        self.members("actor", ".root_component", ".relative_location")
        self.assertEqual(COUNTS["getset.get"], 0)
        self.assertEqual(COUNTS["gep"], 0)

    def test_res_003_only_hard_object_references_are_read(self):
        live = self.unreal.StaticMeshComponent()
        for name in ("root_component", "some_struct", "items", "old_component"):
            _VALUES[(id(self.actor), name)] = live
        self.dream.hard_object_props = lambda type_name: frozenset({"rootcomponent", "somestruct", "items",
                                                                   "oldcomponent"})
        result = self.members("actor", ".root_component")
        self.assertEqual(COUNTS["gep:root_component"], 1)
        self.assertEqual(result["type_name"], "StaticMeshComponent")        # the live value's concrete type
        self.assertIn("set_static_mesh", labels(result))
        self.assertEqual(self.members("actor", ".some_struct")["type_name"], "Vector")
        self.assertEqual(self.members("actor", ".items")["type_name"], "Array")
        self.assertEqual(self.members("actor", ".old_component")["type_name"], "SceneComponent")
        self.assertEqual(COUNTS["gep"], 1)
        self.assertEqual(COUNTS["getset.get"], 0)

    def test_res_004_soft_property_never_read(self):
        _VALUES[(id(self.actor), "soft_mesh")] = self.unreal.StaticMesh()
        self.dream.hard_object_props = lambda type_name: frozenset({"rootcomponent"})
        result = self.members("actor", ".soft_mesh")
        self.assertEqual(COUNTS["gep"], 0)
        self.assertEqual(result["type_name"], "StaticMesh")
        self.assertIn("get_bounds", labels(result))

    def test_res_005_literal_subscripts(self):
        actor = self.actor
        calls = collections.Counter()

        class DictSub(dict):
            def __getitem__(self, key):
                calls["dictsub"] += 1
                return actor

            def get(self, key, default=None):
                calls["dictsub.get"] += 1
                return actor

        self.session.update(lst=[actor], tup=(actor,), dct={"a": actor, 1: actor}, sub=DictSub(a=actor),
                            arr=self.unreal.Array([actor]), mp=self.unreal.Map({"a": actor}))
        for steps in (("lst", "[0]"), ("lst", "[-1]"), ("tup", "[0]"), ("dct", '["a"]'), ("dct", "[1]")):
            with self.subTest(steps=steps):
                self.assertEqual(self.members(*steps)["type_name"], "StaticMeshActor")
        self.assertEqual(self.members("lst", "[5]")["items"], [])
        self.assertEqual(self.members("lst", '["a"]')["items"], [])
        self.assertEqual(self.members("sub", '["a"]')["items"], [])
        bad = self.complete([["name", "lst"], ["sub", 0.5], ["prefix", ""]])
        self.assertTrue(bad["error"].startswith("bad request"))
        self.assertEqual(sum(calls.values()), 0)

        # No container_element_kind in M1: never read; a value from the Session has no declared type to fall back to.
        self.assertEqual(self.members("arr", "[0]")["items"], [])
        for kind in ("SoftObjectProperty", "SoftClassProperty", "LazyObjectProperty", None):
            with self.subTest(kind=kind):
                self.fresh()
                self.dream.container_element_kind = lambda value, kind=kind: kind
                self.assertEqual(self.members("arr", "[0]")["items"], [])
                self.assertEqual(self.members("mp", '["a"]')["items"], [])
        self.assertEqual(COUNTS["array.getitem"] + COUNTS["map.getitem"] + COUNTS["array.iter"], 0)

        def raising(value):
            raise RuntimeError("no")
        self.dream.container_element_kind = raising
        self.fresh()
        self.assertEqual(self.members("arr", "[0]")["items"], [])
        self.assertEqual(COUNTS["array.getitem"], 0)

        # A hard element kind is subscripted once, on the exact type only.
        self.dream.container_element_kind = lambda value: "ObjectProperty"
        self.fresh()
        self.assertEqual(self.members("arr", "[0]")["type_name"], "StaticMeshActor")
        self.assertEqual(self.members("mp", '["a"]')["type_name"], "StaticMeshActor")
        self.assertEqual((COUNTS["array.getitem"], COUNTS["map.getitem"], COUNTS["array.iter"]), (1, 1, 0))

        # At type level the declared element type is used and nothing is subscripted.
        self.dream.container_element_kind = lambda value: "SoftObjectProperty"
        result = self.members("actor", ".items", "[0]")
        self.assertEqual(result["type_name"], "Actor")
        self.assertEqual((COUNTS["array.getitem"], COUNTS["getset.get"]), (1, 0))

    def test_res_006_no_inspect_signature(self):
        with mock.patch.object(inspect, "signature", side_effect=AssertionError("inspect.signature")) as signature:
            self.session["func"] = lambda: 1
            for steps in (("actor",), ("actor", ".get_actor_location", "()"), ("func", "()"),
                          ("unreal", ".EditorAssetLibrary", ".load_asset", "()")):
                self.members(*steps)
            self.complete(chain("actor", prefix="r"), context="string_arg", func="get_editor_property", index=0)
            self.complete(chain(prefix=""), context="name")
            self.assertEqual(signature.call_count, 0)

    def test_res_007_deadline_with_100000_names_and_deep_chains(self):
        self.session.update({"n%d" % i: i for i in range(100_000)})
        best = None
        for attempt in range(3):
            self.fresh()
            start = time.perf_counter_ns()
            result = self.complete(chain(prefix="n"), context="name")
            elapsed = time.perf_counter_ns() - start
            self.assertIs(result["incomplete"], True)
            best = elapsed if best is None else min(best, elapsed)
        self.assertLessEqual(best, 5_500_000)

        bases = [object]
        for i in range(60):
            bases = [type("Deep%d" % i, (bases[0],), {})]
        node = bases[0]()
        object.__setattr__(node, "next", node)
        self.session["node"] = node
        deep = [["name", "node"]] + [["attr", "next"]] * 3000 + [["prefix", ""]]
        best = None
        for attempt in range(3):
            self.fresh()
            start = time.perf_counter_ns()
            result = self.complete(deep)
            elapsed = time.perf_counter_ns() - start
            self.assertIs(result["incomplete"], True)
            best = elapsed if best is None else min(best, elapsed)
        self.assertLessEqual(best, 5_500_000)
        result = self.complete([["name", "node"]] + [["attr", "next"]] * 20_000 + [["prefix", ""]])
        self.assertEqual((result["incomplete"], result["items"], result["error"]), (True, [], ""))

    def test_res_018_descriptor_with_a_property_doc(self):
        calls = collections.Counter()

        class Weird:
            @property
            def __doc__(self):
                calls["doc"] += 1
                return "(int): [Read-Write] trap"

            def __get__(self, obj, owner=None):
                calls["get"] += 1
                return 1

        class Holder:
            w = Weird()

        self.session["holder"] = Holder()
        result = self.members("holder")
        self.assertEqual(item(result, "w")[1:4], ("variable", "", None))
        self.assertEqual(self.members("holder", ".w")["items"], [])
        self.assertEqual(sum(calls.values()), 0)

    def test_res_019_counting_class_property(self):
        calls = collections.Counter()

        class Sneaky:
            @property
            def __class__(self):
                calls["class"] += 1
                return int

        self.session["sneaky"] = Sneaky()
        result = self.members("sneaky")
        self.assertEqual(result["type_name"], "Sneaky")
        self.assertEqual(item(result, "__class__")[1], "property")
        self.complete(chain(prefix=""), context="name")
        resolver.prewarm({"namespace": "session"})
        self.assertEqual(calls["class"], 0)

    def test_module_getattr_never_called(self):
        calls = collections.Counter()
        module = types.ModuleType("lazy")

        def module_getattr(name):
            calls["getattr"] += 1
            return 1
        module.__getattr__ = module_getattr
        module.real = 3
        self.session["lazy"] = module
        result = self.members("lazy")
        self.assertIn("real", labels(result))
        self.assertEqual(self.members("lazy", ".missing")["items"], [])
        self.assertEqual(self.members("lazy", ".real")["type_name"], "int")
        self.assertEqual(calls["getattr"], 0)

    def test_res_025_class_unknown_to_hard_object_props(self):
        _VALUES[(id(self.actor), "root_component")] = self.unreal.StaticMeshComponent()
        self.dream.hard_object_props = lambda type_name: frozenset()
        result = self.members("actor", ".root_component")
        self.assertEqual(COUNTS["gep"], 0)
        self.assertEqual(result["type_name"], "SceneComponent")
        self.dream.hard_object_props = lambda type_name: ["rootcomponent"]          # not a set: "do not read"
        self.fresh()
        self.members("actor", ".root_component")
        self.assertEqual(COUNTS["gep"], 0)


class Chains(ResolverTestCase):
    def test_selected_actors_chain_at_type_level(self):
        steps = ("unreal", ".EditorActorSubsystem", "()", ".get_selected_level_actors", "()", "[0]")
        result = self.members(*steps)
        self.assertEqual(result["error"], "")
        self.assertEqual(result["type_name"], "Actor")
        self.assertTrue(result["token"].startswith("I:"))
        self.assertIn("root_component", labels(result))
        self.assertEqual(COUNTS["method.call"] + COUNTS["method.get"] + COUNTS["array.getitem"], 0)

    def test_call_steps(self):
        self.session["make"] = self.unreal.StaticMeshActor

        def typed() -> list:
            return []
        self.session["typed"] = typed
        self.assertEqual(self.members("make", "()")["type_name"], "StaticMeshActor")
        self.assertEqual(self.members("typed", "()")["type_name"], "list")
        self.assertEqual(self.members("actor", ".get_actor_location", "()")["type_name"], "Vector")
        self.assertEqual(self.members("unreal", ".EditorAssetLibrary", ".load_asset", "()")["type_name"], "Object")
        self.assertEqual(self.members("unreal", ".Vector", ".ZERO")["type_name"], "Vector")
        self.assertEqual(self.members("unreal", ".CollisionChannel", ".ECC_PAWN")["type_name"], "CollisionChannel")
        self.assertEqual(self.members("unreal", ".get_editor_subsystem", "()")["items"], [])
        self.assertEqual(self.members("unreal", ".get_actor_subsystem", "()")["type_name"], "EditorActorSubsystem")
        self.assertEqual(COUNTS["method.call"] + COUNTS["function.call"] + COUNTS["constant.get"]
                         + COUNTS["enum.get"], 0)

    def test_int02_edit_only_editor_property_inserts_get_editor_property(self):
        result = self.members("actor")
        self.assertEqual(item(result, "lod_group"),
                         ("lod_group", "editor_property", "Name [Read-Write]", 'get_editor_property("lod_group")', ""))
        self.assertEqual(item(result, "static_mesh_component")[1], "property")
        self.assertEqual(item(result, "hidden")[1], "editor_property")       # inherited from Actor's doc
        self.assertEqual(labels(result).count("root_component"), 1)

    def test_res_013_editor_property_names(self):
        result = self.complete(chain("actor", prefix="st"), context="string_arg", func="get_editor_property", index=0)
        self.assertEqual(result["error"], "")
        self.assertEqual(result["type_name"], "StaticMeshActor")
        for name, detail in (("static_mesh_component", "StaticMeshComponent [Read-Only]"),
                             ("lod_group", "Name [Read-Write]"), ("root_component", "SceneComponent [Read-Write]"),
                             ("hidden", "bool [Read-Only]")):
            self.assertEqual(item(result, name)[1:3], ("editor_property", detail))
        self.assertEqual(labels(result).count("root_component"), 1)
        setter = self.complete(chain("actor", prefix=""), context="string_arg", func="set_editor_property", index=0)
        self.assertEqual(labels(setter), labels(result))
        on_class = self.complete(chain("unreal", ".StaticMeshComponent", prefix=""), context="string_arg",
                                 func="get_editor_property", index=0)
        self.assertEqual(set(labels(on_class)), {"static_mesh", "mobility"})
        self.assertEqual(self.complete(chain("actor"), context="string_arg", func="get_editor_property",
                                       index=1)["items"], [])
        self.assertEqual(self.complete(chain("actor"), context="string_arg", func="load_asset", index=0)["items"], [])
        self.assertEqual(COUNTS["getset.get"] + COUNTS["gep"], 0)

    def test_res_014_enum_members_first(self):
        result = self.members("unreal", ".CollisionChannel")
        self.assertEqual(item(result, "ECC_WORLD_STATIC"), ("ECC_WORLD_STATIC", "enum_member", "World static", None, "0"))
        self.assertEqual(item(result, "ECC_PAWN")[4], "0")
        self.assertEqual(item(result, "get_display_name")[4], "1")
        for entry in result["items"]:
            self.assertEqual(entry[4], "0" if entry[1] == "enum_member" else "1")
        self.assertEqual(COUNTS["enum.get"], 0)
        module = self.members("unreal")
        self.assertEqual(item(module, "CollisionChannel")[2], "enum")
        self.assertEqual(item(module, "Vector")[2], "struct")
        self.assertEqual(item(module, "Actor")[2], "class")


class Namespaces(ResolverTestCase):
    def test_res_010_namespace_by_buffer_mode(self):
        self.session["sess_only"] = 1
        namespaces.set_last_run({"iso_only": 2})
        self.addCleanup(namespaces.release_last_run)
        session = labels(self.complete(chain(prefix=""), context="name", namespace="session"))
        isolated = labels(self.complete(chain(prefix=""), context="name", namespace="isolated:00ab"))
        none = labels(self.complete(chain(prefix=""), context="name", namespace="none"))
        self.assertIn("sess_only", session)
        self.assertNotIn("iso_only", session)
        self.assertIn("iso_only", isolated)
        self.assertNotIn("sess_only", isolated)
        self.assertNotIn("sess_only", none)
        for listing in (session, isolated, none):
            self.assertIn("unreal", listing)
        self.assertEqual(self.members("sess_only", namespace="isolated:00ab")["items"], [])
        self.assertEqual(self.members("iso_only", namespace="isolated:00ab")["type_name"], "int")
        self.assertIs(self.members("iso_only", namespace="isolated:00ab")["root_bound"], True)
        for namespace in ("session", "isolated:00ab", "none"):
            result = self.members("unreal", ".Actor", namespace=namespace)
            self.assertIn("root_component", labels(result))
        namespaces.release_last_run()
        self.fresh()
        self.assertNotIn("iso_only", labels(self.complete(chain(prefix=""), context="name", namespace="isolated:00ab")))
        self.assertEqual(resolver.session_names("none"), ())

    def test_res_012_paused_frame_roots(self):
        resolver.set_frame(7, {"x": [1], "only_local": 1}, {"x": {}, "g": 1.5})
        result = self.members("x", namespace="frame:7")
        self.assertEqual(result["type_name"], "list")
        self.assertEqual(self.members("g", namespace="frame:7")["type_name"], "float")
        names = labels(self.complete(chain(prefix=""), context="name", namespace="frame:7"))
        self.assertEqual(names.count("x"), 1)
        self.assertLess(names.index("only_local"), names.index("g"))
        self.assertEqual(resolver.session_names("frame:7"), ("x", "only_local", "g"))
        resolver.clear_frame(7)
        self.fresh()
        self.assertEqual(self.members("x", namespace="frame:7")["items"], [])

    def test_res_020_session_names_with_10000_names(self):
        self.session.update({"v%d" % i: i for i in range(10_000)})
        best = None
        for attempt in range(3):
            start = time.perf_counter_ns()
            names = resolver.session_names("session")
            elapsed = time.perf_counter_ns() - start
            best = elapsed if best is None else min(best, elapsed)
        self.assertIn("v9999", names)
        self.assertIn("actor", names)
        self.assertNotIn("print", names)
        self.assertLessEqual(best, 2_000_000)


class Caches(ResolverTestCase):
    def test_res_009_cache_hit_without_namespace_access(self):
        calls = collections.Counter()
        real = namespaces.session

        def counting():
            calls["session"] += 1
            return real()

        with mock.patch.object(namespaces, "session", counting):
            first = self.members("actor")
            self.assertEqual(calls["session"], 1)
            second = self.members("actor")
            self.assertEqual(calls["session"], 1)
            self.assertEqual(second["items"], first["items"])
            self.assertEqual(second["token"], first["token"])
            self.fresh()
            self.members("actor")
            self.assertEqual(calls["session"], 2)

    def test_res_022_circuit_breaker(self):
        ticks = iter(range(0, 10**12, 30_000_000))
        with mock.patch.object(resolver, "_clock", lambda: next(ticks)):
            slow = self.members("actor")
        self.assertEqual(slow["breaker"], "StaticMeshActor")
        self.fresh()
        broken = self.members("actor")
        self.assertEqual((broken["items"], broken["error"], broken["breaker"]), ([], "breaker", ""))
        self.assertEqual(self.complete(chain("actor"), context="string_arg", func="get_editor_property",
                                       index=0)["error"], "breaker")
        self.assertTrue(self.members("unreal", ".Actor")["items"])          # other root types are unaffected

    def test_res_022_breaker_through_a_module_holds_for_the_member_it_reaches(self):
        ticks = iter(range(0, 10**12, 30_000_000))
        with mock.patch.object(resolver, "_clock", lambda: next(ticks)):
            slow = self.members("unreal", ".EditorAssetLibrary")
        self.assertEqual(slow["breaker"], "module unreal.EditorAssetLibrary")
        self.fresh()
        self.assertEqual(self.members("unreal", ".EditorAssetLibrary")["error"], "breaker")
        self.assertTrue(self.members("unreal", ".Actor")["items"])          # the rest of the module still resolves
        self.assertEqual(self.members("unreal")["error"], "")

    def test_res_027_alias_rewrite_and_expired_values(self):
        now = [1000.0]
        self.addCleanup(setattr, resolver, "_wall", resolver._wall)
        resolver._wall = lambda: now[0]
        _VALUES[(id(self.actor), "root_component")] = self.unreal.StaticMeshComponent()
        self.dream.hard_object_props = lambda type_name: frozenset({"rootcomponent"})
        steps = chain("actor", ".root_component")
        self.complete(steps, epoch=0)
        self.complete(steps, epoch=0)
        self.assertEqual(COUNTS["gep"], 1)
        listed = collections.Counter()
        real_list = resolver._list_members

        def counting_list(*args):
            listed["calls"] += 1
            return real_list(*args)

        with mock.patch.object(resolver, "_list_members", counting_list), \
                mock.patch.object(namespaces, "session", wraps=namespaces.session) as session:
            result = self.complete(steps, epoch=1)                 # alias rewrite: roots re-resolved
            self.assertEqual(session.call_count, 1)
            self.assertEqual(listed["calls"], 0)                    # member lists kept
            self.assertEqual(result["type_name"], "StaticMeshComponent")
            self.assertEqual(COUNTS["gep"], 1)                      # still fresh
            now[0] += 11.0
            self.complete(steps, epoch=1)
            self.assertEqual(COUNTS["gep"], 2)                      # a 10 s-old value is re-read


class Prewarm(ResolverTestCase):
    def test_prewarm_pages_roots_and_members(self):
        for i in range(20):
            self.session["obj%d" % i] = self.unreal.StaticMeshActor()
        self.session["num"] = 3
        cursor, roots, members, calls = 0, [], [], 0
        while True:
            result = resolver.prewarm({"namespace": "session", "cursor": cursor, "budget_ns": 1,
                                       "generation": self.generation})
            calls += 1
            self.assertEqual(result["error"], "")
            roots.extend(result["roots"])
            members.extend(result["members"])
            cursor = result["cursor"]
            if cursor == -1:
                self.assertIs(result["incomplete"], False)
                break
            self.assertIs(result["incomplete"], True)
        self.assertEqual(calls, len(self.session))
        self.assertEqual(len(roots), len(self.session))
        by_name = {name: (kind, token, type_name) for name, kind, token, type_name in roots}
        self.assertEqual(by_name["obj3"][1], self.members("obj3")["token"])
        self.assertEqual(by_name["obj3"][0], "variable")
        self.assertEqual(by_name["num"][2], "int")
        tokens = [entry[0] for entry in members]
        self.assertEqual(len(tokens), len(set(tokens)))             # each type's member list once per pass
        actor_entry = [entry for entry in members if entry[1] == "StaticMeshActor"][0]
        self.assertIs(actor_entry[2], True)
        self.assertIn("root_component", [entry[0] for entry in actor_entry[3]])

        skip = resolver.prewarm({"namespace": "session", "generation": self.generation,
                                 "known_tokens": [actor_entry[0]], "budget_ns": 10**9})
        self.assertEqual(skip["cursor"], -1)
        self.assertNotIn(actor_entry[0], [entry[0] for entry in skip["members"]])
        self.assertEqual(len(skip["roots"]), len(self.session))

    def test_prewarm_over_10000_names_exports_member_lists_only(self):
        self.session.update({"w%d" % i: i for i in range(10_001)})
        result = resolver.prewarm({"namespace": "session", "budget_ns": 10**10, "generation": self.generation})
        self.assertEqual(result["cursor"], -1)
        self.assertEqual(result["roots"], [])
        self.assertGreater(result["count"], 10_000)
        self.assertIn("int", [entry[1] for entry in result["members"]])

    def test_unreal_names_pages(self):
        cursor, items, count = 0, [], 0
        while True:
            result = resolver.unreal_names({"cursor": cursor, "budget_ns": 1})
            self.assertEqual(result["error"], "")
            items.extend(result["items"])
            count = result["count"]
            cursor = result["cursor"]
            if cursor == -1:
                break
        names = [entry[0] for entry in items]
        self.assertEqual(len(names), count)
        self.assertEqual(len(names), len(set(names)))
        by_name = {entry[0]: entry for entry in items}
        self.assertEqual(by_name["Actor"][1:3], ("class", "class"))
        self.assertEqual(by_name["CollisionChannel"][2], "enum")
        self.assertEqual(by_name["Vector"][2], "struct")
        self.assertEqual(by_name["get_editor_subsystem"][1], "function")
        self.assertEqual(by_name["get_actor_subsystem"][1:3], ("function", "-> EditorActorSubsystem"))
        self.assertEqual(by_name["SOME_INT"][1], "constant")
        self.assertEqual(by_name["sub_module"][1], "module")
        with mock.patch.dict(sys.modules):
            sys.modules.pop("unreal")
            self.assertEqual(resolver.unreal_names({}), {"items": [], "cursor": -1, "count": 0, "error": ""})


class Bounds(ResolverTestCase):
    """Review round 1: every walk keeps to the deadline, _GEP is captured once, the value cache is bounded."""

    def test_instance_dict_walk_keeps_the_deadline(self):
        class Bag:
            pass

        bag = Bag()
        for index in range(50_000):
            setattr(bag, "attr_%d" % index, index)
        self.session["bag"] = bag
        ticks = iter(range(0, 10**15, 100_000))     # 100 us per clock read: the 5 ms budget ends inside the dict walk
        with mock.patch.object(resolver, "_clock", lambda: next(ticks)):
            result = self.members("bag")
        self.assertTrue(result["incomplete"])
        self.assertLess(len(result["items"]), 50_000)
        self.assertEqual(result["breaker"], "")

    def test_discovery_is_resumed_across_calls_and_nothing_is_cached_meanwhile(self):
        for index in range(3_000):                  # a long walk: the discovery scans module values and classes
            setattr(self.unreal, "Filler%d" % index, type("Filler%d" % index, (), {"m%d" % k: k for k in range(5)}))
        resolver._disc["module"] = None            # start the discovery again over the bigger module
        ticks = iter(range(0, 10**15, 300_000))     # 300 us per clock read: the budget ends inside the walk
        with mock.patch.object(resolver, "_clock", lambda: next(ticks)):
            first = self.members("actor")
        self.assertTrue(first["incomplete"])
        self.assertEqual(first["breaker"], "")     # at most one chunk past the deadline: far from the 20 ms breaker
        self.assertFalse(resolver._kinds_ready())
        self.assertEqual(len(resolver._result_cache), 0)
        self.assertEqual(len(resolver._member_cache), 0)
        for _ in range(100):                        # each later call walks on from where the last one stopped
            if resolver._kinds_ready():
                break
            self.members("actor")
        self.assertTrue(resolver._kinds_ready())
        done = self.members("actor")
        self.assertFalse(done["incomplete"])
        self.assertIn("get_actor_location", labels(done))

    def test_gep_is_captured_once_per_unreal_module(self):
        live = self.unreal.StaticMeshComponent()
        _VALUES[(id(self.actor), "root_component")] = live
        self.dream.hard_object_props = lambda type_name: frozenset({"rootcomponent"})
        self.assertEqual(self.members("actor", ".root_component")["type_name"], "StaticMeshComponent")
        self.assertEqual(COUNTS["gep:root_component"], 1)
        calls = collections.Counter()

        def rebound(obj, name):
            calls["rebound"] += 1
            return None

        self.unreal.Object.get_editor_property = rebound    # user code rebinding the method after the capture
        self.fresh()                                        # new generation: the cached value is gone
        self.assertEqual(self.members("actor", ".root_component")["type_name"], "StaticMeshComponent")
        self.assertEqual(calls["rebound"], 0)
        self.assertEqual(COUNTS["gep:root_component"], 2)

    def test_gep_value_cache_is_bounded(self):
        gep = resolver._gep_fn()
        self.assertIsNotNone(gep)
        ctx = resolver._Ctx(resolver._clock() + 10**9, self.generation)
        owners = [self.unreal.StaticMeshComponent() for _ in range(resolver._GEP_CACHE_MAX + 50)]
        for owner in owners:
            resolver._gep_read(owner, "static_mesh", ctx)
        self.assertLessEqual(len(resolver._gep_cache), resolver._GEP_CACHE_MAX)
        now = [resolver._wall() + resolver._GEP_TTL_S + 1.0]
        with mock.patch.object(resolver, "_wall", lambda: now[0]):
            resolver._gep_read(owners[0], "static_mesh", ctx)
        self.assertEqual(len(resolver._gep_cache), 1)      # expired values are dropped, not kept until the next run

    def test_gep_values_expire_without_a_new_read(self):
        ctx = resolver._Ctx(resolver._clock() + 10**9, self.generation)
        for _ in range(3):
            resolver._gep_read(self.unreal.StaticMeshComponent(), "static_mesh", ctx)
        self.assertEqual(len(resolver._gep_cache), 3)
        later = resolver._wall() + resolver._GEP_TTL_S + 1.0
        with mock.patch.object(resolver, "_wall", lambda: later):
            resolver.prewarm({"cursor": 0, "generation": self.generation})       # the idle slice after a run
        self.assertEqual(len(resolver._gep_cache), 0)
        for _ in range(2):
            resolver._gep_read(self.unreal.StaticMeshComponent(), "static_mesh", ctx)
        with mock.patch.object(resolver, "_wall", lambda: later + resolver._GEP_TTL_S):
            self.members("actor")                                                # any completion call
        self.assertEqual(len(resolver._gep_cache), 0)


if __name__ == "__main__":
    unittest.main()
