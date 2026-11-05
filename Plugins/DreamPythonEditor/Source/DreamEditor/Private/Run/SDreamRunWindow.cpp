// SPDX-License-Identifier: Apache-2.0
// The Run window (TDD §5.5.3, D2, RT-03, RT-04, RV-02, RV-16, RV-17, PO-03): a popup modal exactly over the output area,
// rows as mockup 02 (Title, State, Location, Progress and undo meter, live run view, Notes, Footer), placement in desktop
// pixels with the per-dimension minimum, growth for notes, and teardown steps 1-4. Game thread; opened by the supervisor
// inside the first pump's GIL-released region, before PumpMessages. Every value is read at paint from the supervisor's
// FDreamRunLiveStats and the active FDreamRunRecord (no locks, §5.4.7).
#include "Run/SDreamRunWindow.h"

#include "Commands/DreamCommands.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Run/DreamRunSourceText.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamTheme.h"   // (theme)

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamRunWindowPrivate
{
	constexpr float MinWidthUnits = 480.f;          // §5.5.3 step 4 (proposed)
	constexpr float HiddenWidthUnits = 640.f;       // the area is not on screen: 640 x 360, centred (proposed)
	constexpr float HiddenHeightUnits = 360.f;
	constexpr int32 LiveViewMinLines = 5;           // H_min: the other rows plus 5 live-view lines

	/** Teardown step 4 entries whose old parent was still referenced; retried at the next pump or idle tick. */
	struct FKeptChild
	{
		TSharedRef<SWindow> Window;
		FDelegateHandle ClosedHandle;           // drops the entry when the child is closed meanwhile
	};
	TArray<FKeptChild> KeptChildWindows;
	bool bReparentTickerArmed = false;

	/**
	 * The top-level window that hosted the focus Dream restores (the Dream tab's window) when the Run window opened. Teardown
	 * hands activation back to it before the destroy, so the OS never picks the next window itself (a floating Message Log
	 * window took it, and the main window's later reactivation then restored its old focus over Dream's: chrome's focus trace).
	 */
	TWeakPtr<SWindow> ReturnWindow;
	TWeakPtr<SWindow> LastReturnWindowForTest;

	/** The regular window that contains Widget (a menu or popup window gives its regular ancestor). */
	TSharedPtr<SWindow> RegularWindowOf(const TSharedPtr<SWidget>& Widget)
	{
		TSharedPtr<SWindow> Window = Widget.IsValid() ? FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) : nullptr;
		while (Window.IsValid() && !Window->IsRegularWindow() && Window->GetParentWindow().IsValid())
		{
			Window = Window->GetParentWindow();
		}
		return Window.IsValid() && Window->IsRegularWindow() ? Window : nullptr;
	}

	void ForgetKeptChild(const TSharedRef<SWindow>& Closed)
	{
		KeptChildWindows.RemoveAll([&Closed](const FKeptChild& Entry) { return Entry.Window == Closed; });
	}

	void KeepChild(const TSharedRef<SWindow>& Child)
	{
		FKeptChild Entry{Child, Child->GetOnWindowClosedEvent().AddStatic(&ForgetKeptChild)};
		KeptChildWindows.Add(MoveTemp(Entry));
	}

	const FDreamRunLiveStats& EmptyStats()
	{
		static const FDreamRunLiveStats Empty;
		return Empty;
	}

	const FDreamRunRecord* ActiveRecord(int32 RunNumber)
	{
		if (!FDreamEditorModule::IsAvailable())
		{
			return nullptr;
		}
		const FDreamRunRecord* Rec = FDreamEditorModule::Get().GetRunService().GetActiveRecord();
		return (Rec && Rec->RunNumber == RunNumber) ? Rec : nullptr;
	}

	FText Plain(int64 Value)
	{
		return FText::AsNumber(Value, &FNumberFormattingOptions::DefaultNoGrouping());
	}

	/** "41 s", "1 min 54 s", "2 h 5 min". */
	FText Duration(double Seconds)
	{
		const int64 Total = FMath::Max<int64>(0, FMath::FloorToInt64(Seconds));
		if (Total < 60)
		{
			return FText::Format(LOCTEXT("RunWindow.Seconds", "{0} s"), Plain(Total));
		}
		if (Total < 3600)
		{
			return FText::Format(LOCTEXT("RunWindow.MinSec", "{0} min {1} s"), Plain(Total / 60), Plain(Total % 60));
		}
		return FText::Format(LOCTEXT("RunWindow.HourMin", "{0} h {1} min"), Plain(Total / 3600), Plain((Total % 3600) / 60));
	}

	FText OneDecimal(double Value)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = 1;
		Options.MaximumFractionalDigits = 1;
		return FText::AsNumber(Value, &Options);
	}

	FText Megabytes(SIZE_T Bytes)
	{
		return FText::AsNumber(static_cast<int64>(Bytes / (1024 * 1024)));
	}

	FText Separator()
	{
		return FText::FromString(TEXT(" · "));
	}

	/** The Location row's file part: the tab or file name ("fix_materials.py", "Untitled-3", "REPL"). */
	FString FileLabel(const FDreamRunRecord& Rec)
	{
		if (!Rec.TabTitle.IsEmpty())
		{
			return Rec.TabTitle;
		}
		return FPaths::GetCleanFilename(Rec.FilePath.IsEmpty() ? Rec.Filename : Rec.FilePath);
	}

	/** The three parts of the Location row (§5.4.7): lead ("Line 43 · "), source text (monospace), tail. */
	struct FLocationParts
	{
		FText Lead;
		FText Source;
		FText Tail;
	};

	FLocationParts BuildLocation(const FDreamRunLiveStats& S, const FDreamRunRecord* Rec)
	{
		FLocationParts Parts;
		if (Rec == nullptr)
		{
			return Parts;
		}
		const bool bStatements = Rec->bStatementMode && Rec->Statements.Num() > 0;
		int32 SourceLine = S.Line;
		if (bStatements && S.bBetweenStatements)
		{
			// Between statements: the last started statement's range, "Lines 12–14" (proposed).
			const FDreamStatementRange& Range = Rec->Statements[FMath::Clamp(Rec->StatementsStarted - 1, 0, Rec->Statements.Num() - 1)];
			Parts.Lead = Range.FirstLine == Range.LastLine
				? FText::Format(LOCTEXT("RunWindow.LocLine", "Line {0}"), Plain(Range.FirstLine))
				: FText::Format(LOCTEXT("RunWindow.LocLines", "Lines {0}–{1}"), Plain(Range.FirstLine), Plain(Range.LastLine));
			SourceLine = Range.FirstLine;
		}
		else if (S.Line > 0)
		{
			Parts.Lead = FText::Format(LOCTEXT("RunWindow.LocLine", "Line {0}"), Plain(S.Line));
		}

		// Source text from the run's snapshot, never through Python; a run whose code was read from a file (an API run of a
		// file, Dream.RunFile of a file that is not open) shows none, though Submit gave it a snapshot (§5.4.7, T-SUP-037).
		const bool bCutSource = Rec->BufferId.IsValid() || Rec->Route == EDreamRunRoute::Repl || Rec->Route == EDreamRunRoute::ConsoleExecutor;
		if (SourceLine > 0 && bCutSource && DreamRunSource::HasSource(Rec->SourceSnapshot))
		{
			const FString Text = DreamRunSource::ForLocation(DreamRunSource::GetLine(Rec->SourceSnapshot, SourceLine));
			if (!Text.IsEmpty())
			{
				Parts.Source = FText::FromString(Text);
				Parts.Lead = FText::Format(LOCTEXT("RunWindow.LocLeadSep", "{0}{1}"), Parts.Lead, Separator());
			}
		}

		FText Tail = FText::FromString(FileLabel(*Rec));
		if (!S.InnermostFrame.IsEmpty())
		{
			Tail = FText::Format(LOCTEXT("RunWindow.LocInnermost", "{0} (in {1})"), Tail, FText::FromString(S.InnermostFrame));
		}
		if (bStatements && Rec->StatementsStarted > 0)
		{
			// RUN-27: k is the started count that _dream.run_statement records, n the prepared statements.
			Tail = FText::Format(LOCTEXT("RunWindow.LocStatement", "{0}{1}statement {2} of {3}"), Tail, Separator(),
				Plain(Rec->StatementsStarted), Plain(Rec->Statements.Num()));
		}
		const bool bHasLead = !Parts.Lead.IsEmpty() || !Parts.Source.IsEmpty();
		Parts.Tail = bHasLead ? FText::Format(LOCTEXT("RunWindow.LocTailSep", "{0}{1}"), Separator(), Tail) : Tail;
		return Parts;
	}

	FText ProgressText(const FDreamRunLiveStats& S)
	{
		if (S.ProgressDone < 0)
		{
			return FText::GetEmpty();
		}
		// U12: "312 / 1,204 · 7.8/s · ETA 1 min 54 s"; rate and ETA after 3 samples; n=None shows the count alone.
		const FText Count = S.ProgressTotal.IsSet()
			? FText::Format(LOCTEXT("RunWindow.ProgressOf", "{0} / {1}"), FText::AsNumber(S.ProgressDone), FText::AsNumber(S.ProgressTotal.GetValue()))
			: FText::AsNumber(S.ProgressDone);
		if (S.ProgressSamples < 3 || S.ProgressRate <= 0.0)
		{
			return Count;
		}
		const FText Rate = FText::Format(LOCTEXT("RunWindow.Rate", "{0}/s"),
			S.ProgressRate < 10.0 ? OneDecimal(S.ProgressRate) : FText::AsNumber(FMath::RoundToInt64(S.ProgressRate)));
		if (S.ProgressEtaSeconds < 0.0)
		{
			return FText::Format(LOCTEXT("RunWindow.CountRate", "{0}{1}{2}"), Count, Separator(), Rate);
		}
		return FText::Format(LOCTEXT("RunWindow.CountRateEta", "{0}{1}{2}{1}ETA {3}"), Count, Separator(), Rate, Duration(S.ProgressEtaSeconds));
	}

	TOptional<float> ProgressPercent(const FDreamRunLiveStats& S)
	{
		if (S.ProgressDone >= 0)
		{
			if (S.ProgressTotal.IsSet() && S.ProgressTotal.GetValue() > 0)
			{
				return FMath::Clamp(static_cast<float>(static_cast<double>(S.ProgressDone) / static_cast<double>(S.ProgressTotal.GetValue())), 0.f, 1.f);
			}
			return TOptional<float>();                  // indeterminate bar (U12)
		}
		return S.SlowTaskFraction >= 0.f ? TOptional<float>(S.SlowTaskFraction) : TOptional<float>(0.f);
	}

	FText UndoText(const FDreamRunLiveStats& S)
	{
		if (S.TxnBudget == 0)
		{
			return LOCTEXT("RunWindow.UndoOff", "Undo record: off for this run");
		}
		if (S.TxnBytes >= S.TxnBudget)
		{
			return LOCTEXT("RunWindow.UndoStopped", "Recording stopped at 100%: partly undoable");   // RT-08
		}
		return FText::Format(LOCTEXT("RunWindow.UndoMeter", "Undo record {0} MB of {1} MB"), Megabytes(S.TxnBytes), Megabytes(S.TxnBudget));
	}

	float UndoFraction(const FDreamRunLiveStats& S)
	{
		return S.TxnBudget > 0 ? FMath::Clamp(static_cast<float>(static_cast<double>(S.TxnBytes) / static_cast<double>(S.TxnBudget)), 0.f, 1.f) : 0.f;
	}

	bool ShowsSwallow(const FDreamRunLiveStats& S) { return S.SwallowLine > 0 && !S.bCleanupRunning; }
	bool ShowsCleanup(const FDreamRunLiveStats& S) { return S.bCleanupRunning || S.bShowInterruptCleanup; }
	bool ShowsNative(const FDreamRunLiveStats& S) { return S.LastNativeSeconds > 1.0; }
	bool ShowsDebugpy(const FDreamRunLiveStats& S) { return S.bDebugpyPaused; }

	/** "finally at line 88", "__exit__ at line 88" or "line 88", from the header line's source (§5.8 table). */
	FText CleanupWhere(const FDreamRunLiveStats& S, const FDreamRunRecord* Rec)
	{
		DreamRunSource::EHandlerKind Kind = DreamRunSource::EHandlerKind::None;
		if (Rec != nullptr && S.CleanupLine > 0)
		{
			Kind = DreamRunSource::ClassifyHeader(DreamRunSource::GetLine(Rec->SourceSnapshot, S.CleanupLine));
		}
		switch (Kind)
		{
		case DreamRunSource::EHandlerKind::Finally:
			return FText::Format(LOCTEXT("RunWindow.CleanupFinally", "finally at line {0}"), Plain(S.CleanupLine));
		case DreamRunSource::EHandlerKind::With:
			return FText::Format(LOCTEXT("RunWindow.CleanupExit", "__exit__ at line {0}"), Plain(S.CleanupLine));
		default:
			return FText::Format(LOCTEXT("RunWindow.CleanupLine", "line {0}"), Plain(S.CleanupLine));
		}
	}

	FText StopChordText()
	{
		if (FDreamCommands::IsRegistered())
		{
			const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(EDreamCommand::Stop);
			if (Info.IsValid())
			{
				const FText Chord = Info->GetInputText();
				if (!Chord.IsEmpty())
				{
					return Chord;
				}
			}
		}
		return LOCTEXT("RunWindow.DefaultStopChord", "Shift+F5");
	}

	/** Moves Rect inside the work area of its monitor (SWindow's sane placement checks only the top-left corner). */
	FSlateRect KeepInWorkArea(const FSlateRect& Rect)
	{
		const FSlateRect Work = FSlateApplication::Get().GetWorkArea(Rect);
		const float WorkWidth = Work.Right - Work.Left;
		const float WorkHeight = Work.Bottom - Work.Top;
		if (WorkWidth <= 0.f || WorkHeight <= 0.f)
		{
			return Rect;
		}
		const float Width = FMath::Min(Rect.Right - Rect.Left, WorkWidth);
		const float Height = FMath::Min(Rect.Bottom - Rect.Top, WorkHeight);
		const float Left = FMath::Clamp(Rect.Left, Work.Left, Work.Right - Width);
		const float Top = FMath::Clamp(Rect.Top, Work.Top, Work.Bottom - Height);
		return FSlateRect(Left, Top, Left + Width, Top + Height);
	}

	/** The window the not-visible case centres on: the active regular window, else the main frame. */
	TSharedPtr<SWindow> AnchorWindow()
	{
		TSharedPtr<SWindow> Anchor = FSlateApplication::Get().GetActiveTopLevelRegularWindow();
		if (!Anchor.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("MainFrame")))
		{
			Anchor = IMainFrameModule::Get().GetParentWindow();
		}
		if (!Anchor.IsValid())
		{
			Anchor = FGlobalTabmanager::Get()->GetRootWindow();
		}
		return Anchor;
	}

	/** §5.5.3 step 4: the final rect in desktop pixels, after the minimum size and the work-area move. */
	FSlateRect ComputePlacement(const FDreamRunPolicy& Policy, float MinHeightUnits)
	{
		const float AreaWidth = Policy.OutputAreaRect.Right - Policy.OutputAreaRect.Left;
		const float AreaHeight = Policy.OutputAreaRect.Bottom - Policy.OutputAreaRect.Top;
		if (Policy.bOutputAreaVisible && AreaWidth > 0.f && AreaHeight > 0.f)
		{
			const float Scale = Policy.DpiScale > 0.f ? Policy.DpiScale : 1.f;
			const float Width = FMath::Max(AreaWidth, MinWidthUnits * Scale);        // narrower: 480 wide from the area's left edge
			const float Height = FMath::Max(AreaHeight, MinHeightUnits * Scale);     // shorter: grow toward the code area
			const float Left = Policy.OutputAreaRect.Left;
			const float Top = Policy.bOutputAreaAtTop ? Policy.OutputAreaRect.Top : Policy.OutputAreaRect.Bottom - Height;
			return KeepInWorkArea(FSlateRect(Left, Top, Left + Width, Top + Height));
		}

		// Not visible (closed or background tab, minimised window): 640 x 360 Slate units centred on the anchor, tall
		// enough for 5 live-view lines (T-SUP-036).
		const TSharedPtr<SWindow> Anchor = AnchorWindow();
		const float Scale = Anchor.IsValid() ? Anchor->GetDPIScaleFactor() : 1.f;
		const float Width = HiddenWidthUnits * Scale;
		const float Height = FMath::Max(HiddenHeightUnits, MinHeightUnits) * Scale;
		FVector2D Center(0.0, 0.0);
		if (Anchor.IsValid())
		{
			const FVector2D Pos = Anchor->GetPositionInScreen();
			const FVector2D Size = Anchor->GetSizeInScreen();
			Center = Pos + Size * 0.5;
		}
		else
		{
			const FSlateRect Work = FSlateApplication::Get().GetPreferredWorkArea();
			Center = FVector2D((Work.Left + Work.Right) * 0.5f, (Work.Top + Work.Bottom) * 0.5f);
		}
		const float Left = static_cast<float>(Center.X) - Width * 0.5f;
		const float Top = static_cast<float>(Center.Y) - Height * 0.5f;
		return KeepInWorkArea(FSlateRect(Left, Top, Left + Width, Top + Height));
	}

