// SPDX-License-Identifier: Apache-2.0
// Dream.Shell.Chrome: the Maya-minimal chrome of owner ruling PO-04 point 1 (chrome group). With the View settings at their
// defaults the optional parts are hidden: no status line, no output tab row, no REPL prompt, no run headers, no breadcrumb
// row, no language badge, while line numbers show and the caret's line:col sits at the right of the input tab row. Each
// View toggle (REPL tab, Problems tab, REPL prompt, run headers, status line, line numbers) shows its part and hides it
// again. Every former toolbar and menu action is reachable from the menu bar (File, Edit, History, Command, View, Help),
// the split Run menu or the icon row, with the same command, including the One undo step per run toggle's B40 mixed state
// and the theme list; the icon row's toggles work through a real Slate click. The View settings and the theme persist in
// the user's EditorSettings.ini and the theme applies live. When a Run-window run starts, no chrome widget (menu title, icon,
// tab row, banner) holds keyboard focus at its first pump: it is in the code view, or in the Run window when the editor has
// OS focus (§5.5.3 activation per FApp::HasFocus), and it is back in the code view after the run (§5.5.2, A59). Review r6:
// a banner's × hides only what it showed (review r7: by line id; the count line keeps its id when a pending buffer is
// handled, a new line shows again even with the same text) and View ▸ Show Hidden Notices brings it back; the "+"
// opens a tab; transient messages and health warnings (PO-04-R1: journal Degraded, forced with the journal's test hook)
// show beside line:col, a warning first; the REPL placeholder is "Python"; Edit ▸ Complete opens the completion list;
// File ▸ Leave Safe Mode is there. The spec saves every setting it changes and writes back only the keys that changed.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamScratch.h"   // (scratch) R-p1-12
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamCommands.h"
#include "Commands/DreamKeyRouter.h"
#include "CoreGlobals.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Module/DreamEditorModule.h"
#include "Misc/App.h"
#include "Output/DreamOutputStore.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/DreamOutputRows.h"
#include "Shell/SDreamReplStrip.h"
#include "CodeView/SDreamCompletionPopup.h"
#include "Persistence/DreamJournal.h"
#include "Python/DreamPython.h"
#include "Shell/SDreamRecoveryBanner.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamChrome.h"
#include "Shell/DreamTheme.h"
#include "Styling/SlateBrush.h"
#include "Types/ISlateMetaData.h"
#include "UObject/Package.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace DreamShellChromeSpec
{
	const TCHAR* const SettingsSection = TEXT("/Script/DreamEditor.DreamUserSettings");

	/** Every View setting and the theme, as the spec found them. */
	struct FSaved
	{
		bool bSaved = false;
		bool bValues[6] = {};
		FString ThemeId;                 // UDreamUserSettings::ThemeId (a user theme that is missing now keeps its id)
		bool bTransactionSelectionAndLine = true;
		bool bTransactionBuffer = true;
		bool bEchoInput = true;          // UDreamUserSettings::bEchoInput (the Echo toggle saves it)
		bool bEchoVisible = true;        // the store's echo visibility (what the views show)
		bool bRunPump = true;
		EDreamRunPresentationSetting RunPresentation = EDreamRunPresentationSetting::RunWindow;
	};

	const EDreamViewSetting AllSettings[] = { EDreamViewSetting::ReplTab, EDreamViewSetting::ProblemsTab, EDreamViewSetting::ReplPrompt,
		EDreamViewSetting::RunHeaders, EDreamViewSetting::StatusLine, EDreamViewSetting::LineNumbers };

	/** The declared defaults (PO-04): everything off but line numbers. */
	bool DefaultOf(EDreamViewSetting Setting)
	{
		return Setting == EDreamViewSetting::LineNumbers;
	}

	void Collect(const TSharedRef<SWidget>& Root, TFunctionRef<bool(const TSharedRef<SWidget>&)> Match, TArray<TSharedRef<SWidget>>& Out)
	{
		if (Match(Root))
		{
			Out.Add(Root);
		}
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			Collect(Children->GetChildAt(Index), Match, Out);
		}
	}

	TSharedPtr<SWidget> FindTagged(const TSharedRef<SWidget>& Root, FName Tag)
	{
		TArray<TSharedRef<SWidget>> Found;
		Collect(Root, [Tag](const TSharedRef<SWidget>& W)
		{
			const TSharedPtr<FTagMetaData> Meta = W->GetMetaData<FTagMetaData>();
			return Meta.IsValid() && Meta->Tag == Tag;
		}, Found);
		return Found.Num() > 0 ? TSharedPtr<SWidget>(Found[0]) : nullptr;
	}

	/** Widget and every widget between it and Stop are visible (a collapsed parent hides it whatever its own state). */
	bool IsShownUnder(const TSharedPtr<SWidget>& Widget, const TSharedPtr<SWidget>& Stop)
	{
		if (!Widget.IsValid())
		{
			return false;
		}
		for (TSharedPtr<SWidget> W = Widget; W.IsValid() && W != Stop; W = W->GetParentWidget())
		{
			if (!W->GetVisibility().IsVisible())
			{
				return false;
			}
		}
		return true;
	}

	/** A left click at the centre of Widget routed along its path, as Slate routes a platform click (Dream.Keys.Focus). */
	bool Click(const TSharedRef<SWidget>& Widget)
	{
		FSlateApplication& App = FSlateApplication::Get();
		FWidgetPath Path;
		if (!DreamTest::MakePointerPath(Widget, Path))
		{
			return false;
		}
		const FVector2D At(Widget->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
		const uint32 User = App.GetUserIndexForMouse();
		const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
		TSet<FKey> Pressed;
		Pressed.Add(EKeys::LeftMouseButton);
		// Two moves: the first enters the widget from wherever the pointer was, so it is hovered when the button goes down.
		App.RoutePointerMoveEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState()), false);
		App.RoutePointerMoveEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState()), false);
		App.RoutePointerDownEvent(Path, FPointerEvent(User, Pointer, At, At, Pressed, EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		App.RoutePointerUpEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		return true;
	}

	/** The prompt strip is shown: the strip and every widget up to the output area are visible. */
	bool IsPromptStripShown(SDreamOutputArea& Area)
	{
		return IsShownUnder(Area.GetReplStrip(), Area.AsShared());
	}

	/** The active code view's line-number gutter is shown. */
	bool IsGutterShown(SDreamTab& Tab)
	{
		const TSharedPtr<SDreamCodeView> View = Tab.GetActiveCodeView();
		if (!View.IsValid())
		{
			return false;
		}
		TArray<TSharedRef<SWidget>> Gutters;
		Collect(View.ToSharedRef(), [](const TSharedRef<SWidget>& W) { return W->GetType() == FName(TEXT("SDreamGutter")); }, Gutters);
		return Gutters.Num() > 0 && IsShownUnder(Gutters[0], View);
	}

	FString CommandName(EDreamCommand Command)
	{
		return FDreamCommands::GetCommandName(Command).ToString();
	}
}

BEGIN_DEFINE_SPEC(FDreamShellChromeSpec, "Dream.Shell.Chrome", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	TSharedPtr<DreamShellChromeSpec::FSaved> Saved;

	/** The entry names of every menu-bar menu, the split Run menu and the submenus (the icon row is checked by its tags). */
	TSet<FString> ReachableNames();
	void RestoreSettings();
END_DEFINE_SPEC(FDreamShellChromeSpec)

TSet<FString> FDreamShellChromeSpec::ReachableNames()
{
	TSet<FString> Names;
	for (const TCHAR* Menu : { TEXT("File"), TEXT("Edit"), TEXT("History"), TEXT("Command"), TEXT("View"), TEXT("Help"), TEXT("Run"),
		TEXT("RecentFiles"), TEXT("ShowInOutput"), TEXT("Theme"), TEXT("OutputPosition") })
	{
		for (const SDreamTab::FChromeEntry& Entry : Tab->GetMenuEntriesForTest(FName(Menu)))
		{
			Names.Add(Entry.Name.ToString());
		}
	}
	return Names;
}

void FDreamShellChromeSpec::RestoreSettings()
{
	using namespace DreamShellChromeSpec;
	if (!Saved.IsValid() || !Saved->bSaved || !UObjectInitialized())
	{
		return;
	}
	// Each setter and SaveKey below writes one key, and only for a value that changed (never the whole section, which would
	// write another spec's in-memory pins).
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(AllSettings); ++Index)
	{
		UDreamUserSettings::SetViewSetting(AllSettings[Index], Saved->bValues[Index]);   // writes only when it changes
	}
	UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
	if (Settings->ThemeId != Saved->ThemeId)
	{
		UDreamUserSettings::SetThemeId(Saved->ThemeId);   // the theme follows the setting (FDreamThemes listens)
	}
	if (FDreamThemes::GetCurrentId() != Saved->ThemeId && FDreamThemes::Find(Saved->ThemeId).IsValid())
	{
		FDreamThemes::SetCurrent(Saved->ThemeId, /*bPersist*/ false);
	}
	auto RestoreBool = [Settings](bool UDreamUserSettings::* Member, bool bValue, FName Key)
	{
		if (Settings->*Member != bValue)
		{
			Settings->*Member = bValue;
			UDreamUserSettings::SaveKey(Key);
		}
	};
	RestoreBool(&UDreamUserSettings::bTransactionSelectionAndLine, Saved->bTransactionSelectionAndLine,
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bTransactionSelectionAndLine));
	RestoreBool(&UDreamUserSettings::bTransactionBuffer, Saved->bTransactionBuffer, GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bTransactionBuffer));
	RestoreBool(&UDreamUserSettings::bRunPump, Saved->bRunPump, GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bRunPump));
	// Echo: the icon toggle changes both the views' visibility and the saved setting.
	RestoreBool(&UDreamUserSettings::bEchoInput, Saved->bEchoInput, GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));
	if (Settings->RunPresentation != Saved->RunPresentation)
	{
		Settings->RunPresentation = Saved->RunPresentation;
		UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, RunPresentation));
	}
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		FDreamOutputVisibility Visibility = Store.GetVisibility();
		if (Visibility.bEcho != Saved->bEchoVisible)
		{
			Visibility.bEcho = Saved->bEchoVisible;
			Store.SetVisibility(Visibility);
		}
		FDreamEditorModule::Get().GetJournal().SetFailCommitsForTest(false);   // the health case's outage, on every path
	}
	Saved->bSaved = false;
}

