// SPDX-License-Identifier: Apache-2.0
// One queue, one start point, one run at a time (TDD §5.1, §5.2, §3.6 INV-1..INV-7, RT-01, RT-14, RT-15, U7, U8).
// Game thread unless a function says otherwise. Submit never runs Python (D14(1)); every run starts from the one-shot
// FCoreDelegates::OnEndFrame handler, which is also where boot, Enable Python and Reset Session ride (choice A14).
#include "Run/DreamRunService.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Diagnostics/DreamAudit.h"
#include "Diagnostics/DreamCrashContext.h"
#include "Diagnostics/DreamTrace.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputDevice.h"
#include "Output/DreamOutputStore.h"
#include "Output/DreamRuntimeProblems.h"
#include "Persistence/DreamJournal.h"
#include "Python/DreamPython.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunFile.h"
#include "Run/DreamRunSourceText.h"   // (run) CutRunLine
#include "Run/DreamTransactions.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamTab.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectThreadContext.h"
#include "dream/Checksum.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamRunServicePrivate
{
	FDreamEditorModule& Module()
	{
		return FDreamEditorModule::Get();
	}

	bool IsPromptRoute(EDreamRunRoute Route)
	{
		return Route == EDreamRunRoute::Repl || Route == EDreamRunRoute::ConsoleExecutor;
	}

	/** The tab routes whose per-tab toggle "One undo step per run" applies on top of the route default (U3). */
	bool IsTabRoute(EDreamRunRoute Route)
	{
		switch (Route)
		{
		case EDreamRunRoute::Selection:
		case EDreamRunRoute::Line:
		case EDreamRunRoute::Cell:
		case EDreamRunRoute::Buffer:
		case EDreamRunRoute::RunFile:
			return true;
		default:
			return false;
		}
	}

	/** The runtime-Problems source key (§5.2.4): the buffer ID, the Run File path, or "repl" / "console". */
	FString SourceKey(const FDreamRunRecord& Rec)
	{
		if (Rec.BufferId.IsValid())
		{
			return DreamText::GuidToHex(Rec.BufferId);
		}
		switch (Rec.Route)
		{
		case EDreamRunRoute::Repl:
			return TEXT("repl");
		case EDreamRunRoute::ConsoleExecutor:
			return TEXT("console");
		default:
			return !Rec.FilePath.IsEmpty() ? Rec.FilePath : Rec.Filename;
		}
	}

	/** Number of lines in Text: "a\nb" and "a\nb\n" are two lines (the trailing newline opens no line that runs). */
	int32 CountLines(const FString& Text)
	{
		int32 Newlines = 0;
		for (const TCHAR Ch : Text)
		{
			Newlines += Ch == TEXT('\n') ? 1 : 0;
		}
		if (Newlines > 0 && Text.EndsWith(TEXT("\n")))
		{
			--Newlines;
		}
		return Newlines + 1;
	}

	FString Sha256Hex(const FString& Text, int64* OutUtf8Bytes = nullptr)
	{
		const std::string Utf8 = DreamText::ToUtf8(Text);
		if (OutUtf8Bytes != nullptr)
		{
			*OutUtf8Bytes = static_cast<int64>(Utf8.size());
		}
		dream::Sha256 Hasher;
		Hasher.Update(Utf8.data(), Utf8.size());
		const dream::Sha256Digest Digest = Hasher.Final();
		return DreamText::Utf8ToFString(dream::Sha256::ToHex(Digest));
	}

	/** Buffer lines First..Last (1-based) of a snapshot, cut at EndColumn on the last line (RUN-12 re-run text). */
	FString SnapshotRange(const dream::TextSnapshot& Snapshot, int32 First, int32 Last, const TOptional<int32>& EndColumn,
		const TOptional<int32>& StartColumn)   // (run) PO-06 F2: the exact selection, start column included
	{
		FString Out;
		std::u16string Scratch;
		const int32 LineCount = Snapshot.LineCount();
		for (int32 Line = FMath::Max(First, 1); Line <= Last && Line - 1 < LineCount; ++Line)
		{
			const FStringView View = DreamRunSource::CutRunLine(DreamText::ToView(Snapshot.LineView(Line - 1, Scratch)), Line,
				First, Last, StartColumn, EndColumn);   // (run)
			if (Line > First)
			{
				Out += TEXT("\n");
			}
			Out += View;
		}
		return Out;
	}

	/** Code points of a string: traceback places the caret by characters, so the block is built over code points. */
	TArray<uint32> ToCodePoints(const FString& Text)
	{
		TArray<uint32> Points;
		Points.Reserve(Text.Len());
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const uint32 Unit = static_cast<uint32>(Text[Index]);
			if (Unit >= 0xD800 && Unit <= 0xDBFF && Index + 1 < Text.Len())
			{
				const uint32 Low = static_cast<uint32>(Text[Index + 1]);
				if (Low >= 0xDC00 && Low <= 0xDFFF)
				{
					Points.Add(0x10000 + ((Unit - 0xD800) << 10) + (Low - 0xDC00));
					++Index;
					continue;
				}
			}
			Points.Add(Unit);
		}
		return Points;
	}

	/**
	 * The code-point index at a 0-based UTF-8 byte column of Points (FDreamSyntaxError offsets are UTF-8 columns; a lone
	 * surrogate counts 3 bytes, as the runner's "surrogatepass" encoding does). A column past the end adds one per byte.
	 */
	int32 Utf8ColumnToPointIndex(TConstArrayView<uint32> Points, int32 ByteColumn)
	{
		int32 Bytes = 0;
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			if (Bytes >= ByteColumn)
			{
				return Index;
			}
			const uint32 Point = Points[Index];
			Bytes += Point < 0x80 ? 1 : Point < 0x800 ? 2 : Point < 0x10000 ? 3 : 4;
		}
		return Points.Num() + FMath::Max(0, ByteColumn - Bytes);
	}

	FString FromCodePoints(TConstArrayView<uint32> Points)
	{
		FString Out;
		Out.Reserve(Points.Num());
		for (const uint32 Point : Points)
		{
			if (Point >= 0x10000)
			{
				Out.AppendChar(static_cast<TCHAR>(0xD800 + ((Point - 0x10000) >> 10)));
				Out.AppendChar(static_cast<TCHAR>(0xDC00 + ((Point - 0x10000) & 0x3FF)));
			}
			else
			{
				Out.AppendChar(static_cast<TCHAR>(Point));
			}
		}
		return Out;
	}

	/**
	 * A compile error as CPython 3.11's traceback prints it (ARCHITECTURE section 6; written from behaviour): the File
	 * line, the source line without its indentation, the caret line, and "Type: message".
	 */
	TArray<FString> FormatSyntaxErrorBlock(const FString& Filename, const FDreamSyntaxError& Error)
	{
		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("  File \"%s\", line %d"), *Filename, Error.Line));
		FString Right = Error.Text;
		while (Right.EndsWith(TEXT("\n")))
		{
			Right.LeftChopInline(1);
		}
		if (!Right.IsEmpty())
		{
			const TArray<uint32> RightPoints = ToCodePoints(Right);
			int32 Spaces = 0;
			while (Spaces < RightPoints.Num() && (RightPoints[Spaces] == ' ' || RightPoints[Spaces] == '\n' || RightPoints[Spaces] == '\f'))
			{
				++Spaces;
			}
			const TConstArrayView<uint32> Left = MakeArrayView(RightPoints).RightChop(Spaces);
			Lines.Add(TEXT("    ") + FromCodePoints(Left));
			if (Error.Offset > 0)
			{
				// The offsets are 1-based UTF-8 columns in Text (CR-06); traceback's arithmetic below is in characters.
				const int32 Offset = Utf8ColumnToPointIndex(RightPoints, Error.Offset - 1) + 1;
				int32 EndOffset = Error.EndOffset > Error.Offset ? Utf8ColumnToPointIndex(RightPoints, Error.EndOffset - 1) + 1 : Offset + 1;
				if (EndOffset <= Offset)
				{
					EndOffset = Offset + 1;
				}
				const int32 Column = Offset - 1 - Spaces;
				const int32 EndColumn = EndOffset - 1 - Spaces;
				if (Column >= 0)
				{
					FString Caret = TEXT("    ");
					for (int32 Index = 0; Index < Column; ++Index)
					{
						const uint32 Point = Index < Left.Num() ? Left[Index] : ' ';
						Caret.AppendChar(Point == '\t' || Point == '\f' || Point == '\v' ? static_cast<TCHAR>(Point) : TEXT(' '));
					}
					Caret += FString::ChrN(FMath::Max(EndColumn - Column, 1), TEXT('^'));
					Lines.Add(Caret);
				}
			}
		}
		Lines.Add(FString::Printf(TEXT("%s: %s"), *Error.TypeName, Error.Message.IsEmpty() ? TEXT("<no detail available>") : *Error.Message));
		return Lines;
	}

	/** The 0-based UTF-16 column of a SyntaxError's 1-based UTF-8 offset into its source line (Error.Text); 0 when unknown. */
	int32 SyntaxErrorColumn(const FDreamSyntaxError& Error)
	{
		if (Error.Offset <= 0 || Error.Text.IsEmpty())
		{
			return 0;
		}
		const TArray<uint32> Points = ToCodePoints(Error.Text);
		const int32 PointIndex = FMath::Min(Utf8ColumnToPointIndex(Points, Error.Offset - 1), Points.Num());
		int32 Units = 0;
		for (int32 Index = 0; Index < PointIndex; ++Index)
		{
			Units += Points[Index] >= 0x10000 ? 2 : 1;
		}
		return Units;
	}

	/** The diagnostics services: the audit record and the crash breadcrumbs (§5.2.3, §11.4, §11.8.1). */
	FDreamAudit& Audit()
	{
		return Module().GetAudit();
	}

	FDreamCrashContext& Crash()
	{
		return Module().GetCrashContext();
	}

	/** The SHA-256 of the file on disk behind a clean buffer, from its base version (§10.14); empty when unknown. */
	FString CleanFileSha256(const FGuid& BufferId)
	{
		if (!BufferId.IsValid())
		{
			return FString();
		}
		const TSharedPtr<FDreamBuffer> Buffer = Module().GetBufferManager().FindById(BufferId);
		if (!Buffer.IsValid() || !Buffer->HasPath() || Buffer->IsDirty() || !Buffer->GetBase().IsSet())
		{
			return FString();
		}
		return Buffer->GetBase()->Sha256Hex.ToLower();
	}

	void PushLines(uint32 RunNumber, EDreamStream Stream, const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);
		FDreamOutputStore& Store = Module().GetOutputStore();
		for (const FString& Line : Lines)
		{
			Store.PushDreamLine(RunNumber, Stream, Line);
		}
	}
}

