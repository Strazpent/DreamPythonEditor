// SPDX-License-Identifier: Apache-2.0
// FDreamLocalHistory (TDD §10.4). The game thread only queues work and receives results; the store (index, index writer,
// its own fenced FileOps) lives on the journal writer thread, and each task is a small state machine whose every step is
// one background slice of about 50 ms at most (§10.2.1): the hash in 1M-unit slices, one 1 MiB zlib chunk per slice, the
// index commit, an index rotation step, a retention listing of 16 object folders, a batch of tombstones or deletes.
#include "Persistence/DreamLocalHistory.h"

#include "Containers/Queue.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Compression.h"
#include "Misc/DateTime.h"
#include "Diagnostics/DreamAudit.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamWin32FileOps.h"
#include "Run/DreamDeferral.h"
#include "dream/Checksum.h"
#include "dream/HistoryFormat.h"
#include "dream/Utf.h"

#include <algorithm>

namespace DreamLocalHistoryLocal
{
	using dream::history::EntryKind;

	constexpr int64 HashUnitsPerSlice = 1 << 20;          // UTF-16 units converted and hashed per slice
	constexpr int32 ObjectDirsPerSlice = 16;              // retention and Clear list 16 of the 256 objects/<aa> folders
	constexpr int32 RecordsPerSlice = 1000;               // tombstones committed per slice
	constexpr int32 DeletesPerSlice = 200;

	int64 NowUtcMs()
	{
		return static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds());
	}

	FDateTime FromUtcMs(int64 Ms)
	{
		return FDateTime(1970, 1, 1) + FTimespan::FromMilliseconds(static_cast<double>(Ms));
	}

	EntryKind ToCoreKind(EDreamHistoryKind Kind)
	{
		switch (Kind)
		{
		case EDreamHistoryKind::Run: return EntryKind::Run;
		case EDreamHistoryKind::Reload: return EntryKind::Reload;
		case EDreamHistoryKind::Closed: return EntryKind::Closed;
		case EDreamHistoryKind::Recovered: return EntryKind::Recovered;
		case EDreamHistoryKind::Discarded: return EntryKind::Discarded;
		case EDreamHistoryKind::Restore: return EntryKind::Restore;
		default: return EntryKind::Save;
		}
	}

	EDreamHistoryKind FromCoreKind(EntryKind Kind)
	{
		switch (Kind)
		{
		case EntryKind::Run: return EDreamHistoryKind::Run;
		case EntryKind::Reload: return EDreamHistoryKind::Reload;
		case EntryKind::Closed: return EDreamHistoryKind::Closed;
		case EntryKind::Recovered: return EDreamHistoryKind::Recovered;
		case EntryKind::Discarded: return EDreamHistoryKind::Discarded;
		case EntryKind::Restore: return EDreamHistoryKind::Restore;
		default: return EDreamHistoryKind::Save;
		}
	}

	FDreamLocalHistoryEntry ToGameEntry(const dream::history::Entry& E)
	{
		FDreamLocalHistoryEntry Out;
		Out.Id = DreamText::Utf8ToFString(E.Id);
		FGuid::Parse(DreamText::Utf8ToFString(E.Uuid), Out.Buffer);
		Out.Path = DreamText::Utf8ToFString(E.Path);
		Out.DisplayName = DreamText::Utf8ToFString(E.Display);
		Out.Kind = FromCoreKind(E.Kind);
		for (const uint32 Run : E.Runs)
		{
			Out.Runs.Add(static_cast<int32>(Run));
		}
		Out.Utc = FromUtcMs(E.UtcMs);
		Out.BlobSha256 = DreamText::Utf8ToFString(E.Blob);
		Out.SizeBytes = E.Size;
		Out.Lines = static_cast<int32>(E.Lines);
		return Out;
	}

	std::string HexDir(int32 Index)
	{
		char Buffer[3];
		static constexpr char Digits[] = "0123456789abcdef";
		Buffer[0] = Digits[(Index >> 4) & 0xF];
		Buffer[1] = Digits[Index & 0xF];
		Buffer[2] = '\0';
		return Buffer;
	}
}

/** The writer-thread state of one history (§10.4). Only the journal writer thread touches the fields below Completions. */
struct FDreamLocalHistory::FStore
{
	/** Game-thread results, drained by the history's FDreamDeferral ticker. */
	TQueue<TFunction<void()>, EQueueMode::Mpsc> Completions;
	std::atomic<bool> bAlive { true };
	std::atomic<int64> MaxAgeMs { dream::history::RetentionLimits().MaxAgeMs };
	std::atomic<int64> MaxBytes { dream::history::RetentionLimits().MaxBytes };
	std::atomic<int64> ClockOffsetMs { 0 };

	// Writer thread
	uint64 SessionId = 0;
	FString Dir;
	std::string DirUtf8;
	TUniquePtr<FDreamWin32FileOps> Ops;
	TUniquePtr<dream::journal::Writer> IndexWriter;
	dream::history::Index Index;
	bool bReady = false;
	bool bReadOnly = false;                     // a newer index format (§10.6): read, never written
	/** persist-r2-M2: an index file could not be read; nothing is written above it, the index is read again at RetryAt. */
	bool bBlocked = false;
	double RetryAt = 0.0;
	std::atomic<bool> bPausedNow { false };     // bBlocked for the game thread (the notice)
	std::atomic<bool> bProbeQueued { false };   // review r3: the game thread's re-read while paused is in the queue

	~FStore()
	{
		IndexWriter.Reset();                    // closes its handle through Ops, which outlives it
		Ops.Reset();
	}

	void Complete(TFunction<void()> Work)
	{
		if (bAlive.load(std::memory_order_acquire))
		{
			Completions.Enqueue(MoveTemp(Work));
		}
	}

	int64 Now() const
	{
		return DreamLocalHistoryLocal::NowUtcMs() + ClockOffsetMs.load(std::memory_order_acquire);
	}

