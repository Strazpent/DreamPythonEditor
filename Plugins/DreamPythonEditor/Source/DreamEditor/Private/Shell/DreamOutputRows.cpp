// SPDX-License-Identifier: Apache-2.0
// Output rows (TDD §6.4 rendering by type, header and footer, result lines, traceback links). See DreamOutputRows.h.
// (output, PO-04 point 2) The Maya-style history: no gutter and no per-line icons; results, warnings and errors carry the
// "# Result: … #", "# Warning: … #" and "# … / # Error: … #" decorations; echo is muted; stderr keeps its colour with no
// prefix. PO-04 outranks A11Y-05 here: echo and stderr differ from stdout by colour only. Colours come from
// FDreamEditorStyle, which follows the current theme; the view makes its rows again when the theme changes.
#include "Shell/DreamOutputRows.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "HAL/FileManager.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Persistence/DreamJournal.h"
#include "Run/DreamRunService.h"
#include "Run/DreamTransactions.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamOutputSelection.h"     // (output, PO-06 F3) the pieces of a row's drawn text
#include "Styling/AppStyle.h"
#include "Styling/SlateStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "dream/OutputLines.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamOutputRowsPrivate
{
	FString Utf8(std::string_view Text)
	{
		return DreamText::Utf8ToFString(Text);
	}

	FSlateColor LinkColor()
	{
		return FDreamEditorStyle::Get().GetSlateColor(DreamStyleKeys::OutputLink);
	}

	/** (output, PO-04) Muted text (chips, badges, the eviction marker): the theme's header colour, as run headers are drawn. */
	FSlateColor MutedColor()
	{
		return FDreamEditorStyle::GetLineTypeColor(dream::LineType::RunHeader);
	}

	FSlateFontInfo SmallFont(const FSlateFontInfo& Font)
	{
		FSlateFontInfo Small = Font;
		Small.Size = FMath::Max(6.f, Font.Size - 1.f);
		return Small;
	}

	bool IsPromptRoute(EDreamRunRoute Route)
	{
		return Route == EDreamRunRoute::Repl || Route == EDreamRunRoute::ConsoleExecutor;
	}

	FString ShortFile(const FString& File)
	{
		return File.StartsWith(TEXT("<")) ? File : FPaths::GetCleanFilename(File);
	}

	FString LocalTime(const FDateTime& Utc)
	{
		if (Utc.GetTicks() == 0)
		{
			return FString();
		}
		const FTimespan Offset = FDateTime::Now() - FDateTime::UtcNow();
		return (Utc + Offset).ToString(TEXT("%H:%M:%S"));
	}

	FString RouteBadge(EDreamRunRoute Route)
	{
		switch (Route)
		{
		case EDreamRunRoute::Repl:            return TEXT("REPL");
		case EDreamRunRoute::ConsoleExecutor: return TEXT("console");
		case EDreamRunRoute::RunFile:         return TEXT("run file");
		case EDreamRunRoute::Api:             return TEXT("api");
		default:                             return FString();
		}
	}

	FString SourceAndRange(const FDreamRunRecord& Rec)
	{
		if (IsPromptRoute(Rec.Route))
		{
			return FString();
		}
		FString Source = !Rec.TabTitle.IsEmpty() ? Rec.TabTitle : FPaths::GetCleanFilename(Rec.FilePath);
		if (Rec.Route == EDreamRunRoute::Selection || Rec.Route == EDreamRunRoute::Line || Rec.Route == EDreamRunRoute::Cell)
		{
			Source += Rec.FirstLine == Rec.LastLine
				? FString::Printf(TEXT(" · line %d"), Rec.FirstLine)
				: FString::Printf(TEXT(" · lines %d–%d"), Rec.FirstLine, Rec.LastLine);
		}
		return Source;
	}

	/** A prompt run's submitted code for its header: the first line plus "(+n lines)" (§6.4, PO-02). */
	FString SubmittedCode(const FDreamRunRecord& Rec)
	{
		const dream::TextSnapshot& Snapshot = Rec.SourceSnapshot;
		int32 Lines = Snapshot.LineCount();
		while (Lines > 1 && Snapshot.LineLength(Lines - 1) == 0)
		{
			--Lines;
		}
		FString First = DreamText::ToFString(Snapshot.LineText(0));
		if (First.Len() > 120)
		{
			First = First.Left(119) + TEXT("…");
		}
		return Lines > 1 ? FString::Printf(TEXT("%s (+%d lines)"), *First, Lines - 1) : First;
	}

	FString TransactionClause(const FDreamRunRecord& Rec)
	{
		const FDreamTxnHandle& Txn = Rec.Txn;
		switch (Txn.Status)
		{
		case EDreamTxnStatus::Recorded:
			return FString::Printf(TEXT("undo: \"%s\" (%s objects, %s)"), *Txn.Title.ToString(),
				*FText::AsNumber(Txn.ObjectCount).ToString(), *FText::AsMemory(Txn.LastSize).ToString());
		case EDreamTxnStatus::Partial:
			return TEXT("partly undoable (recording stopped at 100% of the undo buffer)");
		case EDreamTxnStatus::Off:
			return Rec.Route == EDreamRunRoute::RunFile ? TEXT("no transaction (Run File)") : TEXT("no transaction");
		case EDreamTxnStatus::Cleared:
			return TEXT("not undoable");
		case EDreamTxnStatus::NotStarted:
		{
			const FString Note = Txn.Note.ToString();
			if (Note.IsEmpty())
			{
				return FString();
			}
			return Note.StartsWith(TEXT("undo unavailable")) ? Note : TEXT("undo unavailable: ") + Note;
		}
		case EDreamTxnStatus::Undone:
			return FString::Printf(TEXT("undone: \"%s\""), *Txn.Title.ToString());
		case EDreamTxnStatus::Purged:
			return TEXT("undo record purged");
		case EDreamTxnStatus::CancelledByScript:
			return TEXT("the script cancelled its transaction");
		default:
			return FString();
		}
	}

	const FDreamRunRecord* FindRecord(const IDreamOutputRowHost& Host, uint32 RunNumber)
	{
		FDreamRunService* RunService = Host.GetRunService();
		return RunService != nullptr && RunNumber != 0 ? RunService->FindRecord(static_cast<int32>(RunNumber)) : nullptr;
	}

	/** The text of a line row, its EchoSplit parts joined (§6.1). */
	FString JoinedText(const FDreamOutputRow& Row, const FDreamOutputStore& Store, std::string_view First)
	{
		FString Text = Utf8(First);
		for (const uint64 Part : Row.JoinedSeqs)
		{
			dream::LineView View;
			if (Store.GetLine(Part, View))
			{
				Text += Utf8(View.Text);
			}
		}
		return Text;
	}

	/**
	 * (output, PO-04) The badges that carry what nothing else on the row shows: a background line's origin (a link back to
	 * its run) and the thread name of stdout and result lines written off the game thread (B90). The type badges
	 * ([stderr], [dream], [LogPython], [unreal.log], ...) are gone with the Maya look: the decorations name the type.
	 */
	FString Badge(const dream::LineInfo& Info, const FDreamOutputStore& Store)
	{
		if ((Info.Flags & dream::OLF_Background) != 0)
		{
			return Info.RunNumber != 0 ? FString::Printf(TEXT("background · from #%u"), Info.RunNumber) : FString(TEXT("background"));
		}
		if ((Info.Type == dream::LineType::Stdout || Info.Type == dream::LineType::Result) && Info.ThreadTag != 0)
		{
			return Store.GetThreadName(Info.ThreadTag);
		}
		return FString();
	}

	/** Whether a link target can be opened, and the tooltip when it cannot (§6.4 "Missing file", "closed tab"). */
	bool LinkTarget(const FString& Path, dream::LinkKind Kind, FText& OutTooltip)
	{
		if (Kind == dream::LinkKind::PseudoFile)
		{
			std::string_view Name;
			std::string_view Id;
			const std::string PathUtf8 = DreamText::ToUtf8(Path);
			if (dream::ParsePseudoFilename(PathUtf8, Name, Id) && (Name == "REPL" || Name == "Console"))
			{
				OutTooltip = LOCTEXT("Out.PromptLinkTip", "Scroll to this input's echo in the output");
				return true;
			}
			if (FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetBufferManager().ResolveFilename(Path).IsValid())
			{
				OutTooltip = FText::Format(LOCTEXT("Out.BufferLinkTip", "Open {0}"), FText::FromString(Path));
				return true;
			}
			// (buffers, B86) A closed tab whose text Dream still keeps offers Reopen tab: the click reopens it with its ID, so this
			// pseudo filename names it again (FDreamBufferManager::ReopenClosed(Id) through SDreamTab::OpenLocation, §6.4).
			if (FDreamEditorModule::IsAvailable())
			{
				if (const TSharedPtr<FDreamBuffer> Closed = FDreamEditorModule::Get().GetBufferManager().FindClosedByFilename(Path))
				{
					OutTooltip = FText::Format(LOCTEXT("Out.ReopenTabTip", "Reopen tab: {0} is closed; a click reopens it with its text"),
						FText::FromString(Closed->GetDisplayName()));
					return true;
				}
			}
			OutTooltip = LOCTEXT("Out.ClosedTabTip", "The tab this code ran from is closed");
			return false;
		}
		if (!IFileManager::Get().FileExists(*Path))
		{
			OutTooltip = FText::Format(LOCTEXT("Out.MissingFileTip", "{0} no longer exists"), FText::FromString(Path));
			return false;
		}
		OutTooltip = FText::Format(LOCTEXT("Out.FileLinkTip", "Open {0}"), FText::FromString(Path));
		return true;
	}

	/** bWrap (the soft-wrap toggle, §6.4): the text fills the rest of the row and wraps at its width. */
	void AddPlain(const TSharedRef<SHorizontalBox>& Box, const FString& Text, const FSlateFontInfo& Font, const FSlateColor& Color,
		bool bWrap = false)
	{
		if (Text.IsEmpty())
		{
			return;
		}
		// (output, PO-06 F3) Every plain piece is part of the row's drawn text, which the selection maps.
		if (bWrap)
		{
			Box->AddSlot().FillWidth(1.f)
			[
				DreamOutputSelection::Piece(SNew(STextBlock).Text(FText::FromString(Text)).Font(Font).ColorAndOpacity(Color).AutoWrapText(true),
					0, /*bWrap*/ true)
			];
			return;
		}
		Box->AddSlot().AutoWidth()
		[
			DreamOutputSelection::Piece(SNew(STextBlock).Text(FText::FromString(Text)).Font(Font).ColorAndOpacity(Color))
		];
	}

	/** Find (§6.4): a line's matches, (start, length) in UTF-16 units of its text, and the current one's index. */
	struct FFindMarks
	{
		TArray<TPair<int32, int32>> Ranges;
		int32 Current = INDEX_NONE;
	};

	/** Plain text that starts at UTF-16 offset Start16 of its line, with the find matches inside it on a highlight. */
	void AddPlainMarked(const TSharedRef<SHorizontalBox>& Box, const FString& Text, int32 Start16, const FFindMarks& Marks,
		const FSlateFontInfo& Font, const FSlateColor& Color, bool bWrap)
	{
		int32 Pos = 0;
		for (int32 Index = 0; Index < Marks.Ranges.Num(); ++Index)
		{
			const int32 From = FMath::Clamp(Marks.Ranges[Index].Key - Start16, 0, Text.Len());
			const int32 To = FMath::Clamp(Marks.Ranges[Index].Key + Marks.Ranges[Index].Value - Start16, 0, Text.Len());
			if (To <= From || To <= Pos)
			{
				continue;           // outside this piece, or an empty match (nothing to paint)
			}
			const int32 Begin = FMath::Max(From, Pos);
			AddPlain(Box, Text.Mid(Pos, Begin - Pos), Font, Color);
			const FSlateColor Tint = FDreamEditorStyle::Get().GetSlateColor(
				Index == Marks.Current ? DreamStyleKeys::EditorFindMatchCurrent : DreamStyleKeys::EditorFindMatch);
			Box->AddSlot().AutoWidth()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(Tint)
				.Padding(0.f)
				[
					DreamOutputSelection::Piece(SNew(STextBlock).Text(FText::FromString(Text.Mid(Begin, To - Begin))).Font(Font).ColorAndOpacity(Color))
				]
			];
			Pos = To;
		}
		AddPlain(Box, Text.Mid(Pos), Font, Color, bWrap);
	}

	/** The line's text with its links; JoinedTail (EchoSplit parts, then the " #" decoration) follows the last piece. */
	void AddTextWithLinks(const TSharedRef<SHorizontalBox>& Box, const dream::LineView& View, const FString& JoinedTail,
		const TSharedRef<IDreamOutputRowHost>& Host, const FSlateColor& Color, const FFindMarks& Marks, bool bWrapTail)
	{
		const FSlateFontInfo& Font = Host->GetRowFont();
		const std::string_view Text = View.Text;
		size_t Pos = 0;
		int32 Pos16 = 0;            // UTF-16 offset of Pos in the line's text (the find matches' unit)
		const TWeakPtr<IDreamOutputRowHost> WeakHost = Host;
		for (int32 Index = 0; Index < static_cast<int32>(View.Links.size()); ++Index)
		{
			const dream::LinkSpan& Link = View.Links[Index];
			if (Link.Start < Pos || static_cast<size_t>(Link.Start) + Link.Len > Text.size())
			{
				continue;
			}
			const FString Before = Utf8(Text.substr(Pos, Link.Start - Pos));
			AddPlainMarked(Box, Before, Pos16, Marks, Font, Color, false);
			Pos16 += Before.Len();
			const FString LinkText = Utf8(Text.substr(Link.Start, Link.Len));
			const FString Path = Utf8(Text.substr(Link.PathStart, Link.PathLen));
			Pos16 += LinkText.Len();
			if (Host->IsLiveView())
			{
				AddPlain(Box, LinkText, Font, LinkColor());   // no clicks reach the live run view (§5.6.2)
			}
			else
			{
				FText Tooltip;
				const bool bEnabled = LinkTarget(Path, Link.Kind, Tooltip);
				const uint64 Seq = View.Seq;
				Box->AddSlot().AutoWidth()
				[
					DreamOutputSelection::Piece(SNew(SHyperlink)       // (output, PO-06 F3) a link is part of the drawn text
					.Text(FText::FromString(LinkText))
					.TextStyle(&Host->GetLinkTextStyle())
					.ToolTipText(Tooltip)
					.IsEnabled(bEnabled)
					.OnNavigate_Lambda([WeakHost, Seq, Index]()
					{
						if (const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin())
						{
							Pinned->OpenLink(Seq, Index);
						}
					}))
				];
			}
			Pos = static_cast<size_t>(Link.Start) + Link.Len;
		}
		AddPlainMarked(Box, Utf8(Text.substr(FMath::Min(Pos, Text.size()))) + JoinedTail, Pos16, Marks, Font, Color, bWrapTail);
	}

	TSharedRef<SWidget> MakeLine(const FDreamOutputRowPtr& Row, const TSharedRef<IDreamOutputRowHost>& Host)
	{
		const FDreamOutputStore& Store = Host->GetStore();
		const FSlateFontInfo& Font = Host->GetRowFont();
		dream::LineView View;
		if (!Store.GetLine(Row->Seq, View))
		{
			return DreamOutputSelection::Piece(SNew(STextBlock).Font(Font).ColorAndOpacity(MutedColor())
				.Text(LOCTEXT("Out.DroppedLine", "(line dropped)")));
		}
		const dream::LineInfo Info = View.Info;
		// PO-04: the whole row in its type's colour, decorations included ("all in the theme's error colour"); echo is muted
		// by its own theme colour.
		const FSlateColor Color = FDreamEditorStyle::GetLineTypeColor(Info.Type);
		const TWeakPtr<IDreamOutputRowHost> WeakHost = Host;

		TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
		const FString BadgeText = Badge(Info, Store);
		if (!BadgeText.IsEmpty())
		{
			const uint32 RunNumber = Info.RunNumber;
			const bool bBackLink = (Info.Flags & dream::OLF_Background) != 0 && RunNumber != 0 && !Host->IsLiveView();
			TSharedRef<SWidget> BadgeWidget = bBackLink
				? StaticCastSharedRef<SWidget>(SNew(SHyperlink)
					.Text(FText::FromString(TEXT("↑ ") + BadgeText))
					.TextStyle(&Host->GetLinkTextStyle())
					.ToolTipText(FText::Format(LOCTEXT("Out.BackToRunTip", "Go to the block of run #{0}"), FText::AsNumber(RunNumber)))
					.OnNavigate_Lambda([WeakHost, RunNumber]()
					{
						if (const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin())
						{
							Pinned->JumpToRun(RunNumber);
						}
					}))
				: StaticCastSharedRef<SWidget>(SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("[%s]"), *BadgeText)))
					.Font(SmallFont(Font))
					.ColorAndOpacity(MutedColor()));
			Box->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 6.f, 0.f))[BadgeWidget];
		}

		// Decoration (§6.4 as PO-04 point 2 restyles it): "# Result: ", "# Warning: ", "# Error: " or "# ".
		AddPlain(Box, DreamOutputRows::DecorationPrefix(*Row), Font, Color);

		FString Tail;
		for (const uint64 Part : Row->JoinedSeqs)
		{
			dream::LineView PartView;
			if (Store.GetLine(Part, PartView))
			{
				Tail += Utf8(PartView.Text);
			}
		}
		Tail += DreamOutputRows::DecorationSuffix(*Row);
		FFindMarks Marks;
		Host->GetFindMatches(Row->Seq, Marks.Ranges, Marks.Current);
		// Soft wrap (§6.4): the line's last text piece wraps; a truncated result keeps its chips right after the text.
		const bool bTruncatedResult = (Info.Flags & dream::OLF_Truncated) != 0 && Info.Type == dream::LineType::Result;
		const bool bWrapTail = Host->IsSoftWrap() && !bTruncatedResult;
		AddTextWithLinks(Box, View, Tail, Host, Color, Marks, bWrapTail);

		if (bTruncatedResult)
		{
			// The "… n more characters" chip and Show all stay inline (a fold); the result's Copy is the context menu's Copy,
			// which copies the whole raw repr (PO-04: Copy moved to the context menu).
			const FString Blob = Store.GetResultBlob(Row->Seq);
			const uint64 Seq = Row->Seq;
			const bool bExpanded = Host->IsResultExpanded(Seq);
			FText Chip = Blob.IsEmpty()
				? LOCTEXT("Out.ResultEvicted", "… (the rest of this result was dropped)")
				: FText::Format(LOCTEXT("Out.ResultMore", "… {0} more characters"), FText::AsNumber(Blob.Len()));
			Box->AddSlot().AutoWidth().Padding(FMargin(8.f, 0.f, 0.f, 0.f))
			[
				SNew(STextBlock).Text(Chip).Font(SmallFont(Font)).ColorAndOpacity(MutedColor())
			];
			if (!Blob.IsEmpty() && !Host->IsLiveView())
			{
				constexpr int32 InPlaceMaxChars = 1 << 20;          // §6.4: Show all in place up to 1 MB
				const bool bInPlace = Blob.Len() + View.Text.size() <= InPlaceMaxChars;
				Box->AddSlot().AutoWidth().Padding(FMargin(6.f, 0.f, 0.f, 0.f))
				[
					SNew(SHyperlink)
					.Text(bExpanded ? LOCTEXT("Out.ResultHide", "Hide") : LOCTEXT("Out.ResultShowAll", "Show all"))
					.TextStyle(&Host->GetLinkTextStyle())
					.IsEnabled(bInPlace)
					// Deferred (G1): results over 1 MB open in a read-only "Result #N" tab (plain mode edits only files under 1 MB, A40).
					.ToolTipText(bInPlace ? FText::GetEmpty() : LOCTEXT("Out.ResultTooLarge", "Over 1 MB: use Copy in the context menu"))
					.OnNavigate_Lambda([WeakHost, Seq]()
					{
						if (const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin())
						{
							Pinned->ToggleResultExpanded(Seq);
						}
					})
				];
			}
		}

		if ((Info.Flags & dream::OLF_Background) != 0)
		{
			return SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush(TEXT("NoBorder")))
				.Padding(0.f)
				.ColorAndOpacity(FDreamEditorStyle::Get().GetColor(DreamStyleKeys::OutputBackgroundDim))   // "dimmed" (§6.4)
				[Box];
		}
		return Box;
	}

	/** (output, PO-04) The header as one line's left part: the run, source, namespace and time, then any transaction notes
	 *  (§6.4, §5.9.2; they had a line of their own), and the eviction note of a synthetic header (§6.2). */
	FText HeaderLeft(const IDreamOutputRowHost& Host, uint32 RunNumber, bool bSynthetic)
	{
		const FDreamRunRecord* Rec = FindRecord(Host, RunNumber);
		const bool bShowCode = Rec != nullptr && IsPromptRoute(Rec->Route) && Host.IsRunEchoHidden(RunNumber);
		FText Text = DreamOutputRows::HeaderLeftText(Rec, RunNumber, Host.IsRunCollapsed(RunNumber), bShowCode);
		const FText Notes = DreamOutputRows::TransactionNotesText(Rec);
		if (!Notes.IsEmpty())
		{
			Text = FText::Format(LOCTEXT("Out.HeaderNotes", "{0} · {1}"), Text, Notes);
		}
		if (bSynthetic)
		{
			const FString Path = Host.GetStore().GetRunLogPath(RunNumber);
			Text = FText::Format(LOCTEXT("Out.SyntheticHeader", "{0} (earlier lines dropped; full log: {1})"), Text,
				Path.IsEmpty() ? LOCTEXT("Out.EditorLog", "the editor log") : FText::FromString(Path));
		}
		return Text;
	}

	/** PO-04: a header is one muted line; a click on its left part collapses or expands the block (folding stays). */
	TSharedRef<SWidget> MakeHeader(const FDreamOutputRowPtr& Row, const TSharedRef<IDreamOutputRowHost>& Host)
	{
		const uint32 RunNumber = Row->RunNumber;
		const bool bSynthetic = Row->Kind == EDreamOutputRowKind::SyntheticHeader;
		const TWeakPtr<IDreamOutputRowHost> WeakHost = Host;
		const FSlateFontInfo& Font = Host->GetRowFont();
		const FSlateColor Color = FDreamEditorStyle::GetLineTypeColor(dream::LineType::RunHeader);

		auto Left = [WeakHost, RunNumber, bSynthetic]()
		{
			const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin();
			return Pinned.IsValid() ? HeaderLeft(*Pinned, RunNumber, bSynthetic) : FText::GetEmpty();
		};
		auto Right = [WeakHost, RunNumber]()
		{
			const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin();
			return Pinned.IsValid() ? DreamOutputRows::HeaderRightText(FindRecord(*Pinned, RunNumber)) : FText::GetEmpty();
		};

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), TEXT("NoBorder"))
				.ContentPadding(FMargin(0.f))
				.ToolTipText(LOCTEXT("Out.ToggleRunTip", "Collapse or expand this run's block"))
				.IsFocusable(false)
				.OnClicked_Lambda([WeakHost, RunNumber]()
				{
					if (const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin())
					{
						Pinned->ToggleRunCollapsed(RunNumber);
					}
					return FReply::Handled();
				})
				[
					DreamOutputSelection::Piece(SNew(STextBlock).Text_Lambda(Left).Font(Font).ColorAndOpacity(Color))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12.f, 0.f, 4.f, 0.f))
			[
				// (output, PO-06 F3) The drawn text puts two spaces between the parts (GetRowDisplayText).
				DreamOutputSelection::Piece(SNew(STextBlock).Text_Lambda(Right).Font(Font).ColorAndOpacity(Color), /*Gap*/ 2)
			];
	}

	/** PO-04: a footer is one muted line. Its former chips (Undo this run, Run without snapshot, Open storage settings,
	 *  Re-run, Run in transaction) are entries of the view's context menu on any line of the run. */
	TSharedRef<SWidget> MakeFooter(const FDreamOutputRowPtr& Row, const TSharedRef<IDreamOutputRowHost>& Host)
	{
		const uint32 RunNumber = Row->RunNumber;
		const TWeakPtr<IDreamOutputRowHost> WeakHost = Host;
		return DreamOutputSelection::Piece(SNew(STextBlock)
			.Font(Host->GetRowFont())
			.ColorAndOpacity(FDreamEditorStyle::GetLineTypeColor(dream::LineType::RunFooter))
			.Text_Lambda([WeakHost, RunNumber]()
			{
				const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin();
				return Pinned.IsValid()
					? DreamOutputRows::FooterText(FindRecord(*Pinned, RunNumber), RunNumber, Pinned->GetStore())
					: FText::GetEmpty();
			}));
	}

	FText EvictionText(const IDreamOutputRowHost& Host)
	{
		const FDreamOutputStore& Store = Host.GetStore();
		const TConstArrayView<uint32> Runs = Store.GetEvictedRunNumbers();
		FText RunsText = FText::GetEmpty();
		if (Runs.Num() == 1)
		{
			RunsText = FText::Format(LOCTEXT("Out.EvictedOneRun", " (run #{0})"), FText::AsNumber(Runs[0]));
		}
		else if (Runs.Num() > 1)
		{
			RunsText = FText::Format(LOCTEXT("Out.EvictedRuns", " (runs #{0}–#{1})"), FText::AsNumber(Runs[0]), FText::AsNumber(Runs.Last()));
		}
		FString Folder = TEXT("<root>\\RunLogs");
		if (FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetJournal().IsOpen())
		{
			Folder = FPaths::ConvertRelativePathToFull(FDreamEditorModule::Get().GetJournal().GetStorageRoot().RunLogsDir());
		}
		// PO-04: no per-line icons, so no leading marker glyph.
		return FText::Format(LOCTEXT("Out.EvictionMarker",
			"{0} earlier lines dropped{1}. Large runs have logs in {2}; all Python output is also in the editor log."),
			FText::AsNumber(Store.GetEvictedLineCount()), RunsText, FText::FromString(Folder));
	}

	TSharedRef<SWidget> MakeEviction(const TSharedRef<IDreamOutputRowHost>& Host)
	{
		const TWeakPtr<IDreamOutputRowHost> WeakHost = Host;
		return DreamOutputSelection::Piece(SNew(STextBlock)
			.Font(Host->GetRowFont())
			.ColorAndOpacity(MutedColor())
			.Text_Lambda([WeakHost]()
			{
				const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin();
				return Pinned.IsValid() ? EvictionText(*Pinned) : FText::GetEmpty();
			}));
	}

	FText FoldChipText(int32 Count)
	{
		return FText::Format(LOCTEXT("Out.FoldChip", "… {0} more lines"), FText::AsNumber(Count));
	}

	FText StartupHeaderText(bool bExpanded, int32 Count)
	{
		return FText::Format(LOCTEXT("Out.StartupBlock", "{0} Editor startup · {1} warnings and errors from LogPython"),
			FText::FromString(bExpanded ? TEXT("▼") : TEXT("▶")), FText::AsNumber(Count));
	}

	/** (review r6 M1) The outcome line as drawn: "# Error: Run #N <outcome> #", or "# Warning: … #" for a stopped run. */
	FString RunOutcomeLine(const IDreamOutputRowHost& Host, uint32 RunNumber, bool& bOutStopped)
	{
		const FDreamRunRecord* Rec = FindRecord(Host, RunNumber);
		bOutStopped = Rec != nullptr && DreamOutputRows::IsStoppedRun(*Rec);
		return FString::Printf(TEXT("%s%s #"), bOutStopped ? TEXT("# Warning: ") : TEXT("# Error: "), *DreamOutputRows::RunOutcomeText(Rec, RunNumber));
	}

	/** A startup-ring line (A47) as the history draws a warning or an error line: "# Warning: <text> #" / "# Error: <text> #". */
	FString StartupLineText(const FDreamOutputRow& Row)
	{
		return FString::Printf(TEXT("%s%s #"), Row.Verbosity == ELogVerbosity::Error ? TEXT("# Error: ") : TEXT("# Warning: "), *Row.Text);
	}

	// ---- (output, PO-04) Decoration blocks ----

	enum class EErrorLine : uint8
	{
		Blank,
		Indented,           // a frame, source or caret line
		TracebackStart,     // "Traceback (most recent call last):"
		ChainSeparator,     // "During handling of the above exception, ..." / "The above exception was the direct cause ..."
		ExceptionLike,      // "<dotted name>", "<dotted name>:" or "<dotted name>: <message>" (CPython's exception line)
		Other,
	};

	bool IsNameStart(unsigned char C)
	{
		return C == '_' || (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C >= 0x80;   // any non-ASCII byte: Unicode names
	}

	EErrorLine ClassifyErrorLine(std::string_view Text)
	{
		if (Text.find_first_not_of(" \t\r\f") == std::string_view::npos)
		{
			return EErrorLine::Blank;
		}
		if (Text[0] == ' ' || Text[0] == '\t')
		{
			return EErrorLine::Indented;
		}
		if (Text.starts_with("Traceback (most recent call last):"))
		{
			return EErrorLine::TracebackStart;
		}
		if (Text.starts_with("During handling of the above exception") || Text.starts_with("The above exception was the direct cause"))
		{
			return EErrorLine::ChainSeparator;
		}
		size_t Pos = 0;
		for (;;)
		{
			if (Pos >= Text.size() || !IsNameStart(static_cast<unsigned char>(Text[Pos])))
			{
				return EErrorLine::Other;
			}
			++Pos;
			while (Pos < Text.size() && (IsNameStart(static_cast<unsigned char>(Text[Pos])) || (Text[Pos] >= '0' && Text[Pos] <= '9')))
			{
				++Pos;
			}
			if (Pos < Text.size() && Text[Pos] == '.')
			{
				++Pos;
				continue;
			}
			break;
		}
		if (Pos == Text.size() || (Text[Pos] == ':' && (Pos + 1 == Text.size() || Text[Pos + 1] == ' ')))
		{
			return EErrorLine::ExceptionLike;
		}
		return EErrorLine::Other;
	}

	bool IsIndented(std::string_view Text)
	{
		return !Text.empty() && (Text[0] == ' ' || Text[0] == '\t');
	}

	/** Changes a row made earlier and notes it, so the view makes its widget again. */
	void SetDeco(const FDreamOutputRowPtr& Row, EDreamRowDeco Deco, bool bEnd, TArray<FDreamOutputRowPtr>& OutChanged)
	{
		if (Row.IsValid() && (Row->Deco != Deco || Row->bDecoEnd != bEnd))
		{
			Row->Deco = Deco;
			Row->bDecoEnd = bEnd;
			OutChanged.Add(Row);
		}
	}

	/** Row becomes the block's last row: the " #" moves from the row before it. */
	void MoveEnd(FDreamDecoBlock& Block, const FDreamOutputRowPtr& Row, TArray<FDreamOutputRowPtr>& OutChanged)
	{
		if (Block.Last.IsValid() && Block.Last != Row)
		{
			SetDeco(Block.Last, Block.Last->Deco, false, OutChanged);
		}
		Row->bDecoEnd = true;
		Block.Last = Row;
	}

	void DecorateError(FDreamDecoBlock& Block, const FDreamOutputRowPtr& Row, const dream::LineInfo& Info, std::string_view Text,
		TArray<FDreamOutputRowPtr>& OutChanged)
	{
		const uint16 Origin = static_cast<uint16>(Info.Flags & (dream::OLF_FromUnrealLog | dream::OLF_FromLogPython));
		const EErrorLine Kind = ClassifyErrorLine(Text);
		// A frame line ("  File \"<path>\", line N, in f", or the SyntaxError block's first line) opens a frame section.
		const size_t FirstChar = Text.find_first_not_of(" \t");
		const bool bFrameLine = Kind == EErrorLine::Indented && FirstChar != std::string_view::npos
			&& Text.substr(FirstChar).starts_with("File \"");
		bool bJoin = Block.Type == dream::LineType::Error && Block.Origin == Origin && Block.Last.IsValid();
		if (bJoin && Origin != 0)
		{
			// One log record per line (unreal.log_error, captured LogPython): an unindented line is an error of its own,
			// unless a traceback is still being written (no exception line yet, or a chained traceback is starting).
			bJoin = Kind == EErrorLine::Indented
				|| (Block.bTraceback && (!Block.bExceptionFound || Block.bAwaitTraceback || Kind == EErrorLine::Blank
					|| Kind == EErrorLine::ChainSeparator));
		}
		if (!bJoin)
		{
			DreamOutputRows::CloseDecoration(Block);
			Block.Type = dream::LineType::Error;
			Block.Origin = Origin;
			Block.ErrorRow = Row;               // the fallback until an exception line comes
			Block.bExceptionFound = Kind == EErrorLine::ExceptionLike;
			Block.bTraceback = Kind == EErrorLine::TracebackStart;
			Block.bFrames = Kind == EErrorLine::TracebackStart || bFrameLine;
			Row->Deco = EDreamRowDeco::Error;
			MoveEnd(Block, Row, OutChanged);
		}
		else
		{
			MoveEnd(Block, Row, OutChanged);
			Row->Deco = EDreamRowDeco::Comment;
			if (Kind == EErrorLine::ExceptionLike && Block.bLastIndented && Block.bFrames)
			{
				// CPython's exception line follows the last frame's source or caret line; in a chained traceback the last
				// one is the error the run ended with. It closes the frame section, so lines indented under it and the
				// unindented lines between them (a ValidationError's field names) stay plain "# " lines.
				if (Block.ErrorRow.IsValid() && Block.ErrorRow != Row)
				{
					SetDeco(Block.ErrorRow, EDreamRowDeco::Comment, Block.ErrorRow->bDecoEnd, OutChanged);
				}
				Row->Deco = EDreamRowDeco::Error;
				Block.ErrorRow = Row;
				Block.bExceptionFound = true;
				Block.bFrames = false;
			}
			Block.bTraceback |= Kind == EErrorLine::TracebackStart;
			Block.bFrames |= Kind == EErrorLine::TracebackStart || bFrameLine;
		}
		Block.bLastIndented = Kind == EErrorLine::Indented;
		Block.bAwaitTraceback = Block.bTraceback && (Kind == EErrorLine::Blank || Kind == EErrorLine::ChainSeparator);
	}
}

