// SPDX-License-Identifier: Apache-2.0
// The output view (TDD §6.4, OUT-02, OUT-05, OUT-07, OUT-09, OUT-12, A11Y-05). See SDreamOutputView.h.
// (slice A choice, A33) An SListView of rows built incrementally from FDreamOutputStore as each drain appends, with a
// dream::VisibleIndex of the shown sequences; filters (type toggles, the REPL route filter, the live run filter),
// collapsed blocks and Normal echo folding decide the rows. Only visible rows get widgets (D1).
// (output, PO-06 F3) FImpl also holds the text selection (anchor and caret as a row and an offset in its drawn text); the
// row frames, rows and surface of Shell/DreamOutputSelection.h ask it what to paint and hand it the mouse.
#include "Shell/SDreamOutputView.h"

#include "Algo/BinarySearch.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamFindBar.h"
#include "Commands/DreamKeyRouter.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Persistence/DreamInstances.h"     // (instances, B23) Open storage settings, now a context menu entry (PO-04)
#include "Rendering/SlateRenderer.h"
#include "Run/DreamRunService.h"
#include "Run/DreamTransactions.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamOutputRows.h"
#include "Shell/DreamOutputSelection.h"     // (output, PO-06 F3) the selectable history
#include "Shell/DreamTheme.h"
#include "Styling/AppStyle.h"
#include "Tasks/Task.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "dream/CancelToken.h"
#include "dream/OutputLines.h"
#include "dream/SearchEngine.h"
#include "dream/VisibleIndex.h"

#include <memory>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamOutputViewPrivate
{
	using FRowList = SListView<FDreamOutputRowPtr>;

	/** (output choice) The soft-wrap toggle is one session-wide value, off at start and not saved (§6.4 "soft wrap is off by
	 *  default"): every output view, the Run window's live run view included, follows it at its next tick. */
	bool& SoftWrapFlag()
	{
		static bool bSoftWrap = false;
		return bSoftWrap;
	}

	// ---- Find (§6.4 widget): dream::SearchEngine over a ring snapshot, on a UE::Tasks worker ----

	/** One match on a stored line: (Start, Len) in UTF-16 units of the line's text; the line's run and type for filters. */
	struct FFindHit
	{
		uint64 Seq = 0;
		int32 Start = 0;
		int32 Len = 0;
		uint32 RunNumber = 0;
		dream::LineType Type = dream::LineType::Stdout;
		uint16 Flags = 0;
	};

	/** The result of one worker pass over [From, EndSeq) of a snapshot. */
	struct FFindPass
	{
		TArray<FFindHit> Hits;
		uint64 EndSeq = 0;          // the pass covered every line before this (unless cancelled)
		bool bCapped = false;
		int32 ComplexLines = 0;
	};

	/** §4.4's cap, shared with the code view's find ("the cap is 100,000 matches (proposed)"). */
	constexpr int32 FindMaxMatches = 100000;
	/** New lines are searched in the next worker pass, at most every 100 ms (§6.4 "Design: filters", proposed). */
	constexpr double FindPassIntervalSeconds = 0.1;

	/** Worker thread: reads only the snapshot (lock-free, §6.2) and the immutable compiled query. */
	FFindPass SearchSnapshot(const dream::OutputSnapshot& Snapshot, uint64 From, const std::shared_ptr<const dream::CompiledQuery>& Query,
		int32 Budget, const std::shared_ptr<dream::CancelToken>& Cancel)
	{
		FFindPass Pass;
		Pass.EndSeq = Snapshot.EndSeq();
		dream::LineView View;
		for (uint64 Seq = FMath::Max(From, Snapshot.FirstSeq()); Seq < Pass.EndSeq; ++Seq)
		{
			if (Cancel->IsCancelled())
			{
				break;
			}
			if (!Snapshot.GetLine(Seq, View) || View.Text.empty()
				|| View.Info.Type == dream::LineType::RunHeader || View.Info.Type == dream::LineType::RunFooter)
			{
				continue;
			}
			const FString Text = DreamText::Utf8ToFString(View.Text);     // the unit the rows paint in (UTF-16)
			dream::SearchSummary Summary;
			dream::SearchLimits Limits;
			Limits.MaxMatches = static_cast<size_t>(FMath::Max(1, Budget - Pass.Hits.Num()));
			const std::vector<dream::SearchMatch> Matches = dream::SearchEngine::FindAll(DreamText::ToU16(Text), *Query, 0, -1, &Summary,
				Cancel.get(), Limits);
			Pass.ComplexLines += Summary.ComplexLines;
			for (const dream::SearchMatch& Match : Matches)
			{
				if (Match.Len <= 0)
				{
					continue;           // an empty match has nothing to show or select
				}
				FFindHit& Hit = Pass.Hits.AddDefaulted_GetRef();
				Hit.Seq = Seq;
				Hit.Start = static_cast<int32>(Match.At);
				Hit.Len = static_cast<int32>(Match.Len);
				Hit.RunNumber = View.Info.RunNumber;
				Hit.Type = View.Info.Type;
				Hit.Flags = View.Info.Flags;
			}
			if (Pass.Hits.Num() >= Budget || Summary.bCapped)
			{
				Pass.bCapped = true;
				Pass.EndSeq = Seq + 1;
				break;
			}
		}
		return Pass;
	}

	FText FindErrorText(const dream::SearchError& Error)
	{
		const FText Detail = FText::FromString(DreamText::ToFString(Error.Detail));
		switch (Error.Code)
		{
		case dream::SearchErrorCode::Syntax:
			return FText::Format(LOCTEXT("Out.FindSyntax", "Invalid pattern: {0}"), Detail);
		case dream::SearchErrorCode::Unsupported:
			return FText::Format(LOCTEXT("Out.FindUnsupported", "Not supported in Dream's find: {0}"), Detail);
		case dream::SearchErrorCode::TooComplex:
			return LOCTEXT("Out.FindTooComplex", "Pattern too complex");
		default:
			return FText::GetEmpty();
		}
	}

	uint64 FoldKey(uint32 RunNumber, int32 Statement)
	{
		return (static_cast<uint64>(RunNumber) << 32) | static_cast<uint32>(Statement);
	}

	bool IsPromptRoute(EDreamRunRoute Route)
	{
		return Route == EDreamRunRoute::Repl || Route == EDreamRunRoute::ConsoleExecutor;
	}

	void ShowStatus(const FText& Message)
	{
		if (FDreamEditorModule::IsAvailable())
		{
			if (const TSharedPtr<SDreamTab> Tab = FDreamEditorModule::Get().GetTab())
			{
				Tab->ShowStatusMessage(Message);
			}
		}
	}

	/** (output, PO-04) The rows' table style: the stock row with transparent row backgrounds (the history pane's
	 *  OutputBackground shows through; Starship's TableView.Row paints every row FStyleColors::Recessed, near-black), the
	 *  theme's current-line colour under the hovered row and its selection colour under selected rows. STableRow keeps a
	 *  pointer to it and reads its brushes at paint, so RefreshRowStyle on a theme switch recolours every view's rows. */
	FTableRowStyle& RowStyle()
	{
		static FTableRowStyle Style = FAppStyle::Get().GetWidgetStyle<FTableRowStyle>(TEXT("TableView.Row"));
		return Style;
	}

	void RefreshRowStyle()
	{
		const FSlateBrush& Selected = *FDreamThemes::Brush(EDreamThemeSlot::Selection);
		const FSlateBrush& Hovered = *FDreamThemes::Brush(EDreamThemeSlot::CurrentLine);
		RowStyle()
			// The keyboard selector's outline in the theme's Accent (review r6 m7), not the stock selector colour.
			.SetSelectorFocusedBrush(FSlateRoundedBoxBrush(FLinearColor::Transparent, 0.f, FDreamThemes::LinearColor(EDreamThemeSlot::Accent), 1.f))
			.SetEvenRowBackgroundBrush(FSlateNoResource())
			.SetOddRowBackgroundBrush(FSlateNoResource())
			.SetEvenRowBackgroundHoveredBrush(Hovered)
			.SetOddRowBackgroundHoveredBrush(Hovered)
			.SetActiveHighlightedBrush(FSlateNoResource())
			.SetInactiveHighlightedBrush(FSlateNoResource())
			.SetActiveBrush(Selected)
			.SetActiveHoveredBrush(Selected)
			.SetInactiveBrush(Selected)
			.SetInactiveHoveredBrush(Selected);
	}
}

struct SDreamOutputView::FImpl : public TSharedFromThis<FImpl>, public IDreamOutputRowHost, public IDreamOutputSelectionHost
{
	/** Per-run build state while rows are made (fold counters, the EchoSplit join, visible echo for the header). */
	struct FRunBuild
	{
		bool bHeaderShown = false;
		int32 Statement = -1;
		int32 StatementEcho = 0;
		FDreamOutputRowPtr Chip;
		int32 VisibleEcho = 0;
		FDreamOutputRowPtr PendingSplit;
		dream::LineType PendingSplitType = dream::LineType::Stdout;
		bool bEchoSinceSplit = false;
		FDreamDecoBlock Deco;                // (output, PO-04) the run's open result, warning or error block
		/** (review r6 M1, ruling on r7 m1) A row of the run's own exception block is shown: an error line of its Python or of
		 *  Dream's compile-error block, not an unreal.log_error or captured log line, and not one a filter hides. */
		bool bExceptionShown = false;
	};

	FDreamOutputStore* Store = nullptr;
	FDreamRunService* RunService = nullptr;
	TOptional<EDreamRunRoute> RouteFilter;
	TOptional<int32> RunFilter;
	bool bLive = false;
	TWeakPtr<IDreamCommandTarget> CommandTarget;
	FDreamOnOpenPromptLink OnOpenPromptLink;
	TWeakPtr<SDreamOutputView> Owner;

	TArray<FDreamOutputRowPtr> Rows;
	TSharedPtr<DreamOutputViewPrivate::FRowList> List;
	dream::VisibleIndex Visible;
	uint64 NextSeq = 0;
	int32 TopFixedRows = 0;
	FDreamOutputRowPtr ProvisionalRow;

	FDreamOutputVisibility Vis;
	TSet<uint32> Collapsed;
	TSet<uint64> ExpandedFolds;
	TSet<uint32> ExpandedRuns;
	TSet<uint64> ExpandedResults;
	bool bStartupExpanded = false;
	int32 StartupRowsBuilt = -1;
	TMap<uint32, FRunBuild> Builds;
	TMap<uint32, EDreamRunRoute> RouteCache;
	uint32 LastRowRun = MAX_uint32;
	uint32 OpenRun = 0;
	bool bEvictionRowShown = false;

	bool bFollowing = true;
	int32 NewLinesBelow = 0;
	uint64 TickCount = 0;
	bool bProgrammaticScroll = false;
	float LastOffset = 0.f;
	/** The offset the list last applied (its CurrentScrollOffset, which its navigation scroll reads): GetScrollOffset is
	 *  the desired offset, the row count after ScrollBottom until the list's next tick clamps it (P4-shl-01). */
	float ShownOffset = 0.f;
	uint64 BottomCheckAfterTick = 0;

	uint64 WalkSeq = 0;
	int32 WalkLink = -1;
	TSet<uint32> UndoConfirm;

	FSlateFontInfo Font;
	FTextBlockStyle LinkStyle;
	int32 FoldLines = 6;

	FDelegateHandle AppendedHandle;
	FDelegateHandle VisibilityHandle;
	FDelegateHandle RunFinishedHandle;
	FDelegateHandle ViewSettingsHandle;
	FDelegateHandle ThemeHandle;

	/** The soft-wrap value the row widgets were made with (SoftWrapFlag changes regenerate them at the next tick). */
	bool bWrapApplied = false;
	/** (output, PO-04) UDreamUserSettings::bShowRunHeaders as the rows were built: run headers and footers, synthetic
	 *  headers and the "Outside runs" line are rows only while it is on (a collapsed run shows its header either way). */
	bool bShowHeaders = false;
	/** Rows whose decoration changed after they were made (a block that grew across drains); the next tick makes their
	 *  widgets again when the list shows them. */
	TArray<FDreamOutputRowPtr> DecoChanged;

	// ---- Find (§6.4 widget) ----
	TSharedPtr<SDreamFindBar> FindBar;
	TSharedPtr<SBox> FindHost;                   // holds FindBar while the bar is open
	bool bFocusFindBoxPending = false;           // the bar was just put in the tree: focus its box at the next tick
	bool bFindOpen = false;
	bool bApplyingQuery = false;                 // SetFindQuery sets the bar's fields: one search for all of them
	std::shared_ptr<const dream::CompiledQuery> FindQuery;
	FText FindError;
	TArray<DreamOutputViewPrivate::FFindHit> FindRaw;    // every match in the ring, in order (no view filter)
	TArray<DreamOutputViewPrivate::FFindHit> FindHits;   // the matches this view shows, in order
	int32 FindCurrent = INDEX_NONE;              // into FindHits
	uint64 FindSearchedEnd = 0;                  // FindRaw covers the lines before this sequence
	bool bFindCapped = false;
	int32 FindComplexLines = 0;
	uint32 FindGeneration = 0;                   // a new query or a Clear makes the pass in flight stale
	uint32 FindTaskGeneration = 0;
	TOptional<UE::Tasks::TTask<DreamOutputViewPrivate::FFindPass>> FindTask;
	std::shared_ptr<dream::CancelToken> FindCancel;
	double FindNextPassSeconds = 0.0;
	bool bRowWidgetsStale = false;               // the highlights changed: regenerate the shown row widgets
	int32 FindPassesForTest = 0;                 // worker passes started (Dream.Output.Find)
	int32 RowRebuildsForTest = 0;                // RebuildList calls (Dream.Output.Find)

	// ---- IDreamOutputRowHost ----

	virtual const FDreamOutputStore& GetStore() const override { return *Store; }
	virtual FDreamRunService* GetRunService() const override { return RunService; }
	virtual bool IsLiveView() const override { return bLive; }
	virtual const FSlateFontInfo& GetRowFont() const override { return Font; }
	virtual const FTextBlockStyle& GetLinkTextStyle() const override { return LinkStyle; }
	virtual bool IsRunCollapsed(uint32 RunNumber) const override { return Collapsed.Contains(RunNumber); }

	virtual void ToggleRunCollapsed(uint32 RunNumber) override
	{
		if (Collapsed.Remove(RunNumber) == 0)
		{
			Collapsed.Add(RunNumber);
		}
		RebuildKeeping(RunNumber);
	}

	virtual bool IsRunEchoHidden(uint32 RunNumber) const override
	{
		const FRunBuild* Build = Builds.Find(RunNumber);
		return Build == nullptr || Build->VisibleEcho == 0;
	}

	virtual void ExpandFold(uint32 RunNumber, int32 Statement) override
	{
		ExpandedFolds.Add(DreamOutputViewPrivate::FoldKey(RunNumber, Statement));
		RebuildKeeping(0);
	}

	virtual bool IsResultExpanded(uint64 Seq) const override { return ExpandedResults.Contains(Seq); }

	virtual void ToggleResultExpanded(uint64 Seq) override
	{
		if (ExpandedResults.Remove(Seq) == 0)
		{
			ExpandedResults.Add(Seq);
		}
		RebuildKeeping(0);
	}

	virtual bool IsStartupExpanded() const override { return bStartupExpanded; }

	virtual void ToggleStartupExpanded() override
	{
		bStartupExpanded = !bStartupExpanded;
		RebuildKeeping(0);
	}

	virtual void OpenLink(uint64 Seq, int32 LinkIndex) override;
	virtual void JumpToRun(uint32 RunNumber) override { ScrollToRun(RunNumber); }

	// ---- (output, PO-04) The run actions of the context menu (the footer's chips before PO-04) ----

	/** Undo this run (U9): a Partial record asks for a confirm first, the entry's second choice (RT-08). */
	void UndoRun(uint32 RunNumber)
	{
		if (RunService == nullptr)
		{
			return;
		}
		FText Reason;
		FDreamUndoPreview Preview;
		if (!RunService->CanUndoRun(static_cast<int32>(RunNumber), Reason, Preview))
		{
			DreamOutputViewPrivate::ShowStatus(Reason);
			return;
		}
		if (Preview.bPartial && !UndoConfirm.Contains(RunNumber))
		{
			UndoConfirm.Add(RunNumber);          // RT-08: a partial record asks for confirmation first
			DreamOutputViewPrivate::ShowStatus(FText::Format(LOCTEXT("Out.UndoConfirmStatus",
				"Only part of run #{0} was recorded; choose Confirm in the menu to undo the recorded part"), FText::AsNumber(RunNumber)));
			return;
		}
		const bool bConfirmed = UndoConfirm.Remove(RunNumber) > 0;
		if (!RunService->UndoRun(static_cast<int32>(RunNumber), bConfirmed, Reason))
		{
			DreamOutputViewPrivate::ShowStatus(Reason);
		}
	}

	bool IsUndoConfirmPending(uint32 RunNumber) const { return UndoConfirm.Contains(RunNumber); }

	/** Re-run (RUN-12): the same code again as a new numbered run; a refusal shows its reason. */
	void Rerun(uint32 RunNumber)
	{
		if (RunService == nullptr)
		{
			return;
		}
		const FDreamSubmitResult Result = RunService->Rerun(static_cast<int32>(RunNumber));
		if (!Result.bAccepted)
		{
			DreamOutputViewPrivate::ShowStatus(Result.Reason);
		}
	}

	/** Run without snapshot after "Journal snapshot failed: <reason>" (§10.2.5 step 4, PER-09). */
	void RerunWithoutSnapshot(uint32 RunNumber)
	{
		if (RunService == nullptr)
		{
			return;
		}
		// §10.2.5 step 4: a new numbered run with the journal snapshot step skipped; refused like any submission.
		const FDreamSubmitResult Result = RunService->RerunWithoutSnapshot(static_cast<int32>(RunNumber));
		if (!Result.bAccepted)
		{
			DreamOutputViewPrivate::ShowStatus(Result.Reason);
		}
	}

