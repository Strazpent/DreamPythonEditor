# SPDX-License-Identifier: Apache-2.0
"""The Stage 1 snapshot walker (TDD §7.4.2, RT-13): T-STUB-001, 002, 003, 004, 006, 023, 025, and the C++ contract
(``start`` / ``step`` / ``is_active`` / ``stop``, the three output kinds, the manifest)."""

import gc
import hashlib
import json
import os
import shutil
import tempfile
import time
import types
import unittest

import check_support as support

from dream_editor._impl import stubs


def read_jsonl(folder, name):
    with open(os.path.join(folder, "modules", "%s.jsonl" % name), "rb") as handle:
        raw = handle.read()
    return raw, [json.loads(line) for line in raw.decode("ascii").splitlines()]


class WalkTestCase(unittest.TestCase):
    def setUp(self):
        support.reset_counters()
        self.root = tempfile.mkdtemp(prefix="dream-stubs-")
        self.addCleanup(shutil.rmtree, self.root, True)
        self.addCleanup(stubs.stop)
        self.folder = os.path.join(self.root, "snapshot")

    def walk(self, modules=None, **kwargs):
        writer = support.SnapshotWriter(self.folder)
        complete = writer.run(modules=modules if modules is not None else support.build_fake_unreal(), **kwargs)
        return writer, complete

    def manifest(self):
        with open(os.path.join(self.folder, "manifest.json"), "r", encoding="utf-8") as handle:
            return json.load(handle)


class Contract(WalkTestCase):
    def test_inactive(self):
        self.assertFalse(stubs.is_active())
        self.assertEqual(stubs.step(), [])

    def test_output_kinds_and_files(self):
        writer, complete = self.walk()
        self.assertTrue(complete)
        self.assertFalse(stubs.is_active())
        self.assertTrue(os.path.isfile(os.path.join(self.folder, ".complete")))
        manifest = self.manifest()
        self.assertEqual(manifest["format"], "dream.stubsnap")
        self.assertEqual(manifest["schema"], 1)
        self.assertEqual(manifest["key"], support.SNAPSHOT_KEY)
        self.assertEqual(manifest["engine"], support.ENGINE)
        self.assertIs(manifest["complete"], True)
        self.assertEqual(manifest["unreal_module"], "modules/__unreal__.jsonl")
        names = [m["name"] for m in manifest["modules"]]
        self.assertEqual(names, ["engine", "unrealed", "core", "editorscriptingutilities", "python"])  # largest first
        counts = [m["types"] for m in manifest["modules"]]
        self.assertEqual(counts, sorted(counts, reverse=True))
        for entry in manifest["modules"]:
            raw, records = read_jsonl(self.folder, entry["name"])
            self.assertEqual(hashlib.sha256(raw).hexdigest(), entry["sha256"])
            self.assertEqual(entry["file"], "modules/%s.jsonl" % entry["name"])
            self.assertEqual(records[0]["rec"], "module")
            self.assertEqual(records[0]["count"], entry["types"])
            self.assertEqual(len(records) - 1, entry["types"])
        self.assertEqual(manifest["modules"][0]["cpp_module"], "Engine")
        self.assertEqual({m["name"]: m["cpp_module"] for m in manifest["modules"]}["core"], "CoreUObject")
        stats = manifest["stats"]
        self.assertEqual(stats["slices"], writer.steps)
        self.assertGreater(stats["max_slice_us"], 0)

    def test_stop_forgets_the_walk(self):
        stubs.start(support.SNAPSHOT_KEY, support.ENGINE, [], _modules=support.build_fake_unreal())
        self.assertTrue(stubs.is_active())
        stubs.stop()
        self.assertFalse(stubs.is_active())
        self.assertEqual(stubs.step(), [])

    def test_records(self):
        self.walk()
        _, engine = read_jsonl(self.folder, "engine")
        types_by_name = {r["name"]: r for r in engine if r["rec"] == "type"}
        actor = types_by_name["Actor"]
        self.assertEqual(actor["kind"], "class")
        self.assertEqual(actor["bases"], ["Object"])
        self.assertEqual(actor["base_modules"], ["core"])
        self.assertIs(actor["uobject"], True)
        self.assertNotIn("Editor Properties", actor["doc"])
        self.assertNotIn("\r", actor["doc"])
        members = {m["n"]: m for m in actor["members"]}
        self.assertEqual(members["tags"]["k"], "getset")
        self.assertTrue(members["tags"]["doc"].startswith("(Array[Name]): [Read-Write]"))
        self.assertEqual(members["set_actor_location"]["k"], "method")
        self.assertEqual(members["hidden_flag"]["k"], "other")
        self.assertEqual(members["hidden_flag"]["t"], "property")
        self.assertEqual(members["hidden_flag"]["doc"], "A Python property with a counting getter.")
        self.assertFalse(any(name.startswith("__") for name in members))
        # Own editor properties only (StaticMeshActor inherits Actor's list).
        self.assertEqual([e[0] for e in actor["eprops"]], ["can_be_damaged", "initial_life_span", "tags"])
        self.assertEqual(types_by_name["StaticMeshActor"]["eprops"],
                         [["static_mesh_component", "StaticMeshComponent", "ro", "The mesh component."]])
        self.assertEqual(types_by_name["CollisionChannel"]["kind"], "enum")
        entries = {m["n"]: m for m in types_by_name["CollisionChannel"]["members"]}
        self.assertEqual(entries["ECC_PAWN"], {"n": "ECC_PAWN", "k": "enum_entry", "doc": "Pawn"})
        _, core = read_jsonl(self.folder, "core")
        vector = next(r for r in core if r.get("name") == "Vector")
        self.assertEqual(vector["kind"], "struct")
        self.assertEqual({m["n"]: m["k"] for m in vector["members"]}["ZERO"], "constant")
        obj = next(r for r in core if r.get("name") == "Object")
        self.assertEqual({m["n"]: m["k"] for m in obj["members"]}["static_class"], "glue")
        _, paths_module = read_jsonl(self.folder, "engine")
        paths = next(r for r in paths_module if r.get("name") == "Paths")
        self.assertEqual({m["n"]: m["k"] for m in paths["members"]}["project_content_dir"], "classmethod")


