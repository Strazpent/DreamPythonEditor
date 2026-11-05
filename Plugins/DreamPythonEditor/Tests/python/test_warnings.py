# SPDX-License-Identifier: Apache-2.0
"""Compile-time and run-time warnings in statement mode (T-RUN-031; TDD §5.3.2 show-hook silencing)."""

import unittest
import warnings
from unittest import mock

from dream_testing import OK, RunnerTestCase

from dream_editor._impl import native
from dream_editor._impl import runner


class CompileWarnings(RunnerTestCase):
    def setUp(self):
        super().setUp()
        self.shown = []
        self.addCleanup(setattr, warnings, "showwarning", warnings.showwarning)
        # Stands in for the engine's _redirect_warning, the stock showwarning while no run is active.
        warnings.showwarning = lambda message, category, filename, lineno, file=None, line=None: \
            self.shown.append((category, str(message), filename, lineno))
        self.filters_mutated = 0
        original = warnings._filters_mutated

        def counting():
            self.filters_mutated += 1
            original()

        patcher = mock.patch.object(warnings, "_filters_mutated", counting)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_syntax_warning_shown_once(self):
        filters = warnings.filters
        snapshot = list(filters)
        text = "x = 1\nok = x is 1\ny = 2\n"
        result = self.run_code(text, filename="<dream:Warn#0012>")
        self.assertEqual(result.outcome, OK, result.errors)
        syntax = [entry for entry in self.shown if entry[0] is SyntaxWarning]
        self.assertEqual(len(syntax), 1, self.shown)
        self.assertEqual(syntax[0][2:], ("<dream:Warn#0012>", 2))
        self.assertIs(warnings.filters, filters)
        self.assertEqual(warnings.filters, snapshot)
        self.assertEqual(self.filters_mutated, 0)

    def test_display_slots_are_restored_after_prepare(self):
        show = warnings.showwarning
        impl = warnings._showwarnmsg_impl
        runner.prepare(5001, {"text": "a = 1 is 1\nb = 2\n", "filename": "<dream:Warn#0013>"})
        runner.discard(5001)
        self.assertIs(warnings.showwarning, show)
        self.assertIs(warnings._showwarnmsg_impl, impl)

    def test_deprecation_warning_once_over_three_runs(self):
        saved = list(warnings.filters)

        def restore_filters():
            warnings.filters[:] = saved
            warnings._filters_mutated()

        self.addCleanup(restore_filters)
        warnings.filterwarnings("default", category=DeprecationWarning, message="old api")
        filters = warnings.filters
        self.filters_mutated = 0
        texts = []
        for _ in range(3):
            snapshot = list(warnings.filters)
            result = self.run_code("import warnings\nwarnings.warn('old api', DeprecationWarning)\n",
                                   filename="<dream:Dep#0014>")
            self.assertEqual(result.outcome, OK, result.errors)
            texts.append(result.text(native.STREAM_WARNING))
            self.assertIs(warnings.filters, filters)
            self.assertEqual(warnings.filters, snapshot)
        self.assertEqual(sum("old api" in text for text in texts), 1, texts)
        self.assertEqual(self.filters_mutated, 0)

    def test_classify_input_leaves_the_filters_alone(self):
        self.assertEqual(runner.classify_input("x is 1\n", False), "complete")
        self.assertEqual(self.filters_mutated, 0)
        self.assertEqual(self.shown, [])


if __name__ == "__main__":
    unittest.main()
