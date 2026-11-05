// SPDX-License-Identifier: Apache-2.0
// The Dream tab body (TDD §6.4 layout, §10.3 restore, §4.5 run commands, WS-01..WS-03, WS-05, BUF-01..BUF-03, PO-01, PO-02).
// Every command reaches ExecuteDreamCommand: from FDreamKeyRouter (keys), the toolbar and the menus. Nothing here calls
// Python: run commands build an FDreamRunRequest and Submit it (D14(1)).
#include "Shell/SDreamTab.h"

#include "Debug/DreamDebugger.h"      // (debug)
#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "CodeView/SDreamCompletionPopup.h"   // (chrome) Edit ▸ Complete
#include "DesktopPlatformModule.h"
#include "Framework/Application/IMenu.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IDesktopPlatform.h"
#include "IPythonScriptPlugin.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Language/DreamLanguageServices.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamInstances.h"   // (instances)
#include "Persistence/DreamLocalHistory.h"   // (persist) the paused notice
#include "Persistence/DreamRestoreGuard.h"
#include "Shell/SDreamRecoveryReview.h"
#include "Python/DreamPython.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Run/DreamRunSourceText.h"   // (run) ExactSelection
#include "Run/DreamTransactions.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamEditorTabs.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamRecoveryBanner.h"
#include "Shell/SDreamStatusStrip.h"
#include "Shell/SDreamSideBar.h"   // (sidebar)
#include "Shell/SDreamTabImpl.h"
#include "Shell/DreamChrome.h"
#include "Shell/DreamProjectFiles.h"   // (sidebar) PO-05-A1
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "dream/EditBuilders.h"
#include "dream/JournalFormat.h"
#include "dream/StatementExtent.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamTabLocal
{
	/** The banner line for buffers still pending from an earlier session (§10.7.1): present tense, so it follows the count. */
	FText EarlierSessionText(int32 Count)
	{
		return FText::Format(LOCTEXT("RecoveredEarlier", "{0} recovered {0}|plural(one=buffer,other=buffers) from an earlier session {0}|plural(one=is,other=are) not restored yet."),
			FText::AsNumber(Count));
	}

	constexpr float MinAreaPixels = 120.f;          // WS-02: each slot keeps at least 120 px
	// (chrome) The centre splitter's handle: drawn 2 px thin (FDreamChrome::Splitter), grabbed over 6 px, its gap plus a
	// grip of 2 px at each area's edge (review r7: the history's rows and the input tab row took a press next to it).
	constexpr float SplitterHandlePixels = 2.f;
	constexpr float SplitterGripPixels = 2.f;
	constexpr double StatusMessageSeconds = 6.0;
	constexpr float SessionSaveDebounceSeconds = 30.f;   // §10.3: never per keystroke

	/** The session is restored once per editor session; a re-spawned tab adopts the open buffers (§10.3). */
	bool GSessionRestored = false;
	/** Safe mode (§10.8): the skipped session's ini tab list, written back with this session's tabs until Restore tabs. */
	TArray<FDreamTabState> GSkippedIniTabs;
	bool bGSkippedSessionPending = false;
	/** (tests) Set for the duration of ApplyRestore (SDreamTab::IsApplyingRestoreForTest). */
	bool bGApplyingRestore = false;

	FDreamEditorModule& Module()
	{
		return FDreamEditorModule::Get();
	}

	FText NamespaceName(EDreamNamespaceMode Mode)
	{
		return Mode == EDreamNamespaceMode::Isolated ? LOCTEXT("NsIsolated", "Isolated") : LOCTEXT("NsSession", "Session");
	}

	EDreamTabKind ToTabKind(EDreamBufferKind Kind)
	{
		switch (Kind)
		{
		case EDreamBufferKind::File: return EDreamTabKind::File;
		case EDreamBufferKind::Recovered: return EDreamTabKind::Recovered;
		default: return EDreamTabKind::Scratch;
		}
	}

	EDreamBufferKind FromTabKind(EDreamTabKind Kind)
	{
		switch (Kind)
		{
		case EDreamTabKind::File: return EDreamBufferKind::File;
		case EDreamTabKind::Recovered: return EDreamBufferKind::Recovered;
		default: return EDreamBufferKind::Scratch;
		}
	}

	/** Lines [First, Last] of a snapshot joined with '\n' (no trailing newline). */
	FString JoinLines(const dream::TextSnapshot& Snapshot, int32 First, int32 Last)
	{
		const dream::Offset Begin = Snapshot.LineStart(First);
		const dream::Offset End = Snapshot.LineEnd(Last);
		std::u16string Text;
		Snapshot.CopyRange(Begin, End - Begin, Text);
		return DreamText::ToFString(Text);
	}

	/** "Label (chord)" with the chord as bound now (§4.9.2, T-KEY-005: rebinds apply without a restart). */
	FText ChordClause(EDreamCommand Command)
	{
		return FDreamKeyRouter::Get().GetChordText(Command);
	}

	/** The code area's empty state (SHL-17): the hint names the chords as they are bound now. */
	FText EmptyStateText(bool bAfterClose)
	{
		const FText NewChord = ChordClause(EDreamCommand::NewTab);
		const FText ReopenChord = ChordClause(EDreamCommand::ReopenClosedTab);
		if (NewChord.IsEmpty())
		{
			return LOCTEXT("NoTabsPlain", "No tab is open.");
		}
		if (bAfterClose && !ReopenChord.IsEmpty())
		{
			return FText::Format(LOCTEXT("NoTabs2", "No tab is open. {0} opens a scratch tab; {1} reopens the last closed one."), NewChord, ReopenChord);
		}
		return FText::Format(LOCTEXT("NoTabs", "No tab is open. {0} opens a scratch tab."), NewChord);
	}

	/** (chrome) The Python states the tab's banner line explains (§3.5): no Python to run with. */
	bool ShowsPythonBanner(EDreamPythonState State)
	{
		return State == EDreamPythonState::Unavailable || State == EDreamPythonState::Disabled || State == EDreamPythonState::BootFailed
			|| State == EDreamPythonState::ShutDown || State == EDreamPythonState::Headless;
	}

	/** (chrome) What a banner's × remembers (PO-04-R1): the Python state and its reason; the safe-mode reason. */
	FString PythonKey(const FDreamPython& Python)
	{
		return FString::Printf(TEXT("%d|%s"), static_cast<int32>(Python.GetState()), *Python.GetDisabledReason().ToString());
	}

	FString SafeModeKey(EDreamSafeModeReason Reason)
	{
		return FString::FromInt(static_cast<int32>(Reason));
	}

	bool IsRunCommand(EDreamCommand Command)
	{
		return Command == EDreamCommand::RunSelectionOrBuffer || Command == EDreamCommand::RunBuffer || Command == EDreamCommand::RunLine
			|| Command == EDreamCommand::RunLineAdvance || Command == EDreamCommand::RunFile
			|| Command == EDreamCommand::DebugRun;   // (debug) PO-06 F5: F5 when idle (U21)
	}

	/**
	 * The banner's Enable Python (§3.5, §9.5, D16, SEC-07): never in restrictive mode; after a shutdown in this session;
	 * for Disabled only when the project's Python lifecycle is dynamic and Python is configured but not initialized.
	 * Deferred (M4): the policy key python.allow_enable_at_runtime joins the condition.
	 */
	bool CanOfferEnablePython(const FDreamPython& Python)
	{
		if (Python.IsRestrictiveMode())
		{
			return false;
		}
		if (Python.GetState() == EDreamPythonState::ShutDown)
		{
			return true;
		}
		if (Python.GetState() != EDreamPythonState::Disabled)
		{
			return false;
		}
		const IConsoleVariable* Dynamic = IConsoleManager::Get().FindConsoleVariable(TEXT("Engine.Python.IsPythonLifeCycleDynamic"));
		const IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
		return Dynamic != nullptr && Dynamic->GetBool() && Plugin != nullptr && Plugin->IsPythonConfigured() && !Plugin->IsPythonInitialized();
	}
}

using DreamTabLocal::Module;

// ---------------------------------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::Construct(const FArguments& InArgs, const TSharedRef<SDockTab>& InOwnerTab)
{
	OwnerTab = InOwnerTab;
	Impl = MakeShared<FImpl>();

	const UDreamPerProjectUserSettings* Session = UDreamPerProjectUserSettings::Get();
	Impl->Position = UDreamUserSettings::Get()->OutputPosition;
	Impl->OutputValue = FMath::Clamp(Session->OutputAreaFraction, 0.1f, 0.9f);   // WS-02: 40% in a fresh profile
	Impl->CodeValue = 1.f - Impl->OutputValue;

	BindMenuCommands();
	BuildLayout();
	BindEvents();

	// A tab spawned again in the same editor session shows the buffers that stayed open (they live in the manager), and the
	// banner again for those still waiting as recoveries: Restore all and Discard are their only controls (PER-06).
	for (const TSharedRef<FDreamBuffer>& Buffer : Module().GetBufferManager().GetOpenBuffers())
	{
		Impl->TabOrder.AddUnique(Buffer->GetId());
		if (Buffer->HasPendingRecovery())
		{
			Impl->PendingRecoveries.AddUnique(Buffer->GetId());
		}
	}
	if (Impl->PendingRecoveries.Num() > 0)
	{
		Impl->SetRecoveryLines({ DreamTabLocal::EarlierSessionText(Impl->PendingRecoveries.Num()) }, 0);
		Impl->bShowRecoveryBanner = true;
	}
	if (Impl->TabOrder.Num() > 0)
	{
		FGuid Active;
		if (!FGuid::Parse(Session->ActiveTab, Active) || !Impl->TabOrder.Contains(Active))
		{
			Active = Impl->TabOrder[0];
		}
		ActivateBuffer(Active);
	}
	RefreshTabStrip();
	RebuildBanners();
}

SDreamTab::~SDreamTab()
{
	if (!Impl.IsValid())
	{
		return;
	}
	if (FDreamEditorModule::IsAvailable())
	{
		SaveSessionState(true);
		FDreamEditorModule& M = Module();
		M.GetBufferManager().OnBufferOpened().Remove(Impl->BufferOpenedHandle);
		M.GetBufferManager().OnBufferClosed().Remove(Impl->BufferClosedHandle);
		M.GetBufferManager().OnFileStateChanged().Remove(Impl->FileStateHandle);   // (buffers)
		M.GetRunService().OnRunFinished().Remove(Impl->RunFinishedHandle);
		M.GetPython().OnStateChanged().Remove(Impl->PythonStateHandle);
		M.GetInstances().OnChanged().Remove(Impl->InstancesHandle);   // (instances)
		M.GetLocalHistory().OnPausedChanged().Remove(Impl->HistoryPausedHandle);   // (persist)
	}
	if (UObjectInitialized())
	{
		UDreamUserSettings::GetMutable()->OnSettingChanged().Remove(Impl->SettingChangedHandle);
	}
	UDreamUserSettings::OnViewSettingsChanged().Remove(Impl->ViewSettingsHandle);   // (chrome)
	FDreamDeferral::RemoveTicker(Impl->FocusCheckTicker);
	FDreamDeferral::RemoveTicker(Impl->SaveTicker);
}

void SDreamTab::BuildLayout()
{
	OutputArea = SNew(SDreamOutputArea)
		.CommandTarget(SharedThis(this))
		.InitialTab(static_cast<EDreamOutputTab>(FMath::Clamp(UDreamPerProjectUserSettings::Get()->ActiveOutputTab, 0, 2)))
		.OnActiveTabChanged_Lambda([this]() { SaveSessionState(false); });

	SAssignNew(Impl->EditorTabs, SDreamEditorTabs)
		.OnActivate_Lambda([this](const FGuid& Id)
		{
			ActivateBuffer(Id);
			if (TSharedPtr<SDreamCodeView> View = GetActiveCodeView())
			{
				View->FocusEditor();
			}
		})
		.OnClose_Lambda([this](const FGuid& Id) { CloseBuffer(Id); })
		.OnNewTab_Lambda([this]() { ExecuteDreamCommand(EDreamCommand::NewTab); })
		.CaretText_Lambda([this]() { return GetCaretText(); })          // (chrome) PO-04: line:col at the row's right end
		.MessageText_Lambda([this]() { return GetTabRowMessage(); })
		.MessageTooltip_Lambda([this]() { return GetTabRowMessageTooltip(); })
		.MessageColor_Lambda([this]() { return GetTabRowMessageColor(); })
		.OnMessageClicked_Lambda([this]() { ClickTabRowMessage(); });

	// The code area: the input tab row, then the active buffer's view. (chrome) PO-04 removed the breadcrumb row; symbol
	// breadcrumbs (NAV-02) are Deferred (M2) and would not bring it back by default.
	CodeArea = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Impl->EditorTabs.ToSharedRef()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			// (persist) The active buffer's "Not restored yet" bar (§10.7.1, §10.7.2).
			SAssignNew(Impl->PendingBarHost, SBox)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			// (buffers) The active buffer's file bars: changed or deleted on disk, a failed save (§10.14, §10.15; SDreamTabFiles.cpp).
			SAssignNew(Impl->FileBarHost, SBox)
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		[
			// (buffers) The code host under the RUN-26 hint chip's layer (§4.9.4; SDreamTabFiles.cpp).
			BuildCodeOverlay(SAssignNew(Impl->CodeHost, SBox)
			[
				SNew(SBox)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([]() { return DreamTabLocal::EmptyStateText(/*bAfterClose*/ false); })
				]
			])
		];

	SAssignNew(CentreSplitter, SSplitter)
		.Style(&FDreamChrome::Splitter())                 // (chrome) a thin theme-coloured handle
		.Orientation(Orient_Vertical)
		.PhysicalSplitterHandleSize(DreamTabLocal::SplitterHandlePixels)
		// SSplitter's hit range is open at both ends: one more pixel so it covers each grip's outer edge row (review r8).
		.HitDetectionSplitterHandleSize(DreamTabLocal::SplitterHandlePixels + 2.f * DreamTabLocal::SplitterGripPixels + 1.f);
	SetOutputPosition(Impl->Position);

	// (chrome) The whole tab draws on the theme's chrome background, and every UseForeground()/UseSubduedForeground() below
	// follows the theme's chrome text (PO-04: the tab no longer looks like the engine's default frame).
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamChrome::Brush(EDreamChromeSlot::Background))
		.ForegroundColor(FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
		.Padding(FMargin(0.f))
		[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildToolbar()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(Impl->BannerHost, SVerticalBox)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			// (persist) The Recovery review panel (§10.7.3), docked under the banners until the right side bar exists.
			SAssignNew(Impl->ReviewHost, SBox)
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		[
			BuildSideBarLayout()   // (sidebar) the side bar beside the centre splitter
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			// (chrome) Hidden unless View ▸ Status Line is on (PO-04: no status bar by default); HandleViewSettingsChanged.
			SAssignNew(Impl->StatusLine, SDreamStatusStrip)
			.Visibility(UDreamUserSettings::Get()->bShowStatusLine ? EVisibility::Visible : EVisibility::Collapsed)
			.Message_Lambda([this]()
			{
				return FPlatformTime::Seconds() < Impl->StatusMessageUntil ? Impl->StatusMessage : FText::GetEmpty();
			})
			.NamespaceText_Lambda([this]()
			{
				TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
				return DreamTabLocal::NamespaceName(Buffer.IsValid() ? Buffer->GetViewState().Namespace : EDreamNamespaceMode::Session);
			})
			.CursorText_Lambda([this]()
			{
				TSharedPtr<SDreamCodeView> View = GetActiveCodeView();
				if (!View.IsValid())
				{
					return FText::GetEmpty();
				}
				int32 Line = 0;
				int32 Column = 0;
				View->GetCaret(Line, Column);
				return FText::Format(LOCTEXT("CursorFmt", "Ln {0}, Col {1}"), FText::AsNumber(Line + 1), FText::AsNumber(Column + 1));
			})
		]
		]
	];
}