struct FDreamRunService::FImpl
{
	/** The run in Starting or Running; its address is stable for the whole run (the Run window reads it). */
	TUniquePtr<FDreamRunRecord> Active;
	/** The active run's request: text, linecache text, argv and the U6 override, for prepare and entry. */
	FDreamRunRequest ActiveRequest;
	/** Finished runs, newest last, at most HistoryCapacity (RUN-12). */
	TArray<FDreamRunRecord> History;

	int32 LastRunNumber = 0;            // per editor session, never reset (U8)
	uint32 EntrySerial = 0;             // per process: one per entry into _run (§12.1.2)
	uint32 RunGeneration = 0;

	FDelegateHandle EndFrameHandle;
	bool bInEndFrame = false;
	bool bEntered = false;              // IsRunActive: from entry to finish
	std::atomic<FDreamRunId> ActiveRunId{ 0 };
	std::atomic<bool> bActiveAgentOrBridge{ false };   // STREAM_AGENT is accepted (§6.1): route Bridge or Mcp (1.1)
	bool bShutdown = false;

	// Per-run Starting state.
	bool bPrepared = false;             // runner._PREPARED holds the run: Discard when Starting is aborted
	bool bSkipSnapshot = false;         // [Run without snapshot] (§10.2.5 step 4)
	bool bSnapshotRequested = false;
	FDreamJournalTicket Ticket;
	double SnapshotRequestedAt = 0.0;
	bool bSaveStarted = false;
	bool bSaveDone = false;
	bool bSaveOk = false;
	FText SaveError;
	bool bPythonShutDuringRun = false;
	FString LastEntryRefusal;

	void ResetPerRun()
	{
		bPrepared = false;
		bSkipSnapshot = false;
		bSnapshotRequested = false;
		Ticket = FDreamJournalTicket();
		SnapshotRequestedAt = 0.0;
		bSaveStarted = false;
		bSaveDone = false;
		bSaveOk = false;
		SaveError = FText::GetEmpty();
		bPythonShutDuringRun = false;
		LastEntryRefusal.Reset();
	}

	/**
	 * Undo Last Run's run (RunNumber 0): the newest run with a recorded transaction (Recorded or Partial), skipping runs
	 * that recorded nothing (Off, NotStarted, Empty, CancelledByScript, Undone). A Cleared or Purged run ends the walk and is
	 * returned itself, so its refusal shows: the reset it saw removed every older entry (§5.9.2, §5.9.5).
	 */
	FDreamRunRecord* FindUndoCandidate()
	{
		for (int32 Index = History.Num() - 1; Index >= 0; --Index)
		{
			FDreamRunRecord& Rec = History[Index];
			if (Rec.RunNumber <= 0)
			{
				continue;
			}
			switch (Rec.Txn.Status)
			{
			case EDreamTxnStatus::Recorded:
			case EDreamTxnStatus::Partial:
			case EDreamTxnStatus::Cleared:
			case EDreamTxnStatus::Purged:
				return &Rec;
			default:
				break;
			}
		}
		return nullptr;
	}

	FDreamRunRecord* FindMutable(int32 RunNumber)
	{
		if (Active && Active->RunNumber == RunNumber)
		{
			return Active.Get();
		}
		for (int32 Index = History.Num() - 1; Index >= 0; --Index)
		{
			if (History[Index].RunNumber == RunNumber)
			{
				return &History[Index];
			}
		}
		return nullptr;
	}
};

FDreamRunService::FDreamRunService()
	: Impl(MakeUnique<FImpl>())
{
}

FDreamRunService::~FDreamRunService()
{
	if (Impl->EndFrameHandle.IsValid())
	{
		FCoreDelegates::OnEndFrame.Remove(Impl->EndFrameHandle);
	}
}

void FDreamRunService::Initialize()
{
	check(IsInGameThread());
	Impl->bShutdown = false;
}

void FDreamRunService::Shutdown()
{
	check(IsInGameThread());
	Impl->bShutdown = true;
	if (Impl->EndFrameHandle.IsValid())
	{
		FCoreDelegates::OnEndFrame.Remove(Impl->EndFrameHandle);
		Impl->EndFrameHandle.Reset();
	}
}

// ---- Requests ----------------------------------------------------------------------------------------------------

bool FDreamRunService::CanSubmit(EDreamRunRoute Route, FText& OutReason) const
{
	(void)Route;
	if (!IsInGameThread() || Impl->bShutdown || !FDreamEditorModule::IsAvailable())
	{
		OutReason = LOCTEXT("SubmitUnavailable", "Runs are not available right now.");
		return false;
	}
	if (Impl->Active)
	{
		// One run at a time (D14(4), INV-5): the UI disables run commands; this is the backstop.
		OutReason = FText::Format(LOCTEXT("RunIsActive", "Run #{0} is active."),
			FText::AsNumber(Impl->Active->RunNumber, &FNumberFormattingOptions::DefaultNoGrouping()));
		return false;
	}
	const FDreamPython& Python = DreamRunServicePrivate::Module().GetPython();
	if (!Python.IsReady() && !Python.CanQueueRuns())
	{
		// Refused without a number while Python is disabled, shut down or failed (§5.2.2, D16).
		OutReason = Python.GetDisabledReason();
		if (OutReason.IsEmpty())
		{
			OutReason = LOCTEXT("PythonUnavailable", "Python is not available.");
		}
		return false;
	}
	if (Python.IsRestrictiveMode())
	{
		OutReason = Python.GetDisabledReason();
		return false;
	}
	return true;
}

