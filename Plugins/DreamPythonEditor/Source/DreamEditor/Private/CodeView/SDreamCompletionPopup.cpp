// SPDX-License-Identifier: Apache-2.0
// The completion popup and its controller (TDD §7.6.2, D6, D8, REPL-03). See SDreamCompletionPopup.h. Game thread. Nothing
// here calls Python: candidates come from FDreamCompletionService's C++ caches at once and from its idle-tick resolver
// later (D14(5), RV-23); a keystroke never waits for Python (RT-17).
#include "CodeView/SDreamCompletionPopup.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamCommands.h"
#include "Commands/DreamNumpadEnter.h"
#include "Framework/Application/IMenu.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Shell/DreamTheme.h"   // (theme)
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "dream/PyLexer.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamCompletionPopupLocal
{
	FText KindGlyph(dream::CompletionKind Kind)
	{
		switch (Kind)
		{
		case dream::CompletionKind::Function:
		case dream::CompletionKind::Method:
			return LOCTEXT("Completion.KindFunction", "f");
		case dream::CompletionKind::Class:
			return LOCTEXT("Completion.KindClass", "C");
		case dream::CompletionKind::Module:
			return LOCTEXT("Completion.KindModule", "M");
		case dream::CompletionKind::Property:
		case dream::CompletionKind::Field:
			return LOCTEXT("Completion.KindProperty", "p");
		case dream::CompletionKind::EditorProperty:
			return LOCTEXT("Completion.KindEditorProperty", "e");
		case dream::CompletionKind::Constant:
			return LOCTEXT("Completion.KindConstant", "c");
		case dream::CompletionKind::EnumMember:
			return LOCTEXT("Completion.KindEnumMember", "E");
		case dream::CompletionKind::Keyword:
			return LOCTEXT("Completion.KindKeyword", "k");
		case dream::CompletionKind::Snippet:
			return LOCTEXT("Completion.KindSnippet", "s");
		case dream::CompletionKind::Variable:
			return LOCTEXT("Completion.KindVariable", "v");
		default:
			return FText::GetEmpty();
		}
	}

	/** The U4 badges (T-MRG-003): runtime, static, keyword, snippet. */
	FText BadgeText(dream::CompletionBadge Badge)
	{
		switch (Badge)
		{
		case dream::CompletionBadge::Static:
			return LOCTEXT("Completion.BadgeStatic", "static");
		case dream::CompletionBadge::Keyword:
			return LOCTEXT("Completion.BadgeKeyword", "keyword");
		case dream::CompletionBadge::Snippet:
			return LOCTEXT("Completion.BadgeSnippet", "snippet");
		case dream::CompletionBadge::Runtime:
		default:
			return LOCTEXT("Completion.BadgeRuntime", "runtime");
		}
	}

	/** Characters after which typing opens the popup (§7.6.2 triggers): identifier characters, '.', a quote. Whether a
	 *  quote opens it is decided by the chain (only an unreal-aware string argument does). */
	bool IsIdentChar(TCHAR Typed)
	{
		return Typed == TEXT('_') || FChar::IsAlnum(Typed) || (Typed >= 0x80 && FChar::IsAlpha(Typed));
	}

	bool IsTriggerChar(TCHAR Typed)
	{
		return Typed == TEXT('_') || Typed == TEXT('.') || Typed == TEXT('\'') || Typed == TEXT('"') || FChar::IsAlnum(Typed)
			|| (Typed >= 0x80 && FChar::IsAlpha(Typed));
	}

	const std::vector<dream::CompletionCandidate>& Keywords()
	{
		static const std::vector<dream::CompletionCandidate> Table = dream::KeywordCandidates();
		return Table;
	}

	FDreamCompletionService* GetService()
	{
		return FDreamEditorModule::IsAvailable() ? &FDreamEditorModule::Get().GetCompletion() : nullptr;
	}
}

// ---- SDreamCompletionPopup ----------------------------------------------------------------------------------------------

