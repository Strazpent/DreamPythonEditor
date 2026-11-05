// SPDX-License-Identifier: Apache-2.0
// The journal writer thread (TDD §10.2.1-§10.2.5). Commits run first and each is flushed before its tickets turn durable;
// a failed commit marks its tickets failed (a waiting run ends not executed) and the retry replays the whole queue as one
// batch every second (P2-per-02). Between the commit drains it does one unit of background work at a time: a rotation's
// checkpoint slice (4 MiB, or the flush after it, §10.2.3 steps 1-2) or one local-history slice (§10.4).
#include "Persistence/DreamJournalWriter.h"

#include "Buffers/DreamHash.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Diagnostics/DreamAudit.h"
#include "Persistence/DreamLegacyMigration.h"   // (rename)
#include "Persistence/DreamWin32FileOps.h"
#include "dream/Checksum.h"
#include "dream/Diff.h"
#include "dream/StorageIndex.h"
#include "dream/TextCodec.h"
#include "dream/Utf.h"

#include <algorithm>

namespace DreamJournalWriterLocal
{
	constexpr double RetrySeconds = 1.0;
	/** A rotation that failed (a sharing violation on the .tmp, §10.11) is tried again this much later; appends go on. */
	constexpr double RotationRetrySeconds = 10.0;

	/** Every writer of this process gets its own id, so history work can tell one journal session from the next. */
	std::atomic<uint64> GNextSessionId { 1 };

	int64 NowUtcMs()
	{
		return static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds());
	}

	FDateTime FromUtcMs(int64 Ms)
	{
		return FDateTime(1970, 1, 1) + FTimespan::FromMilliseconds(static_cast<double>(Ms));
	}

	dream::codec::Encoding EncodingFromName(const std::string& Name)
	{
		using dream::codec::Encoding;
		if (Name == "utf-16-le") return Encoding::Utf16LE;
		if (Name == "utf-16-be") return Encoding::Utf16BE;
		if (Name == "latin-1") return Encoding::Latin1;
		if (Name == "ascii") return Encoding::Ascii;
		if (Name == "cp1252") return Encoding::Cp1252;
		return Encoding::Utf8;
	}

	dream::codec::Eol EolFromName(const std::string& Name)
	{
		if (Name == "crlf") return dream::codec::Eol::CrLf;
		if (Name == "cr") return dream::codec::Eol::Cr;
		return dream::codec::Eol::Lf;
	}

	EDreamBufferKind KindFromName(const std::string& Name)
	{
		if (Name == "file") return EDreamBufferKind::File;
		if (Name == "recovered") return EDreamBufferKind::Recovered;
		return EDreamBufferKind::Scratch;
	}

	EDreamRunRoute RouteFromKey(const std::string& Key)
	{
		static const std::pair<const char*, EDreamRunRoute> Table[] = {
			{ "selection", EDreamRunRoute::Selection }, { "line", EDreamRunRoute::Line }, { "cell", EDreamRunRoute::Cell },
			{ "buffer", EDreamRunRoute::Buffer }, { "repl", EDreamRunRoute::Repl }, { "console", EDreamRunRoute::ConsoleExecutor },
			{ "run_file", EDreamRunRoute::RunFile }, { "api", EDreamRunRoute::Api },
		};
		for (const auto& Entry : Table)
		{
			if (Key == Entry.first)
			{
				return Entry.second;
			}
		}
		return EDreamRunRoute::Buffer;
	}

	TOptional<FDreamBaseVersion> ToBase(const std::optional<dream::journal::BaseVersion>& Base)
	{
		if (!Base.has_value())
		{
			return TOptional<FDreamBaseVersion>();
		}
		FDreamBaseVersion Out;
		Out.Size = Base->Size;
		Out.MtimeUtc = FromUtcMs(Base->MtimeUtcMs);
		Out.Sha256Hex = DreamText::Utf8ToFString(Base->Sha256Hex);
		return Out;
	}

	dream::TextSnapshot ModelSnapshot(const FString& Text)
	{
		return dream::TextBuffer(DreamText::ToU16(DreamText::ToModelText(Text))).Snapshot();
	}

	/** "+12 -3 lines" against the text shown while pending (§10.7.1): the line diff of dream::Diff (T-PER-011). */
	void CountLineChanges(const FString& Recovered, const FString& Shown, int32& OutAdded, int32& OutRemoved)
	{
		const dream::DiffResult Result = dream::Diff::Lines(ModelSnapshot(Shown), ModelSnapshot(Recovered));
		OutAdded = Result.AddedLines();
		OutRemoved = Result.RemovedLines();
	}

	/** The disk state of a recovered file buffer: conflict when missing or changed since the journal's base (§10.7.2). */
	void CompareWithDisk(dream::journal::FileOps& Ops, FDreamRecoveredBuffer& Buffer, bool& bOutDiffers)
	{
		bOutDiffers = false;
		std::string Bytes;
		if (Buffer.Path.IsEmpty() || !Ops.ReadAll(FDreamWin32FileOps::ToUtf8Path(Buffer.Path), Bytes))
		{
			Buffer.bConflict = true;
			bOutDiffers = true;
			CountLineChanges(Buffer.RecoveredText, FString(), Buffer.LinesAdded, Buffer.LinesRemoved);
			return;
		}
		const FString DiskSha = DreamHash::Sha256Hex(Bytes);
		Buffer.bConflict = Buffer.Base.IsSet() && !Buffer.Base->Sha256Hex.IsEmpty() && DiskSha != Buffer.Base->Sha256Hex;
		const dream::codec::DecodeResult Decoded = dream::codec::Decode(Bytes);
		const FString DiskText = DreamText::ToFString(Decoded.Text);
		bOutDiffers = Decoded.Refused != dream::codec::Refusal::None || DiskText != Buffer.RecoveredText;
		if (bOutDiffers)
		{
			CountLineChanges(Buffer.RecoveredText, DiskText, Buffer.LinesAdded, Buffer.LinesRemoved);
		}
	}
}

FDreamJournalWriter::FDreamJournalWriter(const FDreamStorageRoot& InRoot, const FString& InNamespace, int64 InSessionStartUtcMs,
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> InFence, FDreamHistorySink InHistorySink, const FDreamJournalWriterOptions& InOptions)
	: Root(InRoot)
	, Namespace(InNamespace)
	, Dir(InRoot.JournalDir(InNamespace))
	, Options(InOptions)
	, SessionStartUtcMs(InSessionStartUtcMs)
	, SessionId(DreamJournalWriterLocal::GNextSessionId.fetch_add(1))
	, Fence(MoveTemp(InFence))
	, HistorySink(MoveTemp(InHistorySink))
{
	Ops = MakeUnique<FDreamWin32FileOps>(Fence);
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
	if (Options.HistoryDir.IsEmpty())
	{
		Options.HistoryDir = InRoot.HistoryDir();
	}
}

FString FDreamJournalWriter::GetNamespace() const
{
	FScopeLock Lock(&SharedLock);
	return Namespace;
}

