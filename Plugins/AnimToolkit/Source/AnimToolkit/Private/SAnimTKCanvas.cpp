// SPDX-License-Identifier: Apache-2.0
#include "SAnimTKCanvas.h"

#include "AnimTKBrushes.h"
#include "AnimTKWidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

void SAnimTKCanvas::Construct(const FArguments& InArgs, const TSharedRef<FAnimTKWidgetTree>& InTree, const FString& InWidgetId)
{
	Tree = InTree;
	WidgetId = InWidgetId;
}

void SAnimTKCanvas::SetItems(const TArray<TSharedPtr<FJsonValue>>& InItems)
{
	TSet<FString> WasSelected;
	for (const FAnimTKCanvasItem& Item : Items)
	{
		if (Item.bSelected)
		{
			WasSelected.Add(Item.Id);
		}
	}

	Items.Reset();
	for (const TSharedPtr<FJsonValue>& Value : InItems)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj))
		{
			continue;
		}
		FAnimTKCanvasItem Item;
		(*Obj)->TryGetStringField(TEXT("id"), Item.Id);
		if (!(*Obj)->TryGetStringField(TEXT("label"), Item.Label))
		{
			Item.Label.Reset();
		}
		double X = 0, Y = 0, W = 24, H = 24;
		(*Obj)->TryGetNumberField(TEXT("x"), X);
		(*Obj)->TryGetNumberField(TEXT("y"), Y);
		(*Obj)->TryGetNumberField(TEXT("w"), W);
		if (!(*Obj)->TryGetNumberField(TEXT("h"), H))
		{
			H = W;
		}
		Item.Pos = FVector2D(X, Y);
		Item.Size = FVector2D(FMath::Max(W, 2.0), FMath::Max(H, 2.0));
		Item.Color = FAnimTKWidgetTree::ParseColor((*Obj)->TryGetField(TEXT("color")), Item.Color);
		FString Shape;
		Item.bCircle = (*Obj)->TryGetStringField(TEXT("shape"), Shape) && Shape == TEXT("circle");
		Item.bSelected = WasSelected.Contains(Item.Id);
		Items.Add(MoveTemp(Item));
	}
}

void SAnimTKCanvas::SetSelected(const TArray<FString>& Ids)
{
	const TSet<FString> IdSet(Ids);
	for (FAnimTKCanvasItem& Item : Items)
	{
		Item.bSelected = IdSet.Contains(Item.Id);
	}
}

TArray<FString> SAnimTKCanvas::GetSelected() const
{
	TArray<FString> Out;
	for (const FAnimTKCanvasItem& Item : Items)
	{
		if (Item.bSelected)
		{
			Out.Add(Item.Id);
		}
	}
	return Out;
}

