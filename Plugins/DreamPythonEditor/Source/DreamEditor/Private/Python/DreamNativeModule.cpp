// SPDX-License-Identifier: Apache-2.0
// The _dream native module (TDD §12.1, D7, GIL-2): the slice-A table of DreamNativeModule.h. Every function is called from
// Python with the GIL held; none releases it or waits. The only Python calls are the ones the contract names: str() in
// the log shims, the cancel-class constructor, and run_statement's PyEval_EvalCode of the user's statement (§5.3.7).
#include "Python/DreamNativeModule.h"

#include "Debug/DreamDebugger.h"                    // (debug)
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Misc/Char.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Output/DreamOutputTypes.h"
#include "Python/DreamNativeInternal.h"
#include "Python/DreamPyUtil.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "UObject/Class.h"
#include "UObject/GarbageCollection.h"            // (debug) object_props
#include "UObject/ObjectKey.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "dream/Json.h"

#include <string>

#if WITH_PYTHON

namespace DreamNativeInternal
{
	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	bool ParseStream(PyObject* Object, int32& OutStream, bool bWriterStreamsOnly)
	{
		if (!DreamPy::IsStrictInt(Object))
		{
			PyErr_Format(PyExc_TypeError, "stream must be an int STREAM_* constant, not %.100s", Py_TYPE(Object)->tp_name);
			return false;
		}
		const long Value = PyLong_AsLong(Object);
		if (Value == -1 && PyErr_Occurred())
		{
			PyErr_Clear();
		}
		const bool bAccepted = bWriterStreamsOnly
			? (Value == static_cast<long>(EDreamStream::Stdout) || Value == static_cast<long>(EDreamStream::Stderr))
			: (Value >= DreamNative::FirstStream && Value <= DreamNative::LastStream);
		if (!bAccepted)
		{
			PyErr_Format(PyExc_ValueError, bWriterStreamsOnly
				? "%ld is not STREAM_STDOUT or STREAM_STDERR"
				: "%ld is not an accepted STREAM_* constant", Value);
			return false;
		}
		OutStream = static_cast<int32>(Value);
		return true;
	}

	bool ParseWriteRunId(PyObject* Object, FDreamRunId& OutId)
	{
		if (!DreamPy::IsStrictInt(Object))
		{
			PyErr_Format(PyExc_TypeError, "run_id must be an int, not %.100s", Py_TYPE(Object)->tp_name);
			return false;
		}
		const unsigned long long Value = PyLong_AsUnsignedLongLong(Object);
		if (PyErr_Occurred())
		{
			PyErr_Clear();
			OutId = UnknownRunId;
			return true;
		}
		OutId = static_cast<FDreamRunId>(Value);
		return true;
	}

	void PushText(int32 Stream, FDreamRunId RunId, const UTF8CHAR* Utf8, int32 Bytes)
	{
		if (FDreamOutputStore* Store = State().OutputStore)
		{
			// (output group, B90) The writer's PyThreadState* keys the thread table with the native ID (§6.1). GIL held.
			Store->PushText(RunId, static_cast<EDreamStream>(Stream), EDreamOutputOrigin::Python, Utf8, Bytes,
				reinterpret_cast<uint64>(PyThreadState_Get()));
		}
	}
}

namespace DreamNativePrivate
{
	using DreamNativeInternal::FState;
	using DreamNativeInternal::State;

	thread_local TArray<UTF8CHAR> GLogScratch;

	/** The public names of the API commands, for the §11.7 thread-check text. */
	const char* PublicName(const char* NativeName)
	{
		static const char* const Pairs[][2] = {
			{ "open_tab", "dream_editor.open" },
			{ "new_tab", "dream_editor.new_tab" },
			{ "enqueue_run", "dream_editor.run" },
			{ "suppress", "dream_editor.suppress" },
			{ "clear_output", "dream_editor.clear_output" },
		};
		for (const auto& Pair : Pairs)
		{
			if (FCStringAnsi::Strcmp(NativeName, Pair[0]) == 0)
			{
				return Pair[1];
			}
		}
		return nullptr;
	}

	/** GT-only functions (§12.1 errors, §11.7 thread check). Also refuses a module whose generation has ended. */
	bool RequireGameThread(const char* Name)
	{
		const FState& S = State();
		if (!S.bAlive || S.RunService == nullptr)
		{
			PyErr_Format(PyExc_RuntimeError, "_dream.%s(): the Python session this module belongs to has ended", Name);
			return false;
		}
		if (PyThread_get_thread_ident() != S.GameThreadIdent)
		{
			if (const char* Public = PublicName(Name))
			{
				PyErr_Format(PyExc_RuntimeError, "%s() must be called on the game thread", Public);
			}
			else
			{
				PyErr_Format(PyExc_RuntimeError, "_dream.%s() must be called on the game thread", Name);
			}
			return false;
		}
		return true;
	}

	bool IsGameThreadCaller()
	{
		return PyThread_get_thread_ident() == State().GameThreadIdent;
	}

	/** A run_id that must be the active entry: TypeError for a non-int, RuntimeError for a stale or foreign ID. */
	bool ParseActiveRunId(PyObject* Object, const char* Name, FDreamRunId& OutId)
	{
		if (!DreamPy::IsStrictInt(Object))
		{
			PyErr_Format(PyExc_TypeError, "%s(): run_id must be an int, not %.100s", Name, Py_TYPE(Object)->tp_name);
			return false;
		}
		const unsigned long long Value = PyLong_AsUnsignedLongLong(Object);
		if (PyErr_Occurred())
		{
			PyErr_Clear();
		}
		const FDreamRunId Active = State().RunService ? State().RunService->GetActiveRunId() : 0;
		if (Active == 0 || static_cast<FDreamRunId>(Value) != Active)
		{
			PyErr_Format(PyExc_RuntimeError, "%s(): run %llu is not the active run", Name, Value);
			return false;
		}
		OutId = Active;
		return true;
	}

	bool ParseStr(PyObject* Object, const char* Function, const char* Argument, FString& Out)
	{
		if (!PyUnicode_Check(Object))
		{
			PyErr_Format(PyExc_TypeError, "%s(): %s must be str, not %.100s", Function, Argument, Py_TYPE(Object)->tp_name);
			return false;
		}
		return DreamPy::ToString(Object, Out);
	}

	bool ParseInt(PyObject* Object, const char* Function, const char* Argument, int64& Out)
	{
		if (!DreamPy::IsStrictInt(Object))
		{
			PyErr_Format(PyExc_TypeError, "%s(): %s must be an int, not %.100s", Function, Argument, Py_TYPE(Object)->tp_name);
			return false;
		}
		Out = PyLong_AsLongLong(Object);
		return !(Out == -1 && PyErr_Occurred());
	}

	FText ToText(const char* Utf8)
	{
		return FText::FromString(FString(UTF8_TO_TCHAR(Utf8)));
	}

	// ---- Output (§6.1, §6.2) -----------------------------------------------------------------------------------

