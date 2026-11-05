// SPDX-License-Identifier: Apache-2.0
// The Paused window's content (TDD §8.3.5, DBG-03). Debug group (PO-06 F5).
#include "Debug/SDreamPausedWindow.h"

#include "Commands/DreamCommands.h"
#include "Debug/SDreamBreakpointLane.h"
#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Misc/Paths.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamIcons.h"   // (icons)
#include "Shell/DreamTheme.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/STableRow.h"

#include <string>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamPausedWindowPrivate
{
	std::string ToUtf8(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return std::string(Utf8.Get(), Utf8.Length());
	}

	FString FromUtf8(const std::string& Text)
	{
		return FString(UTF8_TO_TCHAR(Text.c_str()));
	}

	FSlateFontInfo UiFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", 9);
	}

	FSlateFontInfo BoldFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 9);
	}

	FString ShortName(const FString& Source)
	{
		if (Source.StartsWith(TEXT("<")))
		{
			return Source;
		}
		return FPaths::GetCleanFilename(Source);
	}

	bool MatchesCommand(EDreamCommand Command, const FKeyEvent& Event)
	{
		if (!FDreamCommands::IsRegistered())
		{
			return false;
		}
		const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(Command);
		if (!Info.IsValid())
		{
			return false;
		}
		const FInputChord Chord(Event.GetKey(), EModifierKey::FromBools(Event.IsControlDown(), Event.IsAltDown(), Event.IsShiftDown(), Event.IsCommandDown()));
		return Info->HasActiveChord(Chord);
	}

	/**
	 * Review q1 m8 / ruling R-q1-6: the Paused window is a popup window (RV-02), which has no OS title bar, so its header
	 * moves it (Move) and a grip in the bottom-right corner resizes it (Resize). Buttons inside handle their own clicks.
	 */
	class SWindowGrip final : public SCompoundWidget
	{
	public:
		enum class EMode : uint8 { Move, Resize };

		SLATE_BEGIN_ARGS(SWindowGrip) : _Mode(EMode::Move) {}
			SLATE_ARGUMENT(EMode, Mode)
			SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Mode = InArgs._Mode;
			SetCursor(Mode == EMode::Move ? EMouseCursor::CardinalCross : EMouseCursor::ResizeSouthEast);
			ChildSlot[InArgs._Content.Widget];
		}

		virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
		{
			const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared());
			if (Event.GetEffectingButton() != EKeys::LeftMouseButton || !Window.IsValid())
			{
				return FReply::Unhandled();
			}
			GrabScreen = Event.GetScreenSpacePosition();
			StartPosition = Window->GetPositionInScreen();
			StartSize = Window->GetSizeInScreen();
			bDragging = true;
			return FReply::Handled().CaptureMouse(AsShared());
		}

		virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& Event) override
		{
			if (!bDragging || !HasMouseCapture())
			{
				return FReply::Unhandled();
			}
			const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared());
			if (!Window.IsValid())
			{
				return FReply::Unhandled();
			}
			const FVector2D Delta = Event.GetScreenSpacePosition() - GrabScreen;
			if (Mode == EMode::Move)
			{
				Window->MoveWindowTo(StartPosition + Delta);
			}
			else
			{
				const FVector2D Size(FMath::Max(StartSize.X + Delta.X, 480.0), FMath::Max(StartSize.Y + Delta.Y, 300.0));
				Window->ReshapeWindow(StartPosition, Size);
			}
			return FReply::Handled();
		}

		virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent&) override
		{
			if (!bDragging)
			{
				return FReply::Unhandled();
			}
			bDragging = false;
			return FReply::Handled().ReleaseMouseCapture();
		}

		virtual void OnMouseCaptureLost(const FCaptureLostEvent&) override
		{
			bDragging = false;
		}

	private:
		EMode Mode = EMode::Move;
		bool bDragging = false;
		FVector2D GrabScreen = FVector2D::ZeroVector;
		FVector2D StartPosition = FVector2D::ZeroVector;
		FVector2D StartSize = FVector2D::ZeroVector;
	};

	/** The source view's marks column: breakpoint dot, execution arrow; a click toggles the line's breakpoint. */
	class SSourceMark final : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSourceMark) {}
			SLATE_ARGUMENT(FString, Source)
			SLATE_ARGUMENT(int32, Line)
			SLATE_ARGUMENT(int32, CurrentLine)
			SLATE_ARGUMENT(TSharedPtr<const TSet<int32>>, Breakpoints)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Source = InArgs._Source;
			Line = InArgs._Line;
			CurrentLine = InArgs._CurrentLine;
			Breakpoints = InArgs._Breakpoints;
		}

		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(16.0, 16.0); }
		virtual bool ComputeVolatility() const override { return true; }

		virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
		{
			if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
			{
				return FReply::Unhandled();
			}
			FDreamDebugger::Get().ToggleBreakpointAtSource(Source, Line);
			return FReply::Handled();
		}

		virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out, int32 LayerId,
			const FWidgetStyle&, bool) const override
		{
			const float Size = static_cast<float>(FMath::Min(Geometry.GetLocalSize().X, Geometry.GetLocalSize().Y));
			const FVector2f Origin(0.f, (static_cast<float>(Geometry.GetLocalSize().Y) - Size) * 0.5f);
			if (Breakpoints.IsValid() && Breakpoints->Contains(Line))
			{
				DreamDebugPaint::DrawBreakpoint(Out, LayerId, Geometry, Origin, Size, false);
			}
			if (Line == CurrentLine)
			{
				DreamDebugPaint::DrawArrow(Out, LayerId + 1, Geometry, Origin, Size);
			}
			return LayerId + 2;
		}

	private:
		FString Source;
		int32 Line = 0;
		int32 CurrentLine = 0;
		TSharedPtr<const TSet<int32>> Breakpoints;
	};
}

