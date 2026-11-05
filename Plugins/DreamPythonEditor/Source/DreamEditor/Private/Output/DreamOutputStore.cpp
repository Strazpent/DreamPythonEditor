// SPDX-License-Identifier: Apache-2.0
// FDreamOutputStore: producers, run blocks, the game-thread drain into dream::OutputRing, Clear and the view queries
// (TDD §6.2, §6.4 Clear, D19, D22, OUT-01..OUT-09, OUT-12). The ingest thread is in DreamOutputIngest.cpp.
#include "Output/DreamOutputStore.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputDevice.h"
#include "Output/DreamOutputStoreImpl.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Python/DreamPython.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamProjectSettings.h"
#include "Settings/DreamUserSettings.h"

// TEL-03, §6.2 "Budgets": the game-thread drain cost; the other Dream.Output/* counters are in DreamOutputIngest.cpp.
TRACE_DECLARE_FLOAT_COUNTER(DreamOutputDrainMs, TEXT("Dream.Output/DrainMs"));

namespace DreamStorePrivate
{
	void SetText(FDreamRawRecord& Record, FStringView Text)
	{
		const FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
		Record.Text.Append(reinterpret_cast<const UTF8CHAR*>(Utf8.Get()), Utf8.Length());
	}

	const FString& PluginVersion()
	{
		static FString Version;
		if (Version.IsEmpty())
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor"));
			Version = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString(TEXT("unknown"));
		}
		return Version;
	}

	FString NamespaceKey(EDreamNamespaceMode Mode)
	{
		switch (Mode)
		{
		case EDreamNamespaceMode::Isolated: return TEXT("isolated");
		case EDreamNamespaceMode::Agent:    return TEXT("agent");
		default:                           return TEXT("session");
		}
	}

	FString NamespaceName(EDreamNamespaceMode Mode)
	{
		switch (Mode)
		{
		case EDreamNamespaceMode::Isolated: return TEXT("Isolated");
		case EDreamNamespaceMode::Agent:    return TEXT("Agent");
		default:                           return TEXT("Session");
		}
	}

	bool IsPromptRoute(EDreamRunRoute Route)
	{
		return Route == EDreamRunRoute::Repl || Route == EDreamRunRoute::ConsoleExecutor;
	}

	/** "Untitled-3 · lines 12–20" for buffer routes, the file for Run File, "REPL" / "console" for prompts. */
	FString SourceText(const FDreamRunRecord& Rec)
	{
		if (Rec.Route == EDreamRunRoute::Repl)
		{
			return TEXT("REPL");
		}
		if (Rec.Route == EDreamRunRoute::ConsoleExecutor)
		{
			return TEXT("console");
		}
		FString Source = !Rec.TabTitle.IsEmpty() ? Rec.TabTitle : FPaths::GetCleanFilename(Rec.FilePath);
		if (Rec.Route == EDreamRunRoute::Selection || Rec.Route == EDreamRunRoute::Line || Rec.Route == EDreamRunRoute::Cell)
		{
			Source += Rec.FirstLine == Rec.LastLine
				? FString::Printf(TEXT(" · line %d"), Rec.FirstLine)
				: FString::Printf(TEXT(" · lines %d–%d"), Rec.FirstLine, Rec.LastLine);
		}
		return Source;
	}

	TSharedPtr<const FDreamRunBlockInfo, ESPMode::ThreadSafe> MakeHeaderInfo(uint32 RunNumber)
	{
		TSharedRef<FDreamRunBlockInfo, ESPMode::ThreadSafe> Info = MakeShared<FDreamRunBlockInfo, ESPMode::ThreadSafe>();
		const UDreamUserSettings* Settings = UDreamUserSettings::Get();
		Info->bAlwaysWriteRunLogs = Settings == nullptr || Settings->bAlwaysWriteRunLogs;
		const FDreamRunRecord* Rec = FDreamEditorModule::IsAvailable()
			? FDreamEditorModule::Get().GetRunService().FindRecord(static_cast<int32>(RunNumber)) : nullptr;
		if (Rec == nullptr)
		{
			Info->RunLogHeader = FString::Printf(TEXT("# run #%u engine=%s plugin=%s"), RunNumber,
				*FEngineVersion::Current().ToString(), *PluginVersion());
			Info->LogSummary = FString::Printf(TEXT("Run #%u"), RunNumber);
			return Info;
		}
		Info->RouteKey = DreamRunText::RouteKey(Rec->Route);
		Info->bReplPrompts = IsPromptRoute(Rec->Route);
		const FString Source = SourceText(*Rec);
		Info->RunLogHeader = FString::Printf(TEXT("# run #%u route=%s source=\"%s\" file=\"%s\" namespace=%s start=%s engine=%s plugin=%s"),
			RunNumber, *Info->RouteKey, *Source, *Rec->Filename, *NamespaceKey(Rec->Namespace),
			*Rec->StartUtc.ToIso8601(), *FEngineVersion::Current().ToString(), *PluginVersion());
		Info->LogSummary = FString::Printf(TEXT("Run #%u · %s · %s"), RunNumber, *Source, *NamespaceName(Rec->Namespace));
		return Info;
	}

	TSharedPtr<const FDreamRunBlockInfo, ESPMode::ThreadSafe> MakeFooterInfo(uint32 RunNumber)
	{
		TSharedRef<FDreamRunBlockInfo, ESPMode::ThreadSafe> Info = MakeShared<FDreamRunBlockInfo, ESPMode::ThreadSafe>();
		const FDreamRunRecord* Rec = FDreamEditorModule::IsAvailable()
			? FDreamEditorModule::Get().GetRunService().FindRecord(static_cast<int32>(RunNumber)) : nullptr;
		if (Rec == nullptr)
		{
			Info->OutcomeKey = TEXT("lost");
			Info->LogSummary = FString::Printf(TEXT("Run #%u ended"), RunNumber);
			return Info;
		}
		Info->RouteKey = DreamRunText::RouteKey(Rec->Route);
		Info->OutcomeKey = DreamRunText::OutcomeName(Rec->Outcome).ToString();
		// The same duration as the view's footer (DreamOutputRows::FooterText): from entry into _run, or from acceptance
		// for a run that never entered (not executed).
		const double Begin = Rec->EnterSeconds > 0.0 ? Rec->EnterSeconds : Rec->StartSeconds;
		Info->DurationSeconds = Rec->EndSeconds > Begin ? Rec->EndSeconds - Begin : 0.0;
		FString Detail = Rec->Detail;
		if (Rec->ExitCode.IsSet())
		{
			Detail = FString::Printf(TEXT("exited (code %d)"), Rec->ExitCode.GetValue());
		}
		Info->LogSummary = FString::Printf(TEXT("Run #%u %s%s%s · %.2f s"), RunNumber, *Info->OutcomeKey,
			Detail.IsEmpty() ? TEXT("") : TEXT(" "), *Detail, Info->DurationSeconds);
		const bool bBad = Rec->Outcome == EDreamRunOutcome::Error || Rec->Outcome == EDreamRunOutcome::NotExecuted
			|| Rec->Outcome == EDreamRunOutcome::Lost || Rec->bInternalError;
		Info->SummaryVerbosity = bBad ? ELogVerbosity::Warning : ELogVerbosity::Log;
		return Info;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// FImpl: construction, the game-thread half

FDreamOutputStore::FImpl::FImpl(FDreamOutputStore& InOwner)
	: Owner(InOwner)
{
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false);
	EmergencyDoneEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false);
	SessionStartSeconds = FPlatformTime::Seconds();
	SessionStartUtc = FDateTime::UtcNow();
	ThreadNames.Add(FString());          // tag 0: the game thread
	Ring.SetEvictionListener([this](const dream::EvictionInfo& Eviction) { OnEvicted(Eviction); });
	Ingest = MakeUnique<FDreamIngest>(*this);
}

