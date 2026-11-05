# SPDX-License-Identifier: Apache-2.0
"""The null ``_dream`` mirrors Python/DreamNativeModule.h (ARCHITECTURE.md section 6; the T-API-001 surface rule).

The header's function table is the contract; ``native.NullDream`` must expose exactly those names and constants, plus
``test_*`` hooks, so tier 2 never passes against a surface the C++ module does not have.
"""

import os
import re
import unittest

import dream_testing

from dream_editor._impl import native

HEADER = os.path.normpath(os.path.join(dream_testing.CONTENT_PYTHON, "..", "..", "Source", "DreamEditor", "Private",
                                       "Python", "DreamNativeModule.h"))


def _header_surface():
    with open(HEADER, encoding="utf-8") as handle:
        text = handle.read()
    table = text.split("_dream function table", 1)[1].split("Helper contracts", 1)[0]
    functions_part, constants_part = table.split("//  Constants", 1)
    constants_part = constants_part.split("//  Not in this table", 1)[0]
    names = set()
    for line in functions_part.splitlines():
        if re.match(r"^//   \w+\(", line):
            names.update(re.findall(r"(\w+)\((?:\w+:|\))", line))
    names.update(re.findall(r"\b(ABI_VERSION|GENERATION|HEADLESS|STREAM_[A-Z]+)\b", constants_part))
    return names