FDreamSubmitResult FDreamRunService::Submit(FDreamRunRequest&& Request)
{
	check(IsInGameThread());
	using namespace DreamRunServicePrivate;
	FDreamSubmitResult Result;
	if (!CanSubmit(Request.Route, Result.Reason))
	{
		return Result;
	}

	// The run number is assigned at acceptance, so a run that never executes is numbered too (U7, U8).
	TUniquePtr<FDreamRunRecord> Rec = MakeUnique<FDreamRunRecord>();
	const int32 RunNumber = ++Impl->LastRunNumber;
	Rec->RunNumber = RunNumber;
	Rec->Route = Request.Route;
	Rec->Badge = Request.Badge;
	Rec->BufferId = Request.BufferId;
	Rec->TabTitle = Request.TabTitle;
	Rec->FilePath = Request.FilePath;
	Rec->bFileTarget = Request.bFileTarget && Request.Route == EDreamRunRoute::Api;
	Request.bFileTarget = Rec->bFileTarget;

	// The model is LF-only (§4.1.2); a lone CR is a line break for Python's tokenizer too.
	Request.Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Request.Text.ReplaceInline(TEXT("\r"), TEXT("\n"));
	if (Request.Filename.IsEmpty())
	{
		// §5.3.2: REPL and console pseudo files carry the run number; other routes pass their compile filename.
		if (Request.Route == EDreamRunRoute::Repl)
		{
			Request.Filename = FString::Printf(TEXT("<dream:REPL#%d>"), RunNumber);
		}
		else if (Request.Route == EDreamRunRoute::ConsoleExecutor)
		{
			Request.Filename = FString::Printf(TEXT("<dream:Console#%d>"), RunNumber);
		}
		else if (!Request.FilePath.IsEmpty())
		{
			Request.Filename = Request.FilePath;
		}
		else
		{
			Request.Filename = FString::Printf(TEXT("<dream:%s#%d>"), Request.TabTitle.IsEmpty() ? TEXT("run") : *Request.TabTitle, RunNumber);
		}
	}
	if (Request.TabTitle.IsEmpty())
	{
		Rec->TabTitle = Request.Route == EDreamRunRoute::Repl ? TEXT("REPL")
			: Request.Route == EDreamRunRoute::ConsoleExecutor ? TEXT("Console")
			: !Request.FilePath.IsEmpty() ? FPaths::GetCleanFilename(Request.FilePath) : FString();
	}
	Rec->Filename = Request.Filename;
	Rec->FirstLine = FMath::Max(Request.FirstLine, 1);
	Request.FirstLine = Rec->FirstLine;
	Rec->LastLine = Rec->FirstLine + CountLines(Request.Text) - 1;
	Rec->SourceSha256 = Sha256Hex(Request.Text, &Rec->CodeBytes);
	// The audit's source block (§11.4): dirty at submit; the disk file's SHA-256 for a clean buffer or a file Run File read.
	Rec->bDirty = Request.bDirty;
	Rec->FileSha256 = !Request.FileSha256.IsEmpty() ? Request.FileSha256 : (Request.bDirty ? FString() : CleanFileSha256(Request.BufferId));

	// REPL and console requests (and a file read by the API) bring no snapshot: one is made from the text, numbered
	// from 1, so the echo, the Location row and the journal read the submitted code (§5.2.3, §6.2).
	if (Request.SourceSnapshot.Length() == 0 && !Request.Text.IsEmpty() && Request.FirstLine == 1)
	{
		const dream::TextBuffer Made(DreamText::ToU16(Request.Text));
		Request.SourceSnapshot = Made.Snapshot();
	}
	Rec->SourceSnapshot = Request.SourceSnapshot;
	Rec->SourceVersion = Request.SourceSnapshot.Version();
	Rec->SelectionEndColumn = Request.SelectionEndColumn;
	Rec->SelectionStartColumn = Request.SelectionStartColumn;   // (run) PO-06 F2
	if (!Request.BufferText.IsValid() && Request.BufferId.IsValid() && Request.SourceSnapshot.Length() > 0)
	{
		// (slice A choice) linecache gets the whole buffer, from the snapshot the text was cut from (§5.3.2).
		Request.BufferText = MakeShared<const FString>(DreamText::ToFString(Request.SourceSnapshot.Text()));
	}

	Rec->Namespace = Request.Namespace;
	Rec->bStatementMode = DreamRunText::RouteUsesStatementMode(Request.Route, Request.Namespace, Request.bFileTarget);
	Rec->bAutoPrint = Rec->bStatementMode;
	Rec->StartSeconds = FPlatformTime::Seconds();
	Rec->StartUtc = FDateTime::UtcNow();
	Rec->State = EDreamRunState::Starting;
	Rec->StartingStep = EDreamStartingStep::Queued;

	Impl->ResetPerRun();
	Impl->ActiveRequest = MoveTemp(Request);
	Impl->Active = MoveTemp(Rec);
	FDreamRunRecord& Active = *Impl->Active;

	FDreamEditorModule& M = Module();
	M.GetOutputStore().BeginRunBlock(static_cast<uint32>(RunNumber));
	M.GetRuntimeProblems().OnRunStarted(SourceKey(Active));
	// FDreamAudit::RunStarted comes at Running and RunEnded at finish (§5.2.3, §11.4); the crash state follows U2 (§11.8.1).
	Crash().OnStateChanged(EDreamRunState::Starting);
	RunStateChangedEvent.Broadcast(Active);

	ArmEndFrame();
	FDreamPython& Python = M.GetPython();
	if (!Python.IsReady())
	{
		Python.RequestStart(EDreamStartReason::RunRequest);
	}

	Result.bAccepted = true;
	Result.RunNumber = RunNumber;
	return Result;
}

FDreamSubmitResult FDreamRunService::Rerun(int32 RunNumber)
{
	return RerunInternal(RunNumber, /*bSkipSnapshot*/ false, {});
}

FDreamSubmitResult FDreamRunService::RerunWithoutSnapshot(int32 RunNumber)
{
	return RerunInternal(RunNumber, /*bSkipSnapshot*/ true, {});
}

FDreamSubmitResult FDreamRunService::RerunWithTransaction(int32 RunNumber)
{
	// RT-08: "Run in transaction" is the U6 override on a re-run of the same snapshot (§5.9.1, §5.9.4).
	return RerunInternal(RunNumber, /*bSkipSnapshot*/ false, TOptional<bool>(true));
}

bool FDreamRunService::CanRerunInTransaction(int32 RunNumber) const
{
	if (RunNumber <= 0 || (Impl->Active && Impl->Active->RunNumber == RunNumber))
	{
		return false;
	}
	for (int32 Index = Impl->History.Num() - 1; Index >= 0; --Index)
	{
		const FDreamRunRecord& Rec = Impl->History[Index];
		if (Rec.RunNumber == RunNumber)
		{
			// A run that executed without a transaction: Run File by default (RT-08), the U6 override, a toggle turned off.
			return Rec.Txn.Status == EDreamTxnStatus::Off && Rec.Outcome != EDreamRunOutcome::NotExecuted
				&& Rec.Outcome != EDreamRunOutcome::Lost && Rec.Outcome != EDreamRunOutcome::None;
		}
	}
	return false;
}

FDreamSubmitResult FDreamRunService::RerunInternal(int32 RunNumber, bool bSkipSnapshot, TOptional<bool> TransactionOverride)
{
	check(IsInGameThread());
	FDreamSubmitResult Result;
	const FDreamRunRecord* Old = nullptr;
	for (int32 Index = Impl->History.Num() - 1; Index >= 0 && Old == nullptr; --Index)
	{
		if (Impl->History[Index].RunNumber == RunNumber && RunNumber > 0)
		{
			Old = &Impl->History[Index];
		}
	}
	if (Old == nullptr)
	{
		Result.Reason = FText::Format(LOCTEXT("RerunUnknown", "Run #{0} is no longer in the run history."),
			FText::AsNumber(RunNumber, &FNumberFormattingOptions::DefaultNoGrouping()));
		return Result;
	}

	// RUN-12: the recorded snapshot, the same route options, a new number.
	FDreamRunRequest Request;
	Request.Route = Old->Route;
	Request.BufferId = Old->BufferId;
	Request.TabTitle = Old->TabTitle;
	Request.FilePath = Old->FilePath;
	Request.bFileTarget = Old->bFileTarget;
	Request.Badge = Old->Badge;
	Request.Namespace = Old->Namespace;
	Request.FirstLine = Old->FirstLine;
	Request.SelectionEndColumn = Old->SelectionEndColumn;
	Request.SelectionStartColumn = Old->SelectionStartColumn;   // (run) PO-06 F2
	Request.SourceSnapshot = Old->SourceSnapshot;
	Request.FileSha256 = Old->FileSha256;    // the replayed snapshot came from these bytes (audit code.sha256, §11.4)
	if (Old->SourceSnapshot.Length() > 0)
	{
		Request.Text = DreamRunServicePrivate::SnapshotRange(Old->SourceSnapshot, Old->FirstLine, Old->LastLine, Old->SelectionEndColumn,
			Old->SelectionStartColumn);   // (run)
	}
	else if (!Old->FilePath.IsEmpty())
	{
		// An empty recorded snapshot (an empty file): read the file again, decoded as a file tab would be (§4.13).
		FText ReadError;
		if (!DreamRunFile::ReadRunnableFile(Old->FilePath, Request.Text, Request.SourceSnapshot, ReadError, &Request.FileSha256))
		{
			Result.Reason = FText::Format(LOCTEXT("RerunNoFile", "Cannot re-run #{0}: {1}."),
				FText::AsNumber(RunNumber, &FNumberFormattingOptions::DefaultNoGrouping()), ReadError);
			return Result;
		}
	}
	if (!DreamRunServicePrivate::IsPromptRoute(Old->Route))
	{
		Request.Filename = Old->Filename;     // REPL and console runs get a new <dream:REPL#N> name at Submit
	}
	// Argv holds only the arguments after the path (RUN-14, Later): the runner itself sets sys.argv = [path, *argv] for a
	// run with Run File semantics, so a re-run passes none.
	if (TransactionOverride.IsSet())
	{
		Request.TransactionOverride = TransactionOverride;
	}
	else if (Old->Txn.Status == EDreamTxnStatus::Off)
	{
		Request.TransactionOverride = false;
	}
	Result = Submit(MoveTemp(Request));
	if (Result.bAccepted)
	{
		Impl->bSkipSnapshot = bSkipSnapshot;
		if (bSkipSnapshot && Impl->Active)
		{
			DreamRunServicePrivate::Audit().SnapshotSkipped(*Impl->Active, RunNumber);    // §10.2.5, PER-09
		}
	}
	return Result;
}

