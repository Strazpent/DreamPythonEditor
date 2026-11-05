// SPDX-License-Identifier: Apache-2.0
// (output, PO-06 F3) Free text selection in the history: row frames, the list's rows and the surface. See
// DreamOutputSelection.h; the selection state and its rules live in SDreamOutputView.cpp (FImpl).
#include "Shell/DreamOutputSelection.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Text/SlateTextLayout.h"
#include "Framework/Text/SlateTextRun.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Shell/DreamTheme.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"   // GetDoubleClickTime
#endif

namespace DreamOutputSelectionPrivate
{
	enum class ECharClass : uint8
	{
		Word,
		Space,
		Other,
	};

	ECharClass ClassOf(TCHAR Char)
	{
		if (FChar::IsAlnum(Char) || Char == TEXT('_') || Char >= 0x80)
		{
			return ECharClass::Word;        // Python identifiers (non-ASCII letters included)
		}
		return FChar::IsWhitespace(Char) ? ECharClass::Space : ECharClass::Other;
	}

	bool IsButton(const SWidget& Widget)
	{
		const FName Type = Widget.GetType();
		return Type == FName(TEXT("SButton")) || Type == FName(TEXT("SHyperlink"));
	}

	TSharedPtr<STextBlock> FindTextBlock(const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			return StaticCastSharedRef<STextBlock>(Widget);
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			if (const TSharedPtr<STextBlock> Found = FindTextBlock(Children->GetChildAt(Index)))
			{
				return Found;
			}
		}
		return nullptr;
	}
}

namespace DreamOutputSelection
{
	void WordAt(const FString& Text, int32 Offset, int32& OutStart, int32& OutEnd)
	{
		using namespace DreamOutputSelectionPrivate;
		const int32 Len = Text.Len();
		if (Len == 0)
		{
			OutStart = OutEnd = 0;
			return;
		}
		int32 At = FMath::Max(0, Offset);
		if (At >= Len)
		{
			At = Len - 1;               // the end of the text: its last character
		}
		else if (At > 0 && ClassOf(Text[At]) != ECharClass::Word && ClassOf(Text[At - 1]) == ECharClass::Word)
		{
			At = At - 1;                // the nearest boundary is just after the word the pointer is on: that word
		}
		const ECharClass Class = ClassOf(Text[At]);
		OutStart = At;
		while (OutStart > 0 && ClassOf(Text[OutStart - 1]) == Class)
		{
			--OutStart;
		}
		OutEnd = At + 1;
		while (OutEnd < Len && ClassOf(Text[OutEnd]) == Class)
		{
			++OutEnd;
		}
	}
}

namespace DreamOutputSelection
{
	double DoubleClickSeconds()
	{
#if PLATFORM_WINDOWS
		return static_cast<double>(::GetDoubleClickTime()) / 1000.0;
#else
		return 0.5;
#endif
	}

	double DragDistance()
	{
		return FSlateApplication::IsInitialized() ? FMath::Max(1.0, static_cast<double>(FSlateApplication::Get().GetDragTriggerDistance())) : 5.0;
	}
}

// ---- SDreamOutputRowFrame -------------------------------------------------------------------------------------------

void SDreamOutputRowFrame::Construct(const FArguments& InArgs, const FDreamOutputRowPtr& InRow, const TWeakPtr<IDreamOutputSelectionHost>& InHost)
{
	Row = InRow;
	Host = InHost;
	ChildSlot
	[
		InArgs._Content.Widget
	];
	CollectPieces(InArgs._Content.Widget);
	Layouts.SetNum(Pieces.Num());
}

