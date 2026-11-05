// SPDX-License-Identifier: Apache-2.0
// The code view's live problems (PO-06 F4). See SDreamDiagnostics.h.
#include "CodeView/SDreamDiagnostics.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamCommands.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Language/DreamLanguageServices.h"
#include "Module/DreamEditorModule.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamIcons.h"   // (icons)
#include "Shell/DreamTheme.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "dream/TextBuffer.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamDiagnosticsLocal
{
	constexpr float MarksWidth = 9.f;           // the scroll bar's thickness (SDreamCodeView)
	constexpr float BulbSize = 14.f;
	constexpr float WaveStep = 2.f;             // half a wave period, px
	constexpr float WaveAmplitude = 1.5f;

	/** Errors paint last (on top), infos and hints first. */
	int32 PaintRank(EDreamSeverity Severity)
	{
		switch (Severity)
		{
		case EDreamSeverity::Error: return 3;
		case EDreamSeverity::Warning: return 2;
		case EDreamSeverity::Info: return 1;
		case EDreamSeverity::Hint: return 0;
		}
		return 0;
	}

	/** The colour of a severity's squiggle, read now (never stored). */
	FLinearColor SeverityColor(EDreamSeverity Severity, bool bStale)
	{
		FLinearColor Color;
		switch (Severity)
		{
		case EDreamSeverity::Error: Color = FDreamThemes::LinearColor(EDreamThemeSlot::SynError); break;
		case EDreamSeverity::Warning: Color = FDreamThemes::LinearColor(EDreamThemeSlot::SynWarning); break;
		case EDreamSeverity::Info: Color = FDreamThemes::LinearColor(EDreamThemeSlot::ChromeMutedText); break;
		case EDreamSeverity::Hint: Color = FDreamThemes::LinearColor(EDreamThemeSlot::ChromeMutedText) * FLinearColor(1.f, 1.f, 1.f, 0.6f); break;
		}
		if (bStale)
		{
			Color.A *= 0.5f;
		}
		return Color;
	}

	const TCHAR* SeverityName(EDreamSeverity Severity)
	{
		switch (Severity)
		{
		case EDreamSeverity::Error: return TEXT("error");
		case EDreamSeverity::Warning: return TEXT("warning");
		case EDreamSeverity::Info: return TEXT("info");
		case EDreamSeverity::Hint: return TEXT("hint");
		}
		return TEXT("");
	}

	bool Contains(const FDreamProblem& Item, int32 Line, int32 Column)
	{
		// A problem covers at least one character (its start column), and a position just past its end still counts, so the
		// tooltip shows over the whole squiggle.
		const int32 EndColumn = Item.EndLine == Item.StartLine ? FMath::Max(Item.EndColumn, Item.StartColumn + 1) : Item.EndColumn;
		if (Line < Item.StartLine || Line > Item.EndLine)
		{
			return false;
		}
		if (Line == Item.StartLine && Column < Item.StartColumn)
		{
			return false;
		}
		return Line != Item.EndLine || Column <= EndColumn;
	}

	/** Paints the squiggles of the visible lines behind nothing and over the text (hit-test invisible). */
	class SSquiggles final : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSquiggles) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&, const TSharedRef<FDreamCodeViewDiagnostics>& InOwner)
		{
			Owner = InOwner;
			SetVisibility(EVisibility::HitTestInvisible);
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = Owner.Pin();
			if (!Pinned.IsValid() || Pinned->GetItems().IsEmpty())
			{
				return LayerId;
			}
			TArray<FDreamSquiggle> Squiggles;
			Pinned->GetSquiggles(Squiggles);
			const float Tint = InWidgetStyle.GetColorAndOpacityTint().A;
			TArray<FVector2f> Points;
			for (const FDreamSquiggle& Squiggle : Squiggles)
			{
				FLinearColor Color = SeverityColor(Squiggle.Severity, Squiggle.bStale);
				Color.A *= Tint;
				const float X0 = static_cast<float>(Squiggle.Rect.Left);
				const float X1 = static_cast<float>(Squiggle.Rect.Right);
				const float Base = static_cast<float>(Squiggle.Rect.Bottom) - 2.f;
				Points.Reset();
				if (Squiggle.Severity == EDreamSeverity::Error || Squiggle.Severity == EDreamSeverity::Warning)
				{
					// PyCharm's wave: a zigzag of 2 px half-periods, 3 px high, on the line's bottom edge.
					int32 Step = 0;
					for (float X = X0; X < X1 + 0.01f; X += WaveStep, ++Step)
					{
						Points.Add(FVector2f(FMath::Min(X, X1), Base + ((Step & 1) != 0 ? -WaveAmplitude : WaveAmplitude * 0.5f)));
					}
					if (Points.Num() < 2)
					{
						Points.Add(FVector2f(X1, Base - WaveAmplitude));
					}
					FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Points, ESlateDrawEffect::None,
						Color, /*bAntialias*/ true, 1.2f);
				}
				else if (Squiggle.Severity == EDreamSeverity::Info)
				{
					Points.Add(FVector2f(X0, Base + 1.f));
					Points.Add(FVector2f(X1, Base + 1.f));
					FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Points, ESlateDrawEffect::None,
						Color, /*bAntialias*/ false, 1.f);
				}
				else
				{
					// A hint: dots every 3 px.
					for (float X = X0; X < X1; X += 3.f)
					{
						Points.Reset();
						Points.Add(FVector2f(X, Base + 1.f));
						Points.Add(FVector2f(FMath::Min(X + 1.f, X1), Base + 1.f));
						FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Points, ESlateDrawEffect::None,
							Color, /*bAntialias*/ false, 1.f);
					}
				}
			}
			return LayerId + 1;
		}

		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
		virtual bool ComputeVolatility() const override { return true; }

	private:
		TWeakPtr<FDreamCodeViewDiagnostics> Owner;
	};

	/** Paints the error and warning marks over the vertical scroll bar's track (hit-test invisible). */
	class SMarks final : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SMarks) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&, const TSharedRef<FDreamCodeViewDiagnostics>& InOwner, const TSharedRef<SDreamCodeView>& InView)
		{
			Owner = InOwner;
			View = InView;
			SetVisibility(EVisibility::HitTestInvisible);
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = Owner.Pin();
			const TSharedPtr<SDreamCodeView> PinnedView = View.Pin();
			if (!Pinned.IsValid() || !PinnedView.IsValid() || !Pinned->HasMarks())
			{
				return LayerId;
			}
			const FVector2f Size(AllottedGeometry.GetLocalSize());
			const float Lines = static_cast<float>(FMath::Max(1, PinnedView->GetLineCount()));
			const FSlateBrush* White = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
			const float Tint = InWidgetStyle.GetColorAndOpacityTint().A;
			// Warnings first, so an error on the same pixel row wins.
			for (const EDreamSeverity Pass : { EDreamSeverity::Warning, EDreamSeverity::Error })
			{
				FLinearColor Color = SeverityColor(Pass, false);
				Color.A *= Tint;
				for (const FDreamProblem& Item : Pinned->GetItems())
				{
					if (Item.Severity != Pass)
					{
						continue;
					}
					const float Y = FMath::Clamp((static_cast<float>(Item.StartLine) + 0.5f) / Lines * Size.Y - 1.5f, 0.f, FMath::Max(0.f, Size.Y - 3.f));
					FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
						AllottedGeometry.ToPaintGeometry(FVector2f(FMath::Max(2.f, Size.X - 2.f), 3.f), FSlateLayoutTransform(FVector2f(1.f, Y))),
						White, ESlateDrawEffect::None, Color);
				}
			}
			return LayerId + 1;
		}

		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
		virtual bool ComputeVolatility() const override { return true; }

	private:
		TWeakPtr<FDreamCodeViewDiagnostics> Owner;
		TWeakPtr<SDreamCodeView> View;
	};
}

