// SPDX-License-Identifier: Apache-2.0
// REPL history persisted per project (TDD §6.5 history, REPL-02, D17, SEC-06): <root>/Repl/history.jsonl, JSON lines
// through dream::json, single-line appends on a UE::Tasks::FPipe "DreamReplHistory", cap 10,000, dedupe, read-only import
// of ConsoleHistory.ini once. Game thread API; file I/O on the pipe. Body: DreamReplHistory.cpp (output slice; the
// second-instance staging and merge of §10.5 are the instances group's).
// §10.5 "Ownership of the shared files": only the primary appends to and compacts history.jsonl. A secondary reads it into
// memory and appends its own entries to its namespace's <sec>/repl-history.jsonl (SetStagingFile); the primary merges a
// staged file (ReadStagedFile, MergeEntries) before it deletes that namespace: in time order, deduplicated by id.
#pragma once

#include "CoreMinimal.h"

enum class EDreamHistorySource : uint8
{
	Repl,
	Console,
	UeConsole,          // imported from ConsoleHistory.ini
};

enum class EDreamHistoryMatch : uint8
{
	Prefix,
	Substring,          // Ctrl+R (M2)
};

struct FDreamHistoryEntry
{
	FString Id;         // UUID
	int64 TimeMs = 0;   // unix ms
	EDreamHistorySource Source = EDreamHistorySource::Repl;
	FString Text;
};

class DREAMEDITOR_API FDreamReplHistory
{
public:
	static constexpr int32 Cap = 10000;

	FDreamReplHistory();
	~FDreamReplHistory();

	/** Loads <Root>/Repl/history.jsonl (a torn last line is dropped; a corrupt file is renamed .corrupt, §6.7). */
	void Open(const FString& StorageRoot);
	void Close();

	/** Adds an entry unless it equals the newest one; async append. */
	void Add(const FString& Text, EDreamHistorySource Source);
	/** Newest last. Filter empty = all. */
	TArray<FDreamHistoryEntry> Query(const FString& Filter, EDreamHistoryMatch Match, int32 Max) const;
	/** The newest Max texts, oldest first (FDreamConsoleExecutor::GetExecHistory). */
	void GetLatest(int32 Max, TArray<FString>& Out) const;
	/** IConsoleManager::GetConsoleHistory("Python") and ("PythonREPL"), once per project; never writes ConsoleHistory.ini. */
	void ImportConsoleHistoryOnce();
	/** Palette command Clear REPL history. */
	void Clear();

	/**
	 * (slice A choice, A36) History is memory-only until this editor is known to be the primary instance: the module opens it
	 * memory-only at engine init and turns that off once the first tab spawn's FDreamJournal::Open returns Primary (OUT-05,
	 * §10.3 step B, §10.5 "At tab spawn"). Appends, rewrites and Clear then never touch the file, and a corrupt file is not
	 * renamed. Turning it off on an open history appends the entries added meanwhile; when another editor wrote the file
	 * since it was read, the file is read again first and keeps everything it holds (§6.5, P3-out-01). It is written whole
	 * only when it needs that (new, torn or corrupt, the import flag), and never replaced when it cannot be read.
	 */
	void SetMemoryOnly(bool bInMemoryOnly);

	/**
	 * §10.5: a secondary's own entries go to Path (<sec>/repl-history.jsonl, JSON lines with the history.jsonl header)
	 * while history.jsonl stays memory-only; the entries this editor added before the call are appended first. An empty
	 * Path stops staging. Game thread; the writes run on the pipe.
	 */
	void SetStagingFile(const FString& Path);
	/** Any thread: the entries of a staged file (a torn last line dropped); false when it cannot be read or is corrupt. */
	static bool ReadStagedFile(const FString& Path, TArray<FDreamHistoryEntry>& OutEntries);
	/**
	 * §10.5 Merge: another instance's staged entries join this history in time order, deduplicated by id (the newest copy
	 * of a repeated text wins, as on load); the file is rewritten once when this editor owns it. OnWritten(kept, joined)
	 * tells whether they reached history.jsonl: true once the rewrite landed (on the history's pipe thread) or when every
	 * entry was here already; false at once when this editor cannot write the file (memory-only, unreadable, OUT-03).
	 * Game thread; OnWritten may run on any thread.
	 */
	void MergeEntries(TArray<FDreamHistoryEntry>&& Staged, TFunction<void(bool /*bKept*/, int32 /*Joined*/)> OnWritten);

	/** After every Add, Clear and load (the REPL tab's history list). Game thread. */
	FSimpleMulticastDelegate& OnChanged();

	/** Test hook: waits for the queued file work (T-REPL-007). */
	void WaitForWrites() const;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