void SDreamCompletionPopup::Construct(const FArguments& InArgs)
{
	Font = InArgs._Font;
	OnRowClicked = InArgs._OnRowClicked;
	const float RowHeight = static_cast<float>(FMath::Max(Font.Size, 8)) * 1.6f + 4.f;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground))   // (theme) PO-04: the popup in the theme
		.ForegroundColor(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
		.Padding(FMargin(1.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.MinDesiredWidth(260.f)
				.MaxDesiredWidth(720.f)
				.MaxDesiredHeight(RowHeight * VisibleRows)
				[
					SAssignNew(List, SListView<FRowPtr>)
					.ListViewStyle(&FDreamThemes::ListViewStyle())   // (theme) no dark list background under a light theme
					.ScrollBarStyle(&FDreamThemes::ScrollBarStyle())   // (theme)
					.ListItemsSource(&Rows)
					.SelectionMode(ESelectionMode::Single)
					// Keyboard focus stays in the text (T-KEY-007): the list is never a focus target.
					.IsFocusable(false)
					.OnGenerateRow(this, &SDreamCompletionPopup::MakeRow)
					.OnMouseButtonClick_Lambda([this](FRowPtr Row)
					{
						if (Row.IsValid())
						{
							OnRowClicked.ExecuteIfBound(Row->Index);
						}
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(6.f, 2.f))
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return Footer; })
				.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]() { return Footer.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
			]
		]
	];
}

