// SPDX-License-Identifier: Apache-2.0
// Per-run log files (TDD §6.2 "Run logs (spill)", OUT-08, D17): <root>/RunLogs/<session start>/run-<N>.log, written on
// the UE::Tasks::FPipe "DreamRunLog" so neither the ingest thread nor the game thread waits on the disk. Output slice,
// internal. Threads: every member except the pipe tasks runs on the DreamOutput ingest thread; Shutdown and
// WaitForWrites may run on the game thread once the ingest thread has stopped (or for the 100 ms crash flush).
#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Output/DreamOutputTypes.h"
#include "Tasks/Pipe.h"

#include <atomic>
#include <string>
#include <vector>

/** One line as the run log writes it (§6.2 format). */
struct FDreamRunLogLine
{
	dream::LineType Type = dream::LineType::Stdout;
	uint16 Flags = 0;
	int32 EchoLine = 0;
	FDateTime Utc;
	FString ThreadName;                 // "GameThread" for the game thread
	std::string_view Text;              // UTF-8
};

class FDreamRunLogWriter
{
public:
	static constexpr int32 SpillLines = 10000;              // §6.2, setting off: create the file at this many output lines
	static constexpr int64 SpillBytes = 4ll << 20;          // per-run spill buffer bound
	static constexpr double SpillSeconds = 30.0;            // ... or after the run has lasted this long
	static constexpr int64 MaxPendingBytes = 32ll << 20;    // back-pressure: then gap lines
	// Deferred (M4): the studio policy key retention.run_logs_days (7-3650, merged with min, §11.2.2; no user layer, A48).
	static constexpr int32 RetentionDays = 30;              // D17 default
	static constexpr int64 RetentionMaxBytes = 1ll << 30;   // 1 GB per project

	FDreamRunLogWriter();
	~FDreamRunLogWriter();

	/** <root>/RunLogs/<session start>; empty disables run logs. The first non-empty root also queues the retention sweep. */
	void SetRoot(const FString& InRoot);

	void BeginRun(uint32 RunNumber, const FDreamRunBlockInfo& Header, double StartSeconds);
	void AddLine(uint32 RunNumber, const FDreamRunLogLine& Line, double NowSeconds);
	/** The run's footer: the end line, or the dropped spill buffer of a run that never needed a file. */
	void EndRun(uint32 RunNumber, const FDreamRunBlockInfo& Footer);
	/** Forces a file now (echo cap suppressing, view queue over its cap, §6.2 / §6.3). */
	void Force(uint32 RunNumber);

	/** Once per ingest wake: the 30 s rule, then one pipe task per run with pending bytes. */
	void Pump(double NowSeconds);

	/** The path of a run's file, or empty when it has none (yet). */
	FString GetPath(uint32 RunNumber) const;
	/** Paths created and write errors since the last call, for the game thread's FDreamRunSpan. */
	void TakeEvents(TArray<TPair<uint32, FString>>& OutCreated, TArray<TPair<uint32, FString>>& OutErrors);

	/** Waits for the queued writes (crash flush: 100 ms; shutdown: a few seconds). */
	bool WaitForWrites(double TimeoutSeconds);
	/** Closes every handle after the queued writes. */
	void Shutdown(double TimeoutSeconds);

private:
	struct FRun
	{
		FString Path;
		bool bFile = false;
		bool bEnded = false;
		bool bAlways = true;
		bool bReplPrompts = false;
		bool bHasOutput = false;
		bool bForced = false;
		bool bFailed = false;
		double StartSeconds = 0.0;
		std::string HeaderText;
		std::vector<std::string> Held;  // echo lines held before the first output line, or the whole spill buffer
		int64 HeldBytes = 0;
		int32 HeldOutputLines = 0;      // the held lines that are not echo: the SpillLines count (T-OUT-023)
		std::string Pending;            // bytes for the next pipe task
		int64 Lines = 0;
		int64 SkippedLines = 0;         // back-pressure: becomes one gap line
		bool bCloseAfterPending = false;
	};

	FRun* FindRun(uint32 RunNumber);
	void CreateFile(uint32 RunNumber, FRun& Run);
	void AppendFormatted(FRun& Run, std::string&& Line);
	static std::string FormatLine(const FRun& Run, const FDreamRunLogLine& Line);
	void LaunchWrite(uint32 RunNumber, FRun& Run);
	void PruneRuns();
	void QueueRetentionSweep(const FString& SessionDir);

	// Pipe-only state (touched only inside pipe tasks).
	struct FPipeState
	{
		TMap<FString, class IFileHandle*> Handles;
		TSet<FString> FailedPaths;
		FString MadeDirectory;
	};

	FString Root;
	bool bSweepQueued = false;
	TMap<uint32, FRun> Runs;
	uint32 NewestRun = 0;

	UE::Tasks::FPipe Pipe;
	TSharedRef<FPipeState, ESPMode::ThreadSafe> PipeState;
	std::atomic<int64> PendingBytes{0};

	FCriticalSection EventLock;          // pipe tasks -> ingest thread
	TArray<TPair<uint32, FString>> CreatedEvents;
	TArray<TPair<uint32, FString>> ErrorEvents;
	TSet<uint32> FailedRuns;            // under EventLock
};
