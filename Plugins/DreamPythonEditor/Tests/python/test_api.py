# SPDX-License-Identifier: Apache-2.0
"""The public API, M1 subset (T-API-005, T-API-007, T-API-008, T-API-010, T-API-011; TDD §5.10, §12.2)."""

import inspect
import io
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
import warnings
from unittest import mock

import dream_editor
import dream_testing
from dream_testing import RunnerTestCase

from dream_editor._impl import deprecation
from dream_editor._impl import native
from dream_editor._impl import task


class Surface(unittest.TestCase):
    def test_dir_is_all_plus_dunders(self):
        # T-API-007
        names = dir(dream_editor)
        public = [name for name in names if not (name.startswith("__") and name.endswith("__"))]
        self.assertEqual(sorted(public), sorted(dream_editor.__all__))
        for name in dream_editor.__all__:
            self.assertTrue(hasattr(dream_editor, name), name)

    def test_script_cancelled(self):
        cls = dream_editor.ScriptCancelled
        self.assertTrue(issubclass(cls, KeyboardInterrupt))
        self.assertFalse(issubclass(cls, Exception))
        self.assertEqual((cls.run_number, cls.source, cls.delivery), (0, "async", 0))

    def test_signatures(self):
        self.assertEqual(str(inspect.signature(dream_editor.progress)), "(i, n=None, msg='')")
        self.assertEqual(str(inspect.signature(dream_editor.open)), "(path, line=None, column=None)")
        self.assertEqual(str(inspect.signature(dream_editor.run)), "(target, *, namespace=None, transaction=None)")
        self.assertEqual(str(inspect.signature(dream_editor.suppress)),
                         "(*, results=None, info=None, warnings=None, errors=None, echo=None)")

    def test_version(self):
        self.assertRegex(dream_editor.__version__, r"^\d+\.\d+\.\d+$")

    def test_content_python_has_no_tests_or_init_unreal(self):
        for root, _dirs, files in os.walk(dream_testing.CONTENT_PYTHON):
            for name in files:
                self.assertFalse(name.startswith("test_") and name.endswith(".py"), os.path.join(root, name))
                self.assertNotEqual(name, "init_unreal.py")


class AbiCheck(unittest.TestCase):
    def test_mismatch_raises_import_error_at_import(self):
        # T-API-005: a separate interpreter, so the package imports from scratch.
        script = (
            "import sys, types\n"
            f"sys.path.insert(0, {dream_testing.CONTENT_PYTHON!r})\n"
            "fake = types.ModuleType('_dream'); fake.ABI_VERSION = 99\n"
            "sys.modules['_dream'] = fake\n"
            "try:\n"
            "    import dream_editor\n"
            "except ImportError as e:\n"
            "    print('ImportError:', e)\n"
        )
        done = subprocess.run([sys.executable, "-I", "-B", "-c", script], capture_output=True, text=True,
                              timeout=60)
        self.assertEqual(done.stdout.strip(),
                         "ImportError: Dream Python Editor Python package and native module do not match; reinstall Dream Python Editor",
                         done.stderr)

    def test_boot_checks_again(self):
        fake = types.ModuleType("_dream")
        fake.ABI_VERSION = 2
        native.reset_for_test()
        sys.modules["_dream"] = fake
        try:
            with self.assertRaisesRegex(ImportError, "reinstall Dream Python Editor"):
                dream_editor._boot({}, 1)
        finally:
            del sys.modules["_dream"]
            native.reset_for_test()


class BootResult(RunnerTestCase):
    """Ruling R3: _boot returns exactly True; an ABI mismatch raises (AbiCheck); any other failure is written to stderr
    (LogPython in the editor) and returns False; FDreamPython::Boot shows the "Dream Python Editor did not start:" reason it finds in
    the command's LogOutput, else "returned False; see LogPython"."""

    def test_success_is_exactly_true(self):
        self.assertIs(dream_editor._boot(self.session, 1), True)
        self.assertEqual(repr(dream_editor._boot(self.session, 1)), "True")     # the EvaluateStatement result

    def test_other_failure_is_logged_and_returns_false(self):
        captured = io.StringIO()
        with mock.patch("sys.stderr", captured):
            self.assertIs(dream_editor._boot(["not", "a", "dict"], 1), False)
        self.assertTrue(captured.getvalue().startswith(
            "Dream Python Editor did not start: TypeError: the Session must be a dict (the console globals)\n"), captured.getvalue())
        self.assertIn("Traceback (most recent call last):", captured.getvalue())

    def test_shadowing_package_is_refused_with_its_path(self):
        # T-ARCH-017 / ENV-07 at tier 2: a copy of the package outside the plugin, first on sys.path.
        with tempfile.TemporaryDirectory() as folder:
            shutil.copytree(os.path.join(dream_testing.CONTENT_PYTHON, "dream_editor"),
                            os.path.join(folder, "dream_editor"), ignore=shutil.ignore_patterns("__pycache__"))
            script = (
                "import sys\n"
                f"sys.path.insert(0, {folder!r})\n"
                "import dream_editor\n"
                "print(repr(dream_editor._boot({}, 1)))\n"
            )
            done = subprocess.run([sys.executable, "-I", "-B", "-c", script], capture_output=True, text=True,
                                  timeout=60)
        self.assertEqual(done.stdout.strip(), "False", done.stderr)
        expected = os.path.join(folder, "dream_editor")
        self.assertIn(f"Dream Python Editor did not start: another dream_editor package shadows Dream's own on sys.path: {expected}",
                      done.stderr)


