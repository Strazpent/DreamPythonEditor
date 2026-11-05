# SPDX-License-Identifier: Apache-2.0
"""Namespaces, dunders, sys.path and Reset Session (T-RUN-007, T-RUN-008, T-RUN-011, T-RUN-012, T-RUN-025 subset;
RUN-10, RUN-11, RT-15, TDD §5.3.3, §5.3.4)."""

import builtins
import linecache
import os
import sys
import tempfile
import unittest

import dream_editor
from dream_testing import ERROR, OK, RunnerTestCase

from dream_editor._impl import namespaces
from dream_editor._impl import runner


class Session(RunnerTestCase):
    def test_session_is_the_captured_dict(self):
        identity = id(self.session)
        self.run_code("shared = 1\n", filename="<dream:Tab A#000a>")
        result = self.run_code("seen = shared + 1\n", filename="<dream:Tab B#000b>")
        self.assertEqual(result.outcome, OK)
        self.assertIs(namespaces.session(), self.session)
        self.assertEqual(id(self.session), identity)
        self.assertEqual(self.session["seen"], 2)

    def test_boot_never_writes_dream_editor(self):
        self.assertNotIn("dream_editor", self.session)

    def test_name_is_main_and_restored(self):
        self.session["__name__"] = "console"
        result = self.run_code("probe = __name__\nok = __name__ == '__main__'\n")
        self.assertEqual(result.outcome, OK)
        self.assertEqual(self.session["probe"], "__main__")
        self.assertEqual(self.session["__name__"], "console")

    def test_file_for_saved_and_untitled_buffers(self):
        # T-RUN-007
        self.session["__file__"] = "previous.py"
        self.run_code("during = globals().get('__file__')\n", file_path="C:/proj/tool.py",
                      filename="C:/proj/tool.py", dirty=True)
        self.assertEqual(self.session["during"], "C:/proj/tool.py")
        self.assertEqual(self.session["__file__"], "previous.py")
        self.run_code("during = globals().get('__file__', 'absent')\n")
        self.assertEqual(self.session["during"], "absent")
        self.assertEqual(self.session["__file__"], "previous.py")
        for route, name in (("repl", "<dream:REPL#3>"), ("console", "<dream:Console#4>")):
            self.run_code("during = globals().get('__file__', 'absent')\n", route=route, filename=name)
            self.assertEqual(self.session["during"], "absent")
        linecache.cache.pop("C:/proj/tool.py", None)

    def test_file_deleted_mid_run(self):
        self.session["__file__"] = "previous.py"
        result = self.run_code("del __file__\n", file_path="C:/proj/tool.py", filename="C:/proj/tool.py",
                               dirty=True)
        self.assertEqual(result.outcome, OK)
        self.assertEqual(self.session["__file__"], "previous.py")
        result = self.run_code("globals().clear()\n")
        self.assertEqual(result.outcome, OK)
        self.assertEqual(self.session["__file__"], "previous.py")
        self.assertEqual(self.session["__name__"], "__main__")
        linecache.cache.pop("C:/proj/tool.py", None)

    def test_no_previous_file_stays_absent(self):
        self.run_code("x = __file__\n", file_path="C:/proj/a.py", filename="C:/proj/a.py", dirty=True)
        self.assertNotIn("__file__", self.session)
        self.assertEqual(self.session["x"], "C:/proj/a.py")
        linecache.cache.pop("C:/proj/a.py", None)


