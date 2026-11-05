// SPDX-License-Identifier: Apache-2.0
// The Python lifecycle (TDD §3.4, §3.5, §3.8, D16, RT-17) and the helper calls of §12.1.3 (INV-6). Game thread unless
// a function says otherwise. The two engine entry calls (EnterRun, Boot) are in DreamPythonEntry.cpp (§2.5 rule 5).
#include "Python/DreamPython.h"

#include "Compat/DreamPyCompat.h"
#include "Containers/Queue.h"
#include "Debug/DreamDebugger.h"      // (debug)
#include "HAL/IConsoleManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Output/DreamOutputStore.h"
#include "Python/DreamGil.h"
#include "Python/DreamNativeModule.h"
#include "Python/DreamPyUtil.h"
#include "Python/DreamPythonImpl.h"
#include "Python/DreamPythonSnapshot.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamPythonPrivate
{
	/** The generation whose interpreter is alive (FDreamPyRef stamps); 0 when none. Any thread. */
	std::atomic<uint32> GAliveGeneration{ 0 };
	/** The registry of §3.8: live FDreamPyRef count of the counted (current) generation. */
	std::atomic<uint32> GCountedGeneration{ 0 };
	std::atomic<int32> GLiveRefs{ 0 };

	struct FQueuedDecref
	{
		FDreamPyObject* Object = nullptr;
		uint32 Generation = 0;
	};

	TQueue<FQueuedDecref, EQueueMode::Mpsc>& DecrefQueue()
	{
		static TQueue<FQueuedDecref, EQueueMode::Mpsc> Queue;
		return Queue;
	}
	std::atomic<int32> GQueuedDecrefs{ 0 };

	/**
	 * A console variable read often (the banner and CanSubmit ask every paint): looked up by name once and cached, the
	 * engine's usual pattern, because every FindConsoleVariable by name counts toward LogConsoleManager's warning about
	 * repeated lookups. A missing variable (PythonScriptPlugin absent) is looked up again and reads as bDefault. Any thread.
	 */
	struct FCachedConsoleBool
	{
		const TCHAR* Name;
		bool bDefault;
		mutable std::atomic<IConsoleVariable*> Variable{ nullptr };

		bool Get() const
		{
			IConsoleVariable* Found = Variable.load(std::memory_order_acquire);
			if (Found == nullptr)
			{
				Found = IConsoleManager::Get().FindConsoleVariable(Name);
				if (Found != nullptr)
				{
					Variable.store(Found, std::memory_order_release);
				}
			}
			return Found != nullptr ? Found->GetBool() : bDefault;
		}
	};

	const FCachedConsoleBool RestrictiveModeVar{ TEXT("Engine.Python.IsPythonInRestrictiveMode"), false };
	const FCachedConsoleBool LifeCycleDynamicVar{ TEXT("Engine.Python.IsPythonLifeCycleDynamic"), false };
	const FCachedConsoleBool EnabledByDefaultVar{ TEXT("Engine.Python.IsEnabledByDefault"), true };

	/** PythonScriptPluginUserSettings::EnablePythonOverride, read by reflection: its header is private (§9.5). */
	FString GetEnablePythonOverrideName()
	{
		const UClass* SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/PythonScriptPlugin.PythonScriptPluginUserSettings"));
		const FEnumProperty* Property = SettingsClass
			? CastField<FEnumProperty>(SettingsClass->FindPropertyByName(TEXT("EnablePythonOverride")))
			: nullptr;
		const UObject* Defaults = SettingsClass ? SettingsClass->GetDefaultObject(false) : nullptr;
		if (Property == nullptr || Defaults == nullptr || Property->GetEnum() == nullptr)
		{
			return FString();
		}
		const int64 Value = Property->GetUnderlyingProperty()->GetSignedIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Defaults));
		return Property->GetEnum()->GetNameStringByValue(Value);
	}

	bool IsHeadlessProcess()
	{
		return IsRunningCommandlet() || FApp::IsUnattended() || !FApp::CanEverRender();
	}

	FDreamRunService* RunService()
	{
		return FDreamEditorModule::IsAvailable() ? &FDreamEditorModule::Get().GetRunService() : nullptr;
	}
}

void FDreamPython::FImpl::CancelSavedPaths()
{
	// NotifyBufferSaved's queue (R2): the idle tick is removed and the waiting paths are forgotten.
	if (SavedPathsTicker.IsValid())
	{
		FDreamDeferral::RemoveTicker(SavedPathsTicker);
		SavedPathsTicker.Reset();
	}
	SavedPaths.Reset();
	SavedPathsGeneration = 0;
}

#if WITH_PYTHON
namespace DreamPythonPrivate
{
	/** The helper's callable, imported once per generation. GIL held. New reference semantics stay in the cache. */
	PyObject* GetHelper(FDreamPyRef& Cache, const char* ModuleName, const char* FunctionName)
	{
		if (!Cache)
		{
			PyObject* Module = PyImport_ImportModule(ModuleName);
			if (Module == nullptr)
			{
				return nullptr;
			}
			Cache = FDreamPyRef::Steal(PyObject_GetAttrString(Module, FunctionName));
			Py_DECREF(Module);
		}
		return Cache.Get();
	}