@unittest.skipUnless(os.path.exists(HEADER), "DreamNativeModule.h is not in this checkout")
class NativeContract(unittest.TestCase):
    def test_null_surface_equals_the_header_table(self):
        expected = _header_surface()
        null = native.NullDream()
        actual = {name for name in dir(null) if not name.startswith("_") and not name.startswith("test_")}
        self.assertGreater(len(expected), 30)
        self.assertEqual(sorted(actual - expected), [], "NullDream has names the header does not declare")
        self.assertEqual(sorted(expected - actual), [], "the header declares names NullDream lacks")

    def test_resolver_helpers_mean_do_not_read(self):
        # §7.5.2 (N1, RV-21): the null module knows no unreal classes or wrappers: an empty set and None, which the live
        # resolver treats as "do not read"; a non-str class name is a TypeError, as in the C module.
        null = native.NullDream()
        self.assertIs(type(null.hard_object_props("Actor")), frozenset)
        self.assertEqual(null.hard_object_props("Actor"), frozenset())
        with self.assertRaises(TypeError):
            null.hard_object_props(5)
        with self.assertRaises(TypeError):
            null.hard_object_props()
        for value in ([1, 2], {"a": 1}, {1}, None, object()):
            self.assertIsNone(null.container_element_kind(value))

    def test_constants_match(self):
        null = native.NullDream()
        self.assertEqual(null.ABI_VERSION, 1)
        streams = ["STDOUT", "STDERR", "RESULT", "NOTICE", "INFO", "WARNING", "ERROR", "AGENT", "DEBUGGER",
                   "LOGPOINT", "EVALUATE"]
        self.assertEqual([getattr(null, f"STREAM_{name}") for name in streams], list(range(1, 12)))

    def test_argument_errors(self):
        null = native.NullDream()
        with self.assertRaises(TypeError):
            null.write("stdout", 0, "x")
        with self.assertRaises(ValueError):
            null.write(12, 0, "x")
        with self.assertRaises(ValueError):
            null.Writer(native.STREAM_RESULT, 0)
        with self.assertRaises(RuntimeError):
            null.user_code_begin(5)             # not the active run
        with self.assertRaises(TypeError):
            null.register_cancel_class(ValueError)

    def test_stale_and_foreign_run_ids_raise(self):
        # T-API-003 (tier-2 mirror): every GT call that takes the active run raises RuntimeError for another ID, as the
        # C module's ParseActiveRunId does; write() alone tags the line background instead.
        null = native.NullDream()
        calls = {
            "user_code_begin": lambda rid: null.user_code_begin(rid),
            "user_code_done": lambda rid: null.user_code_done(rid),
            "capture_begin": lambda rid: null.capture_begin(rid, []),
            "capture_end": lambda rid: null.capture_end(rid),
            "statement_counts": lambda rid: null.statement_counts(rid),
            "report_outcome": lambda rid: null.report_outcome(rid, native.OUTCOME_OK, "", None),
            "run_statement": lambda rid: null.run_statement(rid, 0, None, {}),
        }
        for name, call in calls.items():
            with self.subTest(name=name, state="idle"):
                with self.assertRaisesRegex(RuntimeError, rf"^{name}\(\): run 7 is not the active run$"):
                    call(7)
        null.test_begin_run(8)
        for name, call in calls.items():
            with self.subTest(name=name, state="another run active"):
                with self.assertRaises(RuntimeError):
                    call(7)
            with self.subTest(name=name, state="bool id"):
                with self.assertRaises(TypeError):
                    call(True)
        null.write(native.STREAM_STDOUT, 7, "late\n")
        self.assertEqual(null.test_lines[-1], (native.STREAM_STDOUT, 7, "late\n", True))
        null.test_end_run(8)
        with self.assertRaises(RuntimeError):
            null.user_code_done(8)              # a past run is stale too

    def test_active_run_calls_work(self):
        null = native.NullDream()
        null.test_begin_run(9)
        null.capture_begin(9, (1, 2))
        self.assertTrue(null.user_code_begin(9))
        null.user_code_done(9)
        null.user_code_done(9)                  # idempotent for the active run
        null.report_outcome(9, native.OUTCOME_OK, "", None)
        null.capture_end(9)
        self.assertEqual([kind for kind, _rid, _payload in null.test_events],
                         ["capture_begin", "user_code_begin", "user_code_done", "user_code_done", "capture_end"])
        self.assertEqual(null.test_events[0][2], (1, 2))

    def test_capture_begin_needs_int_ids(self):
        null = native.NullDream()
        null.test_begin_run(10)
        with self.assertRaises(TypeError):
            null.capture_begin(10, None)
        with self.assertRaises(TypeError):
            null.capture_begin(10, [1, "2"])
        with self.assertRaises(TypeError):
            null.capture_begin(10, [True])

    def test_report_outcome_treats_bool_as_malformed(self):
        null = native.NullDream()
        null.test_begin_run(11)
        null.report_outcome(11, True, "x", None)                   # True == 1, but a bool is no outcome code
        self.assertEqual(null.test_outcomes[11], (native.OUTCOME_ERROR, "internal: malformed outcome", None))
        null.report_outcome(11, native.OUTCOME_ERROR, "ValueError: v", ("<dream:A#1>", False))
        self.assertEqual(null.test_outcomes[11], (native.OUTCOME_ERROR, "internal: malformed outcome", None))
        null.report_outcome(11, native.OUTCOME_ERROR, "ValueError: v", ("<dream:A#1>", 3))
        self.assertEqual(null.test_outcomes[11], (native.OUTCOME_ERROR, "ValueError: v", ("<dream:A#1>", 3)))

    def test_lone_surrogates_become_one_replacement_character_each(self):
        # As DreamPy's UTF-8 conversion: every code point in D800-DFFF becomes U+FFFD (§6.1).
        null = native.NullDream()
        replacement = "\N{REPLACEMENT CHARACTER}"
        lone = chr(0xDCFF)
        pair = chr(0xD83D) + chr(0xDE00)        # two lone surrogates in a str, not U+1F600
        null.write(native.STREAM_STDOUT, 0, f"a{lone}b{pair}c\n")
        self.assertEqual(null.test_lines[-1][2], f"a{replacement}b{replacement}{replacement}c\n")
        valid = "caf\N{LATIN SMALL LETTER E WITH ACUTE} \N{GRINNING FACE}\n"
        null.write(native.STREAM_STDOUT, 0, valid)
        self.assertEqual(null.test_lines[-1][2], valid)


if __name__ == "__main__":
    unittest.main()
