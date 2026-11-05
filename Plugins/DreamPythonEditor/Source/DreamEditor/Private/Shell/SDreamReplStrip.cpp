// SPDX-License-Identifier: Apache-2.0
// The REPL prompt strip (TDD §6.5 keys, completeness, submission, REPL state by run state, history popup; §4.14 prompt
// mode; REPL-01, REPL-02 basic, REPL-06). See SDreamReplStrip.h.
// D14(5): no key handler calls Python. Enter's decision uses dream::ReplPreCheck in C++ and, when that cannot decide,
// FDreamPython::ClassifyInput from the next idle FDreamDeferral tick; keys typed meanwhile are queued and replayed.
#include "Shell/SDreamReplStrip.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamNumpadEnter.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/IMenu.h"
#include "Framework/Application/SlateApplication.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Python/DreamPython.h"
#include "Rendering/SlateRenderer.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamTheme.h"   // (theme)
#include "Styling/AppStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "dream/StatementExtent.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamReplPrivate
{
	constexpr double DecisionTimeoutSeconds = 10.0;     // a boot slower than this submits as Ctrl+Enter would
	constexpr int32 HistoryPopupMax = 200;
	constexpr int32 HistoryPopupVisibleRows = 12;

	/** Keys the stock text layout handles through its line views (MoveCursor's vertical moves, JumpTo), any modifiers. */
	bool MovesByLineViews(const FKey& Key)
	{
		return Key == EKeys::Up || Key == EKeys::Down || Key == EKeys::PageUp || Key == EKeys::PageDown || Key == EKeys::Home
			|| Key == EKeys::End;
	}

	bool IsPythonUnusable(EDreamPythonState State)
	{
		switch (State)
		{
		case EDreamPythonState::Unavailable:
		case EDreamPythonState::Headless:
		case EDreamPythonState::Disabled:
		case EDreamPythonState::BootFailed:
		case EDreamPythonState::ShuttingDown:
		case EDreamPythonState::ShutDown:
			return true;
		default:
			return false;
		}
	}

	bool IsRunBlockingPrompt(EDreamRunState State)
	{
		// §6.5 "REPL state by run state": read-only in Starting, Running, Stopping… and Paused; editable in Task running.
		// Deferred (M3): the Paused-window footer message "The REPL is suspended; use Evaluate" (REPL-07, §8.3.5).
		return State == EDreamRunState::Starting || State == EDreamRunState::Running
			|| State == EDreamRunState::Stopping || State == EDreamRunState::Paused;
	}

	TArray<FString> SplitLines(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty*/ false);
		if (Lines.IsEmpty())
		{
			Lines.Add(FString());
		}
		return Lines;
	}

	FString LeadingWhitespace(const FString& Line)
	{
		int32 Count = 0;
		while (Count < Line.Len() && (Line[Count] == TEXT(' ') || Line[Count] == TEXT('\t')))
		{
			++Count;
		}
		return Line.Left(Count);
	}

	FString HistoryLabel(const FDreamHistoryEntry& Entry)
	{
		TArray<FString> Lines = SplitLines(Entry.Text);
		FString Label = Lines[0];
		if (Lines.Num() > 1)
		{
			Label += FString::Printf(TEXT("  (+%d lines)"), Lines.Num() - 1);
		}
		return Label;
	}
}

DECLARE_DELEGATE_OneParam(FDreamOnHistoryAccept, const FString&);