	/** Calls a helper with a new-reference argument tuple (stolen). Returns a new reference or null with an error. */
	PyObject* CallHelper(FDreamPyRef& Cache, const char* ModuleName, const char* FunctionName, PyObject* ArgsTuple)
	{
		if (ArgsTuple == nullptr)
		{
			return nullptr;
		}
		PyObject* Function = GetHelper(Cache, ModuleName, FunctionName);
		PyObject* Result = Function ? PyObject_CallObject(Function, ArgsTuple) : nullptr;
		Py_DECREF(ArgsTuple);
		return Result;
	}

	/** Sets Dict[Key] = Value and steals Value; false on failure. */
	bool SetItem(PyObject* Dict, const char* Key, PyObject* Value)
	{
		if (Value == nullptr)
		{
			return false;
		}
		const bool bOk = PyDict_SetItemString(Dict, Key, Value) == 0;
		Py_DECREF(Value);
		return bOk;
	}

	PyObject* StrOrNone(const FString& Text)
	{
		return Text.IsEmpty() ? DreamPy::NewNone() : DreamPy::FromString(Text);
	}

	int32 GetIntItem(PyObject* Dict, const char* Key, int32 Default = 0)
	{
		PyObject* Value = PyDict_GetItemString(Dict, Key);
		if (Value == nullptr || !DreamPy::IsStrictInt(Value))
		{
			return Default;
		}
		const long Result = PyLong_AsLong(Value);
		if (PyErr_Occurred())
		{
			PyErr_Clear();
			return Default;
		}
		return static_cast<int32>(FMath::Clamp<long>(Result, MIN_int32, MAX_int32));
	}

	FString GetStrItem(PyObject* Dict, const char* Key)
	{
		PyObject* Value = PyDict_GetItemString(Dict, Key);
		FString Result;
		if (Value != nullptr && PyUnicode_Check(Value) && !DreamPy::ToString(Value, Result))
		{
			PyErr_Clear();
		}
		return Result;
	}

	PyObject* BuildPrepareRequest(FDreamRunId Id, const FDreamRunRecord& Record, const FDreamRunRequest& Request)
	{
		PyObject* Dict = PyDict_New();
		if (Dict == nullptr)
		{
			return nullptr;
		}
		PyObject* Argv = PyList_New(0);
		bool bOk = Argv != nullptr;
		for (int32 Index = 0; bOk && Index < Request.Argv.Num(); ++Index)
		{
			PyObject* Arg = DreamPy::FromString(Request.Argv[Index]);
			bOk = Arg != nullptr && PyList_Append(Argv, Arg) == 0;
			Py_XDECREF(Arg);
		}
		if (!bOk)
		{
			Py_XDECREF(Argv);
			Py_DECREF(Dict);
			return nullptr;
		}
		// The Run File semantics of §5.3.4 (sys.path entry, sys.argv) also apply to an API run of a file (§5.2.1 API row),
		// which EnqueueApiRun marks bFileTarget whether or not a tab has the file open; a tab target (ID or name) does not.
		const bool bRunFile = Record.Route == EDreamRunRoute::RunFile
			|| (DreamRunText::HasRunFileSemantics(Record.Route, Record.bFileTarget) && !Request.FilePath.IsEmpty());
		bOk = SetItem(Dict, "argv", Argv)
			&& SetItem(Dict, "run_id", PyLong_FromUnsignedLongLong(Id))
			&& SetItem(Dict, "run_number", PyLong_FromLong(Record.RunNumber))
			&& SetItem(Dict, "route", DreamPy::FromString(DreamRunText::RouteKey(Record.Route)))
			&& SetItem(Dict, "text", DreamPy::FromString(Request.Text))
			&& SetItem(Dict, "first_line", PyLong_FromLong(Request.FirstLine))
			// (run, PO-06 F2) A selection fragment starts at this UTF-16 column of first_line; the runner maps columns from it.
			&& SetItem(Dict, "first_column", PyLong_FromLong(Request.SelectionStartColumn.Get(0)))
			&& SetItem(Dict, "filename", DreamPy::FromString(Record.Filename))
			&& SetItem(Dict, "linecache_text", Request.BufferText.IsValid() ? DreamPy::FromString(*Request.BufferText) : DreamPy::NewNone())
			&& SetItem(Dict, "file_path", StrOrNone(Request.FilePath))
			&& SetItem(Dict, "dirty", DreamPy::NewBool(Request.bDirty))
			&& SetItem(Dict, "namespace", DreamPy::FromString(Record.Namespace == EDreamNamespaceMode::Session ? TEXT("session") : TEXT("isolated")))
			&& SetItem(Dict, "autoprint", DreamPy::NewBool(Record.bAutoPrint))
			&& SetItem(Dict, "statement_mode", DreamPy::NewBool(Record.bStatementMode))
			&& SetItem(Dict, "run_file", DreamPy::NewBool(bRunFile))
			&& SetItem(Dict, "tab_title", DreamPy::FromString(Record.TabTitle))
			// §5.3.8 (UEI-03 "can be disabled"): the runner refreshes the aliases only for the U19 routes, never Run File.
			&& SetItem(Dict, "aliases", DreamPy::NewBool(UDreamUserSettings::Get() == nullptr || UDreamUserSettings::Get()->bSelectionAliases))
			// (debug, PO-06 F5) A Debug run (F5) and its DebugConfig (§8.1.5): breakpoints keyed for the runner.
			&& SetItem(Dict, "debug", DreamPy::NewBool(Request.bDebug))
			&& SetItem(Dict, "debug_config", Request.bDebug
				? DreamPy::FromString(FDreamDebugger::Get().BuildDebugConfigJson(Id, Request.BufferId, Record.Filename))
				: DreamPy::NewNone());
		if (!bOk)
		{
			Py_DECREF(Dict);
			return nullptr;
		}
		return Dict;
	}