TSharedRef<SWidget> SDreamTab::BuildSideBarLayout()
{
	// (sidebar) PO-05 section 1, WS-02: the side bar on the left, full height beside the output and code areas and outside
	// their splitter, so moving the output Top or Bottom never moves it. Its slot is sized to the side bar's width
	// (SizeToContent, set by the drag through OnSlotResized; 240 px by default, 160 at least) and the centre column takes
	// the rest. With both sections off the side bar is collapsed, and SSplitter leaves out a collapsed slot and its handle:
	// no strip and no gap. The rule between them is 1 px; a grip on each side of it takes a press for the splitter (A220).
	Impl->SideBar = SNew(SDreamSideBar, TWeakPtr<SDreamTab>(SharedThis(this)));
	SAssignNew(Impl->OuterSplitter, SSplitter)
		.Style(&FDreamChrome::Splitter())
		.Orientation(Orient_Horizontal)
		.PhysicalSplitterHandleSize(SDreamSideBar::HandlePixels)
		.HitDetectionSplitterHandleSize(SDreamSideBar::HitPixels)
		+ SSplitter::Slot()
		.SizeRule(SSplitter::SizeToContent)
		.Resizable(true)
		.MinSize(SDreamSideBar::MinWidth)
		.OnSlotResized_Lambda([this](float NewWidth)
		{
			if (Impl->SideBar.IsValid())
			{
				Impl->SideBar->SetWidth(NewWidth);   // saved with the session, as the output/code split is
			}
		})
		[
			Impl->SideBar.ToSharedRef()
		]
		+ SSplitter::Slot()
		.MinSize(SDreamSideBar::MinCentreWidth)   // (sidebar)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				CentreSplitter.ToSharedRef()
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Left)
			[
				SAssignNew(Impl->CentreGrip, SBox)
				.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamSideBar.Grip")))
				.WidthOverride(SDreamSideBar::GripPixels)
				.Visibility_Lambda([this]()
				{
					return Impl->SideBar.IsValid() && Impl->SideBar->IsSideBarShown() ? EVisibility::Visible : EVisibility::Collapsed;
				})
			]
		];
	return Impl->OuterSplitter.ToSharedRef();
}

TSharedPtr<SDreamSideBar> SDreamTab::GetSideBar() const
{
	return Impl.IsValid() ? Impl->SideBar : nullptr;   // (sidebar)
}

TSharedPtr<SSplitter> SDreamTab::GetOuterSplitter() const
{
	return Impl.IsValid() ? Impl->OuterSplitter : nullptr;   // (sidebar)
}

void SDreamTab::BindEvents()
{
	FDreamEditorModule& M = Module();
	Impl->BufferOpenedHandle = M.GetBufferManager().OnBufferOpened().AddSP(this, &SDreamTab::HandleBufferOpened);
	Impl->BufferClosedHandle = M.GetBufferManager().OnBufferClosed().AddSP(this, &SDreamTab::HandleBufferClosed);
	Impl->FileStateHandle = M.GetBufferManager().OnFileStateChanged().AddSP(this, &SDreamTab::HandleFileStateChanged);   // (buffers)
	Impl->RunFinishedHandle = M.GetRunService().OnRunFinished().AddSP(this, &SDreamTab::HandleRunFinished);
	Impl->PythonStateHandle = M.GetPython().OnStateChanged().AddSP(this, &SDreamTab::RebuildBanners_PythonState);
	Impl->HistoryPausedHandle = M.GetLocalHistory().OnPausedChanged().AddSP(this, &SDreamTab::RebuildBanners);   // (persist)
	Impl->SettingChangedHandle = UDreamUserSettings::GetMutable()->OnSettingChanged().AddSP(this, &SDreamTab::HandleSettingChanged);
	Impl->ViewSettingsHandle = UDreamUserSettings::OnViewSettingsChanged().AddSP(this, &SDreamTab::HandleViewSettingsChanged);   // (chrome)
}

// ---------------------------------------------------------------------------------------------------------------------
// (chrome) View settings (PO-04 point 1)
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::HandleViewSettingsChanged()
{
	const bool bFocusWasInside = HasFocusedDescendants() || HasKeyboardFocus() || FSlateApplication::Get().AnyMenusVisible();
	// An explicit value, not a bound getter: it applies at once, so a toggle and a check of it can share a frame.
	if (Impl->StatusLine.IsValid())
	{
		Impl->StatusLine->SetVisibility(UDreamUserSettings::Get()->bShowStatusLine ? EVisibility::Visible : EVisibility::Collapsed);
	}
	if (OutputArea.IsValid())
	{
		OutputArea->ApplyViewSettings();   // its tab row and prompt strip, before the focus check
	}
	if (Impl->SideBar.IsValid())
	{
		Impl->SideBar->ApplyViewSettings();   // (sidebar) View ▸ Project and View ▸ Structure, before the focus check
	}
	CheckFocusAfterViewChange(bFocusWasInside);
}

void SDreamTab::CheckFocusAfterViewChange(bool bFocusWasInside)
{
	// A last focus home that is hidden now is no home: a click on chrome goes to the code view instead (OnFocusReceived).
	if (const TSharedPtr<SWidget> Last = Impl->LastFocusedInside.Pin())
	{
		FWidgetPath Path;
		if (!FSlateApplication::Get().FindPathToWidget(Last.ToSharedRef(), Path))
		{
			Impl->LastFocusedInside.Reset();
		}
	}
	if (!bFocusWasInside)
	{
		return;
	}
	// True while a menu is still open (look again once it closes), false when done.
	auto Check = [WeakTab = TWeakPtr<SDreamTab>(SharedThis(this))]() -> bool
	{
		const TSharedPtr<SDreamTab> Tab = WeakTab.Pin();
		if (!Tab.IsValid() || !FSlateApplication::IsInitialized())
		{
			return false;
		}
		if (FSlateApplication::Get().AnyMenusVisible())
		{
			return true;
		}
		// Focus on a widget a toggle hid or destroyed (the prompt, a removed tab-row segment, a list under a hidden tab), or
		// on nothing: the active code view takes it, so typed keys never go nowhere.
		const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
		bool bLost = !Focused.IsValid();
		if (Focused.IsValid())
		{
			bool bInTab = false;
			for (TSharedPtr<SWidget> W = Focused; W.IsValid() && !bInTab; W = W->GetParentWidget())
			{
				bInTab = W == Tab;
			}
			FWidgetPath Path;
			bLost = bInTab && Focused != Tab && !FSlateApplication::Get().FindPathToWidget(Focused.ToSharedRef(), Path);
		}
		if (bLost)
		{
			if (const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView())
			{
				View->FocusEditor();
			}
		}
		return false;
	};
	FDreamDeferral::RemoveTicker(Impl->FocusCheckTicker);
	Impl->FocusCheckTicker.Reset();
	if (!Check())
	{
		return;   // settled now (a toggle from a key or from code)
	}
	// The View menu that sent the toggle is still open: look once it has closed and given focus back.
	Impl->FocusCheckTicker = FDreamDeferral::AddTicker(TEXT("DreamViewFocusCheck"), EDreamDeferPolicy::DeferWhileExecuting, 0.f, false,
		[Check, WeakImpl = TWeakPtr<FImpl>(Impl)](float)
		{
			const bool bAgain = Check();
			if (!bAgain)
			{
				if (const TSharedPtr<FImpl> Pinned = WeakImpl.Pin())
				{
					Pinned->FocusCheckTicker.Reset();
				}
			}
			return bAgain;
		});
}

bool SDreamTab::IsStatusLineShown() const
{
	return Impl->StatusLine.IsValid() && Impl->StatusLine->GetVisibility().IsVisible();
}

FText SDreamTab::GetCaretText() const
{
	const TSharedPtr<SDreamCodeView> View = GetActiveCodeView();
	if (!View.IsValid())
	{
		return FText::GetEmpty();
	}
	int32 Line = 0;
	int32 Column = 0;
	View->GetCaret(Line, Column);
	return FText::Format(LOCTEXT("Chrome.CaretFmt", "{0}:{1}"), FText::AsNumber(Line + 1, &FNumberFormattingOptions::DefaultNoGrouping()),
		FText::AsNumber(Column + 1, &FNumberFormattingOptions::DefaultNoGrouping()));
}

SDreamTab::EHealthWarning SDreamTab::GetHealthWarning() const
{
	// PO-04 hides bars, not warnings (PO-04-R1): with the status line hidden, what it or a hidden tab shows as a warning.
	if (IsStatusLineShown() || !FDreamEditorModule::IsAvailable())
	{
		return EHealthWarning::None;
	}
	FDreamEditorModule& M = Module();
	const EDreamJournalHealth Journal = M.GetJournal().GetHealth();
	if (Journal == EDreamJournalHealth::Degraded)
	{
		return EHealthWarning::JournalDegraded;   // edits are not crash-safe while commits fail (§10.11)
	}
	if (Journal == EDreamJournalHealth::Disabled && Impl->bRestoreRequested)
	{
		return EHealthWarning::JournalOff;
	}
	const EDreamPythonState Python = M.GetPython().GetState();
	if (DreamTabLocal::ShowsPythonBanner(Python) && Python != EDreamPythonState::Headless)
	{
		return EHealthWarning::PythonUnavailable;
	}
	if (M.GetLanguageServices().GetHealth() == EDreamLanguageHealth::Paused)
	{
		return EHealthWarning::LintPaused;
	}
	return EHealthWarning::None;
}

FText SDreamTab::GetTabRowMessage() const
{
	// A health warning outranks the transient messages (PO-04-R1).
	switch (GetHealthWarning())
	{
	case EHealthWarning::JournalDegraded: return LOCTEXT("Chrome.HealthJournalDegraded", "Journal retrying: edits not crash-safe");
	case EHealthWarning::JournalOff: return LOCTEXT("Chrome.HealthJournalOff", "Journal off");
	case EHealthWarning::PythonUnavailable: return LOCTEXT("Chrome.HealthPython", "Python not available");
	case EHealthWarning::LintPaused: return LOCTEXT("Chrome.HealthLintPaused", "Lint paused");
	default: break;
	}
	// With the status line hidden, its transient messages (refusal reasons, "End of buffer", "Saved x") show beside line:col.
	if (IsStatusLineShown() || FPlatformTime::Seconds() >= Impl->StatusMessageUntil)
	{
		return FText::GetEmpty();
	}
	return Impl->StatusMessage;
}

FText SDreamTab::GetTabRowMessageTooltip() const
{
	switch (GetHealthWarning())
	{
	case EHealthWarning::JournalDegraded:
		return LOCTEXT("Chrome.HealthJournalDegradedTip", "The journal cannot write its folder (a full disk or a held file); a crash now would lose the latest edits. Click to open the journal folder.");
	case EHealthWarning::JournalOff:
		return LOCTEXT("Chrome.HealthJournalOffTip", "This session's tabs are not journaled: no storage folder was writable. Click for the storage folder setting.");
	case EHealthWarning::PythonUnavailable:
		return FText::Format(LOCTEXT("Chrome.HealthPythonTip", "{0} Click to retry or to enable Python where Dream Python Editor can, else to show its notice."),
			Module().GetPython().GetDisabledReason());
	case EHealthWarning::LintPaused:
		return FText::Format(LOCTEXT("Chrome.HealthLintTip", "{0}. Click to restart dream-check."), Module().GetLanguageServices().GetHealthText());
	default:
		return GetTabRowMessage();
	}
}

FSlateColor SDreamTab::GetTabRowMessageColor() const
{
	return FDreamChrome::Color(GetHealthWarning() != EHealthWarning::None ? EDreamChromeSlot::Warning : EDreamChromeSlot::MutedText);
}

void SDreamTab::ClickTabRowMessage()
{
	FDreamEditorModule& M = Module();
	switch (GetHealthWarning())
	{
	case EHealthWarning::JournalDegraded:
		FPlatformProcess::ExploreFolder(*M.GetJournal().GetNamespaceDir());
		break;
	case EHealthWarning::JournalOff:
		if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
		{
			Settings->ShowViewer(TEXT("Editor"), TEXT("Plugins"), TEXT("Dream"));
		}
		break;
	case EHealthWarning::PythonUnavailable:
		if (M.GetPython().GetState() == EDreamPythonState::BootFailed)
		{
			M.GetPython().RetryBoot();
		}
		else if (DreamTabLocal::CanOfferEnablePython(M.GetPython()))
		{
			M.GetPython().RequestEnable();
		}
		else
		{
			// The Python banner line says why: shown again if its × hid it (the other banners keep their state).
			Impl->HiddenPythonKey.Reset();
			RebuildBanners();
		}
		break;
	case EHealthWarning::LintPaused:
		M.GetLanguageServices().RestartServers();
		break;
	default:
		break;   // a transient message has no action
	}
}