void SDreamOutputRowFrame::CollectPieces(const TSharedRef<SWidget>& Widget)
{
	if (DreamOutputSelectionPrivate::IsButton(*Widget))
	{
		Buttons.Add(StaticCastSharedRef<SButton>(Widget));
	}
	if (const TSharedPtr<FDreamOutputPieceMeta> Meta = Widget->GetMetaData<FDreamOutputPieceMeta>())
	{
		FPiece Piece;
		Piece.Widget = Widget;
		Piece.Text = DreamOutputSelectionPrivate::FindTextBlock(Widget);
		Piece.Gap = Meta->Gap;
		Piece.bWrap = Meta->bWrap;
		if (Piece.Text.IsValid())
		{
			Pieces.Add(MoveTemp(Piece));
		}
		return;             // a piece's own children (a link's text block) are part of it
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
	{
		CollectPieces(Children->GetChildAt(Index));
	}
}

void SDreamOutputRowFrame::Place(const FGeometry& Geometry, TArray<FPlaced>& OutPlaced) const
{
	OutPlaced.Reset();
	if (Pieces.IsEmpty())
	{
		return;
	}
	TSet<TSharedRef<SWidget>> Wanted;
	for (const FPiece& Piece : Pieces)
	{
		Wanted.Add(Piece.Text.ToSharedRef());
	}
	TMap<TSharedRef<SWidget>, FArrangedWidget> Found;
	FindChildGeometries(Geometry, Wanted, Found);
	int32 Start = 0;
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		const FPiece& Piece = Pieces[Index];
		Start += Piece.Gap;
		FString Text = Piece.Text->GetText().ToString();
		const int32 Len = Text.Len();
		if (const FArrangedWidget* Arranged = Found.Find(Piece.Text.ToSharedRef()))
		{
			FPlaced& Placed = OutPlaced.AddDefaulted_GetRef();
			Placed.Index = Index;
			Placed.Start = Start;
			Placed.Text = MoveTemp(Text);
			Placed.Geometry = Arranged->Geometry;
		}
		Start += Len;               // a collapsed piece still holds its characters' place
	}
}

FSlateTextLayout& SDreamOutputRowFrame::LayoutOf(const FPlaced& Placed) const
{
	const FPiece& Piece = Pieces[Placed.Index];
	const float Scale = FMath::Max(0.01f, Placed.Geometry.Scale);
	// STextBlock with AutoWrapText wraps at its allotted width (FTextBlockLayout); unwrapped text lays out on one line.
	const float Width = Piece.bWrap ? static_cast<float>(Placed.Geometry.GetLocalSize().X) : 0.f;
	FLayoutCache& Cache = Layouts[Placed.Index];
	if (!Cache.Layout.IsValid() || Cache.Text != Placed.Text || Cache.Width != Width || Cache.Scale != Scale)
	{
		FTextBlockStyle Style = FTextBlockStyle::GetDefault();
		Style.SetFont(Piece.Text->GetFont());
		const TSharedRef<FSlateTextLayout> Layout = FSlateTextLayout::Create(const_cast<SDreamOutputRowFrame*>(this), Style);
		const TSharedRef<FString> Text = MakeShared<FString>(Placed.Text);
		TArray<TSharedRef<IRun>> Runs;
		Runs.Add(FSlateTextRun::Create(FRunInfo(), Text, Style, FTextRange(0, Text->Len())));
		Layout->AddLine(FSlateTextLayout::FNewLineData(Text, MoveTemp(Runs)));
		Layout->SetWrappingWidth(Width);
		Layout->SetScale(Scale);
		Layout->UpdateIfNeeded();
		Cache.Text = Placed.Text;
		Cache.Width = Width;
		Cache.Scale = Scale;
		Cache.Layout = Layout;
	}
	return *Cache.Layout;
}

int32 SDreamOutputRowFrame::OffsetAt(const FGeometry& Geometry, const FVector2D& ScreenPosition) const
{
	TArray<FPlaced> Placed;
	Place(Geometry, Placed);
	if (Placed.IsEmpty())
	{
		return 0;
	}
	// The piece nearest the pointer horizontally (the gap between a header's parts goes to the nearer part).
	int32 Best = 0;
	double BestDistance = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < Placed.Num(); ++Index)
	{
		const FVector2D TopLeft(Placed[Index].Geometry.GetAbsolutePosition());
		const FVector2D Size(Placed[Index].Geometry.GetAbsoluteSize());
		const double Distance = ScreenPosition.X < TopLeft.X ? TopLeft.X - ScreenPosition.X
			: ScreenPosition.X > TopLeft.X + Size.X ? ScreenPosition.X - (TopLeft.X + Size.X) : 0.0;
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = Index;
		}
	}
	const FPlaced& Piece = Placed[Best];
	const FVector2D Local(Piece.Geometry.AbsoluteToLocal(ScreenPosition));
	const FSlateTextLayout& Layout = LayoutOf(Piece);
	const FTextLocation Location = Layout.GetTextLocationAt(Local * Piece.Geometry.Scale);
	return Piece.Start + FMath::Clamp(Location.GetOffset(), 0, Piece.Text.Len());
}

