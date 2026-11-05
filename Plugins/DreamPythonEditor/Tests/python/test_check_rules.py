# SPDX-License-Identifier: Apache-2.0
"""dream-check rules in process (TDD §7.3.3, §7.12): T-DIAG-001..011, T-DIAG-026, and the INT-08 fixtures (a positive
and a negative per rule). A fixture marks each expected diagnostic with ``# expect: <code>`` on the line it starts on.
"""

import os
import re
import time
import unittest

import check_support as support

from dream_tools.check import analysis

EXPECT_RE = re.compile(r"#\s*expect:\s*([A-Z0-9, ]+)")
OFF = {"oneUndoStepPerRun": False}


def expected(text):
    result = set()
    for number, line in enumerate(text.split("\n"), 1):
        match = EXPECT_RE.search(line)
        if match:
            for code in match.group(1).replace(",", " ").split():
                result.add((number, code))
    return result


def found(diagnostics):
    return {(d.start[0] + 1, d.code) for d in diagnostics if d.code.startswith("SUM") and d.code != "SUM900"}


class Fixtures(unittest.TestCase):
    """INT-08: every code fires on its positive fixture and not on its negative fixture."""

    RULES = ("101", "102", "103", "104", "201", "202", "203", "204", "205", "301")

    def check(self, name, config=None):
        text = support.read_fixture(name)
        diagnostics = support.analyse(text, config)
        self.assertEqual(found(diagnostics), expected(text),
                         "%s: %s" % (name, [d for d in diagnostics if d.code.startswith("SUM")]))
        return diagnostics

    def test_every_rule_has_both_fixtures(self):
        for rule in self.RULES:
            for kind in ("pos", "neg"):
                self.assertTrue(os.path.isfile(os.path.join(support.FIXTURES, "sum%s_%s.py" % (rule, kind))), rule)

    def test_positive_fixtures(self):
        for rule in self.RULES:
            with self.subTest(rule=rule):
                text = support.read_fixture("sum%s_pos.py" % rule)
                self.assertTrue(any(code == "SUM" + rule for _, code in expected(text)), "no expectation")
                self.check("sum%s_pos.py" % rule, OFF if rule == "104" else None)

    def test_negative_fixtures(self):
        for rule in self.RULES:
            with self.subTest(rule=rule):
                diagnostics = self.check("sum%s_neg.py" % rule, OFF if rule == "104" else None)
                self.assertNotIn("SUM" + rule, support.codes(diagnostics))


class Sum101(unittest.TestCase):
    def test_import_forms_and_closest_name(self):
        # T-DIAG-001
        diagnostics = support.analyse(support.read_fixture("sum101_pos.py"))
        sum101 = [d for d in diagnostics if d.code == "SUM101"]
        self.assertEqual(len(sum101), 5)
        titles = sorted(d.fix.title for d in sum101 if d.fix is not None)
        self.assertIn("Replace with unreal.EditorAssetLibrary", titles)
        self.assertIn("Replace with ue.StaticMeshActor", titles)
        self.assertIn("Replace with unreal.EditorAssetLibrary.load_asset", titles)
        level1 = next(d for d in sum101 if "EditorAssetLibary'" in d.message and d.start[0] == 7)
        text = support.apply_edits(support.read_fixture("sum101_pos.py"), level1.fix.edits)
        self.assertIn('asset = unreal.EditorAssetLibrary.load_asset("/Game/A")', text)

    def test_without_a_snapshot_only_sum900(self):
        # T-DIAG-001 / RT-13: SUM101-SUM103 are off and SUM900 says why.
        text = support.read_fixture("sum101_pos.py")
        diagnostics = support.analyse(text, index=None)
        self.assertEqual(support.codes(diagnostics), ["SUM900"])
        self.assertIn("unavailable", diagnostics[0].message)

    def test_building_snapshot_says_so(self):
        folder = os.path.join(support.tempfile.mkdtemp(prefix="dream-building-"), "snapshot")
        os.makedirs(folder)
        index = support.SnapshotIndex(folder, support.SNAPSHOT_KEY)
        diagnostics = support.analyse("import unreal\nx = unreal.Nope\n", index=index)
        self.assertEqual(support.codes(diagnostics), ["SUM900"])
        self.assertIn("being built", diagnostics[0].message)

    def test_wrong_key_is_unavailable(self):
        index = support.SnapshotIndex(support.fixture_snapshot(), "ffffffffffffffff")
        diagnostics = support.analyse("import unreal\nx = unreal.Nope\n", index=index)
        self.assertEqual(support.codes(diagnostics), ["SUM900"])

    def test_no_sum900_without_unreal(self):
        self.assertEqual(support.analyse("import os\nx = 1\n", index=None), [])