void SDreamPausedWindow::Construct(const FArguments& InArgs)
{
	using namespace DreamPausedWindowPrivate;
	Stop = InArgs._Stop;
	for (const FDreamDebugFrame& Frame : Stop.Frames)
	{
		Frames.Add(MakeShared<FDreamDebugFrame>(Frame));
	}

	SAssignNew(FrameList, SListView<TSharedPtr<FDreamDebugFrame>>)
		.ListItemsSource(&Frames)
		.ListViewStyle(&FDreamThemes::ListViewStyle())
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SDreamPausedWindow::MakeFrameRow)
		.OnSelectionChanged(this, &SDreamPausedWindow::OnFrameSelected);

	SAssignNew(VariableTree, STreeView<TSharedPtr<FDreamVariableNode>>)
		.TreeItemsSource(&Roots)
		.TreeViewStyle(&FDreamThemes::ListViewStyle())
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SDreamPausedWindow::MakeVariableRow)
		.OnGetChildren(this, &SDreamPausedWindow::GetVariableChildren)
		.OnExpansionChanged(this, &SDreamPausedWindow::OnVariableExpansion);

	SAssignNew(SourceList, SListView<TSharedPtr<FDreamSourceLine>>)
		.ListItemsSource(&SourceLines)
		.ListViewStyle(&FDreamThemes::ListViewStyle())
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SDreamPausedWindow::MakeSourceRow)
		.OnSelectionChanged_Lambda([this](TSharedPtr<FDreamSourceLine> Line, ESelectInfo::Type)
		{
			SelectedSourceLine = Line.IsValid() ? Line->Line : 0;
		});

	const bool bCanStep = Stop.bCanStep;
	auto SectionLabel = [](const FText& Text)
	{
		return SNew(STextBlock).Text(Text).Font(DreamPausedWindowPrivate::BoldFont()).ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeMutedText));
	};

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground))
		.Padding(0.f)
		[
			SNew(SVerticalBox)
			// Header: what paused and where, then the step controls (PO-04: one slim row, flat buttons).
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SWindowGrip)
				.Mode(SWindowGrip::EMode::Move)
				[
				SNew(SBox)
				.Padding(FMargin(8.f, 6.f))
				[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						// (icons) PO-06 F6: the Paused window's icon (a sleeping crescent with the pause bars) before the header.
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 6.f, 0.f))
						[
							SNew(SImage)
							.Image(FDreamIcons::Brush(EDreamIcon::PausedWindow))
							.DesiredSizeOverride(FVector2D(16.0, 16.0))
							.ColorAndOpacity(FDreamIcons::TintAttr(EDreamIcon::PausedWindow))
						]
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(this, &SDreamPausedWindow::HeaderText)
							.Font(BoldFont())
							.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
						]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(Stop.Description))
						.Visibility(Stop.Description.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
						.Font(UiFont())
						.ColorAndOpacity(FDreamThemes::ColorAttr(Stop.Reason == TEXT("exception") ? EDreamThemeSlot::OutError : EDreamThemeSlot::ChromeMutedText))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
				[
					MakeButton(LOCTEXT("Paused.Continue", "Continue"), LOCTEXT("Paused.ContinueTip", "Continue to the next breakpoint (F5)"), EDreamResumeKind::Continue, false)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
				[
					MakeButton(LOCTEXT("Paused.StepOver", "Step Over"), LOCTEXT("Paused.StepOverTip", "Run this line and stop at the next one (F10)"), EDreamResumeKind::StepOver, true)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
				[
					MakeButton(LOCTEXT("Paused.StepInto", "Step Into"), LOCTEXT("Paused.StepIntoTip", "Stop at the first line of a function this line calls (F11)"), EDreamResumeKind::StepInto, true)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
				[
					MakeButton(LOCTEXT("Paused.StepOut", "Step Out"), LOCTEXT("Paused.StepOutTip", "Run to the end of this function and stop in its caller (Shift+F11)"), EDreamResumeKind::StepOut, true)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6.f, 0.f, 0.f, 0.f)
				[
					MakeButton(LOCTEXT("Paused.Stop", "Stop"), LOCTEXT("Paused.StopTip", "Stop the run (Shift+F5 or Esc)"), EDreamResumeKind::Stop, false)
				]
				]
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				+ SSplitter::Slot()
				.Value(0.42f)
				[
					SNew(SSplitter)
					.Orientation(Orient_Vertical)
					+ SSplitter::Slot()
					.Value(0.35f)
					[
						SNew(SBorder)
						.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::OutputBackground))
						.Padding(4.f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 0.f, 0.f, 4.f)
							[
								SectionLabel(LOCTEXT("Paused.CallStack", "Call Stack"))
							]
							+ SVerticalBox::Slot().FillHeight(1.f)
							[
								FrameList.ToSharedRef()
							]
						]
					]
					+ SSplitter::Slot()
					.Value(0.65f)
					[
						SNew(SBorder)
						.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::OutputBackground))
						.Padding(4.f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 0.f, 0.f, 4.f)
							[
								SectionLabel(LOCTEXT("Paused.Variables", "Variables"))
							]
							+ SVerticalBox::Slot().FillHeight(1.f)
							[
								VariableTree.ToSharedRef()
							]
						]
					]
				]
				+ SSplitter::Slot()
				.Value(0.58f)
				[
					SNew(SBorder)
					.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::EditorBackground))
					.Padding(4.f)
					[
						SourceList.ToSharedRef()
					]
				]
			]
			// Footer (U11, REPL-07; RV-16 moved the REPL message here).
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.Padding(FMargin(8.f, 4.f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Paused.Footer", "The editor is paused. Viewports and other windows resume when you continue. The REPL is suspended."))
					.Font(UiFont())
					.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeMutedText))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Bottom)
				[
					SNew(SWindowGrip)
					.Mode(SWindowGrip::EMode::Resize)
					[
						SNew(SBox)
						.WidthOverride(16.f)
						.HeightOverride(16.f)
						.ToolTipText(LOCTEXT("Paused.ResizeTip", "Drag to resize the Paused window"))
						[
							SNew(SBorder)
							.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBorder))
						]
					]
				]
			]
		]
	];
	(void)bCanStep;
	BreakpointsChangedHandle = FDreamDebugger::Get().OnBreakpointsChanged().AddSP(this, &SDreamPausedWindow::RefreshBreakpointCache);

	if (Frames.Num() > 0)
	{
		FrameList->SetSelection(Frames[0], ESelectInfo::Direct);
		SelectFrame(0);
	}
	else
	{
		LoadSource(Stop.Source, Stop.Line);
	}
}