TSharedRef<ITableRow> SDreamCompletionPopup::MakeRow(FRowPtr Row, const TSharedRef<STableViewBase>& Owner)
{
	const FSlateFontInfo SmallFont = FCoreStyle::GetDefaultFontStyle("Regular", FMath::Max(7, Font.Size - 1));
	return SNew(STableRow<FRowPtr>, Owner)
		.Style(&FDreamThemes::TableRowStyle())   // (theme) the highlighted row in the theme's selection colour
		.Padding(FMargin(4.f, 1.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 6.f, 0.f))
			[
				SNew(SBox)
				.WidthOverride(12.f)
				[
					SNew(STextBlock)
					.Text(Row->KindGlyph)
					.Font(SmallFont)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Row->Label)
				.Font(Font)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(12.f, 0.f, 6.f, 0.f))
			[
				SNew(STextBlock)
				.Text(Row->Detail)
				.Font(SmallFont)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Row->Badge)
				.Font(SmallFont)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

void SDreamCompletionPopup::SetList(const dream::MergedList& Merged, int32 InSelected, const FText& InFooter)
{
	using namespace DreamCompletionPopupLocal;
	Rows.Reset(static_cast<int32>(Merged.Items.size()));
	for (int32 Index = 0; Index < static_cast<int32>(Merged.Items.size()); ++Index)
	{
		const dream::MergedItem& Item = Merged.Items[static_cast<size_t>(Index)];
		FRowPtr Row = MakeShared<FDreamCompletionRow>();
		Row->Label = FText::FromString(DreamText::Utf8ToFString(Item.Item.Label));
		Row->Detail = FText::FromString(DreamText::Utf8ToFString(dream::DisplayDetail(Item)));
		Row->Badge = BadgeText(Item.Badge);
		Row->KindGlyph = KindGlyph(Item.Item.Kind);
		Row->Index = Index;
		Rows.Add(MoveTemp(Row));
	}
	Footer = InFooter;
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
	SetSelected(InSelected);
}

void SDreamCompletionPopup::SetSelected(int32 Index)
{
	Selected = Rows.IsValidIndex(Index) ? Index : -1;
	if (!List.IsValid())
	{
		return;
	}
	if (Selected >= 0)
	{
		List->SetSelection(Rows[Selected], ESelectInfo::Direct);
		List->RequestScrollIntoView(Rows[Selected]);
	}
	else
	{
		List->ClearSelection();
	}
}

FString SDreamCompletionPopup::GetRowLabel(int32 Index) const
{
	return Rows.IsValidIndex(Index) ? Rows[Index]->Label.ToString() : FString();
}

FString SDreamCompletionPopup::GetRowBadge(int32 Index) const
{
	return Rows.IsValidIndex(Index) ? Rows[Index]->Badge.ToString() : FString();
}

// ---- FDreamCompletionController -----------------------------------------------------------------------------------------

TSharedRef<FDreamCompletionController> FDreamCompletionController::Create(const TSharedRef<SDreamCodeView>& InView)
{
	TSharedRef<FDreamCompletionController> Controller = MakeShareable(new FDreamCompletionController());
	Controller->Bind(InView);
	return Controller;
}

FDreamCompletionController::~FDreamCompletionController()
{
	if (FDreamEditorModule::IsAvailable() && RunStateHandle.IsValid())
	{
		FDreamEditorModule::Get().GetRunService().OnRunStateChanged().Remove(RunStateHandle);
	}
	if (LiveTicket != 0)
	{
		if (FDreamCompletionService* Service = DreamCompletionPopupLocal::GetService())
		{
			Service->CancelLive(LiveTicket);
		}
	}
	if (Menu.IsValid())
	{
		const TSharedPtr<IMenu> Old = MoveTemp(Menu);
		Menu.Reset();
		if (FSlateApplication::IsInitialized())
		{
			Old->Dismiss();
		}
	}
}

void FDreamCompletionController::Bind(const TSharedRef<SDreamCodeView>& InView)
{
	View = InView;
	if (FDreamEditorModule::IsAvailable())
	{
		RunStateHandle = FDreamEditorModule::Get().GetRunService().OnRunStateChanged().AddSP(this, &FDreamCompletionController::HandleRunState);
	}
}

void FDreamCompletionController::Detach()
{
	Close();
	if (FDreamEditorModule::IsAvailable() && RunStateHandle.IsValid())
	{
		FDreamEditorModule::Get().GetRunService().OnRunStateChanged().Remove(RunStateHandle);
	}
	RunStateHandle.Reset();
	View.Reset();
}

// ---- Hooks ----

bool FDreamCompletionController::IsCompleteChord(const FKeyEvent& KeyEvent) const
{
	if (!FDreamCommands::IsRegistered())
	{
		return false;
	}
	const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(EDreamCommand::Complete);
	if (!Info.IsValid())
	{
		return false;
	}
	const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
	const FInputChord Chord(KeyEvent.GetKey(), EModifierKey::FromBools(Mods.IsControlDown(), Mods.IsAltDown(), Mods.IsShiftDown(), Mods.IsCommandDown()));
	return Info->HasActiveChord(Chord);
}

bool FDreamCompletionController::HandleKeyDown(const FKeyEvent& KeyEvent)
{
	const bool bHandled = HandleKeyDownImpl(KeyEvent);
	if (bHandled)
	{
		ConsumedKeys.Add(KeyEvent.GetKey());
	}
	return bHandled;
}

bool FDreamCompletionController::HandleKeyUp(const FKeyEvent& KeyEvent)
{
	return ConsumedKeys.Remove(KeyEvent.GetKey()) > 0;
}

bool FDreamCompletionController::HandleKeyDownImpl(const FKeyEvent& KeyEvent)
{
	bCompleteChordDown = false;
	if (!View.IsValid())
	{
		return false;
	}
	// The Complete chord (D8: Ctrl+Space, never Ctrl+Tab, which passes through to Unreal). Resolved here, before the REPL's
	// prompt keys and the key router, so it works in the REPL prompt too; the key router keeps the binding consumed in
	// the other Dream views.
	if (IsCompleteChord(KeyEvent))
	{
		bCompleteChordDown = true;
		Trigger(/*bExplicit*/ true);
		return true;
	}
	if (State == EState::Closed)
	{
		return false;
	}
	const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
	const bool bPlain = !Mods.IsControlDown() && !Mods.IsAltDown() && !Mods.IsCommandDown() && !Mods.IsShiftDown();
	const FKey Key = KeyEvent.GetKey();
	if (Key == EKeys::Escape && bPlain)
	{
		Close();                // §6.5 / §7.6.2: Esc closes the popup first (the REPL's Esc clear needs a second Esc)
		return true;
	}
	if (State != EState::Open || List.Items.empty() || !bPlain)
	{
		return false;
	}
	const int32 Count = static_cast<int32>(List.Items.size());
	const int32 Current = FMath::Clamp(List.SelectedIndex, 0, Count - 1);
	if (Key == EKeys::Down)
	{
		SetSelected((Current + 1) % Count);
		return true;
	}
	if (Key == EKeys::Up)
	{
		SetSelected((Current + Count - 1) % Count);
		return true;
	}
	if (Key == EKeys::PageDown)
	{
		SetSelected(FMath::Min(Current + SDreamCompletionPopup::VisibleRows, Count - 1));
		return true;
	}
	if (Key == EKeys::PageUp)
	{
		SetSelected(FMath::Max(Current - SDreamCompletionPopup::VisibleRows, 0));
		return true;
	}
	if (Key == EKeys::Enter && FDreamNumpadEnter::Get().IsCurrentEnterNumpad())
	{
		Close();                // Numpad Enter always runs (§6.5 table, G7); it never accepts
		return false;
	}
	if (Key == EKeys::Enter || Key == EKeys::Tab)
	{
		// §7.6.2 accept keys (T-MRG-024); in the REPL prompt this is the "completion popup open" row of §6.5. A refused
		// accept (read-only during a run) still closes the popup and consumes the key.
		Accept();
		return true;
	}
	return false;
}

bool FDreamCompletionController::OnCharTyped(const FCharacterEvent& CharacterEvent)
{
	const TCHAR Typed = CharacterEvent.GetCharacter();
	const bool bChord = bCompleteChordDown;
	bCompleteChordDown = false;
	if (bChord && Typed == TEXT(' ') && CharacterEvent.GetModifierKeys().IsControlDown())
	{
		return true;            // Windows sends Ctrl+Space's WM_CHAR as a space: the chord inserts nothing
	}
	LastTyped = Typed;
	LastTypedFrame = GFrameCounter;
	return false;
}

void FDreamCompletionController::OnTextEdited()
{
	LastEditFrame = GFrameCounter;
	if (bAccepting)
	{
		return;
	}
	const TCHAR Typed = LastTypedFrame == GFrameCounter ? LastTyped : 0;
	LastTyped = 0;
	if (State == EState::Closed && (Typed == 0 || !DreamCompletionPopupLocal::IsTriggerChar(Typed)))
	{
		return;
	}
	// The caret is not final yet: an edit the view builds itself (an auto-closed pair, a smart newline) places it after
	// this call, without a cursor event. The decision runs once the input event is done, before the frame paints.
	if (Typed != 0)
	{
		PendingTyped = Typed;
	}
	ScheduleEvaluate();
}

void FDreamCompletionController::ScheduleEvaluate()
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (bEvaluatePending || !Pinned.IsValid())
	{
		return;
	}
	bEvaluatePending = true;
	// No Python and no run state here: the decision reads the text and C++ caches only (D14(5)).
	DREAM_TIMER_EXEMPT(paint-only)
	Pinned->RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &FDreamCompletionController::HandleEvaluateTimer));
}