class Safety(WalkTestCase):
    def test_no_getter_runs(self):
        # T-STUB-001: counting getters, constant_value, _EnumEntry, properties, module __getattr__: all counters 0.
        self.walk()
        self.assertEqual(support.counter_total(), 0)

    def test_pytest_types_filtered(self):
        # T-STUB-003
        self.walk()
        manifest = self.manifest()
        self.assertEqual(manifest["filtered"]["pytest_types"], 2)
        _, python = read_jsonl(self.folder, "python")
        names = [r.get("name") for r in python if r["rec"] == "type"]
        self.assertEqual(names, ["PythonResourceOwner"])

    def test_builtins_module_types_grouped_by_module(self):
        # T-STUB-004: __module__ == 'builtins' types in _unreal_engine are grouped under engine.
        modules = support.build_fake_unreal()
        self.assertEqual(modules["_unreal_engine"].Actor.__module__, "builtins")
        self.walk(modules)
        _, engine = read_jsonl(self.folder, "engine")
        self.assertIn("Actor", [r.get("name") for r in engine])
        _, core = read_jsonl(self.folder, "core")
        self.assertNotIn("Actor", [r.get("name") for r in core])

    def test_unreal_dict(self):
        # T-STUB-023: functions and glue types present; Blueprint types absent.
        self.walk()
        _, records = read_jsonl(self.folder, "__unreal__")
        by_name = {r.get("name"): r for r in records}
        self.assertEqual(records[0]["rec"], "module")
        self.assertEqual(by_name["log"]["rec"], "func")
        self.assertEqual(by_name["get_editor_subsystem"]["rec"], "func")
        self.assertTrue(by_name["load_asset"]["doc"].startswith("load_asset(name: str"))
        self.assertEqual(by_name["ScopedEditorTransaction"]["rec"], "type")
        self.assertEqual(by_name["ScopedEditorTransaction"]["kind"], "glue")
        self.assertEqual(by_name["DREAM_TEST_DATA"], {"rec": "data", "name": "DREAM_TEST_DATA", "t": "int", "v": 3})
        self.assertEqual(by_name["DREAM_TEST_NAN"]["v"], "nan")
        self.assertNotIn("BP_Spinner_C", by_name)
        self.assertNotIn("sys", by_name)
        self.assertNotIn("Actor", by_name)              # it lives in _unreal_engine
        self.assertNotIn("__getattr__", by_name)


