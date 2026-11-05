// SPDX-License-Identifier: Apache-2.0
// The Stage 1 snapshot helpers (TDD §7.4.2, RT-13): typed wrappers over dream_editor._impl.stubs (start, step, stop), in
// the style of A15. Game thread, only from FDreamStubSnapshot's FDreamDeferral idle ticker (bNeedsPython: Python Ready, no run
// active, no pump on the stack, INV-6), never from a Slate handler (D14(5)). Each call takes the GIL, calls the helper and
// converts its result; no PyObject outlives the call. The walk executes no user code and calls no getter (D5).
// This header names no CPython type, so Language/ may include it (§2.5 rule 6); the body is in Private/Python.
// lang group (B52). Body: DreamPythonSnapshot.cpp.
#pragma once

#include "CoreMinimal.h"

/** One output of stubs.step (see stubs.py): Kind 0 = a JSONL line for modules/<Module>.jsonl (no newline), 1 = that module
 *  is finished (Data = its manifest entry), 2 = the walk is complete (Data = manifest.json). */
struct FDreamStubOutput
{
	int32 Kind = 0;
	FString Module;
	TArray<uint8> Data;
};

namespace DreamPythonSnapshot
{
	/** The engine CPython's version ("3.11.8"); any thread, no GIL needed (a static string). Empty without Python. */
	DREAMEDITOR_API FString GetPythonVersion();
	/** stubs.start(key, engine, done): EngineJson is an object, DoneJson an array of manifest entries. */
	DREAMEDITOR_API bool Start(const FString& Key, const FString& EngineJson, const FString& DoneJson, FString& OutError);
	/** One slice: stubs.step(budget_ns). Appends the outputs; false with the exception text when the helper raised. */
	DREAMEDITOR_API bool Step(int64 BudgetNs, TArray<FDreamStubOutput>& Out, FString& OutError);
	/** stubs.stop() when Python is Ready (a new key, a failed walk). */
	DREAMEDITOR_API void Stop();
	/**
	 * FDreamPython::HandleShutdown step 3 (§3.8): the GIL is held; forgets the walk if dream_editor._impl.stubs was imported.
	 * FDreamStubSnapshot stops its own ticker on FDreamPython::OnShutdown.
	 */
	DREAMEDITOR_API void OnPythonShutdown();
}