FDreamOutputStore::FImpl::~FImpl()
{
	FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
	FPlatformProcess::ReturnSynchEventToPool(EmergencyDoneEvent);
}

void FDreamOutputStore::FImpl::Wake()
{
	// Trigger only when the ingest thread waits, so a busy producer pays no syscall per write (§6.2 producer budget).
	if (bIngestWaiting.exchange(false, std::memory_order_acq_rel))
	{
		WakeEvent->Trigger();
	}
}

void FDreamOutputStore::FImpl::PullSettings()
{
	if (EchoCapOverride >= 0)
	{
		EchoCap.store(EchoCapOverride, std::memory_order_relaxed);
	}
	else if (const UDreamProjectSettings* ProjectSettings = UDreamProjectSettings::Get())
	{
		EchoCap.store(FMath::Max(0, ProjectSettings->EchoCapLinesPerSecond), std::memory_order_relaxed);
	}
	if (const UDreamUserSettings* UserSettings = UDreamUserSettings::Get())
	{
		bCaptureOutsideRuns.store(UserSettings->bCaptureLogPythonOutsideRuns, std::memory_order_relaxed);
		if (UserSettings->RunLogCategories != PublishedExtraCategories)
		{
			PublishedExtraCategories = UserSettings->RunLogCategories;
			FScopeLock Guard(&DeviceLock);
			if (Device != nullptr)
			{
				Device->SetExtraRunCategories(PublishedExtraCategories);
			}
		}
	}
	if (!bStartupDoneSent && Python != nullptr && Python->GetGeneration() > 0)
	{
		bStartupDoneSent = true;
		FDreamRawRecord Record;
		Record.Kind = EDreamRawKind::StartupDone;
		Record.Time = FPlatformTime::Seconds();
		Owner.Push(MoveTemp(Record));
	}
}