SDreamPausedWindow::~SDreamPausedWindow()
{
	if (IsInGameThread())
	{
		FDreamDebugger::Get().OnBreakpointsChanged().Remove(BreakpointsChangedHandle);
	}
}

void SDreamPausedWindow::RefreshBreakpointCache()
{
	ShownBreakpoints->Reset();
	if (!ShownSource.IsEmpty())
	{
		ShownBreakpoints->Append(FDreamDebugger::Get().GetSourceBreakpointLines(ShownSource));
	}
}

FText SDreamPausedWindow::HeaderText() const
{
	return FText::Format(LOCTEXT("Paused.Header", "Paused · Run #{0} · {1}:{2} · {3}"),
		FText::AsNumber(Stop.RunNumber, &FNumberFormattingOptions::DefaultNoGrouping()),
		FText::FromString(DreamPausedWindowPrivate::ShortName(Stop.Source)),
		FText::AsNumber(Stop.Line, &FNumberFormattingOptions::DefaultNoGrouping()),
		FText::FromString(Stop.Reason));
}

TSharedRef<SWidget> SDreamPausedWindow::MakeButton(const FText& Label, const FText& Tip, EDreamResumeKind Kind, bool bStep)
{
	const bool bEnabled = !bStep || Stop.bCanStep;
	return SNew(SButton)
		.ButtonStyle(&FDreamThemes::FlatButtonStyle())
		.ToolTipText(bEnabled ? Tip : LOCTEXT("Paused.NoStepAtException", "Stepping is not available at an exception stop: Continue ends the run, Stop cancels it"))
		.IsEnabled(bEnabled)
		.ContentPadding(FMargin(8.f, 2.f))
		.OnClicked_Lambda([Kind]()
		{
			FDreamDebugger::Get().RequestResume(Kind);
			return FReply::Handled();
		})
		[
			// (icons) PO-06 F6: the control's icon before its label (labels kept): Continue, Step Over / Into / Out in the accent,
			// Stop in the theme's danger colour.
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 5.f, 0.f))
			[
				SNew(SImage)
				.Image(FDreamIcons::Brush(Kind == EDreamResumeKind::Continue ? EDreamIcon::Continue
					: Kind == EDreamResumeKind::StepOver ? EDreamIcon::StepOver
					: Kind == EDreamResumeKind::StepInto ? EDreamIcon::StepInto
					: Kind == EDreamResumeKind::StepOut ? EDreamIcon::StepOut
					: EDreamIcon::Stop))
				.DesiredSizeOverride(FVector2D(14.0, 14.0))
				.ColorAndOpacity(FDreamThemes::ColorAttr(Kind == EDreamResumeKind::Stop ? EDreamThemeSlot::Danger : EDreamThemeSlot::Accent))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(DreamPausedWindowPrivate::UiFont())
				.ColorAndOpacity(FDreamThemes::ColorAttr(Kind == EDreamResumeKind::Stop ? EDreamThemeSlot::Danger : EDreamThemeSlot::ChromeText))
			]
		];
}