#if PLATFORM_WINDOWS
	struct FReownData
	{
		HWND From;
		HWND To;
	};

	BOOL CALLBACK ReownThreadWindow(HWND Hwnd, LPARAM Param)
	{
		const FReownData* Data = reinterpret_cast<const FReownData*>(Param);
		if (::GetWindow(Hwnd, GW_OWNER) == Data->From)
		{
			// The same call PySlate.cpp:189 makes with the literal -8.
			::SetWindowLongPtrW(Hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(Data->To));
		}
		return 1;
	}

	/** Teardown step 2 (RV-02): ::DestroyWindow destroys every window the destroyed one owns (WindowsWindow.cpp:672). */
	void ReownToMainFrame(const TSharedRef<SWindow>& RunWindow)
	{
		const TSharedPtr<FGenericWindow> RunNative = RunWindow->GetNativeWindow();
		const TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
		const TSharedPtr<FGenericWindow> RootNative = Root.IsValid() ? Root->GetNativeWindow() : nullptr;
		if (!RunNative.IsValid() || !RootNative.IsValid())
		{
			return;
		}
		FReownData Data;
		Data.From = static_cast<HWND>(RunNative->GetOSWindowHandle());
		Data.To = static_cast<HWND>(RootNative->GetOSWindowHandle());
		if (Data.From != nullptr && Data.To != nullptr && Data.From != Data.To)
		{
			::EnumThreadWindows(static_cast<DWORD>(FPlatformTLS::GetCurrentThreadId()), &ReownThreadWindow, reinterpret_cast<LPARAM>(&Data));
		}
	}