	/** {"ok": True, "statements": [[first, last], ...] | None} or {"ok": False, "error": {...}} (DreamNativeModule.h). */
	void ParsePrepareResult(PyObject* Result, FDreamPrepareResult& Out)
	{
		if (!PyDict_Check(Result))
		{
			Out.InternalError = FString::Printf(TEXT("runner.prepare returned %hs, not a dict"), Py_TYPE(Result)->tp_name);
			return;
		}
		Out.AliasesWritten = FMath::Max(GetIntItem(Result, "aliases_written"), 0);
		PyObject* Ok = PyDict_GetItemString(Result, "ok");
		if (Ok != nullptr && PyObject_IsTrue(Ok) == 1)
		{
			Out.bOk = true;
			PyObject* Statements = PyDict_GetItemString(Result, "statements");
			if (Statements != nullptr && Statements != Py_None)
			{
				PyObject* Sequence = PySequence_Fast(Statements, "statements must be a list");
				if (Sequence == nullptr)
				{
					PyErr_Clear();
					Out.bOk = false;
					Out.InternalError = TEXT("runner.prepare returned malformed statement ranges");
					return;
				}
				Out.bStatementMode = true;
				for (Py_ssize_t Index = 0; Index < PySequence_Fast_GET_SIZE(Sequence); ++Index)
				{
					PyObject* Pair = PySequence_Fast_GET_ITEM(Sequence, Index);
					FDreamStatementRange Range;
					if (PySequence_Check(Pair) && PySequence_Size(Pair) == 2)
					{
						PyObject* First = PySequence_GetItem(Pair, 0);
						PyObject* Last = PySequence_GetItem(Pair, 1);
						Range.FirstLine = First ? static_cast<int32>(PyLong_AsLong(First)) : 0;
						Range.LastLine = Last ? static_cast<int32>(PyLong_AsLong(Last)) : 0;
						Py_XDECREF(First);
						Py_XDECREF(Last);
					}
					PyErr_Clear();
					if (Range.FirstLine <= 0 || Range.LastLine < Range.FirstLine)
					{
						Out.bOk = false;
						Out.InternalError = TEXT("runner.prepare returned a malformed statement range");
						break;
					}
					Out.Statements.Add(Range);
				}
				Py_DECREF(Sequence);
			}
			return;
		}
		PyErr_Clear();
		PyObject* Error = PyDict_GetItemString(Result, "error");
		if (Error == nullptr || !PyDict_Check(Error))
		{
			Out.InternalError = TEXT("runner.prepare failed without an error description");
			return;
		}
		Out.Error.Message = GetStrItem(Error, "msg");
		Out.Error.Line = GetIntItem(Error, "lineno");
		Out.Error.Offset = GetIntItem(Error, "offset");
		Out.Error.EndOffset = GetIntItem(Error, "end_offset");
		Out.Error.Text = GetStrItem(Error, "text");
		const FString TypeName = GetStrItem(Error, "type");
		if (!TypeName.IsEmpty())
		{
			Out.Error.TypeName = TypeName;
		}
	}
}
#endif // WITH_PYTHON

// ---- FDreamPyRef registry (§3.8) --------------------------------------------------------------------------------

uint32 FDreamPython::GetAliveGeneration()
{
	return DreamPythonPrivate::GAliveGeneration.load();
}

void FDreamPython::RegisterRef(uint32 InGeneration)
{
	if (InGeneration != 0 && InGeneration == DreamPythonPrivate::GCountedGeneration.load())
	{
		++DreamPythonPrivate::GLiveRefs;
	}
}

void FDreamPython::UnregisterRef(uint32 InGeneration)
{
	if (InGeneration != 0 && InGeneration == DreamPythonPrivate::GCountedGeneration.load())
	{
		--DreamPythonPrivate::GLiveRefs;
	}
}

void FDreamPython::QueueDecref(FDreamPyObject* Object, uint32 InGeneration)
{
	if (Object == nullptr)
	{
		return;
	}
	DreamPythonPrivate::DecrefQueue().Enqueue(DreamPythonPrivate::FQueuedDecref{ Object, InGeneration });
	++DreamPythonPrivate::GQueuedDecrefs;
}

// ---- Construction and module lifecycle --------------------------------------------------------------------------

FDreamPython::FDreamPython()
	: Impl(MakeUnique<FImpl>())
{
}

FDreamPython::~FDreamPython() = default;

void FDreamPython::Initialize()
{
	check(IsInGameThread());
	Impl->bHeadless = DreamPythonPrivate::IsHeadlessProcess();

#if WITH_PYTHON
	IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
	if (Plugin == nullptr)
	{
		Impl->State = EDreamPythonState::Unavailable;
		return;
	}
	if (Impl->bHeadless)
	{
		Impl->State = EDreamPythonState::Headless;
	}
	else
	{
		Impl->State = EDreamPythonState::WaitingForInit;
		if (Plugin->IsPythonConfigured())
		{
			DecideUnstartedState();
		}
		else
		{
			// Loaded before OnPostEngineInit configured Python (an early load as another module's dependency): decide once
			// ConfigureAndInitializePython has returned, on the next tick (inside the broadcast InitializePython has not run).
			Impl->ConfiguredHandle = Plugin->OnPythonConfigured().AddRaw(this, &FDreamPython::HandleConfigured);
		}
	}

	// Both delegates are cleared after their broadcast (D16), so HandleInitialized and HandleShutdown re-add each other.
	Plugin->OnPythonShutdown().AddRaw(this, &FDreamPython::HandleShutdown);
	Plugin->RegisterOnPythonInitialized(FSimpleDelegate::CreateRaw(this, &FDreamPython::HandleInitialized));
	BindEditorEvents();     // the alias refresh and the map-change release (§5.3.8, D3); no Python here
#else
	Impl->State = EDreamPythonState::Unavailable;
#endif
}