FReply SDreamPausedWindow::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	using namespace DreamPausedWindowPrivate;
	if (InKeyEvent.IsRepeat())
	{
		return FReply::Handled();
	}
	FDreamDebugger& Debugger = FDreamDebugger::Get();
	if (MatchesCommand(EDreamCommand::Stop, InKeyEvent) || (InKeyEvent.GetKey() == EKeys::Escape && !InKeyEvent.IsShiftDown()
		&& !InKeyEvent.IsControlDown() && !InKeyEvent.IsAltDown()))
	{
		Debugger.RequestResume(EDreamResumeKind::Stop);
		return FReply::Handled();
	}
	if (MatchesCommand(EDreamCommand::DebugRun, InKeyEvent))
	{
		Debugger.RequestResume(EDreamResumeKind::Continue);
		return FReply::Handled();
	}
	if (Stop.bCanStep)
	{
		if (MatchesCommand(EDreamCommand::DebugStepOver, InKeyEvent))
		{
			Debugger.RequestResume(EDreamResumeKind::StepOver);
			return FReply::Handled();
		}
		if (MatchesCommand(EDreamCommand::DebugStepInto, InKeyEvent))
		{
			Debugger.RequestResume(EDreamResumeKind::StepInto);
			return FReply::Handled();
		}
		if (MatchesCommand(EDreamCommand::DebugStepOut, InKeyEvent))
		{
			Debugger.RequestResume(EDreamResumeKind::StepOut);
			return FReply::Handled();
		}
	}
	if (MatchesCommand(EDreamCommand::ToggleBreakpoint, InKeyEvent))
	{
		const int32 Line = SelectedSourceLine > 0 ? SelectedSourceLine : CurrentLineOfShownSource();
		if (Line > 0 && !ShownSource.IsEmpty())
		{
			Debugger.ToggleBreakpointAtSource(ShownSource, Line);
		}
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

// ---- Call stack ------------------------------------------------------------------------------------------------------

TSharedRef<ITableRow> SDreamPausedWindow::MakeFrameRow(TSharedPtr<FDreamDebugFrame> Frame, const TSharedRef<STableViewBase>& Owner)
{
	using namespace DreamPausedWindowPrivate;
	const FText Text = FText::Format(LOCTEXT("Paused.FrameRow", "{0}   {1}:{2}"), FText::FromString(Frame->Name),
		FText::FromString(ShortName(Frame->Source)), FText::AsNumber(Frame->Line, &FNumberFormattingOptions::DefaultNoGrouping()));
	return SNew(STableRow<TSharedPtr<FDreamDebugFrame>>, Owner)
		.Style(&FDreamThemes::TableRowStyle())
		[
			SNew(STextBlock)
			.Text(Text)
			.ToolTipText(FText::FromString(Frame->Source))
			.Font(UiFont())
			.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
		];
}

void SDreamPausedWindow::OnFrameSelected(TSharedPtr<FDreamDebugFrame> Frame, ESelectInfo::Type Info)
{
	if (!Frame.IsValid() || Info == ESelectInfo::Direct)
	{
		return;
	}
	SelectFrame(Frames.IndexOfByKey(Frame));
}

void SDreamPausedWindow::SelectFrame(int32 Index)
{
	using namespace dream::json;
	if (!Frames.IsValidIndex(Index))
	{
		return;
	}
	SelectedFrame = Index;
	const TSharedPtr<FDreamDebugFrame> Frame = Frames[Index];
	LoadSource(Frame->Source, Frame->Line);
	Roots.Reset();
	VariableTree->RequestTreeRefresh();

	Value Args = Value::MakeObject();
	Args.Set("frameId", Value::MakeInt(Frame->Id));
	const TWeakPtr<SDreamPausedWindow> Weak = SharedThis(this);
	const int32 Epoch = Index;
	FDreamDebugger::Get().QueueRequest(TEXT("scopes"), MoveTemp(Args), [Weak, Epoch](const Value& Response)
	{
		const TSharedPtr<SDreamPausedWindow> Self = Weak.Pin();
		if (!Self.IsValid() || Self->SelectedFrame != Epoch)
		{
			return;
		}
		const Value& Scopes = Response.Get("body").Get("scopes");
		for (size_t I = 0; I < Scopes.Size(); ++I)
		{
			const Value& Scope = Scopes.At(I);
			TSharedPtr<FDreamVariableNode> Node = MakeShared<FDreamVariableNode>();
			Node->Name = DreamPausedWindowPrivate::FromUtf8(Scope.Get("name").AsString());
			Node->Reference = Scope.Get("variablesReference").AsInt(0);
			Node->bScope = true;
			Self->Roots.Add(Node);
		}
		Self->VariableTree->RequestTreeRefresh();
		// Locals (and Special at an exception stop) open at once; Globals waits for a click (it can be the whole Session).
		for (const TSharedPtr<FDreamVariableNode>& Node : Self->Roots)
		{
			if (Node->Name != TEXT("Globals"))
			{
				Self->VariableTree->SetItemExpansion(Node, true);
				Self->RequestChildren(Node);
			}
		}
	});
}

// ---- Variables -------------------------------------------------------------------------------------------------------

TSharedRef<ITableRow> SDreamPausedWindow::MakeVariableRow(TSharedPtr<FDreamVariableNode> Node, const TSharedRef<STableViewBase>& Owner)
{
	using namespace DreamPausedWindowPrivate;
	TSharedRef<STableRow<TSharedPtr<FDreamVariableNode>>> Row = SNew(STableRow<TSharedPtr<FDreamVariableNode>>, Owner)
		.Style(&FDreamThemes::TableRowStyle());
	Row->SetContent(
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SExpanderArrow, Row)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox)
			.MinDesiredWidth(Node->bScope ? 0.f : 120.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Node->Name))
				.Font(Node->bScope ? BoldFont() : FDreamEditorStyle::GetCodeFont(9))
				.ColorAndOpacity(FDreamThemes::ColorAttr(Node->bScope ? EDreamThemeSlot::ChromeMutedText : EDreamThemeSlot::SynNormal))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Node->Value))
			.ToolTipText(FText::FromString(Node->Type.IsEmpty() ? Node->Value : Node->Type + TEXT(": ") + Node->Value))
			.Font(FDreamEditorStyle::GetCodeFont(9))
			.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::OutResult))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 4.f, 0.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Node->Type))
			.Font(UiFont())
			.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeMutedText))
		]);
	return Row;
}