	std::string ObjectsDir(int32 Folder) const
	{
		return dream::journal::JoinPath(dream::journal::JoinPath(DirUtf8, "objects"), DreamLocalHistoryLocal::HexDir(Folder));
	}

	std::string BlobPath(const std::string& Sha) const
	{
		return dream::journal::JoinPath(DirUtf8, dream::history::BlobRelativePath(Sha));
	}

	/**
	 * The store for this journal session: its own FileOps behind the session's fence, the index read with fallback, and a
	 * new index generation (a session never appends to a generation another process or session wrote, §10.2.3). False when
	 * the history cannot be written now (the caller fails its request; the next one tries again).
	 */
	bool Prepare(FDreamIoContext& Context)
	{
		if (bReady && SessionId == Context.SessionId && Dir == Context.HistoryDir)
		{
			return !bReadOnly;
		}
		if (bBlocked && SessionId == Context.SessionId && Dir == Context.HistoryDir && FPlatformTime::Seconds() < RetryAt)
		{
			return false;   // still paused: the index is read again at RetryAt
		}
		IndexWriter.Reset();
		Ops = MakeUnique<FDreamWin32FileOps>(Context.Fence);
		SessionId = Context.SessionId;
		Dir = Context.HistoryDir;
		DirUtf8 = FDreamWin32FileOps::ToUtf8Path(Dir);
		bReady = false;
		bReadOnly = false;
		if (Dir.IsEmpty() || !Ops->MakeDirs(DirUtf8))
		{
			return false;
		}
		dream::history::RecoveredIndex Read = dream::history::ReadIndex(*Ops, DirUtf8, true);
		Index = MoveTemp(Read.Entries);
		if (Read.bNewerFormat)
		{
			UE_LOG(LogDream, Warning, TEXT("Local history in %s was written by a newer Dream Python Editor; it is read only in this session."), *Dir);
			bReady = true;
			bReadOnly = true;
			bBlocked = false;   // review r3: a held index that proves a newer format is the read-only latch, not a pause
			bPausedNow.store(false, std::memory_order_release);
			return false;
		}
		if (Read.bSkippedUnreadable)
		{
			// persist-M2, persist-r2-M2 (§10.2.4, §10.11): an index generation could not be read even after the FileOps retries
			// (a scanner or a backup tool). It stays where it is and nothing is written above it (the next read would delete it
			// under a newer generation) and no retention runs (its entries' blobs would look unreferenced). History is paused,
			// not read only: the index is read again every second, and an AddEntry waits for it at the front of the queue.
			if (!bBlocked)
			{
				UE_LOG(LogDream, Warning, TEXT("A local history index file in %s could not be read; history is paused until it can be."), *Dir);
			}
			bBlocked = true;
			bPausedNow.store(true, std::memory_order_release);
			RetryAt = FPlatformTime::Seconds() + 1.0;
			return false;
		}
		if (bBlocked)
		{
			UE_LOG(LogDream, Log, TEXT("The local history index in %s can be read again; history resumes."), *Dir);
			bBlocked = false;
			bPausedNow.store(false, std::memory_order_release);
		}
		if (Read.bFallbackUsed || Read.bSalvageUsed)
		{
			UE_LOG(LogDream, Log, TEXT("Local history index in %s was read from its previous generation or salvaged."), *Dir);
		}
		uint64 Highest = Read.Generation;
		std::vector<std::string> Names;
		Ops->List(DirUtf8, Names);
		for (const std::string& Name : Names)
		{
			uint64 G = 0;
			const size_t Shi = Name.find(".shi");
			if (Shi != std::string::npos && dream::journal::ParseLogFileName(dream::history::IndexLogFormat, Name.substr(0, Shi + 4), G))
			{
				Highest = FMath::Max(Highest, G);
			}
		}
		dream::journal::FileHeader Proto;
		Proto.InstancePid = FPlatformProcess::GetCurrentProcessId();
		IndexWriter = MakeUnique<dream::journal::Writer>(*Ops, DirUtf8, Proto, dream::history::IndexLogFormat);
		dream::json::Value Ext = dream::json::Value::MakeObject();
		Ext.Set("kind", dream::json::Value::MakeString("history-index"));
		IndexWriter->SetHeaderExtFields(MoveTemp(Ext));
		if (!IndexWriter->StartGeneration(Highest + 1, Index.Checkpoint(), Now(), FPlatformTime::Seconds()))
		{
			UE_LOG(LogDream, Warning, TEXT("Local history could not start its index in %s: %s"), *Dir,
				*DreamText::Utf8ToFString(IndexWriter->LastError()));
			IndexWriter.Reset();
			return false;
		}
		bReady = true;
		return true;
	}

	/** Commits index records; the in-memory index follows only once they are durable. */
	bool CommitIndex(const std::vector<dream::journal::Record>& Records)
	{
		if (!IndexWriter.IsValid() || !IndexWriter->Commit(Records))
		{
			UE_LOG(LogDream, Warning, TEXT("Local history index commit failed: %s"),
				IndexWriter.IsValid() ? *DreamText::Utf8ToFString(IndexWriter->LastError()) : TEXT("no index"));
			bReady = false;   // the next request starts a new generation (the cut-back state of the failed one is unknown)
			return false;
		}
		for (const dream::journal::Record& R : Records)
		{
			Index.Apply(R);
		}
		return true;
	}

	/** One unit of an index rotation (sliced like the journal's, §10.2.3); true while it is not finished. */
	bool StepIndexRotation(bool bForce)
	{
		if (!IndexWriter.IsValid())
		{
			return false;
		}
		if (!IndexWriter->IsRotating())
		{
			if (!bForce && !IndexWriter->NeedsRotation(FPlatformTime::Seconds()))
			{
				return false;
			}
			if (!IndexWriter->BeginRotation(Index.Checkpoint(), Now()))
			{
				UE_LOG(LogDream, Warning, TEXT("Local history index rotation failed: %s"), *DreamText::Utf8ToFString(IndexWriter->LastError()));
				return false;
			}
			return true;
		}
		const dream::journal::RotationStep Step = IndexWriter->StepRotation(dream::journal::RotationSliceBytes);
		if (Step == dream::journal::RotationStep::More)
		{
			return true;
		}
		if (Step == dream::journal::RotationStep::Failed || !IndexWriter->FinishRotation(FPlatformTime::Seconds()))
		{
			UE_LOG(LogDream, Warning, TEXT("Local history index rotation failed: %s"), *DreamText::Utf8ToFString(IndexWriter->LastError()));
		}
		return false;
	}
};