void SDreamOutputRowFrame::RangeRects(const FGeometry& Geometry, int32 From, int32 To, bool bPastEnd, TArray<FSlateRect>& OutRects) const
{
	OutRects.Reset();
	TArray<FPlaced> Placed;
	Place(Geometry, Placed);
	const float FrameHeight = static_cast<float>(Geometry.GetLocalSize().Y);
	float TextRight = 0.f;
	FSlateFontInfo LastFont;
	bool bHasFont = false;
	for (const FPlaced& Piece : Placed)
	{
		const FVector2D PieceRight(Geometry.AbsoluteToLocal(Piece.Geometry.LocalToAbsolute(FVector2D(Piece.Geometry.GetLocalSize().X, 0.0))));
		TextRight = FMath::Max(TextRight, static_cast<float>(PieceRight.X));
		LastFont = Pieces[Piece.Index].Text->GetFont();
		bHasFont = true;
		const int32 Len = Piece.Text.Len();
		const int32 PieceFrom = FMath::Clamp(From - Piece.Start, 0, Len);
		const int32 PieceTo = FMath::Clamp(To == MAX_int32 ? Len : To - Piece.Start, 0, Len);
		if (PieceTo <= PieceFrom)
		{
			continue;
		}
		const FSlateTextLayout& Layout = LayoutOf(Piece);
		const float InvScale = 1.f / FMath::Max(0.01f, Piece.Geometry.Scale);
		const TArray<FTextLayout::FLineView>& Views = Layout.GetLineViews();
		const bool bOneLine = Views.Num() <= 1;
		for (const FTextLayout::FLineView& View : Views)
		{
			const int32 Begin = FMath::Max(PieceFrom, View.Range.BeginIndex);
			const int32 End = FMath::Min(PieceTo, View.Range.EndIndex);
			if (End <= Begin)
			{
				continue;
			}
			const double X1 = Begin == View.Range.BeginIndex ? View.Offset.X : Layout.GetLocationAt(FTextLocation(0, Begin), false).X;
			const double X2 = End == View.Range.EndIndex ? View.Offset.X + View.Size.X : Layout.GetLocationAt(FTextLocation(0, End), true).X;
			FVector2D TopLeft(Geometry.AbsoluteToLocal(Piece.Geometry.LocalToAbsolute(FVector2D(X1 * InvScale, View.Offset.Y * InvScale))));
			FVector2D BottomRight(Geometry.AbsoluteToLocal(Piece.Geometry.LocalToAbsolute(
				FVector2D(X2 * InvScale, (View.Offset.Y + View.Size.Y) * InvScale))));
			if (bOneLine)
			{
				TopLeft.Y = 0.0;            // one band per row, so a selection over several rows has no gaps
				BottomRight.Y = FrameHeight;
			}
			if (BottomRight.X > TopLeft.X)
			{
				OutRects.Add(FSlateRect(TopLeft, BottomRight));
			}
		}
	}
	if (bPastEnd)
	{
		// The line break: a space's width after the drawn text, as text views show a selected line end.
		float Width = FrameHeight * 0.5f;
		if (bHasFont && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer() != nullptr)
		{
			const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
			Width = FMath::Max(2.f, static_cast<float>(Measure->Measure(TEXT(" "), LastFont).X));
		}
		OutRects.Add(FSlateRect(TextRight, 0.f, TextRight + Width, FrameHeight));
	}
}

TSharedPtr<SButton> SDreamOutputRowFrame::ButtonAt(const FGeometry& Geometry, const FVector2D& ScreenPosition) const
{
	if (Buttons.IsEmpty())
	{
		return nullptr;
	}
	TSet<TSharedRef<SWidget>> Wanted;
	for (const TSharedRef<SButton>& Button : Buttons)
	{
		Wanted.Add(Button);
	}
	TMap<TSharedRef<SWidget>, FArrangedWidget> Found;
	FindChildGeometries(Geometry, Wanted, Found);
	// The innermost first: a link inside a header's button is the one meant.
	for (int32 Index = Buttons.Num() - 1; Index >= 0; --Index)
	{
		const FArrangedWidget* Arranged = Found.Find(Buttons[Index]);
		if (Arranged != nullptr && Arranged->Geometry.IsUnderLocation(ScreenPosition) && Buttons[Index]->IsEnabled())
		{
			return Buttons[Index];
		}
	}
	return nullptr;
}

FString SDreamOutputRowFrame::GetDrawnText() const
{
	FString Text;
	for (const FPiece& Piece : Pieces)
	{
		Text += FString::ChrN(Piece.Gap, TEXT(' '));
		Text += Piece.Text->GetText().ToString();
	}
	return Text;
}