void FDreamPython::DecideUnstartedState()
{
#if WITH_PYTHON
	const IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
	if (Plugin == nullptr || GetState() != EDreamPythonState::WaitingForInit || !Plugin->IsPythonConfigured())
	{
		return;
	}
	if (!Plugin->IsPythonAvailable())
	{
		Impl->State = EDreamPythonState::Disabled;   // -DisablePython, the user override, IsEnabledByDefault = false (§9.5)
		return;
	}
	// D16, §9.5 "IsPythonLifeCycleDynamic with no force-enable yet": ConfigureAndInitializePython returned before
	// InitializePython (PythonScriptPlugin.cpp:1001), so no OnPythonInitialized comes until something calls
	// ForceEnablePythonAtRuntime. That is Disabled ("This project starts Python on demand.", Enable Python offered,
	// T-LIFE-004), not WaitingForInit. (run group choice) Py_IsInitialized() tells the two apart: it is already true in a
	// normal or force-enabled start, where only the startup scripts are still pending. Dream never enables Python itself.
	if (DreamPythonPrivate::LifeCycleDynamicVar.Get() && !Py_IsInitialized())
	{
		Impl->State = EDreamPythonState::Disabled;
	}
#endif
}

void FDreamPython::HandleConfigured()
{
	check(IsInGameThread());
	Impl->ConfiguredHandle.Reset();     // the engine clears the delegate after its broadcast
	if (Impl->ConfiguredTicker.IsValid())
	{
		return;
	}
	Impl->ConfiguredTicker = FDreamDeferral::AddTicker(TEXT("DreamPythonConfigured"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
		/*bNeedsPython*/ false, [this](float)
		{
			Impl->ConfiguredTicker.Reset();
			const EDreamPythonState Before = GetState();
			DecideUnstartedState();
			if (GetState() != Before)
			{
				StateChangedEvent.Broadcast(GetState());
			}
			return false;
		});
}

void FDreamPython::Shutdown()
{
	if (Impl->EnableTimeout.IsValid())
	{
		FDreamDeferral::RemoveTicker(Impl->EnableTimeout);
		Impl->EnableTimeout.Reset();
	}
	if (Impl->ConfiguredTicker.IsValid())
	{
		FDreamDeferral::RemoveTicker(Impl->ConfiguredTicker);
		Impl->ConfiguredTicker.Reset();
	}
	Impl->CancelSavedPaths();
	UnbindEditorEvents();
#if WITH_PYTHON
	if (IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get())
	{
		Plugin->OnPythonInitialized().RemoveAll(this);
		Plugin->OnPythonShutdown().RemoveAll(this);
		Plugin->OnPythonConfigured().RemoveAll(this);
	}
	// §3.8 module shutdown: Python is already finalized (OnPreExit ran ShutdownPython), so no ref may be alive.
	if (DreamPythonPrivate::GAliveGeneration.load() == 0)
	{
		ensureMsgf(DreamPythonPrivate::GLiveRefs.load() == 0, TEXT("Dream: %d Python references are still registered at module shutdown."),
			DreamPythonPrivate::GLiveRefs.load());
	}
#endif
}

EDreamPythonState FDreamPython::GetState() const
{
	return Impl->State.load();
}

uint32 FDreamPython::GetGeneration() const
{
	return Impl->Generation.load();
}

uint32 FDreamPython::GetSessionEpoch() const
{
	return Impl->SessionEpoch.load();
}

bool FDreamPython::IsRestrictiveMode() const
{
	return DreamPythonPrivate::RestrictiveModeVar.Get();
}

bool FDreamPython::CanQueueRuns() const
{
	if (IsRestrictiveMode())
	{
		return false;
	}
	switch (GetState())
	{
	case EDreamPythonState::Cold:
	case EDreamPythonState::Booting:
	case EDreamPythonState::WaitingForInit:
	case EDreamPythonState::Enabling:
		return true;
	default:
		return false;
	}
}

FText FDreamPython::GetDisabledReason() const
{
	// §9.5 banner table, in the engine's own order (PythonScriptPlugin.cpp IsPythonEnabled).
	if (GetState() != EDreamPythonState::Unavailable && IsRestrictiveMode())
	{
		return LOCTEXT("RestrictiveMode", "Python is in restrictive mode in this project. Dream Python Editor does not run code.");
	}
	switch (GetState())
	{
	case EDreamPythonState::Unavailable:
		return LOCTEXT("PluginNotEnabled", "The Python Editor Script Plugin is not enabled.");
	case EDreamPythonState::Headless:
		return LOCTEXT("Headless", "This editor runs without a user interface (commandlet, -unattended or -nullrhi); Dream runs only through -run=PythonScript.");
	case EDreamPythonState::Disabled:
	{
		if (!Impl->FailureReason.IsEmpty())
		{
			return Impl->FailureReason;
		}
		if (FParse::Param(FCommandLine::Get(), TEXT("DisablePython")))
		{
			return LOCTEXT("DisabledByFlag", "Python is disabled in this editor session (command-line flag -DisablePython).");
		}
		if (DreamPythonPrivate::GetEnablePythonOverrideName() == TEXT("Disable"))
		{
			return LOCTEXT("DisabledByUser", "Python is disabled in Editor Preferences ▸ Plugins ▸ Python.");
		}
		if (!DreamPythonPrivate::EnabledByDefaultVar.Get())
		{
			return LOCTEXT("DisabledByProject", "Python is disabled by this project (Engine.Python.IsEnabledByDefault).");
		}
		if (DreamPythonPrivate::LifeCycleDynamicVar.Get())
		{
			return LOCTEXT("OnDemand", "This project starts Python on demand.");
		}
		return LOCTEXT("DisabledOther", "Python is disabled in this editor session.");
	}
	case EDreamPythonState::BootFailed:
		return Impl->FailureReason.IsEmpty() ? LOCTEXT("BootFailed", "Dream Python Editor could not start its Python package.") : Impl->FailureReason;
	case EDreamPythonState::ShutDown:
		return FText::Format(LOCTEXT("ShutDownAt", "Python was shut down at {0}."),
			FText::AsTime(Impl->ShutDownLocal, EDateTimeStyle::Short, FText::GetInvariantTimeZone()));
	case EDreamPythonState::ShuttingDown:
		return LOCTEXT("ShuttingDown", "Python is shutting down.");
	default:
		return FText::GetEmpty();
	}
}

void FDreamPython::RequestStart(EDreamStartReason Reason)
{
	check(IsInGameThread());
	switch (GetState())
	{
	case EDreamPythonState::Cold:
		UE_LOG(LogDream, Verbose, TEXT("Deferred start (reason %d): dream_editor boots at the next end of frame."), static_cast<int32>(Reason));
		Impl->bBootPending = true;
		Impl->SetState(*this, EDreamPythonState::Booting);
		if (FDreamRunService* Runs = DreamPythonPrivate::RunService())
		{
			Runs->ArmEndFrame();
		}
		break;
	case EDreamPythonState::WaitingForInit:
	case EDreamPythonState::Enabling:
		Impl->bStartWanted = true;   // boots once OnPythonInitialized has made _dream (§3.4 triggers act only in Cold)
		break;
	default:
		break;
	}
}

void FDreamPython::RetryBoot()
{
	check(IsInGameThread());
	if (GetState() == EDreamPythonState::BootFailed && Impl->bModuleCreated)
	{
		Impl->FailureReason = FText::GetEmpty();
		Impl->State = EDreamPythonState::Cold;
		RequestStart(EDreamStartReason::RunRequest);
	}
}

void FDreamPython::RequestEnable()
{
	check(IsInGameThread());
	const EDreamPythonState Current = GetState();
	if ((Current != EDreamPythonState::Disabled && Current != EDreamPythonState::ShutDown) || IsRestrictiveMode())
	{
		return;   // Dream never force-enables in restrictive mode (SEC-07)
	}
	// D16: the click only arms the next OnEndFrame; ForceEnablePythonAtRuntime runs the user's startup scripts.
	Impl->FailureReason = FText::GetEmpty();
	Impl->bEnablePending = true;
	Impl->SetState(*this, EDreamPythonState::Enabling);
	if (FDreamRunService* Runs = DreamPythonPrivate::RunService())
	{
		Runs->ArmEndFrame();
	}
	// §3.9: no OnPythonInitialized within 10 s (proposed) -> back to Disabled with a reason.
	Impl->EnableTimeout = FDreamDeferral::AddTicker(TEXT("DreamEnableTimeout"), EDreamDeferPolicy::DeferWhileExecuting, 10.f,
		/*bNeedsPython*/ false, [this](float)
		{
			Impl->EnableTimeout.Reset();
			if (GetState() == EDreamPythonState::Enabling)
			{
				Impl->bEnablePending = false;
				Impl->FailureReason = LOCTEXT("EnableTimedOut", "Python could not be started.");
				Impl->SetState(*this, EDreamPythonState::Disabled);
			}
			return false;
		});
}

bool FDreamPython::RequestResetSession(FText& OutReason)
{
	check(IsInGameThread());
	const FDreamRunService* Runs = DreamPythonPrivate::RunService();
	if (Runs != nullptr && Runs->IsRunActive())
	{
		OutReason = LOCTEXT("ResetDuringRun", "Reset Session is not available while a run is active.");
		return false;
	}
	if (!IsReady())
	{
		OutReason = LOCTEXT("ResetNotReady", "The Session has not started yet; there is nothing to reset.");
		return false;
	}
	Impl->bResetPending = true;
	if (FDreamRunService* Service = DreamPythonPrivate::RunService())
	{
		Service->ArmEndFrame();
	}
	return true;
}

// ---- OnEndFrame work (slice A choice A14) -------------------------------------------------------------------------

bool FDreamPython::ProcessEndFrame()
{
	check(IsInGameThread());
#if WITH_PYTHON
	if (Impl->bEnablePending)
	{
		Impl->bEnablePending = false;
		IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
		// Runs the startup scripts synchronously when the plugin has ticked; OnPythonInitialized may fire inside.
		if (Plugin == nullptr || !Plugin->ForceEnablePythonAtRuntime())
		{
			Impl->FailureReason = LOCTEXT("EnableFailed", "Python could not be started.");
			Impl->SetState(*this, EDreamPythonState::Disabled);
		}
	}

	const uint32 Alive = DreamPythonPrivate::GAliveGeneration.load();
	if (Alive != 0 && DreamPythonPrivate::GQueuedDecrefs.load() > 0)
	{
		FDreamScopedGil Gil;
		DreamPythonPrivate::FQueuedDecref Item;
		while (DreamPythonPrivate::DecrefQueue().Dequeue(Item))
		{
			--DreamPythonPrivate::GQueuedDecrefs;
			if (Item.Generation == Alive)
			{
				Py_DECREF(Item.Object);
			}
			else
			{
				ensureMsgf(false, TEXT("Dream: a queued Python reference of generation %u outlived its interpreter; leaked."), Item.Generation);
			}
		}
	}

	FDreamRunService* Runs = DreamPythonPrivate::RunService();
	if (Impl->bResetPending && IsReady() && Runs != nullptr && !Runs->IsRunActive())
	{
		Impl->bResetPending = false;
		TArray<FString> Removed;
		{
			FDreamScopedGil Gil;
			PyObject* Result = DreamPythonPrivate::CallHelper(Impl->ResetSessionFn, "dream_editor._impl.namespaces", "reset_session", PyTuple_New(0));
			if (Result == nullptr)
			{
				UE_LOG(LogDream, Error, TEXT("Reset Session failed: %s"), *DreamPy::FetchExceptionText());
			}
			else if (PyObject* Sequence = PySequence_Fast(Result, "reset_session returned no list"))
			{
				for (Py_ssize_t Index = 0; Index < PySequence_Fast_GET_SIZE(Sequence); ++Index)
				{
					Removed.Add(DreamPy::ToStringLossy(PySequence_Fast_GET_ITEM(Sequence, Index)));
				}
				Py_DECREF(Sequence);
			}
			PyErr_Clear();
			Py_XDECREF(Result);
		}
		UE_LOG(LogDream, Log, TEXT("Reset Session: %d name(s) removed%s%s."), Removed.Num(), Removed.IsEmpty() ? TEXT("") : TEXT(": "),
			*FString::Join(Removed, TEXT(", ")));
		Runs->NotifySessionReset();
		++Impl->SessionEpoch;       // the aliases left the Session and their ownership table is cleared (§5.3.3, U19)
	}

	if (Impl->bBootPending && GetState() == EDreamPythonState::Booting)
	{
		Impl->bBootPending = false;
		Boot();
	}
#endif
	return IsReady();
}

// ---- Lifecycle delegates (§3.5, §3.8) -----------------------------------------------------------------------------

void FDreamPython::HandleInitialized()
{
	check(IsInGameThread());
#if WITH_PYTHON
	IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
	if (Plugin != nullptr && !Plugin->OnPythonShutdown().IsBoundToObject(this))
	{
		Plugin->OnPythonShutdown().AddRaw(this, &FDreamPython::HandleShutdown);
	}
	if (Impl->EnableTimeout.IsValid())
	{
		FDreamDeferral::RemoveTicker(Impl->EnableTimeout);
		Impl->EnableTimeout.Reset();
	}

	const uint32 NewGeneration = ++Impl->Generation;
	Impl->bModuleCreated = false;
	Impl->bBooted = false;
	Impl->bBootPending = false;
	Impl->FailureReason = FText::GetEmpty();

	FText Reason;
	bool bCreated = false;
	{
		// OnPythonInitialized is broadcast with the GIL released (the plugin saved the main thread state after init).
		FDreamScopedGil Gil;
		DreamPythonPrivate::GAliveGeneration = NewGeneration;
		DreamPythonPrivate::GCountedGeneration = NewGeneration;
		DreamPythonPrivate::GLiveRefs = 0;
		bCreated = FDreamPyCompat::CheckRuntime(Reason) && DreamNative::CreateModule(NewGeneration, Impl->bHeadless, Reason);
		if (bCreated)
		{
			// bPythonAlive under PostLock before any BeginRun (§3.5).
			FDreamEditorModule::Get().GetSupervisor().OnPythonInitialized(NewGeneration);
		}
	}
	if (FDreamRunService* Runs = DreamPythonPrivate::RunService())
	{
		Runs->NotifyPythonRestarted();
	}
	if (!bCreated)
	{
		UE_LOG(LogDream, Error, TEXT("%s"), *Reason.ToString());
		Impl->FailureReason = Reason;
		Impl->SetState(*this, EDreamPythonState::BootFailed);
		return;
	}
	Impl->bModuleCreated = true;
	if (Impl->bHeadless)
	{
		Impl->SetState(*this, EDreamPythonState::Headless);   // _dream exists for -run=PythonScript (tier 5); no executor, no runs
		return;
	}
	Impl->SetState(*this, EDreamPythonState::Cold);
	InitializedEvent.Broadcast();
	if (Impl->bStartWanted)
	{
		Impl->bStartWanted = false;
		RequestStart(EDreamStartReason::RunRequest);
	}
#endif
}

void FDreamPython::HandleShutdown()
{
	check(IsInGameThread());
#if WITH_PYTHON
	// The plugin restored its main thread state before the broadcast: the GIL is held here (07a Q6).
	const uint32 OldGeneration = Impl->Generation.load();
	Impl->SetState(*this, EDreamPythonState::ShuttingDown);

	// Step 1: shutdown while executing is pathological (user code called ForceDisablePythonAtRuntime).
	FDreamRunService* Runs = DreamPythonPrivate::RunService();
	const bool bExecuting = Runs != nullptr && Runs->IsRunActive();
	if (bExecuting)
	{
		const FDreamRunRecord* Active = Runs->GetActiveRecord();
		UE_LOG(LogDream, Error, TEXT("Python was shut down during run #%d."), Active ? Active->RunNumber : 0);
		Runs->NotifyPythonShutdownDuringRun();
	}

	// Step 2: invalidate the run, wait (GIL released) for the watchdog to leave Python, delete its thread state (RV-01).
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetSupervisor().OnPythonShutdown();
	}

	// Step 3: (lang) the Stage 1 stub snapshot forgets its walk (§7.4.2); FDreamStubSnapshot stops its ticker on OnShutdown.
	// Never while executing, as step 4 (no slice runs during a run, so no walk is mid-slice then).
	if (!bExecuting)
	{
		DreamPythonSnapshot::OnPythonShutdown();
	}
	// (completion) The resolver (§7.5) holds no Python object across calls: FDreamCompletionService drops its caches and
	// queued requests on the ShuttingDown state set above (§7.11), and its tickers need Python, so none runs after this.
	// Deferred (M2): close the debug server (DebugpyBridge).

	// Step 4: dream_editor._shutdown(generation), best effort, never while executing.
	if (!bExecuting && Impl->bBooted)
	{
		PyObject* Name = PyUnicode_FromString("dream_editor");
		PyObject* Package = Name ? PyImport_GetModule(Name) : nullptr;
		Py_XDECREF(Name);
		if (Package != nullptr)
		{
			PyObject* Result = PyObject_CallMethod(Package, "_shutdown", "I", static_cast<unsigned int>(OldGeneration));
			if (Result == nullptr)
			{
				UE_LOG(LogDream, Warning, TEXT("dream_editor._shutdown failed: %s"), *DreamPy::FetchExceptionText());
			}
			Py_XDECREF(Result);
			Py_DECREF(Package);
		}
		PyErr_Clear();
	}

	// The console executor unregisters here, before refs are released (§6.6).
	ShutdownEvent.Broadcast();

	// Step 5: release every ref, drain the decref queue, assert the registry is empty. The linecache entries die with the
	// interpreter, so saved paths still waiting for their idle tick have nothing left to drop.
	Impl->CancelSavedPaths();
	Impl->ReleaseHelpers();
	DreamNative::DestroyModule();
	DreamPythonPrivate::FQueuedDecref Item;
	while (DreamPythonPrivate::DecrefQueue().Dequeue(Item))
	{
		--DreamPythonPrivate::GQueuedDecrefs;
		if (Item.Generation == OldGeneration)
		{
			Py_DECREF(Item.Object);
		}
	}
	ensureMsgf(DreamPythonPrivate::GLiveRefs.load() == 0,
		TEXT("Dream: %d Python references are still alive at Python shutdown; they are leaked, not released."),
		DreamPythonPrivate::GLiveRefs.load());
	DreamPythonPrivate::GAliveGeneration = 0;
	DreamPythonPrivate::GCountedGeneration = 0;
	DreamPythonPrivate::GLiveRefs = 0;

	// Step 6: OnPythonInitialized is bound again (its own broadcast cleared it), state ShutDown, banner.
	if (IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get())
	{
		if (!Plugin->OnPythonInitialized().IsBoundToObject(this))
		{
			Plugin->OnPythonInitialized().AddRaw(this, &FDreamPython::HandleInitialized);
		}
	}
	Impl->bModuleCreated = false;
	Impl->bBooted = false;
	Impl->bBootPending = false;
	Impl->bResetPending = false;
	Impl->ShutDownLocal = FDateTime::Now();
	Impl->SetState(*this, EDreamPythonState::ShutDown);
#endif
}