#endif
}

// =====================================================================================================================
// Content
// =====================================================================================================================

void SDreamRunWindow::Construct(const FArguments& InArgs)
{
	using namespace DreamRunWindowPrivate;
	Stats = InArgs._Stats;
	RunNumber = InArgs._RunNumber;
	TabOrItem = InArgs._TabOrItem;
	OnStop = InArgs._OnStop;
	OnInterruptCleanup = InArgs._OnInterruptCleanup;

	// One live-view line in Slate units, for H_min's 5 lines (about 19 at the default size, §5.5.3).
	const FSlateFontInfo CodeFont = FDreamEditorStyle::GetCodeFont(UDreamUserSettings::Get()->FontSize);
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer() != nullptr)
	{
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		LiveLineHeight = FMath::Max(16.f, static_cast<float>(Measure->GetMaxCharacterHeight(CodeFont)) + 3.f);
	}

	const FText Title = FText::Format(LOCTEXT("RunWindow.Title", "Dream · Run #{0} · {1}"), Plain(RunNumber), FText::FromString(TabOrItem));
	const FText Hint = FText::Format(LOCTEXT("RunWindow.Hint", "Esc or {0}"), StopChordText());
	const FSlateFontInfo StateFont = FCoreStyle::GetDefaultFontStyle("Bold", 11);   // (chrome) PO-04: one slim header row
	const FSlateFontInfo BodyFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);

	auto LiveStats = [this]() -> const FDreamRunLiveStats& { return Stats ? *Stats : EmptyStats(); };

	// (chrome) PO-04: the flat layout. One header row (state, the tab or item, the hint and Stop), the location, the meters,
	// the live run view edge to edge between one-pixel lines, the notes, and a muted footer line; no title or footer bands.
	auto Rule = []() -> TSharedRef<SWidget>
	{
		return SNew(SBox).HeightOverride(1.f)[ SNew(SImage).Image(FDreamThemes::Brush(EDreamThemeSlot::ChromeBorder)) ];   // (theme)
	};

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground))   // (theme) PO-04: flat, in the theme's colours
		.ForegroundColor(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
		.Padding(0.f)
		[
			SNew(SVerticalBox)

			// Header: U2 state, elapsed time and run number, then "· <tab or item>"; the hint and Stop at the right (Stopping…
			// once requested). The window title "Dream · Run #22 · <tab or item>" is the tooltip.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(10.f, 6.f, 8.f, 2.f))
			[
				SNew(SHorizontalBox)
				.ToolTipText(Title)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
				[
					SNew(SCircularThrobber)
					.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::Accent))   // (theme)
					.Radius(6.f)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(StateFont)
					.Text_Lambda([this, LiveStats]()
					{
						const FDreamRunLiveStats& S = LiveStats();
						return FText::Format(LOCTEXT("RunWindow.State", "{0} {1} · Run #{2}"),
							DreamRunText::StateName(S.State == EDreamRunState::Idle ? EDreamRunState::Running : S.State),
							Duration(S.ElapsedSeconds), Plain(RunNumber));
					})
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.f, 0.f, 8.f, 0.f))
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Text(FText::FromString(TabOrItem))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(Hint)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SAssignNew(StopButton, SButton)
					.IsFocusable(false)
					// A216: Stop acts on the press. A click made while user code blocks the game thread is read after the call
					// returns; by then Slate's hover follows the live cursor, and a release-click needs hover, so a cursor
					// moved off Stop in between would lose the emergency control. Keys stop through the pre-processor.
					.ClickMethod(EButtonClickMethod::MouseDown)
					.TouchMethod(EButtonTouchMethod::Down)
					.ButtonStyle(&FDreamEditorStyle::Get().GetWidgetStyle<FButtonStyle>(DreamStyleKeys::RunWindowStopButton))
					.ContentPadding(FMargin(8.f, 2.f))
					.IsEnabled_Lambda([LiveStats]() { return LiveStats().State != EDreamRunState::Stopping; })
					.OnClicked_Lambda([this]()
					{
						OnStop.ExecuteIfBound();
						return FReply::Handled();
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
						[
							SNew(SImage)
							.Image(FDreamEditorStyle::Get().GetBrush(DreamStyleKeys::StopIcon))
							.DesiredSizeOverride(FVector2D(12.0, 12.0))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Font(BodyFont)
							.Text_Lambda([LiveStats]()
							{
								return LiveStats().State == EDreamRunState::Stopping
									? DreamRunText::StateName(EDreamRunState::Stopping)
									: LOCTEXT("RunWindow.Stop", "Stop");
							})
						]
					]
				]
			]

			// Location: "Line 43 · `for a in actors:` · fix_materials.py (in json/decoder.py:337) · statement 7 of 12"
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(10.f, 1.f, 10.f, 1.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("RunWindow.Location", "Location"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.Text_Lambda([this, LiveStats]() { return BuildLocation(LiveStats(), ActiveRecord(RunNumber)).Lead; })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(CodeFont)
					.Text_Lambda([this, LiveStats]() { return BuildLocation(LiveStats(), ActiveRecord(RunNumber)).Source; })
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Text_Lambda([this, LiveStats]() { return BuildLocation(LiveStats(), ActiveRecord(RunNumber)).Tail; })
				]
			]

			// Progress and undo meter: one row of two thin meters
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(10.f, 4.f, 10.f, 4.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(0.6f)
				[
					SNew(SVerticalBox)
					.Visibility_Lambda([LiveStats]()
					{
						const FDreamRunLiveStats& S = LiveStats();
						return (S.ProgressDone >= 0 || S.SlowTaskFraction >= 0.f) ? EVisibility::Visible : EVisibility::Hidden;
					})
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SBox)
						.HeightOverride(3.f)
						[
							SNew(SProgressBar)
							.Style(&FDreamThemes::ProgressBarStyle())   // (theme)
							.FillColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::Accent))
							.Percent_Lambda([LiveStats]() { return ProgressPercent(LiveStats()); })
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(STextBlock)
							.Font(CodeFont)
							.Text_Lambda([LiveStats]() { return ProgressText(LiveStats()); })
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.HAlign(HAlign_Right)
						.Padding(FMargin(12.f, 0.f, 0.f, 0.f))
						[
							SNew(STextBlock)
							.Font(BodyFont)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
							.Text_Lambda([LiveStats]()
							{
								const FDreamRunLiveStats& S = LiveStats();
								// A user slow task's message and fraction appear when present (LRS-04).
								if (S.SlowTaskFraction >= 0.f && !S.SlowTaskMessage.IsEmpty())
								{
									return S.ProgressMessage.IsEmpty()
										? S.SlowTaskMessage
										: FText::Format(LOCTEXT("RunWindow.ProgressAndTask", "{0}{1}{2}"), FText::FromString(S.ProgressMessage), Separator(), S.SlowTaskMessage);
								}
								return FText::FromString(S.ProgressMessage);
							})
						]
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.4f)
				.Padding(FMargin(16.f, 0.f, 0.f, 0.f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SBox)
						.HeightOverride(3.f)
						.Visibility_Lambda([LiveStats]() { return LiveStats().TxnBudget > 0 ? EVisibility::Visible : EVisibility::Hidden; })
						[
							SNew(SProgressBar)
							.Style(&FDreamThemes::ProgressBarStyle())   // (theme)
							.Percent_Lambda([LiveStats]() { return TOptional<float>(UndoFraction(LiveStats())); })
							.FillColorAndOpacity_Lambda([LiveStats]()
							{
								// Warning style at 50% (RT-08).
								return UndoFraction(LiveStats()) >= 0.5f ? FDreamThemes::Color(EDreamThemeSlot::OutWarning) : FDreamThemes::Color(EDreamThemeSlot::Accent);   // (theme)
							})
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
					[
						SNew(STextBlock)
						.Font(BodyFont)
						.ColorAndOpacity_Lambda([LiveStats]()
						{
							const FDreamRunLiveStats& S = LiveStats();
							if (S.TxnBudget == 0)
							{
								return FSlateColor::UseSubduedForeground();
							}
							return UndoFraction(S) >= 0.5f ? FDreamThemes::Color(EDreamThemeSlot::OutWarning) : FSlateColor::UseForeground();   // (theme)
						})
						.Text_Lambda([LiveStats]() { return UndoText(LiveStats()); })
					]
				]
			]

			// Live run view: this run's echo and output as each pump drains them (PO-03); at least 5 lines (H_min)
			+ SVerticalBox::Slot().AutoHeight()[ Rule() ]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SNew(SBorder)
				.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::OutputBackground))   // (theme)
				.Padding(FMargin(4.f, 2.f, 0.f, 2.f))
				[
					SNew(SBox)
					.HeightOverride_Lambda([this]()
					{
						return bMeasuring ? FOptionalSize(LiveViewMinLines * LiveLineHeight) : FOptionalSize();
					})
					[
						SAssignNew(LiveRunView, SDreamOutputView)
						.RunFilter(TOptional<int32>(RunNumber))
						.bLiveRunView(true)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()[ Rule() ]

			// Notes: below the live view, no height while empty (proposed)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(10.f, 0.f, 10.f, 0.f))
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([LiveStats]()
				{
					const FDreamRunLiveStats& S = LiveStats();
					return (ShowsSwallow(S) || ShowsCleanup(S) || ShowsNative(S) || ShowsDebugpy(S)) ? EVisibility::Visible : EVisibility::Collapsed;
				})

				// Swallow report (§5.7.4, LRS-14)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.f, 2.f))
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::OutWarning))   // (theme)
					.AutoWrapText(true)
					.Visibility_Lambda([LiveStats]() { return ShowsSwallow(LiveStats()) ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text_Lambda([LiveStats]()
					{
						return FText::Format(LOCTEXT("RunWindow.Swallow", "The script caught Stop at line {0}. Dream Python Editor keeps stopping it."), Plain(LiveStats().SwallowLine));
					})
				]

				// The cleanup timer with Interrupt cleanup (RT-06, LRS-12)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.f, 2.f))
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([LiveStats]() { return ShowsCleanup(LiveStats()) ? EVisibility::Visible : EVisibility::Collapsed; })
					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(BodyFont)
						.AutoWrapText(true)
						.Text_Lambda([this, LiveStats]()
						{
							const FDreamRunLiveStats& S = LiveStats();
							if (!S.bCleanupRunning)
							{
								return FText::GetEmpty();
							}
							return FText::Format(LOCTEXT("RunWindow.Cleanup", "{0} cleanup is running ({1}), {2}"),
								DreamRunText::StateName(EDreamRunState::Stopping), CleanupWhere(S, ActiveRecord(RunNumber)), Duration(S.CleanupSeconds));
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(8.f, 0.f, 6.f, 0.f))
					[
						SAssignNew(InterruptButton, SButton)
						.IsFocusable(false)
						.ButtonColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::Danger))   // (theme)
						.Visibility_Lambda([LiveStats]() { return LiveStats().bShowInterruptCleanup ? EVisibility::Visible : EVisibility::Collapsed; })
						.OnClicked_Lambda([this]()
						{
							OnInterruptCleanup.ExecuteIfBound();
							return FReply::Handled();
						})
						[
							SNew(STextBlock)
							.Font(BodyFont)
							.Text(LOCTEXT("RunWindow.InterruptCleanup", "Interrupt cleanup"))
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(BodyFont)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Visibility_Lambda([LiveStats]() { return LiveStats().bShowInterruptCleanup ? EVisibility::Visible : EVisibility::Collapsed; })
						.Text(LOCTEXT("RunWindow.InterruptCleanupHint", "delivers Stop once more"))
					]
				]

				// The last native interval over 1 s (§5.8, LRS-03)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.f, 2.f))
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.AutoWrapText(true)
					.Visibility_Lambda([LiveStats]() { return ShowsNative(LiveStats()) ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text_Lambda([LiveStats]()
					{
						const FDreamRunLiveStats& S = LiveStats();
						return S.LastNativeLine > 0
							? FText::Format(LOCTEXT("RunWindow.NativeAtLine", "The last native call took {0} s (line {1})."), OneDecimal(S.LastNativeSeconds), Plain(S.LastNativeLine))
							: FText::Format(LOCTEXT("RunWindow.Native", "The last native call took {0} s."), OneDecimal(S.LastNativeSeconds));
					})
				]

				// pydevd holds the game thread (§5.4.3)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.f, 2.f))
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.AutoWrapText(true)
					.Visibility_Lambda([LiveStats]() { return ShowsDebugpy(LiveStats()) ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text(LOCTEXT("RunWindow.Debugpy", "Paused in VS Code: Stop takes effect when you continue in the IDE."))
				]
			]

			// Footer (U11, RV-17): one muted line
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(10.f, 3.f, 10.f, 4.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("RunWindow.FooterViewports", "Viewports resume when the run ends."))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.Padding(FMargin(6.f, 0.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					.Font(BodyFont)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Text(LOCTEXT("RunWindow.FooterInput", "Clicks and keys outside this window are ignored until the run ends."))
				]
			]
		]
	];
}