bool SDreamTab::IsRecoveryLineHidden() const
{
	if (Impl->HiddenRecoveryLineIds.IsEmpty() || Impl->RecoveryLineIds.IsEmpty())
	{
		return false;
	}
	for (const uint32 Id : Impl->RecoveryLineIds)
	{
		if (!Impl->HiddenRecoveryLineIds.Contains(Id))
		{
			return false;
		}
	}
	return true;
}

bool SDreamTab::HasHiddenNotices() const
{
	// What a banner would show now that its × hides (a hide that no longer matches anything counts for nothing).
	FDreamEditorModule& M = Module();
	const bool bRecovery = Impl->bShowRecoveryBanner && !Impl->bConfirmDiscard && IsRecoveryLineHidden();
	const bool bSafe = Impl->bSafeMode && Impl->SafeModeSession.IsSet() && !Impl->HiddenSafeModeKey.IsEmpty()
		&& Impl->HiddenSafeModeKey == DreamTabLocal::SafeModeKey(M.GetRestoreGuard().GetSafeModeReason());
	const bool bHistory = Impl->bHideHistoryBanner && M.GetLocalHistory().IsPaused();
	const bool bPython = DreamTabLocal::ShowsPythonBanner(M.GetPython().GetState()) && !Impl->HiddenPythonKey.IsEmpty()
		&& Impl->HiddenPythonKey == DreamTabLocal::PythonKey(M.GetPython());
	return bRecovery || bSafe || bHistory || bPython;
}

void SDreamTab::ShowHiddenNotices()
{
	Impl->HiddenRecoveryLineIds.Reset();
	Impl->HiddenSafeModeKey.Reset();
	Impl->HiddenPythonKey.Reset();
	Impl->bHideHistoryBanner = false;
	RebuildBanners();
}

void SDreamTab::AddRecoveryLineForTest(const FText& Line)
{
	Impl->AddRecoveryLine(Line);
	Impl->bShowRecoveryBanner = true;
	RebuildBanners();
}

void SDreamTab::SetEarlierSessionLineForTest(const TOptional<FText>& Text)
{
	Impl->SetEarlierSessionLine(Text);
	Impl->bShowRecoveryBanner = Impl->bShowRecoveryBanner && Impl->RecoveryLines.Num() > 0;
	RebuildBanners();
}

void SDreamTab::GetRecoveryLinesForTest(TArray<FText>& OutLines, int32& OutEarlierSessionLine, bool& bOutShown) const
{
	OutLines = Impl->RecoveryLines;
	OutEarlierSessionLine = Impl->EarlierSessionLine;
	bOutShown = Impl->bShowRecoveryBanner;
}

void SDreamTab::SetRecoveryLinesForTest(const TArray<FText>& Lines, int32 EarlierSessionLine, bool bShown)
{
	Impl->SetRecoveryLines(TArray<FText>(Lines), EarlierSessionLine);
	Impl->bShowRecoveryBanner = bShown;
	RebuildBanners();
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout: output position (WS-03)
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::SetOutputPosition(EDreamOutputPosition Position)
{
	Impl->Position = Position;
	while (CentreSplitter->NumSlots() > 0)
	{
		CentreSplitter->RemoveAt(CentreSplitter->NumSlots() - 1);
	}

	// Each slot's size is bound to the widget's own value, so a swap keeps both sizes (WS-03: "keep its size").
	// (chrome) Each area carries an invisible grip on its edge next to the handle: hit-testable but handling nothing, it
	// keeps a press there from the widgets under it, so the press bubbles to the splitter, whose hit detection covers the
	// grips (SplitterGripPixels): a 6 px target for a 2 px line.
	auto AddArea = [this](const TSharedRef<SWidget>& Widget, bool bOutput, bool bGripAtBottom)
	{
		CentreSplitter->AddSlot()
			.Value(TAttribute<float>::CreateLambda([this, bOutput]() { return bOutput ? Impl->OutputValue : Impl->CodeValue; }))
			.MinSize(DreamTabLocal::MinAreaPixels)
			.OnSlotResized_Lambda([this, bOutput](float NewValue)
			{
				(bOutput ? Impl->OutputValue : Impl->CodeValue) = NewValue;
				SaveSessionState(false);   // OnSlotResized writes the fraction to the session state (WS-02)
			})
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					Widget
				]
				+ SOverlay::Slot()
				.VAlign(bGripAtBottom ? VAlign_Bottom : VAlign_Top)
				[
					// The lower area's grip (at its top) is one pixel taller: the open 7 px hit range ends inside its
					// third row, so that row is the grip's and not the content's (review r9 nit).
					SNew(SBox)
					.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamChrome.SplitterGrip")))
					.HeightOverride(DreamTabLocal::SplitterGripPixels + (bGripAtBottom ? 0.f : 1.f))
					.Visibility(EVisibility::Visible)   // SBox defaults to SelfHitTestInvisible
				]
			];
	};
	if (Position == EDreamOutputPosition::Top)
	{
		AddArea(OutputArea.ToSharedRef(), true, /*bGripAtBottom*/ true);
		AddArea(CodeArea.ToSharedRef(), false, /*bGripAtBottom*/ false);
	}
	else
	{
		AddArea(CodeArea.ToSharedRef(), false, /*bGripAtBottom*/ true);
		AddArea(OutputArea.ToSharedRef(), true, /*bGripAtBottom*/ false);
	}

	UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
	if (Settings->OutputPosition != Position)
	{
		Settings->OutputPosition = Position;
		UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, OutputPosition));   // survives a restart (WS-03); this key only
	}
}

void SDreamTab::HandleSettingChanged(UObject* Settings, FPropertyChangedEvent& Event)
{
	const FName Property = Event.GetMemberPropertyName();
	const UDreamUserSettings* User = UDreamUserSettings::Get();
	if (Property == GET_MEMBER_NAME_CHECKED(UDreamUserSettings, OutputPosition) || Property.IsNone())
	{
		if (User->OutputPosition != Impl->Position)
		{
			SetOutputPosition(User->OutputPosition);   // applies at once (WS-03)
		}
	}
	if (Property == GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput) || Property.IsNone())
	{
		FDreamOutputStore& Store = Module().GetOutputStore();
		FDreamOutputVisibility Visibility = Store.GetVisibility();
		if (Visibility.bEcho != User->bEchoInput)
		{
			Visibility.bEcho = User->bEchoInput;
			Store.SetVisibility(Visibility);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Buffers and editor tabs
// ---------------------------------------------------------------------------------------------------------------------

TSharedPtr<FDreamBuffer> SDreamTab::GetActiveBuffer() const
{
	return Impl->ActiveId.IsValid() ? Module().GetBufferManager().FindById(Impl->ActiveId) : nullptr;
}

TSharedPtr<SDreamCodeView> SDreamTab::GetActiveCodeView() const
{
	return FindCodeView(Impl->ActiveId);
}

TSharedPtr<SDreamCodeView> SDreamTab::FindCodeView(const FGuid& BufferId) const
{
	const TSharedRef<SDreamCodeView>* View = Impl->Views.Find(BufferId);
	return View ? TSharedPtr<SDreamCodeView>(*View) : nullptr;
}

TArray<TSharedRef<FDreamBuffer>> SDreamTab::GetTabBuffers() const
{
	TArray<TSharedRef<FDreamBuffer>> Out;
	const FDreamBufferManager& Buffers = Module().GetBufferManager();
	for (const FGuid& Id : Impl->TabOrder)
	{
		if (TSharedPtr<FDreamBuffer> Buffer = Buffers.FindById(Id))
		{
			Out.Add(Buffer.ToSharedRef());
		}
	}
	return Out;
}

TSharedRef<SDreamCodeView> SDreamTab::FindOrCreateView(const TSharedRef<FDreamBuffer>& Buffer)
{
	if (const TSharedRef<SDreamCodeView>* Existing = Impl->Views.Find(Buffer->GetId()))
	{
		return *Existing;
	}
	const FGuid Id = Buffer->GetId();
	const int32 RestoredFirstLine = Buffer->GetViewState().FirstVisibleLine;   // (scratch, PER-02) before the view's caret move resets it
	TSharedRef<SDreamCodeView> View = SNew(SDreamCodeView)
		.Buffer(Buffer)
		.CommandTarget(SharedThis(this))
		.GutterMode(EDreamGutterMode::LineNumbers)
		.ContextMask(dream::Ctx_Buffer)
		.FontSize(UDreamUserSettings::Get()->FontSize)
		.OnCaretMoved_Lambda([Id]()
		{
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetJournal().OnViewStateChanged(Id);
			}
		})
		.OnEdited_Lambda([WeakImpl = TWeakPtr<FImpl>(Impl)]()
		{
			// (buffers) The RUN-26 chip waits a second after typing (§4.9.4).
			if (const TSharedPtr<FImpl> Pinned = WeakImpl.Pin())
			{
				Pinned->LastEditSeconds = FPlatformTime::Seconds();
			}
		});
	Impl->Views.Add(Id, View);
	ScheduleRestoredScroll(Buffer, View, RestoredFirstLine);   // (scratch, PER-02) a restored tab's scroll comes back (SDreamTabFiles.cpp)
	return View;
}

void SDreamTab::ActivateBuffer(const FGuid& BufferId)
{
	TSharedPtr<FDreamBuffer> Buffer = Module().GetBufferManager().FindById(BufferId);
	if (!Buffer.IsValid() || Buffer->GetKind() == EDreamBufferKind::ReplInput)
	{
		return;
	}
	Impl->TabOrder.AddUnique(BufferId);
	Impl->ActiveId = BufferId;
	Impl->CodeHost->SetContent(FindOrCreateView(Buffer.ToSharedRef()));
	RefreshTabStrip();
	SaveSessionState(false);
	RefreshRecoveryUi();   // (persist) the pending bar follows the active buffer
	RefreshFileBar();      // (buffers) and so do the file bars
}

void SDreamTab::RefreshTabStrip()
{
	Impl->EditorTabs->SetTabs(GetTabBuffers(), Impl->ActiveId);
	Module().GetJournal().SetSessionState(Impl->TabOrder, Impl->ActiveId);
}

TSharedRef<FDreamBuffer> SDreamTab::NewScratchTab(const FString& Text, const FString& Name, const FGuid& Id)
{
	if (Name.IsEmpty())
	{
		return NewScratchFileTab(Text, Id);   // (scratch, PO-05) an unnamed tab is scratch_N.py (SDreamTabFiles.cpp)
	}
	TSharedRef<FDreamBuffer> Buffer = Module().GetBufferManager().CreateScratch(Text, Name, Id);
	// The user's buffer-run default seeds the tab's toggle, which then decides for the tab's runs (§5.9.1, U3, §9.4).
	Buffer->GetViewState().bOneUndoStepPerRun = UDreamUserSettings::Get()->GetTransactionDefault(EDreamRunRoute::Buffer);
	ActivateBuffer(Buffer->GetId());
	return Buffer;
}

void SDreamTab::HandleBufferOpened(const TSharedRef<FDreamBuffer>& Buffer)
{
	if (Buffer->GetKind() == EDreamBufferKind::ReplInput || Impl->TabOrder.Contains(Buffer->GetId()))
	{
		return;
	}
	Impl->TabOrder.Add(Buffer->GetId());   // new and reopened tabs open at the end; a restore keeps its order
	if (!Impl->ActiveId.IsValid())
	{
		ActivateBuffer(Buffer->GetId());
	}
	RefreshTabStrip();
	SaveSessionState(false);
}

void SDreamTab::HandleBufferClosed(const TSharedRef<FDreamBuffer>& Buffer)
{
	const FGuid Id = Buffer->GetId();
	const int32 Index = Impl->TabOrder.IndexOfByKey(Id);
	// The focused text widget dies with its view: focus must move to a Dream widget, or every later key (Ctrl+S, Ctrl+W, a
	// letter) bubbles to the Level Editor (SHL-02, §4.9.4 "while a Dream widget has focus").
	// A click on the tab strip's x moved focus to this tab itself before the close (the leaf-most focusable widget under the
	// pointer, SlateApplication.cpp:5485), so focus held by SDreamTab counts as inside the closed tab too.
	const TSharedPtr<SDreamCodeView> ClosingView = FindCodeView(Id);
	const bool bFocusWasInClosed = (ClosingView.IsValid() && (ClosingView->HasFocusedDescendants() || ClosingView->HasKeyboardFocus()))
		|| HasKeyboardFocus();
	Impl->TabOrder.Remove(Id);
	Impl->Views.Remove(Id);
	const bool bWasPending = Impl->PendingRecoveries.Remove(Id) > 0;
	Impl->RecoveryDetails.Remove(Id);   // (persist)
	Impl->DiscardsInFlight.Remove(Id);
	if (Impl->ActiveId == Id)
	{
		Impl->ActiveId.Invalidate();
		if (Impl->TabOrder.Num() > 0)
		{
			ActivateBuffer(Impl->TabOrder[FMath::Clamp(Index - 1, 0, Impl->TabOrder.Num() - 1)]);
		}
		else
		{
			Impl->CodeHost->SetContent(
				SNew(SBox)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([]() { return DreamTabLocal::EmptyStateText(/*bAfterClose*/ true); })
				]);
		}
	}
	if (bFocusWasInClosed && FSlateApplication::IsInitialized())
	{
		if (const TSharedPtr<SDreamCodeView> Next = GetActiveCodeView())
		{
			Next->FocusEditor();
		}
		else
		{
			FSlateApplication::Get().SetKeyboardFocus(SharedThis(this), EFocusCause::SetDirectly);   // the empty state routes keys
		}
	}
	RefreshTabStrip();
	if (bWasPending)
	{
		// The banner's Restore all and Discard belong to the pending buffers: with the last one closed it shows its x (PER-06).
		if (Impl->PendingRecoveries.Num() == 0)
		{
			Impl->bConfirmDiscard = false;
		}
		// The line that counts the pending buffers follows their count, and goes with the last one (P3-shl-05); the
		// unclean-exit line is past tense and stays.
		if (Impl->RecoveryLines.IsValidIndex(Impl->EarlierSessionLine))
		{
			// (chrome) The count line keeps its id, so a line hidden with × stays hidden (review r7).
			Impl->SetEarlierSessionLine(Impl->PendingRecoveries.Num() > 0
				? TOptional<FText>(DreamTabLocal::EarlierSessionText(Impl->PendingRecoveries.Num())) : TOptional<FText>());
			Impl->bShowRecoveryBanner = Impl->bShowRecoveryBanner && Impl->RecoveryLines.Num() > 0;
		}
		RebuildBanners();
	}
	SaveSessionState(false);
	RefreshRecoveryUi();   // (persist)
	RefreshFileBar();      // (buffers)
}