class Isolated(RunnerTestCase):
    def test_fresh_dict_with_the_dunders(self):
        # T-RUN-011, D8 matrix: Run File gets a fresh dict, kept as the last-run namespace.
        self.session["hidden"] = 1
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "job.py")
            text = "import sys\nfacts = (__name__, __file__, __doc__, __package__, __spec__)\ntry:\n" \
                   "    hidden\nexcept NameError:\n    sees_session = False\nelse:\n    sees_session = True\n"
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)
            result = self.run_code(text, route="run_file", namespace="isolated", filename=path, file_path=path)
        self.assertEqual(result.outcome, OK, result.errors)
        ns = result.ns
        self.assertIsNot(ns, self.session)
        self.assertEqual(ns["facts"], ("__main__", path, None, None, None))
        self.assertFalse(ns["sees_session"])
        self.assertIs(ns["__builtins__"], builtins)
        self.assertIs(namespaces.last_run(), ns)
        self.assertNotIn("facts", self.session)
        # Released when the next run starts.
        self.run_code("x = 1\n")
        self.assertIsNone(namespaces.last_run())

    def test_isolated_untitled_has_no_file(self):
        result = self.run_code("has_file = '__file__' in globals()\n", namespace="isolated")
        self.assertFalse(result.ns["has_file"])

    def test_run_file_sys_path_entry(self):
        # T-RUN-008: before equals after except for the user's own changes; cwd unchanged.
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "job.py")
            entry = os.path.dirname(path)
            before = list(sys.path)
            cwd = os.getcwd()
            result = self.run_code("import sys\nseen = sys.path[0]\nargv = list(sys.argv)\n", route="run_file",
                                   namespace="isolated", filename=path, file_path=path, argv=["-x", "7"])
            self.assertEqual(result.outcome, OK, result.errors)
            self.assertEqual(result.ns["seen"], entry)
            self.assertEqual(result.ns["argv"], [path, "-x", "7"])
            self.assertEqual(sys.path, before)
            self.assertEqual(os.getcwd(), cwd)

            # The user removed our entry: nothing else is touched.
            self.run_code("import sys\nsys.path.pop(0)\n", route="run_file", namespace="isolated",
                          filename=path, file_path=path)
            self.assertEqual(sys.path, before)

            # The user inserted an equal string: theirs stays, only our object goes.
            self.run_code(f"import sys\nsys.path.append({entry!r})\n", route="run_file", namespace="isolated",
                          filename=path, file_path=path)
            self.assertEqual(sys.path, before + [entry])
            sys.path.pop()

            # An entry the script added stays.
            self.run_code("import sys\nsys.path.append('added-by-script')\n", route="run_file",
                          namespace="isolated", filename=path, file_path=path)
            self.assertEqual(sys.path[-1], "added-by-script")
            sys.path.pop()
            self.assertEqual(sys.path, before)

    def test_other_routes_do_not_touch_sys_path(self):
        before = list(sys.path)
        self.run_code("x = 1\n", file_path="C:/proj/tool.py", filename="C:/proj/tool.py", dirty=True)
        self.assertEqual(sys.path, before)
        linecache.cache.pop("C:/proj/tool.py", None)

    def test_sys_argv_restored_after_an_error(self):
        before = sys.argv
        self.run_code("raise ValueError()\n", route="run_file", namespace="isolated", filename="C:/p/r.py",
                      file_path="C:/p/r.py")
        self.assertIs(sys.argv, before)


class ResetSession(RunnerTestCase):
    def test_reset_restores_the_first_capture(self):
        # T-RUN-012
        self.session["kept"] = "original"
        dream_editor._boot(self.session, 1)          # a new generation captures again
        self.run_code("kept = 'changed'\nadded = 1\ndel __doc__\n")
        self.session["dream_editor"] = dream_editor
        identity = id(self.session)
        removed = namespaces.reset_session()
        self.assertEqual(sorted(removed), ["added"])
        self.assertEqual(self.session["kept"], "original")
        self.assertIsNone(self.session["__doc__"])
        self.assertIs(self.session["dream_editor"], dream_editor)
        self.assertEqual(id(self.session), identity)

    def test_runs_work_after_reset_and_clearing_globals(self):
        namespaces.reset_session()
        result = self.run_code("import dream_editor\ndel dream_editor\nglobals().clear()\n")
        self.assertEqual(result.outcome, OK)
        result = self.run_code("x = len('abc')\nx\n")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(result.results, "3\n")

    def test_reset_forgets_pseudo_file_entries_and_the_last_run(self):
        self.run_code("x = 1\n", filename="<dream:Gone#0011>")
        self.run_code("y = 1\n", namespace="isolated")
        self.assertIsNotNone(namespaces.last_run())
        self.assertIn("<dream:Gone#0011>", linecache.cache)
        namespaces.reset_session()
        self.assertNotIn("<dream:Gone#0011>", linecache.cache)
        self.assertIsNone(namespaces.last_run())

    def test_reset_is_refused_during_a_run(self):
        self.session["reset"] = namespaces.reset_session
        result = self.run_code("reset()\n")
        self.assertEqual((result.outcome, result.detail),
                         (ERROR, "RuntimeError: Reset Session is not available while a run is active"))
        self.assertIn("not available while a run is active", result.errors)
        self.assertEqual(runner.active_run_id(), 0)


if __name__ == "__main__":
    unittest.main()
