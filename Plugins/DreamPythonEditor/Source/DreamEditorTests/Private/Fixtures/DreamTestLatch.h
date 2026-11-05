// SPDX-License-Identifier: Apache-2.0
// DreamTestLatch (TDD §14.2): named latches and time marks shared by a run's Python (the _dream_test module) and the
// driver (a spec on the game thread or the input bot on its own thread). Every function is safe on any thread; nothing
// here calls Slate, UObjects or Python.
//  - A latch is held until a driver releases it: _dream_test.wait(name) loops on dream_editor.sleep(0.01) while it is held
//    (a Stop during the wait raises ScriptCancelled as in any sleep), _dream_test.held(name) polls it without blocking.
//    A spec arms a fresh name per case (UniqueSuffix), so a latch released by one case never leaks into the next.
//  - A mark is the FPlatformTime::Seconds() at which Python (or C++) passed a named point, read back by the driver:
//    the probe "the try body has started" of T-CAN-004, and the end timestamp of T-CAN-001.
#pragma once

#include "CoreMinimal.h"

namespace DreamTest
{
	class FDreamTestLatch
	{
	public:
		/** Ends the hold: wait(Name) returns at its next slice and held(Name) turns false. */
		static void Release(const FString& Name);
		/** Arms Name again (held, no waiter seen). */
		static void Reset(const FString& Name);
		static bool IsHeld(const FString& Name);
		/** wait(Name) was entered at least once since the last Reset (the run reached the latch). */
		static bool WasReached(const FString& Name);
		/** A wait(Name) is in progress now. */
		static bool IsWaiting(const FString& Name);

		/** wait() bookkeeping (the _dream_test module). */
		static void BeginWait(const FString& Name);
		static void EndWait(const FString& Name);

		/** Records FPlatformTime::Seconds() under Name (the latest call wins) and returns it. */
		static double Mark(const FString& Name);
		/** The time of the latest Mark(Name), 0 when none. */
		static double GetMark(const FString& Name);
		static int32 GetMarkCount(const FString& Name);
		static void ClearMark(const FString& Name);

		/** Spec teardown: forgets every latch and mark whose name starts with Prefix. */
		static void ForgetPrefix(const FString& Prefix);
	};
}