void FDreamOutputStore::FImpl::OnEvicted(const dream::EvictionInfo& Eviction)
{
	// §6.2 caps: the view draws "▲ n earlier lines dropped (runs #a–#b)" from these.
	EvictedLines += Eviction.LastEvicted - Eviction.FirstEvicted + 1;
	for (uint32 RunNumber : Eviction.RunNumbers)
	{
		if (RunNumber != 0)
		{
			EvictedRunNumbers.AddUnique(RunNumber);
		}
	}
	EvictedRunNumbers.Sort();
	bEvictedThisDrain = true;
	DropBlobsBefore(Eviction.LastEvicted + 1);
}

void FDreamOutputStore::FImpl::DropBlobsBefore(uint64 Seq)
{
	int32 Removed = 0;
	while (Removed < BlobOrder.Num() && BlobOrder[Removed] < Seq)
	{
		if (const FString* Blob = Blobs.Find(BlobOrder[Removed]))
		{
			BlobBytes -= Blob->Len() * sizeof(TCHAR);
			Blobs.Remove(BlobOrder[Removed]);
		}
		++Removed;
	}
	BlobOrder.RemoveAt(0, Removed, EAllowShrinking::No);
}

void FDreamOutputStore::FImpl::AppendLine(FDreamViewItem& Item, FDreamAppendInfo& Info, bool& bAppended)
{
	const uint64 Seq = Ring.Append(Item.Info, Item.Text, {}, Item.Links);
	if (!bAppended)
	{
		Info.FirstSeq = Seq;
		bAppended = true;
	}
	const uint32 RunNumber = Item.Info.RunNumber;
	if (Item.Info.Type == dream::LineType::RunHeader)
	{
		FDreamRunSpan& Span = Spans.FindOrAdd(RunNumber);
		Span.HeaderSeq = Seq;
		Span.FooterSeq = 0;
		OpenRuns.AddUnique(RunNumber);
	}
	else if (Item.Info.Type == dream::LineType::RunFooter)
	{
		FDreamRunSpan& Span = Spans.FindOrAdd(RunNumber);
		Span.FooterSeq = Seq;
		OpenRuns.Remove(RunNumber);
	}
	else if (RunNumber != 0)
	{
		FDreamRunSpan& Span = Spans.FindOrAdd(RunNumber);
		++Span.LineCount;
		Span.Bytes += static_cast<int64>(Item.Text.size());
	}

	if (!Item.Str.IsEmpty() && Item.Info.Type == dream::LineType::Result)
	{
		// §6.2 memory bounds: blobs <= 64 MB in total, the oldest go first.
		BlobBytes += Item.Str.Len() * sizeof(TCHAR);
		Blobs.Add(Seq, MoveTemp(Item.Str));
		BlobOrder.Add(Seq);
		while (BlobBytes > BlobTotalCapBytes && BlobOrder.Num() > 1)
		{
			DropBlobsBefore(BlobOrder[0] + 1);
		}
	}
}

