# SPDX-License-Identifier: Apache-2.0
"""The run swaps and compare-and-restore (T-RUN-013; TDD §5.3.5, §6.1; D19, D25, RT-07)."""

import sys
import time
import types
import unittest

from dream_testing import ERROR, OK, RunnerTestCase, global_slots

from dream_editor._impl import autoprint
from dream_editor._impl import native
from dream_editor._impl import streams
from dream_editor._impl import task


def _fake_unreal():
    module = types.ModuleType("unreal")
    module.logged = []

    def log(text):
        module.logged.append(("log", text))

    def log_warning(text):
        module.logged.append(("log_warning", text))

    def log_error(text):
        module.logged.append(("log_error", text))

    module.log, module.log_warning, module.log_error = log, log_warning, log_error
    return module


class Swaps(RunnerTestCase):
    def setUp(self):
        self.unreal = _fake_unreal()
        sys.modules["unreal"] = self.unreal
        self.addCleanup(sys.modules.pop, "unreal", None)
        super().setUp()
        self.session["probe"] = self.probe = {}

    def test_slots_during_and_after_a_run(self):
        before = global_slots()
        text = ("import sys, time, warnings, unreal\n"
                "probe['stdout'] = sys.stdout\n"
                "probe['stderr'] = sys.stderr\n"
                "probe['displayhook'] = sys.displayhook\n"
                "probe['sleep'] = time.sleep\n"
                "probe['showwarning'] = warnings.showwarning\n"
                "probe['log'] = (unreal.log, unreal.log_warning, unreal.log_error)\n")
        result = self.run_code(text)
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertIsInstance(self.probe["stdout"], self.dream.Writer)
        self.assertEqual(self.probe["stdout"].name, "<dream stdout>")
        self.assertEqual(self.probe["stderr"].name, "<dream stderr>")
        self.assertIs(self.probe["displayhook"], autoprint.displayhook)
        self.assertIs(self.probe["sleep"], task.sliced_sleep)
        self.assertIs(self.probe["showwarning"], streams.showwarning)
        self.assertEqual([fn.__name__ for fn in self.probe["log"]], ["shim_log", "shim_log_warning", "shim_log_error"])
        self.assertEqual(global_slots(), before)
        self.assertEqual(result.notices, "")

    def test_slots_restored_after_an_error(self):
        before = global_slots()
        result = self.run_code("print('x')\nraise ValueError('after print')\n")
        self.assertEqual(result.outcome, ERROR)
        self.assertEqual(global_slots(), before)

    def test_streams_are_tagged_and_stderr_is_not_an_error(self):
        result = self.run_code("import sys\nprint('out')\nprint('err', file=sys.stderr)\n")
        self.assertEqual(result.stdout, "out\n")
        self.assertEqual(result.stderr, "err\n")
        self.assertEqual(result.errors, "")

    def test_unreal_log_goes_through_the_shims(self):
        result = self.run_code("import unreal\nunreal.log('a')\nunreal.log_warning(2)\nunreal.log_error('c')\n"
                               "unreal.log('   ')\n")
        self.assertEqual(self.unreal.logged, [])
        self.assertEqual(result.text(native.STREAM_INFO), "a\n")
        self.assertEqual(result.text(native.STREAM_WARNING), "2\n")
        self.assertEqual(result.text(native.STREAM_ERROR), "c\n")

    def test_pre_bound_log_bypasses_the_shim(self):
        # Documented (§5.3.5): a binding made before the run keeps the original.
        self.session["early_log"] = self.unreal.log
        self.run_code("early_log('direct')\n")
        self.assertEqual(self.unreal.logged, [("log", "direct")])

    def test_user_replacement_is_kept_with_one_note(self):
        original_stdout = sys.stdout
        self.addCleanup(setattr, sys, "stdout", original_stdout)
        text = ("import sys\n"
                "class StreamWrapper:\n"
                "    def __init__(self, inner):\n"
                "        self.inner = inner\n"
                "    def write(self, s):\n"
                "        return self.inner.write(s)\n"
                "    def flush(self):\n"
                "        pass\n"
                "sys.stdout = StreamWrapper(sys.stdout)\n")
        result = self.run_code(text, run_number=22)
        self.assertEqual(type(sys.stdout).__name__, "StreamWrapper")
        self.assertEqual(result.notices,
                         "sys.stdout was replaced during run #22 by StreamWrapper; Dream Python Editor left it in place.\n")
        sys.stdout = original_stdout

    def test_user_replacement_of_time_sleep_and_unreal_log(self):
        original_sleep = time.sleep
        original_log = self.unreal.log
        self.addCleanup(setattr, time, "sleep", original_sleep)
        self.addCleanup(setattr, self.unreal, "log", original_log)
        result = self.run_code("import time, unreal\ndef my_sleep(s):\n    pass\ntime.sleep = my_sleep\n"
                               "unreal.log = print\n", run_number=7)
        self.assertEqual(time.sleep.__name__, "my_sleep")
        self.assertIs(self.unreal.log, print)
        notes = result.notices.splitlines()
        self.assertEqual(len(notes), 2)
        self.assertIn("time.sleep was replaced during run #7 by my_sleep; Dream Python Editor left it in place.", notes)
        self.assertIn("unreal.log was replaced during run #7 by print; Dream Python Editor left it in place.", notes)

    def test_deleted_slot_gets_the_pre_run_value(self):
        before = sys.displayhook
        result = self.run_code("import sys\ndel sys.displayhook\n")
        self.assertEqual(result.outcome, OK)
        self.assertIs(sys.displayhook, before)
        self.assertEqual(result.notices, "")

    def test_showwarning_honours_file(self):
        result = self.run_code("import warnings, io\nbuf = io.StringIO()\n"
                               "warnings.showwarning('m', UserWarning, 'f.py', 3, file=buf)\nv = buf.getvalue()\n")
        self.assertEqual(self.session["v"], "f.py:3: UserWarning: m\n")
        self.assertEqual(result.text(native.STREAM_WARNING), "")

    def test_warnings_during_a_run_are_warning_lines(self):
        result = self.run_code("import warnings\nwarnings.warn('careful', UserWarning)\n",
                               filename="<dream:Warn#0010>")
        self.assertEqual(result.text(native.STREAM_WARNING),
                         "<dream:Warn#0010>:2: UserWarning: careful\n  warnings.warn('careful', UserWarning)\n")

    def test_emergency_restore(self):
        before = global_slots()
        swap = streams.StreamSwap(77, 3, self.dream)
        swap.install()
        self.assertIsInstance(sys.stdout, self.dream.Writer)
        streams.emergency_restore()
        self.assertEqual(global_slots(), before)
        streams.emergency_restore()                     # idempotent
        self.assertEqual(global_slots(), before)

    def test_time_sleep_in_a_run_sleeps_the_whole_time(self):
        result = self.run_code("import time\nt0 = time.perf_counter()\ntime.sleep(0.12)\n"
                               "elapsed = time.perf_counter() - t0\n")
        self.assertEqual(result.outcome, OK)
        self.assertGreaterEqual(self.session["elapsed"], 0.11)


