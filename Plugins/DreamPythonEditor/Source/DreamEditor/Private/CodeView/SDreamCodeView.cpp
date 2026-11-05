// SPDX-License-Identifier: Apache-2.0
// Plain mode (TDD §4.15 row 3, EDT-15): the stock SMultiLineEditableText with the PyLexer marshaller, a gutter, a
// current-line highlight and the monospace font. Keys run OnKeyHook, then FDreamKeyRouter::RoutePre from OnKeyDownHandler
// (before the stock handling) and RouteBubble from this widget's OnKeyDown (the consume list), so a Dream chord never
// reaches the Level Editor (D8). Every stock edit is reflected into the buffer's TextBuffer as one DiffSingleRange edit.
#include "CodeView/SDreamCodeView.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCompletionPopup.h"
#include "CodeView/SDreamDiagnostics.h"   // (lang) PO-06 F4
#include "CodeView/SDreamFindBar.h"
#include "CodeView/SDreamGoToLine.h"
#include "CodeView/SDreamGutter.h"
#include "CodeView/SDreamPlainText.h"
#include "CodeView/DreamPythonMarshaller.h"
#include "Debug/SDreamBreakpointLane.h"      // (debug) PO-06 F5
#include "Algo/BinarySearch.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Text/TextLayout.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Input/Events.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamJournal.h"
#include "Python/DreamPython.h"
#include "Rendering/DrawElements.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamTheme.h"   // (theme)
#include "Styling/AppStyle.h"
#include "Tasks/Task.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "dream/CancelToken.h"
#include "dream/Diff.h"
#include "dream/Keymap.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamCodeViewLocal
{
	constexpr double HighlightDelaySeconds = 0.15;  // re-marshal this long after the last edit
	/** (codeview choice) A model change with more ranges goes into the widget as one whole-text set (one re-layout) instead
	 *  of range edits (each a stock InsertAt). */
	constexpr int32 MaxRangeSyncs = 64;
	constexpr int64 LiveHighlightMaxUnits = 256 * 1024;   // above: no live re-marshal after edits (SHL-13, proposed)
	constexpr int32 ZoomMinPercent = 50;            // EDT-10
	constexpr int32 ZoomMaxPercent = 300;
	constexpr int32 ZoomStepPercent = 10;
	constexpr int32 MinPoints = 4;
	constexpr int32 MaxPoints = 160;                // the 48 pt setting maximum at 300%

	bool IsRunActive()
	{
		return FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetRunService().IsRunActive();
	}

	/** Stock edits taken back because the view was read-only (SDreamCodeView::GetReadOnlyTakeBacks). Game thread. */
	int32 ReadOnlyTakeBacks = 0;

	/** Find workers running now, every view (SDreamCodeView::GetFindWorkersRunning). */
	std::atomic<int32> FindWorkersRunning{0};

	/** What a find worker hands back: a search, or Replace All's batch. */
	struct FFindWork
	{
		FDreamFindResult Search;
		FDreamReplaceAllResult Replace;
	};

	/** Counts a worker while it runs. */
	struct FFindWorkerScope
	{
		FFindWorkerScope() { FindWorkersRunning.fetch_add(1, std::memory_order_relaxed); }
		~FFindWorkerScope() { FindWorkersRunning.fetch_sub(1, std::memory_order_relaxed); }
	};

	void RequestPythonStart(EDreamStartReason Reason)
	{
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetPython().RequestStart(Reason);   // RT-17: acts only in Cold; never waits
		}
	}

	int32 ZoomedPoints(int32 SettingPoints, int32 ZoomPercent)
	{
		return FMath::Clamp(FMath::RoundToInt(static_cast<float>(SettingPoints) * static_cast<float>(ZoomPercent) / 100.f), MinPoints, MaxPoints);
	}

	/** True when a range removes the character at At. */
	bool RemovesCharAt(const dream::ChangeSet& Changes, dream::Offset At)
	{
		for (const dream::ChangeRange& Range : Changes.Ranges)
		{
			if (At >= Range.At && At < Range.At + Range.OldLen)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * The stock text widget's own undo chords (SlateEditableTextLayout.cpp HandleKeyDown and its FGenericCommands list):
	 * Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z, Alt+Backspace, Alt+Shift+Backspace. When one reaches the stock handler unbound (the user
	 * cleared Undo's chord), it is consumed here as a no-op: text undo is UndoTree's alone (EDT-08, D8).
	 */
	bool IsStockUndoChord(const FKeyEvent& KeyEvent)
	{
		const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
		const FKey Key = KeyEvent.GetKey();
		if (Mods.IsControlDown() && !Mods.IsAltDown() && (Key == EKeys::Z || Key == EKeys::Y))
		{
			return true;
		}
		return Key == EKeys::BackSpace && Mods.IsAltDown() && !Mods.IsControlDown();
	}

	/** Paints the caret line's band behind the text (plain mode has no SCodeView paint pass, §4.8.2). */
	class SCurrentLineHighlight final : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SCurrentLineHighlight) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&, const TSharedRef<SDreamCodeView>& InView)
		{
			View = InView;
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			TSharedPtr<SDreamCodeView> Pinned = View.Pin();
			if (!Pinned.IsValid())
			{
				return LayerId;
			}
			TArray<FDreamLineGeometry> Lines;
			Pinned->GetVisibleLines(Lines);
			const int32 Caret = Pinned->GetCaretLine();
			for (const FDreamLineGeometry& Line : Lines)
			{
				if (Line.Line == Caret)
				{
					const FLinearColor Tint = FDreamThemes::LinearColor(EDreamThemeSlot::CurrentLine);   // (theme) read at paint
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
						AllottedGeometry.ToPaintGeometry(FVector2f(static_cast<float>(AllottedGeometry.GetLocalSize().X), Line.Height),
							FSlateLayoutTransform(FVector2f(0.f, Line.Top))),
						FDreamEditorStyle::Get().GetBrush(TEXT("DreamEditor.Editor.CurrentLineBrush")), ESlateDrawEffect::None, Tint);
					break;
				}
			}
			return LayerId + 1;
		}

		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
		virtual bool ComputeVolatility() const override { return true; }

	private:
		TWeakPtr<SDreamCodeView> View;
	};

	/** Paints the find matches of the visible lines behind the text, the selected one stronger (EDT-09). */
	class SFindHighlights final : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SFindHighlights) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&, const TSharedRef<SDreamCodeView>& InView)
		{
			View = InView;
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
			if (!Pinned.IsValid() || !Pinned->IsFindBarOpen() || Pinned->GetFindMatches().Num() == 0 || !Pinned->GetBuffer().IsValid())
			{
				return LayerId;
			}
			TArray<FDreamLineGeometry> Lines;
			Pinned->GetVisibleLines(Lines);
			if (Lines.Num() == 0)
			{
				return LayerId;
			}
			const dream::TextSnapshot Snapshot = Pinned->GetBuffer()->Snapshot();
			const dream::Offset VisibleStart = Snapshot.LineStart(Lines[0].Line);
			const dream::Offset VisibleEnd = Snapshot.LineEnd(Lines.Last().Line);
			const TArray<FDreamFindMatch>& Matches = Pinned->GetFindMatches();
			// Matches are ascending and never overlap, so their ends ascend too: start at the first one that ends in view.
			int32 Index = Algo::LowerBoundBy(Matches, VisibleStart, [](const FDreamFindMatch& Match) { return Match.End(); });
			const FSlateBrush* Brush = FDreamEditorStyle::Get().GetBrush(TEXT("DreamEditor.Editor.CurrentLineBrush"));
			const FLinearColor Normal = FDreamThemes::LinearColor(EDreamThemeSlot::FindMatch);      // (theme) read at paint
			const FLinearColor Current = FDreamThemes::LinearColor(EDreamThemeSlot::FindCurrent);
			TArray<FSlateRect> Rects;
			for (; Index < Matches.Num() && Matches[Index].Start <= VisibleEnd; ++Index)
			{
				Rects.Reset();
				Pinned->GetRangeRects(Matches[Index].Start, FMath::Max(Matches[Index].End(), Matches[Index].Start + 1), Rects);
				const FLinearColor& Tint = Index == Pinned->GetCurrentFindMatch() ? Current : Normal;
				for (const FSlateRect& Rect : Rects)
				{
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
						AllottedGeometry.ToPaintGeometry(FVector2f(static_cast<float>(Rect.GetSize().X), static_cast<float>(Rect.GetSize().Y)),
							FSlateLayoutTransform(FVector2f(static_cast<float>(Rect.Left), static_cast<float>(Rect.Top)))),
						Brush, ESlateDrawEffect::None, Tint);
				}
			}
			return LayerId + 1;
		}

		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
		virtual bool ComputeVolatility() const override { return true; }

	private:
		TWeakPtr<SDreamCodeView> View;
	};
}

/** A regex search or Replace All on a UE::Tasks worker (TDD §4.4 Run). The worker owns copies of what it reads (the
 *  snapshot, the compiled query, the token), never the view, so it may outlive the view; its result is then dropped. */
struct FDreamCodeViewFindTask
{
	UE::Tasks::TTask<DreamCodeViewLocal::FFindWork> Task;
	std::shared_ptr<dream::CancelToken> Cancel;
	uint32 Generation = 0;
	uint64 Version = 0;          // the model version the worker reads
	FDreamFindQuery Query;
	bool bSelectFirst = false;
	bool bReplace = false;
	FString Template;            // Replace All
	int32 Attempt = 0;           // Replace All built again after an edit
};

int32 SDreamCodeView::GetReadOnlyTakeBacks()
{
	return DreamCodeViewLocal::ReadOnlyTakeBacks;
}

int32 SDreamCodeView::GetFindWorkersRunning()
{
	return DreamCodeViewLocal::FindWorkersRunning.load(std::memory_order_relaxed);
}

