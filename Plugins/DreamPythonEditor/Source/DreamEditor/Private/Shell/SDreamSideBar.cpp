// SPDX-License-Identifier: Apache-2.0
// (sidebar, PO-05 section 1) The left side bar: Project above Structure (Shell/SDreamSideBar.h).
#include "Shell/SDreamSideBar.h"

#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamKeyRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "Module/DreamEditorModule.h"
#include "Run/DreamRunService.h"
#include "Shell/DreamProjectFiles.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamProjectTree.h"
#include "Shell/SDreamStructureView.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamChrome.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamSideBarLocal
{
	constexpr int32 FocusRetryFrames = 4;

	/** An invisible strip that is hit-testable but handles nothing: a press on it bubbles to the splitter beside it, whose
	 *  hit range covers it, so the target is wider than the 1-px rule that is drawn (chrome R15, A220). */
	TSharedRef<SWidget> Grip(bool bHorizontal)
	{
		return SNew(SBox)
			.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamSideBar.Grip")))
			.WidthOverride(bHorizontal ? FOptionalSize() : FOptionalSize(SDreamSideBar::GripPixels))
			.HeightOverride(bHorizontal ? FOptionalSize(SDreamSideBar::GripPixels) : FOptionalSize())
			.Visibility(EVisibility::Visible);   // SBox defaults to SelfHitTestInvisible
	}
}

void SDreamSideBar::Construct(const FArguments& InArgs, const TWeakPtr<SDreamTab>& InTab)
{
	using namespace DreamSideBarLocal;
	Tab = InTab;
	const UDreamPerProjectUserSettings* Session = UDreamPerProjectUserSettings::Get();
	Width = FMath::Max(MinWidth, Session->SideBarWidth > 0.f ? Session->SideBarWidth : DefaultWidth);
	ProjectFraction = FMath::Clamp(Session->SideBarProjectFraction, 0.1f, 0.9f);

	// Review p2 nit: a hidden Project section lists nothing at start (SetActive(false) after the first listings came too late).
	ProjectTree = SNew(SDreamProjectTree, InTab).StartActive(UDreamUserSettings::Get()->bShowProjectPanel);
	StructureView = MakeStructureView();

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride_Lambda([this]() { return FOptionalSize(GetShownWidth()); })   // the outer splitter sizes this slot to its content
		[
			SNew(SBorder)
			.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamSideBar.Panel")))
			.BorderImage(FDreamChrome::Brush(EDreamChromeSlot::Background))   // a brush whose tint follows the theme
			.ForegroundColor(FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
			.Padding(FMargin(0.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				[
					SAssignNew(SectionSplitter, SSplitter)
					.Style(&FDreamChrome::Splitter())
					.Orientation(Orient_Vertical)
					.PhysicalSplitterHandleSize(HandlePixels)
					.HitDetectionSplitterHandleSize(HitPixels)
					+ SSplitter::Slot()
					.Value(TAttribute<float>::CreateLambda([this]() { return ProjectFraction; }))
					.MinSize(MinSectionHeight)
					.OnSlotResized_Lambda([this](float NewValue) { SetProjectFraction(NewValue); })
					[
						SAssignNew(ProjectSection, SBox)
						[
							MakeSection(LOCTEXT("SideBar.Project", "Project"), TEXT("DreamSideBar.ProjectTitle"), ProjectTree.ToSharedRef(), /*bGripAtTop*/ false)
						]
					]
					+ SSplitter::Slot()
					.Value(TAttribute<float>::CreateLambda([this]() { return 1.f - ProjectFraction; }))
					.MinSize(MinSectionHeight)
					.OnSlotResized_Lambda([this](float NewValue) { SetProjectFraction(1.f - NewValue); })
					[
						SAssignNew(StructureSection, SBox)
						[
							MakeSection(LOCTEXT("SideBar.Structure", "Structure"), TEXT("DreamSideBar.StructureTitle"), StructureView.ToSharedRef(), /*bGripAtTop*/ true)
						]
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					// The right edge, next to the outer splitter's rule: beside the sections, not over the lists' scroll bars.
					Grip(/*bHorizontal*/ false)
				]
			]
		]
	];
	ApplyViewSettings();
}

TSharedRef<SWidget> SDreamSideBar::MakeSection(const FText& Title, FName TitleTag, const TSharedRef<SWidget>& Body, bool bGripAtTop)
{
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(8.f, 4.f, 8.f, 3.f))
			[
				// A small muted title, no header bar (PO-04's look).
				SNew(STextBlock)
				.AddMetaData<FTagMetaData>(FTagMetaData(TitleTag))
				.Font(FDreamChrome::Font(8))
				.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
				.Text(Title)
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				Body
			]
		]
		+ SOverlay::Slot()
		.VAlign(bGripAtTop ? VAlign_Top : VAlign_Bottom)
		[
			DreamSideBarLocal::Grip(/*bHorizontal*/ true)   // next to the sections' 1-px rule
		];
}