// ---- Helpers (§12.1.3, INV-6) ----------------------------------------------------------------------------------------

FDreamPrepareResult FDreamPython::Prepare(FDreamRunId Id, const FDreamRunRecord& Record, const FDreamRunRequest& Request)
{
	check(IsInGameThread());
	FDreamPrepareResult Result;
#if WITH_PYTHON
	if (!IsReady())
	{
		Result.InternalError = TEXT("Python is not ready");
		return Result;
	}
	FDreamScopedGil Gil;
	PyObject* RequestDict = DreamPythonPrivate::BuildPrepareRequest(Id, Record, Request);
	PyObject* Args = RequestDict ? Py_BuildValue("(KN)", static_cast<unsigned long long>(Id), RequestDict) : nullptr;
	PyObject* Returned = DreamPythonPrivate::CallHelper(Impl->PrepareFn, "dream_editor._impl.runner", "prepare", Args);
	if (Returned == nullptr)
	{
		Result.InternalError = DreamPy::FetchExceptionText();
		if (Result.InternalError.IsEmpty())
		{
			Result.InternalError = TEXT("runner.prepare failed");
		}
		return Result;
	}
	DreamPythonPrivate::ParsePrepareResult(Returned, Result);
	Py_DECREF(Returned);
	PyErr_Clear();
	if (Result.AliasesWritten > 0)
	{
		++Impl->SessionEpoch;   // U19: Dream rewrote aliases (§7.5.3 cached roots)
	}
#else
	Result.InternalError = TEXT("This editor was built without Python");
#endif
	return Result;
}