void SDreamTab::CloseBuffer(const FGuid& BufferId)
{
	TSharedPtr<FDreamBuffer> Buffer = Module().GetBufferManager().FindById(BufferId);
	if (!Buffer.IsValid())
	{
		return;
	}
	const FString Name = Buffer->GetDisplayName();
	// BUF_CLOSE; a non-empty scratch tab stays reopenable (§10.2.5). The hint only when Reopen Closed Tab brings this one back.
	const bool bReopenable = Module().GetBufferManager().Close(BufferId);
	const FText Reopen = DreamTabLocal::ChordClause(EDreamCommand::ReopenClosedTab);
	ShowStatusMessage(bReopenable && !Reopen.IsEmpty()
		? FText::Format(LOCTEXT("ClosedTab", "Closed {0} ({1} reopens it)"), FText::FromString(Name), Reopen)
		: FText::Format(LOCTEXT("ClosedTabPlain", "Closed {0}"), FText::FromString(Name)));
}

void SDreamTab::CycleTab(int32 Direction)
{
	if (Impl->TabOrder.Num() < 2)
	{
		return;
	}
	const int32 Index = FMath::Max(0, Impl->TabOrder.IndexOfByKey(Impl->ActiveId));
	const int32 Next = (Index + Direction + Impl->TabOrder.Num()) % Impl->TabOrder.Num();
	ActivateBuffer(Impl->TabOrder[Next]);
	if (TSharedPtr<SDreamCodeView> View = GetActiveCodeView())
	{
		View->FocusEditor();
	}
}

bool SDreamTab::OpenLocation(const FString& PathOrPseudoFile, int32 Line, int32 Column, const FGuid& PreallocatedId)
{
	FDreamBufferManager& Buffers = Module().GetBufferManager();
	TSharedPtr<FDreamBuffer> Buffer;
	if (PathOrPseudoFile.StartsWith(TEXT("<dream:")))
	{
		Buffer = Buffers.ResolveFilename(PathOrPseudoFile);
		if (!Buffer.IsValid())
		{
			// A closed pseudo-file buffer: the link's click is the Reopen tab offer (§6.4). (buffers, B86) The per-buffer reopen
			// brings that buffer back with its text and ID, so the pseudo filename names it again.
			if (TSharedPtr<FDreamBuffer> Closed = Buffers.FindClosedByFilename(PathOrPseudoFile))
			{
				Buffer = Buffers.ReopenClosed(Closed->GetId());
				if (Buffer.IsValid())
				{
					ShowStatusMessage(FText::Format(LOCTEXT("Files.ReopenedTab", "Reopened {0}"), FText::FromString(Buffer->GetDisplayName())));
				}
			}
		}
		if (!Buffer.IsValid())
		{
			ShowStatusMessage(LOCTEXT("PseudoGone", "That tab is no longer open"));
			return false;
		}
	}
	else
	{
		FText Error;
		Buffer = Buffers.OpenFile(PathOrPseudoFile, Error, PreallocatedId);
		if (!Buffer.IsValid())
		{
			ShowStatusMessage(Error);
			return false;
		}
	}

	ActivateBuffer(Buffer->GetId());
	TSharedPtr<SDreamCodeView> View = GetActiveCodeView();
	if (View.IsValid() && Line > 0)
	{
		const int32 LineIndex = FMath::Clamp(Line - 1, 0, View->GetLineCount() - 1);
		if (Column > 0)
		{
			View->SetCaret(LineIndex, Column - 1);
		}
		else
		{
			// "The line is selected and the caret goes to the column if the frame has one" (§6.4 traceback links).
			FDreamTextSelection Selection;
			Selection.AnchorLine = LineIndex;
			Selection.CaretLine = LineIndex;
			Selection.CaretColumn = Buffer->Snapshot().LineLength(LineIndex);
			View->SetSelection(Selection);
		}
		View->ScrollToLine(LineIndex, 3);
	}
	if (View.IsValid())
	{
		View->FocusEditor();
	}
	return true;
}

void SDreamTab::ShowStatusMessage(const FText& Message)
{
	Impl->StatusMessage = Message;
	Impl->StatusMessageUntil = FPlatformTime::Seconds() + DreamTabLocal::StatusMessageSeconds;
}

TSharedPtr<SDreamOutputView> SDreamTab::GetActiveOutputView() const
{
	// CR-25: the output area decides (the REPL view while its REPL tab is active, else the Output view).
	return OutputArea.IsValid() ? OutputArea->GetActiveOutputView() : nullptr;
}

TSharedPtr<SWidget> SDreamTab::GetFocusToRestore() const
{
	TSharedPtr<SDreamCodeView> View = GetActiveCodeView();
	return View.IsValid() ? StaticCastSharedPtr<SWidget>(View) : StaticCastSharedPtr<SWidget>(OutputArea);
}

// ---------------------------------------------------------------------------------------------------------------------
// Commands (IDreamCommandTarget)
// ---------------------------------------------------------------------------------------------------------------------

bool SDreamTab::CanExecuteDreamCommand(EDreamCommand Command, FText& OutReason) const
{
	using namespace DreamTabLocal;
	const FDreamRunService& Runs = Module().GetRunService();
	const TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
	const TSharedPtr<SDreamCodeView> View = GetActiveCodeView();

	if (IsRunCommand(Command))
	{
		if (!Buffer.IsValid() || !View.IsValid())
		{
			OutReason = LOCTEXT("NoTab", "No tab is open");
			return false;
		}
		if (Runs.GetState() != EDreamRunState::Idle)
		{
			OutReason = Runs.GetState() == EDreamRunState::TaskRunning
				? LOCTEXT("TaskRunning", "Task running — stop it or wait")
				: LOCTEXT("RunInProgress", "Run in progress");
			return false;
		}
		EDreamRunRoute Route = EDreamRunRoute::Buffer;
		if (Command == EDreamCommand::RunLine || Command == EDreamCommand::RunLineAdvance)
		{
			Route = EDreamRunRoute::Line;
		}
		else if (Command == EDreamCommand::RunFile)
		{
			if (!Buffer->HasPath())
			{
				OutReason = LOCTEXT("RunFileNeedsPath", "Save this tab to a file first (Run File runs the file)");
				return false;
			}
			Route = EDreamRunRoute::RunFile;
		}
		return Runs.CanSubmit(Route, OutReason);
	}

	switch (Command)
	{
	case EDreamCommand::Stop:
		if (Runs.GetState() == EDreamRunState::Idle)
		{
			OutReason = LOCTEXT("NoActiveRun", "No run is active");
			return false;
		}
		return true;
	// (debug) PO-06 F5: F9 needs a tab; the step keys act only in the Paused window (§8.3.5).
	case EDreamCommand::ToggleBreakpoint:
		if (!Buffer.IsValid() || !View.IsValid())
		{
			OutReason = LOCTEXT("Debug.NoTab", "No tab is open");
			return false;
		}
		return true;
	case EDreamCommand::DebugStepOver:
	case EDreamCommand::DebugStepInto:
	case EDreamCommand::DebugStepOut:
		OutReason = LOCTEXT("Debug.NotPaused", "No Debug run is paused (F5 starts one)");
		return false;
	// end debug
	case EDreamCommand::UndoLastRun:
	{
		FDreamUndoPreview Preview;
		return Runs.CanUndoRun(0, OutReason, Preview);
	}
	case EDreamCommand::ResetSession:
		if (Runs.IsRunActive() || Runs.GetState() != EDreamRunState::Idle)
		{
			OutReason = LOCTEXT("RunInProgress2", "Run in progress");
			return false;
		}
		return true;
	case EDreamCommand::RunInSessionOrIsolated:
	case EDreamCommand::ToggleOneUndoStepPerRun:
	case EDreamCommand::Save:
	case EDreamCommand::SaveAs:
	case EDreamCommand::CloseTab:
	case EDreamCommand::GoToLine:
	case EDreamCommand::ZoomIn:
	case EDreamCommand::ZoomOut:
	case EDreamCommand::ZoomReset:
		if (!Buffer.IsValid())
		{
			OutReason = LOCTEXT("NoTab2", "No tab is open");
			return false;
		}
		if ((Command == EDreamCommand::Save || Command == EDreamCommand::SaveAs) && Buffer->HasPendingRecovery())
		{
			OutReason = LOCTEXT("NotRestoredSave", "Not restored yet: restore or discard the recovered changes first");
			return false;
		}
		return true;
	case EDreamCommand::ToggleComment:
		if (!Buffer.IsValid() || !View.IsValid())
		{
			OutReason = LOCTEXT("NoTab3", "No tab is open");
			return false;
		}
		if (View->IsReadOnly())
		{
			OutReason = Runs.IsRunActive() ? LOCTEXT("RunInProgress3", "Run in progress") : Buffer->GetReadOnlyReason();
			return false;
		}
		return true;
	case EDreamCommand::ReopenClosedTab:
		return true;   // "nothing to reopen" is reported on execution
	// codeview: the text commands belong to the active code view (FDreamCommands::IsCodeViewCommand); a key reaches the tab
	// itself with them only from the tab's chrome or a menu.
	case EDreamCommand::Undo:
	case EDreamCommand::Redo:
	case EDreamCommand::DuplicateLine:
	case EDreamCommand::Find:
	case EDreamCommand::Replace:
	case EDreamCommand::FindNext:
	case EDreamCommand::FindPrevious:
		if (!View.IsValid())
		{
			OutReason = LOCTEXT("NoTab4", "No tab is open");
			return false;
		}
		return View->CanExecuteDreamCommand(Command, OutReason);
	case EDreamCommand::ToggleTabMovesFocus:
		return true;
	// end codeview
	case EDreamCommand::Complete:   // (chrome) Edit ▸ Complete: the active view's completion list
		if (!View.IsValid() || !View->GetCompletion().IsValid())
		{
			OutReason = LOCTEXT("Chrome.NoTabComplete", "No tab is open");
			return false;
		}
		return true;
	case EDreamCommand::NextTab:
	case EDreamCommand::PreviousTab:
		if (Impl->TabOrder.Num() < 2)
		{
			OutReason = LOCTEXT("OneTab", "Only one tab is open");
			return false;
		}
		return true;
	case EDreamCommand::OpenDream:
	case EDreamCommand::None:
	case EDreamCommand::Count:
		return Command == EDreamCommand::OpenDream;
	default:
		return true;   // ToggleEchoInput, ClearOutput, NextError, PrevError, ExpandEcho, FoldEcho, FocusRepl, MoveOutputTo*, NewTab, OpenFile
	}
}

bool SDreamTab::SupportsKeyboardFocus() const
{
	return true;    // the empty state and a click on chrome keep focus in Dream (§4.9.4, SHL-02)
}

void SDreamTab::OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath, const FFocusEvent& InFocusEvent)
{
	SCompoundWidget::OnFocusChanging(PreviousFocusPath, NewWidgetPath, InFocusEvent);
	HandleScratchFocusChange(PreviousFocusPath, NewWidgetPath);   // (scratch, PO-05) a scratch file saves on focus loss
	// Slate calls this on the widgets of both paths; only a focus home inside the tab, never the tab itself, is remembered:
	// a code view (the script, the REPL prompt) or a view's list. A tab-row segment, a button or a link that took focus on
	// its click is not, so the next click on chrome does not hand focus back to it, where typed keys go nowhere (P4-shl-02).
	if (!NewWidgetPath.IsValid() || !NewWidgetPath.ContainsWidget(this) || NewWidgetPath.GetLastWidget() == AsShared())
	{
		return;
	}
	bool bHome = OutputArea.IsValid() && OutputArea->IsFocusHome(NewWidgetPath);
	for (const TPair<FGuid, TSharedRef<SDreamCodeView>>& View : Impl->Views)
	{
		bHome = bHome || NewWidgetPath.ContainsWidget(&View.Value.Get());
	}
	if (bHome)
	{
		Impl->LastFocusedInside = NewWidgetPath.GetLastWidget();
		// (chrome) Focus REPL shows a hidden prompt until another focus home takes focus (an empty prompt hides again).
		if (OutputArea.IsValid() && !OutputArea->IsPromptPath(NewWidgetPath))
		{
			OutputArea->EndPromptReveal(/*bEvenIfTyped*/ false);
		}
	}
}

FReply SDreamTab::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	// A click on non-focusable chrome (a tab label, the strip's x, the toolbar, a banner, the splitter) or on the dock tab's
	// label gives focus to the tab itself. It goes back to the last focus home inside the tab (OnFocusChanging; never a
	// segment, button or link, P4-shl-02), so the REPL prompt keeps its caret when the user clears output between commands
	// (P3-shl-01), while that widget is still shown (not a closed tab's text, not the prompt the Problems tab hides); else
	// on to the active code view, so typed text lands there (P2-shl-02, A2-03). The tab keeps focus only in the empty
	// state, where it routes keys.
	if (const TSharedPtr<SWidget> Last = Impl->LastFocusedInside.Pin())
	{
		FWidgetPath Path;
		if (Last != AsShared() && Last->SupportsKeyboardFocus() && FSlateApplication::Get().FindPathToWidget(Last.ToSharedRef(), Path)
			&& Path.ContainsWidget(this))
		{
			return FReply::Handled().SetUserFocus(Last.ToSharedRef(), InFocusEvent.GetCause());
		}
	}
	if (const TSharedPtr<SDreamCodeView> View = GetActiveCodeView())
	{
		return FReply::Handled().SetUserFocus(View.ToSharedRef(), InFocusEvent.GetCause());
	}
	return SCompoundWidget::OnFocusReceived(MyGeometry, InFocusEvent);
}

FReply SDreamTab::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// Keys that reached the tab itself: it has focus (no tab open, a click on the toolbar or a banner), or a child left them
	// unhandled. Bound chords run, the consume list holds, pass-through chords bubble on to Unreal (§4.9.4, D8). A child that
	// routes keys has already offered them to the same keymap with at least these contexts, so nothing runs twice.
	uint32 Context = dream::Ctx_Focused;
	if (Module().GetRunService().IsRunActive())
	{
		Context |= dream::Ctx_RunActive;
	}
	const FReply Reply = FDreamKeyRouter::Get().RoutePre(InKeyEvent, Context, *this);
	if (Reply.IsEventHandled())
	{
		return Reply;
	}
	return FDreamKeyRouter::Get().RouteBubble(InKeyEvent, Context);
}