EActiveTimerReturnType FDreamCompletionController::HandleEvaluateTimer(double /*Now*/, float /*Delta*/)
{
	EvaluateAfterEdit();
	return EActiveTimerReturnType::Stop;
}

void FDreamCompletionController::EvaluateAfterEdit()
{
	using namespace DreamCompletionPopupLocal;
	const TCHAR Typed = PendingTyped;
	PendingTyped = 0;
	bEvaluatePending = false;
	if (State != EState::Closed)
	{
		if (Typed == 0 || IsIdentChar(Typed))
		{
			Trigger(bExplicit);     // the word grew or shrank: re-filter, or close when the caret left it
			return;
		}
		Close();                    // a character that ends the word closes: no commit characters (§7.6.2, T-MRG-024)
	}
	if (Typed != 0 && IsTriggerChar(Typed))
	{
		Trigger(/*bExplicit*/ false);   // '.', a quote or an identifier character starts a list (§7.6.2 triggers)
	}
}

void FDreamCompletionController::OnCaretMoved()
{
	if (State == EState::Closed || bAccepting || bEvaluatePending)
	{
		return;                 // an edit's own caret move is decided once the edit is done (EvaluateAfterEdit)
	}
	dream::CompletionChain Now;
	int32 Line = 0;
	int32 Column = 0;
	const bool bRead = ReadChain(Now, Line, Column);
	if (bRead && Line == SessionLine && Column == LastColumn)
	{
		return;                 // nothing moved since the list was built
	}
	if (!bRead || Line != SessionLine || Column < Chain.PrefixStart || Now.Context != Chain.Context
		|| Now.PrefixStart != Chain.PrefixStart || Now.Key() != Chain.Key())
	{
		Close();                // the caret left the word (§7.6.2)
		return;
	}
	Trigger(bExplicit);         // moved inside the word: the prefix up to the caret
}