	/** (run group, B38) Run in transaction for a run that executed without one (RT-08): a re-run of the same snapshot
	 *  with the transaction override on. */
	void RerunInTransaction(uint32 RunNumber)
	{
		if (RunService == nullptr)
		{
			return;
		}
		const FDreamSubmitResult Result = RunService->RerunWithTransaction(static_cast<int32>(RunNumber));
		if (!Result.bAccepted)
		{
			DreamOutputViewPrivate::ShowStatus(Result.Reason);
		}
	}

	virtual void GetFindMatches(uint64 Seq, TArray<TPair<int32, int32>>& OutRanges, int32& OutCurrent) const override
	{
		OutRanges.Reset();
		OutCurrent = INDEX_NONE;
		if (!bFindOpen || FindHits.IsEmpty())
		{
			return;
		}
		for (int32 Index = Algo::LowerBoundBy(FindHits, Seq, [](const DreamOutputViewPrivate::FFindHit& Hit) { return Hit.Seq; });
			Index < FindHits.Num() && FindHits[Index].Seq == Seq; ++Index)
		{
			if (Index == FindCurrent)
			{
				OutCurrent = OutRanges.Num();
			}
			OutRanges.Emplace(FindHits[Index].Start, FindHits[Index].Len);
		}
	}

	virtual bool IsSoftWrap() const override { return DreamOutputViewPrivate::SoftWrapFlag(); }

	// ---- Find (§6.4 widget: Ctrl+F; dream::SearchEngine over a ring snapshot, off the game thread) ----

	/** Drops every match and the pass in flight (a new query, Clear, closing the bar). */
	void ResetFindResults()
	{
		++FindGeneration;
		if (FindCancel)
		{
			FindCancel->Cancel();
		}
		bRowWidgetsStale |= !FindHits.IsEmpty();     // only shown highlights need the rows made again
		FindRaw.Reset();
		FindHits.Reset();
		FindCurrent = INDEX_NONE;
		FindSearchedEnd = 0;
		bFindCapped = false;
		FindComplexLines = 0;
		FindNextPassSeconds = 0.0;
	}

	void ApplyFindQuery(const FString& Text, bool bMatchCase, bool bRegex, bool bWholeWord)
	{
		ResetFindResults();
		FindQuery.reset();
		FindError = FText::GetEmpty();
		if (Text.IsEmpty())
		{
			return;
		}
		dream::SearchQuery Query;
		Query.Pattern = std::u16string(DreamText::ToU16(Text));
		Query.bRegex = bRegex;
		Query.bMatchCase = bMatchCase;
		Query.bWholeWord = bWholeWord;
		dream::CompileResult Compiled = dream::SearchEngine::Compile(Query);     // game thread: compiling is cheap
		if (!Compiled.Ok())
		{
			FindError = DreamOutputViewPrivate::FindErrorText(Compiled.Error);
			return;
		}
		FindQuery = MoveTemp(Compiled.Query);
		StartFindPass();
	}

	void ApplyFindQueryFromBar()
	{
		if (!bApplyingQuery && FindBar.IsValid())
		{
			ApplyFindQuery(FindBar->GetFindText(), FindBar->IsMatchCase(), FindBar->IsRegex(), FindBar->IsWholeWord());
		}
	}

	/** Searches the lines after FindSearchedEnd on a worker, over a snapshot taken now (§6.2: readable on any thread). */
	void StartFindPass()
	{
		if (!FindQuery || FindTask.IsSet() || bFindCapped)
		{
			return;
		}
		// Nothing new (a cleared or empty ring): no snapshot, and the searched end moves up to the ring's end, so the next
		// pass waits for new lines instead of looking again at every tick.
		if (FMath::Max(FindSearchedEnd, Store->GetFirstSeq()) >= Store->GetEndSeq())
		{
			FindSearchedEnd = FMath::Max(FindSearchedEnd, Store->GetEndSeq());
			return;
		}
		dream::OutputSnapshot Snapshot = Store->GetRing().TakeSnapshot();
		const uint64 From = FMath::Max(FindSearchedEnd, Snapshot.FirstSeq());
		if (From >= Snapshot.EndSeq())
		{
			FindSearchedEnd = FMath::Max(FindSearchedEnd, Snapshot.EndSeq());
			return;
		}
		const int32 Budget = DreamOutputViewPrivate::FindMaxMatches - FindRaw.Num();
		if (Budget <= 0)
		{
			bFindCapped = true;
			return;
		}
		FindCancel = std::make_shared<dream::CancelToken>();
		FindTaskGeneration = FindGeneration;
		++FindPassesForTest;
		FindTask = UE::Tasks::Launch(UE_SOURCE_LOCATION,
			[Snapshot = MoveTemp(Snapshot), From, Query = FindQuery, Budget, Cancel = FindCancel]()
			{
				return DreamOutputViewPrivate::SearchSnapshot(Snapshot, From, Query, Budget, Cancel);
			});
	}

	/** View tick: takes a finished pass, and starts the next one for lines that arrived since (at most every 100 ms). */
	void TickFind(double Now)
	{
		if (FindTask.IsSet() && FindTask->IsCompleted())
		{
			DreamOutputViewPrivate::FFindPass Pass = MoveTemp(FindTask->GetResult());
			FindTask.Reset();
			if (FindTaskGeneration == FindGeneration && FindQuery)
			{
				const uint64 First = Store->GetFirstSeq();
				const int32 RawBefore = FindRaw.Num();
				for (DreamOutputViewPrivate::FFindHit& Hit : Pass.Hits)
				{
					if (Hit.Seq >= First)       // a Clear or an eviction since the snapshot dropped the line
					{
						FindRaw.Add(MoveTemp(Hit));
					}
				}
				FindSearchedEnd = FMath::Max(FindSearchedEnd, Pass.EndSeq);
				bFindCapped |= Pass.bCapped;
				FindComplexLines += Pass.ComplexLines;
				if (FindRaw.Num() != RawBefore)
				{
					RefilterFind();              // a pass that found nothing new changes no highlight
				}
				FindNextPassSeconds = Now + DreamOutputViewPrivate::FindPassIntervalSeconds;
			}
		}
		if (bFindOpen && FindQuery && !FindTask.IsSet() && !bFindCapped && Now >= FindNextPassSeconds
			&& Store->GetEndSeq() > FindSearchedEnd)
		{
			StartFindPass();
		}
	}

	/** The matches this view shows: its run filters, the type toggles and the background toggle (§6.4 filters). */
	void RefilterFind()
	{
		const bool bHadCurrent = FindHits.IsValidIndex(FindCurrent);
		const DreamOutputViewPrivate::FFindHit Current = bHadCurrent ? FindHits[FindCurrent] : DreamOutputViewPrivate::FFindHit();
		const TArray<DreamOutputViewPrivate::FFindHit> Before = MoveTemp(FindHits);
		const int32 CurrentBefore = FindCurrent;
		FindHits.Reset();
		FindCurrent = INDEX_NONE;
		for (const DreamOutputViewPrivate::FFindHit& Hit : FindRaw)
		{
			dream::LineInfo Info;
			Info.Type = Hit.Type;
			Info.Flags = Hit.Flags;
			if (!PassesRunFilters(Hit.RunNumber) || !IsTypeVisible(Info))
			{
				continue;
			}
			if (bHadCurrent && Hit.Seq == Current.Seq && Hit.Start == Current.Start)
			{
				FindCurrent = FindHits.Num();
			}
			FindHits.Add(Hit);
		}
		// The rows are made again only when what they highlight changed.
		bool bChanged = FindCurrent != CurrentBefore || FindHits.Num() != Before.Num();
		for (int32 Index = 0; !bChanged && Index < FindHits.Num(); ++Index)
		{
			const DreamOutputViewPrivate::FFindHit& A = FindHits[Index];
			const DreamOutputViewPrivate::FFindHit& B = Before[Index];
			bChanged = A.Seq != B.Seq || A.Start != B.Start || A.Len != B.Len;
		}
		bRowWidgetsStale |= bChanged;
	}

	/** After a drain that cleared or evicted lines: their matches go, and a cleared ring is searched again from its start. */
	void PruneFind(const FDreamAppendInfo& Info)
	{
		if (!FindQuery)
		{
			return;
		}
		if (Info.bCleared)
		{
			const bool bOpen = bFindOpen;
			std::shared_ptr<const dream::CompiledQuery> Query = FindQuery;
			ResetFindResults();
			FindQuery = MoveTemp(Query);
			bFindOpen = bOpen;
			return;
		}
		if (Info.bEvicted)
		{
			const uint64 First = Info.NewFirstSeq;
			const int32 Keep = Algo::LowerBoundBy(FindRaw, First, [](const DreamOutputViewPrivate::FFindHit& Hit) { return Hit.Seq; });
			if (Keep > 0)
			{
				FindRaw.RemoveAt(0, Keep, EAllowShrinking::No);
				RefilterFind();
			}
		}
	}

	/** The row that shows a stored line: its own row, or the row an EchoSplit part is joined into (§6.1). */
	FDreamOutputRowPtr FindLineRow(uint64 Seq) const
	{
		if (const FDreamOutputRowPtr Row = FindRow(Seq, EDreamOutputRowKind::Line))
		{
			return Row;
		}
		for (int32 Index = FMath::Min(FindRowAtOrAfter(Seq), Rows.Num() - 1); Index >= TopFixedRows && Index >= 0; --Index)
		{
			const FDreamOutputRowPtr& Row = Rows[Index];
			if (Row->Kind == EDreamOutputRowKind::Line && Row->JoinedSeqs.Contains(Seq))
			{
				return Row;
			}
			if (Row->Seq + 64 < Seq)
			{
				break;              // a joined part follows its row closely
			}
		}
		return nullptr;
	}

	/** Opens what hides a line (its collapsed block, its statement's echo fold) so its row exists. */
	void RevealLine(uint64 Seq)
	{
		dream::LineView View;
		if (!Store->GetLine(Seq, View))
		{
			return;
		}
		const uint32 RunNumber = View.Info.RunNumber;
		bool bRebuild = RunNumber != 0 && Collapsed.Remove(RunNumber) > 0;
		const FDreamRunSpan* Span = RunNumber != 0 ? Store->FindRunSpan(RunNumber) : nullptr;
		if (View.Info.Type == dream::LineType::Echo && Span != nullptr && !ExpandedRuns.Contains(RunNumber))
		{
			// The statement and the line's place in its echo, counted as ProcessLine folds them (OUT-12 Normal).
			int32 Statement = -1;
			int32 StatementEcho = 0;
			dream::LineView Echo;
			for (uint64 Scan = FMath::Max(Span->HeaderSeq, Store->GetFirstSeq()); Scan <= Seq; ++Scan)
			{
				if (!Store->GetLine(Scan, Echo) || Echo.Info.RunNumber != RunNumber || Echo.Info.Type != dream::LineType::Echo)
				{
					continue;
				}
				if ((Echo.Info.Flags & dream::OLF_EchoFirst) != 0 || Statement < 0)
				{
					++Statement;
					StatementEcho = 0;
				}
				++StatementEcho;
			}
			if (StatementEcho > FoldLines)
			{
				bool bAlready = false;
				ExpandedFolds.Add(DreamOutputViewPrivate::FoldKey(RunNumber, Statement), &bAlready);
				bRebuild |= !bAlready;
			}
		}
		if (bRebuild)
		{
			Rebuild();
		}
	}

	bool StepFind(int32 Direction)
	{
		if (FindHits.IsEmpty())
		{
			return false;
		}
		const int32 Count = FindHits.Num();
		int32 Pick = INDEX_NONE;
		if (FindHits.IsValidIndex(FindCurrent))
		{
			Pick = (FindCurrent + (Direction >= 0 ? 1 : Count - 1)) % Count;
		}
		else
		{
			// From the selected row, else the first row shown: the first match at or after it (or the last at or before it).
			uint64 Anchor = 0;
			if (const FDreamOutputRowPtr Selected = FirstSelected())
			{
				Anchor = Selected->Seq;
			}
			else if (List.IsValid() && Rows.IsValidIndex(FMath::FloorToInt(List->GetScrollOffset())))
			{
				Anchor = Rows[FMath::FloorToInt(List->GetScrollOffset())]->Seq;
			}
			if (Direction >= 0)
			{
				Pick = Algo::LowerBoundBy(FindHits, Anchor, [](const DreamOutputViewPrivate::FFindHit& Hit) { return Hit.Seq; });
				Pick = Pick < Count ? Pick : 0;
			}
			else
			{
				Pick = Algo::UpperBoundBy(FindHits, Anchor, [](const DreamOutputViewPrivate::FFindHit& Hit) { return Hit.Seq; }) - 1;
				Pick = Pick >= 0 ? Pick : Count - 1;
			}
		}
		FindCurrent = Pick;
		const uint64 Seq = FindHits[Pick].Seq;
		RevealLine(Seq);
		if (const FDreamOutputRowPtr Row = FindLineRow(Seq))
		{
			FocusRow(Row);
		}
		bRowWidgetsStale = true;
		return true;
	}

	/** Focus into the find box with its text selected; a bar put in the tree this frame may not be found yet, so the
	 *  next tick tries again. */
	void FocusFindBox()
	{
		const TSharedPtr<SEditableTextBox> Box = FindBar.IsValid() ? FindBar->GetFindBox() : nullptr;
		if (!Box.IsValid() || !FSlateApplication::IsInitialized())
		{
			return;
		}
		FindBar->FocusFindBox();
		Box->SelectAllText();
		bFocusFindBoxPending = !Box->HasKeyboardFocus();
	}

	FText FindStatus() const
	{
		if (!FindError.IsEmpty())
		{
			return FindError;
		}
		if (!FindQuery)
		{
			return FText::GetEmpty();
		}
		if (FindHits.IsEmpty())
		{
			return FindTask.IsSet() ? LOCTEXT("Out.FindSearching", "Searching…") : LOCTEXT("Out.FindNone", "No results");
		}
		const FText Total = bFindCapped ? FText::Format(LOCTEXT("Out.FindCapped", "{0}+"), FText::AsNumber(FindHits.Num()))
			: FText::AsNumber(FindHits.Num());
		return FindHits.IsValidIndex(FindCurrent)
			? FText::Format(LOCTEXT("Out.FindOf", "{0} of {1}"), FText::AsNumber(FindCurrent + 1), Total)
			: FText::Format(LOCTEXT("Out.FindCount", "{0} results"), Total);
	}

	// ---- Building rows ----

	bool PassesRunFilters(uint32 RunNumber)
	{
		if (RunFilter.IsSet() && static_cast<int32>(RunNumber) != RunFilter.GetValue())
		{
			return false;
		}
		if (RouteFilter.IsSet())
		{
			const TOptional<EDreamRunRoute> Route = RouteOf(RunNumber);
			return Route.IsSet() && Route.GetValue() == RouteFilter.GetValue();
		}
		return true;
	}

	TOptional<EDreamRunRoute> RouteOf(uint32 RunNumber)
	{
		if (RunNumber == 0)
		{
			return {};
		}
		if (const EDreamRunRoute* Cached = RouteCache.Find(RunNumber))
		{
			return *Cached;
		}
		const FDreamRunRecord* Rec = RunService ? RunService->FindRecord(static_cast<int32>(RunNumber)) : nullptr;
		if (Rec == nullptr)
		{
			return {};
		}
		RouteCache.Add(RunNumber, Rec->Route);
		return Rec->Route;
	}

	bool IsTypeVisible(const dream::LineInfo& Info) const
	{
		if ((Info.Flags & dream::OLF_Background) != 0 && !Vis.bBackground)
		{
			return false;
		}
		switch (Info.Type)
		{
		case dream::LineType::Stdout:  return Vis.bStdout;
		case dream::LineType::Stderr:  return Vis.bStderr;
		case dream::LineType::Result:  return Vis.bResults;
		case dream::LineType::Info:    return Vis.bInfo;
		case dream::LineType::Warning: return Vis.bWarnings;
		case dream::LineType::Error:   return Vis.bErrors;
		case dream::LineType::Echo:    return Vis.bEcho;
		case dream::LineType::Notice:  return Vis.bNotice;
		default:                      return true;
		}
	}

	FDreamOutputRowPtr AddRow(EDreamOutputRowKind Kind, uint64 Seq, uint32 RunNumber)
	{
		FDreamOutputRowPtr Row = MakeShared<FDreamOutputRow>();
		Row->Kind = Kind;
		Row->Seq = Seq;
		Row->RunNumber = RunNumber;
		Rows.Add(Row);
		if (Kind == EDreamOutputRowKind::Line || Kind == EDreamOutputRowKind::Header || Kind == EDreamOutputRowKind::Footer
			|| Kind == EDreamOutputRowKind::RunOutcome)
		{
			Visible.AppendOne(Seq);
		}
		LastRowRun = RunNumber;
		if (!bFollowing)
		{
			++NewLinesBelow;
		}
		return Row;
	}

