// SPDX-License-Identifier: Apache-2.0
// (sidebar, PO-05 section 1; WS-02 as amended) The left side bar of the Dream tab: the Project tree on top and the Structure
// view of the active tab below (SDreamStructureView, structure group), split by a thin vertical splitter (60 % / 40 % by
// default). SDreamTab puts it in the left slot of a horizontal splitter beside the centre column, full height beside the
// output and code areas and outside their splitter, so moving the output Top or Bottom never moves it. Width 240 px by
// default, 160 px at least; the width and the split are saved per user and project with the session
// (UDreamPerProjectUserSettings::SideBarWidth, SideBarProjectFraction; SDreamTab::SaveSessionState), as the output/code split
// is. View > Project and View > Structure (UDreamUserSettings::bShowProjectPanel, bShowStructurePanel, both on by default,
// PO-05's exception to PO-04's "off by default") show each section; with one off the other fills the side bar, with both
// off the side bar is collapsed (no strip, no gap). Focus Project and Focus Structure move keyboard focus in (showing the
// section if it was off); Esc gives it back to the code view. PO-04's look: the chrome background, small muted section
// titles, 1-px rules (the splitter handles, drawn in the border colour), no toolbars; every colour a theme slot read at
// paint (A187), so a live theme switch repaints it. Game thread. Owner: sidebar.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SSplitter;
class SDreamProjectTree;
class SDreamTab;

class DREAMEDITOR_API SDreamSideBar : public SCompoundWidget
{
public:
	static constexpr float DefaultWidth = 240.f;
	static constexpr float MinWidth = 160.f;
	/** The centre column's least width beside the side bar (the outer splitter's slot minimum, and the shown width's cap). */
	static constexpr float MinCentreWidth = 240.f;
	static constexpr float DefaultProjectFraction = 0.6f;
	static constexpr float MinSectionHeight = 60.f;
	/** The drawn rule of each splitter, and the invisible grip beside it that takes a press for the splitter (chrome R15). */
	static constexpr float HandlePixels = 1.f;
	static constexpr float GripPixels = 2.f;
	/** SSplitter's hit range is open at both ends: one pixel more than the handle and both grips (review r8, A220). */
	static constexpr float HitPixels = HandlePixels + 2.f * GripPixels + 1.f;

	SLATE_BEGIN_ARGS(SDreamSideBar) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TWeakPtr<SDreamTab>& InTab);

	/** Follows View > Project and View > Structure (an explicit visibility, so it applies in the same frame, A200). */
	void ApplyViewSettings();
	bool IsProjectShown() const;
	bool IsStructureShown() const;
	/** Either section is on (else the side bar is collapsed). */
	bool IsSideBarShown() const;

	/** Focus Project / Focus Structure: the section is shown if it was off (View setting saved), then keyboard focus goes in. */
	void FocusProject();
	void FocusStructure();

	/** The width in Slate units; SetWidth clamps it to MinWidth and saves it with the session (the outer splitter's drag). */
	float GetWidth() const { return Width; }
	void SetWidth(float NewWidth);
	/** The width shown: GetWidth(), but never more than the outer splitter leaves for the centre column's minimum (p1 m1). */
	float GetShownWidth() const;
	/** The Project section's share of the side bar's height. */
	float GetProjectFraction() const { return ProjectFraction; }
	void SetProjectFraction(float Fraction);

	TSharedPtr<SDreamProjectTree> GetProjectTree() const { return ProjectTree; }
	/** The Structure section's view (structure group's SDreamStructureView). */
	TSharedPtr<SWidget> GetStructureView() const { return StructureView; }
	TSharedPtr<SSplitter> GetSectionSplitter() const { return SectionSplitter; }
	TSharedPtr<SWidget> GetProjectSection() const;
	TSharedPtr<SWidget> GetStructureSection() const;

	// ---- SWidget ----
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	/** Esc that no section handled (the Structure view's, after its filter) gives focus back to the code view; other keys are
	 *  routed as in a view that is not a code view (the code view's own commands do nothing here, p1 m4). */
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	/** The key router's context for keys in the side bar: focused, not a code view (Ctx_Output), Ctx_RunActive during a run.
	 *  The Project tree asks it before it takes F5 for Refresh (review p2 nit). */
	static uint32 RouteContext();

private:
	TSharedRef<SWidget> MakeSection(const FText& Title, FName TitleTag, const TSharedRef<SWidget>& Body, bool bGripAtTop);
	TSharedRef<SWidget> MakeStructureView();
	/** The first visible widget under Root that takes keyboard focus. */
	static TSharedPtr<SWidget> FindFocusable(const TSharedRef<SWidget>& Root);
	bool TryFocus(bool bStructure);
	void SaveLater() const;

	TWeakPtr<SDreamTab> Tab;
	TSharedPtr<SSplitter> SectionSplitter;
	TSharedPtr<SBox> ProjectSection;
	TSharedPtr<SBox> StructureSection;
	TSharedPtr<SDreamProjectTree> ProjectTree;
	TSharedPtr<SWidget> StructureView;
	float Width = DefaultWidth;
	float ProjectFraction = DefaultProjectFraction;
	/** A focus request into a section that was just shown: tried again on the next ticks (it needs a layout pass). */
	int32 FocusRetries = 0;
	bool bFocusStructure = false;
};
