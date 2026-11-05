// SPDX-License-Identifier: Apache-2.0
// The _dream native module (TDD §12.1, D7, GIL-2) and the helper contracts C++ <-> dream_editor (§12.1.3).
// Body: DreamNativeModule.cpp (bridge slice). Content/Python/dream_editor/_impl/native.py mirrors this table exactly
// (the null _dream of plain CPython, tier 2). Includes Python.h: only §2.5 rule 6 folders include this header.
//
// ================================================================================================================
// _dream function table, slice A (the §12.1.1 subset that slice A items 6-12 and the M1 public API need)
// All are called from Python with the GIL held; none releases it, calls back into Python, or waits (GIL-2).
// Errors (§12.1): a GT-only function on another thread, or a stale/foreign run ID, raises RuntimeError; a wrong type
// raises TypeError; a bad value raises ValueError. write() never raises for a stale ID (the line is tagged background).
// Only run_statement raises ScriptCancelled.
//
//  Output (§6.1, §6.2)
//   write(stream: int, run_id: int, text: str) -> None                              any thread
//       stream is a STREAM_* constant (1..11); another int -> ValueError; a non-int -> TypeError; STREAM_AGENT ->
//       ValueError unless the current run's route is agent or bridge (§6.1, 1.1; checked after the run_id type); text
//       not str -> TypeError. UTF-8 with lone surrogates -> U+FFFD; one raw record to FDreamOutputStore::Push; never blocks.
//       A run ID that is neither current nor a known past run is tagged run 0 and background (§6.1).
//   Writer(stream: int, run_id: int)                                                 any thread
//       Text stream replacing sys.stdout/sys.stderr for a run; stream is STREAM_STDOUT or STREAM_STDERR (else ValueError).
//       write(s: str) -> int (len(s); TypeError for non-str); flush() -> None (no I/O, commits nothing);
//       buffer -> BinaryWriter; encoding "utf-8"; errors "replace"; name "<dream stdout>" / "<dream stderr>";
//       line_buffering True; isatty() False; readable() False; seekable() False; writable() True;
//       fileno() raises io.UnsupportedOperation; closed False; close() and detach() raise io.UnsupportedOperation.
//   BinaryWriter(stream: int, run_id: int)                                           any thread
//       write(b: bytes-like) -> int (decodes UTF-8 with replacement, then as Writer.write); flush() -> None.
//   thread_label(ident: int, name: str) -> None                                      any thread
//       Caches a thread's name for its native thread id (ident is threading.Thread.native_id, the id capture_begin
//       lists; D24): FDreamOutputStore::SetThreadLabel(ident, name), read by the badges.
//   log_shim(verbosity: int, text: object) -> None                                   any thread
//       0 Log, 1 Warning, 2 Error (else ValueError); str(text); whitespace-only text dropped (ShouldLog);
//       pushes an info/warning/error record with origin UnrealLog. Never broadcasts the engine capture (RT-07).
//   shim_log(text: object) -> None, shim_log_warning(text: object) -> None, shim_log_error(text: object) -> None
//       Pre-bound log_shim(0|1|2, text). Carry the originals' __name__, __qualname__ and __doc__ ("log", "log_warning", "log_error"; §6.1 step 5).
//   capture_begin(run_id: int, alive_native_ids: tuple[int, ...] | list[int]) -> None   GT
//       Starts run-scoped capture and the thread table for the run (§6.1 thread attribution):
//       FDreamOutputStore::CaptureBegin(run_id, alive_native_ids); the listed threads existed before the run.
//   capture_end(run_id: int) -> None                                                 GT
//       FDreamOutputStore::CaptureEnd(run_id).
//   clear_output() -> None                                                           GT
//       FDreamOutputStore::Clear(); allowed during a run (applied at the next drain, the active header kept, §6.4).
//
//  Run handshake (§5.3, §5.7)
//   current_run_id() -> int                                                          any thread; 0 when idle
//   user_code_begin(run_id: int) -> bool                                             GT
//       Sets bUserCodeActive (delivery and the pump on) and captures the native entry mark (§2.6.3).
//       Returns False when Stop was already requested (the runner then raises ScriptCancelled itself; in the run's first
//       window this counts as a delivery).
//       A second begin/done pair may follow the first user_code_done in the same run: the runner's call of a replaced
//       sys.excepthook (§5.3.7). It reopens delivery and the pump with a fresh entry mark and resets neither the run's
//       stats nor its Stop state; returning False there (Stop already requested) skips the hook and counts no delivery.
//   user_code_done(run_id: int) -> None                                              GT; idempotent
//       Clears bUserCodeActive, stops new watchdog posts without waiting (GIL-6), clears a pending async exception
//       for the game thread. First statement of the runner's handler (RT-06); also closes a second window.
//   run_statement(run_id: int, index: int, code: types.CodeType | None, ns: dict) -> None   GT, user-code window
//       Statement mode (RUN-27): (1) raises a new ScriptCancelled if Stop was requested (counted as a delivery);
//       (2) records index as started; (3) pushes the statement's echo lines from the run's source snapshot (OUT-12,
//       each snapshot line at most once per run, built in C++, no Python string); (4) unless code is None, evaluates
//       it in ns as exec does (inserting __builtins__ when ns lacks it); (5) records index + 1 as returned before it
//       returns. An exception from the statement propagates with the returned count unchanged.
//       ValueError for an index outside the prepared range; RuntimeError for a run that is not the active run
//       (nothing is evaluated).
//   statement_counts(run_id: int) -> tuple[int, int]                                 GT
//       (started, returned) as run_statement keeps them in the run record (FDreamRunService::GetStatementCounts):
//       (0, 0) before statement 0 starts and for a run that is not in statement mode. The runner's _classify reads them
//       to build "stopped after statement k of n" / "Stop arrived as the code finished" for a Stop that landed with no
//       user frame (§5.3.7, §5.3.9). RuntimeError for a run that is not the active run.
//   report_outcome(run_id: int, outcome: int, detail: str, location: tuple[str, int] | None) -> None   GT
//       outcome: 1 ok, 2 error, 3 cancelled, 4 not executed (EDreamRunOutcome values); a bool is not an int here.
//       detail: "" or the U2 detail ("exited (code 3)", "stopped after statement 2 of 5", "Stop arrived as the code
//       finished"); for any other BaseException (ruling R1) "<Type>: <first line of str(exc)>", just "<Type>" when
//       str(exc) is empty, cut to 200 characters with "…" ("ZeroDivisionError: division by zero"). C++ uses the detail
//       as given (the footer, FDreamRuntimeProblem::Message); the bare type is the text before the first ": ".
//       location: (filename, 1-based line) where it raised or landed. Malformed input -> outcome error with
//       "internal: malformed outcome". Called once per entry, after the swaps are restored.
//   register_cancel_class(cls: type) -> None                                         GT
//       TypeError unless cls subclasses KeyboardInterrupt (D2). C++ keeps a strong reference until OnPythonShutdown.
//   check_cancel(deliver: bool = False) -> bool                                      any thread
//       check_cancel(): True when Stop is pending for the run of the calling context (False under the Off presentation).
//       Never raises ScriptCancelled (A19). check_cancel(True) is dream_editor.check_cancel() on the game thread asking
//       whether it may raise (§5.10.2): True only when Stop is pending, the caller is inside the user-code window and
//       outside a Dream pump, and no §5.4.3 deferral condition holds (saving a package, GC, PostLoad, native re-entry);
//       that True counts as a delivery (FRunSupervisor::CheckCancel), so the 250 ms re-delivery and the Interrupt
//       cleanup clocks start from it. When Stop is pending but it returns False, dream_editor.check_cancel() returns True
//       instead of raising (the deferral row; it also treats the import machinery on the stack as one). Off the game
//       thread check_cancel(True) is check_cancel(). TypeError for a non-bool argument or more than one argument.
//   stop_requested(run_id: int) -> bool                                              any thread
//   progress(i: int, n: int | None, msg: str) -> None                                any thread
//       ValueError unless 0 <= i and (n is None or i <= n); n None = indeterminate (U12); a no-op outside a run.
//
//  Public API commands (§11.7, §12.2)                                                GT
//   open_tab(path: str, line: int, col: int) -> str
//       Opens or focuses a file tab and places the caret (1-based; 0 = keep). Inside a run it is queued and applied at
//       run end, outside a run at the next idle tick. (slice A choice) Returns the tab ID at once (32 lower-case hex
//       digits of the buffer FGuid; an ID is allocated now for a file that is not open yet), because §12.2 open()
//       returns it while §12.1.1 says None.
//   new_tab(text: str, name: str) -> str
//       Creates a scratch tab ("" name = Untitled-N); returns its tab ID now; the tab appears at run end.
//   enqueue_run(spec_json: str) -> int
//       spec: {"target": "<tab id | tab name | file path>", "namespace": "session" | "isolated" | null,
//              "transaction": true | false | null}. Route Api. Returns the run number. RuntimeError inside a run
//       (one run at a time, D14(4)), or when the target is unknown, or when the request is refused.
//   suppress(flags: dict[str, bool]) -> dict[str, bool]
//       Keys "results", "info", "warnings", "errors", "echo" (others -> ValueError; a non-bool value -> TypeError).
//       The §12.2 polarity: True suppresses (hides) the kind, False shows it, an absent key leaves it, so
//       suppress(echo=True) turns Echo input off (T-OUT-043). Returns the previous state of every key, same polarity
//       (True = was suppressed). C++ stores visibilities (FDreamOutputVisibility) and inverts. Applies to every output
//       view, the Run window's live run view included, at the next drain or pump.
//
//  Live resolver helpers (§7.5.2, N1, RV-21)                                          GT
//   hard_object_props(py_class_name: str) -> frozenset[str]
//       The normalised names (lower case, no '_') of the hard object, class and weak object properties of the class
//       FindFirstObjectSafe<UClass>(py_class_name, NativeFirst) finds, super classes included: FObjectProperty (which
//       covers FClassProperty) and FWeakObjectProperty; never soft, soft-class or lazy. Name: the ScriptName metadata's
//       first item, else the C++ name, without the "b"/"In" prefixes PythonScriptPlugin strips. Two properties with the
//       same normalised name drop both. Cached per native class (FObjectKey); a Blueprint class is read each time. A name
//       that is not an existing FName (looked up with FNAME_Find, so none is added) or is NAME_SIZE characters or longer
//       finds nothing. TypeError for a non-str or a wrong argument count; every other failure (unknown class, off the game
//       thread, saving or GC) is an empty frozenset ("do not read").
//   container_element_kind(value: object) -> str | None
//       The element property's class name of an exact unreal.Array/Map/Set wrapper, else None ("do not read"). (run group
//       choice) Returns None for every value in M1: the wrapper types are private to PythonScriptPlugin. Deferred (M2):
//       reading ArrayProp->Inner, the set's element property and the map's key-or-value property.
//
//  (debug, PO-06 F5) The pause port (§8.1.5, §8.3, §8.4)                                GT
//   pause_context(run_id: int) -> int
//       The C++ pause rules of §8.4 (4-11) as an EDreamPauseRefusal code (Debug/DreamDebugger.h); 0 when a pause may open
//       now. 1 (NotDebugRun) for a run that is not the active run. Allocates nothing. TypeError for a non-int.
//   pause(run_id: int, stop_json: str) -> tuple[str, str]
//       The debugger's stop (the trace callback or the exception stop): opens the Paused window's blocking modal loop
//       with the GIL released (§8.3.1; the one _dream function that releases the GIL, GIL-3) and returns
//       (command, args_json): "continue", "next", "stepIn", "stepOut" or "terminate"; ("refused", "<code>") when a pause
//       rule refuses or the window could not open. Stop at a line stop raises ScriptCancelled instead (counted as a
//       delivery, §8.5); at an exception stop (reason "exception") it returns ("terminate", "{}"). RuntimeError for a
//       run that is not the active run; TypeError for a non-str stop_json.
//   is_game_thread() -> bool                                                         any thread
//   object_props(path_name: str) -> list[tuple[str, str, str]]
//       (debug choice, §8.8 RV-21) The editor properties of the object at path_name as (name, C++ type, export text),
//       read with FProperty::ExportTextItem_Direct: no getter runs and no soft reference loads; [] for an unknown path,
//       off the game thread or while saving or collecting garbage. At most 300 properties. TypeError for a non-str.
//
//  Constants
//   ABI_VERSION = 1; GENERATION (the Python generation); HEADLESS (bool);
//   STREAM_STDOUT 1, STREAM_STDERR 2, STREAM_RESULT 3, STREAM_NOTICE 4, STREAM_INFO 5, STREAM_WARNING 6,
//   STREAM_ERROR 7, STREAM_AGENT 8, STREAM_DEBUGGER 9, STREAM_LOGPOINT 10, STREAM_EVALUATE 11.
//
//  Not in this table (§12.1.1 rows owned by later milestones): post_report (M3), task_register (M3), eval_begin,
//  eval_end (supervised evaluates, M3), container_export_text, struct_export_text, soft_props and
//  prop_export_text (the §8 Variables view, M2/M3). stub_emit and stub_module_done
//  (§7.4.2) are not added: the Stage 1 snapshot helper (_impl.stubs.step, lang group) returns each slice's output to
//  C++, which writes the files on a background task.
//
// ================================================================================================================
// Helper contracts (§12.1.3), called by FDreamPython only from FDreamRunService's OnEndFrame handler or idle ticks.
//
//   ExecPythonCommandEx(EvaluateStatement): __import__('dream_editor')._boot(globals(), <generation>) -> bool
//       Imports the runner core, checks _dream.ABI_VERSION, registers ScriptCancelled, captures the Session (the
//       console dict) and its key set; never writes the name dream_editor into the Session (RT-15).
//       Ruling R3: returns exactly True on success; on an ABI mismatch it raises ImportError("Dream Python package and
//       native module do not match; reinstall Dream"); on any other failure it logs the reason to stderr (LogPython) and
//       returns False. FDreamPython::Boot: CommandResult == "True" is Ready; a failed ExecPythonCommandEx is BootFailed
//       with the last line of CommandResult in the banner (the whole text in LogDream); anything else is BootFailed
//       with _boot's "Dream did not start: <reason>" stderr line from LogOutput in the banner, else "returned <repr>;
//       see LogPython".
//   ExecPythonCommandEx(EvaluateStatement): __import__('dream_editor')._run(<run_id>) -> None (never raises, INV-4)
//   dream_editor._shutdown(generation: int) -> None                                    best effort
//   dream_editor._impl.runner.prepare(run_id: int, request: dict) -> dict
//       request (built by FDreamPython::Prepare from the record and the request):
//         "run_id": int, "run_number": int,
//         "route": str            DreamRunText::RouteKey ("selection", "line", "buffer", "repl", "console", "run_file", "api")
//         "text": str             the code (a selection extended to its first line's start)
//         "first_line": int       1-based buffer line of text's first line (RUN-06 offset)
//         "filename": str         real path, "<dream:Tab Name#id>", "<dream:REPL#N>" or "<dream:Console#N>"
//         "linecache_text": str | None   whole buffer registered in linecache under filename (None: register text)
//         "file_path": str | None real path when the buffer is saved (for __file__; Run File's sys.path entry)
//         "dirty": bool
//         "namespace": "session" | "isolated"
//         "autoprint": bool       == "statement_mode" for slice A routes (§5.2.1)
//         "statement_mode": bool
//         "argv": list[str]       Run File only (RUN-14)
//         "run_file": bool        (slice A choice) the Run File semantics of §5.3.4 (sys.path entry inserted and removed
//                                 by identity, sys.argv = [path, *argv]): True for route "run_file" and for an API run
//                                 that targets a file (EnqueueApiRun with a path: route "api", no buffer, a file path).
//                                 Optional for the runner; its default is route == "run_file".
//         "tab_title": str
//       returns {"ok": True, "statements": [[first_line, last_line], ...] | None}
//            or {"ok": False, "error": {"msg": str, "lineno": int, "offset": int, "end_offset": int, "text": str,
//                                       "type": str}}
//       Buffer coordinates, 1-based. "text" is the whole buffer line lineno. "offset" and "end_offset" are always
//       1-based UTF-8 byte columns in "text" (the runner converts the parser's character offsets), and end_offset >
//       offset (end of line + 1 when the error spans lines). "type" is the class name: SyntaxError, IndentationError,
//       TabError, or ValueError, RecursionError, OverflowError for a non-syntax compile failure; C++ prints
//       "<type>: <msg>" in the not executed block (FDreamSyntaxError::TypeName, default "SyntaxError").
//       Compiling executes no user code (D14(5)).
//   dream_editor._impl.runner.discard(run_id: int) -> None
//   dream_editor._impl.runner.on_buffer_saved(path: str) -> None                       (slice A choice, ruling R2)
//       A dirty buffer with a real path was saved: drops the linecache entry for path if it is still the tuple the
//       runner wrote (§5.3.2 entry lifetime). Called from an FDreamDeferral idle tick (FDreamPython::NotifyBufferSaved).
//   dream_editor._impl.runner.classify_input(source: str, last_logical_indented: bool) -> "complete" | "incomplete" | "invalid"
//   dream_editor._impl.streams.emergency_restore() -> None
//   dream_editor._impl.namespaces.reset_session() -> list[str]                         (slice A choice: RT-15 as a helper)
// ================================================================================================================
#pragma once