TArray<TWeakPtr<SWidget>> SDreamRunWindow::GetStopTargets() const
{
	TArray<TWeakPtr<SWidget>> Targets;
	Targets.Add(StopButton);
	Targets.Add(InterruptButton);
	Targets.Add(StaticCastSharedPtr<SWidget>(LiveRunView));
	return Targets;
}

float SDreamRunWindow::ComputeMinimumHeight()
{
	// H_min is measured, not assumed: the rows' desired heights with the live view held at 5 lines (§5.5.3 step 4).
	bMeasuring = true;
	SlatePrepass(1.f);
	const float Height = GetDesiredSize().Y;
	bMeasuring = false;
	Invalidate(EInvalidateWidgetReason::Layout);
	return Height;
}

uint32 SDreamRunWindow::GetNotesSignature() const
{
	using namespace DreamRunWindowPrivate;
	const FDreamRunLiveStats& S = Stats ? *Stats : EmptyStats();
	return (ShowsSwallow(S) ? 1u : 0u)
		| (ShowsCleanup(S) ? 2u : 0u)
		| (S.bShowInterruptCleanup ? 4u : 0u)
		| (ShowsNative(S) ? 8u : 0u)
		| (ShowsDebugpy(S) ? 16u : 0u);
}

// =====================================================================================================================
// Opening (§5.5.3 steps 3-5) and growth
// =====================================================================================================================