void SDreamPausedWindow::GetVariableChildren(TSharedPtr<FDreamVariableNode> Node, TArray<TSharedPtr<FDreamVariableNode>>& OutChildren)
{
	if (!Node.IsValid() || Node->Reference <= 0)
	{
		return;
	}
	if (!Node->bLoaded)
	{
		// A placeholder so the expander shows; expanding asks for the real children (lazy, §8.8). One per parent: a tree
		// item may appear only once (SListView::ValidateWidgetGeneration).
		if (!Node->Placeholder.IsValid())
		{
			Node->Placeholder = MakeShared<FDreamVariableNode>();
			Node->Placeholder->Name = TEXT("…");
		}
		OutChildren.Add(Node->Placeholder);
		return;
	}
	OutChildren = Node->Children;
}

void SDreamPausedWindow::OnVariableExpansion(TSharedPtr<FDreamVariableNode> Node, bool bExpanded)
{
	if (bExpanded)
	{
		RequestChildren(Node);
	}
}

void SDreamPausedWindow::ExpandForTest(const TSharedPtr<FDreamVariableNode>& Node)
{
	if (Node.IsValid())
	{
		VariableTree->SetItemExpansion(Node, true);
		RequestChildren(Node);
	}
}

void SDreamPausedWindow::RequestChildren(const TSharedPtr<FDreamVariableNode>& Node)
{
	using namespace dream::json;
	if (!Node.IsValid() || Node->Reference <= 0 || Node->bRequested)
	{
		return;
	}
	Node->bRequested = true;
	Value Args = Value::MakeObject();
	Args.Set("variablesReference", Value::MakeInt(Node->Reference));
	const TWeakPtr<SDreamPausedWindow> Weak = SharedThis(this);
	const TWeakPtr<FDreamVariableNode> WeakNode = Node;
	FDreamDebugger::Get().QueueRequest(TEXT("variables"), MoveTemp(Args), [Weak, WeakNode](const Value& Response)
	{
		const TSharedPtr<SDreamPausedWindow> Self = Weak.Pin();
		const TSharedPtr<FDreamVariableNode> Target = WeakNode.Pin();
		if (!Self.IsValid() || !Target.IsValid())
		{
			return;
		}
		Target->Children.Reset();
		if (!Response.Get("success").AsBool(false))
		{
			TSharedPtr<FDreamVariableNode> Error = MakeShared<FDreamVariableNode>();
			Error->Name = DreamPausedWindowPrivate::FromUtf8(Response.Get("message").AsString());
			Target->Children.Add(Error);
		}
		const Value& Items = Response.Get("body").Get("variables");
		for (size_t I = 0; I < Items.Size(); ++I)
		{
			const Value& Item = Items.At(I);
			TSharedPtr<FDreamVariableNode> Child = MakeShared<FDreamVariableNode>();
			Child->Name = DreamPausedWindowPrivate::FromUtf8(Item.Get("name").AsString());
			Child->Value = DreamPausedWindowPrivate::FromUtf8(Item.Get("value").AsString());
			Child->Type = DreamPausedWindowPrivate::FromUtf8(Item.Get("type").AsString());
			Child->Reference = Item.Get("variablesReference").AsInt(0);
			Target->Children.Add(Child);
		}
		Target->bLoaded = true;
		Self->VariableTree->RequestTreeRefresh();
	});
}