	void ProcessLine(const dream::LineView& View)
	{
		const dream::LineInfo& Info = View.Info;
		const uint32 RunNumber = Info.RunNumber;
		const dream::LineType Type = Info.Type;
		const bool bBlockLine = Type == dream::LineType::RunHeader || Type == dream::LineType::RunFooter;
		if (bLive && bBlockLine)
		{
			return;                 // the Run window has its own title and state rows (§5.5.3)
		}
		if (!PassesRunFilters(RunNumber))
		{
			return;
		}
		FRunBuild& Build = Builds.FindOrAdd(RunNumber);
		// (output, PO-04) Run headers and footers are rows only with View ▸ Run headers on; a collapsed block keeps its
		// header either way, so it can be expanded again (folding stays).
		const bool bHeaderRow = bShowHeaders || (RunNumber != 0 && Collapsed.Contains(RunNumber));
		if (Type == dream::LineType::RunHeader)
		{
			Build = FRunBuild();
			Build.bHeaderShown = true;
			OpenRun = RunNumber;
			if (bHeaderRow)
			{
				AddRow(EDreamOutputRowKind::Header, View.Seq, RunNumber);
			}
			return;
		}
		if (RunNumber != 0 && !Build.bHeaderShown && !bLive)
		{
			Build.bHeaderShown = true;          // the header was evicted or cleared: a synthetic one (§6.2)
			if (bHeaderRow)
			{
				AddRow(EDreamOutputRowKind::SyntheticHeader, View.Seq, RunNumber);
			}
		}
		if (Type == dream::LineType::RunFooter)
		{
			Build.PendingSplit.Reset();
			DreamOutputRows::CloseDecoration(Build.Deco);
			if (OpenRun == RunNumber)
			{
				OpenRun = 0;
			}
			if (bShowHeaders && (RunNumber == 0 || !Collapsed.Contains(RunNumber)))
			{
				AddRow(EDreamOutputRowKind::Footer, View.Seq, RunNumber);   // a collapsed block shows its header only (§6.4)
			}
			else if (!bShowHeaders && RunNumber != 0 && !Collapsed.Contains(RunNumber) && !Build.bExceptionShown)
			{
				// (PO-04 rulings on review r6 M1 and r7 m1) Headers off: a run that ended other than ok and shows no exception
				// block of its own (a snapshot that failed, a Stop, SystemExit(3), also after unreal.log_error lines) still shows,
				// as one Maya-style line; it is the run's target for ScrollToRun, the context menu and REPL links. The footer
				// record is final here (A44).
				const FDreamRunRecord* Rec = RunService != nullptr ? RunService->FindRecord(static_cast<int32>(RunNumber)) : nullptr;
				if (Rec != nullptr && DreamOutputRows::NeedsOutcomeLine(*Rec))
				{
					AddRow(EDreamOutputRowKind::RunOutcome, View.Seq, RunNumber);
				}
			}
			return;
		}
		if (RunNumber != 0 && Collapsed.Contains(RunNumber))
		{
			return;                 // a collapsed block shows its header only (§6.4)
		}
		if (!IsTypeVisible(Info))
		{
			if (Info.Type != Build.Deco.Type)
			{
				DreamOutputRows::CloseDecoration(Build.Deco);   // a hidden line still ends the block above it
			}
			return;
		}
		if (bShowHeaders && RunNumber == 0 && (Info.Flags & dream::OLF_Background) == 0 && LastRowRun != 0 && !bLive)
		{
			AddRow(EDreamOutputRowKind::OutsideRuns, View.Seq, 0);         // §6.3 "Outside runs" block
		}

		if (Type == dream::LineType::Echo)
		{
			DreamOutputRows::CloseDecoration(Build.Deco);
			// OUT-12 Normal folding: a statement's echo shows FoldLines lines, then "… n more lines".
			if ((Info.Flags & dream::OLF_EchoFirst) != 0 || Build.Statement < 0)
			{
				++Build.Statement;
				Build.StatementEcho = 0;
				Build.Chip.Reset();
			}
			++Build.StatementEcho;
			const bool bExpanded = bLive ? ExpandedFolds.Contains(DreamOutputViewPrivate::FoldKey(RunNumber, Build.Statement))
				: (ExpandedRuns.Contains(RunNumber) || ExpandedFolds.Contains(DreamOutputViewPrivate::FoldKey(RunNumber, Build.Statement)));
			if (!bExpanded && Build.StatementEcho > FoldLines)
			{
				if (!Build.Chip.IsValid())
				{
					Build.Chip = AddRow(EDreamOutputRowKind::FoldChip, View.Seq, RunNumber);
					Build.Chip->Statement = Build.Statement;
				}
				++Build.Chip->Count;
				return;
			}
			++Build.VisibleEcho;
			Build.bEchoSinceSplit = true;
			FDreamOutputRowPtr Row = AddRow(EDreamOutputRowKind::Line, View.Seq, RunNumber);
			Row->Statement = Build.Statement;
			return;
		}

		// §6.1: a Partial | EchoSplit line is drawn joined with the next line of its type when no echo is shown between.
		if (Build.PendingSplit.IsValid() && Build.PendingSplitType == Type && !Build.bEchoSinceSplit)
		{
			Build.PendingSplit->JoinedSeqs.Add(View.Seq);
			if ((Info.Flags & dream::OLF_EchoSplit) == 0)
			{
				Build.PendingSplit.Reset();
			}
			return;
		}
		Build.PendingSplit.Reset();
		FDreamOutputRowPtr Row = AddRow(EDreamOutputRowKind::Line, View.Seq, RunNumber);
		// (output, PO-04) The Maya-style decoration; a row made in an earlier drain that changed is made again at the tick.
		DreamOutputRows::Decorate(Build.Deco, Row, Info, View.Text, DecoChanged);
		if (Type == dream::LineType::Error && (Info.Flags & (dream::OLF_FromUnrealLog | dream::OLF_FromLogPython)) == 0)
		{
			Build.bExceptionShown = true;      // a shown row of the run's own exception block (ruling on r7 m1)
		}
		if ((Info.Flags & dream::OLF_EchoSplit) != 0)
		{
			Build.PendingSplit = Row;
			Build.PendingSplitType = Type;
			Build.bEchoSinceSplit = false;
		}
		if (Type == dream::LineType::Result && (Info.Flags & dream::OLF_Truncated) != 0 && ExpandedResults.Contains(View.Seq))
		{
			// §6.4 Show all: the rest of the result in place, as continuation rows of at most 10,000 characters.
			const FString Blob = Store->GetResultBlob(View.Seq);
			for (int32 Start = 0; Start < Blob.Len(); Start += 10000)
			{
				FDreamOutputRowPtr More = AddRow(EDreamOutputRowKind::ResultMore, View.Seq, RunNumber);
				More->Text = Blob.Mid(Start, 10000);
				DreamOutputRows::ExtendDecoration(Build.Deco, More, DecoChanged);   // the repr's " #" follows its last row
			}
		}
	}

	/** (output, PO-04) Rows whose decoration changed after the list made their widgets: made again at the next tick. */
	void ApplyDecoChanges()
	{
		if (DecoChanged.IsEmpty())
		{
			return;
		}
		for (const FDreamOutputRowPtr& Row : DecoChanged)
		{
			if (List.IsValid() && List->WidgetFromItem(Row).IsValid())
			{
				bRowWidgetsStale = true;
				break;
			}
		}
		DecoChanged.Reset();
	}

	void AppendThrough(uint64 EndSeq)
	{
		dream::LineView View;
		for (uint64 Seq = FMath::Max(NextSeq, Store->GetFirstSeq()); Seq < EndSeq; ++Seq)
		{
			if (Store->GetLine(Seq, View))
			{
				ProcessLine(View);
			}
		}
		NextSeq = FMath::Max(NextSeq, EndSeq);
	}

	void AddTopRows()
	{
		TopFixedRows = 0;
		bEvictionRowShown = false;
		if (bLive)
		{
			return;
		}
		if (Store->GetEvictedLineCount() > 0)
		{
			AddRow(EDreamOutputRowKind::Eviction, 0, 0);
			bEvictionRowShown = true;
		}
		const TArray<FDreamStartupLine>& Startup = Store->GetStartupProblems();
		StartupRowsBuilt = Startup.Num();
		if (!RouteFilter.IsSet() && Startup.Num() > 0)
		{
			FDreamOutputRowPtr Header = AddRow(EDreamOutputRowKind::StartupHeader, 0, 0);
			Header->Count = Startup.Num();
			if (bStartupExpanded)
			{
				for (const FDreamStartupLine& Line : Startup)
				{
					FDreamOutputRowPtr Row = AddRow(EDreamOutputRowKind::StartupLine, 0, 0);
					Row->Text = Line.Text;
					Row->Verbosity = Line.Verbosity;
				}
			}
		}
		TopFixedRows = Rows.Num();
		LastRowRun = MAX_uint32;
		NewLinesBelow = 0;
	}

	void UpdateProvisional()
	{
		if (ProvisionalRow.IsValid())
		{
			if (!Rows.IsEmpty() && Rows.Last() == ProvisionalRow)
			{
				Rows.Pop(EAllowShrinking::No);
			}
			ProvisionalRow.Reset();
		}
		FString Text;
		if (OpenRun != 0 && !Collapsed.Contains(OpenRun) && Store->GetProvisionalLine(OpenRun, Text) && !Text.IsEmpty())
		{
			ProvisionalRow = MakeShared<FDreamOutputRow>();
			ProvisionalRow->Kind = EDreamOutputRowKind::Provisional;
			ProvisionalRow->Seq = Store->GetEndSeq();
			ProvisionalRow->RunNumber = OpenRun;
			ProvisionalRow->Text = MoveTemp(Text);
			Rows.Add(ProvisionalRow);
		}
	}

	void Rebuild()
	{
		// (output, PO-06 F3) The text selection is kept across the new rows by its rows' sequences and kinds.
		const FSelKey AnchorKey = KeyOf(SelAnchor);
		const FSelKey CaretKey = KeyOf(SelCaret);
		const FSelKey UnitFirstKey = KeyOf(UnitFirst);
		const FSelKey UnitLastKey = KeyOf(UnitLast);
		Vis = Store->GetVisibility();
		if (const UDreamUserSettings* Settings = UDreamUserSettings::Get())
		{
			FoldLines = FMath::Max(1, Settings->EchoFoldLines);
			bShowHeaders = Settings->bShowRunHeaders && !bLive;     // the Run window has its own title and state rows
		}
		Rows.Reset();
		Visible.Clear();
		Builds.Reset();
		DecoChanged.Reset();
		ProvisionalRow.Reset();
		OpenRun = 0;
		NextSeq = Store->GetFirstSeq();
		if (RunFilter.IsSet())
		{
			// The live run view is built inside a pump when the Run window opens (§5.5.3): every line of run N comes after
			// its header, so the scan starts there instead of at the start of the ring.
			if (const FDreamRunSpan* Span = Store->FindRunSpan(static_cast<uint32>(RunFilter.GetValue())))
			{
				NextSeq = FMath::Max(NextSeq, Span->HeaderSeq);
			}
		}
		const bool bWasFollowing = bFollowing;
		bFollowing = true;                  // building is not "new lines below"
		AddTopRows();
		AppendThrough(Store->GetEndSeq());
		UpdateProvisional();
		// Every row was made just now, so none has an old widget to redraw (review r6 m3: cleared after the build, not
		// before it, or the next drain would make the list's rows again for nothing).
		DecoChanged.Reset();
		bFollowing = bWasFollowing;
		NewLinesBelow = 0;
		SelAnchor = PosOf(AnchorKey);
		SelCaret = PosOf(CaretKey);
		UnitFirst = PosOf(UnitFirstKey);
		UnitLast = PosOf(UnitLastKey);
		if (!SelAnchor.Row.IsValid() || !SelCaret.Row.IsValid())
		{
			ClearSelectionState();          // an end's row is no row any more (a hidden type, a collapsed block)
		}
		SyncListSelection();
		if (List.IsValid())
		{
			List->RequestListRefresh();
		}
	}

	/** Rebuilds and keeps the reader's place: following stays at the end; otherwise the first visible row stays first
	 *  (or the given run's header, after a collapse toggle). */
	void RebuildKeeping(uint32 AnchorRun)
	{
		uint64 AnchorSeq = 0;
		if (!bFollowing && List.IsValid() && Rows.Num() > 0)
		{
			const int32 First = FMath::Clamp(FMath::FloorToInt(List->GetScrollOffset()), 0, Rows.Num() - 1);
			AnchorSeq = Rows[First]->Seq;
		}
		if (AnchorRun != 0)
		{
			if (const FDreamRunSpan* Span = Store->FindRunSpan(AnchorRun))
			{
				AnchorSeq = Span->HeaderSeq;
			}
		}
		Rebuild();
		if (bFollowing)
		{
			ScrollBottom();
		}
		else if (List.IsValid())
		{
			SetOffset(static_cast<float>(FindRowAtOrAfter(AnchorSeq)));
		}
	}

	void DropEvicted(uint64 NewFirstSeq)
	{
		int32 Remove = 0;
		while (TopFixedRows + Remove < Rows.Num()
			&& Rows[TopFixedRows + Remove] != ProvisionalRow
			&& Rows[TopFixedRows + Remove]->Seq < NewFirstSeq)
		{
			++Remove;
		}
		if (Remove > 0)
		{
			Rows.RemoveAt(TopFixedRows, Remove, EAllowShrinking::No);
			if (!bFollowing && List.IsValid())
			{
				SetOffset(FMath::Max(0.f, List->GetScrollOffset() - static_cast<float>(Remove)));
			}
		}
		Visible.DropBefore(NewFirstSeq);
		// (output, PO-06 F3) An end of the text selection on a dropped row moves to the first row left.
		if (SelAnchor.Row.IsValid() || SelCaret.Row.IsValid())
		{
			const bool bAnchor = IndexOfRow(SelAnchor.Row.Get()) != INDEX_NONE;
			const bool bCaret = IndexOfRow(SelCaret.Row.Get()) != INDEX_NONE;
			if (!bAnchor && !bCaret)
			{
				ClearSelectionState();
			}
			else if (!bAnchor || !bCaret)
			{
				const FSelPos First{ Rows.IsValidIndex(TopFixedRows) ? Rows[TopFixedRows] : nullptr, 0 };
				(bAnchor ? SelCaret : SelAnchor) = First;
			}
		}
		// A run whose header was evicted while some of its lines remain gets a synthetic header (§6.2 eviction marker),
		// when headers are shown (PO-04).
		if (!bLive && bShowHeaders && Rows.IsValidIndex(TopFixedRows))
		{
			const FDreamOutputRowPtr& First = Rows[TopFixedRows];
			if (First->RunNumber != 0 && First->Kind != EDreamOutputRowKind::Header && First->Kind != EDreamOutputRowKind::SyntheticHeader)
			{
				FDreamOutputRowPtr Synthetic = MakeShared<FDreamOutputRow>();
				Synthetic->Kind = EDreamOutputRowKind::SyntheticHeader;
				Synthetic->Seq = First->Seq;
				Synthetic->RunNumber = First->RunNumber;
				Rows.Insert(Synthetic, TopFixedRows);
			}
		}
	}

	void HandleAppended(const FDreamAppendInfo& Info)
	{
		PruneFind(Info);
		const bool bNeedEvictionRow = !bLive && !bEvictionRowShown && Store->GetEvictedLineCount() > 0;
		const bool bStartupChanged = Store->GetStartupProblems().Num() != StartupRowsBuilt && !bLive;
		if (Info.bCleared || bNeedEvictionRow || bStartupChanged)
		{
			RebuildKeeping(0);
			return;
		}
		if (ProvisionalRow.IsValid() && !Rows.IsEmpty() && Rows.Last() == ProvisionalRow)
		{
			Rows.Pop(EAllowShrinking::No);
			ProvisionalRow.Reset();
		}
		if (Info.bEvicted)
		{
			DropEvicted(Info.NewFirstSeq);
		}
		AppendThrough(Info.EndSeq);
		ApplyDecoChanges();
		UpdateProvisional();
		if (List.IsValid())
		{
			List->RequestListRefresh();
		}
		if (bFollowing && !bSelPressing)    // (output, PO-06 F3) not under a press: the release scrolls (HandleSelectRelease)
		{
			ScrollBottom();
		}
	}

	// ---- Scrolling and follow mode (OUT-09) ----

	/** Our own scrolls (follow, anchors, eviction, hand-off) are not the user leaving follow mode: the table view fires
	 *  its scroll event synchronously from SetScrollOffset (STableViewBase.cpp:1040-1048), so a guard is exact. */
	void SetOffset(float Offset)
	{
		bProgrammaticScroll = true;
		List->SetScrollOffset(Offset);
		bProgrammaticScroll = false;
		LastOffset = List->GetScrollOffset();
		ShownOffset = LastOffset;
	}

	void ScrollBottom()
	{
		if (List.IsValid())
		{
			bProgrammaticScroll = true;
			List->ScrollToBottom();
			bProgrammaticScroll = false;
			LastOffset = List->GetScrollOffset();
		}
	}

	bool IsAtBottom() const
	{
		if (!List.IsValid() || !List->IsScrollbarNeeded())
		{
			return true;
		}
		const double RemainingRows = List->GetScrollDistanceRemaining().Y * static_cast<double>(Rows.Num());
		return RemainingRows < 1.0;
	}

	/** OUT-09: a user scroll toward older lines holds the view; reaching the bottom again resumes following (Tick). */
	void HandleScrolled(double Offset)
	{
		if (bProgrammaticScroll)
		{
			return;
		}
		ShownOffset = static_cast<float>(Offset);
		// Only the user leaves follow mode: a wheel, a scrollbar drag or a key, all routed as Slate input. The list's own
		// tick clamps ScrollToBottom's offset (the item count, STableViewBase.cpp:1346-1351) to the real bottom, or to 0
		// when every row fits (:419-428), and fires this event outside our guard; that is follow mode working, not a
		// scroll toward older lines (in the Run window's live view it happens at every pump, PO-03).
		if (bFollowing && !(FSlateApplication::IsInitialized() && FSlateApplication::Get().IsProcessingInput()))
		{
			LastOffset = static_cast<float>(Offset);
			return;
		}
		if (static_cast<float>(Offset) < LastOffset - 0.001f)
		{
			bFollowing = false;
			BottomCheckAfterTick = TickCount + 2;   // the scrollbar reflects the new offset after the next layout
		}
		LastOffset = static_cast<float>(Offset);
	}

