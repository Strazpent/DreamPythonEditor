# SPDX-License-Identifier: Apache-2.0
"""The bdb debugger engine under the null ``_dream`` (TDD §8.11 tier-2 rows; DBG-01..DBG-06; debug group, PO-06 F5).

The null ``_dream.pause`` calls a test handler in place of the Paused window: it records each stop, may send DAP-shaped
requests through ``debugger.handle_request`` exactly as the window does from its modal loop, and returns the resume
command.
"""

import json
import os
import sys
import tempfile
import threading
import unittest

import dream_testing
from dream_testing import CANCELLED, ERROR, OK, RunnerTestCase

from dream_editor._impl import debugger
from dream_editor._impl import native

FILENAME = "<dream:Debug#d0d0>"


def request(command, **arguments):
    return json.loads(debugger.handle_request(json.dumps({"seq": 1, "command": command, "arguments": arguments})))


def locals_of(frame_id=1):
    scopes = request("scopes", frameId=frame_id)["body"]["scopes"]
    ref = next(scope["variablesReference"] for scope in scopes if scope["name"] == "Locals")
    return {node["name"]: node for node in request("variables", variablesReference=ref)["body"]["variables"]}


class Script:
    """A pause handler that answers each stop with the next command and records what it saw."""

    def __init__(self, test, commands, inspect=None):
        self.test = test
        self.commands = list(commands)
        self.inspect = inspect
        self.stops = []

    def __call__(self, run_id, stop):
        self.test.assertIsNotNone(debugger.active_session(), "handle_request needs the active session")
        self.stops.append((stop["reason"], stop["line"], stop["stackFrames"][0]["name"]))
        if self.inspect is not None:
            self.inspect(stop)
        return (self.commands.pop(0) if self.commands else "continue", "{}")


class DebuggerTestCase(RunnerTestCase):
    def debug_run(self, text, breakpoints=(), script=None, filename=FILENAME, **request_extra):
        if script is not None:
            self.dream.test_set_pause_handler(script)
        config = {"breakpoints": [{"id": index + 1, "source_key": filename, "line": line}
                                  for index, line in enumerate(breakpoints)],
                  "break_on_uncaught": True, "just_my_code": True}
        request_extra.setdefault("linecache_text", text)
        return self.run_code(text, filename=filename, debug=True, debug_config=json.dumps(config), **request_extra)


LOOP = (
    "total = 0\n"                    # 1
    "for i in range(10):\n"          # 2
    "    total += i\n"               # 3
    "done = total\n"                 # 4
)

CALLS = (
    "def helper(x):\n"               # 1
    "    y = x * 2\n"                # 2
    "    return y\n"                 # 3
    "a = 1\n"                        # 4
    "b = helper(a)\n"                # 5
    "c = b + 1\n"                    # 6
)


