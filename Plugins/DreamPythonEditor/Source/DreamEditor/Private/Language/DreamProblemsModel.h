// SPDX-License-Identifier: Apache-2.0
// The Problems model (TDD §7.8.1, §7.8.2; INT-07, OUT-11, U13): lint items from the language servers (M1: dream-check),
// Parse items from a pre-run parse that failed in Starting, and the runtime items that FDreamRuntimeProblems keeps (A34),
// merged, deduplicated per (buffer, start line, DedupClass) and sorted for the Problems view and the F8 / Shift+F8 walk.
// Owned by FDreamLanguageServices. Game thread. Body: DreamProblemsModel.cpp (lang).
// Deferred (M2): the Session-name filter of §7.8.2 (it hides undefined-name items from ruff and the type server; dream-check
// reports none), the type-server and ruff code maps and their dedup classes beyond "syntax".
#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

namespace dream { struct ChangeSet; }
class FDreamRuntimeProblems;

enum class EDreamProblemSource : uint8
{
	TypeServer,
	Ruff,
	DreamCheck,
	Parse,
	Runtime,
	/** (lang) PO-06 F4: the in-process lexer check (Language/DreamSyntaxCheck.h, TDD §7.3.6 "In process: parse check"). It
	 *  needs no server, so syntax errors show while dream-check starts, restarts or is paused. Dedup class "syntax". */
	Syntax,
};

enum class EDreamSeverity : uint8
{
	Error,
	Warning,
	Info,
	Hint,               // drawn faded, never listed (§7.8.2)
};

struct FDreamProblem
{
	/** The buffer the item is in; invalid for a runtime item raised in a file that is not open. */
	FGuid Buffer;
	EDreamProblemSource Source = EDreamProblemSource::DreamCheck;
	EDreamSeverity Severity = EDreamSeverity::Warning;
	FString Code;
	FString Message;
	/** §7.8.2: "syntax" for any syntax error, otherwise "<Source>:<Code>", which never merges. */
	FString DedupClass;
	/** 0-based lines and UTF-16 columns of the buffer's current text; edits move them (§4 anchors). */
	int32 StartLine = 0;
	int32 StartColumn = 0;
	int32 EndLine = 0;
	int32 EndColumn = 0;
	uint64 DocVersion = 0;
	uint32 RunNumber = 0;
	/** A server crashed: kept, greyed, until its first new publish or for at most 10 s (§7.3.5). */
	bool bStale = false;
	/** The server offers a fix other than "Suppress" (dream-check's data.fixable). */
	bool bHasQuickFix = false;
	/** Runtime only: the source key of the raising run, and the file of the raising frame (RLD-01 clearing, M3). */
	FString RunSourceKey;
	FString ModulePath;
	/** Runtime only: the file or pseudo filename of the raising frame, shown for items outside open buffers. */
	FString File;
	/** GetVisible only: the sources of the duplicates merged into this item, for the tooltip (§7.8.2). */
	TArray<FString> MergedFrom;
};

class DREAMEDITOR_API FDreamProblemsModel
{
public:
	static constexpr double StaleSeconds = 10.0;        // §7.3.5 (proposed)

	FDreamProblemsModel();
	~FDreamProblemsModel();

	/** Runtime items come from the output's FDreamRuntimeProblems (A34); its changes re-broadcast here. */
	void BindRuntime(FDreamRuntimeProblems* InRuntime);
	void UnbindRuntime();

