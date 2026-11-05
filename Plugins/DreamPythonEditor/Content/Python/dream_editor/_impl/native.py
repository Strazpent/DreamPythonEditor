# SPDX-License-Identifier: Apache-2.0
"""Binding to the ``_dream`` native module, and the null ``_dream`` of plain CPython (TDD §3.4, §5.3, §12.1, D16).

``get()`` returns the real ``_dream`` (created by DreamEditor at ``OnPythonInitialized``) when it exists, else one
process-wide ``NullDream``. Binding is lazy, so a studio ``init_unreal.py`` that imports ``dream_editor`` early still works.

``NullDream`` mirrors ``Source/DreamEditor/Private/Python/DreamNativeModule.h`` exactly: the same names, signatures,
constants and argument errors. Under plain CPython (tier 2) and ``-run=PythonScript`` (tier 5) it gives the runner a
working in-process implementation (output is recorded, ``run_statement`` evaluates, Stop can be simulated), while the
editor-only commands (``open_tab``, ``new_tab``, ``enqueue_run``) raise ``RuntimeError`` (T-LIFE-010).

Owned by the python slice; the ``_dream`` surface changes only in one commit with DreamNativeModule.h (bridge). Test
hooks (``test_*``) are not part of the surface.
"""

from __future__ import annotations

import builtins
import io
import sys
import threading
import types

__all__ = [
    "ABI_VERSION",
    "STREAM_STDOUT", "STREAM_STDERR", "STREAM_RESULT", "STREAM_NOTICE", "STREAM_INFO", "STREAM_WARNING",
    "STREAM_ERROR", "STREAM_AGENT", "STREAM_DEBUGGER", "STREAM_LOGPOINT", "STREAM_EVALUATE",
    "OUTCOME_OK", "OUTCOME_ERROR", "OUTCOME_CANCELLED", "OUTCOME_NOT_EXECUTED",
    "NullDream", "get", "is_native", "check_abi", "set_for_test", "reset_for_test",
]

# ---- _dream constants (§12.1.1) -----------------------------------------------------------------------------

ABI_VERSION = 1

STREAM_STDOUT = 1
STREAM_STDERR = 2
STREAM_RESULT = 3
STREAM_NOTICE = 4       # runner notice
STREAM_INFO = 5
STREAM_WARNING = 6
STREAM_ERROR = 7
STREAM_AGENT = 8
STREAM_DEBUGGER = 9
STREAM_LOGPOINT = 10
STREAM_EVALUATE = 11

_STREAMS = range(STREAM_STDOUT, STREAM_EVALUATE + 1)

# §6.1: STREAM_AGENT is accepted only while the current run's route is agent or bridge (1.1); no 1.0 route is one.
_AGENT_ROUTES = frozenset({"bridge", "mcp"})
AGENT_STREAM_REFUSED = "STREAM_AGENT (8) is accepted only during an agent or bridge run"

# ---- report_outcome codes ----------------------------------------------------------------------------------
# The values of EDreamRunOutcome (Run/DreamRunTypes.h). (slice A choice) They live here, not on _dream, because
# T-API-001 requires the _dream surface to be exactly the §12.1.1 names and constants.

OUTCOME_OK = 1
OUTCOME_ERROR = 2
OUTCOME_CANCELLED = 3
OUTCOME_NOT_EXECUTED = 4

_SUPPRESS_KEYS = ("results", "info", "warnings", "errors", "echo")

_NATIVE_ONLY = "{name}() needs the Dream Python Editor; the _dream native module is not available in this Python"


def _check_stream(stream, allowed=_STREAMS):
    if isinstance(stream, bool) or not isinstance(stream, int):
        raise TypeError(f"stream must be an int STREAM_* constant, not {type(stream).__name__}")
    if stream not in allowed:
        raise ValueError(f"{stream} is not an accepted STREAM_* constant")


def _check_run_id(run_id):
    if isinstance(run_id, bool) or not isinstance(run_id, int):
        raise TypeError(f"run_id must be an int, not {type(run_id).__name__}")