#include "Python/DreamPythonInclude.h"
#include "Run/DreamRunTypes.h"

#if WITH_PYTHON

namespace DreamNative
{
	inline constexpr int32 AbiVersion = 1;
	inline constexpr int32 FirstStream = 1;          // STREAM_STDOUT
	inline constexpr int32 LastStream = 11;          // STREAM_EVALUATE

	/**
	 * Builds the module with PyModule_Create (single-phase PyModuleDef) and inserts it as sys.modules['_dream'], once per
	 * generation (§12.1). GIL held, game thread, from FDreamPython::HandleInitialized. Returns false with a reason.
	 */
	DREAMEDITOR_API bool CreateModule(uint32 Generation, bool bHeadless, FText& OutReason);

	/** HandleShutdown: drops the cancel class and writer state; later calls through stale objects raise RuntimeError. GIL held. */
	DREAMEDITOR_API void DestroyModule();

	/** The class from register_cancel_class (borrowed), or null before boot. GIL held. */
	DREAMEDITOR_API PyObject* GetCancelClass();

	/**
	 * A new ScriptCancelled instance with run_number, source and delivery set (§5.7.1). GIL held.
	 * Returns a new reference, or null with a Python error set.
	 */
	DREAMEDITOR_API PyObject* NewCancelledInstance(int32 RunNumber, const TCHAR* Source, int32 Delivery);

	/**
	 * "Ours" (§5.7.1): an instance of the registered class whose run_number is RunNumber or 0, found on Exc itself or in
	 * its __context__ / __cause__ chain up to 16 links (§5.3.9, §5.7.4). GIL held.
	 */
	DREAMEDITOR_API bool IsOurCancel(PyObject* Exc, int32 RunNumber);

	/** PyThread_get_thread_ident() of the game thread, captured at CreateModule (§11.7 thread check). */
	DREAMEDITOR_API unsigned long GetGameThreadIdent();
}

#endif // WITH_PYTHON
