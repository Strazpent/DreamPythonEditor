// SPDX-License-Identifier: Apache-2.0
// Text undo of a buffer on dream::UndoTree (TDD §4.2, §4.15 row (3) "Ctrl+Z drives UndoTree", EDT-08, D8) and the
// grouped-edit API (§4.2 R5): several ranges replaced as ONE undo step with a label.
//
// One history per FDreamBuffer, created on first use (FDreamUndoHistory::For; the code view creates it with itself). It
// records EVERY change of the buffer's model, whoever makes it: the stock widget's typing reflected by SDreamCodeView, the
// edit builders, edit groups, a reload, another group's ApplyEdit or ResetText (origin Load starts a new root). Its code
// view hears of each change after it was recorded (OnModelChanged), so the selection before an edit is still the
// widget's when the history reads it. Undo and Redo apply the tree's inverse batch to the model and give the view the
// selection back. The undo memory is bounded per buffer (UndoLimits) and per process (256 MB, EnforceUndoProcessCap);
// saves only mark the save point (EDT-08: the history survives save); nothing is kept across editor restarts (1.0).
// Game thread. Owner: codeview.
#pragma once

#include "CoreMinimal.h"
#include "CodeView/DreamEditGroup.h"
#include "dream/UndoTree.h"

class FDreamBuffer;
class SDreamCodeView;
struct FDreamRunRecord;

class DREAMEDITOR_API FDreamUndoHistory : public TSharedFromThis<FDreamUndoHistory>
{
public:
	/** The history of Buffer, created on first use; it records from then on (the text then is its root). */
	static TSharedRef<FDreamUndoHistory> For(const TSharedRef<FDreamBuffer>& Buffer);
	/** The history of Buffer when one exists. */
	static TSharedPtr<FDreamUndoHistory> Find(const FDreamBuffer& Buffer);

	// ---- The grouped-edit API (buffers: reload; persist: Restore, restore from history; lang: quick fix; completion) ----

	/**
	 * Applies Group.Edits to Buffer as ONE undo step named Group.Label (§4.2 R5). The edits are in the coordinates of the
	 * text before the call and must not overlap. With a code view, the selection becomes Group.SelectionAfter (else a
	 * caret after the last edit), and one Ctrl+Z gives back the text and the selection from before. Works without a view
	 * too (a background tab). False, and nothing changes, when a range is out of bounds or ranges overlap, when the buffer
	 * is read-only and Group.bEvenIfReadOnly is false, or while a run is active (RT-01).
	 */
	static bool ApplyEditGroup(FDreamBuffer& Buffer, const FDreamEditGroup& Group);
	/**
	 * Replaces the whole text with NewText as ONE undo step, through the minimal edits between the two texts
	 * (dream::Diff::ToMinimalEdits), so unchanged lines, the caret and the scroll stay (BUF-04, U30). Origin Reload gives an
	 * UndoKind::Reload step. True without a step when the text is already NewText; false as for ApplyEditGroup.
	 */
	static bool ReplaceAllText(FDreamBuffer& Buffer, const FString& NewText, const FText& Label,
		dream::EditOrigin Origin = dream::EditOrigin::Command, bool bEvenIfReadOnly = false);
	/** The offset form of ApplyEditGroup (Replace All, a quick fix built in offsets). SelectionAfter in post-edit text. */
	static bool ApplyEditBatch(FDreamBuffer& Buffer, const dream::EditBatch& Batch, const FText& Label, dream::UndoKind Kind,
		const TOptional<FDreamTextSelection>& SelectionAfter, bool bEvenIfReadOnly);

	// ---- The code view ----

	void AttachView(const TSharedRef<SDreamCodeView>& View);
	void DetachView(const SDreamCodeView* View);

