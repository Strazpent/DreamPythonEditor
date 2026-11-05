// SPDX-License-Identifier: Apache-2.0
// The Dream tab's status strip (TDD §6.4 layout, WS-05 M1 subset): run state with the U2 names ("Idle", "Starting",
// "Running 41 s", "Stopping…", "Paused", "Task running"), the namespace mode, the cursor and the journal age, plus a
// transient message (refusal reasons, "End of buffer"). Every field is an attribute read when the editor paints, so it
// follows within a frame (WS-05: 250 ms). (chrome) PO-04: shown only while View ▸ Status Line is on (SDreamTab), in the
// theme's chrome colours. Game thread. Slice-internal (chrome).
// Deferred (M2): language-service health, problem counts, indentation, encoding, line endings, the debug-server line.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class DREAMEDITOR_API SDreamStatusStrip final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamStatusStrip) {}
		SLATE_ATTRIBUTE(FText, Message)
		SLATE_ATTRIBUTE(FText, NamespaceText)
		SLATE_ATTRIBUTE(FText, CursorText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** "Running 41 s", "Starting", "Stopping…" (U2 names from DreamRunText::StateName). */
	static FText GetRunStateText();
	/** "Journal saved 3 s ago", "Journal off" (WS-05 journal age). */
	static FText GetJournalText();
	static FSlateColor GetJournalColor();
};
