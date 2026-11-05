// SPDX-License-Identifier: Apache-2.0
// (sidebar, PO-05 section 2, ruling R-p1-10) The Project tree's roots, per user and project, alone in their own settings
// class: config=EditorPerProjectUserSettings (<Project>/Saved/Config/<Platform>/EditorPerProjectUserSettings.ini), section
// [/Script/DreamEditor.DreamProjectTreeSettings]. It is the only Dream class shown as Editor Preferences > Plugins > Dream (this
// project), so that page's Reset to Defaults and Set as Default touch the tree's settings and nothing else (the session
// state of UDreamPerProjectUserSettings stays unregistered). PO-05-A1 adds Add plugin Python folders here. The PO-05 build wrote the roots under
// [/Script/DreamEditor.DreamPerProjectUserSettings]; MigrateOldRootsOnce moves that key here once. Game thread. Owner: sidebar.
// Bodies: Shell/DreamProjectFiles.cpp.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DreamProjectTreeSettings.generated.h"

UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Dream Python Editor (this project)"))
class DREAMEDITOR_API UDreamProjectTreeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	static const UDreamProjectTreeSettings* Get() { return GetDefault<UDreamProjectTreeSettings>(); }
	static UDreamProjectTreeSettings* GetMutable() { return GetMutableDefault<UDreamProjectTreeSettings>(); }

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("DreamProject"); }

	/** The folders the side bar's Project tree shows, like PyCharm's content roots. A relative path resolves against the
	 *  project folder; a missing folder shows as "(not found)" and is never created. <Project>/Content/Python shows first
	 *  whenever it exists, with or without roots here (p3 ruling). The Scratches section always comes last. Add Root... and
	 *  Remove Root in the tree edit this list. */
	UPROPERTY(config, EditAnywhere, Category = "Project Tree", meta = (DisplayName = "Project Roots",
		ToolTip = "Folders shown in the Dream Python Editor side bar's Project tree, after the project's Content/Python (shown whenever it exists). Relative paths resolve against the project folder."))
	TArray<FDirectoryPath> ProjectRoots;

	/** PO-05-A1: the Python folders of the enabled plugins, as Unreal's Python plugin puts them on sys.path (<content
	 *  root>/Python of each mounted content root), shown under "Project Plugins" and "Engine" in the Project tree. Changes
	 *  take effect at once; View > Side Bar > Plugin Python Folders and the tree's background menu toggle the same value. */
	UPROPERTY(config, EditAnywhere, Category = "Project Tree", meta = (DisplayName = "Add plugin Python folders",
		ToolTip = "Show the Python folders of the enabled engine and project plugins in the Dream Python Editor side bar's Project tree."))
	bool bAddPluginPythonFolders = true;

	/** Moves a ProjectRoots key the first PO-05 build wrote under [/Script/DreamEditor.DreamPerProjectUserSettings] into this
	 *  class's section, once per editor session (the old key is removed; roots already set here win). GT. */
	static void MigrateOldRootsOnce();

	/** Fires on the game thread after the roots changed outside an Editor Preferences edit: Add Root / Remove Root, the plugin
	 *  folders toggle (PO-05-A1), and a reload of the config (Reset to Defaults on the page, review p2 nit). The tree rebuilds
	 *  its top level on it. */
	static FSimpleMulticastDelegate& OnRootsChanged();
	virtual void PostReloadConfig(FProperty* PropertyThatWasLoaded) override;
};