FDreamCodeViewDiagnostics::FDreamCodeViewDiagnostics(const TSharedRef<SDreamCodeView>& InView, bool bInEnabled)
	: View(InView)
	, bEnabled(bInEnabled)
{
}

void FDreamCodeViewDiagnostics::Refresh() const
{
	if (!bEnabled || !FDreamEditorModule::IsAvailable())
	{
		Items.Reset();
		return;
	}
	const FDreamProblemsModel& Model = FDreamEditorModule::Get().GetLanguageServices().GetProblems();
	if (Model.GetRevision() == Revision)
	{
		return;
	}
	Revision = Model.GetRevision();
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	const TSharedPtr<FDreamBuffer> Buffer = Pinned.IsValid() ? Pinned->GetBuffer() : nullptr;
	Items = Buffer.IsValid() ? Model.GetForBuffer(Buffer->GetId()) : TArray<FDreamProblem>();
}

const TArray<FDreamProblem>& FDreamCodeViewDiagnostics::GetItems() const
{
	Refresh();
	return Items;
}

void FDreamCodeViewDiagnostics::GetCounts(int32& OutErrors, int32& OutWarnings) const
{
	OutErrors = 0;
	OutWarnings = 0;
	for (const FDreamProblem& Item : GetItems())
	{
		OutErrors += Item.Severity == EDreamSeverity::Error ? 1 : 0;
		OutWarnings += Item.Severity == EDreamSeverity::Warning ? 1 : 0;
	}
}