void FDreamCompletionController::OnFocusLost(const FWidgetPath& NewFocusPath)
{
	if (State == EState::Closed)
	{
		return;
	}
	// A click on a row moves focus into the popup's window before the click is seen: keep the list for it.
	if (Popup.IsValid() && (Popup->IsHovered() || NewFocusPath.ContainsWidget(Popup.Get())))
	{
		return;
	}
	if (Menu.IsValid())
	{
		const TSharedPtr<SWindow> MenuWindow = Menu->GetOwnedWindow();
		if (MenuWindow.IsValid() && NewFocusPath.IsValid() && &NewFocusPath.GetWindow().Get() == MenuWindow.Get())
		{
			return;
		}
	}
	Close();
}

// ---- Lifecycle ----

bool FDreamCompletionController::ReadChain(dream::CompletionChain& OutChain, int32& OutLine, int32& OutColumn) const
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	const TSharedPtr<FDreamBuffer> Buffer = Pinned.IsValid() ? Pinned->GetBuffer() : nullptr;
	if (!Buffer.IsValid())
	{
		return false;
	}
	Pinned->GetCaret(OutLine, OutColumn);
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	if (OutLine < 0 || OutLine >= static_cast<int32>(Snapshot.LineCount()))
	{
		return false;
	}
	const std::u16string Text = Snapshot.LineText(OutLine);
	OutColumn = FMath::Clamp(OutColumn, 0, static_cast<int32>(Text.size()));
	bool bStartsInString = false;
	if (OutLine > 0)
	{
		// A line that continues a triple-quoted string completes nothing (§7.5.3 chain extraction reads one line).
		dream::PyLexer& Lexer = Buffer->GetLexer();
		Lexer.RelexThrough(Snapshot, OutLine);
		bStartsInString = Lexer.Facts(OutLine).Has(dream::LF_StartsInString);
	}
	OutChain = dream::ExtractCompletionChain(Text, OutColumn, bStartsInString);
	return true;
}

dream::MergeContext FDreamCompletionController::MakeContext(int32 SelectedIndex) const
{
	dream::MergeContext Context;
	Context.Kind = Query.Kind;
	Context.bRootBoundInSession = bRootBound && Query.Namespace == TEXT("session");
	Context.Prefix = Chain.Prefix;
	Context.SelectedIndex = SelectedIndex;
	Context.bIncomingIncomplete = bLiveIncomplete;
	return Context;
}