namespace DreamOutputRows
{
	using namespace DreamOutputRowsPrivate;

	FString DecorationPrefix(const FDreamOutputRow& Row)
	{
		// (A226, owner 11-04) A REPL or console echo draws no '>>> ' / '... ' prompt, as in Maya: every echo row is its
		// source line as it is. OLF_ReplPrompt and OLF_PromptFirst stay in the store for the run log (§6.2).
		switch (Row.Deco)
		{
		case EDreamRowDeco::Result:  return TEXT("# Result: ");
		case EDreamRowDeco::Warning: return TEXT("# Warning: ");
		case EDreamRowDeco::Error:   return TEXT("# Error: ");
		case EDreamRowDeco::Comment: return TEXT("# ");
		default:                    return FString();
		}
	}

	FString DecorationSuffix(const FDreamOutputRow& Row)
	{
		return Row.bDecoEnd ? FString(TEXT(" #")) : FString();
	}

	void Decorate(FDreamDecoBlock& Block, const FDreamOutputRowPtr& Row, const dream::LineInfo& Info, std::string_view Text,
		TArray<FDreamOutputRowPtr>& OutChanged)
	{
		Row->Deco = EDreamRowDeco::None;
		Row->bDecoEnd = false;
		if ((Info.Flags & dream::OLF_Continued) != 0 && Block.Type == Info.Type && Block.Last.IsValid())
		{
			// A line over 64 KB goes on as Continued records, one row each (§6.1): still one line, so its rows extend the
			// open block and only the last takes the " #" (review r6 m2).
			MoveEnd(Block, Row, OutChanged);
			return;
		}
		switch (Info.Type)
		{
		case dream::LineType::Result:
		{
			const bool bCont = (Info.Flags & dream::OLF_ResultCont) != 0;
			if (bCont && Block.Type == dream::LineType::Result && Block.Last.IsValid())
			{
				MoveEnd(Block, Row, OutChanged);            // A63: a repr's later lines carry no decoration of their own
				return;
			}
			CloseDecoration(Block);
			Block.Type = dream::LineType::Result;
			Row->Deco = bCont ? EDreamRowDeco::None : EDreamRowDeco::Result;   // a repr whose first line this view does not show
			MoveEnd(Block, Row, OutChanged);
			return;
		}
		case dream::LineType::Warning:
			if (IsIndented(Text) && Block.Type == dream::LineType::Warning && Block.Last.IsValid())
			{
				MoveEnd(Block, Row, OutChanged);            // warnings.formatwarning's source line
				return;
			}
			CloseDecoration(Block);
			Block.Type = dream::LineType::Warning;
			Row->Deco = EDreamRowDeco::Warning;
			MoveEnd(Block, Row, OutChanged);
			return;
		case dream::LineType::Error:
			DecorateError(Block, Row, Info, Text, OutChanged);
			return;
		default:
			CloseDecoration(Block);
			return;
		}
	}