class Breakpoints(DebuggerTestCase):
    def test_pauses_at_a_breakpoint_and_reads_locals(self):
        seen = {}

        def inspect(stop):
            if not seen:
                variables = locals_of()
                seen["i"] = variables["i"]["value"]
                seen["total"] = variables["total"]["value"]

        script = Script(self, ["continue"] * 20, inspect)
        result = self.debug_run(LOOP, breakpoints=[3], script=script)
        self.assertEqual(result.outcome, OK, result.text())
        self.assertEqual([stop[1] for stop in script.stops], [3] * 10)
        self.assertEqual(seen, {"i": "0", "total": "0"})
        self.assertEqual(result.ns["done"], 45)

    def test_breakpoint_on_a_blank_line_moves_to_the_next_code_line(self):
        text = "a = 1\n\n# note\nb = 2\n"
        script = Script(self, ["continue"])
        result = self.debug_run(text, breakpoints=[2], script=script)
        self.assertEqual(result.outcome, OK)
        self.assertEqual([stop[1] for stop in script.stops], [4])

    def test_breakpoint_past_five_lines_of_no_code_is_unverified(self):
        table = debugger.BreakpointTable()
        import linecache
        linecache.cache["<dream:Verify#1>"] = (0, None, ["a = 1\n"] + ["\n"] * 8 + ["b = 2\n"], "<dream:Verify#1>")
        try:
            results = table.set_source("<dream:Verify#1>", [{"id": 1, "line": 2}, {"id": 2, "line": 6}])
        finally:
            linecache.cache.pop("<dream:Verify#1>", None)
        self.assertEqual([(r["verified"], r["line"]) for r in results], [(False, 2), (True, 10)])
        self.assertEqual(results[0]["message"], "No code on this line")

    def test_condition_pauses_once(self):
        # T-DBG-003: i == 7 in a 0-9 loop.
        config = {"breakpoints": [{"id": 1, "source_key": FILENAME, "line": 3, "condition": "i == 7"}]}
        seen = []
        script = Script(self, ["continue"], lambda stop: seen.append(locals_of()["i"]["value"]))
        self.dream.test_set_pause_handler(script)
        result = self.run_code(LOOP, filename=FILENAME, debug=True, debug_config=json.dumps(config), linecache_text=LOOP)
        self.assertEqual(result.outcome, OK)
        self.assertEqual(seen, ["7"])

    def test_breakpoint_in_one_function_traces_only_that_function(self):
        # T-DBG-001: the call filter gives other code of the same file no local tracer.
        text = (
            "import sys\n"                         # 1
            "seen = []\n"                          # 2
            "def f():\n"                           # 3
            "    return 1\n"                       # 4
            "def g():\n"                           # 5
            "    seen.append(sys._getframe().f_trace)\n"   # 6
            "    return 2\n"                       # 7
            "f()\n"                                # 8
            "g()\n"                                # 9
        )
        script = Script(self, ["continue"])
        result = self.debug_run(text, breakpoints=[4], script=script)
        self.assertEqual(result.outcome, OK, result.text())
        self.assertEqual([stop[1] for stop in script.stops], [4])
        self.assertEqual(result.ns["seen"], [None])

    def test_imported_module_breakpoint_pauses(self):
        # DBG-02: a breakpoint in a module the run imports, keyed by its real path.
        folder = tempfile.mkdtemp(prefix="dream_dbg_")
        path = os.path.join(folder, "dream_dbg_tool.py")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("def work(n):\n    m = n + 1\n    return m\n")
        sys.path.insert(0, folder)
        self.addCleanup(lambda: sys.path.remove(folder))
        self.addCleanup(lambda: sys.modules.pop("dream_dbg_tool", None))
        script = Script(self, ["continue"])
        self.dream.test_set_pause_handler(script)
        config = {"breakpoints": [{"id": 1, "source_key": path.replace("\\", "/"), "line": 2}]}
        result = self.run_code("import dream_dbg_tool\nr = dream_dbg_tool.work(4)\n", filename=FILENAME, debug=True,
                               debug_config=json.dumps(config))
        self.assertEqual(result.outcome, OK, result.text())
        self.assertEqual(script.stops, [("breakpoint", 2, "work")])


class Stepping(DebuggerTestCase):
    def test_step_over_into_and_out(self):
        # T-DBG-011: golden stop sequence; no stop in runner frames.
        script = Script(self, ["stepIn", "next", "stepOut", "next", "continue"])
        result = self.debug_run(CALLS, breakpoints=[5], script=script)
        self.assertEqual(result.outcome, OK, result.text())
        self.assertEqual(script.stops, [
            ("breakpoint", 5, "<module>"),
            ("step", 2, "helper"),
            ("step", 3, "helper"),
            ("step", 6, "<module>"),
        ])
        self.assertEqual(result.ns["c"], 3)

    def test_step_over_crosses_statement_boundaries(self):
        # RUN-27 / T-DBG-058: F10 from statement 1 stops on the next statement's first line.
        script = Script(self, ["next", "next", "next", "continue"])
        result = self.debug_run(LOOP, breakpoints=[1], script=script)
        self.assertEqual(result.outcome, OK)
        self.assertEqual([stop[1] for stop in script.stops], [1, 2, 3, 2])
        for _reason, _line, name in script.stops:
            self.assertEqual(name, "<module>")

    def test_step_out_of_the_outermost_frame_runs_to_the_end(self):
        script = Script(self, ["stepOut"])
        result = self.debug_run(CALLS, breakpoints=[4], script=script)
        self.assertEqual(result.outcome, OK)
        self.assertEqual(len(script.stops), 1)
        self.assertEqual(result.ns["c"], 3)

    def test_stack_hides_dream_frames(self):
        frames = []
        script = Script(self, ["continue"], lambda stop: frames.extend(request("stackTrace")["body"]["stackFrames"]))
        self.debug_run(CALLS, breakpoints=[2], script=script)
        self.assertEqual([frame["name"] for frame in frames], ["helper", "<module>"])
        for frame in frames:
            self.assertEqual(frame["source"], FILENAME)


