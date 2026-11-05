# SPDX-License-Identifier: Apache-2.0
"""Compile rules (TDD §5.3.2; T-RUN-001..003, T-RUN-005, T-RUN-006, T-RUN-032; RUN-06, RUN-16, RT-14)."""

import inspect
import linecache
import os
import tempfile
import unittest

from dream_testing import ERROR, NOT_EXECUTED, OK, RunnerTestCase

from dream_editor._impl import runner

E_ACUTE = "\u00e9"


def _buffer(lines_before, *code_lines):
    """A whole buffer: ``lines_before`` filler lines, then the code lines; returns (buffer text, first code line)."""
    filler = [f"# filler {n}" for n in range(1, lines_before + 1)]
    return "\n".join(filler + list(code_lines)) + "\n", lines_before + 1


class LineNumbers(RunnerTestCase):
    def test_column0_selection_reports_the_buffer_line(self):
        # T-RUN-001: lines 40-45 selected, raising on 43.
        code = ["a = 1", "b = 2", "c = 3", "d = undefined_name", "e = 5", "f = 6"]
        whole, first = _buffer(39, *code)
        result = self.run_code("\n".join(code) + "\n", first_line=first, linecache_text=whole,
                               filename="<dream:Tab Name#a41f>", route="selection")
        self.assertEqual(first, 40)
        self.assertEqual(result.outcome, ERROR)
        self.assertEqual(result.location, ("<dream:Tab Name#a41f>", 43))
        self.assertIn('File "<dream:Tab Name#a41f>", line 43, in <module>', result.errors)
        self.assertIn("    d = undefined_name\n", result.errors)
        self.assertIn("NameError: name 'undefined_name' is not defined", result.errors)

    def test_indented_selection_keeps_carets(self):
        # T-RUN-002: RT-14 wrapper; 3.11 carets stay exact because the indentation is kept.
        self.session["x"] = {}
        whole, first = _buffer(11, "def outer():", '    y = x["k"] + 1')
        result = self.run_code('    y = x["k"] + 1\n', first_line=first + 1, linecache_text=whole,
                               filename="<dream:Tab#0002>", route="selection")
        self.assertEqual(result.outcome, ERROR)
        self.assertEqual(result.location, ("<dream:Tab#0002>", 13))
        self.assertIn('File "<dream:Tab#0002>", line 13, in <module>\n    y = x["k"] + 1\n        ~^^^^^\n',
                      result.errors)

    def test_indented_selection_with_a_triple_quoted_string(self):
        # T-RUN-003: lines of the string sit at a lower indentation than the first line.
        text = '    s = """a\nb\n  c"""\n    n = len(s)\n'
        result = self.run_code(text, first_line=20, route="selection")
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(self.session["s"], "a\nb\n  c")
        self.assertEqual(result.prepared["statements"], [[20, 22], [23, 23]])

    def test_indented_selection_then_a_column0_line(self):
        result = self.run_code("    a = 1\n    b = 2\nc = a + b\n", first_line=5)
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(self.session["c"], 3)
        self.assertEqual(result.prepared["statements"], [[5, 5], [6, 6], [7, 7]])

    def test_indented_selection_after_a_blank_and_a_comment(self):
        result = self.run_code("\n# note\n    z = 7\n", first_line=3)
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(result.prepared["statements"], [[5, 5]])

    def test_nested_function_frames_in_the_traceback(self):
        text = "def inner():\n    return 1 / 0\ndef outer():\n    return inner()\nouter()\n"
        result = self.run_code(text, filename="<dream:Frames#0003>", first_line=100)
        self.assertEqual(result.location, ("<dream:Frames#0003>", 101))
        frames = [line.strip() for line in result.errors.splitlines() if line.strip().startswith("File ")]
        self.assertEqual(frames, ['File "<dream:Frames#0003>", line 104, in <module>',
                                  'File "<dream:Frames#0003>", line 103, in outer',
                                  'File "<dream:Frames#0003>", line 101, in inner'])

    def test_no_runner_frames_in_tracebacks(self):
        result = self.run_code("def f():\n    raise RuntimeError('x')\nf()\n")
        self.assertNotIn("_impl", result.errors)
        self.assertNotIn("runner.py", result.errors)
        self.assertTrue(result.errors.startswith("Traceback (most recent call last):\n"))

    def test_chained_exception_tracebacks_are_filtered_too(self):
        text = "try:\n    {}['k']\nexcept KeyError as e:\n    raise ValueError('wrapped') from e\n"
        result = self.run_code(text)
        self.assertIn("The above exception was the direct cause of the following exception", result.errors)
        self.assertNotIn("_impl", result.errors)