FDreamJournalWriter::~FDreamJournalWriter()
{
	if (WakeEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
}

void FDreamJournalWriter::Push(TUniquePtr<DreamJournal::FRequest> Request)
{
	if (Request->Commit.Ticket != 0)
	{
		FScopeLock Lock(&SharedLock);
		PendingTickets.Add(Request->Commit.Ticket);
		IssuedTicket = FMath::Max(IssuedTicket, Request->Commit.Ticket);
	}
	Queue.Enqueue(MoveTemp(Request));
	WakeEvent->Trigger();
}

void FDreamJournalWriter::PostIo(FDreamIoSlice Work)
{
	if (!Work)
	{
		return;
	}
	IoQueue.Enqueue(MoveTemp(Work));
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}

void FDreamJournalWriter::RequestStop(double DrainUntilSeconds)
{
	DrainUntil.store(DrainUntilSeconds, std::memory_order_release);
	bStopRequested.store(true, std::memory_order_release);
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}

bool FDreamJournalWriter::IsDurable(uint64 Ticket) const
{
	if (Ticket == 0)
	{
		return true;
	}
	FScopeLock Lock(&SharedLock);
	return Ticket <= IssuedTicket && !PendingTickets.Contains(Ticket) && !FailedTickets.Contains(Ticket);
}

bool FDreamJournalWriter::HasFailed(uint64 Ticket, FString& OutReason) const
{
	FScopeLock Lock(&SharedLock);
	if (const FString* Reason = FailedTickets.Find(Ticket))
	{
		OutReason = *Reason;
		return true;
	}
	return false;
}

bool FDreamJournalWriter::TakeScanResult(FDreamRecoveredSession& Out)
{
	FScopeLock Lock(&SharedLock);
	if (!ScanResult.IsSet())
	{
		return false;
	}
	Out = MoveTemp(ScanResult.GetValue());
	ScanResult.Reset();
	return true;
}

uint32 FDreamJournalWriter::Run()
{
	while (!bStopRequested.load(std::memory_order_acquire))
	{
		TUniquePtr<DreamJournal::FRequest> Request;
		bool bDidWork = false;
		// Commits run first (§10.2.1): every due request is written before the next unit of background work.
		while (Queue.Dequeue(Request))
		{
			bDidWork = true;
			if (Request.IsValid())
			{
				Process(*Request);
			}
		}
		RetryIfDue();
		if (!bStopRequested.load(std::memory_order_acquire) && DoBackgroundUnit())
		{
			bDidWork = true;
		}
		if (!bDidWork)
		{
			WakeEvent->Wait(FTimespan::FromMilliseconds(250.0));
		}
	}
	// Drain what the game thread queued before it asked us to stop (final snapshots and CLEAN, §10.2.5).
	TUniquePtr<DreamJournal::FRequest> Request;
	while (Queue.Dequeue(Request))
	{
		if (Request.IsValid())
		{
			Process(*Request);
		}
	}
	if (Writer.IsValid() && Writer->IsRotating())
	{
		Writer->AbandonRotation();   // its .tmp goes now (or the next reader removes it)
	}
	// Review r1: the queued history work gets what is left of the shutdown budget (a scratch tab closed just before exit has
	// its text only in its "closed" entry); long maintenance stops at once (bExiting). FDreamJournal::Shutdown joins after
	// this and only then releases the locks, so no other instance writes <root>/history meanwhile. Then the rest is dropped.
	bExitDrain = true;
	const double Until = DrainUntil.load(std::memory_order_acquire);
	while (FPlatformTime::Seconds() < Until && !(Fence.IsValid() && Fence->load(std::memory_order_acquire)) && DoBackgroundUnit())
	{
	}
	Background.Reset();
	FDreamIoSlice Dropped;
	while (IoQueue.Dequeue(Dropped))
	{
	}
	return 0;
}

void FDreamJournalWriter::ServeCommitsFor(float Seconds)
{
	// The history slice that waits is not touched here: commits only post new history work (IoQueue), never run it.
	const double Until = FPlatformTime::Seconds() + Seconds;
	for (;;)
	{
		TUniquePtr<DreamJournal::FRequest> Request;
		while (Queue.Dequeue(Request))
		{
			if (Request.IsValid())
			{
				Process(*Request);
			}
		}
		RetryIfDue();
		const double Left = Until - FPlatformTime::Seconds();
		if (Left <= 0.0)
		{
			return;
		}
		WakeEvent->Wait(FTimespan::FromSeconds(Left));
	}
}

bool FDreamJournalWriter::DoBackgroundUnit()
{
	const double Now = FPlatformTime::Seconds();
	if (Writer.IsValid() && Writer->IsRotating())
	{
		const dream::journal::RotationStep Step = Writer->StepRotation(dream::journal::RotationSliceBytes);
		if (Step == dream::journal::RotationStep::Ready)
		{
			// Steps 2-5: the catch-up of the batches committed meanwhile, the rename, then generation g-1 by its header. A rename
			// that succeeded is the switch even when g+1 could not be opened (persist-M4): the commits then fail into the retry
			// queue until the writer opens it, or the next rotation goes past it.
			const bool bFinished = Writer->FinishRotation(Now);
			CurrentGeneration.store(Writer->Generation(), std::memory_order_release);
			if (!bFinished)
			{
				UE_LOG(LogDream, Warning, TEXT("Journal rotation failed: %s"), *DreamText::Utf8ToFString(Writer->LastError()));
				NextRotationSeconds = Now + DreamJournalWriterLocal::RotationRetrySeconds;
			}
		}
		else if (Step == dream::journal::RotationStep::Failed)
		{
			UE_LOG(LogDream, Warning, TEXT("Journal rotation failed: %s"), *DreamText::Utf8ToFString(Writer->LastError()));
			NextRotationSeconds = Now + DreamJournalWriterLocal::RotationRetrySeconds;
		}
		return true;
	}

	FDreamIoSlice Item;
	while (IoQueue.Dequeue(Item))
	{
		Background.Add(MoveTemp(Item));
	}
	if (Background.Num() == 0)
	{
		return false;
	}
	// §10.5: a secondary's entries are staged in <sec>/pending-history; the primary merges them.
	FDreamIoContext Context{ *Ops, Fence, SessionId, Options.HistoryDir };
	Context.bExiting = bExitDrain;
	// §10.2.1/§10.2.3 (review r1): a history or maintenance slice that waits out a file another program holds (the FileOps
	// retries, up to about 0.9 s per call) serves the due commits during that wait, so no commit waits behind it.
	const FDreamWin32FileOps::FScopedRetryWait ServeCommits([this](float Seconds) { ServeCommitsFor(Seconds); });
	const bool bMore = Background[0](Context);
	if (!bMore)
	{
		Background.RemoveAt(0);
	}
	return true;
}

void FDreamJournalWriter::Process(DreamJournal::FRequest& Request)
{
	using DreamJournal::ERequestKind;
	switch (Request.Kind)
	{
	case ERequestKind::Scan:
		ProcessScan();
		MarkTickets({ Request.Commit.Ticket }, true, FString());
		return;
	case ERequestKind::StartGeneration:
		if (ProcessStartGeneration(Request))
		{
			// The writer held every commit requested before the reconcile until this checkpoint (§10.2.3).
			TArray<TUniquePtr<DreamJournal::FRequest>> Held = MoveTemp(HeldUntilReconcile);
			for (TUniquePtr<DreamJournal::FRequest>& HeldRequest : Held)
			{
				Process(*HeldRequest);
			}
		}
		return;
	case ERequestKind::Commit:
	case ERequestKind::Clean:
		break;
	}

	if (!bGenerationStarted && GetHealth() == EDreamJournalHealth::ReadOnly)
	{
		// §10.6: nothing is written to a newer format's namespace; a run that asked for a snapshot ends not executed.
		HeldUntilReconcile.Reset();
		MarkTickets({ Request.Commit.Ticket }, Request.Kind == ERequestKind::Clean, TEXT("the journal is read-only: a newer Dream Python Editor wrote it"));
		if (Request.Kind == ERequestKind::Clean)
		{
			bCleanAcked.store(true, std::memory_order_release);
		}
		return;
	}
	if (!bGenerationStarted)
	{
		if (Request.Kind == ERequestKind::Clean && HeldUntilReconcile.Num() == 0 && Planner.BufferCount() == 0)
		{
			bCleanAcked.store(true, std::memory_order_release);   // nothing was ever journaled in this session
			return;
		}
		HeldUntilReconcile.Add(MakeUnique<DreamJournal::FRequest>(MoveTemp(Request)));
		return;
	}
	if (Planner.HasRetryQueue())
	{
		// An earlier commit failed: this one waits behind it, so the records reach the disk in the order they were made
		// (a BUF_OPEN before the BUF_CLOSE of the same buffer); its plain text parts fold into the waiting requests, and the
		// retry writes the whole queue as one batch. Its ticket fails now with the writer's error, as the failed request's
		// did, so a run's pre-run snapshot reports the real reason instead of a 2 s timeout.
		const uint64 Ticket = Request.Commit.Ticket;
		Request.Commit.bClean = Request.Kind == ERequestKind::Clean;
		Request.Commit.CleanUtcMs = DreamJournalWriterLocal::NowUtcMs();
		Planner.QueueBehindFailure(MoveTemp(Request.Commit));
		RetryQueueLength.store(static_cast<int32>(Planner.RetryQueueLength()), std::memory_order_release);
		MarkTickets({ Ticket }, false, LastErrorText());
		return;
	}
	CommitRequest(Request);
}

FString FDreamJournalWriter::LastErrorText() const
{
	return DreamText::Utf8ToFString(Writer.IsValid() ? Writer->LastError() : Ops->LastError());
}

bool FDreamJournalWriter::CommitRequest(DreamJournal::FRequest& Request)
{
	using DreamJournal::ERequestKind;
	Request.Commit.bClean = Request.Kind == ERequestKind::Clean;
	Request.Commit.CleanUtcMs = DreamJournalWriterLocal::NowUtcMs();
	dream::journal::BatchPlan Plan = Planner.Plan(Request.Commit, Writer.IsValid() && Writer->NeedsCheckpoint());
	const bool bOk = Plan.Records.empty() || CommitBatch(Plan.Records);
	if (!bOk)
	{
		// Nothing of the plan was applied: the retry builds and writes the whole request again (§10.2.5 "RUN_END gets a
		// forced commit"; a BUF_OPEN, META, BUF_CLOSE or RECOVERY_RESOLVED is never dropped).
		MarkTickets({ Request.Commit.Ticket }, false, LastErrorText());
		Planner.QueueFailed(MoveTemp(Request.Commit));
		RetryQueueLength.store(static_cast<int32>(Planner.RetryQueueLength()), std::memory_order_release);
		bNeedsRetry = true;
		NextRetrySeconds = FPlatformTime::Seconds() + DreamJournalWriterLocal::RetrySeconds;
		return false;
	}
	Planner.Committed(MoveTemp(Plan));
	if (Request.Kind == ERequestKind::Clean)
	{
		bCleanAcked.store(true, std::memory_order_release);
	}
	// §10.4: the history work this commit caused runs after it, in slices that yield to commits.
	if (HistorySink)
	{
		for (FDreamHistoryNote& Note : Request.History)
		{
			PostIo(HistorySink(MoveTemp(Note)));
		}
	}
	Request.History.Reset();
	AfterDurableCommit();
	MarkTickets({ Request.Commit.Ticket }, true, FString());
	return true;
}

void FDreamJournalWriter::AfterDurableCommit()
{
	if (!Writer.IsValid() || Writer->IsRotating())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	const bool bForced = bForceRotationForTest.load(std::memory_order_acquire);
	if ((bForced || Writer->NeedsRotation(Now)) && (bForced || Now >= NextRotationSeconds))
	{
		bForceRotationForTest.store(false, std::memory_order_release);
		// §10.2.3 step 1 begins: the checkpoint of what is durable now; its slices run as background work, and the batches
		// committed meanwhile are the catch-up.
		if (!Writer->BeginRotation(Planner.BuildCheckpoint(), DreamJournalWriterLocal::NowUtcMs()))
		{
			UE_LOG(LogDream, Warning, TEXT("Journal rotation could not start: %s"), *DreamText::Utf8ToFString(Writer->LastError()));
			NextRotationSeconds = Now + DreamJournalWriterLocal::RotationRetrySeconds;
		}
	}
}

bool FDreamJournalWriter::ProcessStartGeneration(DreamJournal::FRequest& Request)
{
	using namespace dream::journal;
	if (bGenerationStarted)
	{
		return true;
	}
	if (GetHealth() == EDreamJournalHealth::ReadOnly)
	{
		// §10.6: a namespace in a newer format is never written, rotated or deleted in; nothing of this session is journaled
		// there (FDreamJournal does not ask for this, the guard is the writer's own).
		TArray<uint64> Tickets = { Request.Commit.Ticket };
		for (const TUniquePtr<DreamJournal::FRequest>& Held : HeldUntilReconcile)
		{
			Tickets.Add(Held->Commit.Ticket);
		}
		HeldUntilReconcile.Reset();
		MarkTickets(Tickets, false, TEXT("the journal is read-only: a newer Dream Python Editor wrote it"));
		return false;
	}

	// persist-M2: a generation the scan could not use (unreadable even after the FileOps retries, or damaged) must outlive
	// this session's generation, which the next read would treat as newer and delete it under. It is set aside first (kept
	// 30 days); while one is still in place nothing starts: Degraded, the commits held, retried every second.
	bool bNewerFormatAbove = false;
	const bool bClearAbove = SetAsideUnusedNewer(*Ops, FDreamWin32FileOps::ToUtf8Path(Dir), JournalLogFormat, ScanUsedGeneration,
		&bNewerFormatAbove);   // once: its FileOps retries are not paid twice while the commits are held (review r3)
	if (!bClearAbove && bNewerFormatAbove)
	{
		// Review r2 (§10.6): a newer format's generation turned up above the one read (its header could not be read at the
		// scan): the namespace is read only from here, as after a scan that saw it.
		Health.store(static_cast<uint8>(EDreamJournalHealth::ReadOnly), std::memory_order_release);
		TArray<uint64> Tickets = { Request.Commit.Ticket };
		for (const TUniquePtr<DreamJournal::FRequest>& Held : HeldUntilReconcile)
		{
			Tickets.Add(Held->Commit.Ticket);
		}
		HeldUntilReconcile.Reset();
		MarkTickets(Tickets, false, TEXT("the journal is read-only: a newer Dream Python Editor wrote it"));
		UE_LOG(LogDream, Warning, TEXT("The journal of %s holds a generation of a newer Dream Python Editor; this session's tabs are not journaled."), *Dir);
		return false;
	}
	if (!bClearAbove)
	{
		const FString Reason = TEXT("a journal generation the recovery could not read is held by another program");
		UE_LOG(LogDream, Warning, TEXT("The journal of %s does not start yet: %s."), *Dir, *Reason);
		Health.store(static_cast<uint8>(EDreamJournalHealth::Degraded), std::memory_order_release);
		bNeedsRetry = true;
		NextRetrySeconds = FPlatformTime::Seconds() + DreamJournalWriterLocal::RetrySeconds;
		TArray<uint64> Tickets = { Request.Commit.Ticket };
		for (const TUniquePtr<DreamJournal::FRequest>& Held : HeldUntilReconcile)
		{
			Tickets.Add(Held->Commit.Ticket);
		}
		MarkTickets(Tickets, false, Reason);
		HeldUntilReconcile.Insert(MakeUnique<DreamJournal::FRequest>(MoveTemp(Request)), 0);
		return false;
	}

	// This session's generation is above every existing one, corrupt and migrated files included (§10.2.3).
	std::vector<std::string> Names;
	Ops->List(FDreamWin32FileOps::ToUtf8Path(Dir), Names);
	uint64 Highest = ScanGeneration;
	for (const std::string& Name : Names)
	{
		uint64 Generation = 0;
		const std::string Base = Name.substr(0, Name.find(".sjl") == std::string::npos ? Name.size() : Name.find(".sjl") + 4);
		if (ParseGenerationFileName(Base, Generation))
		{
			Highest = FMath::Max(Highest, Generation);
		}
	}

	for (const BufferPart& Part : Request.Commit.Buffers)
	{
		Planner.SeedBuffer(Part);
	}
	Planner.SetPendingRecoveries(Request.Pending);
	if (Request.Commit.bHasSession)
	{
		Planner.SetSession(Request.Commit.Session);
	}

	FileHeader Proto;
	Proto.InstancePid = FPlatformProcess::GetCurrentProcessId();
	Proto.InstanceStart = static_cast<uint64>(SessionStartUtcMs);
	Proto.TextUnit = 1;
	Writer = MakeUnique<dream::journal::Writer>(*Ops, FDreamWin32FileOps::ToUtf8Path(Dir), Proto);
	{
		// HEADER_EXT (§10.2.2): engine and plugin versions, project hash, namespace, host; the writer adds the previous generation.
		dream::json::Value Ext = dream::json::Value::MakeObject();
		Ext.Set("engine", dream::json::Value::MakeString(DreamText::ToUtf8(FEngineVersion::Current().ToString())));
		Ext.Set("plugin", dream::json::Value::MakeString("0.1.0"));
		Ext.Set("project_hash", dream::json::Value::MakeString(DreamText::ToUtf8(Root.ProjectHash)));
		Ext.Set("namespace", dream::json::Value::MakeString(DreamText::ToUtf8(Namespace)));
		Ext.Set("host", dream::json::Value::MakeString(DreamText::ToUtf8(FString(FPlatformProcess::ComputerName()))));
		Writer->SetHeaderExtFields(MoveTemp(Ext));
	}

	// (slice B choice) The session's first generation is written in one go (every commit waits for it anyway, §10.2.3);
	// the rotations after it are sliced.
	const std::vector<Record> Checkpoint = Planner.BuildCheckpoint();
	const bool bOk = Writer->StartGeneration(Highest + 1, Checkpoint, DreamJournalWriterLocal::NowUtcMs(), FPlatformTime::Seconds());
	if (!bOk)
	{
		const FString Reason = DreamText::Utf8ToFString(Writer->LastError());
		UE_LOG(LogDream, Warning, TEXT("The journal could not start generation %llu: %s"), Highest + 1, *Reason);
		Health.store(static_cast<uint8>(EDreamJournalHealth::Degraded), std::memory_order_release);
		Writer.Reset();
		bNeedsRetry = true;
		NextRetrySeconds = FPlatformTime::Seconds() + DreamJournalWriterLocal::RetrySeconds;
		// The held commits are not durable either: a run waiting on its snapshot ends not executed now, with the reason,
		// instead of reading durable from a later ticket (PER-09). They stay held and are written after the retry.
		TArray<uint64> Tickets = { Request.Commit.Ticket };
		for (const TUniquePtr<DreamJournal::FRequest>& Held : HeldUntilReconcile)
		{
			Tickets.Add(Held->Commit.Ticket);
		}
		MarkTickets(Tickets, false, Reason);
		HeldUntilReconcile.Insert(MakeUnique<DreamJournal::FRequest>(MoveTemp(Request)), 0);
		return false;
	}
	bGenerationStarted = true;
	CurrentGeneration.store(Writer->Generation(), std::memory_order_release);
	LastDurableUtcTicks.store(FDateTime::UtcNow().GetTicks(), std::memory_order_release);
	if (GetHealth() == EDreamJournalHealth::Degraded)
	{
		Health.store(static_cast<uint8>(EDreamJournalHealth::Ok), std::memory_order_release);   // a retried start succeeded
	}
	if (HistorySink)
	{
		for (FDreamHistoryNote& Note : Request.History)
		{
			PostIo(HistorySink(MoveTemp(Note)));   // the pending texts as "recovered" entries (§10.7.1)
		}
	}
	Request.History.Reset();
	MarkTickets({ Request.Commit.Ticket }, true, FString());
	return true;
}

bool FDreamJournalWriter::CommitBatch(const std::vector<dream::journal::Record>& Batch)
{
	if (!Writer.IsValid())
	{
		return false;
	}
	if (bFailCommitsForTest.load(std::memory_order_acquire))
	{
		UE_LOG(LogDream, Log, TEXT("Journal commit failed: the test fault is on"));
		Health.store(static_cast<uint8>(EDreamJournalHealth::Degraded), std::memory_order_release);
		bNeedsRetry = true;
		NextRetrySeconds = FPlatformTime::Seconds() + DreamJournalWriterLocal::RetrySeconds;
		return false;
	}
	if (Writer->Commit(Batch))
	{
		if (GetHealth() == EDreamJournalHealth::Degraded)
		{
			Health.store(static_cast<uint8>(EDreamJournalHealth::Ok), std::memory_order_release);
		}
		LastDurableUtcTicks.store(FDateTime::UtcNow().GetTicks(), std::memory_order_release);
		return true;
	}
	UE_LOG(LogDream, Warning, TEXT("Journal commit failed: %s"), *DreamText::Utf8ToFString(Writer->LastError()));
	Health.store(static_cast<uint8>(EDreamJournalHealth::Degraded), std::memory_order_release);
	bNeedsRetry = true;
	NextRetrySeconds = FPlatformTime::Seconds() + DreamJournalWriterLocal::RetrySeconds;
	return false;
}

void FDreamJournalWriter::MarkTickets(const TArray<uint64>& Tickets, bool bOk, const FString& Reason)
{
	FScopeLock Lock(&SharedLock);
	for (const uint64 Ticket : Tickets)
	{
		if (Ticket == 0)
		{
			continue;
		}
		PendingTickets.Remove(Ticket);
		if (!bOk)
		{
			FailedTickets.Add(Ticket, Reason.IsEmpty() ? FString(TEXT("the journal write failed")) : Reason);
		}
	}
}

void FDreamJournalWriter::RetryIfDue()
{
	if (!bNeedsRetry || FPlatformTime::Seconds() < NextRetrySeconds)
	{
		return;
	}
	bNeedsRetry = false;
	if (!bGenerationStarted)
	{
		// The reconcile checkpoint failed: try it again with the request it was given.
		if (HeldUntilReconcile.Num() > 0 && HeldUntilReconcile[0]->Kind == DreamJournal::ERequestKind::StartGeneration)
		{
			TUniquePtr<DreamJournal::FRequest> Start = MoveTemp(HeldUntilReconcile[0]);
			HeldUntilReconcile.RemoveAt(0);
			Process(*Start);
		}
		return;
	}
	if (!Planner.HasRetryQueue())
	{
		return;
	}

	// P2-per-02: every kept request, in order and rebuilt against the durable state (a part older than it is covered,
	// PER-03), as ONE batch with one flush; a failure keeps the whole queue for the next retry.
	dream::journal::BatchPlan Replay = Planner.PlanReplay(Writer.IsValid() && Writer->NeedsCheckpoint());
	if (!Replay.Records.empty() && !CommitBatch(Replay.Records))
	{
		// persist-M4: a generation renamed into place but never opened makes NeedsRotation true; with no commit succeeding,
		// the rotation that goes past it has to start from here.
		AfterDurableCommit();
		return;
	}
	const bool bClean = Replay.bClean;
	const TArray<uint64> Tickets(Replay.Tickets.data(), static_cast<int32>(Replay.Tickets.size()));
	Planner.Committed(MoveTemp(Replay));
	RetryQueueLength.store(0, std::memory_order_release);
	if (bClean)
	{
		bCleanAcked.store(true, std::memory_order_release);
	}
	AfterDurableCommit();
	MarkTickets(Tickets, true, FString());
}

void FDreamJournalWriter::ProcessScan()
{
	using namespace DreamJournalWriterLocal;
	FDreamRecoveredSession Session;
	std::string DirUtf8 = FDreamWin32FileOps::ToUtf8Path(Dir);
	Ops->MakeDirs(DirUtf8);

	// §10.6: the newest generation's header first. A newer format (a newer major, or a newer minor of this major) is read
	// without touching the folder: no stale *.tmp deleted, no damaged file renamed, and this session writes nothing there.
	bool bNewerFormat = false;
	int32 NewerMajor = 0;
	int32 NewerMinor = 0;
	{
		std::vector<std::string> Names;
		uint64 Newest = 0;
		Ops->List(DirUtf8, Names);
		for (const std::string& Name : Names)
		{
			uint64 Generation = 0;
			if (dream::journal::ParseGenerationFileName(Name, Generation))
			{
				Newest = FMath::Max(Newest, Generation);
			}
		}
		std::string HeadBytes;
		dream::journal::FileHeader Header;
		if (Newest > 0
			&& Ops->ReadPrefix(dream::journal::JoinPath(DirUtf8, dream::journal::GenerationFileName(Newest)), dream::journal::HeaderBytes, HeadBytes)
			&& dream::journal::DecodeHeader(HeadBytes, Header))
		{
			bNewerFormat = Header.Major > dream::journal::FormatMajor
				|| (Header.Major == dream::journal::FormatMajor && Header.Minor > dream::journal::FormatMinor);
			NewerMajor = Header.Major;
			NewerMinor = Header.Minor;
		}
	}
	dream::journal::RecoveredNamespace NS = dream::journal::ReadNamespace(*Ops, DirUtf8, /*bMayModify*/ !bNewerFormat);
	if (NS.bNewerFormat && !bNewerFormat)
	{
		NewerMajor = NS.Header.Major;
		NewerMinor = NS.Header.Minor;
	}
	bNewerFormat = bNewerFormat || NS.bNewerFormat || NS.bNewerMinor;   // the reader's own check of the same header
	FString SourceNamespace = Namespace;
	uint64 SourceGenerationForStart = NS.Generation;
	uint64 UsedGenerationForStart = NS.UsedGeneration;

	// §10.6 "Older plugin, newer data": nothing is written, rotated or deleted in that namespace; this session journals its
	// own in journal/compat-<format>/ under that namespace's own ns.lock. (slice B choice) The compat namespace's previous
	// session is what this build restores from; only while it has none yet (the first older-build session), a newer minor's
	// main is read and restored as slice A did, read-only.
	bool bCompat = false;
	if (bNewerFormat && Options.bPrimary)
	{
		const FString CompatName = DreamText::Utf8ToFString(dream::storage::MakeCompatNamespace(dream::journal::FormatMajor, dream::journal::FormatMinor));
		const FString CompatDir = Root.JournalDir(CompatName);
		Ops->MakeDirs(FDreamWin32FileOps::ToUtf8Path(CompatDir));
		bool bHeld = false;
		const uint64 Lock = FDreamWin32FileOps::AcquireLockFile(CompatDir / TEXT("ns.lock"), bHeld, DreamJournalScan::MakeLockContent(CompatName));
		if (Lock != 0)
		{
			CompatLock.store(Lock, std::memory_order_release);
			dream::journal::RecoveredNamespace CompatNS = dream::journal::ReadNamespace(*Ops, FDreamWin32FileOps::ToUtf8Path(CompatDir), true);
			SourceGenerationForStart = CompatNS.Generation;
			UsedGenerationForStart = CompatNS.UsedGeneration;
			if (CompatNS.bFound && !CompatNS.bNewerFormat && !CompatNS.bNewerMinor)
			{
				NS = MoveTemp(CompatNS);
				SourceNamespace = CompatName;
			}
			{
				FScopeLock Lock2(&SharedLock);
				Namespace = CompatName;
			}
			Dir = CompatDir;
			DirUtf8 = FDreamWin32FileOps::ToUtf8Path(Dir);
			bCompat = true;
			UE_LOG(LogDream, Log, TEXT("This project's journal was written by a newer Dream Python Editor (format %d.%d); this session journals in %s (§10.6)."),
				NewerMajor, NewerMinor, *Dir);
		}
		else
		{
			UE_LOG(LogDream, Warning, TEXT("This project's journal was written by a newer Dream Python Editor and %s is held by another editor; this session's tabs are not journaled."),
				*CompatDir);
		}
	}
	ScanGeneration = SourceGenerationForStart;
	ScanUsedGeneration = UsedGenerationForStart;
	if (NS.SourceMajor != dream::journal::FormatMajor)
	{
		UE_LOG(LogDream, Log, TEXT("The journal of %s was written in format %d; this session writes format %d (§10.6)."), *Dir,
			NS.SourceMajor, dream::journal::FormatMajor);
	}

	Session.bPreviousExitClean = !NS.bFound || NS.bClean;
	Session.bUnreadable = NS.bUnreadable;
	Session.bNewerFormat = bNewerFormat;
	Session.bCompat = bCompat;
	Session.NewerMajor = NewerMajor;
	Session.NewerMinor = NewerMinor;
	Session.SourceNamespace = SourceNamespace;
	Session.bFallbackUsed = NS.bFallbackUsed;
	Session.bSalvageUsed = NS.bSalvageUsed;
	Session.PreviousEndUtc = NS.LastRecordUtcMs > 0 ? FromUtcMs(NS.LastRecordUtcMs) : FDateTime();
	Session.SourceGeneration = NS.Generation;
	if (bNewerFormat && !bCompat)
	{
		Health.store(static_cast<uint8>(EDreamJournalHealth::ReadOnly), std::memory_order_release);
	}

	// SESSION head (shell format): {"tabs": [{"uuid", "caret": [l, c], "first", "isolated", "one_undo"}], "active"}
	if (const dream::json::Value* Tabs = NS.Session.Find("tabs"))
	{
		for (size_t Index = 0; Index < Tabs->Size(); ++Index)
		{
			const dream::json::Value& Tab = Tabs->At(Index);
			FGuid Id;
			if (!FGuid::Parse(DreamText::Utf8ToFString(Tab.Get("uuid").AsString()), Id))
			{
				continue;
			}
			FDreamBufferViewState View;
			View.CaretLine = static_cast<int32>(Tab.Get("caret").At(0).AsInt());
			View.CaretColumn = static_cast<int32>(Tab.Get("caret").At(1).AsInt());
			View.FirstVisibleLine = static_cast<int32>(Tab.Get("first").AsInt());
			View.bOneUndoStepPerRun = Tab.Get("one_undo").AsBool(true);
			View.Namespace = Tab.Get("isolated").AsBool(false) ? EDreamNamespaceMode::Isolated : EDreamNamespaceMode::Session;
			Session.SessionViewStates.Add(Id, View);
			if (!Session.bPreviousExitClean)
			{
				Session.SessionTabOrder.Add(Id);
			}
		}
		if (!Session.bPreviousExitClean)
		{
			FGuid::Parse(DreamText::Utf8ToFString(NS.Session.Get("active").AsString()), Session.SessionActiveTab);
		}
	}

	TMap<FString, FString> DisplayByUuid;
	TSet<FGuid> ClosedIds;
	for (const dream::journal::RecoveredBuffer& RB : NS.Buffers)
	{
		DisplayByUuid.Add(DreamText::Utf8ToFString(RB.Open.Uuid), DreamText::Utf8ToFString(RB.Open.Display));
		if (RB.bClosed)
		{
			FGuid ClosedId;
			if (FGuid::Parse(DreamText::Utf8ToFString(RB.Open.Uuid), ClosedId))
			{
				ClosedIds.Add(ClosedId);
			}
			continue;
		}
		if (!RB.bHasText)
		{
			++Session.UnverifiedBuffers;
			continue;
		}
		if (RB.Open.Uuid == "repl-input")
		{
			Session.ReplInputText = DreamText::Utf8ToFString(RB.TextUtf8);
			continue;
		}
		FDreamRecoveredBuffer Buffer;
		if (!FGuid::Parse(DreamText::Utf8ToFString(RB.Open.Uuid), Buffer.Id))
		{
			continue;
		}
		Buffer.Kind = KindFromName(RB.Open.Kind);
		Buffer.DisplayName = DreamText::Utf8ToFString(RB.Open.Display);
		Buffer.Path = DreamLegacyMigration::RemapLegacyPath(DreamText::Utf8ToFString(RB.Open.Path));   // (rename)
		Buffer.RecoveredText = DreamText::Utf8ToFString(RB.TextUtf8);
		Buffer.Format.Enc = EncodingFromName(RB.Open.Encoding);
		Buffer.Format.bBom = RB.Open.bBom;
		Buffer.Format.Dominant = EolFromName(RB.Open.Eol);
		Buffer.Base = ToBase(RB.Open.Base);
		Buffer.LastEditUtc = Session.PreviousEndUtc;
		if (Buffer.Kind != EDreamBufferKind::Scratch && !Buffer.Path.IsEmpty())
		{
			bool bDiffers = false;
			CompareWithDisk(*Ops, Buffer, bDiffers);
			Buffer.bDiffersFromDisk = bDiffers;
			// n counts file buffers whose journal holds changes not on disk, after an unclean exit (§10.7.1). After a clean
			// exit the journal simply supplies the dirty text (§10.3).
			Buffer.bCounted = bDiffers && !Session.bPreviousExitClean;
			if (!bDiffers)
			{
				Buffer.bConflict = false;
			}
		}
		else
		{
			// §10.7.1 scratch buffers: the clean text is the text this buffer had when the crashed session restored it
			// (CLEAN_TEXT); one created in that session has none, so its clean text is empty. After an unclean exit a scratch
			// buffer counts when its journal text differs from its clean text: it then shows the clean text, pending.
			Buffer.CleanText = RB.CleanTextUtf8.has_value() ? DreamText::Utf8ToFString(*RB.CleanTextUtf8) : FString();
			Buffer.bCounted = !Session.bPreviousExitClean && Buffer.RecoveredText != Buffer.CleanText.GetValue();
			if (Buffer.bCounted)
			{
				CountLineChanges(Buffer.RecoveredText, Buffer.CleanText.GetValue(), Buffer.LinesAdded, Buffer.LinesRemoved);
			}
		}
		Session.Buffers.Add(MoveTemp(Buffer));
	}

	// A tab closed after the last SESSION record is closed: its BUF_CLOSE is newer than that tab list (§10.2.5), so the
	// restore after an unclean exit must not bring it back (it would come back empty, its text was never recovered).
	Session.SessionTabOrder.RemoveAll([&ClosedIds](const FGuid& Id) { return ClosedIds.Contains(Id); });
	if (ClosedIds.Contains(Session.SessionActiveTab))
	{
		Session.SessionActiveTab.Invalidate();
	}
	Session.ClosedIds = ClosedIds;

	// An unresolved RECOVERY_PENDING from an earlier start is offered again however the last session ended (§10.7.1).
	for (const dream::journal::RecoveredPending& Pending : NS.Pending)
	{
		FGuid Id;
		if (!FGuid::Parse(DreamText::Utf8ToFString(Pending.Info.Uuid), Id))
		{
			continue;
		}
		FDreamRecoveredBuffer* Existing = Session.Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; });
		FDreamRecoveredBuffer Fresh;
		FDreamRecoveredBuffer& Buffer = Existing ? *Existing : Fresh;
		if (!Existing)
		{
			Buffer.Id = Id;
			Buffer.Kind = KindFromName(Pending.Info.Kind);
			Buffer.Path = DreamLegacyMigration::RemapLegacyPath(DreamText::Utf8ToFString(Pending.Info.Path));   // (rename)
			Buffer.DisplayName = DisplayByUuid.FindRef(DreamText::Utf8ToFString(Pending.Info.Uuid));
			Buffer.Base = ToBase(Pending.Info.Base);
			Buffer.Format.Dominant = EolFromName(Pending.Info.Eol);
		}
		const FString Shown = Existing ? Existing->RecoveredText : FString();   // the live text the pending bar covers
		Buffer.RecoveredText = DreamText::Utf8ToFString(Pending.TextUtf8);
		Buffer.bFromEarlierStart = true;
		bool bDiffers = true;
		if (!Buffer.Path.IsEmpty() && Buffer.Kind != EDreamBufferKind::Scratch)
		{
			CompareWithDisk(*Ops, Buffer, bDiffers);
		}
		else
		{
			// A scratch buffer's pending text waits over the text it shows, which is its clean text.
			if (!Buffer.CleanText.IsSet())
			{
				Buffer.CleanText = Shown;
			}
			bDiffers = Buffer.RecoveredText != Buffer.CleanText.GetValue();
			CountLineChanges(Buffer.RecoveredText, Buffer.CleanText.GetValue(), Buffer.LinesAdded, Buffer.LinesRemoved);
		}
		Buffer.bCounted = bDiffers;
		if (!Existing)
		{
			Session.Buffers.Add(MoveTemp(Fresh));
		}
	}

	for (const dream::journal::RecoveredRun& Run : NS.OpenRuns)
	{
		FDreamLostRun Lost;
		Lost.PreviousSessionUtc = Session.PreviousEndUtc;
		Lost.RunId = Run.Begin.RunId;
		Lost.RunNumber = static_cast<int32>(Run.Begin.Number);
		FGuid::Parse(DreamText::Utf8ToFString(Run.Begin.TabUuid), Lost.TabUuid);
		Lost.TabTitle = DisplayByUuid.FindRef(DreamText::Utf8ToFString(Run.Begin.TabUuid));
		Lost.Route = RouteFromKey(Run.Begin.Route);
		Lost.StartUtc = FromUtcMs(Run.Begin.UtcMs);
		Lost.bPaused = Run.bPaused;
		Session.bPausedAtExit |= Run.bPaused;
		Session.LostRuns.Add(MoveTemp(Lost));
	}

	for (const FDreamRecoveredBuffer& Buffer : Session.Buffers)
	{
		Session.CountedBuffers += Buffer.bCounted ? 1 : 0;
	}

	// §10.3 step G, §10.5 "Orphans": the primary also reads every other namespace of its root whose ns.lock is free, and
	// keeps that lock while the namespace is listed. (slice B choice) An older build on newer data (compat) lists none: it
	// may not be able to read what the newer build wrote, and it must never delete it.
	if (Options.bPrimary && !bNewerFormat)
	{
		ScanOrphans(Session.Orphans);
	}

	FScopeLock Lock(&SharedLock);
	ScanResult = MoveTemp(Session);
}