def _is_strict_int(value):
    # The C module's DreamPy::IsStrictInt: an int that is not a bool.
    return isinstance(value, int) and not isinstance(value, bool)


_surrogate_map = None


def _to_text(text):
    global _surrogate_map
    if not isinstance(text, str):
        raise TypeError(f"write() argument must be str, not {type(text).__name__}")
    if text.isascii():
        return text
    try:
        text.encode("utf-8")
    except UnicodeEncodeError:
        # Each lone surrogate becomes one U+FFFD, as the C side does when it encodes UTF-8 (§6.1). (The "replace"
        # encode error handler would write "?" instead.)
        if _surrogate_map is None:
            _surrogate_map = dict.fromkeys(range(0xD800, 0xE000), "\N{REPLACEMENT CHARACTER}")
        return text.translate(_surrogate_map)
    return text


class NullDream:
    """The null ``_dream``. One instance stands in for the module; attribute names equal the module's.

    Test hooks (not part of the ``_dream`` surface, all prefixed ``test_``): ``test_begin_run``, ``test_end_run``,
    ``test_request_stop``, ``test_set_statement_count``, ``test_set_deferral``, ``test_suppressed``, and the records
    ``test_deliveries`` (True results of ``check_cancel(True)``), ``test_lines``,
    ``test_events``, ``test_outcomes``, ``test_progress``, ``test_timeline`` (echo and write records in one ordered list,
    so a test can check that each statement's echo comes before its output, OUT-12). ``test_echo_to_process`` (default
    False) also prints output to the process's original streams.
    """

    ABI_VERSION = ABI_VERSION
    GENERATION = 0
    HEADLESS = True

    STREAM_STDOUT = STREAM_STDOUT
    STREAM_STDERR = STREAM_STDERR
    STREAM_RESULT = STREAM_RESULT
    STREAM_NOTICE = STREAM_NOTICE
    STREAM_INFO = STREAM_INFO
    STREAM_WARNING = STREAM_WARNING
    STREAM_ERROR = STREAM_ERROR
    STREAM_AGENT = STREAM_AGENT
    STREAM_DEBUGGER = STREAM_DEBUGGER
    STREAM_LOGPOINT = STREAM_LOGPOINT
    STREAM_EVALUATE = STREAM_EVALUATE

    def __init__(self):
        self._lock = threading.Lock()
        self._current_run_id = 0
        self._current_route = ""
        self._user_code_active = False
        self._deferral = False          # a §5.4.3 C++ deferral condition, simulated (test_set_deferral)
        self._stop_requested = set()
        self._past_runs = set()
        self._statement_counts = {}     # run_id -> number of prepared statements (test_set_statement_count)
        self._started_returned = {}     # run_id -> [started, returned], as run_statement records them in C++
        self._cancel_class = None
        self._suppressed = {key: False for key in _SUPPRESS_KEYS}   # §12.2 polarity: True = suppressed (hidden)
        self._thread_labels = {}
        self.test_lines = []        # (stream, run_id, text, background)
        self.test_events = []       # (kind, run_id, payload)
        self.test_outcomes = {}     # run_id -> (outcome, detail, location)
        self.test_progress = {}     # run_id -> (i, n, msg)
        self.test_timeline = []     # ("write", run_id, (stream, text)) and ("echo", run_id, index), in call order
        self.test_echo_to_process = False
        self.test_deliveries = 0
        self.Writer = _make_writer_type(self)
        self.BinaryWriter = _make_binary_writer_type(self)

    # ---- test hooks ----------------------------------------------------------------------------------------

    def test_begin_run(self, run_id, route=None):
        """Make run_id the active run, as FDreamRunService does at entry before ``_run``; ``route`` is its route key
        ("buffer" when None), which decides whether STREAM_AGENT is accepted."""
        _check_run_id(run_id)
        with self._lock:
            self._current_run_id = run_id
            self._current_route = route or "buffer"
            self._stop_requested.discard(run_id)
            self._started_returned[run_id] = [0, 0]

    def test_end_run(self, run_id):
        """End the active run, as FinishRun does after ``_run`` returns."""
        with self._lock:
            if self._current_run_id == run_id:
                self._current_run_id = 0
                self._current_route = ""
                self._user_code_active = False
            self._past_runs.add(run_id)

    def test_request_stop(self, run_id):
        """Simulate a Stop click: the next ``run_statement`` raises ScriptCancelled, ``check_cancel`` is True."""
        with self._lock:
            self._stop_requested.add(run_id)

    def test_set_deferral(self, holds):
        """Simulate a §5.4.3 deferral condition (saving, GC, PostLoad, native re-entry): ``check_cancel(True)`` is False."""
        self._deferral = bool(holds)

    def test_set_statement_count(self, run_id, count):
        """The number of prepared statements of a run, so ``run_statement`` validates ``index`` as C++ does."""
        self._statement_counts[run_id] = count

    def test_suppressed(self):
        """The current suppress() state of every key (True = suppressed), for tier-2 checks of T-API-011."""
        return dict(self._suppressed)

    # ---- output (§6.1, §6.2) -------------------------------------------------------------------------------

    def _record(self, stream, run_id, text):
        background = run_id != self._current_run_id
        with self._lock:
            self.test_lines.append((stream, run_id, text, background))
            self.test_timeline.append(("write", run_id, (stream, text)))
        if self.test_echo_to_process:
            target = sys.__stderr__ if stream in (STREAM_STDERR, STREAM_ERROR, STREAM_WARNING) else sys.__stdout__
            if target is not None:
                target.write(text)

    def write(self, stream, run_id, text):
        """write(stream: int, run_id: int, text: str) -> None. Any thread.

        ``stream`` is a STREAM_* constant (1..11): another int raises ValueError, a non-int TypeError; STREAM_AGENT
        raises ValueError unless the current run's route is agent or bridge (§6.1, 1.1); ``text`` must be str
        (TypeError). Pushes one raw record; never blocks. A run ID that is neither current nor a known past run is
        tagged run 0 and background. Never raises for a stale ID.
        """
        _check_stream(stream)
        _check_run_id(run_id)
        if stream == STREAM_AGENT and (not self._current_run_id or self._current_route not in _AGENT_ROUTES):
            raise ValueError(AGENT_STREAM_REFUSED)
        self._record(stream, run_id, _to_text(text))

    def thread_label(self, ident, name):
        """thread_label(ident: int, name: str) -> None. Any thread. Caches a thread name for tagging (D24)."""
        if not isinstance(ident, int) or not isinstance(name, str):
            raise TypeError("thread_label(ident: int, name: str)")
        self._thread_labels[ident] = name

    def log_shim(self, verbosity, text):
        """log_shim(verbosity: int, text: object) -> None. Any thread.

        0 Log, 1 Warning, 2 Error (else ValueError). Converts with ``str(text)``; whitespace-only text is dropped
        (the engine's ShouldLog); pushes an info, warning or error record with origin UnrealLog (RT-07).
        """
        if isinstance(verbosity, bool) or not isinstance(verbosity, int):
            raise TypeError("verbosity must be an int")
        if verbosity not in (0, 1, 2):
            raise ValueError(f"verbosity {verbosity} is not 0, 1 or 2")
        message = str(text)
        if not message.strip():
            return
        stream = (STREAM_INFO, STREAM_WARNING, STREAM_ERROR)[verbosity]
        self._record(stream, self._current_run_id, _to_text(message) + "\n")

    def shim_log(self, text):
        """shim_log(text: object) -> None. Pre-bound log_shim(0, text); replaces unreal.log for a run (§6.1)."""
        self.log_shim(0, text)

    def shim_log_warning(self, text):
        """shim_log_warning(text: object) -> None. Pre-bound log_shim(1, text); replaces unreal.log_warning."""
        self.log_shim(1, text)

    def shim_log_error(self, text):
        """shim_log_error(text: object) -> None. Pre-bound log_shim(2, text); replaces unreal.log_error."""
        self.log_shim(2, text)

    def capture_begin(self, run_id, alive_native_ids):
        """capture_begin(run_id: int, alive_native_ids: tuple[int, ...] | list[int]) -> None. GT.

        Starts run-scoped capture and the thread table: threads in ``alive_native_ids`` existed before the run (§6.1).
        RuntimeError when ``run_id`` is not the active run (T-API-003); TypeError when the IDs are not a sequence of
        ints.
        """
        self._require_active(run_id, "capture_begin")
        try:
            alive = tuple(alive_native_ids)
        except TypeError:
            raise TypeError("capture_begin(): alive_native_ids must be a tuple or list of ints") from None
        if not all(_is_strict_int(native_id) for native_id in alive):
            raise TypeError("capture_begin(): alive_native_ids must hold ints")
        self.test_events.append(("capture_begin", run_id, alive))

    def capture_end(self, run_id):
        """capture_end(run_id: int) -> None. GT. Ends run-scoped capture. RuntimeError when ``run_id`` is not the
        active run (T-API-003)."""
        self._require_active(run_id, "capture_end")
        self.test_events.append(("capture_end", run_id, None))

    def clear_output(self):
        """clear_output() -> None. GT. FDreamOutputStore::Clear(); allowed during a run. A no-op here (T-OUT-035)."""
        self.test_events.append(("clear_output", self._current_run_id, None))

    # ---- run handshake (§5.3, §5.7) ------------------------------------------------------------------------

    def _require_active(self, run_id, name):
        # The C module's ParseActiveRunId: TypeError for a non-int, RuntimeError for a stale or foreign ID (§12.1).
        if not _is_strict_int(run_id):
            raise TypeError(f"{name}(): run_id must be an int, not {type(run_id).__name__}")
        if run_id == 0 or run_id != self._current_run_id:
            raise RuntimeError(f"{name}(): run {run_id} is not the active run")

    def current_run_id(self):
        """current_run_id() -> int. Any thread. The active run's ID; 0 when idle."""
        return self._current_run_id

    def user_code_begin(self, run_id):
        """user_code_begin(run_id: int) -> bool. GT.

        Opens the user-code window (delivery and the pump on) and captures the native entry mark (§2.6.3).
        Returns False when Stop was already requested; the runner then raises ScriptCancelled itself.
        """
        self._require_active(run_id, "user_code_begin")
        self._user_code_active = True
        self.test_events.append(("user_code_begin", run_id, None))
        return run_id not in self._stop_requested

    def user_code_done(self, run_id):
        """user_code_done(run_id: int) -> None. GT; idempotent.

        Closes the user-code window: no delivery, no pump, no new watchdog posts (it never waits, GIL-6); clears a
        pending async exception for the game thread. First statement of the runner's handler (RT-06). RuntimeError
        when ``run_id`` is not the active run (T-API-003).
        """
        self._require_active(run_id, "user_code_done")
        self._user_code_active = False
        self.test_events.append(("user_code_done", run_id, None))

    def run_statement(self, run_id, index, code, ns):
        """run_statement(run_id: int, index: int, code: types.CodeType | None, ns: dict) -> None. GT, user-code window.

        Statement mode (RUN-27): (1) raises a new ScriptCancelled if Stop was requested (a delivery); (2) records
        ``index`` as started; (3) pushes the statement's echo lines (built in C++ from the run's source snapshot,
        OUT-12; here an ("echo", run_id, index) event); (4) unless ``code`` is None, evaluates it in ``ns`` as
        ``exec`` does, inserting ``__builtins__`` when missing; (5) records ``index + 1`` as returned before it
        returns. An exception from the statement propagates with the returned count unchanged.
        ValueError for an index outside the prepared range; RuntimeError when ``run_id`` is not the active run, and
        then nothing is evaluated.
        """
        self._require_active(run_id, "run_statement")
        if isinstance(index, bool) or not isinstance(index, int):
            raise TypeError("index must be an int")
        count = self._statement_counts.get(run_id)
        if index < 0 or (count is not None and index >= count):
            raise ValueError(f"statement index {index} is outside the prepared range")
        if code is not None and not isinstance(code, types.CodeType):
            raise TypeError("code must be a code object or None")
        if not isinstance(ns, dict):
            raise TypeError("ns must be a dict")
        if run_id in self._stop_requested:
            raise self._new_cancelled(source="Stop button", delivery=1)
        counts = self._started_returned.setdefault(run_id, [0, 0])
        counts[0] = index + 1
        self.test_events.append(("started", run_id, index))
        self.test_events.append(("echo", run_id, index))
        with self._lock:
            self.test_timeline.append(("echo", run_id, index))
        if code is not None:
            if "__builtins__" not in ns:
                ns["__builtins__"] = builtins
            exec(code, ns)  # noqa: S102 - the null module stands in for PyEval_EvalCode(code, ns, ns)
        counts[1] = index + 1
        self.test_events.append(("returned", run_id, index + 1))

    def statement_counts(self, run_id):
        """statement_counts(run_id: int) -> tuple[int, int]. GT.

        ``(started, returned)`` as ``run_statement`` keeps them in the run record: ``(0, 0)`` before statement 0 starts
        and for a run that is not in statement mode. ``_classify`` reads them for a Stop that landed with no user frame
        ("stopped after statement k of n", "Stop arrived as the code finished", §5.3.7, §5.3.9). RuntimeError when
        ``run_id`` is not the active run.
        """
        self._require_active(run_id, "statement_counts")
        started, returned = self._started_returned.get(run_id, (0, 0))
        return (started, returned)

    def report_outcome(self, run_id, outcome, detail, location):
        """report_outcome(run_id: int, outcome: int, detail: str, location: tuple[str, int] | None) -> None. GT.

        ``outcome`` is 1 ok, 2 error, 3 cancelled or 4 not executed (EDreamRunOutcome); ``detail`` the U2 detail text;
        ``location`` (filename, 1-based line) or None. Malformed input becomes outcome error with
        "internal: malformed outcome"; a bool is not an int here, as in C (``True`` is no outcome code). Called once
        per entry, after the swaps are restored.
        """
        self._require_active(run_id, "report_outcome")
        well_formed = (
            _is_strict_int(outcome)
            and outcome in (OUTCOME_OK, OUTCOME_ERROR, OUTCOME_CANCELLED, OUTCOME_NOT_EXECUTED)
            and isinstance(detail, str)
            and (location is None or (isinstance(location, tuple) and len(location) == 2
                                      and isinstance(location[0], str) and _is_strict_int(location[1])))
        )
        if not well_formed:
            outcome, detail, location = OUTCOME_ERROR, "internal: malformed outcome", None
        self.test_outcomes[run_id] = (outcome, detail, location)

    def register_cancel_class(self, cls):
        """register_cancel_class(cls: type) -> None. GT.

        TypeError unless ``cls`` subclasses KeyboardInterrupt (D2). C++ keeps a strong reference until
        OnPythonShutdown and creates each delivered instance with run_number, source and delivery set.
        """
        if not (isinstance(cls, type) and issubclass(cls, KeyboardInterrupt)):
            raise TypeError("register_cancel_class() needs a subclass of KeyboardInterrupt")
        self._cancel_class = cls

    def _new_cancelled(self, source, delivery):
        cls = self._cancel_class or KeyboardInterrupt
        exc = cls()
        try:
            exc.run_number = 0
            exc.source = source
            exc.delivery = delivery
        except AttributeError:
            pass
        return exc

    def check_cancel(self, *args):
        """check_cancel(deliver: bool = False) -> bool. Any thread. Never raises ScriptCancelled.

        ``check_cancel()``: True when Stop is pending for the run of the calling context. ``check_cancel(True)`` is the
        public ``dream_editor.check_cancel()`` asking, on the game thread, whether it may raise: True only when Stop is
        pending and no §5.4.3 deferral condition holds (C++: inside the user-code window, outside a Dream Python Editor pump, not
        saving, collecting garbage or routing PostLoad, no native re-entry); that True counts as a delivery (§5.10.2,
        A19). Off the game thread (here: the main thread) ``check_cancel(True)`` is ``check_cancel()``. TypeError for a
        non-bool argument or more than one argument.
        """
        if len(args) > 1:
            raise TypeError(f"check_cancel() takes at most 1 argument ({len(args)} given)")
        deliver = False
        if args:
            if not isinstance(args[0], bool):
                raise TypeError(f"check_cancel(): deliver must be a bool, not {type(args[0]).__name__}")
            deliver = args[0]
        run_id = self._current_run_id
        pending = run_id != 0 and run_id in self._stop_requested
        if not deliver or threading.current_thread() is not threading.main_thread():
            return pending
        if pending and self._user_code_active and not self._deferral:
            self.test_deliveries += 1
            return True
        return False

    def stop_requested(self, run_id):
        """stop_requested(run_id: int) -> bool. Any thread. Stop requested for that run."""
        _check_run_id(run_id)
        return run_id in self._stop_requested

    def progress(self, i, n, msg):
        """progress(i: int, n: int | None, msg: str) -> None. Any thread.

        ValueError unless 0 <= i and (n is None or i <= n); n None is an indeterminate bar (U12). A no-op outside
        a run; the latest sample feeds the Run window, the banner and WS-07.
        """
        if isinstance(i, bool) or not isinstance(i, int) or (n is not None and (isinstance(n, bool) or not isinstance(n, int))):
            raise TypeError("progress(i: int, n: int | None, msg: str)")
        if not isinstance(msg, str):
            raise TypeError("msg must be str")
        if i < 0 or (n is not None and i > n):
            raise ValueError(f"progress({i}, {n}): need 0 <= i <= n")
        run_id = self._current_run_id
        if run_id:
            self.test_progress[run_id] = (i, n, msg)

    # ---- (debug, PO-06 F5) the pause port (§8.1.5, §12.1.1) ---------------------------------------------------

    def test_set_pause_handler(self, handler):
        """Tier 2's fake Paused window: ``handler(run_id, stop: dict) -> (command, args_json)`` answers each ``pause``.
        It may call ``debugger.handle_request`` as the window's requests do. None: every pause continues."""
        self._pause_handler = handler

    def test_set_pause_refusal(self, code):
        """Simulate a C++ pause refusal (EDreamPauseRefusal code) from ``pause_context``; 0 clears it."""
        self._pause_refusal = int(code)

    def pause_context(self, run_id):
        """pause_context(run_id: int) -> int. GT. The C++ pause rules of §8.4 (4-11) as an EDreamPauseRefusal code; 0 when
        a pause may open now. 1 (NotDebugRun) for a run that is not the active run. Allocates nothing."""
        _check_run_id(run_id)
        if run_id == 0 or run_id != self._current_run_id:
            return 1
        if getattr(self, "_paused", False):
            return 8
        return getattr(self, "_pause_refusal", 0)

    def pause(self, run_id, stop_json):
        """pause(run_id: int, stop_json: str) -> tuple[str, str]. GT; blocks in the Paused window's modal loop with the
        GIL released (§8.3.1). Returns (command, args_json): "continue", "next", "stepIn", "stepOut", "terminate", or
        ("refused", "<code>") when a pause rule refuses or the window could not open. Stop at a line stop raises
        ScriptCancelled (counted as a delivery) instead of returning "terminate"; at an exception stop
        (reason "exception") it returns ("terminate", "{}"). RuntimeError for a run that is not the active run."""
        self._require_active(run_id, "pause")
        if not isinstance(stop_json, str):
            raise TypeError("pause(): stop_json must be str")
        import json
        stop = json.loads(stop_json)
        code = self.pause_context(run_id)
        if code:
            return ("refused", str(code))
        stops = getattr(self, "test_stops", None)
        if stops is None:
            stops = self.test_stops = []
        stops.append(stop)
        handler = getattr(self, "_pause_handler", None)
        self._paused = True
        try:
            result = handler(run_id, stop) if handler is not None else ("continue", "{}")
        finally:
            self._paused = False
        command, args = result if isinstance(result, tuple) else (result, "{}")
        if command == "terminate":
            self.test_request_stop(run_id)
            if stop.get("reason") != "exception":
                raise self._new_cancelled(source="Paused window", delivery=1)
        return (command, args)

    def is_game_thread(self):
        """is_game_thread() -> bool. Any thread. True on the game thread (here: the main thread)."""
        return threading.current_thread() is threading.main_thread()

    def object_props(self, path_name):
        """object_props(path_name: str) -> list[tuple[str, str, str]]. GT. (debug choice, §8.8 RV-21) The editor
        properties of the object at path_name as (name, C++ type, export text), read with FProperty::ExportTextItem_Direct,
        which runs no getter and loads nothing; [] for an unknown path. TypeError for a non-str. Plain CPython has no
        objects: always []."""
        if not isinstance(path_name, str):
            raise TypeError(f"object_props() argument must be str, not {type(path_name).__name__}")
        return []

    # ---- public API commands (§11.7, §12.2) ----------------------------------------------------------------

    def open_tab(self, path, line, col):
        """open_tab(path: str, line: int, col: int) -> str. GT.

        Opens or focuses a file tab and places the caret (1-based; 0 keeps it). Inside a run it is applied at run
        end, outside a run at the next idle tick. (slice A choice) Returns the tab ID (32 lower-case hex digits) at
        once. Raises RuntimeError here: it needs the editor.
        """
        raise RuntimeError(_NATIVE_ONLY.format(name="open_tab"))

    def new_tab(self, text, name):
        """new_tab(text: str, name: str) -> str. GT.

        Creates a scratch tab (name "" gives Untitled-N) and returns its tab ID now; inside a run the tab appears at
        run end. Raises RuntimeError here: it needs the editor.
        """
        raise RuntimeError(_NATIVE_ONLY.format(name="new_tab"))

    def enqueue_run(self, spec_json):
        """enqueue_run(spec_json: str) -> int. GT.

        spec: {"target": "<tab id | tab name | file path>", "namespace": "session" | "isolated" | null,
        "transaction": true | false | null}. Route Api; returns the run number. RuntimeError inside a run (one run at
        a time, D14(4)), for an unknown target or a refused request. Raises RuntimeError here: it needs the editor.
        """
        raise RuntimeError(_NATIVE_ONLY.format(name="enqueue_run"))

    def suppress(self, flags):
        """suppress(flags: dict[str, bool]) -> dict[str, bool]. GT.

        Keys "results", "info", "warnings", "errors", "echo" (another key raises ValueError, a non-bool value
        TypeError). The §12.2 polarity: True suppresses (hides) the kind, False shows it, an absent key leaves it, so
        ``suppress(echo=True)`` turns Echo input off (T-OUT-043). Returns the previous state of every key, same
        polarity (True = was suppressed).
        """
        if not isinstance(flags, dict):
            raise TypeError("suppress() takes a dict")
        for key, value in flags.items():
            if key not in _SUPPRESS_KEYS:
                raise ValueError(f"suppress(): unknown key {key!r}")
            if not isinstance(value, bool):
                raise TypeError(f"suppress(): {key} must be a bool")
        previous = dict(self._suppressed)
        self._suppressed.update(flags)
        return previous

    # ---- live resolver helpers (§7.5.2, N1, RV-21) ----------------------------------------------------------

    def hard_object_props(self, py_class_name):
        """hard_object_props(py_class_name: str) -> frozenset[str]. GT.

        The normalised names of a class's hard object, class and weak object properties (C++ reads reflection).
        TypeError for a non-str. Plain CPython has no unreal classes, so every class gives an empty frozenset, which
        callers treat as "do not read" (N1).
        """
        if not isinstance(py_class_name, str):
            raise TypeError(f"hard_object_props() argument must be str, not {type(py_class_name).__name__}")
        return frozenset()

    def container_element_kind(self, value):
        """container_element_kind(value: object) -> str | None. GT.

        The element property's class name of an exact unreal.Array, Map or Set wrapper; None ("do not read") for any
        other value. The C module returns None for every value in M1 (Deferred (M2): the wrapper types are private to
        PythonScriptPlugin), and so does this one.
        """
        return None