	void ExtendDecoration(FDreamDecoBlock& Block, const FDreamOutputRowPtr& Row, TArray<FDreamOutputRowPtr>& OutChanged)
	{
		if (Block.Last.IsValid())
		{
			MoveEnd(Block, Row, OutChanged);
		}
	}

	void CloseDecoration(FDreamDecoBlock& Block)
	{
		Block = FDreamDecoBlock();
	}

	bool CanOfferUndo(const FDreamRunRecord& Rec)
	{
		return Rec.Txn.Status == EDreamTxnStatus::Recorded || Rec.Txn.Status == EDreamTxnStatus::Partial;
	}

	bool NeedsOutcomeLine(const FDreamRunRecord& Rec)
	{
		return Rec.Outcome != EDreamRunOutcome::None && Rec.Outcome != EDreamRunOutcome::Ok;
	}

	bool IsStoppedRun(const FDreamRunRecord& Rec)
	{
		return Rec.Outcome == EDreamRunOutcome::Cancelled || (Rec.Outcome == EDreamRunOutcome::NotExecuted && Rec.bStopRequested);
	}

	FString RunOutcomeText(const FDreamRunRecord* Rec, uint32 RunNumber)
	{
		return Rec != nullptr ? FString::Printf(TEXT("Run #%u %s"), RunNumber, *OutcomeText(*Rec)) : FString::Printf(TEXT("Run #%u"), RunNumber);
	}