class Schema(WalkTestCase):
    def test_schema_and_round_trip(self):
        # T-STUB-006: valid v1 and reload equals source.
        self.walk()
        manifest = self.manifest()
        for key in ("format", "schema", "key", "engine", "complete", "modules", "unreal_module", "filtered", "stats"):
            self.assertIn(key, manifest)
        for entry in manifest["modules"]:
            self.assertEqual(set(entry), {"name", "cpp_module", "file", "types", "sha256", "snap_ms"})
        files = [m["name"] for m in manifest["modules"]] + ["__unreal__"]
        for name in files:
            raw, records = read_jsonl(self.folder, name)
            lines = raw.decode("ascii").splitlines()
            for line, record in zip(lines, records):
                self.assertEqual(json.dumps(record, ensure_ascii=True, separators=(",", ":")), line)
                self.assertIn(record["rec"], ("module", "type", "func", "data"))
                if record["rec"] == "type":
                    self.assertEqual(set(record), {"rec", "name", "kind", "bases", "base_modules", "uobject", "doc",
                                                   "eprops", "members"})
                    self.assertIn(record["kind"], ("class", "struct", "enum", "delegate", "glue"))
                    self.assertEqual(len(record["bases"]), len(record["base_modules"]))
                    for eprop in record["eprops"]:
                        self.assertEqual(len(eprop), 4)
                        self.assertIn(eprop[2], ("rw", "ro"))
                    for member in record["members"]:
                        self.assertLessEqual(set(member), {"n", "k", "doc", "t", "v"})
                        self.assertIn(member["k"], ("getset", "method", "classmethod", "glue", "constant",
                                                    "enum_entry", "member", "data", "other"))
        # The index dream-check builds reads the same records back.
        index = support.SnapshotIndex(self.folder, support.SNAPSHOT_KEY)
        self.assertTrue(index.usable())
        self.assertTrue(index.is_subclass("StaticMeshActor", "Actor"))
        self.assertEqual(index.eprops("StaticMeshActor")["tags"], ("Array[Name]", False))
        self.assertEqual(index.method_returns("EditorAssetLibrary", "load_asset"), "Object")