class StopAndExceptions(DebuggerTestCase):
    def test_stop_while_paused_raises_through_finally(self):
        # T-DBG-013.
        text = "flag = []\ntry:\n    x = 1\n    y = 2\nfinally:\n    flag.append('finally')\n"
        script = Script(self, ["terminate"])
        result = self.debug_run(text, breakpoints=[3], script=script)
        self.assertEqual(result.outcome, CANCELLED, result.text())
        self.assertEqual(result.ns["flag"], ["finally"])
        self.assertNotIn("y", result.ns)

    def test_uncaught_exception_pauses_at_the_raise_line(self):
        # T-DBG-009: reason exception, after user_code_done, steps disabled, Continue ends the run as error.
        text = "data = {}\ndef read():\n    return data['missing']\nvalue = read()\n"
        seen = {}

        def inspect(stop):
            seen["canStep"] = stop["canStep"]
            seen["description"] = stop["description"]
            seen["events"] = [kind for kind, _rid, _payload in self.dream.test_events if kind.startswith("user_code")]
            scopes = request("scopes", frameId=1)["body"]["scopes"]
            seen["scopes"] = [scope["name"] for scope in scopes]

        script = Script(self, ["continue"], inspect)
        result = self.debug_run(text, script=script)
        self.assertEqual(result.outcome, ERROR)
        self.assertEqual(script.stops, [("exception", 3, "read")])
        self.assertFalse(seen["canStep"])
        self.assertTrue(seen["description"].startswith("KeyError"))
        self.assertEqual(seen["events"][-1], "user_code_done")
        self.assertIn("Special", seen["scopes"])

    def test_stop_at_the_exception_ends_cancelled(self):
        script = Script(self, ["terminate"])
        result = self.debug_run("raise ValueError('x')\n", script=script)
        self.assertEqual(result.outcome, CANCELLED)

    def test_system_exit_zero_and_cancel_never_stop(self):
        # T-DBG-010.
        script = Script(self, ["continue"])
        result = self.debug_run("raise SystemExit(0)\n", script=script)
        self.assertEqual(result.outcome, OK)
        self.assertEqual(script.stops, [])

    def test_breakpoint_builtin_pauses_in_a_debug_run(self):
        # T-DBG-008 (first half).
        script = Script(self, ["continue"])
        result = self.debug_run("a = 1\nbreakpoint()\nb = 2\n", script=script)
        self.assertEqual(result.outcome, OK)
        self.assertEqual(script.stops, [("breakpoint()", 3, "<module>")])

    def test_breakpoint_builtin_is_a_notice_in_a_normal_run(self):
        # T-DBG-008 (second half).
        result = self.run_code("breakpoint()\nbreakpoint()\nb = 2\n")
        self.assertEqual(result.outcome, OK)
        notices = result.text(native.STREAM_DEBUGGER)
        self.assertEqual(notices.count("breakpoint() ignored"), 1)

    def test_refused_pause_writes_one_notice_and_continues(self):
        self.dream.test_set_pause_refusal(debugger.REFUSAL_INSIDE_PUMP)
        script = Script(self, [])
        result = self.debug_run(LOOP, breakpoints=[3], script=script)
        self.assertEqual(result.outcome, OK)
        self.assertEqual(script.stops, [])
        text = result.text(native.STREAM_DEBUGGER)
        self.assertEqual(text.count("skipped: inside an editor update during the run"), 1, text)