bool FDreamRunService::RequestStop(FDreamRunId Id, EDreamStopSource Source)
{
	if (!IsInGameThread())
	{
		// Only the supervisor's atomics are touched off the game thread (ARCHITECTURE section 3).
		if (Id != 0 && Id == Impl->ActiveRunId.load())
		{
			DreamRunServicePrivate::Module().GetSupervisor().RequestStop(Id, Source);
			return true;
		}
		return false;
	}
	if (!Impl->Active || Id == 0 || !Impl->Active->RunIds.Contains(Id))
	{
		return false;
	}
	return RequestStopActive(Source);
}

bool FDreamRunService::RequestStopActive(EDreamStopSource Source)
{
	if (!IsInGameThread())
	{
		const FDreamRunId Id = Impl->ActiveRunId.load();
		if (Id != 0)
		{
			DreamRunServicePrivate::Module().GetSupervisor().RequestStop(Id, Source);
		}
		return Id != 0;
	}
	FDreamRunRecord* Rec = Impl->Active.Get();
	if (Rec == nullptr)
	{
		return false;
	}
	if (!Rec->bStopRequested)
	{
		Rec->bStopRequested = true;
		Rec->StopSource = Source;
	}
	if (Rec->State == EDreamRunState::Starting)
	{
		// The Starting handler ends the run not executed at the next OnEndFrame (§5.1.2 step 5).
		ArmEndFrame();
	}
	else if (Impl->bEntered && Impl->ActiveRunId.load() != 0)
	{
		// Running: every Stop control (Run window, pre-processor, WS-07, toolbar) reaches the supervisor only here.
		FRunSupervisor& Supervisor = DreamRunServicePrivate::Module().GetSupervisor();
		const FDreamRunId Id = Impl->ActiveRunId.load();
		Supervisor.RequestStop(Id, Source);
		if (Supervisor.IsStopRequested(Id))
		{
			Rec->State = EDreamRunState::Stopping;    // not under the Off presentation, which has no Stop (§5.5.5)
			DreamRunServicePrivate::Crash().OnStateChanged(EDreamRunState::Stopping);
		}
	}
	RunStateChangedEvent.Broadcast(*Rec);
	return true;
}

void FDreamRunService::RequestInterruptCleanup()
{
	const FDreamRunId Id = Impl->ActiveRunId.load();
	if (Id != 0)
	{
		DreamRunServicePrivate::Module().GetSupervisor().RequestInterruptCleanup(Id);
	}
}

// ---- State ---------------------------------------------------------------------------------------------------------

EDreamRunState FDreamRunService::GetState() const
{
	const FDreamRunRecord* Rec = Impl->Active.Get();
	if (Rec == nullptr)
	{
		return EDreamRunState::Idle;
	}
	if (Rec->State == EDreamRunState::Running)
	{
		const FDreamRunId Id = Impl->ActiveRunId.load();
		if (Rec->bStopRequested || (Id != 0 && DreamRunServicePrivate::Module().GetSupervisor().IsStopRequested(Id)))
		{
			return EDreamRunState::Stopping;    // set by FRunSupervisor (U2)
		}
	}
	return Rec->State;
}

bool FDreamRunService::IsRunActive() const
{
	return Impl->bEntered;
}

bool FDreamRunService::IsInEndFrameStart() const
{
	return Impl->bInEndFrame;
}

uint32 FDreamRunService::GetRunGeneration() const
{
	return Impl->RunGeneration;
}

FDreamRunId FDreamRunService::GetActiveRunId() const
{
	return Impl->ActiveRunId.load();
}

bool FDreamRunService::IsActiveRunAgentOrBridge() const
{
	return Impl->bActiveAgentOrBridge.load();
}

bool FDreamRunService::ResolveTabTransaction(const FDreamBufferViewState& View, EDreamRunRoute Route)
{
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	if (Route == EDreamRunRoute::RunFile)
	{
		// SHL-05: Run File keeps its route default (off, RT-08); the toggle can only turn it off.
		const bool bDefault = Settings ? Settings->GetTransactionDefault(EDreamRunRoute::RunFile) : DreamRunText::RouteTransactionDefault(Route);
		const bool bToggle = View.bOneUndoStepPerRunSet || Settings == nullptr ? View.bOneUndoStepPerRun
			: Settings->GetTransactionDefault(EDreamRunRoute::Buffer);
		return bDefault && bToggle;
	}
	if (View.bOneUndoStepPerRunSet || Settings == nullptr)
	{
		return View.bOneUndoStepPerRun;     // set on this tab: it decides every tab run (A56)
	}
	// Untouched: seeded per route (P2-run-02), selection, line and cell runs from bTransactionSelectionAndLine.
	return Settings->GetTransactionDefault(Route == EDreamRunRoute::Api ? EDreamRunRoute::Buffer : Route);
}

TOptional<bool> FDreamRunService::TabToggleShown(const FDreamBufferViewState& View)
{
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	if (View.bOneUndoStepPerRunSet || Settings == nullptr)
	{
		return View.bOneUndoStepPerRun;
	}
	const bool bBuffer = Settings->GetTransactionDefault(EDreamRunRoute::Buffer);
	const bool bLines = Settings->GetTransactionDefault(EDreamRunRoute::Selection);
	return bBuffer == bLines ? TOptional<bool>(bBuffer) : TOptional<bool>();
}

FText FDreamRunService::TabToggleTooltip(const FDreamBufferViewState& View)
{
	const FText What = LOCTEXT("OneUndoTipBase", "Records each run of this tab as one undo step (Dream: <tab> #N).");
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	if (View.bOneUndoStepPerRunSet || Settings == nullptr)
	{
		return FText::Format(LOCTEXT("OneUndoTipSet", "{0}\nSet on this tab: it decides every run of the tab."), What);
	}
	const bool bBuffer = Settings->GetTransactionDefault(EDreamRunRoute::Buffer);
	const bool bLines = Settings->GetTransactionDefault(EDreamRunRoute::Selection);
	if (bBuffer == bLines)
	{
		return FText::Format(LOCTEXT("OneUndoTipSeed", "{0}\nNot set on this tab: it follows Editor Preferences > Plugins > Dream Python Editor > Undo."), What);
	}
	auto Does = [](bool bOn) { return bOn ? LOCTEXT("OneUndoOn", "record one undo step") : LOCTEXT("OneUndoOff", "record no undo step"); };
	return FText::Format(LOCTEXT("OneUndoTipMixed", "{0}\nNot set on this tab, and Editor Preferences differ by route: buffer runs {1}, selection, line and cell runs {2}. Click to decide for every run of this tab."),
		What, Does(bBuffer), Does(bLines));
}

const FDreamRunRecord* FDreamRunService::GetActiveRecord() const
{
	return Impl->Active.Get();
}

TConstArrayView<FDreamRunRecord> FDreamRunService::GetHistory() const
{
	return Impl->History;
}

const FDreamRunRecord* FDreamRunService::FindRecord(int32 RunNumber) const
{
	return Impl->FindMutable(RunNumber);
}

const FDreamRunRecord* FDreamRunService::FindRecordByRunId(FDreamRunId Id) const
{
	if (Id == 0)
	{
		return nullptr;
	}
	if (Impl->Active && Impl->Active->RunIds.Contains(Id))
	{
		return Impl->Active.Get();
	}
	for (int32 Index = Impl->History.Num() - 1; Index >= 0; --Index)
	{
		if (Impl->History[Index].RunIds.Contains(Id))
		{
			return &Impl->History[Index];
		}
	}
	return nullptr;
}

const FDreamRunRecord* FDreamRunService::GetLastFinished() const
{
	for (int32 Index = Impl->History.Num() - 1; Index >= 0; --Index)
	{
		if (Impl->History[Index].RunNumber > 0)
		{
			return &Impl->History[Index];
		}
	}
	return nullptr;
}

