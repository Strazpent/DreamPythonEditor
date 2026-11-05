// SPDX-License-Identifier: Apache-2.0
// The Problems tab (TDD §7.8). See SDreamProblemsView.h.
#include "Shell/SDreamProblemsView.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformProcess.h"
#include "Language/DreamLanguageServices.h"
#include "Language/DreamProblemsModel.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamIcons.h"   // (icons)
#include "Shell/DreamTheme.h"   // (theme)
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"   // (icons)
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "dream/OutputLines.h"
#include "dream/OutputRing.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamProblemsViewLocal
{
	FDreamLanguageServices* Services()
	{
		return FDreamEditorModule::IsAvailable() ? &FDreamEditorModule::Get().GetLanguageServices() : nullptr;
	}

	FString WhereText(const FDreamProblem& Item)
	{
		FString Name;
		if (Item.Buffer.IsValid() && FDreamEditorModule::IsAvailable())
		{
			if (const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(Item.Buffer))
			{
				Name = Buffer->GetDisplayName();
			}
		}
		if (Name.IsEmpty())
		{
			Name = Item.File.StartsWith(TEXT("<")) ? Item.File : FPaths::GetCleanFilename(Item.File);
		}
		return FString::Printf(TEXT("%s:%d"), *Name, Item.StartLine + 1);
	}

	FText SourceText(const FDreamProblem& Item)
	{
		switch (Item.Source)
		{
		case EDreamProblemSource::Runtime:
			return FText::Format(LOCTEXT("Lang.ProblemRuntime", "runtime · #{0}"), FText::AsNumber(Item.RunNumber));
		case EDreamProblemSource::Parse:
			return FText::Format(LOCTEXT("Lang.ProblemParse", "parse · #{0}"), FText::AsNumber(Item.RunNumber));
		default:
			return FText::Format(LOCTEXT("Lang.ProblemLint", "{0} · {1}"), FText::FromString(FDreamProblemsModel::SourceName(Item.Source)),
				FText::FromString(Item.Code));
		}
	}

	FSlateColor SeverityColor(EDreamSeverity Severity)
	{
		switch (Severity)
		{
		case EDreamSeverity::Error: return FDreamEditorStyle::GetLineTypeColor(dream::LineType::Error);
		case EDreamSeverity::Warning: return FDreamEditorStyle::GetLineTypeColor(dream::LineType::Warning);
		default: return FDreamEditorStyle::GetLineTypeColor(dream::LineType::Info);
		}
	}

	/** (icons) PO-06 F6: the severity's icon (it replaced the ✖ ▲ ● glyphs). */
	EDreamIcon SeverityIcon(EDreamSeverity Severity)
	{
		switch (Severity)
		{
		case EDreamSeverity::Error: return EDreamIcon::Error;
		case EDreamSeverity::Warning: return EDreamIcon::Warning;
		default: return EDreamIcon::Info;
		}
	}
}