void FDreamOutputStore::FImpl::ApplyClear(FDreamAppendInfo& Info)
{
	// §6.4 Clear: ClearBefore(EndSeq) keeping the active run's header, so its later output still has a home. The ring
	// drops only a prefix, so the open runs' headers are appended again (the view renders them from the record).
	Ring.ClearBefore(Ring.EndSeq());
	Blobs.Reset();
	BlobOrder.Reset();
	BlobBytes = 0;
	EvictedLines = 0;
	EvictedRunNumbers.Reset();
	for (const uint32 RunNumber : OpenRuns)
	{
		dream::LineInfo HeaderInfo;
		HeaderInfo.RunNumber = RunNumber;
		HeaderInfo.Type = dream::LineType::RunHeader;
		HeaderInfo.TimeMs = static_cast<uint32>((FPlatformTime::Seconds() - SessionStartSeconds) * 1000.0);
		const uint64 Seq = Ring.Append(HeaderInfo, std::string_view(), {}, {});
		FDreamRunSpan& Span = Spans.FindOrAdd(RunNumber);
		Span.HeaderSeq = Seq;
		Span.LineCount = 0;
		Span.Bytes = 0;
	}
	for (auto It = Spans.CreateIterator(); It; ++It)
	{
		if (!OpenRuns.Contains(It->Key))
		{
			It.RemoveCurrent();
		}
	}
	Info.bCleared = true;
}