void FDreamCompletionController::Trigger(bool bExplicitIn)
{
	using namespace DreamCompletionPopupLocal;
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	FDreamCompletionService* Service = GetService();
	dream::CompletionChain NewChain;
	int32 Line = 0;
	int32 Column = 0;
	if (!Pinned.IsValid() || Service == nullptr || !ReadChain(NewChain, Line, Column) || NewChain.Context == dream::ChainContext::None)
	{
		Close();
		return;
	}
	const FDreamBuffer& Buffer = *Pinned->GetBuffer();
	FDreamCompletionQuery NewQuery;
	NewQuery.Kind = FDreamCompletionService::BufferKindFor(Buffer);
	NewQuery.Namespace = FDreamCompletionService::NamespaceFor(Buffer);
	const FString NewKey = NewQuery.Namespace + TEXT("\n") + DreamText::Utf8ToFString(NewChain.Key());
	// The same word: the chain and the start of the prefix are unchanged, only the prefix grew or shrank.
	const bool bSameWord = State != EState::Closed && NewKey == ChainKey && Line == SessionLine && NewChain.PrefixStart == Chain.PrefixStart;
	const bool bSameChain = bSameWord && Live.IsValid();
	// Ctrl+Space holds for its word only: a character that ends the word ('(', a space) makes it a typed trigger again.
	const bool bExplicitNow = bExplicitIn || (bSameWord && bExplicit);
	if (!bExplicitNow && NewChain.Context == dream::ChainContext::Name && static_cast<int32>(NewChain.Prefix.size()) < MinChars)
	{
		Close();                // Completion.MinChars (§7.6.2); Ctrl+Space opens on an empty prefix
		return;
	}
	NewQuery.Chain = NewChain;
	Chain = MoveTemp(NewChain);
	Query = MoveTemp(NewQuery);
	SessionLine = Line;
	LastColumn = Column;
	bExplicit = bExplicitNow;

	// §7.6.1 "Incomplete lists": a complete list (or one whose live answer is on its way) is re-filtered locally.
	if (bSameChain && (!bLiveIncomplete || bWaitingLive))
	{
		Rebuild();
		ShowOrUpdate();
		return;
	}

	// A new chain, or an incomplete list: the C++ caches answer now; a miss goes to the resolver's next idle tick.
	Service->CancelLive(LiveTicket);
	LiveTicket = 0;
	bWaitingLive = false;
	ChainKey = NewKey;
	FDreamCompletionAnswer Answer;
	const bool bHit = Service->QueryCached(Query, Answer);
	Live = Answer.Candidates;
	bLiveIncomplete = Answer.bIncomplete;
	bRootBound = Answer.bRootBound;
	bLivePaused = Answer.bLivePaused;
	RuntimeType = Answer.TypeName;
	if ((!bHit || Answer.bIncomplete) && !Answer.bLivePaused && !Answer.bLiveOff)
	{
		// The resolver never runs while a run is active (D6): during a run only the caches and FUnrealNameIndex answer.
		LiveTicket = Service->RequestLive(Query,
			FDreamOnCompletionAnswer::CreateSP(this, &FDreamCompletionController::HandleLiveAnswer, ChainKey), EDreamStartReason::Keystroke);
		bWaitingLive = LiveTicket != 0;
	}
	Rebuild();
	ShowOrUpdate();
}

void FDreamCompletionController::Rebuild()
{
	using namespace DreamCompletionPopupLocal;
	static const dream::MergedList Empty;
	const std::span<const dream::CompletionCandidate> LiveItems = Live.IsValid()
		? std::span<const dream::CompletionCandidate>(Live->data(), Live->size()) : std::span<const dream::CompletionCandidate>();
	List = dream::MergeCompletions(Empty, LiveItems, dream::CompletionSource::Live, MakeContext(-1));
	if (Chain.Context == dream::ChainContext::Name)
	{
		// Root names also offer the keyword table (keywords, soft keywords and builtins, the "keyword" badge, G3).
		const std::vector<dream::CompletionCandidate>& Table = Keywords();
		List = dream::MergeCompletions(List, std::span<const dream::CompletionCandidate>(Table.data(), Table.size()),
			dream::CompletionSource::Keyword, MakeContext(-1));
	}
	if (List.Items.size() > static_cast<size_t>(MaxItems))
	{
		List.Items.resize(static_cast<size_t>(MaxItems));
	}
	List.SelectedIndex = List.Items.empty() ? 0 : FMath::Clamp(List.SelectedIndex, 0, static_cast<int32>(List.Items.size()) - 1);
}