def _make_writer_type(owner):
    class Writer:
        """Writer(stream: int, run_id: int). Any thread.

        The text stream that replaces sys.stdout / sys.stderr for a run (§6.1). ``stream`` is STREAM_STDOUT or
        STREAM_STDERR (ValueError otherwise). write(s) returns len(s) and raises TypeError for non-str; flush() does
        no I/O; encoding "utf-8", errors "replace", name "<dream stdout>" / "<dream stderr>", line_buffering True;
        isatty/readable/seekable False, writable True; fileno, close and detach raise io.UnsupportedOperation.
        """

        def __init__(self, stream, run_id):
            _check_stream(stream, (STREAM_STDOUT, STREAM_STDERR))
            _check_run_id(run_id)
            self._stream = stream
            self._run_id = run_id
            self._buffer = owner.BinaryWriter(stream, run_id)

        @property
        def buffer(self):
            return self._buffer

        @property
        def encoding(self):
            return "utf-8"

        @property
        def errors(self):
            return "replace"

        @property
        def name(self):
            return "<dream stdout>" if self._stream == STREAM_STDOUT else "<dream stderr>"

        @property
        def line_buffering(self):
            return True

        @property
        def closed(self):
            return False

        def write(self, s):
            text = _to_text(s)
            owner.write(self._stream, self._run_id, text)
            return len(s)

        def flush(self):
            return None

        def isatty(self):
            return False

        def readable(self):
            return False

        def seekable(self):
            return False

        def writable(self):
            return True

        def fileno(self):
            raise io.UnsupportedOperation("fileno")

        def close(self):
            raise io.UnsupportedOperation("close")

        def detach(self):
            raise io.UnsupportedOperation("detach")

    return Writer