class Cooperative(RunnerTestCase):
    def test_check_cancel(self):
        # T-API-010; §5.10.2
        self.assertFalse(dream_editor.check_cancel())
        self.dream.test_begin_run(31)
        self.addCleanup(self.dream.test_end_run, 31)
        self.assertFalse(dream_editor.check_cancel())
        self.dream.test_request_stop(31)
        # §5.10.2: outside the user-code window a deferral condition holds (OutsideUserCode): True, not raised.
        self.assertIs(dream_editor.check_cancel(), True)
        self.dream.user_code_begin(31)
        with self.assertRaises(dream_editor.ScriptCancelled):
            dream_editor.check_cancel()
        self.assertEqual(self.dream.test_deliveries, 1)       # the raise counted as a delivery (§5.10.2)
        seen = []
        worker = threading.Thread(target=lambda: seen.append(dream_editor.check_cancel()))
        worker.start()
        worker.join(10)
        self.assertEqual(seen, [True])          # a worker decides for itself; never raises

    def test_check_cancel_carries_the_run_number(self):
        self.session["check_cancel"] = dream_editor.check_cancel
        result = self.run_code(
            "try:\n    check_cancel()\nexcept KeyboardInterrupt as e:\n    number = e.run_number\n",
            run_number=17, before_run=lambda dream, run_id: None)
        self.assertEqual(result.outcome, dream_testing.OK)
        self.assertNotIn("number", self.session)            # no Stop: nothing raised
        stop = lambda: self.dream.test_request_stop(self.dream.current_run_id())  # noqa: E731
        self.session["stop"] = stop
        result = self.run_code("if True:\n    stop()\n    try:\n        check_cancel()\n"
                               "    except KeyboardInterrupt as e:\n        number = e.run_number\n", run_number=17)
        self.assertEqual(self.session["number"], 17)
        self.assertEqual(result.detail, "the script handled Stop and returned")

    def test_progress(self):
        dream_editor.progress(1, 10, "outside a run")          # a no-op outside a run
        self.assertEqual(self.dream.test_progress, {})
        self.dream.test_begin_run(32)
        self.addCleanup(self.dream.test_end_run, 32)
        dream_editor.progress(3, 10, "item")
        self.assertEqual(self.dream.test_progress[32], (3, 10, "item"))
        dream_editor.progress(5)
        self.assertEqual(self.dream.test_progress[32], (5, None, ""))
        with self.assertRaises(ValueError):
            dream_editor.progress(11, 10)
        with self.assertRaises(ValueError):
            dream_editor.progress(-1)
        with self.assertRaises(TypeError):
            dream_editor.progress(1.5, 10)
        worker = threading.Thread(target=dream_editor.progress, args=(7, 10, "worker"))
        worker.start()
        worker.join(10)
        self.assertEqual(self.dream.test_progress[32], (7, 10, "worker"))

    def test_sleep_slices_and_raises_after_stop(self):
        slices = []
        original = task._ORIGINAL_SLEEP

        def recording(seconds):
            slices.append(seconds)
            original(seconds)

        task._ORIGINAL_SLEEP = recording
        self.addCleanup(setattr, task, "_ORIGINAL_SLEEP", original)
        start = time.perf_counter()
        dream_editor.sleep(0.16)
        self.assertGreaterEqual(time.perf_counter() - start, 0.15)
        # Sliced: no slice asks for more than 50 ms. Four slices on an idle machine; a loaded one oversleeps a slice, so
        # only "more than one" is certain (tier 2 runs while the editor builds).
        self.assertGreaterEqual(len(slices), 2)
        self.assertTrue(all(seconds <= task.SLICE_SECONDS for seconds in slices))

        self.dream.test_begin_run(33)
        self.addCleanup(self.dream.test_end_run, 33)
        self.dream.user_code_begin(33)           # a delivery lands only inside the user-code window (§5.10.2)
        timer = threading.Timer(0.12, self.dream.test_request_stop, args=(33,))
        timer.start()
        start = time.perf_counter()
        with self.assertRaises(dream_editor.ScriptCancelled):
            dream_editor.sleep(30)
        self.assertLess(time.perf_counter() - start, 5)
        timer.join()

    def test_sleep_validates_like_time_sleep(self):
        with self.assertRaisesRegex(ValueError, "non-negative"):
            dream_editor.sleep(-1)
        with self.assertRaises(TypeError):
            dream_editor.sleep("1")


