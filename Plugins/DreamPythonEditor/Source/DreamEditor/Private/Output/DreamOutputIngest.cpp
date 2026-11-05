// SPDX-License-Identifier: Apache-2.0
// The DreamOutput ingest thread (TDD §6.1 line splitting and thread attribution, §6.2 threads, run blocks, input echo,
// result cap, run logs, memory bounds; §6.3 forwarding and the startup ring). It runs no Python, touches no Slate and no
// UObject (§3 thread table). FDreamOutputStore's game-thread half is in DreamOutputStore.cpp.
#include "Output/DreamOutputStoreImpl.h"

#include "CoreGlobals.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputDevice.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

// TEL-03, §6.2 "Budgets": the DreamOutput counters on Insights' counters channel, next to the run group's Dream.Pump/* and
// Dream.Run/* counters (Diagnostics/DreamTrace.h). Dream.Output/DrainMs is declared with the drain (DreamOutputStore.cpp).
TRACE_DECLARE_INT_COUNTER(DreamOutputQueueBytes, TEXT("Dream.Output/QueueBytes"));
TRACE_DECLARE_INT_COUNTER(DreamOutputLinesPerSecond, TEXT("Dream.Output/LinesPerSecond"));
TRACE_DECLARE_INT_COUNTER(DreamOutputViewBacklog, TEXT("Dream.Output/ViewBacklogLines"));
TRACE_DECLARE_INT_COUNTER(DreamOutputEchoSuppressed, TEXT("Dream.Output/EchoSuppressedLines"));
TRACE_DECLARE_INT_COUNTER(DreamOutputDropped, TEXT("Dream.Output/DroppedLines"));

namespace DreamIngestPrivate
{
	std::string_view View(const FDreamRawRecord& Record)
	{
		return std::string_view(reinterpret_cast<const char*>(Record.Text.GetData()), static_cast<size_t>(Record.Text.Num()));
	}

	FString ToFString(std::string_view Utf8)
	{
		return DreamText::Utf8ToFString(Utf8);
	}

	ELogVerbosity::Type VerbosityOf(EDreamStream Stream)
	{
		switch (Stream)
		{
		case EDreamStream::Error:   return ELogVerbosity::Error;
		case EDreamStream::Warning: return ELogVerbosity::Warning;
		default:                   return ELogVerbosity::Log;
		}
	}

