// SPDX-License-Identifier: Apache-2.0
// Runtime items in the Problems tab (OUT-11, the runtime part; TDD §7.8.1 runtime items). Game thread.
// (A34) The runtime items stay here, with the entry points FDreamRunService calls (OnRunStarted at Starting, an item per
// error outcome); FDreamLanguageServices' Problems model (Language/DreamProblemsModel.h) reads them and merges them with the
// lint and parse items for the Problems view and the F8 walk (B56).
// Body: DreamRuntimeProblems.cpp (lang group in slice B).
#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

namespace dream { struct ChangeSet; }
class FDreamBuffer;

struct FDreamRuntimeProblem
{
	FString SourceKey;          // buffer ID (32 hex), the file path of a Run File, or "repl" / "console"
	int32 RunNumber = 0;
	FString File;               // real path or pseudo filename of the raising frame
	int32 Line = 0;             // 1-based
	/** "ZeroDivisionError: division by zero": FDreamRunRecord::Detail as the runner reports it ("<Type>: <first line of
	 *  str(exc)>", at most 200 characters, ruling R1); cut to MaxMessageChars here. */
	FString Message;
	/** (slice A choice) The open buffer File resolves to (FDreamBufferManager::ResolveFilename) when the item was added;
	 *  invalid for a file that is not open. Its edits move or clear the item (OUT-11 "when the line is edited"). */
	FGuid BufferId;
};

class DREAMEDITOR_API FDreamRuntimeProblems
{
public:
	static constexpr int32 MaxMessageChars = 500;       // §7.8.1 (proposed)

	FDreamRuntimeProblems();
	~FDreamRuntimeProblems();

	/** At Starting (§5.2.4): clears the runtime items of that source. Game thread. */
	void OnRunStarted(const FString& SourceKey);
	/** From FDreamRunService::FinishRun for an error outcome with a location (not for internal errors). Resolves File to an
	 *  open buffer when BufferId is unset. Game thread. */
	void AddRuntimeError(const FDreamRuntimeProblem& Problem);
	/** Problems > Clear runtime items. */
	void ClearRuntimeItems();

	TConstArrayView<FDreamRuntimeProblem> GetItems() const { return Items; }

	/** An edit of an item's line removes it; items below an edit move with the text (§7.8.1 anchors). Bound to the
	 *  buffer's OnChanged while it has items. */
	void OnBufferEdited(const FGuid& BufferId, const dream::ChangeSet& Changes);

	DECLARE_EVENT(FDreamRuntimeProblems, FOnChanged);
	FOnChanged& OnChanged() { return ChangedEvent; }

private:
	void BindBuffer(const TSharedPtr<FDreamBuffer>& Buffer);
	void UnbindUnusedBuffers();

	TArray<FDreamRuntimeProblem> Items;
	FOnChanged ChangedEvent;
	struct FBound
	{
		TWeakPtr<FDreamBuffer> Buffer;
		FDelegateHandle Handle;
	};
	TMap<FGuid, FBound> BoundBuffers;
};