int32 SAnimTKCanvas::HitTest(const FVector2D& Local) const
{
	const FVector2D P = ToCanvas(Local);
	// Last drawn is on top.
	for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
	{
		const FAnimTKCanvasItem& Item = Items[Index];
		const FVector2D Half = Item.Size * 0.5;
		if (Item.bCircle)
		{
			const FVector2D D = (P - Item.Pos) / Half;
			if (D.SizeSquared() <= 1.0)
			{
				return Index;
			}
		}
		else if (FMath::Abs(P.X - Item.Pos.X) <= Half.X && FMath::Abs(P.Y - Item.Pos.Y) <= Half.Y)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void SAnimTKCanvas::Fit(const FVector2D& LocalSize) const
{
	if (LocalSize.X < 2 || LocalSize.Y < 2 || CanvasSize.X < 1 || CanvasSize.Y < 1)
	{
		return;
	}
	const float Margin = 8.f;
	Zoom = FMath::Min((LocalSize.X - 2 * Margin) / CanvasSize.X, (LocalSize.Y - 2 * Margin) / CanvasSize.Y);
	Zoom = FMath::Clamp(Zoom, 0.05f, 20.f);
	Pan = (LocalSize - CanvasSize * Zoom) * 0.5;
	bNeedsFit = false;
}

void SAnimTKCanvas::Emit(const FString& EventName, const TSharedPtr<FJsonObject>& Payload) const
{
	if (const TSharedPtr<FAnimTKWidgetTree> Pinned = Tree.Pin())
	{
		Pinned->Emit(WidgetId, EventName, Payload);
	}
}

int32 SAnimTKCanvas::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	if (bNeedsFit)
	{
		Fit(LocalSize);
	}

	const FSlateBrush* White = FAppStyle::Get().GetBrush("WhiteBrush");
	const ESlateDrawEffect Effect = ShouldBeEnabled(bParentEnabled) ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;

	// Panel background
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), White, Effect, FLinearColor(0.02f, 0.02f, 0.02f));

	// Canvas area (and the optional background image)
	const FVector2D CanvasTopLeft = ToLocal(FVector2D::ZeroVector);
	const FPaintGeometry CanvasGeo = AllottedGeometry.ToPaintGeometry(CanvasSize * Zoom, FSlateLayoutTransform(CanvasTopLeft));
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1, CanvasGeo, White, Effect, FLinearColor(0.07f, 0.07f, 0.075f));
	if (const FSlateBrush* Image = AnimTK::ResolveBrush(Background, CanvasSize))
	{
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 2, CanvasGeo, Image, Effect, FLinearColor::White);
	}

	// Grid in edit mode
	if (bEdit)
	{
		const float Step = Snap > 1.f ? Snap * 4.f : 25.f;
		const FLinearColor GridColor(1, 1, 1, 0.06f);
		for (float X = 0; X <= CanvasSize.X; X += Step)
		{
			TArray<FVector2f> Line = { FVector2f(ToLocal(FVector2D(X, 0))), FVector2f(ToLocal(FVector2D(X, CanvasSize.Y))) };
			FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 3, AllottedGeometry.ToPaintGeometry(), Line, Effect, GridColor, false, 1.f);
		}
		for (float Y = 0; Y <= CanvasSize.Y; Y += Step)
		{
			TArray<FVector2f> Line = { FVector2f(ToLocal(FVector2D(0, Y))), FVector2f(ToLocal(FVector2D(CanvasSize.X, Y))) };
			FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 3, AllottedGeometry.ToPaintGeometry(), Line, Effect, GridColor, false, 1.f);
		}
	}

	// Items
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 8);
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const FAnimTKCanvasItem& Item = Items[Index];
		const FVector2D SizeLocal = Item.Size * Zoom;
		const FVector2D TopLeft = ToLocal(Item.Pos - Item.Size * 0.5);
		const FPaintGeometry Geo = AllottedGeometry.ToPaintGeometry(SizeLocal, FSlateLayoutTransform(TopLeft));
		const float Radius = Item.bCircle ? FMath::Min(SizeLocal.X, SizeLocal.Y) * 0.5f : FMath::Min(4.f, FMath::Min(SizeLocal.X, SizeLocal.Y) * 0.3f);

		FLinearColor Fill = Item.Color;
		if (Hovered.IsSet() && Hovered.GetValue() == Index)
		{
			Fill = Fill * 1.35f;
			Fill.A = Item.Color.A;
		}
		const FSlateRoundedBoxBrush FillBrush(FLinearColor::White, Radius);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 4, Geo, &FillBrush, Effect, Fill);

		const FLinearColor Outline = Item.bSelected ? FLinearColor::White : FLinearColor(0, 0, 0, 0.6f);
		const float Thickness = Item.bSelected ? 2.f : 1.f;
		const FSlateRoundedBoxBrush OutlineBrush(FLinearColor::Transparent, Radius, FLinearColor::White, Thickness);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 5, Geo, &OutlineBrush, Effect, Outline);

		if (!Item.Label.IsEmpty() && SizeLocal.Y >= 10.f)
		{
			const FVector2D TextSize = Measure->Measure(Item.Label, Font);
			const FVector2D TextPos = TopLeft + (SizeLocal - TextSize) * 0.5;
			FSlateDrawElement::MakeText(OutDrawElements, LayerId + 6, AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(TextPos)), Item.Label, Font, Effect, FLinearColor(0.02f, 0.02f, 0.02f));
		}
	}

	// Marquee
	if (Drag == EDrag::Marquee && bDragged)
	{
		const FVector2D Min(FMath::Min(DragStartLocal.X, DragNowLocal.X), FMath::Min(DragStartLocal.Y, DragNowLocal.Y));
		const FVector2D Max(FMath::Max(DragStartLocal.X, DragNowLocal.X), FMath::Max(DragStartLocal.Y, DragNowLocal.Y));
		const FPaintGeometry Geo = AllottedGeometry.ToPaintGeometry(Max - Min, FSlateLayoutTransform(Min));
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 7, Geo, White, Effect, FLinearColor(0.2f, 0.5f, 1.f, 0.15f));
		const FSlateRoundedBoxBrush Border(FLinearColor::Transparent, 0.f, FLinearColor::White, 1.f);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 8, Geo, &Border, Effect, FLinearColor(0.3f, 0.6f, 1.f, 0.9f));
	}

	if (bEdit)
	{
		const FString Hint = TEXT("EDIT MODE: drag to move, Shift+click to add, F to fit");
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 9, AllottedGeometry.ToPaintGeometry(FVector2D(LocalSize.X, 16), FSlateLayoutTransform(FVector2D(6, 4))), Hint, FCoreStyle::GetDefaultFontStyle("Regular", 8), Effect, FLinearColor(1.f, 0.7f, 0.2f));
	}
	return LayerId + 10;
}