	/** Writer streams keep a partial line per key across writes (§6.1); every other write is whole lines. */
	bool IsWriterStream(EDreamStream Stream)
	{
		return Stream == EDreamStream::Stdout || Stream == EDreamStream::Stderr;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// FDreamIngest

FDreamIngest::FDreamIngest(FDreamOutputStore::FImpl& InOwner)
	: Owner(InOwner)
	, Batch(MakeUnique<FDreamViewBatch>())
{
	TagNames.Add(0, FString());
	Owner.Forwarder.SetRunLogPathProvider([this](uint32 RunNumber) { return Owner.RunLogs.GetPath(RunNumber); });
}

FDreamIngest::~FDreamIngest() = default;

FDreamIngest::FRunState* FDreamIngest::FindRun(uint32 RunNumber)
{
	return RunNumber != 0 ? Runs.Find(RunNumber) : nullptr;
}

FDateTime FDreamIngest::ToUtc(double Seconds) const
{
	return Owner.SessionStartUtc + FTimespan::FromSeconds(Seconds - Owner.SessionStartSeconds);
}

void FDreamIngest::Process(FDreamRawRecord& Record)
{
	switch (Record.Kind)
	{
	case EDreamRawKind::Text:
		ProcessText(Record);
		break;
	case EDreamRawKind::EchoLine:
		ProcessEcho(Record);
		break;
	case EDreamRawKind::BindRunId:
		Bindings.Add(Record.RunId, Record.RunNumber);
		if (CaptureRunId == Record.RunId)
		{
			CaptureRunNumber = Record.RunNumber;
		}
		break;
	case EDreamRawKind::RunHeader:
		ProcessHeader(Record);
		break;
	case EDreamRawKind::RunFooter:
		ProcessFooter(Record);
		break;
	case EDreamRawKind::CaptureBegin:
	{
		CaptureRunId = Record.RunId;
		CaptureRunNumber = Bindings.FindRef(Record.RunId);
		AliveAtCapture.Reset();
		FString List = DreamIngestPrivate::ToFString(DreamIngestPrivate::View(Record));
		TArray<FString> Ids;
		List.ParseIntoArray(Ids, TEXT(","));
		for (const FString& Id : Ids)
		{
			AliveAtCapture.Add(FCString::Strtoui64(*Id, nullptr, 10));
		}
		break;
	}
	case EDreamRawKind::CaptureEnd:
		if (CaptureRunId == Record.RunId)
		{
			CaptureRunId = 0;
			CaptureRunNumber = 0;
			AliveAtCapture.Reset();
		}
		break;
	case EDreamRawKind::FlushPartials:
		FlushRunPartials(Bindings.FindRef(Record.RunId), /*bBeforeEcho*/ true, /*bGameThreadOnly*/ true);
		break;
	case EDreamRawKind::Clear:
	{
		FDreamViewItem Item;
		Item.Kind = FDreamViewItem::EKind::Clear;
		QueueItem(MoveTemp(Item));
		break;
	}
	case EDreamRawKind::StartupDone:
	{
		bStartupDone = true;
		FDreamViewItem Item;
		Item.Kind = FDreamViewItem::EKind::StartupProblems;
		Item.Startup = StartupProblems;
		QueueItem(MoveTemp(Item));
		break;
	}
	}
}

void FDreamIngest::Attribute(const FDreamRawRecord& Record, uint32& OutRunNumber, uint16& OutFlags)
{
	OutRunNumber = 0;
	OutFlags = 0;
	auto EndedRun = [this](uint32 RunNumber)
	{
		const FRunState* State = FindRun(RunNumber);
		return State == nullptr || State->bEnded;
	};

	if (Record.NativeThreadId == GGameThreadId)
	{
		// §6.1: a write from the game thread is tagged with the current run; a stale or unknown run ID gets run 0 and
		// Background (§6.2 run blocks, T-OUT-013).
		if (Record.RunId != 0)
		{
			if (const uint32* RunNumber = Bindings.Find(Record.RunId))
			{
				OutRunNumber = *RunNumber;
				OutFlags = EndedRun(*RunNumber) ? dream::OLF_Background : 0;
			}
			else
			{
				OutFlags = dream::OLF_Background;
			}
		}
		else
		{
			OutRunNumber = CaptureRunId != 0 ? CaptureRunNumber : 0;
		}
		return;
	}

	// Worker threads (D24): the origin run decides, not the writer the thread happens to hold.
	uint32 Origin = 0;
	if (Owner.ThreadTable.FindOrigin(Record.NativeThreadId, Origin))
	{
		OutRunNumber = Origin;
		if (Origin == 0 ? CaptureRunId != 0 : EndedRun(Origin))
		{
			OutFlags = dream::OLF_Background;
		}
		return;
	}
	if (CaptureRunId != 0)
	{
		if (!AliveAtCapture.Contains(Record.NativeThreadId))
		{
			Owner.ThreadTable.SetOrigin(Record.NativeThreadId, CaptureRunNumber);   // started during this run
			OutRunNumber = CaptureRunNumber;
		}
		else
		{
			Owner.ThreadTable.SetOrigin(Record.NativeThreadId, 0);                  // alive before the run
			OutFlags = dream::OLF_Background;
		}
		return;
	}
	// Idle and unknown: a kept writer names its run; anything else is outside runs.
	if (Record.RunId != 0)
	{
		const uint32* RunNumber = Bindings.Find(Record.RunId);
		OutRunNumber = RunNumber ? *RunNumber : 0;
		OutFlags = dream::OLF_Background;
	}
}

FDreamIngest::FTag& FDreamIngest::FindOrAddTag(uint32 NativeThreadId)
{
	if (FTag* Found = Tags.Find(NativeThreadId))
	{
		return *Found;
	}
	if (Tags.Num() >= 4 * FDreamThreadTable::MaxEntries)
	{
		Tags.Reset();                 // old tags stay valid in the ring with their names
	}
	FTag& Added = Tags.Add(NativeThreadId);
	Added.Tag = NextTag++;
	return Added;
}

void FDreamIngest::ObserveThread(const FDreamRawRecord& Record)
{
	const uint32 NativeThreadId = Record.NativeThreadId;
	if (NativeThreadId == GGameThreadId || NativeThreadId == 0)
	{
		return;                       // the game thread is attributed by run ID, not by the table (§6.1)
	}
	FTag& Seen = FindOrAddTag(NativeThreadId);
	const uint32 Generation = Owner.ThreadTable.ObserveWrite(NativeThreadId, Record.ThreadState, Seen.LabelStamp);
	if (Generation != 0)
	{
		if (Seen.Generation != 0 && Seen.Generation != Generation)
		{
			// §6.1: a new (native ID, PyThreadState*) pair under a native ID the OS reused: a new badge tag, so the lines of
			// the thread that had the ID keep its name. Its origin run was forgotten by the table, so Attribute decides
			// afresh from the capture's alive list.
			Seen.Tag = NextTag++;
			Seen.NameVersion = MAX_uint32;
		}
		Seen.Generation = Generation;
	}
	Seen.LabelStamp = Owner.ThreadTable.GetLabelStamp();
}

uint32 FDreamIngest::TagForThread(uint32 NativeThreadId)
{
	if (NativeThreadId == GGameThreadId || NativeThreadId == 0)
	{
		return 0;
	}
	FTag* Found = &FindOrAddTag(NativeThreadId);
	uint32 NameVersion = 0;
	FString Name = Owner.ThreadTable.GetName(NativeThreadId, NameVersion);
	if (NameVersion != Found->NameVersion)
	{
		Found->NameVersion = NameVersion;
		if (Name.IsEmpty())
		{
			Name = FString::Printf(TEXT("thread %u"), NativeThreadId);
		}
		TagNames.Add(Found->Tag, Name);
		FDreamViewItem Item;
		Item.Kind = FDreamViewItem::EKind::ThreadName;
		Item.Number = Found->Tag;
		Item.Str = MoveTemp(Name);
		QueueItem(MoveTemp(Item));
	}
	return Found->Tag;
}

void FDreamIngest::ProcessText(FDreamRawRecord& Record)
{
	uint32 RunNumber = 0;
	uint16 Flags = 0;
	if (Record.Origin == EDreamOutputOrigin::Dream)
	{
		RunNumber = Record.RunNumber;
	}
	else
	{
		ObserveThread(Record);
		Attribute(Record, RunNumber, Flags);
	}
	Flags |= Record.Flags;

	if (Record.Origin == EDreamOutputOrigin::LogPython)
	{
		ProcessDevice(Record, RunNumber, Flags);
		return;
	}

	const dream::LineType Type = ToLineType(Record.Stream);
	std::vector<dream::SplitLine> Pieces;
	if (Record.Origin == EDreamOutputOrigin::Python && DreamIngestPrivate::IsWriterStream(Record.Stream))
	{
		const FSplitKey Key{Record.NativeThreadId, RunNumber, static_cast<uint8>(Record.Stream)};
		dream::LineSplitter& Splitter = Splitters.FindOrAdd(Key);
		Splitter.Feed(DreamIngestPrivate::View(Record), Pieces);
		EmitSplit(RunNumber, Type, Flags, Record.NativeThreadId, Record.Time, Pieces, Record.Origin);
		UpdateProvisional(RunNumber, Key, Splitter);
		return;
	}

	// The shims, _dream.write of the other streams and Dream's own lines: each write is whole lines.
	dream::LineSplitter Whole;
	Whole.Feed(DreamIngestPrivate::View(Record), Pieces);
	Whole.Flush(Pieces);
	if (Type == dream::LineType::Result)
	{
		EmitResultPieces(RunNumber, Flags, Record.NativeThreadId, Record.Time, Pieces, Record.Origin);
	}
	else
	{
		EmitSplit(RunNumber, Type, Flags, Record.NativeThreadId, Record.Time, Pieces, Record.Origin);
	}
}

void FDreamIngest::ProcessDevice(FDreamRawRecord& Record, uint32 RunNumber, uint16 Flags)
{
	const std::string_view Text = DreamIngestPrivate::View(Record);
	// §6.3 startup ring: the session's first LogPython lines before the first OnPythonInitialized (init_unreal.py) feed
	// the doctor; Warning and Error lines show in the collapsed "Editor startup" block, not as loose lines.
	if (!bStartupDone && RunNumber == 0 && StartupLines < FDreamOutputDevice::StartupRingCapacity)
	{
		++StartupLines;
		FDreamStartupLine Line;
		Line.Verbosity = DreamIngestPrivate::VerbosityOf(Record.Stream);
		Line.Text = DreamIngestPrivate::ToFString(Text);
		Line.Time = Record.Time;
		if (Line.Verbosity != ELogVerbosity::Log)
		{
			StartupProblems.Add(Line);
		}
		FScopeLock Guard(&Owner.DeviceLock);
		if (Owner.Device != nullptr)
		{
			Owner.Device->AppendStartupLine(MoveTemp(Line));
		}
		return;
	}
	if (RunNumber == 0 && (Flags & dream::OLF_Background) == 0 && !Owner.bCaptureOutsideRuns.load(std::memory_order_relaxed))
	{
		return;                   // "Capture LogPython outside runs" is off
	}
	if (FRunState* State = FindRun(RunNumber))
	{
		++State->BypassLines;     // §6.3 bypass counter
	}
	std::vector<dream::SplitLine> Pieces;
	dream::LineSplitter Whole;
	Whole.Feed(Text, Pieces);
	Whole.Flush(Pieces);
	EmitSplit(RunNumber, ToLineType(Record.Stream), Flags, Record.NativeThreadId, Record.Time, Pieces, Record.Origin);
}

void FDreamIngest::ProcessEcho(FDreamRawRecord& Record)
{
	const uint32* RunNumber = Bindings.Find(Record.RunId);
	if (RunNumber == nullptr)
	{
		return;                   // only C++ makes echo lines, always for a bound run (§6.2 input echo)
	}
	const std::string_view Text = DreamIngestPrivate::View(Record);
	EmitLine(*RunNumber, dream::LineType::Echo, Record.Flags, Record.NativeThreadId, Record.Time, std::string(Text),
		EDreamOutputOrigin::Dream, Record.EchoLine);
}

void FDreamIngest::ProcessHeader(FDreamRawRecord& Record)
{
	const uint32 RunNumber = Record.RunNumber;
	FRunState& State = Runs.FindOrAdd(RunNumber);
	State = FRunState();
	State.bOpen = true;
	State.StartSeconds = Record.Time;
	State.bReplPrompts = Record.Block.IsValid() && Record.Block->bReplPrompts;
	NewestRun = FMath::Max(NewestRun, RunNumber);

	// Bindings and run states are kept for the 500-record history (RUN-12); later lines of older runs get run 0.
	if (Runs.Num() > 600)
	{
		for (auto It = Runs.CreateIterator(); It; ++It)
		{
			if (It->Value.bEnded && It->Key + 500 < NewestRun)
			{
				It.RemoveCurrent();
			}
		}
		for (auto It = Bindings.CreateIterator(); It; ++It)
		{
			if (It->Value + 500 < NewestRun)
			{
				It.RemoveCurrent();
			}
		}
	}

	const FDreamRunBlockInfo Empty;
	const FDreamRunBlockInfo& Block = Record.Block.IsValid() ? *Record.Block : Empty;
	Owner.RunLogs.BeginRun(RunNumber, Block, Record.Time);

	FDreamViewItem Item;
	Item.Info.RunNumber = RunNumber;
	Item.Info.Type = dream::LineType::RunHeader;
	Item.Info.TimeMs = static_cast<uint32>((Record.Time - Owner.SessionStartSeconds) * 1000.0);
	QueueItem(MoveTemp(Item));

	if (!Block.LogSummary.IsEmpty())
	{
		FDreamForwardLine Summary;
		Summary.Category = FDreamLogForwarder::LogDreamName();
		Summary.Verbosity = ELogVerbosity::Log;
		Summary.Text = Block.LogSummary;
		Summary.RunNumber = RunNumber;
		Summary.Type = dream::LineType::RunHeader;
		ForwardLines.Add(MoveTemp(Summary));
	}
}

void FDreamIngest::ProcessFooter(FDreamRawRecord& Record)
{
	const uint32 RunNumber = Record.RunNumber;
	// "A partial line is committed by a newline, at run end" and a held CR ends the line (§6.1).
	FlushRunPartials(RunNumber, /*bBeforeEcho*/ false, /*bGameThreadOnly*/ false);

	// The run's held EchoSplit text goes to the log alone, after its last lines (§6.3 rule 1).
	Owner.Forwarder.Forward(ForwardLines);
	ForwardLines.Reset();
	Owner.Forwarder.FlushRun(RunNumber);

	FRunState* State = FindRun(RunNumber);
	if (State != nullptr && State->BypassLines > 0)
	{
		FDreamViewItem Bypass;
		Bypass.Kind = FDreamViewItem::EKind::Bypass;
		Bypass.Number = RunNumber;
		Bypass.Count = State->BypassLines;
		QueueItem(MoveTemp(Bypass));
	}
	for (auto It = Partials.CreateIterator(); It; ++It)
	{
		if (It->Key.RunNumber == RunNumber)
		{
			It.RemoveCurrent();
		}
	}
	if (ProvisionalSent.Remove(RunNumber) > 0)
	{
		FDreamViewItem Clear;
		Clear.Kind = FDreamViewItem::EKind::Provisional;
		Clear.Number = RunNumber;
		QueueItem(MoveTemp(Clear));
	}

	FDreamViewItem Item;
	Item.Info.RunNumber = RunNumber;
	Item.Info.Type = dream::LineType::RunFooter;
	Item.Info.TimeMs = static_cast<uint32>((Record.Time - Owner.SessionStartSeconds) * 1000.0);
	QueueItem(MoveTemp(Item));

	const FDreamRunBlockInfo Empty;
	const FDreamRunBlockInfo& Block = Record.Block.IsValid() ? *Record.Block : Empty;
	if (!Block.LogSummary.IsEmpty())
	{
		FDreamForwardLine Summary;
		Summary.Category = FDreamLogForwarder::LogDreamName();
		Summary.Verbosity = Block.SummaryVerbosity;
		Summary.Text = Block.LogSummary;
		Summary.RunNumber = RunNumber;
		Summary.Type = dream::LineType::RunFooter;
		ForwardLines.Add(MoveTemp(Summary));
	}
	Owner.RunLogs.EndRun(RunNumber, Block);

	if (State != nullptr)
	{
		State->bOpen = false;
		State->bEnded = true;
	}
	if (CaptureRunNumber == RunNumber && CaptureRunId != 0)
	{
		CaptureRunId = 0;
		CaptureRunNumber = 0;
		AliveAtCapture.Reset();
	}
}

void FDreamIngest::FlushRunPartials(uint32 RunNumber, bool bBeforeEcho, bool bGameThreadOnly)
{
	TArray<FSplitKey> Done;
	for (TPair<FSplitKey, dream::LineSplitter>& Pair : Splitters)
	{
		const FSplitKey& Key = Pair.Key;
		if (Key.RunNumber != RunNumber || (bGameThreadOnly && Key.NativeThreadId != GGameThreadId))
		{
			continue;
		}
		std::vector<dream::SplitLine> Pieces;
		Pair.Value.Flush(Pieces, bBeforeEcho);
		EmitSplit(RunNumber, ToLineType(static_cast<EDreamStream>(Key.Stream)), 0, Key.NativeThreadId,
			FPlatformTime::Seconds(), Pieces, EDreamOutputOrigin::Python);
		UpdateProvisional(RunNumber, Key, Pair.Value);
		if (!bBeforeEcho)
		{
			Done.Add(Key);
		}
	}
	for (const FSplitKey& Key : Done)
	{
		Splitters.Remove(Key);
	}
}

void FDreamIngest::EmitSplit(uint32 RunNumber, dream::LineType Type, uint16 Flags, uint32 NativeThreadId, double Time,
	std::vector<dream::SplitLine>& Pieces, EDreamOutputOrigin Origin)
{
	for (dream::SplitLine& Piece : Pieces)
	{
		EmitLine(RunNumber, Type, static_cast<uint16>(Flags | Piece.Flags), NativeThreadId, Time, MoveTemp(Piece.Text), Origin);
	}
}

void FDreamIngest::EmitResultPieces(uint32 RunNumber, uint16 Flags, uint32 NativeThreadId, double Time,
	std::vector<dream::SplitLine>& Pieces, EDreamOutputOrigin Origin)
{
	// D8, §6.4 result lines: the first 10,000 characters are the line (flag Truncated), the rest a blob of at most 16 MB.
	// A repr over 64 KB arrives as Continued pieces of the same write; they belong to the blob, not to new lines.
	// One write is one repr (autoprint): every line after its first is OLF_ResultCont, drawn and logged without the
	// "# Result: " decoration (§6.4).
	size_t Index = 0;
	while (Index < Pieces.size())
	{
		FString Full = DreamIngestPrivate::ToFString(Pieces[Index].Text);
		uint16 LineFlags = static_cast<uint16>(Flags | (Pieces[Index].Flags & ~dream::OLF_Continued));
		if (Index > 0)
		{
			LineFlags |= dream::OLF_ResultCont;
		}
		size_t Next = Index + 1;
		int64 BlobBytes = 0;
		FString Blob;
		while (Next < Pieces.size() && (Pieces[Next].Flags & dream::OLF_Continued) != 0)
		{
			if (BlobBytes < BlobCapBytes)
			{
				FString More = DreamIngestPrivate::ToFString(Pieces[Next].Text);
				BlobBytes += More.Len() * sizeof(TCHAR);
				Blob += More;
			}
			++Next;
		}
		if (Full.Len() > ResultCapChars || !Blob.IsEmpty())
		{
			int32 Cut = FMath::Min(Full.Len(), ResultCapChars);
			if (Cut > 0 && Cut < Full.Len() && StringConv::IsHighSurrogate(static_cast<uint32>(Full[Cut - 1])))
			{
				--Cut;                // never split a surrogate pair
			}
			FString Rest = Full.Mid(Cut) + Blob;
			if (Rest.Len() * static_cast<int64>(sizeof(TCHAR)) > BlobCapBytes)
			{
				Rest.LeftInline(static_cast<int32>(BlobCapBytes / sizeof(TCHAR)));
			}
			Full.LeftInline(Cut);
			LineFlags |= dream::OLF_Truncated;

			FDreamViewItem Item;
			Item.Info.RunNumber = RunNumber;
			Item.Info.Type = dream::LineType::Result;
			Item.Info.Flags = LineFlags;
			Item.Info.ThreadTag = TagForThread(NativeThreadId);
			Item.Info.TimeMs = static_cast<uint32>((Time - Owner.SessionStartSeconds) * 1000.0);
			Item.Text = DreamText::ToUtf8(Full);
			Item.Str = MoveTemp(Rest);
			if (dream::ScanLinks(Item.Text, Item.Links))
			{
				Item.Info.Flags |= dream::OLF_HasLinks;
			}
			QueueLine(MoveTemp(Item), Origin, Time);
		}
		else
		{
			EmitLine(RunNumber, dream::LineType::Result, LineFlags, NativeThreadId, Time, MoveTemp(Pieces[Index].Text), Origin);
		}
		Index = Next;
	}
}

void FDreamIngest::EmitLine(uint32 RunNumber, dream::LineType Type, uint16 Flags, uint32 NativeThreadId, double Time,
	std::string&& Text, EDreamOutputOrigin Origin, int32 EchoLine)
{
	FDreamViewItem Item;
	Item.Info.RunNumber = RunNumber;
	Item.Info.Type = Type;
	Item.Info.Flags = Flags;
	Item.Info.ThreadTag = TagForThread(NativeThreadId);
	Item.Info.TimeMs = static_cast<uint32>(FMath::Max(0.0, Time - Owner.SessionStartSeconds) * 1000.0);
	Item.Info.EchoLine = EchoLine;
	Item.Text = MoveTemp(Text);
	if (Type != dream::LineType::Echo && dream::ScanLinks(Item.Text, Item.Links))
	{
		Item.Info.Flags |= dream::OLF_HasLinks;
	}
	QueueLine(MoveTemp(Item), Origin, Time);
}

void FDreamIngest::QueueLine(FDreamViewItem&& Item, EDreamOutputOrigin Origin, double Time)
{
	const dream::LineInfo& Info = Item.Info;
	++RateWindowLines;            // TEL-03 lines/s
	Owner.CounterLinesTotal.fetch_add(1, std::memory_order_relaxed);

	// §6.3: forward on this thread, so Saved/Logs stays current while the game thread is stuck in a native call.
	FName Category;
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	if (FDreamLogForwarder::MapLine(Info.Type, Info.Flags, Category, Verbosity))
	{
		if (Origin == EDreamOutputOrigin::Dream && Info.Type != dream::LineType::Error)
		{
			// Dream info, warning and notice lines go to LogDream (§6.3 table); a compile-error block from the run service
			// is a traceback and stays on LogPython at Error, as stock logs it (T-OUT-018).
			Category = FDreamLogForwarder::LogDreamName();
		}
		FDreamForwardLine Line;
		Line.Category = Category;
		Line.Verbosity = Verbosity;
		Line.Text = DreamIngestPrivate::ToFString(Item.Text);
		Line.RunNumber = Info.RunNumber;
		Line.Type = Info.Type;
		Line.Flags = Info.Flags;
		ForwardLines.Add(MoveTemp(Line));
	}

	// OUT-08: the file matches the view, echo lines included.
	if (Info.RunNumber != 0)
	{
		FDreamRunLogLine LogLine;
		LogLine.Type = Info.Type;
		LogLine.Flags = Info.Flags;
		LogLine.EchoLine = Info.EchoLine;
		LogLine.Utc = ToUtc(Time);
		LogLine.ThreadName = TagNames.FindRef(Info.ThreadTag);
		LogLine.Text = Item.Text;
		Owner.RunLogs.AddLine(Info.RunNumber, LogLine, Time);
	}

	QueueItem(MoveTemp(Item));
}

void FDreamIngest::QueueItem(FDreamViewItem&& Item)
{
	if (Item.Kind == FDreamViewItem::EKind::Line)
	{
		const dream::LineType Type = Item.Info.Type;
		const bool bBlockLine = Type == dream::LineType::RunHeader || Type == dream::LineType::RunFooter;
		const int64 Lines = Owner.ViewPendingLines.load(std::memory_order_relaxed) + Batch->Lines;
		const int64 Bytes = Owner.ViewPendingBytes.load(std::memory_order_relaxed) + Batch->Bytes;
		if (!bViewOverCap && (Lines >= ViewCapLines || Bytes >= ViewCapBytes))
		{
			// §6.2 memory bounds: stop feeding the view, keep echoing and logging, force a run log.
			bViewOverCap = true;
			ForceOpenRunLogs();
		}
		if (bViewOverCap && !bBlockLine)
		{
			if (FRunState* State = FindRun(Item.Info.RunNumber))
			{
				++State->ViewDropped;
			}
			Owner.CounterViewDropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		++Batch->Lines;
		Batch->Bytes += static_cast<int64>(Item.Text.size() + sizeof(FDreamViewItem));
	}
	Batch->Items.Add(MoveTemp(Item));
}

void FDreamIngest::ForceOpenRunLogs()
{
	for (const TPair<uint32, FRunState>& Pair : Runs)
	{
		if (Pair.Value.bOpen)
		{
			Owner.RunLogs.Force(Pair.Key);
		}
	}
}

void FDreamIngest::UpdateProvisional(uint32 RunNumber, const FSplitKey& Key, const dream::LineSplitter& Splitter)
{
	if (RunNumber == 0)
	{
		return;
	}
	// §6.1: the open partial text is shown as a provisional last line of its block (a \r redraw replaces it). Each
	// (thread, run, stream) keeps its own partial, so a full stdout line never erases a stderr progress bar (OUT-12); the
	// run's provisional row shows the most recently changed one that is still open.
	FString Own = Splitter.HasPartial() ? DreamIngestPrivate::ToFString(Splitter.PartialText()) : FString();
	if (Own.Len() > 1000)
	{
		Own.LeftInline(1000);
	}
	if (Own.IsEmpty())
	{
		Partials.Remove(Key);
	}
	else
	{
		FPartialText& Entry = Partials.FindOrAdd(Key);
		if (!Entry.Text.Equals(Own, ESearchCase::CaseSensitive))
		{
			Entry.Text = MoveTemp(Own);
			Entry.Order = ++PartialOrder;
		}
	}
	FString Text;
	uint64 Newest = 0;
	for (const TPair<FSplitKey, FPartialText>& Pair : Partials)
	{
		if (Pair.Key.RunNumber == RunNumber && Pair.Value.Order > Newest)
		{
			Newest = Pair.Value.Order;
			Text = Pair.Value.Text;
		}
	}
	FString* Sent = ProvisionalSent.Find(RunNumber);
	if ((Sent == nullptr && Text.IsEmpty()) || (Sent != nullptr && Sent->Equals(Text, ESearchCase::CaseSensitive)))
	{
		return;
	}
	if (Text.IsEmpty())
	{
		ProvisionalSent.Remove(RunNumber);
	}
	else
	{
		ProvisionalSent.Add(RunNumber, Text);
	}
	FDreamViewItem Item;
	Item.Kind = FDreamViewItem::EKind::Provisional;
	Item.Number = RunNumber;
	Item.Str = MoveTemp(Text);
	QueueItem(MoveTemp(Item));
}

void FDreamIngest::Tick(double Now)
{
	{
		FScopeLock Guard(&Owner.SettingsLock);
		if (Owner.bRunLogRootChanged)
		{
			Owner.bRunLogRootChanged = false;
			Owner.RunLogs.SetRoot(Owner.PendingRunLogRoot);
		}
	}
	const int32 Cap = Owner.EchoCap.load(std::memory_order_relaxed);
	if (Cap != AppliedEchoCap)
	{
		AppliedEchoCap = Cap;
		Owner.Forwarder.SetEchoCap(Cap);
	}
	Owner.Forwarder.Tick(Now);

	// The view caught up: report what it missed, per run, with the run log that has every line (§6.2).
	if (bViewOverCap
		&& Owner.ViewPendingLines.load(std::memory_order_relaxed) < ViewCapLines / 2
		&& Owner.ViewPendingBytes.load(std::memory_order_relaxed) < ViewCapBytes / 2)
	{
		bViewOverCap = false;
		for (TPair<uint32, FRunState>& Pair : Runs)
		{
			if (Pair.Value.ViewDropped == 0)
			{
				continue;
			}
			const int32 Count = Pair.Value.ViewDropped;
			Pair.Value.ViewDropped = 0;
			FDreamViewItem Dropped;
			Dropped.Kind = FDreamViewItem::EKind::Dropped;
			Dropped.Number = Pair.Key;
			Dropped.Count = Count;
			QueueItem(MoveTemp(Dropped));
			const FString Path = Owner.RunLogs.GetPath(Pair.Key);
			const FString Text = FString::Printf(TEXT("%d lines not shown (the view could not keep up); full log: %s"),
				Count, Path.IsEmpty() ? TEXT("the editor log") : *Path);
			EmitLine(Pair.Key, dream::LineType::Warning, 0, GGameThreadId, Now, DreamText::ToUtf8(Text), EDreamOutputOrigin::Dream);
		}
	}

	// MPSC hard cap: dropped writes are reported in the current run (§6.2 memory bounds).
	const int64 Dropped = Owner.DroppedRecords.load(std::memory_order_relaxed);
	if (Dropped != LastDroppedRecords)
	{
		const int64 Count = Dropped - LastDroppedRecords;
		LastDroppedRecords = Dropped;
		FDreamViewItem Item;
		Item.Kind = FDreamViewItem::EKind::Dropped;
		Item.Number = CaptureRunNumber;
		Item.Count = static_cast<int32>(FMath::Min<int64>(Count, MAX_int32));
		QueueItem(MoveTemp(Item));
		const FString Text = FString::Printf(TEXT("%lld writes dropped: output arrived faster than Dream Python Editor could take it (64 MB queue)."), Count);
		EmitLine(CaptureRunNumber, dream::LineType::Warning, 0, GGameThreadId, Now, DreamText::ToUtf8(Text), EDreamOutputOrigin::Dream);
	}
}

void FDreamIngest::FlushBatch(double Now)
{
	if (!ForwardLines.IsEmpty())
	{
		Owner.Forwarder.Forward(ForwardLines);
		ForwardLines.Reset();
	}
	if (Owner.Forwarder.IsSuppressing())
	{
		ForceOpenRunLogs();       // §6.3 rule 5: suppression forces a run log, so the summary names a real file
	}
	Owner.RunLogs.Pump(Now);

	TArray<TPair<uint32, FString>> Created;
	TArray<TPair<uint32, FString>> Errors;
	Owner.RunLogs.TakeEvents(Created, Errors);
	for (TPair<uint32, FString>& Pair : Created)
	{
		FDreamViewItem Item;
		Item.Kind = FDreamViewItem::EKind::RunLogPath;
		Item.Number = Pair.Key;
		Item.Str = MoveTemp(Pair.Value);
		QueueItem(MoveTemp(Item));
	}
	for (TPair<uint32, FString>& Pair : Errors)
	{
		FDreamViewItem Item;
		Item.Kind = FDreamViewItem::EKind::RunLogError;
		Item.Number = Pair.Key;
		Item.Str = MoveTemp(Pair.Value);
		QueueItem(MoveTemp(Item));
	}

	if (!Batch->Items.IsEmpty())
	{
		Owner.ViewPendingLines.fetch_add(Batch->Lines, std::memory_order_relaxed);
		Owner.ViewPendingBytes.fetch_add(Batch->Bytes, std::memory_order_relaxed);
		Owner.ViewQueue.Enqueue(MoveTemp(Batch));
		Batch = MakeUnique<FDreamViewBatch>();
	}
	PublishCounters(Now);
}

void FDreamIngest::PublishCounters(double Now)
{
	// TEL-03, §6.2 "Budgets": queue bytes, lines/s, view-queue backlog, echo-suppressed and dropped lines. Lines/s is the
	// count of the last full second; the other values are read now.
	if (RateWindowStart == 0.0)
	{
		RateWindowStart = Now;
	}
	const double Elapsed = Now - RateWindowStart;
	if (Elapsed >= 1.0)
	{
		const int32 Rate = static_cast<int32>(FMath::Min<double>(static_cast<double>(RateWindowLines) / Elapsed, MAX_int32));
		Owner.CounterLinesPerSecond.store(Rate, std::memory_order_relaxed);
		RateWindowLines = 0;
		RateWindowStart = Now;
		TRACE_COUNTER_SET(DreamOutputLinesPerSecond, Rate);
	}
	const int64 Suppressed = Owner.Forwarder.GetSuppressedTotal();
	const int64 Dropped = Owner.DroppedRecords.load(std::memory_order_relaxed) + Owner.CounterViewDropped.load(std::memory_order_relaxed);
	Owner.CounterEchoSuppressed.store(Suppressed, std::memory_order_relaxed);
	Owner.CounterEchoCap.store(Owner.Forwarder.GetEchoCap(), std::memory_order_relaxed);
	TRACE_COUNTER_SET(DreamOutputQueueBytes, Owner.QueuedBytes.load(std::memory_order_relaxed));
	TRACE_COUNTER_SET(DreamOutputViewBacklog, Owner.ViewPendingLines.load(std::memory_order_relaxed));
	TRACE_COUNTER_SET(DreamOutputEchoSuppressed, Suppressed);
	TRACE_COUNTER_SET(DreamOutputDropped, Dropped);
}

void FDreamIngest::FlushEverything(bool bEmergency)
{
	TArray<uint32> Open;
	for (const TPair<uint32, FRunState>& Pair : Runs)
	{
		if (Pair.Value.bOpen)
		{
			Open.Add(Pair.Key);
		}
	}
	for (const uint32 RunNumber : Open)
	{
		FlushRunPartials(RunNumber, /*bBeforeEcho*/ false, /*bGameThreadOnly*/ false);
	}
	if (bEmergency)
	{
		Owner.Forwarder.EmergencyFlush(ForwardLines);    // §6.3 crash flush: ignore the cap
		ForwardLines.Reset();
	}
	const double Now = FPlatformTime::Seconds();
	FlushBatch(Now);
	Owner.RunLogs.WaitForWrites(bEmergency ? 0.09 : 2.0);
}

// ---------------------------------------------------------------------------------------------------------------------
// FDreamOutputStore::FImpl: the FRunnable body

uint32 FDreamOutputStore::FImpl::Run()
{
	auto DequeueOne = [this](FDreamRawRecord& Record)
	{
		if (!InQueue.Dequeue(Record))
		{
			return false;
		}
		QueuedBytes.fetch_sub(static_cast<int64>(Record.Text.Num()) + static_cast<int64>(sizeof(FDreamRawRecord)), std::memory_order_relaxed);
		return true;
	};

	while (true)
	{
		const bool bStop = bStopRequested.load(std::memory_order_acquire);
		const bool bEmergency = bEmergencyRequested.load(std::memory_order_acquire);
		if (!bStop && !bEmergency && InQueue.IsEmpty())
		{
			// seq_cst: the store must be visible before the queue is read, or a producer that enqueues in between sees
			// false and skips Trigger (a store-load race, OUT-10); the release order alone allows that on x86.
			bIngestWaiting.store(true, std::memory_order_seq_cst);
			if (InQueue.IsEmpty())
			{
				WakeEvent->Wait(100);         // also the periodic tick: echo-cap windows, the 30 s run-log rule
			}
			bIngestWaiting.store(false, std::memory_order_release);
		}

		// Collect for up to 5 ms after a wake, so one batch reaches the view per window (§6.2).
		const double Start = FPlatformTime::Seconds();
		FDreamRawRecord Record;
		while (true)
		{
			int32 Processed = 0;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Dream.Output.Ingest);      // §13.1 timing scope (the cpu channel)
				while (Processed < 8192 && DequeueOne(Record))
				{
					Ingest->Process(Record);
					Record = FDreamRawRecord();
					++Processed;
				}
			}
			const double Elapsed = FPlatformTime::Seconds() - Start;
			if (Elapsed >= FDreamIngest::CollectSeconds || Processed >= 8192
				|| bStopRequested.load(std::memory_order_relaxed) || bEmergencyRequested.load(std::memory_order_relaxed))
			{
				break;
			}
			// seq_cst: the store must be visible before the queue is read, or a producer that enqueues in between sees
			// false and skips Trigger (a store-load race, OUT-10); the release order alone allows that on x86.
			bIngestWaiting.store(true, std::memory_order_seq_cst);
			if (InQueue.IsEmpty())
			{
				const uint32 WaitMs = static_cast<uint32>(FMath::Max(1.0, (FDreamIngest::CollectSeconds - Elapsed) * 1000.0));
				WakeEvent->Wait(WaitMs);
			}
			bIngestWaiting.store(false, std::memory_order_release);
		}

		const double Now = FPlatformTime::Seconds();
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Dream.Output.Ingest);
			Ingest->Tick(Now);
			Ingest->FlushBatch(Now);
		}

		if (bEmergencyRequested.exchange(false, std::memory_order_acq_rel))
		{
			while (DequeueOne(Record))
			{
				Ingest->Process(Record);
				Record = FDreamRawRecord();
			}
			Ingest->FlushEverything(/*bEmergency*/ true);
			EmergencyDoneEvent->Trigger();
		}
		if (bStopRequested.load(std::memory_order_acquire) && InQueue.IsEmpty())
		{
			Ingest->FlushEverything(/*bEmergency*/ false);
			break;
		}
	}
	return 0;
}

void FDreamOutputStore::FImpl::Stop()
{
	bStopRequested.store(true, std::memory_order_release);
	WakeEvent->Trigger();
}
