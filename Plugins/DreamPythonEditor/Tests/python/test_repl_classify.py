# SPDX-License-Identifier: Apache-2.0
"""REPL completeness (T-REPL-001 tier-2 part; TDD §6.5 classify_input)."""

import io
import unittest
from contextlib import redirect_stdout

import dream_testing  # noqa: F401 - puts Content/Python on sys.path

from dream_editor._impl import runner


class ClassifyInput(unittest.TestCase):
    def check(self, source, indented, expected):
        with self.subTest(source=source, indented=indented):
            self.assertEqual(runner.classify_input(source, indented), expected)

    def test_indentation_rule(self):
        # On 3.11.8 "exec" mode calls this block complete; the blank Enter rule makes it wait (REPL-01).
        self.check("for i in range(3):\n    print(i)", True, "incomplete")
        self.check("for i in range(3):\n    print(i)\n", True, "complete")
        self.check("def f():\n pass", True, "incomplete")

    def test_unindented_last_logical_line(self):
        self.check("x = [\n    1,\n    2]", False, "complete")
        self.check("x = 1\ny = 2", False, "complete")

    def test_incomplete_and_invalid(self):
        self.check("if x:", False, "incomplete")
        self.check("x = (", False, "incomplete")
        self.check('s = """abc', False, "incomplete")
        self.check("x = )", False, "invalid")
        self.check("else:", False, "invalid")
        self.check("1 +* 2", False, "invalid")

    def test_comments_and_blank(self):
        self.check("# just a comment", False, "complete")
        self.check("", False, "complete")

    def test_never_executes(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.check("print('side effect')", False, "complete")
        self.assertEqual(out.getvalue(), "")

    def test_type_check(self):
        with self.assertRaises(TypeError):
            runner.classify_input(b"x", False)


if __name__ == "__main__":
    unittest.main()