TSharedRef<SWidget> SDreamSideBar::MakeStructureView()
{
	// (structure group) The active tab's outline; it draws no title and no background of its own.
	return SNew(SDreamStructureView, Tab);
}

void SDreamSideBar::ApplyViewSettings()
{
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	const bool bProject = Settings->bShowProjectPanel;
	const bool bStructure = Settings->bShowStructurePanel;
	if (ProjectSection.IsValid())
	{
		ProjectSection->SetVisibility(bProject ? EVisibility::Visible : EVisibility::Collapsed);
	}
	if (StructureSection.IsValid())
	{
		StructureSection->SetVisibility(bStructure ? EVisibility::Visible : EVisibility::Collapsed);
	}
	// Both off: the outer splitter skips a collapsed slot and its handle, so neither a strip nor a gap is left.
	SetVisibility(bProject || bStructure ? EVisibility::Visible : EVisibility::Collapsed);
	if (ProjectTree.IsValid())
	{
		ProjectTree->SetActive(bProject);   // hidden: the tree watches and lists nothing (review p1 nit)
	}
}

bool SDreamSideBar::IsProjectShown() const
{
	return GetVisibility() != EVisibility::Collapsed && ProjectSection.IsValid() && ProjectSection->GetVisibility() != EVisibility::Collapsed;
}

bool SDreamSideBar::IsStructureShown() const
{
	return GetVisibility() != EVisibility::Collapsed && StructureSection.IsValid() && StructureSection->GetVisibility() != EVisibility::Collapsed;
}

bool SDreamSideBar::IsSideBarShown() const
{
	return GetVisibility() != EVisibility::Collapsed;
}

TSharedPtr<SWidget> SDreamSideBar::GetProjectSection() const
{
	return ProjectSection;
}

TSharedPtr<SWidget> SDreamSideBar::GetStructureSection() const
{
	return StructureSection;
}

float SDreamSideBar::GetShownWidth() const
{
	// Review p1 m1: never wider than the outer splitter leaves beside the centre column's minimum (a width saved while the
	// window was maximized, a narrower dock later); the saved width stays as it is and comes back when there is room.
	const TSharedPtr<SWidget> Parent = GetParentWidget();
	const float Available = Parent.IsValid() ? static_cast<float>(Parent->GetTickSpaceGeometry().GetLocalSize().X) : 0.f;
	if (Available <= 0.f)
	{
		return Width;   // not laid out yet
	}
	return FMath::Min(Width, FMath::Max(MinWidth, Available - MinCentreWidth - HandlePixels));
}

void SDreamSideBar::SetWidth(float NewWidth)
{
	const float Clamped = FMath::Max(MinWidth, NewWidth);
	if (FMath::IsNearlyEqual(Clamped, Width, 0.01f))
	{
		return;
	}
	Width = Clamped;
	if (!FDreamProjectFiles::IsSecondaryInstance())   // a secondary instance keeps the primary's saved layout (review p1 nit)
	{
		UDreamPerProjectUserSettings::GetMutable()->SideBarWidth = Width;
		SaveLater();
	}
}

void SDreamSideBar::SetProjectFraction(float Fraction)
{
	const float Clamped = FMath::Clamp(Fraction, 0.1f, 0.9f);   // the same clamp as Construct's (review p1 nit)
	if (FMath::IsNearlyEqual(Clamped, ProjectFraction, 0.0001f))
	{
		return;
	}
	ProjectFraction = Clamped;
	if (!FDreamProjectFiles::IsSecondaryInstance())
	{
		UDreamPerProjectUserSettings::GetMutable()->SideBarProjectFraction = ProjectFraction;
		SaveLater();
	}
}

