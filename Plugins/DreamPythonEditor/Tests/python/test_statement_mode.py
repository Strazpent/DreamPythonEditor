# SPDX-License-Identifier: Apache-2.0
"""Statement mode against one exec (T-RUN-030, T-RUN-031; RUN-27, TDD §5.3.2, §5.3.7)."""

import contextlib
import inspect
import io
import linecache
import types
import unittest

import dream_testing
from dream_testing import OK, RunnerTestCase

from dream_editor._impl import namespaces

CORPUS = [
    "x = 1\ny = x + 1\n",
    "def f(a, b=2):\n    return a + b\nr = f(1)\n",
    "@staticmethod\ndef g():\n    return 5\nkind = type(g).__name__\n",
    "import functools\n@functools.lru_cache(maxsize=None)\ndef fib(n):\n    return n if n < 2 else fib(n - 1) + fib(n - 2)\n"
    "v = fib(20)\n",
    "class A:\n    x = 1\n    def m(self):\n        return self.x\na = A().m()\n",
    "counter = 0\ndef inc():\n    global counter\n    counter += 1\ninc()\ninc()\n",
    "squares = [i * i for i in range(10)]\nevens = {i for i in squares if i % 2 == 0}\n"
    "mapping = {i: str(i) for i in range(3)}\ngen = sum(x for x in range(5))\n",
    "try:\n    1 / 0\nexcept ZeroDivisionError as e:\n    msg = str(e)\nelse:\n    msg = 'none'\nfinally:\n"
    "    done = True\n",
    "import io\nwith io.StringIO() as s:\n    s.write('abc')\n    val = s.getvalue()\n",
    "point = (1, 2)\nmatch point:\n    case (0, 0):\n        kind = 'origin'\n    case (x, 0):\n        kind = 'x-axis'\n"
    "    case (x, y):\n        kind = f'{x},{y}'\n",
    "a = 1\nb = 2\ndel a\n",
    "from os.path import *\nj = join('a', 'b')\n",
    "if __name__ == '__main__':\n    ran = True\nelse:\n    ran = False\n",
    "x: int = 5\ny: 'str'\n",
    "import math\nprint(math.pi)\nprint('a', 'b', sep='-')\n",
    "for i in range(3):\n    print(i)\nelse:\n    print('done')\n",
    "while False:\n    pass\nn = 0\nwhile n < 3:\n    n += 1\n",
    "lam = lambda z: z * 2\nres = lam(4)\n",
    "a, *rest = [1, 2, 3]\n(b, c) = rest\n",
    "x = 10\ndef outer():\n    y = 1\n    def inner():\n        nonlocal y\n        y += 1\n        return y\n"
    "    return inner()\nz = outer()\n",
    "class B(dict):\n    def __missing__(self, k):\n        return k\nb = B()['q']\n",
    "print('one'); print('two')\nq = 3; r = q * 2\n",
    "import sys as _s\nv = _s.maxsize > 0\n",
    "async def co():\n    return 1\nis_co = inspect_name = co.__name__\n",
    "x = 1\nx += 1\nx *= 3\n",
    "s = 'a' 'b'\nt = f'{s!r:>5}'\n",
    "def deco(f):\n    f.tag = 'x'\n    return f\n@deco\nclass K:\n    pass\ntag = K.tag\n",
    "import collections\nP = collections.namedtuple('P', 'a b')\np = P(1, 2)\n",
    "data = {'k': [1, 2, {'z': None}]}\nflat = str(data)\n",
    "x = 0\nfor i in range(5):\n    if i == 3:\n        break\n    x += i\n",
    "def gen():\n    yield 1\n    yield 2\nvals = list(gen())\n",
    "assert True\nok = 1\n",
    "import dataclasses\n@dataclasses.dataclass\nclass D:\n    a: int\n    b: str = 'x'\nd = D(1)\n",
    "def kw(*args, **kwargs):\n    return args, kwargs\nr = kw(1, k=2)\n",
    "x = [1,\n     2,\n     3]\ny = len(x)\n",
    "text = '''line1\nline2'''\nn = text.count('\\n')\n",
    "print(*range(3))\nprint()\n",
    "global g\ng = 1\n",
    "import itertools\nc = list(itertools.islice(itertools.count(), 3))\n",
    "'''Module doc.'''\nx = __doc__\n",
    "def documented():\n    '''Doc.'''\n    return documented.__doc__\nd = documented()\n",
    "class E(Exception):\n    pass\ntry:\n    raise E('boom')\nexcept E as err:\n    caught = repr(err)\n",
    "nums = sorted([3, 1, 2], key=lambda v: -v)\nprint(nums)\n",
]