	FString DurationText(double Seconds)
	{
		if (Seconds < 60.0)
		{
			return FString::Printf(TEXT("%.2f s"), Seconds);
		}
		const int64 Whole = static_cast<int64>(Seconds);
		return Whole < 3600
			? FString::Printf(TEXT("%lld min %lld s"), Whole / 60, Whole % 60)
			: FString::Printf(TEXT("%lld h %lld min"), Whole / 3600, (Whole / 60) % 60);
	}

	bool IsSnapshotFailure(const FDreamRunRecord& Rec)
	{
		// FDreamRunService ends the run with this detail for a failed commit and for the 2 s timeout (§10.2.5 step 4 wording).
		return Rec.Outcome == EDreamRunOutcome::NotExecuted
			&& Rec.Detail.StartsWith(DreamRunText::SnapshotFailedPrefix, ESearchCase::CaseSensitive);
	}

	FString OutcomeText(const FDreamRunRecord& Rec)
	{
		FString Text = DreamRunText::OutcomeName(Rec.Outcome).ToString();
		switch (Rec.Outcome)
		{
		case EDreamRunOutcome::Error:
		case EDreamRunOutcome::NotExecuted:
			if (!Rec.Detail.IsEmpty())
			{
				Text += TEXT(" ") + Rec.Detail;
			}
			break;
		case EDreamRunOutcome::Cancelled:
			if (Rec.Location.Line > 0)
			{
				Text += FString::Printf(TEXT(" at %s:%d"), *ShortFile(Rec.Location.File), Rec.Location.Line);
			}
			if (!Rec.Detail.IsEmpty())
			{
				Text += TEXT(" · ") + Rec.Detail;
			}
			break;
		case EDreamRunOutcome::None:
			Text = DreamRunText::StateName(Rec.State).ToString();
			break;
		default:
			break;
		}
		if (Rec.ExitCode.IsSet())
		{
			Text += FString::Printf(TEXT(" · exited (code %d)"), Rec.ExitCode.GetValue());     // RUN-18
		}
		if (Rec.bInternalError)
		{
			Text += TEXT(" · internal error (details in the editor log)");
		}
		return Text;
	}