void FDreamPython::Discard(FDreamRunId Id)
{
	check(IsInGameThread());
#if WITH_PYTHON
	if (!IsReady())
	{
		return;
	}
	FDreamScopedGil Gil;
	PyObject* Returned = DreamPythonPrivate::CallHelper(Impl->DiscardFn, "dream_editor._impl.runner", "discard",
		Py_BuildValue("(K)", static_cast<unsigned long long>(Id)));
	if (Returned == nullptr)
	{
		UE_LOG(LogDream, Warning, TEXT("runner.discard failed: %s"), *DreamPy::FetchExceptionText());
	}
	Py_XDECREF(Returned);
#endif
}

TValueOrError<EDreamReplClassify, FText> FDreamPython::ClassifyInput(const FString& Source, bool bLastLogicalIndented)
{
	check(IsInGameThread());
#if WITH_PYTHON
	if (!IsReady())
	{
		return MakeError(LOCTEXT("ClassifyNotReady", "Python is not ready yet."));
	}
	const FDreamRunService* Runs = DreamPythonPrivate::RunService();
	if ((Runs != nullptr && Runs->IsRunActive()) || FDreamEditorModule::Get().GetSupervisor().GetPumpDepth() > 0)
	{
		return MakeError(LOCTEXT("ClassifyDuringRun", "A run is active."));   // INV-6: never while executing
	}
	FDreamScopedGil Gil;
	PyObject* SourceObject = DreamPy::FromString(Source);
	PyObject* Args = SourceObject ? Py_BuildValue("(NO)", SourceObject, bLastLogicalIndented ? Py_True : Py_False) : nullptr;
	PyObject* Returned = DreamPythonPrivate::CallHelper(Impl->ClassifyFn, "dream_editor._impl.runner", "classify_input", Args);
	if (Returned == nullptr)
	{
		const FString Error = DreamPy::FetchExceptionText();
		UE_LOG(LogDream, Warning, TEXT("runner.classify_input failed: %s"), *Error);
		return MakeError(FText::FromString(Error));
	}
	FString Verdict;
	if (!DreamPy::ToString(Returned, Verdict))
	{
		PyErr_Clear();
	}
	Py_DECREF(Returned);
	if (Verdict == TEXT("complete"))
	{
		return MakeValue(EDreamReplClassify::Complete);
	}
	if (Verdict == TEXT("incomplete"))
	{
		return MakeValue(EDreamReplClassify::Incomplete);
	}
	if (Verdict == TEXT("invalid"))
	{
		return MakeValue(EDreamReplClassify::Invalid);
	}
	return MakeError(FText::Format(LOCTEXT("ClassifyBadResult", "runner.classify_input returned \"{0}\"."), FText::FromString(Verdict)));
#else
	return MakeError(LOCTEXT("ClassifyNoPython", "This editor was built without Python."));
#endif
}

