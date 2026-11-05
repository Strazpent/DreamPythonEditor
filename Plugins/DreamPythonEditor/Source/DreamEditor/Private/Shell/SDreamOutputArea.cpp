// SPDX-License-Identifier: Apache-2.0
// The output area (TDD §6.4 "The output area", §6.5 placement, §5.5.3 rect and hand-off; WS-02, WS-03, RV-18, PO-01).
// See SDreamOutputArea.h. Tab row Output · REPL · Problems, the tab body, and the one REPL prompt strip below the body,
// shown for Output and REPL and hidden for Problems.
#include "Shell/SDreamOutputArea.h"

#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamKeyRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SComboButton.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamReplHistory.h"
#include "Output/DreamRuntimeProblems.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamProblemsView.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/DreamChrome.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamTheme.h"   // (theme)
#include "Styling/AppStyle.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Dream"

/** The REPL tab's history list (§6.5: "a filtered view of the same FDreamOutputStore ... plus the history list").
 *  Newest at the bottom; a double click or Enter puts the entry in the prompt without running it. */
class SDreamReplHistoryList : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamReplHistoryList) {}
		SLATE_ARGUMENT(TWeakPtr<SDreamReplStrip>, Strip)
	SLATE_END_ARGS()

	using FItem = TSharedPtr<FDreamHistoryEntry>;
	static constexpr int32 MaxEntries = 500;

	void Construct(const FArguments& InArgs)
	{
		Strip = InArgs._Strip;
		const UDreamUserSettings* Settings = UDreamUserSettings::Get();
		const FSlateFontInfo Font = FDreamEditorStyle::GetCodeFont(Settings ? Settings->FontSize : 10);
		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(4.f, 2.f))
			[
				SNew(STextBlock).Text(LOCTEXT("Out.ReplHistoryTitle", "History")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SAssignNew(List, SListView<FItem>)
				.ListViewStyle(&FDreamThemes::ListViewStyle())   // (theme) no dark list background under a light theme
				.ScrollBarStyle(&FDreamThemes::ScrollBarStyle())   // (theme)
				.ListItemsSource(&Items)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow_Lambda([Font](FItem Item, const TSharedRef<STableViewBase>& Owner) -> TSharedRef<ITableRow>
				{
					TArray<FString> Lines;
					Item->Text.ParseIntoArray(Lines, TEXT("\n"), false);
					FString Label = Lines.IsEmpty() ? FString() : Lines[0];
					if (Lines.Num() > 1)
					{
						Label += FString::Printf(TEXT("  (+%d lines)"), Lines.Num() - 1);
					}
					if (Item->Source == EDreamHistorySource::UeConsole)
					{
						Label += TEXT("   [UE console]");
					}
					return SNew(STableRow<FItem>, Owner)
						.Style(&FDreamThemes::TableRowStyle())   // (theme)
						.Padding(FMargin(4.f, 0.f))
						.ToolTipText(FText::FromString(Item->Text))
						[
							SNew(STextBlock).Text(FText::FromString(Label)).Font(Font)
						];
				})
				.OnMouseButtonDoubleClick_Lambda([this](FItem Item) { Put(Item); })
				.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent)
				{
					if (KeyEvent.GetKey() == EKeys::Enter && List->GetNumItemsSelected() > 0)
					{
						Put(List->GetSelectedItems()[0]);
						return FReply::Handled();
					}
					return FReply::Unhandled();
				})
			]
		];
		ChangedHandle = FDreamEditorModule::Get().GetReplHistory().OnChanged().AddSP(this, &SDreamReplHistoryList::Refresh);
		Refresh();
	}

	virtual ~SDreamReplHistoryList() override
	{
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetReplHistory().OnChanged().Remove(ChangedHandle);
		}
	}

	/** Widget is the list, the widget here that takes keyboard focus (SDreamTab's focus home, P4-shl-02). */
	bool IsFocusList(const SWidget& Widget) const { return List.IsValid() && List.Get() == &Widget; }

