// SPDX-License-Identifier: Apache-2.0
// FDreamThreadTable (TDD §6.1, D24). See DreamThreadTable.h.
#include "Output/DreamThreadTable.h"

void FDreamThreadTable::SetName(uint32 NativeThreadId, const FString& Name)
{
	FString Copy = Name;    // read before the lock (§6.1)
	FWriteScopeLock WriteLock(Lock);
	FEntry& Entry = FindOrAddLocked(NativeThreadId);
	Entry.NameStamp = LabelStamp.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (!Entry.Name.Equals(Copy, ESearchCase::CaseSensitive))
	{
		Entry.Name = MoveTemp(Copy);
		++Entry.NameVersion;
	}
}

uint32 FDreamThreadTable::ObserveWrite(uint32 NativeThreadId, uint64 ThreadState, uint64 LastWriteLabelStamp)
{
	{
		FReadScopeLock ReadLock(Lock);
		const FEntry* Entry = Entries.Find(NativeThreadId);
		if (Entry == nullptr ? ThreadState == 0 : (ThreadState == 0 || Entry->ThreadState == ThreadState))
		{
			return Entry != nullptr ? Entry->Generation : 0;     // the common case: the pair is already known
		}
	}
	FWriteScopeLock WriteLock(Lock);
	FEntry& Entry = FindOrAddLocked(NativeThreadId);
	if (Entry.ThreadState == 0)
	{
		Entry.ThreadState = ThreadState;     // the first pair of an entry a label or a device record made
	}
	else if (ThreadState != 0 && Entry.ThreadState != ThreadState)
	{
		// §6.1: the OS reused the native ID for a new thread. A label that came after the old thread's last write names
		// the new one (the runner labels the threads alive at capture_begin and at run end); an older one does not.
		Entry.ThreadState = ThreadState;
		Entry.Generation = ++GenerationCounter;
		Entry.OriginRun = 0;
		Entry.bHasOrigin = false;
		if (Entry.NameStamp <= LastWriteLabelStamp && !Entry.Name.IsEmpty())
		{
			Entry.Name.Reset();
			++Entry.NameVersion;
		}
	}
	return Entry.Generation;
}

bool FDreamThreadTable::FindOrigin(uint32 NativeThreadId, uint32& OutRunNumber)
{
	FReadScopeLock ReadLock(Lock);
	const FEntry* Entry = Entries.Find(NativeThreadId);
	if (Entry == nullptr || !Entry->bHasOrigin)
	{
		return false;
	}
	OutRunNumber = Entry->OriginRun;
	return true;
}

void FDreamThreadTable::SetOrigin(uint32 NativeThreadId, uint32 RunNumber)
{
	FWriteScopeLock WriteLock(Lock);
	FEntry& Entry = FindOrAddLocked(NativeThreadId);
	Entry.OriginRun = RunNumber;
	Entry.bHasOrigin = true;
}

FString FDreamThreadTable::GetName(uint32 NativeThreadId, uint32& OutNameVersion)
{
	FReadScopeLock ReadLock(Lock);
	const FEntry* Entry = Entries.Find(NativeThreadId);
	OutNameVersion = Entry ? Entry->NameVersion : 0;
	return Entry ? Entry->Name : FString();
}

void FDreamThreadTable::Reset()
{
	FWriteScopeLock WriteLock(Lock);
	Entries.Reset();
}

FDreamThreadTable::FEntry& FDreamThreadTable::FindOrAddLocked(uint32 NativeThreadId)
{
	if (FEntry* Existing = Entries.Find(NativeThreadId))
	{
		Existing->LastUse = ++UseCounter;
		return *Existing;
	}
	if (Entries.Num() >= MaxEntries)
	{
		uint32 OldestId = 0;
		uint64 OldestUse = TNumericLimits<uint64>::Max();
		for (const TPair<uint32, FEntry>& Pair : Entries)
		{
			if (Pair.Value.LastUse < OldestUse)
			{
				OldestUse = Pair.Value.LastUse;
				OldestId = Pair.Key;
			}
		}
		Entries.Remove(OldestId);
	}
	FEntry& Added = Entries.Add(NativeThreadId);
	Added.LastUse = ++UseCounter;
	Added.Generation = ++GenerationCounter;      // a recreated entry never matches a tag the ingest keeps for an older one
	return Added;
}