/** The history popup (§6.5): newest at the bottom; Enter puts the entry in the prompt without running it; Esc closes. */
class SDreamReplHistoryPopup : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamReplHistoryPopup) {}
		SLATE_ARGUMENT(TArray<FDreamHistoryEntry>, Entries)
		SLATE_ARGUMENT(FSlateFontInfo, Font)
		SLATE_ARGUMENT(float, RowHeight)
		SLATE_EVENT(FDreamOnHistoryAccept, OnAccept)
		SLATE_EVENT(FSimpleDelegate, OnCancel)
	SLATE_END_ARGS()

	using FItem = TSharedPtr<FDreamHistoryEntry>;

	void Construct(const FArguments& InArgs)
	{
		OnAccept = InArgs._OnAccept;
		OnCancel = InArgs._OnCancel;
		for (const FDreamHistoryEntry& Entry : InArgs._Entries)
		{
			Items.Add(MakeShared<FDreamHistoryEntry>(Entry));
		}
		const FSlateFontInfo Font = InArgs._Font;
		const float Height = FMath::Min(Items.Num(), DreamReplPrivate::HistoryPopupVisibleRows) * (InArgs._RowHeight + 4.f) + 8.f;

		// (output, review r6 M2) The theme's chrome, not the stock Menu.Background (#383838 under a light theme's dark
		// ChromeText): ChromeBackground inside a 1-px ChromeBorder outline, ChromeText as the foreground.
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBorder))
			.Padding(FMargin(1.f))
			[
			SNew(SBorder)
			.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground))
			.ForegroundColor(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
			.Padding(FMargin(1.f))
			[
				SNew(SBox)
				.WidthOverride(560.f)
				.HeightOverride(Height)
				[
					SAssignNew(List, SListView<FItem>)
					.ListViewStyle(&FDreamThemes::ListViewStyle())   // (theme) no dark list background under a light theme
					.ScrollBarStyle(&FDreamThemes::ScrollBarStyle())   // (theme)
					.ListItemsSource(&Items)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow_Lambda([Font](FItem Item, const TSharedRef<STableViewBase>& Owner) -> TSharedRef<ITableRow>
					{
						const bool bImported = Item->Source == EDreamHistorySource::UeConsole;
						return SNew(STableRow<FItem>, Owner)
							.Style(&FDreamThemes::TableRowStyle())   // (theme)
							.Padding(FMargin(4.f, 1.f))
							.ToolTipText(FText::FromString(Item->Text))
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().FillWidth(1.f)
								[
									SNew(STextBlock).Text(FText::FromString(DreamReplPrivate::HistoryLabel(*Item))).Font(Font)
								]
								+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(8.f, 0.f, 2.f, 0.f))
								[
									SNew(STextBlock)
									.Text(bImported ? LOCTEXT("Out.UeConsoleBadge", "UE console") : FText::GetEmpty())
									.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeMutedText))
								]
							];
					})
					.OnMouseButtonDoubleClick_Lambda([this](FItem Item) { Accept(Item); })
					.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent)
					{
						if (KeyEvent.GetKey() == EKeys::Enter)
						{
							const TArray<FItem> Selected = List->GetSelectedItems();
							Accept(Selected.IsEmpty() ? FItem() : Selected[0]);
							return FReply::Handled();
						}
						if (KeyEvent.GetKey() == EKeys::Escape)
						{
							OnCancel.ExecuteIfBound();
							return FReply::Handled();
						}
						return FReply::Unhandled();
					})
				]
			]
			]
		];
		if (!Items.IsEmpty())
		{
			List->SetSelection(Items.Last(), ESelectInfo::Direct);
			List->RequestScrollIntoView(Items.Last());
		}
	}

	TSharedRef<SWidget> GetFocusTarget() const { return List.ToSharedRef(); }

private:
	void Accept(const FItem& Item)
	{
		if (Item.IsValid())
		{
			OnAccept.ExecuteIfBound(Item->Text);
		}
		else
		{
			OnCancel.ExecuteIfBound();
		}
	}

	TArray<FItem> Items;
	TSharedPtr<SListView<FItem>> List;
	FDreamOnHistoryAccept OnAccept;
	FSimpleDelegate OnCancel;
};

// ---------------------------------------------------------------------------------------------------------------------

