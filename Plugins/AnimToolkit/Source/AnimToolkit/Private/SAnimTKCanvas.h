// SPDX-License-Identifier: Apache-2.0
// SAnimTKCanvas: the picker surface. Draws rectangles and circles at canvas coordinates over an optional background
// image; click, shift/ctrl click, marquee, double click, context menu, middle-drag pan, wheel zoom, and in edit mode
// dragging items around (with grid snap). It never changes rig state itself: it reports events and Python decides.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

class FAnimTKWidgetTree;
class FJsonValue;
class FJsonObject;

struct FAnimTKCanvasItem
{
	FString Id;
	FString Label;
	FVector2D Pos = FVector2D::ZeroVector;     // centre, canvas units
	FVector2D Size = FVector2D(24, 24);
	FLinearColor Color = FLinearColor(0.8f, 0.8f, 0.2f);
	bool bCircle = false;
	bool bSelected = false;
};

class SAnimTKCanvas : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SAnimTKCanvas) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<FAnimTKWidgetTree>& InTree, const FString& InWidgetId);

	void SetItems(const TArray<TSharedPtr<FJsonValue>>& InItems);
	void SetSelected(const TArray<FString>& Ids);
	void SetEditMode(bool bInEdit) { bEdit = bInEdit; }
	void SetBackground(const FString& Path) { Background = Path; }
	void SetCanvasSize(const FVector2D& InSize) { CanvasSize = InSize; bNeedsFit = true; }
	void SetSnap(float InSnap) { Snap = InSnap; }
	void SetMenu(const TArray<TSharedPtr<FJsonValue>>& InMenu) { Menu = InMenu; }
	void RequestFit() { bNeedsFit = true; }
	TArray<FString> GetSelected() const;

	// SWidget
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(200, 200); }
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override { Hovered.Reset(); }

private:
	enum class EDrag : uint8 { None, Marquee, Pan, Move };

	FVector2D ToCanvas(const FVector2D& Local) const { return (Local - Pan) / Zoom; }
	FVector2D ToLocal(const FVector2D& CanvasPos) const { return CanvasPos * Zoom + Pan; }
	int32 HitTest(const FVector2D& Local) const;
	void Fit(const FVector2D& LocalSize) const;
	void Emit(const FString& EventName, const TSharedPtr<FJsonObject>& Payload) const;

	TWeakPtr<FAnimTKWidgetTree> Tree;
	FString WidgetId;
	TArray<FAnimTKCanvasItem> Items;
	TArray<TSharedPtr<FJsonValue>> Menu;
	FString Background;
	FVector2D CanvasSize = FVector2D(400, 600);
	float Snap = 0.f;
	bool bEdit = false;

	// View (mutable: fitting happens in paint, once the size is known)
	mutable FVector2D Pan = FVector2D::ZeroVector;
	mutable float Zoom = 1.f;
	mutable bool bNeedsFit = true;

	EDrag Drag = EDrag::None;
	FVector2D DragStartLocal = FVector2D::ZeroVector;
	FVector2D DragNowLocal = FVector2D::ZeroVector;
	TMap<int32, FVector2D> MoveStart;
	bool bDragged = false;
	int32 PressedIndex = INDEX_NONE;
	TOptional<int32> Hovered;
};
