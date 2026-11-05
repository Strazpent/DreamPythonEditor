// SPDX-License-Identifier: Apache-2.0
// The Dream tab's chrome (PO-04 point 1, the Maya-minimal look; TDD §6.4, SPEC §9.1, U3, U6, OUT-12): a slim menu bar
// (File, Edit, History, Command, View, Help) and one row of small flat icon buttons with tooltips (Open, Save | Run with
// its split arrow, Run line, Stop | Clear history, Undo last run | Echo commands, Line numbers). Every former toolbar and
// menu action is here with the same command and chord: the split Run menu (routes, Run in / without transaction, One undo
// step per run with its B40 tri-state, Undo Last Run, Session / Isolated, Reset Session), Stop, Echo input, Clear,
// File ▸ Recent Files (BUF-03 M1), View ▸ Output Position (WS-03) and View ▸ Theme.
// Menus are built from an FUICommandList so they show the active chords; keys never go through it (§4.9.1).
#include "Shell/SDreamTab.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Commands/DreamCommands.h"
#include "Commands/DreamKeyRouter.h"
#include "Framework/MultiBox/MultiBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "ISettingsModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamOutputStore.h"
#include "Persistence/DreamJournal.h"
#include "Run/DreamRunService.h"
#include "Run/DreamTransactions.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamTabImpl.h"
#include "Shell/DreamChrome.h"
#include "Shell/DreamIcons.h"   // (icons) menu entry icons
#include "Shell/DreamProjectFiles.h"   // (sidebar) PO-05-A1: View > Side Bar > Plugin Python Folders
#include "Shell/DreamTheme.h"   // (theme) View > Theme > Open Themes Folder, Export Built-in Themes
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamToolbarLocal
{
	constexpr float IconSize = 16.f;

	FText Tooltip(EDreamCommand Command)
	{
		if (!FDreamCommands::IsRegistered())
		{
			return FText::GetEmpty();
		}
		const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(Command);
		const FText Chord = FDreamKeyRouter::Get().GetChordText(Command);
		if (!Info.IsValid())
		{
			return FText::GetEmpty();
		}
		return Chord.IsEmpty() ? Info->GetDescription() : FText::Format(LOCTEXT("TipWithChord", "{0} ({1})"), Info->GetDescription(), Chord);
	}

	/** "Label (chord)" for an icon whose tooltip names the action, not the command's description. */
	FText LabelTip(const FText& Label, EDreamCommand Command)
	{
		const FText Chord = FDreamKeyRouter::Get().GetChordText(Command);
		return Chord.IsEmpty() ? Label : FText::Format(LOCTEXT("Chrome.LabelChord", "{0} ({1})"), Label, Chord);
	}

	/** The tag every icon-row button and menu title carries (Dream.Shell.Chrome finds them by it). */
	FName ChromeTag(const TCHAR* Prefix, const FString& Name)
	{
		return FName(*FString::Printf(TEXT("DreamChrome.%s%s"), Prefix, *Name));
	}

	FName CommandTag(EDreamCommand Command)
	{
		return ChromeTag(TEXT(""), FDreamCommands::GetCommandName(Command).ToString());
	}

	TSharedRef<SWidget> IconImage(EDreamChromeIcon Icon, const TAttribute<FSlateColor>& Tint)
	{
		return SNew(SImage)
			.Image(FDreamChrome::Icon(Icon))
			.DesiredSizeOverride(FVector2D(IconSize, IconSize))
			.ColorAndOpacity(Tint);
	}

	/** A small flat icon button: no label, a tooltip, focus never taken (the code view keeps it, SHL-02). */
	TSharedRef<SWidget> IconButton(FName InTag, EDreamChromeIcon Icon, const TAttribute<FText>& Tip, const TAttribute<bool>& bEnabled,
		FOnClicked OnClicked, const TAttribute<FSlateColor>& Tint = FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
	{
		return SNew(SButton)
			.AddMetaData<FTagMetaData>(FTagMetaData(InTag))
			.ButtonStyle(&FDreamChrome::FlatButton())
			.ContentPadding(FMargin(3.f, 2.f))
			.IsFocusable(false)
			.ToolTipText(Tip)
			.IsEnabled(bEnabled)
			.OnClicked(OnClicked)
			[
				IconImage(Icon, Tint)
			];
	}

	/** A small flat toggle (Echo commands, Line numbers): the checked state is a filled, outlined face. */
	TSharedRef<SWidget> IconToggle(FName InTag, EDreamChromeIcon Icon, const TAttribute<FText>& Tip, const TAttribute<ECheckBoxState>& State,
		FOnCheckStateChanged OnChanged)
	{
		return SNew(SCheckBox)
			.AddMetaData<FTagMetaData>(FTagMetaData(InTag))
			.Style(&FDreamChrome::FlatToggle())
			.Padding(FMargin(3.f, 2.f))
			.IsFocusable(false)
			.ToolTipText(Tip)
			.IsChecked(State)
			.OnCheckStateChanged(OnChanged)
			[
				IconImage(Icon, FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
			];
	}

	/** The thin group separator of the icon row. */
	TSharedRef<SWidget> Separator()
	{
		return SNew(SBox)
			.WidthOverride(1.f)
			.HeightOverride(14.f)
			.Padding(FMargin(0.f))
			[
				SNew(SImage).Image(FDreamChrome::Brush(EDreamChromeSlot::Border))
			];
	}

	ECheckBoxState CheckState(bool bOn)
	{
		return bOn ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
	}

	/** The View toggle a command drives (ToggleReplTab ... ToggleLineNumbers); false for any other command. */
	bool ViewSettingOf(EDreamCommand Command, EDreamViewSetting& OutSetting)
	{
		switch (Command)
		{
		case EDreamCommand::ToggleReplTab: OutSetting = EDreamViewSetting::ReplTab; return true;
		case EDreamCommand::ToggleProblemsTab: OutSetting = EDreamViewSetting::ProblemsTab; return true;
		case EDreamCommand::ToggleReplPrompt: OutSetting = EDreamViewSetting::ReplPrompt; return true;
		case EDreamCommand::ToggleRunHeaders: OutSetting = EDreamViewSetting::RunHeaders; return true;
		case EDreamCommand::ToggleStatusLine: OutSetting = EDreamViewSetting::StatusLine; return true;
		case EDreamCommand::ToggleLineNumbers: OutSetting = EDreamViewSetting::LineNumbers; return true;
		case EDreamCommand::ToggleProjectPanel: OutSetting = EDreamViewSetting::ProjectPanel; return true;       // (sidebar)
		case EDreamCommand::ToggleStructurePanel: OutSetting = EDreamViewSetting::StructurePanel; return true;   // (sidebar)
		default: return false;
		}
	}

	EDreamCommand CommandByName(FName Name)
	{
		for (int32 Index = 1; Index < static_cast<int32>(EDreamCommand::Count); ++Index)
		{
			if (FDreamCommands::GetCommandName(static_cast<EDreamCommand>(Index)) == Name)
			{
				return static_cast<EDreamCommand>(Index);
			}
		}
		return EDreamCommand::None;
	}
}

bool SDreamTab::IsViewToggleOn(EDreamCommand Command) const
{
	EDreamViewSetting Setting;
	if (!DreamToolbarLocal::ViewSettingOf(Command, Setting))
	{
		return false;
	}
	if (Command == EDreamCommand::ToggleReplPrompt && OutputArea.IsValid())
	{
		return OutputArea->IsPromptShown();   // what the user sees: the setting, or Focus REPL's reveal
	}
	return UDreamUserSettings::Get()->GetViewSetting(Setting);
}

void SDreamTab::ToggleViewSetting(EDreamCommand Command)
{
	EDreamViewSetting Setting;
	if (!DreamToolbarLocal::ViewSettingOf(Command, Setting))
	{
		return;
	}
	const bool bOn = IsViewToggleOn(Command);
	if (Command == EDreamCommand::ToggleReplPrompt && bOn && OutputArea.IsValid())
	{
		OutputArea->EndPromptReveal(/*bEvenIfTyped*/ true);
	}
	UDreamUserSettings::SetViewSetting(Setting, !bOn);   // saves and broadcasts OnViewSettingsChanged (HandleViewSettingsChanged)
}

void SDreamTab::BindMenuCommands()
{
	Impl->MenuCommands = MakeShared<FUICommandList>();
	if (!FDreamCommands::IsRegistered())
	{
		return;
	}
	const FDreamCommands& Commands = FDreamCommands::Get();
	for (int32 Index = 1; Index < static_cast<int32>(EDreamCommand::Count); ++Index)
	{
		const EDreamCommand Command = static_cast<EDreamCommand>(Index);
		const TSharedPtr<FUICommandInfo> Info = Commands.GetInfo(Command);
		if (!Info.IsValid() || Command == EDreamCommand::OpenDream)
		{
			continue;
		}
		Impl->MenuCommands->MapAction(Info,
			FExecuteAction::CreateSP(this, &SDreamTab::ExecuteDreamCommand, Command),
			FCanExecuteAction::CreateLambda([this, Command]() { FText Reason; return CanExecuteDreamCommand(Command, Reason); }),
			FGetActionCheckState::CreateLambda([this, Command]() -> ECheckBoxState
			{
				using DreamToolbarLocal::CheckState;
				const TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
				switch (Command)
				{
				case EDreamCommand::ToggleOneUndoStepPerRun:
				{
					// (run group, B40) Mixed while the toggle is untouched and the per-route settings differ (A56).
					const TOptional<bool> Shown = Buffer.IsValid() ? FDreamRunService::TabToggleShown(Buffer->GetViewState()) : TOptional<bool>(false);
					return Shown.IsSet() ? CheckState(Shown.GetValue()) : ECheckBoxState::Undetermined;
				}
				case EDreamCommand::RunInSessionOrIsolated:
					return CheckState(Buffer.IsValid() && Buffer->GetViewState().Namespace == EDreamNamespaceMode::Isolated);
				case EDreamCommand::ToggleEchoInput:
					return CheckState(FDreamEditorModule::Get().GetOutputStore().GetVisibility().bEcho);
				case EDreamCommand::MoveOutputToTop:
					return CheckState(Impl->Position == EDreamOutputPosition::Top);
				case EDreamCommand::MoveOutputToBottom:
					return CheckState(Impl->Position == EDreamOutputPosition::Bottom);
				case EDreamCommand::ToggleTabMovesFocus:
					return CheckState(FDreamKeyRouter::Get().IsTabMovesFocus());
				case EDreamCommand::ToggleReplTab:
				case EDreamCommand::ToggleProblemsTab:
				case EDreamCommand::ToggleReplPrompt:
				case EDreamCommand::ToggleRunHeaders:
				case EDreamCommand::ToggleStatusLine:
				case EDreamCommand::ToggleLineNumbers:
				case EDreamCommand::ToggleProjectPanel:     // (sidebar)
				case EDreamCommand::ToggleStructurePanel:   // (sidebar)
					return CheckState(IsViewToggleOn(Command));
				case EDreamCommand::TogglePluginPythonFolders:   // (sidebar) PO-05-A1: the per-project setting, read live
					return CheckState(FDreamProjectFiles::IsAddPluginFolders());
				default:
					return ECheckBoxState::Unchecked;
				}
			}));
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The menu bar and the icon row
// ---------------------------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SDreamTab::BuildToolbar()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ BuildMenuBar() ]
		+ SVerticalBox::Slot().AutoHeight()[ BuildIconRow() ]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).HeightOverride(1.f)[ SNew(SImage).Image(FDreamChrome::Brush(EDreamChromeSlot::Border)) ]
		];
}

TSharedRef<SWidget> SDreamTab::BuildMenuBar()
{
	using namespace DreamToolbarLocal;
	struct FTitle
	{
		const TCHAR* Name;
		FText Label;
		TSharedRef<SWidget> (SDreamTab::*Build)();
	};
	const FTitle Titles[] =
	{
		{ TEXT("File"), LOCTEXT("FileMenu", "File"), &SDreamTab::BuildFileMenu },
		{ TEXT("Edit"), LOCTEXT("Chrome.EditMenu", "Edit"), &SDreamTab::BuildEditMenu },
		{ TEXT("History"), LOCTEXT("Chrome.HistoryMenu", "History"), &SDreamTab::BuildHistoryMenu },
		{ TEXT("Command"), LOCTEXT("Chrome.CommandMenu", "Command"), &SDreamTab::BuildCommandMenu },
		{ TEXT("View"), LOCTEXT("ViewMenu", "View"), &SDreamTab::BuildViewMenu },
		{ TEXT("Help"), LOCTEXT("Chrome.HelpMenu", "Help"), &SDreamTab::BuildHelpMenu },
	};
	TSharedRef<SHorizontalBox> Bar = SNew(SHorizontalBox);
	Impl->MenuTitles.Reset();
	for (const FTitle& Title : Titles)
	{
		TSharedPtr<SComboButton> Combo;
		const auto Build = Title.Build;
		Bar->AddSlot()
			.AutoWidth()
			[
				SAssignNew(Combo, SComboButton)
				.AddMetaData<FTagMetaData>(FTagMetaData(ChromeTag(TEXT("Menu."), Title.Name)))
				.ComboButtonStyle(&FDreamChrome::FlatCombo())
				.HasDownArrow(false)
				.IsFocusable(false)
				.ContentPadding(FMargin(6.f, 1.f))
				.OnGetMenuContent_Lambda([this, Build]() { return (this->*Build)(); })
				.ButtonContent()
				[
					SNew(STextBlock)
					.Font(FDreamChrome::Font(9))
					.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
					.Text(Title.Label)
				]
			];
		Impl->MenuTitles.Add(Combo);
	}
	// A menu bar's hover rule: while one of its menus is open, hovering another title opens that one instead.
	for (const TSharedPtr<SComboButton>& Combo : Impl->MenuTitles)
	{
		const TWeakPtr<SComboButton> WeakCombo = Combo;
		Combo->SetOnMouseEnter(FNoReplyPointerEventHandler::CreateLambda([this, WeakCombo](const FGeometry&, const FPointerEvent&)
		{
			const TSharedPtr<SComboButton> Hovered = WeakCombo.Pin();
			for (const TSharedPtr<SComboButton>& Other : Impl->MenuTitles)
			{
				if (Hovered.IsValid() && Other.IsValid() && Other != Hovered && Other->IsOpen())
				{
					Other->SetIsOpen(false);
					Hovered->SetIsOpen(true);
					return;
				}
			}
		}));
	}
	return SNew(SBox)
		.Padding(FMargin(2.f, 1.f, 2.f, 0.f))
		[
			Bar
		];
}

TSharedRef<SWidget> SDreamTab::BuildIconRow()
{
	using namespace DreamToolbarLocal;
	auto Enabled = [this](EDreamCommand Command)
	{
		return TAttribute<bool>::CreateLambda([this, Command]() { FText Reason; return CanExecuteDreamCommand(Command, Reason); });
	};
	auto Run = [this](EDreamCommand Command)
	{
		return FOnClicked::CreateLambda([this, Command]() { ExecuteDreamCommand(Command); return FReply::Handled(); });
	};
	auto Tip = [](EDreamCommand Command)
	{
		return TAttribute<FText>::CreateLambda([Command]() { return Tooltip(Command); });
	};
	const FMargin Gap(1.f, 0.f);
	const FMargin GroupGap(6.f, 0.f);

	return SNew(SBox)
		.Padding(FMargin(4.f, 1.f, 4.f, 2.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(Gap).VAlign(VAlign_Center)
			[
				IconButton(CommandTag(EDreamCommand::OpenFile), EDreamChromeIcon::Open, Tip(EDreamCommand::OpenFile), true, Run(EDreamCommand::OpenFile))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(Gap).VAlign(VAlign_Center)
			[
				IconButton(CommandTag(EDreamCommand::Save), EDreamChromeIcon::Save, Tip(EDreamCommand::Save), Enabled(EDreamCommand::Save), Run(EDreamCommand::Save))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(GroupGap).VAlign(VAlign_Center)[ Separator() ]
			// The split Run button (U6): the face runs the selection or buffer; the arrow holds the route, undo and namespace
			// choices. The face's tooltip names the namespace, as the old "▶ Run · Session" label did.
			+ SHorizontalBox::Slot().AutoWidth().Padding(Gap).VAlign(VAlign_Center)
			[
				IconButton(CommandTag(EDreamCommand::RunSelectionOrBuffer), EDreamChromeIcon::Run,
					TAttribute<FText>::CreateLambda([this]()
					{
						if (FDreamEditorModule::Get().GetRunService().GetState() == EDreamRunState::Starting)
						{
							return LOCTEXT("RunStarting", "Starting…");
						}
						const TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
						const bool bIsolated = Buffer.IsValid() && Buffer->GetViewState().Namespace == EDreamNamespaceMode::Isolated;
						return LabelTip(FText::Format(LOCTEXT("Chrome.RunTip", "Run the selection, or everything ({0})"),
							bIsolated ? LOCTEXT("RunNsIsolated", "Isolated") : LOCTEXT("RunNsSession", "Session")), EDreamCommand::RunSelectionOrBuffer);
					}),
					Enabled(EDreamCommand::RunSelectionOrBuffer), Run(EDreamCommand::RunSelectionOrBuffer),
					FDreamChrome::ColorAttr(EDreamChromeSlot::Accent))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.AddMetaData<FTagMetaData>(FTagMetaData(ChromeTag(TEXT("Menu."), TEXT("Run"))))
				.ComboButtonStyle(&FDreamChrome::FlatCombo())
				.HasDownArrow(false)
				.IsFocusable(false)
				.ContentPadding(FMargin(1.f, 2.f))
				.ToolTipText(LOCTEXT("RunMenuTip", "Run selection, buffer, line or file; transaction and namespace"))
				.OnGetMenuContent(this, &SDreamTab::BuildRunMenu)
				.ButtonContent()
				[
					SNew(SImage)
					.Image(FDreamChrome::Icon(EDreamChromeIcon::ChevronDown))
					.DesiredSizeOverride(FVector2D(10.0, 10.0))
					.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(Gap).VAlign(VAlign_Center)
			[
				IconButton(CommandTag(EDreamCommand::RunLine), EDreamChromeIcon::RunLine, Tip(EDreamCommand::RunLine), Enabled(EDreamCommand::RunLine),
					Run(EDreamCommand::RunLine))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(Gap).VAlign(VAlign_Center)
			[
				IconButton(CommandTag(EDreamCommand::Stop), EDreamChromeIcon::Stop, Tip(EDreamCommand::Stop), Enabled(EDreamCommand::Stop),
					FOnClicked::CreateLambda([]()
					{
						FDreamEditorModule::Get().GetRunService().RequestStopActive(EDreamStopSource::RunWindowButton);
						return FReply::Handled();
					}),
					TAttribute<FSlateColor>::CreateLambda([]()
					{
						// Danger while a run is active; the disabled face otherwise.
						const bool bActive = FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetRunService().GetState() != EDreamRunState::Idle;
						return FDreamChrome::Color(bActive ? EDreamChromeSlot::Danger : EDreamChromeSlot::MutedText);
					}))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(GroupGap).VAlign(VAlign_Center)[ Separator() ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(Gap).VAlign(VAlign_Center)
			[
				IconButton(CommandTag(EDreamCommand::ClearOutput), EDreamChromeIcon::Clear,
					TAttribute<FText>::CreateLambda([]() { return LabelTip(LOCTEXT("Chrome.ClearTip", "Clear history (run logs and REPL history are kept)"), EDreamCommand::ClearOutput); }),
					true, Run(EDreamCommand::ClearOutput))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(Gap).VAlign(VAlign_Center)
			[
				IconButton(CommandTag(EDreamCommand::UndoLastRun), EDreamChromeIcon::UndoRun,
					TAttribute<FText>::CreateLambda([]()
					{
						FText Reason;
						FDreamUndoPreview Preview;
						if (!FDreamEditorModule::Get().GetRunService().CanUndoRun(0, Reason, Preview))
						{
							return FText::Format(LOCTEXT("Chrome.UndoRunRefusedTip", "Undo last run: {0}"), Reason);
						}
						return FText::Format(LOCTEXT("UndoTip", "Undo {0} ({1})"), Preview.Title, FDreamKeyRouter::Get().GetChordText(EDreamCommand::UndoLastRun));
					}),
					Enabled(EDreamCommand::UndoLastRun), Run(EDreamCommand::UndoLastRun))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(GroupGap).VAlign(VAlign_Center)[ Separator() ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(Gap).VAlign(VAlign_Center)
			[
				IconToggle(CommandTag(EDreamCommand::ToggleEchoInput), EDreamChromeIcon::Echo,
					TAttribute<FText>::CreateLambda([]() { return LabelTip(LOCTEXT("Chrome.EchoTip", "Echo commands: show each statement's source before its output"), EDreamCommand::ToggleEchoInput); }),
					TAttribute<ECheckBoxState>::CreateLambda([]()
					{
						return DreamToolbarLocal::CheckState(FDreamEditorModule::Get().GetOutputStore().GetVisibility().bEcho);
					}),
					FOnCheckStateChanged::CreateLambda([this](ECheckBoxState) { ExecuteDreamCommand(EDreamCommand::ToggleEchoInput); }))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(Gap).VAlign(VAlign_Center)
			[
				IconToggle(CommandTag(EDreamCommand::ToggleLineNumbers), EDreamChromeIcon::LineNumbers,
					TAttribute<FText>::CreateLambda([]() { return LabelTip(LOCTEXT("Chrome.LineNumbersTip", "Line numbers"), EDreamCommand::ToggleLineNumbers); }),
					TAttribute<ECheckBoxState>::CreateLambda([this]() { return DreamToolbarLocal::CheckState(IsViewToggleOn(EDreamCommand::ToggleLineNumbers)); }),
					FOnCheckStateChanged::CreateLambda([this](ECheckBoxState) { ExecuteDreamCommand(EDreamCommand::ToggleLineNumbers); }))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)[ SNew(SSpacer) ]
		];
}

// ---------------------------------------------------------------------------------------------------------------------
// The menus. Each Fill* adds one menu's entries; the menu bar, the split arrow and the test hook build from them. An entry
// that is not a command carries a name (its tutorial highlight name), so the hook can list and run every entry.
// ---------------------------------------------------------------------------------------------------------------------

namespace DreamToolbarLocal
{
	/** (icons) PO-06 F6: the dream icon of a command's menu entry, or an empty icon for the commands without one. */
	FSlateIcon CommandIcon(EDreamCommand Command)
	{
		switch (Command)
		{
		case EDreamCommand::OpenFile: return FDreamIcons::SlateIcon(EDreamIcon::Open);
		case EDreamCommand::Save: return FDreamIcons::SlateIcon(EDreamIcon::Save);
		case EDreamCommand::RunSelectionOrBuffer: return FDreamIcons::SlateIcon(EDreamIcon::Run);
		case EDreamCommand::RunLine: return FDreamIcons::SlateIcon(EDreamIcon::RunLine);
		case EDreamCommand::Stop: return FDreamIcons::SlateIcon(EDreamIcon::Stop);
		case EDreamCommand::ClearOutput: return FDreamIcons::SlateIcon(EDreamIcon::Clear);
		case EDreamCommand::UndoLastRun: return FDreamIcons::SlateIcon(EDreamIcon::UndoRun);
		case EDreamCommand::DebugRun: return FDreamIcons::SlateIcon(EDreamIcon::DebugRun);
		case EDreamCommand::ToggleBreakpoint: return FDreamIcons::SlateIcon(EDreamIcon::Breakpoint);
		case EDreamCommand::DebugStepOver: return FDreamIcons::SlateIcon(EDreamIcon::StepOver);
		case EDreamCommand::DebugStepInto: return FDreamIcons::SlateIcon(EDreamIcon::StepInto);
		case EDreamCommand::DebugStepOut: return FDreamIcons::SlateIcon(EDreamIcon::StepOut);
		default: return FSlateIcon();
		}
	}

	void AddCommand(FMenuBuilder& Menu, EDreamCommand Command, const TAttribute<FText>& Label = TAttribute<FText>())
	{
		if (const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(Command))
		{
			Menu.AddMenuEntry(Info, NAME_None, Label, TAttribute<FText>(), CommandIcon(Command));   // (icons) the icon override
		}
	}

	void AddAction(FMenuBuilder& Menu, const TCHAR* Name, const TAttribute<FText>& Label, const TAttribute<FText>& Tip, const FUIAction& Action,
		EUserInterfaceActionType Type = EUserInterfaceActionType::Button)
	{
		Menu.AddMenuEntry(Label, Tip, FSlateIcon(), Action, NAME_None, Type, FName(Name));
	}
}

void SDreamTab::FillRunMenu(FMenuBuilder& Menu)
{
	using namespace DreamToolbarLocal;
	Menu.BeginSection(TEXT("DreamRun"), LOCTEXT("RunSection", "Run"));
	AddCommand(Menu, EDreamCommand::RunSelectionOrBuffer);
	AddCommand(Menu, EDreamCommand::RunBuffer);
	AddCommand(Menu, EDreamCommand::RunLine);
	AddCommand(Menu, EDreamCommand::RunLineAdvance);
	AddCommand(Menu, EDreamCommand::RunFile);
	Menu.EndSection();

	// U6: "Run in / without transaction" apply to the next run only.
	auto CanRun = FCanExecuteAction::CreateLambda([this]() { FText R; return CanExecuteDreamCommand(EDreamCommand::RunSelectionOrBuffer, R); });
	Menu.BeginSection(TEXT("DreamTransaction"), LOCTEXT("TransactionSection", "Undo"));
	AddAction(Menu, TEXT("Dream.RunInTransaction"), LOCTEXT("RunInTxn", "Run in transaction"), LOCTEXT("RunInTxnTip", "Runs once as one undo step"),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			Impl->TransactionOverride = true;
			ExecuteDreamCommand(EDreamCommand::RunSelectionOrBuffer);
		}), CanRun));
	AddAction(Menu, TEXT("Dream.RunWithoutTransaction"), LOCTEXT("RunWithoutTxn", "Run without transaction"), LOCTEXT("RunWithoutTxnTip", "Runs once without an undo step"),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			Impl->TransactionOverride = false;
			ExecuteDreamCommand(EDreamCommand::RunSelectionOrBuffer);
		}), CanRun));
	// (run group, B40) The command's own entry, so a chord the user bound shows; its check state is the command list's (mixed
	// while untouched and the route settings differ, BindMenuCommands) and its tooltip names both kinds of run
	// (FDreamRunService::TabToggleShown / TabToggleTooltip, A56).
	if (const TSharedPtr<FUICommandInfo> OneUndo = FDreamCommands::Get().GetInfo(EDreamCommand::ToggleOneUndoStepPerRun))
	{
		Menu.AddMenuEntry(OneUndo, NAME_None, TAttribute<FText>(), TAttribute<FText>::CreateLambda([this]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
			return Buffer.IsValid() ? FDreamRunService::TabToggleTooltip(Buffer->GetViewState())
				: LOCTEXT("OneUndoTip", "Records each run of this tab as one undo step (Dream: <tab> #N)");
		}));
	}
	AddCommand(Menu, EDreamCommand::UndoLastRun);
	Menu.EndSection();

	Menu.BeginSection(TEXT("DreamNamespace"), LOCTEXT("NamespaceSection", "Namespace"));
	auto NamespaceEntry = [this, &Menu](const TCHAR* Name, EDreamNamespaceMode Mode, const FText& Label, const FText& Tip)
	{
		AddAction(Menu, Name, Label, Tip,
			FUIAction(
				FExecuteAction::CreateLambda([this, Mode]()
				{
					if (TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer())
					{
						Buffer->GetViewState().Namespace = Mode;
						FDreamEditorModule::Get().GetJournal().OnViewStateChanged(Buffer->GetId());   // the crash copy (§10.3)
						SaveSessionState(false);
					}
				}),
				FCanExecuteAction::CreateLambda([this]() { return GetActiveBuffer().IsValid(); }),
				FIsActionChecked::CreateLambda([this, Mode]()
				{
					const TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
					return Buffer.IsValid() && Buffer->GetViewState().Namespace == Mode;
				})),
			EUserInterfaceActionType::RadioButton);
	};
	NamespaceEntry(TEXT("Dream.NamespaceSession"), EDreamNamespaceMode::Session, LOCTEXT("NsSessionEntry", "Session"),
		LOCTEXT("NsSessionTip", "Runs share the editor's Python console namespace"));
	NamespaceEntry(TEXT("Dream.NamespaceIsolated"), EDreamNamespaceMode::Isolated, LOCTEXT("NsIsolatedEntry", "Isolated"),
		LOCTEXT("NsIsolatedTip", "Each run gets a fresh namespace; no auto-print"));
	AddCommand(Menu, EDreamCommand::ResetSession);
	Menu.EndSection();
}

void SDreamTab::FillFileMenu(FMenuBuilder& Menu)
{
	using namespace DreamToolbarLocal;
	AddCommand(Menu, EDreamCommand::NewTab);
	AddCommand(Menu, EDreamCommand::OpenFile);
	// (buffers) BUF-03 M1 Recent Files: most recent first, kept per project; a missing file says so in its tooltip and leaves the list when chosen.
	Menu.AddSubMenu(LOCTEXT("RecentFilesMenu", "Recent Files"), LOCTEXT("RecentFilesMenuTip", "Files opened, saved or closed recently in this project"),
		FNewMenuDelegate::CreateSP(this, &SDreamTab::FillRecentFilesMenu), false, FSlateIcon(), true, NAME_None, FName(TEXT("Dream.RecentFiles")));
	Menu.AddMenuSeparator();
	AddCommand(Menu, EDreamCommand::Save);
	AddCommand(Menu, EDreamCommand::SaveAs);
	Menu.AddMenuSeparator();
	AddCommand(Menu, EDreamCommand::CloseTab);
	AddCommand(Menu, EDreamCommand::ReopenClosedTab);
	AddCommand(Menu, EDreamCommand::NextTab);
	AddCommand(Menu, EDreamCommand::PreviousTab);
	Menu.AddMenuSeparator();
	// (persist) Leave safe mode (§10.8), also here: the safe-mode banner line can be hidden with its × (PO-04).
	AddAction(Menu, TEXT("Dream.LeaveSafeMode"), LOCTEXT("Chrome.LeaveSafeMode", "Leave Safe Mode"),
		LOCTEXT("Chrome.LeaveSafeModeTip", "Applies the skipped session restore and starts the services safe mode kept off"),
		FUIAction(FExecuteAction::CreateLambda([this]() { LeaveSafeMode(); }),
			FCanExecuteAction::CreateLambda([this]() { return IsInSafeMode(); })));
	// (persist) The Recovery review panel (§10.7.3), also reachable here once its banner line is dismissed (PO-04).
	AddAction(Menu, TEXT("Dream.RecoveryReview"), LOCTEXT("Chrome.RecoveryReview", "Review Recovered Buffers"),
		LOCTEXT("Chrome.RecoveryReviewTip", "Each recovered buffer with Restore, Restore as new tab, Compare and Discard"),
		FUIAction(FExecuteAction::CreateLambda([this]() { ToggleRecoveryReview(); }),
			FCanExecuteAction::CreateLambda([this]() { return Impl->PendingRecoveries.Num() > 0 || Impl->bSafeMode || IsRecoveryReviewOpen(); }),
			FIsActionChecked::CreateLambda([this]() { return IsRecoveryReviewOpen(); })),
		EUserInterfaceActionType::ToggleButton);
	// Deferred (M2): Save All, Revert, Rename on disk and Close Others (BUF-03 M2 rows).
}

void SDreamTab::FillRecentFilesMenu(FMenuBuilder& Sub)
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return;
	}
	const TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	const TArray<FString> Recent = FDreamEditorModule::Get().GetBufferManager().GetRecentFiles();
	if (Recent.Num() == 0)
	{
		Sub.AddMenuEntry(LOCTEXT("RecentFilesNone", "No recent files"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return;
	}
	for (const FString& Path : Recent)
	{
		// A missing file stays clickable: the click removes it from the list with a status message (OpenRecentFile).
		const bool bExists = IFileManager::Get().FileExists(*Path);
		Sub.AddMenuEntry(FText::FromString(FPaths::GetCleanFilename(Path)),
			bExists ? FText::FromString(Path)
				: FText::Format(LOCTEXT("RecentFileMissingTip2", "{0} no longer exists; choosing it removes it from this list"), FText::FromString(Path)),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([WeakTab, Path]()
			{
				if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
				{
					Tab->OpenRecentFile(Path);
				}
			})));
	}
	Sub.AddMenuSeparator();
	Sub.AddMenuEntry(LOCTEXT("RecentFilesClear", "Clear Recent Files"), LOCTEXT("RecentFilesClearTip", "Empties this list"), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetBufferManager().ClearRecentFiles();
			}
		})), NAME_None, EUserInterfaceActionType::Button, FName(TEXT("Dream.ClearRecentFiles")));
}

void SDreamTab::FillEditMenu(FMenuBuilder& Menu)
{
	using namespace DreamToolbarLocal;
	AddCommand(Menu, EDreamCommand::Undo);
	AddCommand(Menu, EDreamCommand::Redo);
	Menu.AddMenuSeparator();
	AddCommand(Menu, EDreamCommand::DuplicateLine);
	AddCommand(Menu, EDreamCommand::ToggleComment);
	Menu.AddMenuSeparator();
	AddCommand(Menu, EDreamCommand::Find);
	AddCommand(Menu, EDreamCommand::Replace);
	AddCommand(Menu, EDreamCommand::FindNext);
	AddCommand(Menu, EDreamCommand::FindPrevious);
	AddCommand(Menu, EDreamCommand::GoToLine);
	Menu.AddMenuSeparator();
	AddCommand(Menu, EDreamCommand::Complete);
	AddCommand(Menu, EDreamCommand::QuickFix);
	AddCommand(Menu, EDreamCommand::ToggleTabMovesFocus);
}

void SDreamTab::FillHistoryMenu(FMenuBuilder& Menu)
{
	using namespace DreamToolbarLocal;
	AddCommand(Menu, EDreamCommand::ClearOutput, LOCTEXT("Chrome.ClearHistory", "Clear History"));
	AddCommand(Menu, EDreamCommand::ToggleEchoInput, LOCTEXT("Chrome.EchoCommands", "Echo Commands"));
	Menu.AddMenuSeparator();
	// OUT-03: the per-type toggles (§6.4 "Design: filters"), the same entries as the output area's Filter button.
	Menu.AddSubMenu(LOCTEXT("ShowOutputMenu", "Show in Output"), LOCTEXT("ShowOutputMenuTip", "Show or hide output lines by type"),
		FNewMenuDelegate::CreateSP(this, &SDreamTab::FillShowInOutputMenu), false, FSlateIcon(), true, NAME_None, FName(TEXT("Dream.ShowInOutput")));
	AddCommand(Menu, EDreamCommand::ExpandEcho);
	AddCommand(Menu, EDreamCommand::FoldEcho);
	Menu.AddMenuSeparator();
	AddCommand(Menu, EDreamCommand::NextError);
	AddCommand(Menu, EDreamCommand::PrevError);
	AddCommand(Menu, EDreamCommand::NextProblem);
	AddCommand(Menu, EDreamCommand::PrevProblem);
	AddCommand(Menu, EDreamCommand::NextProblemInFile);   // (lang) PO-06 F4
	AddCommand(Menu, EDreamCommand::PrevProblemInFile);   // (lang)
}

void SDreamTab::FillShowInOutputMenu(FMenuBuilder& Sub)
{
	SDreamOutputView::AddShowMenuEntries(Sub, TWeakPtr<IDreamCommandTarget>(SharedThis(this)));
}

void SDreamTab::FillCommandMenu(FMenuBuilder& Menu)
{
	using namespace DreamToolbarLocal;
	FillRunMenu(Menu);
	Menu.BeginSection(TEXT("DreamCommandMore"));
	AddCommand(Menu, EDreamCommand::Stop);
	AddCommand(Menu, EDreamCommand::FocusRepl);
	Menu.EndSection();
	// (debug) PO-06 F5: the debugger (TDD §8.3.5); the step entries act in the Paused window.
	Menu.BeginSection(TEXT("DreamDebug"), LOCTEXT("Debug.MenuSection", "Debug"));
	AddCommand(Menu, EDreamCommand::DebugRun);
	AddCommand(Menu, EDreamCommand::ToggleBreakpoint);
	AddCommand(Menu, EDreamCommand::DebugStepOver);
	AddCommand(Menu, EDreamCommand::DebugStepInto);
	AddCommand(Menu, EDreamCommand::DebugStepOut);
	Menu.EndSection();
}

void SDreamTab::FillViewMenu(FMenuBuilder& Menu)
{
	using namespace DreamToolbarLocal;
	// (sidebar) PO-05: the side bar's sections (on by default) and focus into them.
	Menu.BeginSection(TEXT("DreamViewSideBar"), LOCTEXT("SideBar.MenuSection", "Side Bar"));
	AddCommand(Menu, EDreamCommand::ToggleProjectPanel);
	AddCommand(Menu, EDreamCommand::ToggleStructurePanel);
	AddCommand(Menu, EDreamCommand::FocusProject);
	AddCommand(Menu, EDreamCommand::FocusStructure);
	AddCommand(Menu, EDreamCommand::TogglePluginPythonFolders);   // (sidebar) PO-05-A1
	Menu.EndSection();
	Menu.BeginSection(TEXT("DreamViewParts"), LOCTEXT("Chrome.ShowSection", "Show"));
	AddCommand(Menu, EDreamCommand::ToggleReplTab);
	AddCommand(Menu, EDreamCommand::ToggleProblemsTab);
	AddCommand(Menu, EDreamCommand::ToggleReplPrompt);
	AddCommand(Menu, EDreamCommand::ToggleRunHeaders);
	AddCommand(Menu, EDreamCommand::ToggleStatusLine);
	AddCommand(Menu, EDreamCommand::ToggleLineNumbers);
	// A dismissed banner line (×) comes back from here (PO-04: every hidden part is one View toggle away).
	AddAction(Menu, TEXT("Dream.ShowHiddenNotices"), LOCTEXT("Chrome.ShowNotices", "Show Hidden Notices"),
		LOCTEXT("Chrome.ShowNoticesTip", "Shows again the banner lines closed with ×"),
		FUIAction(FExecuteAction::CreateLambda([this]() { ShowHiddenNotices(); }),
			FCanExecuteAction::CreateLambda([this]() { return HasHiddenNotices(); })));
	Menu.EndSection();
	Menu.BeginSection(TEXT("DreamViewLayout"), LOCTEXT("Chrome.LayoutSection", "Layout"));
	Menu.AddSubMenu(LOCTEXT("Chrome.ThemeMenu", "Theme"), LOCTEXT("Chrome.ThemeMenuTip", "The colours of the whole Dream tab"),
		FNewMenuDelegate::CreateSP(this, &SDreamTab::FillThemeMenu), false, FSlateIcon(), true, NAME_None, FName(TEXT("Dream.Theme")));
	Menu.AddSubMenu(LOCTEXT("OutputPositionMenu", "Output Position"), LOCTEXT("OutputPositionTip", "Where the output area sits (WS-03)"),
		FNewMenuDelegate::CreateSP(this, &SDreamTab::FillOutputPositionMenu), false, FSlateIcon(), true, NAME_None, FName(TEXT("Dream.OutputPosition")));
	Menu.EndSection();
	Menu.BeginSection(TEXT("DreamViewZoom"), LOCTEXT("Chrome.ZoomSection", "Zoom"));
	AddCommand(Menu, EDreamCommand::ZoomIn);
	AddCommand(Menu, EDreamCommand::ZoomOut);
	AddCommand(Menu, EDreamCommand::ZoomReset);
	Menu.EndSection();
}

void SDreamTab::FillOutputPositionMenu(FMenuBuilder& Sub)
{
	DreamToolbarLocal::AddCommand(Sub, EDreamCommand::MoveOutputToTop, LOCTEXT("OutputTop", "Top"));
	DreamToolbarLocal::AddCommand(Sub, EDreamCommand::MoveOutputToBottom, LOCTEXT("OutputBottom", "Bottom"));
}

void SDreamTab::FillThemeMenu(FMenuBuilder& Sub)
{
	// PO-04 point 3: View ▸ Theme lists the bundled themes and the user's (FDreamThemes::List, which loads user themes lazily);
	// a choice applies live and persists (UDreamUserSettings::ThemeId).
	for (const TPair<FString, FText>& Theme : FDreamChrome::ListThemes())
	{
		const FString Id = Theme.Key;
		Sub.AddMenuEntry(Theme.Value, FText::Format(LOCTEXT("Chrome.ThemeTip", "Use the {0} theme"), Theme.Value), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Id]() { FDreamChrome::SetTheme(Id); }), FCanExecuteAction(),
				FIsActionChecked::CreateLambda([Id]() { return FDreamChrome::GetThemeId() == Id; })),
			NAME_None, EUserInterfaceActionType::RadioButton, FName(*(TEXT("Dream.Theme.") + Id)));
	}
	// (theme) review r6 m2: the user themes folder and the built-ins as JSON starting points, without the console.
	Sub.AddMenuSeparator();
	Sub.AddMenuEntry(LOCTEXT("Theme.OpenFolder", "Open Themes Folder"),
		FText::Format(LOCTEXT("Theme.OpenFolderTip", "Your theme files (*.json with \"format\": \"dream-theme\"): {0}"), FText::FromString(FDreamThemes::GetUserThemesDir())),
		FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&FDreamThemes::OpenUserThemesFolder)),
		NAME_None, EUserInterfaceActionType::Button, FName(TEXT("Dream.Theme.OpenFolder")));
	Sub.AddMenuEntry(LOCTEXT("Theme.ExportBuiltIns", "Export Built-in Themes"),
		LOCTEXT("Theme.ExportBuiltInsTip", "Writes every built-in theme as JSON to the Built-in subfolder of the themes folder, to copy and edit"),
		FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() { FDreamThemes::ExportBuiltInsToUserFolder(); }),
			FCanExecuteAction::CreateLambda([]() { return !FDreamEditorModule::IsAvailable() || !FDreamEditorModule::Get().GetRunService().IsRunActive(); })),
		NAME_None, EUserInterfaceActionType::Button, FName(TEXT("Dream.Theme.ExportBuiltIns")));
}