def _corpus_200():
    buffers = list(CORPUS)
    count = len(CORPUS)
    step = 0
    while len(buffers) < 200:
        first = CORPUS[step % count]
        second = CORPUS[(step * 7 + 3) % count]
        buffers.append(first + second)
        step += 1
    return buffers


def _describe(value):
    if isinstance(value, types.FunctionType):
        try:
            source = inspect.getsource(value)
        except (OSError, TypeError):
            source = None
        return ("function", value.__qualname__, value.__code__.co_firstlineno, repr(value.__annotations__), source)
    if isinstance(value, type):
        return ("class", value.__qualname__, sorted(vars(value)))
    if isinstance(value, types.ModuleType):
        return ("module", value.__name__)
    try:
        text = repr(value)
    except Exception:  # noqa: BLE001
        text = "<repr failed>"
    if " at 0x" in text:
        text = type(value).__qualname__
    return text


def _snapshot(ns):
    skip = {"__builtins__", "__warningregistry__"}
    return {key: _describe(value) for key, value in ns.items() if key not in skip}


class StatementModeEquivalence(RunnerTestCase):
    """T-RUN-030: equal namespaces, print order, linecache and getsource; one run_statement per statement."""

    def _one_exec(self, text, filename):
        ns = {"__name__": "__main__", "__doc__": None}
        out = io.StringIO()
        code = compile(text, filename, "exec", dont_inherit=True)
        with contextlib.redirect_stdout(out):
            exec(code, ns)
        return ns, out.getvalue()

    def test_corpus_of_200_buffers(self):
        for index, text in enumerate(_corpus_200()):
            with self.subTest(buffer=index):
                filename = f"<dream:Corpus-{index}#c{index:03x}>"
                session = dream_testing.new_session()
                namespaces.capture_session(session, 10_000 + index)
                result = self.run_code(text, filename=filename, autoprint=False)
                self.assertEqual(result.outcome, OK, result.errors)
                statements = result.prepared["statements"]
                self.assertEqual(self.started_indices(result), list(range(len(statements))))
                self.assertEqual(self.echo_indices(result), list(range(len(statements))))
                # The one-exec reference reads the same linecache entry the runner registered.
                entry = linecache.cache[filename]
                reference, printed = self._one_exec(text, filename)
                self.assertIs(linecache.cache[filename], entry)
                self.assertEqual(result.stdout, printed)
                self.assertEqual(_snapshot(session), _snapshot(reference))

    def test_echo_before_each_statements_output(self):
        result = self.run_code("print(1)\nprint(2)\nx = 3\nprint(x)\n", autoprint=False)
        flow = []
        for kind, payload in self.timeline(result.run_id):
            if kind == "echo":
                flow.append(f"echo{payload}")
            elif payload[1].strip():
                flow.append(payload[1])
        self.assertEqual(flow, ["echo0", "1", "echo1", "2", "echo2", "echo3", "3"])

    def test_annotations_exist_before_the_first_annotated_statement(self):
        text = "print('__annotations__' in globals())\nx: int = 1\n"
        result = self.run_code(text, autoprint=False)
        reference, printed = self._one_exec(text, "<ref>")
        self.assertEqual(printed, "True\n")
        self.assertEqual(result.stdout, "True\n")
        self.assertEqual(self.session["__annotations__"], reference["__annotations__"])

    def test_annotation_inside_a_compound_statement(self):
        text = "seen = '__annotations__' in globals()\nif True:\n    z: int = 3\n"
        self.run_code(text, autoprint=False)
        reference, _ = self._one_exec(text, "<ref>")
        self.assertTrue(reference["seen"])
        self.assertTrue(self.session["seen"])

    def test_no_annotations_without_an_annotated_module_statement(self):
        self.run_code("def f(a: int) -> int:\n    b: int = a\n    return b\n", autoprint=False)
        self.assertNotIn("__annotations__", self.session)