void SDreamCodeView::Construct(const FArguments& InArgs)
{
	using namespace DreamCodeViewLocal;
	Buffer = InArgs._Buffer;
	check(Buffer.IsValid());
	CommandTarget = InArgs._CommandTarget;
	GutterMode = InArgs._GutterMode;
	BaseContextMask = InArgs._ContextMask;
	SettingsFontSize = FMath::Max(MinPoints, InArgs._FontSize);
	FontSize = ZoomedPoints(SettingsFontSize, GetZoomPercent());
	ReadOnlyAttribute = InArgs._IsReadOnly;
	HintText = InArgs._HintText;
	OnKeyHook = InArgs._OnKeyHook;
	OnEdited = InArgs._OnEdited;
	OnCaretMoved = InArgs._OnCaretMoved;
	OnFocused = InArgs._OnFocused;

	Marshaller = FDreamPythonMarshaller::Create(Buffer, FontSize);
	VScrollBar = SNew(SScrollBar).Style(&FDreamThemes::ScrollBarStyle()).Orientation(Orient_Vertical).Thickness(FVector2D(9.0, 9.0));     // (theme)
	HScrollBar = SNew(SScrollBar).Style(&FDreamThemes::ScrollBarStyle()).Orientation(Orient_Horizontal).Thickness(FVector2D(9.0, 9.0));   // (theme)
	ReflectedText = Buffer->GetTextAsString();
	SyncedSnapshot = Buffer->Snapshot();
	DetectIndentStyle();

	bSettingText = true;
	SAssignNew(TextWidget, SDreamPlainText)
		.Text(FText::FromString(ReflectedText))
		.Marshaller(Marshaller)
		.TextStyle(&FDreamEditorStyle::GetCodeTextStyle())
		.Font(GetCodeFont())
		.AutoWrapText(false)
		.Margin(FMargin(4.f, 2.f, 4.f, 2.f))
		.IsReadOnly(TAttribute<bool>::CreateSP(this, &SDreamCodeView::IsReadOnly))
		.HScrollBar(HScrollBar)
		.VScrollBar(VScrollBar)
		.ClearTextSelectionOnFocusLoss(false)
		.ClearKeyboardFocusOnCommit(false)
		.RevertTextOnEscape(false)
		.SelectWordOnMouseDoubleClick(true)
		.OnContextMenuOpening(this, &SDreamCodeView::BuildContextMenu)
		.OnKeyDownHandler(this, &SDreamCodeView::HandleTextKeyDown)
		.OnKeyCharHandler(this, &SDreamCodeView::HandleTextKeyChar)
		.OnTextChanged(this, &SDreamCodeView::HandleTextChanged)
		.OnCursorMoved(this, &SDreamCodeView::HandleCursorMoved);
	bSettingText = false;
	TextWidget->DropStockUndo();
	TextWidget->SetLiveReadOnly(TAttribute<bool>::CreateSP(this, &SDreamCodeView::IsReadOnly));   // A27, RT-01: never a stale value
	TextWidget->SetOnSelectLines([WeakView = TWeakPtr<SDreamCodeView>(SharedThis(this))](int32 AnchorLine, int32 Line)   // (editing)
	{
		if (const TSharedPtr<SDreamCodeView> View = WeakView.Pin())
		{
			View->SelectWholeLines(AnchorLine, Line);
		}
	});
	// (lang) PO-06 F4: live problems (squiggles, bulb, scroll-bar marks, inspection widget) for buffer views; the text's
	// tooltip is the problem under the mouse (an empty text shows no tooltip; Slate asks again every frame).
	Diagnostics = MakeShared<FDreamCodeViewDiagnostics>(SharedThis(this),
		GutterMode == EDreamGutterMode::LineNumbers && Buffer->GetKind() != EDreamBufferKind::ReplInput);
	TextWidget->SetToolTipText(TAttribute<FText>::CreateLambda([WeakDiagnostics = TWeakPtr<FDreamCodeViewDiagnostics>(Diagnostics)]()
	{
		const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = WeakDiagnostics.Pin();
		return Pinned.IsValid() ? Pinned->GetHoverToolTip() : FText::GetEmpty();
	}));
	// The stock Construct set the text but built no line views. They are not built here: the scale is not known before
	// the first paint (a layout at scale 1 would be done again at 150 % DPI). A key that reaches the view before its first
	// paint lays the text out then (HandleTextKeyDown); no click can come before a paint.

	TSharedRef<SWidget> Gutter = GutterMode == EDreamGutterMode::None
		? StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
		: StaticCastSharedRef<SWidget>(SNew(SDreamGutter, SharedThis(this)).Mode(GutterMode));
	if (GutterMode == EDreamGutterMode::LineNumbers)
	{
		// (chrome) PO-04: View ▸ Line Numbers hides the line-number gutter (the prompt's >>> gutter stays).
		Gutter->SetVisibility(TAttribute<EVisibility>::CreateLambda([]()
		{
			return UDreamUserSettings::Get()->bShowLineNumbers ? EVisibility::Visible : EVisibility::Collapsed;
		}));
	}

	const TWeakPtr<SDreamCodeView> WeakSelf = SharedThis(this);
	SAssignNew(GoToLine, SDreamGoToLine)
		.OnAccept_Lambda([WeakSelf](int32 Line, int32 Column)
		{
			if (const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin())
			{
				Self->CloseGoToLine();
				const int32 Target = FMath::Clamp(Line, 0, Self->GetLineCount() - 1);
				// A line without a column goes to its first non-blank column (§7.9.3 line provider, as Run line and advance).
				int32 Col = Column;
				if (Col == INDEX_NONE)
				{
					const FString Text = Self->GetLineText(Target);
					Col = 0;
					while (Col < Text.Len() && (Text[Col] == TEXT(' ') || Text[Col] == TEXT('\t')))
					{
						++Col;
					}
				}
				Self->SetCaret(Target, Col);
				Self->ScrollToLine(Target, 3);
				Self->FocusEditor();
			}
		})
		.OnCancel_Lambda([WeakSelf]()
		{
			if (const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin())
			{
				Self->CloseGoToLine();
			}
		});

	bFindEnabled = InArgs._EnableFind && GutterMode == EDreamGutterMode::LineNumbers;
	TSharedRef<SWidget> FindSlot = SNullWidget::NullWidget;
	if (bFindEnabled)
	{
		SAssignNew(FindBar, SDreamFindBar)
			.StatusText_Lambda([WeakSelf]()
			{
				const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin();
				return Self.IsValid() ? Self->FindStatus : FText::GetEmpty();
			})
			.IsStatusError_Lambda([WeakSelf]()
			{
				const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin();
				return Self.IsValid() && !Self->FindError.IsEmpty();
			})
			.CanReplace_Lambda([WeakSelf]()
			{
				const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin();
				return Self.IsValid() && !Self->IsReadOnly();
			})
			.OnQueryChanged_Lambda([WeakSelf]()
			{
				if (const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin())
				{
					Self->HandleFindQueryChanged();
				}
			})
			.OnStep_Lambda([WeakSelf](int32 Direction, bool bReturnFocus)
			{
				if (const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin())
				{
					Self->FindStep(Direction, bReturnFocus);
				}
			})
			.OnReplace_Lambda([WeakSelf]()
			{
				if (const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin())
				{
					Self->ReplaceCurrent();
				}
			})
			.OnReplaceAll_Lambda([WeakSelf]()
			{
				if (const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin())
				{
					Self->ReplaceAll();
				}
			})
			.OnClose_Lambda([WeakSelf]()
			{
				if (const TSharedPtr<SDreamCodeView> Self = WeakSelf.Pin())
				{
					Self->CloseFindBar();
				}
			});
		FindSlot = SNew(SBox)
			.Visibility_Lambda([this]() { return bFindBarOpen ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				FindBar.ToSharedRef()
			];
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::EditorBackground))   // (theme) retinted in place on a switch
		.Padding(0.f)
		[
			SNew(SVerticalBox)
			// The find bar sits above the text (EDT-09), as PyCharm's does.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				FindSlot
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SHorizontalBox)
					// (debug) PO-06 F5: the breakpoint lane (red dots, the execution arrow; a click toggles), buffers only.
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						GutterMode == EDreamGutterMode::LineNumbers
							? StaticCastSharedRef<SWidget>(SNew(SDreamBreakpointLane, SharedThis(this)))
							: StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						Gutter
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							SNew(SCurrentLineHighlight, SharedThis(this))
						]
						+ SOverlay::Slot()
						[
							SNew(SDreamPausedLineBand, SharedThis(this))   // (debug) the paused line's band
						]
						+ SOverlay::Slot()
						[
							SNew(SFindHighlights, SharedThis(this))
						]
						+ SOverlay::Slot()
						[
							TextWidget.ToSharedRef()
						]
						// (lang) PO-06 F4: squiggles over the text, the quick-fix bulb after the caret line, the inspection
						// widget at the top right (each collapsed or empty while the buffer has no problem).
						+ SOverlay::Slot()
						[
							Diagnostics->MakeSquiggles()
						]
						+ SOverlay::Slot()
						.HAlign(HAlign_Left)
						.VAlign(VAlign_Top)
						.Padding(TAttribute<FMargin>::CreateLambda([WeakDiagnostics = TWeakPtr<FDreamCodeViewDiagnostics>(Diagnostics)]()
						{
							const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = WeakDiagnostics.Pin();
							return Pinned.IsValid() ? Pinned->GetBulbMargin() : FMargin(0.f);
						}))
						[
							Diagnostics->MakeBulb()
						]
						+ SOverlay::Slot()
						.HAlign(HAlign_Right)
						.VAlign(VAlign_Top)
						.Padding(FMargin(0.f, 2.f, 12.f, 0.f))   // (lang) clear of the marks stripe
						[
							Diagnostics->MakeInspectionWidget()
						]
						+ SOverlay::Slot()
						.Padding(FMargin(4.f, 2.f))
						.HAlign(HAlign_Left)
						.VAlign(VAlign_Top)
						[
							SNew(STextBlock)
							.Text(HintText)
							.Font(GetCodeFont())
							.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::GutterText))   // (theme)
							.Visibility_Lambda([this]()
							{
								return Buffer.IsValid() && Buffer->GetModel().Length() == 0 && !HintText.Get().IsEmpty()
									? EVisibility::HitTestInvisible : EVisibility::Collapsed;
							})
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						VScrollBar.ToSharedRef()
					]
				]
				// (lang) PO-06 F4: error and warning marks over the scroll bar's place at the right edge; an overlay, so the
				// text never shifts when the first problem appears or the last one goes (review lang-q1-m7).
				+ SOverlay::Slot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Fill)
				[
					Diagnostics->MakeScrollMarks()
				]
				// Go to line sits over the top of the text, as the palette's ':' provider will (§7.9.3).
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Top)
				.Padding(FMargin(0.f, 6.f, 0.f, 0.f))
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return bGoToLineOpen ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						GoToLine.ToSharedRef()
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				HScrollBar.ToSharedRef()
			]
		]
	];

	// Every model change reaches this view through the buffer's undo history, after the history recorded it (with the
	// selection the widget still had before it).
	History = FDreamUndoHistory::For(Buffer.ToSharedRef());
	History->AttachView(SharedThis(this));
	ModelChangedHandle = History->OnModelChanged().AddSP(this, &SDreamCodeView::HandleModelChanged);
	SettingChangedHandle = UDreamUserSettings::GetMutable()->OnSettingChanged().AddSP(this, &SDreamCodeView::HandleSettingChanged);
	ThemeChangedHandle = FDreamThemes::OnChanged().AddSP(this, &SDreamCodeView::HandleThemeChanged);   // (theme)

	const FDreamBufferViewState& ViewState = Buffer->GetViewState();
	if (ViewState.CaretLine > 0 || ViewState.CaretColumn > 0)
	{
		SetCaret(ViewState.CaretLine, ViewState.CaretColumn);
		TextWidget->ScrollTo(FTextLocation(FMath::Max(0, ViewState.CaretLine), 0));
	}
	LastKnownSelection = GetSelection();
	Completion = FDreamCompletionController::Create(SharedThis(this));   // (completion) §7.6.2 popup lifecycle
}

SDreamCodeView::~SDreamCodeView()
{
	CancelFindTask();   // a worker in flight stops at its next poll; its poll ticker finds no view and ends
	if (Completion.IsValid())
	{
		Completion->Detach();   // (completion)
	}
	if (History.IsValid())
	{
		History->OnModelChanged().Remove(ModelChangedHandle);
		History->DetachView(this);
	}
	if (UObjectInitialized() && SettingChangedHandle.IsValid())
	{
		UDreamUserSettings::GetMutable()->OnSettingChanged().Remove(SettingChangedHandle);
	}
	FDreamThemes::OnChanged().Remove(ThemeChangedHandle);   // (theme)
}

// ---- Text ----

FString SDreamCodeView::GetText() const
{
	return Buffer->GetTextAsString();
}

FString SDreamCodeView::GetShownText() const
{
	return TextWidget.IsValid() ? TextWidget->GetText().ToString() : FString();
}

void SDreamCodeView::SetTextNoUndo(const FString& Text)
{
	int32 Line = 0;
	int32 Column = 0;
	GetCaret(Line, Column);
	bSettingText = true;
	Buffer->ResetText(Text, dream::EditOrigin::Load);   // origin Load: the history starts a new root
	ReflectedText = Buffer->GetTextAsString();
	SyncedSnapshot = Buffer->Snapshot();
	PendingClosers.Reset();
	TextWidget->SetWholeText(ReflectedText, Marshaller->GetMutableLayout());
	bSettingText = false;
	SetCaret(FMath::Min(Line, GetLineCount() - 1), Column);
}

void SDreamCodeView::ReplaceLines(int32 FirstLine, int32 LastLine, const FString& NewText)
{
	const int32 LastIndex = GetLineCount() - 1;
	FirstLine = FMath::Clamp(FirstLine, 0, LastIndex);
	LastLine = FMath::Clamp(LastLine, FirstLine, LastIndex);
	// One edit group (PER-03: Restore is one undoable edit; R5: comment and indent are one step each).
	FDreamEditGroup Group;
	FDreamTextEdit& Edit = Group.Edits.AddDefaulted_GetRef();
	Edit.StartLine = FirstLine;
	Edit.StartColumn = 0;
	Edit.EndLine = LastLine;
	Edit.EndColumn = GetLineText(LastLine).Len();
	Edit.Text = NewText;
	ApplyEditGroup(Group);
}

void SDreamCodeView::InsertAtCaret(const FString& Text)
{
	if (ReplaceSelection(Text, dream::UndoKind::Typing))
	{
		return;   // over a selection: through the model (the stock delete of a selection can drop a line break)
	}
	SetPendingKind(dream::UndoKind::Typing);
	TextWidget->InsertTextAtCursor(Text);
}

bool SDreamCodeView::ApplyEditGroup(const FDreamEditGroup& Group)
{
	if (IsReadOnly() && !Group.bEvenIfReadOnly)
	{
		return false;
	}
	return FDreamUndoHistory::ApplyEditGroup(*Buffer, Group);
}

bool SDreamCodeView::ReplaceAllText(const FString& NewText, const FText& Label, dream::EditOrigin Origin, bool bEvenIfReadOnly)
{
	if (IsReadOnly() && !bEvenIfReadOnly)
	{
		return false;
	}
	return FDreamUndoHistory::ReplaceAllText(*Buffer, NewText, Label, Origin, bEvenIfReadOnly);
}

bool SDreamCodeView::ApplyEditBatchGroup(const dream::EditBatch& Batch, const FDreamEditGroup& Group)
{
	if (IsReadOnly() && !Group.bEvenIfReadOnly)
	{
		return false;
	}
	return FDreamUndoHistory::ApplyEditBatch(*Buffer, Batch, Group.Label, dream::UndoKind::Command, Group.SelectionAfter, Group.bEvenIfReadOnly);
}

void SDreamCodeView::SetPendingKind(dream::UndoKind Kind)
{
	PendingKind = Kind;
	PendingKindFrame = GFrameCounter;
}

// ---- Undo ----

bool SDreamCodeView::CanUndo() const
{
	return History.IsValid() && History->CanUndo();
}

bool SDreamCodeView::CanRedo() const
{
	return History.IsValid() && History->CanRedo();
}

void SDreamCodeView::Undo()
{
	if (History.IsValid() && !IsReadOnly())
	{
		History->Undo();
	}
}

void SDreamCodeView::Redo()
{
	if (History.IsValid() && !IsReadOnly())
	{
		History->Redo();
	}
}

FString SDreamCodeView::GetLineText(int32 Line) const
{
	if (!Buffer.IsValid())
	{
		return FString();
	}
	return DreamText::ToFString(Buffer->Snapshot().LineText(FMath::Max(0, Line)));
}

// ---- Selection and caret ----

FDreamTextSelection SDreamCodeView::GetSelection() const
{
	// The stock selection is (anchor, caret): FSlateEditableTextLayout::GetSelection builds it from the selection start
	// and the cursor's interaction location.
	const FTextSelection Stock = TextWidget->GetSelection();
	FDreamTextSelection Out;
	Out.AnchorLine = FMath::Max(0, Stock.LocationA.GetLineIndex());
	Out.AnchorColumn = FMath::Max(0, Stock.LocationA.GetOffset());
	Out.CaretLine = FMath::Max(0, Stock.LocationB.GetLineIndex());
	Out.CaretColumn = FMath::Max(0, Stock.LocationB.GetOffset());
	return Out;
}

FString SDreamCodeView::GetSelectedText() const
{
	return TextWidget->GetSelectedText().ToString();
}

void SDreamCodeView::SetSelection(const FDreamTextSelection& Selection)
{
	TextWidget->SelectText(FTextLocation(Selection.AnchorLine, Selection.AnchorColumn), FTextLocation(Selection.CaretLine, Selection.CaretColumn));
	LastKnownSelection = GetSelection();
	bStickyXStale = true;   // the stock SelectText keeps the old preferred column (§4.5 sticky X follows the caret)
}

