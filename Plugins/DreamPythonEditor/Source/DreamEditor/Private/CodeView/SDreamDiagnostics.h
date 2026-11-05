// SPDX-License-Identifier: Apache-2.0
// The code view's live problems, PyCharm style (owner ruling PO-06 F4; INT-07, TDD §7.8): what one SDreamCodeView shows of
// FDreamProblemsModel's items for its buffer:
// - squiggles under the exact range, at least one character wide: a red wave for errors (SynError), a yellow / orange wave
//   for warnings (SynWarning), a weak straight underline for infos and a dotted one for hints; stale items (a crashed
//   server, §7.3.5) at half strength;
// - a hover tooltip on the text with each problem's message and code (and "Ctrl+. for quick fixes" when it has one);
// - a quick-fix bulb after the caret line's text when a problem on that line has a fix (a click is Ctrl+.);
// - error and warning marks on the vertical scroll bar (an overlay stripe: the text never shifts);
// - the inspection widget at the top right of the text (the Error and Warning icons with their counts, (icons) PO-06 F6);
//   a click goes to the next problem (F2).
// Every colour is read at paint or bound (the FSlateColor copy rule, theme R1). Nothing shows while the buffer has no
// problem, so views without problems look and lay out exactly as before. Game thread. Body: SDreamDiagnostics.cpp (lang).
#pragma once

#include "CoreMinimal.h"
#include "Language/DreamProblemsModel.h"
#include "Layout/Margin.h"
#include "Layout/SlateRect.h"   // (icons) FDreamSquiggle::Rect; the header compiles on its own (-FileList)
#include "Templates/SharedPointer.h"

class SDreamCodeView;
struct FSlateBrush;
class SWidget;

/** One painted squiggle segment (one line of a problem's range), in the text widget's local space. */
struct FDreamSquiggle
{
	FSlateRect Rect;
	EDreamSeverity Severity = EDreamSeverity::Error;
	bool bStale = false;
};

/** The per-view state: the buffer's items (re-read when the model's revision moves) and the widgets that show them. */
class DREAMEDITOR_API FDreamCodeViewDiagnostics : public TSharedFromThis<FDreamCodeViewDiagnostics>
{
public:
	/** bEnabled false (the REPL prompt, a view without line numbers): every widget stays collapsed. */
	FDreamCodeViewDiagnostics(const TSharedRef<SDreamCodeView>& InView, bool bInEnabled);

	/** The buffer's merged items, hints included, by position (FDreamProblemsModel::GetForBuffer, cached per revision). */
	const TArray<FDreamProblem>& GetItems() const;
	/** Errors and warnings among GetItems() (the inspection widget's numbers). */
	void GetCounts(int32& OutErrors, int32& OutWarnings) const;
	bool HasMarks() const;

	/** The squiggle segments of the visible lines, in the text widget's local space, infos and hints first, errors last. */
	void GetSquiggles(TArray<FDreamSquiggle>& Out) const;
	/** The problems under a text position as tooltip text ("message (code)" lines); empty when there is none. */
	FText GetToolTipAt(int32 Line, int32 Column) const;
	/** GetToolTipAt under the mouse cursor (the text widget's tooltip attribute: empty means no tooltip). */
	FText GetHoverToolTip() const;
	/** True when a problem on the caret line offers a quick fix; OutSeverity is the worst such problem's. */
	bool GetCaretQuickFix(EDreamSeverity& OutSeverity) const;
	/** Where the bulb sits: after the caret line's text, in the text widget's local space (the overlay slot's padding). */
	FMargin GetBulbMargin() const;

	/** The widgets SDreamCodeView puts into its layout (text overlay, scroll-bar slot). */
	TSharedRef<SWidget> MakeSquiggles();
	TSharedRef<SWidget> MakeBulb();
	TSharedRef<SWidget> MakeInspectionWidget();
	/** The marks: a 9 px hit-test-invisible stripe for an overlay over the scroll bar's place (no layout of its own). */
	TSharedRef<SWidget> MakeScrollMarks();
	/**
	 * The bulb's glyph: (icons) the dream set's Lightbulb (FDreamIcons). Null (no style set yet): a dot is drawn instead.
	 */
	static const FSlateBrush* GetBulbBrush();

	/** Tests: the inspection widget is shown, and its click (the next problem, F2). */
	bool IsInspectionWidgetShown() const;
	void ClickInspectionWidgetForTest();
	/** Tests: the squiggles in the code view's own local space (as FSlateApplication::TakeScreenshot of the view reads). */
	void GetSquigglesInViewSpaceForTest(TArray<FDreamSquiggle>& Out) const;
	/** Tests: the bulb is shown on the caret line. */
	bool IsBulbShown() const;

	/** The character width of the view's code font (a squiggle is at least this wide). */
	float GetCharWidth() const;

private:
	void Refresh() const;

	TWeakPtr<SDreamCodeView> View;
	bool bEnabled = false;
	mutable TArray<FDreamProblem> Items;
	mutable uint64 Revision = MAX_uint64;
	mutable float CachedCharWidth = 0.f;
	mutable int32 CachedCharFontSize = 0;
};