private:
	void Refresh()
	{
		Items.Reset();
		for (const FDreamHistoryEntry& Entry : FDreamEditorModule::Get().GetReplHistory().Query(FString(), EDreamHistoryMatch::Prefix, MaxEntries))
		{
			Items.Add(MakeShared<FDreamHistoryEntry>(Entry));
		}
		List->RequestListRefresh();
		if (!Items.IsEmpty())
		{
			List->RequestScrollIntoView(Items.Last());
		}
	}

	void Put(const FItem& Item)
	{
		if (const TSharedPtr<SDreamReplStrip> Pinned = Strip.Pin())
		{
			if (Item.IsValid())
			{
				Pinned->PutInPrompt(Item->Text);
			}
		}
	}

	TWeakPtr<SDreamReplStrip> Strip;
	TArray<FItem> Items;
	TSharedPtr<SListView<FItem>> List;
	FDelegateHandle ChangedHandle;
};

// ---------------------------------------------------------------------------------------------------------------------

void SDreamOutputArea::Construct(const FArguments& InArgs)
{
	CommandTarget = InArgs._CommandTarget;
	ActiveTab = InArgs._InitialTab;
	OnActiveTabChanged = InArgs._OnActiveTabChanged;
	if (!IsTabShown(ActiveTab))
	{
		ActiveTab = EDreamOutputTab::Output;   // (chrome) the saved tab is hidden now (PO-04)
	}

	const TWeakPtr<SDreamOutputArea> Weak = SharedThis(this);
	// OUT-05: a <dream:REPL#N> or <dream:Console#N> link scrolls Output to run N's echo lines, from either view.
	const FDreamOnOpenPromptLink OpenPromptLink = FDreamOnOpenPromptLink::CreateLambda([Weak](int32 RunNumber, int32 Line)
	{
		const TSharedPtr<SDreamOutputArea> Pinned = Weak.Pin();
		if (!Pinned.IsValid() || !Pinned->OutputView.IsValid())
		{
			return false;
		}
		Pinned->SetActiveTab(EDreamOutputTab::Output);
		return Pinned->OutputView->ScrollToEchoLine(RunNumber, Line);
	});
	SAssignNew(OutputView, SDreamOutputView).CommandTarget(CommandTarget).OnOpenPromptLink(OpenPromptLink);
	SAssignNew(ReplView, SDreamOutputView).RouteFilter(EDreamRunRoute::Repl).CommandTarget(CommandTarget).OnOpenPromptLink(OpenPromptLink);
	SAssignNew(ReplStrip, SDreamReplStrip).ForwardTarget(CommandTarget);
	ReplHistoryList = SNew(SDreamReplHistoryList).Strip(ReplStrip);

	ProblemsView = SNew(SDreamProblemsView)
		.OnOpenPromptLocation_Lambda([Weak](int32 RunNumber, int32 Line)
		{
			if (const TSharedPtr<SDreamOutputArea> Pinned = Weak.Pin())
			{
				Pinned->SetActiveTab(EDreamOutputTab::Output);
				Pinned->OutputView->ScrollToEchoLine(RunNumber, Line);
			}
		});

	ChildSlot
	[
		SNew(SVerticalBox)
		// The tab row (chrome: only while the REPL or Problems tab is on; RebuildTabRow).
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(TabRowBox, SBorder)
			.BorderImage(FDreamChrome::Brush(EDreamChromeSlot::Background))
			.Padding(FMargin(2.f, 1.f, 2.f, 0.f))
			[
				SAssignNew(TabRow, SHorizontalBox)
			]
		]
		// The tab body, on the history's background.
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SBorder)
			.BorderImage(FDreamChrome::Brush(EDreamChromeSlot::OutputBackground))
			.Padding(FMargin(0.f))
			[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([Weak]()
			{
				const TSharedPtr<SDreamOutputArea> Pinned = Weak.Pin();
				return Pinned.IsValid() ? static_cast<int32>(Pinned->ActiveTab) : 0;
			})
			+ SWidgetSwitcher::Slot()[OutputView.ToSharedRef()]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				+ SSplitter::Slot().Value(0.75f)[ReplView.ToSharedRef()]
				+ SSplitter::Slot().Value(0.25f)[ReplHistoryList.ToSharedRef()]
			]
			+ SWidgetSwitcher::Slot()[ProblemsView.ToSharedRef()]
			]
		]
		// The one prompt strip, directly below the body in both output positions (RV-18). (chrome) Shown only while View ▸
		// REPL Prompt is on or Focus REPL revealed it, never under Problems (RefreshPromptVisibility).
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(PromptBox, SBox)
			[
				ReplStrip.ToSharedRef()
			]
		]
	];

	RebuildTabRow();
	RefreshPromptVisibility();
	RunStateHandle = FDreamEditorModule::Get().GetRunService().OnRunStateChanged().AddSP(this, &SDreamOutputArea::HandleRunStateChanged);
}

