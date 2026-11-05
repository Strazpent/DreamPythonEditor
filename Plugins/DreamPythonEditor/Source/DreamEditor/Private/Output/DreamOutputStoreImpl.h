// SPDX-License-Identifier: Apache-2.0
// FDreamOutputStore's state, shared by its two halves (output slice, internal): the producers and the game-thread drain
// (DreamOutputStore.cpp) and the DreamOutput ingest thread (DreamOutputIngest.cpp). TDD §6.1-§6.3, D19, D22, D24.
//
//   producers (any thread) --MPSC InQueue--> DreamOutput thread --SPSC ViewQueue--> game thread Drain -> dream::OutputRing
//                                              |-> FDreamLogForwarder (UE_LOG)   |-> FDreamRunLogWriter (FPipe)
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Output/DreamLogForwarder.h"
#include "Output/DreamOutputStore.h"
#include "Output/DreamRunLog.h"
#include "Output/DreamThreadTable.h"
#include "dream/OutputLines.h"
#include "dream/OutputRing.h"

#include <atomic>
#include <string>
#include <vector>

class FDreamOutputDevice;

/** One processed item handed from the ingest thread to the game thread, in queue order. */
struct FDreamViewItem
{
	enum class EKind : uint8
	{
		Line,               // Info, Text, Links (+ Str: the rest of a truncated result)
		Clear,
		RunLogPath,         // Number = run, Str = path
		RunLogError,        // Number = run, Str = error
		ThreadName,         // Number = tag, Str = name
		Provisional,        // Number = run, Str = the open partial line ("" = none)
		Dropped,            // Number = run, Count = lines the view did not get
		Bypass,             // Number = run, Count = LogPython lines captured by the device so far
		StartupProblems,    // Startup = the Warning and Error lines of the startup ring
	};

	EKind Kind = EKind::Line;
	dream::LineInfo Info;
	std::string Text;
	std::vector<dream::LinkSpan> Links;
	FString Str;
	uint32 Number = 0;
	int32 Count = 0;
	TArray<FDreamStartupLine> Startup;
};

struct FDreamViewBatch
{
	TArray<FDreamViewItem> Items;
	int32 Lines = 0;
	int64 Bytes = 0;
};

/** The DreamOutput thread's private state and work (§6.2 "Design: threads"). Defined in DreamOutputIngest.cpp. */
class FDreamIngest
{
public:
	static constexpr double CollectSeconds = 0.005;        // §6.2: collect for up to 5 ms after a wake
	static constexpr int32 ResultCapChars = 10000;         // D8 result line cap
	static constexpr int64 BlobCapBytes = 16ll << 20;      // §6.2 memory bounds: one full-result blob
	static constexpr int64 ViewCapLines = 200000;          // view queue bound (§6.2 memory bounds)
	static constexpr int64 ViewCapBytes = 64ll << 20;

	explicit FDreamIngest(FDreamOutputStore::FImpl& InOwner);
	~FDreamIngest();

	void Process(FDreamRawRecord& Record);
	/** End of a collection window: forward, write, hand the batch to the game thread. */
	void FlushBatch(double Now);
	/** Every wake: the forwarder's window, the run-log 30 s rule and pending writes, settings from the game thread. */
	void Tick(double Now);
	/** Shutdown / crash: commit every open partial line, echo everything ignoring the cap, close run logs. */
	void FlushEverything(bool bEmergency);

private:
	struct FRunState
	{
		bool bOpen = false;
		bool bEnded = false;
		double StartSeconds = 0.0;
		bool bReplPrompts = false;
		int32 BypassLines = 0;
		int32 ViewDropped = 0;          // lines the view did not get while its queue was over the cap
	};

	struct FSplitKey
	{
		uint32 NativeThreadId = 0;
		uint32 RunNumber = 0;
		uint8 Stream = 0;
		bool operator==(const FSplitKey& Other) const
		{
			return NativeThreadId == Other.NativeThreadId && RunNumber == Other.RunNumber && Stream == Other.Stream;
		}
		friend uint32 GetTypeHash(const FSplitKey& Key)
		{
			return HashCombineFast(HashCombineFast(::GetTypeHash(Key.NativeThreadId), ::GetTypeHash(Key.RunNumber)), ::GetTypeHash(Key.Stream));
		}
	};