TSharedPtr<SWindow> SDreamRunWindow::OpenModal(const FDreamRunPolicy& Policy, const TSharedRef<SDreamRunWindow>& Content)
{
	using namespace DreamRunWindowPrivate;
	if (!FSlateApplication::IsInitialized())
	{
		return nullptr;
	}
	FSlateApplication& App = FSlateApplication::Get();
	// Step 2 and §5.5.1: no window while a foreign modal is up, or when no modal can be added.
	if (!App.CanAddModalWindow() || App.GetActiveModalWindow().IsValid())
	{
		return nullptr;
	}

	const FSlateRect Rect = ComputePlacement(Policy, Content->ComputeMinimumHeight());
	const bool bFocus = FApp::HasFocus();

	// The window activation goes back to at teardown: the one that holds the focus Dream restores (the Dream tab's window,
	// §5.5.2), else the output area's, else the active regular window, else the main frame.
	TSharedPtr<SWindow> Return = RegularWindowOf(Policy.FocusToRestore.Pin());
	if (!Return.IsValid())
	{
		Return = RegularWindowOf(Policy.OutputAreaWidget.Pin());
	}
	if (!Return.IsValid())
	{
		Return = AnchorWindow();
	}
	ReturnWindow = Return;
	LastReturnWindowForTest = Return;

	// Step 3: the slow-task modal's popup and activation settings (FeedbackContextEditor.cpp:530-536, RV-02), so the
	// window is never the active regular window that parent_external_window_to_slate parents tool windows to; placed
	// in desktop pixels with no DPI rescaling and no auto-centring (MenuStack.cpp:494-501).
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::Format(LOCTEXT("RunWindow.WindowTitle", "Dream · Run #{0} · {1}"), Plain(Policy.RunNumber), FText::FromString(Policy.TabOrItem)))
		.SizingRule(ESizingRule::FixedSize)
		.AutoCenter(EAutoCenter::None)
		.ScreenPosition(FVector2D(Rect.Left, Rect.Top))
		.ClientSize(FVector2D(Rect.Right - Rect.Left, Rect.Bottom - Rect.Top))
		.AdjustInitialSizeAndPositionForDPIScale(false)
		.IsPopupWindow(true)
		.CreateTitleBar(false)
		.HasCloseButton(false)
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		.ActivationPolicy(bFocus ? EWindowActivationPolicy::Always : EWindowActivationPolicy::Never)
		.FocusWhenFirstShown(bFocus);
	Window->SetContent(Content);
	// Alt+F4 and every other close request do nothing while the run is active; Teardown clears this.
	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride::CreateLambda([](const TSharedRef<SWindow>&)
	{
		UE_LOG(LogDream, Verbose, TEXT("The Run window refuses to close while the run is active."));
	}));

	TSharedPtr<const SWidget> Parent = Policy.bOutputAreaVisible ? Policy.OutputAreaWidget.Pin() : nullptr;
	if (!Parent.IsValid())
	{
		Parent = AnchorWindow();
	}
	// Step 5: non-blocking (bSlowTaskWindow), created hidden, so ShowWindow (07b Q3).
	App.AddModalWindow(Window, Parent, /*bSlowTaskWindow=*/true);
	if (App.GetActiveModalWindow() != Window)
	{
		UE_LOG(LogDream, Warning, TEXT("Run #%d: the Run window could not be added as a modal window."), Policy.RunNumber);
		return nullptr;
	}
	Window->ShowWindow();
	return Window;
}

