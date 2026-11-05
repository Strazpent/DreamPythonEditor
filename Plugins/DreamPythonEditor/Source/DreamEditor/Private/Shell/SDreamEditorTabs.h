// SPDX-License-Identifier: Apache-2.0
// The code area's input tab row (TDD §4.13, BUF-02 M1, BUF-03 subset; PO-04 point 1, the Maya-minimal look): plain tabs
// showing the file or scratch name with a dirty "*", a small lock glyph only while the buffer or its file is read-only, a
// small recovered glyph only while a recovery is pending or for a "(recovered)" tab (PER-03), the path as tooltip, a close
// × on the active or hovered tab and middle-click close; a small "+" after the last tab opens a scratch tab. At the
// row's right end: a transient message (while the status line is hidden) and the caret's line:col, muted. No language
// badge (PO-04 overrides BUF-02's). Colours are the theme's chrome slots (Shell/DreamChrome.h). Game thread. Body:
// SDreamEditorTabs.cpp (chrome).
// Deferred (M2): pin and drag reorder (BUF-02). Deferred (M4): the restricted badge (SEC-01).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FDreamBuffer;
class SHorizontalBox;

DECLARE_DELEGATE_OneParam(FDreamOnEditorTabAction, const FGuid& /*BufferId*/);

/** What a tab shows besides its name (BUF-02 M1); the strip's widgets read it on paint. */
struct FDreamTabBadges
{
	/** The buffer's language ("PY" for Python files and every scratch or recovered buffer; another file's extension, upper
	 *  case, up to 4 letters). Data only: PO-04 draws no language badge. */
	FText Language;
	bool bDirty = false;
	/** The buffer is read-only (a decode error, a stub, "Not restored yet") or its file has the read-only attribute. */
	bool bLock = false;
	bool bRecovered = false;
};

class DREAMEDITOR_API SDreamEditorTabs final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamEditorTabs) {}
		SLATE_EVENT(FDreamOnEditorTabAction, OnActivate)
		SLATE_EVENT(FDreamOnEditorTabAction, OnClose)
		SLATE_EVENT(FSimpleDelegate, OnNewTab)
		/** The caret's "line:col" of the active code view (PO-04), small and muted at the row's right end. */
		SLATE_ATTRIBUTE(FText, CaretText)
		/** Left of line:col while the status line is hidden: a health warning, else a transient message (PO-04-R1). */
		SLATE_ATTRIBUTE(FText, MessageText)
		SLATE_ATTRIBUTE(FText, MessageTooltip)
		/** The warning colour for a health warning, muted for a message. */
		SLATE_ATTRIBUTE(FSlateColor, MessageColor)
		/** A click on the message: a health warning's action (SDreamTab::ClickTabRowMessage). */
		SLATE_EVENT(FSimpleDelegate, OnMessageClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Rebuilds the strip (buffers opened, closed, renamed, reordered; the active tab changed). */
	void SetTabs(const TArray<TSharedRef<FDreamBuffer>>& Buffers, const FGuid& ActiveId);

	/** The badges of Buffer as the strip draws them (also Dream.Buffers.Tabs). The dirty "*" and the glyphs carry the tag
	 *  "DreamTab.Dirty", "DreamTab.Lock" or "DreamTab.Recovered" (FTagMetaData); there is no language badge widget. */
	static FDreamTabBadges GetBadges(const FDreamBuffer& Buffer);
	/** The tab's tooltip: the path (or "Scratch tab"), then the read-only, deleted or pending state. */
	static FText GetTooltip(const FDreamBuffer& Buffer);

private:
	TSharedRef<SWidget> MakeTab(const TSharedRef<FDreamBuffer>& Buffer, bool bActive);
	TSharedRef<SWidget> MakeNewTabButton();

	TSharedPtr<SHorizontalBox> Strip;
	FDreamOnEditorTabAction OnActivate;
	FDreamOnEditorTabAction OnClose;
	FSimpleDelegate OnNewTab;
	FSimpleDelegate OnMessageClicked;
};