class Sum102(unittest.TestCase):
    def test_messages_and_severities(self):
        # T-DIAG-002
        diagnostics = [d for d in support.analyse(support.read_fixture("sum102_pos.py")) if d.code == "SUM102"]
        by_line = {d.start[0] + 1: d for d in diagnostics}
        self.assertEqual(by_line[7].message, "`lod_group` is not a Python attribute of `StaticMesh` (editor-only "
                                             "property); this assignment raises AttributeError. Use set_editor_property.")
        self.assertEqual(by_line[7].severity, analysis.WARNING)
        self.assertEqual(by_line[8].message, "`light_map_resolution` is read-only as an attribute; use set_editor_property.")
        self.assertEqual(by_line[8].severity, analysis.WARNING)
        self.assertEqual(by_line[13].severity, analysis.INFORMATION)
        self.assertIn("`initial_life_span` is not a Python attribute of `Actor`", by_line[13].message)
        self.assertIsNone(by_line[13].fix and None)

    def test_variant(self):
        # T-DIAG-002: with the variant on, tex.lod_bias = 1 gets the notification message.
        text = "import unreal\ntex = unreal.Texture2D()\ntex.lod_bias = 1\n"
        self.assertEqual(support.analyse(text), [])
        diagnostics = support.analyse(text, {"sum102NotifyVariant": True})
        self.assertEqual([(d.code, d.message) for d in diagnostics],
                         [("SUM102", "Attribute writes skip edit-change notifications; set_editor_property sends them.")])

    def test_fix_goldens(self):
        # T-DIAG-003: Assign and AugAssign rewritten; a multi-line value kept; no fix for chained or tuple targets.
        text = (
            "import unreal\n"
            "mesh = unreal.StaticMesh()\n"
            "mesh.lod_group = 'LargeProp'  # keep\n"
            "mesh.light_map_resolution += 2 * 32\n"
            "mesh.lod_group = choose(\n"
            "    'A',\n"
            "    'B')\n"
            "mesh.lod_group = other.lod_group = 'X'\n"
            "mesh.lod_group, n = 'X', 1\n"
        )
        diagnostics = [d for d in support.analyse(text) if d.code == "SUM102"]
        by_line = {}
        for d in diagnostics:
            by_line.setdefault(d.start[0] + 1, []).append(d)
        self.assertTrue(by_line[3][0].fix.preferred)
        self.assertEqual(support.apply_edits(text, by_line[3][0].fix.edits).split("\n")[2],
                         "mesh.set_editor_property(\"lod_group\", 'LargeProp')  # keep")
        self.assertEqual(support.apply_edits(text, by_line[4][0].fix.edits).split("\n")[3],
                         'mesh.set_editor_property("light_map_resolution", '
                         'mesh.get_editor_property("light_map_resolution") + (2 * 32))')
        rewritten = support.apply_edits(text, by_line[5][0].fix.edits).split("\n")
        self.assertEqual(rewritten[4:7], ["mesh.set_editor_property(\"lod_group\", choose(", "    'A',", "    'B'))"])
        self.assertTrue(all(d.fix is None for d in by_line[8]))
        self.assertTrue(all(d.fix is None for d in by_line[9]))