void FDreamShellChromeSpec::Define()
{
	using namespace DreamShellChromeSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		Saved = MakeShared<FSaved>();
		const UDreamUserSettings* Settings = UDreamUserSettings::Get();
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(AllSettings); ++Index)
		{
			Saved->bValues[Index] = Settings->GetViewSetting(AllSettings[Index]);
		}
		Saved->ThemeId = Settings->ThemeId;
		Saved->bTransactionSelectionAndLine = Settings->bTransactionSelectionAndLine;
		Saved->bTransactionBuffer = Settings->bTransactionBuffer;
		Saved->bEchoInput = Settings->bEchoInput;
		Saved->bEchoVisible = FDreamEditorModule::Get().GetOutputStore().GetVisibility().bEcho;
		Saved->bRunPump = Settings->bRunPump;
		Saved->RunPresentation = Settings->RunPresentation;
		Saved->bSaved = true;
		if (Tab.IsValid())
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("chrome_probe = 1\nchrome_line_two = 2\n"), TEXT("Chrome"));
			BufferId = Buffer.IsValid() ? Buffer->GetId() : FGuid();
		}
	});

	AfterEach([this]()
	{
		RestoreSettings();
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
		Saved.Reset();
	});

	It("declares the PO-04 defaults: every optional part off, line numbers on, Darcula", [this]()
	{
		// A settings object built by its C++ constructor alone (no archetype copy, no ini), as Dream.Theme.Default builds one:
		// what a profile that never changed a View setting reads, whatever this profile stores.
		UClass* Class = UDreamUserSettings::StaticClass();
		UObject* Raw = StaticAllocateObject(Class, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), Class, FName(TEXT("DreamT4FreshChromeSettings"))), RF_Transient);
		(*Class->ClassConstructor)(FObjectInitializer(Raw, nullptr, EObjectInitializerOptions::None));
		const UDreamUserSettings* Fresh = CastChecked<UDreamUserSettings>(Raw);
		const TPair<const TCHAR*, EDreamViewSetting> Keys[] = {
			{ TEXT("bShowReplTab"), EDreamViewSetting::ReplTab }, { TEXT("bShowProblemsTab"), EDreamViewSetting::ProblemsTab },
			{ TEXT("bShowReplPrompt"), EDreamViewSetting::ReplPrompt }, { TEXT("bShowRunHeaders"), EDreamViewSetting::RunHeaders },
			{ TEXT("bShowStatusLine"), EDreamViewSetting::StatusLine }, { TEXT("bShowLineNumbers"), EDreamViewSetting::LineNumbers } };
		for (const TPair<const TCHAR*, EDreamViewSetting>& Key : Keys)
		{
			TestEqual(*FString::Printf(TEXT("%s defaults to %s"), Key.Key, DefaultOf(Key.Value) ? TEXT("on") : TEXT("off")),
				Fresh->GetViewSetting(Key.Value), DefaultOf(Key.Value));
		}
		TestEqual(TEXT("ThemeId defaults to Darcula"), Fresh->ThemeId, FString(TEXT("Darcula")));
		TestTrue(TEXT("Echo input defaults to on"), Fresh->bEchoInput);
	});

	LatentIt("hides the optional parts at the defaults: no status line, tab row, prompt, breadcrumb or language badge (PO-04)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !Tab->FindCodeView(BufferId).IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			if (!Tab.IsValid() || !Tab->FindCodeView(BufferId).IsValid())
			{
				AddError(TEXT("no Dream tab with a code view"));
			}
			Done.Execute();
			return;
		}
		// (review r7) A short REPL run, so the run-header check below always has a run to look at.
		const TSharedRef<DreamTest::FRunResult> Repl = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("put every View setting at its default"), [this]()
		{
			for (const EDreamViewSetting Setting : AllSettings)
			{
				UDreamUserSettings::SetViewSetting(Setting, DefaultOf(Setting));
			}
			return true;
		});
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a short REPL line"), []() { return DreamTest::MakeReplRequest(TEXT("6 * 7")); }, Repl, 60.0);
		Steps.Do(TEXT("focus the script"), [this]()
		{
			Tab->ActivateBuffer(BufferId);
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			View->FocusEditor();       // a focus home other than the prompt ends any Focus REPL reveal
			Tab->GetOutputArea()->EndPromptReveal(/*bEvenIfTyped*/ true);
			View->SetCaret(1, 4);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("check the Maya-minimal layout"), [this, Repl]()
		{
			const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
			TestFalse(TEXT("no status line (View ▸ Status Line off)"), Tab->IsStatusLineShown());
			TestFalse(TEXT("no output tab row (REPL and Problems tabs off)"), Area->IsTabRowShown());
			TestFalse(TEXT("the REPL tab is hidden"), Area->IsTabShown(EDreamOutputTab::Repl));
			TestFalse(TEXT("the Problems tab is hidden"), Area->IsTabShown(EDreamOutputTab::Problems));
			TestFalse(TEXT("the REPL prompt is off"), Area->IsPromptShown());
			TestFalse(TEXT("the REPL prompt strip is not shown"), IsPromptStripShown(*Area));
			// Run headers off: the history draws the REPL run's rows without a header or footer row.
			{
				TArray<EDreamOutputRowKind> Kinds;
				TArray<FString> Texts;
				TArray<uint64> Seqs;
				Area->GetOutputView()->GetRowsForTest(Repl->RunNumber, Kinds, Texts, Seqs);
				TestTrue(*FString::Printf(TEXT("the REPL run #%d has rows in the history (%d)"), Repl->RunNumber, Kinds.Num()), Kinds.Num() > 0);
				TestFalse(*FString::Printf(TEXT("no header or footer row for run #%d (run headers off)"), Repl->RunNumber),
					Kinds.Contains(EDreamOutputRowKind::Header) || Kinds.Contains(EDreamOutputRowKind::Footer));
			}
			TestTrue(TEXT("line numbers show (View ▸ Line Numbers is on by default)"), IsGutterShown(*Tab));
			// The breadcrumb row is gone: no "<name> › scratch" line anywhere in the tab.
			TArray<TSharedRef<SWidget>> Crumbs;
			Collect(Tab.ToSharedRef(), [](const TSharedRef<SWidget>& W)
			{
				return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString().Contains(TEXT(" › scratch"));
			}, Crumbs);
			TestEqual(TEXT("no breadcrumb row"), Crumbs.Num(), 0);
			TestFalse(TEXT("no language badge widget on any input tab"), FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamTab.Language"))).IsValid());
			// The caret's line:col at the right end of the input tab row.
			TestEqual(TEXT("the caret text is line:col"), Tab->GetCaretText().ToString(), FString(TEXT("2:5")));
			const TSharedPtr<SWidget> Caret = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.CaretPosition")));
			if (TestTrue(TEXT("the caret position text is in the input tab row"), Caret.IsValid() && Caret->GetType() == FName(TEXT("STextBlock"))))
			{
				TestEqual(TEXT("and shows 2:5"), StaticCastSharedPtr<STextBlock>(Caret)->GetText().ToString(), FString(TEXT("2:5")));
				TestTrue(TEXT("and is shown"), IsShownUnder(Caret, Tab));
			}
			// The menu bar and the icon row.
			const TArray<FName> Titles = Tab->GetMenuTitlesForTest();
			TArray<FString> Names;
			for (const FName& Title : Titles)
			{
				Names.Add(Title.ToString());
			}
			TestEqual(TEXT("the menu bar: File, Edit, History, Command, View, Help"), FString::Join(Names, TEXT(",")),
				FString(TEXT("DreamChrome.Menu.File,DreamChrome.Menu.Edit,DreamChrome.Menu.History,DreamChrome.Menu.Command,DreamChrome.Menu.View,DreamChrome.Menu.Help")));
			for (const EDreamCommand Command : { EDreamCommand::OpenFile, EDreamCommand::Save, EDreamCommand::RunSelectionOrBuffer, EDreamCommand::RunLine,
				EDreamCommand::Stop, EDreamCommand::ClearOutput, EDreamCommand::UndoLastRun, EDreamCommand::ToggleEchoInput, EDreamCommand::ToggleLineNumbers })
			{
				const TSharedPtr<SWidget> Icon = FindTagged(Tab.ToSharedRef(), FName(*(TEXT("DreamChrome.") + CommandName(Command))));
				if (TestTrue(*FString::Printf(TEXT("the icon row has %s"), *CommandName(Command)), Icon.IsValid()))
				{
					TestTrue(*FString::Printf(TEXT("%s is shown"), *CommandName(Command)), IsShownUnder(Icon, Tab));
					TestTrue(*FString::Printf(TEXT("%s has a tooltip"), *CommandName(Command)), Icon->GetToolTip().IsValid() && !Icon->GetToolTip()->IsEmpty());
					// No text labels on the icon row (PO-04): its buttons hold an image only.
					TArray<TSharedRef<SWidget>> Labels;
					Collect(Icon.ToSharedRef(), [](const TSharedRef<SWidget>& W) { return W->GetType() == FName(TEXT("STextBlock")); }, Labels);
					TestEqual(*FString::Printf(TEXT("%s has no text label"), *CommandName(Command)), Labels.Num(), 0);
				}
			}
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("shows each part while its View toggle is on, and hides it again (PO-04)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("start from the defaults"), [this]()
		{
			if (!Tab.IsValid() || !Tab->FindCodeView(BufferId).IsValid())
			{
				AddError(TEXT("no Dream tab with a code view"));
				return false;
			}
			for (const EDreamViewSetting Setting : AllSettings)
			{
				UDreamUserSettings::SetViewSetting(Setting, DefaultOf(Setting));
			}
			Tab->ActivateBuffer(BufferId);
			Tab->GetActiveCodeView()->FocusEditor();
			Tab->GetOutputArea()->EndPromptReveal(true);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("turn every View toggle through its command"), [this]()
		{
			for (const EDreamCommand Command : { EDreamCommand::ToggleReplTab, EDreamCommand::ToggleProblemsTab, EDreamCommand::ToggleReplPrompt,
				EDreamCommand::ToggleRunHeaders, EDreamCommand::ToggleStatusLine, EDreamCommand::ToggleLineNumbers })
			{
				FText Reason;
				TestTrue(*FString::Printf(TEXT("%s can execute"), *CommandName(Command)), Tab->CanExecuteDreamCommand(Command, Reason));
				const bool bBefore = Tab->IsViewToggleOn(Command);
				Tab->ExecuteDreamCommand(Command);
				TestEqual(*FString::Printf(TEXT("%s flips its setting"), *CommandName(Command)), Tab->IsViewToggleOn(Command), !bBefore);
			}
			const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
			// Explicit visibilities: the parts follow within the same frame.
			TestTrue(TEXT("the status line shows"), Tab->IsStatusLineShown());
			TestTrue(TEXT("the output tab row shows"), Area->IsTabRowShown());
			TestTrue(TEXT("with the REPL tab"), Area->IsTabShown(EDreamOutputTab::Repl) && FindTagged(Area.ToSharedRef(), FName(TEXT("DreamChrome.OutputTab.1"))).IsValid());
			TestTrue(TEXT("and the Problems tab"), Area->IsTabShown(EDreamOutputTab::Problems) && FindTagged(Area.ToSharedRef(), FName(TEXT("DreamChrome.OutputTab.2"))).IsValid());
			TestTrue(TEXT("the REPL prompt is on"), Area->IsPromptShown());
			TestTrue(TEXT("run headers are on"), UDreamUserSettings::Get()->bShowRunHeaders);
			TestFalse(TEXT("line numbers are off"), UDreamUserSettings::Get()->bShowLineNumbers);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the shown parts are painted; turn them back off"), [this]()
		{
			const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
			TestTrue(TEXT("the prompt strip is shown under Output"), IsPromptStripShown(*Area));
			TestFalse(TEXT("the gutter is hidden"), IsGutterShown(*Tab));
			// The Problems tab hides the strip (RV-18) while the REPL prompt stays on.
			Area->SetActiveTab(EDreamOutputTab::Problems);
			TestFalse(TEXT("the Problems tab hides the prompt strip"), IsPromptStripShown(*Area));
			Area->SetActiveTab(EDreamOutputTab::Output);
			for (const EDreamCommand Command : { EDreamCommand::ToggleReplTab, EDreamCommand::ToggleProblemsTab, EDreamCommand::ToggleReplPrompt,
				EDreamCommand::ToggleRunHeaders, EDreamCommand::ToggleStatusLine, EDreamCommand::ToggleLineNumbers })
			{
				Tab->ExecuteDreamCommand(Command);
			}
			TestFalse(TEXT("the status line is gone again"), Tab->IsStatusLineShown());
			TestFalse(TEXT("so is the tab row"), Area->IsTabRowShown());
			TestFalse(TEXT("and the prompt strip"), IsPromptStripShown(*Area));
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("line numbers are back; a hidden active tab falls back to Output"), [this]()
		{
			const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
			TestTrue(TEXT("the gutter is shown again"), IsGutterShown(*Tab));
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplTab, true);
			Area->SetActiveTab(EDreamOutputTab::Repl);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplTab, false);
			TestEqual(TEXT("hiding the active REPL tab shows Output"), static_cast<int32>(Area->GetActiveTab()), static_cast<int32>(EDreamOutputTab::Output));
			// Focus REPL shows a hidden prompt for this use, and another focus home ends that when the prompt is empty.
			const TSharedRef<FDreamBuffer> Repl = FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer();
			const FString PromptBefore = Repl->GetTextAsString();
			Repl->ResetText(FString(), dream::EditOrigin::Load);
			Tab->ExecuteDreamCommand(EDreamCommand::FocusRepl);
			const bool bRevealed = Area->IsPromptShown() && Area->GetReplStrip()->GetPromptView()->HasEditorFocus();
			Tab->GetActiveCodeView()->FocusEditor();
			const bool bEnded = !Area->IsPromptShown();
			Repl->ResetText(PromptBefore, dream::EditOrigin::Load);
			TestTrue(TEXT("Focus REPL shows the hidden prompt and focuses it"), bRevealed);
			TestTrue(TEXT("focusing the script hides the empty prompt again"), bEnded);
			return true;
		});
		Steps.Finally(TEXT("restore the settings"), [this]() { RestoreSettings(); });
		Steps.Run(Done);
	});

	LatentIt("reaches every former toolbar and menu action from the menu bar, the split Run menu or the icon row", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("list the menus' entries"), [this]()
		{
			if (!Tab.IsValid() || !Tab->FindCodeView(BufferId).IsValid())
			{
				AddError(TEXT("no Dream tab with a code view"));
				return false;
			}
			Tab->ActivateBuffer(BufferId);
			const TSet<FString> Names = ReachableNames();
			// The old toolbar: the split Run button (face and arrow), Stop, One undo step per run, Undo Last Run, Echo, Clear;
			// the old File and View menus; Run in / without transaction and the namespace radios.
			const TCHAR* const Former[] = {
				TEXT("RunSelectionOrBuffer"), TEXT("RunBuffer"), TEXT("RunLine"), TEXT("RunLineAdvance"), TEXT("RunFile"),
				TEXT("Dream.RunInTransaction"), TEXT("Dream.RunWithoutTransaction"), TEXT("ToggleOneUndoStepPerRun"), TEXT("UndoLastRun"),
				TEXT("Dream.NamespaceSession"), TEXT("Dream.NamespaceIsolated"), TEXT("ResetSession"), TEXT("Stop"),
				TEXT("ToggleEchoInput"), TEXT("ClearOutput"),
				TEXT("NewTab"), TEXT("OpenFile"), TEXT("Dream.RecentFiles"), TEXT("Save"), TEXT("SaveAs"), TEXT("CloseTab"),
				TEXT("ReopenClosedTab"), TEXT("NextTab"), TEXT("PreviousTab"),
				TEXT("Dream.OutputPosition"), TEXT("MoveOutputToTop"), TEXT("MoveOutputToBottom"), TEXT("FocusRepl"), TEXT("Dream.ShowInOutput"),
				TEXT("ExpandEcho"), TEXT("FoldEcho"), TEXT("NextError"), TEXT("PrevError"), TEXT("GoToLine"), TEXT("ToggleComment"),
				TEXT("ZoomIn"), TEXT("ZoomOut"), TEXT("ZoomReset"),
				// PO-04 additions: the View toggles, the theme list, the notices.
				TEXT("ToggleReplTab"), TEXT("ToggleProblemsTab"), TEXT("ToggleReplPrompt"), TEXT("ToggleRunHeaders"), TEXT("ToggleStatusLine"),
				TEXT("ToggleLineNumbers"), TEXT("Dream.Theme"), TEXT("Dream.ShowHiddenNotices"), TEXT("Dream.RecoveryReview"), TEXT("Dream.LeaveSafeMode") };
			for (const TCHAR* Name : Former)
			{
				TestTrue(*FString::Printf(TEXT("%s is in a menu"), Name), Names.Contains(Name));
			}
			// Every bound command except the code view's own primitives is in a menu too (its chord shows there).
			for (int32 Index = 1; Index < static_cast<int32>(EDreamCommand::Count); ++Index)
			{
				const EDreamCommand Command = static_cast<EDreamCommand>(Index);
				if (Command == EDreamCommand::OpenDream || Command == EDreamCommand::RunInSessionOrIsolated)
				{
					continue;   // the global chord (main frame); the namespace radios above replace the toggle command
				}
				TestTrue(*FString::Printf(TEXT("the command %s is in a menu"), *CommandName(Command)), Names.Contains(CommandName(Command)));
			}
			// The chords did not change with the chrome: the menus show the commands' own.
			TestFalse(TEXT("Run Selection or Buffer keeps its chord"), FDreamKeyRouter::Get().GetChordText(EDreamCommand::RunSelectionOrBuffer).IsEmpty());
			TestFalse(TEXT("Stop keeps its chord"), FDreamKeyRouter::Get().GetChordText(EDreamCommand::Stop).IsEmpty());
			TestFalse(TEXT("Undo Last Run keeps its chord"), FDreamKeyRouter::Get().GetChordText(EDreamCommand::UndoLastRun).IsEmpty());
			// View ▸ Theme lists every theme.
			int32 Themes = 0;
			for (const TSharedRef<const FDreamTheme>& Theme : FDreamThemes::List())
			{
				++Themes;
				TestTrue(*FString::Printf(TEXT("View ▸ Theme lists %s"), *Theme->Id), Names.Contains(TEXT("Dream.Theme.") + Theme->Id));
			}
			TestTrue(*FString::Printf(TEXT("at least the ten bundled themes (%d)"), Themes), Themes >= 10);
			// History ▸ Show in Output holds one filter per type (§6.4, OUT-03), each by name, each with an action.
			int32 Filters = 0;
			for (const SDreamTab::FChromeEntry& Entry : Tab->GetMenuEntriesForTest(TEXT("ShowInOutput")))
			{
				Filters += Entry.Action.ExecuteAction.IsBound() ? 1 : 0;
			}
			const TArray<FString> FilterLabels = Tab->GetMenuLabelsForTest(TEXT("ShowInOutput"));
			const TCHAR* const Types[] = { TEXT("Stdout"), TEXT("Stderr"), TEXT("Results"), TEXT("Info"), TEXT("Warnings"), TEXT("Errors"),
				TEXT("Echo input"), TEXT("Notices"), TEXT("Background") };
			for (const TCHAR* Type : Types)
			{
				TestTrue(*FString::Printf(TEXT("History ▸ Show in Output has %s"), Type), FilterLabels.Contains(FString(Type)));
			}
			// The menu holds more than the types (Soft wrap below them), so the count is a floor; the names are the check.
			TestTrue(*FString::Printf(TEXT("each type's entry has its action (%d entries with one)"), Filters),
				Filters >= static_cast<int32>(UE_ARRAY_COUNT(Types)));
			// File ▸ Leave Safe Mode exists and is enabled only in safe mode.
			for (const SDreamTab::FChromeEntry& Entry : Tab->GetMenuEntriesForTest(TEXT("File")))
			{
				if (Entry.Name == FName(TEXT("Dream.LeaveSafeMode")))
				{
					TestEqual(TEXT("Leave Safe Mode is enabled exactly in safe mode"), Entry.Action.CanExecute(), Tab->IsInSafeMode());
				}
			}
			return true;
		});
		// Each menu title opens its own menu: a label only that menu holds is in the window it opened.
		const TPair<const TCHAR*, FString> TitleProbes[] = {
			{ TEXT("DreamChrome.Menu.File"), TEXT("New Tab") }, { TEXT("DreamChrome.Menu.Edit"), TEXT("Duplicate Line") },
			{ TEXT("DreamChrome.Menu.History"), TEXT("Clear History") }, { TEXT("DreamChrome.Menu.Command"), TEXT("Run Selection or Buffer") },
			{ TEXT("DreamChrome.Menu.View"), TEXT("Status Line") }, { TEXT("DreamChrome.Menu.Help"), TEXT("Keyboard Shortcuts...") } };
		for (const TPair<const TCHAR*, FString>& Probe : TitleProbes)
		{
			const FName TitleTag(Probe.Key);
			const FString Label = Probe.Value;
			Steps.Do(FString::Printf(TEXT("open %s"), Probe.Key), [this, TitleTag]()
			{
				FSlateApplication::Get().DismissAllMenus();
				const TSharedPtr<SWidget> Title = FindTagged(Tab.ToSharedRef(), TitleTag);
				if (!TestTrue(*FString::Printf(TEXT("%s is in the menu bar"), *TitleTag.ToString()), Title.IsValid() && Title->GetType() == FName(TEXT("SComboButton"))))
				{
					return false;
				}
				StaticCastSharedPtr<SComboButton>(Title)->SetIsOpen(true, /*bFocusMenu*/ false);
				return true;
			});
			Steps.WaitFrames(2);
			Steps.Do(FString::Printf(TEXT("%s holds \"%s\""), Probe.Key, *Label), [this, TitleTag, Label]()
			{
				const TSharedPtr<SWidget> Title = FindTagged(Tab.ToSharedRef(), TitleTag);
				const TSharedPtr<SComboButton> Combo = Title.IsValid() ? StaticCastSharedPtr<SComboButton>(Title) : nullptr;
				const TSharedPtr<SWindow> MenuWindow = Combo.IsValid() && Combo->IsOpen() ? Combo->GetMenuWindow() : nullptr;
				TArray<TSharedRef<SWidget>> Found;
				if (MenuWindow.IsValid())
				{
					Collect(MenuWindow.ToSharedRef(), [&Label](const TSharedRef<SWidget>& W)
					{
						return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString() == Label;
					}, Found);
				}
				TestTrue(*FString::Printf(TEXT("%s opened its own menu (\"%s\" is in it)"), *TitleTag.ToString(), *Label), Found.Num() > 0);
				if (Combo.IsValid())
				{
					Combo->SetIsOpen(false);
				}
				FSlateApplication::Get().DismissAllMenus();
				return true;
			});
		}
		Steps.Do(TEXT("the One undo step per run entry shows the B40 mixed state and runs the toggle"), [this]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Settings->bTransactionSelectionAndLine = false;
			Settings->bTransactionBuffer = true;
			const TSharedPtr<FDreamBuffer> Buffer = Tab->GetActiveBuffer();
			Buffer->GetViewState().bOneUndoStepPerRunSet = false;
			const SDreamTab::FChromeEntry* Entry = nullptr;
			const TArray<SDreamTab::FChromeEntry> Entries = Tab->GetMenuEntriesForTest(TEXT("Command"));
			for (const SDreamTab::FChromeEntry& Candidate : Entries)
			{
				Entry = Candidate.Name == FName(TEXT("ToggleOneUndoStepPerRun")) ? &Candidate : Entry;
			}
			if (!TestTrue(TEXT("the Command menu has One undo step per run"), Entry != nullptr))
			{
				return false;
			}
			TestEqual(TEXT("untouched with differing route settings it is mixed (B40, A87)"), static_cast<int32>(Entry->Action.GetCheckState()),
				static_cast<int32>(ECheckBoxState::Undetermined));
			TestEqual(TEXT("the entry is the toggle command's own (its chord shows)"), static_cast<int32>(Entry->Command),
				static_cast<int32>(EDreamCommand::ToggleOneUndoStepPerRun));
			TestTrue(TEXT("and runs the command list's mapping"), Entry->Action.ExecuteAction.IsBound());
			Entry->Action.Execute();
			TestTrue(TEXT("a click from mixed sets the toggle on"), Buffer->GetViewState().bOneUndoStepPerRunSet && Buffer->GetViewState().bOneUndoStepPerRun);
			TestEqual(TEXT("and the entry shows it checked"), static_cast<int32>(Entry->Action.GetCheckState()), static_cast<int32>(ECheckBoxState::Checked));
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the icon row's toggles work through a Slate click"), [this]()
		{
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			const bool bEchoBefore = Store.GetVisibility().bEcho;
			const bool bLinesBefore = UDreamUserSettings::Get()->bShowLineNumbers;
			const TSharedPtr<SWidget> Echo = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.ToggleEchoInput")));
			const TSharedPtr<SWidget> Lines = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.ToggleLineNumbers")));
			if (!TestTrue(TEXT("both toggles are in the icon row"), Echo.IsValid() && Lines.IsValid())
				|| !TestTrue(TEXT("both are check boxes"), Echo->GetType() == FName(TEXT("SCheckBox")) && Lines->GetType() == FName(TEXT("SCheckBox"))))
			{
				return false;
			}
			// A neutral first click (the caret position text: no action; focus goes back to the code view): the first routed
			// click of a test can be eaten by the pointer state a real OS cursor or an earlier window left (seen: the first
			// click changed nothing, every later one worked).
			if (const TSharedPtr<SWidget> Caret = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.CaretPosition"))))
			{
				Click(Caret.ToSharedRef());
			}
			int32 Broadcasts = 0;
			const FDelegateHandle Counter = Store.OnVisibilityChanged().AddLambda([&Broadcasts]() { ++Broadcasts; });
			const ECheckBoxState ShownBefore = StaticCastSharedPtr<SCheckBox>(Echo)->GetCheckedState();
			TestTrue(TEXT("Echo is in a visible window"), Click(Echo.ToSharedRef()));
			Store.OnVisibilityChanged().Remove(Counter);
			AddInfo(FString::Printf(TEXT("Echo click: echo %d -> %d, %d visibility broadcasts, check box %d -> %d, focused %s"), bEchoBefore ? 1 : 0,
				Store.GetVisibility().bEcho ? 1 : 0, Broadcasts, static_cast<int32>(ShownBefore),
				static_cast<int32>(StaticCastSharedPtr<SCheckBox>(Echo)->GetCheckedState()),
				FSlateApplication::Get().GetKeyboardFocusedWidget().IsValid() ? *FSlateApplication::Get().GetKeyboardFocusedWidget()->GetType().ToString() : TEXT("-")));
			TestEqual(TEXT("a click on Echo flips Echo input"), Store.GetVisibility().bEcho, !bEchoBefore);
			Click(Echo.ToSharedRef());
			TestEqual(TEXT("a second click brings it back"), Store.GetVisibility().bEcho, bEchoBefore);
			TestTrue(TEXT("Line numbers is in a visible window"), Click(Lines.ToSharedRef()));
			TestEqual(TEXT("a click on Line numbers flips the setting"), UDreamUserSettings::Get()->bShowLineNumbers, !bLinesBefore);
			Click(Lines.ToSharedRef());
			TestEqual(TEXT("a second click brings it back"), UDreamUserSettings::Get()->bShowLineNumbers, bLinesBefore);
			return true;
		});
		Steps.Finally(TEXT("restore the settings"), [this]() { RestoreSettings(); });
		Steps.Run(Done);
	});

	LatentIt("persists the View settings and the theme in EditorSettings.ini, and applies a theme live", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("each toggle writes its key at once"), [this]()
		{
			for (const EDreamViewSetting Setting : AllSettings)
			{
				const bool bValue = !UDreamUserSettings::Get()->GetViewSetting(Setting);
				UDreamUserSettings::SetViewSetting(Setting, bValue);
			}
			const TPair<const TCHAR*, EDreamViewSetting> Keys[] = {
				{ TEXT("bShowReplTab"), EDreamViewSetting::ReplTab }, { TEXT("bShowProblemsTab"), EDreamViewSetting::ProblemsTab },
				{ TEXT("bShowReplPrompt"), EDreamViewSetting::ReplPrompt }, { TEXT("bShowRunHeaders"), EDreamViewSetting::RunHeaders },
				{ TEXT("bShowStatusLine"), EDreamViewSetting::StatusLine }, { TEXT("bShowLineNumbers"), EDreamViewSetting::LineNumbers } };
			for (const TPair<const TCHAR*, EDreamViewSetting>& Key : Keys)
			{
				bool bStored = false;
				const bool bFound = GConfig->GetBool(SettingsSection, Key.Key, bStored, GEditorSettingsIni);
				TestTrue(*FString::Printf(TEXT("%s is stored in EditorSettings.ini"), Key.Key), bFound);
				TestEqual(*FString::Printf(TEXT("%s holds the new value"), Key.Key), bStored, UDreamUserSettings::Get()->GetViewSetting(Key.Value));
			}
			return true;
		});
		Steps.Do(TEXT("View ▸ Theme ▸ GitHub Light applies live and persists"), [this]()
		{
			const FLinearColor Before = FDreamChrome::Linear(EDreamChromeSlot::Background);
			const FString Target = FDreamThemes::GetCurrentId() == TEXT("GitHub Light") ? FString(TEXT("Darcula")) : FString(TEXT("GitHub Light"));
			const TSharedPtr<const FDreamTheme> Theme = FDreamThemes::Find(Target);
			if (!TestTrue(*FString::Printf(TEXT("the %s theme exists"), *Target), Theme.IsValid()))
			{
				return false;
			}
			// Through the menu entry, as a user picks it.
			bool bRan = false;
			for (const SDreamTab::FChromeEntry& Entry : Tab->GetMenuEntriesForTest(TEXT("Theme")))
			{
				if (Entry.Name == FName(*(TEXT("Dream.Theme.") + Target)))
				{
					Entry.Action.Execute();
					bRan = true;
				}
			}
			TestTrue(TEXT("the theme entry ran"), bRan);
			TestEqual(TEXT("the theme is current"), FDreamThemes::GetCurrentId(), Target);
			FString Stored;
			GConfig->GetString(SettingsSection, TEXT("ThemeId"), Stored, GEditorSettingsIni);
			TestEqual(TEXT("ThemeId is stored in EditorSettings.ini"), Stored, Target);
			const FLinearColor After = FDreamChrome::Linear(EDreamChromeSlot::Background);
			TestTrue(TEXT("the chrome background follows the theme"), After.Equals(Theme->Get(EDreamThemeSlot::ChromeBackground), 0.002f));
			TestFalse(TEXT("and differs from before"), After.Equals(Before, 0.002f));
			const FSlateBrush* Brush = FDreamChrome::Brush(EDreamChromeSlot::Background);
			TestTrue(TEXT("the chrome brush is retinted in place (live, no rebuild)"),
				Brush != nullptr && Brush->TintColor.GetSpecifiedColor().Equals(After, 0.002f));
			TestEqual(TEXT("a light theme says so"), FDreamChrome::IsLightTheme(), Theme->bLight);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Finally(TEXT("restore the settings"), [this]() { RestoreSettings(); });
		Steps.Run(Done);
	});

	LatentIt("gives no chrome widget keyboard focus when a Run-window run starts, and focus is back in the code view after it (A59)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Loop = DreamTest::OpenScratch(*this, *Tab, TEXT("import dream_editor\nfor _ in range(100): dream_editor.sleep(0.01)"),
			TEXT("ChromeRun"));
		if (!Loop.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid LoopId = Loop->GetId();
		const FGuid TargetId = BufferId;
		struct FFocusState
		{
			bool bSeen = false;
			bool bInside = false;
			bool bInRunWindow = false;
			bool bInTabChrome = false;
			bool bRunWindowOpen = false;
			bool bAppHasFocus = false;
			FString Path;
			bool bSavedRunPump = true;
			EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
			FDelegateHandle PreTick;
		};
		const TSharedRef<FFocusState> State = MakeShared<FFocusState>();
		State->bSavedRunPump = UDreamUserSettings::Get()->bRunPump;
		State->SavedPresentation = UDreamUserSettings::Get()->RunPresentation;
		const TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the probe's code view"), [this, TargetId]()
		{
			Tab->ActivateBuffer(TargetId);
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(TargetId);
			if (!TestTrue(TEXT("the probe has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("focus is in the probe's code view"), [this, TargetId]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(TargetId);
			if (View.IsValid() && !View->HasEditorFocus())
			{
				View->FocusEditor();   // the view was built this frame: laid out now
			}
			return TestTrue(TEXT("focus is in the probe's code view"), View.IsValid() && View->HasEditorFocus());
		});
		Steps.Do(TEXT("submit a pumping run; note where focus is at its first pump"), [this, TargetId, LoopId, State, Run]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			const TWeakPtr<SDreamTab> WeakTab = Tab;
			State->PreTick = FSlateApplication::Get().OnPreTick().AddLambda([WeakTab, TargetId, State](float)
			{
				const TSharedPtr<SDreamTab> Pinned = WeakTab.Pin();
				if (State->bSeen || !Pinned.IsValid() || FDreamEditorModule::Get().GetSupervisor().GetPumpDepth() <= 0)
				{
					return;
				}
				State->bSeen = true;
				State->bRunWindowOpen = FDreamEditorModule::Get().GetSupervisor().IsRunWindowOpen();
				State->bAppHasFocus = FApp::HasFocus();
				const TSharedPtr<SDreamCodeView> View = Pinned->FindCodeView(TargetId);
				const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
				const TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow();
				State->bInRunWindow = Focused.IsValid() && Modal.IsValid()
					&& (Focused == Modal || FSlateApplication::Get().FindWidgetWindow(Focused.ToSharedRef()) == Modal);
				bool bInTab = false;
				TArray<FString> Types;
				for (TSharedPtr<SWidget> W = Focused; W.IsValid(); W = W->GetParentWidget())
				{
					if (Types.Num() < 12)
					{
						const TSharedPtr<FTagMetaData> Meta = W->GetMetaData<FTagMetaData>();
						Types.Add(Meta.IsValid() ? FString::Printf(TEXT("%s[%s]"), *W->GetType().ToString(), *Meta->Tag.ToString()) : W->GetType().ToString());
					}
					State->bInside = State->bInside || (View.IsValid() && W == View);
					bInTab = bInTab || W == Pinned;
				}
				// Inside the Dream tab but in no code view: a menu title, an icon, the tab row or a banner took it.
				State->bInTabChrome = bInTab && !State->bInside && Focused != Pinned;
				State->Path = Types.Num() > 0 ? FString::Join(Types, TEXT(" < ")) : FString(TEXT("<nothing focused>"));
			});
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*FDreamEditorModule::Get().GetBufferManager().FindById(LoopId),
				EDreamRunRoute::Buffer, 1, 2));
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the pumping run"), Run, 60.0);
		Steps.WaitFrames(2);
		Steps.Do(TEXT("no chrome widget held focus; focus is back in the code view"), [this, TargetId, State, Run]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			AddInfo(FString::Printf(TEXT("at the first pump: Run window %s, the editor %s OS focus, keyboard focus: %s"),
				State->bRunWindowOpen ? TEXT("open") : TEXT("not open"), State->bAppHasFocus ? TEXT("has") : TEXT("lacks"), *State->Path));
			TestTrue(TEXT("a pump was seen"), State->bSeen);
			TestFalse(TEXT("no menu title, icon, tab row or banner of the Dream tab holds keyboard focus at the first pump"), State->bInTabChrome);
			TestTrue(TEXT("keyboard focus is in the code view, or in the Run window (the editor has OS focus, §5.5.3)"),
				State->bInside || (State->bInRunWindow && State->bAppHasFocus));
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(TargetId);
			TestTrue(TEXT("after the run keyboard focus is back in the code view (§5.5.2)"), View.IsValid() && View->HasEditorFocus());
			return true;
		});
		Steps.Finally(TEXT("remove the hook, restore the run settings, close the loop tab"), [State, LoopId]()
		{
			DreamTest::CloseBuffer(LoopId);
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().OnPreTick().Remove(State->PreTick);
			}
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Settings->bRunPump = State->bSavedRunPump;
			Settings->RunPresentation = State->SavedPresentation;
		});
		Steps.Run(Done);
	});

	LatentIt("hides a banner line with its x until a new line comes, even with the same text, and View > Show Hidden Notices brings it back (PO-04-R1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FBannerState
		{
			TArray<FText> Lines;
			int32 Earlier = INDEX_NONE;
			bool bShown = false;
			bool bSaved = false;
			FString A;
			FString Count;
		};
		const TSharedRef<FBannerState> State = MakeShared<FBannerState>();
		const FString Sfx = DreamTest::UniqueSuffix();
		State->A = FString::Printf(TEXT("Chrome banner probe A %s."), *Sfx);
		State->Count = FString::Printf(TEXT("Chrome banner count line %s:"), *Sfx);
		// The banner (an SDreamRecoveryBanner) whose one-line text holds Needle, or null.
		auto BannerWith = [this](const FString& Needle) -> TSharedPtr<SWidget>
		{
			TArray<TSharedRef<SWidget>> Banners;
			Collect(Tab.ToSharedRef(), [](const TSharedRef<SWidget>& W) { return W->GetType() == FName(TEXT("SDreamRecoveryBanner")); }, Banners);
			for (const TSharedRef<SWidget>& Banner : Banners)
			{
				TArray<TSharedRef<SWidget>> Texts;
				Collect(Banner, [&Needle](const TSharedRef<SWidget>& W)
				{
					return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString().Contains(Needle);
				}, Texts);
				if (Texts.Num() > 0)
				{
					return Banner;
				}
			}
			return nullptr;
		};
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a count line (two pending buffers) and a line A, as a Discard that could not be kept adds one"), [this, State]()
		{
			if (!Tab.IsValid())
			{
				return false;
			}
			Tab->GetRecoveryLinesForTest(State->Lines, State->Earlier, State->bShown);
			State->bSaved = true;
			Tab->ShowHiddenNotices();
			Tab->SetRecoveryLinesForTest({ FText::AsCultureInvariant(State->Count + TEXT(" 2")), FText::AsCultureInvariant(State->A) }, 0, true);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("× hides both; handling the pending buffers keeps them hidden; the same text added again shows the banner"),
			[this, State, BannerWith]()
		{
			const TSharedPtr<SWidget> Banner = BannerWith(State->A);
			if (!TestTrue(TEXT("the banner shows line A"), Banner.IsValid() && IsShownUnder(Banner, Tab)))
			{
				return false;
			}
			const TSharedPtr<SWidget> Dismiss = FindTagged(Banner.ToSharedRef(), FName(TEXT("DreamChrome.BannerDismiss")));
			if (!TestTrue(TEXT("the banner line has its ×"), Dismiss.IsValid() && IsShownUnder(Dismiss, Tab)))
			{
				return false;
			}
			if (const TSharedPtr<SWidget> Caret = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.CaretPosition"))))
			{
				Click(Caret.ToSharedRef());   // a neutral first click (see the icon row case)
			}
			TestTrue(TEXT("× is in a visible window"), Click(Dismiss.ToSharedRef()));
			TestFalse(TEXT("× hid the lines"), BannerWith(State->A).IsValid());
			TestTrue(TEXT("View ▸ Show Hidden Notices is enabled"), Tab->HasHiddenNotices());
			// Restoring one pending buffer from its bar updates the count line (its id stays): the banner stays hidden.
			Tab->SetEarlierSessionLineForTest(FText::AsCultureInvariant(State->Count + TEXT(" 1")));
			TestFalse(TEXT("an updated count keeps the banner hidden"), BannerWith(State->Count).IsValid() || BannerWith(State->A).IsValid());
			TestTrue(TEXT("and still hidden for Show Hidden Notices"), Tab->HasHiddenNotices());
			// The last one handled: the count line goes, the hidden line A stays hidden.
			Tab->SetEarlierSessionLineForTest(TOptional<FText>());
			TestFalse(TEXT("the count line gone keeps the banner hidden"), BannerWith(State->A).IsValid());
			TestTrue(TEXT("and A is still hidden"), Tab->HasHiddenNotices());
			// A new failed Discard with the very same text is a new line: the banner shows again.
			Tab->AddRecoveryLineForTest(FText::AsCultureInvariant(State->A));
			TestTrue(TEXT("the same text added again shows the banner"), BannerWith(State->A).IsValid());
			TestFalse(TEXT("nothing is hidden now"), Tab->HasHiddenNotices());
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("× again, then View ▸ Show Hidden Notices brings it back"), [this, State, BannerWith]()
		{
			const TSharedPtr<SWidget> Banner = BannerWith(State->A);
			const TSharedPtr<SWidget> Dismiss = Banner.IsValid() ? FindTagged(Banner.ToSharedRef(), FName(TEXT("DreamChrome.BannerDismiss"))) : nullptr;
			if (!TestTrue(TEXT("the banner and its × are there"), Dismiss.IsValid()) || !TestTrue(TEXT("× is in a visible window"), Click(Dismiss.ToSharedRef())))
			{
				return false;
			}
			TestFalse(TEXT("× hid both lines"), BannerWith(State->A).IsValid());
			bool bRan = false;
			for (const SDreamTab::FChromeEntry& Entry : Tab->GetMenuEntriesForTest(TEXT("View")))
			{
				if (Entry.Name == FName(TEXT("Dream.ShowHiddenNotices")))
				{
					TestTrue(TEXT("Show Hidden Notices is enabled"), Entry.Action.CanExecute());
					bRan = Entry.Action.Execute();
				}
			}
			TestTrue(TEXT("View ▸ Show Hidden Notices ran"), bRan);
			TestTrue(TEXT("the banner is back"), BannerWith(State->A).IsValid());
			TestFalse(TEXT("and nothing is hidden"), Tab->HasHiddenNotices());
			return true;
		});
		Steps.Finally(TEXT("the banner's lines as they were"), [this, State]()
		{
			if (State->bSaved && Tab.IsValid())
			{
				Tab->SetRecoveryLinesForTest(State->Lines, State->Earlier, State->bShown);
				Tab->ShowHiddenNotices();
			}
		});
		Steps.Run(Done);
	});

	LatentIt("opens a tab with the +, shows messages beside line:col, and the REPL placeholder is Python (PO-04)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		const TSharedRef<TArray<FGuid>> Opened = MakeShared<TArray<FGuid>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("the status line hidden; a transient message"), [this]()
		{
			if (!Tab.IsValid() || !Tab->FindCodeView(BufferId).IsValid())
			{
				AddError(TEXT("no Dream tab with a code view"));
				return false;
			}
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StatusLine, false);
			Tab->ActivateBuffer(BufferId);
			Tab->ShowStatusMessage(NSLOCTEXT("DreamTests", "Chrome.ProbeMessage", "Chrome probe message"));
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the message is beside line:col; the + opens a tab"), [this, Opened]()
		{
			const TSharedPtr<SWidget> Message = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.TabRowMessage")));
			if (Tab->GetHealthWarning() != SDreamTab::EHealthWarning::None)
			{
				AddInfo(FString::Printf(TEXT("a health warning holds the slot (%s); the message check is the health case's"), *Tab->GetTabRowMessage().ToString()));
			}
			else if (TestTrue(TEXT("the tab row has its message text"), Message.IsValid() && Message->GetType() == FName(TEXT("STextBlock"))))
			{
				TestEqual(TEXT("the message shows beside line:col while the status line is hidden"),
					StaticCastSharedPtr<STextBlock>(Message)->GetText().ToString(), FString(TEXT("Chrome probe message")));
				TestTrue(TEXT("and is shown"), IsShownUnder(Message, Tab));
			}
			const int32 Before = Tab->GetTabBuffers().Num();
			const TSharedPtr<SWidget> Plus = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.NewTab")));
			if (!TestTrue(TEXT("the input tab row has its +"), Plus.IsValid() && IsShownUnder(Plus, Tab)))
			{
				return false;
			}
			if (const TSharedPtr<SWidget> Caret = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.CaretPosition"))))
			{
				Click(Caret.ToSharedRef());   // a neutral first click
			}
			TestTrue(TEXT("the + is in a visible window"), Click(Plus.ToSharedRef()));
			const TArray<TSharedRef<FDreamBuffer>> After = Tab->GetTabBuffers();
			TestEqual(TEXT("the + opened one scratch tab"), After.Num(), Before + 1);
			if (const TSharedPtr<FDreamBuffer> Active = Tab->GetActiveBuffer())
			{
				if (Active->GetId() != BufferId)
				{
					Opened->Add(Active->GetId());
				}
				// (scratch, PO-05) The + makes a scratch file (scratch_N.py), or the journaled Untitled-N when the folder is not writable.
				TestTrue(TEXT("and made it the active one"), (Active->IsScratchFile() || Active->GetKind() == EDreamBufferKind::Scratch) && Active->GetId() != BufferId);
			}
			// The REPL prompt's placeholder is short (PO-04).
			const EDreamPythonState Python = FDreamEditorModule::Get().GetPython().GetState();
			if (Python == EDreamPythonState::Ready || Python == EDreamPythonState::Cold)
			{
				TestEqual(TEXT("the REPL placeholder is Python"), Tab->GetOutputArea()->GetReplStrip()->GetHintTextForTest().ToString(), FString(TEXT("Python")));
			}
			else
			{
				AddInfo(TEXT("Python is not usable now: the placeholder says why instead"));
			}
			return true;
		});
		Steps.Finally(TEXT("close the tab the + opened"), [Opened]()
		{
			for (const FGuid& Id : *Opened)
			{
				DreamTest::CloseBuffer(Id);
			}
		});
		Steps.Run(Done);
	});

	LatentIt("shows a health warning beside line:col, before any message, while the status line is hidden (PO-04-R1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("the journal's writes fail (its test hook), then an edit"), [this]()
		{
			FDreamJournal& Journal = FDreamEditorModule::Get().GetJournal();
			if (!Tab.IsValid() || !Tab->FindCodeView(BufferId).IsValid() || !Journal.IsOpen())
			{
				AddError(TEXT("no Dream tab with a code view and an open journal"));
				return false;
			}
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StatusLine, false);
			Tab->ActivateBuffer(BufferId);
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			View->FocusEditor();
			Journal.SetFailCommitsForTest(true);
			View->SetCaret(0, 0);
			View->InsertAtCaret(TEXT("# health probe\n"));
			Journal.ForceCommit();
			return true;
		});
		Steps.Poll(TEXT("the journal is Degraded"), [this]()
		{
			return Tab->GetHealthWarning() == SDreamTab::EHealthWarning::JournalDegraded ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 15.0);
		Steps.Do(TEXT("the warning shows in the warning colour and outranks a message"), [this]()
		{
			const FString Warning = Tab->GetTabRowMessage().ToString();
			TestTrue(*FString::Printf(TEXT("the slot names the journal (%s)"), *Warning), Warning.StartsWith(TEXT("Journal")));
			TestTrue(TEXT("in the warning colour"), Tab->GetTabRowMessageColor().GetSpecifiedColor().Equals(FDreamChrome::Linear(EDreamChromeSlot::Warning), 0.002f));
			TestFalse(TEXT("its tooltip says what a click does"), Tab->GetTabRowMessageTooltip().IsEmpty());
			Tab->ShowStatusMessage(NSLOCTEXT("DreamTests", "Chrome.ProbeMessage2", "Chrome probe message two"));
			TestEqual(TEXT("a health warning outranks a transient message"), Tab->GetTabRowMessage().ToString(), Warning);
			// The status line shown holds it again (the warning slot is for the hidden status line only).
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StatusLine, true);
			TestEqual(TEXT("no tab-row warning while the status line shows"), static_cast<int32>(Tab->GetHealthWarning()),
				static_cast<int32>(SDreamTab::EHealthWarning::None));
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StatusLine, false);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the slot's text in the tab row; the disk recovers"), [this]()
		{
			const TSharedPtr<SWidget> Message = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.TabRowMessage")));
			if (TestTrue(TEXT("the tab row has its message text"), Message.IsValid() && Message->GetType() == FName(TEXT("STextBlock"))))
			{
				TestEqual(TEXT("the tab row shows the warning"), StaticCastSharedPtr<STextBlock>(Message)->GetText().ToString(), Tab->GetTabRowMessage().ToString());
				TestTrue(TEXT("and it is shown"), IsShownUnder(Message, Tab));
			}
			FDreamEditorModule::Get().GetJournal().SetFailCommitsForTest(false);
			return true;
		});
		Steps.Poll(TEXT("the journal is healthy again"), []()
		{
			return FDreamEditorModule::Get().GetJournal().GetHealth() == EDreamJournalHealth::Ok ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 20.0);
		Steps.Do(TEXT("the warning is gone"), [this]()
		{
			TestEqual(TEXT("no health warning once the journal recovered"), static_cast<int32>(Tab->GetHealthWarning()),
				static_cast<int32>(SDreamTab::EHealthWarning::None));
			return true;
		});
		Steps.Finally(TEXT("the journal writes again"), []()
		{
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetJournal().SetFailCommitsForTest(false);
			}
		});
		Steps.Run(Done);
	});

	LatentIt("opens the completion list from Edit > Complete (review r6)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			AddError(TEXT("no Dream tab"));
			Done.Execute();
			return;
		}
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("a prefix at the caret; Edit ▸ Complete"), [this]()
		{
			Tab->ActivateBuffer(BufferId);
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			if (!TestTrue(TEXT("the probe has a code view with completion"), View.IsValid() && View->GetCompletion().IsValid()))
			{
				return false;
			}
			// A root name: the keyword table (keywords and builtins) answers it at once, without the live resolver.
			View->SetTextNoUndo(TEXT("x = 1\npri"));
			View->FocusEditor();
			View->SetCaret(1, 3);
			bool bRan = false;
			for (const SDreamTab::FChromeEntry& Entry : Tab->GetMenuEntriesForTest(TEXT("Edit")))
			{
				if (Entry.Command == EDreamCommand::Complete)
				{
					TestTrue(TEXT("Edit ▸ Complete is enabled"), Entry.Action.CanExecute());
					bRan = Entry.Action.Execute();
				}
			}
			if (!TestTrue(TEXT("Edit ▸ Complete ran"), bRan))
			{
				return false;
			}
			TestTrue(TEXT("Edit ▸ Complete opened or requested the list"),
				View->GetCompletion()->GetState() != FDreamCompletionController::EState::Closed);
			return true;
		});
		Steps.Poll(TEXT("the completion list opens"), [this]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			return View.IsValid() && View->GetCompletion().IsValid() && View->GetCompletion()->IsOpen() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 15.0);
		Steps.Finally(TEXT("close the list"), [this]()
		{
			if (Tab.IsValid())
			{
				if (const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView())
				{
					if (View->GetCompletion().IsValid())
					{
						View->GetCompletion()->Close();
					}
				}
			}
		});
		Steps.Run(Done);
	});

	LatentIt("shows another tab's close x while the pointer is over that tab (review r6)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const TSharedRef<FGuid> OtherId = MakeShared<FGuid>();
		const TSharedRef<FString> OtherName = MakeShared<FString>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a second tab, the probe active"), [this, OtherId, OtherName]()
		{
			if (!Tab.IsValid())
			{
				return false;
			}
			const TSharedPtr<FDreamBuffer> Other = DreamTest::OpenScratch(*this, *Tab, TEXT("hover_probe = 1"), TEXT("ChromeHover"));
			if (!Other.IsValid())
			{
				return false;
			}
			*OtherId = Other->GetId();
			*OtherName = Other->GetDisplayName();
			Tab->ActivateBuffer(BufferId);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("hover the other tab"), [this, OtherName]()
		{
			// The other tab's name label, its row (the face's content) and its close button.
			TArray<TSharedRef<SWidget>> Labels;
			Collect(Tab.ToSharedRef(), [&OtherName](const TSharedRef<SWidget>& W)
			{
				return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString() == *OtherName;
			}, Labels);
			if (!TestEqual(TEXT("the other tab's label is in the strip once"), Labels.Num(), 1))
			{
				return false;
			}
			const TSharedPtr<SWidget> Row = Labels[0]->GetParentWidget();
			const TSharedPtr<SWidget> Face = Row.IsValid() ? Row->GetParentWidget() : nullptr;
			const TSharedPtr<SWidget> Close = Row.IsValid() ? FindTagged(Row.ToSharedRef(), FName(TEXT("DreamTab.Close"))) : nullptr;
			if (!TestTrue(TEXT("the tab has its face and close button"), Face.IsValid() && Close.IsValid()))
			{
				return false;
			}
			TestEqual(TEXT("an inactive tab's × is hidden"), Close->GetVisibility(), EVisibility::Hidden);
			FWidgetPath Path;
			if (!TestTrue(TEXT("the tab is in a visible window"), DreamTest::MakePointerPath(Face.ToSharedRef(), Path)))
			{
				return false;
			}
			const FVector2D At(Face->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.3f, 0.5f)));
			FSlateApplication& App = FSlateApplication::Get();
			App.RoutePointerMoveEvent(Path, FPointerEvent(App.GetUserIndexForMouse(), FSlateApplicationBase::CursorPointerIndex, At, At, TSet<FKey>(),
				EKeys::Invalid, 0.f, FModifierKeysState()), false);
			TestTrue(TEXT("the pointer is over the tab"), Face->IsHovered());
			Close->SlatePrepass(1.f);   // the bound visibility read now (SlatePrepass updates the attributes first)
			TestEqual(TEXT("its × shows while hovered"), Close->GetVisibility(), EVisibility::Visible);
			// Away again: over the caret position text.
			if (const TSharedPtr<SWidget> Caret = FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.CaretPosition"))))
			{
				FWidgetPath Away;
				if (DreamTest::MakePointerPath(Caret.ToSharedRef(), Away))
				{
					const FVector2D There(Caret->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
					App.RoutePointerMoveEvent(Away, FPointerEvent(App.GetUserIndexForMouse(), FSlateApplicationBase::CursorPointerIndex, There, There,
						TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState()), false);
				}
			}
			return true;
		});
		Steps.Finally(TEXT("close the second tab"), [OtherId]() { DreamTest::CloseBuffer(*OtherId); });
		Steps.Run(Done);
	});

	It("starts the output/input splitter's drag from a press on either grip next to its thin handle (review r7)", [this]()
	{
		// (chrome) The handle is drawn 2 px thin; the grips (2 px at each area's edge) take a press from the widgets under
		// them and leave it to the splitter, so the target is 6 px. The press is routed along the grip's path, as Slate
		// routes a platform press; the splitter capturing the mouse is the start of its drag.
		if (!Tab.IsValid())
		{
			AddError(TEXT("no Dream tab"));
			return;
		}
		TArray<TSharedRef<SWidget>> Grips;
		Collect(Tab.ToSharedRef(), [](const TSharedRef<SWidget>& W)
		{
			const TSharedPtr<FTagMetaData> Meta = W->GetMetaData<FTagMetaData>();
			return Meta.IsValid() && Meta->Tag == FName(TEXT("DreamChrome.SplitterGrip"));
		}, Grips);
		if (!TestEqual(TEXT("two grips, one on each area's edge next to the handle"), Grips.Num(), 2))
		{
			return;
		}
		FSlateApplication& App = FSlateApplication::Get();
		const uint32 User = App.GetUserIndexForMouse();
		const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
		TArray<float> Heights;
		for (const TSharedRef<SWidget>& Grip : Grips)
		{
			Heights.Add(Grip->GetPaintSpaceGeometry().GetLocalSize().Y);
		}
		Heights.Sort();
		TestTrue(*FString::Printf(TEXT("the upper area's grip is 2 px and the lower area's 3 px (%.1f, %.1f)"), Heights[0], Heights[1]),
			FMath::IsNearlyEqual(Heights[0], 2.f, 0.01f) && FMath::IsNearlyEqual(Heights[1], 3.f, 0.01f));
		for (const TSharedRef<SWidget>& Grip : Grips)
		{
			FWidgetPath Path;
			if (!TestTrue(TEXT("the grip is in a visible window"), DreamTest::MakePointerPath(Grip, Path)))
			{
				continue;
			}
			const FVector2D At(Grip->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			App.RoutePointerMoveEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState()), false);
			App.RoutePointerMoveEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState()), false);
			App.RoutePointerDownEvent(Path, FPointerEvent(User, Pointer, At, At, Pressed, EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
			const TSharedPtr<FSlateUser> CursorUser = App.GetCursorUser();
			const TSharedPtr<SWidget> Captor = CursorUser.IsValid() ? CursorUser->GetCursorCaptor() : nullptr;
			TestTrue(*FString::Printf(TEXT("a press on the grip starts the splitter's drag (the mouse captor is %s)"),
				Captor.IsValid() ? *Captor->GetTypeAsString() : TEXT("nothing")), Captor.IsValid() && Captor->GetType() == FName(TEXT("SSplitter")));
			// The release, without a move in between: the sizes stay as they were.
			App.RoutePointerUpEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
			App.ReleaseAllPointerCapture();
		}
	});

	LatentIt("retries a refused settings write with a backoff timed from the end of each attempt, and warns once after 5 s (review r9)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		// (chrome) The test hook makes the next four write attempts fail as a refused write does (the file is not touched);
		// the fifth writes. Each attempt starts 0.5, 1, 2, then 4 s after the END of the previous one (never every frame), one
		// Warning comes after 5 s pending, and the written file ends the pending state and broadcasts OnFileWritten once.
		struct FRetryProbe
		{
			int32 Written = 0;
			FDelegateHandle Handle;
		};
		const TSharedRef<FRetryProbe> Probe = MakeShared<FRetryProbe>();
		AddExpectedMessage(TEXT("Could not write EditorSettings.ini for 5 s; still retrying."), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 1);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("four refused writes ahead; a SaveKey"), [this, Probe]()
		{
			if (!TestFalse(TEXT("no settings write is pending before the case"), UDreamUserSettings::GetSaveRetryStateForTest().bPending))
			{
				return false;
			}
			Probe->Handle = UDreamUserSettings::OnFileWritten().AddLambda([Probe]() { ++Probe->Written; });
			UDreamUserSettings::SetSaveFailuresForTest(4);
			UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));   // its own value: only the write
			const UDreamUserSettings::FSaveRetryStateForTest R = UDreamUserSettings::GetSaveRetryStateForTest();
			TestTrue(TEXT("the refused write is pending"), R.bPending);
			TestEqual(TEXT("nothing reached the file yet"), Probe->Written, 0);
			TestTrue(TEXT("the first retry is due 0.5 s after the end of the refused attempt"),
				R.AttemptEnds.Num() == 1 && FMath::IsNearlyEqual(R.NextAttemptSeconds - R.AttemptEnds[0], 0.5, 0.001));
			return true;
		});
		Steps.Poll(TEXT("the retries end in a written file"), []()
		{
			return UDreamUserSettings::GetSaveRetryStateForTest().bPending ? DreamTest::EStep::Wait : DreamTest::EStep::Next;
		}, 25.0);
		Steps.Do(TEXT("the attempts' timing"), [this, Probe]()
		{
			const UDreamUserSettings::FSaveRetryStateForTest R = UDreamUserSettings::GetSaveRetryStateForTest();
			if (!TestEqual(TEXT("five attempts: four refused, the last written"), R.AttemptStarts.Num(), 5)
				|| !TestEqual(TEXT("each attempt has its end"), R.AttemptEnds.Num(), 5))
			{
				return false;
			}
			const double Backoff[] = { 0.5, 1.0, 2.0, 4.0 };
			for (int32 Index = 1; Index < 5; ++Index)
			{
				const double Gap = R.AttemptStarts[Index] - R.AttemptEnds[Index - 1];
				TestTrue(*FString::Printf(TEXT("attempt %d starts %.3f s after the end of attempt %d (backoff %.1f s)"), Index + 1, Gap, Index,
					Backoff[Index - 1]), Gap >= Backoff[Index - 1] - 0.001 && Gap < Backoff[Index - 1] + 0.5);
			}
			TestEqual(TEXT("OnFileWritten once, for the written file"), Probe->Written, 1);
			return true;
		});
		Steps.Finally(TEXT("no forced failures left"), [Probe]()
		{
			UDreamUserSettings::SetSaveFailuresForTest(0);
			UDreamUserSettings::OnFileWritten().Remove(Probe->Handle);
		});
		Steps.Run(Done);
	});

	LatentIt("ends a pending settings write when the next SaveKey writes the file (review r9)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		struct FEndProbe
		{
			int32 Written = 0;
			FDelegateHandle Handle;
			double RefusedAt = 0.0;
			bool bStrict = false;       // the SaveKey's own write ended the pending state (not refused by a scanner)
			int32 AttemptsAfter = 0;
		};
		const TSharedRef<FEndProbe> Probe = MakeShared<FEndProbe>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("one refused write"), [this, Probe]()
		{
			Probe->Handle = UDreamUserSettings::OnFileWritten().AddLambda([Probe]() { ++Probe->Written; });
			UDreamUserSettings::SetSaveFailuresForTest(1);
			UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));
			Probe->RefusedAt = FPlatformTime::Seconds();
			return TestTrue(TEXT("the refused write is pending"), UDreamUserSettings::GetSaveRetryStateForTest().bPending);
		});
		// 0.3 s: before the retry is due (0.5 s), and past the window in which a real write right after another is refused.
		Steps.Poll(TEXT("0.3 s"), [Probe]()
		{
			return FPlatformTime::Seconds() - Probe->RefusedAt >= 0.3 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("the next SaveKey writes the file"), [this, Probe]()
		{
			const int32 Attempts = UDreamUserSettings::GetSaveRetryStateForTest().AttemptStarts.Num();
			UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));
			const UDreamUserSettings::FSaveRetryStateForTest R = UDreamUserSettings::GetSaveRetryStateForTest();
			if (R.bPending)
			{
				AddInfo(TEXT("the file refused this real write too (a scanner): the retry writes it; the SaveKey-success path is not checked this time"));
				return true;
			}
			TestEqual(TEXT("the SaveKey's write was the one after the refused attempt (no retry ran)"), R.AttemptStarts.Num(), Attempts + 1);
			TestEqual(TEXT("OnFileWritten once"), Probe->Written, 1);
			Probe->bStrict = true;
			Probe->AttemptsAfter = R.AttemptStarts.Num();
			return true;
		});
		Steps.Poll(TEXT("nothing pending"), []()
		{
			return UDreamUserSettings::GetSaveRetryStateForTest().bPending ? DreamTest::EStep::Wait : DreamTest::EStep::Next;
		}, 10.0);
		// Past the time the dropped retry would have been due (0.5 s after the refused attempt).
		Steps.Poll(TEXT("0.8 s after the refused write"), [Probe]()
		{
			return FPlatformTime::Seconds() - Probe->RefusedAt >= 0.8 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("no retry rewrites the file later"), [this, Probe]()
		{
			if (!Probe->bStrict)
			{
				TestTrue(TEXT("the file was written once the write ended"), Probe->Written >= 1);
				return true;
			}
			TestEqual(TEXT("the file was written once: no retry ran after the SaveKey's write"), Probe->Written, 1);
			TestEqual(TEXT("no attempt after it"), UDreamUserSettings::GetSaveRetryStateForTest().AttemptStarts.Num(), Probe->AttemptsAfter);
			return true;
		});
		Steps.Finally(TEXT("no forced failures left"), [Probe]()
		{
			UDreamUserSettings::SetSaveFailuresForTest(0);
			UDreamUserSettings::OnFileWritten().Remove(Probe->Handle);
		});
		Steps.Run(Done);
	});

	LatentIt("says once that a read-only settings file is not saved and does not retry it (review r10)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		// (chrome) The test hook's ReadOnly kind answers as a read-only EditorSettings.ini does (the file is not touched): one
		// Warning, nothing pending, no retry; a second refused SaveKey says nothing more; the next SaveKey writes the file.
		struct FReadOnlyProbe
		{
			int32 Written = 0;
			FDelegateHandle Handle;
			double At = 0.0;
		};
		const TSharedRef<FReadOnlyProbe> Probe = MakeShared<FReadOnlyProbe>();
		AddExpectedMessage(TEXT("EditorSettings.ini is read-only; Dream Python Editor settings are not saved."), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 1);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("two SaveKeys on a read-only file"), [this, Probe]()
		{
			if (!TestFalse(TEXT("no settings write is pending before the case"), UDreamUserSettings::GetSaveRetryStateForTest().bPending))
			{
				return false;
			}
			Probe->Handle = UDreamUserSettings::OnFileWritten().AddLambda([Probe]() { ++Probe->Written; });
			UDreamUserSettings::SetSaveFailuresForTest(2, UDreamUserSettings::ESaveFailureForTest::ReadOnly);
			UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));
			TestFalse(TEXT("a read-only file leaves nothing pending (no retry)"), UDreamUserSettings::GetSaveRetryStateForTest().bPending);
			UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));   // no second warning
			TestFalse(TEXT("still nothing pending"), UDreamUserSettings::GetSaveRetryStateForTest().bPending);
			TestEqual(TEXT("nothing was written"), Probe->Written, 0);
			Probe->At = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("0.8 s: no retry comes"), [Probe]()
		{
			return FPlatformTime::Seconds() - Probe->At >= 0.8 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("no retry; the next SaveKey writes"), [this, Probe]()
		{
			TestEqual(TEXT("no retry wrote the file"), Probe->Written, 0);
			TestFalse(TEXT("nothing pending"), UDreamUserSettings::GetSaveRetryStateForTest().bPending);
			UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));
			if (UDreamUserSettings::GetSaveRetryStateForTest().bPending)
			{
				AddInfo(TEXT("the file refused this real write (a scanner); its retry writes it"));
			}
			return true;
		});
		Steps.Poll(TEXT("the file is written"), [Probe]()
		{
			return Probe->Written > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Finally(TEXT("no forced failures left"), [Probe]()
		{
			UDreamUserSettings::SetSaveFailuresForTest(0);
			UDreamUserSettings::OnFileWritten().Remove(Probe->Handle);
		});
		Steps.Run(Done);
	});

	LatentIt("flushes a pending settings write once at shutdown, and warns when that flush fails too (review r10)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		// (chrome) UDreamUserSettings::CancelSaveRetry is what ShutdownModule calls: with a write pending it flushes once,
		// checked, whether or not the core ticker still holds the retry; afterwards nothing is pending and no retry runs.
		struct FShutdownProbe
		{
			int32 Written = 0;
			FDelegateHandle Handle;
			double At = 0.0;
			int32 Attempts = 0;
			bool bRefused = false;      // a scanner refused the real shutdown flush
		};
		const TSharedRef<FShutdownProbe> Probe = MakeShared<FShutdownProbe>();
		// At least once (the forced refusal); a second time only when a scanner also refuses the real flush (said below).
		AddExpectedMessage(TEXT("Could not write EditorSettings.ini at shutdown; the last setting changes may be lost."), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 0);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a pending write; the shutdown flush is refused too"), [this, Probe]()
		{
			if (!TestFalse(TEXT("no settings write is pending before the case"), UDreamUserSettings::GetSaveRetryStateForTest().bPending))
			{
				return false;
			}
			Probe->Handle = UDreamUserSettings::OnFileWritten().AddLambda([Probe]() { ++Probe->Written; });
			UDreamUserSettings::SetSaveFailuresForTest(2);
			UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));
			TestTrue(TEXT("the refused write is pending"), UDreamUserSettings::GetSaveRetryStateForTest().bPending);
			UDreamUserSettings::CancelSaveRetry();   // its flush is the second forced refusal: the Warning
			TestFalse(TEXT("nothing pending after the shutdown flush"), UDreamUserSettings::GetSaveRetryStateForTest().bPending);
			TestEqual(TEXT("nothing was written"), Probe->Written, 0);
			Probe->At = FPlatformTime::Seconds();
			return true;
		});
		// 0.3 s since the last real write anywhere in the pass is past the window in which the file refuses a quick rewrite.
		Steps.Poll(TEXT("0.3 s"), [Probe]()
		{
			return FPlatformTime::Seconds() - Probe->At >= 0.3 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("a pending write; the shutdown flush writes it"), [this, Probe]()
		{
			UDreamUserSettings::SetSaveFailuresForTest(1);
			UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));
			TestTrue(TEXT("the refused write is pending"), UDreamUserSettings::GetSaveRetryStateForTest().bPending);
			UDreamUserSettings::CancelSaveRetry();
			const UDreamUserSettings::FSaveRetryStateForTest R = UDreamUserSettings::GetSaveRetryStateForTest();
			TestFalse(TEXT("nothing pending after the shutdown flush"), R.bPending);
			// (scratch, R-p1-12) Strict again (the r11 fallback passed when the real flush wrote nothing). The one excuse is a
			// write of the same file just before by the scratch files' sample seeding (bSamplesSeeded), which the file can
			// refuse a quick rewrite after: then that write is what this case asserts.
			const double SinceSeedWrite = DreamScratch::GetLastFlagWriteSecondsForTest() > 0.0
				? FPlatformTime::Seconds() - DreamScratch::GetLastFlagWriteSecondsForTest() : -1.0;
			if (Probe->Written == 0 && SinceSeedWrite >= 0.0 && SinceSeedWrite < 2.0)
			{
				TestTrue(*FString::Printf(TEXT("the sample seeding wrote EditorSettings.ini %.2f s before the refused shutdown flush"), SinceSeedWrite), true);
				Probe->bRefused = true;
			}
			else
			{
				TestEqual(TEXT("the shutdown flush wrote the file (OnFileWritten once)"), Probe->Written, 1);
			}
			Probe->Attempts = R.AttemptStarts.Num();
			Probe->At = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("0.8 s: no retry comes"), [Probe]()
		{
			return FPlatformTime::Seconds() - Probe->At >= 0.8 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("the retry is gone"), [this, Probe]()
		{
			TestEqual(TEXT("no retry wrote the file afterwards"), Probe->Written, Probe->bRefused ? 0 : 1);
			TestEqual(TEXT("no attempt afterwards"), UDreamUserSettings::GetSaveRetryStateForTest().AttemptStarts.Num(), Probe->Attempts);
			return true;
		});
		Steps.Finally(TEXT("no forced failures left"), [Probe]()
		{
			UDreamUserSettings::SetSaveFailuresForTest(0);
			UDreamUserSettings::OnFileWritten().Remove(Probe->Handle);
		});
		Steps.Run(Done);
	});
}
