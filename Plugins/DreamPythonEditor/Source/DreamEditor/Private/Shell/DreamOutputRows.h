// SPDX-License-Identifier: Apache-2.0
// The output view's rows (TDD §6.4 rendering by type, header and footer, result lines, links, folding, eviction marker):
// the row model SDreamOutputView builds over FDreamOutputStore, the row widgets and the header and footer text rendered
// from FDreamRunRecord. Output slice, internal. Game thread.
// (output, PO-04 point 2) The Maya-style history: echo muted with no gutter, "# Result: <repr> #", "# Warning: <text> #",
// a traceback with every line prefixed "# " and its exception line as "# Error: <Type>: <message> #", no per-line icons,
// and run headers and footers (one muted line each) only with UDreamUserSettings::bShowRunHeaders. The decorations are
// presentation only: the store keeps the raw text, and the forwarder writes LogPython as before (§6.3).
#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Run/DreamRunTypes.h"
#include "Styling/SlateTypes.h"
#include "dream/OutputRing.h"

#include <string_view>

class FDreamOutputStore;
class FDreamRunService;
class SWidget;

enum class EDreamOutputRowKind : uint8
{
	Line,               // a stored line (stdout ... echo); Seq is its sequence
	Header,             // a RunHeader line, rendered from the record
	Footer,             // a RunFooter line, rendered from the record
	SyntheticHeader,    // the header of a run whose header line was evicted (§6.2 eviction marker)
	FoldChip,           // "… 34 more lines" of a folded statement echo (OUT-12, Normal)
	ResultMore,         // a continuation row of an expanded result (§6.4 Show all)
	Eviction,           // "n earlier lines dropped (runs #a–#b)" at the top
	OutsideRuns,        // the "Outside runs" block header (§6.3, foreign LogPython while idle)
	StartupHeader,      // the collapsed "Editor startup" block (§6.3 startup ring, T-OUT-037)
	StartupLine,        // one Warning or Error line of the startup ring
	Provisional,        // the open partial line of the active run (§6.1)
	/** (output, PO-04 ruling on review r6 M1) With run headers off, the one line of a run that ended other than ok with no
	 *  error line of its own: "# Error: Run #N <outcome text> #", "# Warning: … #" for a stopped run. Seq is its footer's. */
	RunOutcome,
};

/** (output, PO-04) The Maya-style decoration drawn before a line row's text. */
enum class EDreamRowDeco : uint8
{
	None,
	Result,             // "# Result: " (the first line of a repr, A63)
	Warning,            // "# Warning: "
	Error,              // "# Error: " (a traceback's exception line, or the first line of an error block without one)
	Comment,            // "# " (every other line of an error block)
};

struct FDreamOutputRow
{
	EDreamOutputRowKind Kind = EDreamOutputRowKind::Line;
	uint64 Seq = 0;                     // the line, or the sequence the row sorts at
	uint32 RunNumber = 0;
	int32 Statement = 0;                // FoldChip: the statement's index in its run
	int32 Count = 0;                    // FoldChip: hidden echo lines
	TArray<uint64> JoinedSeqs;          // EchoSplit parts drawn on this row (§6.1: no echo shown between them)
	FString Text;                       // StartupLine, ResultMore and Provisional text
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	/** (output, PO-04) Decided by the view as it builds rows (DreamOutputRows::Decorate); it moves while a result, warning
	 *  or error block still grows, and the view makes a changed row's widget again. */
	EDreamRowDeco Deco = EDreamRowDeco::None;
	bool bDecoEnd = false;              // " #" after the text: the last row of its result, warning or error block
};

using FDreamOutputRowPtr = TSharedPtr<FDreamOutputRow>;

/** (output, PO-04) The open decoration block of one run while the view builds its rows: a repr (A63), a warning with its
 *  indented lines, or an error block (a traceback, a SyntaxError block, an unreal.log_error line). */
