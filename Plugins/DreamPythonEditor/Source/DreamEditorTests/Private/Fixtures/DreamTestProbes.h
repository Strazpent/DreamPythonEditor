// SPDX-License-Identifier: Apache-2.0
// Run-state probes and widget geometry that the game thread publishes for drivers (TDD §14.2 DreamTestLatch probes and
// the input bot's "widget geometry that the game thread publishes"). The game thread refreshes the snapshot at every
// Slate pre-tick, which also runs inside the supervisor's pump (§5.4.4) and inside a modal loop, and at every modal-loop
// tick (FSlateApplication::GetOnModalLoopTickEvent); the core ticker does not tick during a run (LRS-01), so it is not
// used. Any thread reads GetSnapshot(); nothing on the reading side touches Slate or UObjects.
#pragma once

#include "CoreMinimal.h"
#include "Layout/SlateRect.h"

class SWidget;
class SWindow;

namespace DreamTest
{
	/** Which modal window is active (§14.2: ours, a user dialog, none). */
	enum class EDreamModalKind : uint8
	{
		None,
		RunWindow,          // SDreamRunWindow's popup modal (ours)
		Foreign,            // any other modal: a script's EditorDialog, a slow task dialog, a Save As dialog
	};

	struct FDreamProbeSnapshot
	{
		uint64 Serial = 0;              // +1 per publish
		double PublishedAt = 0.0;       // FPlatformTime::Seconds()
		int32 ActiveRunNumber = 0;      // the active run's number, 0 when Idle
		uint8 RunState = 0;             // EDreamRunState of the active run (Idle when none)
		int32 PumpDepth = 0;            // FRunSupervisor::GetPumpDepth() at the publish
		int32 PumpPublishes = 0;        // publishes made inside a pump of the active run (resets per run)
		bool bRunWindowOpen = false;
		/** FRunSupervisor::IsStopRequested for the current run. */
		bool bStopRequested = false;
		/** The Run window's live run view follows the end of the run (SDreamOutputView::IsFollowing); true when none. */
		bool bLiveViewFollowing = true;
		/** Publishes (since Start) at which GetActiveTopLevelRegularWindow() was the Run window (RV-02: must stay 0). */
		int32 RunWindowWasActiveRegular = 0;
		EDreamModalKind Modal = EDreamModalKind::None;
		bool bMenusVisible = false;
		int64 ProgressDone = -1;
		int64 ProgressTotal = -1;
		/** The Run window's Stop button and the window itself, in desktop pixels, once painted (zero size otherwise). */
		FSlateRect StopButton;
		FSlateRect RunWindow;
		FSlateRect InterruptButton;
		FSlateRect LiveRunView;
		void* RunWindowHwnd = nullptr;  // the OS handle of the Run window (Win64 HWND)
		void* ForeignModalHwnd = nullptr;
		void* MainFrameHwnd = nullptr;
		/** The widgets under the OS cursor at the publish, the innermost three, outer to inner (empty over no Slate window). */
		FString WidgetUnderCursor;
		/** Rects of the widgets a spec registered with Track, by name, in desktop pixels (absent when not shown). */
		TMap<FName, FSlateRect> Tracked;
		TMap<FName, void*> TrackedHwnd;
	};

	class FDreamTestProbes
	{
	public:
		/** DreamEditorTests StartupModule / ShutdownModule (game thread). */
		static void Start();
		static void Stop();

		/** Any thread: a copy of the last publish. */
		static FDreamProbeSnapshot GetSnapshot();

		/** Game thread: publish now (a spec step does this before handing work to the bot). */
		static void PublishNow();

		/** Game thread: the rect of Widget (and its window's OS handle) is published under Name while it is shown. */
		static void Track(FName Name, const TSharedPtr<SWidget>& Widget);
		static void Untrack(FName Name);
		static void UntrackAll();

		/** Game thread helpers shared by specs. */
		static bool GetDesktopRect(const TSharedPtr<SWidget>& Widget, FSlateRect& OutRect);
		static void* GetOsWindowHandle(const TSharedPtr<SWindow>& Window);
	};
}