SDreamOutputArea::~SDreamOutputArea()
{
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetRunService().OnRunStateChanged().Remove(RunStateHandle);
	}
}

void SDreamOutputArea::SetActiveTab(EDreamOutputTab Tab)
{
	if (ActiveTab == Tab)
	{
		return;
	}
	ActiveTab = Tab;
	RefreshPromptVisibility();
	OnActiveTabChanged.ExecuteIfBound();
}

// ---- (chrome) PO-04: the tab row and the prompt strip follow the View toggles ----

bool SDreamOutputArea::IsTabShown(EDreamOutputTab Tab) const
{
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	switch (Tab)
	{
	case EDreamOutputTab::Repl: return Settings->bShowReplTab;
	case EDreamOutputTab::Problems: return Settings->bShowProblemsTab;
	default: return true;
	}
}

bool SDreamOutputArea::IsTabRowShown() const
{
	return TabRowBox.IsValid() && TabRowBox->GetVisibility().IsVisible();
}

bool SDreamOutputArea::IsPromptShown() const
{
	return UDreamUserSettings::Get()->bShowReplPrompt || bPromptRevealed;
}

void SDreamOutputArea::EndPromptReveal(bool bEvenIfTyped)
{
	if (!bPromptRevealed)
	{
		return;
	}
	const TSharedPtr<SDreamCodeView> Prompt = ReplStrip.IsValid() ? ReplStrip->GetPromptView() : nullptr;
	if (bEvenIfTyped || !Prompt.IsValid() || Prompt->GetText().IsEmpty())
	{
		bPromptRevealed = false;
		RefreshPromptVisibility();
	}
}

bool SDreamOutputArea::IsPromptPath(const FWidgetPath& Path) const
{
	const TSharedPtr<SDreamCodeView> Prompt = ReplStrip.IsValid() ? ReplStrip->GetPromptView() : nullptr;
	return Path.IsValid() && Prompt.IsValid() && Path.ContainsWidget(Prompt.Get());
}

void SDreamOutputArea::RefreshPromptVisibility()
{
	if (PromptBox.IsValid())
	{
		const bool bShow = IsPromptShown() && ActiveTab != EDreamOutputTab::Problems;
		const bool bWasShown = PromptBox->GetVisibility().IsVisible();
		PromptBox->SetVisibility(bShow ? EVisibility::Visible : EVisibility::Collapsed);
		if (bShow && !bWasShown)
		{
			// A collapsed strip has no desired size until the next frame's prepass, so its prompt is in no arranged path and
			// Focus REPL's FocusPrompt in this same call would not land; a prepass of the strip now lays it out (as
			// SDreamReplStrip::RefreshPromptLayout does for the prompt's text).
			PromptBox->SlatePrepass(GetPrepassLayoutScaleMultiplier());
		}
	}
}

void SDreamOutputArea::ApplyViewSettings()
{
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	if (Settings->bShowReplTab != bRowReplTab || Settings->bShowProblemsTab != bRowProblemsTab)
	{
		RebuildTabRow();   // not for a theme or another toggle: the row's segments keep their state and focus
	}
	if (!IsTabShown(ActiveTab))
	{
		SetActiveTab(EDreamOutputTab::Output);
	}
	if (UDreamUserSettings::Get()->bShowReplPrompt)
	{
		bPromptRevealed = false;   // the setting holds it now
	}
	RefreshPromptVisibility();
}

