// SPDX-License-Identifier: Apache-2.0
// Spec-side helpers around the input bot (Fixtures/DreamInputBot.h) for the G2, G5 and supervisor suites: the Stop jobs of
// T-CAN-001 (an OS click on the Run window's Stop button, or Shift+F5, through SendInput, after the first pump), the
// safety net that stops a run the bot could not stop (so a failing case never leaves the editor in `while True: pass`),
// command-line counts, and percentile reports. Game thread unless a comment says otherwise.
#pragma once

#include "CoreMinimal.h"
#include "Fixtures/DreamInputBot.h"
#include "Misc/AutomationTest.h"
#include "Run/DreamRunTypes.h"

namespace DreamTest
{
	enum class EBotStop : uint8
	{
		Click,      // SendInput left click at the centre of the Run window's Stop button
		ShiftF5,    // SendInput Shift+F5 (the run-level Stop chord, §4.9.4, §5.6.2)
		Esc,        // SendInput Esc (RT-05: our modal active, no menu, no text focus)
		Api,        // no OS input: FDreamRunService::RequestStopActive from the bot thread (any-thread Stop, §12.3); needs no
		            // foreground window (the lifecycle cycles, where the input path is not what is tested)
	};

	const TCHAR* BotStopName(EBotStop Method);

	struct FBotStopOptions
	{
		int32 RunNumber = 0;
		EBotStop Method = EBotStop::Click;
		/** Wait for this mark (FDreamTestLatch) before stopping, e.g. the probe in a try body (T-CAN-004). Empty: none. */
		FString AwaitMark;
		/** How long the run may take to show its Run window. */
		double ShowTimeoutSeconds = 15.0;
		/** After a bot failure: request Stop through the any-thread API (then the drain below). */
		bool bSafetyNet = true;
		/**
		 * A run still alive this long after the Stop (or the safety net's Stop) is drained: the job keeps the RT-06 Interrupt
		 * cleanup flag set until the run ends (at most 300 s), so the supervisor's fast-chain call right after a swallowed
		 * delivery delivers again at the loop's back-edge. A script that swallows every delivery (§5.7.4) would otherwise
		 * keep the editor in its loop for minutes and fail every later case. Times["Drained"] records when it started. 0: off.
		 */
		double DrainAfterSeconds = 10.0;
	};

	/**
	 * Submits a bot job that waits until run RunNumber's Run window is up with its Stop button painted (by the second pump:
	 * "after the first pump", T-CAN-001), checks the desktop and the foreground window, and stops the run with Method.
	 * The ticket's Times["Injected"] is FPlatformTime::Seconds() right before SendInput. On any failure the job stops the
	 * run through FDreamRunService::RequestStopActive (Api); a run that outlives DrainAfterSeconds is drained with Interrupt
	 * cleanup (see FBotStopOptions), so the editor never stays in a loop. The job ends once the run has ended.
	 */
	TSharedRef<FDreamInputBotTicket> SubmitBotStop(const FBotStopOptions& Options);

	/** Polls Ticket: Wait until done, then its notes become infos and its errors test errors (false when it had any). */
	bool IsTicketDone(const TSharedRef<FDreamInputBotTicket>& Ticket);
	bool ReportTicket(FAutomationTestBase& Test, const TSharedRef<FDreamInputBotTicket>& Ticket, const FString& What);

	/** -Name=<int> on the command line, else Default (for example -DreamG2Stops=20 for a quick pass). */
	int32 CommandLineInt(const TCHAR* Name, int32 Default);
	/** -Name=<value> on the command line, else empty. */
	FString CommandLineString(const TCHAR* Name);

	/** The p-th percentile (0..100, nearest rank) of Values; 0 for none. */
	double Percentile(TArray<double> Values, double P);
	/** "n 200, p50 41.2 ms, p95 63.0 ms, p99 70.4 ms, max 80.1 ms" in milliseconds. */
	FString DescribeMs(const TArray<double>& Seconds);

	/** Brings the editor to the front for SendInput (FDreamInputBot::BringEditorToFront) and says so on Test when it cannot. */
	bool RequireEditorInFront(FAutomationTestBase& Test);
}
