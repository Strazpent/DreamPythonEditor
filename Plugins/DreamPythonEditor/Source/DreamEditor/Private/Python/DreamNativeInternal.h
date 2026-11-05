// SPDX-License-Identifier: Apache-2.0
// State shared by the _dream module (DreamNativeModule.cpp) and its Writer types (DreamNativeWriter.cpp). Bridge-internal;
// include only from Private/Python. The service pointers are captured on the game thread in CreateModule, so the
// any-thread functions (write, Writer.write, progress, check_cancel) never go through FModuleManager.
#pragma once

#include "CoreMinimal.h"
#include "Python/DreamGil.h"
#include "Run/DreamRunTypes.h"

#if WITH_PYTHON

class FRunSupervisor;
class FDreamEditorModule;
class FDreamOutputStore;
class FDreamRunService;

namespace DreamNativeInternal
{
	struct FState
	{
		/** False before CreateModule and after DestroyModule: GT functions then raise RuntimeError. */
		bool bAlive = false;
		bool bHeadless = false;
		uint32 Generation = 0;
		unsigned long GameThreadIdent = 0;

		FDreamEditorModule* Module = nullptr;
		FDreamOutputStore* OutputStore = nullptr;     // kept after DestroyModule: the store outlives Python
		FDreamRunService* RunService = nullptr;
		FRunSupervisor* Supervisor = nullptr;

		FDreamPyRef CancelClass;                      // register_cancel_class (§5.7.1)
		FDreamPyRef UnsupportedOperation;             // io.UnsupportedOperation, for Writer.fileno/close/detach
		FDreamPyRef WriterType;
		FDreamPyRef BinaryWriterType;
	};

	/** GIL held (the GIL serialises every reader and writer). */
	FState& State();

	/** A run ID no run ever has (IDs are (generation << 32) | serial with a serial from 1). */
	inline constexpr FDreamRunId UnknownRunId = ~static_cast<FDreamRunId>(0);

	/** A STREAM_* argument: TypeError for a non-int (bools included), ValueError outside the accepted set. */
	bool ParseStream(PyObject* Object, int32& OutStream, bool bWriterStreamsOnly);

	/**
	 * The run_id of write() and the Writers: TypeError for a non-int. Any int is accepted and passed on unchanged (a
	 * negative or oversized one becomes UnknownRunId): the output store tags an ID that is neither current nor a known
	 * past run as run 0 and background, so a stale ID never raises (§6.1).
	 */
	bool ParseWriteRunId(PyObject* Object, FDreamRunId& OutId);

	/** One raw text record to FDreamOutputStore (origin Python). Any thread; never blocks. */
	void PushText(int32 Stream, FDreamRunId RunId, const UTF8CHAR* Utf8, int32 Bytes);

	/** Creates the Writer and BinaryWriter heap types and adds them to Module. False with a Python error set. */
	bool AddWriterTypes(PyObject* Module);
}

#endif // WITH_PYTHON