	void TickFollow()
	{
		++TickCount;
		if (!bFollowing && TickCount >= BottomCheckAfterTick && List.IsValid() && !List->IsPendingRefresh() && IsAtBottom())
		{
			bFollowing = true;
			NewLinesBelow = 0;
		}
	}

	/** Find passes, the soft-wrap toggle and match highlights: the shown row widgets are made again when they change. */
	void TickRows()
	{
		if (bFocusFindBoxPending && bFindOpen)
		{
			bFocusFindBoxPending = false;
			FocusFindBox();
			bFocusFindBoxPending = false;        // one retry: a reader who clicked elsewhere keeps that focus
		}
		TickFind(FPlatformTime::Seconds());
		if (bWrapApplied != DreamOutputViewPrivate::SoftWrapFlag())
		{
			bWrapApplied = DreamOutputViewPrivate::SoftWrapFlag();
			bRowWidgetsStale = true;
		}
		if (bRowWidgetsStale && List.IsValid())
		{
			bRowWidgetsStale = false;
			++RowRebuildsForTest;
			List->RebuildList();
		}
	}

	int32 FindRowAtOrAfter(uint64 Seq) const
	{
		int32 Low = TopFixedRows;
		int32 High = Rows.Num();
		while (Low < High)
		{
			const int32 Mid = (Low + High) / 2;
			if (Rows[Mid]->Seq < Seq)
			{
				Low = Mid + 1;
			}
			else
			{
				High = Mid;
			}
		}
		return FMath::Min(Low, FMath::Max(0, Rows.Num() - 1));
	}

	FDreamOutputRowPtr FindRow(uint64 Seq, EDreamOutputRowKind Kind) const
	{
		for (int32 Index = FindRowAtOrAfter(Seq); Index < Rows.Num() && Rows[Index]->Seq == Seq; ++Index)
		{
			if (Rows[Index]->Kind == Kind)
			{
				return Rows[Index];
			}
		}
		return nullptr;
	}

	void FocusRow(const FDreamOutputRowPtr& Row)
	{
		if (!List.IsValid() || !Row.IsValid())
		{
			return;
		}
		bFollowing = false;
		BottomCheckAfterTick = TickCount + 2;
		List->RequestScrollIntoView(Row);
		ClearSelectionState();              // (output, PO-06 F3) a row chosen for the reader replaces the text selection
		if (!bLive)
		{
			List->SetSelection(Row, ESelectInfo::Direct);
		}
	}

	bool ScrollToRun(uint32 RunNumber)
	{
		if (Collapsed.Remove(RunNumber) > 0)
		{
			Rebuild();
		}
		// (PO-04 ruling on review r6 M1) With run headers hidden, a run that ended other than ok with no error line has one
		// outcome line, which stands for the header: it is the run's target (RV-16). It sits at the footer's sequence.
		if (!bShowHeaders)
		{
			const FDreamRunSpan* Span = Store->FindRunSpan(RunNumber);
			if (const FDreamOutputRowPtr Outcome = Span != nullptr && Span->FooterSeq != 0 ? FindRow(Span->FooterSeq, EDreamOutputRowKind::RunOutcome) : nullptr)
			{
				FocusRow(Outcome);
				return true;
			}
		}
		// The run's header; (output, PO-04) with run headers hidden, the first row the run shows (a header comes first).
		for (int32 Index = TopFixedRows; Index < Rows.Num(); ++Index)
		{
			const FDreamOutputRowPtr& Row = Rows[Index];
			if (Row->RunNumber == RunNumber
				&& (Row->Kind == EDreamOutputRowKind::Header || Row->Kind == EDreamOutputRowKind::SyntheticHeader
					|| Row->Kind == EDreamOutputRowKind::Line || Row->Kind == EDreamOutputRowKind::FoldChip
					|| Row->Kind == EDreamOutputRowKind::RunOutcome))
			{
				FocusRow(Row);
				return true;
			}
		}
		return false;
	}

	bool ScrollToEchoLine(uint32 RunNumber, int32 Line)
	{
		const FDreamRunSpan* Span = Store->FindRunSpan(RunNumber);
		if (Span == nullptr)
		{
			return false;
		}
		const uint64 Begin = FMath::Max(Span->HeaderSeq, Store->GetFirstSeq());
		const uint64 End = Span->FooterSeq != 0 ? Span->FooterSeq : Store->GetEndSeq();
		int32 Statement = -1;
		int32 StatementEcho = 0;
		uint64 Found = 0;
		bool bFound = false;
		dream::LineView View;
		for (uint64 Seq = Begin; Seq < End; ++Seq)
		{
			if (!Store->GetLine(Seq, View) || View.Info.RunNumber != RunNumber || View.Info.Type != dream::LineType::Echo)
			{
				continue;
			}
			if ((View.Info.Flags & dream::OLF_EchoFirst) != 0 || Statement < 0)
			{
				++Statement;
				StatementEcho = 0;
			}
			++StatementEcho;
			if (View.Info.EchoLine == Line)
			{
				Found = Seq;
				bFound = true;
				break;
			}
		}
		if (!bFound || !Vis.bEcho)
		{
			return ScrollToRun(RunNumber);     // hidden or evicted echo: the header shows the submitted code (§6.4)
		}
		bool bRebuild = Collapsed.Remove(RunNumber) > 0;
		if (StatementEcho > FoldLines)
		{
			bool bAlready = false;
			ExpandedFolds.Add(DreamOutputViewPrivate::FoldKey(RunNumber, Statement), &bAlready);
			bRebuild |= !bAlready;
		}
		if (bRebuild)
		{
			Rebuild();
		}
		const FDreamOutputRowPtr Row = FindRow(Found, EDreamOutputRowKind::Line);
		if (!Row.IsValid())
		{
			return ScrollToRun(RunNumber);
		}
		FocusRow(Row);
		return true;
	}

	bool WalkErrors(int32 Direction, FText& OutStatus)
	{
		// U13: link spans on error, stderr and warning lines, in output order, wrapping at either end.
		struct FStop
		{
			uint64 Seq;
			int32 Link;
		};
		TArray<FStop> Stops;
		dream::LineView View;
		for (uint64 Seq = Store->GetFirstSeq(); Seq < Store->GetEndSeq(); ++Seq)
		{
			if (!Store->GetLine(Seq, View) || View.Links.empty())
			{
				continue;
			}
			const dream::LineType Type = View.Info.Type;
			if (Type != dream::LineType::Error && Type != dream::LineType::Stderr && Type != dream::LineType::Warning)
			{
				continue;
			}
			if (!PassesRunFilters(View.Info.RunNumber))
			{
				continue;
			}
			for (int32 Link = 0; Link < static_cast<int32>(View.Links.size()); ++Link)
			{
				Stops.Add({Seq, Link});
			}
		}
		if (Stops.IsEmpty())
		{
			OutStatus = LOCTEXT("Out.NoErrors", "No errors in output");
			return false;
		}
		auto After = [this](const FStop& Stop) { return Stop.Seq > WalkSeq || (Stop.Seq == WalkSeq && Stop.Link > WalkLink); };
		auto Before = [this](const FStop& Stop) { return Stop.Seq < WalkSeq || (Stop.Seq == WalkSeq && Stop.Link < WalkLink); };
		int32 Pick = INDEX_NONE;
		if (Direction >= 0)
		{
			Pick = Stops.IndexOfByPredicate(After);
			if (Pick == INDEX_NONE)
			{
				Pick = 0;
				OutStatus = LOCTEXT("Out.WrappedFirst", "Wrapped to the first error");
			}
		}
		else
		{
			for (int32 Index = Stops.Num() - 1; Index >= 0; --Index)
			{
				if (Before(Stops[Index]))
				{
					Pick = Index;
					break;
				}
			}
			if (Pick == INDEX_NONE)
			{
				Pick = Stops.Num() - 1;
				OutStatus = LOCTEXT("Out.WrappedLast", "Wrapped to the last error");
			}
		}
		WalkSeq = Stops[Pick].Seq;
		WalkLink = Stops[Pick].Link;
		if (Store->GetLine(WalkSeq, View) && Collapsed.Remove(View.Info.RunNumber) > 0)
		{
			Rebuild();      // "expands a collapsed block if needed"
		}
		if (const FDreamOutputRowPtr Row = FindRow(WalkSeq, EDreamOutputRowKind::Line))
		{
			FocusRow(Row);
		}
		OpenLink(WalkSeq, WalkLink);
		return true;
	}

	void HandleRunFinished(const FDreamRunRecord& Record)
	{
		UndoConfirm.Remove(static_cast<uint32>(Record.RunNumber));
		if (Record.Outcome == EDreamRunOutcome::Error)
		{
			// §6.4: after a failure the next F4 lands on that run's first frame.
			if (const FDreamRunSpan* Span = Store->FindRunSpan(static_cast<uint32>(Record.RunNumber)))
			{
				WalkSeq = Span->HeaderSeq;
				WalkLink = -1;
			}
		}
	}

	// ---- Copy and the context menu (§6.4 "Design: copy") ----

	FString SelectedText() const
	{
		if (HasTextSelection())
		{
			return TextSelectionText();     // (output, PO-06 F3) exactly the selected characters, as drawn
		}
		if (!List.IsValid() || bLive)
		{
			return FString();
		}
		TArray<FString> Lines;
		for (const FDreamOutputRowPtr& Row : Rows)
		{
			if (List->IsItemSelected(Row))
			{
				if (Row->Kind != EDreamOutputRowKind::FoldChip && Row->Kind != EDreamOutputRowKind::Eviction)
				{
					Lines.Add(DreamOutputRows::GetRowRawText(*Row, *this));
				}
			}
		}
		return FString::Join(Lines, TEXT("\n"));
	}

	FDreamOutputRowPtr FirstSelected() const
	{
		if (!List.IsValid())
		{
			return nullptr;
		}
		for (const FDreamOutputRowPtr& Row : Rows)
		{
			if (List->IsItemSelected(Row))
			{
				return Row;
			}
		}
		return nullptr;
	}

	/** Every non-echo output line of a run, raw (Copy run output). */
	FString RunOutputText(uint32 RunNumber) const
	{
		const FDreamRunSpan* Span = Store->FindRunSpan(RunNumber);
		if (Span == nullptr)
		{
			return FString();
		}
		TArray<FString> Lines;
		dream::LineView View;
		const uint64 End = Span->FooterSeq != 0 ? Span->FooterSeq : Store->GetEndSeq();
		for (uint64 Seq = FMath::Max(Span->HeaderSeq, Store->GetFirstSeq()); Seq < End; ++Seq)
		{
			if (Store->GetLine(Seq, View) && View.Info.RunNumber == RunNumber && View.Info.Type != dream::LineType::Echo
				&& View.Info.Type != dream::LineType::RunHeader)
			{
				Lines.Add(DreamText::Utf8ToFString(View.Text) + ((View.Info.Flags & dream::OLF_Truncated) ? Store->GetResultBlob(Seq) : FString()));
			}
		}
		return FString::Join(Lines, TEXT("\n"));
	}

	/** The run of error lines around the selected one (Copy traceback). */
	FString TracebackText(const FDreamOutputRowPtr& Row) const
	{
		dream::LineView View;
		if (!Row.IsValid() || !Store->GetLine(Row->Seq, View) || View.Info.Type != dream::LineType::Error)
		{
			return FString();
		}
		const uint32 RunNumber = View.Info.RunNumber;
		auto IsTracebackLine = [this, RunNumber](uint64 Seq, dream::LineView& Out)
		{
			return Store->GetLine(Seq, Out) && Out.Info.RunNumber == RunNumber && Out.Info.Type == dream::LineType::Error;
		};
		uint64 First = Row->Seq;
		while (First > Store->GetFirstSeq() && IsTracebackLine(First - 1, View))
		{
			--First;
		}
		TArray<FString> Lines;
		for (uint64 Seq = First; IsTracebackLine(Seq, View); ++Seq)
		{
			Lines.Add(DreamText::Utf8ToFString(View.Text));
		}
		return FString::Join(Lines, TEXT("\n"));
	}

	TSharedPtr<SWidget> MakeContextMenu();
	/** The context menu's entries on the selected rows; OutActions (tests) gets each plain entry's action by its label. */
	void FillContextMenu(FMenuBuilder& Menu, TMap<FString, FUIAction>* OutActions);
	void ExpandEcho(bool bExpand);
	FReply HandleListKey(const FGeometry& Geometry, const FKeyEvent& KeyEvent);

	/** (output, PO-04) A view setting changed: the run headers toggle rebuilds the rows, anything else repaints them. */
	void HandleViewSettingsChanged()
	{
		const UDreamUserSettings* Settings = UDreamUserSettings::Get();
		if (Settings != nullptr && (Settings->bShowRunHeaders && !bLive) != bShowHeaders)
		{
			// The selected rows by (sequence, kind) are selected again in the rows made anew (review r6 m5); a header or
			// footer that is no row any more drops out.
			TArray<TPair<uint64, EDreamOutputRowKind>> Selected;
			for (const FDreamOutputRowPtr& Row : Rows)
			{
				if (List.IsValid() && List->IsItemSelected(Row))
				{
					Selected.Emplace(Row->Seq, Row->Kind);
				}
			}
			RebuildKeeping(0);
			if (List.IsValid() && !Selected.IsEmpty())
			{
				List->ClearSelection();
				for (const TPair<uint64, EDreamOutputRowKind>& Key : Selected)
				{
					if (const FDreamOutputRowPtr Row = FindRow(Key.Key, Key.Value))
					{
						List->SetItemSelection(Row, true);
					}
				}
			}
			return;
		}
		bRowWidgetsStale = true;
	}

	/** (output, PO-04) The theme changed: the link style and every shown row take the new colours. */
	void HandleThemeChanged()
	{
		LinkStyle.SetColorAndOpacity(FDreamEditorStyle::Get().GetSlateColor(DreamStyleKeys::OutputLink));
		DreamOutputViewPrivate::RefreshRowStyle();
		bRowWidgetsStale = true;
	}

	// ---- (output, PO-06 F3) Text selection (Shell/DreamOutputSelection.h) ----

	/** A place in the history: a row and an offset in its drawn text (DreamOutputRows::GetRowDisplayText). */
	struct FSelPos
	{
		FDreamOutputRowPtr Row;
		int32 Offset = 0;
	};

	/** What a press extends by while it drags: characters, the words of a double-click, the lines of a triple-click. */
	enum class ESelUnit : uint8
	{
		Char,
		Word,
		Line,
	};

	/** A place kept across a rebuild of the rows (new row objects): the row by its sequence, kind and rank among the rows
	 *  of that sequence and kind (the top rows by kind and rank). */
	struct FSelKey
	{
		uint64 Seq = 0;
		EDreamOutputRowKind Kind = EDreamOutputRowKind::Line;
		int32 Ordinal = INDEX_NONE;
		bool bTop = false;
		int32 Offset = 0;
	};

	FSelPos SelAnchor;
	FSelPos SelCaret;
	ESelUnit SelUnit = ESelUnit::Char;
	FSelPos UnitFirst;                      // the word or line a double or triple click took: [UnitFirst, UnitLast)
	FSelPos UnitLast;
	bool bSelPressing = false;              // the left button is down on the rows (the surface has the mouse capture)
	bool bSelDragged = false;               // it moved past the drag distance since the press
	bool bSelShift = false;                 // the press extended the selection (Shift)
	FVector2D SelPressPos = FVector2D::ZeroVector;
	FVector2D SelPointerPos = FVector2D::ZeroVector;
	FSelPos SelPressHit;
	TWeakPtr<SButton> SelPressButton;       // the link, chip or header button under the press: a click without a drag clicks it
	double SelLastClickSeconds = -1000.0;
	FVector2D SelLastClickPos = FVector2D::ZeroVector;
	int32 SelClicks = 0;                    // 2 after a double-click: the next press within the double-click time is a triple
	bool bSyncingListSelection = false;     // our own changes of the list's selection do not drop the text selection
	TSharedPtr<SDreamOutputSurface> Surface;

	/** The index of a row in Rows, INDEX_NONE when it is no row now (O(log n): the rows are in sequence order). */
	int32 IndexOfRow(const FDreamOutputRow* Row) const
	{
		if (Row == nullptr)
		{
			return INDEX_NONE;
		}
		const int32 Fixed = FMath::Min(TopFixedRows, Rows.Num());
		for (int32 Index = 0; Index < Fixed; ++Index)
		{
			if (Rows[Index].Get() == Row)
			{
				return Index;
			}
		}
		if (Rows.Num() > Fixed)
		{
			for (int32 Index = FindRowAtOrAfter(Row->Seq); Index < Rows.Num() && Rows[Index]->Seq == Row->Seq; ++Index)
			{
				if (Rows[Index].Get() == Row)
				{
					return Index;
				}
			}
			if (Rows.Last().Get() == Row)
			{
				return Rows.Num() - 1;          // the provisional row
			}
		}
		return INDEX_NONE;
	}

	FString RowText(int32 Index) const
	{
		return Rows.IsValidIndex(Index) ? DreamOutputRows::GetRowDisplayText(*Rows[Index], *this) : FString();
	}

	FString RowText(const FDreamOutputRowPtr& Row) const
	{
		return Row.IsValid() ? DreamOutputRows::GetRowDisplayText(*Row, *this) : FString();
	}