	PyObject* Write(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!DreamPy::CheckArgCount("write", NArgs, 3))
		{
			return nullptr;
		}
		int32 Stream = 0;
		FDreamRunId RunId = 0;
		if (!DreamNativeInternal::ParseStream(Args[0], Stream, /*bWriterStreamsOnly*/ false)
			|| !DreamNativeInternal::ParseWriteRunId(Args[1], RunId))
		{
			return nullptr;
		}
		// §6.1: STREAM_AGENT only while the current run's route is agent or bridge (1.1); no 1.0 route is one (B33).
		if (Stream == static_cast<int32>(EDreamStream::Agent)
			&& (State().RunService == nullptr || !State().RunService->IsActiveRunAgentOrBridge()))
		{
			PyErr_SetString(PyExc_ValueError, "STREAM_AGENT (8) is accepted only during an agent or bridge run");
			return nullptr;
		}
		if (!PyUnicode_Check(Args[2]))
		{
			PyErr_Format(PyExc_TypeError, "write() argument must be str, not %.100s", Py_TYPE(Args[2])->tp_name);
			return nullptr;
		}
		const UTF8CHAR* Utf8 = nullptr;
		int32 Bytes = 0;
		if (DreamPy::EncodeUtf8Replacing(Args[2], GLogScratch, Utf8, Bytes) && Bytes > 0)
		{
			DreamNativeInternal::PushText(Stream, RunId, Utf8, Bytes);
		}
		return DreamPy::NewNone();
	}

	PyObject* ThreadLabel(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!DreamPy::CheckArgCount("thread_label", NArgs, 2))
		{
			return nullptr;
		}
		int64 Ident = 0;
		FString Name;
		if (!ParseInt(Args[0], "thread_label", "ident", Ident) || !ParseStr(Args[1], "thread_label", "name", Name))
		{
			return nullptr;
		}
		if (FDreamOutputStore* Store = State().OutputStore)
		{
			Store->SetThreadLabel(static_cast<uint32>(Ident), Name);
		}
		return DreamPy::NewNone();
	}

	/** The engine's ShouldLog: empty or whitespace-only text is not logged (RT-07, §6.1 step 2). */
	bool ShouldLog(PyObject* Text)
	{
		const Py_ssize_t Length = PyUnicode_GET_LENGTH(Text);
		const int Kind = PyUnicode_KIND(Text);
		const void* Data = PyUnicode_DATA(Text);
		for (Py_ssize_t Index = 0; Index < Length; ++Index)
		{
			const uint32 CodePoint = PyUnicode_READ(Kind, Data, Index);
			if (CodePoint > 0xFFFF || !FChar::IsWhitespace(static_cast<TCHAR>(CodePoint)))
			{
				return true;
			}
		}
		return false;
	}

	/** §6.1 "RT-07 log-shim emit path": str(obj), ShouldLog, one info/warning/error record with origin UnrealLog. */
	PyObject* EmitLog(int32 Verbosity, PyObject* Object)
	{
		PyObject* Text = PyObject_Str(Object);
		if (Text == nullptr)
		{
			return nullptr;
		}
		if (PyUnicode_READY(Text) < 0)
		{
			Py_DECREF(Text);
			return nullptr;
		}
		if (ShouldLog(Text))
		{
			const UTF8CHAR* Utf8 = nullptr;
			int32 Bytes = 0;
			DreamPy::EncodeUtf8Replacing(Text, GLogScratch, Utf8, Bytes);

			static constexpr EDreamStream Streams[] = { EDreamStream::Info, EDreamStream::Warning, EDreamStream::Error };
			FDreamRawRecord Record;
			Record.Kind = EDreamRawKind::Text;
			// The game thread writes for the active run; another thread is attributed by the ingest thread table (§6.1).
			Record.RunId = IsGameThreadCaller() && State().RunService ? State().RunService->GetActiveRunId() : 0;
			Record.Stream = Streams[Verbosity];
			Record.Origin = EDreamOutputOrigin::UnrealLog;
			Record.Flags = static_cast<uint16>(dream::OLF_FromUnrealLog);
			Record.NativeThreadId = FPlatformTLS::GetCurrentThreadId();
			Record.ThreadState = reinterpret_cast<uint64>(PyThreadState_Get());     // (output group, B90) §6.1
			Record.Time = FPlatformTime::Seconds();
			Record.Text.Reserve(Bytes + 1);
			Record.Text.Append(Utf8, Bytes);
			Record.Text.Add(static_cast<UTF8CHAR>('\n'));
			if (FDreamOutputStore* Store = State().OutputStore)
			{
				Store->Push(MoveTemp(Record));
			}
		}
		Py_DECREF(Text);
		return DreamPy::NewNone();
	}

	PyObject* LogShim(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!DreamPy::CheckArgCount("log_shim", NArgs, 2))
		{
			return nullptr;
		}
		int64 Verbosity = 0;
		if (!ParseInt(Args[0], "log_shim", "verbosity", Verbosity))
		{
			return nullptr;
		}
		if (Verbosity < 0 || Verbosity > 2)
		{
			PyErr_Format(PyExc_ValueError, "verbosity %lld is not 0, 1 or 2", static_cast<long long>(Verbosity));
			return nullptr;
		}
		return EmitLog(static_cast<int32>(Verbosity), Args[1]);
	}

	PyObject* ShimLog(PyObject*, PyObject* Object)
	{
		return EmitLog(0, Object);
	}
	PyObject* ShimLogWarning(PyObject*, PyObject* Object)
	{
		return EmitLog(1, Object);
	}
	PyObject* ShimLogError(PyObject*, PyObject* Object)
	{
		return EmitLog(2, Object);
	}

	PyObject* CaptureBegin(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("capture_begin") || !DreamPy::CheckArgCount("capture_begin", NArgs, 2))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		if (!ParseActiveRunId(Args[0], "capture_begin", Id))
		{
			return nullptr;
		}
		PyObject* Sequence = PySequence_Fast(Args[1], "capture_begin(): alive_native_ids must be a tuple or list of ints");
		if (Sequence == nullptr)
		{
			return nullptr;
		}
		TArray<uint64> Alive;
		const Py_ssize_t Count = PySequence_Fast_GET_SIZE(Sequence);
		Alive.Reserve(static_cast<int32>(Count));
		for (Py_ssize_t Index = 0; Index < Count; ++Index)
		{
			PyObject* Item = PySequence_Fast_GET_ITEM(Sequence, Index);
			if (!DreamPy::IsStrictInt(Item))
			{
				Py_DECREF(Sequence);
				PyErr_SetString(PyExc_TypeError, "capture_begin(): alive_native_ids must hold ints");
				return nullptr;
			}
			const unsigned long long Value = PyLong_AsUnsignedLongLong(Item);
			if (PyErr_Occurred())
			{
				PyErr_Clear();
				continue;
			}
			Alive.Add(static_cast<uint64>(Value));
		}
		Py_DECREF(Sequence);
		State().OutputStore->CaptureBegin(Id, Alive);
		return DreamPy::NewNone();
	}

	PyObject* CaptureEnd(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("capture_end") || !DreamPy::CheckArgCount("capture_end", NArgs, 1))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		if (!ParseActiveRunId(Args[0], "capture_end", Id))
		{
			return nullptr;
		}
		State().OutputStore->CaptureEnd(Id);
		return DreamPy::NewNone();
	}

	PyObject* ClearOutput(PyObject*, PyObject*)
	{
		if (!RequireGameThread("clear_output"))
		{
			return nullptr;
		}
		// Allowed during a run: the store applies it in queue order at the next drain, keeping the active header (§6.4).
		State().OutputStore->Clear();
		return DreamPy::NewNone();
	}

	// ---- Run handshake (§5.3, §5.7) ----------------------------------------------------------------------------

	PyObject* CurrentRunId(PyObject*, PyObject*)
	{
		const FDreamRunService* Runs = State().RunService;
		return PyLong_FromUnsignedLongLong(Runs ? Runs->GetActiveRunId() : 0);
	}

	PyObject* UserCodeBegin(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("user_code_begin") || !DreamPy::CheckArgCount("user_code_begin", NArgs, 1))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		if (!ParseActiveRunId(Args[0], "user_code_begin", Id))
		{
			return nullptr;
		}
		// Captures the native entry mark and opens delivery and the pump (§2.6.3, INV-7). False: Stop came first.
		const bool bProceed = State().Supervisor->UserCodeBegin(Id);
		State().RunService->OnUserCodeWindow(Id, true);
		return DreamPy::NewBool(bProceed);
	}

	PyObject* UserCodeDone(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("user_code_done") || !DreamPy::CheckArgCount("user_code_done", NArgs, 1))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		if (!ParseActiveRunId(Args[0], "user_code_done", Id))
		{
			return nullptr;
		}
		// Idempotent; never waits for the watchdog (GIL-6).
		State().Supervisor->UserCodeDone(Id);
		State().RunService->OnUserCodeWindow(Id, false);
		return DreamPy::NewNone();
	}

	PyObject* RunStatement(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("run_statement") || !DreamPy::CheckArgCount("run_statement", NArgs, 4))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		int64 Index = 0;
		if (!ParseActiveRunId(Args[0], "run_statement", Id) || !ParseInt(Args[1], "run_statement", "index", Index))
		{
			return nullptr;
		}
		PyObject* Code = Args[2];
		PyObject* Namespace = Args[3];
		if (Code != Py_None && !PyCode_Check(Code))
		{
			PyErr_Format(PyExc_TypeError, "run_statement(): code must be a code object or None, not %.100s", Py_TYPE(Code)->tp_name);
			return nullptr;
		}
		if (!PyDict_Check(Namespace))
		{
			PyErr_Format(PyExc_TypeError, "run_statement(): ns must be a dict, not %.100s", Py_TYPE(Namespace)->tp_name);
			return nullptr;
		}

		FDreamRunService& Runs = *State().RunService;
		FString Error;
		bool bValueError = false;
		const int32 StatementIndex = Index >= 0 && Index <= MAX_int32 ? static_cast<int32>(Index) : -1;
		if (!Runs.ValidateStatement(Id, StatementIndex, Error, bValueError))
		{
			PyErr_SetString(bValueError ? PyExc_ValueError : PyExc_RuntimeError, TCHAR_TO_UTF8(*Error));
			return nullptr;
		}

		// (1) Stop requested: a new ScriptCancelled, counted as a delivery (§5.7.3); nothing of this statement runs.
		if (State().Supervisor->RaiseCancelIfStopRequested(Id))
		{
			return nullptr;
		}
		// (2) started = index + 1, (3) the statement's echo lines from the source snapshot (OUT-12, §6.2).
		if (!Runs.BeginStatement(Id, StatementIndex, Error, bValueError))
		{
			PyErr_SetString(bValueError ? PyExc_ValueError : PyExc_RuntimeError, TCHAR_TO_UTF8(*Error));
			return nullptr;
		}
		// (4) exec's rules: __builtins__ inserted when the dict lacks it, globals and locals the same dict.
		if (Code != Py_None)
		{
			if (PyDict_GetItemString(Namespace, "__builtins__") == nullptr)
			{
				if (PyDict_SetItemString(Namespace, "__builtins__", PyEval_GetBuiltins()) != 0)
				{
					return nullptr;
				}
			}
			PyObject* Result = PyEval_EvalCode(Code, Namespace, Namespace);
			if (Result == nullptr)
			{
				return nullptr;     // propagates with the returned count unchanged
			}
			Py_DECREF(Result);
		}
		// (5) returned = index + 1, before control is back in the runner's frame (§5.3.7 "Where a Stop lands").
		Runs.EndStatement(Id, StatementIndex);
		return DreamPy::NewNone();
	}

	PyObject* StatementCounts(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("statement_counts") || !DreamPy::CheckArgCount("statement_counts", NArgs, 1))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		if (!ParseActiveRunId(Args[0], "statement_counts", Id))
		{
			return nullptr;
		}
		int32 Started = 0;
		int32 Returned = 0;
		if (!State().RunService->GetStatementCounts(Id, Started, Returned))
		{
			PyErr_Format(PyExc_RuntimeError, "statement_counts(): run %llu is not the active run", static_cast<unsigned long long>(Id));
			return nullptr;
		}
		return Py_BuildValue("(ii)", Started, Returned);
	}

	PyObject* ReportOutcome(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("report_outcome") || !DreamPy::CheckArgCount("report_outcome", NArgs, 4))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		if (!ParseActiveRunId(Args[0], "report_outcome", Id))
		{
			return nullptr;
		}

		// Malformed input becomes outcome error with "internal: malformed outcome" (§12.1.1), never an exception.
		bool bWellFormed = DreamPy::IsStrictInt(Args[1]) && PyUnicode_Check(Args[2]);
		long OutcomeValue = 0;
		if (bWellFormed)
		{
			OutcomeValue = PyLong_AsLong(Args[1]);
			if (PyErr_Occurred())
			{
				PyErr_Clear();
				bWellFormed = false;
			}
			bWellFormed = bWellFormed && OutcomeValue >= static_cast<long>(EDreamRunOutcome::Ok)
				&& OutcomeValue <= static_cast<long>(EDreamRunOutcome::NotExecuted);
		}
		FString Detail;
		if (bWellFormed && !DreamPy::ToString(Args[2], Detail))
		{
			PyErr_Clear();
			bWellFormed = false;
		}
		TOptional<FDreamRunLocation> Location;
		PyObject* LocationObject = Args[3];
		if (bWellFormed && LocationObject != Py_None)
		{
			bWellFormed = PyTuple_Check(LocationObject) && PyTuple_GET_SIZE(LocationObject) == 2
				&& PyUnicode_Check(PyTuple_GET_ITEM(LocationObject, 0)) && DreamPy::IsStrictInt(PyTuple_GET_ITEM(LocationObject, 1));
			if (bWellFormed)
			{
				FDreamRunLocation Where;
				const long Line = PyLong_AsLong(PyTuple_GET_ITEM(LocationObject, 1));
				if (PyErr_Occurred() || !DreamPy::ToString(PyTuple_GET_ITEM(LocationObject, 0), Where.File))
				{
					PyErr_Clear();
					bWellFormed = false;
				}
				else
				{
					Where.Line = Line > 0 && Line <= MAX_int32 ? static_cast<int32>(Line) : 0;
					Location = MoveTemp(Where);
				}
			}
		}

		EDreamRunOutcome Outcome = static_cast<EDreamRunOutcome>(OutcomeValue);
		if (!bWellFormed)
		{
			Outcome = EDreamRunOutcome::Error;
			Detail = TEXT("internal: malformed outcome");
			Location.Reset();
		}
		if (!State().RunService->ReportOutcome(Id, Outcome, Detail, Location))
		{
			PyErr_Format(PyExc_RuntimeError, "report_outcome(): run %llu is not the active run", static_cast<unsigned long long>(Id));
			return nullptr;
		}
		return DreamPy::NewNone();
	}

	PyObject* RegisterCancelClass(PyObject*, PyObject* Class)
	{
		if (!RequireGameThread("register_cancel_class"))
		{
			return nullptr;
		}
		if (!PyType_Check(Class) || !PyType_IsSubtype(reinterpret_cast<PyTypeObject*>(Class), reinterpret_cast<PyTypeObject*>(PyExc_KeyboardInterrupt)))
		{
			PyErr_SetString(PyExc_TypeError, "register_cancel_class() needs a subclass of KeyboardInterrupt");
			return nullptr;
		}
		// Strong reference until OnPythonShutdown (§5.7.1, D16).
		State().CancelClass = FDreamPyRef::Borrow(Class);
		return DreamPy::NewNone();
	}

	PyObject* CheckCancel(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		// Any thread, never raises ScriptCancelled (A19). check_cancel(True) is dream_editor.check_cancel() asking, on the
		// game thread, whether it may raise: True only when no §5.4.3 deferral condition holds, and that True is counted as
		// a delivery by the supervisor (§5.10.2 deferral row, B34).
		if (NArgs > 1)
		{
			PyErr_Format(PyExc_TypeError, "check_cancel() takes at most 1 argument (%zd given)", NArgs);
			return nullptr;
		}
		bool bDeliver = false;
		if (NArgs == 1)
		{
			if (!PyBool_Check(Args[0]))
			{
				PyErr_Format(PyExc_TypeError, "check_cancel(): deliver must be a bool, not %.100s", Py_TYPE(Args[0])->tp_name);
				return nullptr;
			}
			bDeliver = Args[0] == Py_True;
		}
		FRunSupervisor* Supervisor = State().Supervisor;
		return DreamPy::NewBool(Supervisor != nullptr && Supervisor->CheckCancel(bDeliver));
	}

	PyObject* StopRequested(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!DreamPy::CheckArgCount("stop_requested", NArgs, 1))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		if (!DreamNativeInternal::ParseWriteRunId(Args[0], Id))
		{
			return nullptr;
		}
		const FRunSupervisor* Supervisor = State().Supervisor;
		return DreamPy::NewBool(Supervisor != nullptr && Id != 0 && Supervisor->IsStopRequested(Id));
	}

	PyObject* Progress(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!DreamPy::CheckArgCount("progress", NArgs, 3))
		{
			return nullptr;
		}
		if (!DreamPy::IsStrictInt(Args[0]) || (Args[1] != Py_None && !DreamPy::IsStrictInt(Args[1])))
		{
			PyErr_SetString(PyExc_TypeError, "progress(i: int, n: int | None, msg: str)");
			return nullptr;
		}
		FString Message;
		if (!ParseStr(Args[2], "progress", "msg", Message))
		{
			return nullptr;
		}
		const long long Done = PyLong_AsLongLong(Args[0]);
		if (Done == -1 && PyErr_Occurred())
		{
			return nullptr;
		}
		TOptional<int64> Total;
		if (Args[1] != Py_None)
		{
			const long long N = PyLong_AsLongLong(Args[1]);
			if (N == -1 && PyErr_Occurred())
			{
				return nullptr;
			}
			Total = static_cast<int64>(N);
		}
		if (Done < 0 || (Total.IsSet() && Done > Total.GetValue()))
		{
			if (Total.IsSet())
			{
				PyErr_Format(PyExc_ValueError, "progress(%lld, %lld): need 0 <= i <= n", Done, static_cast<long long>(Total.GetValue()));
			}
			else
			{
				PyErr_Format(PyExc_ValueError, "progress(%lld, None): need 0 <= i", Done);
			}
			return nullptr;
		}
		const FDreamRunId Active = State().RunService ? State().RunService->GetActiveRunId() : 0;
		if (Active != 0 && State().Supervisor != nullptr)
		{
			State().Supervisor->SetProgress(Active, static_cast<int64>(Done), Total, Message);
		}
		return DreamPy::NewNone();
	}

	// ---- Public API commands (§11.7, §12.2) --------------------------------------------------------------------

	PyObject* OpenTab(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("open_tab") || !DreamPy::CheckArgCount("open_tab", NArgs, 3))
		{
			return nullptr;
		}
		FString Path;
		int64 Line = 0;
		int64 Column = 0;
		if (!ParseStr(Args[0], "open_tab", "path", Path) || !ParseInt(Args[1], "open_tab", "line", Line)
			|| !ParseInt(Args[2], "open_tab", "col", Column))
		{
			return nullptr;
		}
		// Queued to run end inside a run, applied at the next idle tick otherwise (FDreamEditorModule::OpenLocationFromApi).
		const FString TabId = State().Module->OpenLocationFromApi(Path, static_cast<int32>(FMath::Clamp<int64>(Line, 0, MAX_int32)),
			static_cast<int32>(FMath::Clamp<int64>(Column, 0, MAX_int32)));
		if (TabId.IsEmpty())
		{
			PyErr_Format(PyExc_RuntimeError, "dream_editor.open(): cannot open %s", TCHAR_TO_UTF8(*Path));
			return nullptr;
		}
		return DreamPy::FromString(TabId);
	}

	PyObject* NewTab(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("new_tab") || !DreamPy::CheckArgCount("new_tab", NArgs, 2))
		{
			return nullptr;
		}
		FString Text;
		FString Name;
		if (!ParseStr(Args[0], "new_tab", "text", Text) || !ParseStr(Args[1], "new_tab", "name", Name))
		{
			return nullptr;
		}
		Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		const FString TabId = State().Module->NewScratchTab(Text, Name);
		if (TabId.IsEmpty())
		{
			PyErr_SetString(PyExc_RuntimeError, "dream_editor.new_tab(): the tab could not be created");
			return nullptr;
		}
		return DreamPy::FromString(TabId);
	}

	PyObject* EnqueueRun(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("enqueue_run") || !DreamPy::CheckArgCount("enqueue_run", NArgs, 1))
		{
			return nullptr;
		}
		FString SpecText;
		if (!ParseStr(Args[0], "enqueue_run", "spec_json", SpecText))
		{
			return nullptr;
		}
		dream::json::Value Spec;
		std::string ParseError;
		if (!dream::json::Parse(DreamText::ToUtf8(SpecText), Spec, &ParseError) || !Spec.IsObject())
		{
			PyErr_Format(PyExc_ValueError, "enqueue_run(): spec is not a JSON object (%s)", ParseError.c_str());
			return nullptr;
		}
		const dream::json::Value& TargetValue = Spec.Get("target");
		if (TargetValue.GetKind() != dream::json::Kind::String || TargetValue.AsString().empty())
		{
			PyErr_SetString(PyExc_ValueError, "enqueue_run(): spec needs a non-empty \"target\"");
			return nullptr;
		}
		TOptional<EDreamNamespaceMode> Namespace;
		const dream::json::Value& NamespaceValue = Spec.Get("namespace");
		if (NamespaceValue.GetKind() == dream::json::Kind::String)
		{
			const std::string& Mode = NamespaceValue.AsString();
			if (Mode == "session")
			{
				Namespace = EDreamNamespaceMode::Session;
			}
			else if (Mode == "isolated")
			{
				Namespace = EDreamNamespaceMode::Isolated;
			}
			else
			{
				PyErr_Format(PyExc_ValueError, "enqueue_run(): namespace must be \"session\" or \"isolated\", not \"%s\"", Mode.c_str());
				return nullptr;
			}
		}
		else if (!NamespaceValue.IsNull())
		{
			PyErr_SetString(PyExc_ValueError, "enqueue_run(): namespace must be \"session\", \"isolated\" or null");
			return nullptr;
		}
		TOptional<bool> Transaction;
		const dream::json::Value& TransactionValue = Spec.Get("transaction");
		if (TransactionValue.GetKind() == dream::json::Kind::Bool)
		{
			Transaction = TransactionValue.AsBool();
		}
		else if (!TransactionValue.IsNull())
		{
			PyErr_SetString(PyExc_ValueError, "enqueue_run(): transaction must be true, false or null");
			return nullptr;
		}

		FText Error;
		const int32 RunNumber = State().RunService->EnqueueApiRun(DreamText::Utf8ToFString(TargetValue.AsString()), Namespace, Transaction, Error);
		if (RunNumber == 0)
		{
			PyErr_SetString(PyExc_RuntimeError, TCHAR_TO_UTF8(*Error.ToString()));
			return nullptr;
		}
		return PyLong_FromLong(RunNumber);
	}

	struct FSuppressKey
	{
		const char* Name;
		bool FDreamOutputVisibility::* Field;
	};

	const FSuppressKey SuppressKeys[] = {
		{ "results", &FDreamOutputVisibility::bResults },
		{ "info", &FDreamOutputVisibility::bInfo },
		{ "warnings", &FDreamOutputVisibility::bWarnings },
		{ "errors", &FDreamOutputVisibility::bErrors },
		{ "echo", &FDreamOutputVisibility::bEcho },
	};

	PyObject* Suppress(PyObject*, PyObject* Flags)
	{
		if (!RequireGameThread("suppress"))
		{
			return nullptr;
		}
		if (!PyDict_Check(Flags))
		{
			PyErr_SetString(PyExc_TypeError, "suppress() takes a dict");
			return nullptr;
		}
		// Validate every key first, so a bad call changes nothing.
		PyObject* Key = nullptr;
		PyObject* Value = nullptr;
		Py_ssize_t Position = 0;
		while (PyDict_Next(Flags, &Position, &Key, &Value))
		{
			bool bKnown = false;
			if (PyUnicode_Check(Key))
			{
				for (const FSuppressKey& Entry : SuppressKeys)
				{
					bKnown = bKnown || PyUnicode_CompareWithASCIIString(Key, Entry.Name) == 0;
				}
			}
			if (!bKnown)
			{
				PyErr_Format(PyExc_ValueError, "suppress(): unknown key %R", Key);
				return nullptr;
			}
			if (!PyBool_Check(Value))
			{
				PyErr_Format(PyExc_TypeError, "suppress(): %S must be a bool", Key);
				return nullptr;
			}
		}

		// The §12.2 polarity: True suppresses (hides); the store keeps visibilities, so the bridge inverts (T-OUT-043).
		FDreamOutputStore& Store = *State().OutputStore;
		const FDreamOutputVisibility Previous = Store.GetVisibility();
		FDreamOutputVisibility Next = Previous;
		PyObject* Result = PyDict_New();
		if (Result == nullptr)
		{
			return nullptr;
		}
		for (const FSuppressKey& Entry : SuppressKeys)
		{
			if (PyDict_SetItemString(Result, Entry.Name, Previous.*Entry.Field ? Py_False : Py_True) != 0)
			{
				Py_DECREF(Result);
				return nullptr;
			}
			if (PyObject* Requested = PyDict_GetItemString(Flags, Entry.Name))
			{
				Next.*Entry.Field = Requested != Py_True;
			}
		}
		if (!(Next == Previous))
		{
			Store.SetVisibility(Next);
		}
		return Result;
	}

	// ---- Live resolver helpers (§7.5.2, N1, RV-21) ----------------------------------------------------------------

	/**
	 * A property's name as the resolver compares it (§7.5.2 "normalised name"): the ScriptName metadata's first item,
	 * else the C++ name; the leading "b" and "In" prefixes PythonScriptPlugin strips from property names
	 * (PyGenUtil::PythonizePropertyName) are stripped too, so the result equals the Python attribute name with its case and
	 * underscores removed; then lower case without '_'.
	 */
	FString NormalisedPropertyName(const FProperty& Property)
	{
		FString Name;
#if WITH_METADATA
		Name = Property.GetMetaData(TEXT("ScriptName"));
		int32 Semicolon = INDEX_NONE;
		if (Name.FindChar(TEXT(';'), Semicolon))
		{
			Name.LeftInline(Semicolon);
		}
#endif
		if (Name.IsEmpty())
		{
			Name = Property.GetName();
		}
		int32 Offset = 0;
		for (;;)
		{
			const int32 Left = Name.Len() - Offset;
			if (Left >= 2 && Name[Offset] == TEXT('b') && FChar::IsUpper(Name[Offset + 1]))
			{
				Offset += 1;
			}
			else if (Left >= 3 && Name[Offset] == TEXT('I') && Name[Offset + 1] == TEXT('n') && FChar::IsUpper(Name[Offset + 2]))
			{
				Offset += 2;
			}
			else
			{
				break;
			}
		}
		FString Out;
		Out.Reserve(Name.Len() - Offset);
		for (int32 Index = Offset; Index < Name.Len(); ++Index)
		{
			if (Name[Index] != TEXT('_'))
			{
				Out.AppendChar(FChar::ToLower(Name[Index]));
			}
		}
		return Out;
	}

	/** The normalised names of Class's hard object, class and weak object properties, colliding names dropped. GT. */
	TArray<FString> CollectHardObjectProps(const UClass& Class)
	{
		TMap<FString, int32> Counts;            // every property's normalised name: a collision drops both (§7.5.2 step 4)
		TArray<FString> Hard;
		for (TFieldIterator<FProperty> It(&Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			const FString Name = NormalisedPropertyName(*Property);
			if (Name.IsEmpty())
			{
				continue;
			}
			++Counts.FindOrAdd(Name);
			// FObjectProperty covers FClassProperty; FSoftObjectProperty, FSoftClassProperty and FLazyObjectProperty derive
			// from FObjectPropertyBase, not FObjectProperty, so CastField leaves them out (N1).
			if (CastField<FObjectProperty>(Property) != nullptr || CastField<FWeakObjectProperty>(Property) != nullptr)
			{
				Hard.Add(Name);
			}
		}
		Hard.RemoveAll([&Counts](const FString& Name) { return Counts.FindRef(Name) != 1; });
		Hard.Sort();
		return Hard;
	}

	PyObject* NewFrozenSet(const TArray<FString>& Names)
	{
		PyObject* List = PyList_New(0);
		if (List == nullptr)
		{
			return nullptr;
		}
		for (const FString& Name : Names)
		{
			PyObject* Item = DreamPy::FromString(Name);
			if (Item == nullptr || PyList_Append(List, Item) != 0)
			{
				Py_XDECREF(Item);
				Py_DECREF(List);
				return nullptr;
			}
			Py_DECREF(Item);
		}
		PyObject* Set = PyFrozenSet_New(List);
		Py_DECREF(List);
		return Set;
	}

	PyObject* HardObjectProps(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		// §7.5.2, §12.1.1 (N1, E2): which properties get_editor_property may read for completion. Every failure after the
		// argument checks is an empty set ("do not read"). Game thread: reflection data and the cache below.
		if (!DreamPy::CheckArgCount("hard_object_props", NArgs, 1))
		{
			return nullptr;
		}
		if (!PyUnicode_Check(Args[0]))
		{
			PyErr_Format(PyExc_TypeError, "hard_object_props() argument must be str, not %.100s", Py_TYPE(Args[0])->tp_name);
			return nullptr;
		}
		FString ClassName;
		if (!DreamPy::ToString(Args[0], ClassName))
		{
			PyErr_Clear();
			return NewFrozenSet({});
		}
		ClassName.TrimStartAndEndInline();
		// A name FName cannot hold (NAME_SIZE, its checkf) or a path is no class name; FNAME_Find first, so an unknown name
		// never enters the name table (review r1).
		if (!IsInGameThread() || ClassName.IsEmpty() || ClassName.Len() >= NAME_SIZE
			|| FName(*ClassName, FNAME_Find).IsNone())
		{
			return NewFrozenSet({});
		}
		// The Safe variant returns null while a package saves or GC holds the hash tables, instead of asserting.
		const UClass* Class = FindFirstObjectSafe<UClass>(ClassName, EFindFirstObjectOptions::NativeFirst);
		if (Class == nullptr)
		{
			return NewFrozenSet({});
		}
		// Per class (§7.5.2 "Results are cached per class"); FObjectKey, so a reinstanced class at a reused address misses.
		// Only native classes: a Blueprint recompile keeps its generated class object (and key) but changes its properties,
		// so those are read each time (one property walk, review r1).
		if (!Class->HasAnyClassFlags(CLASS_Native))
		{
			return NewFrozenSet(CollectHardObjectProps(*Class));
		}
		static TMap<FObjectKey, TArray<FString>> Cache;
		const FObjectKey Key(Class);
		if (const TArray<FString>* Hit = Cache.Find(Key))
		{
			return NewFrozenSet(*Hit);
		}
		if (Cache.Num() >= 4096)
		{
			Cache.Reset();      // bounded: classes reinstanced over a long session leave stale keys
		}
		return NewFrozenSet(Cache.Add(Key, CollectHardObjectProps(*Class)));
	}

	PyObject* ContainerElementKind(PyObject*, PyObject* Value)
	{
		// §7.5.2, §12.1.1 (RV-21): None for every value, which callers treat as "do not read": elements of an unreal.Array,
		// Map or Set are never subscripted, iterated or paged, and those steps resolve at type level.
		// Deferred (M2): the element property's class name read from the wrapper's property pointer. FPyWrapperArray,
		// FPyWrapperMap and FPyWrapperSet are declared in PythonScriptPlugin's Private folder, which is not on DreamEditor's
		// include path, and their layout is not ours to copy (run group B-note).
		(void)Value;
		return DreamPy::NewNone();
	}

	// ---- (debug, PO-06 F5) The pause port (§8.1.5, §8.3, §8.4) --------------------------------------------------

	PyObject* PauseContext(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("pause_context") || !DreamPy::CheckArgCount("pause_context", NArgs, 1))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		if (!DreamNativeInternal::ParseWriteRunId(Args[0], Id))
		{
			return nullptr;
		}
		return PyLong_FromLong(static_cast<long>(FDreamDebugger::Get().CanPause(Id)));
	}

	PyObject* PauseTuple(const char* Command, const FString& ArgsText)
	{
		PyObject* Second = DreamPy::FromString(ArgsText);
		if (Second == nullptr)
		{
			return nullptr;
		}
		PyObject* Tuple = Py_BuildValue("(sN)", Command, Second);
		return Tuple;
	}

	PyObject* Pause(PyObject*, PyObject* const* Args, Py_ssize_t NArgs)
	{
		if (!RequireGameThread("pause") || !DreamPy::CheckArgCount("pause", NArgs, 2))
		{
			return nullptr;
		}
		FDreamRunId Id = 0;
		FString StopJson;
		if (!ParseActiveRunId(Args[0], "pause", Id) || !ParseStr(Args[1], "pause", "stop_json", StopJson))
		{
			return nullptr;
		}
		FDreamDebugger& Debugger = FDreamDebugger::Get();
		const EDreamPauseRefusal Refusal = Debugger.CanPause(Id);
		if (Refusal != EDreamPauseRefusal::None)
		{
			return PauseTuple("refused", FString::FromInt(static_cast<int32>(Refusal)));
		}
		FDreamResume Resume;
		// Review q1 m4: the paused gate is up before the GIL is released, so neither a pending call nor the §5.7.5 channel
		// can act in the window between; it comes down only once the GIL is back.
		State().Supervisor->SetPaused(Id, true);
		{
			// §8.3.1 step 8 (GIL-3): the GIL is released around the Paused window's modal loop; requests re-take it.
			FDreamScopedGilRelease NoGil;
			Resume = Debugger.RunPause(Id, StopJson);
		}
		State().Supervisor->SetPaused(Id, false);
		if (Resume.Refusal != EDreamPauseRefusal::None)
		{
			return PauseTuple("refused", FString::FromInt(static_cast<int32>(Resume.Refusal)));
		}
		switch (Resume.Kind)
		{
		case EDreamResumeKind::StepOver:
			return PauseTuple("next", TEXT("{}"));
		case EDreamResumeKind::StepInto:
			return PauseTuple("stepIn", TEXT("{}"));
		case EDreamResumeKind::StepOut:
			return PauseTuple("stepOut", TEXT("{}"));
		case EDreamResumeKind::Stop:
			// §8.5 Stop: at a line stop the trace callback raises ScriptCancelled (a delivery: finally blocks run, RT-06 re-
			// delivery applies); at the exception stop the run ends cancelled without raising again.
			if (!Resume.bInspectOnly && State().Supervisor->RaiseCancelIfStopRequested(Id))
			{
				return nullptr;
			}
			return PauseTuple("terminate", TEXT("{}"));
		case EDreamResumeKind::Continue:
		default:
			return PauseTuple("continue", TEXT("{}"));
		}
	}

	PyObject* IsGameThreadFn(PyObject*, PyObject*)
	{
		return DreamPy::NewBool(State().bAlive && PyThread_get_thread_ident() == State().GameThreadIdent);
	}

	PyObject* ObjectProps(PyObject*, PyObject* Arg)
	{
		FString Path;
		if (!ParseStr(Arg, "object_props", "path_name", Path))
		{
			return nullptr;
		}
		PyObject* List = PyList_New(0);
		if (List == nullptr)
		{
			return nullptr;
		}
		// §8.8 (RV-21): export text only, which runs no BlueprintGetter and loads no soft reference.
		if (!IsInGameThread() || UE::IsSavingPackage() || IsGarbageCollecting() || Path.IsEmpty())
		{
			return List;
		}
		const UObject* Object = StaticFindObject(UObject::StaticClass(), nullptr, *Path);
		if (Object == nullptr)
		{
			return List;
		}
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Object->GetClass()); It && Count < 300; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;                                   // only shown properties count against the limit (review q1 m7)
			}
			++Count;
			const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
			FString Text;
			// Review q1 m7: containers show their count only; exporting a huge array whole costs time and memory.
			if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
			{
				Text = FString::Printf(TEXT("Array[%d]"), FScriptArrayHelper(ArrayProp, Value).Num());
			}
			else if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
			{
				Text = FString::Printf(TEXT("Map[%d]"), FScriptMapHelper(MapProp, Value).Num());
			}
			else if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
			{
				Text = FString::Printf(TEXT("Set[%d]"), FScriptSetHelper(SetProp, Value).Num());
			}
			else
			{
				Property->ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
			}
			if (Text.Len() > 200)
			{
				Text = Text.Left(199) + TEXT("…");
			}
			PyObject* Item = Py_BuildValue("(NNN)", DreamPy::FromString(Property->GetName()), DreamPy::FromString(Property->GetCPPType()),
				DreamPy::FromString(Text));
			if (Item == nullptr || PyList_Append(List, Item) != 0)
			{
				Py_XDECREF(Item);
				Py_DECREF(List);
				return nullptr;
			}
			Py_DECREF(Item);
		}
		return List;
	}

	// ---- Module definition -------------------------------------------------------------------------------------

	PyMethodDef ModuleMethods[] = {
		{ "write", DREAM_PY_CFUNCTION(&Write), METH_FASTCALL, "write(stream: int, run_id: int, text: str) -> None" },
		{ "thread_label", DREAM_PY_CFUNCTION(&ThreadLabel), METH_FASTCALL, "thread_label(ident: int, name: str) -> None" },
		{ "log_shim", DREAM_PY_CFUNCTION(&LogShim), METH_FASTCALL, "log_shim(verbosity: int, text: object) -> None" },
		{ "capture_begin", DREAM_PY_CFUNCTION(&CaptureBegin), METH_FASTCALL, "capture_begin(run_id: int, alive_native_ids) -> None" },
		{ "capture_end", DREAM_PY_CFUNCTION(&CaptureEnd), METH_FASTCALL, "capture_end(run_id: int) -> None" },
		{ "clear_output", ClearOutput, METH_NOARGS, "clear_output() -> None" },
		{ "current_run_id", CurrentRunId, METH_NOARGS, "current_run_id() -> int" },
		{ "user_code_begin", DREAM_PY_CFUNCTION(&UserCodeBegin), METH_FASTCALL, "user_code_begin(run_id: int) -> bool" },
		{ "user_code_done", DREAM_PY_CFUNCTION(&UserCodeDone), METH_FASTCALL, "user_code_done(run_id: int) -> None" },
		{ "run_statement", DREAM_PY_CFUNCTION(&RunStatement), METH_FASTCALL, "run_statement(run_id: int, index: int, code, ns: dict) -> None" },
		{ "statement_counts", DREAM_PY_CFUNCTION(&StatementCounts), METH_FASTCALL, "statement_counts(run_id: int) -> tuple[int, int]" },
		{ "report_outcome", DREAM_PY_CFUNCTION(&ReportOutcome), METH_FASTCALL, "report_outcome(run_id: int, outcome: int, detail: str, location) -> None" },
		{ "register_cancel_class", RegisterCancelClass, METH_O, "register_cancel_class(cls: type) -> None" },
		{ "check_cancel", DREAM_PY_CFUNCTION(&CheckCancel), METH_FASTCALL, "check_cancel(deliver: bool = False) -> bool" },
		{ "stop_requested", DREAM_PY_CFUNCTION(&StopRequested), METH_FASTCALL, "stop_requested(run_id: int) -> bool" },
		{ "progress", DREAM_PY_CFUNCTION(&Progress), METH_FASTCALL, "progress(i: int, n: int | None, msg: str) -> None" },
		{ "open_tab", DREAM_PY_CFUNCTION(&OpenTab), METH_FASTCALL, "open_tab(path: str, line: int, col: int) -> str" },
		{ "new_tab", DREAM_PY_CFUNCTION(&NewTab), METH_FASTCALL, "new_tab(text: str, name: str) -> str" },
		{ "enqueue_run", DREAM_PY_CFUNCTION(&EnqueueRun), METH_FASTCALL, "enqueue_run(spec_json: str) -> int" },
		{ "suppress", Suppress, METH_O, "suppress(flags: dict[str, bool]) -> dict[str, bool]" },
		{ "hard_object_props", DREAM_PY_CFUNCTION(&HardObjectProps), METH_FASTCALL, "hard_object_props(py_class_name: str) -> frozenset[str]" },
		{ "container_element_kind", ContainerElementKind, METH_O, "container_element_kind(value: object) -> str | None" },
		// (debug, PO-06 F5) the pause port
		{ "pause_context", DREAM_PY_CFUNCTION(&PauseContext), METH_FASTCALL, "pause_context(run_id: int) -> int" },
		{ "pause", DREAM_PY_CFUNCTION(&Pause), METH_FASTCALL, "pause(run_id: int, stop_json: str) -> tuple[str, str]" },
		{ "is_game_thread", IsGameThreadFn, METH_NOARGS, "is_game_thread() -> bool" },
		{ "object_props", ObjectProps, METH_O, "object_props(path_name: str) -> list[tuple[str, str, str]]" },
		{ nullptr, nullptr, 0, nullptr },
	};

	PyModuleDef ModuleDef = {
		PyModuleDef_HEAD_INIT,
		"_dream",
		"Dream Python Editor native module (TDD 12.1). Created by DreamEditor at OnPythonInitialized; not a public API.",
		-1,
		ModuleMethods,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
	};

	/**
	 * The RT-07 shims carry the originals' __name__ ("log", "log_warning", "log_error"), so they are separate method
	 * definitions added under the shim_* attribute names; __doc__ is copied from _unreal_core at creation when available.
	 */
	PyMethodDef ShimDefs[] = {
		{ "log", ShimLog, METH_O, "log(arg) -> None: log the argument as information through Dream's run output." },
		{ "log_warning", ShimLogWarning, METH_O, "log_warning(arg) -> None: log the argument as a warning through Dream's run output." },
		{ "log_error", ShimLogError, METH_O, "log_error(arg) -> None: log the argument as an error through Dream's run output." },
	};
	const char* const ShimAttributeNames[] = { "shim_log", "shim_log_warning", "shim_log_error" };
	std::string ShimDocs[3];

	void CopyOriginalShimDocs()
	{
		PyObject* CoreName = PyUnicode_FromString("_unreal_core");
		PyObject* Core = CoreName ? PyImport_GetModule(CoreName) : nullptr;
		Py_XDECREF(CoreName);
		if (Core == nullptr)
		{
			PyErr_Clear();
			return;
		}
		for (int32 Index = 0; Index < 3; ++Index)
		{
			PyObject* Original = PyObject_GetAttrString(Core, ShimDefs[Index].ml_name);
			PyObject* Doc = Original ? PyObject_GetAttrString(Original, "__doc__") : nullptr;
			Py_ssize_t Size = 0;
			const char* Utf8 = Doc && PyUnicode_Check(Doc) ? PyUnicode_AsUTF8AndSize(Doc, &Size) : nullptr;
			if (Utf8 != nullptr)
			{
				ShimDocs[Index].assign(Utf8, static_cast<size_t>(Size));
				ShimDefs[Index].ml_doc = ShimDocs[Index].c_str();
			}
			PyErr_Clear();
			Py_XDECREF(Doc);
			Py_XDECREF(Original);
		}
		Py_DECREF(Core);
	}

	bool AddConstants(PyObject* Module, uint32 Generation, bool bHeadless)
	{
		struct FConstant
		{
			const char* Name;
			long Value;
		};
		const FConstant Constants[] = {
			{ "ABI_VERSION", DreamNative::AbiVersion },
			{ "STREAM_STDOUT", static_cast<long>(EDreamStream::Stdout) },
			{ "STREAM_STDERR", static_cast<long>(EDreamStream::Stderr) },
			{ "STREAM_RESULT", static_cast<long>(EDreamStream::Result) },
			{ "STREAM_NOTICE", static_cast<long>(EDreamStream::Notice) },
			{ "STREAM_INFO", static_cast<long>(EDreamStream::Info) },
			{ "STREAM_WARNING", static_cast<long>(EDreamStream::Warning) },
			{ "STREAM_ERROR", static_cast<long>(EDreamStream::Error) },
			{ "STREAM_AGENT", static_cast<long>(EDreamStream::Agent) },
			{ "STREAM_DEBUGGER", static_cast<long>(EDreamStream::Debugger) },
			{ "STREAM_LOGPOINT", static_cast<long>(EDreamStream::Logpoint) },
			{ "STREAM_EVALUATE", static_cast<long>(EDreamStream::Evaluate) },
		};
		for (const FConstant& Constant : Constants)
		{
			if (PyModule_AddIntConstant(Module, Constant.Name, Constant.Value) != 0)
			{
				return false;
			}
		}
		PyObject* GenerationObject = PyLong_FromUnsignedLong(Generation);
		const bool bOk = GenerationObject != nullptr && PyModule_AddObjectRef(Module, "GENERATION", GenerationObject) == 0
			&& PyModule_AddObjectRef(Module, "HEADLESS", bHeadless ? Py_True : Py_False) == 0;
		Py_XDECREF(GenerationObject);
		return bOk;
	}

	bool AddShims(PyObject* Module)
	{
		PyObject* ModuleName = PyUnicode_FromString("_dream");
		if (ModuleName == nullptr)
		{
			return false;
		}
		bool bOk = true;
		for (int32 Index = 0; Index < 3 && bOk; ++Index)
		{
			PyObject* Function = PyCFunction_NewEx(&ShimDefs[Index], Module, ModuleName);
			bOk = Function != nullptr && PyModule_AddObjectRef(Module, ShimAttributeNames[Index], Function) == 0;
			Py_XDECREF(Function);
		}
		Py_DECREF(ModuleName);
		return bOk;
	}
}