FReply SDreamTab::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	return FDreamKeyRouter::Get().ShouldConsumeKeyUp(InKeyEvent) ? FReply::Handled() : FReply::Unhandled();
}

void SDreamTab::ShowCommandRefusal(EDreamCommand Command, const FText& Reason)
{
	ShowStatusMessage(Reason.IsEmpty() ? LOCTEXT("CannotNow", "Not available now") : Reason);
}

void SDreamTab::ExecuteDreamCommand(EDreamCommand Command)
{
	FText Reason;
	if (!CanExecuteDreamCommand(Command, Reason))
	{
		ShowCommandRefusal(Command, Reason);
		return;
	}

	FDreamEditorModule& M = Module();
	TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
	TSharedPtr<SDreamCodeView> View = GetActiveCodeView();
	switch (Command)
	{
	case EDreamCommand::RunSelectionOrBuffer:
	case EDreamCommand::RunBuffer:
	case EDreamCommand::RunLine:
	case EDreamCommand::RunLineAdvance:
	case EDreamCommand::RunFile:
	case EDreamCommand::DebugRun:        // (debug)
		SubmitBufferRun(Command);
		break;
	case EDreamCommand::ToggleBreakpoint:    // (debug) PO-06 F5: F9 on the caret line (DBG-01)
		FDreamDebugger::Get().ToggleBreakpoint(*Buffer, View->GetCaretLine());
		break;
	case EDreamCommand::RunInSessionOrIsolated:
	{
		EDreamNamespaceMode& Mode = Buffer->GetViewState().Namespace;
		Mode = Mode == EDreamNamespaceMode::Isolated ? EDreamNamespaceMode::Session : EDreamNamespaceMode::Isolated;
		M.GetJournal().OnViewStateChanged(Buffer->GetId());
		SaveSessionState(false);
		break;
	}
	case EDreamCommand::Stop:
		M.GetRunService().RequestStopActive(EDreamStopSource::ShiftF5);
		break;
	case EDreamCommand::UndoLastRun:
		UndoLastRunFromToolbar();
		break;
	case EDreamCommand::ToggleOneUndoStepPerRun:
	{
		// (run group, B40) The click flips what the toggle shows; from the mixed state (untouched, route settings differ) it
		// turns the toggle on. Set from now on: it decides every tab run.
		const TOptional<bool> Shown = FDreamRunService::TabToggleShown(Buffer->GetViewState());
		Buffer->GetViewState().bOneUndoStepPerRun = Shown.IsSet() ? !Shown.GetValue() : true;
		Buffer->GetViewState().bOneUndoStepPerRunSet = true;
		M.GetJournal().OnViewStateChanged(Buffer->GetId());
		SaveSessionState(false);
		break;
	}
	case EDreamCommand::ResetSession:
		if (!M.GetPython().RequestResetSession(Reason))
		{
			ShowStatusMessage(Reason);
		}
		break;
	case EDreamCommand::ToggleEchoInput:
	{
		FDreamOutputVisibility Visibility = M.GetOutputStore().GetVisibility();
		Visibility.bEcho = !Visibility.bEcho;
		M.GetOutputStore().SetVisibility(Visibility);
		UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
		Settings->bEchoInput = Visibility.bEcho;
		UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bEchoInput));   // (chrome) this key only
		break;
	}
	case EDreamCommand::ClearOutput:
		M.GetOutputStore().Clear();
		break;
	case EDreamCommand::NextError:
	case EDreamCommand::PrevError:
		// U13 (CR-25): over the view the user is looking at; the view opens each target through OpenLocation.
		if (TSharedPtr<SDreamOutputView> Output = GetActiveOutputView())
		{
			FText Status;
			Output->WalkErrors(Command == EDreamCommand::NextError ? 1 : -1, Status);
			if (!Status.IsEmpty())
			{
				ShowStatusMessage(Status);
			}
		}
		break;
	case EDreamCommand::ExpandEcho:
	case EDreamCommand::FoldEcho:
		// OUT-12 folding (§6.4): the statement at the view's caret, or every statement of its selected runs.
		if (TSharedPtr<SDreamOutputView> Output = GetActiveOutputView())
		{
			Output->ExpandEcho(Command == EDreamCommand::ExpandEcho);
		}
		break;
	case EDreamCommand::FocusRepl:
		if (OutputArea.IsValid())
		{
			OutputArea->FocusRepl();
		}
		break;
	case EDreamCommand::MoveOutputToTop:
		SetOutputPosition(EDreamOutputPosition::Top);
		break;
	case EDreamCommand::MoveOutputToBottom:
		SetOutputPosition(EDreamOutputPosition::Bottom);
		break;
	case EDreamCommand::NewTab:
		if (TSharedPtr<SDreamCodeView> NewView = FindCodeView(NewScratchTab()->GetId()))
		{
			NewView->FocusEditor();
		}
		break;
	case EDreamCommand::OpenFile:
		OpenFileDialog();
		break;
	case EDreamCommand::Save:
		SaveActive(!Buffer->HasPath());
		break;
	case EDreamCommand::SaveAs:
		SaveActive(true);
		break;
	case EDreamCommand::CloseTab:
		CloseBuffer(Buffer->GetId());
		break;
	case EDreamCommand::ReopenClosedTab:
		if (TSharedPtr<FDreamBuffer> Reopened = M.GetBufferManager().ReopenClosed())
		{
			ActivateBuffer(Reopened->GetId());
			if (TSharedPtr<SDreamCodeView> ReopenedView = GetActiveCodeView())
			{
				ReopenedView->FocusEditor();
			}
		}
		else
		{
			ShowStatusMessage(LOCTEXT("NothingToReopen", "No closed tab to reopen"));
		}
		break;
	case EDreamCommand::NextTab:
		CycleTab(1);
		break;
	case EDreamCommand::PreviousTab:
		CycleTab(-1);
		break;
	case EDreamCommand::GoToLine:
		OpenGoToLine();
		break;
	case EDreamCommand::ToggleComment:
		ToggleCommentInActiveView();
		break;
	case EDreamCommand::ZoomIn:
	case EDreamCommand::ZoomOut:
	case EDreamCommand::ZoomReset:
		if (View.IsValid())
		{
			View->Zoom(Command == EDreamCommand::ZoomIn ? 1 : Command == EDreamCommand::ZoomOut ? -1 : 0);
			SaveSessionState(false);
		}
		break;
	// codeview: the active code view executes the text commands (Undo, Redo, DuplicateLine, Find, ...).
	case EDreamCommand::Undo:
	case EDreamCommand::Redo:
	case EDreamCommand::DuplicateLine:
	case EDreamCommand::Find:
	case EDreamCommand::Replace:
	case EDreamCommand::FindNext:
	case EDreamCommand::FindPrevious:
	case EDreamCommand::ToggleTabMovesFocus:
		if (View.IsValid())
		{
			View->ExecuteDreamCommand(Command);
		}
		else if (Command == EDreamCommand::ToggleTabMovesFocus)
		{
			FDreamKeyRouter::Get().SetTabMovesFocus(!FDreamKeyRouter::Get().IsTabMovesFocus());
		}
		break;
	// end codeview
	// lang (B56): F8 / Shift+F8 walk Problems (U13); Ctrl+. offers the quick fixes on the caret line (U22, §7.8.3).
	case EDreamCommand::NextProblem:
	case EDreamCommand::PrevProblem:
		M.GetLanguageServices().WalkProblems(*this, Command == EDreamCommand::NextProblem ? 1 : -1);
		break;
	case EDreamCommand::QuickFix:
		M.GetLanguageServices().ShowQuickFixes(*this);
		break;
	case EDreamCommand::NextProblemInFile:     // (lang) PO-06 F4: F2 / Shift+F2 and the inspection widget
	case EDreamCommand::PrevProblemInFile:
		M.GetLanguageServices().WalkProblemsInBuffer(*this, Command == EDreamCommand::NextProblemInFile ? 1 : -1);
		break;
	// end lang
	// (chrome) Edit ▸ Complete: what Ctrl+Space does in the code view (§7.6.2); the chord itself goes to the controller.
	case EDreamCommand::Complete:
		if (View.IsValid() && View->GetCompletion().IsValid())
		{
			View->FocusEditor();
			View->GetCompletion()->Trigger(/*bExplicit*/ true);
		}
		break;
	// (chrome) PO-04: the View toggles (SDreamTabToolbar.cpp).
	case EDreamCommand::ToggleReplTab:
	case EDreamCommand::ToggleProblemsTab:
	case EDreamCommand::ToggleReplPrompt:
	case EDreamCommand::ToggleRunHeaders:
	case EDreamCommand::ToggleStatusLine:
	case EDreamCommand::ToggleLineNumbers:
	case EDreamCommand::ToggleProjectPanel:     // (sidebar) PO-05: View ▸ Project, View ▸ Structure
	case EDreamCommand::ToggleStructurePanel:   // (sidebar)
		ToggleViewSetting(Command);
		break;
	case EDreamCommand::TogglePluginPythonFolders:   // (sidebar) PO-05-A1: the tree follows through OnRootsChanged
		FDreamProjectFiles::SetAddPluginFolders(!FDreamProjectFiles::IsAddPluginFolders());
		break;
	case EDreamCommand::FocusProject:           // (sidebar) Esc in the section returns focus to the code view
	case EDreamCommand::FocusStructure:
		if (Impl->SideBar.IsValid())
		{
			if (Command == EDreamCommand::FocusProject)
			{
				Impl->SideBar->FocusProject();
			}
			else
			{
				Impl->SideBar->FocusStructure();
			}
		}
		break;
	case EDreamCommand::OpenDream:
		if (View.IsValid())
		{
			View->FocusEditor();
		}
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Runs (§4.5, §5.2.1, RT-14, RUN-01, RUN-02, RUN-26)
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::SubmitBufferRun(EDreamCommand Command)
{
	using namespace DreamTabLocal;
	TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
	TSharedPtr<SDreamCodeView> View = GetActiveCodeView();
	if (!Buffer.IsValid() || !View.IsValid())
	{
		return;
	}
	FDreamEditorModule& M = Module();
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::PyLexer& Lexer = Buffer->GetLexer();
	const FDreamTextSelection Selection = View->GetSelection();
	const bool bHasSelection = !Selection.IsEmpty();
	int32 CaretLine = 0;
	int32 CaretColumn = 0;
	View->GetCaret(CaretLine, CaretColumn);

	FDreamRunRequest Request;
	Request.BufferId = Buffer->GetId();
	Request.TabTitle = Buffer->GetDisplayName();
	Request.FilePath = Buffer->GetPath();
	Request.bDirty = Buffer->IsDirty();
	Request.SourceSnapshot = Snapshot;
	Request.Filename = M.GetBufferManager().GetCompileFilename(*Buffer);
	Request.Namespace = Buffer->GetViewState().Namespace;
	// U6 "Run in / without transaction" is a one-shot override; otherwise the run service combines the route default, the
	// user setting and this tab's One undo step per run toggle (§5.9.1).
	Request.TransactionOverride = Impl->TransactionOverride;
	Impl->TransactionOverride.Reset();

	TOptional<dream::LineRange> Ran;
	const bool bUseSelection = bHasSelection && (Command == EDreamCommand::RunSelectionOrBuffer || Command == EDreamCommand::RunLineAdvance
		|| Command == EDreamCommand::DebugRun);   // (debug) F5 debug-runs the selection or the buffer (U21)

	if (Command == EDreamCommand::RunFile)
	{
		Request.Route = EDreamRunRoute::RunFile;
		Request.Namespace = EDreamNamespaceMode::Isolated;   // §5.2.1: Run File is Isolated, one exec, transaction off
		Request.Text = DreamText::ToFString(Snapshot.Text());
		Request.FirstLine = 1;
	}
	else if (bUseSelection)
	{
		// (run) PO-06 F2 (amends RUN-01): exactly the selected characters run, not the whole lines they touch. A fragment
		// inside a line starts at its column; a start preceded by blanks only keeps the indentation (the RT-14 wrapper
		// removes it at compile time); an end at column 0 of a line ends on the line before (§4.5). DreamRunSourceText.h.
		const dream::Offset AnchorOffset = Snapshot.LineStart(Selection.AnchorLine) + Selection.AnchorColumn;
		const dream::Offset CaretOffset = Snapshot.LineStart(Selection.CaretLine) + Selection.CaretColumn;
		DreamRunSource::FSelectionText Exact = DreamRunSource::ExactSelection(Snapshot, AnchorOffset, CaretOffset);
		Request.Route = EDreamRunRoute::Selection;
		Request.Text = MoveTemp(Exact.Text);
		Request.FirstLine = Exact.FirstLine;
		Request.SelectionStartColumn = Exact.StartColumn;
		Request.SelectionEndColumn = Exact.EndColumn;   // the echo stops there (§6.2)
		Ran = dream::LineRange{ Exact.FirstLine - 1, Exact.LastLine - 1 };
	}
	else if (Command == EDreamCommand::RunLine || Command == EDreamCommand::RunLineAdvance)
	{
		const std::optional<dream::LineRange> Extent = dream::StatementExtent(Snapshot, Lexer, CaretLine);
		if (!Extent.has_value())
		{
			if (Command == EDreamCommand::RunLine)
			{
				ShowStatusMessage(LOCTEXT("NoStatement", "No statement on this line"));
				return;
			}
			// §4.5 rule 1: Run line and advance on a blank or comment line moves to the next statement without a run.
			const std::optional<dream::LineNo> Next = dream::NextStatementStartFrom(Snapshot, Lexer, CaretLine);
			if (Next.has_value())
			{
				View->SetCaret(*Next, dream::FirstNonBlankColumn(Snapshot, *Next));
				View->ScrollToLine(*Next, 3);
			}
			else
			{
				ShowStatusMessage(LOCTEXT("EndOfBuffer", "End of buffer"));
			}
			return;
		}
		Request.Route = EDreamRunRoute::Line;
		Request.Text = JoinLines(Snapshot, Extent->First, Extent->Last);
		Request.FirstLine = Extent->First + 1;
		Ran = *Extent;
	}
	else
	{
		Request.Route = EDreamRunRoute::Buffer;
		Request.Text = DreamText::ToFString(Snapshot.Text());
		Request.FirstLine = 1;
	}

	Request.bDebug = Command == EDreamCommand::DebugRun;     // (debug) PO-06 F5
	const FDreamSubmitResult Result = M.GetRunService().Submit(MoveTemp(Request));
	if (!Result.bAccepted)
	{
		ShowStatusMessage(Result.Reason);   // a refused request leaves the caret and the selection as they were (§4.5)
		return;
	}

	if (Command == EDreamCommand::RunLineAdvance && Ran.IsSet())
	{
		// §4.5 "When the caret moves": at acceptance, before the run executes, so the outcome never changes where it lands.
		const std::optional<dream::LineNo> Next = dream::NextStatementLine(Snapshot, Lexer, Ran.GetValue());
		if (Next.has_value())
		{
			View->SetCaret(*Next, dream::FirstNonBlankColumn(Snapshot, *Next));
			View->ScrollToLine(*Next, 3);
		}
		else
		{
			View->SetCaret(CaretLine, CaretColumn);   // clears a selection; the caret stays
			ShowStatusMessage(LOCTEXT("EndOfBuffer2", "End of buffer"));
		}
		// (buffers) The first RunLineAdvance hides the RUN-26 hint chip for good (§4.9.4, SDreamTabFiles.cpp).
		DismissRunLineHint();
	}
}