class Sum103(unittest.TestCase):
    def test_fix_renames(self):
        # T-DIAG-004
        text = support.read_fixture("sum103_pos.py")
        diagnostics = [d for d in support.analyse(text) if d.code == "SUM103"]
        renamed = next(d for d in diagnostics if "k2_set_actor_location" in d.message)
        self.assertEqual(renamed.fix.title, "Rename to set_actor_location")
        self.assertIn("actor.set_actor_location(unreal.Vector(), False, False)",
                      support.apply_edits(text, renamed.fix.edits))
        deprecated_class = next(d for d in diagnostics if "EditorLevelLibrary" in d.message)
        self.assertIsNone(deprecated_class.fix)
        self.assertIn("Level Editor Subsystem", deprecated_class.message)


class Sum104(unittest.TestCase):
    def test_only_when_the_toggle_is_off(self):
        # T-DIAG-005
        text = support.read_fixture("sum104_pos.py")
        self.assertNotIn("SUM104", support.codes(support.analyse(text)))
        self.assertNotIn("SUM104", support.codes(support.analyse(text, {"oneUndoStepPerRun": True})))
        self.assertEqual(support.codes(support.analyse(text, OFF)).count("SUM104"), 3)

    def test_wrap_fix(self):
        text = ("import unreal\n"
                "subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n"
                "for actor in subsystem.get_all_level_actors():\n"
                "    actor.set_actor_label('A')\n"
                "\n"
                "    actor.modify()\n"
                "print('done')\n")
        diagnostic = next(d for d in support.analyse(text, OFF) if d.code == "SUM104")
        fixed = support.apply_edits(text, diagnostic.fix.edits)
        self.assertEqual(fixed.split("\n")[2:8], [
            'with unreal.ScopedEditorTransaction("Edit actors"):',
            "    for actor in subsystem.get_all_level_actors():",
            "        actor.set_actor_label('A')",
            "",
            "        actor.modify()",
            "print('done')",
        ])
        self.assertNotIn("SUM104", support.codes(support.analyse(fixed, OFF)))


class Threads(unittest.TestCase):
    def test_sum201_shapes(self):
        # T-DIAG-006: direct, depth-2 helper, Thread.run, executor.submit (and a lambda).
        text = support.read_fixture("sum201_pos.py")
        diagnostics = [d for d in support.analyse(text) if d.code == "SUM201"]
        messages = " | ".join(d.message for d in diagnostics)
        self.assertIn("called from Thread target outer", messages)
        self.assertIn("Loader.run", messages)
        self.assertIn("executor submit submitted", messages)

    def test_sum202_fix(self):
        # T-DIAG-007: the fix inserts dream_editor.sleep and the import.
        text = '"""Doc."""\nfrom __future__ import annotations\nimport time\nfor i in range(3):\n    time.sleep(1)\n'
        diagnostic = next(d for d in support.analyse(text) if d.code == "SUM202")
        fixed = support.apply_edits(text, diagnostic.fix.edits)
        self.assertEqual(fixed, '"""Doc."""\nfrom __future__ import annotations\nimport dream_editor\nimport time\n'
                                'for i in range(3):\n    dream_editor.sleep(1)\n')
        self.assertEqual(support.codes(support.analyse(fixed)), [])
        aliased = "import dream_editor as se\nimport time\nwhile True:\n    time.sleep(1)\n"
        diagnostic = next(d for d in support.analyse(aliased) if d.code == "SUM202")
        self.assertEqual(support.apply_edits(aliased, diagnostic.fix.edits),
                         "import dream_editor as se\nimport time\nwhile True:\n    se.sleep(1)\n")

    def test_sum203_to_205(self):
        # T-DIAG-008, T-DIAG-009
        self.assertEqual(support.codes(support.analyse(support.read_fixture("sum203_pos.py"))).count("SUM203"), 3)
        self.assertEqual(support.codes(support.analyse(support.read_fixture("sum204_pos.py"))).count("SUM204"), 2)
        sum205 = [d for d in support.analyse(support.read_fixture("sum205_pos.py")) if d.code == "SUM205"]
        self.assertEqual(len(sum205), 2)
        self.assertTrue(all(d.severity == analysis.INFORMATION for d in sum205))


