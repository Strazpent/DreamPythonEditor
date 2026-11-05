# SPDX-License-Identifier: Apache-2.0
"""Stop in statement mode through the null _dream (T-RUN-034 tier-2 analogue, T-RUN-014; TDD §5.3.7, §5.3.9)."""

import sys
import unittest
from unittest import mock

import dream_editor
from dream_testing import CANCELLED, ERROR, RunnerTestCase

from dream_editor._impl import native
from dream_editor._impl import runner


class StopAfterReturn(native.NullDream):
    """A Stop that becomes due just before statement ``index``'s run_statement returns (T-RUN-034 (a), (b)).

    The delivery lands at the eval-breaker check in the runner's loop, after the counts are final; raising here, from
    a frame that is not the run's file, is the same landing as far as classification can see.
    """

    stop_after_index = 0

    def run_statement(self, run_id, index, code, ns):
        super().run_statement(run_id, index, code, ns)
        if index == self.stop_after_index:
            self.test_request_stop(run_id)
            raise self._new_cancelled(source="Stop button", delivery=1)


class StopInStatementMode(RunnerTestCase):
    def setUp(self):
        super().setUp()
        self.marker = []
        self.session["marker"] = self.marker.append
        self.session["check_cancel"] = dream_editor.check_cancel
        self.session["stop"] = lambda: self.dream.test_request_stop(self.dream.current_run_id())

    def test_stop_between_statements(self):
        result = self.run_code("marker(1)\nstop()\nmarker(3)\n")
        self.assertEqual((result.outcome, result.detail, result.location),
                         (CANCELLED, "stopped after statement 2 of 3", None))
        self.assertEqual(self.marker, [1])
        self.assertEqual(self.started_indices(result), [0, 1])
        self.assertEqual(self.echo_indices(result), [0, 1])
        self.assertEqual(self.dream._started_returned[result.run_id], [2, 2])
        self.assertEqual(result.errors, "")

    def test_swallowed_stop_never_lets_a_later_statement_start(self):
        text = ("marker(1)\n"
                "try:\n"
                "    stop()\n"
                "    check_cancel()\n"
                "except BaseException:\n"
                "    pass\n"
                "marker(3)\n")
        result = self.run_code(text)
        self.assertEqual((result.outcome, result.detail), (CANCELLED, "stopped after statement 2 of 3"))
        self.assertEqual(self.marker, [1])

    def test_stop_in_its_own_statement_stops_before_the_next(self):
        # check_cancel() in statement 3 never runs: run_statement's Stop check raises first.
        result = self.run_code("marker(1)\nstop()\ncheck_cancel()\n", filename="<dream:Stop#000a>")
        self.assertEqual((result.outcome, result.detail, result.location),
                         (CANCELLED, "stopped after statement 2 of 3", None))

    def test_stop_lands_in_user_code(self):
        result = self.run_code("marker(1)\nif True:\n    stop()\n    check_cancel()\n", filename="<dream:Stop#000a>",
                               first_line=20)
        self.assertEqual((result.outcome, result.detail, result.location),
                         (CANCELLED, "", ("<dream:Stop#000a>", 23)))
        self.assertEqual(result.errors, "")

    def test_stop_inside_a_function_lands_at_the_users_line(self):
        text = "def work():\n    for i in range(3):\n        if i == 1:\n            stop()\n            check_cancel()\nwork()\n"
        result = self.run_code(text, filename="<dream:Stop#000b>")
        self.assertEqual(result.location, ("<dream:Stop#000b>", 5))

    def test_stop_before_user_code(self):
        result = self.run_code("marker(1)\n", before_run=lambda dream, run_id: dream.test_request_stop(run_id))
        self.assertEqual((result.outcome, result.detail), (CANCELLED, "stopped before user code started"))
        self.assertEqual(self.started_indices(result), [])
        self.assertEqual(self.marker, [])

    def test_script_handled_stop_and_returned(self):
        text = "try:\n    stop()\n    check_cancel()\nexcept KeyboardInterrupt:\n    handled = True\n"
        result = self.run_code(text)
        self.assertEqual((result.outcome, result.detail), (CANCELLED, "the script handled Stop and returned"))
        isolated = ("import dream_editor\n"
                    "from dream_editor._impl import native\n"
                    "s = native.get()\n"
                    "try:\n"
                    "    s.test_request_stop(s.current_run_id())\n"
                    "    dream_editor.check_cancel()\n"
                    "except KeyboardInterrupt:\n"
                    "    pass\n")
        result = self.run_code(isolated, namespace="isolated", route="run_file")
        self.assertEqual((result.outcome, result.detail), (CANCELLED, "the script handled Stop and returned"))

    def test_stop_due_just_before_statement_one_returns(self):
        dream = StopAfterReturn()
        dream.stop_after_index = 0
        native.set_for_test(dream)
        result = self.run_code("marker(1)\nmarker(2)\nmarker(3)\n", dream=dream)
        self.assertEqual((result.outcome, result.detail), (CANCELLED, "stopped after statement 1 of 3"))
        self.assertEqual([p for k, p in result.events if k == "echo"], [0])
        self.assertEqual(self.marker, [1])

    def test_stop_due_just_before_the_last_statement_returns(self):
        dream = StopAfterReturn()
        dream.stop_after_index = 2
        native.set_for_test(dream)
        result = self.run_code("marker(1)\nmarker(2)\nmarker(3)\n", dream=dream)
        self.assertEqual((result.outcome, result.detail), (CANCELLED, "Stop arrived as the code finished"))
        self.assertEqual(self.marker, [1, 2, 3])

    def test_stop_at_a_statements_first_instruction(self):
        # T-RUN-034 (c): the Stop lands at statement 2's RESUME (line 0): reported at its first line, not ":0".
        cancel = dream_editor.ScriptCancelled()

        def arm(dream, run_id):
            statements = runner._PREPARED[run_id].statements
            target = statements[1][2]

            def tracer(frame, event, arg):
                if event == "call" and frame.f_code is target:
                    sys.settrace(None)
                    raise cancel
                return None

            sys.settrace(tracer)

        try:
            result = self.run_code("marker(1)\n\nmarker(\n  2)\nmarker(3)\n", filename="<dream:Line0#000c>",
                                   first_line=30, before_run=arm)
        finally:
            sys.settrace(None)
        self.assertEqual((result.outcome, result.detail, result.location),
                         (CANCELLED, "", ("<dream:Line0#000c>", 32)))
        self.assertEqual(self.dream._started_returned[result.run_id], [2, 1])
        self.assertEqual(self.marker, [1])
        # A traceback of that landing shows the statement's first line, never line 0 (§5.3.7).
        self.assertIn('File "<dream:Line0#000c>", line 0,', runner.format_exception(cancel, None))
        fixed = runner.format_exception(cancel, _prep_like("<dream:Line0#000c>", 32, cancel))
        self.assertIn('File "<dream:Line0#000c>", line 32, in <module>\n    marker(\n', fixed)
        self.assertNotIn("line 0,", fixed)

    def test_handler_raising_while_stopping_is_cancelled(self):
        text = "try:\n    stop()\n    check_cancel()\nexcept KeyboardInterrupt:\n    raise RuntimeError('cleanup failed')\n"
        result = self.run_code(text, filename="<dream:Chain#000d>")
        self.assertEqual(result.outcome, CANCELLED)
        self.assertEqual(result.location, ("<dream:Chain#000d>", 5))
        self.assertIn("RuntimeError: cleanup failed", result.errors)

    def test_foreign_keyboard_interrupt_is_an_error(self):
        result = self.run_code("raise KeyboardInterrupt\n")
        self.assertEqual((result.outcome, result.detail), (ERROR, "KeyboardInterrupt"))

    def test_delivery_right_after_one_exec_returns(self):
        # Outside statement mode a delivery can land at the check right after _execute returns (§5.3.7).
        original = runner._execute

        def execute_then_deliver(prep, dream):
            original(prep, dream)
            raise dream_editor.ScriptCancelled()

        with mock.patch.object(runner, "_execute", execute_then_deliver):
            result = self.run_code("marker(1)\n", statement_mode=False, autoprint=False)
        self.assertIsNone(result.prepared["statements"])
        self.assertEqual((result.outcome, result.detail), (CANCELLED, "Stop arrived as the code finished"))
        self.assertEqual(self.marker, [1])


def _prep_like(filename, first_line, exc):
    """A stand-in prepared request whose statement map covers the frames of ``exc`` (for format_exception)."""

    class Prep:
        pass

    prep = Prep()
    prep.filename = filename
    prep.first_line = first_line
    prep.first_line_by_code = {}
    tb = exc.__traceback__
    while tb is not None:
        if tb.tb_frame.f_code.co_filename == filename:
            prep.first_line_by_code[id(tb.tb_frame.f_code)] = first_line
        tb = tb.tb_next
    return prep


if __name__ == "__main__":
    unittest.main()