	/** The selection's ends in order: rows A <= B and their offsets. False without both ends on rows shown now. */
	bool SelectionRange(int32& OutA, int32& OutAOffset, int32& OutB, int32& OutBOffset) const
	{
		const int32 Anchor = IndexOfRow(SelAnchor.Row.Get());
		const int32 Caret = Anchor != INDEX_NONE ? IndexOfRow(SelCaret.Row.Get()) : INDEX_NONE;
		if (Anchor == INDEX_NONE || Caret == INDEX_NONE)
		{
			return false;
		}
		const bool bForward = Anchor < Caret || (Anchor == Caret && SelAnchor.Offset <= SelCaret.Offset);
		OutA = bForward ? Anchor : Caret;
		OutAOffset = bForward ? SelAnchor.Offset : SelCaret.Offset;
		OutB = bForward ? Caret : Anchor;
		OutBOffset = bForward ? SelCaret.Offset : SelAnchor.Offset;
		return true;
	}

	/** -1, 0 or 1 as A is before, at or after B (both on rows shown now). */
	int32 ComparePos(const FSelPos& A, const FSelPos& B) const
	{
		const int32 IndexA = IndexOfRow(A.Row.Get());
		const int32 IndexB = IndexOfRow(B.Row.Get());
		if (IndexA != IndexB)
		{
			return IndexA < IndexB ? -1 : 1;
		}
		return A.Offset == B.Offset ? 0 : (A.Offset < B.Offset ? -1 : 1);
	}

	void ClearSelectionState()
	{
		SelAnchor = FSelPos();
		SelCaret = FSelPos();
		UnitFirst = FSelPos();
		UnitLast = FSelPos();
		SelUnit = ESelUnit::Char;
	}

	/** The list selects the rows the text selection spans, so the context menu, Copy run output, Expand Echo and a
	 *  right-click inside the selection act on them (a right-click on another row selects that row, which drops it). */
	void SyncListSelection()
	{
		int32 A = 0;
		int32 AOffset = 0;
		int32 B = 0;
		int32 BOffset = 0;
		if (bLive || !List.IsValid() || !HasTextSelection() || !SelectionRange(A, AOffset, B, BOffset))
		{
			return;
		}
		TGuardValue<bool> Guard(bSyncingListSelection, true);
		List->Private_ClearSelection();
		for (int32 Index = A; Index <= B; ++Index)
		{
			List->Private_SetItemSelection(Rows[Index], true, Index == B);
		}
		List->Private_SignalSelectionChanged(ESelectInfo::Direct);
	}

	void SetSelection(const FSelPos& Anchor, const FSelPos& Caret, bool bSyncList)
	{
		SelAnchor = Anchor;
		SelCaret = Caret;
		if (bSyncList)
		{
			SyncListSelection();
		}
	}

	/** The word (double-click) or the whole drawn line (triple-click) around a place: [OutFirst, OutLast). */
	void UnitAround(const FSelPos& At, ESelUnit Unit, FSelPos& OutFirst, FSelPos& OutLast) const
	{
		const FString Text = RowText(At.Row);
		int32 Start = 0;
		int32 End = Text.Len();
		if (Unit == ESelUnit::Word)
		{
			DreamOutputSelection::WordAt(Text, At.Offset, Start, End);
		}
		OutFirst = FSelPos{ At.Row, Start };
		OutLast = FSelPos{ At.Row, End };
	}

	void SelectUnit(const FSelPos& At, ESelUnit Unit)
	{
		SelUnit = Unit;
		UnitAround(At, Unit, UnitFirst, UnitLast);
		SetSelection(UnitFirst, UnitLast, /*bSyncList*/ true);
	}

	/** The list's row widget under a point in desktop space. bClamp: above the rows the top row shown, below them the
	 *  bottom one (a drag outside the view); otherwise only a point on a row, or under the last row, hits. */
	TSharedPtr<SDreamOutputTableRow> RowWidgetAt(const FVector2D& Screen, bool bClamp, bool& bOutBelowLast) const
	{
		bOutBelowLast = false;
		if (!List.IsValid() || Rows.IsEmpty())
		{
			return nullptr;
		}
		const int32 First = FMath::Clamp(FMath::FloorToInt(List->GetScrollOffset()), 0, Rows.Num() - 1);
		const int32 End = FMath::Min(Rows.Num(), First + List->GetNumGeneratedChildren() + 3);
		TSharedPtr<SDreamOutputTableRow> Hit;
		TSharedPtr<SDreamOutputTableRow> Top;
		TSharedPtr<SDreamOutputTableRow> Bottom;
		double TopY = TNumericLimits<double>::Max();
		double BottomY = TNumericLimits<double>::Lowest();
		double Left = 0.0;
		double Right = 0.0;
		for (int32 Index = FMath::Max(0, First - 2); Index < End; ++Index)
		{
			const TSharedPtr<ITableRow> Widget = List->WidgetFromItem(Rows[Index]);
			if (!Widget.IsValid())
			{
				continue;
			}
			const TSharedPtr<SDreamOutputTableRow> RowWidget = StaticCastSharedPtr<SDreamOutputTableRow>(Widget);
			const FGeometry& Geometry = RowWidget->GetPaintSpaceGeometry();
			const FVector2D Pos(Geometry.GetAbsolutePosition());
			const FVector2D Size(Geometry.GetAbsoluteSize());
			if (Size.X <= 0.0 || Size.Y <= 0.0 || !RowWidget->GetFrame().IsValid())
			{
				continue;           // made, not laid out yet
			}
			Left = Pos.X;
			Right = Pos.X + Size.X;
			if (Pos.Y < TopY)
			{
				TopY = Pos.Y;
				Top = RowWidget;
			}
			if (Pos.Y + Size.Y > BottomY)
			{
				BottomY = Pos.Y + Size.Y;
				Bottom = RowWidget;
			}
			if (Screen.Y >= Pos.Y && Screen.Y < Pos.Y + Size.Y)
			{
				Hit = RowWidget;
			}
		}
		if (!Top.IsValid())
		{
			return nullptr;
		}
		if (!bClamp && (Screen.X < Left || Screen.X > Right))
		{
			return nullptr;         // the scroll bar, or outside the rows
		}
		if (!Hit.IsValid())
		{
			const bool bBelow = Screen.Y >= BottomY;
			if (bClamp)
			{
				Hit = bBelow ? Bottom : Top;
			}
			else if (bBelow && Bottom->GetFrame()->GetRow() == Rows.Last())
			{
				Hit = Bottom;       // the empty space under the last row: its end
				bOutBelowLast = true;
			}
		}
		return Hit;
	}

	/** The place under a point in desktop space (see RowWidgetAt), and the enabled button there. */
	bool HitTest(const FVector2D& Screen, bool bClamp, FSelPos& OutPos, TSharedPtr<SButton>* OutButton) const
	{
		bool bBelowLast = false;
		const TSharedPtr<SDreamOutputTableRow> RowWidget = RowWidgetAt(Screen, bClamp, bBelowLast);
		if (!RowWidget.IsValid())
		{
			return false;
		}
		const TSharedPtr<SDreamOutputRowFrame>& Frame = RowWidget->GetFrame();
		const FGeometry& Geometry = Frame->GetPaintSpaceGeometry();
		OutPos.Row = Frame->GetRow();
		OutPos.Offset = bBelowLast ? RowText(OutPos.Row).Len() : Frame->OffsetAt(Geometry, Screen);
		if (OutButton != nullptr)
		{
			*OutButton = bBelowLast ? nullptr : Frame->ButtonAt(Geometry, Screen);
		}
		return OutPos.Row.IsValid();
	}

	/** A drag moved to Screen: the caret follows it, by characters, words or lines. */
	void ExtendTo(const FVector2D& Screen)
	{
		FSelPos Hit;
		if (!HitTest(Screen, /*bClamp*/ true, Hit, nullptr))
		{
			return;
		}
		if (SelUnit == ESelUnit::Char || IndexOfRow(UnitFirst.Row.Get()) == INDEX_NONE || IndexOfRow(UnitLast.Row.Get()) == INDEX_NONE)
		{
			SelCaret = Hit;
			return;
		}
		FSelPos First;
		FSelPos Last;
		UnitAround(Hit, SelUnit, First, Last);
		if (ComparePos(First, UnitFirst) < 0)
		{
			SelAnchor = UnitLast;           // dragged above the first word or line: from its end up to here
			SelCaret = First;
		}
		else
		{
			SelAnchor = UnitFirst;
			SelCaret = ComparePos(Last, UnitLast) > 0 ? Last : UnitLast;
		}
	}

	FReply CaptureReply() const
	{
		FReply Reply = FReply::Handled();
		if (Surface.IsValid())
		{
			Reply.CaptureMouse(Surface.ToSharedRef());
		}
		if (!bLive && List.IsValid())
		{
			Reply.SetUserFocus(List.ToSharedRef(), EFocusCause::Mouse);
		}
		return Reply;
	}

	// ---- IDreamOutputSelectionHost ----

	virtual bool HasTextSelection() const override
	{
		int32 A = 0;
		int32 AOffset = 0;
		int32 B = 0;
		int32 BOffset = 0;
		return SelectionRange(A, AOffset, B, BOffset) && (A != B || AOffset != BOffset);
	}

	virtual bool GetRowSelection(const FDreamOutputRow& Row, int32& OutFrom, int32& OutTo, bool& bOutPastEnd) const override
	{
		int32 A = 0;
		int32 AOffset = 0;
		int32 B = 0;
		int32 BOffset = 0;
		if (!SelectionRange(A, AOffset, B, BOffset) || (A == B && AOffset == BOffset))
		{
			return false;
		}
		const int32 Index = IndexOfRow(&Row);
		if (Index == INDEX_NONE || Index < A || Index > B)
		{
			return false;
		}
		OutFrom = Index == A ? AOffset : 0;
		OutTo = Index == B ? BOffset : MAX_int32;
		bOutPastEnd = Index < B;
		return OutTo > OutFrom || bOutPastEnd;
	}

	virtual FReply HandleSelectPress(const FPointerEvent& Event) override
	{
		const FVector2D Screen = Event.GetScreenSpacePosition();
		FSelPos Hit;
		TSharedPtr<SButton> Button;
		if (Event.IsTouchEvent() || !HitTest(Screen, /*bClamp*/ false, Hit, &Button))
		{
			return FReply::Unhandled();     // touch scrolling, the scroll bar and everything that is no row go on as before
		}
		const double Now = FPlatformTime::Seconds();
		const bool bTriple = SelClicks == 2 && Now - SelLastClickSeconds <= DreamOutputSelection::DoubleClickSeconds()
			&& FVector2D::Distance(Screen, SelLastClickPos) <= DreamOutputSelection::DragDistance();
		bSelPressing = true;
		bSelDragged = false;
		bSelShift = Event.IsShiftDown() && !bTriple;
		SelPressPos = Screen;
		SelPointerPos = Screen;
		SelPressHit = Hit;
		SelPressButton.Reset();
		if (bTriple)
		{
			SelClicks = 3;
			SelectUnit(Hit, ESelUnit::Line);
		}
		else if (bSelShift)
		{
			SelClicks = 1;
			SelUnit = ESelUnit::Char;
			if (IndexOfRow(SelAnchor.Row.Get()) == INDEX_NONE)
			{
				// No caret yet: from the start of the row the list has selected (a keyboard or Find selection), else here.
				const FDreamOutputRowPtr Selected = bLive ? nullptr : FirstSelected();
				SelAnchor = Selected.IsValid() ? FSelPos{ Selected, 0 } : Hit;
			}
			SelCaret = Hit;
		}
		else
		{
			SelClicks = 1;
			SelLastClickSeconds = Now;
			SelLastClickPos = Screen;
			SelUnit = ESelUnit::Char;
			SelAnchor = Hit;
			SelCaret = Hit;
			SelPressButton = Button;
		}
		return CaptureReply();
	}

	virtual FReply HandleSelectDoubleClick(const FPointerEvent& Event) override
	{
		const FVector2D Screen = Event.GetScreenSpacePosition();
		FSelPos Hit;
		if (!HitTest(Screen, /*bClamp*/ false, Hit, nullptr))
		{
			return FReply::Unhandled();
		}
		SelClicks = 2;
		SelLastClickSeconds = FPlatformTime::Seconds();
		SelLastClickPos = Screen;
		bSelPressing = true;
		bSelDragged = false;
		bSelShift = false;
		SelPressPos = Screen;
		SelPointerPos = Screen;
		SelPressHit = Hit;
		SelPressButton.Reset();
		SelectUnit(Hit, ESelUnit::Word);
		return CaptureReply();
	}

	virtual FReply HandleSelectMove(const FPointerEvent& Event) override
	{
		if (!bSelPressing)
		{
			return FReply::Unhandled();
		}
		SelPointerPos = Event.GetScreenSpacePosition();
		if (!bSelDragged && FVector2D::Distance(SelPointerPos, SelPressPos) >= DreamOutputSelection::DragDistance())
		{
			bSelDragged = true;
		}
		if (bSelDragged || SelUnit != ESelUnit::Char || bSelShift)
		{
			ExtendTo(SelPointerPos);
		}
		return FReply::Handled();
	}

	virtual FReply HandleSelectRelease(const FPointerEvent& Event) override
	{
		if (!bSelPressing)
		{
			return FReply::Unhandled();
		}
		bSelPressing = false;
		const bool bClick = SelUnit == ESelUnit::Char && !bSelDragged && !bSelShift;
		if (bClick)
		{
			// A click without a drag: a caret there (no characters selected), its row selected in the list as before, and the
			// link, chip or header button under it clicked (a link still navigates, a fold chip still opens).
			SelAnchor = SelPressHit;
			SelCaret = SelPressHit;
			if (!bLive && List.IsValid() && IndexOfRow(SelPressHit.Row.Get()) != INDEX_NONE)
			{
				TGuardValue<bool> Guard(bSyncingListSelection, true);
				List->SetSelection(SelPressHit.Row, ESelectInfo::OnMouseClick);
			}
		}
		else
		{
			SyncListSelection();
		}
		const TSharedPtr<SButton> Button = bClick ? SelPressButton.Pin() : nullptr;
		SelPressButton.Reset();
		if (bFollowing)
		{
			ScrollBottom();         // lines that arrived while the button was down
		}
		if (Button.IsValid() && Button->IsEnabled() && Button->GetPaintSpaceGeometry().IsUnderLocation(Event.GetScreenSpacePosition()))
		{
			Button->SimulateClick();
		}
		return FReply::Handled().ReleaseMouseCapture();
	}

	virtual void HandleSelectCaptureLost() override
	{
		if (bSelPressing)
		{
			bSelPressing = false;
			SelPressButton.Reset();
			SyncListSelection();
		}
	}

	virtual bool IsOverRows(const FVector2D& ScreenPosition) const override
	{
		bool bBelowLast = false;
		return RowWidgetAt(ScreenPosition, /*bClamp*/ false, bBelowLast).IsValid();
	}

	/** While a drag holds the pointer above or below the rows, the view scrolls that way and the selection follows. */
	void TickSelection()
	{
		if (!bSelPressing || !List.IsValid() || Rows.IsEmpty() || (!bSelDragged && SelUnit == ESelUnit::Char))
		{
			return;
		}
		const FGeometry& Geometry = List->GetPaintSpaceGeometry();
		const double Top = Geometry.GetAbsolutePosition().Y;
		const double Bottom = Top + Geometry.GetAbsoluteSize().Y;
		double Beyond = 0.0;
		if (SelPointerPos.Y < Top)
		{
			Beyond = SelPointerPos.Y - Top;
		}
		else if (SelPointerPos.Y > Bottom)
		{
			Beyond = SelPointerPos.Y - Bottom;
		}
		if (Beyond == 0.0)
		{
			return;
		}
		const float Step = FMath::Clamp(static_cast<float>(FMath::Abs(Beyond)) / 16.f, 0.25f, 4.f) * (Beyond < 0.0 ? -1.f : 1.f);
		const float Offset = FMath::Clamp(List->GetScrollOffset() + Step, 0.f, static_cast<float>(Rows.Num()));
		if (Step < 0.f)
		{
			bFollowing = false;     // toward older lines, as a wheel (OUT-09); reaching the bottom resumes following
			BottomCheckAfterTick = TickCount + 2;
		}
		SetOffset(Offset);
		ExtendTo(SelPointerPos);
	}

	void SelectAll()
	{
		if (Rows.IsEmpty())
		{
			return;
		}
		SelUnit = ESelUnit::Char;
		SetSelection(FSelPos{ Rows[0], 0 }, FSelPos{ Rows.Last(), RowText(Rows.Num() - 1).Len() }, /*bSyncList*/ true);
	}

	/** The selected characters exactly as drawn, the rows joined with '\n'. */
	FString TextSelectionText() const
	{
		int32 A = 0;
		int32 AOffset = 0;
		int32 B = 0;
		int32 BOffset = 0;
		if (!HasTextSelection() || !SelectionRange(A, AOffset, B, BOffset))
		{
			return FString();
		}
		FString Out;
		Out.Reserve((B - A + 1) * 48);
		for (int32 Index = A; Index <= B; ++Index)
		{
			const FString Text = RowText(Index);
			const int32 From = Index == A ? FMath::Clamp(AOffset, 0, Text.Len()) : 0;
			const int32 To = Index == B ? FMath::Clamp(BOffset, 0, Text.Len()) : Text.Len();
			if (Index > A)
			{
				Out.AppendChar(TEXT('\n'));
			}
			if (To > From)
			{
				Out.AppendChars(*Text + From, To - From);
			}
		}
		return Out;
	}

