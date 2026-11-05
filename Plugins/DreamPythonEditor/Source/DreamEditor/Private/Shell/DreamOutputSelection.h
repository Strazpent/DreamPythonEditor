// SPDX-License-Identifier: Apache-2.0
// (output, PO-06 F3) Free text selection in the history, as in a read-only text view (Maya's history pane): press and drag
// selects characters across rows, a double-click a word, a triple-click a line, Shift+click extends, Ctrl+A selects every
// shown row and Ctrl+C copies exactly the selected characters as drawn (DreamOutputRows::GetRowDisplayText). Output slice,
// internal, game thread.
//
// (output choice) The rows keep their widgets (STextBlock pieces, SHyperlink links and chips, SButton headers), so the
// rest of the history and its specs are unchanged; each widget that draws part of a row's drawn text is tagged with
// FDreamOutputPieceMeta. SDreamOutputRowFrame wraps a row's widget: it finds the tagged pieces, maps a point to an offset in
// the row's drawn text and an offset range to rectangles (an FSlateTextLayout per piece, so a soft-wrapped piece maps as
// its STextBlock wraps), and paints the selection in the theme's Selection colour under the text. SDreamOutputSurface wraps
// the list: it takes every left press on a row before the row's links and buttons (OnPreviewMouseButtonDown), so a drag
// that starts on a link selects; a press and release without a drag clicks the link or button under it. SDreamOutputTableRow
// hides the list's whole-row selection brush while characters are selected and turns a double-click into a word.
#pragma once

#include "CoreMinimal.h"
#include "Shell/DreamOutputRows.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"      // STableRow's template code needs SListView (STableRow.h)
#include "Widgets/Views/STableRow.h"

class FSlateTextLayout;
class SButton;
class STextBlock;

/** (output, PO-06 F3) Marks a widget that draws a piece of its row's drawn text, in order (DreamOutputSelection::Piece). */
class FDreamOutputPieceMeta : public ISlateMetaData
{
public:
	SLATE_METADATA_TYPE(FDreamOutputPieceMeta, ISlateMetaData)

	FDreamOutputPieceMeta(int32 InGap, bool bInWrap)
		: Gap(InGap)
		, bWrap(bInWrap)
	{
	}

	/** Characters of the row's drawn text between the previous piece and this one that no piece draws (the header's two
	 *  spaces between its left and right parts). */
	int32 Gap = 0;
	/** The piece's STextBlock wraps at its width (the soft-wrap toggle). */
	bool bWrap = false;
};

/** What the row frames, the rows and the surface ask of the view's selection (SDreamOutputView's state implements it). */
class IDreamOutputSelectionHost
{
public:
	virtual ~IDreamOutputSelectionHost() = default;

	/** True while characters are selected (a non-empty selection whose rows are all still shown). */
	virtual bool HasTextSelection() const = 0;
	/** Row's selected characters as [OutFrom, OutTo) of its drawn text (OutTo may be MAX_int32: to its end); bOutPastEnd
	 *  when the selection goes on below the row. False when none of the row is selected. */
	virtual bool GetRowSelection(const FDreamOutputRow& Row, int32& OutFrom, int32& OutTo, bool& bOutPastEnd) const = 0;

	virtual FReply HandleSelectPress(const FPointerEvent& Event) = 0;
	virtual FReply HandleSelectDoubleClick(const FPointerEvent& Event) = 0;
	virtual FReply HandleSelectMove(const FPointerEvent& Event) = 0;
	virtual FReply HandleSelectRelease(const FPointerEvent& Event) = 0;
	virtual void HandleSelectCaptureLost() = 0;
	/** The text cursor over a row's text. */
	virtual bool IsOverRows(const FVector2D& ScreenPosition) const = 0;
};

namespace DreamOutputSelection
{
	/** Tags Widget as the next piece of its row's drawn text (Gap: characters before it that no piece draws). */
	template <typename WidgetType>
	TSharedRef<WidgetType> Piece(const TSharedRef<WidgetType>& Widget, int32 Gap = 0, bool bWrap = false)
	{
		Widget->AddMetadata(MakeShared<FDreamOutputPieceMeta>(Gap, bWrap));
		return Widget;
	}

