// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FWorkspaceItem;

class FAnimToolkitModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Window > Tools > Anim Toolkit: the group every listed panel tab joins. */
	static TSharedRef<FWorkspaceItem> GetMenuGroup();

private:
	static TSharedPtr<FWorkspaceItem> MenuGroup;
};
