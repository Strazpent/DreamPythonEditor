// SPDX-License-Identifier: Apache-2.0
// The Stage 1 snapshot helpers (TDD §7.4.2). See DreamPythonSnapshot.h.
#include "Python/DreamPythonSnapshot.h"

#include "Module/DreamEditorModule.h"
#include "Python/DreamGil.h"
#include "Python/DreamPyUtil.h"
#include "Python/DreamPython.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"

namespace DreamPythonSnapshotPrivate
{
	constexpr const char* StubsModule = "dream_editor._impl.stubs";

	/** INV-6, D14(5): Python Ready, no run active, no pump on the stack, game thread. */
	bool CanCall()
	{
		if (!IsInGameThread() || !FDreamEditorModule::IsAvailable())
		{
			return false;
		}
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		return Module.GetPython().IsReady() && !Module.GetRunService().IsRunActive() && Module.GetSupervisor().GetPumpDepth() == 0;
	}

#if WITH_PYTHON
	/** dream_editor._impl.stubs, a new reference (imported on first use), or null with a Python error set. */
	PyObject* GetStubs(bool bImport)
	{
		PyObject* Name = PyUnicode_FromString(StubsModule);
		if (Name == nullptr)
		{
			return nullptr;
		}
		PyObject* Module = PyImport_GetModule(Name);    // sys.modules lookup; no import machinery when loaded
		Py_DECREF(Name);
		if (Module == nullptr && !PyErr_Occurred() && bImport)
		{
			Module = PyImport_ImportModule(StubsModule);
		}
		return Module;
	}

	/** json.loads(Text), a new reference, or null with a Python error set. */
	PyObject* JsonLoads(const FString& Text)
	{
		PyObject* Json = PyImport_ImportModule("json");
		if (Json == nullptr)
		{
			return nullptr;
		}
		PyObject* Source = DreamPy::FromString(Text);
		PyObject* Result = Source != nullptr ? PyObject_CallMethod(Json, "loads", "O", Source) : nullptr;
		Py_XDECREF(Source);
		Py_DECREF(Json);
		return Result;
	}
#endif
}

FString DreamPythonSnapshot::GetPythonVersion()
{
#if WITH_PYTHON
	FString Version = UTF8_TO_TCHAR(Py_GetVersion());
	int32 Space = INDEX_NONE;
	if (Version.FindChar(TEXT(' '), Space))
	{
		Version.LeftInline(Space);
	}
	return Version;
#else
	return FString();
#endif
}

bool DreamPythonSnapshot::Start(const FString& Key, const FString& EngineJson, const FString& DoneJson, FString& OutError)
{
	using namespace DreamPythonSnapshotPrivate;
	if (!CanCall())
	{
		OutError = TEXT("Python is not ready");
		return false;
	}
#if WITH_PYTHON
	FDreamScopedGil Gil;
	PyObject* Stubs = GetStubs(/*bImport*/ true);
	PyObject* Engine = Stubs != nullptr ? JsonLoads(EngineJson) : nullptr;
	PyObject* Done = Engine != nullptr ? JsonLoads(DoneJson) : nullptr;
	PyObject* KeyText = Done != nullptr ? DreamPy::FromString(Key) : nullptr;
	PyObject* Result = KeyText != nullptr ? PyObject_CallMethod(Stubs, "start", "OOO", KeyText, Engine, Done) : nullptr;
	const bool bOk = Result != nullptr;
	if (!bOk)
	{
		OutError = DreamPy::FetchExceptionText();
	}
	Py_XDECREF(Result);
	Py_XDECREF(KeyText);
	Py_XDECREF(Done);
	Py_XDECREF(Engine);
	Py_XDECREF(Stubs);
	PyErr_Clear();
	return bOk;
#else
	(void)Key;
	(void)EngineJson;
	(void)DoneJson;
	OutError = TEXT("Python is not available");
	return false;
#endif
}

bool DreamPythonSnapshot::Step(int64 BudgetNs, TArray<FDreamStubOutput>& Out, FString& OutError)
{
	using namespace DreamPythonSnapshotPrivate;
	if (!CanCall())
	{
		OutError = TEXT("Python is not ready");
		return false;
	}
#if WITH_PYTHON
	FDreamScopedGil Gil;
	PyObject* Stubs = GetStubs(/*bImport*/ false);
	if (Stubs == nullptr)
	{
		OutError = PyErr_Occurred() ? DreamPy::FetchExceptionText() : FString(TEXT("dream_editor._impl.stubs is not imported"));
		PyErr_Clear();
		return false;
	}
	PyObject* Result = PyObject_CallMethod(Stubs, "step", "L", static_cast<long long>(BudgetNs));
	Py_DECREF(Stubs);
	if (Result == nullptr || !PyList_Check(Result))
	{
		OutError = Result == nullptr ? DreamPy::FetchExceptionText() : FString(TEXT("stubs.step did not return a list"));
		Py_XDECREF(Result);
		PyErr_Clear();
		return false;
	}
	bool bOk = true;
	const Py_ssize_t Count = PyList_GET_SIZE(Result);
	for (Py_ssize_t Index = 0; Index < Count && bOk; ++Index)
	{
		PyObject* Item = PyList_GET_ITEM(Result, Index);     // borrowed
		if (!PyTuple_Check(Item) || PyTuple_GET_SIZE(Item) != 3)
		{
			bOk = false;
			break;
		}
		PyObject* Kind = PyTuple_GET_ITEM(Item, 0);
		PyObject* Module = PyTuple_GET_ITEM(Item, 1);
		PyObject* Data = PyTuple_GET_ITEM(Item, 2);
		FDreamStubOutput Output;
		if (!DreamPy::IsStrictInt(Kind) || !DreamPy::ToString(Module, Output.Module) || !PyBytes_Check(Data))
		{
			bOk = false;
			break;
		}
		Output.Kind = static_cast<int32>(PyLong_AsLong(Kind));
		const Py_ssize_t Size = PyBytes_GET_SIZE(Data);
		Output.Data.SetNumUninitialized(static_cast<int32>(Size));
		if (Size > 0)
		{
			FMemory::Memcpy(Output.Data.GetData(), PyBytes_AS_STRING(Data), static_cast<SIZE_T>(Size));
		}
		Out.Add(MoveTemp(Output));
	}
	Py_DECREF(Result);
	if (!bOk)
	{
		OutError = TEXT("stubs.step returned a malformed output");
	}
	PyErr_Clear();
	return bOk;
#else
	(void)BudgetNs;
	(void)Out;
	OutError = TEXT("Python is not available");
	return false;
#endif
}

void DreamPythonSnapshot::Stop()
{
	using namespace DreamPythonSnapshotPrivate;
	if (!CanCall())
	{
		return;
	}
#if WITH_PYTHON
	FDreamScopedGil Gil;
	OnPythonShutdown();
#endif
}

void DreamPythonSnapshot::OnPythonShutdown()
{
#if WITH_PYTHON
	using namespace DreamPythonSnapshotPrivate;
	PyObject* Stubs = GetStubs(/*bImport*/ false);
	if (Stubs != nullptr)
	{
		PyObject* Result = PyObject_CallMethod(Stubs, "stop", nullptr);
		Py_XDECREF(Result);
		Py_DECREF(Stubs);
	}
	PyErr_Clear();
#endif
}