void SDreamCodeView::SetCaret(int32 Line, int32 Column)
{
	const int32 SafeLine = FMath::Clamp(Line, 0, FMath::Max(0, GetLineCount() - 1));
	const int32 SafeColumn = FMath::Clamp(Column, 0, GetLineText(SafeLine).Len());
	TextWidget->ClearSelection();
	TextWidget->GoTo(FTextLocation(SafeLine, SafeColumn));
	LastKnownSelection = GetSelection();
	// GoTo takes the preferred column (sticky X, §4.5) from the line views; before the first paint there are none and it
	// reads 0 (a restored caret, a link that opens at a column). It is taken again before the next key (RefreshStickyX).
	bStickyXStale = !HasLineViews();
}

bool SDreamCodeView::HasLineViews() const
{
	const FTextLayout* Layout = Marshaller.IsValid() ? Marshaller->GetLayout() : nullptr;
	return Layout != nullptr && Layout->GetLineViews().Num() > 0;
}

void SDreamCodeView::RefreshStickyX()
{
	bStickyXStale = false;
	// The stock GoTo is the only public way to set the preferred column; it clears the selection, so the selection is put
	// back after it. Under bSettingText the cursor event changes nothing else (no typing-group close, R4).
	const FTextSelection Stock = TextWidget->GetSelection();
	TGuardValue<bool> Guard(bSettingText, true);
	TextWidget->GoTo(Stock.LocationB);
	if (!(Stock.LocationA == Stock.LocationB))
	{
		TextWidget->SelectText(Stock.LocationA, Stock.LocationB);
	}
}

void SDreamCodeView::GetCaret(int32& OutLine, int32& OutColumn) const
{
	const FTextLocation Location = TextWidget->GetCursorLocation();
	OutLine = FMath::Max(0, Location.GetLineIndex());
	OutColumn = FMath::Max(0, Location.GetOffset());
}

// ---- Scrolling and view ----

void SDreamCodeView::ScrollToLine(int32 Line, int32 Margin)
{
	// The stock widget scrolls a location just into view; scrolling to the margin line on the side the caret moved
	// toward keeps Margin lines between the caret and that edge (§4.5: 3 lines for Run line and advance).
	TArray<FDreamLineGeometry> Lines;
	GetVisibleLines(Lines);
	const int32 LastIndex = FMath::Max(0, GetLineCount() - 1);
	const int32 Middle = Lines.Num() > 0 ? (Lines[0].Line + Lines.Last().Line) / 2 : 0;
	const int32 Target = Line >= Middle ? FMath::Min(LastIndex, Line + Margin) : FMath::Max(0, Line - Margin);
	TextWidget->ScrollTo(FTextLocation(Target, 0));
}

int32 SDreamCodeView::GetFirstVisibleLine() const
{
	TArray<FDreamLineGeometry> Lines;
	GetVisibleLines(Lines);
	return Lines.Num() > 0 ? Lines[0].Line : 0;
}

void SDreamCodeView::SetFontSize(int32 Points)
{
	FontSize = FMath::Clamp(Points, DreamCodeViewLocal::MinPoints, DreamCodeViewLocal::MaxPoints);
	TextWidget->SetFont(GetCodeFont());
	Marshaller->SetFontSize(FontSize);
	Invalidate(EInvalidateWidgetReason::Layout);
}

int32 SDreamCodeView::GetZoomPercent() const
{
	using namespace DreamCodeViewLocal;
	return Buffer.IsValid() ? FMath::Clamp(Buffer->GetViewState().ZoomPercent, ZoomMinPercent, ZoomMaxPercent) : 100;
}

void SDreamCodeView::Zoom(int32 Delta)
{
	using namespace DreamCodeViewLocal;
	// EDT-10: 50% to 300% in 10% steps of the Font size setting; Ctrl+0 resets. The percentage is per tab (D23).
	FDreamBufferViewState& ViewState = Buffer->GetViewState();
	const int32 Before = GetZoomPercent();
	const int32 After = Delta == 0 ? 100 : FMath::Clamp(Before + ZoomStepPercent * FMath::Sign(Delta), ZoomMinPercent, ZoomMaxPercent);
	ViewState.ZoomPercent = After;
	ApplyFontSize();
	ShowStatus(After == Before && Delta != 0
		? FText::Format(LOCTEXT("CodeView.ZoomLimit", "Zoom {0}% (the limit is 50% to 300%)"), FText::AsNumber(After))
		: FText::Format(LOCTEXT("CodeView.Zoom", "Zoom {0}%"), FText::AsNumber(After)));
}

void SDreamCodeView::ApplyFontSize()
{
	SetFontSize(DreamCodeViewLocal::ZoomedPoints(SettingsFontSize, GetZoomPercent()));
}

void SDreamCodeView::HandleSettingChanged(UObject* Settings, FPropertyChangedEvent& Event)
{
	const FName Property = Event.GetMemberPropertyName();
	if (Property != GET_MEMBER_NAME_CHECKED(UDreamUserSettings, FontSize) && !Property.IsNone())
	{
		return;
	}
	const int32 NewSetting = FMath::Max(DreamCodeViewLocal::MinPoints, UDreamUserSettings::Get()->FontSize);
	if (NewSetting != SettingsFontSize)
	{
		SettingsFontSize = NewSetting;   // B74: the Font size setting applies to open views at once, keeping each zoom
		ApplyFontSize();
	}
}

void SDreamCodeView::HandleThemeChanged()
{
	// (theme) PO-04 live switch: the stock widget copied the code text style (caret = its colour, selection = its selected
	// background), so it takes the new one; the marshaller's token styles are rebuilt and re-marshalled (the stock Tick keeps
	// the cursor and the selection, as for a highlight refresh). A large buffer is re-marshalled too: a switch is one event.
	TextWidget->SetTextStyle(&FDreamEditorStyle::GetCodeTextStyle());
	Marshaller->RefreshStyles();
	Invalidate(EInvalidateWidgetReason::Paint);
}

void SDreamCodeView::RefreshHighlighting()
{
	Buffer->GetLexer().Reset(Buffer->Snapshot());
	Marshaller->MakeDirty();
}

void SDreamCodeView::FocusEditor()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(TextWidget, EFocusCause::SetDirectly);
	}
}

bool SDreamCodeView::HasEditorFocus() const
{
	return TextWidget.IsValid() && TextWidget->HasKeyboardFocus();
}

bool SDreamCodeView::IsReadOnly() const
{
	// (slice A choice, A27) read-only during runs is an attribute, not a push.
	return ReadOnlyAttribute.Get(false) || (Buffer.IsValid() && Buffer->IsReadOnly()) || DreamCodeViewLocal::IsRunActive();
}

FText SDreamCodeView::ReadOnlyReason() const
{
	if (DreamCodeViewLocal::IsRunActive())
	{
		return LOCTEXT("CodeView.RunInProgress", "Run in progress");
	}
	if (Buffer.IsValid() && Buffer->IsReadOnly() && !Buffer->GetReadOnlyReason().IsEmpty())
	{
		return Buffer->GetReadOnlyReason();
	}
	return LOCTEXT("CodeView.ReadOnly", "This text is read-only now");
}

void SDreamCodeView::ShowStatus(const FText& Message)
{
	if (const TSharedPtr<IDreamCommandTarget> Target = CommandTarget.Pin())
	{
		Target->ShowCommandRefusal(EDreamCommand::None, Message);
	}
}

// ---- Go to line ----

void SDreamCodeView::OpenGoToLine()
{
	int32 Line = 0;
	int32 Column = 0;
	GetCaret(Line, Column);
	GoToLine->Reset(Line, Column, GetLineCount());
	bGoToLineOpen = true;
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(GoToLine->GetFocusTarget(), EFocusCause::SetDirectly);
	}
}

void SDreamCodeView::CloseGoToLine()
{
	if (!bGoToLineOpen)
	{
		return;
	}
	bGoToLineOpen = false;
	if (GoToLine.IsValid() && (GoToLine->HasFocusedDescendants() || GoToLine->HasKeyboardFocus()))
	{
		FocusEditor();
	}
}

// ---- Find and replace (EDT-09 M1) ----

FDreamFindQuery SDreamCodeView::CurrentFindQuery() const
{
	FDreamFindQuery Query;
	if (FindBar.IsValid())
	{
		Query.Text = FindBar->GetFindText();
		Query.bMatchCase = FindBar->IsMatchCase();
		Query.bRegex = FindBar->IsRegex();
		Query.bWholeWord = FindBar->IsWholeWord();
	}
	return Query;
}

void SDreamCodeView::OpenFindBar(bool bReplace)
{
	if (!bFindEnabled)
	{
		return;
	}
	CloseGoToLine();
	// A one-line selection seeds the query (PyCharm, VS Code); the incremental find starts at the selection.
	const FDreamTextSelection Selection = GetSelection();
	const FString Selected = Selection.FirstLine() == Selection.LastLine() ? GetSelectedText() : FString();
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::Offset Start = 0;
	dream::Offset End = 0;
	SelectionOffsets(Snapshot, Start, End);
	FindAnchor = Start;
	const bool bWasOpen = bFindBarOpen;
	bFindBarOpen = true;
	FindBar->Open(bReplace, Selected);
	if (!bWasOpen || !(FindQuery == CurrentFindQuery()) || FindVersion != Snapshot.Version())
	{
		RunFind(/*bSelectFirst*/ false);
	}
	FindBar->FocusFindBox();
}

void SDreamCodeView::CloseFindBar()
{
	if (!bFindBarOpen)
	{
		return;
	}
	bFindBarOpen = false;
	CancelFindTask();
	PendingFindStep = 0;
	FindMatches.Reset();
	CurrentFindMatch = INDEX_NONE;
	FindVersion = 0;
	if (FindTimerHandle.IsValid())
	{
		UnRegisterActiveTimer(FindTimerHandle.ToSharedRef());
		FindTimerHandle.Reset();
	}
	if (FindBar.IsValid() && (FindBar->HasFocusedDescendants() || FindBar->HasKeyboardFocus()))
	{
		FocusEditor();
	}
}

void SDreamCodeView::HandleFindQueryChanged()
{
	if (!bFindBarOpen)
	{
		return;
	}
	// §4.4: a change in the find box restarts the search, at once for literals and after 150 ms idle for regexes. While
	// the user types the query, the first match from where the find started is selected (incremental find). A new query
	// cancels the worker in flight (a search or Replace All).
	CancelFindTask();
	PendingFindStep = 0;
	const FDreamFindQuery Query = CurrentFindQuery();
	if (Query.bRegex)
	{
		ScheduleFind(0.15, /*bSelectFirst*/ true);
	}
	else
	{
		RunFind(/*bSelectFirst*/ true);
	}
}

void SDreamCodeView::ScheduleFind(double DelaySeconds, bool bSelectFirst)
{
	FindDueSeconds = FPlatformTime::Seconds() + DelaySeconds;
	bFindSelectFirst = bFindSelectFirst || bSelectFirst;
	if (FindTask.IsValid() && !FindTask->bReplace)
	{
		CancelFindTask();   // its text is gone; the search runs again when the delay ends
	}
	if (!FindTimerHandle.IsValid())
	{
		// Reads the model and repaints the view; no run state, no Python.
		DREAM_TIMER_EXEMPT(paint-only)
		FindTimerHandle = RegisterActiveTimer(0.05f, FWidgetActiveTimerDelegate::CreateSP(this, &SDreamCodeView::FindTimer));
	}
}

EActiveTimerReturnType SDreamCodeView::FindTimer(double InCurrentTime, float InDeltaTime)
{
	if (FPlatformTime::Seconds() < FindDueSeconds)
	{
		return EActiveTimerReturnType::Continue;
	}
	FindTimerHandle.Reset();
	const bool bSelectFirst = bFindSelectFirst;
	bFindSelectFirst = false;
	const bool bCurrent = !FindTask.IsValid() && FindQuery == CurrentFindQuery() && FindVersion == Buffer->GetModel().Version();
	if (bFindBarOpen && !bCurrent)
	{
		RunFind(bSelectFirst);
	}
	return EActiveTimerReturnType::Stop;
}

void SDreamCodeView::UpdateFindNow()
{
	if (bFindBarOpen && !(FindTask.IsValid() && FindTask->bReplace))
	{
		EnsureFindResults();
	}
	// Specs only: wait for the worker (a Replace All starts a search of its result, so up to two in a row).
	for (int32 Guard = 0; FindTask.IsValid() && Guard < 4; ++Guard)
	{
		FindTask->Task.Wait();
		PollFindTask(FindTask->Generation);
	}
}

bool SDreamCodeView::EnsureFindResults()
{
	if (!bFindBarOpen)
	{
		return false;
	}
	if (FindTask.IsValid())
	{
		return false;   // a search or Replace All is on its way
	}
	if (FindQuery == CurrentFindQuery() && FindVersion == Buffer->GetModel().Version())
	{
		return true;    // current; a pending timer would only search the same text again (FindTimer skips it)
	}
	if (FindTimerHandle.IsValid() || !(FindQuery == CurrentFindQuery()) || FindVersion != Buffer->GetModel().Version())
	{
		const bool bSelectFirst = bFindSelectFirst;
		bFindSelectFirst = false;
		if (FindTimerHandle.IsValid())
		{
			UnRegisterActiveTimer(FindTimerHandle.ToSharedRef());
			FindTimerHandle.Reset();
		}
		RunFind(bSelectFirst);
	}
	return !FindTask.IsValid();
}

void SDreamCodeView::CancelFindTask()
{
	++FindGeneration;
	if (FindTask.IsValid())
	{
		FindTask->Cancel->Cancel();   // the worker stops at its next poll (SearchEngine checks every few thousand steps)
		FindTask.Reset();
	}
}