class Sum301(unittest.TestCase):
    def test_content_paths_only(self):
        # T-DIAG-010
        diagnostics = support.analyse(support.read_fixture("sum301_pos.py"))
        self.assertEqual(support.codes(diagnostics).count("SUM301"), 7)
        self.assertTrue(all("rename_asset" in d.message for d in diagnostics if d.code == "SUM301"))
        self.assertNotIn("SUM301", support.codes(support.analyse(support.read_fixture("sum301_neg.py"))))


class Noqa(unittest.TestCase):
    def test_noqa(self):
        # T-DIAG-011
        text = support.read_fixture("noqa_cases.py")
        diagnostics = support.analyse(text)
        self.assertEqual(found(diagnostics), expected(text))


class RuleFailure(unittest.TestCase):
    def test_a_rule_raises(self):
        # T-DIAG-026: SUM901; the other rules still report.
        def broken(ctx):
            raise RuntimeError("boom")

        families = [("broken", broken, False)] + list(analysis._families())
        text = support.read_fixture("sum202_pos.py")
        diagnostics = analysis.analyze(text, None, support.fixture_index(), families=families)
        codes = support.codes(diagnostics)
        self.assertIn("SUM901", codes)
        self.assertIn("SUM202", codes)
        failure = next(d for d in diagnostics if d.code == "SUM901")
        self.assertIn("'broken' failed: RuntimeError: boom", failure.message)
        self.assertEqual(failure.severity, analysis.INFORMATION)


class Syntax(unittest.TestCase):
    def test_one_syntax_error_and_no_rules(self):
        text = "import time\nfor i in range(3):\n    time.sleep(1)\nx = (\n"
        diagnostics = support.analyse(text)
        self.assertEqual([d.code for d in diagnostics], [analysis.SYNTAX_CODE])
        self.assertEqual(diagnostics[0].severity, analysis.ERROR)
        self.assertTrue(diagnostics[0].message.startswith("SyntaxError: "))

    def test_null_byte(self):
        diagnostics = support.analyse("x = 1\x00\n")
        self.assertEqual([d.code for d in diagnostics], [analysis.SYNTAX_CODE])


PLAIN = '''

def compute(values, factor=2):
    total = 0
    for value in values:
        if value % 3 == 0:
            total += value * factor
        elif value % 5:
            total -= value
        else:
            total = max(total, value)
    return {"total": total, "count": len(values), "label": "x" + str(total)}


class Thing:
    def __init__(self, name):
        self.name = name
        self.items = [i * i for i in range(10)]

    def describe(self):
        return f"{self.name}: {sum(self.items)}"
'''


def five_thousand_lines():
    """A 5,000-line script: plain code with the findings of five positive fixtures mixed in (about one line in
    eight is a finding or its context)."""
    rules = "".join(support.read_fixture("sum%s_pos.py" % rule).split("\n", 1)[1]
                    for rule in ("104", "202", "301", "102", "201"))
    chunk = (PLAIN * 4 + rules).split("\n")
    lines = []
    while len(lines) < 5000:
        lines.extend(chunk)
    lines = lines[:5000]
    while lines and (lines[-1].startswith((" ", "\t")) or lines[-1].rstrip().endswith(":")):
        lines.pop()
    return "\n".join(lines) + "\n"


class Timing(unittest.TestCase):
    def test_5000_lines(self):
        # §7.3.3 target: <= 100 ms for 5,000 lines (proposed). Asserted with a generous bound; the value is printed.
        text = five_thousand_lines()
        self.assertGreaterEqual(text.count("\n"), 4900)
        index = support.fixture_index()
        self.assertNotEqual(analysis.analyze(text, OFF, index)[0].code, analysis.SYNTAX_CODE)
        analysis.analyze(text, OFF, index)                       # warm the index
        best = None
        for _ in range(3):
            began = time.perf_counter()
            diagnostics = analysis.analyze(text, OFF, index)
            elapsed = (time.perf_counter() - began) * 1000.0
            best = elapsed if best is None else min(best, elapsed)
        print("\ndream-check: 5,000 lines analysed in %.1f ms (best of 3), %d diagnostics" % (best, len(diagnostics)))
        self.assertLess(best, 1000.0)


if __name__ == "__main__":
    unittest.main()