void FDreamRunService::RecordLostRun(const FDreamLostRun& Lost)
{
	check(IsInGameThread());
	// §10.7.4: a lost block and a history entry; it takes no run number of this session (choice A23: run-0 lines).
	DreamRunServicePrivate::Audit().RecordLostRun(Lost);       // run.end with outcome lost (§10.7.4, §11.4)
	FDreamRunRecord Rec;
	Rec.RunNumber = 0;
	Rec.Route = Lost.Route;
	Rec.TabTitle = Lost.TabTitle;
	Rec.StartUtc = Lost.StartUtc;
	Rec.EndUtc = Lost.PreviousSessionUtc;
	Rec.State = EDreamRunState::Idle;
	Rec.StartingStep = EDreamStartingStep::Done;
	Rec.Outcome = EDreamRunOutcome::Lost;
	Rec.Detail = FString::Printf(TEXT("run #%d of the previous session"), Lost.RunNumber);
	Impl->History.Add(MoveTemp(Rec));
	if (Impl->History.Num() > HistoryCapacity)
	{
		Impl->History.RemoveAt(0, Impl->History.Num() - HistoryCapacity);
	}

	FText Text = FText::Format(LOCTEXT("LostRunLine", "Run #{0} of the previous session ({1}, started {2}) was lost: the editor exited during the run{3}."),
		FText::AsNumber(Lost.RunNumber, &FNumberFormattingOptions::DefaultNoGrouping()),
		FText::FromString(Lost.TabTitle.IsEmpty() ? FString(DreamRunText::RouteKey(Lost.Route)) : Lost.TabTitle),
		FText::AsDateTime(Lost.StartUtc, EDateTimeStyle::Short, EDateTimeStyle::Short),
		Lost.bPaused ? LOCTEXT("LostWhilePaused", " while it was paused") : FText::GetEmpty());
	if (!Lost.RunLogPath.IsEmpty())
	{
		Text = FText::Format(LOCTEXT("LostRunLog", "{0} Run log: {1}"), Text, FText::FromString(Lost.RunLogPath));
	}
	DreamRunServicePrivate::Module().GetOutputStore().PushDreamLine(0, EDreamStream::Warning, Text.ToString());
}

// ---- Undo Last Run (§5.9.5) ----------------------------------------------------------------------------------------

bool FDreamRunService::CanUndoRun(int32 RunNumber, FText& OutReason, FDreamUndoPreview& OutPreview) const
{
	check(IsInGameThread());
	if (Impl->Active)
	{
		OutReason = LOCTEXT("UndoDuringRun", "Undo Last Run is not available while a run is active.");
		return false;
	}
	const FDreamRunRecord* Rec = RunNumber == 0 ? Impl->FindUndoCandidate() : FindRecord(RunNumber);
	if (Rec == nullptr)
	{
		OutReason = LOCTEXT("NothingToUndo", "There is no run to undo.");
		return false;
	}
	return DreamRunServicePrivate::Module().GetTransactions().CanUndoLastRun(*Rec, OutReason, OutPreview);
}

bool FDreamRunService::UndoRun(int32 RunNumber, bool bConfirmedPartial, FText& OutReason)
{
	check(IsInGameThread());
	FDreamUndoPreview Preview;
	if (!CanUndoRun(RunNumber, OutReason, Preview))
	{
		return false;
	}
	FDreamRunRecord* Rec = RunNumber == 0 ? Impl->FindUndoCandidate() : Impl->FindMutable(RunNumber);
	if (Rec == nullptr)
	{
		OutReason = LOCTEXT("NothingToUndo", "There is no run to undo.");
		return false;
	}
	const FText Number = FText::AsNumber(Rec->RunNumber, &FNumberFormattingOptions::DefaultNoGrouping());
	if (Preview.bPartial && !bConfirmedPartial)
	{
		OutReason = FText::Format(LOCTEXT("ConfirmPartial", "Run #{0} is partly undoable. Undo the recorded part?"), Number);
		return false;
	}
	if (!DreamRunServicePrivate::Module().GetTransactions().UndoLastRun(*Rec))
	{
		OutReason = FText::Format(LOCTEXT("UndoFailed", "The editor could not undo Run #{0}."), Number);
		return false;
	}
	RunStateChangedEvent.Broadcast(*Rec);
	return true;
}

// ---- Called from _dream (GIL held, game thread) ------------------------------------------------------------------------

bool FDreamRunService::ValidateStatement(FDreamRunId Id, int32 Index, FString& OutError, bool& bOutIsValueError) const
{
	const FDreamRunRecord* Rec = Impl->Active.Get();
	bOutIsValueError = false;
	if (Rec == nullptr || Id == 0 || Id != Impl->ActiveRunId.load())
	{
		OutError = FString::Printf(TEXT("run_statement(): run %llu is not the active run"), static_cast<unsigned long long>(Id));
		return false;
	}
	if (!Rec->bStatementMode || Index < 0 || Index >= Rec->Statements.Num())
	{
		OutError = FString::Printf(TEXT("run_statement(): statement index %d is outside the prepared range"), Index);
		bOutIsValueError = true;
		return false;
	}
	return true;
}

bool FDreamRunService::BeginStatement(FDreamRunId Id, int32 Index, FString& OutError, bool& bOutIsValueError)
{
	if (!ValidateStatement(Id, Index, OutError, bOutIsValueError))
	{
		return false;
	}
	FDreamRunRecord& Rec = *Impl->Active;
	Rec.StatementsStarted = Index + 1;

	// §6.2 input echo: lines max(first, last echoed + 1) .. last, read in C++ from the snapshot, each line once per run.
	const FDreamStatementRange& Range = Rec.Statements[Index];
	const dream::TextSnapshot& Snapshot = Rec.SourceSnapshot;
	const int32 SnapshotLines = Snapshot.Length() > 0 ? Snapshot.LineCount() : 0;
	const bool bPrompt = DreamRunServicePrivate::IsPromptRoute(Rec.Route);
	FDreamOutputStore& Store = DreamRunServicePrivate::Module().GetOutputStore();
	std::u16string Scratch;
	bool bFirst = true;
	for (int32 Line = FMath::Max(Range.FirstLine, Rec.LastEchoedLine + 1); Line <= Range.LastLine && Line - 1 < SnapshotLines; ++Line)
	{
		FStringView Text = DreamText::ToView(Snapshot.LineView(Line - 1, Scratch));
		// The echo never shows text that did not run: cut at the selection end and (run, PO-06 F2) its start.
		Text = DreamRunSource::CutRunLine(Text, Line, Rec.FirstLine, Rec.LastLine, Rec.SelectionStartColumn, Rec.SelectionEndColumn);
		// '>>>' marks the statement's own first line, not the first line pushed for it (§6.4).
		Store.PushEcho(Id, Line, Text, bFirst, bPrompt, bPrompt && Line == Range.FirstLine);
		bFirst = false;
	}
	Rec.LastEchoedLine = FMath::Max(Rec.LastEchoedLine, Range.LastLine);
	return true;
}

void FDreamRunService::EndStatement(FDreamRunId Id, int32 Index)
{
	if (Impl->Active && Id != 0 && Id == Impl->ActiveRunId.load() && Index >= 0)
	{
		Impl->Active->StatementsReturned = Index + 1;
	}
}

bool FDreamRunService::GetStatementCounts(FDreamRunId Id, int32& OutStarted, int32& OutReturned) const
{
	OutStarted = 0;
	OutReturned = 0;
	const FDreamRunRecord* Rec = Impl->Active.Get();
	if (Rec == nullptr || Id == 0 || Id != Impl->ActiveRunId.load())
	{
		return false;
	}
	if (Rec->bStatementMode)
	{
		OutStarted = Rec->StatementsStarted;
		OutReturned = Rec->StatementsReturned;
	}
	return true;
}

bool FDreamRunService::ReportOutcome(FDreamRunId Id, EDreamRunOutcome Outcome, const FString& Detail, const TOptional<FDreamRunLocation>& Location)
{
	FDreamRunRecord* Rec = Impl->Active.Get();
	if (Rec == nullptr || Id == 0 || Id != Impl->ActiveRunId.load())
	{
		return false;
	}
	const bool bValid = Outcome == EDreamRunOutcome::Ok || Outcome == EDreamRunOutcome::Error
		|| Outcome == EDreamRunOutcome::Cancelled || Outcome == EDreamRunOutcome::NotExecuted;
	Rec->Outcome = bValid ? Outcome : EDreamRunOutcome::Error;
	Rec->Detail = bValid ? Detail : FString(TEXT("internal: malformed outcome"));
	Rec->Location = bValid && Location.IsSet() ? Location.GetValue() : FDreamRunLocation();
	Rec->bOutcomeReported = true;

	// RUN-18: SystemExit is never silent; the header says "exited (code n)".
	static const FString ExitPrefix = TEXT("exited (code ");
	if (Rec->Detail.StartsWith(ExitPrefix))
	{
		const FString Code = Rec->Detail.Mid(ExitPrefix.Len()).LeftChop(1);
		if (Code.IsNumeric())
		{
			Rec->ExitCode = FCString::Atoi(*Code);
		}
	}
	return true;
}