namespace DreamLocalHistoryLocal
{
	using FStore = FDreamLocalHistory::FStore;
	using FStorePtr = TSharedPtr<FStore, ESPMode::ThreadSafe>;

	/** AddEntry: hash, deduplicate, blob in chunks, index (with run merging), maybe an index rotation. */
	struct FAddTask
	{
		enum class EPhase : uint8 { Prepare, Hash, Blob, Compress, FinishBlob, Index, Rotate, Done };

		FStorePtr Store;
		FDreamHistoryNote Note;
		TFunction<void(bool)> OnDone;
		EPhase Phase = EPhase::Prepare;
		dream::Sha256 Sha;
		uint32 Crc = 0;
		std::string Utf8;
		int64 NextUnit = 0;
		int64 Lines = 1;
		std::string BlobHex;
		std::string BlobPath;
		std::string TempPath;
		dream::journal::FileOps::Handle Temp = dream::journal::FileOps::InvalidHandle;
		size_t NextChunk = 0;
		int64 SizeBytes = 0;

		~FAddTask()
		{
			if (Temp != dream::journal::FileOps::InvalidHandle && Store.IsValid() && Store->Ops.IsValid())
			{
				Store->Ops->Close(Temp);
				Store->Ops->Remove(TempPath);
			}
		}

		bool Finish(bool bOk)
		{
			if (Temp != dream::journal::FileOps::InvalidHandle)
			{
				Store->Ops->Close(Temp);
				Store->Ops->Remove(TempPath);
				Temp = dream::journal::FileOps::InvalidHandle;
			}
			if (OnDone)
			{
				TFunction<void(bool)> Callback = MoveTemp(OnDone);
				Store->Complete([Callback, bOk]() { Callback(bOk); });
			}
			Phase = EPhase::Done;
			return false;
		}

		bool Step(FDreamIoContext& Context)
		{
			switch (Phase)
			{
			case EPhase::Prepare:
				if (!Store->Prepare(Context))
				{
					if (Store->bBlocked && !Context.bExiting)
					{
						// persist-r2-M2: history is paused (an index file another program holds). This entry waits at the front of
						// the queue and is written once the index reads again (a closed scratch tab's text is in it only); the wait
						// serves the due commits (FScopedRetryWait).
						FDreamWin32FileOps::WaitLikeARetry(static_cast<float>(FMath::Clamp(Store->RetryAt - FPlatformTime::Seconds(), 0.05, 1.0)));
						return true;
					}
					return Finish(false);
				}
				Phase = EPhase::Hash;
				return true;
			case EPhase::Hash:
				return StepHash();
			case EPhase::Blob:
				return StepBlob();
			case EPhase::Compress:
				return StepCompress();
			case EPhase::FinishBlob:
				return StepFinishBlob();
			case EPhase::Index:
				return StepIndex();
			case EPhase::Rotate:
				// Review r2: no index rotation starts or goes on during the exit drain (the entry itself is durable already).
				if (!Context.bExiting && Store->StepIndexRotation(false))
				{
					return true;
				}
				return Finish(true);
			default:
				return false;
			}
		}

		/** The UTF-8 text, its SHA-256 (the blob's address) and its CRC32C, 1M UTF-16 units per slice. */
		bool StepHash()
		{
			const int64 Length = Note.Text.Length();
			const int64 End = FMath::Min(Length, NextUnit + HashUnitsPerSlice);
			std::u16string Units;
			Note.Text.CopyRange(NextUnit, End - NextUnit, Units);
			if (End < Length && !Units.empty() && dream::utf::IsHighSurrogate(Units.back()))
			{
				Units.pop_back();   // a pair split by the slice: its first half waits for the next one
			}
			std::string Piece;
			dream::utf::Utf16ToUtf8(Units, Piece);
			Sha.Update(Piece);
			Crc = dream::Crc32c(Piece, Crc);
			Lines += static_cast<int64>(std::count(Piece.begin(), Piece.end(), '\n'));
			Utf8.append(Piece);
			NextUnit += static_cast<int64>(Units.size());
			if (Units.empty() && NextUnit < Length)
			{
				NextUnit = Length;   // cannot happen with a well-formed snapshot; never loop for ever
			}
			if (NextUnit < Length)
			{
				return true;
			}
			BlobHex = dream::Sha256::ToHex(Sha.Final());
			SizeBytes = static_cast<int64>(Utf8.size());
			BlobPath = Store->BlobPath(BlobHex);
			Phase = Store->Ops->Exists(BlobPath) ? EPhase::Index : EPhase::Blob;   // deduplicated: written once
			return true;
		}

		bool StepBlob()
		{
			const std::string Folder = BlobPath.substr(0, BlobPath.find_last_of('/'));
			TempPath = BlobPath + ".tmp";
			if (!Store->Ops->MakeDirs(Folder))
			{
				return Finish(false);
			}
			if (Store->Ops->Exists(TempPath))
			{
				Store->Ops->Remove(TempPath);   // a temp file of an earlier session that never finished
			}
			if (!Store->Ops->CreateExclusive(TempPath, Temp))
			{
				Temp = dream::journal::FileOps::InvalidHandle;
				UE_LOG(LogDream, Warning, TEXT("Local history could not write %s: %s"), *DreamText::Utf8ToFString(TempPath),
					*DreamText::Utf8ToFString(Store->Ops->LastError()));
				return Finish(false);
			}
			dream::history::BlobHeader Header;
			Header.RawLength = Utf8.size();
			Header.RawCrc32c = Crc;
			std::string Bytes;
			dream::history::EncodeBlobHeader(Header, Bytes);
			if (!Store->Ops->Append(Temp, Bytes))
			{
				return Finish(false);
			}
			Phase = EPhase::Compress;
			return true;
		}