class ZeroCost(DebuggerTestCase):
    def test_no_trace_in_a_normal_run(self):
        # T-DBG-002, DBG-06.
        result = self.run_code("import sys\ntrace = sys.gettrace()\n")
        self.assertIsNone(result.ns["trace"])

    def test_no_trace_in_a_debug_run_without_breakpoints(self):
        result = self.debug_run("import sys\ntrace = sys.gettrace()\n")
        self.assertIsNone(result.ns["trace"])

    def test_trace_removed_after_a_debug_run(self):
        script = Script(self, ["continue"])
        self.debug_run(LOOP, breakpoints=[3], script=script)
        self.assertIsNone(sys.gettrace())
        self.assertIsNone(debugger.active_session())

    def test_trace_stays_while_a_breakpoint_remains(self):
        # Continue keeps the call filter installed while a verified breakpoint is left, so later hits still pause.
        text = "import sys\na = 1\nb = 2\ntrace = sys.gettrace()\n"
        script = Script(self, ["continue"])
        result = self.debug_run(text, script=script, breakpoints=[2])
        self.assertEqual(len(script.stops), 1)
        self.assertIsNotNone(result.ns["trace"])

    def test_continue_after_breakpoint_builtin_runs_untraced(self):
        # With no breakpoint verified, Continue after breakpoint() removes the trace (full speed, §8.5).
        text = "import sys\nbreakpoint()\nx = 1\ntrace = sys.gettrace()\n"
        script = Script(self, ["continue"])
        result = self.debug_run(text, script=script)
        self.assertEqual(len(script.stops), 1)
        self.assertIsNone(result.ns["trace"])

    def test_foreign_tracer_comes_back_after_stop_at_a_line(self):
        def foreign(frame, event, arg):
            return None

        sys.settrace(foreign)
        self.addCleanup(sys.settrace, None)
        script = Script(self, ["terminate"])
        result = self.debug_run(LOOP, breakpoints=[3], script=script)
        self.assertEqual(result.outcome, CANCELLED)
        self.assertIs(sys.gettrace(), foreign)
        sys.settrace(None)


