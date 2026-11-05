// SPDX-License-Identifier: Apache-2.0
// The debugger's request call (TDD §8.1.2 FDreamPython::CallDebugger, §8.3.3). Debug group (PO-06 F5). This header includes
// no Python.h, so Debug/* may include it; the body (DreamPythonDebug.cpp) lives in Private/Python (rule 6).
#pragma once

#include "CoreMinimal.h"

namespace DreamPythonDebug
{
	/**
	 * dream_editor._impl.debugger.handle_request(RequestJson) under FDreamScopedGil, on the game thread, from a clean point
	 * (the Paused window's modal-loop tick). True with the response text; false with the reason in OutResponse when
	 * Python is not alive or the call failed (the Python error is cleared).
	 */
	DREAMEDITOR_API bool CallHandleRequest(const FString& RequestJson, FString& OutResponse);
}