void SDreamSideBar::SaveLater() const
{
	// As the output/code split: the session save writes the per-project ini with its 30 s debounce, and at shutdown.
	if (const TSharedPtr<SDreamTab> Pinned = Tab.Pin())
	{
		Pinned->SaveSessionState(false);
	}
}

TSharedPtr<SWidget> SDreamSideBar::FindFocusable(const TSharedRef<SWidget>& Root)
{
	if (!Root->GetVisibility().IsVisible())
	{
		return nullptr;
	}
	if (Root->SupportsKeyboardFocus())
	{
		return Root;
	}
	FChildren* Children = Root->GetChildren();
	for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
	{
		if (TSharedPtr<SWidget> Found = FindFocusable(Children->GetChildAt(Index)))
		{
			return Found;
		}
	}
	return nullptr;
}

bool SDreamSideBar::TryFocus(bool bStructure)
{
	if (!FSlateApplication::IsInitialized())
	{
		return true;
	}
	TSharedPtr<SWidget> Target;
	if (bStructure)
	{
		if (StructureView.IsValid())
		{
			StaticCastSharedPtr<SDreamStructureView>(StructureView)->FocusList();
			Target = StaticCastSharedPtr<SDreamStructureView>(StructureView)->GetFocusWidget();
			if (!Target.IsValid())
			{
				Target = FindFocusable(StructureView.ToSharedRef());
			}
		}
	}
	else if (ProjectTree.IsValid())
	{
		ProjectTree->FocusTree();
		Target = ProjectTree->GetFocusWidget();
	}
	if (!Target.IsValid())
	{
		return true;   // nothing that takes focus: nothing more to try
	}
	if (!Target->HasKeyboardFocus())
	{
		FSlateApplication::Get().SetKeyboardFocus(Target, EFocusCause::SetDirectly);
	}
	return Target->HasKeyboardFocus();
}

void SDreamSideBar::FocusProject()
{
	if (!UDreamUserSettings::Get()->bShowProjectPanel)
	{
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, true);   // saves it and broadcasts (ApplyViewSettings)
		ApplyViewSettings();
	}
	bFocusStructure = false;
	FocusRetries = TryFocus(false) ? 0 : DreamSideBarLocal::FocusRetryFrames;   // a section just shown takes focus after a layout pass
}

void SDreamSideBar::FocusStructure()
{
	if (!UDreamUserSettings::Get()->bShowStructurePanel)
	{
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, true);
		ApplyViewSettings();
	}
	bFocusStructure = true;
	FocusRetries = TryFocus(true) ? 0 : DreamSideBarLocal::FocusRetryFrames;
}

void SDreamSideBar::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	if (FocusRetries > 0)
	{
		FocusRetries = TryFocus(bFocusStructure) ? 0 : FocusRetries - 1;
	}
}

FReply SDreamSideBar::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const TSharedPtr<SDreamTab> Pinned = Tab.Pin();
	if (!Pinned.IsValid())
	{
		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		if (const TSharedPtr<SDreamCodeView> View = Pinned->GetActiveCodeView())
		{
			View->FocusEditor();
			return FReply::Handled();
		}
	}
	// Review p1 m4: keys the sections left alone are routed here as for a view that is not a code view (Ctx_Output), so the
	// code view's own commands (Undo, Redo, Duplicate Line, Find...) never edit the active script from the side bar; the
	// tab's commands (Run, Save, New Tab...) still run, and the consume list still keeps keys from the Level Editor.
	const uint32 Context = RouteContext();
	const FReply Reply = FDreamKeyRouter::Get().RoutePre(InKeyEvent, Context, *Pinned);
	if (Reply.IsEventHandled())
	{
		return Reply;
	}
	return FDreamKeyRouter::Get().RouteBubble(InKeyEvent, Context);
}

uint32 SDreamSideBar::RouteContext()
{
	uint32 Context = dream::Ctx_Focused | dream::Ctx_Output;
	if (FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetRunService().IsRunActive())
	{
		Context |= dream::Ctx_RunActive;
	}
	return Context;
}

#undef LOCTEXT_NAMESPACE
