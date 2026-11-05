// SPDX-License-Identifier: Apache-2.0
// FDreamRunLogWriter (TDD §6.2 "Run logs (spill)", OUT-08, D17). See DreamRunLog.h.
#include "Output/DreamRunLog.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"

namespace DreamRunLogPrivate
{
	const char* TypeKey(dream::LineType Type)
	{
		switch (Type)
		{
		case dream::LineType::Stdout:   return "stdout";
		case dream::LineType::Stderr:   return "stderr";
		case dream::LineType::Result:   return "result";
		case dream::LineType::Notice:   return "notice";
		case dream::LineType::Info:     return "info";
		case dream::LineType::Warning:  return "warn";
		case dream::LineType::Error:    return "error";
		case dream::LineType::Agent:    return "agent";
		case dream::LineType::Debugger: return "debugger";
		case dream::LineType::Logpoint: return "logpoint";
		case dream::LineType::Evaluate: return "evaluate";
		case dream::LineType::Echo:     return "echo";
		default:                       return "notice";
		}
	}

	void AppendFString(std::string& Out, const FString& Str)
	{
		Out += DreamText::ToUtf8(Str);
	}

	/** "yyyymmdd-hhmmss" session folder names (A42); anything else under RunLogs is never touched. */
	bool ParseSessionFolder(const FString& Name, FDateTime& OutUtc)
	{
		if (Name.Len() != 15 || Name[8] != TEXT('-'))
		{
			return false;
		}
		for (int32 Index = 0; Index < 15; ++Index)
		{
			if (Index != 8 && !FChar::IsDigit(Name[Index]))
			{
				return false;
			}
		}
		const int32 Year = FCString::Atoi(*Name.Mid(0, 4));
		const int32 Month = FCString::Atoi(*Name.Mid(4, 2));
		const int32 Day = FCString::Atoi(*Name.Mid(6, 2));
		const int32 Hour = FCString::Atoi(*Name.Mid(9, 2));
		const int32 Minute = FCString::Atoi(*Name.Mid(11, 2));
		const int32 Second = FCString::Atoi(*Name.Mid(13, 2));
		if (!FDateTime::Validate(Year, Month, Day, Hour, Minute, Second, 0))
		{
			return false;
		}
		OutUtc = FDateTime(Year, Month, Day, Hour, Minute, Second);
		return true;
	}
}

FDreamRunLogWriter::FDreamRunLogWriter()
	: Pipe(TEXT("DreamRunLog"))
	, PipeState(MakeShared<FPipeState, ESPMode::ThreadSafe>())
{
}

FDreamRunLogWriter::~FDreamRunLogWriter()
{
	Shutdown(5.0);
}

void FDreamRunLogWriter::SetRoot(const FString& InRoot)
{
	Root = InRoot;
	if (!Root.IsEmpty() && !bSweepQueued)
	{
		bSweepQueued = true;
		QueueRetentionSweep(Root);
	}
}

void FDreamRunLogWriter::BeginRun(uint32 RunNumber, const FDreamRunBlockInfo& Header, double StartSeconds)
{
	if (RunNumber == 0)
	{
		return;
	}
	FRun& Run = Runs.FindOrAdd(RunNumber);
	Run = FRun();
	Run.bAlways = Header.bAlwaysWriteRunLogs;
	Run.bReplPrompts = Header.bReplPrompts;
	Run.StartSeconds = StartSeconds;
	Run.HeaderText = "# dream-run-log 1\n";
	DreamRunLogPrivate::AppendFString(Run.HeaderText, Header.RunLogHeader);
	Run.HeaderText += "\n";
	NewestRun = FMath::Max(NewestRun, RunNumber);
	PruneRuns();
}

FDreamRunLogWriter::FRun* FDreamRunLogWriter::FindRun(uint32 RunNumber)
{
	return RunNumber != 0 ? Runs.Find(RunNumber) : nullptr;
}

std::string FDreamRunLogWriter::FormatLine(const FRun& Run, const FDreamRunLogLine& Line)
{
	// [hh:mm:ss.mmm] <type> [<thread>] <text>, or for input echo [hh:mm:ss.mmm] echo [<line>] <text> (§6.2 format).
	std::string Out;
	Out.reserve(Line.Text.size() + 48);
	char Stamp[24];
	FCStringAnsi::Snprintf(Stamp, sizeof(Stamp), "[%02d:%02d:%02d.%03d] ", Line.Utc.GetHour(), Line.Utc.GetMinute(),
		Line.Utc.GetSecond(), Line.Utc.GetMillisecond());
	Out += Stamp;
	Out += DreamRunLogPrivate::TypeKey(Line.Type);
	Out += " [";
	if (Line.Type == dream::LineType::Echo)
	{
		if ((Line.Flags & dream::OLF_ReplPrompt) != 0 || Run.bReplPrompts)
		{
			Out += (Line.Flags & dream::OLF_PromptFirst) != 0 ? ">>>" : "...";   // a statement's first line (§6.4)
		}
		else
		{
			Out += std::to_string(Line.EchoLine);
		}
	}
	else
	{
		DreamRunLogPrivate::AppendFString(Out, Line.ThreadName.IsEmpty() ? FString(TEXT("GameThread")) : Line.ThreadName);
	}
	Out += "] ";
	Out.append(Line.Text.data(), Line.Text.size());
	Out += "\n";
	return Out;
}