void SDreamReplStrip::Construct(const FArguments& InArgs)
{
	ForwardTarget = InArgs._ForwardTarget;
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	Buffer = Module.GetBufferManager().GetReplInputBuffer();     // journaled as "repl-input" (§6.5, T-REPL-016)

	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	const int32 FontSize = Settings ? Settings->FontSize : 10;
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	LineHeight = FMath::Max(8.f, static_cast<float>(Measure->GetMaxCharacterHeight(FDreamEditorStyle::GetCodeFont(FontSize))));

	const TSharedRef<SDreamReplStrip> Self = SharedThis(this);
	const TSharedRef<IDreamCommandTarget> Target = Self;
	const TWeakPtr<SDreamReplStrip> Weak = Self;

	// (output, review r6 m7 and arch-r6-m1) A 1-px ChromeBorder rule above and below the prompt, on the input pane's
	// EditorBackground; the stock ToolPanel.GroupBorder drew 2-px #242424 bands in every theme.
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBorder))
		.Padding(FMargin(0.f, 1.f))
		[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::EditorBackground))
		.Padding(FMargin(0.f, 1.f))
		[
			// Auto-height up to 12 lines, then the code view scrolls (§6.5, proposed).
			SNew(SBox)
			.MinDesiredHeight(LineHeight + 6.f)
			.MaxDesiredHeight(MaxAutoHeightLines * LineHeight + 6.f)
			[
				SAssignNew(PromptView, SDreamCodeView)
				.Buffer(Buffer)
				.CommandTarget(Target)
				.GutterMode(EDreamGutterMode::Prompt)
				.ContextMask(dream::Ctx_Prompt)
				.FontSize(FontSize)
				.IsReadOnly_Lambda([Weak]()
				{
					const TSharedPtr<SDreamReplStrip> Pinned = Weak.Pin();
					return !Pinned.IsValid() || Pinned->IsPromptReadOnly();
				})
				.HintText_Lambda([Weak]()
				{
					const TSharedPtr<SDreamReplStrip> Pinned = Weak.Pin();
					return Pinned.IsValid() ? Pinned->GetHintText() : FText::GetEmpty();
				})
				.OnKeyHook(this, &SDreamReplStrip::HandlePromptKey)
				.OnFocused_Lambda([]()
				{
					if (FDreamEditorModule::IsAvailable())
					{
						FDreamEditorModule::Get().GetPython().RequestStart(EDreamStartReason::Focus);    // RT-17
					}
				})
			]
		]
		]
	];
}

SDreamReplStrip::~SDreamReplStrip()
{
	RemoveDecisionTickers();
	CloseHistory();
}

FString SDreamReplStrip::GetPromptText() const
{
	return PromptView.IsValid() ? PromptView->GetText() : FString();
}

bool SDreamReplStrip::IsPromptReadOnly() const
{
	if (bClassifyPending || !FDreamEditorModule::IsAvailable())
	{
		return true;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	return DreamReplPrivate::IsRunBlockingPrompt(Module.GetRunService().GetState())
		|| DreamReplPrivate::IsPythonUnusable(Module.GetPython().GetState());
}

FText SDreamReplStrip::GetHintText() const
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return FText::GetEmpty();
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	FDreamRunService& RunService = Module.GetRunService();
	if (DreamReplPrivate::IsRunBlockingPrompt(RunService.GetState()))
	{
		const FDreamRunRecord* Active = RunService.GetActiveRecord();
		return FText::Format(LOCTEXT("Out.ReplRunActive", "Run #{0} is active"), FText::AsNumber(Active ? Active->RunNumber : 0));
	}
	if (DreamReplPrivate::IsPythonUnusable(Module.GetPython().GetState()))
	{
		return Module.GetPython().GetDisabledReason();
	}
	// (chrome) PO-04: a short placeholder instead of the §6.5 key summary (the keys themselves are unchanged).
	return LOCTEXT("Out.ReplHintShort", "Python");
}