	void ProcessText(FDreamRawRecord& Record);
	void ProcessDevice(FDreamRawRecord& Record, uint32 RunNumber, uint16 Flags);
	void ProcessEcho(FDreamRawRecord& Record);
	void ProcessHeader(FDreamRawRecord& Record);
	void ProcessFooter(FDreamRawRecord& Record);
	void FlushRunPartials(uint32 RunNumber, bool bBeforeEcho, bool bGameThreadOnly);

	/** §6.1 attribution: the run number and the Background flag of a record. */
	void Attribute(const FDreamRawRecord& Record, uint32& OutRunNumber, uint16& OutFlags);
	/** §6.1: binds a worker write's (native ID, PyThreadState*) pair in the thread table before Attribute; a new thread
	 *  under a reused native ID gets a new badge tag, so the old thread's lines keep their name. */
	void ObserveThread(const FDreamRawRecord& Record);
	uint32 TagForThread(uint32 NativeThreadId);
	FRunState* FindRun(uint32 RunNumber);

	/** Splits, caps, scans links, forwards, logs and queues one committed line. */
	void EmitLine(uint32 RunNumber, dream::LineType Type, uint16 Flags, uint32 NativeThreadId, double Time,
		std::string&& Text, EDreamOutputOrigin Origin, int32 EchoLine = 0);
	/** One result write: its first piece is the line (cut at 10,000 characters), its Continued pieces join the blob. */
	void EmitResultPieces(uint32 RunNumber, uint16 Flags, uint32 NativeThreadId, double Time,
		std::vector<dream::SplitLine>& Pieces, EDreamOutputOrigin Origin);
	void EmitSplit(uint32 RunNumber, dream::LineType Type, uint16 Flags, uint32 NativeThreadId, double Time,
		std::vector<dream::SplitLine>& Pieces, EDreamOutputOrigin Origin);
	void QueueLine(FDreamViewItem&& Item, EDreamOutputOrigin Origin, double Time);
	void ForceOpenRunLogs();
	FDateTime ToUtc(double Seconds) const;
	void QueueItem(FDreamViewItem&& Item);
	/** §6.1: one partial line per (thread, run, stream); the run's provisional row shows the newest open one (OUT-12). */
	void UpdateProvisional(uint32 RunNumber, const FSplitKey& Key, const dream::LineSplitter& Splitter);

	FDreamOutputStore::FImpl& Owner;
	TUniquePtr<FDreamViewBatch> Batch;
	TArray<FDreamForwardLine> ForwardLines;

	TMap<FDreamRunId, uint32> Bindings;
	TMap<uint32, FRunState> Runs;
	uint32 NewestRun = 0;
	TMap<FSplitKey, dream::LineSplitter> Splitters;
	TMap<uint32, FString> ProvisionalSent;
	struct FPartialText
	{
		FString Text;
		uint64 Order = 0;               // when it last changed: the newest open partial is the one shown
	};
	TMap<FSplitKey, FPartialText> Partials;
	uint64 PartialOrder = 0;

	// Capture of the current entry (§6.1): capture_begin .. capture_end.
	FDreamRunId CaptureRunId = 0;
	uint32 CaptureRunNumber = 0;
	TSet<uint64> AliveAtCapture;

	// Thread tags for the badges: 0 = game thread. One tag per (native ID, PyThreadState*) pair (§6.1).
	struct FTag
	{
		uint32 Tag = 0;
		uint32 NameVersion = MAX_uint32;    // the table's name version last sent for Tag (MAX: not sent yet)
		uint32 Generation = 0;              // FDreamThreadTable::ObserveWrite's generation for Tag (0: not known yet)
		uint64 LabelStamp = 0;              // FDreamThreadTable::GetLabelStamp at this thread's last write
	};
	FTag& FindOrAddTag(uint32 NativeThreadId);
	TMap<uint32, FTag> Tags;
	TMap<uint32, FString> TagNames;      // tag -> name, for run-log lines
	uint32 NextTag = 1;

	bool bStartupDone = false;
	int32 StartupLines = 0;
	TArray<FDreamStartupLine> StartupProblems;

	bool bViewOverCap = false;
	int64 LastDroppedRecords = 0;
	int32 AppliedEchoCap = -1;

	// TEL-03 counters (§6.2 "Budgets"): lines committed in the current 1 s rate window.
	int64 RateWindowLines = 0;
	double RateWindowStart = 0.0;
	void PublishCounters(double Now);
};

struct FDreamOutputStore::FImpl : public FRunnable
{
	explicit FImpl(FDreamOutputStore& InOwner);
	virtual ~FImpl() override;

