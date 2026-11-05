# SPDX-License-Identifier: Apache-2.0
"""check_cancel's deferral row (TDD §5.10.2, A19, T-API-010) and STREAM_AGENT outside agent and bridge runs (§6.1, B33).

§5.10.2: on the game thread inside the user-code window ``dream_editor.check_cancel()`` raises ScriptCancelled; when a
deferral condition holds it returns True instead, so the caller can return early. The conditions are the import
machinery on the stack (an import hook or a module body that ``import`` runs) and the §5.4.3 C++ conditions, which
``_dream.check_cancel(True)`` reports (simulated here with the null module's ``test_set_deferral``).
"""

import os
import shutil
import sys
import tempfile
import threading

import dream_editor
from dream_testing import CANCELLED, OK, RunnerTestCase

from dream_editor._impl import native


class CheckCancelDeferral(RunnerTestCase):
    def setUp(self):
        super().setUp()
        self.session["check_cancel"] = dream_editor.check_cancel
        self.session["stop"] = lambda: self.dream.test_request_stop(self.dream.current_run_id())
        self.seen = []
        self.session["seen"] = self.seen

    def test_inside_the_user_code_window_it_raises_and_counts_a_delivery(self):
        result = self.run_code("stop()\ncheck_cancel()\nseen.append('after')\n")
        self.assertEqual(result.outcome, CANCELLED)
        self.assertEqual(self.seen, [])
        self.assertEqual(self.dream.test_deliveries, 0)      # run_statement's own Stop check raised first

        result = self.run_code("if True:\n    stop()\n    check_cancel()\n    seen.append('after')\n")
        self.assertEqual(result.outcome, CANCELLED)
        self.assertEqual(self.seen, [])
        self.assertEqual(self.dream.test_deliveries, 1)

    def test_a_cpp_deferral_condition_returns_true(self):
        # Saving a package, GC, PostLoad or native re-entry (§5.4.3): check_cancel(True) is False, so no raise.
        self.session["defer"] = self.dream.test_set_deferral
        result = self.run_code("if True:\n    stop()\n    defer(True)\n    seen.append(check_cancel())\n"
                               "    defer(False)\n")
        self.assertEqual(self.seen, [True])
        self.assertEqual(self.dream.test_deliveries, 0)
        # The script returned early by itself: it handled Stop (§5.3.9).
        self.assertEqual((result.outcome, result.detail), (CANCELLED, "the script handled Stop and returned"))

    def test_inside_an_import_hook_it_returns_true(self):
        text = ("import sys\n"
                "class Finder:\n"
                "    def find_spec(self, name, path, target=None):\n"
                "        if name == 'dream_t2_missing_module':\n"
                "            seen.append(check_cancel())\n"
                "        return None\n"
                "finder = Finder()\n"
                "sys.meta_path.insert(0, finder)\n"
                "try:\n"
                "    stop()\n"
                "    try:\n"
                "        import dream_t2_missing_module\n"
                "    except ImportError:\n"
                "        seen.append('import failed')\n"
                "finally:\n"
                "    sys.meta_path.remove(finder)\n")
        result = self.run_code(text)
        self.assertEqual(self.seen, [True, "import failed"])
        self.assertEqual(result.outcome, CANCELLED)

    def test_inside_a_module_body_run_by_import_it_returns_true(self):
        folder = tempfile.mkdtemp(prefix="dream_t2_")
        self.addCleanup(shutil.rmtree, folder, True)
        name = "dream_t2_body_" + os.path.basename(folder).replace("-", "_").lower()
        with open(os.path.join(folder, name + ".py"), "w", encoding="utf-8") as handle:
            handle.write("import builtins, dream_editor\nbuiltins._dream_t2_body = dream_editor.check_cancel()\n")
        sys.path.insert(0, folder)
        self.addCleanup(sys.path.remove, folder)
        self.addCleanup(sys.modules.pop, name, None)
        import builtins
        self.addCleanup(builtins.__dict__.pop, "_dream_t2_body", None)
        result = self.run_code(f"if True:\n    stop()\n    import {name}\n    seen.append('imported')\n")
        self.assertIs(builtins.__dict__.get("_dream_t2_body"), True)
        self.assertEqual(self.seen, ["imported"])
        self.assertEqual(result.outcome, CANCELLED)

    def test_a_worker_thread_gets_the_bool(self):
        self.dream.test_begin_run(41)
        self.addCleanup(self.dream.test_end_run, 41)
        self.dream.user_code_begin(41)
        self.dream.test_request_stop(41)
        seen = []
        worker = threading.Thread(target=lambda: seen.append(dream_editor.check_cancel()))
        worker.start()
        worker.join(10)
        self.assertEqual(seen, [True])
        self.assertEqual(self.dream.test_deliveries, 0)

    def test_sleep_under_a_deferral_keeps_sleeping_on_the_game_thread(self):
        self.dream.test_begin_run(42)
        self.addCleanup(self.dream.test_end_run, 42)
        self.dream.user_code_begin(42)
        self.dream.test_request_stop(42)
        self.dream.test_set_deferral(True)
        dream_editor.sleep(0.06)                 # no raise while the condition holds
        self.dream.test_set_deferral(False)
        with self.assertRaises(dream_editor.ScriptCancelled):
            dream_editor.sleep(0.06)

    def test_no_stop_returns_false(self):
        result = self.run_code("seen.append(check_cancel())\n")
        self.assertEqual(result.outcome, OK)
        self.assertEqual(self.seen, [False])