void SDreamTab::FillHelpMenu(FMenuBuilder& Menu)
{
	using namespace DreamToolbarLocal;
	AddAction(Menu, TEXT("Dream.Settings"), LOCTEXT("Chrome.SettingsEntry", "Dream Python Editor Settings..."),
		LOCTEXT("Chrome.SettingsTip", "Editor Preferences ▸ Plugins ▸ Dream Python Editor"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
			{
				Settings->ShowViewer(TEXT("Editor"), TEXT("Plugins"), TEXT("Dream"));
			}
		})));
	AddAction(Menu, TEXT("Dream.KeyboardShortcuts"), LOCTEXT("Chrome.ShortcutsEntry", "Keyboard Shortcuts..."),
		LOCTEXT("Chrome.ShortcutsTip", "Editor Preferences ▸ Keyboard Shortcuts (the Dream Python Editor group)"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
			{
				Settings->ShowViewer(TEXT("Editor"), TEXT("General"), TEXT("InputBindings"));
			}
		})));
	// (scratch, PO-05) The sample scripts are seeded into <Scratch folder>/Samples once; this copies the missing ones again.
	AddAction(Menu, TEXT("Dream.RestoreSamples"), LOCTEXT("Scratch.RestoreSamplesEntry", "Restore Sample Scripts"),
		LOCTEXT("Scratch.RestoreSamplesTip", "Copies the sample scripts that are missing into the Samples folder of the scratch folder (existing files are kept)"),
		FUIAction(FExecuteAction::CreateLambda([WeakTab = TWeakPtr<SDreamTab>(SharedThis(this))]()
		{
			if (const TSharedPtr<SDreamTab> Tab = WeakTab.Pin(); Tab.IsValid() && FDreamEditorModule::IsAvailable())
			{
				Tab->RestoreSampleScripts();
			}
		})));
	Menu.AddMenuSeparator();
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor"));
	const FText Version = Plugin.IsValid() ? FText::FromString(Plugin->GetDescriptor().VersionName) : FText::GetEmpty();
	AddAction(Menu, TEXT("Dream.About"), FText::Format(LOCTEXT("Chrome.AboutEntry", "Dream Python Editor {0}"), Version),
		LOCTEXT("Chrome.AboutTip", "A Python script editor for Unreal Engine"),
		FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
}