class CompileErrors(RunnerTestCase):
    def setUp(self):
        super().setUp()
        self.marker = []
        self.session["marker"] = self.marker.append

    def test_misplaced_future_import_runs_nothing(self):
        # T-RUN-032: ast.parse accepts it; only the whole-module compile raises.
        result = self.run_code("marker(1)\nfrom __future__ import annotations\n", first_line=7)
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["lineno"], 8)
        self.assertEqual(result.error["msg"], "from __future__ imports must occur at the beginning of the file")
        self.assertEqual(self.marker, [])
        self.assertEqual(result.events, [])
        self.assertEqual(result.lines, [])

    def test_return_in_statement_three_runs_nothing(self):
        result = self.run_code("marker(1)\nx = 2\nreturn x\n")
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["lineno"], 3)
        self.assertEqual(result.error["msg"], "'return' outside function")
        self.assertEqual(result.error["text"], "return x")
        self.assertEqual(self.marker, [])
        self.assertEqual(result.events, [])

    def test_the_same_text_as_a_repl_submission(self):
        result = self.run_code("marker(1)\nreturn 2\n", route="repl", filename="<dream:REPL#9>")
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["lineno"], 2)
        self.assertEqual(self.marker, [])
        self.assertEqual(result.events, [])

    def test_unclosed_bracket_reports_the_bracket(self):
        # RUN-16: "'(' was never closed" points at the opening bracket.
        result = self.run_code("x = 1\ny = foo(1,\n\n", first_line=40)
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["msg"], "'(' was never closed")
        self.assertEqual((result.error["lineno"], result.error["offset"]), (41, 8))

    def test_syntax_error_in_an_indented_selection(self):
        result = self.run_code("    a = 1\n    b = = 2\n", first_line=10)
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["lineno"], 11)
        self.assertEqual(result.error["text"], "    b = = 2")
        self.assertEqual(result.error["offset"], 9)

    def test_unmatched_else_in_an_indented_selection(self):
        result = self.run_code("    x = 1\nelse:\n    y = 2\n", first_line=3)
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["lineno"], 4)
        self.assertEqual(result.error["msg"], "invalid syntax")

    def test_indentation_error_type(self):
        result = self.run_code("if True:\nx = 1\n")
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["type"], "IndentationError")
        self.assertEqual(result.error["lineno"], 2)

    def test_offsets_are_utf8_columns(self):
        # Parser errors come in characters; FDreamSyntaxError takes UTF-8 columns.
        result = self.run_code(E_ACUTE + " = (\n")
        self.assertEqual(result.error["offset"], 6)
        self.assertEqual(result.error["text"], E_ACUTE + " = (")
        # Compiler errors (from the AST) already come in UTF-8 bytes.
        result = self.run_code("def f():\n    pass\n" + E_ACUTE + E_ACUTE + " = 2; return 5\n")
        self.assertEqual(result.error["msg"], "'return' outside function")
        self.assertEqual(result.error["offset"], 11)     # 'return' at character 8, byte 10

    def test_null_byte(self):
        result = self.run_code("x = 1\0\n")
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["lineno"], 1)

    def test_per_statement_compiles_happen_only_after_the_whole_module_passes(self):
        result = self.run_code("x = 1\ndef f():\n    nonlocal q\n")
        self.assertEqual(result.outcome, NOT_EXECUTED)
        self.assertEqual(result.error["lineno"], 3)


class Linecache(RunnerTestCase):
    def test_untitled_entry_survives_checkcache_and_getsource(self):
        # T-RUN-005
        text = "def f():\n    return 41 + 1\n"
        result = self.run_code(text, filename="<dream:Untitled-3#a41f>")
        self.assertEqual(result.outcome, OK)
        linecache.checkcache()
        size, mtime, lines, name = linecache.cache["<dream:Untitled-3#a41f>"]
        self.assertIsNone(mtime)
        self.assertEqual(lines, ["def f():\n", "    return 41 + 1\n"])
        self.assertEqual(inspect.getsource(self.session["f"]), text)

    def test_whole_buffer_is_registered_for_a_selection(self):
        whole = "a = 1\ndef g():\n    return 2\nb = 3\n"
        self.run_code("def g():\n    return 2\n", first_line=2, linecache_text=whole, filename="<dream:Sel#0004>")
        self.assertEqual("".join(linecache.cache["<dream:Sel#0004>"][2]), whole)
        self.assertEqual(inspect.getsource(self.session["g"]), "def g():\n    return 2\n")

    def test_dirty_saved_file_shows_the_buffer_text(self):
        # T-RUN-006
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "tool.py")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("x = 1\n")
            buffer_text = "x = 1\nraise KeyError('from the buffer')\n"
            result = self.run_code(buffer_text, filename=path, file_path=path, dirty=True)
            self.assertIn("    raise KeyError('from the buffer')\n", result.errors)
            entry = linecache.cache[path]
            self.assertIsNone(entry[1])
            runner.on_buffer_saved(path)
            self.assertNotIn(path, linecache.cache)

    def test_on_save_leaves_an_entry_that_is_not_ours(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "tool.py")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("y = 2\n")
            self.run_code("y = 2\n", filename=path, file_path=path, dirty=True)
            replacement = (6, None, ["y = 2\n"], path)
            linecache.cache[path] = replacement
            runner.on_buffer_saved(path)
            self.assertIs(linecache.cache[path], replacement)
            del linecache.cache[path]

    def test_clean_saved_file_is_read_from_disk(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "clean.py")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("value = 5\n")
            result = self.run_code("value = 5\n", filename=path, file_path=path, dirty=False)
            self.assertEqual(result.outcome, OK)
            self.assertNotIn(path, runner._linecache_ours)

    def test_repl_inputs_keep_the_last_500(self):
        runner._forget_pseudo_entries()
        names = [f"<dream:REPL#{n}>" for n in range(1, 503)]
        for serial, name in enumerate(names, start=900_000):
            runner.prepare(serial, {"text": "1\n", "filename": name, "route": "repl"})
            runner.discard(serial)
        self.assertNotIn(names[0], linecache.cache)
        self.assertNotIn(names[1], linecache.cache)
        self.assertIn(names[2], linecache.cache)
        self.assertIn(names[-1], linecache.cache)

    def test_crlf_text_counts_lines_like_the_compiler(self):
        result = self.run_code("a = 1\r\nb = 2\r\nraise ValueError()\r\n", filename="<dream:Crlf#0005>")
        self.assertEqual(result.location, ("<dream:Crlf#0005>", 3))
        self.assertEqual(linecache.cache["<dream:Crlf#0005>"][2][2], "raise ValueError()\n")


if __name__ == "__main__":
    unittest.main()
