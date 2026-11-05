// SPDX-License-Identifier: Apache-2.0
// _dream_test (see DreamTestPython.h). CPython only through DreamEditor's DreamPythonInclude.h (§2.5 rule 6; the scan allows
// this test-only folder, tests B-note). Every function runs on the game thread with the GIL held, inside a run.
#include "Python/DreamTestPython.h"

#include "Containers/Ticker.h"
#include "Fixtures/DreamInputBot.h"
#include "Fixtures/DreamTestLatch.h"
#include "Fixtures/DreamTestModalDialog.h"
#include "Fixtures/DreamTestProbes.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPythonScriptPlugin.h"
#include "Misc/App.h"
#include "Module/DreamEditorModule.h"
#include "Python/DreamGil.h"
#include "Python/DreamPythonInclude.h"
#include "Run/RunSupervisor.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#if WITH_PYTHON

namespace DreamTestPythonPrivate
{
	bool bRegistered = false;
	bool bModuleCreated = false;
	FDelegateHandle InitializedHandle;
	FDelegateHandle ShutdownHandle;

	/** FillPendingCallQueue: until this time, each no-op call queues itself again (§5.7.5 saturation). */
	double FillUntil = 0.0;

	FString ArgString(PyObject* Object)
	{
		if (Object == nullptr || !PyUnicode_Check(Object))
		{
			return FString();
		}
		const char* Utf8 = PyUnicode_AsUTF8(Object);
		return Utf8 ? FString(UTF8_TO_TCHAR(Utf8)) : FString();
	}

	bool ParseName(PyObject* Args, const char* Function, FString& OutName)
	{
		PyObject* Name = nullptr;
		if (!PyArg_ParseTuple(Args, "U", &Name))
		{
			return false;
		}
		OutName = ArgString(Name);
		if (OutName.IsEmpty())
		{
			PyErr_Format(PyExc_ValueError, "%s(): the latch name must not be empty", Function);
			return false;
		}
		return true;
	}

	// ---- DreamTestLatch ----

	PyObject* Wait(PyObject* /*Self*/, PyObject* Args)
	{
		FString Name;
		if (!ParseName(Args, "wait", Name))
		{
			return nullptr;
		}
		PyObject* Package = PyImport_ImportModule("dream_editor");
		if (Package == nullptr)
		{
			return nullptr;
		}
		PyObject* Sleep = PyObject_GetAttrString(Package, "sleep");
		Py_DECREF(Package);
		if (Sleep == nullptr)
		{
			return nullptr;
		}
		DreamTest::FDreamTestLatch::BeginWait(Name);
		PyObject* Result = Py_None;
		while (DreamTest::FDreamTestLatch::IsHeld(Name))
		{
			// The run-scoped sliced sleep (§5.10.3): Stop raises ScriptCancelled here, as in any sleep (§14.2).
			PyObject* Slept = PyObject_CallFunction(Sleep, "d", 0.01);
			if (Slept == nullptr)
			{
				Result = nullptr;
				break;
			}
			Py_DECREF(Slept);
		}
		DreamTest::FDreamTestLatch::EndWait(Name);
		Py_DECREF(Sleep);
		if (Result == nullptr)
		{
			return nullptr;
		}
		Py_RETURN_NONE;
	}

	PyObject* Held(PyObject* /*Self*/, PyObject* Args)
	{
		FString Name;
		if (!ParseName(Args, "held", Name))
		{
			return nullptr;
		}
		return PyBool_FromLong(DreamTest::FDreamTestLatch::IsHeld(Name) ? 1 : 0);
	}

	PyObject* Release(PyObject* /*Self*/, PyObject* Args)
	{
		FString Name;
		if (!ParseName(Args, "release", Name))
		{
			return nullptr;
		}
		DreamTest::FDreamTestLatch::Release(Name);
		Py_RETURN_NONE;
	}

	PyObject* Mark(PyObject* /*Self*/, PyObject* Args)
	{
		FString Name;
		if (!ParseName(Args, "mark", Name))
		{
			return nullptr;
		}
		return PyFloat_FromDouble(DreamTest::FDreamTestLatch::Mark(Name));
	}

