# SPDX-License-Identifier: Apache-2.0
"""Auto-print and ``_`` (TDD §5.3.6, RUN-08, D8).

``prepare`` rewrites the last top-level expression of a Session run into ``__dream_display__(<expr>)`` (see
``rewrite_last_expression``); the runner places this run's display function in the namespace for the run and removes
it afterwards if it is still ours. The display function runs inside the user-code window, so a slow ``__repr__`` can
be stopped, and its frame (in this directory) never shows in a traceback.

Result lines: ``repr(value)`` written with ``STREAM_RESULT`` and no prefix, the whole repr in one write. The ingest
thread splits it into lines and flags every line after the first ``OLF_ResultCont``; the output view draws
``# Result: `` before the first line only, and the forwarder prepends it for ``LogPython`` the same way (§6.3, §6.4).
The ingest thread truncates at 10,000 characters (§6.4), so the full repr is sent.
"""

import ast

from . import native

__all__ = ["DISPLAY_NAME", "rewrite_last_expression", "make_display", "displayhook", "set_context"]

DISPLAY_NAME = "__dream_display__"

# The run that sys.displayhook writes to and the dict where it binds "_" (the Session; §5.3.5).
_context = {"dream": None, "run_id": 0, "ns": None}


def rewrite_last_expression(module):
    """If the last top-level statement is an expression, wrap it as ``__dream_display__(value)``. Returns True if so."""
    if not module.body:
        return False
    last = module.body[-1]
    if not isinstance(last, ast.Expr):
        return False
    value = last.value
    func = ast.Name(id=DISPLAY_NAME, ctx=ast.Load())
    call = ast.Call(func=func, args=[value], keywords=[])
    ast.copy_location(func, value)
    ast.copy_location(call, value)
    last.value = call
    return True


def _write_result(dream, run_id, value):
    # One write per result: only its first line carries "# Result: " (§6.4), which the ingest thread can tell only
    # when the lines arrive together.
    dream.write(dream.STREAM_RESULT, run_id, repr(value) + "\n")


def make_display(dream, run_id, ns):
    """The display function of one Session run: None prints nothing and keeps ``_``; else a result line and ``_``."""

    def __dream_display__(value):
        if value is None:
            return
        _write_result(dream, run_id, value)
        ns["_"] = value     # in the Session, never builtins._ (D8)

    return __dream_display__


def set_context(dream, run_id, ns):
    """Bind ``displayhook`` to a run (called by the runner at install). ``ns`` is where ``_`` goes."""
    _context["dream"] = dream
    _context["run_id"] = run_id
    _context["ns"] = ns


def displayhook(value):
    """``sys.displayhook`` during a run: ``single``-mode code prints a result line and binds ``_`` in the Session."""
    if value is None:
        return
    dream = _context["dream"] or native.get()
    _write_result(dream, _context["run_id"], value)
    ns = _context["ns"]
    if ns is not None:
        ns["_"] = value