TSharedRef<SWidget> SDreamTab::MakeMenu(void (SDreamTab::*Fill)(FMenuBuilder&))
{
	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ true, Impl->MenuCommands);
	(this->*Fill)(Menu);
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SDreamTab::BuildRunMenu() { return MakeMenu(&SDreamTab::FillRunMenu); }
TSharedRef<SWidget> SDreamTab::BuildFileMenu() { return MakeMenu(&SDreamTab::FillFileMenu); }
TSharedRef<SWidget> SDreamTab::BuildEditMenu() { return MakeMenu(&SDreamTab::FillEditMenu); }
TSharedRef<SWidget> SDreamTab::BuildHistoryMenu() { return MakeMenu(&SDreamTab::FillHistoryMenu); }
TSharedRef<SWidget> SDreamTab::BuildCommandMenu() { return MakeMenu(&SDreamTab::FillCommandMenu); }
TSharedRef<SWidget> SDreamTab::BuildViewMenu() { return MakeMenu(&SDreamTab::FillViewMenu); }
TSharedRef<SWidget> SDreamTab::BuildHelpMenu() { return MakeMenu(&SDreamTab::FillHelpMenu); }

SDreamTab::FMenuFill SDreamTab::FindMenuFillForTest(FName MenuName)
{
	static const TPair<const TCHAR*, FMenuFill> Menus[] =
	{
		{ TEXT("File"), &SDreamTab::FillFileMenu },
		{ TEXT("Edit"), &SDreamTab::FillEditMenu },
		{ TEXT("History"), &SDreamTab::FillHistoryMenu },
		{ TEXT("Command"), &SDreamTab::FillCommandMenu },
		{ TEXT("View"), &SDreamTab::FillViewMenu },
		{ TEXT("Help"), &SDreamTab::FillHelpMenu },
		{ TEXT("Run"), &SDreamTab::FillRunMenu },
		{ TEXT("RecentFiles"), &SDreamTab::FillRecentFilesMenu },
		{ TEXT("ShowInOutput"), &SDreamTab::FillShowInOutputMenu },
		{ TEXT("Theme"), &SDreamTab::FillThemeMenu },
		{ TEXT("OutputPosition"), &SDreamTab::FillOutputPositionMenu },
	};
	for (const auto& Entry : Menus)
	{
		if (MenuName == FName(Entry.Key))
		{
			return Entry.Value;
		}
	}
	return nullptr;
}