void SDreamCodeView::StartFindPoll()
{
	// The result is taken on the game thread through the deferral gate, never while a run executes (RT-01); the ticker
	// ends by itself once its generation is stale or the view is gone.
	const TWeakPtr<SDreamCodeView> Weak = SharedThis(this);
	const uint32 Generation = FindGeneration;
	FDreamDeferral::AddTicker(TEXT("DreamCodeViewFind"), EDreamDeferPolicy::DeferWhileExecuting, 0.02f, /*bNeedsPython*/ false,
		[Weak, Generation](float)
		{
			const TSharedPtr<SDreamCodeView> Self = Weak.Pin();
			return Self.IsValid() && Self->PollFindTask(Generation);
		});
}

void SDreamCodeView::StartFindTask(const FDreamFindQuery& Query, bool bSelectFirst)
{
	if (FindTask.IsValid() && !FindTask->bReplace && FindTask->Version == Buffer->GetModel().Version() && FindTask->Query == Query)
	{
		FindTask->bSelectFirst |= bSelectFirst;   // the same search is on its way: keep it
		return;
	}
	CancelFindTask();
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	std::shared_ptr<const dream::CompiledQuery> Compiled;
	FText Error;
	if (!DreamFindSearch::Compile(Query, Compiled, Error))
	{
		FindQuery = Query;
		FindVersion = Snapshot.Version();
		FindMatches.Reset();
		bFindCapped = false;
		FindSkippedLines = 0;
		FindError = Error;
		CurrentFindMatch = INDEX_NONE;
		UpdateFindStatus();
		return;
	}
	const TSharedRef<FDreamCodeViewFindTask> Task = MakeShared<FDreamCodeViewFindTask>();
	Task->Cancel = std::make_shared<dream::CancelToken>();
	Task->Generation = FindGeneration;
	Task->Version = Snapshot.Version();
	Task->Query = Query;
	Task->bSelectFirst = bSelectFirst;
	Task->Task = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Snapshot, Compiled, Cancel = Task->Cancel]()
	{
		const DreamCodeViewLocal::FFindWorkerScope Running;
		DreamCodeViewLocal::FFindWork Work;
		Work.Search = DreamFindSearch::SearchCompiled(Snapshot, *Compiled, Cancel.get());
		return Work;
	});
	FindTask = Task;
	FindError = FText::GetEmpty();
	UpdateFindStatus();
	StartFindPoll();
}

void SDreamCodeView::StartReplaceAllTask(const FDreamFindQuery& Query, const FString& Template, int32 Attempt)
{
	std::shared_ptr<const dream::CompiledQuery> Compiled;
	FText Error;
	if (!DreamFindSearch::Compile(Query, Compiled, Error) || !DreamFindSearch::ValidateTemplate(*Compiled, Template, Error))
	{
		ShowStatus(Error);
		return;
	}
	CancelFindTask();
	PendingFindStep = 0;
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	const TSharedRef<FDreamCodeViewFindTask> Task = MakeShared<FDreamCodeViewFindTask>();
	Task->Cancel = std::make_shared<dream::CancelToken>();
	Task->Generation = FindGeneration;
	Task->Version = Snapshot.Version();
	Task->Query = Query;
	Task->bReplace = true;
	Task->Template = Template;
	Task->Attempt = Attempt;
	Task->Task = UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[Snapshot, Compiled, Template16 = std::u16string(DreamText::ToU16(Template)), Cancel = Task->Cancel]()
		{
			const DreamCodeViewLocal::FFindWorkerScope Running;
			DreamCodeViewLocal::FFindWork Work;
			Work.Replace = DreamFindSearch::BuildReplaceAllCompiled(Snapshot, *Compiled, Template16, Cancel.get());
			return Work;
		});
	FindTask = Task;
	UpdateFindStatus();
	StartFindPoll();
}

bool SDreamCodeView::PollFindTask(uint32 Generation)
{
	if (!FindTask.IsValid() || FindTask->Generation != Generation)
	{
		return false;   // cancelled or replaced: this ticker ends
	}
	if (!FindTask->Task.IsCompleted())
	{
		return true;
	}
	const TSharedRef<FDreamCodeViewFindTask> Task = FindTask.ToSharedRef();
	DreamCodeViewLocal::FFindWork Work = MoveTemp(Task->Task.GetResult());
	FindTask.Reset();
	const uint64 Version = Buffer->GetModel().Version();
	const FNumberFormattingOptions& Plain = FNumberFormattingOptions::DefaultNoGrouping();

	if (Task->bReplace)
	{
		// Edits made while Replace All was in flight did not search (RunFind waits for it): every exit that applies no
		// batch searches again when the matches are not those of the current text and query.
		const auto SearchAgainIfStale = [this]()
		{
			if (bFindBarOpen && (FindVersion != Buffer->GetModel().Version() || !(FindQuery == CurrentFindQuery())))
			{
				RunFind(/*bSelectFirst*/ false);
			}
		};
		if (Work.Replace.bCancelled)
		{
			UpdateFindStatus();
			SearchAgainIfStale();
			return false;
		}
		if (Task->Version != Version)
		{
			// The text changed while the worker built the batch: build it again on the new text (§4.4: one undo step of
			// the text the user sees), a few times at most.
			if (bFindBarOpen && Task->Attempt < 3)
			{
				StartReplaceAllTask(Task->Query, Task->Template, Task->Attempt + 1);
			}
			else
			{
				UpdateFindStatus();
				ShowStatus(LOCTEXT("CodeView.ReplaceAllStale", "Replace All stopped: the text kept changing"));
				SearchAgainIfStale();
			}
			return false;
		}
		UpdateFindStatus();
		if (Work.Replace.bCapped)
		{
			ShowStatus(FText::Format(LOCTEXT("CodeView.ReplaceCapped", "Not replaced: more than {0} matches"),
				FText::AsNumber(DreamFindSearch::MaxMatches)));
			SearchAgainIfStale();
			return false;
		}
		if (Work.Replace.bFailed)
		{
			ShowStatus(LOCTEXT("CodeView.ReplaceFailed", "Not replaced: a replacement could not be built"));
			SearchAgainIfStale();
			return false;
		}
		if (Work.Replace.Count == 0)
		{
			ShowStatus(LOCTEXT("CodeView.ReplaceNone", "No results to replace"));
			SearchAgainIfStale();
			return false;
		}
		FDreamEditGroup Group;
		Group.Label = LOCTEXT("CodeView.ReplaceAllStep", "Replace All");
		if (Work.Replace.Batch.Ranges.empty() || ApplyEditBatchGroup(Work.Replace.Batch, Group))
		{
			ShowStatus(FText::Format(LOCTEXT("CodeView.ReplacedN", "Replaced {0} {0}|plural(one=match,other=matches) (one undo step)"),
				FText::AsNumber(Work.Replace.Count, &Plain)));
		}
		if (bFindBarOpen)
		{
			RunFind(/*bSelectFirst*/ false);
		}
		return false;
	}

	if (Work.Search.bCancelled || !bFindBarOpen)
	{
		UpdateFindStatus();
		return false;
	}
	if (Task->Version != Version || !(Task->Query == CurrentFindQuery()))
	{
		RunFind(Task->bSelectFirst);   // stale (an edit or a new query raced the poll): search again
		return false;
	}
	FindQuery = Task->Query;
	FindVersion = Task->Version;
	FindMatches = MoveTemp(Work.Search.Matches);
	bFindCapped = Work.Search.bCapped;
	FindSkippedLines = Work.Search.SkippedLines;
	FindError = FText::GetEmpty();
	CurrentFindMatch = FindMatchAtSelection();
	if (Task->bSelectFirst && FindMatches.Num() > 0)
	{
		const int32 First = Algo::LowerBoundBy(FindMatches, FindAnchor, [](const FDreamFindMatch& Match) { return Match.Start; });
		SelectFindMatch(First < FindMatches.Num() ? First : 0);
	}
	UpdateFindStatus();
	if (PendingFindStep != 0)
	{
		const int32 Step = PendingFindStep;
		PendingFindStep = 0;
		// The F3 or Enter that waited for these matches; Enter gives focus back to the text only if the user is still in the bar.
		const bool bFocusText = bPendingFindStepFocusText && FindBar.IsValid() && (FindBar->HasFocusedDescendants() || FindBar->HasKeyboardFocus());
		FindStep(Step, bFocusText);
	}
	return false;
}

void SDreamCodeView::RunFind(bool bSelectFirst)
{
	if (FindTask.IsValid() && FindTask->bReplace)
	{
		return;   // a Replace All in flight is never cancelled by an edit's search; it searches again when it lands
	}
	const FDreamFindQuery Current = CurrentFindQuery();
	if (Current.bRegex && !Current.Text.IsEmpty())
	{
		StartFindTask(Current, bSelectFirst);   // §4.4 Run: a regex search never runs on the game thread
		return;
	}
	CancelFindTask();   // a literal answers at once (linear)
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	FindQuery = Current;
	FindVersion = Snapshot.Version();
	FDreamFindResult Result = DreamFindSearch::Search(Snapshot, FindQuery);
	FindMatches = MoveTemp(Result.Matches);
	bFindCapped = Result.bCapped;
	FindSkippedLines = Result.SkippedLines;
	FindError = Result.Error;
	CurrentFindMatch = FindMatchAtSelection();
	if (bSelectFirst && FindMatches.Num() > 0)
	{
		int32 First = Algo::LowerBoundBy(FindMatches, FindAnchor, [](const FDreamFindMatch& Match) { return Match.Start; });
		SelectFindMatch(First < FindMatches.Num() ? First : 0);
	}
	UpdateFindStatus();
}

