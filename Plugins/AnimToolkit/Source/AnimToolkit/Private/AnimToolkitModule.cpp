// SPDX-License-Identifier: Apache-2.0
#include "AnimToolkitModule.h"

#include "AnimTKBrushes.h"
#include "AnimTKPanel.h"
#include "Framework/Docking/WorkspaceItem.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "AnimToolkit"

TSharedPtr<FWorkspaceItem> FAnimToolkitModule::MenuGroup;

void FAnimToolkitModule::StartupModule()
{
}

void FAnimToolkitModule::ShutdownModule()
{
	UAnimTKPanel::ShutdownAll();
	AnimTK::ClearBrushCache();
	MenuGroup.Reset();
}

TSharedRef<FWorkspaceItem> FAnimToolkitModule::GetMenuGroup()
{
	if (!MenuGroup.IsValid())
	{
		MenuGroup = WorkspaceMenu::GetMenuStructure().GetToolsCategory()->AddGroup(
			LOCTEXT("MenuGroup", "Anim Toolkit"),
			LOCTEXT("MenuGroupTooltip", "Animation tools built with the Anim Toolkit Python API"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Layout"),
			true);
	}
	return MenuGroup.ToSharedRef();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAnimToolkitModule, AnimToolkit)
