// SPDX-License-Identifier: Apache-2.0
// Tools ▸ Dream Python Editor, the Level Editor toolbar button and the WS-07 status-bar item (TDD §11.6.1, §11.6.2, U1, U14).
// LevelEditor.MainMenu.Tools inherits MainFrame.MainMenu.Tools, so the Tools entry is registered once and appears once.
#include "Module/DreamModuleInternal.h"

#include "Module/DreamEditorModule.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamStatusItem.h"
#include "Shell/DreamEditorStyle.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamMenusLocal
{
	FUIAction OpenAction()
	{
		return FUIAction(FExecuteAction::CreateLambda([]()
		{
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().InvokeTab();
			}
		}));
	}

	FSlateIcon TabIcon()
	{
		return FSlateIcon(FDreamEditorStyle::GetStyleSetName(), DreamStyleKeys::TabIcon);
	}
}

void DreamMenus::Register(void* Owner)
{
	using namespace DreamMenusLocal;
	UToolMenus* Menus = UToolMenus::Get();
	if (Menus == nullptr)
	{
		return;
	}
	FToolMenuOwnerScoped OwnerScoped(Owner);

	if (UToolMenu* Tools = Menus->ExtendMenu(TEXT("MainFrame.MainMenu.Tools")))
	{
		FToolMenuSection& Section = Tools->FindOrAddSection(TEXT("Dream"), LOCTEXT("DreamSection", "Dream Python Editor"));
		Section.AddMenuEntry(TEXT("DreamPythonEditor"), LOCTEXT("ToolsEntry", "Dream Python Editor"),
			LOCTEXT("ToolsEntryTip", "Dream Python Editor for Unreal Engine"), TabIcon(), OpenAction());
	}

	if (UDreamUserSettings::Get()->bShowToolbarButton)
	{
		if (UToolMenu* Toolbar = Menus->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User")))
		{
			FToolMenuSection& Section = Toolbar->FindOrAddSection(TEXT("Dream"));
			Section.AddEntry(FToolMenuEntry::InitToolBarButton(TEXT("DreamOpen"), OpenAction(), LOCTEXT("ToolbarLabel", "Dream"),
				LOCTEXT("ToolbarTip", "Open the Dream Python Editor"), TabIcon()));
		}
	}

	// WS-07 (§11.6.2): the widget-entry pattern of the source-control status item.
	if (UToolMenu* StatusBar = Menus->ExtendMenu(TEXT("LevelEditor.StatusBar.ToolBar")))
	{
		FToolMenuSection& Section = StatusBar->FindOrAddSection(TEXT("Dream"));
		Section.AddEntry(FToolMenuEntry::InitWidget(TEXT("DreamStatus"), SNew(SDreamStatusItem), FText::GetEmpty(), true, false));
	}
}

void DreamMenus::Unregister(void* Owner)
{
	UToolMenus::UnregisterOwner(Owner);
}

#undef LOCTEXT_NAMESPACE