void FDreamRunLogWriter::AddLine(uint32 RunNumber, const FDreamRunLogLine& Line, double NowSeconds)
{
	FRun* Run = FindRun(RunNumber);
	if (Run == nullptr || Run->bFailed || Root.IsEmpty())
	{
		return;
	}
	const bool bEcho = Line.Type == dream::LineType::Echo;
	std::string Formatted = FormatLine(*Run, Line);

	if (Run->bFile)
	{
		AppendFormatted(*Run, MoveTemp(Formatted));
		return;
	}

	if (Run->bEnded)
	{
		// A background line after the footer: it reopens (or, as the run's first output, creates) the file (§6.2).
		if (bEcho || !Run->bAlways)
		{
			return;
		}
		Run->Held.clear();
		Run->HeldBytes = 0;
		Run->HeldOutputLines = 0;
		CreateFile(RunNumber, *Run);
		AppendFormatted(*Run, MoveTemp(Formatted));
		return;
	}

	// Echo lines alone never create a file: they wait for the run's first other line (§6.2 "Input echo in run logs").
	Run->bHasOutput |= !bEcho;
	if (Run->HeldBytes + static_cast<int64>(Formatted.size()) <= SpillBytes)
	{
		Run->HeldBytes += static_cast<int64>(Formatted.size());
		Run->HeldOutputLines += bEcho ? 0 : 1;
		Run->Held.push_back(MoveTemp(Formatted));
	}
	else if (Run->bHasOutput)
	{
		// The line that would overflow the spill buffer: the buffer goes to a new file and this line follows it, so the run
		// log keeps every line the view had (§6.2, OUT-08).
		CreateFile(RunNumber, *Run);
		AppendFormatted(*Run, MoveTemp(Formatted));
		return;
	}
	else
	{
		++Run->SkippedLines;     // echo only so far: no file may be created yet (§6.2 "Input echo in run logs")
	}

	// (output choice) The 10,000-line threshold counts output lines: a run's echo lines are held with them but, as they
	// never create a file alone, they do not bring the file forward either (T-OUT-023: 9,999 printed lines make none).
	const bool bSpillFull = Run->HeldOutputLines >= SpillLines || Run->HeldBytes >= SpillBytes || Run->SkippedLines > 0;
	const bool bLong = NowSeconds - Run->StartSeconds >= SpillSeconds;
	if (Run->bHasOutput && (Run->bAlways || Run->bForced || bSpillFull || bLong))
	{
		CreateFile(RunNumber, *Run);
	}
}

void FDreamRunLogWriter::CreateFile(uint32 RunNumber, FRun& Run)
{
	Run.Path = FPaths::Combine(Root, FString::Printf(TEXT("run-%u.log"), RunNumber));
	Run.bFile = true;
	Run.Pending += Run.HeaderText;
	if (Run.SkippedLines > 0)
	{
		Run.Pending += "[" + std::to_string(Run.SkippedLines) + " lines not written: the spill buffer was full]\n";
		Run.SkippedLines = 0;
	}
	for (std::string& Held : Run.Held)
	{
		Run.Pending += Held;
		++Run.Lines;
	}
	Run.Held.clear();
	Run.HeldBytes = 0;
	Run.HeldOutputLines = 0;

	FScopeLock Guard(&EventLock);
	CreatedEvents.Emplace(RunNumber, Run.Path);
}

void FDreamRunLogWriter::AppendFormatted(FRun& Run, std::string&& Line)
{
	if (PendingBytes.load(std::memory_order_relaxed) + static_cast<int64>(Run.Pending.size()) > MaxPendingBytes)
	{
		++Run.SkippedLines;       // the disk is behind: one gap line later (§6.2 back-pressure)
		return;
	}
	if (Run.SkippedLines > 0)
	{
		Run.Pending += "[" + std::to_string(Run.SkippedLines) + " lines not written: disk too slow]\n";
		Run.SkippedLines = 0;
	}
	Run.Pending += Line;
	++Run.Lines;
}

