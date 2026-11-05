# SPDX-License-Identifier: Apache-2.0
"""Selection aliases (T-RUN-017; TDD §5.3.8, §5.3.3, U19, UEI-03, D3) through prepare with a fake ``unreal`` module.

Absent names are written, Dream's own values are refreshed, a user-rebound name is never touched again (and the table
records the run that rebound it), Reset Session clears the table, Run File and Isolated file runs get no aliases, and a
map change releases Dream's values and the last-run namespace. (From M4: shelf runs get fresh aliases.)
"""

import sys
import types

from dream_testing import OK, RunnerTestCase

from dream_editor._impl import aliases
from dream_editor._impl import namespaces


class _Actor:
    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return f"<Actor {self.name}>"


def _fake_unreal(editor):
    module = types.ModuleType("unreal")

    class EditorActorSubsystem:
        def get_selected_level_actors(self):
            editor["reads"] += 1
            return list(editor["selection"])

    class UnrealEditorSubsystem:
        def get_editor_world(self):
            return editor["world"]

    class EditorUtilityLibrary:
        @staticmethod
        def get_selected_asset_data():
            return list(editor["assets"])

    class SomeEngineSubsystem:
        pass

    instances = {EditorActorSubsystem: EditorActorSubsystem(), UnrealEditorSubsystem: UnrealEditorSubsystem()}
    engine_instances = {SomeEngineSubsystem: SomeEngineSubsystem()}
    module.EditorActorSubsystem = EditorActorSubsystem
    module.UnrealEditorSubsystem = UnrealEditorSubsystem
    module.EditorUtilityLibrary = EditorUtilityLibrary
    module.SomeEngineSubsystem = SomeEngineSubsystem
    module.get_editor_subsystem = lambda cls: instances.get(cls)
    module.get_engine_subsystem = lambda cls: engine_instances.get(cls)
    module.log = module.log_warning = module.log_error = lambda text: None
    return module