void SDreamRunWindow::GrowForNotes(const TSharedPtr<SWindow>& Window, SDreamRunWindow& Content, const FDreamRunPolicy& Policy)
{
	using namespace DreamRunWindowPrivate;
	if (!Window.IsValid())
	{
		return;
	}
	const float Scale = (Policy.bOutputAreaVisible && Policy.DpiScale > 0.f) ? Policy.DpiScale : Window->GetDPIScaleFactor();
	const float Needed = Content.ComputeMinimumHeight() * Scale;
	const FVector2D Pos = Window->GetPositionInScreen();
	const FVector2D Size = Window->GetSizeInScreen();
	if (Size.Y + 0.5 >= Needed)
	{
		return;                                         // the live view still has 5 lines; never shrinks either
	}
	const float Delta = Needed - static_cast<float>(Size.Y);
	const bool bGrowUp = Policy.bOutputAreaVisible && !Policy.bOutputAreaAtTop;   // toward the code area
	const float Left = static_cast<float>(Pos.X);
	const float Top = static_cast<float>(Pos.Y) - (bGrowUp ? Delta : 0.f);
	const FSlateRect Grown = KeepInWorkArea(FSlateRect(Left, Top, Left + static_cast<float>(Size.X), Top + static_cast<float>(Size.Y) + Delta));
	Window->ReshapeWindow(Grown);
}