void FDreamOutputStore::FImpl::ApplyItem(FDreamViewItem& Item, FDreamAppendInfo& Info, bool& bAppended)
{
	switch (Item.Kind)
	{
	case FDreamViewItem::EKind::Line:
		AppendLine(Item, Info, bAppended);
		break;
	case FDreamViewItem::EKind::Clear:
		ApplyClear(Info);
		bAppended = false;
		break;
	case FDreamViewItem::EKind::RunLogPath:
		Spans.FindOrAdd(Item.Number).RunLogPath = MoveTemp(Item.Str);
		break;
	case FDreamViewItem::EKind::RunLogError:
		Spans.FindOrAdd(Item.Number).RunLogError = MoveTemp(Item.Str);
		break;
	case FDreamViewItem::EKind::ThreadName:
		if (ThreadNames.Num() <= static_cast<int32>(Item.Number))
		{
			ThreadNames.SetNum(Item.Number + 1);
		}
		ThreadNames[Item.Number] = MoveTemp(Item.Str);
		break;
	case FDreamViewItem::EKind::Provisional:
		if (Item.Str.IsEmpty())
		{
			Provisional.Remove(Item.Number);
		}
		else
		{
			Provisional.Add(Item.Number, MoveTemp(Item.Str));
		}
		break;
	case FDreamViewItem::EKind::Dropped:
		if (Item.Number != 0)
		{
			Spans.FindOrAdd(Item.Number).DroppedLines += Item.Count;
		}
		break;
	case FDreamViewItem::EKind::Bypass:
		if (Item.Number != 0)
		{
			Spans.FindOrAdd(Item.Number).BypassLines = Item.Count;
		}
		break;
	case FDreamViewItem::EKind::StartupProblems:
		StartupProblems = MoveTemp(Item.Startup);
		break;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// FDreamOutputStore

FDreamOutputStore::FDreamOutputStore()
	: Impl(MakeUnique<FImpl>(*this))
{
}

FDreamOutputStore::~FDreamOutputStore()
{
	Shutdown();
}

void FDreamOutputStore::Start(const FString& RunLogRoot)
{
	check(IsInGameThread());
	if (Impl->bStarted)
	{
		SetRunLogRoot(RunLogRoot);
		return;
	}
	Impl->bStarted = true;
	SetRunLogRoot(RunLogRoot);
	if (const UDreamUserSettings* Settings = UDreamUserSettings::Get())
	{
		Impl->Visibility.bEcho = Settings->bEchoInput;
	}
	if (FDreamEditorModule::IsAvailable())
	{
		Impl->Python = &FDreamEditorModule::Get().GetPython();
	}
	Impl->PullSettings();

	// A29: the thread starts here, at OnFEngineLoopInitComplete; records pushed since StartupModule wait in the queue.
	Impl->Thread = FRunnableThread::Create(Impl.Get(), TEXT("DreamOutput"), 0, TPri_BelowNormal);

	// §6.2: Drain every idle frame. During a run the core ticker does not tick; the pending call drains before each pump.
	DREAM_TIMER_EXEMPT(paint-only: moves processed output into the ring and calls no Python)
	Impl->DrainTicker = FTSTicker::GetCoreTicker().AddTicker(TEXT("DreamOutputDrain"), 0.f, [this](float)
	{
		Drain(0.002);
		return true;
	});
	Impl->SystemErrorHandle = FCoreDelegates::OnHandleSystemError.AddRaw(this, &FDreamOutputStore::EmergencyFlush);
}

void FDreamOutputStore::SetRunLogRoot(const FString& RunLogRoot)
{
	FScopeLock Guard(&Impl->SettingsLock);
	Impl->PendingRunLogRoot = RunLogRoot;
	Impl->bRunLogRootChanged = true;
	Impl->Wake();
}

void FDreamOutputStore::Shutdown()
{
	if (!Impl.IsValid())
	{
		return;
	}
	if (Impl->DrainTicker.IsValid())
	{
		FTSTicker::RemoveTicker(Impl->DrainTicker);
		Impl->DrainTicker.Reset();
	}
	if (Impl->SystemErrorHandle.IsValid())
	{
		FCoreDelegates::OnHandleSystemError.Remove(Impl->SystemErrorHandle);
		Impl->SystemErrorHandle.Reset();
	}
	if (Impl->Thread != nullptr)
	{
		Impl->bStopRequested.store(true);
		Impl->WakeEvent->Trigger();
		Impl->Thread->WaitForCompletion();
		delete Impl->Thread;
		Impl->Thread = nullptr;
	}
	Impl->RunLogs.Shutdown(5.0);
	Impl->Python = nullptr;
}

void FDreamOutputStore::EmergencyFlush()
{
	// §6.3 crash flush: the ingest thread echoes everything queued ignoring the cap and flushes the run logs; wait 100 ms.
	if (Impl->Thread == nullptr)
	{
		return;
	}
	Impl->bEmergencyRequested.store(true);
	Impl->WakeEvent->Trigger();
	Impl->EmergencyDoneEvent->Wait(100);
}

// ---- Producers ----

void FDreamOutputStore::Push(FDreamRawRecord&& Record)
{
	const int64 Bytes = static_cast<int64>(Record.Text.Num()) + static_cast<int64>(sizeof(FDreamRawRecord));
	if (Record.Kind == EDreamRawKind::Text && Impl->QueuedBytes.load(std::memory_order_relaxed) + Bytes > FImpl::QueueCapBytes)
	{
		// §6.2 memory bounds: above 64 MB new writes are dropped and counted; producers never block. Control records
		// (headers, footers, bindings, echo, Clear) always go through.
		Impl->DroppedRecords.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	Impl->QueuedBytes.fetch_add(Bytes, std::memory_order_relaxed);
	Impl->InQueue.Enqueue(MoveTemp(Record));
	Impl->Wake();
}

void FDreamOutputStore::PushText(FDreamRunId RunId, EDreamStream Stream, EDreamOutputOrigin Origin, const UTF8CHAR* Utf8, int32 Bytes,
	uint64 ThreadState)
{
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::Text;
	Record.RunId = RunId;
	Record.Stream = Stream;
	Record.Origin = Origin;
	Record.Flags = Origin == EDreamOutputOrigin::UnrealLog ? dream::OLF_FromUnrealLog : 0;
	Record.NativeThreadId = FPlatformTLS::GetCurrentThreadId();
	Record.ThreadState = ThreadState;
	Record.Time = FPlatformTime::Seconds();
	if (Utf8 != nullptr && Bytes > 0)
	{
		Record.Text.Append(Utf8, Bytes);
	}
	Push(MoveTemp(Record));
}

void FDreamOutputStore::PushDreamLine(uint32 RunNumber, EDreamStream Stream, const FString& Text)
{
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::Text;
	Record.RunNumber = RunNumber;
	Record.Stream = Stream;
	Record.Origin = EDreamOutputOrigin::Dream;
	Record.NativeThreadId = FPlatformTLS::GetCurrentThreadId();
	Record.Time = FPlatformTime::Seconds();
	DreamStorePrivate::SetText(Record, Text);
	Push(MoveTemp(Record));
}

void FDreamOutputStore::SetThreadLabel(uint32 NativeThreadId, const FString& Name)
{
	Impl->ThreadTable.SetName(NativeThreadId, Name);
}

// ---- Run blocks and echo (game thread) ----

void FDreamOutputStore::BeginRunBlock(uint32 RunNumber)
{
	check(IsInGameThread());
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::RunHeader;
	Record.RunNumber = RunNumber;
	Record.Origin = EDreamOutputOrigin::Dream;
	Record.NativeThreadId = FPlatformTLS::GetCurrentThreadId();
	Record.Time = FPlatformTime::Seconds();
	Record.Block = DreamStorePrivate::MakeHeaderInfo(RunNumber);
	Push(MoveTemp(Record));
}

void FDreamOutputStore::BindRunId(FDreamRunId Id, uint32 RunNumber)
{
	check(IsInGameThread());
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::BindRunId;
	Record.RunId = Id;
	Record.RunNumber = RunNumber;
	Record.Time = FPlatformTime::Seconds();
	Push(MoveTemp(Record));
}

void FDreamOutputStore::EndRunBlock(uint32 RunNumber)
{
	check(IsInGameThread());
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::RunFooter;
	Record.RunNumber = RunNumber;
	Record.Origin = EDreamOutputOrigin::Dream;
	Record.NativeThreadId = FPlatformTLS::GetCurrentThreadId();
	Record.Time = FPlatformTime::Seconds();
	Record.Block = DreamStorePrivate::MakeFooterInfo(RunNumber);
	Push(MoveTemp(Record));
}

void FDreamOutputStore::CaptureBegin(FDreamRunId Id, TConstArrayView<uint64> AliveNativeIds)
{
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::CaptureBegin;
	Record.RunId = Id;
	Record.NativeThreadId = FPlatformTLS::GetCurrentThreadId();
	Record.Time = FPlatformTime::Seconds();
	FString List;
	for (const uint64 NativeId : AliveNativeIds)
	{
		List += FString::Printf(TEXT("%llu,"), NativeId);
	}
	DreamStorePrivate::SetText(Record, List);
	Push(MoveTemp(Record));
}

void FDreamOutputStore::CaptureEnd(FDreamRunId Id)
{
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::CaptureEnd;
	Record.RunId = Id;
	Record.NativeThreadId = FPlatformTLS::GetCurrentThreadId();
	Record.Time = FPlatformTime::Seconds();
	Push(MoveTemp(Record));
}

void FDreamOutputStore::PushEcho(FDreamRunId Id, int32 BufferLine, FStringView Text, bool bFirstOfStatement, bool bReplPrompt,
	bool bPromptFirst)
{
	const uint32 ThreadId = FPlatformTLS::GetCurrentThreadId();
	const double Now = FPlatformTime::Seconds();
	if (bFirstOfStatement)
	{
		// §6.1: statement k's unterminated print commits above statement k+1's echo, flagged Partial | EchoSplit.
		FDreamRawRecord Flush;
		Flush.Kind = EDreamRawKind::FlushPartials;
		Flush.RunId = Id;
		Flush.NativeThreadId = ThreadId;
		Flush.Time = Now;
		Push(MoveTemp(Flush));
	}
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::EchoLine;
	Record.RunId = Id;
	Record.Origin = EDreamOutputOrigin::Dream;
	Record.Flags = static_cast<uint16>((bFirstOfStatement ? dream::OLF_EchoFirst : 0) | (bReplPrompt ? dream::OLF_ReplPrompt : 0)
		| (bPromptFirst ? dream::OLF_PromptFirst : 0));
	Record.NativeThreadId = ThreadId;
	Record.EchoLine = BufferLine;
	Record.Time = Now;
	DreamStorePrivate::SetText(Record, Text);
	Push(MoveTemp(Record));
}

// ---- Drain and Clear (game thread) ----

int32 FDreamOutputStore::Drain(double BudgetSeconds)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Dream.Output.Drain);      // §13.1 timing scope (the cpu channel)
	const double DrainStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		const double Ms = (FPlatformTime::Seconds() - DrainStart) * 1000.0;
		Impl->CounterDrainMicros.store(static_cast<int64>(Ms * 1000.0), std::memory_order_relaxed);
		TRACE_COUNTER_SET(DreamOutputDrainMs, Ms);
	};
	Impl->PullSettings();

	const double Deadline = DrainStart + BudgetSeconds;
	FDreamAppendInfo Info;
	bool bAppended = false;
	bool bAnyItem = false;
	int32 Moved = 0;
	Impl->bEvictedThisDrain = false;

	while (true)
	{
		if (!Impl->DrainBatch.IsValid())
		{
			if (!Impl->ViewQueue.Dequeue(Impl->DrainBatch))
			{
				break;
			}
			Impl->DrainIndex = 0;
		}
		TArray<FDreamViewItem>& Items = Impl->DrainBatch->Items;
		bool bOutOfTime = false;
		while (Impl->DrainIndex < Items.Num())
		{
			FDreamViewItem& Item = Items[Impl->DrainIndex++];
			Moved += Item.Kind == FDreamViewItem::EKind::Line ? 1 : 0;
			bAnyItem = true;
			Impl->ApplyItem(Item, Info, bAppended);
			if ((Impl->DrainIndex & 255) == 0 && FPlatformTime::Seconds() >= Deadline)
			{
				bOutOfTime = true;      // D22: unfinished work waits for the next call
				break;
			}
		}
		if (Impl->DrainIndex >= Items.Num())
		{
			Impl->ViewPendingLines.fetch_sub(Impl->DrainBatch->Lines, std::memory_order_relaxed);
			Impl->ViewPendingBytes.fetch_sub(Impl->DrainBatch->Bytes, std::memory_order_relaxed);
			Impl->DrainBatch.Reset();
		}
		if (bOutOfTime || FPlatformTime::Seconds() >= Deadline)
		{
			break;
		}
	}

	if (bAnyItem || Impl->bEvictedThisDrain)
	{
		if (!bAppended)
		{
			Info.FirstSeq = Impl->Ring.EndSeq();
		}
		Info.EndSeq = Impl->Ring.EndSeq();
		Info.bEvicted = Impl->bEvictedThisDrain;
		Info.NewFirstSeq = Impl->Ring.FirstSeq();
		LinesAppendedEvent.Broadcast(Info);      // the views invalidate once per drain (§6.2)
	}
	return Moved;
}

