# SPDX-License-Identifier: Apache-2.0
"""Outcome classification and the catch-all entry (T-RUN-014, T-RUN-015, T-RUN-028; RUN-18, TDD §5.3.1, §5.3.9)."""

import threading
import unittest
from unittest import mock

import dream_editor
from dream_testing import ERROR, NOT_EXECUTED, OK, RunnerTestCase

from dream_editor._impl import native
from dream_editor._impl import runner


class SystemExitCodes(RunnerTestCase):
    def test_exit_codes(self):
        cases = [
            ("import sys\nsys.exit(0)\n", OK, "exited (code 0)"),
            ("import sys\nsys.exit()\n", OK, "exited (code 0)"),
            ("import sys\nsys.exit(3)\n", ERROR, "exited (code 3)"),
            ("import sys\nsys.exit('msg')\n", ERROR, "exited (code 1)"),
            ("raise SystemExit(False)\n", OK, "exited (code 0)"),
        ]
        for text, outcome, detail in cases:
            with self.subTest(text=text):
                result = self.run_code(text)
                self.assertEqual((result.outcome, result.detail), (outcome, detail))
                self.assertEqual(result.errors, "")       # SystemExit prints no traceback

    def test_message_code_goes_to_stderr(self):
        result = self.run_code("import sys\nsys.exit('bad input')\n")
        self.assertEqual(result.stderr, "bad input\n")

    def test_exit_stops_the_following_statements(self):
        self.session["ran"] = []
        result = self.run_code("ran.append(1)\nraise SystemExit(2)\nran.append(3)\n", filename="<dream:Exit#0008>")
        self.assertEqual(result.detail, "exited (code 2)")
        self.assertEqual(result.location, ("<dream:Exit#0008>", 2))
        self.assertEqual(self.session["ran"], [1])

    def test_exit_in_run_file(self):
        result = self.run_code("raise SystemExit(4)\n", route="run_file", namespace="isolated")
        self.assertEqual((result.outcome, result.detail), (ERROR, "exited (code 4)"))


class EveryBaseException(RunnerTestCase):
    def setUp(self):
        super().setUp()
        self.session["ScriptCancelled"] = dream_editor.ScriptCancelled

    def test_run_never_raises(self):
        kinds = [
            ("ValueError('v')", ERROR, "ValueError: v"),
            ("KeyboardInterrupt()", ERROR, "KeyboardInterrupt"),        # a foreign KeyboardInterrupt (§5.3.9)
            ("GeneratorExit()", ERROR, "GeneratorExit"),
            ("BaseExceptionGroup('g', [ValueError(1)])", ERROR, "ExceptionGroup: g (1 sub-exception)"),
            ("BaseExceptionGroup('g', [GeneratorExit()])", ERROR, "BaseExceptionGroup: g (1 sub-exception)"),
            ("type('Custom', (BaseException,), {})()", ERROR, "Custom"),
            ("MemoryError()", ERROR, "MemoryError"),
        ]
        for expression, outcome, detail in kinds:
            with self.subTest(expression=expression):
                result = self.run_code(f"raise {expression}\n")
                self.assertEqual((result.outcome, result.detail), (outcome, detail))
                self.assertIn("Traceback (most recent call last):", result.errors)

    def test_cancel_of_another_run_is_an_error(self):
        result = self.run_code("e = ScriptCancelled()\ne.run_number = 999\nraise e\n", run_number=5)
        self.assertEqual((result.outcome, result.detail), (ERROR, "dream_editor.ScriptCancelled"))

    def test_module_qualified_exception_name(self):
        result = self.run_code("import json\njson.loads('{')\n")
        self.assertEqual(result.detail, "json.decoder.JSONDecodeError: Expecting property name enclosed in double "
                                        "quotes: line 1 column 2 (char 1)")

    def test_ok_outcome(self):
        result = self.run_code("x = 1\n")
        self.assertEqual((result.outcome, result.detail, result.location), (OK, "", None))

    def test_expired_request(self):
        self.dream.test_begin_run(4242)
        self.assertIsNone(dream_editor._run(4242))
        self.assertEqual(self.dream.test_outcomes[4242], (NOT_EXECUTED, "request expired", None))
        self.dream.test_end_run(4242)

    def test_runner_bug_is_an_internal_error(self):
        with mock.patch.object(runner, "_classify", side_effect=RuntimeError("runner bug")):
            result = self.run_code("raise ValueError('user')\n")
        self.assertEqual((result.outcome, result.detail), (ERROR, "Dream Python Editor internal error"))
        self.assertIn("Dream Python Editor internal error", result.errors)
        self.assertIn("runner bug", result.errors)
        self.assertEqual(runner.active_run_id(), 0)

    def test_failure_before_user_code_is_an_internal_error(self):
        with mock.patch.object(self.dream, "capture_begin", side_effect=RuntimeError("capture failed")):
            result = self.run_code("x = 1\n")
        self.assertEqual(result.detail, "Dream Python Editor internal error")
        self.assertNotIn("x", self.session)

    def test_user_excepthook_runs_in_a_second_window(self):
        calls = []

        def hook(exc_type, exc, tb):
            frames = []
            while tb is not None:
                frames.append(tb.tb_frame.f_code.co_filename)
                tb = tb.tb_next
            calls.append((exc_type, frames))
            print("hooked")

        with mock.patch("sys.excepthook", hook):
            result = self.run_code("raise LookupError('x')\n", filename="<dream:Hook#0009>")
        self.assertEqual(result.outcome, ERROR)
        self.assertEqual(calls, [(LookupError, ["<dream:Hook#0009>"])])
        self.assertEqual([kind for kind, _ in result.events].count("user_code_begin"), 2)
        self.assertIn("hooked", result.stdout)