	/**
	 * (lang) PO-06 F4: the buffer version a source last analysed (dream-check's publish version, the syntax check's snapshot).
	 * GetVisible and GetForBuffer show one syntax verdict per buffer: dream-check's when it analysed the same or a newer
	 * version than the in-process check and its items are not stale, otherwise the in-process check's (then dream-check's
	 * older "syntax" items are left out). Call it before the ReplaceSource of the same result.
	 */
	void NoteAnalysed(const FGuid& Buffer, EDreamProblemSource Source, uint64 Version);
	/** (lang) NoteAnalysed and ReplaceSource as one change: at most one OnChanged broadcast (review lang-q1 nit). */
	void ReplaceAnalysed(const FGuid& Buffer, EDreamProblemSource Source, uint64 Version, TArray<FDreamProblem>&& Items);
	/** (lang) The source's verdict is gone (its set was dropped without a new analysis: Save As, stale expiry). */
	void ForgetAnalysed(const FGuid& Buffer, EDreamProblemSource Source);
	/** A publish replaces that source's set for the buffer (§7.8.1 "Cleared when"). */
	void ReplaceSource(const FGuid& Buffer, EDreamProblemSource Source, TArray<FDreamProblem>&& Items);
	/** Document close: every lint and parse item of the buffer. */
	void ClearBuffer(const FGuid& Buffer);
	/** A server crashed: its items turn stale until the first new publish or StaleSeconds. */
	void MarkSourceStale(EDreamProblemSource Source, double NowSeconds);
	/** Drops stale items older than StaleSeconds. */
	void ExpireStale(double NowSeconds);
	/** Lines below an edit move with the text; a Parse item on an edited line is cleared (§7.8.1). */
	void OnBufferEdited(const FGuid& Buffer, const dream::ChangeSet& Changes);
	/** A run of the buffer started: its Parse items clear (the next parse adds a new one when it fails). */
	void ClearParse(const FGuid& Buffer);
	/** Problems > Clear runtime items. */
	void ClearRuntimeItems();

	/**
	 * Merged, deduplicated and sorted: items of open buffers in tab order, by line and column, then runtime items of files
	 * that are not open, by file and line. Hints are left out (§7.8.2).
	 */
	TArray<FDreamProblem> GetVisible() const;
	int32 CountVisible() const { return GetVisible().Num(); }
	/**
	 * (lang) PO-06 F4: the merged, deduplicated items of one open buffer, hints included (the code view draws them faded),
	 * sorted by position: what the squiggles, the scroll-bar marks, the inspection widget and F2 read.
	 */
	TArray<FDreamProblem> GetForBuffer(const FGuid& Buffer) const;
	/** (lang) Moves on with every OnChanged broadcast: a painter re-reads GetForBuffer only when it changed. */
	uint64 GetRevision() const { return Revision; }
	/** The raw items of one source in one buffer (tests). */
	TArray<FDreamProblem> GetItems(const FGuid& Buffer, EDreamProblemSource Source) const;

	/**
	 * The F8 / Shift+F8 target in Visible (U13): the first item after (Line, Column) of Buffer, or the last before it, in list
	 * order; past the end it wraps. When CurrentIndex is the item the previous step reached and the caret is still on it,
	 * the walk continues in list order from there, so several items at one position are each visited. INDEX_NONE when
	 * Visible is empty.
	 */
	static int32 FindNeighbour(const TArray<FDreamProblem>& Visible, const FGuid& Buffer, int32 Line, int32 Column, bool bForward,
		int32 CurrentIndex = INDEX_NONE);

	static const TCHAR* SourceName(EDreamProblemSource Source);
	static EDreamSeverity SeverityFromLsp(int32 LspSeverity);

	DECLARE_EVENT(FDreamProblemsModel, FOnChanged);
	FOnChanged& OnChanged() { return ChangedEvent; }

private:
	struct FKey
	{
		FGuid Buffer;
		EDreamProblemSource Source;
		bool operator==(const FKey& Other) const { return Buffer == Other.Buffer && Source == Other.Source; }
		friend uint32 GetTypeHash(const FKey& Key) { return HashCombine(GetTypeHash(Key.Buffer), static_cast<uint32>(Key.Source)); }
	};

	/** (lang) Every item (runtime ones too) before the merge; OnlyBuffer limits it to one buffer. */
	TArray<FDreamProblem> Collect(const FGuid* OnlyBuffer, bool bHints) const;
	/** (lang) The merge and dedup of §7.8.2 over All (in place of GetVisible's old body). */
	static TArray<FDreamProblem> Dedup(TArray<FDreamProblem>&& All);
	/** (lang) True when Source's "syntax" items of Buffer are hidden by the other syntax verdict (NoteAnalysed). */
	bool IsSyntaxHidden(const FGuid& Buffer, EDreamProblemSource Source) const;
	void Broadcast();

	TMap<FKey, TArray<FDreamProblem>> Sets;
	TMap<FKey, uint64> AnalysedVersions;   // (lang)
	uint64 Revision = 0;                   // (lang)
	TMap<EDreamProblemSource, double> StaleSince;
	FDreamRuntimeProblems* Runtime = nullptr;
	FDelegateHandle RuntimeHandle;
	FOnChanged ChangedEvent;
};