void SDreamTab::HandleRunFinished(const FDreamRunRecord& Record)
{
	// A SyntaxError run ends not executed with the error's location: the caret moves there (§5.2.4 step 5).
	if (Record.Outcome != EDreamRunOutcome::NotExecuted || Record.Location.Line <= 0 || !Record.BufferId.IsValid())
	{
		return;
	}
	TSharedPtr<SDreamCodeView> View = FindCodeView(Record.BufferId);
	TSharedPtr<FDreamBuffer> Buffer = Module().GetBufferManager().FindById(Record.BufferId);
	if (!View.IsValid() || !Buffer.IsValid())
	{
		return;
	}
	const FString Filename = Module().GetBufferManager().GetCompileFilename(*Buffer);
	if (!Record.Location.File.IsEmpty() && Record.Location.File != Filename && Record.Location.File != Record.Filename)
	{
		return;
	}
	const dream::LineNo Mapped = Buffer->GetModel().MapLine(Record.SourceVersion, Record.Location.Line - 1);
	const int32 Line = FMath::Clamp<int32>(Mapped == dream::InvalidLine ? Record.Location.Line - 1 : Mapped, 0, FMath::Max(0, View->GetLineCount() - 1));
	// RUN-16: CPython's reported position (for "'(' was never closed", the bracket), clamped to the line as it is now.
	const int32 Column = FMath::Clamp(Record.Location.Column, 0, Buffer->Snapshot().LineLength(Line));
	View->SetCaret(Line, Column);
	View->ScrollToLine(Line, 3);
}