void SDreamOutputArea::RebuildTabRow()
{
	if (!TabRow.IsValid())
	{
		return;
	}
	TabRow->ClearChildren();
	bRowReplTab = IsTabShown(EDreamOutputTab::Repl);
	bRowProblemsTab = IsTabShown(EDreamOutputTab::Problems);
	const TWeakPtr<SDreamOutputArea> Weak = SharedThis(this);
	auto AddTab = [this, &Weak](EDreamOutputTab Tab, const TAttribute<FText>& Label, const FText& Tip)
	{
		// A segment takes keyboard focus on its click (an SCheckBox, as SSegmentedControl's were); it is no focus home.
		TabRow->AddSlot()
			.AutoWidth()
			[
				SNew(SCheckBox)
				.AddMetaData<FTagMetaData>(FTagMetaData(*FString::Printf(TEXT("DreamChrome.OutputTab.%d"), static_cast<int32>(Tab))))
				.Style(&FDreamChrome::TabToggle())
				.ToolTipText(Tip)
				.IsChecked_Lambda([Weak, Tab]()
				{
					const TSharedPtr<SDreamOutputArea> Pinned = Weak.Pin();
					return Pinned.IsValid() && Pinned->ActiveTab == Tab ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([Weak, Tab](ECheckBoxState)
				{
					if (const TSharedPtr<SDreamOutputArea> Pinned = Weak.Pin())
					{
						Pinned->SetActiveTab(Tab);
					}
				})
				[
					SNew(STextBlock).Font(FDreamChrome::Font(9)).Text(Label)
				]
			];
	};
	AddTab(EDreamOutputTab::Output, LOCTEXT("Out.OutputTab", "Output"), LOCTEXT("Out.OutputTabTip", "Every run as a numbered block"));
	if (IsTabShown(EDreamOutputTab::Repl))
	{
		AddTab(EDreamOutputTab::Repl, LOCTEXT("Out.ReplTab", "REPL"), LOCTEXT("Out.ReplTabTip", "REPL runs only, and the REPL history"));
	}
	if (IsTabShown(EDreamOutputTab::Problems))
	{
		AddTab(EDreamOutputTab::Problems, TAttribute<FText>::CreateLambda([Weak]()
			{
				const TSharedPtr<SDreamOutputArea> Pinned = Weak.Pin();
				return Pinned.IsValid() ? Pinned->GetProblemsLabel() : FText::GetEmpty();
			}),
			LOCTEXT("Out.ProblemsTabTip", "Lint, syntax and runtime problems (F8 / Shift+F8 walk them)"));
	}
	TabRow->AddSlot().FillWidth(1.f)[ SNullWidget::NullWidget ];
	// OUT-03 (§6.4 "Design: filters", mockup 01): Filter ▾, one toggle per type plus background (also History ▸ Show in Output).
	TabRow->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(4.f, 0.f))
		[
			SNew(SComboButton)
			.ComboButtonStyle(&FDreamChrome::FlatCombo())
			.HasDownArrow(false)
			.IsFocusable(false)
			.ContentPadding(FMargin(4.f, 1.f))
			.ToolTipText(LOCTEXT("Out.FilterTip", "Show or hide output lines by type"))
			.ButtonContent()
			[
				SNew(STextBlock).Font(FDreamChrome::Font(9)).ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText)).Text(LOCTEXT("Out.Filter", "Filter"))
			]
			.OnGetMenuContent_Lambda([Weak]() -> TSharedRef<SWidget>
			{
				const TSharedPtr<SDreamOutputArea> Pinned = Weak.Pin();
				FMenuBuilder Menu(/*bCloseAfterSelection*/ false, nullptr);
				SDreamOutputView::AddShowMenuEntries(Menu, Pinned.IsValid() ? Pinned->CommandTarget : TWeakPtr<IDreamCommandTarget>());
				return Menu.MakeWidget();
			})
		];
	if (TabRowBox.IsValid())
	{
		TabRowBox->SetVisibility(IsTabShown(EDreamOutputTab::Repl) || IsTabShown(EDreamOutputTab::Problems) ? EVisibility::Visible : EVisibility::Collapsed);
	}
}

