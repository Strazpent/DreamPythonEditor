# SPDX-License-Identifier: Apache-2.0
"""Shared tier-2 fixtures (TDD §14.1): the dream_editor package under the engine's CPython with the null ``_dream``.

Every test gets a fresh ``NullDream`` (``native.set_for_test``) and a fresh Session dict captured by ``_boot``, as the
editor does at boot. ``run_code`` drives one request through ``runner.execute_for_test`` exactly as FDreamRunService
would: prepare, make the run active, ``dream_editor._run(id)``, end the run.
"""

import builtins
import itertools
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
CONTENT_PYTHON = os.path.normpath(os.path.join(_HERE, "..", "..", "Content", "Python"))
if CONTENT_PYTHON not in sys.path:
    sys.path.insert(0, CONTENT_PYTHON)

import dream_editor  # noqa: E402
from dream_editor._impl import native  # noqa: E402
from dream_editor._impl import runner  # noqa: E402
from dream_editor._impl import streams  # noqa: E402

_generation = itertools.count(100)

OK = native.OUTCOME_OK
ERROR = native.OUTCOME_ERROR
CANCELLED = native.OUTCOME_CANCELLED
NOT_EXECUTED = native.OUTCOME_NOT_EXECUTED


def new_session(**extra):
    session = {"__name__": "__main__", "__builtins__": builtins, "__doc__": None}
    session.update(extra)
    return session


class RunnerTestCase(unittest.TestCase):
    """A fresh null ``_dream`` and Session per test; the interpreter-global slots are checked after every test."""

    def setUp(self):
        self.dream = self.make_dream()
        native.set_for_test(self.dream)
        self.addCleanup(native.reset_for_test)
        self.session = new_session()
        # Ruling R3: exactly True, the repr FDreamPython::Boot reads.
        self.assertIs(dream_editor._boot(self.session, next(_generation)), True)
        self._slots_before = global_slots()
        self.addCleanup(self._check_slots)

    def make_dream(self):
        return native.NullDream()

    def _check_slots(self):
        streams.emergency_restore()
        self.assertEqual(global_slots(), self._slots_before, "a run left an interpreter-global slot swapped")

    def run_code(self, text, dream=None, before_run=None, **request):
        request.setdefault("filename", "<dream:Untitled-1#a41f>")
        request["text"] = text
        return runner.execute_for_test(request, dream or self.dream, before_run=before_run)

    def echo_indices(self, result):
        return [payload for kind, payload in result.events if kind == "echo"]

    def started_indices(self, result):
        return [payload for kind, payload in result.events if kind == "started"]

    def timeline(self, run_id):
        return [(kind, payload) for kind, rid, payload in self.dream.test_timeline if rid == run_id]


def global_slots():
    """The identity of every slot the run swaps, for before/after comparisons."""
    import time
    import warnings

    slots = {
        "sys.stdout": id(sys.stdout),
        "sys.stderr": id(sys.stderr),
        "sys.displayhook": id(sys.displayhook),
        "sys.argv": id(sys.argv),
        "warnings.showwarning": id(warnings.showwarning),
        "time.sleep": id(time.sleep),
    }
    unreal = sys.modules.get("unreal")
    if unreal is not None:
        for name in ("log", "log_warning", "log_error"):
            slots[f"unreal.{name}"] = id(getattr(unreal, name, None))
    return slots