bool FDreamCodeViewDiagnostics::HasMarks() const
{
	return GetItems().ContainsByPredicate([](const FDreamProblem& Item)
	{
		return Item.Severity == EDreamSeverity::Error || Item.Severity == EDreamSeverity::Warning;
	});
}

float FDreamCodeViewDiagnostics::GetCharWidth() const
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!Pinned.IsValid() || !FSlateApplication::IsInitialized())
	{
		return 7.f;
	}
	if (CachedCharFontSize != Pinned->GetFontSize() || CachedCharWidth <= 0.f)
	{
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		CachedCharWidth = FMath::Max(1.f, static_cast<float>(Measure->Measure(TEXT("M"), Pinned->GetCodeFont()).X));
		CachedCharFontSize = Pinned->GetFontSize();
	}
	return CachedCharWidth;
}

void FDreamCodeViewDiagnostics::GetSquiggles(TArray<FDreamSquiggle>& Out) const
{
	Out.Reset();
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	const TSharedPtr<FDreamBuffer> Buffer = Pinned.IsValid() ? Pinned->GetBuffer() : nullptr;
	const TArray<FDreamProblem>& All = GetItems();
	if (!Buffer.IsValid() || All.IsEmpty())
	{
		return;
	}
	TArray<FDreamLineGeometry> Lines;
	Pinned->GetVisibleLines(Lines);
	if (Lines.IsEmpty())
	{
		return;
	}
	const int32 FirstShown = Lines[0].Line;
	const int32 LastShown = Lines.Last().Line;
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	const int32 LineCount = static_cast<int32>(Snapshot.LineCount());
	const float CharWidth = GetCharWidth();
	TArray<const FDreamProblem*> Ordered;
	for (const FDreamProblem& Item : All)
	{
		if (Item.EndLine >= FirstShown && Item.StartLine <= LastShown && Item.StartLine < LineCount)
		{
			Ordered.Add(&Item);
		}
	}
	Ordered.StableSort([](const FDreamProblem& A, const FDreamProblem& B)
	{
		return DreamDiagnosticsLocal::PaintRank(A.Severity) < DreamDiagnosticsLocal::PaintRank(B.Severity);
	});
	TArray<FSlateRect> Rects;
	for (const FDreamProblem* Item : Ordered)
	{
		const int32 StartLine = FMath::Clamp(Item->StartLine, 0, LineCount - 1);
		const int32 EndLine = FMath::Clamp(Item->EndLine, StartLine, LineCount - 1);
		const dream::Offset Start = Snapshot.LineStart(StartLine) + FMath::Clamp(Item->StartColumn, 0, Snapshot.LineLength(StartLine));
		dream::Offset End = Snapshot.LineStart(EndLine) + FMath::Clamp(Item->EndColumn, 0, Snapshot.LineLength(EndLine));
		if (End <= Start)
		{
			End = Start + 1;    // at least one character (at the line end GetRangeRects gives a thin mark, widened below)
		}
		Rects.Reset();
		Pinned->GetRangeRects(Start, End, Rects, Lines);   // one GetVisibleLines per paint (review lang-q1-m6)
		for (FSlateRect& Rect : Rects)
		{
			if (Rect.Right - Rect.Left < CharWidth)
			{
				Rect.Right = Rect.Left + CharWidth;
			}
			FDreamSquiggle& Squiggle = Out.AddDefaulted_GetRef();
			Squiggle.Rect = Rect;
			Squiggle.Severity = Item->Severity;
			Squiggle.bStale = Item->bStale;
		}
	}
}