class SlicedSleep(unittest.TestCase):
    def setUp(self):
        self.calls = []
        task.set_game_thread()

    def fake_sleep(self, seconds):
        self.calls.append(seconds)

    def test_slices_at_most_50ms_during_a_run(self):
        task.begin_run(5, 1, self.fake_sleep)
        try:
            start = time.perf_counter()
            task.sliced_sleep(0.0)
        finally:
            task.end_run()
        self.assertEqual(self.calls, [0.0])
        self.assertLess(time.perf_counter() - start, 0.05)

    def test_slices_real_time(self):
        task.begin_run(5, 1, time.sleep)
        recorded = []
        original = task._ORIGINAL_SLEEP

        def recording(seconds):
            recorded.append(seconds)
            original(seconds)

        task.begin_run(5, 1, recording)
        try:
            task.sliced_sleep(0.13)
        finally:
            task.end_run()
        self.assertGreaterEqual(len(recorded), 3)
        self.assertTrue(all(0 <= seconds <= task.SLICE_SECONDS for seconds in recorded))

    def test_outside_a_run_calls_the_original_once(self):
        original = task._ORIGINAL_SLEEP
        task._ORIGINAL_SLEEP = self.fake_sleep
        try:
            task.sliced_sleep(0.2)
        finally:
            task._ORIGINAL_SLEEP = original
        self.assertEqual(self.calls, [0.2])

    def test_validation_matches_time_sleep(self):
        for bad in (-1, float("nan"), "1", None, 1j, float("inf"), 2 ** 80):
            with self.subTest(value=bad):
                with self.assertRaises(Exception) as ours:
                    task.sliced_sleep(bad)
                with self.assertRaises(Exception) as theirs:
                    time.sleep(bad)
                self.assertIs(type(ours.exception), type(theirs.exception))
                self.assertEqual(str(ours.exception), str(theirs.exception))

    def test_shim_reads_like_time_sleep(self):
        self.assertEqual(task.sliced_sleep.__name__, "sleep")
        self.assertEqual(task.sliced_sleep.__doc__, time.sleep.__doc__)


if __name__ == "__main__":
    unittest.main()