namespace DreamNative
{
	bool CreateModule(uint32 Generation, bool bHeadless, FText& OutReason)
	{
		using namespace DreamNativePrivate;
		check(IsInGameThread());

		FState& S = State();
		S.bAlive = false;
		S.Generation = Generation;
		S.bHeadless = bHeadless;
		S.GameThreadIdent = PyThread_get_thread_ident();
		S.Module = &FDreamEditorModule::Get();
		S.OutputStore = &S.Module->GetOutputStore();
		S.RunService = &S.Module->GetRunService();
		S.Supervisor = &S.Module->GetSupervisor();

		auto Fail = [&OutReason](const TCHAR* Step) -> bool
		{
			const FString Detail = DreamPy::FetchExceptionText();
			OutReason = FText::Format(NSLOCTEXT("Dream", "NativeModuleFailed", "Dream Python Editor could not create its native module (_dream) at {0}: {1}"),
				FText::FromString(Step), FText::FromString(Detail));
			return false;
		};

		if (PyObject* Io = PyImport_ImportModule("io"))
		{
			S.UnsupportedOperation = FDreamPyRef::Steal(PyObject_GetAttrString(Io, "UnsupportedOperation"));
			Py_DECREF(Io);
		}
		if (!S.UnsupportedOperation)
		{
			return Fail(TEXT("io.UnsupportedOperation"));
		}

		CopyOriginalShimDocs();
		PyObject* Module = PyModule_Create(&ModuleDef);
		if (Module == nullptr)
		{
			return Fail(TEXT("PyModule_Create"));
		}
		if (!AddConstants(Module, Generation, bHeadless) || !DreamNativeInternal::AddWriterTypes(Module) || !AddShims(Module))
		{
			Py_DECREF(Module);
			return Fail(TEXT("module contents"));
		}
		PyObject* Modules = PyImport_GetModuleDict();     // borrowed
		const int SetResult = PyDict_SetItemString(Modules, "_dream", Module);
		Py_DECREF(Module);
		if (SetResult != 0)
		{
			return Fail(TEXT("sys.modules"));
		}
		S.bAlive = true;
		return true;
	}

