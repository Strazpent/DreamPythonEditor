// SPDX-License-Identifier: Apache-2.0
// FDreamFileSaver (TDD §10.14): the game thread hands over a snapshot; a worker task encodes it (§4.13), resolves the final
// path, writes a temp file in the target's directory, flushes it and replaces the target (ReplaceFileW / MoveFileExW
// write-through; in place with a backup for hard links); the result is applied on the game thread through FDreamDeferral, so
// it never lands while user code is on the stack (RT-01). Never runs on the journal writer. Before the replace the worker
// registers the expected stamp of the written bytes with FDreamFileWatch, so the change event of Dream's own write is
// ignored whichever arrives first (§10.14 step 6, §10.15). Slice-internal.
#pragma once

#include "CoreMinimal.h"
#include "Buffers/DreamBuffer.h"

/** Why a save failed (the tab's bar picks its actions from it, §10.14 step 5, §11.6.5, §4.13). */
enum class EDreamSaveFailure : uint8
{
	None,
	Io,                 // an OS error: "Not saved: <OS error>. [Retry] [Save As…]"
	ReadOnlyTarget,     // the file has the read-only attribute: [Make writable] [Save As…] (BUF-05 M1; Check out is M4)
	Encoding,           // the file's encoding cannot hold a character: [Save as UTF-8] (BUF-06)
	BufferState,        // the buffer cannot be saved now (closed, pending recovery, read-only buffer, open elsewhere)
};

struct FDreamSaveResult
{
	bool bOk = false;
	FText Error;                    // "Not saved: <OS error>" and the encoding refusal (§10.14 step 5, §4.13)
	EDreamSaveFailure Failure = EDreamSaveFailure::None;
	int32 BadLine = 0;              // Encoding: the 1-based line of the first character the encoding cannot hold
	FString FinalPath;              // the resolved target (a symlink writes the real file, §10.14 step 2)
	FDreamBaseVersion Base;          // size, mtime and SHA-256 of the written bytes
};

struct FDreamSaveOptions
{
	/** Make writable (BUF-05 M1, §11.6.5): the worker clears the target's read-only attribute before it writes. */
	bool bMakeWritable = false;
	/** (scratch, R-p1-3) The save adds its local-history entry (PER-04). A throttled scratch auto-save commits the META only. */
	bool bHistoryEntry = true;
};

class DREAMEDITOR_API FDreamFileSaver
{
public:
	/** GT call; the callback runs on the game thread, outside runs. */
	static void Save(const FString& Path, const dream::TextSnapshot& Snapshot, const dream::codec::FileFormat& Format,
		TFunction<void(const FDreamSaveResult&)> OnGameThread, const FDreamSaveOptions& Options = FDreamSaveOptions());

	/** The worker part, synchronous (any thread but the game thread in the editor; tests call it directly). (scratch) The
	 *  scratch files' exit save calls it on the game thread at OnPreExit, when no async result could land any more. */
	static FDreamSaveResult SaveNow(const FString& Path, const dream::TextSnapshot& Snapshot, const dream::codec::FileFormat& Format,
		const FDreamSaveOptions& Options = FDreamSaveOptions());

	/** (scratch) Waits until every worker write that Save started has finished (their game-thread results may still be
	 *  queued), at most TimeoutSeconds; false on the timeout. OnPreExit only, before a synchronous save of the same files. */
	static bool WaitForWorkerWrites(double TimeoutSeconds);
};