// ---- Source view -----------------------------------------------------------------------------------------------------

int32 SDreamPausedWindow::CurrentLineOfShownSource() const
{
	return Frames.IsValidIndex(SelectedFrame) ? Frames[SelectedFrame]->Line : Stop.Line;
}

void SDreamPausedWindow::LoadSource(const FString& Source, int32 Line)
{
	using namespace dream::json;
	if (Source == ShownSource && SourceLines.Num() > 0)
	{
		if (SourceLines.IsValidIndex(Line - 1))
		{
			SourceList->RequestScrollIntoView(SourceLines[Line - 1]);
		}
		SourceList->RebuildList();
		return;
	}
	ShownSource = Source;
	RefreshBreakpointCache();
	SourceLines.Reset();
	SourceList->RequestListRefresh();
	Value Args = Value::MakeObject();
	Value SourceValue = Value::MakeObject();
	SourceValue.Set("path", Value::MakeString(DreamPausedWindowPrivate::ToUtf8(Source)));
	Args.Set("source", MoveTemp(SourceValue));
	const TWeakPtr<SDreamPausedWindow> Weak = SharedThis(this);
	FDreamDebugger::Get().QueueRequest(TEXT("source"), MoveTemp(Args), [Weak, Source, Line](const Value& Response)
	{
		const TSharedPtr<SDreamPausedWindow> Self = Weak.Pin();
		if (!Self.IsValid() || Self->ShownSource != Source)
		{
			return;
		}
		const FString Content = DreamPausedWindowPrivate::FromUtf8(Response.Get("body").Get("content").AsString());
		TArray<FString> Lines;
		Content.ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty=*/false);
		Self->SourceLines.Reset();
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			TSharedPtr<FDreamSourceLine> Item = MakeShared<FDreamSourceLine>();
			Item->Line = Index + 1;
			Item->Text = Lines[Index].Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\t"), TEXT("    "));
			Self->SourceLines.Add(Item);
		}
		Self->SourceList->RequestListRefresh();
		if (Self->SourceLines.IsValidIndex(Line - 1))
		{
			Self->SourceList->RequestScrollIntoView(Self->SourceLines[Line - 1]);
		}
	});
}