TArray<FString> SDreamTab::GetMenuLabelsForTest(FName MenuName)
{
	TArray<FString> Out;
	const FMenuFill Fill = FindMenuFillForTest(MenuName);
	if (Fill == nullptr || !FDreamCommands::IsRegistered())
	{
		return Out;
	}
	FMenuBuilder Menu(true, Impl->MenuCommands);
	(this->*Fill)(Menu);
	// The menu's widget builds its entries' widgets at once; their text blocks hold what the menu shows.
	TArray<TSharedRef<SWidget>> Stack = { Menu.MakeWidget() };
	while (Stack.Num() > 0)
	{
		const TSharedRef<SWidget> Widget = Stack.Pop();
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			Out.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = Children != nullptr ? Children->Num() - 1 : -1; Index >= 0; --Index)
		{
			Stack.Add(Children->GetChildAt(Index));
		}
	}
	return Out;
}

TArray<SDreamTab::FChromeEntry> SDreamTab::GetMenuEntriesForTest(FName MenuName)
{
	const FMenuFill Fill = FindMenuFillForTest(MenuName);
	TArray<FChromeEntry> Out;
	if (Fill == nullptr || !FDreamCommands::IsRegistered())
	{
		return Out;
	}
	FMenuBuilder Menu(true, Impl->MenuCommands);
	(this->*Fill)(Menu);
	for (const TSharedRef<const FMultiBlock>& Block : Menu.GetMultiBox()->GetBlocks())
	{
		if (Block->GetType() != EMultiBlockType::MenuEntry)
		{
			continue;   // separators, headings, widgets
		}
		FChromeEntry Entry;
		if (const TSharedPtr<const FUICommandInfo> Info = Block->GetAction())
		{
			Entry.Command = FDreamCommands::Get().FindCommand(Info);
			Entry.Name = FDreamCommands::GetCommandName(Entry.Command);
			// The action a click runs: the tab's command list's mapping (execute, can-execute, check state).
			if (const FUIAction* Mapped = Impl->MenuCommands.IsValid() ? Impl->MenuCommands->GetActionForCommand(Info) : nullptr)
			{
				Entry.Action = *Mapped;
			}
		}
		else
		{
			Entry.Name = Block->GetTutorialHighlightName();   // NAME_None for an entry with no name of its own (a filter)
			Entry.Command = DreamToolbarLocal::CommandByName(Entry.Name);
			Entry.Action = Block->GetDirectActions();
		}
		Out.Add(MoveTemp(Entry));
	}
	return Out;
}

TArray<FName> SDreamTab::GetMenuTitlesForTest() const
{
	TArray<FName> Out;
	for (const TSharedPtr<SComboButton>& Combo : Impl->MenuTitles)
	{
		const TSharedPtr<FTagMetaData> Meta = Combo.IsValid() ? Combo->GetMetaData<FTagMetaData>() : nullptr;
		if (Meta.IsValid())
		{
			Out.Add(Meta->Tag);
		}
	}
	return Out;
}

#undef LOCTEXT_NAMESPACE