// =====================================================================================================================
// Teardown (§5.5.3 steps 1-4)
// =====================================================================================================================

bool SDreamRunWindow::Teardown(TSharedPtr<SWindow>& Window)
{
	using namespace DreamRunWindowPrivate;
	if (!Window.IsValid())
	{
		return false;
	}
	bool bWasForeground = false;
	{
		const TSharedRef<SWindow> RunWindow = Window.ToSharedRef();
		Window.Reset();

		// 1. Detach the Slate child windows; destroying a window destroys the children still in its list
		//    (SlateApplication.cpp:2435-2456). The return value is false for a direct child (SWindow.cpp:1896-1911).
		const TArray<TSharedRef<SWindow>> Kept = RunWindow->GetChildWindows();
		for (const TSharedRef<SWindow>& Child : Kept)
		{
			(void)RunWindow->RemoveDescendantWindow(Child);
			KeepChild(Child);
		}

#if PLATFORM_WINDOWS
		// 2. Re-own the HWNDs the Run window owns (PySide or Qt tool windows, and the detached children's natives).
		ReownToMainFrame(RunWindow);
#endif

		// 3a. Hand activation back to the window the run came from before the destroy: an active window that is destroyed
		//     lets the OS activate the next one in its Z-order (an owned floating window such as Message Log), and Slate
		//     would follow it. Enabled first (the modal disabled it; the destroy re-enables every window anyway), then
		//     activated; its activation restores the focus it had when the Run window took over. Only when the Run window
		//     itself is the OS foreground window, the one case where its destroy makes the OS choose (review r6 m3): a tool
		//     window the run made active (an unparented PySide window, another of the process's windows) or another app keeps
		//     the foreground; FApp::HasFocus is process-wide and would push such a window behind the editor. The run's user
		//     code has ended (finish step 3), so this is no focus change during the run (RT-05).
		const TSharedPtr<SWindow> Return = ReturnWindow.Pin();
		ReturnWindow.Reset();
		const TSharedPtr<FGenericWindow> RunNative = RunWindow->GetNativeWindow();
		const bool bRunWindowForeground = RunNative.IsValid() && RunNative->IsForegroundWindow();
		bWasForeground = bRunWindowForeground;
		const bool bHandBack = Return.IsValid() && Return != RunWindow && bRunWindowForeground && Return->IsVisible()
			&& !Return->IsWindowMinimized() && !FSlateApplication::Get().IsWindowInDestroyQueue(Return.ToSharedRef());
		if (bHandBack)
		{
			Return->EnableWindow(true);
			Return->BringToFront(/*bForce*/ true);
		}

		// 3. Clear the destroy override; RequestDestroyWindow destroys at once and pops the modal stack.
		RunWindow->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		RunWindow->RequestDestroyWindow();
		if (bHandBack)
		{
			Return->BringToFront(/*bForce*/ true);       // the destroy left activation elsewhere after all: take it back
		}
	}   // Dream's last reference to the window is released here, so the children's parent pointers can expire

	// 4. Re-parent the kept children to the root window; never while a child's old parent is alive (SWindow.cpp:1863-1872).
	ReparentKeptChildWindows();
	if (KeptChildWindows.Num() > 0 && !bReparentTickerArmed)
	{
		bReparentTickerArmed = true;
		FDreamDeferral::AddTicker(TEXT("DreamRunWindowReparent"), EDreamDeferPolicy::DeferWhileExecuting, 0.f, /*bNeedsPython=*/false,
			[](float)
			{
				ReparentKeptChildWindows();
				const bool bKeep = KeptChildWindows.Num() > 0;
				bReparentTickerArmed = bKeep;
				return bKeep;
			});
	}
	return bWasForeground;
}