void SDreamProblemsView::Construct(const FArguments& InArgs)
{
	using namespace DreamProblemsViewLocal;
	OnOpenPromptLocation = InArgs._OnOpenPromptLocation;

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(4.f, 2.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]() { return GetSummaryText(); })
			]
			// INT-11: the server's health while it is not ready; paused: [Restart] [View log].
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.f, 0.f))
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([]()
				{
					const FDreamLanguageServices* Lang = Services();
					return Lang != nullptr && Lang->GetHealth() != EDreamLanguageHealth::Ready ? Lang->GetHealthText() : FText::GetEmpty();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.f, 0.f, 4.f, 0.f))
			[
				SNew(SButton)
				.IsFocusable(false)
				.Text(LOCTEXT("Lang.Restart", "Restart"))
				.ToolTipText(LOCTEXT("Lang.RestartTip", "Starts dream-check again"))
				.Visibility_Lambda([]()
				{
					const FDreamLanguageServices* Lang = Services();
					return Lang != nullptr && Lang->GetHealth() == EDreamLanguageHealth::Paused ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.OnClicked_Lambda([]()
				{
					if (FDreamLanguageServices* Lang = Services())
					{
						Lang->RestartServers();
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.f, 0.f, 4.f, 0.f))
			[
				SNew(SButton)
				.IsFocusable(false)
				.Text(LOCTEXT("Lang.ViewLog", "View log"))
				.ToolTipText(LOCTEXT("Lang.ViewLogTip", "Shows dream-check's log file"))
				.Visibility_Lambda([]()
				{
					const FDreamLanguageServices* Lang = Services();
					return Lang != nullptr && Lang->GetHealth() == EDreamLanguageHealth::Paused && !Lang->GetLogPath().IsEmpty()
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				.OnClicked_Lambda([]()
				{
					if (const FDreamLanguageServices* Lang = Services())
					{
						FPlatformProcess::ExploreFolder(*Lang->GetLogPath());
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.IsFocusable(false)     // chrome: a click leaves focus where it was (SDreamTab::OnFocusReceived, P4-shl-02)
				.Text(LOCTEXT("Out.ClearRuntimeItems", "Clear runtime items"))
				.ToolTipText(LOCTEXT("Out.ClearRuntimeItemsTip", "Remove every runtime item; the next error adds new ones"))
				.IsEnabled_Lambda([this]()
				{
					return Items.ContainsByPredicate([](const FItem& Item) { return Item->Source == EDreamProblemSource::Runtime; });
				})
				.OnClicked_Lambda([]()
				{
					if (FDreamLanguageServices* Lang = Services())
					{
						Lang->GetProblems().ClearRuntimeItems();
					}
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(List, SListView<FItem>)
			.ListViewStyle(&FDreamThemes::ListViewStyle())   // (theme) no dark list background under a light theme
			.ScrollBarStyle(&FDreamThemes::ScrollBarStyle())   // (theme)
			.ListItemsSource(&Items)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SDreamProblemsView::MakeRow)
			.OnMouseButtonDoubleClick_Lambda([this](FItem Item) { OpenItem(Item); })
			.OnContextMenuOpening(this, &SDreamProblemsView::MakeContextMenu)
			.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent)
			{
				if (KeyEvent.GetKey() == EKeys::Enter && List.IsValid() && List->GetNumItemsSelected() > 0)
				{
					OpenItem(List->GetSelectedItems()[0]);
					return FReply::Handled();
				}
				return FReply::Unhandled();
			})
		]
	];

	if (FDreamLanguageServices* Lang = Services())
	{
		ChangedHandle = Lang->GetProblems().OnChanged().AddSP(this, &SDreamProblemsView::Refresh);
	}
	Refresh();
}

SDreamProblemsView::~SDreamProblemsView()
{
	if (FDreamLanguageServices* Lang = DreamProblemsViewLocal::Services())
	{
		Lang->GetProblems().OnChanged().Remove(ChangedHandle);
	}
}

void SDreamProblemsView::Refresh()
{
	Items.Reset();
	ErrorCount = 0;
	WarningCount = 0;
	if (FDreamLanguageServices* Lang = DreamProblemsViewLocal::Services())
	{
		for (FDreamProblem& Problem : Lang->GetProblems().GetVisible())
		{
			ErrorCount += Problem.Severity == EDreamSeverity::Error ? 1 : 0;
			WarningCount += Problem.Severity == EDreamSeverity::Warning ? 1 : 0;
			Items.Add(MakeShared<FDreamProblem>(MoveTemp(Problem)));
		}
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

FText SDreamProblemsView::GetSummaryText() const
{
	if (Items.IsEmpty())
	{
		return LOCTEXT("Lang.NoProblemsListed", "No problems");
	}
	return FText::Format(LOCTEXT("Lang.ProblemSummary", "{0} {0}|plural(one=problem,other=problems): {1} {1}|plural(one=error,other=errors), {2} {2}|plural(one=warning,other=warnings)"),
		FText::AsNumber(Items.Num()), FText::AsNumber(ErrorCount), FText::AsNumber(WarningCount));
}

TArray<FDreamProblem> SDreamProblemsView::GetItemsForTest() const
{
	TArray<FDreamProblem> Out;
	for (const FItem& Item : Items)
	{
		Out.Add(*Item);
	}
	return Out;
}

void SDreamProblemsView::OpenItemForTest(int32 Index)
{
	if (Items.IsValidIndex(Index))
	{
		OpenItem(Items[Index]);
	}
}

TSharedRef<ITableRow> SDreamProblemsView::MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	using namespace DreamProblemsViewLocal;
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	const FSlateFontInfo Font = FDreamEditorStyle::GetCodeFont(Settings ? Settings->FontSize : 10);
	FString Tip = Item->File.IsEmpty() ? WhereText(*Item) : Item->File;
	if (!Item->MergedFrom.IsEmpty())
	{
		// §7.8.2: the tooltip names the duplicates that were merged into this item.
		Tip += TEXT("\n") + FText::Format(LOCTEXT("Lang.MergedFrom", "Also reported by: {0}"),
			FText::FromString(FString::Join(Item->MergedFrom, TEXT(", ")))).ToString();
	}
	if (Item->bStale)
	{
		Tip += TEXT("\n") + LOCTEXT("Lang.StaleTip", "Stale: the server restarted; refreshed by its next report").ToString();
	}
	const float Opacity = Item->bStale ? 0.5f : 1.f;
	return SNew(STableRow<FItem>, Owner)
		.Style(&FDreamThemes::TableRowStyle())   // (theme)
		.Padding(FMargin(4.f, 1.f))
		.ToolTipText(FText::FromString(Tip))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.f, 0.f, 6.f, 0.f))
			[
				SNew(SImage)   // (icons) the severity icon, same tint as the glyph it replaced
				.Image(FDreamIcons::Brush(SeverityIcon(Item->Severity)))
				.DesiredSizeOverride(FVector2D(14.0, 14.0))
				.ColorAndOpacity_Lambda([Severity = Item->Severity]() { return SeverityColor(Severity); })   // (theme) live on a switch
				.RenderOpacity(Opacity)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock).Text(FText::FromString(Item->Message)).Font(Font).RenderOpacity(Opacity)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12.f, 0.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Lang.FixHint", "quick fix"))
				.Font(Font)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility(Item->bHasQuickFix ? EVisibility::Visible : EVisibility::Collapsed)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12.f, 0.f, 0.f, 0.f))
			[
				SNew(STextBlock).Text(FText::FromString(WhereText(*Item))).Font(Font).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12.f, 0.f, 4.f, 0.f))
			[
				SNew(STextBlock).Text(SourceText(*Item)).Font(Font).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

void SDreamProblemsView::OpenItem(const FItem& Item)
{
	if (!Item.IsValid() || !FDreamEditorModule::IsAvailable())
	{
		return;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	if (Item->Source == EDreamProblemSource::Runtime && !Item->Buffer.IsValid())
	{
		std::string_view Name;
		std::string_view Id;
		const std::string File = DreamText::ToUtf8(Item->File);
		if (dream::ParsePseudoFilename(File, Name, Id) && (Name == "REPL" || Name == "Console"))
		{
			OnOpenPromptLocation.ExecuteIfBound(FCString::Atoi(*DreamText::Utf8ToFString(Id)), Item->StartLine + 1);
			return;
		}
	}
	const TSharedPtr<SDreamTab> Tab = Module.GetTab();
	if (!Tab.IsValid() || !Module.GetLanguageServices().OpenProblem(*Tab, *Item))
	{
		if (Tab.IsValid())
		{
			Tab->ShowStatusMessage(FText::Format(LOCTEXT("Out.ProblemUnresolved", "Cannot open {0}"), FText::FromString(Item->File)));
		}
	}
}

void SDreamProblemsView::ShowFixes(const FItem& Item, const FVector2D& ScreenPosition)
{
	FDreamLanguageServices* Lang = DreamProblemsViewLocal::Services();
	if (Lang == nullptr || !Item.IsValid() || !Item->Buffer.IsValid())
	{
		return;
	}
	dream::LspRange Range;
	Range.StartLine = Item->StartLine;
	Range.StartCol = Item->StartColumn;
	Range.EndLine = Item->EndLine;
	Range.EndCol = Item->EndColumn;
	const TWeakPtr<SDreamProblemsView> Weak = StaticCastSharedRef<SDreamProblemsView>(AsShared());
	const bool bAsked = Lang->RequestCodeActions(Item->Buffer, Range, [Weak, ScreenPosition](TArray<dream::LspCodeAction>&& Actions)
	{
		const TSharedPtr<SDreamProblemsView> Pinned = Weak.Pin();
		FDreamLanguageServices* Services = DreamProblemsViewLocal::Services();
		if (!Pinned.IsValid() || Services == nullptr)
		{
			return;
		}
		if (Actions.IsEmpty())
		{
			if (const TSharedPtr<SDreamTab> Tab = FDreamEditorModule::Get().GetTab())
			{
				Tab->ShowStatusMessage(LOCTEXT("Lang.NoFixesForItem", "No quick fixes for this problem now"));
			}
			return;
		}
		Services->ShowCodeActionMenu(Pinned.ToSharedRef(), ScreenPosition, MoveTemp(Actions));
	});
	if (!bAsked)
	{
		if (const TSharedPtr<SDreamTab> Tab = FDreamEditorModule::Get().GetTab())
		{
			Tab->ShowStatusMessage(FText::Format(LOCTEXT("Lang.FixUnavailable", "Quick fixes are unavailable now ({0})"), Lang->GetHealthText()));
		}
	}
}

TSharedPtr<SWidget> SDreamProblemsView::MakeContextMenu()
{
	if (!List.IsValid() || List->GetNumItemsSelected() == 0)
	{
		return nullptr;
	}
	const FItem Item = List->GetSelectedItems()[0];
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("Lang.GoToProblem", "Go to Problem"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, Item]() { OpenItem(Item); })));
	if (Item->Source == EDreamProblemSource::DreamCheck)
	{
		Menu.AddMenuEntry(LOCTEXT("Lang.QuickFixMenu", "Quick Fix..."),
			LOCTEXT("Lang.QuickFixMenuTip", "The fixes for this problem, and Suppress on this line"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, Item]()
			{
				const FVector2D Cursor = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetCursorPos() : FVector2D::ZeroVector;
				ShowFixes(Item, Cursor);
			})));
	}
	if (Item->Source == EDreamProblemSource::Runtime)
	{
		Menu.AddMenuEntry(LOCTEXT("Out.ClearRuntimeItems", "Clear runtime items"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				if (FDreamLanguageServices* Lang = DreamProblemsViewLocal::Services())
				{
					Lang->GetProblems().ClearRuntimeItems();
				}
			})));
	}
	return Menu.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