		/** One 1 MiB chunk per slice (zlib through FCompression, §10.4). */
		bool StepCompress()
		{
			if (NextChunk >= Utf8.size())
			{
				Phase = EPhase::FinishBlob;
				return true;
			}
			const size_t RawLength = FMath::Min<size_t>(dream::history::BlobChunkBytes, Utf8.size() - NextChunk);
			const int32 Bound = FCompression::CompressMemoryBound(NAME_Zlib, static_cast<int32>(RawLength));
			std::string Compressed(static_cast<size_t>(Bound), '\0');
			int32 CompressedSize = Bound;
			if (!FCompression::CompressMemory(NAME_Zlib, Compressed.data(), CompressedSize, Utf8.data() + NextChunk, static_cast<int32>(RawLength)))
			{
				return Finish(false);
			}
			Compressed.resize(static_cast<size_t>(CompressedSize));
			std::string Chunk;
			dream::history::AppendBlobChunk(Compressed, static_cast<uint32>(RawLength), Chunk);
			if (!Store->Ops->Append(Temp, Chunk))
			{
				return Finish(false);
			}
			NextChunk += RawLength;
			return true;
		}

		bool StepFinishBlob()
		{
			const bool bFlushed = Store->Ops->Flush(Temp);
			Store->Ops->Close(Temp);
			Temp = dream::journal::FileOps::InvalidHandle;
			if (!bFlushed || !Store->Ops->RenameNew(TempPath, BlobPath))
			{
				const bool bExists = Store->Ops->Exists(BlobPath);
				Store->Ops->Remove(TempPath);
				if (!bExists)
				{
					return Finish(false);
				}
			}
			Utf8.clear();
			Utf8.shrink_to_fit();
			Phase = EPhase::Index;
			return true;
		}

		bool StepIndex()
		{
			dream::history::Entry E;
			E.Id = DreamText::ToUtf8(DreamText::GuidToHex(FGuid::NewGuid()));
			E.Uuid = DreamText::ToUtf8(Note.BufferUuid);
			E.Path = DreamText::ToUtf8(Note.Path);
			E.Display = DreamText::ToUtf8(Note.DisplayName);
			E.Kind = ToCoreKind(Note.Kind);
			if (Note.Kind == EDreamHistoryKind::Run && Note.RunNumber > 0)
			{
				E.Runs.push_back(static_cast<uint32>(Note.RunNumber));
			}
			E.UtcMs = Note.UtcMs > 0 ? Note.UtcMs : Store->Now();
			E.Blob = BlobHex;
			E.Size = SizeBytes;
			E.Lines = Lines;
			if (Note.Kind == EDreamHistoryKind::Recovered)
			{
				// §10.7.1: a pending text is added once, not again at every start that carries it.
				const dream::history::Entry* Newest = Store->Index.NewestOf(E.Uuid);
				if (Newest != nullptr && Newest->Kind == EntryKind::Recovered && Newest->Blob == E.Blob)
				{
					return Finish(true);
				}
			}
			E = dream::history::PlanRunEntry(Store->Index, MoveTemp(E));
			if (!Store->CommitIndex({ dream::history::MakeEntry(E) }))
			{
				return Finish(false);
			}
			Phase = EPhase::Rotate;
			return true;
		}
	};

	FDreamIoSlice MakeAddSlice(const FStorePtr& Store, FDreamHistoryNote&& Note, TFunction<void(bool)> OnDone)
	{
		TSharedPtr<FAddTask, ESPMode::ThreadSafe> Task = MakeShared<FAddTask, ESPMode::ThreadSafe>();
		Task->Store = Store;
		Task->Note = MoveTemp(Note);
		Task->OnDone = MoveTemp(OnDone);
		return [Task](FDreamIoContext& Context) { return Task->Step(Context); };
	}

	/** Lists the blobs of 16 objects/<aa> folders per slice: name, size, write time; stale temp files older than a day go. */
	struct FBlobScan
	{
		int32 NextDir = 0;
		std::vector<dream::history::BlobInfo> Blobs;

		/** True while folders remain. */
		bool Step(FStore& Store, bool bRemoveAll, int32& InOutRemoved)
		{
			const int32 Last = FMath::Min(256, NextDir + ObjectDirsPerSlice);
			for (; NextDir < Last; ++NextDir)
			{
				const std::string Folder = Store.ObjectsDir(NextDir);
				std::vector<std::string> Names;
				if (!Store.Ops->List(Folder, Names))
				{
					continue;
				}
				for (const std::string& Name : Names)
				{
					const std::string Path = dream::journal::JoinPath(Folder, Name);
					std::string Sha;
					if (bRemoveAll)
					{
						InOutRemoved += Store.Ops->Remove(Path) ? 1 : 0;   // Clear: every blob and every temp file
						continue;
					}
					int64_t Written = 0;
					const bool bHasTime = Store.Ops->ModifiedUtcMs(Path, Written);
					if (dream::history::ParseBlobFileName(Name, Sha))
					{
						int64 Bytes = 0;
						Store.Ops->FileSize(Path, Bytes);
						Blobs.push_back(dream::history::BlobInfo{ Sha, Bytes, bHasTime ? Written : 0 });
					}
					else if (Name.size() > 6 && Name.compare(Name.size() - 6, 6, ".z.tmp") == 0 && bHasTime
						&& Store.Now() - Written > dream::history::DayMs)
					{
						Store.Ops->Remove(Path);   // a blob that never finished, from a session that ended mid-write
					}
				}
			}
			return NextDir < 256;
		}
	};

	/** Retention (§10.4 steps 1-4) and Clear share the scan and the index rotation. */
	struct FMaintenanceTask
	{
		enum class EPhase : uint8 { Prepare, Clear, Scan, Plan, Tombstone, Sweep, Rotate, Done };