FReply SAnimTKCanvas::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	DragStartLocal = DragNowLocal = Local;
	bDragged = false;

	if (MouseEvent.GetEffectingButton() == EKeys::MiddleMouseButton || (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && MouseEvent.IsAltDown()))
	{
		Drag = EDrag::Pan;
		return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this));
	}
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		return FReply::Handled();
	}
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	const int32 Hit = HitTest(Local);
	PressedIndex = Hit;
	if (bEdit && Hit != INDEX_NONE)
	{
		if (!Items[Hit].bSelected)
		{
			if (!MouseEvent.IsShiftDown())
			{
				for (FAnimTKCanvasItem& Item : Items)
				{
					Item.bSelected = false;
				}
			}
			Items[Hit].bSelected = true;
		}
		MoveStart.Reset();
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (Items[Index].bSelected)
			{
				MoveStart.Add(Index, Items[Index].Pos);
			}
		}
		Drag = EDrag::Move;
	}
	else if (Hit != INDEX_NONE)
	{
		// Selection click: update locally for instant feedback; Python owns the real selection and syncs back.
		FAnimTKCanvasItem& Item = Items[Hit];
		if (MouseEvent.IsControlDown())
		{
			Item.bSelected = !Item.bSelected;
		}
		else if (MouseEvent.IsShiftDown())
		{
			Item.bSelected = true;
		}
		else
		{
			for (FAnimTKCanvasItem& Other : Items)
			{
				Other.bSelected = false;
			}
			Item.bSelected = true;
		}
		TSharedPtr<FJsonObject> Payload = FAnimTKWidgetTree::ModifierPayload();
		Payload->SetStringField(TEXT("item"), Item.Id);
		Emit(TEXT("click"), Payload);
		Drag = EDrag::None;
		return FReply::Handled().SetUserFocus(SharedThis(this));
	}
	else
	{
		Drag = EDrag::Marquee;
	}
	return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this));
}

FReply SAnimTKCanvas::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	if (Drag == EDrag::None)
	{
		const int32 Hit = HitTest(Local);
		Hovered = Hit == INDEX_NONE ? TOptional<int32>() : TOptional<int32>(Hit);
		return FReply::Unhandled();
	}

	if ((Local - DragStartLocal).Size() > 3.0)
	{
		bDragged = true;
	}
	const FVector2D Delta = Local - DragNowLocal;
	DragNowLocal = Local;

	if (Drag == EDrag::Pan)
	{
		Pan += Delta;
	}
	else if (Drag == EDrag::Move && bDragged)
	{
		const FVector2D Offset = (DragNowLocal - DragStartLocal) / Zoom;
		for (const TPair<int32, FVector2D>& Pair : MoveStart)
		{
			FVector2D P = Pair.Value + Offset;
			if (Snap > 0.5f)
			{
				P.X = FMath::GridSnap(P.X, (double)Snap);
				P.Y = FMath::GridSnap(P.Y, (double)Snap);
			}
			Items[Pair.Key].Pos = P;
		}
	}
	return FReply::Handled();
}