void FDreamRunLogWriter::EndRun(uint32 RunNumber, const FDreamRunBlockInfo& Footer)
{
	FRun* Run = FindRun(RunNumber);
	if (Run == nullptr)
	{
		return;
	}
	Run->bEnded = true;
	if (!Run->bFile && Run->bHasOutput && !Root.IsEmpty() && (Run->bAlways || Run->bForced))
	{
		CreateFile(RunNumber, *Run);
	}
	if (!Run->bFile)
	{
		// Runs without output, and small runs with the setting off, get no file (§6.2).
		Run->Held.clear();
		Run->HeldBytes = 0;
		Run->HeldOutputLines = 0;
		return;
	}
	char End[160];
	FCStringAnsi::Snprintf(End, sizeof(End), "# end outcome=%s duration=%.3f lines=%lld\n",
		DreamText::ToUtf8(Footer.OutcomeKey).c_str(), Footer.DurationSeconds, static_cast<long long>(Run->Lines));
	if (Run->SkippedLines > 0)
	{
		Run->Pending += "[" + std::to_string(Run->SkippedLines) + " lines not written: disk too slow]\n";
		Run->SkippedLines = 0;
	}
	Run->Pending += End;
	Run->bCloseAfterPending = true;
	LaunchWrite(RunNumber, *Run);
}

void FDreamRunLogWriter::Force(uint32 RunNumber)
{
	FRun* Run = FindRun(RunNumber);
	if (Run == nullptr || Run->bFile || Run->bFailed || Root.IsEmpty())
	{
		return;
	}
	Run->bForced = true;
	if (Run->bHasOutput)
	{
		CreateFile(RunNumber, *Run);
	}
}

void FDreamRunLogWriter::Pump(double NowSeconds)
{
	{
		FScopeLock Guard(&EventLock);
		for (uint32 Failed : FailedRuns)
		{
			if (FRun* Run = Runs.Find(Failed))
			{
				Run->bFailed = true;
				Run->Pending.clear();
				Run->Held.clear();
			}
		}
		FailedRuns.Reset();
	}
	for (TPair<uint32, FRun>& Pair : Runs)
	{
		FRun& Run = Pair.Value;
		if (!Run.bFile && !Run.bEnded && Run.bHasOutput && !Root.IsEmpty() && NowSeconds - Run.StartSeconds >= SpillSeconds)
		{
			CreateFile(Pair.Key, Run);        // J5: long jobs get a log even with the setting off
		}
		if (!Run.Pending.empty() || Run.bCloseAfterPending)
		{
			LaunchWrite(Pair.Key, Run);
		}
	}
}

void FDreamRunLogWriter::LaunchWrite(uint32 RunNumber, FRun& Run)
{
	if (Run.bFailed || (Run.Pending.empty() && !Run.bCloseAfterPending))
	{
		return;
	}
	std::string Data = MoveTemp(Run.Pending);
	Run.Pending = std::string();
	const bool bClose = Run.bCloseAfterPending;
	Run.bCloseAfterPending = false;
	const int64 Bytes = static_cast<int64>(Data.size());
	PendingBytes.fetch_add(Bytes, std::memory_order_relaxed);

	const FString Path = Run.Path;
	const FString Dir = Root;
	TSharedRef<FPipeState, ESPMode::ThreadSafe> State = PipeState;
	Pipe.Launch(TEXT("DreamRunLogAppend"), [this, State, Path, Dir, RunNumber, Bytes, bClose, Data = MoveTemp(Data)]()
	{
		ON_SCOPE_EXIT { PendingBytes.fetch_sub(Bytes, std::memory_order_relaxed); };
		if (State->FailedPaths.Contains(Path))
		{
			return;
		}
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		IFileHandle* Handle = State->Handles.FindRef(Path);
		if (Handle == nullptr)
		{
			if (State->MadeDirectory != Dir && PlatformFile.CreateDirectoryTree(*Dir))
			{
				State->MadeDirectory = Dir;
			}
			Handle = PlatformFile.OpenWrite(*Path, /*bAppend*/ true, /*bAllowRead*/ true);
			if (Handle == nullptr)
			{
				State->FailedPaths.Add(Path);
				FScopeLock Guard(&EventLock);
				ErrorEvents.Emplace(RunNumber, FString::Printf(TEXT("could not open %s (error %u)"), *Path, FPlatformMisc::GetLastError()));
				FailedRuns.Add(RunNumber);
				return;
			}
			State->Handles.Add(Path, Handle);
		}
		if (!Data.empty() && !Handle->Write(reinterpret_cast<const uint8*>(Data.data()), static_cast<int64>(Data.size())))
		{
			State->FailedPaths.Add(Path);
			delete Handle;
			State->Handles.Remove(Path);
			FScopeLock Guard(&EventLock);
			ErrorEvents.Emplace(RunNumber, FString::Printf(TEXT("write to %s failed (error %u)"), *Path, FPlatformMisc::GetLastError()));
			FailedRuns.Add(RunNumber);
			return;
		}
		if (bClose)
		{
			Handle->Flush();
			delete Handle;
			State->Handles.Remove(Path);
		}
	});
}