void SDreamTab::UndoLastRunFromToolbar()
{
	FDreamRunService& Runs = Module().GetRunService();
	FText Reason;
	FDreamUndoPreview Preview;
	if (!Runs.CanUndoRun(0, Reason, Preview))
	{
		ShowStatusMessage(Reason);
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Preview.bPartial && Now > Impl->UndoConfirmUntil)
	{
		// §5.9.5 check 4: a partial record needs a confirm; the second press within 5 s is it (inline, never modal, U26).
		Impl->UndoConfirmUntil = Now + 5.0;
		ShowStatusMessage(FText::Format(LOCTEXT("UndoPartialConfirm", "Run #{0} is partly undoable. Undo the recorded part?"),
			FText::AsNumber(Preview.RunNumber, &FNumberFormattingOptions::DefaultNoGrouping())));
		return;
	}
	Impl->UndoConfirmUntil = 0.0;
	if (Runs.UndoRun(0, Preview.bPartial, Reason))
	{
		ShowStatusMessage(FText::Format(LOCTEXT("UndidRun", "Undid {0}"), Preview.Title));
	}
	else
	{
		ShowStatusMessage(Reason);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Editing helpers
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::ToggleCommentInActiveView()
{
	TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
	TSharedPtr<SDreamCodeView> View = GetActiveCodeView();
	if (!Buffer.IsValid() || !View.IsValid())
	{
		return;
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	const FDreamTextSelection Selection = View->GetSelection();
	const dream::Offset A = Snapshot.LineStart(Selection.AnchorLine) + Selection.AnchorColumn;
	const dream::Offset B = Snapshot.LineStart(Selection.CaretLine) + Selection.CaretColumn;
	const dream::LineRange Lines = dream::SelectionLines(Snapshot, FMath::Min(A, B), FMath::Max(A, B));
	const dream::EditBatch Batch = dream::ToggleComment(Snapshot, Buffer->GetLexer(), Lines);
	if (Batch.Ranges.empty())
	{
		return;
	}
	// Apply the batch to a copy, then hand the changed lines to the stock widget as one undoable edit (EDT-04).
	dream::TextBuffer Copy(Snapshot.Text());
	Copy.Apply(Batch);
	const dream::TextSnapshot After = Copy.Snapshot();
	const int32 LastAfter = FMath::Min<int32>(Lines.Last, After.LineCount() - 1);
	const FString NewText = DreamTabLocal::JoinLines(After, Lines.First, LastAfter);
	View->ReplaceLines(Lines.First, Lines.Last, NewText);
	FDreamTextSelection Reselect;
	Reselect.AnchorLine = Lines.First;
	Reselect.CaretLine = LastAfter;
	Reselect.CaretColumn = After.LineLength(LastAfter);
	View->SetSelection(Reselect);
}

void SDreamTab::OpenGoToLine()
{
	// codeview: Go to line (NAV-03) is the code view's own input box over its text (CodeView/SDreamGoToLine.h).
	if (TSharedPtr<SDreamCodeView> View = GetActiveCodeView())
	{
		View->OpenGoToLine();
	}
}

void SDreamTab::OpenFileDialog()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (Desktop == nullptr)
	{
		return;
	}
	TArray<FString> Files;
	const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
	if (Desktop->OpenFileDialog(Parent, LOCTEXT("OpenTitle", "Open Python File").ToString(), FPaths::ProjectDir(), FString(),
		TEXT("Python files (*.py)|*.py|All files (*.*)|*.*"), EFileDialogFlags::Multiple, Files))
	{
		for (const FString& File : Files)
		{
			OpenLocation(File, 0, 0);
		}
	}
}

void SDreamTab::SaveActive(bool bSaveAs)
{
	TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
	if (!Buffer.IsValid())
	{
		return;
	}
	FString Target = Buffer->GetPath();
	if (bSaveAs || Target.IsEmpty())
	{
		IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
		TArray<FString> Files;
		const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
		const FString DefaultName = Buffer->HasPath() ? FPaths::GetCleanFilename(Buffer->GetPath()) : Buffer->GetDisplayName() + TEXT(".py");
		const FString DefaultDir = Buffer->HasPath() ? FPaths::GetPath(Buffer->GetPath()) : FPaths::ProjectDir();
		if (Desktop == nullptr || !Desktop->SaveFileDialog(Parent, LOCTEXT("SaveAsTitle", "Save Python File").ToString(), DefaultDir,
			DefaultName, TEXT("Python files (*.py)|*.py|All files (*.*)|*.*"), EFileDialogFlags::None, Files) || Files.Num() == 0)
		{
			return;
		}
		Target = FPaths::ConvertRelativePathToFull(Files[0]);
	}

	TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	const FString Name = FPaths::GetCleanFilename(Target);
	const FDreamOnSaved OnSaved = FDreamOnSaved::CreateLambda([WeakTab, Name](bool bOk, const FText& Error)
	{
		if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
		{
			Tab->ShowStatusMessage(bOk ? FText::Format(LOCTEXT("Saved", "Saved {0}"), FText::FromString(Name)) : Error);
			Tab->RefreshTabStrip();
			Tab->SaveSessionState(false);
		}
	});
	if (bSaveAs || !Buffer->HasPath())
	{
		Module().GetBufferManager().SaveAsAsync(Buffer->GetId(), Target, OnSaved);
	}
	else
	{
		Module().GetBufferManager().SaveAsync(Buffer->GetId(), OnSaved);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Session restore (§10.3) and recovery (§10.7)
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::RestoreSession()
{
	if (!Impl->InstancesHandle.IsValid())
	{
		// (instances) Orphans, the secondary's banner and the moved-project offer follow FDreamInstances (§10.5, §10.1).
		Impl->InstancesHandle = Module().GetInstances().OnChanged().AddSP(this, &SDreamTab::HandleInstancesChanged);
	}
	if (DreamTabLocal::GSessionRestored || Impl->bRestoreRequested)
	{
		if (Impl->TabOrder.Num() == 0)
		{
			NewScratchTab();
		}
		return;
	}
	Impl->bRestoreRequested = true;

	// (persist) Safe mode (§10.8): -DreamSafeMode, Shift held while the tab opens, or the restore guard after two crashes
	// during restore. Otherwise the guard counts this restore, flushed before any restore work; the scan runs either way.
	const bool bShiftHeld = FSlateApplication::IsInitialized() && FSlateApplication::Get().GetModifierKeys().IsShiftDown();
	EDreamSafeModeReason SafeReason = FParse::Param(FCommandLine::Get(), TEXT("DreamSafeMode")) ? EDreamSafeModeReason::Switch
		: (bShiftHeld ? EDreamSafeModeReason::Shift : EDreamSafeModeReason::None);
	FDreamRestoreGuard& Guard = Module().GetRestoreGuard();
	if (SafeReason == EDreamSafeModeReason::None && Module().GetJournal().IsOpen()
		&& Guard.BeginRestore(Module().GetJournal().GetStateDir(), FDreamRestoreGuard::IsAutomationSession()))   // (instances) <sec>/state for a secondary
	{
		SafeReason = EDreamSafeModeReason::Guard;
	}
	Guard.EnterSafeMode(SafeReason);
	Impl->bSafeMode = SafeReason != EDreamSafeModeReason::None;

	TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	Module().GetJournal().RequestRecoveryScan(FDreamOnRecoveryScan::CreateLambda([WeakTab](FDreamRecoveredSession& Session) -> bool
	{
		TSharedPtr<SDreamTab> Tab = WeakTab.Pin();
		if (!Tab.IsValid() || !FDreamEditorModule::IsAvailable())
		{
			return false;   // the journal keeps the result for the next tab, so a fast close never loses the restore (PER-02)
		}
		if (Tab->Impl->bSafeMode)
		{
			Tab->ApplySafeMode(MoveTemp(Session));
			return true;
		}
		Tab->ApplyRestore(MoveTemp(Session));
		return true;
	}));
}

void SDreamTab::ApplySafeMode(FDreamRecoveredSession&& Session)
{
	FDreamEditorModule& M = Module();
	FDreamBufferManager& Buffers = M.GetBufferManager();
	// The session counts as handled (a re-spawned tab adopts the open buffers), but the ini's tab list of the skipped
	// session is kept for the next normal start until Restore tabs applies it (SHL-01).
	DreamTabLocal::GSessionRestored = true;
	DreamTabLocal::GSkippedIniTabs = UDreamPerProjectUserSettings::Get()->OpenTabs;
	DreamTabLocal::bGSkippedSessionPending = true;
	if (Impl->TabOrder.Num() == 0)
	{
		NewScratchTab();
	}
	// The unsent REPL prompt is text, not a tab: it comes back in safe mode too, so the journal keeps it (ADD3-01).
	if (Session.ReplInputText.IsSet() && Buffers.GetReplInputBuffer()->GetModel().Length() == 0)
	{
		Buffers.GetReplInputBuffer()->ResetText(Session.ReplInputText.GetValue(), dream::EditOrigin::Load);
	}
	// §10.7.1, §10.8: safe mode keeps the journal and lowers no safety. Every buffer of the skipped session is carried in
	// this session's generation and its counted recoveries stay pending, so ignoring the banner loses nothing, a crash in
	// safe mode loses nothing, and Restore tabs later still finds them (PER-01, SHL-01, ADD3-02).
	TArray<TSharedRef<FDreamBuffer>> Live = Buffers.GetOpenBuffers();
	Live.Add(Buffers.GetReplInputBuffer());
	M.GetJournal().Reconcile(Live, &Session);
	HandOrphansToInstances(Session);   // (instances) §10.5: the recovery list, orphans included, still shows in safe mode
	Impl->SafeModeSession = MoveTemp(Session);
	RebuildBanners();
}

bool SDreamTab::IsApplyingRestoreForTest()
{
	return DreamTabLocal::bGApplyingRestore;
}

void SDreamTab::ApplyRestore(FDreamRecoveredSession&& Session)
{
	using namespace DreamTabLocal;
	TGuardValue<bool> ApplyingRestore(bGApplyingRestore, true);   // (tests) see IsApplyingRestoreForTest
	GSessionRestored = true;
	bGSkippedSessionPending = false;
	// A tab opened before the scan arrived (new_tab, Dream.Open, a click) stays active: the latest action wins.
	const FGuid OpenedBeforeRestore = Impl->ActiveId;
	FDreamEditorModule& M = Module();
	FDreamBufferManager& Buffers = M.GetBufferManager();
	const UDreamPerProjectUserSettings* Ini = UDreamPerProjectUserSettings::Get();

	TMap<FGuid, const FDreamTabState*> IniById;
	TArray<FGuid> Order;
	for (const FDreamTabState& Tab : Ini->OpenTabs)
	{
		FGuid Id;
		if (FGuid::Parse(Tab.BufferId, Id))
		{
			IniById.Add(Id, &Tab);
		}
	}
	// After a crash the journal SESSION wins (it is newer); after a clean exit the ini's tab list wins (§10.3 reconcile).
	if (!Session.bPreviousExitClean && Session.SessionTabOrder.Num() > 0)
	{
		Order = Session.SessionTabOrder;
	}
	else
	{
		// (instances) The ini's tab list is the primary's session: a secondary starts with its own (§10.5, slice B choice).
		const bool bSecondary = M.GetJournal().IsOpen() && M.GetJournal().GetRole() == EDreamInstanceRole::Secondary;
		for (const FDreamTabState& Tab : bSecondary ? TArray<FDreamTabState>() : Ini->OpenTabs)
		{
			FGuid Id;
			if (FGuid::Parse(Tab.BufferId, Id))
			{
				Order.AddUnique(Id);
			}
		}
	}
	for (const FDreamRecoveredBuffer& Recovered : Session.Buffers)
	{
		Order.AddUnique(Recovered.Id);   // journaled buffers the (debounced) tab list missed
	}

	TMap<FGuid, const FDreamRecoveredBuffer*> RecoveredById;
	for (const FDreamRecoveredBuffer& Recovered : Session.Buffers)
	{
		RecoveredById.Add(Recovered.Id, &Recovered);
	}
	// A tab whose last journal record is BUF_CLOSE stays closed, whatever a debounced ini tab list says (VIS-07).
	Order.RemoveAll([&Session, &RecoveredById](const FGuid& Id) { return Session.ClosedIds.Contains(Id) && !RecoveredById.Contains(Id); });

	for (const FGuid& Id : Order)
	{
		if (Buffers.FindById(Id).IsValid())
		{
			continue;
		}
		const FDreamTabState* const* IniTab = IniById.Find(Id);
		const FDreamRecoveredBuffer* const* RecoveredPtr = RecoveredById.Find(Id);
		const FDreamRecoveredBuffer* Rec = RecoveredPtr ? *RecoveredPtr : nullptr;
		const EDreamBufferKind Kind = Rec ? Rec->Kind : (IniTab ? FromTabKind((*IniTab)->Kind) : EDreamBufferKind::Scratch);
		const FString Path = Rec ? Rec->Path : (IniTab ? (*IniTab)->Path : FString());
		FString Display = Rec ? Rec->DisplayName : (IniTab ? (*IniTab)->DisplayName : FString());
		if (Display.IsEmpty() && IniTab)
		{
			Display = (*IniTab)->DisplayName;
		}

		TSharedPtr<FDreamBuffer> Buffer;
		FText Error;
		if (Kind == EDreamBufferKind::Scratch || Path.IsEmpty())
		{
			if (Rec == nullptr)
			{
				// A scratch tab's text lives only in the journal: without a journal record (closed, unverified, or written by
				// a session whose journal was off) there is nothing to restore, and an empty "Untitled" ghost helps nobody.
				continue;
			}
			// BUF-01: scratch tabs survive restarts and crashes with their journal text.
			if (Display.IsEmpty())
			{
				UDreamPerProjectUserSettings* Counter = UDreamPerProjectUserSettings::GetMutable();
				const int32 Number = FMath::Max(1, Counter->NextUntitledNumber);
				Counter->NextUntitledNumber = Number + 1;
				Display = FString::Printf(TEXT("Untitled-%d"), Number);   // BUF-01 names, never a bare "Untitled"
			}
			// (persist) §10.7.1: a counted scratch buffer shows its clean text, read-only under "Not restored yet", until
			// Restore, Restore as new tab or Discard; an uncounted one restores its journal text.
			const bool bPendingScratch = Rec->bCounted && Rec->CleanText.IsSet();
			Buffer = Buffers.AddRestored(Id, EDreamBufferKind::Scratch, Display, FString(), bPendingScratch ? Rec->CleanText.GetValue() : Rec->RecoveredText,
				Rec->Format, TOptional<FDreamBaseVersion>());
			M.GetJournal().SetCleanText(Id, Buffer->Snapshot());
			if (bPendingScratch)
			{
				Buffer->SetPendingRecovery(Rec->RecoveredText);
				Impl->PendingRecoveries.AddUnique(Id);
				Impl->RecoveryDetails.Add(Id, *Rec);
			}
		}
		else if (Rec && Rec->bCounted)
		{
			const FString RecoveredName = Rec->Kind == EDreamBufferKind::Recovered
				? Display : FText::Format(LOCTEXT("RecoveredName", "{0} (recovered)"), FText::FromString(Display)).ToString();
			if (Rec->bConflict || !IFileManager::Get().FileExists(*Path))
			{
				// §10.7.2: the disk version opens editable from disk; the recovered text waits in "name (recovered)".
				if (IFileManager::Get().FileExists(*Path) && !Buffers.OpenFile(Path, Error).IsValid())
				{
					UE_LOG(LogDream, Warning, TEXT("Dream Python Editor did not reopen %s: %s; its recovered text is kept"), *Path, *Error.ToString());
				}
				Buffer = Buffers.AddRestored(Id, EDreamBufferKind::Recovered, RecoveredName, Path, Rec->RecoveredText, Rec->Format, Rec->Base);
				Buffer->SetCleanText(FString());
			}
			else
			{
				Buffer = Buffers.OpenFile(Path, Error, Id);   // shows the disk text, read-only until resolved (PER-03)
				if (!Buffer.IsValid())
				{
					// The disk file is refused (over 1 MB in slice A, binary, unreadable): the recovered text still waits, in
					// "name (recovered)", so this session's generation carries it (§10.7.2).
					UE_LOG(LogDream, Warning, TEXT("Dream Python Editor did not reopen %s: %s; its recovered text is kept"), *Path, *Error.ToString());
					Buffer = Buffers.AddRestored(Id, EDreamBufferKind::Recovered, RecoveredName, Path, Rec->RecoveredText, Rec->Format, Rec->Base);
					Buffer->SetCleanText(FString());
				}
			}
			if (Buffer.IsValid())
			{
				Buffer->SetPendingRecovery(Rec->RecoveredText);
				Impl->PendingRecoveries.AddUnique(Buffer->GetId());
				Impl->RecoveryDetails.Add(Buffer->GetId(), *Rec);   // (persist) the Review row
			}
		}
		else if (Rec && Rec->bDiffersFromDisk)
		{
			// A dirty file buffer at a clean exit: the journal supplies its text, the disk stays its clean text (§10.3).
			Buffer = Buffers.OpenFile(Path, Error, Id);
			if (Buffer.IsValid())
			{
				const FString DiskText = Buffer->GetTextAsString();
				Buffer->ResetText(Rec->RecoveredText, dream::EditOrigin::Load);
				Buffer->SetCleanText(DiskText);
				if (Rec->bConflict && Buffer->IsScratchFile())
				{
					Buffers.HoldRestoredDiskVersion(Buffer->GetId(), DiskText, Rec->Base);   // (scratch, p1-m2, p2-M1) the newer disk text waits behind its bar
				}
			}
			else
			{
				Buffer = Buffers.AddRestored(Id, Kind, Display, Path, Rec->RecoveredText, Rec->Format, Rec->Base);
				Buffer->SetCleanText(FString());
			}
		}
		else
		{
			Buffer = Buffers.OpenFile(Path, Error, Id);
			if (!Buffer.IsValid())
			{
				UE_LOG(LogDream, Warning, TEXT("Dream Python Editor did not reopen %s: %s"), *Path, *Error.ToString());
			}
		}
		if (!Buffer.IsValid())
		{
			continue;
		}

		// §10.3: after a crash the journal SESSION is newer than the debounced ini, for the view state as for the tab order
		// (ADD3-03); after a clean exit the ini wins. Each falls back to the other when it has no entry for the tab.
		FDreamBufferViewState& View = Buffer->GetViewState();
		const FDreamBufferViewState* Journaled = Session.SessionViewStates.Find(Id);
		const bool bPreferJournal = !Session.bPreviousExitClean && Journaled != nullptr;
		if (IniTab && !bPreferJournal)
		{
			View.CaretLine = (*IniTab)->CaretLine;
			View.CaretColumn = (*IniTab)->CaretColumn;
			View.FirstVisibleLine = (*IniTab)->FirstVisibleLine;
			View.bOneUndoStepPerRun = (*IniTab)->bOneUndoStepPerRun;
			// (run group, B40) A toggle the user set stays set, from the ini's own flag only (review r1: inferring "set" from a
			// value that differs from the seed misread every tab after the buffer-run setting changed). An untouched toggle
			// takes the current seed again (A56, P2-run-02).
			View.bOneUndoStepPerRunSet = (*IniTab)->bOneUndoStepPerRunSet;
			if (!View.bOneUndoStepPerRunSet)
			{
				View.bOneUndoStepPerRun = UDreamUserSettings::Get()->GetTransactionDefault(EDreamRunRoute::Buffer);
			}
			View.Namespace = (*IniTab)->bIsolated ? EDreamNamespaceMode::Isolated : EDreamNamespaceMode::Session;
			View.ZoomPercent = (*IniTab)->ZoomPercent;
		}
		else if (Journaled != nullptr)
		{
			const int32 Zoom = IniTab ? (*IniTab)->ZoomPercent : View.ZoomPercent;   // the SESSION record carries no zoom
			View = *Journaled;
			View.ZoomPercent = Zoom;
			// (run group, B40) After an unclean exit the SESSION record holds the newest toggle but not whether it was set: the
			// ini's flag, or a value that differs from the buffer-run seed (a click the debounced ini save had not kept yet),
			// counts as set. Only this recovery path infers it.
			View.bOneUndoStepPerRunSet = (IniTab && (*IniTab)->bOneUndoStepPerRunSet)
				|| View.bOneUndoStepPerRun != UDreamUserSettings::Get()->GetTransactionDefault(EDreamRunRoute::Buffer);
		}
	}

	if (Session.ReplInputText.IsSet())
	{
		Buffers.GetReplInputBuffer()->ResetText(Session.ReplInputText.GetValue(), dream::EditOrigin::Load);
	}
	for (const FDreamLostRun& Lost : Session.LostRuns)
	{
		M.GetRunService().RecordLostRun(Lost);   // §10.7.4: a lost block, no run number in this session
	}

	FGuid Active = Session.SessionActiveTab;
	if (!Active.IsValid() || Session.bPreviousExitClean)
	{
		FGuid::Parse(Ini->ActiveTab, Active);
	}
	if (Impl->TabOrder.Num() == 0)
	{
		NewScratchTab();   // a fresh profile opens one scratch tab
	}
	if (OpenedBeforeRestore.IsValid() && Impl->TabOrder.Contains(OpenedBeforeRestore))
	{
		Active = OpenedBeforeRestore;
	}
	ActivateBuffer(Impl->TabOrder.Contains(Active) ? Active : Impl->TabOrder[0]);

	// The first commit of this session waits for this reconcile (§10.2.3).
	TArray<TSharedRef<FDreamBuffer>> Live = Buffers.GetOpenBuffers();
	Live.Add(Buffers.GetReplInputBuffer());
	M.GetJournal().Reconcile(Live);
	HandOrphansToInstances(Session);   // (instances) §10.5 orphans of this root, then the §10.1 cross-root scan
	MigrateUntitledToScratchFiles(Session);   // (scratch, PO-05) restored Untitled-N buffers become scratch files (SDreamTabFiles.cpp)

	// The banner (§10.7.1, §10.7.4). n counts the pending buffers, file and scratch (SHL-10); Restore all, Review and Discard
	// show only while some are pending (bShowActions).
	TArray<FText> Lines;
	int32 EarlierSessionLine = INDEX_NONE;
	const int32 Count = Impl->PendingRecoveries.Num();
	if (Count > 0)
	{
		if (!Session.bPreviousExitClean)
		{
			FText Line = FText::Format(LOCTEXT("RecoveredN", "Recovered {0} {0}|plural(one=buffer,other=buffers) from the session that ended unexpectedly at {1}."),
				FText::AsNumber(Count), FText::AsTime(Session.PreviousEndUtc, EDateTimeStyle::Short));
			if (Session.bPausedAtExit)
			{
				Line = FText::Format(LOCTEXT("RecoveredPaused", "{0} …while the debugger was paused."), Line);
			}
			Lines.Add(Line);
		}
		else
		{
			EarlierSessionLine = Lines.Add(EarlierSessionText(Count));
		}
	}
	for (const FDreamLostRun& Lost : Session.LostRuns)
	{
		Lines.Add(FText::Format(LOCTEXT("LostRun", "Run #{0} (previous session, {1}) — lost: the editor exited during this run{2}"),
			FText::AsNumber(Lost.RunNumber), Lost.TabTitle.IsEmpty() ? LOCTEXT("Recovery.LostRunRepl", "REPL") : FText::FromString(Lost.TabTitle),
			Lost.bPaused ? LOCTEXT("LostPaused", " while paused in the debugger") : FText::GetEmpty()));
	}
	if (Session.bFallbackUsed || Session.bSalvageUsed)
	{
		// §10.11: a generation lasts at most RotateSeconds, which bounds what the fallback can miss.
		Lines.Add(FText::Format(LOCTEXT("JournalFallback", "Recovered from the previous journal generation; up to {0} s of edits may be missing."),
			FText::AsNumber(FMath::RoundToInt(dream::journal::RotateSeconds))));
	}
	if (Session.bUnreadable)
	{
		Lines.Add(FText::Format(LOCTEXT("Unreadable", "The journal could not be read; the files were kept in {0}. Disk files are unaffected."),
			FText::FromString(M.GetJournal().GetStorageRoot().JournalDir(TEXT("main")))));
	}
	if (Session.bNewerFormat)
	{
		// (instances) §10.6: the newer namespace is never written; this session journals in compat-<format>.
		Lines.Add(Session.bCompat
			? FText::Format(LOCTEXT("JournalNewerCompat", "This project's Dream data was written by a newer version (format {0}.{1}). It is open read-only; this session journals separately."),
				FText::AsNumber(Session.NewerMajor), FText::AsNumber(Session.NewerMinor))
			: LOCTEXT("JournalNewer", "A newer Dream Python Editor wrote this project's journal. It was read but not changed, and this session's tabs are not journaled."));
	}
	if (Session.UnverifiedBuffers > 0)
	{
		Lines.Add(FText::Format(LOCTEXT("Unverified", "{0} journaled {0}|plural(one=buffer,other=buffers) could not be verified and {0}|plural(one=was,other=were) not restored."),
			FText::AsNumber(Session.UnverifiedBuffers)));
	}
	Impl->bShowRecoveryBanner = Lines.Num() > 0;
	Impl->SetRecoveryLines(MoveTemp(Lines), EarlierSessionLine);
	RebuildBanners();
	SaveSessionState(false);
	// (persist) §10.8 step 3: every tab is restored; the guard clears 10 s after the deferred import completes.
	M.GetRestoreGuard().OnRestoreApplied();
	RefreshRecoveryUi();
}

void SDreamTab::RestoreAllPending()
{
	// §10.7.3 Restore all: the recovered text as one undoable edit, dirty (PER-03); Ctrl+Z brings back the disk text.
	// Nothing is written to disk.
	// (persist) Each through the per-buffer Restore (SDreamTabRecovery.cpp: codeview's grouped-edit API, works on tabs never
	// shown). The orphaned, imported and compat buffers of the instances work are never touched by Restore all.
	for (const FGuid& Id : TArray<FGuid>(Impl->PendingRecoveries))
	{
		RestoreRecovered(Id, false);
	}
	Impl->bShowRecoveryBanner = Impl->PendingRecoveries.Num() > 0;
	RebuildBanners();
	RefreshTabStrip();
}

void SDreamTab::DiscardAllPending()
{
	// (persist) §10.7.3, U30: the discarded texts move to local history as "discarded" entries; each resolves once its entry
	// is durable, and one that could not be kept stays pending (P2-per-05). SDreamTabRecovery.cpp.
	Impl->bConfirmDiscard = false;
	DiscardRecovered(TArray<FGuid>(Impl->PendingRecoveries));
	RebuildBanners();
}

void SDreamTab::RebuildBanners_PythonState(EDreamPythonState NewState)
{
	RebuildBanners();
}

void SDreamTab::RebuildBanners()
{
	if (!Impl->BannerHost.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;   // (instances) a tab something still holds after Slate shut down (Python's state change at exit) builds nothing
	}
	Impl->BannerHost->ClearChildren();
	FDreamEditorModule& M = Module();
	TWeakPtr<SDreamTab> WeakTab = SharedThis(this);

	if (Impl->bShowRecoveryBanner && Impl->bConfirmDiscard && Impl->PendingRecoveries.Num() > 0)
	{
		// SPEC PER-03: Discard confirms inline (U26); the discarded texts stay in local history.
		const int32 Count = Impl->PendingRecoveries.Num();
		Impl->BannerHost->AddSlot().AutoHeight()
		[
			SNew(SDreamRecoveryBanner)
			.Message(FText::Format(LOCTEXT("Recovery.ConfirmDiscard", "Discard {0} recovered {0}|plural(one=buffer,other=buffers)? The text shown now stays; the recovered {0}|plural(one=text moves,other=texts move) to local history."),
				FText::AsNumber(Count)))
			.bShowActions(false)
			.ExtraActionText(LOCTEXT("ConfirmDiscardAction", "Discard"))
			.OnExtraAction_Lambda([WeakTab]() { if (TSharedPtr<SDreamTab> T = WeakTab.Pin()) { T->DiscardAllPending(); } })
			.DismissText(LOCTEXT("CancelDiscard", "Cancel"))
			.OnDismiss_Lambda([WeakTab]()
			{
				if (TSharedPtr<SDreamTab> T = WeakTab.Pin())
				{
					T->Impl->bConfirmDiscard = false;
					T->RebuildBanners();
				}
			})
		];
	}
	else if (Impl->bShowRecoveryBanner && !IsRecoveryLineHidden())
	{
		SDreamRecoveryBanner::FArguments Args;
		Args.Message(Impl->RecoveryMessage)
			.bShowActions(Impl->PendingRecoveries.Num() > 0)
			.OnRestoreAll_Lambda([WeakTab]() { if (TSharedPtr<SDreamTab> T = WeakTab.Pin()) { T->RestoreAllPending(); } })
			.OnReview_Lambda([WeakTab]() { if (TSharedPtr<SDreamTab> T = WeakTab.Pin()) { T->ToggleRecoveryReview(); } })   // (persist)
			.OnDiscard_Lambda([WeakTab]()
			{
				if (TSharedPtr<SDreamTab> T = WeakTab.Pin())
				{
					T->Impl->bConfirmDiscard = true;
					T->RebuildBanners();
				}
			});
		// (chrome) PO-04, PO-04-R1: the × hides the lines it showed, no more. A line added later (a Discard that could not be
		// kept, a lost run) shows the banner again; with buffers pending each keeps its "Not restored yet" bar (Restore,
		// Discard), File ▸ Review Recovered Buffers opens the panel, and View ▸ Show Hidden Notices brings the line back
		// (PER-06 holds: the pending buffers keep a way out; ignoring the banner loses nothing, §10.7.1).
		Args.OnDismiss_Lambda([WeakTab]()
		{
			if (TSharedPtr<SDreamTab> T = WeakTab.Pin())
			{
				T->Impl->HiddenRecoveryLineIds.Append(T->Impl->RecoveryLineIds);
				T->RebuildBanners();
			}
		});
		Impl->BannerHost->AddSlot().AutoHeight()[ SArgumentNew(Args, SDreamRecoveryBanner) ];
	}

	if (Impl->bSafeMode && Impl->SafeModeSession.IsSet()
		&& Impl->HiddenSafeModeKey != DreamTabLocal::SafeModeKey(M.GetRestoreGuard().GetSafeModeReason()))
	{
		// (persist) §10.8 UI; the reason in brackets says which trigger entered it.
		const EDreamSafeModeReason Reason = M.GetRestoreGuard().GetSafeModeReason();
		const FText Why = Reason == EDreamSafeModeReason::Guard ? LOCTEXT("Recovery.SafeWhyGuard", "two crashes during restore")
			: (Reason == EDreamSafeModeReason::Shift ? LOCTEXT("Recovery.SafeWhyShift", "Shift held while the tab opened")
				: LOCTEXT("Recovery.SafeWhySwitch", "started with -DreamSafeMode"));
		Impl->BannerHost->AddSlot().AutoHeight()
		[
			SNew(SDreamRecoveryBanner)
			.Message(FText::Format(LOCTEXT("Recovery.SafeMode", "Dream Python Editor safe mode ({0}): session restore skipped, language servers off, Run window presentation."), Why))
			.bShowActions(false)
			.bWarning(false)
			.ExtraActionText(LOCTEXT("Recovery.LeaveSafeMode", "Leave safe mode"))
			.OnExtraAction_Lambda([WeakTab]() { if (TSharedPtr<SDreamTab> T = WeakTab.Pin()) { T->LeaveSafeMode(); } })
			.SecondActionText(LOCTEXT("Recovery.ReviewSkipped", "Review recovered buffers"))
			.OnSecondAction_Lambda([WeakTab]() { if (TSharedPtr<SDreamTab> T = WeakTab.Pin()) { T->ToggleRecoveryReview(); } })
			// (chrome) PO-04: the × hides the line for now; View ▸ Show Hidden Notices brings it back.
			.OnDismiss_Lambda([WeakTab, Key = DreamTabLocal::SafeModeKey(Reason)]()
			{
				if (TSharedPtr<SDreamTab> T = WeakTab.Pin())
				{
					T->Impl->HiddenSafeModeKey = Key;
					T->RebuildBanners();
				}
			})
		];
	}

	if (!M.GetLocalHistory().IsPaused())
	{
		Impl->bHideHistoryBanner = false;   // (chrome) the next pause is a new one: it shows (PO-04-R1)
	}
	if (M.GetLocalHistory().IsPaused() && !Impl->bHideHistoryBanner)
	{
		// (persist) persist-r2-M2 (§10.11): history waits while another program holds its index; nothing is lost meanwhile.
		Impl->BannerHost->AddSlot().AutoHeight()
		[
			SNew(SDreamRecoveryBanner)
			.Message(FText::Format(LOCTEXT("History.Paused", "Local history is paused: another program is holding a file in {0}. Closed tabs and discarded texts are kept while the editor stays open and written once it is released."),
				FText::FromString(M.GetJournal().GetHistoryDir())))
			.bShowActions(false)
			.OnDismiss_Lambda([WeakTab]() { if (TSharedPtr<SDreamTab> T = WeakTab.Pin()) { T->Impl->bHideHistoryBanner = true; T->RebuildBanners(); } })   // (chrome)
		];
	}

	const EDreamPythonState PythonState = M.GetPython().GetState();
	if (DreamTabLocal::ShowsPythonBanner(PythonState) && Impl->HiddenPythonKey != DreamTabLocal::PythonKey(M.GetPython()))
	{
		// §3.5 state table and §9.5 (CR-15): BootFailed offers Retry; ShutDown offers Enable Python; Disabled offers it only
		// under D16's condition. The clicks only arm the next OnEndFrame (D16, T-LIFE-005).
		SDreamRecoveryBanner::FArguments Args;
		// (chrome) The × hides this state with this reason; another state (BootFailed after ShutDown) shows again (PO-04-R1).
		Args.Message(M.GetPython().GetDisabledReason()).bShowActions(false)
			.OnDismiss_Lambda([WeakTab, Key = DreamTabLocal::PythonKey(M.GetPython())]()
			{
				if (TSharedPtr<SDreamTab> T = WeakTab.Pin())
				{
					T->Impl->HiddenPythonKey = Key;
					T->RebuildBanners();
				}
			});
		if (PythonState == EDreamPythonState::BootFailed)
		{
			Args.ExtraActionText(LOCTEXT("RetryBoot", "Retry"))
				.OnExtraAction_Lambda([]() { if (FDreamEditorModule::IsAvailable()) { FDreamEditorModule::Get().GetPython().RetryBoot(); } });
		}
		else if (DreamTabLocal::CanOfferEnablePython(M.GetPython()))
		{
			Args.ExtraActionText(LOCTEXT("EnablePython", "Enable Python"))
				.OnExtraAction_Lambda([]() { if (FDreamEditorModule::IsAvailable()) { FDreamEditorModule::Get().GetPython().RequestEnable(); } });
		}
		Impl->BannerHost->AddSlot().AutoHeight()[ SArgumentNew(Args, SDreamRecoveryBanner) ];
	}

	if (!Impl->bDismissedStorageNotice)
	{
		FText Notice;
		const FDreamJournal& Journal = M.GetJournal();
		// (instances) A second instance journals in its own namespace now (§10.5), and the temp fallback is one of
		// FDreamInstances' storage notices (AddInstanceBanners below).
		if (!Journal.IsOpen() && Impl->bRestoreRequested)
		{
			Notice = LOCTEXT("JournalDisabled", "This session's tabs are not journaled: no storage folder was writable, or the journal could not open.");
		}
		if (!Notice.IsEmpty())
		{
			Impl->BannerHost->AddSlot().AutoHeight()
			[
				SNew(SDreamRecoveryBanner)
				.Message(Notice)
				.bShowActions(false)
				.OnDismiss_Lambda([WeakTab]()
				{
					if (TSharedPtr<SDreamTab> T = WeakTab.Pin())
					{
						T->Impl->bDismissedStorageNotice = true;
						T->RebuildBanners();
					}
				})
			];
		}
	}
	AddInstanceBanners();   // (instances) storage notices, the secondary, moved-project and n = 0 banners (SDreamTabRecovery.cpp)
}

void SDreamTab::SaveSessionState(bool bNow)
{
	if (!bNow)
	{
		if (!Impl->SaveTicker.IsValid())
		{
			TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
			Impl->SaveTicker = FDreamDeferral::AddTicker(TEXT("DreamSessionSave"), EDreamDeferPolicy::DeferWhileExecuting,
				DreamTabLocal::SessionSaveDebounceSeconds, false, [WeakTab](float)
				{
					if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
					{
						Tab->Impl->SaveTicker.Reset();
						Tab->SaveSessionState(true);
					}
					return false;
				});
		}
		return;
	}

	FDreamDeferral::RemoveTicker(Impl->SaveTicker);
	Impl->SaveTicker.Reset();
	if (!DreamTabLocal::GSessionRestored)
	{
		return;   // never overwrite the previous session before it was restored
	}
	UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
	// (instances) The ini's tab list is the primary's session (§10.5, slice B choice): a secondary keeps it as it is.
	const bool bSecondary = Module().GetJournal().IsOpen() && Module().GetJournal().GetRole() == EDreamInstanceRole::Secondary;
	const TArray<FDreamTabState> PrimaryTabs = Settings->OpenTabs;
	const FString PrimaryActive = Settings->ActiveTab;
	Settings->OpenTabs.Reset();
	if (DreamTabLocal::bGSkippedSessionPending)
	{
		// Safe mode: the skipped session's tabs stay listed until Restore tabs applies them (SHL-01, §10.7.1).
		Settings->OpenTabs = DreamTabLocal::GSkippedIniTabs;
	}
	RefreshScrollStates();   // (scratch, PER-02) the shown views' scroll, also when only the wheel moved it
	for (const TSharedRef<FDreamBuffer>& Buffer : GetTabBuffers())
	{
		const FString IdHex = Buffer->GetIdHex();
		if (Settings->OpenTabs.ContainsByPredicate([&IdHex](const FDreamTabState& Tab) { return Tab.BufferId == IdHex; }))
		{
			continue;
		}
		const FDreamBufferViewState& View = Buffer->GetViewState();
		FDreamTabState& Tab = Settings->OpenTabs.AddDefaulted_GetRef();
		Tab.BufferId = Buffer->GetIdHex();
		Tab.Kind = DreamTabLocal::ToTabKind(Buffer->GetKind());
		Tab.Path = Buffer->GetPath();
		Tab.DisplayName = Buffer->GetDisplayName();
		Tab.CaretLine = View.CaretLine;
		Tab.CaretColumn = View.CaretColumn;
		Tab.FirstVisibleLine = View.FirstVisibleLine;
		Tab.bOneUndoStepPerRun = View.bOneUndoStepPerRun;
		Tab.bOneUndoStepPerRunSet = View.bOneUndoStepPerRunSet;   // (run group, B40)
		Tab.bIsolated = View.Namespace == EDreamNamespaceMode::Isolated;
		Tab.bPlainMode = true;
		Tab.ZoomPercent = View.ZoomPercent;
	}
	Settings->ActiveTab = Impl->ActiveId.IsValid() ? DreamText::GuidToHex(Impl->ActiveId) : FString();
	if (bSecondary)
	{
		Settings->OpenTabs = PrimaryTabs;
		Settings->ActiveTab = PrimaryActive;
	}
	const float Total = Impl->OutputValue + Impl->CodeValue;
	Settings->OutputAreaFraction = Total > 0.f ? Impl->OutputValue / Total : 0.4f;
	Settings->ActiveOutputTab = OutputArea.IsValid() ? static_cast<int32>(OutputArea->GetActiveTab()) : 0;
	Settings->SaveConfig();
}

#undef LOCTEXT_NAMESPACE