FText SDreamOutputArea::GetProblemsLabel() const
{
	// (lang, B56) The Problems view's listed items: lint, parse and runtime (§7.8).
	const int32 Count = ProblemsView.IsValid() ? StaticCastSharedPtr<SDreamProblemsView>(ProblemsView)->GetItemCount() : 0;
	return Count > 0 ? FText::Format(LOCTEXT("Out.ProblemsTabCount", "Problems ({0})"), FText::AsNumber(Count))
		: LOCTEXT("Out.ProblemsTab", "Problems");
}

void SDreamOutputArea::FocusRepl()
{
	if (ActiveTab == EDreamOutputTab::Problems)
	{
		SetActiveTab(IsTabShown(EDreamOutputTab::Repl) ? EDreamOutputTab::Repl : EDreamOutputTab::Output);
	}
	if (!UDreamUserSettings::Get()->bShowReplPrompt)
	{
		bPromptRevealed = true;   // (chrome) PO-04: the hidden prompt shows for this use (EndPromptReveal)
	}
	RefreshPromptVisibility();
	ReplStrip->FocusPrompt();
}

bool SDreamOutputArea::IsFocusHome(const FWidgetPath& Path) const
{
	if (!Path.IsValid())
	{
		return false;
	}
	const SWidget& Leaf = Path.GetLastWidget().Get();
	const TSharedPtr<SDreamCodeView> Prompt = ReplStrip.IsValid() ? ReplStrip->GetPromptView() : nullptr;
	return (Prompt.IsValid() && Path.ContainsWidget(Prompt.Get()))
		|| (OutputView.IsValid() && OutputView->IsFocusList(Leaf))
		|| (ReplView.IsValid() && ReplView->IsFocusList(Leaf))
		|| (ReplHistoryList.IsValid() && StaticCastSharedPtr<SDreamReplHistoryList>(ReplHistoryList)->IsFocusList(Leaf))
		|| (ProblemsView.IsValid() && StaticCastSharedPtr<SDreamProblemsView>(ProblemsView)->IsFocusList(Leaf));
}

void SDreamOutputArea::HandleRunStateChanged(const FDreamRunRecord& Record)
{
	if (Record.State != EDreamRunState::Starting)
	{
		return;
	}
	// Problems, or the REPL tab for a run that is not a REPL run, cannot show the run: Output, before Python runs.
	if (ActiveTab == EDreamOutputTab::Problems || (ActiveTab == EDreamOutputTab::Repl && Record.Route != EDreamRunRoute::Repl))
	{
		SetActiveTab(EDreamOutputTab::Output);
	}
}

TSharedPtr<SDreamOutputView> SDreamOutputArea::GetHandoffView(EDreamRunRoute Route)
{
	if (Route == EDreamRunRoute::Repl && ActiveTab == EDreamOutputTab::Repl)
	{
		return ReplView;
	}
	SetActiveTab(EDreamOutputTab::Output);
	return OutputView;
}

bool SDreamOutputArea::GetDesktopRect(FSlateRect& OutRect, float& OutDpiScale) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	// A background tab's content is not in any window's widget tree; a minimised or hidden window does not count.
	const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared());
	if (!Window.IsValid() || !Window->IsVisible() || Window->IsWindowMinimized())
	{
		return false;
	}
	const FGeometry& Geometry = GetCachedGeometry();
	const FVector2D Size = FVector2D(Geometry.GetAbsoluteSize());
	if (Size.X < 1.0 || Size.Y < 1.0)
	{
		return false;
	}
	// Slate's absolute space is desktop pixels, which the Run window's ScreenPosition and ClientSize take as they are (§5.5.3 step 4).
	const FVector2D Position = FVector2D(Geometry.GetAbsolutePosition());
	OutRect = FSlateRect(static_cast<float>(Position.X), static_cast<float>(Position.Y),
		static_cast<float>(Position.X + Size.X), static_cast<float>(Position.Y + Size.Y));
	OutDpiScale = Window->GetDPIScaleFactor();
	return true;
}

#undef LOCTEXT_NAMESPACE