FString FDreamRunLogWriter::GetPath(uint32 RunNumber) const
{
	const FRun* Run = RunNumber != 0 ? Runs.Find(RunNumber) : nullptr;
	return Run && Run->bFile ? Run->Path : FString();
}

void FDreamRunLogWriter::TakeEvents(TArray<TPair<uint32, FString>>& OutCreated, TArray<TPair<uint32, FString>>& OutErrors)
{
	FScopeLock Guard(&EventLock);
	OutCreated.Append(MoveTemp(CreatedEvents));
	CreatedEvents.Reset();
	OutErrors.Append(MoveTemp(ErrorEvents));
	ErrorEvents.Reset();
}

bool FDreamRunLogWriter::WaitForWrites(double TimeoutSeconds)
{
	return Pipe.WaitUntilEmpty(FTimespan::FromSeconds(TimeoutSeconds));
}

void FDreamRunLogWriter::Shutdown(double TimeoutSeconds)
{
	TSharedRef<FPipeState, ESPMode::ThreadSafe> State = PipeState;
	Pipe.Launch(TEXT("DreamRunLogClose"), [State]()
	{
		for (TPair<FString, IFileHandle*>& Pair : State->Handles)
		{
			Pair.Value->Flush();
			delete Pair.Value;
		}
		State->Handles.Reset();
	});
	if (!Pipe.WaitUntilEmpty(FTimespan::FromSeconds(TimeoutSeconds)))
	{
		// A hung disk: the pipe's destructor would assert, so wait without a bound only here, at module shutdown.
		UE_LOG(LogDream, Warning, TEXT("Run logs: waiting for a slow disk before shutdown."));
		Pipe.WaitUntilEmpty();
	}
}

void FDreamRunLogWriter::PruneRuns()
{
	// Keep the runs that can still receive background lines (the 500-record history, RUN-12).
	if (Runs.Num() <= 600)
	{
		return;
	}
	for (auto It = Runs.CreateIterator(); It; ++It)
	{
		if (It->Value.bEnded && It->Key + 500 < NewestRun && It->Value.Pending.empty())
		{
			It.RemoveCurrent();
		}
	}
}

void FDreamRunLogWriter::QueueRetentionSweep(const FString& SessionDir)
{
	// D17 retention: session folders older than 30 days go, then the oldest until the folder holds at most 1 GB.
	// Only "yyyymmdd-hhmmss" folders whose files are all run-*.log are ever removed.
	const FString RunLogsDir = FPaths::GetPath(SessionDir);
	const FString Current = FPaths::GetCleanFilename(SessionDir);
	Pipe.Launch(TEXT("DreamRunLogRetention"), [RunLogsDir, Current]()
	{
		struct FFolder
		{
			FString Path;
			FDateTime Utc;
			int64 Bytes = 0;
		};
		IFileManager& FileManager = IFileManager::Get();
		TArray<FString> Names;
		FileManager.FindFiles(Names, *(RunLogsDir / TEXT("*")), /*Files*/ false, /*Directories*/ true);
		TArray<FFolder> Folders;
		for (const FString& Name : Names)
		{
			FDateTime Utc;
			if (Name == Current || !DreamRunLogPrivate::ParseSessionFolder(Name, Utc))
			{
				continue;
			}
			FFolder Folder{RunLogsDir / Name, Utc, 0};
			bool bOnlyRunLogs = true;
			FileManager.IterateDirectoryStat(*Folder.Path, [&Folder, &bOnlyRunLogs](const TCHAR* Path, const FFileStatData& Stat)
			{
				const FString Clean = FPaths::GetCleanFilename(Path);
				if (Stat.bIsDirectory || !Clean.StartsWith(TEXT("run-")) || !Clean.EndsWith(TEXT(".log")))
				{
					bOnlyRunLogs = false;
					return false;
				}
				Folder.Bytes += Stat.FileSize;
				return true;
			});
			if (bOnlyRunLogs)
			{
				Folders.Add(MoveTemp(Folder));
			}
		}
		Folders.Sort([](const FFolder& A, const FFolder& B) { return A.Utc < B.Utc; });
		const FDateTime Cutoff = FDateTime::UtcNow() - FTimespan::FromDays(RetentionDays);
		int64 Total = 0;
		for (const FFolder& Folder : Folders)
		{
			Total += Folder.Bytes;
		}
		for (const FFolder& Folder : Folders)
		{
			if (Folder.Utc >= Cutoff && Total <= RetentionMaxBytes)
			{
				break;
			}
			if (FileManager.DeleteDirectory(*Folder.Path, /*RequireExists*/ false, /*Tree*/ true))
			{
				Total -= Folder.Bytes;
			}
		}
	});
}