TSharedRef<ITableRow> SDreamPausedWindow::MakeSourceRow(TSharedPtr<FDreamSourceLine> Line, const TSharedRef<STableViewBase>& Owner)
{
	using namespace DreamPausedWindowPrivate;
	const int32 Current = CurrentLineOfShownSource();
	const bool bCurrent = Line->Line == Current;
	return SNew(STableRow<TSharedPtr<FDreamSourceLine>>, Owner)
		.Style(&FDreamThemes::TableRowStyle())
		[
			SNew(SBorder)
			.BorderImage(FDreamEditorStyle::Get().GetBrush(TEXT("DreamEditor.Editor.CurrentLineBrush")))
			.BorderBackgroundColor(bCurrent ? FSlateColor(DreamDebugPaint::PausedBandColor()) : FSlateColor(FLinearColor::Transparent))
			.Padding(0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SSourceMark).Source(ShownSource).Line(Line->Line).CurrentLine(Current).Breakpoints(ShownBreakpoints)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f, 8.f, 0.f)
				[
					SNew(SBox)
					.MinDesiredWidth(32.f)
					.HAlign(HAlign_Right)
					[
						SNew(STextBlock)
						.Text(FText::AsNumber(Line->Line, &FNumberFormattingOptions::DefaultNoGrouping()))
						.Font(FDreamEditorStyle::GetCodeFont(9))
						.ColorAndOpacity(FDreamThemes::ColorAttr(bCurrent ? EDreamThemeSlot::ChromeText : EDreamThemeSlot::GutterText))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Line->Text))
					.Font(FDreamEditorStyle::GetCodeFont(9))
					.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::SynNormal))
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
