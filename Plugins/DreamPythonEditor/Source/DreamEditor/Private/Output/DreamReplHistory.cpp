// SPDX-License-Identifier: Apache-2.0
// REPL history persisted per project (TDD §6.5 "Design: history", REPL-02, D17, SEC-06). See DreamReplHistory.h.
// The file is JSON lines through dream::json (never pickle): line 1 the header, then one entry per line. Entries live on
// the game thread; every file write is a task on the "DreamReplHistory" pipe, so they run in order and off the game thread.
#include "Output/DreamReplHistory.h"

#include "Algo/Reverse.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamWin32FileOps.h"
#include "Tasks/Pipe.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "dream/Json.h"

#include <string>
#include <string_view>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamHistoryPrivate
{
	const char* SourceKey(EDreamHistorySource Source)
	{
		switch (Source)
		{
		case EDreamHistorySource::Console:   return "console";
		case EDreamHistorySource::UeConsole: return "ue-console";
		default:                            return "repl";
		}
	}

	bool ParseSource(const std::string& Key, EDreamHistorySource& Out)
	{
		if (Key == "repl")       { Out = EDreamHistorySource::Repl; return true; }
		if (Key == "console")    { Out = EDreamHistorySource::Console; return true; }
		if (Key == "ue-console") { Out = EDreamHistorySource::UeConsole; return true; }
		return false;
	}

	std::string HeaderLine(bool bImported)
	{
		dream::json::Value Header = dream::json::Value::MakeObject();
		Header.Set("format", dream::json::Value::MakeString("dream.repl-history"));
		Header.Set("schema", dream::json::Value::MakeInt(1));
		Header.Set("importedConsoleHistory", dream::json::Value::MakeBool(bImported));
		return dream::json::Serialize(Header) + "\n";
	}

	std::string EntryLine(const FDreamHistoryEntry& Entry)
	{
		dream::json::Value Line = dream::json::Value::MakeObject();
		Line.Set("id", dream::json::Value::MakeString(DreamText::ToUtf8(Entry.Id)));
		Line.Set("t", dream::json::Value::MakeInt(Entry.TimeMs));
		Line.Set("src", dream::json::Value::MakeString(SourceKey(Entry.Source)));
		Line.Set("text", dream::json::Value::MakeString(DreamText::ToUtf8(Entry.Text)));
		return dream::json::Serialize(Line) + "\n";
	}

	bool ParseEntry(std::string_view Utf8, FDreamHistoryEntry& Out)
	{
		dream::json::Value Line;
		if (!dream::json::Parse(Utf8, Line) || !Line.IsObject())
		{
			return false;
		}
		const dream::json::Value* Text = Line.Find("text");
		if (Text == nullptr || Text->GetKind() != dream::json::Kind::String)
		{
			return false;
		}
		if (!ParseSource(Line.Get("src").AsString(), Out.Source))
		{
			Out.Source = EDreamHistorySource::Repl;
		}
		Out.Id = DreamText::Utf8ToFString(Line.Get("id").AsString());
		Out.TimeMs = Line.Get("t").AsInt();
		Out.Text = DreamText::Utf8ToFString(Text->AsString());
		return true;
	}

	bool WriteWhole(const FString& Path, const std::string& Data)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Path));
		IFileHandle* Handle = PlatformFile.OpenWrite(*Path, /*bAppend*/ false, /*bAllowRead*/ false);
		if (Handle == nullptr)
		{
			return false;
		}
		const bool bOk = Handle->Write(reinterpret_cast<const uint8*>(Data.data()), static_cast<int64>(Data.size())) && Handle->Flush();
		delete Handle;
		return bOk;
	}

	bool AppendLine(const FString& Path, const std::string& Data)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		IFileHandle* Handle = PlatformFile.OpenWrite(*Path, /*bAppend*/ true, /*bAllowRead*/ true);
		if (Handle == nullptr)
		{
			return false;
		}
		// A torn last line is never appended to: Open rewrites the file without it first (OUT-01), and the pipe runs that
		// rewrite before any append. When the rewrite failed, the entry is not glued onto the torn bytes either.
		const int64 Size = Handle->Size();
		if (Size > 0 && Handle->Seek(Size - 1))
		{
			uint8 Last = 0;
			if (!Handle->Read(&Last, 1) || Last != '\n')
			{
				delete Handle;
				return false;
			}
		}
		const bool bOk = Handle->SeekFromEnd(0)
			&& Handle->Write(reinterpret_cast<const uint8*>(Data.data()), static_cast<int64>(Data.size()));
		delete Handle;
		return bOk;
	}

	/** §6.5 cap, T-REPL-007 "compaction is atomic": Temp replaces Path in one step (ReplaceFileW keeps the previous file as
	 *  Backup); history.jsonl never goes missing between two moves. */
	bool ReplaceWith(const FString& Path, const FString& Temp, const FString& Backup)
	{
		FDreamWin32FileOps Ops;
		return Ops.ReplaceAtomic(FDreamWin32FileOps::ToUtf8Path(Path), FDreamWin32FileOps::ToUtf8Path(Temp), FDreamWin32FileOps::ToUtf8Path(Backup));
	}

	/** One read of history.jsonl, or of its .1 when it is missing (T-REPL-007), parsed; nothing is written. */
	struct FHistoryRead
	{
		TArray<FDreamHistoryEntry> Entries;
		int32 Lines = 0;                    // entry lines read
		bool bImported = false;
		bool bUnreadable = false;           // history.jsonl exists but could not be read now
		bool bCorrupt = false;
		bool bRewriteDue = false;           // missing, empty, torn or corrupt: the file is written whole once it is ours
		FString CorruptSource;              // the .1 it was restored from; empty: history.jsonl itself
		FFileStatData Stat;                 // history.jsonl just before the read (invalid when missing)
	};

	FHistoryRead ReadHistory(const FString& Path)
	{
		FHistoryRead Read;
		Read.Stat = IFileManager::Get().GetStatData(*Path);
		TArray<uint8> Bytes;
		const bool bFileExists = Read.Stat.bIsValid && !Read.Stat.bIsDirectory;
		bool bHaveFile = bFileExists && FFileHelper::LoadFileToArray(Bytes, *Path);
		if (bFileExists && !bHaveFile)
		{
			// Present but not readable now (another process holds it without read sharing): this session keeps its history in
			// memory and never replaces the file, whose entries are newer than any backup (OUT-03).
			UE_LOG(LogDream, Warning, TEXT("REPL history: %s could not be read; this session keeps its history in memory"), *Path);
			Read.bUnreadable = true;
			Bytes.Reset();
		}
		else if (!bFileExists)
		{
			// An older build moved history.jsonl to .1 before moving the new file in; a crash between the two left only .1.
			// Read it, and the rewrite writes history.jsonl again (T-REPL-007).
			const FString Previous = Path + TEXT(".1");
			if (IFileManager::Get().FileExists(*Previous) && FFileHelper::LoadFileToArray(Bytes, *Previous) && Bytes.Num() > 0)
			{
				UE_LOG(LogDream, Warning, TEXT("REPL history: %s was missing; restored from %s"), *Path, *Previous);
				Read.CorruptSource = Previous;      // a damaged backup is the one set aside, never the absent file
			}
			else
			{
				Bytes.Reset();
			}
		}
		// §6.5: a torn last line is dropped on load, and the file is rewritten without it, so the next append never lands on it.
		const bool bTornTail = Bytes.Num() > 0 && Bytes.Last() != '\n';
		if (Bytes.Num() > 0)
		{
			const std::string_view All(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
			size_t Start = 0;
			int32 LineIndex = 0;
			while (Start < All.size())
			{
				size_t End = All.find('\n', Start);
				const bool bTerminated = End != std::string_view::npos;
				if (!bTerminated)
				{
					End = All.size();
				}
				std::string_view Line = All.substr(Start, End - Start);
				if (!Line.empty() && Line.back() == '\r')
				{
					Line.remove_suffix(1);
				}
				Start = End + 1;
				if (Line.empty())
				{
					continue;
				}
				if (LineIndex++ == 0)
				{
					dream::json::Value Header;
					// (rename, A246) A history file written before the rename keeps its "sumi.repl-history" header and is read as it is.
					if (!dream::json::Parse(Line, Header) || (Header.Get("format").AsString() != "dream.repl-history"
						&& Header.Get("format").AsString() != "sumi.repl-history"))
					{
						Read.bCorrupt = true;
						break;
					}
					Read.bImported = Header.Get("importedConsoleHistory").AsBool(false);
					continue;
				}
				FDreamHistoryEntry Entry;
				if (ParseEntry(Line, Entry))
				{
					Read.Entries.Add(MoveTemp(Entry));
					++Read.Lines;
				}
				else if (!bTerminated)
				{
					break;                // a torn last line is dropped (§6.5 writes)
				}
				else
				{
					Read.bCorrupt = true;
					break;
				}
			}
		}
		if (Read.bCorrupt)
		{
			// §6.7: nothing of a corrupt file is kept; it is set aside as .corrupt once the file is ours, and a new one starts.
			Read.Entries.Reset();
			Read.Lines = 0;
			Read.bImported = false;
			bHaveFile = false;
		}
		Read.bRewriteDue = !bHaveFile || Bytes.Num() == 0 || bTornTail;
		return Read;
	}

	/** The file is as it was when Then was taken: no other editor wrote it since (§6.5 "Second instance"). */
	bool IsSameFile(const FFileStatData& Now, const FFileStatData& Then)
	{
		return Now.bIsValid == Then.bIsValid
			&& (!Now.bIsValid || (Now.FileSize == Then.FileSize && Now.ModificationTime == Then.ModificationTime));
	}

	/** "C:/tools/fix.py" in the stock Python executor history is a file run, not code (§6.5 import). */
	bool IsOnlyPyPath(const FString& Text)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (!Trimmed.EndsWith(TEXT(".py"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		for (const TCHAR Char : Trimmed)
		{
			if (Char == TEXT('\n') || Char == TEXT('(') || Char == TEXT('=') || Char == TEXT(';'))
			{
				return false;
			}
		}
		return true;
	}
}

struct FDreamReplHistory::FImpl
{
	TArray<FDreamHistoryEntry> Entries;      // oldest first, at most Cap (game thread)
	FString FilePath;
	bool bOpen = false;
	bool bMemoryOnly = false;
	bool bImported = false;
	bool bCorruptPending = false;           // read as corrupt while memory-only: set aside when the file becomes ours
	FString CorruptSource;                  // the file read as corrupt: history.jsonl, or the .1 it was restored from
	bool bUnreadable = false;               // history.jsonl exists but could not be read: never replaced this session
	int32 FileLines = 0;                    // entry lines in the file, for the 2x-cap compaction
	// While memory-only (§6.5 "Second instance"): what the file needs once it is ours. The read's stat tells whether another
	// editor wrote the file since; only this editor's own entries are then added to it (P3-out-01).
	FFileStatData ReadStat;                 // history.jsonl as the last read found it
	bool bRewriteDue = false;               // a rewrite was skipped (new, torn or corrupt file, the import flag, Clear)
	bool bClearPending = false;             // Clear while memory-only: the file's entries are not taken back
	TSet<FString> UnwrittenIds;             // ids of the entries Add made while memory-only
	FString StagingPath;                    // §10.5: a secondary's <sec>/repl-history.jsonl (instances group)
	UE::Tasks::FPipe Pipe{TEXT("DreamReplHistory")};
	FSimpleMulticastDelegate Changed;

	bool CanWrite() const { return bOpen && !bMemoryOnly && !bUnreadable && !FilePath.IsEmpty(); }

	/** §6.7: history.jsonl.corrupt (from CorruptSource: the file itself or the .1 it was restored from), and the toast. */
	void SetCorruptAside()
	{
		bCorruptPending = false;
		const FString Source = CorruptSource.IsEmpty() ? FilePath : CorruptSource;
		if (!IFileManager::Get().Move(*(FilePath + TEXT(".corrupt")), *Source, /*bReplace*/ true))
		{
			UE_LOG(LogDream, Warning, TEXT("REPL history: %s was unreadable and could not be set aside"), *Source);
			return;
		}
		UE_LOG(LogDream, Warning, TEXT("REPL history was unreadable and has been set aside: %s -> %s.corrupt"), *Source, *FilePath);
		if (FSlateApplication::IsInitialized())
		{
			FNotificationInfo Info(LOCTEXT("Out.ReplHistoryCorrupt", "REPL history was unreadable and has been set aside"));
			Info.ExpireDuration = 6.f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
	}

	/** Rewrites the file atomically: a temp file renamed into place, the previous generation kept as .1 (§6.5 cap). */
	/** OnWritten (instances, review r1): whether this rewrite reached the disk; called on the pipe, or at once with false
	 *  when this editor cannot write the file now. */
	void QueueRewrite(TFunction<void(bool)> OnWritten = nullptr)
	{
		if (!CanWrite())
		{
			bRewriteDue = bRewriteDue || (bOpen && bMemoryOnly);   // written whole once the file is ours (SetMemoryOnly)
			if (OnWritten)
			{
				OnWritten(false);
			}
			return;
		}
		bRewriteDue = false;
		FileLines = Entries.Num();
		const FString Path = FilePath;
		// The lines are built on the pipe: the game thread pays only the copy of the entries (A2-01).
		Pipe.Launch(TEXT("DreamReplHistoryRewrite"), [Path, Lines = Entries, bHeaderImported = bImported, OnWritten = MoveTemp(OnWritten)]()
		{
			const bool bOk = WriteAndReplace(Path, Lines, bHeaderImported);
			if (OnWritten)
			{
				OnWritten(bOk);
			}
		});
	}

	static bool WriteAndReplace(const FString& Path, const TArray<FDreamHistoryEntry>& Lines, bool bHeaderImported)
	{
		std::string Data = DreamHistoryPrivate::HeaderLine(bHeaderImported);
		for (const FDreamHistoryEntry& Entry : Lines)
		{
			Data += DreamHistoryPrivate::EntryLine(Entry);
		}
		const FString Temp = Path + TEXT(".tmp");
		if (!DreamHistoryPrivate::WriteWhole(Temp, Data))
		{
			UE_LOG(LogDream, Warning, TEXT("REPL history: could not write %s"), *Temp);
			return false;
		}
		// One atomic step (T-REPL-007): ReplaceFileW keeps the old file as .1, or a write-through move when there is none.
		if (!DreamHistoryPrivate::ReplaceWith(Path, Temp, Path + TEXT(".1")))
		{
			UE_LOG(LogDream, Warning, TEXT("REPL history: could not replace %s"), *Path);
			return false;
		}
		return true;
	}

	/** §10.5: one of this secondary's entries appended to its staging file (the header first when the file is new). */
	void QueueStagedAppend(const FDreamHistoryEntry& Entry)
	{
		if (StagingPath.IsEmpty())
		{
			return;
		}
		const FString Path = StagingPath;
		Pipe.Launch(TEXT("DreamReplHistoryStage"), [Path, Data = DreamHistoryPrivate::EntryLine(Entry)]()
		{
			if (!IFileManager::Get().FileExists(*Path) && !DreamHistoryPrivate::WriteWhole(Path, DreamHistoryPrivate::HeaderLine(true)))
			{
				UE_LOG(LogDream, Warning, TEXT("REPL history: could not create %s"), *Path);
				return;
			}
			if (!DreamHistoryPrivate::AppendLine(Path, Data))
			{
				UE_LOG(LogDream, Warning, TEXT("REPL history: could not append to %s"), *Path);
			}
		});
	}

	void QueueAppend(const FDreamHistoryEntry& Entry)
	{
		if (!CanWrite())
		{
			return;
		}
		++FileLines;
		const FString Path = FilePath;
		Pipe.Launch(TEXT("DreamReplHistoryAppend"), [Path, Data = DreamHistoryPrivate::EntryLine(Entry)]()
		{
			if (!DreamHistoryPrivate::AppendLine(Path, Data))
			{
				UE_LOG(LogDream, Warning, TEXT("REPL history: could not append to %s"), *Path);
			}
		});
	}

	/** Entries this editor added while memory-only, once the file is ours: one append each (§6.5 writes), or the 2x-cap
	 *  compaction when they would reach it. */
	void QueueAppends(const TArray<FDreamHistoryEntry>& Added)
	{
		if (Added.IsEmpty())
		{
			return;
		}
		if (FileLines + Added.Num() >= 2 * Cap)
		{
			QueueRewrite();
			return;
		}
		for (const FDreamHistoryEntry& Entry : Added)
		{
			QueueAppend(Entry);
		}
	}

	/** Dedupe (the last occurrence of a repeated text wins) and the cap, keeping order. */
	void Normalize()
	{
		TSet<FString> Seen;
		TArray<FDreamHistoryEntry> Kept;
		for (int32 Index = Entries.Num() - 1; Index >= 0 && Kept.Num() < Cap; --Index)
		{
			bool bAlready = false;
			Seen.Add(Entries[Index].Text, &bAlready);
			if (!bAlready)
			{
				Kept.Add(MoveTemp(Entries[Index]));
			}
		}
		Algo::Reverse(Kept);
		Entries = MoveTemp(Kept);
	}
};

FDreamReplHistory::FDreamReplHistory()
	: Impl(MakeUnique<FImpl>())
{
}

FDreamReplHistory::~FDreamReplHistory()
{
	Close();
}

void FDreamReplHistory::Open(const FString& StorageRoot)
{
	check(IsInGameThread());
	Close();
	Impl->FilePath = StorageRoot.IsEmpty() ? FString() : FPaths::Combine(StorageRoot, TEXT("Repl"), TEXT("history.jsonl"));
	Impl->bOpen = !Impl->FilePath.IsEmpty();
	Impl->bCorruptPending = false;
	Impl->bClearPending = false;
	Impl->UnwrittenIds.Reset();

	DreamHistoryPrivate::FHistoryRead Read;
	if (Impl->bOpen)
	{
		Read = DreamHistoryPrivate::ReadHistory(Impl->FilePath);
	}
	Impl->Entries = MoveTemp(Read.Entries);
	Impl->FileLines = Read.Lines;
	Impl->bImported = Read.bImported;
	Impl->bUnreadable = Read.bUnreadable;
	Impl->CorruptSource = Read.CorruptSource;
	Impl->ReadStat = Read.Stat;
	Impl->bRewriteDue = Read.bRewriteDue;
	if (Read.bCorrupt)
	{
		// §6.7: rename to history.jsonl.corrupt and start empty, with a toast. A memory-only instance never renames the file;
		// it sets it aside only if the file becomes its own and is still corrupt then (SetMemoryOnly(false)).
		if (Impl->CanWrite())
		{
			Impl->SetCorruptAside();
		}
		else
		{
			Impl->bCorruptPending = true;
		}
	}

	const int32 LoadedLines = Impl->FileLines;
	Impl->Normalize();
	if (Impl->CanWrite() && Impl->bRewriteDue)
	{
		Impl->QueueRewrite();         // a new file starts with its header; a torn tail is cut off (OUT-01)
	}
	else
	{
		Impl->FileLines = LoadedLines;
	}
	ImportConsoleHistoryOnce();
	Impl->Changed.Broadcast();
}

void FDreamReplHistory::Close()
{
	if (Impl.IsValid())
	{
		if (!Impl->Pipe.WaitUntilEmpty(FTimespan::FromSeconds(2.0)))
		{
			// The pipe must be empty before FImpl and its FPipe die (Pipe.h: ~FPipe checks !HasWork()), as the run-log
			// writer waits: a rewrite retrying on a locked file finishes first (OUT-04).
			UE_LOG(LogDream, Warning, TEXT("REPL history: waiting for the last write of %s"), *Impl->FilePath);
			Impl->Pipe.WaitUntilEmpty();
		}
		Impl->bOpen = false;
	}
}

void FDreamReplHistory::Add(const FString& Text, EDreamHistorySource Source)
{
	check(IsInGameThread());
	FString Clean = Text;
	while (Clean.EndsWith(TEXT("\n")) || Clean.EndsWith(TEXT("\r")))
	{
		Clean.LeftChopInline(1);
	}
	if (Clean.TrimStartAndEnd().IsEmpty())
	{
		return;
	}
	if (!Impl->Entries.IsEmpty() && Impl->Entries.Last().Text.Equals(Clean, ESearchCase::CaseSensitive))
	{
		return;                       // dedupe: the same text as the newest entry is skipped
	}
	FDreamHistoryEntry Entry;
	Entry.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Entry.TimeMs = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond;
	Entry.Source = Source;
	Entry.Text = MoveTemp(Clean);

	// An older copy of the same text moves to the end (on load the last occurrence wins, so the file agrees).
	Impl->Entries.RemoveAll([&Entry](const FDreamHistoryEntry& Old) { return Old.Text.Equals(Entry.Text, ESearchCase::CaseSensitive); });
	Impl->Entries.Add(Entry);
	if (Impl->Entries.Num() > Cap)
	{
		Impl->Entries.RemoveAt(0, Impl->Entries.Num() - Cap);
	}
	if (!Impl->CanWrite() && Impl->bMemoryOnly)
	{
		// Appended when the file becomes this editor's (SetMemoryOnly(false)); ids of entries gone since are dropped.
		Impl->UnwrittenIds.Add(Entry.Id);
		if (Impl->UnwrittenIds.Num() > 2 * Cap)
		{
			TSet<FString> Live;
			for (const FDreamHistoryEntry& Kept : Impl->Entries)
			{
				if (Impl->UnwrittenIds.Contains(Kept.Id))
				{
					Live.Add(Kept.Id);
				}
			}
			Impl->UnwrittenIds = MoveTemp(Live);
		}
	}
	if (Impl->FileLines + 1 >= 2 * Cap)
	{
		Impl->QueueRewrite();         // compaction when the file holds twice the cap (§6.5)
	}
	else
	{
		Impl->QueueAppend(Entry);
	}
	if (!Impl->CanWrite() && Impl->bMemoryOnly)
	{
		Impl->QueueStagedAppend(Entry);   // (instances) §10.5: a secondary stages its own entries
	}
	Impl->Changed.Broadcast();
}

TArray<FDreamHistoryEntry> FDreamReplHistory::Query(const FString& Filter, EDreamHistoryMatch Match, int32 Max) const
{
	TArray<FDreamHistoryEntry> Out;
	for (int32 Index = Impl->Entries.Num() - 1; Index >= 0 && (Max <= 0 || Out.Num() < Max); --Index)
	{
		const FDreamHistoryEntry& Entry = Impl->Entries[Index];
		const bool bMatch = Filter.IsEmpty()
			|| (Match == EDreamHistoryMatch::Prefix ? Entry.Text.StartsWith(Filter, ESearchCase::CaseSensitive)
				: Entry.Text.Contains(Filter, ESearchCase::IgnoreCase));
		if (bMatch)
		{
			Out.Add(Entry);
		}
	}
	Algo::Reverse(Out);              // newest last
	return Out;
}

void FDreamReplHistory::GetLatest(int32 Max, TArray<FString>& Out) const
{
	const int32 First = FMath::Max(0, Impl->Entries.Num() - Max);
	for (int32 Index = First; Index < Impl->Entries.Num(); ++Index)
	{
		Out.Add(Impl->Entries[Index].Text);
	}
}

void FDreamReplHistory::ImportConsoleHistoryOnce()
{
	check(IsInGameThread());
	if (Impl->bImported || !Impl->bOpen)
	{
		return;
	}
	// Read-only (D19): GetConsoleHistory loads ConsoleHistory.ini; Dream never calls AddConsoleHistoryEntry or writes it.
	TArray<FString> Python;
	TArray<FString> PythonRepl;
	IConsoleManager::Get().GetConsoleHistory(TEXT("Python"), Python);
	IConsoleManager::Get().GetConsoleHistory(TEXT("PythonREPL"), PythonRepl);

	TArray<FDreamHistoryEntry> Imported;
	auto AddImported = [&Imported](const FString& Text)
	{
		if (Text.TrimStartAndEnd().IsEmpty())
		{
			return;
		}
		FDreamHistoryEntry Entry;
		Entry.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Entry.Source = EDreamHistorySource::UeConsole;
		Entry.Text = Text;
		Imported.Add(MoveTemp(Entry));
	};
	for (const FString& Text : Python)
	{
		if (!DreamHistoryPrivate::IsOnlyPyPath(Text))
		{
			AddImported(Text);
		}
	}
	for (const FString& Text : PythonRepl)
	{
		AddImported(Text);
	}

	// Imported entries are older than every Dream entry; Normalize keeps the newest copy of a repeated text.
	Imported.Append(MoveTemp(Impl->Entries));
	Impl->Entries = MoveTemp(Imported);
	Impl->Normalize();
	Impl->bImported = true;          // the flag is set even when nothing was imported (§6.7)
	Impl->QueueRewrite();
}

void FDreamReplHistory::Clear()
{
	check(IsInGameThread());
	Impl->Entries.Reset();
	Impl->bClearPending = Impl->bClearPending || (!Impl->CanWrite() && Impl->bMemoryOnly);
	Impl->QueueRewrite();
	Impl->Changed.Broadcast();
}

void FDreamReplHistory::SetMemoryOnly(bool bInMemoryOnly)
{
	check(IsInGameThread());
	const bool bBecomesWritable = Impl->bMemoryOnly && !bInMemoryOnly;
	Impl->bMemoryOnly = bInMemoryOnly;
	if (!bBecomesWritable || !Impl->bOpen || Impl->FilePath.IsEmpty())
	{
		return;
	}
	// The file becomes this editor's (§6.5: only the primary writes it). What this editor added while memory-only, oldest
	// first (Add keeps them at the end).
	TArray<FDreamHistoryEntry> Added;
	for (const FDreamHistoryEntry& Entry : Impl->Entries)
	{
		if (Impl->UnwrittenIds.Contains(Entry.Id))
		{
			Added.Add(Entry);
		}
	}
	Impl->UnwrittenIds.Reset();
	const bool bCleared = Impl->bClearPending;
	Impl->bClearPending = false;

	// Another editor may have been the primary since the read (its entries appended, compacted, or a corrupt file replaced):
	// then the file is read again and only this editor's own entries join it. Nothing of the older read is written back,
	// and its corrupt state no longer counts (P3-out-01). An unchanged file gets just this editor's appends (A2-01).
	Impl->Pipe.WaitUntilEmpty();
	bool bEntriesChanged = false;
	if (Impl->bUnreadable || !DreamHistoryPrivate::IsSameFile(IFileManager::Get().GetStatData(*Impl->FilePath), Impl->ReadStat))
	{
		DreamHistoryPrivate::FHistoryRead Read = DreamHistoryPrivate::ReadHistory(Impl->FilePath);
		Impl->ReadStat = Read.Stat;
		Impl->bUnreadable = Read.bUnreadable;
		if (Impl->bUnreadable)
		{
			return;                   // kept in memory for this session, never replacing the file (OUT-03)
		}
		Impl->CorruptSource = Read.CorruptSource;
		Impl->bCorruptPending = Read.bCorrupt;
		Impl->Entries = bCleared ? TArray<FDreamHistoryEntry>() : MoveTemp(Read.Entries);
		Impl->Entries.Append(Added);
		Impl->FileLines = Read.Lines;
		Impl->bImported = Read.bImported;
		Impl->bRewriteDue = Read.bRewriteDue || bCleared;
		Impl->Normalize();
		bEntriesChanged = true;
	}
	if (Impl->bCorruptPending)
	{
		Impl->SetCorruptAside();
	}
	if (!Impl->bImported)
	{
		ImportConsoleHistoryOnce();   // the file has no import flag yet: import now, and write the file whole
		bEntriesChanged = true;
	}
	else if (Impl->bRewriteDue)
	{
		Impl->QueueRewrite();         // header first: appends to a header-less or torn file would read as corrupt later
	}
	else
	{
		Impl->QueueAppends(Added);
	}
	if (bEntriesChanged)
	{
		Impl->Changed.Broadcast();
	}
}

void FDreamReplHistory::SetStagingFile(const FString& Path)
{
	check(IsInGameThread());
	Impl->StagingPath = Path;
	if (Path.IsEmpty())
	{
		return;
	}
	// The entries this editor added before it knew it was the secondary (memory-only since engine init), oldest first.
	for (const FDreamHistoryEntry& Entry : Impl->Entries)
	{
		if (Impl->UnwrittenIds.Contains(Entry.Id))
		{
			Impl->QueueStagedAppend(Entry);
		}
	}
}

bool FDreamReplHistory::ReadStagedFile(const FString& Path, TArray<FDreamHistoryEntry>& OutEntries)
{
	OutEntries.Reset();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return false;
	}
	DreamHistoryPrivate::FHistoryRead Read = DreamHistoryPrivate::ReadHistory(Path);
	if (Read.bUnreadable || Read.bCorrupt)
	{
		return false;
	}
	OutEntries = MoveTemp(Read.Entries);
	return true;
}

void FDreamReplHistory::MergeEntries(TArray<FDreamHistoryEntry>&& Staged, TFunction<void(bool, int32)> OnWritten)
{
	check(IsInGameThread());
	TSet<FString> Known;
	for (const FDreamHistoryEntry& Entry : Impl->Entries)
	{
		Known.Add(Entry.Id);
	}
	int32 Joined = 0;
	for (FDreamHistoryEntry& Entry : Staged)
	{
		if (Entry.Id.IsEmpty() || Known.Contains(Entry.Id) || Entry.Text.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}
		Known.Add(Entry.Id);
		Impl->Entries.Add(MoveTemp(Entry));
		++Joined;
	}
	if (Joined == 0)
	{
		if (OnWritten)
		{
			OnWritten(true, 0);           // every staged entry is in this history already
		}
		return;
	}
	// Time order (imported console entries carry no time and stay first), then the usual dedupe and cap.
	Impl->Entries.StableSort([](const FDreamHistoryEntry& A, const FDreamHistoryEntry& B) { return A.TimeMs < B.TimeMs; });
	Impl->Normalize();
	// Written whole once when the file is this editor's; the answer comes once the rewrite landed, or false at once when
	// history.jsonl cannot be written now (unreadable, OUT-03: the entries stay in memory only).
	Impl->QueueRewrite([OnWritten, Joined](bool bOk)
	{
		if (OnWritten)
		{
			OnWritten(bOk, Joined);
		}
	});
	Impl->Changed.Broadcast();
}

FSimpleMulticastDelegate& FDreamReplHistory::OnChanged()
{
	return Impl->Changed;
}

void FDreamReplHistory::WaitForWrites() const
{
	Impl->Pipe.WaitUntilEmpty();
}

#undef LOCTEXT_NAMESPACE