	/** The word around Offset in Text: a run of identifier characters, of white space, or of other characters, as a
	 *  double-click selects it. [OutStart, OutEnd); empty only for an empty Text. */
	void WordAt(const FString& Text, int32 Offset, int32& OutStart, int32& OutEnd);
	/** The platform's double-click time: a press this soon after a double-click, near it, is a triple-click. */
	double DoubleClickSeconds();
	/** How far the pointer moves, in desktop pixels, before a press is a drag (Slate's drag trigger distance). */
	double DragDistance();
}

/** (output, PO-06 F3) A row's widget with its pieces: hit tests, range rectangles and the selection painted under it. */
class SDreamOutputRowFrame : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamOutputRowFrame) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const FDreamOutputRowPtr& InRow, const TWeakPtr<IDreamOutputSelectionHost>& InHost);

	const FDreamOutputRowPtr& GetRow() const { return Row; }
	/** The offset in the row's drawn text nearest to a point in desktop space (Geometry: this frame's, as last painted). */
	int32 OffsetAt(const FGeometry& Geometry, const FVector2D& ScreenPosition) const;
	/** Rectangles in this frame's local space that cover drawn text [From, To); bPastEnd adds the line break's width after
	 *  the text when the selection goes on below the row. */
	void RangeRects(const FGeometry& Geometry, int32 From, int32 To, bool bPastEnd, TArray<FSlateRect>& OutRects) const;
	/** The enabled link, chip or header button under a point in desktop space, else null. */
	TSharedPtr<SButton> ButtonAt(const FGeometry& Geometry, const FVector2D& ScreenPosition) const;
	/** The pieces' texts joined with their gaps (spaces): the row's drawn text as the widgets show it now. */
	FString GetDrawnText() const;

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	struct FPiece
	{
		TSharedPtr<SWidget> Widget;         // the tagged widget (an STextBlock, or an SHyperlink around one)
		TSharedPtr<STextBlock> Text;        // the text block that draws it
		int32 Gap = 0;
		bool bWrap = false;
	};

	/** A piece where it is drawn now: its text, its first offset in the row's drawn text and its text block's geometry. */
	struct FPlaced
	{
		int32 Index = 0;
		int32 Start = 0;
		FString Text;
		FGeometry Geometry;
	};

	struct FLayoutCache
	{
		FString Text;
		float Width = -1.f;
		float Scale = -1.f;
		TSharedPtr<FSlateTextLayout> Layout;
	};

	void CollectPieces(const TSharedRef<SWidget>& Widget);
	void Place(const FGeometry& Geometry, TArray<FPlaced>& OutPlaced) const;
	/** The piece laid out as its STextBlock lays it out (wrapped at its width when it wraps), in its geometry's scale. */
	FSlateTextLayout& LayoutOf(const FPlaced& Placed) const;

	FDreamOutputRowPtr Row;
	TWeakPtr<IDreamOutputSelectionHost> Host;
	TArray<FPiece> Pieces;
	TArray<TSharedRef<SButton>> Buttons;
	mutable TArray<FLayoutCache> Layouts;
};

/** (output, PO-06 F3) The list's row: no whole-row selection brush while characters are selected; a double-click selects
 *  a word (the list's own double-click no longer opens a line's first link: a click on the link does). */
class SDreamOutputTableRow : public STableRow<FDreamOutputRowPtr>
{
public:
	SLATE_BEGIN_ARGS(SDreamOutputTableRow)
		: _Style(nullptr)
		, _ShowSelection(true)
	{}
		SLATE_ARGUMENT(const FTableRowStyle*, Style)
		SLATE_ARGUMENT(bool, ShowSelection)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner, const TSharedRef<SDreamOutputRowFrame>& InFrame,
		const TWeakPtr<IDreamOutputSelectionHost>& InHost);

	const TSharedPtr<SDreamOutputRowFrame>& GetFrame() const { return Frame; }

	virtual const FSlateBrush* GetBorder() const override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent) override;

private:
	TSharedPtr<SDreamOutputRowFrame> Frame;
	TWeakPtr<IDreamOutputSelectionHost> Host;
};

/** (output, PO-06 F3) Wraps the list: left presses on rows start a selection before the rows' links and buttons see them;
 *  the drag and the release come here through the mouse capture. Presses on the scroll bar go on to the list. */
class SDreamOutputSurface : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamOutputSurface) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TWeakPtr<IDreamOutputSelectionHost>& InHost);

	virtual FReply OnPreviewMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;

private:
	TWeakPtr<IDreamOutputSelectionHost> Host;
};