int32 FDreamRunService::EnqueueApiRun(const FString& Target, TOptional<EDreamNamespaceMode> Namespace, TOptional<bool> Transaction, FText& OutError)
{
	check(IsInGameThread());
	if (Impl->bEntered || Impl->Active)
	{
		// INV-5, D14(4): one run at a time; run() raises inside a run.
		OutError = LOCTEXT("ApiRunInsideRun", "dream_editor.run() cannot queue a run while a run is active (one run at a time).");
		return 0;
	}
	FDreamEditorModule& M = DreamRunServicePrivate::Module();
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	FDreamBufferManager& Buffers = M.GetBufferManager();

	// A tab ID is exactly 32 hex digits; a shorter hex-looking target is a tab name, never an ID prefix.
	bool bLooksLikeId = Target.Len() == 32;
	for (const TCHAR Ch : Target)
	{
		bLooksLikeId = bLooksLikeId && FChar::IsHexDigit(Ch);
	}
	// §12.2: the target is a tab (ID or name) or a file. A path is a file target whether or not a tab has it open, so it
	// keeps Run File semantics either way (§5.2.1 API row); only an ID or a tab name selects a tab.
	TSharedPtr<FDreamBuffer> Buffer = bLooksLikeId ? Buffers.FindByIdHex(Target) : nullptr;
	if (!Buffer.IsValid())
	{
		Buffer = Buffers.FindByName(Target);
	}

	FDreamRunRequest Request;
	Request.Route = EDreamRunRoute::Api;
	if (Buffer.IsValid())
	{
		// A tab target runs as the buffer would: the Session, statement mode, the buffer route's transaction default.
		Request.BufferId = Buffer->GetId();
		Request.TabTitle = Buffer->GetDisplayName();
		Request.FilePath = Buffer->GetPath();
		Request.bDirty = Buffer->IsDirty();
		Request.SourceSnapshot = Buffer->Snapshot();
		Request.Text = DreamText::ToFString(Request.SourceSnapshot.Text());
		Request.FirstLine = 1;
		Request.Filename = Buffers.GetCompileFilename(*Buffer);
		Request.Namespace = Namespace.Get(EDreamNamespaceMode::Session);
		// The tab's One undo step per run decides, as for its own buffer runs (§5.9.1; seeded from the buffer-run setting).
		Request.TransactionOverride = Transaction.Get(ResolveTabTransaction(Buffer->GetViewState(), EDreamRunRoute::Buffer));
	}
	else
	{
		FString Path = FPaths::ConvertRelativePathToFull(Target);
		FPaths::NormalizeFilename(Path);
		if (!FPaths::FileExists(Path))
		{
			OutError = FText::Format(LOCTEXT("ApiRunUnknownTarget", "dream_editor.run(): no open tab or file named \"{0}\"."), FText::FromString(Target));
			return 0;
		}
		// A file target runs like Run File: Isolated, one exec, sys.path and sys.argv = [path], the Run File transaction
		// default (§5.2.1, §5.3.4); a tab open on the file runs its text and a dirty one is saved first (Starting step 4).
		FText ReadError;
		if (!DreamRunFile::MakeFileRequest(Path, EDreamRunRoute::Api, Request, ReadError))
		{
			OutError = FText::Format(LOCTEXT("ApiRunUnreadable", "dream_editor.run(): {0}."), ReadError);
			return 0;
		}
		Request.Namespace = Namespace.Get(EDreamNamespaceMode::Isolated);
		Request.TransactionOverride = Transaction.Get(Settings ? Settings->GetTransactionDefault(EDreamRunRoute::RunFile) : false);
	}

	const FDreamSubmitResult Result = Submit(MoveTemp(Request));
	if (!Result.bAccepted)
	{
		OutError = Result.Reason;
		return 0;
	}
	return Result.RunNumber;
}

void FDreamRunService::OnUserCodeWindow(FDreamRunId Id, bool bActive)
{
	if (Id != 0 && Id == Impl->ActiveRunId.load())
	{
		DreamRunServicePrivate::Module().GetTransactions().OnUserCodeWindow(bActive);
		DreamRunServicePrivate::Crash().OnUserCode(bActive);     // Dream.InUserCode (§11.8.1)
	}
}

// ---- Called by FDreamPython --------------------------------------------------------------------------------------------

void FDreamRunService::ArmEndFrame()
{
	check(IsInGameThread());
	if (Impl->bShutdown || Impl->EndFrameHandle.IsValid())
	{
		return;
	}
	// Added during the OnEndFrame broadcast it fires at the next one (the delegate list is walked as it was at the start).
	Impl->EndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(this, &FDreamRunService::HandleEndFrame);
}

void FDreamRunService::NotifySessionReset()
{
	++Impl->RunGeneration;
}

void FDreamRunService::NotifyPythonRestarted()
{
	++Impl->RunGeneration;
}

void FDreamRunService::NotifyPythonShutdownDuringRun()
{
	Impl->bPythonShutDuringRun = true;
}

// ---- The one-shot OnEndFrame handler (INV-2) --------------------------------------------------------------------------

void FDreamRunService::HandleEndFrame()
{
	check(IsInGameThread());
	FCoreDelegates::OnEndFrame.Remove(Impl->EndFrameHandle);
	Impl->EndFrameHandle.Reset();
	if (Impl->bShutdown || !FDreamEditorModule::IsAvailable())
	{
		return;
	}
	if (Impl->bEntered || Impl->bInEndFrame)
	{
		ArmEndFrame();      // something is executing: start at a later OnEndFrame
		return;
	}
	TGuardValue<bool> InEndFrame(Impl->bInEndFrame, true);

	// The RT-02 trio defers a start by one frame (§5.2.2).
	if (UE::IsSavingPackage() || IsGarbageCollecting() || FUObjectThreadContext::Get().IsRoutingPostLoad)
	{
		ArmEndFrame();
		return;
	}

	FDreamPython& Python = DreamRunServicePrivate::Module().GetPython();
	const bool bReady = Python.ProcessEndFrame();
	FDreamRunRecord* Rec = Impl->Active.Get();
	if (Rec == nullptr || Rec->State != EDreamRunState::Starting)
	{
		return;
	}
	if (Rec->bStopRequested)
	{
		if (Impl->bPrepared && bReady)
		{
			Python.Discard(Rec->RunIds.Last());
		}
		EndNotExecuted(*Rec, TEXT("stopped before user code started"), {});
		return;
	}
	if (!bReady)
	{
		HandlePythonNotReady(*Rec);
		return;
	}
	AdvanceStarting(*Rec);
}

void FDreamRunService::HandlePythonNotReady(FDreamRunRecord& Rec)
{
	FDreamPython& Python = DreamRunServicePrivate::Module().GetPython();
	switch (Python.GetState())
	{
	case EDreamPythonState::Cold:
		Python.RequestStart(EDreamStartReason::RunRequest);   // arms the boot frame
		break;
	case EDreamPythonState::Booting:
		ArmEndFrame();
		break;
	case EDreamPythonState::WaitingForInit:
	case EDreamPythonState::Enabling:
		break;      // HandleInitialized boots and arms once _dream exists (the request set bStartWanted)
	default:
	{
		const FText Reason = Python.GetDisabledReason();
		EndNotExecuted(Rec, FString::Printf(TEXT("Python is not available: %s"),
			Reason.IsEmpty() ? TEXT("not running") : *Reason.ToString()), {});
		break;
	}
	}
}

bool FDreamRunService::AdvanceStarting(FDreamRunRecord& Rec)
{
	switch (Rec.StartingStep)
	{
	case EDreamStartingStep::Queued:
	case EDreamStartingStep::Prepare:
		Rec.StartingStep = EDreamStartingStep::Prepare;
		return StepPrepare(Rec);
	case EDreamStartingStep::AwaitSnapshot:
		return StepAwaitSnapshot(Rec);
	case EDreamStartingStep::AwaitSave:
		return StepAwaitSave(Rec);
	case EDreamStartingStep::Enter:
		return StepEnter(Rec);
	case EDreamStartingStep::Done:
		return true;
	}
	return true;
}