void FDreamOutputStore::Clear()
{
	// Toolbar, context menu, clear_output(), Dream.ClearOutput: in queue order, so a mid-run call lands at the next drain.
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::Clear;
	Record.Time = FPlatformTime::Seconds();
	Push(MoveTemp(Record));
}

// ---- Queries (game thread) ----

const dream::OutputRing& FDreamOutputStore::GetRing() const
{
	return Impl->Ring;
}

uint64 FDreamOutputStore::GetFirstSeq() const
{
	return Impl->Ring.FirstSeq();
}

uint64 FDreamOutputStore::GetEndSeq() const
{
	return Impl->Ring.EndSeq();
}

bool FDreamOutputStore::GetLine(uint64 Seq, dream::LineView& Out) const
{
	return Impl->Ring.GetLine(Seq, Out);
}

FString FDreamOutputStore::GetThreadName(uint32 ThreadTag) const
{
	return Impl->ThreadNames.IsValidIndex(static_cast<int32>(ThreadTag)) ? Impl->ThreadNames[ThreadTag] : FString();
}

FString FDreamOutputStore::GetResultBlob(uint64 Seq) const
{
	const FString* Blob = Impl->Blobs.Find(Seq);
	return Blob ? *Blob : FString();
}

bool FDreamOutputStore::GetProvisionalLine(uint32 RunNumber, FString& OutText) const
{
	if (const FString* Text = Impl->Provisional.Find(RunNumber))
	{
		OutText = *Text;
		return true;
	}
	return false;
}

