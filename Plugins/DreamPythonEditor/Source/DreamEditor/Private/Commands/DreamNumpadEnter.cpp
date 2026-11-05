// SPDX-License-Identifier: Apache-2.0
// Numpad Enter on Win64 (TDD §4.9.3 G7): a Windows message hook records lParam bit 24 (extended key) of each VK_RETURN
// key-down in a FIFO; the Slate pre-input listener pops one entry per EKeys::Enter. An empty FIFO means main Enter, so a
// synthetic Enter is a newline, the safe default. Game thread only (WndProc and Slate input both run there).
#include "Commands/DreamNumpadEnter.h"

#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsApplication.h"
#endif

namespace DreamNumpadLocal
{
	constexpr int32 MaxFifoEntries = 16;    // proposed, §4.9.3
}

#if PLATFORM_WINDOWS
struct FDreamNumpadEnter::FImpl final : public IWindowsMessageHandler
{
	struct FEntry
	{
		bool bNumpad = false;
		bool bSurvivedPostTick = false;
	};

	TArray<FEntry> Fifo;
	FWindowsApplication* App = nullptr;

	virtual bool ProcessMessage(HWND Hwnd, uint32 Msg, WPARAM WParam, LPARAM LParam, int32& OutResult) override
	{
		if ((Msg == WM_KEYDOWN || Msg == WM_SYSKEYDOWN) && WParam == VK_RETURN)
		{
			if (Fifo.Num() >= DreamNumpadLocal::MaxFifoEntries)
			{
				Fifo.RemoveAt(0);
			}
			FEntry Entry;
			Entry.bNumpad = (static_cast<uint64>(LParam) & (1ull << 24)) != 0;
			Fifo.Add(Entry);
		}
		return false;   // never consumes the message
	}

	bool Pop()
	{
		if (Fifo.Num() == 0)
		{
			return false;
		}
		const bool bNumpad = Fifo[0].bNumpad;
		Fifo.RemoveAt(0);
		return bNumpad;
	}

	/** Every key message pumped before this frame's Slate tick has been delivered by its post-tick; an entry that is still
	 *  there one post-tick later belongs to a key-down Slate never routed (focus elsewhere, IME), so it is dropped. */
	void PurgeStale()
	{
		Fifo.RemoveAll([](const FEntry& Entry) { return Entry.bSurvivedPostTick; });
		for (FEntry& Entry : Fifo)
		{
			Entry.bSurvivedPostTick = true;
		}
	}
};
#else
struct FDreamNumpadEnter::FImpl
{
	bool Pop() { return false; }
	void PurgeStale() {}
};
#endif

FDreamNumpadEnter& FDreamNumpadEnter::Get()
{
	static FDreamNumpadEnter Instance;
	return Instance;
}

void FDreamNumpadEnter::Register()
{
#if PLATFORM_WINDOWS
	if (Impl.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	TSharedPtr<GenericApplication> PlatformApp = FSlateApplication::Get().GetPlatformApplication();
	if (!PlatformApp.IsValid())
	{
		return;
	}

	TSharedPtr<FImpl> NewImpl = MakeShared<FImpl>();
	NewImpl->App = static_cast<FWindowsApplication*>(PlatformApp.Get());
	NewImpl->App->AddMessageHandler(*NewImpl);
	Impl = NewImpl;

	PreInputKeyDownHandle = FSlateApplication::Get().OnApplicationPreInputKeyDownListener().AddLambda(
		[this](const FKeyEvent& KeyEvent)
		{
			bCurrentEnterIsNumpad = (KeyEvent.GetKey() == EKeys::Enter && Impl.IsValid()) ? Impl->Pop() : false;
		});
	PostTickHandle = FSlateApplication::Get().OnPostTick().AddLambda(
		[this](float)
		{
			if (Impl.IsValid())
			{
				Impl->PurgeStale();
			}
		});
#endif
}

void FDreamNumpadEnter::Unregister()
{
#if PLATFORM_WINDOWS
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnApplicationPreInputKeyDownListener().Remove(PreInputKeyDownHandle);
		FSlateApplication::Get().OnPostTick().Remove(PostTickHandle);
		if (Impl.IsValid() && Impl->App != nullptr && FSlateApplication::Get().GetPlatformApplication().Get() == Impl->App)
		{
			Impl->App->RemoveMessageHandler(*Impl);
		}
	}
	PreInputKeyDownHandle.Reset();
	PostTickHandle.Reset();
#endif
	Impl.Reset();
	bCurrentEnterIsNumpad = false;
}

bool FDreamNumpadEnter::IsAvailable() const
{
#if PLATFORM_WINDOWS
	return Impl.IsValid() && PreInputKeyDownHandle.IsValid();
#else
	return false;
#endif
}