void FDreamCompletionController::HandleLiveAnswer(const FDreamCompletionAnswer& Answer, FString Key)
{
	if (State == EState::Closed || Key != ChainKey)
	{
		return;                 // a stale answer: the chain changed or the popup closed meanwhile
	}
	LiveTicket = 0;
	bWaitingLive = false;
	Live = Answer.Candidates;
	bLiveIncomplete = Answer.bIncomplete;
	bRootBound = bRootBound || Answer.bRootBound;
	bLivePaused = Answer.bLivePaused;
	if (!Answer.TypeName.IsEmpty())
	{
		RuntimeType = Answer.TypeName;
	}
	if (State == EState::Open && !List.Items.empty() && Live.IsValid())
	{
		// The no-reorder rule (D6, T-MRG-004): items 0..S stay where they are; the late items go below S.
		const int32 Pinned = GetSelected();
		List = dream::MergeCompletions(List, std::span<const dream::CompletionCandidate>(Live->data(), Live->size()),
			dream::CompletionSource::Live, MakeContext(Pinned));
		if (List.Items.size() > static_cast<size_t>(MaxItems))
		{
			List.Items.resize(static_cast<size_t>(MaxItems));
		}
	}
	else
	{
		Rebuild();
	}
	ShowOrUpdate();
}

FText FDreamCompletionController::GetFooter() const
{
	if (bLivePaused)
	{
		return LOCTEXT("Completion.FooterPaused", "Runtime results paused while code runs");   // §7.5.3
	}
	if (bWaitingLive && List.Items.empty())
	{
		return LOCTEXT("Completion.FooterLoading", "Loading…");
	}
	return FText::GetEmpty();
}

void FDreamCompletionController::ShowOrUpdate()
{
	if (List.Items.empty())
	{
		if (bWaitingLive)
		{
			// §7.6.2 Requested: waiting for the first answer with items. Ctrl+Space shows a "Loading…" row meanwhile.
			State = EState::Requested;
			if (bExplicit)
			{
				ShowMenu();
			}
			else
			{
				DismissMenu();
			}
			return;
		}
		Close();                // Requested -> Closed: every source is empty
		return;
	}
	State = EState::Open;
	ShowMenu();
}

void FDreamCompletionController::ShowMenu()
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!Pinned.IsValid())
	{
		return;
	}
	if (!Popup.IsValid())
	{
		SAssignNew(Popup, SDreamCompletionPopup)
			.Font(Pinned->GetCodeFont())
			.OnRowClicked(FDreamOnCompletionRowClicked::CreateSP(this, &FDreamCompletionController::HandleRowClicked));
	}
	Popup->SetList(List, List.Items.empty() ? -1 : List.SelectedIndex, GetFooter());
	if (Menu.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	// The menu needs the view's window: a view that is not shown (a background tab, a spec's hidden view) keeps the list
	// without a popup.
	FWidgetPath Path;
	if (!FSlateApplication::Get().FindPathToWidget(Pinned.ToSharedRef(), Path) || !Path.IsValid())
	{
		return;
	}
	FSlateRect Caret = Pinned->GetCaretScreenRect();
	if (Caret.Bottom <= Caret.Top)
	{
		const FGeometry& Geometry = Pinned->GetCachedGeometry();
		const FVector2D TopLeft = Geometry.GetAbsolutePosition();
		Caret = FSlateRect(TopLeft, TopLeft);
	}
	// Below the caret line (the menu stack flips it above when the screen ends), anchored where the list opened; the
	// text keeps keyboard focus (bFocusImmediately false).
	Menu = FSlateApplication::Get().PushMenu(Pinned.ToSharedRef(), Path, Popup.ToSharedRef(), Caret.GetTopLeft(),
		FPopupTransitionEffect(FPopupTransitionEffect::None), /*bFocusImmediately*/ false, Caret.GetSize());
	if (Menu.IsValid())
	{
		Menu->GetOnMenuDismissed().AddSP(this, &FDreamCompletionController::HandleMenuDismissed);
	}
}

