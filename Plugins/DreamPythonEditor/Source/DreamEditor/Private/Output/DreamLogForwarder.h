// SPDX-License-Identifier: Apache-2.0
// Echo of Dream output to the Unreal log (TDD §6.3, D19, RT-07, E3): one UE_LOG per line in bursts, never GLog->Flush(),
// never through unreal.log; echo lines (OUT-12) and device-captured lines are never forwarded; the echo cap.
// Threads: Forward and SetEchoCap on the DreamOutput thread; EchoNow on any thread. Body: DreamLogForwarder.cpp (output slice).
// (slice A choice) PythonScriptPlugin's LogPython category is private (Private/PyUtil.h:16), so the .cpp defines its own
// static category object named LogPython; the log shows "LogPython:" exactly as stock output does.
#pragma once

#include "CoreMinimal.h"
#include "Output/DreamOutputTypes.h"

/** Set on the forwarding thread around its UE_LOG calls, so FDreamOutputDevice ignores our own echo (T-OUT-021). */
// Not exported: MSVC refuses a DLL interface on thread_local data (C2492); only this module reads it.
extern thread_local bool GDreamForwarding;

class DREAMEDITOR_API FDreamLogForwarder
{
public:
	static constexpr int32 DefaultEchoCapLinesPerSecond = 5000;   // D19; 0 = unlimited

	/** DreamOutput thread: one UE_LOG per line with GDreamForwarding set; whitespace-only lines skipped (ShouldLog);
	 *  Result lines as "# Result: <text>"; an OLF_EchoSplit line is held and forwarded joined with the next line of its
	 *  type and run (FlushRun forwards a held one alone at run end, §6.3 rule 1); above the cap in a 1 s window lines
	 *  are counted, and the window's end echoes one summary naming the run log. */
	void Forward(TConstArrayView<FDreamForwardLine> Lines);
	/** DreamOutput thread, at a run's footer: forwards the run's held EchoSplit lines alone. */
	void FlushRun(uint32 RunNumber);

	/** Any thread: one UE_LOG with GDreamForwarding set (the console executor's input echo, §6.6). */
	static void EchoNow(const FDreamForwardLine& Line);

	void SetEchoCap(int32 LinesPerSecond);
	/** True when the current window has suppressed lines (forces a run log, §6.2). */
	bool IsSuppressing() const;
	/** DreamOutput thread: lines the cap kept out of the log this session, and the cap in force (the TEL-03 counters). */
	int64 GetSuppressedTotal() const { return SuppressedTotal; }
	int32 GetEchoCap() const { return EchoCap; }
	/** Echoes everything given, ignoring the cap (crash flush, §6.3). */
	void EmergencyFlush(TConstArrayView<FDreamForwardLine> Lines);

	/** The forwarding rules of §6.3: category and verbosity for a stored line, or false when it is never forwarded. */
	static bool MapLine(dream::LineType Type, uint16 Flags, FName& OutCategory, ELogVerbosity::Type& OutVerbosity);

	/** DreamOutput thread: closes a window whose second has passed and echoes its summary (the ingest thread calls it on
	 *  every wake, so a summary does not wait for the next line). */
	void Tick(double Now);
	/** DreamOutput thread: the run-log path named by the cap summary (the ingest thread's run-log writer). */
	void SetRunLogPathProvider(TFunction<FString(uint32 /*RunNumber*/)> Provider);

	/** The two category names of §6.3 (built on first use, never during static initialization). */
	static FName LogPythonName();
	static FName LogDreamName();

private:
	void Emit(const FDreamForwardLine& Line, const FString& Text) const;
	void CloseWindow();

	int32 EchoCap = DefaultEchoCapLinesPerSecond;
	double WindowStart = 0.0;
	int32 WindowLines = 0;
	int32 WindowSuppressed = 0;
	int64 SuppressedTotal = 0;
	uint32 WindowRunNumber = 0;
	/** Held OLF_EchoSplit text per (type, run): §6.3 rule 1. */
	TMap<uint64, FString> HeldSplits;
	TFunction<FString(uint32)> RunLogPathProvider;
};