void SDreamRunWindow::ReparentKeptChildWindows()
{
	using namespace DreamRunWindowPrivate;
	if (KeptChildWindows.Num() == 0 || !FSlateApplication::IsInitialized())
	{
		return;
	}
	const TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
	if (!Root.IsValid())
	{
		return;
	}
	for (int32 Index = KeptChildWindows.Num() - 1; Index >= 0; --Index)
	{
		const TSharedRef<SWindow> Child = KeptChildWindows[Index].Window;
		if (FSlateApplication::Get().IsWindowInDestroyQueue(Child))
		{
			KeptChildWindows.RemoveAt(Index);             // being closed: nothing to keep
			continue;
		}
		if (Child->GetParentWindow().IsValid())
		{
			continue;                                     // the old parent is still referenced: retry later
		}
		Child->GetOnWindowClosedEvent().Remove(KeptChildWindows[Index].ClosedHandle);
		KeptChildWindows.RemoveAt(Index);
		if (ensure(!Child->GetParentWindow().IsValid()))
		{
			Root->AddChildWindow(Child);
		}
	}
}

TSharedPtr<SWindow> SDreamRunWindow::GetLastReturnWindowForTest()
{
	return DreamRunWindowPrivate::LastReturnWindowForTest.Pin();
}

void SDreamRunWindow::ReleaseKeptChildWindows()
{
	using namespace DreamRunWindowPrivate;
	for (const FKeptChild& Entry : KeptChildWindows)
	{
		Entry.Window->GetOnWindowClosedEvent().Remove(Entry.ClosedHandle);
	}
	KeptChildWindows.Empty();
}

#undef LOCTEXT_NAMESPACE