	FText HeaderLeftText(const FDreamRunRecord* Rec, uint32 RunNumber, bool bCollapsed, bool bShowCode)
	{
		TArray<FString> Parts;
		Parts.Add(FString::Printf(TEXT("%s #%u"), bCollapsed ? TEXT("▶") : TEXT("▼"), RunNumber));
		if (Rec != nullptr)
		{
			const FString Badge = RouteBadge(Rec->Route);
			if (!Badge.IsEmpty())
			{
				Parts.Add(Badge);
			}
			if (!Rec->Badge.IsEmpty())
			{
				Parts.Add(Rec->Badge);
			}
			const FString Source = SourceAndRange(*Rec);
			if (!Source.IsEmpty())
			{
				Parts.Add(Source);
			}
			Parts.Add(Rec->Namespace == EDreamNamespaceMode::Isolated ? TEXT("Isolated") : TEXT("Session"));
			const FString Start = LocalTime(Rec->StartUtc);
			if (!Start.IsEmpty())
			{
				Parts.Add(Start);
			}
			if (bShowCode)
			{
				Parts.Add(SubmittedCode(*Rec));
			}
		}
		return FText::FromString(FString::Join(Parts, TEXT(" · ")));
	}

	FText HeaderRightText(const FDreamRunRecord* Rec)
	{
		if (Rec == nullptr)
		{
			return FText::GetEmpty();
		}
		if (Rec->Outcome == EDreamRunOutcome::None)
		{
			// The live state (U2) and the elapsed time; the line updates in place when the outcome is known (§6.4).
			const double Elapsed = FPlatformTime::Seconds() - Rec->StartSeconds;
			return FText::FromString(FString::Printf(TEXT("%s %s"), *DreamRunText::StateName(Rec->State).ToString(),
				*DurationText(FMath::Max(0.0, Elapsed))));
		}
		const double Begin = Rec->EnterSeconds > 0.0 ? Rec->EnterSeconds : Rec->StartSeconds;
		return FText::FromString(FString::Printf(TEXT("%s · %s"), *DreamRunText::OutcomeName(Rec->Outcome).ToString(),
			*DurationText(FMath::Max(0.0, Rec->EndSeconds - Begin))));
	}