bool FDreamRunService::StepPrepare(FDreamRunRecord& Rec)
{
	using namespace DreamRunServicePrivate;
	// Step 1, trust. // Deferred (M4): FDreamTrust::Evaluate of the source path and SHA-256 (U39) runs here first.

	// Step 2: parse and compile (D14(5): no user code). The run ID is allocated here, one per entry (choice A16).
	FDreamPython& Python = Module().GetPython();
	const FDreamRunId Id = DreamRunId::Make(Python.GetGeneration(), ++Impl->EntrySerial);
	Rec.RunIds.Add(Id);
	const FDreamPrepareResult Prepared = Python.Prepare(Id, Rec, Impl->ActiveRequest);
	if (!Prepared.bOk)
	{
		if (!Prepared.InternalError.IsEmpty())
		{
			Rec.bInternalError = true;
			Rec.CommandResult = Prepared.InternalError;
			PushLines(static_cast<uint32>(Rec.RunNumber), EDreamStream::Error, TEXT("Dream Python Editor internal error while compiling:\n") + Prepared.InternalError);
			Python.Discard(Id);
			EndNotExecuted(Rec, TEXT("Dream Python Editor internal error"), {});
			return true;
		}
		// RUN-16: not executed, the error block in the run, the caret placed at the error by OnRunFinished.
		const FDreamSyntaxError& Error = Prepared.Error;
		for (const FString& Line : FormatSyntaxErrorBlock(Rec.Filename, Error))
		{
			Module().GetOutputStore().PushDreamLine(static_cast<uint32>(Rec.RunNumber), EDreamStream::Error, Line);
		}
		FDreamRunLocation Where;
		Where.File = Rec.Filename;
		Where.Line = Error.Line;
		Where.Column = SyntaxErrorColumn(Error);    // RUN-16: the caret goes to CPython's position, not column 0
		EndNotExecuted(Rec, FString::Printf(TEXT("%s: %s"), *Error.TypeName, *Error.Message), Where);
		return true;
	}
	Impl->bPrepared = true;
	Rec.bStatementMode = Prepared.bStatementMode;
	Rec.bAutoPrint = Rec.bAutoPrint && Prepared.bStatementMode;
	Rec.Statements = Prepared.Statements;

	// Step 3: the pre-run journal snapshot, <= 0.2 ms, never waits on disk (§10.2.5).
	Rec.StartingStep = EDreamStartingStep::AwaitSnapshot;
	if (!Impl->bSkipSnapshot)
	{
		Impl->Ticket = Module().GetJournal().SnapshotForRun(Id, Rec.RunNumber, Rec.BufferId, Rec.SourceSnapshot, Rec.Route,
			Impl->ActiveRequest.bDebug);
		Impl->bSnapshotRequested = true;
		Impl->SnapshotRequestedAt = FPlatformTime::Seconds();
		Rec.JournalTicket = Impl->Ticket.Seq;
	}
	ArmEndFrame();      // at most one Starting step per frame (choice A16)
	return false;
}

bool FDreamRunService::StepAwaitSnapshot(FDreamRunRecord& Rec)
{
	using namespace DreamRunServicePrivate;
	FDreamPython& Python = Module().GetPython();
	if (!Impl->bSkipSnapshot)
	{
		FDreamJournal& Journal = Module().GetJournal();
		FText Reason;
		if (Journal.HasFailed(Impl->Ticket, Reason))
		{
			Python.Discard(Rec.RunIds.Last());
			EndNotExecuted(Rec, FString::Printf(TEXT("%s: %s"), DreamRunText::SnapshotFailedPrefix, *Reason.ToString()), {});
			return true;
		}
		if (!Journal.IsDurable(Impl->Ticket))
		{
			if (FPlatformTime::Seconds() - Impl->SnapshotRequestedAt >= SnapshotTimeoutSeconds)
			{
				Python.Discard(Rec.RunIds.Last());
				EndNotExecuted(Rec, FString::Printf(TEXT("%s: the journal did not confirm the snapshot within 2 s"), DreamRunText::SnapshotFailedPrefix), {});
				return true;
			}
			ArmEndFrame();
			return false;
		}
	}

	// Step 4: Run File of a dirty buffer saves first; the run stays Starting and the editor stays live (§5.1.2).
	const FDreamRunRequest& Request = Impl->ActiveRequest;
	if (DreamRunText::HasRunFileSemantics(Rec.Route, Rec.bFileTarget) && Request.bDirty && Request.BufferId.IsValid() && !Impl->bSaveStarted)
	{
		Impl->bSaveStarted = true;
		Rec.StartingStep = EDreamStartingStep::AwaitSave;
		const int32 RunNumber = Rec.RunNumber;
		const bool bShared = DoesSharedInstanceExist();
		TWeakPtr<FDreamRunService> Weak = bShared ? TWeakPtr<FDreamRunService>(AsShared()) : TWeakPtr<FDreamRunService>();
		Module().GetBufferManager().SaveAsync(Request.BufferId, FDreamOnSaved::CreateLambda(
			[this, Weak, bShared, RunNumber](bool bOk, const FText& Error)
			{
				if ((bShared && !Weak.IsValid()) || !Impl->Active || Impl->Active->RunNumber != RunNumber)
				{
					return;
				}
				Impl->bSaveDone = true;
				Impl->bSaveOk = bOk;
				Impl->SaveError = Error;
				ArmEndFrame();
			}));
		return false;
	}
	Rec.StartingStep = EDreamStartingStep::Enter;
	return StepEnter(Rec);
}

bool FDreamRunService::StepAwaitSave(FDreamRunRecord& Rec)
{
	if (!Impl->bSaveDone)
	{
		return false;       // the save callback arms the next frame
	}
	if (!Impl->bSaveOk)
	{
		// The original bytes on disk survive and the buffer stays dirty (§10.14); a parse failure never got here.
		DreamRunServicePrivate::Module().GetPython().Discard(Rec.RunIds.Last());
		EndNotExecuted(Rec, FString::Printf(TEXT("Save failed: %s"), *Impl->SaveError.ToString()), {});
		return true;
	}
	Rec.StartingStep = EDreamStartingStep::Enter;
	return StepEnter(Rec);
}

bool FDreamRunService::StepEnter(FDreamRunRecord& Rec)
{
	FString Why;
	if (!FDreamPython::IsEntryContextValid(Why))
	{
		// §3.9: refuse, re-arm, report; one more frame in Starting.
		if (Impl->LastEntryRefusal != Why)
		{
			UE_LOG(LogDream, Verbose, TEXT("Run #%d waits one more frame to start: %s."), Rec.RunNumber, *Why);
			Impl->LastEntryRefusal = Why;
		}
		ArmEndFrame();
		return false;
	}
	Rec.StartingStep = EDreamStartingStep::Done;
	EnterAndFinish(Rec);
	return true;
}

void FDreamRunService::EnterAndFinish(FDreamRunRecord& Rec)
{
	using namespace DreamRunServicePrivate;
	FDreamEditorModule& M = Module();
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	const FDreamRunRequest& Request = Impl->ActiveRequest;
	const FDreamRunId Id = Rec.RunIds.Last();
	Impl->bPrepared = false;            // _run pops the prepared entry from here on

	// Step 6a: the transaction (§5.9.1): the U6 override, else the route default and user setting; a tab run follows its
	// One undo step per run toggle once the user set it, and until then the per-route setting it was seeded from (B40,
	// A56, P2-run-02). Run File keeps its own default (off) and the toggle can only turn it off.
	bool bTransaction = Settings ? Settings->GetTransactionDefault(Rec.Route) : DreamRunText::RouteTransactionDefault(Rec.Route);
	if (Request.TransactionOverride.IsSet())
	{
		bTransaction = Request.TransactionOverride.GetValue();
	}
	else if (Rec.BufferId.IsValid() && IsTabRoute(Rec.Route))
	{
		if (const TSharedPtr<FDreamBuffer> Buffer = M.GetBufferManager().FindById(Rec.BufferId))
		{
			bTransaction = ResolveTabTransaction(Buffer->GetViewState(), Rec.Route);
		}
	}
	if (DreamRunText::HasRunFileSemantics(Rec.Route, Rec.bFileTarget) && Request.bDirty)
	{
		// Starting step 4 saved the buffer: the file's bytes are now the base version's (audit code.sha256, T-AUD-002).
		Rec.FileSha256 = CleanFileSha256(Rec.BufferId);
	}
	M.GetTransactions().OnRunEntry();
	Rec.Txn = bTransaction ? M.GetTransactions().BeginRunTransaction(Rec) : FDreamTxnHandle();

	// Step 6b: the supervisor, with the output area's geometry for the Run window (§5.5.3 step 4, PO-03).
	FDreamRunPolicy Policy;
	Policy.Presentation = Settings && !Settings->bRunPump ? EDreamRunPresentation::Off : EDreamRunPresentation::RunWindow;  // A28
	Policy.ArmDelaySeconds = 0.3;
	Policy.RunNumber = Rec.RunNumber;
	Policy.TabOrItem = !Rec.TabTitle.IsEmpty() ? Rec.TabTitle : Rec.Filename;
	Policy.Filename = Rec.Filename;
	Policy.CleanupInterruptSeconds = Settings ? Settings->CleanupInterruptSeconds : 10.0;
	Policy.bOutputAreaAtTop = !Settings || Settings->OutputPosition == EDreamOutputPosition::Top;
	// The output area itself (the hand-off checks its type and calls GetHandoffView); its rect is false for a background
	// tab or a minimised window, and the Run window then opens centred (§5.5.3 step 4).
	const TSharedPtr<SDreamTab> Tab = M.GetTab();
	if (Tab.IsValid())
	{
		if (const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea())
		{
			Policy.bOutputAreaVisible = Area->GetDesktopRect(Policy.OutputAreaRect, Policy.DpiScale);
			Policy.OutputAreaWidget = Area;
		}
	}
	// §5.5.2: focus returns to the widget that had it at entry (the REPL prompt for a REPL run, the console box for
	// Dream.RunFile); the tab's code view only when nothing has keyboard focus.
	if (FSlateApplication::IsInitialized())
	{
		Policy.FocusToRestore = FSlateApplication::Get().GetKeyboardFocusedWidget();
	}
	if (!Policy.FocusToRestore.IsValid() && Tab.IsValid())
	{
		Policy.FocusToRestore = Tab->GetFocusToRestore();
	}
	const int32 RunNumber = Rec.RunNumber;
	Policy.OnPump = [this, RunNumber](FDreamRunLiveStats& Live)
	{
		FDreamRunRecord* Active = Impl->Active.Get();
		if (Active == nullptr || Active->RunNumber != RunNumber)
		{
			return;
		}
		FDreamEditorModule::Get().GetTransactions().OnPump(Active->Txn);   // §5.9.4 size watch
		Live.TxnBytes = Active->Txn.LastSize;
		Live.TxnBudget = Active->Txn.Budget;
		// Dream.LastPumpMs, at most 1 Hz (§11.8.1).
		DreamRunServicePrivate::Crash().OnPump(FPlatformTime::Seconds() - Active->EnterSeconds);
	};
	M.GetSupervisor().BeginRun(Id, MoveTemp(Policy));

	M.GetOutputStore().BindRunId(Id, static_cast<uint32>(RunNumber));
	M.GetOutputDevice().SetRunActive(true);
	Rec.State = EDreamRunState::Running;
	Rec.EnterSeconds = FPlatformTime::Seconds();
	Impl->bEntered = true;
	Impl->ActiveRunId = Id;
	Impl->bActiveAgentOrBridge = Rec.Route == EDreamRunRoute::Bridge || Rec.Route == EDreamRunRoute::Mcp;
	// At Running (§5.2.3): the audit's run.start, after the trust, parse, snapshot and save steps (§11.4), and the run's
	// crash breadcrumbs, InUserCode = 1 from the _run entry (§11.8.1).
	DreamRunServicePrivate::Audit().RunStarted(Rec);
	DreamRunServicePrivate::Crash().OnRunStarted(Rec);
	DreamRunServicePrivate::Crash().OnStateChanged(EDreamRunState::Running);
	DreamTrace::TraceRunBegin(Id, RunNumber);
	RunStateChangedEvent.Broadcast(Rec);

	// INV-3: the one entry call. It returns when _run returns (pumps, pending calls and Stop happen inside).
	FString CommandResult;
	int32 LogOutputEntries = 0;
	const bool bExecOk = M.GetPython().EnterRun(Id, CommandResult, LogOutputEntries);
	Rec.Stats.LogOutputEntries = LogOutputEntries;
	FinishRun(Rec, bExecOk, CommandResult);
}