FReply SDreamReplStrip::HandlePromptKey(const FKeyEvent& KeyEvent)
{
	if (bClassifyPending)
	{
		// §6.5 type-ahead: queued (up to 64) and replayed in order once the decision is made.
		if (TypeAhead.Num() < TypeAheadMaxKeys)
		{
			TypeAhead.Add(KeyEvent);
		}
		return FReply::Handled();
	}
	const FKey Key = KeyEvent.GetKey();
	const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
	const bool bCtrl = Mods.IsControlDown();
	const bool bAlt = Mods.IsAltDown();
	const bool bShift = Mods.IsShiftDown();

	if (DreamReplPrivate::MovesByLineViews(Key))
	{
		RefreshPromptLayout();               // the text may have been set whole earlier in this frame (see the header)
	}
	if (Key == EKeys::Enter)
	{
		// Numpad Enter submits like Ctrl+Enter, only while the G7 hooks work; it never acts as main Enter (T-REPL-015).
		const FDreamNumpadEnter& Numpad = FDreamNumpadEnter::Get();
		if ((bCtrl && !bAlt && !bShift) || (Numpad.IsAvailable() && Numpad.IsCurrentEnterNumpad()))
		{
			Submit(EDreamReplSubmit::Force);
			return FReply::Handled();
		}
		if (IsPromptReadOnly())
		{
			return FReply::Handled();
		}
		if (bShift && !bCtrl && !bAlt)
		{
			InsertNewlineAndIndent();
			return FReply::Handled();
		}
		if (!bCtrl && !bAlt && !bShift)
		{
			HandleEnter();
			return FReply::Handled();
		}
		return FReply::Unhandled();         // Alt+Enter and the rest: the key router (RunLineAdvance submits too)
	}
	if (Key == EKeys::Up && !bAlt && !bShift)
	{
		if (bCtrl)
		{
			OpenHistory(GetPromptText(), EDreamHistoryMatch::Prefix);   // Ctrl+Up: filtered by the prompt text (D8)
			return FReply::Handled();
		}
		if (GetPromptText().IsEmpty())
		{
			OpenHistory(FString(), EDreamHistoryMatch::Prefix);
			return FReply::Handled();
		}
		return FReply::Unhandled();         // Up with text moves the caret (stock)
	}
	// Deferred (M2): Ctrl+R substring search and typing to filter the history popup (REPL-02).
	if (Key == EKeys::Escape && !bCtrl && !bAlt && !bShift && !GetPromptText().IsEmpty() && !IsPromptReadOnly())
	{
		ClearPromptUndoable();               // Esc clears; Ctrl+Z restores it as a text undo (§6.5)
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SDreamReplStrip::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	if (bClassifyPending)
	{
		if (TypeAhead.Num() < TypeAheadMaxKeys)
		{
			TypeAhead.Add(FKeyEvent(EKeys::Invalid, InCharacterEvent.GetModifierKeys(), InCharacterEvent.GetUserIndex(),
				InCharacterEvent.IsRepeat(), static_cast<uint32>(InCharacterEvent.GetCharacter()), 0));
		}
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyChar(MyGeometry, InCharacterEvent);
}

void SDreamReplStrip::HandleEnter()
{
	const FString Text = GetPromptText();
	if (Text.TrimStartAndEnd().IsEmpty())
	{
		return;                              // Enter on an empty prompt does nothing
	}
	const TArray<FString> Lines = DreamReplPrivate::SplitLines(Text);
	int32 CaretLine = 0;
	int32 CaretColumn = 0;
	PromptView->GetCaret(CaretLine, CaretColumn);
	if (CaretLine < Lines.Num() - 1)
	{
		InsertNewlineAndIndent();            // not on the last line: newline and auto-indent
		return;
	}
	// The completeness flowchart of §6.5.
	if (Lines.Num() > 1 && Lines.Last().TrimStartAndEnd().IsEmpty())
	{
		// A blank Enter: drop the blank line and append a newline, so the source ends with a blank line.
		TArray<FString> Kept = Lines;
		Kept.Pop();
		QueueClassify(FString::Join(Kept, TEXT("\n")) + TEXT("\n"));
		return;
	}
	if (dream::ReplPreCheck(DreamText::ToU16(Text)) == dream::ReplPrecheck::DefinitelyIncomplete)
	{
		InsertNewlineAndIndent();
		return;
	}
	QueueClassify(Text);
}

void SDreamReplStrip::QueueClassify(const FString& Source)
{
	bClassifyPending = true;
	PendingSource = Source;
	PendingSince = FPlatformTime::Seconds();
	TypeAhead.Reset();
	FDreamPython& Python = FDreamEditorModule::Get().GetPython();
	if (Python.GetState() == EDreamPythonState::Cold)
	{
		Python.RequestStart(EDreamStartReason::Keystroke);
	}
	// D14(5), INV-6: classify_input runs only from an idle FDreamDeferral tick that needs Python, never from this key
	// handler: not while a run is active or a pump is on the stack, only once Python is Ready, within the 5 ms helper
	// budget of an idle frame (§3.7).
	RemoveDecisionTickers();
	const TWeakPtr<SDreamReplStrip> Weak = SharedThis(this);
	ClassifyTicker = FDreamDeferral::AddTicker(TEXT("DreamReplClassify"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
		/*bNeedsPython*/ true, [Weak](float)
		{
			const TSharedPtr<SDreamReplStrip> Pinned = Weak.Pin();
			return Pinned.IsValid() && Pinned->TickClassify();
		});
	// That body never runs while Python is not Ready, so a Python-free tick keeps a failed or slow boot from leaving the
	// prompt waiting.
	ClassifyWaitTicker = FDreamDeferral::AddTicker(TEXT("DreamReplClassifyWait"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
		/*bNeedsPython*/ false, [Weak](float)
		{
			const TSharedPtr<SDreamReplStrip> Pinned = Weak.Pin();
			return Pinned.IsValid() && Pinned->TickClassifyWait();
		});
}

bool SDreamReplStrip::TickClassify()
{
	if (!bClassifyPending)
	{
		return false;
	}
	FDreamPython& Python = FDreamEditorModule::Get().GetPython();
	const bool bIndented = dream::LastLogicalLineIndented(DreamText::ToU16(PendingSource));
	const TValueOrError<EDreamReplClassify, FText> Result = Python.ClassifyInput(PendingSource, bIndented);
	if (Result.HasError())
	{
		UE_LOG(LogDream, Warning, TEXT("REPL: classify_input failed (%s); submitting."), *Result.GetError().ToString());
	}
	// "complete" and "invalid" submit (the run shows the SyntaxError); an error result counts as complete, so the input
	// is submitted and the run reports the problem (§6.5, §6.7 "codeop classification raises something unexpected").
	FinishDecision(!(Result.HasValue() && Result.GetValue() == EDreamReplClassify::Incomplete));
	return false;
}

bool SDreamReplStrip::TickClassifyWait()
{
	if (!bClassifyPending)
	{
		return false;
	}
	FDreamPython& Python = FDreamEditorModule::Get().GetPython();
	const EDreamPythonState State = Python.GetState();
	if (State == EDreamPythonState::Ready)
	{
		return true;                         // the classify tick decides at the next idle frame with helper budget
	}
	if (DreamReplPrivate::IsPythonUnusable(State) || FPlatformTime::Seconds() - PendingSince > DreamReplPrivate::DecisionTimeoutSeconds)
	{
		FinishDecision(/*bSubmit*/ true);   // Submit refuses with the reason (the text stays) or queues the run
		return false;
	}
	if (State == EDreamPythonState::Cold)
	{
		Python.RequestStart(EDreamStartReason::Keystroke);
	}
	return true;
}

void SDreamReplStrip::RemoveDecisionTickers()
{
	// FTSTicker::RemoveTicker is safe from inside the body being removed (it only waits for other threads' execution).
	if (ClassifyTicker.IsValid())
	{
		FDreamDeferral::RemoveTicker(ClassifyTicker);
		ClassifyTicker.Reset();
	}
	if (ClassifyWaitTicker.IsValid())
	{
		FDreamDeferral::RemoveTicker(ClassifyWaitTicker);
		ClassifyWaitTicker.Reset();
	}
}

void SDreamReplStrip::FinishDecision(bool bSubmit)
{
	bClassifyPending = false;
	RemoveDecisionTickers();                 // before the submission and the replay, which may queue a new decision
	if (bSubmit)
	{
		SubmitText(PendingSource);
	}
	else
	{
		InsertNewlineAndIndent();
	}
	PendingSource.Reset();
	ReplayTypeAhead();
}

void SDreamReplStrip::ReplayTypeAhead()
{
	TArray<FKeyEvent> Keys = MoveTemp(TypeAhead);
	TypeAhead.Reset();
	FSlateApplication& Slate = FSlateApplication::Get();
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		if (bClassifyPending)
		{
			// Another Enter started a decision: the rest waits for that one, still in order.
			TypeAhead.Append(Keys.GetData() + Index, Keys.Num() - Index);
			return;
		}
		const FKeyEvent& Key = Keys[Index];
		if (Key.GetKey() == EKeys::Invalid)
		{
			Slate.ProcessKeyCharEvent(FCharacterEvent(static_cast<TCHAR>(Key.GetCharacter()), Key.GetModifierKeys(),
				Key.GetUserIndex(), Key.IsRepeat()));
		}
		else
		{
			Slate.ProcessKeyDownEvent(Key);
		}
	}
}

void SDreamReplStrip::Submit(EDreamReplSubmit Mode)
{
	if (bClassifyPending)
	{
		return;
	}
	const FString Text = GetPromptText();
	if (Text.TrimStartAndEnd().IsEmpty())
	{
		return;
	}
	if (Mode == EDreamReplSubmit::Auto)
	{
		HandleEnter();
		return;
	}
	SubmitText(Text);                        // Force: incomplete input ends not executed with its SyntaxError (RUN-16)
}

void SDreamReplStrip::SubmitText(const FString& Text)
{
	FString Code = Text;
	Code.TrimEndInline();
	if (Code.IsEmpty())
	{
		return;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	FDreamRunRequest Request;
	Request.Route = EDreamRunRoute::Repl;     // Session, auto-print, statement mode, "Dream: REPL #N", <dream:REPL#N>
	Request.TabTitle = TEXT("REPL");
	Request.Text = Code;
	Request.FirstLine = 1;
	Request.Namespace = EDreamNamespaceMode::Session;
	const FDreamSubmitResult Result = Module.GetRunService().Submit(MoveTemp(Request));
	if (!Result.bAccepted)
	{
		ShowCommandRefusal(EDreamCommand::None, Result.Reason);     // a refused submission keeps its text
		return;
	}
	// On acceptance the prompt clears into history, a not executed input included, so Up recalls it (D8).
	Module.GetReplHistory().Add(Code, EDreamHistorySource::Repl);
	PromptView->SetTextNoUndo(FString());
	RefreshPromptLayout();                   // the type-ahead replay right after this may move the caret
}

void SDreamReplStrip::RefreshPromptLayout()
{
	if (PromptView.IsValid())
	{
		PromptView->SlatePrepass(GetPrepassLayoutScaleMultiplier());
	}
}

FReply SDreamReplStrip::OnPreviewMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	RefreshPromptLayout();
	return FReply::Unhandled();
}

void SDreamReplStrip::InsertNewlineAndIndent()
{
	const TArray<FString> Lines = DreamReplPrivate::SplitLines(GetPromptText());
	int32 CaretLine = 0;
	int32 CaretColumn = 0;
	PromptView->GetCaret(CaretLine, CaretColumn);
	const FString Line = Lines.IsValidIndex(CaretLine) ? Lines[CaretLine] : FString();
	FString Indent = DreamReplPrivate::LeadingWhitespace(Line);
	const FString BeforeCaret = Line.Left(FMath::Clamp(CaretColumn, 0, Line.Len())).TrimEnd();
	if (BeforeCaret.EndsWith(TEXT(":")))
	{
		Indent += TEXT("    ");
	}
	PromptView->InsertAtCaret(TEXT("\n") + Indent);
}

void SDreamReplStrip::ClearPromptUndoable()
{
	const TArray<FString> Lines = DreamReplPrivate::SplitLines(GetPromptText());
	PromptView->ReplaceLines(0, Lines.Num() - 1, FString());
}

void SDreamReplStrip::OpenHistory(const FString& Filter, EDreamHistoryMatch Match)
{
	CloseHistory();
	const TArray<FDreamHistoryEntry> Entries = FDreamEditorModule::Get().GetReplHistory().Query(Filter, Match, DreamReplPrivate::HistoryPopupMax);
	if (Entries.IsEmpty())
	{
		ShowCommandRefusal(EDreamCommand::None, Filter.IsEmpty() ? LOCTEXT("Out.NoHistory", "No REPL history yet")
			: FText::Format(LOCTEXT("Out.NoHistoryMatch", "No history entry starts with \"{0}\""), FText::FromString(Filter)));
		return;
	}
	OpenHistoryWith(Entries);
}

TSharedPtr<SWidget> SDreamReplStrip::OpenHistoryForTest(const TArray<FDreamHistoryEntry>& Entries)
{
	CloseHistory();
	return Entries.IsEmpty() ? TSharedPtr<SWidget>() : OpenHistoryWith(Entries);
}

TSharedPtr<SWidget> SDreamReplStrip::OpenHistoryWith(const TArray<FDreamHistoryEntry>& Entries)
{
	const TWeakPtr<SDreamReplStrip> Weak = SharedThis(this);
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	TSharedRef<SDreamReplHistoryPopup> Popup = SNew(SDreamReplHistoryPopup)
		.Entries(Entries)
		.Font(FDreamEditorStyle::GetCodeFont(Settings ? Settings->FontSize : 10))
		.RowHeight(LineHeight)
		.OnAccept_Lambda([Weak](const FString& Text)
		{
			if (const TSharedPtr<SDreamReplStrip> Pinned = Weak.Pin())
			{
				Pinned->CloseHistory();
				Pinned->PutInPrompt(Text);       // Enter puts the entry in the prompt without running it (§6.5)
			}
		})
		.OnCancel_Lambda([Weak]()
		{
			if (const TSharedPtr<SDreamReplStrip> Pinned = Weak.Pin())
			{
				Pinned->CloseHistory();
				Pinned->FocusPrompt();
			}
		});

	// Above the strip, so the newest entry sits next to the prompt (newest at the bottom).
	const FGeometry& Geometry = GetCachedGeometry();
	const float PopupHeight = FMath::Min(Entries.Num(), DreamReplPrivate::HistoryPopupVisibleRows) * (LineHeight + 4.f) + 12.f;
	const FVector2D Location(Geometry.GetAbsolutePosition().X, Geometry.GetAbsolutePosition().Y - PopupHeight * Geometry.Scale);
	HistoryMenu = FSlateApplication::Get().PushMenu(SharedThis(this), FWidgetPath(), Popup, Location,
		FPopupTransitionEffect(FPopupTransitionEffect::None), /*bFocusImmediately*/ true);
	FSlateApplication::Get().SetKeyboardFocus(Popup->GetFocusTarget(), EFocusCause::SetDirectly);
	return Popup;
}

void SDreamReplStrip::PutInPrompt(const FString& Text)
{
	if (!PromptView.IsValid() || IsPromptReadOnly())
	{
		return;
	}
	const TArray<FString> Lines = DreamReplPrivate::SplitLines(GetPromptText());
	PromptView->ReplaceLines(0, Lines.Num() - 1, Text);
	const TArray<FString> NewLines = DreamReplPrivate::SplitLines(Text);
	PromptView->SetCaret(NewLines.Num() - 1, NewLines.Last().Len());
	FocusPrompt();
}

void SDreamReplStrip::CloseHistory()
{
	if (HistoryMenu.IsValid())
	{
		const TSharedPtr<IMenu> Menu = HistoryMenu;
		HistoryMenu.Reset();
		Menu->Dismiss();
	}
}

void SDreamReplStrip::FocusPrompt()
{
	if (PromptView.IsValid())
	{
		PromptView->FocusEditor();
	}
}

bool SDreamReplStrip::CanExecuteDreamCommand(EDreamCommand Command, FText& OutReason) const
{
	switch (Command)
	{
	case EDreamCommand::RunSelectionOrBuffer:
	case EDreamCommand::RunBuffer:
	case EDreamCommand::RunLine:
	case EDreamCommand::RunLineAdvance:
		if (GetPromptText().TrimStartAndEnd().IsEmpty())
		{
			OutReason = LOCTEXT("Out.PromptEmpty", "The REPL prompt is empty");
			return false;
		}
		return FDreamEditorModule::Get().GetRunService().CanSubmit(EDreamRunRoute::Repl, OutReason);
	case EDreamCommand::ZoomIn:
	case EDreamCommand::ZoomOut:
	case EDreamCommand::ZoomReset:
	case EDreamCommand::FocusRepl:
		return true;
	default:
		if (const TSharedPtr<IDreamCommandTarget> Target = ForwardTarget.Pin())
		{
			return Target->CanExecuteDreamCommand(Command, OutReason);
		}
		OutReason = LOCTEXT("Out.NoTarget", "Not available in the REPL");
		return false;
	}
}

void SDreamReplStrip::ExecuteDreamCommand(EDreamCommand Command)
{
	switch (Command)
	{
	case EDreamCommand::RunSelectionOrBuffer:
	case EDreamCommand::RunBuffer:
	case EDreamCommand::RunLine:
	case EDreamCommand::RunLineAdvance:
		Submit(EDreamReplSubmit::Force);      // in the prompt every run chord runs the prompt
		break;
	case EDreamCommand::ZoomIn:
		PromptView->Zoom(+1);
		break;
	case EDreamCommand::ZoomOut:
		PromptView->Zoom(-1);
		break;
	case EDreamCommand::ZoomReset:
		PromptView->Zoom(0);
		break;
	case EDreamCommand::FocusRepl:
		FocusPrompt();
		break;
	default:
		if (const TSharedPtr<IDreamCommandTarget> Target = ForwardTarget.Pin())
		{
			Target->ExecuteDreamCommand(Command);
		}
		break;
	}
}

void SDreamReplStrip::ShowCommandRefusal(EDreamCommand Command, const FText& Reason)
{
	if (const TSharedPtr<IDreamCommandTarget> Target = ForwardTarget.Pin())
	{
		Target->ShowCommandRefusal(Command, Reason);
		return;
	}
	if (FDreamEditorModule::IsAvailable())
	{
		if (const TSharedPtr<SDreamTab> Tab = FDreamEditorModule::Get().GetTab())
		{
			Tab->ShowStatusMessage(Reason);
			return;
		}
	}
	UE_LOG(LogDream, Log, TEXT("REPL: %s"), *Reason.ToString());
}

#undef LOCTEXT_NAMESPACE