	PyObject* Now(PyObject* /*Self*/, PyObject* /*Args*/)
	{
		return PyFloat_FromDouble(FPlatformTime::Seconds());
	}

	// ---- §5.15 fixtures ----

	PyObject* SleepNative(PyObject* /*Self*/, PyObject* Args)
	{
		double Seconds = 0.0;
		if (!PyArg_ParseTuple(Args, "d", &Seconds))
		{
			return nullptr;
		}
		if (Seconds < 0.0 || Seconds > 600.0)
		{
			PyErr_SetString(PyExc_ValueError, "sleep_native(): seconds must be in [0, 600]");
			return nullptr;
		}
		// A native call that keeps the GIL, like most engine calls from Python: no bytecode runs, so no pending call can, and
		// the watchdog measures the interval (§5.8).
		const double End = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < End)
		{
			FPlatformProcess::SleepNoStats(static_cast<float>(FMath::Min(0.01, End - FPlatformTime::Seconds())));
		}
		Py_RETURN_NONE;
	}

	PyObject* TickCoreTicker(PyObject* /*Self*/, PyObject* Args)
	{
		int Count = 1;
		if (!PyArg_ParseTuple(Args, "|i", &Count))
		{
			return nullptr;
		}
		if (Count < 0 || Count > 10000)
		{
			PyErr_SetString(PyExc_ValueError, "tick_core_ticker(): n must be in [0, 10000]");
			return nullptr;
		}
		// What user code can do mid-run (RT-01): tick the core ticker from inside a run. Dream's ticker clients must defer.
		const float Delta = static_cast<float>(FMath::Max(FApp::GetDeltaTime(), 1.0 / 60.0));
		for (int Index = 0; Index < Count; ++Index)
		{
			FTSTicker::GetCoreTicker().Tick(Delta);
		}
		return PyLong_FromLong(Count);
	}

	int RequeueingNoop(void* /*Arg*/)
	{
		if (FPlatformTime::Seconds() < FillUntil)
		{
			Py_AddPendingCall(&RequeueingNoop, nullptr);
		}
		return 0;
	}

	PyObject* FillPendingCallQueue(PyObject* /*Self*/, PyObject* Args)
	{
		double Seconds = 1.0;
		if (!PyArg_ParseTuple(Args, "|d", &Seconds))
		{
			return nullptr;
		}
		FillUntil = FPlatformTime::Seconds() + FMath::Clamp(Seconds, 0.0, 120.0);
		// CPython 3.11 holds 32 pending calls; each no-op that runs queues itself again until FillUntil, so the queue stays
		// full and a post of Dream's fails (make_pending_calls runs at most 32 per check, so this never loops forever).
		int Queued = 0;
		while (Queued < 64 && Py_AddPendingCall(&RequeueingNoop, nullptr) == 0)
		{
			++Queued;
		}
		return PyLong_FromLong(Queued);
	}

	PyObject* PostSupervisorCall(PyObject* /*Self*/, PyObject* Args)
	{
		unsigned long long RunId = 0;
		if (!PyArg_ParseTuple(Args, "|K", &RunId))
		{
			return nullptr;
		}
		if (!FDreamEditorModule::IsAvailable())
		{
			Py_RETURN_FALSE;
		}
		// From this (the calling) thread with the GIL held: no hand-off. A post from the game thread sets the eval breaker
		// itself, so the callback runs at the next bytecode boundary of whatever Python runs next (§14.2, T-SUP-033).
		const bool bPosted = FDreamEditorModule::Get().GetSupervisor().PostPendingCallForTest(static_cast<FDreamRunId>(RunId));
		return PyBool_FromLong(bPosted ? 1 : 0);
	}

	PyObject* QueueOsKeys(PyObject* /*Self*/, PyObject* Args)
	{
		PyObject* KeysObject = nullptr;
		if (!PyArg_ParseTuple(Args, "U", &KeysObject))
		{
			return nullptr;
		}
#if PLATFORM_WINDOWS
		TArray<FString> Chords;
		ArgString(KeysObject).ParseIntoArray(Chords, TEXT(","));
		TArray<INPUT> Inputs;
		for (const FString& ChordText : Chords)
		{
			TArray<DreamTest::FDreamBotKey> Keys;
			if (!DreamTest::FDreamInputBot::ParseChord(ChordText, Keys))
			{
				PyErr_Format(PyExc_ValueError, "queue_os_keys(): unknown key %s", TCHAR_TO_UTF8(*ChordText));
				return nullptr;
			}
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				for (int32 Index = 0; Index < Keys.Num(); ++Index)
				{
					const DreamTest::FDreamBotKey& K = Pass == 0 ? Keys[Index] : Keys[Keys.Num() - 1 - Index];
					INPUT Input{};
					Input.type = INPUT_KEYBOARD;
					Input.ki.wVk = K.VirtualKey;
					Input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(K.VirtualKey, MAPVK_VK_TO_VSC));
					Input.ki.dwFlags = (K.bExtended ? KEYEVENTF_EXTENDEDKEY : 0) | (Pass == 1 ? KEYEVENTF_KEYUP : 0);
					Inputs.Add(Input);
				}
			}
		}
		FString What;
		if (!DreamTest::FDreamInputBot::IsEditorForeground(What))
		{
			PyErr_Format(PyExc_RuntimeError, "queue_os_keys(): the editor is not the foreground window (%s); keys would go elsewhere",
				TCHAR_TO_UTF8(*What));
			return nullptr;
		}
		::GetQueueStatus(QS_KEY);       // resets the "added since the last call" word
		const UINT Sent = ::SendInput(static_cast<UINT>(Inputs.Num()), Inputs.GetData(), sizeof(INPUT));
		if (Sent != static_cast<UINT>(Inputs.Num()))
		{
			PyErr_Format(PyExc_RuntimeError, "queue_os_keys(): SendInput sent %u of %d events (blocked input)", Sent, Inputs.Num());
			return nullptr;
		}
		// The raw input thread posts them to this (the foreground) thread's queue asynchronously: return once it has (§5.15).
		const double Deadline = FPlatformTime::Seconds() + 2.0;
		while (FPlatformTime::Seconds() < Deadline)
		{
			if ((HIWORD(::GetQueueStatus(QS_KEY)) & QS_KEY) != 0)
			{
				Py_RETURN_TRUE;
			}
			FPlatformProcess::SleepNoStats(0.001f);
		}
		Py_RETURN_FALSE;
