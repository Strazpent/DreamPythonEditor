// SPDX-License-Identifier: Apache-2.0
// A foreign modal for the RT-05 cases (TDD §4.16.1 RT-05, T-KEY-013, T-SUP-027, T-SUP-028): a Yes / No / Cancel dialog
// that a run opens from Python with _dream_test.show_modal(title, message) and that blocks in its own modal loop, as
// unreal.EditorDialog.show_message(..., YES_NO_CANCEL) does. (tests B-note) show_message itself cannot be used inside a
// spec: FMessageDialog::Open returns its default without opening anything while FApp::IsUnattended(), which is true for
// the whole automation pass (GIsAutomationTesting, Core Misc/App.cpp). The dialog is a plain SWindow added with
// FSlateApplication::AddModalWindow over the active modal; Esc and its Cancel button return "cancel". Game thread.
#pragma once

#include "CoreMinimal.h"

namespace DreamTest
{
	/** Opens the dialog and runs its modal loop; returns "yes", "no" or "cancel" (Esc, the Cancel button, a close), or
	 *  "timeout" when nothing answered it within 20 s (the dialog then closes itself, so a missed Esc never hangs a pass). */
	FString ShowFixtureModal(const FString& Title, const FString& Message);
}