def _make_binary_writer_type(owner):
    class BinaryWriter:
        """BinaryWriter(stream: int, run_id: int). Any thread.

        ``Writer.buffer``: write(b) decodes UTF-8 with replacement and then behaves as Writer.write; returns len(b).
        """

        def __init__(self, stream, run_id):
            _check_stream(stream, (STREAM_STDOUT, STREAM_STDERR))
            _check_run_id(run_id)
            self._stream = stream
            self._run_id = run_id

        def write(self, b):
            data = bytes(memoryview(b))
            owner.write(self._stream, self._run_id, data.decode("utf-8", "replace"))
            return len(data)

        def flush(self):
            return None

        def writable(self):
            return True

        def readable(self):
            return False

        def seekable(self):
            return False

        def isatty(self):
            return False

        def fileno(self):
            raise io.UnsupportedOperation("fileno")

        def close(self):
            raise io.UnsupportedOperation("close")

    return BinaryWriter


# ---- binding -------------------------------------------------------------------------------------------------

_bound = None
_null = None
_override = None


def get():
    """The ``_dream`` to use: a test override, else the real module once DreamEditor created it, else the null one.

    DreamEditor inserts the module as ``sys.modules['_dream']`` (§12.1), so only that entry is looked at: an import
    attempt per call would cost a failed path scan on every ``progress()`` in plain CPython. The entry is re-read on
    every call, so the module is picked up once it appears (a studio ``init_unreal.py`` may import dream_editor first,
    §3.4) and a Python restart's new module replaces the old one.
    """
    global _bound, _null
    if _override is not None:
        return _override
    real = sys.modules.get("_dream")
    if real is not None:
        if real is not _bound:
            _bound = real
        return real
    if _null is None:
        _null = NullDream()
    return _null


def is_native():
    """True when the real ``_dream`` is bound (editor or commandlet with DreamEditor loaded)."""
    return not isinstance(get(), NullDream)


ABI_MISMATCH_TEXT = "Dream Python Editor Python package and native module do not match; reinstall Dream Python Editor"


def check_abi(expected):
    """§12.2 ABI check: ImportError unless the real ``_dream.ABI_VERSION`` equals ``expected``. The null module passes."""
    dream = get()
    if isinstance(dream, NullDream):
        return
    if getattr(dream, "ABI_VERSION", None) != expected:
        raise ImportError(ABI_MISMATCH_TEXT)


def set_for_test(dream):
    """Tier 2: make ``get()`` return ``dream`` (usually a fresh NullDream). Never used in the editor."""
    global _override
    _override = dream


def reset_for_test():
    """Undo ``set_for_test`` and forget the cached binding."""
    global _override, _bound, _null
    _override = None
    _bound = None
    _null = None