int32 SDreamOutputRowFrame::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	int32 From = 0;
	int32 To = 0;
	bool bPastEnd = false;
	const TSharedPtr<IDreamOutputSelectionHost> Pinned = Host.Pin();
	if (Pinned.IsValid() && Row.IsValid() && Pinned->GetRowSelection(*Row, From, To, bPastEnd))
	{
		// Under the text, in the theme's Selection colour (as the code view paints its selection).
		TArray<FSlateRect> Rects;
		RangeRects(AllottedGeometry, From, To, bPastEnd, Rects);
		const FSlateBrush* Brush = FDreamThemes::Brush(EDreamThemeSlot::Selection);
		const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint() * Brush->GetTint(InWidgetStyle);
		for (const FSlateRect& Rect : Rects)
		{
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
				AllottedGeometry.ToPaintGeometry(FVector2f(Rect.GetSize()), FSlateLayoutTransform(FVector2f(Rect.GetTopLeft()))),
				Brush, ESlateDrawEffect::None, Tint);
		}
	}
	return SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId + 1, InWidgetStyle, bParentEnabled);
}

// ---- SDreamOutputTableRow -------------------------------------------------------------------------------------------

void SDreamOutputTableRow::Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner,
	const TSharedRef<SDreamOutputRowFrame>& InFrame, const TWeakPtr<IDreamOutputSelectionHost>& InHost)
{
	Frame = InFrame;
	Host = InHost;
	STableRow<FDreamOutputRowPtr>::FArguments Args;
	if (InArgs._Style != nullptr)
	{
		Args.Style(InArgs._Style);
	}
	STableRow<FDreamOutputRowPtr>::Construct(Args
		.Padding(FMargin(4.f, 0.f))
		.ShowSelection(InArgs._ShowSelection)
		[
			InFrame
		], InOwner);
}

const FSlateBrush* SDreamOutputTableRow::GetBorder() const
{
	const TSharedPtr<IDreamOutputSelectionHost> Pinned = Host.Pin();
	if (Pinned.IsValid() && Style != nullptr && Pinned->HasTextSelection())
	{
		// Characters are selected: the frames paint them, and no row shows the whole-row selection brush.
		return IsHovered() ? &Style->EvenRowBackgroundHoveredBrush : &Style->EvenRowBackgroundBrush;
	}
	return STableRow<FDreamOutputRowPtr>::GetBorder();
}

FReply SDreamOutputTableRow::OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		const TSharedPtr<IDreamOutputSelectionHost> Pinned = Host.Pin();
		return Pinned.IsValid() ? Pinned->HandleSelectDoubleClick(InMouseEvent) : FReply::Handled();
	}
	return STableRow<FDreamOutputRowPtr>::OnMouseButtonDoubleClick(InMyGeometry, InMouseEvent);
}

// ---- SDreamOutputSurface --------------------------------------------------------------------------------------------

void SDreamOutputSurface::Construct(const FArguments& InArgs, const TWeakPtr<IDreamOutputSelectionHost>& InHost)
{
	Host = InHost;
	ChildSlot
	[
		InArgs._Content.Widget
	];
}

FReply SDreamOutputSurface::OnPreviewMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const TSharedPtr<IDreamOutputSelectionHost> Pinned = Host.Pin();
	return Pinned.IsValid() && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton ? Pinned->HandleSelectPress(MouseEvent) : FReply::Unhandled();
}

FReply SDreamOutputSurface::OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const TSharedPtr<IDreamOutputSelectionHost> Pinned = Host.Pin();
	return Pinned.IsValid() && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton ? Pinned->HandleSelectDoubleClick(MouseEvent) : FReply::Unhandled();
}

FReply SDreamOutputSurface::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const TSharedPtr<IDreamOutputSelectionHost> Pinned = Host.Pin();
	return Pinned.IsValid() ? Pinned->HandleSelectMove(MouseEvent) : FReply::Unhandled();
}

FReply SDreamOutputSurface::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const TSharedPtr<IDreamOutputSelectionHost> Pinned = Host.Pin();
	return Pinned.IsValid() && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton ? Pinned->HandleSelectRelease(MouseEvent) : FReply::Unhandled();
}

void SDreamOutputSurface::OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	if (const TSharedPtr<IDreamOutputSelectionHost> Pinned = Host.Pin())
	{
		Pinned->HandleSelectCaptureLost();
	}
	SCompoundWidget::OnMouseCaptureLost(CaptureLostEvent);
}

FCursorReply SDreamOutputSurface::OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const
{
	const TSharedPtr<IDreamOutputSelectionHost> Pinned = Host.Pin();
	return Pinned.IsValid() && Pinned->IsOverRows(CursorEvent.GetScreenSpacePosition()) ? FCursorReply::Cursor(EMouseCursor::TextEditBeam)
		: FCursorReply::Unhandled();
}