	// FRunnable (the DreamOutput thread)
	virtual uint32 Run() override;
	virtual void Stop() override;

	void Wake();

	FDreamOutputStore& Owner;

	// ---- Producers (any thread) ----
	TQueue<FDreamRawRecord, EQueueMode::Mpsc> InQueue;
	std::atomic<int64> QueuedBytes{0};
	std::atomic<int64> DroppedRecords{0};        // over the 64 MB hard cap (§6.2 memory bounds)
	std::atomic<bool> bIngestWaiting{false};
	FEvent* WakeEvent = nullptr;
	static constexpr int64 QueueCapBytes = 64ll << 20;

	// ---- The DreamOutput thread ----
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopRequested{false};
	std::atomic<bool> bEmergencyRequested{false};
	FEvent* EmergencyDoneEvent = nullptr;
	TUniquePtr<FDreamIngest> Ingest;
	FDreamLogForwarder Forwarder;
	FDreamThreadTable ThreadTable;
	FDreamRunLogWriter RunLogs;
	double SessionStartSeconds = 0.0;
	FDateTime SessionStartUtc;

	// Game thread -> ingest settings.
	FCriticalSection SettingsLock;
	FString PendingRunLogRoot;
	bool bRunLogRootChanged = false;
	std::atomic<int32> EchoCap{FDreamLogForwarder::DefaultEchoCapLinesPerSecond};
	int32 EchoCapOverride = -1;                    // game thread: SetEchoCap wins over the project setting
	std::atomic<bool> bCaptureOutsideRuns{true};

	// The device whose startup ring the ingest fills (A31).
	FCriticalSection DeviceLock;
	FDreamOutputDevice* Device = nullptr;

	// ---- Ingest -> game thread ----
	TQueue<TUniquePtr<FDreamViewBatch>, EQueueMode::Spsc> ViewQueue;
	std::atomic<int64> ViewPendingLines{0};
	std::atomic<int64> ViewPendingBytes{0};

	// ---- TEL-03 counters (FDreamOutputCounters; written by their owning thread, read on any thread) ----
	std::atomic<int32> CounterLinesPerSecond{0};       // ingest
	std::atomic<int64> CounterLinesTotal{0};           // ingest
	std::atomic<int64> CounterViewDropped{0};          // ingest: lines the view did not get while its queue was full
	std::atomic<int64> CounterEchoSuppressed{0};       // ingest, from the forwarder
	std::atomic<int32> CounterEchoCap{FDreamLogForwarder::DefaultEchoCapLinesPerSecond};   // ingest, from the forwarder
	std::atomic<int64> CounterDrainMicros{0};          // game thread

	// ---- Game thread ----
	dream::OutputRing Ring;
	TMap<uint32, FDreamRunSpan> Spans;
	TArray<uint32> OpenRuns;                       // headers without a footer: Clear keeps them (§6.4)
	TMap<uint64, FString> Blobs;                   // full-result blobs by line sequence (§6.4 Show all)
	TArray<uint64> BlobOrder;
	int64 BlobBytes = 0;
	static constexpr int64 BlobTotalCapBytes = 64ll << 20;
	TMap<uint32, FString> Provisional;
	TArray<FString> ThreadNames;                   // by tag
	uint64 EvictedLines = 0;
	TArray<uint32> EvictedRunNumbers;
	bool bEvictedThisDrain = false;
	FDreamOutputVisibility Visibility;
	TArray<FDreamStartupLine> StartupProblems;
	TUniquePtr<FDreamViewBatch> DrainBatch;         // a batch the last drain did not finish
	int32 DrainIndex = 0;
	FTSTicker::FDelegateHandle DrainTicker;
	FDelegateHandle SystemErrorHandle;
	TArray<FName> PublishedExtraCategories;
	class FDreamPython* Python = nullptr;           // cached at Start: the startup phase ends at its first generation
	bool bStartupDoneSent = false;
	bool bStarted = false;

	void ApplyItem(FDreamViewItem& Item, FDreamAppendInfo& Info, bool& bAppended);
	void AppendLine(FDreamViewItem& Item, FDreamAppendInfo& Info, bool& bAppended);
	void ApplyClear(FDreamAppendInfo& Info);
	void OnEvicted(const dream::EvictionInfo& Eviction);
	void DropBlobsBefore(uint64 Seq);
	void PullSettings();
};
