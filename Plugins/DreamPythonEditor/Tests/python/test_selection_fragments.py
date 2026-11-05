# SPDX-License-Identifier: Apache-2.0
"""Exact selection runs (PO-06 F2, amends RUN-01): a selection fragment that starts mid-line runs alone, with buffer
columns for tracebacks, carets and SyntaxError positions (``first_column``), and import statements bind in the Session.

C++ cuts the selected characters (DreamRunSource::ExactSelection) and sends the fragment's UTF-16 start column; these
cases drive the runner with the same requests.
"""

import unittest

from dream_testing import ERROR, NOT_EXECUTED, OK, RunnerTestCase

FILENAME = "<dream:Frag#f2a0>"


def _buffer(*lines):
    return "\n".join(lines) + "\n"


class Fragments(RunnerTestCase):
    def run_fragment(self, whole, line, column, text, **request):
        """Runs ``text`` cut from 1-based buffer ``line`` at UTF-16 ``column`` (the request SDreamTab builds)."""
        request.setdefault("route", "selection")
        request.setdefault("filename", FILENAME)
        return self.run_code(text, first_line=line, first_column=column, linecache_text=whole, **request)

    def test_a_selected_expression_inside_a_line_prints_its_result(self):
        whole = _buffer("import math", "r = math.floor(2.5) + 10")
        self.assertEqual(self.run_code("import math\n", route="selection", linecache_text=whole).outcome, OK)
        result = self.run_fragment(whole, 2, 4, "math.floor(2.5)")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(result.results, "2\n")
        self.assertEqual(self.session["_"], 2)
        self.assertNotIn("r", self.session)            # the rest of the line did not run

    def test_the_module_name_of_an_import_line_prints_the_module(self):
        whole = _buffer("import math")
        self.run_code("import math\n", route="selection", linecache_text=whole)
        result = self.run_fragment(whole, 1, 7, "math")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertTrue(result.results.startswith("<module 'math'"), result.results)

    def test_selected_import_statements_bind_in_the_session(self):
        whole = _buffer("import math", "import json, textwrap as tw", "from os import path as osp")
        result = self.run_code(whole.rstrip("\n"), route="selection", first_line=1, linecache_text=whole)
        self.assertEqual(result.outcome, OK, result.errors)
        for name in ("math", "json", "tw", "osp"):
            self.assertIn(name, self.session, name)
        self.assertEqual(result.prepared["statements"], [[1, 1], [2, 2], [3, 3]])

    def test_a_fragment_that_raises_reports_buffer_columns(self):
        whole = _buffer("total = 0", "total = 1 + undefined_frag * 2")
        # "undefined_frag * 2" starts at column 12 of line 2.
        result = self.run_fragment(whole, 2, 12, "undefined_frag * 2")
        self.assertEqual(result.outcome, ERROR)
        self.assertEqual(result.location, (FILENAME, 2))
        self.assertIn('File "<dream:Frag#f2a0>", line 2, in <module>\n'
                      '    total = 1 + undefined_frag * 2\n'
                      '                ^^^^^^^^^^^^^^\n', result.errors)
        self.assertEqual(self.session.get("total"), None)

    def test_a_syntax_error_in_a_fragment_points_at_the_buffer_column(self):
        whole = _buffer("y = (1 + )")
        result = self.run_fragment(whole, 1, 4, "(1 + )")
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["lineno"], 1)
        self.assertEqual(result.error["text"], "y = (1 + )")
        self.assertEqual(result.error["offset"], whole.index(")") + 1)   # 1-based UTF-8 column of ')'

    def test_a_compile_error_in_a_fragment_points_at_the_buffer_column(self):
        whole = _buffer("if ready: return value")
        result = self.run_fragment(whole, 1, 10, "return value")
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["msg"], "'return' outside function")
        self.assertEqual(result.error["offset"], 11)
        self.assertEqual(result.error["end_offset"], 23)

    def test_columns_after_non_ascii_text_count_utf16_in_and_utf8_out(self):
        astral = "\U0001F600"                             # two UTF-16 units, four UTF-8 bytes
        line = f"s = 'é{astral}'; t = (2 + )"
        whole = _buffer(line)
        start_chars = line.index("(")
        start_units = start_chars + 1                     # the astral character counts twice in UTF-16
        result = self.run_fragment(whole, 1, start_units, line[start_chars:])
        self.assertEqual(result.outcome, NOT_EXECUTED)
        close = line.index(")")
        self.assertEqual(result.error["offset"], len(line[:close].encode("utf-8")) + 1)

    def test_a_multi_line_selection_from_mid_line_runs_from_its_start_character(self):
        whole = _buffer("skipped = 1; values = [1,", "    2, 3]", "after = 9")
        result = self.run_fragment(whole, 1, 13, "values = [1,\n    2, 3]")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(self.session["values"], [1, 2, 3])
        self.assertNotIn("skipped", self.session)
        self.assertNotIn("after", self.session)
        self.assertEqual(result.prepared["statements"], [[1, 2]])

    def test_a_fragment_starting_with_blanks_after_code_runs(self):
        whole = _buffer("a = 1;   b = 2")
        result = self.run_fragment(whole, 1, 6, "   b = 2")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(self.session["b"], 2)
        self.assertNotIn("a", self.session)

    def test_a_mid_line_start_inside_an_indented_body_runs_with_the_lines_after_it(self):
        # q1-m1: the fragment's first line takes its buffer line's indentation, so it lines up with the body after it.
        whole = _buffer("def body():", "    skip_q1 = 1; first_q1 = 2", "    second_q1 = first_q1 + 1", "    return 0")
        result = self.run_fragment(whole, 2, 17, "first_q1 = 2\n    second_q1 = first_q1 + 1")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(self.session["second_q1"], 3)
        self.assertNotIn("skip_q1", self.session)
        self.assertEqual(result.prepared["statements"], [[2, 2], [3, 3]])

    def test_a_mid_line_start_inside_an_indented_body_keeps_buffer_carets(self):
        whole = _buffer("for _ in [0]:", "    keep_q1 = 1; boom_q1 = 1 + missing_q1", "    after_q1 = 2")
        result = self.run_fragment(whole, 2, 17, "boom_q1 = 1 + missing_q1\n    after_q1 = 2")
        self.assertEqual(result.outcome, ERROR)
        self.assertIn('line 2, in <module>\n'
                      '    keep_q1 = 1; boom_q1 = 1 + missing_q1\n'
                      '                               ^^^^^^^^^^\n', result.errors)
        self.assertNotIn("after_q1", self.session)

    def test_a_syntax_error_after_a_mid_line_start_in_a_body_points_at_the_buffer_column(self):
        whole = _buffer("if True:", "    ok_q1 = 1; bad_q1 = (1 + )", "    tail_q1 = 2")
        result = self.run_fragment(whole, 2, 15, "bad_q1 = (1 + )\n    tail_q1 = 2")
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["lineno"], 2)
        self.assertEqual(result.error["offset"], whole.split("\n")[1].index(")") + 1)

    def test_blanks_after_the_code_before_a_mid_line_start_in_a_body(self):
        whole = _buffer("if True:", "    a_q1 = 1;   b_q1 = 2", "    c_q1 = b_q1 + 1")
        result = self.run_fragment(whole, 2, 13, "   b_q1 = 2\n    c_q1 = b_q1 + 1")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(self.session["c_q1"], 3)
        self.assertNotIn("a_q1", self.session)

    def test_without_the_whole_buffer_the_fragment_is_cached_at_its_column(self):
        result = self.run_code("1 / 0", route="selection", filename=FILENAME, first_line=3, first_column=6)
        self.assertEqual(result.outcome, ERROR)
        self.assertIn('line 3, in <module>\n    1 / 0\n', result.errors)

    def test_first_column_must_be_an_int(self):
        with self.assertRaises(TypeError):
            self.run_code("x = 1", first_column="3")


class NoSelectionUnchanged(RunnerTestCase):
    def test_a_request_without_first_column_is_unchanged(self):
        whole = _buffer("v = 1 +")
        result = self.run_code("v = 1 +", route="line", first_line=1, linecache_text=whole, filename=FILENAME)
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["offset"], 8)        # CPython: just past the trailing "+"


if __name__ == "__main__":
    unittest.main()
