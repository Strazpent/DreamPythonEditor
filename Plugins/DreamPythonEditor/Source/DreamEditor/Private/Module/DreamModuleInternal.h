// SPDX-License-Identifier: Apache-2.0
// Module-internal registration helpers (TDD §11.6.1, §11.6.2, §11.7). Game thread. Slice-internal (shell).
#pragma once

#include "CoreMinimal.h"

namespace DreamMenus
{
	/** From the UToolMenus startup callback: Tools ▸ Dream Python Editor, the toolbar button (bShowToolbarButton) and the
	 *  WS-07 item on LevelEditor.StatusBar.ToolBar. The Window-menu mirror comes from the spawner's group (§11.6.1). */
	void Register(void* Owner);
	void Unregister(void* Owner);
}

namespace DreamConsoleCommands
{
	/** Dream.Open [path[:line]], Dream.RunFile <path>, Dream.ClearOutput (§11.7, API-03). */
	void Register();
	void Unregister();
}