FText FDreamCodeViewDiagnostics::GetToolTipAt(int32 Line, int32 Column) const
{
	TArray<FString> Parts;
	bool bFix = false;
	for (const FDreamProblem& Item : GetItems())
	{
		if (!DreamDiagnosticsLocal::Contains(Item, Line, Column))
		{
			continue;
		}
		Parts.Add(Item.Code.IsEmpty() ? Item.Message : FString::Printf(TEXT("%s (%s)"), *Item.Message, *Item.Code));
		bFix |= Item.bHasQuickFix;
	}
	if (Parts.IsEmpty())
	{
		return FText::GetEmpty();
	}
	FString Text = FString::Join(Parts, TEXT("\n"));
	if (bFix)
	{
		Text += TEXT("\n") + LOCTEXT("Lang.TipQuickFix", "Ctrl+. shows the quick fixes").ToString();
	}
	return FText::FromString(Text);
}

FText FDreamCodeViewDiagnostics::GetHoverToolTip() const
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!bEnabled || !Pinned.IsValid() || !FSlateApplication::IsInitialized() || GetItems().IsEmpty())
	{
		return FText::GetEmpty();
	}
	int32 Line = 0;
	int32 Column = 0;
	if (!Pinned->HitTestText(FSlateApplication::Get().GetCursorPos(), Line, Column))
	{
		return FText::GetEmpty();
	}
	return GetToolTipAt(Line, Column);
}

bool FDreamCodeViewDiagnostics::GetCaretQuickFix(EDreamSeverity& OutSeverity) const
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!Pinned.IsValid() || GetItems().IsEmpty() || Pinned->IsReadOnly())
	{
		return false;
	}
	const int32 Caret = Pinned->GetCaretLine();
	bool bFound = false;
	OutSeverity = EDreamSeverity::Hint;
	for (const FDreamProblem& Item : GetItems())
	{
		if (Item.bHasQuickFix && Caret >= Item.StartLine && Caret <= Item.EndLine)
		{
			OutSeverity = bFound ? static_cast<EDreamSeverity>(FMath::Min(static_cast<uint8>(OutSeverity), static_cast<uint8>(Item.Severity))) : Item.Severity;
			bFound = true;
		}
	}
	return bFound;
}

bool FDreamCodeViewDiagnostics::IsBulbShown() const
{
	EDreamSeverity Severity;
	if (!GetCaretQuickFix(Severity))
	{
		return false;
	}
	// Collapsed while the caret line is scrolled out of the view (review lang-q1-m4): never a floating, clickable bulb.
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	TArray<FDreamLineGeometry> Lines;
	Pinned->GetVisibleLines(Lines);
	const int32 Caret = Pinned->GetCaretLine();
	const float Height = static_cast<float>(Pinned->GetTextGeometry().GetLocalSize().Y);
	return Lines.ContainsByPredicate([Caret, Height](const FDreamLineGeometry& L)
	{
		return L.Line == Caret && L.Top >= 0.f && (Height <= 0.f || L.Top + L.Height <= Height + 0.5f);
	});
}