		FStorePtr Store;
		bool bClear = false;
		TFunction<void(int32, int32)> OnRetentionDone;
		TFunction<void(bool, int32, int32)> OnClearDone;   // ok, entries before, blobs deleted
		int32 EntriesBefore = 0;
		EPhase Phase = EPhase::Prepare;
		FBlobScan Scan;
		dream::history::RetentionPlan Plan;
		size_t NextTombstone = 0;
		size_t NextSweep = 0;
		int32 Swept = 0;
		bool bOk = true;
		bool bRotationStarted = false;

		bool Finish()
		{
			Phase = EPhase::Done;
			if (bClear && OnClearDone)
			{
				TFunction<void(bool, int32, int32)> Callback = MoveTemp(OnClearDone);
				const bool bResult = bOk;
				const int32 Entries = EntriesBefore;
				const int32 Blobs = Swept;
				Store->Complete([Callback, bResult, Entries, Blobs]() { Callback(bResult, Entries, Blobs); });
			}
			else if (!bClear && OnRetentionDone)
			{
				TFunction<void(int32, int32)> Callback = MoveTemp(OnRetentionDone);
				const int32 Tombstoned = bOk ? static_cast<int32>(Plan.Tombstones.size()) : 0;
				const int32 SweptCount = Swept;
				Store->Complete([Callback, Tombstoned, SweptCount]() { Callback(Tombstoned, SweptCount); });
			}
			return false;
		}

		bool Step(FDreamIoContext& Context)
		{
			if (Context.bExiting)
			{
				// Review r1: the writer is stopping; the queued AddEntry work gets the rest of the shutdown budget. Retention
				// and Clear stop between slices (a Clear's CLEAR is already durable; its leftover blobs are swept later).
				bOk = false;
				return Finish();
			}
			switch (Phase)
			{
			case EPhase::Prepare:
				if (!Store->Prepare(Context))
				{
					bOk = false;
					return Finish();
				}
				if (bClear)
				{
					EntriesBefore = static_cast<int32>(Store->Index.Entries().size());
					// §10.4 Clear: CLEAR first, so a crash mid-way leaves an empty index and orphan blobs the sweep removes.
					if (!Store->CommitIndex({ dream::history::MakeClear(Store->Now()) }))
					{
						bOk = false;
						return Finish();
					}
					Phase = EPhase::Clear;
				}
				else
				{
					Phase = EPhase::Scan;
				}
				return true;
			case EPhase::Clear:
				if (Scan.Step(*Store, true, Swept))
				{
					return true;
				}
				Phase = EPhase::Rotate;
				return true;
			case EPhase::Scan:
			{
				int32 Unused = 0;
				if (Scan.Step(*Store, false, Unused))
				{
					return true;
				}
				Phase = EPhase::Plan;
				return true;
			}
			case EPhase::Plan:
			{
				dream::history::RetentionLimits Limits;
				Limits.MaxAgeMs = Store->MaxAgeMs.load(std::memory_order_acquire);
				Limits.MaxBytes = Store->MaxBytes.load(std::memory_order_acquire);
				Plan = dream::history::PlanRetention(Store->Index, Scan.Blobs, Store->Now(), Limits);
				Phase = EPhase::Tombstone;
				return true;
			}
			case EPhase::Tombstone:
			{
				if (NextTombstone >= Plan.Tombstones.size())
				{
					Phase = EPhase::Sweep;
					return true;
				}
				std::vector<dream::journal::Record> Records;
				const size_t End = FMath::Min(Plan.Tombstones.size(), NextTombstone + static_cast<size_t>(RecordsPerSlice));
				for (; NextTombstone < End; ++NextTombstone)
				{
					Records.push_back(dream::history::MakeTombstone(Plan.Tombstones[NextTombstone]));
				}
				if (!Store->CommitIndex(Records))
				{
					bOk = false;
					return Finish();   // nothing is swept before its tombstones are durable
				}
				return true;
			}
			case EPhase::Sweep:
			{
				// Step 3: only blobs no live entry references (checked again: an entry added since the plan keeps its blob).
				const size_t End = FMath::Min(Plan.Sweep.size(), NextSweep + static_cast<size_t>(DeletesPerSlice));
				for (; NextSweep < End; ++NextSweep)
				{
					const std::string& Sha = Plan.Sweep[NextSweep];
					if (!Store->Index.References(Sha) && Store->Ops->Remove(Store->BlobPath(Sha)))
					{
						++Swept;
					}
				}
				if (NextSweep < Plan.Sweep.size())
				{
					return true;
				}
				Phase = EPhase::Rotate;
				return true;
			}
			case EPhase::Rotate:
				// Step 4 (and Clear's rotation): a new index generation holding only the live entries.
				if (Store->StepIndexRotation(!bRotationStarted))
				{
					bRotationStarted = true;
					return true;
				}
				return Finish();
			default:
				return false;
			}
		}
	};

	/**
	 * §10.5 Merge / §10.1 Import: another history folder (a secondary's <sec>/pending-history, a moved project's history)
	 * joins this one. Its index is read without modifying anything there; each blob this store lacks is copied (one per
	 * slice, deduplicated by SHA-256); then its entries are committed by id, 200 per slice (an ENTRY with a known id
	 * replaces it, so a second merge of the same folder adds nothing). An entry whose blob is missing is skipped.
	 */
	struct FMergeTask
	{
		enum class EPhase : uint8 { Prepare, ReadSource, CopyBlobs, Index, Done };

		FStorePtr Store;
		FString SourceDir;
		TFunction<void(bool, int32)> OnDone;
		EPhase Phase = EPhase::Prepare;
		std::vector<dream::history::Entry> Entries;
		size_t NextBlob = 0;
		size_t NextEntry = 0;
		int32 Merged = 0;
		bool bOk = true;