class Aliases(RunnerTestCase):
    def setUp(self):
        super().setUp()
        aliases.reset_for_test()
        self.addCleanup(aliases.reset_for_test)
        self.a, self.b = _Actor("a"), _Actor("b")
        self.editor = {"selection": [self.a], "world": object(), "assets": ["/Game/A.A"], "reads": 0}
        self.unreal = _fake_unreal(self.editor)
        sys.modules["unreal"] = self.unreal
        self.addCleanup(sys.modules.pop, "unreal", None)

    def repl(self, text, number):
        return self.run_code(text, route="repl", run_number=number, filename=f"<dream:REPL#{number}>")

    def test_absent_names_are_written_before_user_code(self):
        self.session["unreal_cls"] = self.unreal.EditorActorSubsystem
        result = self.repl("seen = (list(sel), world, list(sel_assets), subsys(unreal_cls))\n", 1)
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(result.prepared["aliases_written"], 4)
        seen = self.session["seen"]
        self.assertEqual(seen[0], [self.a])
        self.assertIs(seen[1], self.editor["world"])
        self.assertEqual(seen[2], ["/Game/A.A"])
        self.assertIs(seen[3], self.unreal.get_editor_subsystem(self.unreal.EditorActorSubsystem))
        self.assertIs(aliases.subsys(self.unreal.SomeEngineSubsystem),
                      self.unreal.get_engine_subsystem(self.unreal.SomeEngineSubsystem))
        for name in aliases.NAMES:
            self.assertIs(self.session[name], aliases.LAST_WRITTEN[name])

    def test_ours_is_refreshed_and_an_unchanged_selection_is_not_rewritten(self):
        self.repl("pass\n", 1)
        first = self.session["sel"]
        result = self.repl("pass\n", 2)
        self.assertEqual(result.prepared["aliases_written"], 0)     # same selection: no rewrite, no epoch bump
        self.assertIs(self.session["sel"], first)
        self.editor["selection"] = [self.a, self.b]
        result = self.repl("count = len(sel)\n", 3)
        self.assertEqual(result.prepared["aliases_written"], 1)
        self.assertEqual(self.session["count"], 2)

    def test_a_user_rebound_name_is_never_touched_again(self):
        self.repl("pass\n", 1)
        self.repl("sel = 5\n", 2)
        self.editor["selection"] = [self.b]
        result = self.repl("value = sel\n", 3)
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(self.session["value"], 5)
        self.assertTrue(aliases.is_owned("sel"))
        self.assertEqual(aliases.owned_since("sel"), 2)             # "sel: your value since Run #2" (VAR-02)
        self.assertIsNone(aliases.owned_since("world"))
        # Deleting a user-owned name does not hand it back: Dream never touches it again in this session.
        self.repl("del sel\n", 4)
        self.repl("pass\n", 5)
        self.assertNotIn("sel", self.session)
        # The other names keep being refreshed.
        new_world = object()
        self.editor["world"] = new_world
        self.repl("pass\n", 6)
        self.assertIs(self.session["world"], new_world)

    def test_a_name_present_before_the_first_refresh_is_user_owned(self):
        self.session["world"] = "mine"
        self.repl("pass\n", 1)
        self.assertEqual(self.session["world"], "mine")
        self.assertTrue(aliases.is_owned("world"))
        self.assertIs(self.session["sel"], aliases.LAST_WRITTEN["sel"])

    def test_reset_session_clears_the_table(self):
        self.repl("pass\n", 1)
        self.repl("sel = 'mine'\n", 2)
        self.repl("pass\n", 3)
        self.assertTrue(aliases.is_owned("sel"))
        removed = namespaces.reset_session()
        self.assertIn("sel", removed)
        self.assertNotIn("world", self.session)
        self.assertFalse(aliases.is_owned("sel"))
        self.assertEqual(aliases.LAST_WRITTEN, {})
        self.editor["selection"] = [self.b]
        result = self.repl("value = list(sel)\n", 4)
        self.assertEqual(result.prepared["aliases_written"], 4)
        self.assertEqual(self.session["value"], [self.b])

    def test_run_file_and_isolated_file_runs_get_none(self):
        result = self.run_code("pass\n", route="run_file", namespace="isolated", run_number=1,
                               filename="C:/scripts/tool.py", file_path="C:/scripts/tool.py")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(result.prepared["aliases_written"], 0)
        self.assertNotIn("sel", self.session)
        result = self.run_code("pass\n", route="api", namespace="isolated", run_file=True, run_number=2,
                               filename="C:/scripts/tool.py", file_path="C:/scripts/tool.py")
        self.assertEqual(result.prepared["aliases_written"], 0)
        self.assertNotIn("sel", self.session)
        # An API run of a tab is "as the target": the U19 rule.
        result = self.run_code("pass\n", route="api", run_number=3)
        self.assertEqual(result.prepared["aliases_written"], 4)

    def test_the_setting_turns_the_aliases_off(self):
        result = self.run_code("pass\n", route="selection", aliases=False, run_number=1)
        self.assertEqual(result.prepared["aliases_written"], 0)
        self.assertNotIn("sel", self.session)

    def test_a_failed_prepare_still_refreshed_and_reports_the_count(self):
        result = self.repl("def broken(:\n", 1)
        self.assertFalse(result.prepared["ok"])
        self.assertEqual(result.prepared["aliases_written"], 4)

    def test_without_the_unreal_module_nothing_is_written(self):
        sys.modules.pop("unreal", None)
        result = self.repl("pass\n", 1)
        self.assertEqual(result.prepared["aliases_written"], 0)
        self.assertNotIn("sel", self.session)

    def test_an_editor_read_that_fails_keeps_the_previous_value(self):
        self.repl("pass\n", 1)
        before = self.session["sel"]

        def broken(self_):
            raise RuntimeError("the editor is busy")

        self.unreal.EditorActorSubsystem.get_selected_level_actors = broken
        result = self.repl("pass\n", 2)
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertIs(self.session["sel"], before)

    def test_map_change_releases_dreams_values_and_the_last_run_namespace(self):
        # T-LIFE-014 tier-2 part (D3): nothing Dream wrote keeps the old world alive.
        self.repl("pass\n", 1)
        self.repl("sel_assets = ['kept']\n", 2)
        self.repl("pass\n", 3)          # finds sel_assets rebound
        result = self.run_code("x = 1\n", namespace="isolated", run_number=4)
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertIsNotNone(namespaces.last_run())
        released = aliases.on_map_change()
        self.assertEqual(released, 2)                       # sel and world; subsys holds no world
        self.assertNotIn("sel", self.session)
        self.assertNotIn("world", self.session)
        self.assertEqual(self.session["sel_assets"], ["kept"])
        self.assertIn("subsys", self.session)
        self.assertIsNone(namespaces.last_run())
        new_world = object()
        self.editor["world"] = new_world
        self.repl("pass\n", 5)
        self.assertIs(self.session["world"], new_world)

    def test_idle_refresh_after_a_selection_change(self):
        # The FDreamPython idle tick calls refresh(0) after a selection change (UEI-03: "a REPL line sees the current
        # selection", J1's completion of sel[0]. before any run).
        self.assertEqual(aliases.refresh(0), 4)
        self.editor["selection"] = [self.b]
        self.assertEqual(aliases.refresh(0), 1)
        self.assertEqual(list(self.session["sel"]), [self.b])
        self.assertIsNotNone(aliases.refreshed_at())