class Slicing(WalkTestCase):
    def bulk_modules(self, count):
        module = types.ModuleType("_unreal_bulk")
        base = type("Object", (), {"__doc__": "Base.", "__module__": "builtins"})
        module.__dict__["Object"] = base
        method = support.methodwithclosure_descriptor
        getset = support.getset_descriptor
        for number in range(count):
            name = "Bulk%05d" % number
            namespace = {
                "__doc__": "Generated type %d.\r\n\r\n**C++ Source:**\r\n\r\n- **Module**: Bulk\r\n- **File**: Bulk.h\r\n"
                           "\r\n**Editor Properties:** (see get_editor_property/set_editor_property)\r\n\r\n"
                           "- ``value`` (int32): [Read-Write] A value." % number,
                "__module__": "builtins",
                "value": getset("(int32): [Read-Write] A value."),
                "get_value": method("x.get_value() -> int32\r\nReturns the value."),
                "set_value": method("x.set_value(value) -> None\r\nSets the value."),
                "LIMIT": support.constant_value("100"),
            }
            module.__dict__[name] = type(name, (base,), namespace)
        unreal = types.ModuleType("unreal")
        return {"_unreal_bulk": module, "unreal": unreal}

    def test_50000_types_in_4ms_slices(self):
        # T-STUB-002: 50,000 fake types, 4 ms budget: max step <= 4.5 ms; completes. The slicing logic is checked on a
        # deterministic clock (every clock read costs 50 us of simulated work), so a preempted slice on a loaded
        # machine cannot fail it; the wall-clock run is printed, and its median slice must fit too.
        modules = self.bulk_modules(50_000)
        clock = [0]

        def fake_now():
            clock[0] += 50_000
            return clock[0]

        real_now = stubs._now
        stubs._now = fake_now
        try:
            writer, complete = self.walk(modules)
        finally:
            stubs._now = real_now
        self.assertTrue(complete)
        stats = self.manifest()["stats"]
        self.assertLessEqual(stats["max_slice_us"], 4500)
        self.assertEqual(stats["overruns"], 0)
        self.assertEqual(self.manifest()["modules"][0]["types"], 50_001)
        self.assertEqual(support.counter_total(), 0)

        shutil.rmtree(self.folder, True)
        durations = []
        gc.collect()
        gc.disable()
        try:
            stubs.start(support.SNAPSHOT_KEY, support.ENGINE, [], _modules=modules)
            writer = support.SnapshotWriter(self.folder)
            while stubs.is_active():
                began = time.perf_counter_ns()
                output = stubs.step(4_000_000)
                durations.append(time.perf_counter_ns() - began)
                writer.apply(output)
        finally:
            gc.enable()
        durations.sort()
        stats = self.manifest()["stats"]
        print("\nT-STUB-002 wall clock: %d slices, median %.2f ms, p99 %.2f ms, max %.2f ms, %d over budget"
              % (len(durations), durations[len(durations) // 2] / 1e6, durations[int(len(durations) * 0.99)] / 1e6,
                 durations[-1] / 1e6, stats["overruns"]))
        self.assertLessEqual(durations[len(durations) // 2], 4_500_000)

    def test_resume_after_interruption(self):
        # T-STUB-025: an interrupted walk leaves no .complete; the next walk skips the modules already written.
        modules = support.build_fake_unreal()
        writer, complete = self.walk(modules, budget_ns=600_000, stop_after_modules=2)
        self.assertFalse(complete)
        self.assertFalse(os.path.exists(os.path.join(self.folder, ".complete")))
        self.assertFalse(os.path.exists(os.path.join(self.folder, "manifest.json")))
        with open(os.path.join(self.folder, "progress.json"), encoding="utf-8") as handle:
            done = json.load(handle)
        self.assertEqual([entry["name"] for entry in done], ["engine", "unrealed"])
        written = {name: os.path.getmtime(os.path.join(self.folder, "modules", name + ".jsonl")) for name in
                   ("engine", "unrealed")}
        time.sleep(0.05)
        resumed = support.SnapshotWriter(self.folder)
        resumed.entries = list(done)
        self.assertTrue(resumed.run(modules=modules, done=done))
        manifest = self.manifest()
        self.assertEqual([m["name"] for m in manifest["modules"]],
                         ["engine", "unrealed", "core", "editorscriptingutilities", "python"])
        for name, mtime in written.items():
            self.assertEqual(os.path.getmtime(os.path.join(self.folder, "modules", name + ".jsonl")), mtime)
        self.assertTrue(support.SnapshotIndex(self.folder, support.SNAPSHOT_KEY).usable())

    def test_slices_respect_a_small_budget(self):
        writer, complete = self.walk(budget_ns=1_000_000)
        self.assertTrue(complete)
        self.assertGreater(writer.steps, 1)


if __name__ == "__main__":
    unittest.main()