	/**
	 * The kind and the carets of the change made next in this frame: Before in the text before it (unset: the view's
	 * selection then), After in the text after it (unset: the view's selection mapped through the change).
	 */
	void SetNextEdit(dream::UndoKind Kind, const TOptional<dream::CaretRange>& Before, const TOptional<dream::CaretRange>& After);
	/** Drops SetNextEdit's hints (the change they were for did not happen). */
	void ClearNextEdit() { bHasNext = false; }
	/** A position mapped through a change: before a range it stays, after it shifts, inside a replaced range it goes to the
	 *  end of the new text; at a range's start it stays before the range unless bStickRight. */
	static dream::Offset MapOffset(const dream::ChangeSet& Changes, dream::Offset At, bool bStickRight);
	/** R4: the caret moved by navigation, a click or find; focus left. Ends a typing or delete burst. */
	void CloseTypingGroup();
	/** R6: an IME composition is one group (plain mode sees its edits as they come). */
	void BeginImeGroup();
	void EndImeGroup();
	bool IsImeGroupOpen() const { return bImeGroupOpen; }

	bool CanUndo() const;
	bool CanRedo() const;
	/** Ctrl+Z / Ctrl+Y: applies the step to the model (the view follows) and restores the selection. False: nothing done. */
	bool Undo();
	bool Redo();

	const dream::UndoTree& GetTree() const { return Tree; }

	/** After every recorded change of the model; Before is the text before it. The code view syncs its widget here. */
	DECLARE_EVENT_TwoParams(FDreamUndoHistory, FOnModelChanged, const dream::ChangeSet& /*Changes*/, const dream::TextSnapshot& /*Before*/);
	FOnModelChanged& OnModelChanged() { return ModelChangedEvent; }

	/** EDT-08's process cap (proposed): 256 MB of undo text over every buffer. */
	static constexpr size_t ProcessCapBytes = size_t(256) << 20;

	explicit FDreamUndoHistory(const TSharedRef<FDreamBuffer>& InBuffer);
	~FDreamUndoHistory();

private:
	enum class EAfter : uint8
	{
		EndOfLastEdit,      // ApplyEditGroup's default
		Mapped,             // ReplaceAllText: the old selection mapped through the change (carets stay put)
	};

	static bool ApplyBatchInternal(FDreamBuffer& Buffer, const dream::EditBatch& Batch, const FText& Label, dream::UndoKind Kind,
		const TOptional<FDreamTextSelection>& SelectionAfter, EAfter Default, bool bEvenIfReadOnly);
	static void EnforceProcessCap();
	/** Drops the histories whose buffer is gone. */
	static void PurgeStale();
	void Bind();
	void HandleBufferChanged(const dream::ChangeSet& Changes);
	void HandleBufferStateChanged();
	void HandleRunStateChanged(const FDreamRunRecord& Record);
	/** "Older undo history trimmed" / "Undo history reset" in the view's status strip (§4.2). */
	void ShowNotices();
	/** The view's selection in Snapshot's offsets; unset without a view. */
	TOptional<dream::CaretRange> ViewCaret(const dream::TextSnapshot& Snapshot) const;
	bool ApplyStep(dream::UndoStep&& Step, bool bUndo);

	TWeakPtr<FDreamBuffer> Buffer;
	dream::UndoTree Tree;
	dream::TextSnapshot Last;                 // the text before the next change
	TWeakPtr<SDreamCodeView> View;
	FOnModelChanged ModelChangedEvent;
	FDelegateHandle ChangedHandle;
	FDelegateHandle StateChangedHandle;
	FDelegateHandle RunStateHandle;

	// The next change's hints (SetNextEdit), valid in the frame they were set.
	dream::UndoKind NextKind = dream::UndoKind::Typing;
	TOptional<dream::CaretRange> NextBefore;
	TOptional<dream::CaretRange> NextAfter;
	uint64 NextFrame = 0;
	bool bHasNext = false;

	bool bImeGroupOpen = false;
	bool bWasDirty = false;
	bool bApplyingStep = false;
	uint32 LastBoundaryRun = 0;
};