	void DestroyModule()
	{
		DreamNativeInternal::FState& S = DreamNativeInternal::State();
		S.bAlive = false;
		S.CancelClass.Reset();
		S.UnsupportedOperation.Reset();
		S.WriterType.Reset();
		S.BinaryWriterType.Reset();
		// The service pointers stay: a writer kept by a library keeps pushing (tagged background) until the
		// interpreter is finalized, and the store outlives Python (§3.8 module shutdown order).
	}

	PyObject* GetCancelClass()
	{
		return DreamNativeInternal::State().CancelClass.Get();
	}

	PyObject* NewCancelledInstance(int32 RunNumber, const TCHAR* Source, int32 Delivery)
	{
		PyObject* Class = GetCancelClass();
		if (Class == nullptr)
		{
			PyErr_SetString(PyExc_RuntimeError, "dream_editor.ScriptCancelled is not registered");
			return nullptr;
		}
		PyObject* Instance = PyObject_CallNoArgs(Class);
		if (Instance == nullptr)
		{
			return nullptr;
		}
		PyObject* RunNumberObject = PyLong_FromLong(RunNumber);
		PyObject* SourceObject = DreamPy::FromString(Source ? Source : TEXT("async"));
		PyObject* DeliveryObject = PyLong_FromLong(Delivery);
		const bool bOk = RunNumberObject && SourceObject && DeliveryObject
			&& PyObject_SetAttrString(Instance, "run_number", RunNumberObject) == 0
			&& PyObject_SetAttrString(Instance, "source", SourceObject) == 0
			&& PyObject_SetAttrString(Instance, "delivery", DeliveryObject) == 0;
		Py_XDECREF(RunNumberObject);
		Py_XDECREF(SourceObject);
		Py_XDECREF(DeliveryObject);
		if (!bOk)
		{
			Py_DECREF(Instance);
			return nullptr;
		}
		return Instance;
	}