		bool Finish()
		{
			Phase = EPhase::Done;
			if (OnDone)
			{
				TFunction<void(bool, int32)> Callback = MoveTemp(OnDone);
				const bool bResult = bOk;
				const int32 Count = Merged;
				Store->Complete([Callback, bResult, Count]() { Callback(bResult, Count); });
			}
			return false;
		}

		bool Step(FDreamIoContext& Context)
		{
			if (Context.bExiting)
			{
				// (instances) The writer is stopping: the exit drain belongs to the queued AddEntry work ("closed" entries).
				// A merge stops between slices and fails, so the namespace it reads is kept and merged at the next start.
				bOk = false;
				return Finish();
			}
			switch (Phase)
			{
			case EPhase::Prepare:
				if (!Store->Prepare(Context))
				{
					bOk = false;
					return Finish();
				}
				Phase = EPhase::ReadSource;
				return true;
			case EPhase::ReadSource:
			{
				FDreamReadOnlyFileOps ReadOps(*Store->Ops);
				const std::string SourceUtf8 = FDreamWin32FileOps::ToUtf8Path(SourceDir);
				if (!ReadOps.Exists(SourceUtf8))
				{
					return Finish();   // nothing was staged
				}
				dream::history::RecoveredIndex Source = dream::history::ReadIndex(ReadOps, SourceUtf8, /*bMayModify*/ false);
				if (Source.bNewerFormat || (Source.bFound && Source.bUnreadable) || Source.bSkippedUnreadable)
				{
					// Never merged by a build that cannot read it (§10.6), nor taken as empty when it is there but unreadable
					// (instances, review r1), nor from an older generation while a newer one could not be read (review r2): the
					// folder holding it is kept and merged at a later start.
					bOk = false;
					return Finish();
				}
				for (const dream::history::Entry& E : Source.Entries.Entries())
				{
					const dream::history::Entry* Mine = Store->Index.Find(E.Id);
					if (Mine == nullptr || Mine->Runs != E.Runs || Mine->Blob != E.Blob)
					{
						Entries.push_back(E);
					}
				}
				Phase = EPhase::CopyBlobs;
				return true;
			}
			case EPhase::CopyBlobs:
			{
				if (NextBlob >= Entries.size())
				{
					Phase = EPhase::Index;
					return true;
				}
				dream::history::Entry& E = Entries[NextBlob++];
				const std::string Target = Store->BlobPath(E.Blob);
				if (Store->Ops->Exists(Target))
				{
					return true;       // deduplicated by SHA-256 (§10.5)
				}
				const std::string From = dream::journal::JoinPath(FDreamWin32FileOps::ToUtf8Path(SourceDir), dream::history::BlobRelativePath(E.Blob));
				std::string Bytes;
				dream::history::BlobHeader Header;
				// (instances, review r1, persist-M1) Only a source blob that is missing or undecodable skips its entry; a
				// source that exists but cannot be read now, or any failure on this side, fails the merge, so the folder that
				// holds the only copy is kept and merged again later.
				if (!Store->Ops->Exists(From))
				{
					E.Blob.clear();
					return true;
				}
				if (!Store->Ops->ReadAll(From, Bytes))
				{
					UE_LOG(LogDream, Warning, TEXT("Local history could not read %s; the merge stops."), *DreamText::Utf8ToFString(From));
					bOk = false;
					return Finish();
				}
				if (!dream::history::DecodeBlobHeader(Bytes, Header))
				{
					E.Blob.clear();
					return true;
				}
				const std::string Temp = Target + ".tmp";
				if (Store->Ops->Exists(Temp))
				{
					Store->Ops->Remove(Temp);   // a copy of an earlier merge that never finished
				}
				dream::journal::FileOps::Handle H = dream::journal::FileOps::InvalidHandle;
				const bool bCopied = Store->Ops->MakeDirs(Target.substr(0, Target.find_last_of('/')))
					&& Store->Ops->CreateExclusive(Temp, H)
					&& Store->Ops->Append(H, Bytes) && Store->Ops->Flush(H);
				if (H != dream::journal::FileOps::InvalidHandle)
				{
					Store->Ops->Close(H);
				}
				if (!bCopied || !Store->Ops->RenameNew(Temp, Target))
				{
					Store->Ops->Remove(Temp);
					if (!Store->Ops->Exists(Target))
					{
						UE_LOG(LogDream, Warning, TEXT("Local history could not copy %s: %s; the merge stops."), *DreamText::Utf8ToFString(Target),
							*DreamText::Utf8ToFString(Store->Ops->LastError()));
						bOk = false;
						return Finish();
					}
				}
				return true;
			}
			case EPhase::Index:
			{
				std::vector<dream::journal::Record> Records;
				const size_t End = FMath::Min(Entries.size(), NextEntry + static_cast<size_t>(200));
				for (; NextEntry < End; ++NextEntry)
				{
					if (!Entries[NextEntry].Blob.empty())
					{
						Records.push_back(dream::history::MakeEntry(Entries[NextEntry]));
					}
				}
				if (!Records.empty())
				{
					if (!Store->CommitIndex(Records))
					{
						bOk = false;
						return Finish();
					}
					Merged += static_cast<int32>(Records.size());
				}
				if (NextEntry < Entries.size())
				{
					return true;
				}
				return Finish();
			}
			default:
				return false;
			}
		}
	};

	/** Query: the entries of one buffer, newest first. */
	FDreamIoSlice MakeQuerySlice(const FStorePtr& Store, const FString& Uuid, TFunction<void(TArray<FDreamLocalHistoryEntry>&&)> OnGameThread)
	{
		return [Store, Uuid, OnGameThread](FDreamIoContext& Context) -> bool
		{
			TArray<FDreamLocalHistoryEntry> Out;
			Store->Prepare(Context);   // a read-only (newer) index is still listed
			const std::string Key = DreamText::ToUtf8(Uuid);
			for (const dream::history::Entry& E : Store->Index.Entries())
			{
				if (Key.empty() || E.Uuid == Key)
				{
					Out.Add(ToGameEntry(E));
				}
			}
			Out.StableSort([](const FDreamLocalHistoryEntry& A, const FDreamLocalHistoryEntry& B) { return A.Utc > B.Utc; });
			Store->Complete([OnGameThread, Entries = MoveTemp(Out)]() mutable { OnGameThread(MoveTemp(Entries)); });
			return false;
		};
	}