struct FDreamDecoBlock
{
	dream::LineType Type = dream::LineType::Stdout;   // Result, Warning or Error while a block is open
	uint16 Origin = 0;                  // the OLF_FromUnrealLog | OLF_FromLogPython bits of an error block's lines
	FDreamOutputRowPtr Last;             // the row that carries " #"
	FDreamOutputRowPtr ErrorRow;         // the row that carries "# Error: "
	bool bExceptionFound = false;       // ErrorRow is an exception line, not the first-line fallback
	bool bTraceback = false;            // the block has a "Traceback (most recent call last):" line
	/** A frame section is open: a "Traceback (most recent call last):" or "File \"…\", line N" line came and no exception
	 *  line was taken since. Only then does a "Type: message" line become the exception line (review r6 m1: the field
	 *  lines of a pydantic ValidationError after it are not). */
	bool bFrames = false;
	bool bLastIndented = false;         // the block's last line starts with white space (a frame, source or caret line)
	bool bAwaitTraceback = false;       // the last line was blank or a chain separator: a chained traceback follows
};

/** What a row widget asks of its view (SDreamOutputView's state implements it). */
class IDreamOutputRowHost
{
public:
	virtual ~IDreamOutputRowHost() = default;

	virtual const FDreamOutputStore& GetStore() const = 0;
	virtual FDreamRunService* GetRunService() const = 0;
	virtual bool IsLiveView() const = 0;
	virtual const FSlateFontInfo& GetRowFont() const = 0;
	virtual const FTextBlockStyle& GetLinkTextStyle() const = 0;

	virtual bool IsRunCollapsed(uint32 RunNumber) const = 0;
	virtual void ToggleRunCollapsed(uint32 RunNumber) = 0;
	/** True when none of a prompt run's echo lines is shown here: its header then shows the submitted code (§6.4). */
	virtual bool IsRunEchoHidden(uint32 RunNumber) const = 0;
	virtual void ExpandFold(uint32 RunNumber, int32 Statement) = 0;
	virtual bool IsResultExpanded(uint64 Seq) const = 0;
	virtual void ToggleResultExpanded(uint64 Seq) = 0;
	virtual bool IsStartupExpanded() const = 0;
	virtual void ToggleStartupExpanded() = 0;
	virtual void OpenLink(uint64 Seq, int32 LinkIndex) = 0;
	virtual void JumpToRun(uint32 RunNumber) = 0;
	// (output, PO-04) Re-run, Run in transaction, Undo this run and Run without snapshot moved from the footer's chips to
	// the view's context menu (SDreamOutputView), so the rows no longer ask for them.
	/** Find (§6.4 widget): the matches on a stored line as (start, length) in UTF-16 units of its text, in order, and the
	 *  index in OutRanges of the current match (INDEX_NONE when it is on another line). Empty while the find bar is closed. */
	virtual void GetFindMatches(uint64 Seq, TArray<TPair<int32, int32>>& OutRanges, int32& OutCurrent) const = 0;
	/** The soft-wrap toggle (§6.4 widget): a line wider than the view wraps instead of running past its right edge. */
	virtual bool IsSoftWrap() const = 0;
};

namespace DreamOutputRows
{
	/** The widget of one row. Host is held weakly by the row's callbacks. */
	TSharedRef<SWidget> MakeRowWidget(const FDreamOutputRowPtr& Row, const TSharedRef<IDreamOutputRowHost>& Host);

	/** A row's raw text without decorations (§6.4 copy): no line numbers, badges or "# Result: ". */
	FString GetRowRawText(const FDreamOutputRow& Row, const IDreamOutputRowHost& Host);
	/** A row's text as its widget draws it, decorations included ("# Result: 4 #", "# Error: ...", the header line). */
	FString GetRowDisplayText(const FDreamOutputRow& Row, const IDreamOutputRowHost& Host);

	// ---- (output, PO-04) Maya-style decorations ----