	bool IsOurCancel(PyObject* Exc, int32 RunNumber)
	{
		PyObject* Class = GetCancelClass();
		if (Class == nullptr || Exc == nullptr || !PyType_Check(Class))
		{
			return false;
		}
		// Breadth-first over __context__ and __cause__, at most 16 links (§5.3.9, §5.7.4); cycles are cut by identity.
		static constexpr int32 MaxLinks = 16;
		TArray<PyObject*, TInlineAllocator<MaxLinks + 1>> Seen;
		TArray<PyObject*, TInlineAllocator<2 * MaxLinks + 2>> Owned;
		Seen.Add(Exc);
		bool bOurs = false;
		for (int32 Cursor = 0; Cursor < Seen.Num() && !bOurs; ++Cursor)
		{
			PyObject* Node = Seen[Cursor];
			if (PyObject_TypeCheck(Node, reinterpret_cast<PyTypeObject*>(Class)))
			{
				PyObject* Number = PyObject_GetAttrString(Node, "run_number");
				if (Number != nullptr && DreamPy::IsStrictInt(Number))
				{
					const long Value = PyLong_AsLong(Number);
					bOurs = Value == 0 || Value == RunNumber;
				}
				Py_XDECREF(Number);
				PyErr_Clear();
			}
			if (bOurs || !PyExceptionInstance_Check(Node))
			{
				continue;
			}
			PyObject* Links[2] = { PyException_GetContext(Node), PyException_GetCause(Node) };
			for (PyObject* Link : Links)
			{
				if (Link == nullptr)
				{
					continue;
				}
				Owned.Add(Link);
				if (Seen.Num() <= MaxLinks && !Seen.Contains(Link))
				{
					Seen.Add(Link);
				}
			}
		}
		for (PyObject* Link : Owned)
		{
			Py_DECREF(Link);
		}
		return bOurs;
	}

	unsigned long GetGameThreadIdent()
	{
		return DreamNativeInternal::State().GameThreadIdent;
	}
}

#endif // WITH_PYTHON