#else
		PyErr_SetString(PyExc_RuntimeError, "queue_os_keys(): Win64 only");
		return nullptr;
#endif
	}

	PyObject* ShowModal(PyObject* /*Self*/, PyObject* Args)
	{
		PyObject* Title = nullptr;
		PyObject* Message = nullptr;
		if (!PyArg_ParseTuple(Args, "UU", &Title, &Message))
		{
			return nullptr;
		}
		// A native call that runs a modal loop with the GIL held, like a script's blocking dialog (§5.12).
		const FString Result = DreamTest::ShowFixtureModal(ArgString(Title), ArgString(Message));
		return PyUnicode_FromString(TCHAR_TO_UTF8(*Result));
	}

	PyObject* Probe(PyObject* /*Self*/, PyObject* Args)
	{
		FString Name;
		if (!ParseName(Args, "probe", Name))
		{
			return nullptr;
		}
		const DreamTest::FDreamProbeSnapshot Snapshot = DreamTest::FDreamTestProbes::GetSnapshot();
		FString Value;
		if (Name == TEXT("run_state"))
		{
			Value = FString::FromInt(Snapshot.RunState);
		}
		else if (Name == TEXT("run_number"))
		{
			Value = FString::FromInt(Snapshot.ActiveRunNumber);
		}
		else if (Name == TEXT("pump_depth"))
		{
			Value = FString::FromInt(Snapshot.PumpDepth);
		}
		else if (Name == TEXT("modal"))
		{
			Value = Snapshot.Modal == DreamTest::EDreamModalKind::RunWindow ? TEXT("ours")
				: Snapshot.Modal == DreamTest::EDreamModalKind::Foreign ? TEXT("foreign") : TEXT("none");
		}
		else
		{
			PyErr_Format(PyExc_ValueError, "probe(): unknown probe %s", TCHAR_TO_UTF8(*Name));
			return nullptr;
		}
		return PyUnicode_FromString(TCHAR_TO_UTF8(*Value));
	}

	PyMethodDef Methods[] = {
		{ "wait", &Wait, METH_VARARGS, "wait(name): hold the run on dream_editor.sleep(0.01) until the driver releases the latch" },
		{ "held", &Held, METH_VARARGS, "held(name) -> bool: the latch is still held" },
		{ "release", &Release, METH_VARARGS, "release(name): release a latch" },
		{ "mark", &Mark, METH_VARARGS, "mark(name) -> float: record the time a named point was passed" },
		{ "now", &Now, METH_NOARGS, "now() -> float: FPlatformTime::Seconds()" },
		{ "sleep_native", &SleepNative, METH_VARARGS, "sleep_native(seconds): a native call that keeps the GIL" },
		{ "tick_core_ticker", &TickCoreTicker, METH_VARARGS, "tick_core_ticker(n=1) -> int: tick the core ticker n times from inside the run" },
		{ "fill_pending_call_queue", &FillPendingCallQueue, METH_VARARGS, "fill_pending_call_queue(seconds=1.0) -> int: keep CPython's pending-call queue full" },
		{ "post_supervisor_call", &PostSupervisorCall, METH_VARARGS, "post_supervisor_call(run_id=0) -> bool: post Dream's pending callback for the run" },
		{ "queue_os_keys", &QueueOsKeys, METH_VARARGS, "queue_os_keys(keys) -> bool: SendInput the keys and wait until they are queued" },
		{ "show_modal", &ShowModal, METH_VARARGS, "show_modal(title, message) -> str: a Yes/No/Cancel modal dialog (Esc: 'cancel')" },
		{ "probe", &Probe, METH_VARARGS, "probe(name) -> str: a run-state probe published by the game thread" },
		{ nullptr, nullptr, 0, nullptr },
	};

	PyModuleDef ModuleDef = {
		PyModuleDef_HEAD_INIT,
		"_dream_test",
		"Dream Python Editor tier-4 test fixtures (TDD 14.2): latches, marks and native-call fixtures. Repository only; never shipped.",
		-1,
		Methods,
	};

	void CreateModule()
	{
		FDreamScopedGil Gil;
		PyObject* Module = PyModule_Create(&ModuleDef);
		if (Module == nullptr)
		{
			PyErr_Clear();
			return;
		}
		PyObject* Modules = PyImport_GetModuleDict();      // borrowed
		if (Modules != nullptr && PyDict_SetItemString(Modules, "_dream_test", Module) == 0)
		{
			bModuleCreated = true;
		}
		PyErr_Clear();
		Py_DECREF(Module);
	}

	void HandleInitialized();

	void HandleShutdown()
	{
		bModuleCreated = false;
		FillUntil = 0.0;
		// Both delegates are cleared after their broadcast: bind the next initialization again (RUN-10, T-LIFE-006).
		if (IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get())
		{
			InitializedHandle = Plugin->OnPythonInitialized().AddStatic(&HandleInitialized);
		}
	}

	void HandleInitialized()
	{
		CreateModule();
		if (IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get())
		{
			ShutdownHandle = Plugin->OnPythonShutdown().AddStatic(&HandleShutdown);
		}
	}
}

namespace DreamTestPython
{
	void Register()
	{
		using namespace DreamTestPythonPrivate;
		IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
		if (bRegistered || Plugin == nullptr)
		{
			return;
		}
		bRegistered = true;
		Plugin->RegisterOnPythonInitialized(FSimpleDelegate::CreateStatic(&HandleInitialized));
	}

	void Unregister()
	{
		using namespace DreamTestPythonPrivate;
		if (!bRegistered)
		{
			return;
		}
		bRegistered = false;
		if (IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get())
		{
			Plugin->OnPythonInitialized().Remove(InitializedHandle);
			Plugin->OnPythonShutdown().Remove(ShutdownHandle);
		}
	}

	bool IsModuleCreated()
	{
		return DreamTestPythonPrivate::bModuleCreated;
	}
}

#else // WITH_PYTHON

namespace DreamTestPython
{
	void Register() {}
	void Unregister() {}
	bool IsModuleCreated() { return false; }
}

#endif // WITH_PYTHON
