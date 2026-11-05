// SPDX-License-Identifier: Apache-2.0
// The input bot (TDD §14.2 "Input fixtures", §4.16.1, §5.15): an FRunnable that injects OS-level input with SendInput, so
// it also acts while the game thread sits inside a supervised run's pump or a modal loop, where AutomationDriver (core
// ticker steps) cannot. It clicks at widget geometry the game thread publishes (FDreamTestProbes) and synchronises on
// run-state probes and latches (§14.2 synchronisation rule), never on sleeps. It never calls Slate from its own thread.
//
// SendInput needs an unlocked interactive desktop with the editor in front (§14.1 agents, 13 QA-F03): every job checks
// the input desktop and the foreground window first and fails with a clear message otherwise. Win64 only; elsewhere
// IsSupported() is false and every job fails.
//
// Usage from a spec step (game thread): Submit a job, then poll IsDone() from FDreamSteps; the job's own steps (wait for a
// probe, click, keys) run on the bot thread, in order. The bot restores the cursor position after each job.
#pragma once

#include "CoreMinimal.h"
#include "Fixtures/DreamTestProbes.h"
#include "Templates/Function.h"

#include <atomic>

namespace DreamTest
{
	/** One key of a chord or sequence, by Windows virtual-key code (VK_*), with its extended-key flag. */
	struct FDreamBotKey
	{
		uint16 VirtualKey = 0;
		bool bExtended = false;
	};

	/** The bot thread's side of a job. Every call blocks the bot thread only. */
	class FDreamInputBotContext
	{
	public:
		/** Polls the probe snapshot every millisecond until Condition holds; false (and the job fails) after Timeout. */
		bool WaitFor(TFunctionRef<bool(const FDreamProbeSnapshot&)> Condition, double TimeoutSeconds, const FString& What);
		/** Polls Condition (any-thread state such as a latch) every millisecond. */
		bool WaitUntil(TFunctionRef<bool()> Condition, double TimeoutSeconds, const FString& What);
		/** The input desktop is unlocked and the foreground window belongs to this process (the editor or its popups). */
		bool RequireForeground();
		/** Moves the cursor to the centre of Rect (desktop pixels) and checks the window under it is ExpectedHwnd's tree
		 *  (null: any window of this process). When another application's window covers the centre (an always-on-top
		 *  overlay), the first uncovered point of a 7 x 7 grid inside Rect is used instead (FindUncoveredPoint). */
		bool MoveTo(const FSlateRect& Rect, void* ExpectedHwnd, const FString& What);
		/** A point inside Rect whose window under it is ExpectedHwnd's tree (null: this process): the centre when it is
		 *  uncovered, else the first of a 7 x 7 grid; false when every one is covered. Moves nothing. */
		static bool FindUncoveredPoint(const FSlateRect& Rect, void* ExpectedHwnd, FVector2D& OutPoint);
		/** FindUncoveredPoint, and when every point is covered, ExpectedHwnd raised to the top without activating it
		 *  (SetWindowPos HWND_TOP, SWP_NOACTIVATE, asynchronously) and looked at again for up to 1 s, as MoveToPoint does
		 *  (a Note says so). For a script that moves the cursor itself later: call it while the game thread still pumps. */
		bool FindUncoveredPointRaising(const FSlateRect& Rect, void* ExpectedHwnd, FVector2D& OutPoint, const FString& What);
		bool MoveToPoint(const FVector2D& Point, void* ExpectedHwnd, const FString& What);
		/** Left button down / up at the current cursor position; returns the FPlatformTime::Seconds() taken right before
		 *  SendInput (0 when the input was blocked). */
		double LeftDown();
		double LeftUp();
		/** Down and up in one SendInput call. */
		double LeftClick();
		/** A wheel notch count (positive = away from the user) at the current position. */
		double Wheel(int32 Notches);
		/** Presses every key of Chord in order, then releases them in reverse, in one SendInput call. */
		double Chord(const TArray<FDreamBotKey>& Keys);
		/** One key down or up (KEYEVENTF_KEYUP). */
		double Key(const FDreamBotKey& Key, bool bUp);
		/** Types Text with KEYEVENTF_UNICODE (layout independent). */
		double TypeUnicode(const FString& Text);
		/** Fails the job with Message (the spec reports it). */
		void Fail(const FString& Message);
		/** A note the spec adds as info (timings, what the bot saw). */
		void Note(const FString& Message);
		bool HasFailed() const { return bFailed; }
		/** The job's scope guard: a left button still down when the job ends (a move after LeftDown failed) is released
		 *  before the cursor is put back, so the OS never keeps it held. Called by the bot after every job. */
		void ReleaseHeldButtons();
		/** Before every job: modifier keys the OS still holds down (pressed and never released) are released first. */
		void ReleaseStuckModifiers();
		const TArray<FString>& GetErrors() const { return Errors; }
		const TArray<FString>& GetNotes() const { return Notes; }

	private:
		bool bFailed = false;
		bool bLeftDown = false;
		TArray<FString> Errors;
		TArray<FString> Notes;
	};

	using FDreamInputBotJob = TFunction<void(FDreamInputBotContext&)>;

	/** Shared state of one submitted job: the spec polls it from the game thread. */
	struct FDreamInputBotTicket
	{
		std::atomic<bool> bDone{ false };
		/** Valid once bDone. */
		TArray<FString> Errors;
		TArray<FString> Notes;
		/** Free-form timestamps the job stored for the spec (bot thread writes before bDone). */
		TMap<FName, double> Times;
	};

	class FDreamInputBot
	{
	public:
		static bool IsSupported();
		/** Starts the bot thread on first use. Game thread. */
		static TSharedRef<FDreamInputBotTicket> Submit(FDreamInputBotJob&& Job);
		/** Joins the thread (module shutdown). */
		static void Shutdown();

		/** Virtual keys for specs (Win64 VK codes; 0 elsewhere). */
		static FDreamBotKey VkEscape();
		static FDreamBotKey VkShift();
		static FDreamBotKey VkControl();
		static FDreamBotKey VkAlt();
		static FDreamBotKey VkRightAlt();
		static FDreamBotKey VkF5();
		static FDreamBotKey VkReturn();
		static FDreamBotKey VkNumpadEnter();
		static FDreamBotKey VkLetter(TCHAR Letter);
		/** "esc", "shift+f5", "ctrl+s", "alt+enter", "numpadenter", single letters and digits: false for an unknown name. */
		static bool ParseChord(const FString& Text, TArray<FDreamBotKey>& OutKeys);

		/** Game thread: tries to bring the editor's main window to the front, or the window WindowHwnd itself
		 *  (AttachThreadInput + SetForegroundWindow); true when the foreground window is now this process's (with
		 *  WindowHwnd: is that window). */
		static bool BringEditorToFront(void* WindowHwnd = nullptr);
		/** Any thread: the foreground window belongs to this process; OutWhat names it otherwise. */
		static bool IsEditorForeground(FString& OutWhat);
		/** Any thread: the input desktop can be opened (the session is not locked). */
		static bool IsDesktopInteractive();
	};
}