	FSelKey KeyOf(const FSelPos& Pos) const
	{
		FSelKey Key;
		const int32 Index = IndexOfRow(Pos.Row.Get());
		if (Index == INDEX_NONE)
		{
			return Key;
		}
		const FDreamOutputRow& Row = *Rows[Index];
		Key.Seq = Row.Seq;
		Key.Kind = Row.Kind;
		Key.Offset = Pos.Offset;
		Key.bTop = Index < TopFixedRows;
		Key.Ordinal = 0;
		for (int32 Before = Index - 1; Before >= (Key.bTop ? 0 : TopFixedRows); --Before)
		{
			if (!Key.bTop && Rows[Before]->Seq != Row.Seq)
			{
				break;
			}
			Key.Ordinal += Rows[Before]->Kind == Row.Kind ? 1 : 0;
		}
		return Key;
	}

	FSelPos PosOf(const FSelKey& Key) const
	{
		if (Key.Ordinal == INDEX_NONE)
		{
			return FSelPos();
		}
		int32 Seen = 0;
		const int32 Begin = Key.bTop ? 0 : FMath::Max(TopFixedRows, FindRowAtOrAfter(Key.Seq));
		const int32 End = Key.bTop ? FMath::Min(TopFixedRows, Rows.Num()) : Rows.Num();
		for (int32 Index = Begin; Index < End; ++Index)
		{
			if (!Key.bTop && Rows[Index]->Seq != Key.Seq)
			{
				break;
			}
			if (Rows[Index]->Kind == Key.Kind && Seen++ == Key.Ordinal)
			{
				return FSelPos{ Rows[Index], Key.Offset };
			}
		}
		return FSelPos();
	}
};

void SDreamOutputView::FImpl::OpenLink(uint64 Seq, int32 LinkIndex)
{
	dream::LineView View;
	if (!Store->GetLine(Seq, View) || LinkIndex < 0 || LinkIndex >= static_cast<int32>(View.Links.size()))
	{
		return;
	}
	const dream::LinkSpan& Link = View.Links[LinkIndex];
	const std::string_view PathUtf8 = View.Text.substr(Link.PathStart, Link.PathLen);
	const FString Path = DreamText::Utf8ToFString(PathUtf8);

	if (Link.Kind == dream::LinkKind::PseudoFile)
	{
		std::string_view Name;
		std::string_view Id;
		if (dream::ParsePseudoFilename(PathUtf8, Name, Id) && (Name == "REPL" || Name == "Console"))
		{
			// "<dream:REPL#42>, line 1": scroll Output to run #42's echo lines and highlight that line (OUT-05, OUT-12). The
			// REPL tab's view shows REPL runs only, so the output area routes the link to its Output view.
			const uint32 RunNumber = static_cast<uint32>(FCString::Atoi(*DreamText::Utf8ToFString(Id)));
			const bool bShown = OnOpenPromptLink.IsBound()
				? OnOpenPromptLink.Execute(static_cast<int32>(RunNumber), Link.Line)
				: ScrollToEchoLine(RunNumber, Link.Line);
			if (!bShown)
			{
				DreamOutputViewPrivate::ShowStatus(FText::Format(LOCTEXT("Out.RunGone", "Run #{0} is no longer in the output"), FText::AsNumber(RunNumber)));
			}
			return;
		}
	}

	// §6.4 line mapping: a link into the run's own buffer maps through the edits made since the run's snapshot.
	int32 Line = Link.Line;
	if (RunService != nullptr && FDreamEditorModule::IsAvailable())
	{
		const FDreamRunRecord* Rec = RunService->FindRecord(static_cast<int32>(View.Info.RunNumber));
		if (Rec != nullptr && Rec->Filename == Path && Line > 0)
		{
			if (const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().ResolveFilename(Path))
			{
				const dream::LineNo Mapped = Buffer->GetModel().MapLine(Rec->SourceVersion, Line - 1);
				if (Mapped != dream::InvalidLine)
				{
					Line = Mapped + 1;
				}
			}
		}
	}
	if (!FDreamEditorModule::IsAvailable() || FDreamEditorModule::Get().OpenLocation(Path, Line, Link.Col).IsEmpty())
	{
		DreamOutputViewPrivate::ShowStatus(FText::Format(LOCTEXT("Out.LinkUnresolved", "Cannot open {0}"), FText::FromString(Path)));
	}
}

void SDreamOutputView::FImpl::ExpandEcho(bool bExpand)
{
	TSet<uint32> RunsToChange;
	bool bChanged = false;
	for (const FDreamOutputRowPtr& Row : Rows)
	{
		if (!List.IsValid() || !List->IsItemSelected(Row) || Row->RunNumber == 0)
		{
			continue;
		}
		dream::LineView View;
		const bool bEchoRow = Row->Kind == EDreamOutputRowKind::FoldChip
			|| (Row->Kind == EDreamOutputRowKind::Line && Store->GetLine(Row->Seq, View) && View.Info.Type == dream::LineType::Echo);
		if (bEchoRow)
		{
			const uint64 Key = DreamOutputViewPrivate::FoldKey(Row->RunNumber, Row->Statement);
			if (bExpand)
			{
				ExpandedFolds.Add(Key);
			}
			else
			{
				ExpandedFolds.Remove(Key);
				ExpandedRuns.Remove(Row->RunNumber);
			}
			bChanged = true;
		}
		else
		{
			RunsToChange.Add(Row->RunNumber);
		}
	}
	for (const uint32 RunNumber : RunsToChange)
	{
		if (bExpand)
		{
			ExpandedRuns.Add(RunNumber);
		}
		else
		{
			ExpandedRuns.Remove(RunNumber);
			for (auto It = ExpandedFolds.CreateIterator(); It; ++It)
			{
				if (static_cast<uint32>(*It >> 32) == RunNumber)
				{
					It.RemoveCurrent();
				}
			}
		}
		bChanged = true;
	}
	if (bChanged)
	{
		RebuildKeeping(0);
	}
}

TSharedPtr<SWidget> SDreamOutputView::FImpl::MakeContextMenu()
{
	if (bLive)
	{
		return nullptr;
	}
	FMenuBuilder Menu(/*bCloseAfterSelection*/ true, nullptr);
	FillContextMenu(Menu, nullptr);
	return Menu.MakeWidget();
}

void SDreamOutputView::FImpl::FillContextMenu(FMenuBuilder& Menu, TMap<FString, FUIAction>* OutActions)
{
	const TWeakPtr<FImpl> Weak = AsShared();
	const FDreamOutputRowPtr Selected = FirstSelected();
	auto Add = [&Menu, OutActions](const FText& Label, const FText& Tip, const FUIAction& Action)
	{
		Menu.AddMenuEntry(Label, Tip, FSlateIcon(), Action);
		if (OutActions != nullptr)
		{
			OutActions->Add(Label.ToString(), Action);
		}
	};

	Menu.BeginSection(TEXT("DreamOutputCopy"), LOCTEXT("Out.CopySection", "Copy"));
	// (output, PO-04) Copy is also the old [Copy] chip of a truncated result: it copies the whole raw repr (§6.4).
	Add(LOCTEXT("Out.Copy", "Copy"), LOCTEXT("Out.CopyTip", "Copy the selected text as drawn; without one, the selected lines as raw text, a long result whole (Ctrl+C)"),
		FUIAction(FExecuteAction::CreateLambda([Weak]()
		{
			if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
			{
				FPlatformApplicationMisc::ClipboardCopy(*Pinned->SelectedText());
			}
		}), FCanExecuteAction::CreateLambda([Selected]() { return Selected.IsValid(); })));
	// The run that the run entries act on: the run of every selected row; with rows of several runs selected there is
	// none, so no entry acts on a run the click did not mean (review r6 m6).
	uint32 RunNumber = Selected.IsValid() ? Selected->RunNumber : 0;
	if (List.IsValid())
	{
		for (const FDreamOutputRowPtr& Row : List->GetSelectedItems())
		{
			if (Row.IsValid() && Row->RunNumber != RunNumber)
			{
				RunNumber = 0;
				break;
			}
		}
	}
	Add(LOCTEXT("Out.CopyRunOutput", "Copy run output"), LOCTEXT("Out.CopyRunOutputTip", "Copy every output line of this run"),
		FUIAction(FExecuteAction::CreateLambda([Weak, RunNumber]()
		{
			if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
			{
				FPlatformApplicationMisc::ClipboardCopy(*Pinned->RunOutputText(RunNumber));
			}
		}), FCanExecuteAction::CreateLambda([RunNumber]() { return RunNumber != 0; })));
	Add(LOCTEXT("Out.CopyTraceback", "Copy traceback"), LOCTEXT("Out.CopyTracebackTip", "Copy the error block around this line"),
		FUIAction(FExecuteAction::CreateLambda([Weak, Selected]()
		{
			if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
			{
				FPlatformApplicationMisc::ClipboardCopy(*Pinned->TracebackText(Selected));
			}
		}), FCanExecuteAction::CreateLambda([Weak, Selected]()
		{
			const TSharedPtr<FImpl> Pinned = Weak.Pin();
			return Pinned.IsValid() && !Pinned->TracebackText(Selected).IsEmpty();
		})));
	// Deferred (M2): Copy as script (OUT-10).
	Menu.EndSection();

	// (output, PO-04) The run's actions, on any line of a run: the footer's chips before PO-04 (§6.4 footer, U9, RT-08,
	// RUN-12, PER-09). They stay here with run headers on too; the footer is one muted line.
	const FDreamRunRecord* Record = RunService != nullptr && RunNumber != 0 ? RunService->FindRecord(static_cast<int32>(RunNumber)) : nullptr;
	if (Record != nullptr)
	{
		Menu.BeginSection(TEXT("DreamOutputRun"), FText::Format(LOCTEXT("Out.RunSection", "Run #{0}"), FText::AsNumber(RunNumber)));
		// The active run cannot be run again until it ends (review r6 m4): the entry shows, disabled, and says why.
		const bool bEnded = Record->Outcome != EDreamRunOutcome::None;
		Add(LOCTEXT("Out.Rerun", "Re-run"), bEnded ? LOCTEXT("Out.RerunTip", "Run the same code again as a new numbered run (RUN-12)")
				: LOCTEXT("Out.RerunRunningTip", "This run is still running"),
			FUIAction(FExecuteAction::CreateLambda([Weak, RunNumber]()
			{
				if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
				{
					Pinned->Rerun(RunNumber);
				}
			}), FCanExecuteAction::CreateLambda([bEnded]() { return bEnded; })));
		if (RunService->CanRerunInTransaction(static_cast<int32>(RunNumber)))
		{
			// (run group, B38) A run that executed without a transaction (RT-08: Run File's default is off; the U6 override or
			// a toggle turned off): a re-run of the same snapshot with the transaction override on.
			Add(LOCTEXT("Out.RunInTransaction", "Run in transaction"),
				LOCTEXT("Out.RunInTransactionTip", "Run the same code again as a new numbered run with one undo step (RT-08)"),
				FUIAction(FExecuteAction::CreateLambda([Weak, RunNumber]()
				{
					if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
					{
						Pinned->RerunInTransaction(RunNumber);
					}
				})));
		}
		if (DreamOutputRows::CanOfferUndo(*Record))
		{
			// A Cleared run offers nothing (§6.4); Off, Empty and Undone have nothing to undo.
			FText Reason;
			FDreamUndoPreview Preview;
			const bool bCanUndo = RunService->CanUndoRun(static_cast<int32>(RunNumber), Reason, Preview);
			const FText Tip = !bCanUndo ? Reason
				: Preview.bPartial ? LOCTEXT("Out.UndoPartialTip", "Only part of this run was recorded; choose it again to confirm")
				: FText::Format(LOCTEXT("Out.UndoTip", "Undo \"{0}\""), Preview.Title);
			Add(IsUndoConfirmPending(RunNumber) ? LOCTEXT("Out.UndoConfirm", "Confirm: undo the recorded part") : LOCTEXT("Out.UndoThisRun", "Undo this run"),
				Tip, FUIAction(FExecuteAction::CreateLambda([Weak, RunNumber]()
				{
					if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
					{
						Pinned->UndoRun(RunNumber);
					}
				}), FCanExecuteAction::CreateLambda([bCanUndo]() { return bCanUndo; })));
		}
		if (DreamOutputRows::IsSnapshotFailure(*Record))
		{
			// §10.2.5 step 4, PER-09: "Journal snapshot failed: <reason>" offers the run again with the snapshot step skipped
			// (FDreamAudit writes run.snapshot_skipped, §11.4); (instances, B23) Open storage settings beside it.
			Add(LOCTEXT("Out.RunWithoutSnapshot", "Run without snapshot"),
				LOCTEXT("Out.RunWithoutSnapshotTip", "Run the same code again as a new numbered run, skipping the pre-run journal snapshot (PER-09)"),
				FUIAction(FExecuteAction::CreateLambda([Weak, RunNumber]()
				{
					if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
					{
						Pinned->RerunWithoutSnapshot(RunNumber);
					}
				})));
			Add(LOCTEXT("Out.OpenStorageSettings", "Open storage settings"),
				LOCTEXT("Out.OpenStorageSettingsTip", "Editor Preferences > Plugins > Dream Python Editor: the storage folder override (storage.root_override), used from the next start"),
				FUIAction(FExecuteAction::CreateLambda([]() { FDreamInstances::OpenStorageSettings(); })));
		}
		// Deferred (M2): [Inspect], the post-mortem inspector (§8.7).
		Menu.EndSection();
	}

	Menu.BeginSection(TEXT("DreamOutputEcho"), LOCTEXT("Out.EchoSection", "Input echo"));
	Add(LOCTEXT("Out.ExpandEcho", "Expand Echo"), LOCTEXT("Out.ExpandEchoTip", "Show every echo line of the selected statement or runs"),
		FUIAction(FExecuteAction::CreateLambda([Weak]()
		{
			if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
			{
				Pinned->ExpandEcho(true);
			}
		})));
	Add(LOCTEXT("Out.FoldEcho", "Fold Echo"), LOCTEXT("Out.FoldEchoTip", "Fold the echo of the selected statement or runs again"),
		FUIAction(FExecuteAction::CreateLambda([Weak]()
		{
			if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
			{
				Pinned->ExpandEcho(false);
			}
		})));
	Menu.EndSection();

	Menu.BeginSection(TEXT("DreamOutputBlocks"), LOCTEXT("Out.BlocksSection", "Output"));
	// OUT-03 (§6.4 "Design: filters"): one toggle per type, plus background.
	Menu.AddSubMenu(LOCTEXT("Out.ShowMenu", "Show"), LOCTEXT("Out.ShowMenuTip", "Show or hide output lines by type"),
		FNewMenuDelegate::CreateLambda([Target = CommandTarget](FMenuBuilder& SubMenu)
		{
			SDreamOutputView::AddShowMenuEntries(SubMenu, Target);
		}));
	if (RunNumber != 0)
	{
		Add(Collapsed.Contains(RunNumber) ? LOCTEXT("Out.ExpandRun", "Expand run block") : LOCTEXT("Out.CollapseRun", "Collapse run block"),
			FText::GetEmpty(), FUIAction(FExecuteAction::CreateLambda([Weak, RunNumber]()
			{
				if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
				{
					Pinned->ToggleRunCollapsed(RunNumber);
				}
			})));
	}
	Add(LOCTEXT("Out.ClearOutput", "Clear output"), LOCTEXT("Out.ClearOutputTip", "Clear the output view; run logs and run history stay"),
		FUIAction(FExecuteAction::CreateLambda([Weak]()
		{
			if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
			{
				Pinned->Store->Clear();
			}
		})));
	Menu.EndSection();
}

