# SPDX-License-Identifier: Apache-2.0
"""Auto-print and ``_`` (T-RUN-009; RUN-08, D8, TDD §5.3.6)."""

import builtins
import unittest

from dream_testing import ERROR, OK, RunnerTestCase

from dream_editor._impl import native

STREAM_RESULT = native.STREAM_RESULT


class AutoPrint(RunnerTestCase):
    def setUp(self):
        super().setUp()
        self._builtin_underscore = getattr(builtins, "_", None)

    def result_writes(self, result):
        return [text for stream, text in result.lines if stream == STREAM_RESULT]

    def test_last_expression_prints_and_binds_underscore(self):
        result = self.run_code("x = 41\nx + 1\n")
        self.assertEqual(result.outcome, OK)
        self.assertEqual(self.result_writes(result), ["42\n"])
        self.assertEqual(self.session["_"], 42)
        self.assertIs(getattr(builtins, "_", None), self._builtin_underscore)

    def test_result_follows_its_echo(self):
        result = self.run_code("a = 1\na + 1\n")
        flow = [kind if kind == "echo" else payload for kind, payload in self.timeline(result.run_id)]
        self.assertEqual(flow, ["echo", "echo", (STREAM_RESULT, "2\n")])

    def test_none_prints_nothing_and_keeps_underscore(self):
        self.session["_"] = "before"
        result = self.run_code("print\nNone\n")
        self.assertEqual(self.result_writes(result), [])
        self.assertEqual(self.session["_"], "before")

    def test_only_the_last_expression_prints(self):
        result = self.run_code("1 + 1\n2 + 2\nx = 3\n")
        self.assertEqual(self.result_writes(result), [])
        result = self.run_code("1 + 1\n2 + 2\n")
        self.assertEqual(self.result_writes(result), ["4\n"])

    def test_off_for_run_file_and_isolated(self):
        for request in ({"route": "run_file", "namespace": "isolated"}, {"namespace": "isolated"}):
            with self.subTest(**request):
                result = self.run_code("7 * 6\n", **request)
                self.assertEqual(result.outcome, OK)
                self.assertEqual(self.result_writes(result), [])
                self.assertNotIn("_", result.ns)
                self.assertNotIn("_", self.session)

    def test_multi_line_repr_is_one_write(self):
        # Only the first line of a repr carries "# Result: " (TDD §6.4); the ingest thread can mark the later lines
        # OLF_ResultCont only when they arrive in one write.
        code = ("class Two:\n"
                "    def __repr__(self):\n"
                "        return 'first\\nsecond\\nthird'\n"
                "Two()\n")
        result = self.run_code(code)
        self.assertEqual(result.outcome, OK)
        self.assertEqual(self.result_writes(result), ["first\nsecond\nthird\n"])

    def test_long_repr_is_sent_whole(self):
        # The ingest thread keeps 10,000 characters as the line and the rest as a blob (TDD §6.4).
        result = self.run_code("'y' * 50000\n")
        writes = self.result_writes(result)
        self.assertEqual(len(writes), 1)
        self.assertEqual(len(writes[0]), 50000 + 3)

    def test_display_function_is_removed_after_the_run(self):
        result = self.run_code("seen = '__dream_display__' in globals()\n5\n")
        self.assertTrue(self.session["seen"])
        self.assertNotIn("__dream_display__", self.session)
        self.assertEqual(result.outcome, OK)

    def test_raising_repr_points_at_the_users_line(self):
        text = "class Bad:\n    def __repr__(self):\n        raise RuntimeError('no repr')\nBad()\n"
        result = self.run_code(text, filename="<dream:Repr#0007>")
        self.assertEqual(result.outcome, ERROR)
        self.assertEqual(result.location, ("<dream:Repr#0007>", 3))
        self.assertIn('File "<dream:Repr#0007>", line 4, in <module>', result.errors)
        self.assertNotIn("autoprint", result.errors)

    def test_displayhook_of_single_mode_code_during_a_run(self):
        result = self.run_code("exec(compile('6 * 7', '<single>', 'single'))\nx = 1\n")
        self.assertEqual(self.result_writes(result), ["42\n"])
        self.assertEqual(self.session["_"], 42)
        self.assertIs(getattr(builtins, "_", None), self._builtin_underscore)


if __name__ == "__main__":
    unittest.main()