class NullCheckCancelArguments(RunnerTestCase):
    def test_argument_rules_mirror_the_c_module(self):
        null = native.NullDream()
        with self.assertRaises(TypeError):
            null.check_cancel(1)            # an int is not a bool
        with self.assertRaises(TypeError):
            null.check_cancel(True, True)
        self.assertIs(null.check_cancel(), False)
        self.assertIs(null.check_cancel(False), False)
        self.assertIs(null.check_cancel(True), False)
        null.test_begin_run(5)
        null.test_request_stop(5)
        self.assertIs(null.check_cancel(), True)
        self.assertIs(null.check_cancel(True), False)        # outside the user-code window (OutsideUserCode)
        null.user_code_begin(5)
        self.assertIs(null.check_cancel(True), True)
        self.assertEqual(null.test_deliveries, 1)


class AgentStream(RunnerTestCase):
    def test_stream_agent_is_refused_outside_agent_and_bridge_runs(self):
        # T-OUT-013's STREAM_AGENT row (tier-2 mirror): ValueError for every 1.0 route, accepted for bridge (1.1).
        null = native.NullDream()
        with self.assertRaisesRegex(ValueError, "STREAM_AGENT"):
            null.write(native.STREAM_AGENT, 0, "idle\n")
        null.test_begin_run(3, route="repl")
        with self.assertRaisesRegex(ValueError, "^STREAM_AGENT \\(8\\) is accepted only during an agent or bridge run$"):
            null.write(native.STREAM_AGENT, 3, "repl\n")
        null.test_end_run(3)
        null.test_begin_run(4, route="bridge")
        null.write(native.STREAM_AGENT, 4, "agent says\n")
        self.assertEqual(null.test_lines[-1], (native.STREAM_AGENT, 4, "agent says\n", False))
        with self.assertRaises(TypeError):
            null.write(native.STREAM_AGENT, "4", "x")       # the run_id type is checked first, as in C
        null.test_end_run(4)

    def test_a_run_that_writes_the_agent_stream_raises(self):
        self.session["dream_native"] = native
        result = self.run_code("try:\n    dream_native.get().write(8, dream_native.get().current_run_id(), 'x')\n"
                               "except ValueError as exc:\n    refused = str(exc)\n", route="buffer")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertIn("agent or bridge", self.session["refused"])