FReply SDreamOutputView::FImpl::HandleListKey(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FKey Key = KeyEvent.GetKey();
	const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
	// OUT-09, A11Y-01: list navigation scrolls at the list's next tick (SListView::NavigationSelect -> RequestScrollIntoView),
	// outside Slate input, so HandleScrolled cannot see it as the user's. A key toward older lines leaves follow mode here;
	// TickFollow resumes it two ticks later if the view is still at the bottom. Down and PageDown step from the list's
	// selector row (row 0 without one, SListView OnNavigation and OnKeyDown_Internal): when the row they reach lies above
	// the first fully shown row (a selection the stream scrolled away), they scroll toward older lines too (P3-out-03).
	// The shown row is the one the list last laid out, not the desired offset a drain's ScrollBottom left unclamped after
	// it (the row count until the list's next tick), and a partly shown top row counts as above: the list's navigation
	// scrolls up to show it whole (SListView ScrollIntoView, P4-shl-01).
	if (bFollowing && !Mods.IsAltDown() && (Key == EKeys::Up || Key == EKeys::PageUp || Key == EKeys::Home))
	{
		bFollowing = false;
		BottomCheckAfterTick = TickCount + 2;
	}
	else if (bFollowing && !Mods.IsAltDown() && (Key == EKeys::Down || Key == EKeys::PageDown) && List.IsValid() && Rows.Num() > 0)
	{
		const int32 Selector = Rows.IndexOfByPredicate([this](const FDreamOutputRowPtr& Row) { return List->Private_HasSelectorFocus(Row); });
		const int32 Page = FMath::Max(1, List->GetNumGeneratedChildren());     // about GetNumLiveWidgets (protected)
		const int32 Target = Key == EKeys::Down ? Selector + 1 : FMath::Min(FMath::Max(Selector, 0) + Page, Rows.Num() - 1);
		const float Shown = List->IsPendingRefresh() ? ShownOffset : List->GetScrollOffset();
		if (Target < FMath::CeilToInt(Shown))
		{
			bFollowing = false;
			BottomCheckAfterTick = TickCount + 2;
		}
	}
	// Find (§6.4 widget). (output choice) Ctrl+F, F3 / Shift+F3 and Esc are hard-wired here, like the REPL prompt keys:
	// the keymap's Find commands are code-view commands, unbound in dream::Ctx_Output (FDreamCommands::IsCodeViewCommand).
	const bool bNoCtrlAlt = !Mods.IsControlDown() && !Mods.IsAltDown() && !Mods.IsCommandDown();
	if (!bLive && Key == EKeys::F && Mods.IsControlDown() && !Mods.IsAltDown() && !Mods.IsShiftDown())
	{
		if (const TSharedPtr<SDreamOutputView> Pinned = Owner.Pin())
		{
			Pinned->OpenFind();
		}
		return FReply::Handled();
	}
	if (bFindOpen && Key == EKeys::F3 && bNoCtrlAlt)
	{
		StepFind(Mods.IsShiftDown() ? -1 : 1);
		return FReply::Handled();
	}
	if (bFindOpen && Key == EKeys::Escape && bNoCtrlAlt && !Mods.IsShiftDown())
	{
		if (const TSharedPtr<SDreamOutputView> Pinned = Owner.Pin())
		{
			Pinned->CloseFind();
		}
		return FReply::Handled();
	}
	if (Key == EKeys::C && Mods.IsControlDown() && !Mods.IsAltDown())
	{
		FPlatformApplicationMisc::ClipboardCopy(*SelectedText());
		return FReply::Handled();
	}
	if (!bLive && Key == EKeys::A && Mods.IsControlDown() && !Mods.IsAltDown() && !Mods.IsShiftDown())
	{
		SelectAll();                // (output, PO-06 F3) every shown row's text, not the list's select-all of rows
		return FReply::Handled();
	}
	if (Key == EKeys::Enter && !Mods.IsControlDown() && !Mods.IsAltDown() && !Mods.IsShiftDown())
	{
		// A11Y-01: Enter on a chip line expands it; on a header it toggles the block; on a link line it opens the link.
		if (const FDreamOutputRowPtr Row = FirstSelected())
		{
			switch (Row->Kind)
			{
			case EDreamOutputRowKind::FoldChip:
				ExpandFold(Row->RunNumber, Row->Statement);
				return FReply::Handled();
			case EDreamOutputRowKind::Header:
			case EDreamOutputRowKind::SyntheticHeader:
				ToggleRunCollapsed(Row->RunNumber);
				return FReply::Handled();
			case EDreamOutputRowKind::StartupHeader:
				ToggleStartupExpanded();
				return FReply::Handled();
			case EDreamOutputRowKind::Line:
			{
				dream::LineView View;
				if (Store->GetLine(Row->Seq, View) && !View.Links.empty())
				{
					OpenLink(Row->Seq, 0);
					return FReply::Handled();
				}
				break;
			}
			default:
				break;
			}
		}
	}
	// Bound Dream chords (F4, Shift+F5, Ctrl+Alt+Z, ...) while the output has focus (§4.9.4).
	if (const TSharedPtr<IDreamCommandTarget> Target = CommandTarget.Pin())
	{
		uint32 Context = dream::Ctx_Focused | dream::Ctx_Output;
		if (RunService != nullptr && RunService->IsRunActive())
		{
			Context |= dream::Ctx_RunActive;
		}
		const FReply Reply = FDreamKeyRouter::Get().RoutePre(KeyEvent, Context, *Target);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	return FReply::Unhandled();
}

// ---------------------------------------------------------------------------------------------------------------------

void SDreamOutputView::Construct(const FArguments& InArgs)
{
	Impl = MakeShared<FImpl>();
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	Impl->Store = &Module.GetOutputStore();
	Impl->RunService = &Module.GetRunService();
	Impl->RouteFilter = InArgs._RouteFilter;
	Impl->RunFilter = InArgs._RunFilter;
	Impl->bLive = InArgs._bLiveRunView;
	Impl->CommandTarget = InArgs._CommandTarget;
	Impl->OnOpenPromptLink = InArgs._OnOpenPromptLink;
	Impl->Owner = SharedThis(this);

	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	Impl->Font = FDreamEditorStyle::GetCodeFont(Settings ? Settings->FontSize : 10);
	Impl->LinkStyle = FAppStyle::GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText"));
	Impl->LinkStyle.SetFont(Impl->Font);
	Impl->LinkStyle.SetColorAndOpacity(FDreamEditorStyle::Get().GetSlateColor(DreamStyleKeys::OutputLink));

	const TWeakPtr<FImpl> Weak = Impl;
	const TWeakPtr<IDreamOutputSelectionHost> WeakSelection = Impl;
	Impl->bWrapApplied = DreamOutputViewPrivate::SoftWrapFlag();
	DreamOutputViewPrivate::RefreshRowStyle();
	// Find (§6.4 widget): the code view's find bar without its replace row, above the rows. (output choice) It is in the
	// widget tree only while open (FindHost), so a closed view's tree is the rows alone, as before Find existed.
	if (!Impl->bLive)
	{
		SAssignNew(Impl->FindBar, SDreamFindBar)
			.OnQueryChanged_Lambda([Weak]()
			{
				if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
				{
					Pinned->ApplyFindQueryFromBar();
				}
			})
			.OnStep_Lambda([Weak](int32 Direction, bool bReturnFocus)
			{
				const TSharedPtr<FImpl> Pinned = Weak.Pin();
				if (!Pinned.IsValid())
				{
					return;
				}
				Pinned->StepFind(Direction);
				if (bReturnFocus && Pinned->List.IsValid())
				{
					FSlateApplication::Get().SetKeyboardFocus(Pinned->List, EFocusCause::SetDirectly);
				}
			})
			.OnClose_Lambda([Weak]()
			{
				const TSharedPtr<FImpl> Pinned = Weak.Pin();
				if (const TSharedPtr<SDreamOutputView> View = Pinned.IsValid() ? Pinned->Owner.Pin() : nullptr)
				{
					View->CloseFind();
				}
			})
			.StatusText_Lambda([Weak]()
			{
				const TSharedPtr<FImpl> Pinned = Weak.Pin();
				return Pinned.IsValid() ? Pinned->FindStatus() : FText::GetEmpty();
			})
			.IsStatusError_Lambda([Weak]()
			{
				const TSharedPtr<FImpl> Pinned = Weak.Pin();
				return Pinned.IsValid() && !Pinned->FindError.IsEmpty();
			})
			.CanReplace(false);
	}
	ChildSlot
	[
		// (output, PO-04) The history pane in the theme's output background (the Run window's live view too).
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::OutputBackground))
		.Padding(0.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(Impl->FindHost, SBox)
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					// (output, PO-06 F3) Left presses on rows select text before the rows' links and buttons see them.
					SAssignNew(Impl->Surface, SDreamOutputSurface, WeakSelection)
					[
						SAssignNew(Impl->List, DreamOutputViewPrivate::FRowList)
						.ListViewStyle(&FDreamThemes::ListViewStyle())   // (theme) no dark list background under a light theme
						.ScrollBarStyle(&FDreamThemes::ScrollBarStyle())   // (theme)
						.ListItemsSource(&Impl->Rows)
						.SelectionMode(Impl->bLive ? ESelectionMode::None : ESelectionMode::Multi)
						.IsFocusable(!Impl->bLive)
						.ConsumeMouseWheel(EConsumeMouseWheel::Always)
						.OnGenerateRow_Lambda([Weak](FDreamOutputRowPtr Item, const TSharedRef<STableViewBase>& Owner) -> TSharedRef<ITableRow>
						{
							const TSharedPtr<FImpl> Pinned = Weak.Pin();
							TSharedRef<SWidget> Content = Pinned.IsValid() ? DreamOutputRows::MakeRowWidget(Item, Pinned.ToSharedRef())
								: StaticCastSharedRef<SWidget>(SNew(STextBlock));
							// (output, PO-06 F3) The row's widget in a frame that maps and paints its text selection.
							const TWeakPtr<IDreamOutputSelectionHost> SelectionHost = Pinned;
							return SNew(SDreamOutputTableRow, Owner, SNew(SDreamOutputRowFrame, Item, SelectionHost)[Content], SelectionHost)
								.Style(&DreamOutputViewPrivate::RowStyle())
								.ShowSelection(Pinned.IsValid() && !Pinned->bLive);
						})
						.OnListViewScrolled_Lambda([Weak](double Offset)
						{
							if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
							{
								Pinned->HandleScrolled(Offset);
							}
						})
						.OnContextMenuOpening_Lambda([Weak]() -> TSharedPtr<SWidget>
						{
							const TSharedPtr<FImpl> Pinned = Weak.Pin();
							return Pinned.IsValid() ? Pinned->MakeContextMenu() : nullptr;
						})
						// (output, PO-06 F3) A double-click selects a word (SDreamOutputTableRow); a click on a link opens it.
						.OnSelectionChanged_Lambda([Weak](FDreamOutputRowPtr, ESelectInfo::Type Info)
						{
							// The reader's own row selection (keys, a right-click on another row) replaces the text selection; the
							// list's pruning of rows that went away (Direct) and our own syncing do not.
							const TSharedPtr<FImpl> Pinned = Weak.Pin();
							if (Pinned.IsValid() && !Pinned->bSyncingListSelection && Info != ESelectInfo::Direct)
							{
								Pinned->ClearSelectionState();
							}
						})
						.OnKeyDownHandler_Lambda([Weak](const FGeometry& Geometry, const FKeyEvent& KeyEvent)
						{
							const TSharedPtr<FImpl> Pinned = Weak.Pin();
							return Pinned.IsValid() ? Pinned->HandleListKey(Geometry, KeyEvent) : FReply::Unhandled();
						})
					]
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Bottom)
				.Padding(FMargin(0.f, 0.f, 18.f, 6.f))
				[
					// OUT-09: "↓ n new lines" while not following; a click jumps to the newest line (passed in the Run window, §5.6.2).
					// (output, review r6 m7) A flat chip in the theme: ChromeBackground with a ChromeBorder outline, ChromeText.
					SNew(SButton)
					.ButtonStyle(&FDreamThemes::FlatButtonStyle())
					.ContentPadding(FMargin(0.f))
					.IsFocusable(false)
					.Visibility_Lambda([Weak]()
					{
						const TSharedPtr<FImpl> Pinned = Weak.Pin();
						return Pinned.IsValid() && !Pinned->bFollowing && Pinned->NewLinesBelow > 0 ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([this]()
					{
						ScrollToEnd();
						return FReply::Handled();
					})
					[
						SNew(SBorder)
						.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBorder))
						.Padding(FMargin(1.f))
						[
							SNew(SBorder)
							.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground))
							.Padding(FMargin(8.f, 2.f))
							[
								SNew(STextBlock)
								.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
								.Text_Lambda([Weak]()
								{
									const TSharedPtr<FImpl> Pinned = Weak.Pin();
									return FText::Format(LOCTEXT("Out.NewLinesChip", "↓ {0} new lines"), FText::AsNumber(Pinned.IsValid() ? Pinned->NewLinesBelow : 0));
								})
							]
						]
					]
				]
			]
		]
	];

	Impl->AppendedHandle = Impl->Store->OnLinesAppended().AddLambda([Weak](const FDreamAppendInfo& Info)
	{
		if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
		{
			Pinned->HandleAppended(Info);
		}
	});
	Impl->VisibilityHandle = Impl->Store->OnVisibilityChanged().AddLambda([Weak]()
	{
		if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
		{
			Pinned->RebuildKeeping(0);
			if (Pinned->FindQuery)
			{
				Pinned->RefilterFind();      // a hidden type's matches leave the count (§6.4 filters)
			}
		}
	});
	Impl->RunFinishedHandle = Impl->RunService->OnRunFinished().AddLambda([Weak](const FDreamRunRecord& Record)
	{
		if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
		{
			Pinned->HandleRunFinished(Record);
		}
	});
	// (output, PO-04) View ▸ Run headers rebuilds the rows; a theme switch makes the shown rows again in its colours.
	Impl->ViewSettingsHandle = UDreamUserSettings::OnViewSettingsChanged().AddLambda([Weak]()
	{
		if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
		{
			Pinned->HandleViewSettingsChanged();
		}
	});
	Impl->ThemeHandle = FDreamThemes::OnChanged().AddLambda([Weak]()
	{
		if (const TSharedPtr<FImpl> Pinned = Weak.Pin())
		{
			Pinned->HandleThemeChanged();
		}
	});

	Impl->Rebuild();
	Impl->ScrollBottom();
}

SDreamOutputView::~SDreamOutputView()
{
	if (Impl.IsValid() && Impl->FindCancel)
	{
		Impl->FindCancel->Cancel();     // a pass in flight owns its snapshot and query; it ends at its next line
	}
	if (Impl.IsValid())
	{
		UDreamUserSettings::OnViewSettingsChanged().Remove(Impl->ViewSettingsHandle);
		FDreamThemes::OnChanged().Remove(Impl->ThemeHandle);
	}
	if (Impl.IsValid() && FDreamEditorModule::IsAvailable())
	{
		Impl->Store->OnLinesAppended().Remove(Impl->AppendedHandle);
		Impl->Store->OnVisibilityChanged().Remove(Impl->VisibilityHandle);
		Impl->RunService->OnRunFinished().Remove(Impl->RunFinishedHandle);
	}
}

bool SDreamOutputView::IsFollowing() const
{
	return Impl->bFollowing;
}

bool SDreamOutputView::IsFocusList(const SWidget& Widget) const
{
	return Impl->List.IsValid() && Impl->List.Get() == &Widget;
}

void SDreamOutputView::ScrollToEnd()
{
	Impl->bFollowing = true;
	Impl->NewLinesBelow = 0;
	Impl->ScrollBottom();
}

int32 SDreamOutputView::GetNewLinesBelow() const
{
	return Impl->NewLinesBelow;
}

bool SDreamOutputView::ScrollToRun(int32 RunNumber)
{
	return RunNumber > 0 && Impl->ScrollToRun(static_cast<uint32>(RunNumber));
}

bool SDreamOutputView::ScrollToRunError(int32 RunNumber)
{
	if (RunNumber <= 0)
	{
		return false;
	}
	FImpl& State = *Impl;
	if (State.Collapsed.Remove(static_cast<uint32>(RunNumber)) > 0)
	{
		State.Rebuild();
	}
	// U14, §11.6.2: the traceback, not the first Error-type line: unreal.log_error lines are Error lines too (A2-02). The
	// run's last "Traceback (most recent call last):" line, else the first line of its last block of Error lines (the
	// runner writes the traceback just before the footer).
	dream::LineView View;
	FDreamOutputRowPtr Traceback;
	FDreamOutputRowPtr LastBlockStart;
	bool bPreviousWasError = false;
	for (int32 Index = State.TopFixedRows; Index < State.Rows.Num(); ++Index)
	{
		const FDreamOutputRowPtr& Row = State.Rows[Index];
		if (Row->RunNumber != static_cast<uint32>(RunNumber) || Row->Kind != EDreamOutputRowKind::Line || !State.Store->GetLine(Row->Seq, View))
		{
			continue;
		}
		const bool bError = View.Info.Type == dream::LineType::Error;
		if (bError && !bPreviousWasError)
		{
			LastBlockStart = Row;
		}
		if (bError && View.Text.starts_with("Traceback (most recent call last):"))
		{
			Traceback = Row;
		}
		bPreviousWasError = bError;
	}
	if (const FDreamOutputRowPtr Target = Traceback.IsValid() ? Traceback : LastBlockStart)
	{
		State.FocusRow(Target);
		return true;
	}
	return false;
}

bool SDreamOutputView::ScrollToEchoLine(int32 RunNumber, int32 Line)
{
	return RunNumber > 0 && Impl->ScrollToEchoLine(static_cast<uint32>(RunNumber), Line);
}

bool SDreamOutputView::WalkErrors(int32 Direction, FText& OutStatus)
{
	return Impl->WalkErrors(Direction, OutStatus);
}

FDreamOutputViewHandoff SDreamOutputView::CaptureHandoff() const
{
	FDreamOutputViewHandoff Handoff;
	Handoff.bFollowing = Impl->bFollowing;
	const TArray<FDreamOutputRowPtr>& Rows = Impl->Rows;
	if (!Impl->List.IsValid() || Rows.IsEmpty())
	{
		return Handoff;
	}
	Handoff.ExpandedFolds = Impl->ExpandedFolds.Array();
	// (output, PO-06 F3) The live view's text selection, when both ends are on stored lines.
	if (Impl->HasTextSelection() && Impl->SelAnchor.Row->Kind == EDreamOutputRowKind::Line && Impl->SelCaret.Row->Kind == EDreamOutputRowKind::Line)
	{
		Handoff.SelectionAnchorSeq = Impl->SelAnchor.Row->Seq;
		Handoff.SelectionAnchorOffset = Impl->SelAnchor.Offset;
		Handoff.SelectionCaretSeq = Impl->SelCaret.Row->Seq;
		Handoff.SelectionCaretOffset = Impl->SelCaret.Offset;
	}
	const int32 First = FMath::Clamp(FMath::FloorToInt(Impl->List->GetScrollOffset()), 0, Rows.Num() - 1);
	Handoff.FirstVisibleSeq = Rows[First]->Seq;
	Handoff.FirstVisibleScreenY = GetCachedGeometry().GetAbsolutePosition().Y;
	if (const TSharedPtr<ITableRow> RowWidget = Impl->List->WidgetFromItem(Rows[First]))
	{
		Handoff.FirstVisibleScreenY = RowWidget->AsWidget()->GetCachedGeometry().GetAbsolutePosition().Y;
	}
	return Handoff;
}

