// SPDX-License-Identifier: Apache-2.0
// The Recovery review panel (TDD §10.7.3, U26, PER-03) and the per-buffer pending bar (§10.7.1, §10.7.2). Review lists this
// session's recovered buffers (name, path or "untitled", +/- lines against the text shown, the conflict flag, the last
// edit time) with Restore, Restore as new tab, Compare and Discard per row; Compare opens the M1 read-only unified diff
// built with dream::Diff (§10.7.2) under the row. Both are non-modal and never take focus. (slice B choice) The right side
// bar does not exist yet, so SDreamTab docks the panel under its banners, above the output and code areas. Orphaned,
// imported, compat and other-root namespaces follow as their own sections (§10.5, §10.6, §10.1), never part of Restore
// all; the live primary's tabs (a secondary's View its tabs) are a read-only section whose rows offer [Open a copy] only.
// Game thread. Slice-internal (persist, then instances).
#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

/** One row of the review panel. */
struct FDreamRecoveryRow
{
	FGuid Id;
	FText Name;
	FText Where;                        // the path, or "untitled"
	int32 LinesAdded = 0;
	int32 LinesRemoved = 0;
	bool bConflict = false;             // the disk changed after the journal's base version (§10.7.2)
	FDateTime LastEditUtc;
};

/** One button of a bar or a row. */
struct FDreamRecoveryAction
{
	FText Label;
	FText Tooltip;
	FSimpleDelegate OnClick;
	bool bEnabled = true;
};

DECLARE_DELEGATE_OneParam(FDreamOnRecoveryRow, const FGuid& /*Buffer*/);

/** A row's action in a section of another namespace (§10.5, §10.1). */
enum class EDreamRecoveryRowAction : uint8
{
	Restore,
	RestoreAsNewTab,
	Compare,
	Discard,
	OpenCopy,                           // read-only sections: a new scratch buffer with the text (§10.5 View its tabs)
};

/** One section of buffers from another namespace: an orphaned session, an imported or compat journal, another root. */
struct FDreamRecoverySection
{
	FString Key;                        // FDreamOrphanNamespace::Key
	FText Title;
	TArray<FDreamRecoveryRow> Rows;
	bool bReadOnly = false;
};

DECLARE_DELEGATE_ThreeParams(FDreamOnRecoverySectionRow, const FString& /*Section*/, const FGuid& /*Buffer*/, EDreamRecoveryRowAction);

namespace DreamRecoveryUi
{
	/** The M1 Compare (§10.7.2): a unified diff of Old against New with dream::Diff, labelled; empty when they are equal. */
	DREAMEDITOR_API FString UnifiedDiff(const FString& Old, const FString& New, const FString& OldLabel, const FString& NewLabel);
	/** "+12 -3 lines" (§10.7.1) between two texts, from the same line diff. */
	DREAMEDITOR_API void CountLines(const FString& Old, const FString& New, int32& OutAdded, int32& OutRemoved);
	/** A read-only, monospace view of a diff text, at most MaxHeight tall. */
	DREAMEDITOR_API TSharedRef<SWidget> MakeDiffView(const FString& DiffText, float MaxHeight = 260.f);
}

/** The per-buffer bar above the code view: a message, buttons, and an optional diff below them. */
class DREAMEDITOR_API SDreamRecoveryActionBar final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamRecoveryActionBar) {}
		SLATE_ARGUMENT(FText, Message)
		SLATE_ARGUMENT(TArray<FDreamRecoveryAction>, Actions)
		/** Set: the Compare diff, shown read-only under the bar. */
		SLATE_ARGUMENT(TOptional<FString>, DiffText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class DREAMEDITOR_API SDreamRecoveryReview final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamRecoveryReview) {}
		SLATE_ARGUMENT(FText, Title)
		SLATE_ARGUMENT(TArray<FDreamRecoveryRow>, Rows)
		/** The row whose Compare is open, and its diff text. */
		SLATE_ARGUMENT(FGuid, CompareFor)
		SLATE_ARGUMENT(FString, CompareText)
		SLATE_EVENT(FDreamOnRecoveryRow, OnRestore)
		SLATE_EVENT(FDreamOnRecoveryRow, OnRestoreAsNewTab)
		SLATE_EVENT(FDreamOnRecoveryRow, OnCompare)
		SLATE_EVENT(FDreamOnRecoveryRow, OnDiscard)
		SLATE_EVENT(FSimpleDelegate, OnClose)
		/** (instances) The sections after this session's rows, unticked (§10.7.3), and the section of the open Compare. */
		SLATE_ARGUMENT(TArray<FDreamRecoverySection>, Sections)
		SLATE_ARGUMENT(FString, CompareSection)
		SLATE_EVENT(FDreamOnRecoverySectionRow, OnSectionAction)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Test hook (Dream.Persistence.Recovery): the rows as built. */
	const TArray<FDreamRecoveryRow>& GetRowsForTest() const { return Rows; }
	/** Test hook (Dream.Persistence.Instances): the other namespaces' sections as built. */
	const TArray<FDreamRecoverySection>& GetSectionsForTest() const { return Sections; }

private:
	TArray<FDreamRecoveryRow> Rows;
	TArray<FDreamRecoverySection> Sections;
};
