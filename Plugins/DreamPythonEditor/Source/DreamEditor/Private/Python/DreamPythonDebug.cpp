// SPDX-License-Identifier: Apache-2.0
// DreamPythonDebug::CallHandleRequest (TDD §8.1.2, §8.3.3). Debug group (PO-06 F5). The callable is looked up in sys.modules
// per call, so no PyObject outlives the call and HandleShutdown has nothing of ours to release.
#include "Python/DreamPythonDebug.h"

#include "Python/DreamGil.h"
#include "Python/DreamPyUtil.h"
#include "Python/DreamPython.h"

bool DreamPythonDebug::CallHandleRequest(const FString& RequestJson, FString& OutResponse)
{
#if WITH_PYTHON
	check(IsInGameThread());
	if (FDreamPython::GetAliveGeneration() == 0)
	{
		OutResponse = TEXT("Python is not running");
		return false;
	}
	FDreamScopedGil Gil;
	PyObject* Name = PyUnicode_FromString("dream_editor._impl.debugger");
	PyObject* Module = Name != nullptr ? PyImport_GetModule(Name) : nullptr;    // sys.modules only: a Debug run imported it
	Py_XDECREF(Name);
	if (Module == nullptr)
	{
		if (PyErr_Occurred())
		{
			PyErr_Clear();
		}
		OutResponse = TEXT("the debugger module is not loaded");
		return false;
	}
	PyObject* Text = DreamPy::FromString(RequestJson);
	PyObject* Result = Text != nullptr ? PyObject_CallMethod(Module, "handle_request", "O", Text) : nullptr;
	Py_XDECREF(Text);
	Py_DECREF(Module);
	if (Result == nullptr)
	{
		OutResponse = DreamPy::FetchExceptionText();
		return false;
	}
	const bool bOk = DreamPy::ToString(Result, OutResponse);
	Py_DECREF(Result);
	if (!bOk)
	{
		PyErr_Clear();
		OutResponse = TEXT("handle_request returned a non-str");
	}
	return bOk;
#else
	OutResponse = TEXT("Python is not available");
	return false;
#endif
}
