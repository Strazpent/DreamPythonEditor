// SPDX-License-Identifier: Apache-2.0
// Numpad Enter on Win64 (TDD §4.9.3 "Numpad Enter (G7)", brief item 5: Numpad Enter runs the selection or buffer, and
// submits the REPL prompt like Ctrl+Enter). Game thread. Body: DreamNumpadEnter.cpp (shell slice).
// EKeys::Enter does not tell the two Enter keys apart (Engine/Source/Runtime/InputCore/Private/Windows/
// WindowsPlatformInput.cpp:23), so two hooks recover it:
//  - an IWindowsMessageHandler (Engine/Source/Runtime/ApplicationCore/Public/Windows/WindowsApplication.h:262,
//    FWindowsApplication::AddMessageHandler :346, reached through FSlateApplication::Get().GetPlatformApplication()):
//    on WM_KEYDOWN / WM_SYSKEYDOWN with VK_RETURN it pushes lParam bit 24 (extended key = Numpad Enter) into a FIFO of at
//    most 16 entries and never consumes the message (ProcessMessage returns false);
//  - FSlateApplication::OnApplicationPreInputKeyDownListener (Slate/Public/Framework/Application/SlateApplication.h:742),
//    which runs before the pre-processors for every key-down: it pops one entry per EKeys::Enter into
//    bCurrentEnterIsNumpad; an empty FIFO gives false, so a synthetic Enter counts as main Enter (a newline).
// FSlateApplication::OnPostTick (SlateApplication.h:573) purges stale entries. The Windows types stay in the .cpp, so this
// header is platform-neutral; on other platforms IsAvailable() is false. If the G7 suite (Dream.Keys.G7, §4.16.2) fails, the
// Numpad Enter binding is removed; Numpad Enter never falls back to running on main Enter (§4.9.3).
#pragma once

#include "CoreMinimal.h"

class DREAMEDITOR_API FDreamNumpadEnter
{
public:
	static FDreamNumpadEnter& Get();

	/** StartupModule (Win64, Slate initialized): registers both hooks. No thread, no Python, no file I/O (B1). */
	void Register();
	/** ShutdownModule; idempotent. */
	void Unregister();

	/** True on Win64 once both hooks are registered: FDreamKeyRouter then adds the bNumpadEnterOnly binding (dream::Binding). */
	bool IsAvailable() const;

	/** True while the key-down being routed is Numpad Enter. FDreamKeyRouter::ToStroke copies it into Stroke::bNumpadEnter,
	 *  and SDreamReplStrip's Enter handling reads it (§6.5 "Ctrl+Enter; Numpad Enter"). */
	bool IsCurrentEnterNumpad() const { return bCurrentEnterIsNumpad; }

private:
	struct FImpl;                       // the IWindowsMessageHandler and the FIFO live in the .cpp
	TSharedPtr<FImpl> Impl;
	FDelegateHandle PreInputKeyDownHandle;
	FDelegateHandle PostTickHandle;
	bool bCurrentEnterIsNumpad = false;
};