int32 SDreamCodeView::FindMatchAtSelection() const
{
	if (FindMatches.Num() == 0)
	{
		return INDEX_NONE;
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::Offset Start = 0;
	dream::Offset End = 0;
	SelectionOffsets(Snapshot, Start, End);
	const int32 Index = Algo::LowerBoundBy(FindMatches, Start, [](const FDreamFindMatch& Match) { return Match.Start; });
	return FindMatches.IsValidIndex(Index) && FindMatches[Index].Start == Start && FindMatches[Index].End() == End ? Index : INDEX_NONE;
}

void SDreamCodeView::SelectFindMatch(int32 Index)
{
	if (!FindMatches.IsValidIndex(Index))
	{
		return;
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	const FDreamFindMatch& Match = FindMatches[Index];
	SetSelection(ToSelection(Snapshot, Match.Start, Match.End()));
	ScrollToLine(Snapshot.LineOf(Match.Start), 3);
	CurrentFindMatch = Index;
	UpdateFindStatus();
}

void SDreamCodeView::UpdateFindStatus()
{
	const FNumberFormattingOptions& Plain = FNumberFormattingOptions::DefaultNoGrouping();
	if (FindTask.IsValid())
	{
		FindStatus = FindTask->bReplace ? LOCTEXT("CodeView.FindReplacing", "Replacing…") : LOCTEXT("CodeView.FindSearching", "Searching…");
		return;
	}
	if (!FindError.IsEmpty())
	{
		FindStatus = FindError;
		return;
	}
	if (FindQuery.Text.IsEmpty())
	{
		FindStatus = FText::GetEmpty();
		return;
	}
	if (FindMatches.Num() == 0)
	{
		FindStatus = FindSkippedLines > 0
			? FText::Format(LOCTEXT("CodeView.FindNoneComplex", "No results; pattern too complex on {0} {0}|plural(one=line,other=lines)"),
				FText::AsNumber(FindSkippedLines, &Plain))
			: LOCTEXT("CodeView.FindNone", "No results");
		return;
	}
	const FText Count = bFindCapped
		? FText::Format(LOCTEXT("CodeView.FindCapped", "{0}+"), FText::AsNumber(FindMatches.Num(), &Plain))
		: FText::AsNumber(FindMatches.Num(), &Plain);
	FText Status = CurrentFindMatch != INDEX_NONE
		? FText::Format(LOCTEXT("CodeView.FindNofM", "{0} of {1}"), FText::AsNumber(CurrentFindMatch + 1, &Plain), Count)
		: FText::Format(LOCTEXT("CodeView.FindResults", "{0} {1}|plural(one=result,other=results)"), Count, FText::AsNumber(FindMatches.Num(), &Plain));
	if (FindSkippedLines > 0)
	{
		Status = FText::Format(LOCTEXT("CodeView.FindComplex", "{0}; pattern too complex on {1} {1}|plural(one=line,other=lines)"),
			Status, FText::AsNumber(FindSkippedLines, &Plain));
	}
	FindStatus = Status;
}

void SDreamCodeView::FindStep(int32 Direction, bool bFocusText)
{
	if (!bFindEnabled)
	{
		return;
	}
	if (!bFindBarOpen || CurrentFindQuery().Text.IsEmpty())
	{
		OpenFindBar(FindBar.IsValid() && FindBar->IsReplaceShown());   // F3 with no query: the bar, as VS Code
		return;
	}
	if (!EnsureFindResults())
	{
		// The worker's matches are on their way: this step runs when they arrive (PollFindTask).
		PendingFindStep = Direction >= 0 ? 1 : -1;
		bPendingFindStepFocusText = bFocusText;
		ShowStatus(LOCTEXT("CodeView.FindStepWaiting", "Searching…"));
		return;
	}
	if (FindMatches.Num() == 0)
	{
		ShowStatus(FindError.IsEmpty() ? LOCTEXT("CodeView.FindNoneStatus", "No results") : FindError);
		return;
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::Offset Start = 0;
	dream::Offset End = 0;
	SelectionOffsets(Snapshot, Start, End);
	const int32 At = FindMatchAtSelection();
	int32 Next = INDEX_NONE;
	bool bWrapped = false;
	if (Direction >= 0)
	{
		Next = At != INDEX_NONE ? At + 1 : Algo::LowerBoundBy(FindMatches, End, [](const FDreamFindMatch& Match) { return Match.Start; });
		if (Next >= FindMatches.Num())
		{
			Next = 0;
			bWrapped = true;
		}
	}
	else
	{
		Next = At != INDEX_NONE ? At - 1 : Algo::LowerBoundBy(FindMatches, Start, [](const FDreamFindMatch& Match) { return Match.Start; }) - 1;
		if (Next < 0)
		{
			Next = FindMatches.Num() - 1;
			bWrapped = true;
		}
	}
	SelectFindMatch(Next);
	if (bWrapped && FindMatches.Num() > 1)
	{
		ShowStatus(Direction >= 0 ? LOCTEXT("CodeView.FindWrappedTop", "Search wrapped to the top")
			: LOCTEXT("CodeView.FindWrappedBottom", "Search wrapped to the bottom"));
	}
	if (bFocusText)
	{
		FocusEditor();
	}
}

void SDreamCodeView::ReplaceCurrent()
{
	if (!bFindBarOpen)
	{
		return;
	}
	if (IsReadOnly())
	{
		ShowStatus(ReadOnlyReason());
		return;
	}
	if (!EnsureFindResults())
	{
		ShowStatus(LOCTEXT("CodeView.ReplaceWaiting", "Searching… Replace again when the matches are shown"));
		return;
	}
	const int32 At = FindMatchAtSelection();
	if (At == INDEX_NONE)
	{
		FindStep(1, /*bFocusText*/ false);   // nothing selected yet: the first Replace selects the next match
		return;
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	const FDreamFindMatch Match = FindMatches[At];
	dream::EditBatch Batch;
	FText Error;
	if (!DreamFindSearch::BuildReplaceOne(Snapshot, FindQuery, Match, FindBar->GetReplaceText(), Batch, Error))
	{
		ShowStatus(Error);
		return;
	}
	dream::Offset InsertedLen = 0;
	for (const dream::EditRange& Range : Batch.Ranges)
	{
		InsertedLen += static_cast<dream::Offset>(Range.Insert.size());
	}
	FDreamEditGroup Group;
	Group.Label = LOCTEXT("CodeView.ReplaceStep", "Replace");
	Group.SelectionAfter = ToSelection(Snapshot, Match.Start, Match.Start);   // in post-edit offsets: before the replacement
	if (!ApplyEditBatchGroup(Batch, Group))
	{
		return;
	}
	// The next match after the replacement (PyCharm: Replace, then on to the next).
	FindAnchor = Match.Start + InsertedLen;
	if (FindQuery.bRegex)
	{
		// The worker searches the edited text; its matches select the first one from FindAnchor (PollFindTask). The
		// matches held now are those of the text before the edit.
		RunFind(/*bSelectFirst*/ true);
		return;
	}
	RunFind(/*bSelectFirst*/ false);
	if (FindMatches.Num() > 0)
	{
		const int32 Next = Algo::LowerBoundBy(FindMatches, FindAnchor, [](const FDreamFindMatch& M) { return M.Start; });
		SelectFindMatch(Next < FindMatches.Num() ? Next : 0);
	}
}

void SDreamCodeView::ReplaceAll()
{
	if (!bFindBarOpen)
	{
		return;
	}
	if (IsReadOnly())
	{
		ShowStatus(ReadOnlyReason());
		return;
	}
	const FDreamFindQuery Query = CurrentFindQuery();
	if (Query.bRegex)
	{
		StartReplaceAllTask(Query, FindBar->GetReplaceText(), /*Attempt*/ 0);   // §4.4: enumerated on a worker
		return;
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::EditBatch Batch;
	int32 Count = 0;
	FText Error;
	if (!DreamFindSearch::BuildReplaceAll(Snapshot, Query, FindBar->GetReplaceText(), Batch, Count, Error))
	{
		ShowStatus(Error);
		return;
	}
	if (Count == 0 || Batch.Ranges.empty())
	{
		ShowStatus(LOCTEXT("CodeView.ReplaceNone", "No results to replace"));
		return;
	}
	FDreamEditGroup Group;
	Group.Label = LOCTEXT("CodeView.ReplaceAllStep", "Replace All");
	if (ApplyEditBatchGroup(Batch, Group))
	{
		RunFind(/*bSelectFirst*/ false);
		ShowStatus(FText::Format(LOCTEXT("CodeView.ReplacedN", "Replaced {0} {0}|plural(one=match,other=matches) (one undo step)"),
			FText::AsNumber(Count, &FNumberFormattingOptions::DefaultNoGrouping())));
	}
}

// ---- Layout queries ----

void SDreamCodeView::GetRangeRects(dream::Offset Start, dream::Offset End, TArray<FSlateRect>& Out) const
{
	TArray<FDreamLineGeometry> Lines;
	GetVisibleLines(Lines);
	GetRangeRects(Start, End, Out, Lines);
}

void SDreamCodeView::GetRangeRects(dream::Offset Start, dream::Offset End, TArray<FSlateRect>& Out, const TArray<FDreamLineGeometry>& Lines) const   // (lang) split
{
	const FTextLayout* Layout = Marshaller.IsValid() ? Marshaller->GetLayout() : nullptr;
	if (Layout == nullptr || !Buffer.IsValid() || End <= Start)
	{
		return;
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	const float Scale = Layout->GetScale() > 0.f ? Layout->GetScale() : 1.f;
	const dream::LineNo FirstLine = Snapshot.LineOf(Start);
	const dream::LineNo LastLine = Snapshot.LineOf(End);
	for (const FDreamLineGeometry& Line : Lines)
	{
		if (Line.Line < FirstLine || Line.Line > LastLine)
		{
			continue;
		}
		const dream::Offset LineStart = Snapshot.LineStart(Line.Line);
		const int32 LineLength = Snapshot.LineLength(Line.Line);
		const int32 From = Line.Line == FirstLine ? static_cast<int32>(Start - LineStart) : 0;
		const int32 To = Line.Line == LastLine ? FMath::Min(static_cast<int32>(End - LineStart), LineLength) : LineLength;
		const FVector2D Left = Layout->GetLocationAt(FTextLocation(Line.Line, From), /*bPerformInclusiveBoundsCheck*/ true);
		FVector2D Right = Layout->GetLocationAt(FTextLocation(Line.Line, To), true);
		if (Left.IsZero() && From > 0)
		{
			continue;   // the layout has no block there (the stock layout reports a miss as zero)
		}
		float X0 = static_cast<float>(Left.X) / Scale;
		float X1 = static_cast<float>(Right.X) / Scale;
		if (X1 <= X0)
		{
			X1 = X0 + FMath::Max(2.f, Line.Height * 0.3f);   // a match on the line break: a thin mark at the line end
		}
		Out.Add(FSlateRect(X0, Line.Top, X1, Line.Top + Line.Height));
	}
}

FSlateRect SDreamCodeView::GetCaretScreenRect() const
{
	const FTextLayout* Layout = Marshaller.IsValid() ? Marshaller->GetLayout() : nullptr;
	if (Layout == nullptr || !TextWidget.IsValid())
	{
		return FSlateRect();
	}
	int32 Line = 0;
	int32 Column = 0;
	GetCaret(Line, Column);
	TArray<FDreamLineGeometry> Lines;
	GetVisibleLines(Lines);
	for (const FDreamLineGeometry& Shown : Lines)
	{
		if (Shown.Line == Line)
		{
			const float Scale = Layout->GetScale() > 0.f ? Layout->GetScale() : 1.f;
			const float X = static_cast<float>(Layout->GetLocationAt(FTextLocation(Line, Column), true).X) / Scale;
			const FGeometry& Geometry = TextWidget->GetCachedGeometry();
			const FVector2D TopLeft = Geometry.LocalToAbsolute(FVector2D(X, Shown.Top));
			const FVector2D BottomRight = Geometry.LocalToAbsolute(FVector2D(X + 1.0, Shown.Top + Shown.Height));
			return FSlateRect(TopLeft, BottomRight);
		}
	}
	return FSlateRect();
}

void SDreamCodeView::GetVisibleLines(TArray<FDreamLineGeometry>& Out) const
{
	Out.Reset();
	const FTextLayout* Layout = Marshaller.IsValid() ? Marshaller->GetLayout() : nullptr;
	if (Layout == nullptr || !TextWidget.IsValid())
	{
		return;
	}
	const float Scale = Layout->GetScale() > 0.f ? Layout->GetScale() : 1.f;
	const float ViewHeight = static_cast<float>(TextWidget->GetCachedGeometry().GetLocalSize().Y);
	int32 LastModel = INDEX_NONE;
	for (const FTextLayout::FLineView& LineView : Layout->GetLineViews())
	{
		const float Top = static_cast<float>(LineView.Offset.Y) / Scale;
		const float Height = static_cast<float>(LineView.Size.Y) / Scale;
		if (Top + Height < 0.f || LineView.ModelIndex == LastModel)
		{
			continue;
		}
		if (ViewHeight > 0.f && Top > ViewHeight)
		{
			break;
		}
		FDreamLineGeometry& Line = Out.AddDefaulted_GetRef();
		Line.Line = LineView.ModelIndex;
		Line.Top = Top;
		Line.Height = Height;
		LastModel = LineView.ModelIndex;
	}
}

int32 SDreamCodeView::GetLineCount() const
{
	return Buffer.IsValid() ? FMath::Max(1, static_cast<int32>(Buffer->GetModel().LineCount())) : 1;
}

int32 SDreamCodeView::GetCaretLine() const
{
	return TextWidget.IsValid() ? FMath::Max(0, TextWidget->GetCursorLocation().GetLineIndex()) : 0;
}

FSlateFontInfo SDreamCodeView::GetCodeFont() const
{
	return FDreamEditorStyle::GetCodeFont(FontSize);
}

// (lang) PO-06 F4: the diagnostics' layout queries.
bool SDreamCodeView::HitTestText(const FVector2D& ScreenPosition, int32& OutLine, int32& OutColumn) const
{
	const FTextLayout* Layout = Marshaller.IsValid() ? Marshaller->GetLayout() : nullptr;
	if (Layout == nullptr || !TextWidget.IsValid() || Layout->GetLineViews().Num() == 0)
	{
		return false;
	}
	const FGeometry& Geometry = TextWidget->GetCachedGeometry();
	if (!Geometry.IsUnderLocation(ScreenPosition))
	{
		return false;
	}
	const float Scale = Layout->GetScale() > 0.f ? Layout->GetScale() : 1.f;
	const FVector2D Local = Geometry.AbsoluteToLocal(ScreenPosition);
	const FTextLocation Location = Layout->GetTextLocationAt(Local * Scale);
	if (!Location.IsValid() || !Buffer.IsValid())
	{
		return false;
	}
	// (review lang-q1 nit) The layout snaps a point right of a line's end or below the last line to the nearest text; a
	// tooltip there would name a problem the mouse is not over. The point must lie on the line and near its text.
	TArray<FDreamLineGeometry> Lines;
	GetVisibleLines(Lines);
	const FDreamLineGeometry* Shown = Lines.FindByPredicate([&Location](const FDreamLineGeometry& L) { return L.Line == Location.GetLineIndex(); });
	if (Shown == nullptr || Local.Y < Shown->Top || Local.Y > Shown->Top + Shown->Height)
	{
		return false;
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	const dream::Offset LineStart = Snapshot.LineStart(Location.GetLineIndex());
	TArray<FSlateRect> Rects;
	GetRangeRects(LineStart, LineStart + FMath::Max(1, Snapshot.LineLength(Location.GetLineIndex())), Rects, Lines);
	if (Rects.Num() > 0 && Local.X > Rects.Last().Right + Shown->Height * 0.6f)
	{
		return false;
	}
	OutLine = Location.GetLineIndex();
	OutColumn = Location.GetOffset();
	return true;
}

FGeometry SDreamCodeView::GetTextGeometry() const
{
	return TextWidget.IsValid() ? TextWidget->GetCachedGeometry() : GetCachedGeometry();
}

uint32 SDreamCodeView::ComputeContextMask() const
{
	uint32 Mask = BaseContextMask | dream::Ctx_Focused;
	if (!IsReadOnly())
	{
		Mask |= dream::Ctx_Editable;
	}
	if (TextWidget.IsValid() && TextWidget->AnyTextSelected())
	{
		Mask |= dream::Ctx_HasSelection;
	}
	if (DreamCodeViewLocal::IsRunActive())
	{
		Mask |= dream::Ctx_RunActive;
	}
	return Mask;
}

// ---- Commands ----

bool SDreamCodeView::CanExecuteDreamCommand(EDreamCommand Command, FText& OutReason) const
{
	switch (Command)
	{
	case EDreamCommand::ToggleTabMovesFocus:
		return true;
	case EDreamCommand::DuplicateLine:
	case EDreamCommand::Undo:
	case EDreamCommand::Redo:
		// Ctrl+Z in a Dream widget is always text undo and always consumed (D8): refused here, it still never reaches the
		// editor's transaction undo. From the find bar's boxes the text's own commands do nothing (the boxes undo their
		// own text).
		if (bRoutingChildKey)
		{
			OutReason = LOCTEXT("CodeView.TextNotFocused", "Click in the text to edit it");
			return false;
		}
		if (IsReadOnly())
		{
			OutReason = ReadOnlyReason();
			return false;
		}
		if (Command == EDreamCommand::DuplicateLine)
		{
			return true;
		}
		if (Command == EDreamCommand::Undo ? !CanUndo() : !CanRedo())
		{
			OutReason = Command == EDreamCommand::Undo ? LOCTEXT("CodeView.NothingToUndo", "Nothing to undo") : LOCTEXT("CodeView.NothingToRedo", "Nothing to redo");
			return false;
		}
		return true;
	case EDreamCommand::Find:
	case EDreamCommand::Replace:
	case EDreamCommand::FindNext:
	case EDreamCommand::FindPrevious:
		if (bFindEnabled)
		{
			return true;
		}
		break;   // the REPL prompt has no find bar: the owner's code view finds
	default:
		break;
	}
	if (const TSharedPtr<IDreamCommandTarget> Target = CommandTarget.Pin())
	{
		return Target->CanExecuteDreamCommand(Command, OutReason);
	}
	OutReason = LOCTEXT("CodeView.NoTarget", "Not available here");
	return false;
}

void SDreamCodeView::ExecuteDreamCommand(EDreamCommand Command)
{
	switch (Command)
	{
	case EDreamCommand::ToggleTabMovesFocus:
	{
		FDreamKeyRouter& Router = FDreamKeyRouter::Get();
		Router.SetTabMovesFocus(!Router.IsTabMovesFocus());
		const FText Chord = Router.GetChordText(EDreamCommand::ToggleTabMovesFocus);
		ShowStatus(Router.IsTabMovesFocus()
			? FText::Format(LOCTEXT("CodeView.TabMovesFocusOn", "Tab moves focus: on ({0} turns it off)"), Chord)
			: LOCTEXT("CodeView.TabMovesFocusOff", "Tab moves focus: off (Tab indents)"));
		return;
	}
	case EDreamCommand::DuplicateLine:
		DuplicateLineOrSelection();
		return;
	case EDreamCommand::Undo:
		Undo();
		return;
	case EDreamCommand::Redo:
		Redo();
		return;
	case EDreamCommand::Find:
	case EDreamCommand::Replace:
		if (bFindEnabled)
		{
			OpenFindBar(Command == EDreamCommand::Replace);
			return;
		}
		break;
	case EDreamCommand::FindNext:
	case EDreamCommand::FindPrevious:
		if (bFindEnabled)
		{
			FindStep(Command == EDreamCommand::FindNext ? 1 : -1, /*bFocusText*/ false);
			return;
		}
		break;
	default:
		break;
	}
	if (const TSharedPtr<IDreamCommandTarget> Target = CommandTarget.Pin())
	{
		Target->ExecuteDreamCommand(Command);
	}
}

void SDreamCodeView::ShowCommandRefusal(EDreamCommand Command, const FText& Reason)
{
	if (const TSharedPtr<IDreamCommandTarget> Target = CommandTarget.Pin())
	{
		Target->ShowCommandRefusal(Command, Reason);
	}
}

void SDreamCodeView::ExecuteThroughTarget(EDreamCommand Command)
{
	FText Reason;
	if (CanExecuteDreamCommand(Command, Reason))
	{
		ExecuteDreamCommand(Command);
	}
	else
	{
		ShowCommandRefusal(Command, Reason);
	}
}

TSharedPtr<SWidget> SDreamCodeView::BuildContextMenu()
{
	// The stock menu's Undo and Redo are the stock widget's own (its private command list): this menu replaces it, so the
	// text undo it offers is UndoTree's (EDT-08). Clipboard and Select All stay the stock widget's.
	TSharedRef<FUICommandList> Actions = MakeShared<FUICommandList>();
	const TWeakPtr<SDreamCodeView> Weak = SharedThis(this);
	auto MapView = [&Actions, Weak](const TSharedPtr<FUICommandInfo>& Info, EDreamCommand Command)
	{
		if (!Info.IsValid())
		{
			return;
		}
		Actions->MapAction(Info,
			FExecuteAction::CreateLambda([Weak, Command]()
			{
				if (const TSharedPtr<SDreamCodeView> Self = Weak.Pin())
				{
					Self->ExecuteThroughTarget(Command);
				}
			}),
			FCanExecuteAction::CreateLambda([Weak, Command]()
			{
				const TSharedPtr<SDreamCodeView> Self = Weak.Pin();
				FText Reason;
				return Self.IsValid() && Self->CanExecuteDreamCommand(Command, Reason);
			}));
	};
	const bool bCommands = FDreamCommands::IsRegistered();
	if (bCommands)
	{
		MapView(FDreamCommands::Get().GetInfo(EDreamCommand::Undo), EDreamCommand::Undo);
		MapView(FDreamCommands::Get().GetInfo(EDreamCommand::Redo), EDreamCommand::Redo);
	}
	const FGenericCommands& Generic = FGenericCommands::Get();
	const TWeakPtr<SDreamPlainText> Text = TextWidget;
	Actions->MapAction(Generic.Cut,
		FExecuteAction::CreateLambda([Text, Weak]()
		{
			const TSharedPtr<SDreamPlainText> T = Text.Pin();
			const TSharedPtr<SDreamCodeView> Self = Weak.Pin();
			if (T.IsValid() && Self.IsValid())
			{
				if (!Self->CutSelection())
				{
					Self->SetPendingKind(dream::UndoKind::Command);   // R5: a cut is one undo step
					T->CutText();
				}
			}
		}),
		FCanExecuteAction::CreateLambda([Text]() { const TSharedPtr<SDreamPlainText> T = Text.Pin(); return T.IsValid() && T->CanCutText(); }));
	Actions->MapAction(Generic.Copy,
		FExecuteAction::CreateLambda([Text]() { if (const TSharedPtr<SDreamPlainText> T = Text.Pin()) { T->CopyText(); } }),
		FCanExecuteAction::CreateLambda([Text]() { const TSharedPtr<SDreamPlainText> T = Text.Pin(); return T.IsValid() && T->CanCopyText(); }));
	Actions->MapAction(Generic.Paste,
		FExecuteAction::CreateLambda([Text, Weak]()
		{
			const TSharedPtr<SDreamPlainText> T = Text.Pin();
			const TSharedPtr<SDreamCodeView> Self = Weak.Pin();
			if (T.IsValid() && Self.IsValid())
			{
				if (!Self->PasteOverSelection())
				{
					Self->SetPendingKind(dream::UndoKind::Command);   // R5: a paste is one undo step
					T->PasteText();
				}
			}
		}),
		FCanExecuteAction::CreateLambda([Text]() { const TSharedPtr<SDreamPlainText> T = Text.Pin(); return T.IsValid() && T->CanPasteText(); }));
	Actions->MapAction(Generic.SelectAll,
		FExecuteAction::CreateLambda([Text]() { if (const TSharedPtr<SDreamPlainText> T = Text.Pin()) { T->SelectAllText(); } }));

	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ true, Actions);
	if (bCommands)
	{
		Menu.BeginSection(TEXT("DreamCodeViewUndo"));
		Menu.AddMenuEntry(FDreamCommands::Get().GetInfo(EDreamCommand::Undo));
		Menu.AddMenuEntry(FDreamCommands::Get().GetInfo(EDreamCommand::Redo));
		Menu.EndSection();
	}
	Menu.BeginSection(TEXT("DreamCodeViewClipboard"));
	Menu.AddMenuEntry(Generic.Cut);
	Menu.AddMenuEntry(Generic.Copy);
	Menu.AddMenuEntry(Generic.Paste);
	Menu.EndSection();
	Menu.BeginSection(TEXT("DreamCodeViewSelect"));
	Menu.AddMenuEntry(Generic.SelectAll);
	Menu.EndSection();
	return Menu.MakeWidget();
}

// ---- Input ----

FReply SDreamCodeView::HandleTextKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	// A view made in this frame (a tab opened and focused while keys are queued) has no line views until its first paint;
	// the stock vertical moves and Home/End read them (see SDreamPlainText::SetWholeText).
	if (FTextLayout* Layout = Marshaller->GetMutableLayout(); Layout != nullptr && Layout->GetLineViews().Num() == 0)
	{
		Layout->UpdateIfNeeded();
	}
	// A caret or selection placed by code: Up/Down keep its column (§4.5 sticky X). Only for the keys that read the preferred
	// column: the refresh's stock GoTo scrolls the caret into view, which must not happen on Ctrl, Ctrl+S or a zoom key while
	// the user has scrolled the view away.
	const FKey StickyKey = KeyEvent.GetKey();
	const bool bReadsStickyX = (StickyKey == EKeys::Up || StickyKey == EKeys::Down || StickyKey == EKeys::PageUp || StickyKey == EKeys::PageDown)
		&& !KeyEvent.GetModifierKeys().IsAltDown();
	if (bStickyXStale && bReadsStickyX && HasLineViews())
	{
		RefreshStickyX();
	}
	// (completion) The popup's keys and the Complete chord first, before the REPL's prompt keys (§6.5: Enter accepts).
	if (Completion.IsValid() && Completion->HandleKeyDown(KeyEvent))
	{
		return FReply::Handled();
	}
	if (OnKeyHook.IsBound())
	{
		const FReply Hooked = OnKeyHook.Execute(KeyEvent);
		if (Hooked.IsEventHandled())
		{
			return Hooked;
		}
	}

	// The keystroke is handled by C++ at once; typing never waits for Python (RT-17, §3.4).
	DreamCodeViewLocal::RequestPythonStart(EDreamStartReason::Keystroke);

	// Bound chords: the text commands run here, the rest through CommandTarget (this view forwards them).
	const FReply Routed = FDreamKeyRouter::Get().RoutePre(KeyEvent, ComputeContextMask(), *this);
	if (Routed.IsEventHandled())
	{
		return Routed;
	}

	const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
	const FKey Key = KeyEvent.GetKey();
	const bool bNoMods = !Mods.IsControlDown() && !Mods.IsAltDown() && !Mods.IsCommandDown();
	bSwallowBackspaceChar = false;
	if (History.IsValid() && History->IsImeGroupOpen() && !TextWidget->IsComposing())
	{
		History->EndImeGroup();   // R6: the composition ended (plain mode hears no EndComposition of its own)
	}
	// Idle Esc closes the find bar or Go to line first (§4.9.1); it is consumed either way (D8).
	if (Key == EKeys::Escape && bNoMods && !Mods.IsShiftDown() && (bFindBarOpen || bGoToLineOpen))
	{
		CloseGoToLine();
		CloseFindBar();
		return FReply::Handled();
	}
	// §4.2 R1-R5: the kind of the stock edit this key makes (deletes coalesce as deletes; paste and cut are one step).
	if (Key == EKeys::BackSpace || (Key == EKeys::Delete && !Mods.IsShiftDown()))
	{
		SetPendingKind(dream::UndoKind::Delete);
	}
	else if ((Mods.IsControlDown() && (Key == EKeys::V || Key == EKeys::X)) || (Mods.IsShiftDown() && (Key == EKeys::Insert || Key == EKeys::Delete)))
	{
		SetPendingKind(dream::UndoKind::Command);
		// Cut, and a paste over a selection, go through the model (the stock delete of a selection can drop a line break).
		if (!Mods.IsAltDown() && !Mods.IsCommandDown() && Mods.IsControlDown() != Mods.IsShiftDown()
			&& ((Key == EKeys::X || Key == EKeys::Delete) ? CutSelection() : PasteOverSelection()))
		{
			return FReply::Handled();
		}
	}
	if (Key == EKeys::Tab && bNoMods)
	{
		if (FDreamKeyRouter::Get().IsTabMovesFocus())
		{
			// CFG-08: Tab and Shift+Tab move focus to the next or previous widget; the key itself never leaves Dream.
			return FReply::Handled().SetNavigation(Mods.IsShiftDown() ? EUINavigation::Previous : EUINavigation::Next,
				ENavigationGenesis::Keyboard);
		}
		return HandleTab(Mods.IsShiftDown());
	}
	// A selection is deleted through the model, never by the stock widget (its delete of a range over several lines that
	// ends at a line end drops one line break too many, SDreamPlainText::ReplaceRange): Backspace, Delete, and with Ctrl
	// held too (the stock Ctrl+Backspace / Ctrl+Delete delete the selection when there is one).
	if ((Key == EKeys::BackSpace || (Key == EKeys::Delete && !Mods.IsShiftDown())) && !Mods.IsAltDown() && !Mods.IsCommandDown()
		&& !IsReadOnly() && !TextWidget->IsComposing() && ReplaceSelection(FString(), dream::UndoKind::Delete))
	{
		bSwallowBackspaceChar = Key == EKeys::BackSpace;   // its WM_CHAR 8 (127 with Ctrl, which the stock swallows) follows
		return FReply::Handled();
	}
	// The IME owns Enter and Backspace while it composes (§4.9.1 flowchart, plain mode keeps Epic's IME path).
	if (bNoMods && !IsReadOnly() && !TextWidget->IsComposing())
	{
		if (Key == EKeys::Enter)
		{
			return HandleSmartEnter();
		}
		if (Key == EKeys::BackSpace && !Mods.IsShiftDown())
		{
			const FReply Reply = HandleSmartBackspace();
			if (Reply.IsEventHandled())
			{
				// The stock widget deletes on Backspace's character event (WM_CHAR 8), which still follows this key-down.
				bSwallowBackspaceChar = true;
				return Reply;
			}
		}
	}
	if (DreamCodeViewLocal::IsStockUndoChord(KeyEvent))
	{
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SDreamCodeView::HandleSmartEnter()
{
	// EDT-03: the newline and its indentation are one edit and one undo group (§4.2 R3). Shift+Enter in a buffer is a
	// newline too (the REPL prompt's Enter keys were taken by OnKeyHook before this).
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::Offset Start = 0;
	dream::Offset End = 0;
	SelectionOffsets(Snapshot, Start, End);
	ApplyBuiltEdit(dream::SmartNewline(Snapshot, Buffer->GetLexer(), Start, End, Indent), dream::UndoKind::Typing);
	return FReply::Handled();
}

FReply SDreamCodeView::HandleSmartBackspace()
{
	// EDT-03 Backspace to the indent stop, and the empty pair "()" deleted whole (EDT-05). Otherwise the stock deletes one.
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::Offset Start = 0;
	dream::Offset End = 0;
	SelectionOffsets(Snapshot, Start, End);
	if (Start != End)
	{
		return FReply::Unhandled();
	}
	return ApplyBuiltEdit(dream::SmartBackspace(Snapshot, Buffer->GetLexer(), End, Indent), dream::UndoKind::Delete)
		? FReply::Handled() : FReply::Unhandled();
}

FReply SDreamCodeView::HandleTextKeyChar(const FGeometry& Geometry, const FCharacterEvent& CharacterEvent)
{
	if (Completion.IsValid() && Completion->OnCharTyped(CharacterEvent))   // (completion) typing triggers; Ctrl+Space's space
	{
		return FReply::Handled();
	}
	const TCHAR Typed = CharacterEvent.GetCharacter();
	if (Typed == TCHAR(8) && bSwallowBackspaceChar)
	{
		bSwallowBackspaceChar = false;   // the smart Backspace of the key-down already deleted
		return FReply::Handled();
	}
	if (IsReadOnly() || TextWidget->IsComposing() || Typed < 0x20 || Typed == 0x7F)
	{
		return FReply::Unhandled();
	}
	// A printable character is typing, whatever key-down came before it in this frame (a Backspace that deleted nothing).
	SetPendingKind(dream::UndoKind::Typing);
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::Offset Start = 0;
	dream::Offset End = 0;
	SelectionOffsets(Snapshot, Start, End);

	// Type-over: the closer that auto-close put right after the caret is stepped over, not doubled.
	if (Start == End && PendingClosers.Num() > 0 && PendingClosers.Last() == End)
	{
		const dream::BuiltEdit Over = dream::TypeOverClosing(Snapshot, End, static_cast<char16_t>(Typed));
		if (!Over.IsEmpty())
		{
			PendingClosers.Pop();
			ApplyBuiltEdit(Over, dream::UndoKind::Typing);
			return FReply::Handled();
		}
	}
	const dream::BuiltEdit Pair = dream::AutoClosePair(Snapshot, Buffer->GetLexer(), Start, End, static_cast<char16_t>(Typed));
	if (ApplyBuiltEdit(Pair, dream::UndoKind::Command))
	{
		// The closer sits right after the caret now (EDT-05 pairing); typing it next steps over it.
		PendingClosers.Add(Pair.Caret);
		return FReply::Handled();
	}
	// Typing over a selection replaces it through the model (the stock delete of the selection can drop a line break).
	if (Start != End && ReplaceSelection(FString(1, &Typed), dream::UndoKind::Typing))
	{
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

bool SDreamCodeView::CutSelection()
{
	if (IsReadOnly() || TextWidget->IsComposing() || !TextWidget->AnyTextSelected())
	{
		return false;
	}
	TextWidget->CopyText();
	return ReplaceSelection(FString(), dream::UndoKind::Command);   // R5: one undo step
}

bool SDreamCodeView::PasteOverSelection()
{
	if (IsReadOnly() || TextWidget->IsComposing() || !TextWidget->AnyTextSelected())
	{
		return false;   // a paste at the caret deletes nothing: the stock paste does it
	}
	FString Pasted;
	FPlatformApplicationMisc::ClipboardPaste(Pasted);
	return ReplaceSelection(Pasted, dream::UndoKind::Command);      // R5: one undo step
}

bool SDreamCodeView::ReplaceSelection(const FString& Text, dream::UndoKind Kind)
{
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::Offset Start = 0;
	dream::Offset End = 0;
	SelectionOffsets(Snapshot, Start, End);
	if (Start == End || IsReadOnly())
	{
		return false;
	}
	dream::BuiltEdit Edit;
	Edit.Batch.Origin = Kind == dream::UndoKind::Typing ? dream::EditOrigin::Typing : dream::EditOrigin::Command;
	dream::EditRange& Range = Edit.Batch.Ranges.emplace_back();
	Range.At = Start;
	Range.DeleteLen = End - Start;
	Range.Insert = std::u16string(DreamText::ToU16(DreamText::ToModelText(Text)));
	Edit.Anchor = Edit.Caret = Start + static_cast<dream::Offset>(Range.Insert.size());
	return ApplyBuiltEdit(Edit, Kind);
}

void SDreamCodeView::DuplicateLineOrSelection()
{
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	dream::Offset Start = 0;
	dream::Offset End = 0;
	SelectionOffsets(Snapshot, Start, End);
	ApplyBuiltEdit(dream::DuplicateLineOrSelection(Snapshot, Start, End), dream::UndoKind::Command);
}

bool SDreamCodeView::ApplyBuiltEdit(const dream::BuiltEdit& Edit, dream::UndoKind Kind)
{
	if (Edit.IsEmpty())
	{
		return false;
	}
	if (!Edit.Batch.Ranges.empty() && IsReadOnly())
	{
		return false;   // every key-driven model edit stops here while read-only or a run is active (A27, RT-01)
	}
	if (!Edit.Batch.Ranges.empty())
	{
		// The history records it with the selection the widget has now and the builder's selection after; then
		// HandleModelChanged pushes the change into the widget.
		History->SetNextEdit(Kind, TOptional<dream::CaretRange>(), dream::CaretRange{ Edit.Anchor, Edit.Caret });
		const dream::ChangeSet Changes = Buffer->ApplyEdit(Edit.Batch);
		if (Changes.IsEmpty())
		{
			History->ClearNextEdit();   // the hints were for this change only
			return false;
		}
	}
	SetSelection(ToSelection(Buffer->Snapshot(), Edit.Anchor, Edit.Caret));
	return true;
}

void SDreamCodeView::SelectWholeLines(int32 AnchorLine, int32 Line)
{
	// (editing) PO-06 F1, as PyCharm and VS Code: the range ends at the start of the line after the last one, so the line
	// break is selected; on the last line, which has none, it ends at the line's end. Dragging up puts the caret at the top.
	const int32 Count = GetLineCount();
	const int32 Anchor = FMath::Clamp(AnchorLine, 0, Count - 1);
	const int32 To = FMath::Clamp(Line, 0, Count - 1);
	const int32 Top = FMath::Min(Anchor, To);
	const int32 Bottom = FMath::Max(Anchor, To);
	const bool bLastLine = Bottom + 1 >= Count;
	const int32 EndLine = bLastLine ? Bottom : Bottom + 1;
	const int32 EndColumn = bLastLine ? GetLineText(Bottom).Len() : 0;
	FDreamTextSelection Selection;
	if (To >= Anchor)
	{
		Selection.AnchorLine = Top;
		Selection.AnchorColumn = 0;
		Selection.CaretLine = EndLine;
		Selection.CaretColumn = EndColumn;
	}
	else
	{
		Selection.AnchorLine = EndLine;
		Selection.AnchorColumn = EndColumn;
		Selection.CaretLine = Top;
		Selection.CaretColumn = 0;
	}
	if (!(GetSelection() == Selection))
	{
		SetSelection(Selection);
	}
}

FReply SDreamCodeView::HandleTab(bool bShift)
{
	if (IsReadOnly())
	{
		return FReply::Handled();   // Tab is consumed in read-only views too (§4.9.4)
	}
	const int32 IndentWidth = FMath::Max(1, Indent.Width);
	const FDreamTextSelection Selection = GetSelection();
	if (Selection.IsEmpty() && !bShift)
	{
		// To the next indentation stop in the buffer's style (new files indent with 4 spaces, BUF-06).
		SetPendingKind(dream::UndoKind::Typing);
		TextWidget->InsertTextAtCursor(Indent.bUseTabs ? FString(TEXT("\t"))
			: FString::ChrN(IndentWidth - (Selection.CaretColumn % IndentWidth), TEXT(' ')));
		return FReply::Handled();
	}

	// (editing) PO-06 F1: any selection, also part of one line or one that starts or ends mid-line, indents (Tab) or
	// dedents (Shift+Tab) every line it touches by one indent unit; Shift+Tab without a selection dedents the caret's
	// line wherever the caret is. Only leading whitespace changes, the selection keeps covering the same text, and the
	// change is one undo step (an edit group of one range per changed line, EDT-03, EDT-08).
	const int32 First = Selection.FirstLine();
	int32 Last = Selection.LastLine();
	const bool bEndsAtColumnZero = Last > First
		&& ((Selection.CaretLine == Last && Selection.CaretColumn == 0) || (Selection.AnchorLine == Last && Selection.AnchorColumn == 0));
	if (bEndsAtColumnZero)
	{
		--Last;   // a whole-line selection (Shift+Down, a triple-click) ends at column 0 of the line after its last line (§4.5)
	}

	const FString Unit = DreamText::ToFString(dream::MakeIndent(IndentWidth, Indent));
	FDreamEditGroup Group;
	Group.Label = bShift ? LOCTEXT("CodeView.Dedent", "Dedent") : LOCTEXT("CodeView.Indent", "Indent");
	TArray<int32> Shift;   // per line in [First, Last]: columns inserted (> 0) or removed (< 0) at column 0
	Shift.SetNumZeroed(Last - First + 1);
	for (int32 Line = First; Line <= Last; ++Line)
	{
		const FString Text = GetLineText(Line);
		if (bShift)
		{
			int32 Remove = 0;
			if (Text.StartsWith(TEXT("\t")))
			{
				Remove = 1;
			}
			else
			{
				while (Remove < IndentWidth && Remove < Text.Len() && Text[Remove] == TEXT(' '))
				{
					++Remove;
				}
			}
			if (Remove > 0)
			{
				FDreamTextEdit& Edit = Group.Edits.AddDefaulted_GetRef();
				Edit.StartLine = Edit.EndLine = Line;
				Edit.StartColumn = 0;
				Edit.EndColumn = Remove;
				Shift[Line - First] = -Remove;
			}
		}
		else if (!Text.IsEmpty())   // an empty line gets no trailing whitespace
		{
			FDreamTextEdit& Edit = Group.Edits.AddDefaulted_GetRef();
			Edit.StartLine = Edit.EndLine = Line;
			Edit.Text = Unit;
			Shift[Line - First] = Unit.Len();
		}
	}
	if (Group.Edits.Num() == 0)
	{
		return FReply::Handled();   // nothing to dedent: no change and no undo step
	}

	// A column keeps its character: an insert at column 0 moves it right, except column 0 itself (a selection that starts
	// at a line start keeps the whole line, its new indent included); a removal moves it left, a column inside the removed
	// whitespace goes to 0.
	const auto MapColumn = [&Shift, First, Last](int32 Line, int32 Column)
	{
		if (Line < First || Line > Last || Shift[Line - First] == 0)
		{
			return Column;
		}
		const int32 Delta = Shift[Line - First];
		return Delta > 0 ? (Column == 0 ? 0 : Column + Delta) : FMath::Max(0, Column + Delta);
	};
	FDreamTextSelection After = Selection;
	After.AnchorColumn = MapColumn(Selection.AnchorLine, Selection.AnchorColumn);
	After.CaretColumn = MapColumn(Selection.CaretLine, Selection.CaretColumn);
	Group.SelectionAfter = After;
	ApplyEditGroup(Group);
	return FReply::Handled();
}

FReply SDreamCodeView::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// A key from a child other than the text (the find bar, the Go to line box) has not met the bound chords yet.
	if (TextWidget.IsValid() && !TextWidget->HasKeyboardFocus())
	{
		TGuardValue<bool> FromChild(bRoutingChildKey, true);
		const FReply Routed = FDreamKeyRouter::Get().RoutePre(InKeyEvent, ComputeContextMask(), *this);
		if (Routed.IsEventHandled())
		{
			return Routed;
		}
	}
	// Key-downs the stock widget left unhandled bubble here: the consume list keeps them from Unreal (§4.9.4, D8).
	return FDreamKeyRouter::Get().RouteBubble(InKeyEvent, ComputeContextMask());
}

FReply SDreamCodeView::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (Completion.IsValid() && Completion->HandleKeyUp(InKeyEvent))   // (completion) the popup's keys and Ctrl+Space
	{
		return FReply::Handled();
	}
	return FDreamKeyRouter::Get().ShouldConsumeKeyUp(InKeyEvent) ? FReply::Handled() : FReply::Unhandled();
}

FReply SDreamCodeView::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!MouseEvent.IsControlDown())
	{
		return FReply::Unhandled();
	}
	// EDT-10: one 10% step per wheel notch; a precision touchpad's fractional deltas add up to whole notches.
	WheelZoomAccumulator += MouseEvent.GetWheelDelta();
	while (WheelZoomAccumulator >= 1.f)
	{
		WheelZoomAccumulator -= 1.f;
		ExecuteThroughTarget(EDreamCommand::ZoomIn);
	}
	while (WheelZoomAccumulator <= -1.f)
	{
		WheelZoomAccumulator += 1.f;
		ExecuteThroughTarget(EDreamCommand::ZoomOut);
	}
	return FReply::Handled();
}

FReply SDreamCodeView::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	return FReply::Handled().SetUserFocus(TextWidget.ToSharedRef(), InFocusEvent.GetCause());
}

void SDreamCodeView::OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath, const FFocusEvent& InFocusEvent)
{
	SCompoundWidget::OnFocusChanging(PreviousFocusPath, NewWidgetPath, InFocusEvent);
	const bool bNowFocused = TextWidget.IsValid() && NewWidgetPath.ContainsWidget(TextWidget.Get());
	if (bNowFocused && !bHadFocus)
	{
		DreamCodeViewLocal::RequestPythonStart(EDreamStartReason::Focus);   // first focus and every focus (RT-17, §4.9.3)
		OnFocused.ExecuteIfBound();
	}
	else if (!bNowFocused && bHadFocus)
	{
		if (Completion.IsValid())
		{
			Completion->OnFocusLost(NewWidgetPath);   // (completion) focus loss closes the popup (§7.6.2)
		}
		if (History.IsValid())
		{
			History->EndImeGroup();        // R6: a composition cannot outlive focus
			History->CloseTypingGroup();   // R4: focus loss ends a typing burst
		}
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetJournal().ForceCommit();   // focus leaving Dream forces a commit (§10.2.5)
		}
	}
	bHadFocus = bNowFocused;
}

// ---- Edits ----

void SDreamCodeView::HandleTextChanged(const FText& NewText)
{
	// The stock widget pushed an undo state (a whole-text copy) before telling us: drop it, its undo never runs (EDT-08).
	TextWidget->DropStockUndo();
	if (bSettingText || !Buffer.IsValid())
	{
		return;
	}
	const bool bComposing = TextWidget->IsComposing();
	if (!bComposing && History->IsImeGroupOpen())
	{
		History->EndImeGroup();   // R6: EndComposition's final OnTextChanged
	}
	FString NewString = NewText.ToString();
	if (NewString.Equals(ReflectedText, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (IsReadOnly())
	{
		// The stock widget is read-only through the same attribute, so this should never happen; if it does, the model wins
		// (A27, RT-01: nothing edits a buffer while it is read-only or a run is active).
		UE_LOG(LogDream, Warning, TEXT("Dream Python Editor code view: %s changed while read-only; the edit was taken back"), *Buffer->GetDisplayName());
		++DreamCodeViewLocal::ReadOnlyTakeBacks;
		bSettingText = true;
		TextWidget->SetWholeText(ReflectedText, Marshaller->GetMutableLayout());
		bSettingText = false;
		return;
	}

	// One contiguous user edit gives the exact range; anything else (a paste over a selection) is still correct.
	dream::EditBatch Batch;
	Batch.Origin = bComposing ? dream::EditOrigin::Ime : dream::EditOrigin::Typing;
	Batch.Ranges.push_back(dream::DiffSingleRange(DreamText::ToU16(ReflectedText), DreamText::ToU16(NewString)));

	// §4.2: the kind this key or menu action set (Delete, Command), an IME composition (R6, one group until it ends), else
	// typing (R1-R3). The selection before is the widget's last one before this edit; the stock edit leaves a caret after
	// the inserted text.
	dream::UndoKind Kind = PendingKindFrame == GFrameCounter ? PendingKind : dream::UndoKind::Typing;
	PendingKindFrame = 0;   // one key's kind is for its own edit only (several keys can arrive in one frame)
	if (bComposing)
	{
		History->BeginImeGroup();
		Kind = dream::UndoKind::Ime;
	}
	const dream::EditRange& Range = Batch.Ranges[0];
	const dream::Offset CaretAfter = Range.At + static_cast<dream::Offset>(Range.Insert.size());
	History->SetNextEdit(Kind,
		dream::CaretRange{ ToOffset(SyncedSnapshot, LastKnownSelection.AnchorLine, LastKnownSelection.AnchorColumn),
			ToOffset(SyncedSnapshot, LastKnownSelection.CaretLine, LastKnownSelection.CaretColumn) },
		dream::CaretRange{ CaretAfter, CaretAfter });

	bReflecting = true;
	const dream::ChangeSet Changes = Buffer->ApplyEdit(Batch);
	if (Changes.IsEmpty() && Buffer->GetModel().Length() != NewString.Len())
	{
		Buffer->ResetText(NewString, dream::EditOrigin::Typing);   // the batch did not validate: take the widget's text
		PendingClosers.Reset();
	}
	bReflecting = false;
	bEditCaretPending = true;   // the stock widget reports the caret this edit moved right after this

	MapPendingClosers(Changes);
	ReflectedText = MoveTemp(NewString);
	SyncedSnapshot = Buffer->Snapshot();
	LastEditSeconds = FPlatformTime::Seconds();
	ScheduleHighlight();
	if (bFindBarOpen)
	{
		ScheduleFind(0.1, /*bSelectFirst*/ false);   // §4.4 after edits: the matches follow the text
	}
	if (Completion.IsValid())
	{
		Completion->OnTextEdited();   // (completion)
	}
	OnEdited.ExecuteIfBound();
}

void SDreamCodeView::HandleModelChanged(const dream::ChangeSet& Changes, const dream::TextSnapshot& Before)
{
	if (bReflecting || bSettingText)
	{
		return;
	}
	// SyncedSnapshot is what the widget shows; it equals Before unless the widget missed a version (then a whole-text sync).
	SyncWidget(Changes, SyncedSnapshot, Buffer->Snapshot());
	if (Changes.Origin == dream::EditOrigin::Load)
	{
		DetectIndentStyle();      // a new text: its indentation is detected again (§4.5)
	}
	LastEditSeconds = FPlatformTime::Seconds();
	ScheduleHighlight();
	if (bFindBarOpen)
	{
		ScheduleFind(0.1, /*bSelectFirst*/ false);
	}
	if (Completion.IsValid())
	{
		Completion->OnTextEdited();   // (completion)
	}
	OnEdited.ExecuteIfBound();
}

void SDreamCodeView::SyncWidget(const dream::ChangeSet& Changes, const dream::TextSnapshot& Before, const dream::TextSnapshot& After)
{
	using namespace DreamCodeViewLocal;
	// The selection the widget had, mapped through the change, so a reload or another view's edit leaves the caret on its
	// text (BUF-04: carets stay put). Callers with a selection of their own set it right after.
	const FDreamTextSelection Old = GetSelection();
	const dream::Offset OldAnchor = FDreamUndoHistory::MapOffset(Changes, ToOffset(Before, Old.AnchorLine, Old.AnchorColumn), false);
	const dream::Offset OldCaret = FDreamUndoHistory::MapOffset(Changes, ToOffset(Before, Old.CaretLine, Old.CaretColumn), false);

	const FString AfterText = DreamText::ToFString(After.Text());
	// Before may be SyncedSnapshot itself, which changes below: decide everything that reads it first.
	const bool bInSync = Changes.VersionBefore == Before.Version();
	const bool bRanges = bInSync && static_cast<int32>(Changes.Ranges.size()) <= MaxRangeSyncs && !TextWidget->IsComposing()
		&& Marshaller->GetLayout() != nullptr;
	bSettingText = true;
	if (bRanges)
	{
		struct FRangeSync
		{
			FTextLocation Start;
			FTextLocation End;
			FString Text;
		};
		TArray<FRangeSync> Syncs;
		Syncs.Reserve(static_cast<int32>(Changes.Ranges.size()));
		int64 Delta = 0;
		for (const dream::ChangeRange& Range : Changes.Ranges)
		{
			const dream::LineNo EndLine = Before.LineOf(Range.At + Range.OldLen);
			std::u16string Inserted;
			After.CopyRange(Range.At + Delta, Range.NewLen, Inserted);
			Delta += Range.NewLen - Range.OldLen;
			FRangeSync& Sync = Syncs.AddDefaulted_GetRef();
			Sync.Start = FTextLocation(Range.StartLine, Range.StartCol16);
			Sync.End = FTextLocation(EndLine, static_cast<int32>(Range.At + Range.OldLen - Before.LineStart(EndLine)));
			Sync.Text = DreamText::ToFString(Inserted);
		}
		// From the last range down, so the earlier ranges' positions stay valid; one stock transaction for all.
		const FTextLayout& Layout = *Marshaller->GetLayout();
		TextWidget->BeginProgrammaticEdit();
		for (int32 Index = Syncs.Num() - 1; Index >= 0; --Index)
		{
			TextWidget->ReplaceRange(Syncs[Index].Start, Syncs[Index].End, Syncs[Index].Text, Layout);
		}
		TextWidget->EndProgrammaticEdit();
		if (!TextWidget->GetText().ToString().Equals(AfterText, ESearchCase::CaseSensitive))
		{
			UE_LOG(LogDream, Warning, TEXT("Dream Python Editor code view: a range sync of %s left the text different from the model; it was set whole"),
				*Buffer->GetDisplayName());
			TextWidget->SetWholeText(AfterText, Marshaller->GetMutableLayout());
		}
	}
	else
	{
		TextWidget->SetWholeText(AfterText, Marshaller->GetMutableLayout());
	}
	bSettingText = false;
	ReflectedText = AfterText;
	SyncedSnapshot = After;
	if (bInSync)
	{
		MapPendingClosers(Changes);
	}
	else
	{
		PendingClosers.Reset();
	}
	SetSelection(ToSelection(After, OldAnchor, OldCaret));
}

void SDreamCodeView::MapPendingClosers(const dream::ChangeSet& Changes)
{
	// A closer auto-close inserted moves with the text; one the change removed is no longer typed over (EDT-05).
	for (int32 Index = PendingClosers.Num() - 1; Index >= 0; --Index)
	{
		if (DreamCodeViewLocal::RemovesCharAt(Changes, PendingClosers[Index]))
		{
			PendingClosers.RemoveAt(Index);
		}
		else
		{
			PendingClosers[Index] = FDreamUndoHistory::MapOffset(Changes, PendingClosers[Index], /*bStickRight*/ true);
		}
	}
}

void SDreamCodeView::HandleCursorMoved(const FTextLocation& Location)
{
	// R4: a caret moved by navigation, a click or find closes the typing burst; the move an edit itself reports does not.
	if (bEditCaretPending)
	{
		bEditCaretPending = false;
	}
	else if (!bSettingText && History.IsValid())
	{
		History->CloseTypingGroup();
	}
	if (!bSettingText)
	{
		LastKnownSelection = GetSelection();
	}
	if (Buffer.IsValid())
	{
		FDreamBufferViewState& ViewState = Buffer->GetViewState();
		ViewState.CaretLine = FMath::Max(0, Location.GetLineIndex());
		ViewState.CaretColumn = FMath::Max(0, Location.GetOffset());
		ViewState.FirstVisibleLine = GetFirstVisibleLine();
		// Type-over holds only while the caret stays inside the pair it typed (EDT-05).
		if (PendingClosers.Num() > 0 && !bSettingText)
		{
			const dream::TextSnapshot Snapshot = Buffer->Snapshot();
			const dream::Offset Caret = ToOffset(Snapshot, ViewState.CaretLine, ViewState.CaretColumn);
			const dream::Offset Closer = PendingClosers.Last();
			if (Caret > Closer || Snapshot.LineOf(Caret) != Snapshot.LineOf(Closer))
			{
				PendingClosers.Reset();
			}
		}
	}
	if (Completion.IsValid() && !bSettingText)
	{
		Completion->OnCaretMoved();   // (completion) a caret that leaves the word closes the popup
	}
	OnCaretMoved.ExecuteIfBound();
}

// ---- Coordinates ----

dream::Offset SDreamCodeView::ToOffset(const dream::TextSnapshot& Snapshot, int32 Line, int32 Column) const
{
	const dream::LineNo SafeLine = FMath::Clamp<dream::LineNo>(Line, 0, Snapshot.LineCount() - 1);
	return Snapshot.LineStart(SafeLine) + FMath::Clamp(Column, 0, Snapshot.LineLength(SafeLine));
}

FDreamTextSelection SDreamCodeView::ToSelection(const dream::TextSnapshot& Snapshot, dream::Offset Anchor, dream::Offset Caret) const
{
	FDreamTextSelection Out;
	const dream::Offset Length = Snapshot.Length();
	Anchor = FMath::Clamp<dream::Offset>(Anchor, 0, Length);
	Caret = FMath::Clamp<dream::Offset>(Caret, 0, Length);
	Out.AnchorLine = Snapshot.LineOf(Anchor);
	Out.AnchorColumn = static_cast<int32>(Anchor - Snapshot.LineStart(Out.AnchorLine));
	Out.CaretLine = Snapshot.LineOf(Caret);
	Out.CaretColumn = static_cast<int32>(Caret - Snapshot.LineStart(Out.CaretLine));
	return Out;
}

void SDreamCodeView::SelectionOffsets(const dream::TextSnapshot& Snapshot, dream::Offset& OutStart, dream::Offset& OutEnd) const
{
	const FDreamTextSelection Selection = GetSelection();
	const dream::Offset A = ToOffset(Snapshot, Selection.AnchorLine, Selection.AnchorColumn);
	const dream::Offset B = ToOffset(Snapshot, Selection.CaretLine, Selection.CaretColumn);
	OutStart = FMath::Min(A, B);
	OutEnd = FMath::Max(A, B);
}

void SDreamCodeView::DetectIndentStyle()
{
	// §4.5 indent detection: the dominant unit of the indented lines; new text and ties take 4 spaces (BUF-06).
	dream::IndentStyle Fallback;
	Fallback.Width = 4;
	Fallback.bUseTabs = false;
	Fallback.TabSize = 4;
	Indent = dream::DetectIndent(Buffer->Snapshot(), Buffer->GetLexer(), Fallback);
}

void SDreamCodeView::ScheduleHighlight()
{
	bHighlightPending = true;
	if (!HighlightTimerHandle.IsValid())
	{
		DREAM_TIMER_EXEMPT(paint-only)
		HighlightTimerHandle = RegisterActiveTimer(0.05f, FWidgetActiveTimerDelegate::CreateSP(this, &SDreamCodeView::HighlightTimer));
	}
}

EActiveTimerReturnType SDreamCodeView::HighlightTimer(double InCurrentTime, float InDeltaTime)
{
	if (FPlatformTime::Seconds() - LastEditSeconds < DreamCodeViewLocal::HighlightDelaySeconds)
	{
		return EActiveTimerReturnType::Continue;
	}
	bHighlightPending = false;
	HighlightTimerHandle.Reset();
	// The stock Tick re-marshals and keeps cursor and selection (ForceRefreshTextLayout), but it re-flows the whole document
	// on the game thread. A large buffer keeps the colours of its load instead (§4.15 row (3): plain mode "with live syntax
	// update off"), so a pause in typing never stalls the editor.
	if (Buffer.IsValid() && Buffer->GetModel().Length() > DreamCodeViewLocal::LiveHighlightMaxUnits)
	{
		return EActiveTimerReturnType::Stop;
	}
	Marshaller->MakeDirty();
	return EActiveTimerReturnType::Stop;
}

#undef LOCTEXT_NAMESPACE