void FDreamCompletionController::DismissMenu()
{
	if (Menu.IsValid())
	{
		const TSharedPtr<IMenu> Old = MoveTemp(Menu);
		Menu.Reset();
		Old->Dismiss();
	}
}

void FDreamCompletionController::HandleMenuDismissed(TSharedRef<IMenu> DismissedMenu)
{
	if (Menu.IsValid() && Menu == DismissedMenu)
	{
		// Something else dismissed it (a click outside, another menu, the window closing).
		Menu.Reset();
		Close();
	}
}

void FDreamCompletionController::Close()
{
	if (LiveTicket != 0)
	{
		if (FDreamCompletionService* Service = DreamCompletionPopupLocal::GetService())
		{
			Service->CancelLive(LiveTicket);
		}
	}
	LiveTicket = 0;
	bWaitingLive = false;
	bExplicit = false;
	State = EState::Closed;
	List = dream::MergedList();
	Live.Reset();
	ChainKey.Reset();
	bLiveIncomplete = false;
	bRootBound = false;
	bLivePaused = false;
	RuntimeType.Reset();
	DismissMenu();
}

void FDreamCompletionController::SetSelected(int32 Index)
{
	if (List.Items.empty())
	{
		return;
	}
	List.SelectedIndex = FMath::Clamp(Index, 0, static_cast<int32>(List.Items.size()) - 1);
	if (Popup.IsValid())
	{
		Popup->SetSelected(List.SelectedIndex);
	}
}

TArray<FString> FDreamCompletionController::GetLabels() const
{
	TArray<FString> Labels;
	Labels.Reserve(static_cast<int32>(List.Items.size()));
	for (const dream::MergedItem& Item : List.Items)
	{
		Labels.Add(DreamText::Utf8ToFString(Item.Item.Label));
	}
	return Labels;
}

bool FDreamCompletionController::Accept(int32 Index)
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	const int32 Pick = Index == INDEX_NONE ? GetSelected() : Index;
	if (State != EState::Open || !Pinned.IsValid() || Pick < 0 || Pick >= static_cast<int32>(List.Items.size()))
	{
		Close();
		return false;
	}
	// The word is re-read now: the replaced range is the prefix typed so far, [PrefixStart, caret) on the caret line.
	dream::CompletionChain Now;
	int32 Line = 0;
	int32 Column = 0;
	if (!ReadChain(Now, Line, Column) || Line != SessionLine || Column < Chain.PrefixStart)
	{
		Close();
		return false;
	}
	const FString Insert = DreamText::Utf8ToFString(List.Items[static_cast<size_t>(Pick)].Item.GetInsertText());
	FDreamEditGroup Group;
	Group.Label = LOCTEXT("Completion.UndoLabel", "Completion");
	FDreamTextEdit& Edit = Group.Edits.AddDefaulted_GetRef();
	Edit.StartLine = Line;
	Edit.StartColumn = Chain.PrefixStart;
	Edit.EndLine = Line;
	Edit.EndColumn = Column;
	Edit.Text = Insert;
	Group.SelectionAfter = FDreamTextSelection::Caret(Line, Chain.PrefixStart + Insert.Len());
	// One undo step (EDT-08); false while read-only or during a run, and the popup closes either way.
	bAccepting = true;
	const bool bApplied = Pinned->ApplyEditGroup(Group);
	bAccepting = false;
	Close();
	return bApplied;
}

void FDreamCompletionController::HandleRowClicked(int32 Index)
{
	Accept(Index);
	if (const TSharedPtr<SDreamCodeView> Pinned = View.Pin())
	{
		Pinned->FocusEditor();  // the click moved focus into the popup's window; typing continues in the text
	}
}

void FDreamCompletionController::HandleRunState(const FDreamRunRecord& Record)
{
	if (Record.State == EDreamRunState::Starting)
	{
		Close();                // a run start closes the popup (D6, T-MRG-013)
	}
}

#undef LOCTEXT_NAMESPACE