const FDreamRunSpan* FDreamOutputStore::FindRunSpan(uint32 RunNumber) const
{
	return Impl->Spans.Find(RunNumber);
}

FString FDreamOutputStore::GetRunLogPath(uint32 RunNumber) const
{
	const FDreamRunSpan* Span = Impl->Spans.Find(RunNumber);
	return Span ? Span->RunLogPath : FString();
}

uint64 FDreamOutputStore::GetEvictedLineCount() const
{
	return Impl->EvictedLines;
}

TConstArrayView<uint32> FDreamOutputStore::GetEvictedRunNumbers() const
{
	return Impl->EvictedRunNumbers;
}

const FDreamOutputVisibility& FDreamOutputStore::GetVisibility() const
{
	return Impl->Visibility;
}

void FDreamOutputStore::SetVisibility(const FDreamOutputVisibility& Visibility)
{
	check(IsInGameThread());
	if (Impl->Visibility == Visibility)
	{
		return;
	}
	Impl->Visibility = Visibility;
	VisibilityChangedEvent.Broadcast();
}

FDreamLogForwarder& FDreamOutputStore::GetForwarder()
{
	return Impl->Forwarder;
}

void FDreamOutputStore::SetEchoCap(int32 LinesPerSecond)
{
	check(IsInGameThread());
	Impl->EchoCapOverride = FMath::Max(0, LinesPerSecond);
	Impl->EchoCap.store(Impl->EchoCapOverride, std::memory_order_relaxed);
	Impl->Wake();
}