void FDreamRunService::FinishRun(FDreamRunRecord& Rec, bool bExecOk, const FString& CommandResult)
{
	using namespace DreamRunServicePrivate;
	FDreamEditorModule& M = Module();
	const FDreamRunId Id = Rec.RunIds.Last();
	const uint32 RunNumber = static_cast<uint32>(Rec.RunNumber);

	// 1: the supervisor's end (waits for the watchdog without the GIL, drains, freezes the stats; §5.7.7).
	const int32 LogOutputEntries = Rec.Stats.LogOutputEntries;
	M.GetSupervisor().EndRun(Id, Rec.Stats);
	Rec.Stats.LogOutputEntries = LogOutputEntries;
	Impl->ActiveRunId = 0;
	Impl->bActiveAgentOrBridge = false;

	// 2: End and the dangling check on every exit path (§5.9.2).
	M.GetTransactions().EndRunTransaction(Rec.Txn, Rec);

	// 3: protections off, live-view hand-off, Run window teardown (§5.5.3).
	M.GetSupervisor().EndPresentation(Id);
	M.GetOutputDevice().SetRunActive(false);

	// 4: the outcome. An entry call that returned false, or no report, is a Dream internal error (§5.2.4, INV-4).
	if (Impl->bPythonShutDuringRun)
	{
		Rec.Outcome = EDreamRunOutcome::Lost;
		Rec.Detail = TEXT("Python was shut down during the run");
	}
	else if (!bExecOk)
	{
		Rec.Outcome = EDreamRunOutcome::Error;
		Rec.Detail = TEXT("Dream Python Editor internal error");
		Rec.bInternalError = true;
		Rec.CommandResult = CommandResult;
		PushLines(RunNumber, EDreamStream::Error, TEXT("Dream Python Editor internal error: the runner failed.\n") + CommandResult);
		M.GetPython().EmergencyRestore();
	}
	else if (!Rec.bOutcomeReported)
	{
		Rec.Outcome = EDreamRunOutcome::Error;
		Rec.Detail = TEXT("Dream Python Editor internal error");
		Rec.bInternalError = true;
		M.GetOutputStore().PushDreamLine(RunNumber, EDreamStream::Error, TEXT("Dream Python Editor internal error: the runner reported no outcome."));
	}
	Rec.EndSeconds = FPlatformTime::Seconds();
	Rec.EndUtc = FDateTime::UtcNow();
	if (Rec.Outcome == EDreamRunOutcome::Error && !Rec.bInternalError && Rec.Location.Line > 0)
	{
		FDreamRuntimeProblem Problem;
		Problem.SourceKey = SourceKey(Rec);
		Problem.RunNumber = Rec.RunNumber;
		Problem.File = Rec.Location.File;
		Problem.Line = Rec.Location.Line;
		Problem.Message = Rec.Detail;
		M.GetRuntimeProblems().AddRuntimeError(Problem);
	}
	Rec.State = EDreamRunState::Idle;
	M.GetOutputStore().EndRunBlock(RunNumber);
	M.GetJournal().MarkRunEnded(Id, Rec.Outcome);
	{
		int32 Deferrals = 0;
		for (const int32 Count : Rec.Stats.Deferrals)
		{
			Deferrals += Count;
		}
		DreamTrace::TraceRunEnd(Id, Rec.RunNumber, static_cast<uint8>(Rec.Outcome), Rec.Stats.Pumps, Deferrals);
	}

	// 5: Idle, OnRunFinished. Nothing waits in a queue: requests are refused while a run is active (§5.2.2).
	Impl->bEntered = false;
	RetireActive();
}

void FDreamRunService::EndNotExecuted(FDreamRunRecord& Rec, const FString& Detail, const TOptional<FDreamRunLocation>& Location)
{
	// U7: a Starting step failed; the run keeps its number and opened no transaction.
	Rec.Outcome = EDreamRunOutcome::NotExecuted;
	Rec.Detail = Detail;
	if (Location.IsSet())
	{
		Rec.Location = Location.GetValue();
	}
	Rec.State = EDreamRunState::Idle;
	Rec.StartingStep = EDreamStartingStep::Done;
	Rec.EndSeconds = FPlatformTime::Seconds();
	Rec.EndUtc = FDateTime::UtcNow();
	Impl->bPrepared = false;

	FDreamEditorModule& M = DreamRunServicePrivate::Module();
	if (Impl->bSnapshotRequested && Rec.RunIds.Num() > 0)
	{
		M.GetJournal().MarkRunEnded(Rec.RunIds.Last(), EDreamRunOutcome::NotExecuted);
	}
	M.GetOutputStore().EndRunBlock(static_cast<uint32>(Rec.RunNumber));
	UE_LOG(LogDream, Verbose, TEXT("Run #%d not executed: %s"), Rec.RunNumber, *Detail);
	RetireActive();
}

void FDreamRunService::RetireActive()
{
	if (!Impl->Active)
	{
		return;
	}
	Impl->History.Add(MoveTemp(*Impl->Active));
	Impl->Active.Reset();
	Impl->ActiveRequest = FDreamRunRequest();
	Impl->ResetPerRun();
	if (Impl->History.Num() > HistoryCapacity)
	{
		Impl->History.RemoveAt(0, Impl->History.Num() - HistoryCapacity);
	}
	++Impl->RunGeneration;

	// Handlers may submit or record (the history can grow while they run), so they get a stable copy.
	const FDreamRunRecord Finished = Impl->History.Last();
	// Finish (§5.2.3, §5.2.4 step 4): the audit's run.end, not executed runs included (T-RUN-024); the run's crash keys go.
	DreamRunServicePrivate::Audit().RunEnded(Finished);
	DreamRunServicePrivate::Crash().OnRunEnded();
	DreamRunServicePrivate::Crash().OnStateChanged(EDreamRunState::Idle);
	RunStateChangedEvent.Broadcast(Finished);
	RunFinishedEvent.Broadcast(Finished);
}

#undef LOCTEXT_NAMESPACE