void FDreamJournalWriter::ScanOrphans(TArray<FDreamOrphanNamespace>& Out)
{
	const FString JournalRoot = Root.Root / TEXT("journal");
	TArray<FString> Names;
	FDreamWin32FileOps::ListDirs(JournalRoot, Names);
	const FString Own = GetNamespace();
	FDreamReadOnlyFileOps ReadOps(*Ops);
	for (const FString& Name : Names)
	{
		if (Fence.IsValid() && Fence->load(std::memory_order_acquire))
		{
			return;
		}
		const dream::storage::NamespaceName Parsed = dream::storage::ParseNamespace(DreamText::ToUtf8(Name));
		if (Name.Equals(Own, ESearchCase::IgnoreCase) || Parsed.Kind == dream::storage::NamespaceKind::Main
			|| Parsed.Kind == dream::storage::NamespaceKind::Invalid)
		{
			continue;
		}
		const FString NsDir = Root.JournalDir(Name);
		bool bHeld = false;
		// (instances, review r1) Only an existing ns.lock: a secondary creates its folder, then its ns.lock with its handle in
		// one step, so a folder without one is a namespace still starting, never an orphan to take and delete.
		const uint64 NsLock = FDreamWin32FileOps::AcquireLockFile(NsDir / TEXT("ns.lock"), bHeld, DreamJournalScan::MakeLockContent(Name),
			/*bOnlyIfExists*/ true);
		if (NsLock == 0)
		{
			continue;   // live (a running secondary, another editor's compat session) or still starting
		}
		FDreamOrphanNamespace Orphan;
		DreamJournalScan::ReadOrphan(ReadOps, *Ops, Root.Root, Name, /*bAll*/ false, Orphan);
		if (Orphan.Key.IsEmpty())
		{
			// A newer format or an unreadable journal: kept as it is, never listed or deleted by this build.
			FDreamWin32FileOps::ReleaseLockFile(NsLock);
			continue;
		}
		switch (Parsed.Kind)
		{
		case dream::storage::NamespaceKind::Secondary:
			Orphan.Source = EDreamOrphanSource::SecondaryInstance;
			break;
		case dream::storage::NamespaceKind::Compat:
			Orphan.Source = EDreamOrphanSource::Compat;
			Orphan.Detail = DreamText::Utf8ToFString(Parsed.Version);
			break;
		case dream::storage::NamespaceKind::Imported:
			Orphan.Source = EDreamOrphanSource::Imported;
			break;
		default:
			Orphan.Source = EDreamOrphanSource::OtherNamespace;
			break;
		}
		Orphan.NamespaceLock = NsLock;
		Out.Add(MoveTemp(Orphan));
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Namespace reads shared with FDreamInstances (§10.1, §10.5)
// ---------------------------------------------------------------------------------------------------------------------

std::string DreamJournalScan::MakeLockContent(const FString& InNamespace)
{
	dream::storage::LockContent Content;
	Content.Pid = FPlatformProcess::GetCurrentProcessId();
	// The process creation time of FDreamAudit's secondary id ("i-<pid>-<start>"), so a reused PID is recognised (§10.5).
	const dream::storage::NamespaceName Parsed = dream::storage::ParseNamespace(DreamText::ToUtf8(FDreamAudit::MakeSecondaryId()));
	Content.ProcessStartMs = Parsed.ProcessStartMs;
	Content.Host = DreamText::ToUtf8(FString(FPlatformProcess::ComputerName()));
	Content.PluginVersion = "0.1.0";
	Content.Namespace = DreamText::ToUtf8(InNamespace);
	Content.UtcMs = DreamJournalWriterLocal::NowUtcMs();
	return dream::storage::SerializeLock(Content);
}

void DreamJournalScan::RecordWalFiles(dream::journal::FileOps& InOps, const FString& InDir, TMap<FString, int64>& OutFiles)
{
	OutFiles.Reset();
	std::vector<std::string> Names;
	const std::string DirUtf8 = FDreamWin32FileOps::ToUtf8Path(InDir);
	InOps.List(DirUtf8, Names);
	for (const std::string& Name : Names)
	{
		uint64 Generation = 0;
		if (!dream::journal::ParseGenerationFileName(Name, Generation))
		{
			continue;
		}
		std::string Bytes;
		const bool bRead = InOps.ReadAll(dream::journal::JoinPath(DirUtf8, Name), Bytes);
		OutFiles.Add(DreamText::Utf8ToFString(Name), bRead ? static_cast<int64>(Bytes.size()) : -1);
	}
}

void DreamJournalScan::ReadOrphan(dream::journal::FileOps& ReadOps, dream::journal::FileOps& DiskOps, const FString& InRoot,
	const FString& InNamespace, bool bAll, FDreamOrphanNamespace& Out)
{
	using namespace DreamJournalWriterLocal;
	const FString NsDir = InRoot / TEXT("journal") / InNamespace;
	RecordWalFiles(ReadOps, NsDir, Out.WalFiles);
	const dream::journal::RecoveredNamespace NS = dream::journal::ReadNamespace(ReadOps, FDreamWin32FileOps::ToUtf8Path(NsDir), /*bMayModify*/ false);
	// (review r2) A generation skipped as unreadable means what was read is older than the namespace: kept as it is, not
	// listed this session, never merged or deleted.
	if (NS.bNewerFormat || NS.bNewerMinor || (NS.bFound && NS.bUnreadable) || NS.bSkippedUnreadable)
	{
		Out.Key.Reset();
		return;
	}
	Out.Key = InRoot + TEXT("|") + InNamespace;
	Out.Root = InRoot;
	Out.Namespace = InNamespace;
	Out.bClean = !NS.bFound || NS.bClean;
	Out.EndUtc = NS.LastRecordUtcMs > 0 ? FromUtcMs(NS.LastRecordUtcMs) : FDateTime();
	const dream::storage::NamespaceName Parsed = dream::storage::ParseNamespace(DreamText::ToUtf8(InNamespace));
	Out.Pid = Parsed.Kind == dream::storage::NamespaceKind::Secondary ? Parsed.Pid : NS.Header.InstancePid;

	TMap<FString, FString> DisplayByUuid;
	for (const dream::journal::RecoveredBuffer& RB : NS.Buffers)
	{
		DisplayByUuid.Add(DreamText::Utf8ToFString(RB.Open.Uuid), DreamText::Utf8ToFString(RB.Open.Display));
		if (RB.bClosed || !RB.bHasText || RB.Open.Uuid == "repl-input")
		{
			continue;
		}
		FDreamRecoveredBuffer Buffer;
		if (!FGuid::Parse(DreamText::Utf8ToFString(RB.Open.Uuid), Buffer.Id))
		{
			continue;
		}
		Buffer.Kind = KindFromName(RB.Open.Kind);
		Buffer.DisplayName = DreamText::Utf8ToFString(RB.Open.Display);
		Buffer.Path = DreamLegacyMigration::RemapLegacyPath(DreamText::Utf8ToFString(RB.Open.Path));   // (rename)
		Buffer.RecoveredText = DreamText::Utf8ToFString(RB.TextUtf8);
		Buffer.Format.Enc = EncodingFromName(RB.Open.Encoding);
		Buffer.Format.bBom = RB.Open.bBom;
		Buffer.Format.Dominant = EolFromName(RB.Open.Eol);
		Buffer.Base = ToBase(RB.Open.Base);
		Buffer.LastEditUtc = Out.EndUtc;
		bool bOffer = bAll;
		if (Buffer.Kind != EDreamBufferKind::Scratch && !Buffer.Path.IsEmpty())
		{
			bool bDiffers = false;
			CompareWithDisk(DiskOps, Buffer, bDiffers);
			Buffer.bDiffersFromDisk = bDiffers;
			bOffer = bOffer || bDiffers;
		}
		else
		{
			bOffer = bOffer || !Buffer.RecoveredText.IsEmpty();
			CountLineChanges(Buffer.RecoveredText, FString(), Buffer.LinesAdded, Buffer.LinesRemoved);
		}
		if (bOffer)
		{
			Out.Buffers.Add(MoveTemp(Buffer));
		}
	}
	// An unresolved RECOVERY_PENDING is offered with its recovered text (§10.1 "listed with its recovered text").
	for (const dream::journal::RecoveredPending& Pending : NS.Pending)
	{
		FGuid Id;
		if (!FGuid::Parse(DreamText::Utf8ToFString(Pending.Info.Uuid), Id))
		{
			continue;
		}
		FDreamRecoveredBuffer* Existing = Out.Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; });
		FDreamRecoveredBuffer Fresh;
		FDreamRecoveredBuffer& Buffer = Existing ? *Existing : Fresh;
		Buffer.Id = Id;
		Buffer.Kind = KindFromName(Pending.Info.Kind);
		Buffer.Path = DreamLegacyMigration::RemapLegacyPath(DreamText::Utf8ToFString(Pending.Info.Path));   // (rename)
		if (Buffer.DisplayName.IsEmpty())
		{
			Buffer.DisplayName = DisplayByUuid.FindRef(DreamText::Utf8ToFString(Pending.Info.Uuid));
		}
		Buffer.Base = ToBase(Pending.Info.Base);
		Buffer.Format.Dominant = EolFromName(Pending.Info.Eol);
		Buffer.RecoveredText = DreamText::Utf8ToFString(Pending.TextUtf8);
		Buffer.LastEditUtc = Out.EndUtc;
		Buffer.bFromEarlierStart = true;
		if (!Buffer.Path.IsEmpty() && Buffer.Kind != EDreamBufferKind::Scratch)
		{
			bool bDiffers = true;
			CompareWithDisk(DiskOps, Buffer, bDiffers);
			Buffer.bDiffersFromDisk = bDiffers;
		}
		else
		{
			CountLineChanges(Buffer.RecoveredText, FString(), Buffer.LinesAdded, Buffer.LinesRemoved);
		}
		if (!Existing)
		{
			Out.Buffers.Add(MoveTemp(Fresh));
		}
	}
}