void SDreamOutputView::ApplyHandoff(const FDreamOutputViewHandoff& Handoff)
{
	// §5.5.3 hand-off: following stays following; otherwise the line the reader was on stays where it was on screen,
	// or at the nearest visible row when that place is outside this view. Folds opened in the live view stay open, so the
	// docked view shows the same block (PO-03).
	bool bNewFold = false;
	for (const uint64 Key : Handoff.ExpandedFolds)
	{
		bool bAlready = false;
		Impl->ExpandedFolds.Add(Key, &bAlready);
		bNewFold |= !bAlready;
	}
	// (output, PO-06 F3) The live view's text selection, on the same stored lines (their drawn text is the same here).
	auto ApplySelection = [this, &Handoff]()
	{
		const FDreamOutputRowPtr Anchor = Handoff.SelectionAnchorSeq != 0 ? Impl->FindRow(Handoff.SelectionAnchorSeq, EDreamOutputRowKind::Line) : nullptr;
		const FDreamOutputRowPtr Caret = Handoff.SelectionCaretSeq != 0 ? Impl->FindRow(Handoff.SelectionCaretSeq, EDreamOutputRowKind::Line) : nullptr;
		if (Anchor.IsValid() && Caret.IsValid())
		{
			Impl->SelUnit = FImpl::ESelUnit::Char;
			Impl->SetSelection(FImpl::FSelPos{ Anchor, Handoff.SelectionAnchorOffset }, FImpl::FSelPos{ Caret, Handoff.SelectionCaretOffset },
				/*bSyncList*/ true);
		}
	};
	if (Handoff.bFollowing)
	{
		if (bNewFold)
		{
			Impl->Rebuild();
		}
		ScrollToEnd();
		ApplySelection();
		return;
	}
	Impl->bFollowing = false;
	if (bNewFold)
	{
		Impl->Rebuild();
	}
	if (!Impl->List.IsValid() || Impl->Rows.IsEmpty())
	{
		return;
	}
	const int32 Row = Impl->FindRowAtOrAfter(Handoff.FirstVisibleSeq);
	const FGeometry& Geometry = GetCachedGeometry();
	const float Scale = FMath::Max(0.01f, Geometry.Scale);
	const float Top = Geometry.GetAbsolutePosition().Y;
	const float Height = Geometry.GetAbsoluteSize().Y;
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const float RowHeight = FMath::Max(1.f, static_cast<float>(Measure->GetMaxCharacterHeight(Impl->Font, Scale)));
	float Dy = Height > 0.f ? Handoff.FirstVisibleScreenY - Top : 0.f;
	Dy = FMath::Clamp(Dy, 0.f, FMath::Max(0.f, Height - RowHeight));
	Impl->BottomCheckAfterTick = Impl->TickCount + 2;
	Impl->SetOffset(FMath::Max(0.f, static_cast<float>(Row) - Dy / RowHeight));
	ApplySelection();
}

void SDreamOutputView::RebuildVisibleIndex()
{
	Impl->RebuildKeeping(0);
}

FString SDreamOutputView::GetSelectedText() const
{
	return Impl->SelectedText();
}

bool SDreamOutputView::HasTextSelection() const
{
	return Impl->HasTextSelection();
}

void SDreamOutputView::SelectAllText()
{
	Impl->SelectAll();
}

void SDreamOutputView::ClearTextSelection()
{
	Impl->ClearSelectionState();
}

TArray<uint64> SDreamOutputView::GetLineSeqsForTest(int32 RunNumber) const
{
	TArray<uint64> Seqs;
	for (const FDreamOutputRowPtr& Row : Impl->Rows)
	{
		if (Row->Kind == EDreamOutputRowKind::Line && Row->RunNumber == static_cast<uint32>(RunNumber))
		{
			Seqs.Add(Row->Seq);
		}
	}
	return Seqs;
}

void SDreamOutputView::ResetSelectionForTest()
{
	Impl->ClearSelectionState();
	if (Impl->List.IsValid())
	{
		Impl->List->ClearSelection();
	}
	Impl->RebuildKeeping(0);    // new row objects: the list's selector item is in no row any more (SListView::Find)
}

int32 SDreamOutputView::GetFindPassesForTest() const
{
	return Impl->FindPassesForTest;
}

int32 SDreamOutputView::GetRowRebuildsForTest() const
{
	return Impl->RowRebuildsForTest;
}

void SDreamOutputView::GetRowsForTest(int32 RunNumber, TArray<EDreamOutputRowKind>& OutKinds, TArray<FString>& OutTexts,
	TArray<uint64>& OutSeqs) const
{
	OutKinds.Reset();
	OutTexts.Reset();
	OutSeqs.Reset();
	for (int32 Index = Impl->TopFixedRows; Index < Impl->Rows.Num(); ++Index)
	{
		const FDreamOutputRow& Row = *Impl->Rows[Index];
		if (Row.RunNumber != static_cast<uint32>(RunNumber))
		{
			continue;
		}
		OutKinds.Add(Row.Kind);
		OutTexts.Add(DreamOutputRows::GetRowDisplayText(Row, *Impl));
		OutSeqs.Add(Row.Kind == EDreamOutputRowKind::Line ? Row.Seq : 0);
	}
}

TArray<TSharedRef<SWidget>> SDreamOutputView::GetShownRowWidgetsForTest() const
{
	TArray<TSharedRef<SWidget>> Widgets;
	if (Impl->List.IsValid())
	{
		for (const FDreamOutputRowPtr& Row : Impl->Rows)
		{
			if (const TSharedPtr<ITableRow> Widget = Impl->List->WidgetFromItem(Row))
			{
				Widgets.Add(Widget->AsWidget());
			}
		}
	}
	return Widgets;
}

namespace DreamOutputViewPrivate
{
	/** A run's row Index in the order of GetRowsForTest, or null. */
	FDreamOutputRowPtr RunRow(const TArray<FDreamOutputRowPtr>& Rows, int32 TopFixedRows, int32 RunNumber, int32 Index)
	{
		int32 Seen = 0;
		for (int32 Row = TopFixedRows; Row < Rows.Num(); ++Row)
		{
			if (Rows[Row]->RunNumber == static_cast<uint32>(RunNumber) && Seen++ == Index)
			{
				return Rows[Row];
			}
		}
		return nullptr;
	}
}

TSharedPtr<SWidget> SDreamOutputView::GetRunRowWidgetForTest(int32 RunNumber, int32 Index) const
{
	const FDreamOutputRowPtr Row = DreamOutputViewPrivate::RunRow(Impl->Rows, Impl->TopFixedRows, RunNumber, Index);
	const TSharedPtr<ITableRow> Widget = Row.IsValid() && Impl->List.IsValid() ? Impl->List->WidgetFromItem(Row) : nullptr;
	return Widget.IsValid() ? Widget->AsWidget() : TSharedPtr<SWidget>();
}

TArray<FSlateRect> SDreamOutputView::GetSelectionRectsForTest(int32 RunNumber, int32 Index) const
{
	TArray<FSlateRect> Rects;
	const FDreamOutputRowPtr Row = DreamOutputViewPrivate::RunRow(Impl->Rows, Impl->TopFixedRows, RunNumber, Index);
	const TSharedPtr<ITableRow> Widget = Row.IsValid() && Impl->List.IsValid() ? Impl->List->WidgetFromItem(Row) : nullptr;
	const TSharedPtr<SDreamOutputRowFrame> Frame = Widget.IsValid() ? StaticCastSharedPtr<SDreamOutputTableRow>(Widget)->GetFrame() : nullptr;
	int32 From = 0;
	int32 To = 0;
	bool bPastEnd = false;
	if (!Frame.IsValid() || !Impl->GetRowSelection(*Row, From, To, bPastEnd))
	{
		return Rects;
	}
	const FGeometry& Geometry = Frame->GetPaintSpaceGeometry();
	Frame->RangeRects(Geometry, From, To, bPastEnd, Rects);
	for (FSlateRect& Rect : Rects)
	{
		Rect = FSlateRect(FVector2D(Geometry.LocalToAbsolute(Rect.GetTopLeft())), FVector2D(Geometry.LocalToAbsolute(Rect.GetBottomRight())));
	}
	return Rects;
}

bool SDreamOutputView::SelectRunRowForTest(int32 RunNumber, int32 Index)
{
	const FDreamOutputRowPtr Row = DreamOutputViewPrivate::RunRow(Impl->Rows, Impl->TopFixedRows, RunNumber, Index);
	if (!Row.IsValid() || !Impl->List.IsValid())
	{
		return false;
	}
	Impl->List->ClearSelection();
	Impl->FocusRow(Row);
	return true;
}

TArray<FString> SDreamOutputView::GetContextMenuLabelsForTest() const
{
	TMap<FString, FUIAction> Actions;
	FMenuBuilder Menu(/*bCloseAfterSelection*/ true, nullptr);
	Impl->FillContextMenu(Menu, &Actions);
	TArray<FString> Labels;
	Actions.GetKeys(Labels);
	return Labels;
}

bool SDreamOutputView::InvokeContextMenuEntryForTest(const FString& Label)
{
	TMap<FString, FUIAction> Actions;
	FMenuBuilder Menu(/*bCloseAfterSelection*/ true, nullptr);
	Impl->FillContextMenu(Menu, &Actions);
	const FUIAction* Action = Actions.Find(Label);
	if (Action == nullptr || !Action->CanExecute())
	{
		return false;
	}
	return Action->Execute();
}

float SDreamOutputView::GetRowHeightForTest(uint64 Seq) const
{
	const FDreamOutputRowPtr Row = Impl->FindLineRow(Seq);
	const TSharedPtr<ITableRow> Widget = Row.IsValid() && Impl->List.IsValid() ? Impl->List->WidgetFromItem(Row) : nullptr;
	return Widget.IsValid() ? static_cast<float>(Widget->AsWidget()->GetCachedGeometry().GetLocalSize().Y) : 0.f;
}

int32 SDreamOutputView::GetSelectedHeaderRunForTest() const
{
	const FDreamOutputRowPtr Row = Impl->FirstSelected();
	return Row.IsValid() && (Row->Kind == EDreamOutputRowKind::Header || Row->Kind == EDreamOutputRowKind::SyntheticHeader)
		? static_cast<int32>(Row->RunNumber) : 0;
}

bool SDreamOutputView::SupportsKeyboardFocus() const
{
	return false;       // the list inside takes focus (none in the live run view, PO-03)
}

FReply SDreamOutputView::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (Impl->bLive)
	{
		return FReply::Unhandled();
	}
	// Ctrl+F from the find box (its keys bubble here): select the box's text again.
	const FModifierKeysState& Mods = InKeyEvent.GetModifierKeys();
	if (InKeyEvent.GetKey() == EKeys::F && Mods.IsControlDown() && !Mods.IsAltDown() && !Mods.IsShiftDown())
	{
		OpenFind();
		return FReply::Handled();
	}
	// The consume list for what the list left unhandled, so Dream-focused keys never reach the Level Editor (D8).
	return FDreamKeyRouter::Get().RouteBubble(InKeyEvent, dream::Ctx_Focused | dream::Ctx_Output);
}

FReply SDreamOutputView::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	return !Impl->bLive && FDreamKeyRouter::Get().ShouldConsumeKeyUp(InKeyEvent) ? FReply::Handled() : FReply::Unhandled();
}

void SDreamOutputView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	Impl->TickFollow();
	Impl->TickSelection();      // (output, PO-06 F3) a drag held above or below the rows scrolls
	Impl->TickRows();
}

void SDreamOutputView::OpenFind()
{
	FImpl& State = *Impl;
	if (State.bLive || !State.FindBar.IsValid())
	{
		return;
	}
	if (!State.bFindOpen)
	{
		State.bFindOpen = true;
		if (State.FindHost.IsValid())
		{
			State.FindHost->SetContent(State.FindBar.ToSharedRef());
		}
		// The selected line seeds the box when it is one short line (as the code view seeds from a one-line selection).
		FString Seed;
		if (State.FirstSelected().IsValid())
		{
			const FString Text = State.SelectedText();
			if (!Text.IsEmpty() && Text.Len() <= 200 && !Text.Contains(TEXT("\n")))
			{
				Seed = Text.TrimStartAndEnd();
			}
		}
		State.FindBar->Open(/*bShowReplace*/ false, Seed);
		State.ApplyFindQueryFromBar();
	}
	State.FocusFindBox();
}

void SDreamOutputView::CloseFind()
{
	FImpl& State = *Impl;
	if (!State.bFindOpen)
	{
		return;
	}
	State.bFindOpen = false;
	State.bFocusFindBoxPending = false;
	if (State.FindHost.IsValid())
	{
		State.FindHost->SetContent(SNullWidget::NullWidget);
	}
	State.ResetFindResults();       // the query stays in the box for the next Ctrl+F; matches are found again then
	State.FindQuery.reset();
	State.FindError = FText::GetEmpty();
	if (State.List.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(State.List, EFocusCause::SetDirectly);
	}
}

bool SDreamOutputView::IsFindOpen() const
{
	return Impl->bFindOpen;
}

void SDreamOutputView::SetFindQuery(const FString& Text, bool bMatchCase, bool bRegex, bool bWholeWord)
{
	FImpl& State = *Impl;
	if (State.FindBar.IsValid())
	{
		TGuardValue<bool> Guard(State.bApplyingQuery, true);
		State.FindBar->SetFindText(Text);
		State.FindBar->SetMatchCase(bMatchCase);
		State.FindBar->SetRegex(bRegex);
		State.FindBar->SetWholeWord(bWholeWord);
	}
	State.ApplyFindQuery(Text, bMatchCase, bRegex, bWholeWord);
}

bool SDreamOutputView::FindStep(int32 Direction)
{
	return Impl->StepFind(Direction);
}

bool SDreamOutputView::IsFindSearching() const
{
	return Impl->FindTask.IsSet() || (Impl->FindQuery && Impl->bFindOpen && !Impl->bFindCapped
		&& Impl->Store->GetEndSeq() > Impl->FindSearchedEnd);
}

int32 SDreamOutputView::GetFindMatchCount() const
{
	return Impl->FindHits.Num();
}

uint64 SDreamOutputView::GetCurrentFindSeq() const
{
	return Impl->FindHits.IsValidIndex(Impl->FindCurrent) ? Impl->FindHits[Impl->FindCurrent].Seq : 0;
}

FText SDreamOutputView::GetFindStatus() const
{
	return Impl->FindStatus();
}

bool SDreamOutputView::IsSoftWrapEnabled()
{
	return DreamOutputViewPrivate::SoftWrapFlag();
}

void SDreamOutputView::SetSoftWrapEnabled(bool bEnabled)
{
	DreamOutputViewPrivate::SoftWrapFlag() = bEnabled;   // each view makes its row widgets again at its next tick
}

void SDreamOutputView::ExpandEcho(bool bExpand)
{
	Impl->ExpandEcho(bExpand);
}

void SDreamOutputView::AddShowMenuEntries(FMenuBuilder& Menu, TWeakPtr<IDreamCommandTarget> EchoTarget)
{
	// §6.4 "Design: filters" (OUT-03, M1): one toggle per type, plus background. Hiding Info hides only info lines.
	// Deferred (M2): the Minimal and Echo All presets and SGR colour spans (OUT-03); the run, source and regex filters on
	// the filter worker (OUT-06).
	struct FEntry
	{
		FText Label;
		bool FDreamOutputVisibility::* Field;
	};
	const FEntry Entries[] = {
		{ LOCTEXT("Out.ShowStdout", "Stdout"), &FDreamOutputVisibility::bStdout },
		{ LOCTEXT("Out.ShowStderr", "Stderr"), &FDreamOutputVisibility::bStderr },
		{ LOCTEXT("Out.ShowResults", "Results"), &FDreamOutputVisibility::bResults },
		{ LOCTEXT("Out.ShowInfo", "Info"), &FDreamOutputVisibility::bInfo },
		{ LOCTEXT("Out.ShowWarnings", "Warnings"), &FDreamOutputVisibility::bWarnings },
		{ LOCTEXT("Out.ShowErrors", "Errors"), &FDreamOutputVisibility::bErrors },
		{ LOCTEXT("Out.ShowEcho", "Echo input"), &FDreamOutputVisibility::bEcho },
		{ LOCTEXT("Out.ShowNotices", "Notices"), &FDreamOutputVisibility::bNotice },
		{ LOCTEXT("Out.ShowBackground", "Background"), &FDreamOutputVisibility::bBackground },
	};
	for (const FEntry& Entry : Entries)
	{
		bool FDreamOutputVisibility::* const Field = Entry.Field;
		const bool bEcho = Field == &FDreamOutputVisibility::bEcho;
		Menu.AddMenuEntry(Entry.Label,
			bEcho ? LOCTEXT("Out.ShowEchoTip", "The code that ran, above its output (the toolbar's Echo input)")
				: FText::Format(LOCTEXT("Out.ShowTypeTip", "Show {0} lines"), Entry.Label),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Field, bEcho, EchoTarget]()
				{
					if (!FDreamEditorModule::IsAvailable())
					{
						return;
					}
					const TSharedPtr<IDreamCommandTarget> Target = EchoTarget.Pin();
					if (bEcho && Target.IsValid())
					{
						Target->ExecuteDreamCommand(EDreamCommand::ToggleEchoInput);   // the same switch, persisted (OUT-12)
						return;
					}
					FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
					FDreamOutputVisibility Visibility = Store.GetVisibility();
					Visibility.*Field = !(Visibility.*Field);
					Store.SetVisibility(Visibility);
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([Field]()
				{
					return FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetOutputStore().GetVisibility().*Field;
				})),
			NAME_None, EUserInterfaceActionType::ToggleButton);
	}
	// §6.4 widget: "soft wrap is off by default. A toggle turns on wrap". (output choice) It sits with the Show toggles, so
	// the context menu, the View menu and the Filter button all carry it.
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(LOCTEXT("Out.SoftWrap", "Soft wrap"),
		LOCTEXT("Out.SoftWrapTip", "Wrap output lines wider than the view instead of running past its edge"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([]() { SDreamOutputView::SetSoftWrapEnabled(!SDreamOutputView::IsSoftWrapEnabled()); }),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([]() { return SDreamOutputView::IsSoftWrapEnabled(); })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
}

#undef LOCTEXT_NAMESPACE