class Variables(DebuggerTestCase):
    def test_no_user_code_runs_while_reading_variables(self):
        # T-INSP-001 / T-INSP-004: getters, __repr__, __len__, __getitem__ and __getattr__ never run.
        text = (
            "calls = []\n"
            "class Thing:\n"
            "    __slots__ = ('slot', '__dict__')\n"
            "    def __init__(self):\n"
            "        self.slot = 5\n"
            "        self.plain = 'p'\n"
            "    def __repr__(self):\n"
            "        calls.append('repr'); return 'T'\n"
            "    def __len__(self):\n"
            "        calls.append('len'); return 1\n"
            "    def __getattr__(self, name):\n"
            "        calls.append('getattr'); return 1\n"
            "    @property\n"
            "    def prop(self):\n"
            "        calls.append('prop'); return 1\n"
            "class L(list):\n"
            "    def __repr__(self):\n"
            "        calls.append('lrepr'); return 'L'\n"
            "t = Thing()\n"
            "box = [t, {'k': t}]\n"
            "sub = L([1])\n"
            "stop_here = 1\n"
        )
        seen = {}

        def inspect(stop):
            variables = locals_of()
            seen["t"] = variables["t"]
            seen["box"] = variables["box"]
            children = request("variables", variablesReference=variables["t"]["variablesReference"])["body"]["variables"]
            seen["t_children"] = {node["name"]: node["value"] for node in children}
            box_children = request("variables", variablesReference=variables["box"]["variablesReference"])["body"]
            seen["box_children"] = [node["value"] for node in box_children["variables"]]
            seen["sub"] = variables["sub"]["value"]

        script = Script(self, ["continue"], inspect)
        result = self.debug_run(text, breakpoints=[22], script=script)
        self.assertEqual(result.outcome, OK, result.text())
        self.assertEqual(result.ns["calls"], [])
        self.assertTrue(seen["t"]["value"].startswith("<__main__.Thing object at"), seen["t"])
        self.assertEqual(seen["t_children"]["plain"], "'p'")
        self.assertEqual(seen["t_children"]["slot"], "5")
        self.assertEqual(seen["t_children"]["prop"], "property: not evaluated")
        self.assertEqual(seen["box"]["value"], "list (2)")
        self.assertEqual(len(seen["box_children"]), 2)
        self.assertTrue(seen["sub"].startswith("<__main__.L object"))

    def test_metaclass_hooks_and_class_body_locals_never_run(self):
        # Review q1 m1: a metaclass __eq__ / __getattribute__ and a __prepare__ mapping are never called while reading.
        text = (
            "calls = []\n"                                                   # 1
            "class Meta(type):\n"                                            # 2
            "    def __eq__(cls, other):\n"                                  # 3
            "        calls.append('eq'); return False\n"                    # 4
            "    __hash__ = type.__hash__\n"                                 # 5
            "    def __getattribute__(cls, name):\n"                         # 6
            "        calls.append('ga:' + name); return type.__getattribute__(cls, name)\n"
            "class Odd(metaclass=Meta):\n"                                   # 8
            "    def __init__(self):\n"                                      # 9
            "        self.v = 1\n"                                           # 10
            "class Spy(dict):\n"                                             # 11
            "    def __iter__(self):\n"                                      # 12
            "        calls.append('iter'); return dict.__iter__(self)\n"    # 13
            "    def keys(self):\n"                                          # 14
            "        calls.append('keys'); return dict.keys(self)\n"        # 15
            "    def items(self):\n"                                         # 16
            "        calls.append('items'); return dict.items(self)\n"      # 17
            "class PMeta(type):\n"                                           # 18
            "    @classmethod\n"                                             # 19
            "    def __prepare__(mcs, name, bases):\n"                       # 20
            "        return Spy()\n"                                         # 21
            "o = Odd()\n"                                                    # 22
            "calls.clear()\n"                                                # 23
            "class Body(metaclass=PMeta):\n"                                 # 24
            "    a = 1\n"                                                    # 25
            "    b = 2\n"                                                    # 26
            "calls.clear()\n"                                                # 27: type() itself calls Spy.keys
            "done = 1\n"                                                     # 28
        )
        seen = {}

        def inspect(stop):
            if stop["line"] == 26:
                seen["body"] = list(locals_of().keys())
                seen["calls26"] = list(stop_calls(self))
            else:
                variables = locals_of()
                ref = variables["o"]["variablesReference"]
                seen["o"] = [n["name"] for n in request("variables", variablesReference=ref)["body"]["variables"]]
                seen["calls"] = list(stop_calls(self))

        def stop_calls(test):
            return debugger.active_session().stop.frames[0][0].f_globals["calls"]

        script = Script(self, ["continue", "continue"], inspect)
        result = self.debug_run(text, breakpoints=[26, 28], script=script)
        self.assertEqual(result.outcome, OK, result.text())
        self.assertEqual(seen["body"], ["(locals)"])
        self.assertEqual(seen["calls26"], [])
        self.assertIn("v", seen["o"])
        self.assertEqual(seen["calls"], [], seen["calls"])

    def test_huge_int_and_container_subclass(self):
        # Review q1 m2 and m5: a huge int shows its size; a list subclass shows its items; >100 attributes page.
        text = (
            "big = 10 ** 6000\n"
            "class L(list):\n"
            "    def __getitem__(self, i):\n"
            "        raise RuntimeError('user code')\n"
            "sub = L([7, 8])\n"
            "class Many:\n"
            "    pass\n"
            "m = Many()\n"
            "for k in range(150):\n"
            "    setattr(m, f'a{k}', k)\n"
            "stop_here = 1\n"
        )
        seen = {}

        def inspect(stop):
            variables = locals_of()
            seen["big"] = variables["big"]["value"]
            children = request("variables", variablesReference=variables["sub"]["variablesReference"])["body"]
            seen["sub"] = [n["value"] for n in children["variables"]]
            first = request("variables", variablesReference=variables["m"]["variablesReference"])["body"]["variables"]
            seen["first"] = len(first)
            more = request("variables", variablesReference=first[-1]["variablesReference"])["body"]["variables"]
            seen["more"] = len(more)

        script = Script(self, ["continue"], inspect)
        result = self.debug_run(text, breakpoints=[11], script=script)
        self.assertEqual(result.outcome, OK, result.text())
        self.assertTrue(seen["big"].startswith("<int, "), seen["big"])
        self.assertEqual(seen["sub"], ["7", "8"])
        self.assertEqual(seen["first"], 101)        # 100 attributes and the "…more" node
        self.assertEqual(seen["more"], 50)

    def test_saferepr_never_calls_user_repr(self):
        calls = []

        class Job:
            def __repr__(self):
                calls.append(1)
                return "J"

        job = Job()
        self.assertEqual(debugger.saferepr([job, {"k": job}]), "[<Job>, {'k': <Job>}]")
        self.assertEqual(calls, [])
        big = list(range(2_000_000))
        self.assertLessEqual(len(debugger.saferepr(big)), 200)

    def test_large_list_pages_by_100(self):
        # T-INSP-003.
        text = "items = list(range(10000))\nstop_here = 1\n"
        pages = {}

        def inspect(stop):
            ref = locals_of()["items"]["variablesReference"]
            groups = request("variables", variablesReference=ref)["body"]["variables"]
            pages["groups"] = len(groups)
            first = request("variables", variablesReference=groups[0]["variablesReference"])["body"]["variables"]
            pages["first"] = len(first)
            pages["last_name"] = first[-1]["name"]

        script = Script(self, ["continue"], inspect)
        self.debug_run(text, breakpoints=[2], script=script)
        self.assertEqual(pages, {"groups": 100, "first": 100, "last_name": "[99]"})

    def test_handles_are_invalid_after_the_stop(self):
        refs = []
        script = Script(self, ["continue", "continue"],
                        lambda stop: refs.append(locals_of()["i"]["variablesReference"] if refs else
                                                 request("scopes", frameId=1)["body"]["scopes"][0]["variablesReference"]))
        self.debug_run("for i in range(2):\n    pass\n", breakpoints=[2], script=script)
        response = request("variables", variablesReference=refs[0])
        self.assertFalse(response["success"])

    def test_set_breakpoints_while_paused(self):
        def inspect(stop):
            if stop["line"] == 1:
                body = request("setBreakpoints", source={"path": FILENAME}, breakpoints=[{"line": 4}])["body"]
                self.assertEqual(body["breakpoints"][0]["line"], 4)

        script = Script(self, ["continue", "continue"], inspect)
        self.debug_run(LOOP, breakpoints=[1], script=script)
        self.assertEqual([stop[1] for stop in script.stops], [1, 4])

    def test_bad_requests_fail_cleanly(self):
        self.assertFalse(json.loads(debugger.handle_request("not json"))["success"])
        self.assertFalse(request("nonsense")["success"])
        self.assertFalse(request("stackTrace")["success"])      # no active session


class Threads(DebuggerTestCase):
    def test_worker_thread_breakpoint_never_pauses(self):
        # T-DBG-018 (no stop; Dream traces only the game thread).
        text = (
            "import threading\n"            # 1
            "out = []\n"                    # 2
            "def work():\n"                 # 3
            "    out.append(1)\n"           # 4
            "t = threading.Thread(target=work)\n"
            "t.start(); t.join()\n"
        )
        script = Script(self, [])
        result = self.debug_run(text, breakpoints=[4], script=script)
        self.assertEqual(result.outcome, OK)
        self.assertEqual(script.stops, [])
        self.assertEqual(result.ns["out"], [1])


if __name__ == "__main__":
    unittest.main()
