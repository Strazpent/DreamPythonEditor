// SPDX-License-Identifier: Apache-2.0
// FDreamThreadTable (TDD §6.1 "thread attribution", D24): native thread ID -> {name, origin run, PyThreadState*}, guarded
// by a reader-writer lock. A thread is the pair (native thread ID, PyThreadState*): the OS reuses native IDs, so a write
// whose PyThreadState* differs from the one the entry holds comes from a new thread, which gets a fresh entry (no origin
// run, a new generation, and the old name only when a label arrived after the old thread's last write).
// The write lock is held only for an insert or a field store; nothing under the lock logs or calls Python. Producers
// never read it: FDreamOutputDevice::Serialize stays lock-free, so a log line emitted on a thread that holds the write
// lock cannot deadlock (T-OUT-021). Output slice, internal.
// Threads: SetName and GetLabelStamp on any thread (_dream.thread_label); the rest on the DreamOutput ingest thread.
#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeRWLock.h"

#include <atomic>

class FDreamThreadTable
{
public:
	static constexpr int32 MaxEntries = 1024;     // §6.2 memory bounds, LRU

	/** _dream.thread_label: names the thread that has NativeThreadId now. The name is copied before the lock is taken. */
	void SetName(uint32 NativeThreadId, const FString& Name);

	/**
	 * Ingest, before a write is attributed: binds the write's (native ID, PyThreadState*) pair (§6.1). ThreadState 0 (a
	 * device record, a Dream line) matches whatever pair the entry holds. A different nonzero ThreadState under a known
	 * native ID means the OS gave the ID to a new thread: the entry forgets its origin run, and its name unless SetName
	 * ran after the previous thread's last write (LastWriteLabelStamp: the GetLabelStamp value the ingest read at that
	 * write), and takes a new generation. Returns the entry's generation; 0 for a thread with no entry.
	 */
	uint32 ObserveWrite(uint32 NativeThreadId, uint64 ThreadState, uint64 LastWriteLabelStamp);
	/** How many SetName calls have run (any thread). The ingest keeps the value it read at each thread's last write. */
	uint64 GetLabelStamp() const { return LabelStamp.load(std::memory_order_acquire); }

	/** The origin run of a thread (0 = known, outside runs). False when the thread has no entry or no origin yet. */
	bool FindOrigin(uint32 NativeThreadId, uint32& OutRunNumber);
	/** Records the run a thread was first seen in (§6.1: an unknown thread not alive at capture_begin). */
	void SetOrigin(uint32 NativeThreadId, uint32 RunNumber);

	/** The cached name and its change count (the ingest resends a name when the count moved). Empty when unknown. */
	FString GetName(uint32 NativeThreadId, uint32& OutNameVersion);

	void Reset();

private:
	struct FEntry
	{
		FString Name;
		uint32 NameVersion = 0;
		uint64 NameStamp = 0;           // LabelStamp after the SetName that set Name
		uint32 OriginRun = 0;
		bool bHasOrigin = false;
		uint64 ThreadState = 0;         // the PyThreadState* key of the thread that has the ID; 0 = none seen yet
		uint32 Generation = 0;          // unique per (native ID, PyThreadState*) pair the table has seen
		uint64 LastUse = 0;
	};

	/** Write lock held. Evicts the least recently used entry when the table is full. */
	FEntry& FindOrAddLocked(uint32 NativeThreadId);

	FRWLock Lock;
	TMap<uint32, FEntry> Entries;
	uint64 UseCounter = 0;
	uint32 GenerationCounter = 0;
	std::atomic<uint64> LabelStamp{0};
};