	/** The decoration drawn before a line row's own text ("# Result: " ...), else empty. Echo rows draw no prompt (A226). */
	FString DecorationPrefix(const FDreamOutputRow& Row);
	/** " #" for the last row of a result, warning or error block, else empty. */
	FString DecorationSuffix(const FDreamOutputRow& Row);
	/**
	 * Decides the decoration of Row, the new row of a stored line of Block's run (Info, Text), and grows or closes Block. A
	 * row made earlier whose decoration changed (the " #" moved on, a later exception line took "# Error: ") is added to
	 * OutChanged. Results: one write is one repr; its OLF_ResultCont lines continue it (A63). Warnings: an indented line
	 * continues the warning above it (warnings.formatwarning's source line). Errors: the consecutive error lines of a run
	 * form one block, except that a log line (unreal.log_error, captured LogPython) starts a new block unless it is indented
	 * or a traceback is still being written; the block's exception line is the last unindented "Type: message" line right
	 * after an indented line (a frame, source or caret line), else its first line.
	 */
	void Decorate(FDreamDecoBlock& Block, const FDreamOutputRowPtr& Row, const dream::LineInfo& Info, std::string_view Text,
		TArray<FDreamOutputRowPtr>& OutChanged);
	/** A row drawn after Block's last one that belongs to it (a result's Show all rows): it takes the " #". */
	void ExtendDecoration(FDreamDecoBlock& Block, const FDreamOutputRowPtr& Row, TArray<FDreamOutputRowPtr>& OutChanged);
	/** Ends Block (another kind of line of the run came). */
	void CloseDecoration(FDreamDecoBlock& Block);

	/** The record of a run whose context menu offers Undo this run: a Recorded or Partial transaction (U9, RT-08). */
	bool CanOfferUndo(const FDreamRunRecord& Record);

	/** (review r6 M1) A run that needs its one outcome line with run headers off: it ended, other than ok. The tests module
	 *  calls these three (DREAMEDITOR_API). */
	DREAMEDITOR_API bool NeedsOutcomeLine(const FDreamRunRecord& Record);
	/** A stopped run (cancelled, or not executed because Stop came first): its outcome line is a warning. */
	DREAMEDITOR_API bool IsStoppedRun(const FDreamRunRecord& Record);
	/** "Run #12 error exited (code 3)": the outcome line's text without its decoration, the footer's outcome text. */
	DREAMEDITOR_API FString RunOutcomeText(const FDreamRunRecord* Record, uint32 RunNumber);

	/** "▼ #22 · Untitled-3 · lines 12–20 · Session · 14:03:11" (left) and "ok · 0.42 s" / "Running 4.1 s" (right). */
	FText HeaderLeftText(const FDreamRunRecord* Record, uint32 RunNumber, bool bCollapsed, bool bShowCode);
	FText HeaderRightText(const FDreamRunRecord* Record);
	/** The transaction notes line under the header (§6.4, §5.9.2); empty when none. */
	FText TransactionNotesText(const FDreamRunRecord* Record);
	/** "└ #22 ok · 0.42 s · undo: "Dream: Untitled-3 #22" (3 objects, 12 KB) · notes": the footer line (PO-04: one muted line). */
	DREAMEDITOR_API FText FooterText(const FDreamRunRecord* Record, uint32 RunNumber, const FDreamOutputStore& Store);
	/** "error ZeroDivisionError: division by zero", "cancelled at fix.py:18", "not executed SyntaxError", "lost" (U2 + §6.4
	 *  footer). The error detail is the runner's "<Type>: <first line of str(exc)>" as given (ruling R1). */
	FString OutcomeText(const FDreamRunRecord& Record);
	FString DurationText(double Seconds);
	/** A not executed run that ended because its pre-run journal snapshot failed or timed out (§10.2.5 step 4). */
	bool IsSnapshotFailure(const FDreamRunRecord& Record);
}
