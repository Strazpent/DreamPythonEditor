// SPDX-License-Identifier: Apache-2.0
// The non-modal recovery banner (TDD §10.7.1, U26, U30, PER-03): "Recovered n buffers from the session that ended
// unexpectedly at 14:03. [Restore all] [Review] [Discard]", the lost-run line (§10.7.4) and the other inline notices of
// the tab (storage notices, a second instance, a moved project, Python disabled, safe mode). [Review] opens the Recovery
// review panel (SDreamRecoveryReview, §10.7.3). (chrome) PO-04: one slim line in the theme's chrome colours with small text
// actions and a dismiss × (SDreamTab decides what the × does for each banner). Game thread. Slice-internal (chrome).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class DREAMEDITOR_API SDreamRecoveryBanner final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamRecoveryBanner)
		: _bShowActions(true)
		, _bWarning(true)
	{}
		SLATE_ATTRIBUTE(FText, Message)
		/** Restore all / Review / Discard; false for a plain notice with a close button only. */
		SLATE_ARGUMENT(bool, bShowActions)
		SLATE_ARGUMENT(bool, bWarning)
		/** An optional extra action (Enable Python, Restore all in safe mode). */
		SLATE_ATTRIBUTE(FText, ExtraActionText)
		SLATE_EVENT(FSimpleDelegate, OnExtraAction)
		SLATE_EVENT(FSimpleDelegate, OnRestoreAll)
		/** [Review]: the Recovery review panel (§10.7.3); hidden when unbound. */
		SLATE_EVENT(FSimpleDelegate, OnReview)
		/** Optional second extra action (safe mode's Review recovered buffers). */
		SLATE_ATTRIBUTE(FText, SecondActionText)
		SLATE_EVENT(FSimpleDelegate, OnSecondAction)
		SLATE_EVENT(FSimpleDelegate, OnDiscard)
		SLATE_EVENT(FSimpleDelegate, OnDismiss)
		/** When set, OnDismiss is a text button with this label (an inline confirm's Cancel) instead of the × close. */
		SLATE_ARGUMENT(FText, DismissText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};