void FDreamPython::NotifyBufferSaved(const FString& Path)
{
	check(IsInGameThread());
#if WITH_PYTHON
	// Only a booted dream_editor has run code and registered linecache entries; Path must be a real file (§5.3.2).
	const uint32 Alive = DreamPythonPrivate::GAliveGeneration.load();
	if (Path.IsEmpty() || Path.StartsWith(TEXT("<")) || !Impl->bBooted || Alive == 0 || Alive != GetGeneration())
	{
		return;
	}
	if (Impl->SavedPathsGeneration != Alive)
	{
		Impl->CancelSavedPaths();
		Impl->SavedPathsGeneration = Alive;
	}
	Impl->SavedPaths.AddUnique(Path);
	if (Impl->SavedPathsTicker.IsValid())
	{
		return;
	}
	// INV-6: the helper runs from an idle tick, never from the save completion that called us (a Slate or ticker context).
	Impl->SavedPathsTicker = FDreamDeferral::AddTicker(TEXT("DreamBufferSaved"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
		/*bNeedsPython*/ true, [this](float)
		{
			Impl->SavedPathsTicker.Reset();
			TArray<FString> Paths = MoveTemp(Impl->SavedPaths);
			Impl->SavedPaths.Reset();
			const uint32 Generation = Impl->SavedPathsGeneration;
			Impl->SavedPathsGeneration = 0;
			if (Generation == 0 || Generation != DreamPythonPrivate::GAliveGeneration.load() || !Impl->bBooted)
			{
				return false;   // a later generation has none of these entries
			}
			FDreamScopedGil Gil;
			for (const FString& SavedPath : Paths)
			{
				PyObject* PathObject = DreamPy::FromString(SavedPath);
				PyObject* Args = PathObject ? Py_BuildValue("(N)", PathObject) : nullptr;
				PyObject* Returned = DreamPythonPrivate::CallHelper(Impl->OnBufferSavedFn, "dream_editor._impl.runner", "on_buffer_saved", Args);
				if (Returned == nullptr)
				{
					UE_LOG(LogDream, Warning, TEXT("runner.on_buffer_saved failed for %s: %s"), *SavedPath, *DreamPy::FetchExceptionText());
				}
				Py_XDECREF(Returned);
			}
			PyErr_Clear();
			return false;
		});
#else
	(void)Path;
#endif
}

void FDreamPython::EmergencyRestore()
{
	check(IsInGameThread());
#if WITH_PYTHON
	if (!IsReady())
	{
		return;
	}
	FDreamScopedGil Gil;
	PyObject* Returned = DreamPythonPrivate::CallHelper(Impl->EmergencyRestoreFn, "dream_editor._impl.streams", "emergency_restore", PyTuple_New(0));
	if (Returned == nullptr)
	{
		UE_LOG(LogDream, Error, TEXT("streams.emergency_restore failed: %s"), *DreamPy::FetchExceptionText());
	}
	Py_XDECREF(Returned);
#endif
}

#undef LOCTEXT_NAMESPACE