FMargin FDreamCodeViewDiagnostics::GetBulbMargin() const
{
	if (GetItems().IsEmpty())
	{
		return FMargin(0.f);    // the bulb is collapsed: no layout work per frame (review lang-q1-m6)
	}
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	const TSharedPtr<FDreamBuffer> Buffer = Pinned.IsValid() ? Pinned->GetBuffer() : nullptr;
	if (!Buffer.IsValid())
	{
		return FMargin(0.f);
	}
	const int32 Caret = Pinned->GetCaretLine();
	TArray<FDreamLineGeometry> Lines;
	Pinned->GetVisibleLines(Lines);
	const FDreamLineGeometry* Shown = Lines.FindByPredicate([Caret](const FDreamLineGeometry& L) { return L.Line == Caret; });
	if (Shown == nullptr)
	{
		return FMargin(0.f, -100.f, 0.f, 0.f);   // scrolled away: out of sight (the widget collapses then too)
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	const dream::Offset Start = Snapshot.LineStart(Caret);
	TArray<FSlateRect> Rects;
	Pinned->GetRangeRects(Start, Start + FMath::Max(1, Snapshot.LineLength(Caret)), Rects, Lines);
	const float CharWidth = GetCharWidth();
	float X = Rects.Num() > 0 ? static_cast<float>(Rects.Last().Right) + CharWidth : CharWidth;
	if (Rects.Num() > 0 && Snapshot.LineLength(Caret) == 0)
	{
		X = static_cast<float>(Rects.Last().Left) + CharWidth;
	}
	const float Width = static_cast<float>(Pinned->GetTextGeometry().GetLocalSize().X);
	if (Width > DreamDiagnosticsLocal::BulbSize * 2.f)
	{
		X = FMath::Min(X, Width - DreamDiagnosticsLocal::BulbSize - 2.f);
	}
	const float Y = Shown->Top + FMath::Max(0.f, (Shown->Height - DreamDiagnosticsLocal::BulbSize) * 0.5f);
	return FMargin(FMath::Max(0.f, X), Y, 0.f, 0.f);
}

TSharedRef<SWidget> FDreamCodeViewDiagnostics::MakeSquiggles()
{
	return SNew(DreamDiagnosticsLocal::SSquiggles, AsShared());
}

TSharedRef<SWidget> FDreamCodeViewDiagnostics::MakeBulb()
{
	const TWeakPtr<FDreamCodeViewDiagnostics> Weak = AsShared();
	const FSlateBrush* Bulb = GetBulbBrush();
	TSharedRef<SWidget> Glyph = Bulb != nullptr
		? StaticCastSharedRef<SWidget>(SNew(SImage)
			.Image(Bulb)
			.DesiredSizeOverride(FVector2D(DreamDiagnosticsLocal::BulbSize, DreamDiagnosticsLocal::BulbSize))
			.ColorAndOpacity_Lambda([Weak]()
			{
				const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = Weak.Pin();
				EDreamSeverity Severity = EDreamSeverity::Warning;
				const bool bShown = Pinned.IsValid() && Pinned->GetCaretQuickFix(Severity);
				return FSlateColor(FDreamThemes::LinearColor(bShown && Severity == EDreamSeverity::Error ? EDreamThemeSlot::SynError : EDreamThemeSlot::SynWarning));
			}))
		: StaticCastSharedRef<SWidget>(SNew(STextBlock).Text(FText::FromString(TEXT("●"))).ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::SynWarning)));
	return SNew(SBox)
		.WidthOverride(DreamDiagnosticsLocal::BulbSize)
		.HeightOverride(DreamDiagnosticsLocal::BulbSize)
		.Visibility_Lambda([Weak]()
		{
			const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = Weak.Pin();
			return Pinned.IsValid() && Pinned->IsBulbShown() ? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			SNew(SButton)
			.ButtonStyle(&FDreamThemes::FlatButtonStyle())
			.ContentPadding(FMargin(0.f))
			.IsFocusable(false)
			.ToolTipText(LOCTEXT("Lang.BulbTip", "Quick fixes (Ctrl+.)"))
			.OnClicked_Lambda([Weak]()
			{
				const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = Weak.Pin();
				if (const TSharedPtr<SDreamCodeView> PinnedView = Pinned.IsValid() ? Pinned->View.Pin() : nullptr)
				{
					PinnedView->ExecuteDreamCommand(EDreamCommand::QuickFix);
				}
				return FReply::Handled();
			})
			[
				Glyph
			]
		];
}