class StatementModeSemantics(RunnerTestCase):
    """T-RUN-031: __future__, bare strings, a raising statement 2."""

    def test_future_annotations_match_one_exec(self):
        text = ("from __future__ import annotations\n"
                "x: int = 1\n"
                "def f(a: Undefined) -> int:\n"
                "    return a\n"
                "y = f.__annotations__\n")
        result = self.run_code(text)
        self.assertEqual(result.outcome, OK, result.errors)
        reference = {"__name__": "__main__"}
        exec(compile(text, "<ref>", "exec", dont_inherit=True), reference)
        self.assertEqual(self.session["y"], reference["y"])
        self.assertEqual(self.session["y"], {"a": "Undefined", "return": "int"})
        self.assertEqual(self.session["__annotations__"], reference["__annotations__"])

    def test_future_import_after_a_leading_docstring(self):
        text = '"""Doc."""\nfrom __future__ import annotations\ndef f() -> Missing:\n    pass\nr = f.__annotations__\n'
        result = self.run_code(text)
        self.assertEqual(result.outcome, OK, result.errors)
        self.assertEqual(self.session["r"], {"return": "Missing"})
        self.assertEqual(self.session["__doc__"], "Doc.")

    def test_mid_buffer_string_is_echoed_not_executed(self):
        self.session["__doc__"] = "session doc"
        result = self.run_code('x = 1\n"""mid doc"""\ny = 2\n')
        self.assertEqual(result.outcome, OK)
        self.assertEqual(self.echo_indices(result), [0, 1, 2])
        self.assertEqual(self.session["__doc__"], "session doc")
        self.assertEqual((self.session["x"], self.session["y"]), (1, 2))

    def test_leading_docstring_sets_doc(self):
        result = self.run_code('"""lead doc"""\nx = 1\n"""mid doc"""\ny = 2\n')
        self.assertEqual(result.outcome, OK)
        self.assertEqual(self.session["__doc__"], "lead doc")

    def test_trailing_string_prints_a_result(self):
        result = self.run_code("x = 1\n'tail'\n")
        self.assertEqual(result.results, "'tail'\n")
        self.assertEqual(self.session["_"], "tail")

    def test_statement_two_raises_at_line_two(self):
        self.session["ran"] = []
        text = "ran.append(1)\nraise ValueError('two')\nran.append(3)\n"
        result = self.run_code(text, filename="<dream:Three#b001>")
        self.assertEqual(result.outcome, dream_testing.ERROR)
        self.assertEqual(result.detail, "ValueError: two")
        self.assertEqual(result.location, ("<dream:Three#b001>", 2))
        self.assertIn('File "<dream:Three#b001>", line 2, in <module>', result.errors)
        self.assertEqual(self.session["ran"], [1])
        self.assertEqual(self.started_indices(result), [0, 1])
        self.assertEqual(self.dream._started_returned[result.run_id], [2, 1])

    def test_statement_ranges_include_decorators(self):
        text = "import functools\n\n@functools.wraps(len)\n@staticmethod\ndef f():\n    pass\nx = [\n  1]\n"
        result = self.run_code(text, first_line=10)
        self.assertEqual(result.prepared["statements"], [[10, 10], [12, 15], [16, 17]])

    def test_one_line_holding_two_statements(self):
        result = self.run_code("import os; cwd = os.getcwd()\n")
        self.assertEqual(result.prepared["statements"], [[1, 1], [1, 1]])
        self.assertEqual(self.echo_indices(result), [0, 1])

    def test_empty_buffer_runs_nothing(self):
        result = self.run_code("# only a comment\n\n")
        self.assertEqual(result.outcome, OK)
        self.assertEqual(result.prepared["statements"], [])
        self.assertEqual(self.started_indices(result), [])

    def test_isolated_and_run_file_do_not_use_statement_mode(self):
        for request in ({"namespace": "isolated"}, {"route": "run_file", "namespace": "isolated"}):
            with self.subTest(**request):
                result = self.run_code("x = 1\ny = 2\n", **request)
                self.assertIsNone(result.prepared["statements"])
                self.assertEqual(self.echo_indices(result), [])
                self.assertEqual(result.outcome, OK)

    def test_statement_mode_routes(self):
        for route in ("selection", "line", "buffer", "repl", "console"):
            with self.subTest(route=route):
                result = self.run_code("a = 1\nb = 2\n", route=route,
                                       filename=f"<dream:{route}#1>")
                self.assertEqual(result.prepared["statements"], [[1, 1], [2, 2]])


if __name__ == "__main__":
    unittest.main()