void FDreamOutputStore::ClearEchoCap()
{
	check(IsInGameThread());
	Impl->EchoCapOverride = -1;
	Impl->PullSettings();
	Impl->Wake();
}

FDreamOutputCounters FDreamOutputStore::GetCounters() const
{
	const FImpl& State = *Impl;
	FDreamOutputCounters Counters;
	Counters.QueueBytes = State.QueuedBytes.load(std::memory_order_relaxed);
	Counters.LinesPerSecond = State.CounterLinesPerSecond.load(std::memory_order_relaxed);
	Counters.DrainMs = static_cast<double>(State.CounterDrainMicros.load(std::memory_order_relaxed)) / 1000.0;
	Counters.ViewBacklogLines = State.ViewPendingLines.load(std::memory_order_relaxed);
	Counters.EchoSuppressedLines = State.CounterEchoSuppressed.load(std::memory_order_relaxed);
	Counters.DroppedLines = State.DroppedRecords.load(std::memory_order_relaxed) + State.CounterViewDropped.load(std::memory_order_relaxed);
	Counters.EchoCap = State.CounterEchoCap.load(std::memory_order_relaxed);
	Counters.LinesTotal = State.CounterLinesTotal.load(std::memory_order_relaxed);
	return Counters;
}

void FDreamOutputStore::SetOutputDevice(FDreamOutputDevice* Device)
{
	FScopeLock Guard(&Impl->DeviceLock);
	Impl->Device = Device;
	if (Device != nullptr && !Impl->PublishedExtraCategories.IsEmpty() && IsInGameThread())
	{
		Device->SetExtraRunCategories(Impl->PublishedExtraCategories);
	}
}

const TArray<FDreamStartupLine>& FDreamOutputStore::GetStartupProblems() const
{
	return Impl->StartupProblems;
}

double FDreamOutputStore::GetSessionStartSeconds() const
{
	return Impl->SessionStartSeconds;
}