class Commands(RunnerTestCase):
    def test_suppress_round_trip(self):
        # T-API-011, T-OUT-043 polarity: True suppresses.
        previous = dream_editor.suppress(echo=True)
        self.assertEqual(previous, {"results": False, "info": False, "warnings": False, "errors": False,
                                    "echo": False})
        self.assertTrue(self.dream.test_suppressed()["echo"])
        previous = dream_editor.suppress(echo=False, results=True)
        self.assertTrue(previous["echo"])
        self.assertEqual(self.dream.test_suppressed(), {"results": True, "info": False, "warnings": False,
                                                       "errors": False, "echo": False})
        self.assertEqual(dream_editor.suppress()["results"], True)
        with self.assertRaises(TypeError):
            dream_editor.suppress(info=1)

    def test_editor_only_commands_raise_without_the_editor(self):
        for call in (lambda: dream_editor.open("C:/x.py"), lambda: dream_editor.new_tab("x = 1"),
                     lambda: dream_editor.run("Untitled-1")):
            with self.subTest(call=call):
                with self.assertRaisesRegex(RuntimeError, "needs the Dream Python Editor"):
                    call()

    def test_game_thread_only_calls_from_a_worker(self):
        errors = []

        def worker():
            for name, call in (("open", lambda: dream_editor.open("C:/x.py")),
                               ("clear_output", dream_editor.clear_output),
                               ("suppress", lambda: dream_editor.suppress(echo=True))):
                try:
                    call()
                except RuntimeError as exc:
                    errors.append(str(exc))

        thread = threading.Thread(target=worker)
        thread.start()
        thread.join(10)
        self.assertEqual(errors, ["dream_editor.open() must be called on the game thread",
                                  "dream_editor.clear_output() must be called on the game thread",
                                  "dream_editor.suppress() must be called on the game thread"])

    def test_run_inside_a_run_raises(self):
        self.session["run"] = dream_editor.run
        result = self.run_code("run('Untitled-2')\n")
        self.assertEqual(result.detail,
                         "RuntimeError: dream_editor.run() cannot start a run inside a Dream run: one run at a time")
        self.assertIn("one run at a time", result.errors)

    def test_argument_validation(self):
        with self.assertRaises(ValueError):
            dream_editor.run("x.py", namespace="agent")
        with self.assertRaises(TypeError):
            dream_editor.run("x.py", transaction="yes")
        with self.assertRaises(TypeError):
            dream_editor.new_tab(5)
        with self.assertRaises(ValueError):
            dream_editor.open("C:/x.py", line=-1)

    def test_clear_output(self):
        dream_editor.clear_output()
        self.assertIn(("clear_output", 0, None), self.dream.test_events)


class Deprecation(unittest.TestCase):
    def test_one_warning_per_call_site(self):
        @deprecation.deprecated(since="0.2.0", removal_version="0.3.0", removal_engine="5.9")
        def old():
            return 1

        caught = []
        original = warnings.showwarning
        warnings.showwarning = lambda message, category, *rest, **kw: caught.append((str(message), category))
        try:
            with_filter = warnings.filters[:]
            warnings.simplefilter("always", DeprecationWarning)
            try:
                for _ in range(3):
                    old()           # one call site
                old()               # a second call site
            finally:
                warnings.filters[:] = with_filter
        finally:
            warnings.showwarning = original
        self.assertEqual(len(caught), 2)
        self.assertTrue(all(category is DeprecationWarning for _message, category in caught))
        self.assertEqual(old.__dream_deprecated__["removal_version"], "0.3.0")

    def test_early_removal_is_blocked(self):
        with self.assertRaises(ValueError):
            deprecation.deprecated(since="0.2.0", removal_version="0.2.9", removal_engine="5.9")
        with self.assertRaises(ValueError):
            deprecation.deprecated(since="0.2.0", removal_version="0.3.0", removal_engine="5.8")
        meta = {"removal_version": "0.3.0", "removal_engine": "5.9"}
        self.assertFalse(deprecation.removal_allowed(meta, "0.3.0", "5.8"))
        self.assertFalse(deprecation.removal_allowed(meta, "0.2.5", "5.9"))
        self.assertTrue(deprecation.removal_allowed(meta, "0.3.0", "5.9"))


if __name__ == "__main__":
    unittest.main()
