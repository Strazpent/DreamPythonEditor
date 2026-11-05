// SPDX-License-Identifier: Apache-2.0
// The Paused window's content (TDD §8.3.5, DBG-03, U17; debug group, PO-06 F5), styled to PO-04's Maya-minimal look:
// a one-line header with flat step buttons, the call stack (user frames only), the Variables tree (lazy, UObject-aware,
// no getters run: debugger.py) and a read-only source view with breakpoint dots, the execution arrow and the current-line
// band; a muted footer says the editor is paused. UI handlers never call Python: every request goes through
// FDreamDebugger::QueueRequest and is answered at the next modal-loop iteration (§8.3.3). Game thread.
// Not in this round: the evaluate console and watches (§8.3.6), run to cursor, condition editing.
#pragma once

#include "CoreMinimal.h"
#include "Debug/DreamDebugger.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

/** One row of the Variables tree. */
struct FDreamVariableNode
{
	FString Name;
	FString Value;
	FString Type;
	int64 Reference = 0;            // DAP variablesReference; 0 = leaf
	bool bRequested = false;        // children asked for
	bool bLoaded = false;           // children arrived
	bool bScope = false;            // Locals / Globals / Special
	TArray<TSharedPtr<FDreamVariableNode>> Children;
	TSharedPtr<FDreamVariableNode> Placeholder;      // the "…" child shown until the children arrive
};

/** One source line of the source view. */
struct FDreamSourceLine
{
	int32 Line = 0;                 // 1-based
	FString Text;
};

class DREAMEDITOR_API SDreamPausedWindow final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamPausedWindow) {}
		SLATE_ARGUMENT(FDreamDebugStop, Stop)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDreamPausedWindow() override;

	virtual bool SupportsKeyboardFocus() const override { return true; }
	/** F5 Continue, F10 / F11 / Shift+F11 steps, Shift+F5 and Esc Stop, F9 toggles the breakpoint on the selected source
	 *  line (§8.3.5; the chords are the Dream commands' active chords). */
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	// ---- Tests (the G4 driver reads widget state, §8.9.1) ----
	const TArray<TSharedPtr<FDreamVariableNode>>& GetRootsForTest() const { return Roots; }
	const TArray<TSharedPtr<FDreamDebugFrame>>& GetFramesForTest() const { return Frames; }
	int32 GetSourceLineCountForTest() const { return SourceLines.Num(); }
	int32 GetSelectedFrameForTest() const { return SelectedFrame; }
	/** Asks for a node's children (what expanding it does). */
	void ExpandForTest(const TSharedPtr<FDreamVariableNode>& Node);

private:
	TSharedRef<ITableRow> MakeFrameRow(TSharedPtr<FDreamDebugFrame> Frame, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeVariableRow(TSharedPtr<FDreamVariableNode> Node, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeSourceRow(TSharedPtr<FDreamSourceLine> Line, const TSharedRef<STableViewBase>& Owner);
	void GetVariableChildren(TSharedPtr<FDreamVariableNode> Node, TArray<TSharedPtr<FDreamVariableNode>>& OutChildren);
	void OnVariableExpansion(TSharedPtr<FDreamVariableNode> Node, bool bExpanded);
	void OnFrameSelected(TSharedPtr<FDreamDebugFrame> Frame, ESelectInfo::Type Info);
	void SelectFrame(int32 Index);
	void RequestChildren(const TSharedPtr<FDreamVariableNode>& Node);
	void LoadSource(const FString& Source, int32 Line);
	TSharedRef<SWidget> MakeButton(const FText& Label, const FText& Tip, EDreamResumeKind Kind, bool bStep);
	FText HeaderText() const;
	int32 CurrentLineOfShownSource() const;
	/** Review q1 m6: the shown source's breakpoint lines, read once per change, not per row paint. */
	void RefreshBreakpointCache();

	FDreamDebugStop Stop;
	TArray<TSharedPtr<FDreamDebugFrame>> Frames;
	TArray<TSharedPtr<FDreamVariableNode>> Roots;
	TArray<TSharedPtr<FDreamSourceLine>> SourceLines;
	TSharedPtr<SListView<TSharedPtr<FDreamDebugFrame>>> FrameList;
	TSharedPtr<STreeView<TSharedPtr<FDreamVariableNode>>> VariableTree;
	TSharedPtr<SListView<TSharedPtr<FDreamSourceLine>>> SourceList;
	FString ShownSource;
	int32 SelectedFrame = 0;
	int32 SelectedSourceLine = 0;
	FText Status;
	TSharedRef<TSet<int32>> ShownBreakpoints = MakeShared<TSet<int32>>();
	FDelegateHandle BreakpointsChangedHandle;
};
