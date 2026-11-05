// SPDX-License-Identifier: Apache-2.0
// The Run window (TDD §5.5.3, D2, RT-03, RT-04, RV-02, RV-16, RV-17, PO-03): a popup modal exactly over the output area.
// Game thread; created inside the first pump's GIL-released region, before PumpMessages. Body: SDreamRunWindow.cpp
// (supervisor slice).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Run/DreamRunTypes.h"

class SWindow;
class SDreamOutputView;
struct FDreamRunPolicy;

/** The window's content: Title, State, Location, Progress, live run view, Undo meter, Notes, Buttons, Footer rows. */
class DREAMEDITOR_API SDreamRunWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamRunWindow)
		: _RunNumber(0)
		, _Stats(nullptr)
	{}
		SLATE_ARGUMENT(int32, RunNumber)
		SLATE_ARGUMENT(FString, TabOrItem)
		/** Bound to FRunSupervisor::GetLiveStats(); read at paint (no locks, §5.4.7). */
		SLATE_ARGUMENT(const FDreamRunLiveStats*, Stats)
		SLATE_EVENT(FSimpleDelegate, OnStop)
		SLATE_EVENT(FSimpleDelegate, OnInterruptCleanup)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * §5.5.3 steps 3-5: a popup SWindow (no title bar, close button, minimise or maximise; activation per FApp::HasFocus;
	 * a destroy override that refuses while the run is active) placed over Policy.OutputAreaRect in desktop pixels
	 * (AutoCenter None, AdjustInitialSizeAndPositionForDPIScale false), clamped to 480 x H_min Slate units and the work
	 * area, or 640 x 360 centred when the area is not visible; then AddModalWindow(Window, Parent, true) and ShowWindow.
	 * Returns null when FSlateApplication::CanAddModalWindow() is false or a foreign modal is active (§5.5.3 step 2).
	 */
	static TSharedPtr<SWindow> OpenModal(const FDreamRunPolicy& Policy, const TSharedRef<SDreamRunWindow>& Content);

	/** Teardown steps 1-4 (§5.5.3): detach child windows, re-own HWNDs to the main frame (Win64), hand activation back to the
	 *  window the run came from (the Dream tab's window, recorded by OpenModal), clear the destroy override and
	 *  RequestDestroyWindow, then re-parent kept children. Leaves Window null. Returns whether the Run window was the OS
	 *  foreground window at teardown: only then is focus Dream's to give back (review r6 m3). */
	static bool Teardown(TSharedPtr<SWindow>& Window);

	/** The live run view: an SDreamOutputView filtered to this run, no focus or selection during the run (PO-03). */
	TSharedPtr<SDreamOutputView> GetLiveRunView() const { return LiveRunView; }
	/** Stop, Interrupt cleanup and the live view, for FDreamInputPreProcessor::FContext (§5.6.4). */
	TArray<TWeakPtr<SWidget>> GetStopTargets() const;

	/** H_min in Slate units (§5.5.3 step 4): the content's desired height with the live run view at 5 lines. */
	float ComputeMinimumHeight();
	/** One bit per visible Notes line; a change may grow the window (§5.5.3 "Notes during the run"). */
	uint32 GetNotesSignature() const;
	/**
	 * §5.5.3 "Notes during the run": when the live view would show fewer than 5 lines, grows Window by the shortfall
	 * toward the code area (up in the bottom layout, else down), inside the work area, with ReshapeWindow. Never shrinks.
	 */
	static void GrowForNotes(const TSharedPtr<SWindow>& Window, SDreamRunWindow& Content, const FDreamRunPolicy& Policy);
	/** Teardown step 4 retries (next pump or next idle tick): re-parents kept child windows whose old parent is gone. */
	static void ReparentKeptChildWindows();
	/** Test hook (Dream.Supervisor.RunWindowFocus): the window the last OpenModal chose to hand activation back to. */
	static TSharedPtr<SWindow> GetLastReturnWindowForTest();
	/** Module shutdown: drops kept child windows that could not be re-parented. */
	static void ReleaseKeptChildWindows();

private:
	bool bMeasuring = false;
	float LiveLineHeight = 19.f;
	TSharedPtr<SDreamOutputView> LiveRunView;
	TSharedPtr<SWidget> StopButton;
	TSharedPtr<SWidget> InterruptButton;
	const FDreamRunLiveStats* Stats = nullptr;
	int32 RunNumber = 0;
	FString TabOrItem;
	FSimpleDelegate OnStop;
	FSimpleDelegate OnInterruptCleanup;
};