FReply SAnimTKCanvas::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());

	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton && Drag == EDrag::None)
	{
		const int32 Hit = HitTest(Local);
		const FString ItemId = Hit == INDEX_NONE ? FString() : Items[Hit].Id;
		if (const TSharedPtr<FAnimTKWidgetTree> Pinned = Tree.Pin())
		{
			if (Menu.Num() > 0)
			{
				const FWidgetPath Path = MouseEvent.GetEventPath() != nullptr ? *MouseEvent.GetEventPath() : FWidgetPath();
				FSlateApplication::Get().PushMenu(SharedThis(this), Path, Pinned->MakeMenu(WidgetId, Menu, ItemId, GetSelected()), MouseEvent.GetScreenSpacePosition(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
			}
		}
		return FReply::Handled();
	}

	const EDrag Was = Drag;
	Drag = EDrag::None;
	FReply Reply = FReply::Handled();
	if (HasMouseCapture())
	{
		Reply.ReleaseMouseCapture();
	}

	if (Was == EDrag::Marquee)
	{
		if (!bDragged)
		{
			if (!MouseEvent.IsShiftDown() && !MouseEvent.IsControlDown())
			{
				for (FAnimTKCanvasItem& Item : Items)
				{
					Item.bSelected = false;
				}
			}
			TSharedPtr<FJsonObject> Payload = FAnimTKWidgetTree::ModifierPayload();
			Payload->SetStringField(TEXT("item"), FString());
			Emit(TEXT("click"), Payload);
			return Reply;
		}
		const FVector2D A = ToCanvas(DragStartLocal);
		const FVector2D B = ToCanvas(Local);
		const FBox2D Box(FVector2D(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y)), FVector2D(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y)));
		TArray<TSharedPtr<FJsonValue>> Hit;
		const bool bAdd = MouseEvent.IsShiftDown();
		const bool bToggle = MouseEvent.IsControlDown();
		for (FAnimTKCanvasItem& Item : Items)
		{
			const bool bInside = Box.IsInside(Item.Pos);
			if (bInside)
			{
				Hit.Add(MakeShared<FJsonValueString>(Item.Id));
				Item.bSelected = bToggle ? !Item.bSelected : true;
			}
			else if (!bAdd && !bToggle)
			{
				Item.bSelected = false;
			}
		}
		TSharedPtr<FJsonObject> Payload = FAnimTKWidgetTree::ModifierPayload();
		Payload->SetArrayField(TEXT("items"), Hit);
		Emit(TEXT("marquee"), Payload);
	}
	else if (Was == EDrag::Move && bDragged)
	{
		TArray<TSharedPtr<FJsonValue>> Moved;
		for (const TPair<int32, FVector2D>& Pair : MoveStart)
		{
			const FAnimTKCanvasItem& Item = Items[Pair.Key];
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("id"), Item.Id);
			Entry->SetNumberField(TEXT("x"), Item.Pos.X);
			Entry->SetNumberField(TEXT("y"), Item.Pos.Y);
			Moved.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetArrayField(TEXT("items"), Moved);
		Emit(TEXT("moved"), Payload);
	}
	else if (Was == EDrag::Move && Items.IsValidIndex(PressedIndex))
	{
		// Edit mode click without a drag: report it so the tool can show the item's properties.
		TSharedPtr<FJsonObject> Payload = FAnimTKWidgetTree::ModifierPayload();
		Payload->SetStringField(TEXT("item"), Items[PressedIndex].Id);
		Emit(TEXT("click"), Payload);
	}
	return Reply;
}

FReply SAnimTKCanvas::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const FVector2D Anchor = ToCanvas(Local);
	const float Factor = MouseEvent.GetWheelDelta() > 0 ? 1.15f : 1.f / 1.15f;
	Zoom = FMath::Clamp(Zoom * Factor, 0.05f, 20.f);
	Pan = Local - Anchor * Zoom;
	return FReply::Handled();
}

FReply SAnimTKCanvas::OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const int32 Hit = HitTest(MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()));
	if (Hit != INDEX_NONE)
	{
		TSharedPtr<FJsonObject> Payload = FAnimTKWidgetTree::ModifierPayload();
		Payload->SetStringField(TEXT("item"), Items[Hit].Id);
		Emit(TEXT("double_click"), Payload);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SAnimTKCanvas::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::F)
	{
		bNeedsFit = true;
		return FReply::Handled();
	}
	TSharedPtr<FJsonObject> Payload = FAnimTKWidgetTree::ModifierPayload();
	Payload->SetStringField(TEXT("key"), InKeyEvent.GetKey().ToString());
	Emit(TEXT("key"), Payload);
	return FReply::Unhandled();
}