	/** LoadText: the blob read, then a few chunks inflated per slice, length and CRC checked. */
	struct FLoadTask
	{
		FStorePtr Store;
		FString Sha;
		TFunction<void(TOptional<FString>&&)> OnGameThread;
		bool bRead = false;
		std::string File;
		size_t Pos = dream::history::BlobHeaderBytes;
		dream::history::BlobHeader Header;
		std::string Text;

		bool Done(TOptional<FString>&& Result)
		{
			TFunction<void(TOptional<FString>&&)> Callback = MoveTemp(OnGameThread);
			Store->Complete([Callback, Value = MoveTemp(Result)]() mutable { Callback(MoveTemp(Value)); });
			return false;
		}

		bool Step(FDreamIoContext& Context)
		{
			if (!bRead)
			{
				bRead = true;
				Store->Prepare(Context);
				if (!Store->Ops.IsValid() || !Store->Ops->ReadAll(Store->BlobPath(DreamText::ToUtf8(Sha)), File)
					|| !dream::history::DecodeBlobHeader(File, Header))
				{
					return Done(TOptional<FString>());
				}
				Text.reserve(static_cast<size_t>(FMath::Min<uint64>(Header.RawLength, 512ull << 20)));
				return true;
			}
			for (int32 K = 0; K < 4; ++K)
			{
				std::string_view Compressed;
				uint32 RawLength = 0;
				if (!dream::history::NextBlobChunk(File, Pos, Compressed, RawLength))
				{
					const bool bOk = Text.size() == Header.RawLength && dream::Crc32c(Text) == Header.RawCrc32c;
					return Done(bOk ? TOptional<FString>(DreamText::Utf8ToFString(Text)) : TOptional<FString>());
				}
				const size_t At = Text.size();
				Text.resize(At + RawLength);
				if (!FCompression::UncompressMemory(NAME_Zlib, Text.data() + At, static_cast<int64>(RawLength), Compressed.data(),
					static_cast<int64>(Compressed.size())))
				{
					return Done(TOptional<FString>());
				}
			}
			return true;
		}
	};
}

int64 DreamHistoryNoteUtcMs()
{
	// The last time handed out; strictly increasing, so the notes of one frame keep the order they were made in.
	static std::atomic<int64> GLast { 0 };
	const int64 Now = DreamLocalHistoryLocal::NowUtcMs();
	int64 Last = GLast.load(std::memory_order_relaxed);
	int64 Next = 0;
	do
	{
		Next = FMath::Max(Now, Last + 1);
	}
	while (!GLast.compare_exchange_weak(Last, Next, std::memory_order_relaxed));
	return Next;
}

FDreamLocalHistory::FDreamLocalHistory(FDreamJournal& InJournal)
	: Journal(InJournal)
	, Store(MakeShared<FStore, ESPMode::ThreadSafe>())
{
	TSharedPtr<FStore, ESPMode::ThreadSafe> StoreRef = Store;
	// Writer thread: a durable commit's note becomes an AddEntry task (run, save, closed, recovered).
	Journal.SetHistorySink([StoreRef](FDreamHistoryNote&& Note) -> FDreamIoSlice
	{
		return DreamLocalHistoryLocal::MakeAddSlice(StoreRef, MoveTemp(Note), nullptr);
	});
}

FDreamLocalHistory::~FDreamLocalHistory()
{
	Shutdown();
}

void FDreamLocalHistory::Shutdown()
{
	Store->bAlive.store(false, std::memory_order_release);
	if (Ticker.IsValid())
	{
		FDreamDeferral::RemoveTicker(Ticker);
		Ticker.Reset();
	}
}

void FDreamLocalHistory::EnsureTicker() const
{
	if (Ticker.IsValid() || !Store->bAlive.load(std::memory_order_acquire))
	{
		return;
	}
	FDreamLocalHistory* Self = const_cast<FDreamLocalHistory*>(this);
	// The results and the retention timer, never inside a run (RT-01); 4 Hz like the journal's scheduler.
	Ticker = FDreamDeferral::AddTicker(TEXT("DreamLocalHistory"), EDreamDeferPolicy::DeferWhileExecuting, 0.25f, false,
		[Self](float) { return Self->Tick(); });
}

bool FDreamLocalHistory::IsPaused() const
{
	return Store->bPausedNow.load(std::memory_order_acquire);
}

bool FDreamLocalHistory::Tick()
{
	TFunction<void()> Work;
	while (Store->Completions.Dequeue(Work))
	{
		if (Work)
		{
			Work();
		}
	}
	if (IsPaused() != bPausedSeen)
	{
		bPausedSeen = !bPausedSeen;
		PausedChanged.Broadcast();   // persist-r2-M2: the tab's notice
	}
	if (IsPaused() && FPlatformTime::Seconds() >= NextPausedProbeSeconds && !Store->bProbeQueued.exchange(true))
	{
		// Review r3: while paused, the index is read again once a second even when no history work is queued, so the pause and
		// its notice end on their own after the release (Prepare reads only once its RetryAt has passed).
		NextPausedProbeSeconds = FPlatformTime::Seconds() + 1.0;
		TSharedPtr<FStore, ESPMode::ThreadSafe> StoreRef = Store;
		if (!Journal.PostIo([StoreRef](FDreamIoContext& Context)
			{
				StoreRef->Prepare(Context);
				StoreRef->bProbeQueued.store(false, std::memory_order_release);
				return false;
			}))
		{
			Store->bProbeQueued.store(false, std::memory_order_release);
		}
	}
	if (bRetentionScheduled && FPlatformTime::Seconds() >= NextRetentionSeconds)
	{
		NextRetentionSeconds = FPlatformTime::Seconds() + RetentionIntervalSeconds;
		if (Journal.IsOpen())
		{
			EnforceRetention();
		}
	}
	return true;
}

