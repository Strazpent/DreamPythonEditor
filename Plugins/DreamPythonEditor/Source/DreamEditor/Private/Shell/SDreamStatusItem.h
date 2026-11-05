// SPDX-License-Identifier: Apache-2.0
// The WS-07 Unreal status-bar item (TDD §11.6.2, U14) on LevelEditor.StatusBar.ToolBar: "Dream · state · detail · [Stop]".
// Its text is computed from game-thread run state when the editor paints (no locks; while the Run window is up only the
// modal is drawn, so the item keeps its last painted text, RV-16). A click opens the tab, at the traceback for an error and
// at the run block for not executed / cancelled; [Stop] stops the active run. Game thread. Slice-internal (shell).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class DREAMEDITOR_API SDreamStatusItem final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamStatusItem) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The U14 text for the current state (also used by tests and screen readers). */
	static FText GetStatusText();

private:
	FReply HandleClicked();
	FReply HandleStopClicked();
	static bool IsStopVisible();
};