class ErrorDetail(RunnerTestCase):
    """Ruling R1: the error detail is "<Type>: <first line of str(exc)>", "<Type>" when that line is empty, at most 200
    characters with "…"; it is also the runtime Problems message, and C++ cuts the type at the first ": "."""

    def test_detail_is_the_last_traceback_line(self):
        result = self.run_code("1 / 0\n", filename="<dream:Zero#0001>")
        self.assertEqual(result.detail, "ZeroDivisionError: division by zero")
        self.assertEqual(result.errors.splitlines()[-1], result.detail)
        self.assertEqual(result.location, ("<dream:Zero#0001>", 1))      # the line stays in location
        self.assertEqual(result.detail.split(": ", 1)[0], "ZeroDivisionError")

    def test_first_line_only(self):
        result = self.run_code("raise ValueError('first line\\nsecond line')\n")
        self.assertEqual(result.detail, "ValueError: first line")

    def test_empty_message_gives_the_bare_type(self):
        cases = [
            ("raise ValueError()\n", "ValueError"),
            ("raise ValueError('')\n", "ValueError"),
            ("raise ValueError('\\nafter a blank first line')\n", "ValueError"),
            ("raise ValueError('   ')\n", "ValueError"),
        ]
        for text, detail in cases:
            with self.subTest(text=text):
                self.assertEqual(self.run_code(text).detail, detail)

    def test_key_error_uses_str(self):
        self.assertEqual(self.run_code("{}['k']\n").detail, "KeyError: 'k'")

    def test_long_message_is_cut_to_200_characters(self):
        result = self.run_code("raise ValueError('x' * 500)\n")
        self.assertEqual(len(result.detail), 200)
        self.assertTrue(result.detail.startswith("ValueError: xxx"))
        self.assertTrue(result.detail.endswith("x\N{HORIZONTAL ELLIPSIS}"))
        exact = "ValueError: " + "y" * (200 - len("ValueError: "))
        self.assertEqual(self.run_code(f"raise ValueError({exact[12:]!r})\n").detail, exact)

    def test_failing_str_does_not_break_classification(self):
        text = ("class Odd(Exception):\n"
                "    def __str__(self):\n"
                "        raise RuntimeError('no text')\n"
                "raise Odd()\n")
        result = self.run_code(text)
        self.assertEqual((result.outcome, result.detail), (ERROR, "Odd: <exception str() failed>"))

    def test_system_exit_and_cancel_details_are_unchanged(self):
        self.assertEqual(self.run_code("raise SystemExit('bye')\n").detail, "exited (code 1)")


class Threads(RunnerTestCase):
    def test_threads_that_outlive_the_run_are_listed(self):
        # T-RUN-028: listed in an info line, never killed.
        release = threading.Event()
        self.session["release"] = release
        text = ("import threading\n"
                "workers = [threading.Thread(target=release.wait, name='fetch-1'),\n"
                "           threading.Thread(target=release.wait, name='fetch-2', daemon=True)]\n"
                "for w in workers:\n"
                "    w.start()\n")
        try:
            result = self.run_code(text)
            self.assertEqual(result.outcome, OK)
            self.assertEqual(result.notices,
                             "2 threads started by this run are still running: fetch-1, fetch-2 (daemon). "
                             "Dream Python Editor does not stop them.\n")
            self.assertTrue(all(worker.is_alive() for worker in self.session["workers"]))
        finally:
            release.set()
            for worker in self.session.get("workers", ()):
                worker.join(5)

    def test_capture_begin_gets_the_threads_alive_before_the_run(self):
        result = self.run_code("x = 1\n")
        alive = [payload for kind, payload in result.events if kind == "capture_begin"][0]
        self.assertIn(threading.current_thread().native_id, alive)
        self.assertEqual([kind for kind, _ in result.events][-1], "capture_end")


class Report(RunnerTestCase):
    def test_report_outcome_is_called_after_the_swaps_are_restored(self):
        seen = {}
        original = self.dream.report_outcome

        def report(run_id, outcome, detail, location):
            import sys
            seen["stdout"] = sys.stdout
            original(run_id, outcome, detail, location)

        import sys
        before = sys.stdout
        with mock.patch.object(self.dream, "report_outcome", report):
            self.run_code("print('x')\n")
        self.assertIs(seen["stdout"], before)
        self.assertNotIsInstance(seen["stdout"], self.dream.Writer)

    def test_location_is_a_str_int_tuple(self):
        result = self.run_code("1 / 0\n")
        self.assertIsInstance(result.location, tuple)
        self.assertIsInstance(result.location[0], str)
        self.assertIsInstance(result.location[1], int)
        self.assertNotEqual(result.detail, "internal: malformed outcome")
        self.assertEqual(native.OUTCOME_ERROR, result.outcome)


if __name__ == "__main__":
    unittest.main()