TSharedRef<SWidget> FDreamCodeViewDiagnostics::MakeInspectionWidget()
{
	const TWeakPtr<FDreamCodeViewDiagnostics> Weak = AsShared();
	auto Count = [Weak](bool bErrors)
	{
		int32 Errors = 0;
		int32 Warnings = 0;
		if (const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = Weak.Pin())
		{
			Pinned->GetCounts(Errors, Warnings);
		}
		return bErrors ? Errors : Warnings;
	};
	const FSlateFontInfo Font = FDreamEditorStyle::GetCodeFont(9);
	return SNew(SBox)
		.Visibility_Lambda([Weak]()
		{
			const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = Weak.Pin();
			return Pinned.IsValid() && Pinned->IsInspectionWidgetShown() ? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			SNew(SBorder)
			.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::EditorBackground))   // the text never shows through the numbers
			.Padding(FMargin(0.f))
			[
				SNew(SButton)
				.ButtonStyle(&FDreamThemes::FlatButtonStyle())
				.ContentPadding(FMargin(4.f, 1.f))
				.IsFocusable(false)
				.ToolTipText_Lambda([Count]()
				{
					return FText::Format(LOCTEXT("Lang.InspectionTip",
						"{0} {0}|plural(one=error,other=errors), {1} {1}|plural(one=warning,other=warnings) in this file. Click or press F2 for the next one (Shift+F2: the previous one)."),
						FText::AsNumber(Count(true)), FText::AsNumber(Count(false)));
				})
				.OnClicked_Lambda([Weak]()
				{
					if (const TSharedPtr<FDreamCodeViewDiagnostics> Pinned = Weak.Pin())
					{
						Pinned->ClickInspectionWidgetForTest();
					}
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SImage)   // (icons) PO-06 F6: the Error icon in place of the ✖ glyph
						.Image(FDreamIcons::Brush(EDreamIcon::Error))
						.DesiredSizeOverride(FVector2D(12.0, 12.0))
						.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::SynError))
						.Visibility_Lambda([Count]() { return Count(true) > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(3.f, 0.f, 0.f, 0.f))
					[
						SNew(STextBlock)
						.Font(Font)
						.Text_Lambda([Count]() { return FText::AsNumber(Count(true)); })
						.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
						.Visibility_Lambda([Count]() { return Count(true) > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.f, 0.f, 0.f, 0.f))
					[
						SNew(SImage)   // (icons) the Warning icon in place of the ▲ glyph
						.Image(FDreamIcons::Brush(EDreamIcon::Warning))
						.DesiredSizeOverride(FVector2D(12.0, 12.0))
						.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::SynWarning))
						.Visibility_Lambda([Count]() { return Count(false) > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(3.f, 0.f, 0.f, 0.f))
					[
						SNew(STextBlock)
						.Font(Font)
						.Text_Lambda([Count]() { return FText::AsNumber(Count(false)); })
						.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
						.Visibility_Lambda([Count]() { return Count(false) > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
					]
				]
			]
		];
}

TSharedRef<SWidget> FDreamCodeViewDiagnostics::MakeScrollMarks()
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!bEnabled || !Pinned.IsValid())
	{
		return SNullWidget::NullWidget;
	}
	return SNew(SBox)
		.WidthOverride(DreamDiagnosticsLocal::MarksWidth)
		.Visibility(EVisibility::HitTestInvisible)
		[
			SNew(DreamDiagnosticsLocal::SMarks, AsShared(), Pinned.ToSharedRef())
		];
}

const FSlateBrush* FDreamCodeViewDiagnostics::GetBulbBrush()
{
	// (icons) PO-06 F6: the dream set's quick-fix bulb (tinted by MakeBulb: the warning colour, the error colour for an error).
	return FDreamIcons::Brush(EDreamIcon::Lightbulb);
}

bool FDreamCodeViewDiagnostics::IsInspectionWidgetShown() const
{
	int32 Errors = 0;
	int32 Warnings = 0;
	GetCounts(Errors, Warnings);
	return bEnabled && (Errors > 0 || Warnings > 0);
}

void FDreamCodeViewDiagnostics::ClickInspectionWidgetForTest()
{
	if (const TSharedPtr<SDreamCodeView> Pinned = View.Pin())
	{
		Pinned->ExecuteDreamCommand(EDreamCommand::NextProblemInFile);
	}
}

void FDreamCodeViewDiagnostics::GetSquigglesInViewSpaceForTest(TArray<FDreamSquiggle>& Out) const
{
	GetSquiggles(Out);
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!Pinned.IsValid())
	{
		Out.Reset();
		return;
	}
	const FGeometry Text = Pinned->GetTextGeometry();
	const FGeometry& Own = Pinned->GetTickSpaceGeometry();
	for (FDreamSquiggle& Squiggle : Out)
	{
		const FVector2D TopLeft = Own.AbsoluteToLocal(Text.LocalToAbsolute(FVector2D(Squiggle.Rect.Left, Squiggle.Rect.Top)));
		const FVector2D BottomRight = Own.AbsoluteToLocal(Text.LocalToAbsolute(FVector2D(Squiggle.Rect.Right, Squiggle.Rect.Bottom)));
		Squiggle.Rect = FSlateRect(TopLeft, BottomRight);
	}
}

#undef LOCTEXT_NAMESPACE