	FText TransactionNotesText(const FDreamRunRecord* Rec)
	{
		if (Rec == nullptr)
		{
			return FText::GetEmpty();
		}
		TArray<FString> Clauses;
		if (Rec->Txn.bPieObjects)
		{
			Clauses.Add(TEXT("changes to PIE objects are not undoable"));
		}
		if (Rec->Txn.bImported)
		{
			Clauses.Add(TEXT("imports and reimports in this run are not undoable"));
		}
		if (Rec->Txn.bRenamed)
		{
			Clauses.Add(TEXT("asset renames and moves in this run are not undoable"));
		}
		if (Rec->Txn.Status == EDreamTxnStatus::Cleared)
		{
			Clauses.Add(TEXT("undo history cleared"));
		}
		return FText::FromString(FString::Join(Clauses, TEXT(" · ")));
	}

	FText FooterText(const FDreamRunRecord* Rec, uint32 RunNumber, const FDreamOutputStore& Store)
	{
		if (Rec == nullptr)
		{
			return FText::FromString(FString::Printf(TEXT("└ #%u"), RunNumber));
		}
		TArray<FString> Parts;
		const double Begin = Rec->EnterSeconds > 0.0 ? Rec->EnterSeconds : Rec->StartSeconds;
		Parts.Add(FString::Printf(TEXT("└ #%u %s"), RunNumber, *OutcomeText(*Rec)));
		Parts.Add(DurationText(FMath::Max(0.0, Rec->EndSeconds - Begin)));
		const bool bStoppedOrFailed = Rec->Outcome == EDreamRunOutcome::Cancelled || Rec->Outcome == EDreamRunOutcome::Error;
		if (bStoppedOrFailed && Rec->Stats.ProgressDone >= 0)
		{
			// SPEC OUT-02: "312 / 1,204 completed", from the last dream_editor.progress() the run reported.
			Parts.Add(Rec->Stats.ProgressTotal.IsSet()
				? FString::Printf(TEXT("%s / %s completed"), *FText::AsNumber(Rec->Stats.ProgressDone).ToString(),
					*FText::AsNumber(Rec->Stats.ProgressTotal.GetValue()).ToString())
				: FString::Printf(TEXT("%s completed"), *FText::AsNumber(Rec->Stats.ProgressDone).ToString()));
		}
		if (Rec->Stats.bStopDuringNative)
		{
			// §5.8: the native interval during which Stop was requested (CR-20), else the longest one.
			const double StopNative = Rec->Stats.StopNativeSeconds > 0.0 ? Rec->Stats.StopNativeSeconds : Rec->Stats.LongestNativeSeconds;
			Parts.Add(Rec->Stats.StopNativeLine > 0
				? FString::Printf(TEXT("Stop took effect when the native call at line %d returned (%.1f s)"), Rec->Stats.StopNativeLine, StopNative)
				: FString::Printf(TEXT("Stop took effect when the last native call returned (%.1f s)"), StopNative));
		}
		else if (bStoppedOrFailed && Rec->Stats.NativeSeconds >= 1.0)
		{
			Parts.Add(FString::Printf(TEXT("%.0f s in native calls"), Rec->Stats.NativeSeconds));   // SPEC OUT-02 example
		}
		const FString Txn = TransactionClause(*Rec);
		if (!Txn.IsEmpty())
		{
			Parts.Add(Txn);
		}
		if (const FDreamRunSpan* Span = Store.FindRunSpan(RunNumber))
		{
			if (Span->DroppedLines > 0)
			{
				Parts.Add(FString::Printf(TEXT("%s lines not shown; full log: %s"), *FText::AsNumber(Span->DroppedLines).ToString(),
					Span->RunLogPath.IsEmpty() ? TEXT("the editor log") : *Span->RunLogPath));
			}
			if (!Span->RunLogError.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("Run log could not be written: %s"), *Span->RunLogError));
			}
			const int32 Bypass = FMath::Max(Span->BypassLines, Rec->Stats.BypassLogPythonLines);
			if (Bypass > 10000)
			{
				Parts.Add(FString::Printf(TEXT("%s lines reached LogPython through a pre-run unreal.log binding. The engine holds them in memory until the run ends (RT-07)."),
					*FText::AsNumber(Bypass).ToString()));
			}
		}
		return FText::FromString(FString::Join(Parts, TEXT(" · ")));
	}

	TSharedRef<SWidget> MakeRowWidget(const FDreamOutputRowPtr& Row, const TSharedRef<IDreamOutputRowHost>& Host)
	{
		const TWeakPtr<IDreamOutputRowHost> WeakHost = Host;
		const FSlateFontInfo& Font = Host->GetRowFont();
		switch (Row->Kind)
		{
		case EDreamOutputRowKind::Line:
			return MakeLine(Row, Host);
		case EDreamOutputRowKind::Header:
		case EDreamOutputRowKind::SyntheticHeader:
			return MakeHeader(Row, Host);
		case EDreamOutputRowKind::Footer:
			return MakeFooter(Row, Host);
		case EDreamOutputRowKind::FoldChip:
		{
			const uint32 RunNumber = Row->RunNumber;
			const int32 Statement = Row->Statement;
			const FDreamOutputRowPtr Chip = Row;
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					DreamOutputSelection::Piece(SNew(SHyperlink)
					.Text_Lambda([Chip]() { return FoldChipText(Chip->Count); })
					.TextStyle(&Host->GetLinkTextStyle())
					.ToolTipText(LOCTEXT("Out.FoldChipTip", "Show the rest of this statement's input (Enter)"))
					.OnNavigate_Lambda([WeakHost, RunNumber, Statement]()
					{
						if (const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin())
						{
							Pinned->ExpandFold(RunNumber, Statement);
						}
					}))
				];
		}
		case EDreamOutputRowKind::ResultMore:
		{
			TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
			AddPlain(Box, Row->Text + DecorationSuffix(*Row), Font, FDreamEditorStyle::GetLineTypeColor(dream::LineType::Result), Host->IsSoftWrap());
			return Box;
		}
		case EDreamOutputRowKind::Eviction:
			return MakeEviction(Host);
		case EDreamOutputRowKind::OutsideRuns:
			return DreamOutputSelection::Piece(SNew(STextBlock).Text(LOCTEXT("Out.OutsideRuns", "─ Outside runs")).Font(Font)
				.ColorAndOpacity(FDreamEditorStyle::GetLineTypeColor(dream::LineType::RunHeader)));
		case EDreamOutputRowKind::StartupHeader:
		{
			const int32 Count = Row->Count;
			return SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), TEXT("NoBorder"))
				.ContentPadding(FMargin(0.f))
				.IsFocusable(false)
				.OnClicked_Lambda([WeakHost]()
				{
					if (const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin())
					{
						Pinned->ToggleStartupExpanded();
					}
					return FReply::Handled();
				})
				[
					DreamOutputSelection::Piece(SNew(STextBlock)
					.Font(Font)
					.ColorAndOpacity(FDreamEditorStyle::GetLineTypeColor(dream::LineType::Warning))
					.Text_Lambda([WeakHost, Count]()
					{
						const TSharedPtr<IDreamOutputRowHost> Pinned = WeakHost.Pin();
						return StartupHeaderText(Pinned.IsValid() && Pinned->IsStartupExpanded(), Count);
					}))
				];
		}
		case EDreamOutputRowKind::StartupLine:
		{
			const dream::LineType Type = Row->Verbosity == ELogVerbosity::Error ? dream::LineType::Error : dream::LineType::Warning;
			TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
			AddPlain(Box, StartupLineText(*Row), Font, FDreamEditorStyle::GetLineTypeColor(Type), Host->IsSoftWrap());
			return Box;
		}
		case EDreamOutputRowKind::Provisional:
		{
			TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
			AddPlain(Box, Row->Text, Font, FDreamEditorStyle::GetLineTypeColor(dream::LineType::Stdout), Host->IsSoftWrap());
			return Box;
		}
		case EDreamOutputRowKind::RunOutcome:
		{
			bool bStopped = false;
			const FString Text = RunOutcomeLine(*Host, Row->RunNumber, bStopped);
			TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
			AddPlain(Box, Text, Font, FDreamEditorStyle::GetLineTypeColor(bStopped ? dream::LineType::Warning : dream::LineType::Error),
				Host->IsSoftWrap());
			return Box;
		}
		}
		return SNew(STextBlock);
	}

	FString GetRowRawText(const FDreamOutputRow& Row, const IDreamOutputRowHost& Host)
	{
		const FDreamOutputStore& Store = Host.GetStore();
		switch (Row.Kind)
		{
		case EDreamOutputRowKind::Line:
		{
			dream::LineView View;
			if (!Store.GetLine(Row.Seq, View))
			{
				return FString();
			}
			FString Text = JoinedText(Row, Store, View.Text);
			if ((View.Info.Flags & dream::OLF_Truncated) != 0)
			{
				Text += Store.GetResultBlob(Row.Seq);     // "Copy always yields the raw repr without the prefix" (§6.4)
			}
			return Text;
		}
		case EDreamOutputRowKind::Header:
		case EDreamOutputRowKind::SyntheticHeader:
		{
			const FDreamRunRecord* Rec = FindRecord(Host, Row.RunNumber);
			return HeaderLeftText(Rec, Row.RunNumber, false, false).ToString() + TEXT("  ") + HeaderRightText(Rec).ToString();
		}
		case EDreamOutputRowKind::Footer:
			return FooterText(FindRecord(Host, Row.RunNumber), Row.RunNumber, Store).ToString();
		case EDreamOutputRowKind::RunOutcome:
			return RunOutcomeText(FindRecord(Host, Row.RunNumber), Row.RunNumber);   // copy is raw: no decoration (§6.4)
		case EDreamOutputRowKind::StartupLine:
		case EDreamOutputRowKind::Provisional:
		case EDreamOutputRowKind::ResultMore:
			return Row.Text;
		default:
			return FString();
		}
	}

	FString GetRowDisplayText(const FDreamOutputRow& Row, const IDreamOutputRowHost& Host)
	{
		const FDreamOutputStore& Store = Host.GetStore();
		switch (Row.Kind)
		{
		case EDreamOutputRowKind::Line:
		{
			dream::LineView View;
			if (!Store.GetLine(Row.Seq, View))
			{
				return LOCTEXT("Out.DroppedLine", "(line dropped)").ToString();
			}
			return DecorationPrefix(Row) + JoinedText(Row, Store, View.Text) + DecorationSuffix(Row);
		}
		case EDreamOutputRowKind::Header:
		case EDreamOutputRowKind::SyntheticHeader:
			return HeaderLeft(Host, Row.RunNumber, Row.Kind == EDreamOutputRowKind::SyntheticHeader).ToString() + TEXT("  ")
				+ HeaderRightText(FindRecord(Host, Row.RunNumber)).ToString();
		case EDreamOutputRowKind::Footer:
			return FooterText(FindRecord(Host, Row.RunNumber), Row.RunNumber, Store).ToString();
		case EDreamOutputRowKind::FoldChip:
			return FoldChipText(Row.Count).ToString();
		case EDreamOutputRowKind::ResultMore:
			return Row.Text + DecorationSuffix(Row);
		case EDreamOutputRowKind::Eviction:
			return EvictionText(Host).ToString();
		case EDreamOutputRowKind::OutsideRuns:
			return LOCTEXT("Out.OutsideRuns", "─ Outside runs").ToString();
		case EDreamOutputRowKind::StartupHeader:
			return StartupHeaderText(Host.IsStartupExpanded(), Row.Count).ToString();
		case EDreamOutputRowKind::StartupLine:
			return StartupLineText(Row);
		case EDreamOutputRowKind::Provisional:
			return Row.Text;
		case EDreamOutputRowKind::RunOutcome:
		{
			bool bStopped = false;
			return RunOutcomeLine(Host, Row.RunNumber, bStopped);
		}
		}
		return FString();
	}
}

#undef LOCTEXT_NAMESPACE