bool FDreamLocalHistory::Post(FDreamIoSlice&& Work) const
{
	EnsureTicker();
	return Journal.PostIo(MoveTemp(Work));
}

void FDreamLocalHistory::AddEntry(const FGuid& Buffer, EDreamHistoryKind Kind, const dream::TextSnapshot& Text, const FString& Path,
	const FString& DisplayName, TOptional<int32> RunNumber, TFunction<void(bool)> OnDone)
{
	FDreamHistoryNote Note;
	Note.BufferUuid = DreamText::GuidToHex(Buffer);
	Note.Kind = Kind;
	Note.Text = Text;
	Note.Path = Path;
	Note.DisplayName = DisplayName;
	Note.RunNumber = RunNumber.Get(0);
	Note.UtcMs = DreamHistoryNoteUtcMs() + Store->ClockOffsetMs.load(std::memory_order_acquire);
	TFunction<void(bool)> Callback = OnDone;
	if (!Post(DreamLocalHistoryLocal::MakeAddSlice(Store, MoveTemp(Note), MoveTemp(OnDone))) && Callback)
	{
		Callback(false);   // no journal writer (not open): nothing can be kept
	}
}

void FDreamLocalHistory::Query(const FGuid& Buffer, TFunction<void(TArray<FDreamLocalHistoryEntry>&&)> OnGameThread) const
{
	const FString Uuid = Buffer.IsValid() ? DreamText::GuidToHex(Buffer) : FString();
	TFunction<void(TArray<FDreamLocalHistoryEntry>&&)> Fallback = OnGameThread;
	if (!Post(DreamLocalHistoryLocal::MakeQuerySlice(Store, Uuid, MoveTemp(OnGameThread))) && Fallback)
	{
		Fallback(TArray<FDreamLocalHistoryEntry>());
	}
}

void FDreamLocalHistory::LoadText(const FDreamLocalHistoryEntry& Entry, TFunction<void(TOptional<FString>&&)> OnGameThread) const
{
	TSharedPtr<DreamLocalHistoryLocal::FLoadTask, ESPMode::ThreadSafe> Task = MakeShared<DreamLocalHistoryLocal::FLoadTask, ESPMode::ThreadSafe>();
	Task->Store = Store;
	Task->Sha = Entry.BlobSha256;
	Task->OnGameThread = OnGameThread;
	if (!Post([Task](FDreamIoContext& Context) { return Task->Step(Context); }) && OnGameThread)
	{
		OnGameThread(TOptional<FString>());
	}
}

void FDreamLocalHistory::Clear(TFunction<void(bool)> OnGameThread)
{
	TSharedPtr<DreamLocalHistoryLocal::FMaintenanceTask, ESPMode::ThreadSafe> Task = MakeShared<DreamLocalHistoryLocal::FMaintenanceTask, ESPMode::ThreadSafe>();
	Task->Store = Store;
	Task->bClear = true;
	FDreamJournal* JournalPtr = &Journal;
	Task->OnClearDone = [OnGameThread, JournalPtr](bool bOk, int32 Entries, int32 Blobs)
	{
		// §10.4: Clear writes an audit entry (the editor's own history only; a test's private root has no audit of its own).
		if (bOk && FDreamEditorModule::IsAvailable() && JournalPtr == &FDreamEditorModule::Get().GetJournal())
		{
			dream::json::Value Fields = dream::json::Value::MakeObject();
			Fields.Set("entries", dream::json::Value::MakeInt(Entries));
			Fields.Set("blobs", dream::json::Value::MakeInt(Blobs));
			FDreamEditorModule::Get().GetAudit().Event(TEXT("history.clear"), Fields);
		}
		if (OnGameThread)
		{
			OnGameThread(bOk);
		}
	};
	if (!Post([Task](FDreamIoContext& Context) { return Task->Step(Context); }) && OnGameThread)
	{
		OnGameThread(false);
	}
}

void FDreamLocalHistory::EnforceRetention(TFunction<void(int32, int32)> OnGameThread)
{
	TSharedPtr<DreamLocalHistoryLocal::FMaintenanceTask, ESPMode::ThreadSafe> Task = MakeShared<DreamLocalHistoryLocal::FMaintenanceTask, ESPMode::ThreadSafe>();
	Task->Store = Store;
	Task->OnRetentionDone = OnGameThread;
	if (!Post([Task](FDreamIoContext& Context) { return Task->Step(Context); }) && OnGameThread)
	{
		OnGameThread(0, 0);
	}
}

void FDreamLocalHistory::MergeFrom(const FString& SourceHistoryDir, TFunction<void(bool, int32)> OnGameThread)
{
	TSharedPtr<DreamLocalHistoryLocal::FMergeTask, ESPMode::ThreadSafe> Task = MakeShared<DreamLocalHistoryLocal::FMergeTask, ESPMode::ThreadSafe>();
	Task->Store = Store;
	Task->SourceDir = SourceHistoryDir;
	Task->OnDone = OnGameThread;
	if (!Post([Task](FDreamIoContext& Context) { return Task->Step(Context); }) && OnGameThread)
	{
		OnGameThread(false, 0);
	}
}

void FDreamLocalHistory::OnJournalOpened(bool bRunRetention)
{
	if (bRunRetention)
	{
		bRetentionScheduled = true;
		NextRetentionSeconds = FPlatformTime::Seconds() + RetentionFirstDelaySeconds;
	}
	EnsureTicker();   // review r3: every role, so a secondary's pause notice and its re-reads run too
}

void FDreamLocalHistory::SetRetentionForTest(int64 MaxAgeMs, int64 MaxBytes, int64 ClockOffsetMs)
{
	Store->MaxAgeMs.store(MaxAgeMs, std::memory_order_release);
	Store->MaxBytes.store(MaxBytes, std::memory_order_release);
	Store->ClockOffsetMs.store(ClockOffsetMs, std::memory_order_release);
}
