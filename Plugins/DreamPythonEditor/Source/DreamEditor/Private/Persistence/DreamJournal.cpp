// SPDX-License-Identifier: Apache-2.0
// FDreamJournal, the game-thread half of the write-ahead journal (TDD §10.2, §10.3, §10.7 basic, D17, U7). Snapshots are
// O(1) TextBuffer references; nothing here touches the disk except the two lock files at Open (§10.3 budgets). The 4 Hz
// commit scheduler goes through FDreamDeferral, so it does nothing while user code is on the stack (RT-01, §10.2.5).
#include "Persistence/DreamJournal.h"

#include "Diagnostics/DreamAudit.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamJournalWriter.h"
#include "Persistence/DreamWin32FileOps.h"
#include "Run/DreamDeferral.h"
#include "dream/TextBuffer.h"
#include "dream/TextCodec.h"

#include <algorithm>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamJournalLocal
{
	constexpr const TCHAR* MainNamespace = TEXT("main");
	constexpr double SessionDebounceSeconds = 2.0;      // view-state-only changes (§10.2.5, proposed)
	constexpr double SessionTimeSeconds = 30.0;         // the SESSION's "utc" while only text changes (PER-13, proposed)

	FString UuidOf(const FDreamBuffer& Buffer)
	{
		return Buffer.GetKind() == EDreamBufferKind::ReplInput ? FString(TEXT("repl-input")) : Buffer.GetIdHex();
	}

	const char* KindName(EDreamBufferKind Kind)
	{
		switch (Kind)
		{
		case EDreamBufferKind::File: return "file";
		case EDreamBufferKind::Recovered: return "recovered";
		default: return "scratch";
		}
	}

	const char* EolName(dream::codec::Eol Eol)
	{
		switch (Eol)
		{
		case dream::codec::Eol::CrLf: return "crlf";
		case dream::codec::Eol::Cr: return "cr";
		default: return "lf";
		}
	}

	int64 ToUtcMs(const FDateTime& Time)
	{
		return static_cast<int64>((Time - FDateTime(1970, 1, 1)).GetTotalMilliseconds());
	}

	std::optional<dream::journal::BaseVersion> ToJournalBase(const TOptional<FDreamBaseVersion>& Base)
	{
		if (!Base.IsSet())
		{
			return std::nullopt;
		}
		dream::journal::BaseVersion Out;
		Out.Size = Base->Size;
		Out.MtimeUtcMs = ToUtcMs(Base->MtimeUtc);
		Out.Sha256Hex = DreamText::ToUtf8(Base->Sha256Hex);
		return Out;
	}

	dream::journal::BufferOpenInfo MakeOpenInfo(const FDreamBuffer& Buffer)
	{
		dream::journal::BufferOpenInfo Info;
		Info.Uuid = DreamText::ToUtf8(UuidOf(Buffer));
		Info.Kind = KindName(Buffer.GetKind());
		Info.Path = DreamText::ToUtf8(Buffer.GetPath());
		Info.Display = DreamText::ToUtf8(Buffer.GetDisplayName());
		Info.Encoding = dream::codec::EncodingName(Buffer.GetFormat().Enc);
		Info.bBom = Buffer.GetFormat().bBom;
		Info.Eol = EolName(Buffer.GetFormat().Dominant);
		Info.Base = ToJournalBase(Buffer.GetBase());
		Info.bReadOnly = Buffer.IsReadOnly() && !Buffer.HasPendingRecovery();
		return Info;
	}

	DreamJournal::FBufferPart MakePart(const FDreamBuffer& Buffer, const dream::TextSnapshot& Snapshot)
	{
		DreamJournal::FBufferPart Part;
		Part.Uuid = DreamText::ToUtf8(UuidOf(Buffer));
		Part.Snapshot = Snapshot;
		Part.bHasSnapshot = true;
		Part.Open = MakeOpenInfo(Buffer);
		Part.bDirty = Buffer.IsDirty();
		return Part;
	}

	const char* OutcomeKey(EDreamRunOutcome Outcome)
	{
		switch (Outcome)
		{
		case EDreamRunOutcome::Ok: return "ok";
		case EDreamRunOutcome::Error: return "error";
		case EDreamRunOutcome::Cancelled: return "cancelled";
		case EDreamRunOutcome::NotExecuted: return "not executed";
		case EDreamRunOutcome::Lost: return "lost";
		default: return "error";
		}
	}

	/** A history note of the buffer's current identity (§10.4). */
	FDreamHistoryNote MakeNote(const FDreamBuffer& Buffer, EDreamHistoryKind Kind, const dream::TextSnapshot& Text, int32 RunNumber = 0)
	{
		FDreamHistoryNote Note;
		Note.BufferUuid = Buffer.GetIdHex();
		Note.Kind = Kind;
		Note.Text = Text;
		Note.Path = Buffer.GetPath();
		Note.DisplayName = Buffer.GetDisplayName();
		Note.RunNumber = RunNumber;
		Note.UtcMs = DreamHistoryNoteUtcMs();   // strictly increasing: run-entry merging orders notes by time
		return Note;
	}

	dream::TextSnapshot ModelSnapshot(const FString& Text)
	{
		return dream::TextBuffer(DreamText::ToU16(DreamText::ToModelText(Text))).Snapshot();
	}
}

struct FDreamJournal::FImpl
{
	FDreamStorageRoot Root;
	FDateTime SessionStartUtc;
	FString SessionStamp;
	bool bOpen = false;
	bool bReconciled = false;
	bool bSecondary = false;
	/** The namespace Open chose ("main" or a secondary's "i-<pid>-<start>"); the writer may move a primary to compat-*. */
	FString Namespace;
	/** The namespace the scan read, which this session's recoveries come from (RECOVERY_PENDING / RESOLVED, §10.2.2). */
	FString SourceNamespace;
	FString SecondaryIdForTest;
	FString PrimaryLockContent;
	uint64 CompatLock = 0;

	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Fence;
	TUniquePtr<FDreamJournalWriter> Writer;
	FRunnableThread* Thread = nullptr;
	uint64 InstanceLock = 0;
	uint64 NamespaceLock = 0;
	uint64 NextTicket = 0;

	dream::journal::CommitPolicy Policy;
	FDreamHistorySink HistorySink;
	/** Scratch buffers' clean texts (§10.7.1): sent with the reconcile's checkpoint, or at once when set later. */
	TMap<FGuid, dream::TextSnapshot> CleanTexts;

	struct FLive
	{
		TWeakPtr<FDreamBuffer> Buffer;
		FDelegateHandle ChangedHandle;
	};
	TMap<FGuid, FLive> Live;

	TArray<FGuid> TabOrder;
	FGuid ActiveTab;
	bool bSessionDirty = false;
	double SessionDirtySince = 0.0;
	uint64 SourceGeneration = 0;

	FTSTicker::FDelegateHandle CommitTicker;
	FTSTicker::FDelegateHandle ScanTicker;
	FDreamOnRecoveryScan ScanCallback;
	/** The scan result until a callback consumes it (PER-02: a tab closed before delivery must not lose the restore). */
	TOptional<FDreamRecoveredSession> CachedScan;
	double LastSessionPushSeconds = 0.0;

	/** Commits are written: open, reconciled, and not a newer format's namespace (§10.6: read only). */
	bool CanWrite() const
	{
		return bOpen && bReconciled && Writer.IsValid() && Writer->GetHealth() != EDreamJournalHealth::ReadOnly;
	}

	/** Every lock this session holds: the namespace's, a compat namespace's (§10.6), the orphans of a scan result nobody
	 *  took, and instance.lock last. The writer is stopped or fenced before this. */
	void ReleaseLocks()
	{
		if (Writer.IsValid() && CompatLock == 0)
		{
			CompatLock = Writer->TakeCompatLock();
		}
		if (CachedScan.IsSet())
		{
			FDreamJournal::ReleaseOrphanLocks(CachedScan->Orphans);
		}
		FDreamWin32FileOps::ReleaseLockFile(CompatLock);
		FDreamWin32FileOps::ReleaseLockFile(NamespaceLock);
		FDreamWin32FileOps::ReleaseLockFile(InstanceLock);
		CompatLock = 0;
		NamespaceLock = 0;
		InstanceLock = 0;
	}

	uint64 Push(TUniquePtr<DreamJournal::FRequest> Request)
	{
		Request->Commit.Ticket = ++NextTicket;
		const uint64 Ticket = Request->Commit.Ticket;
		Writer->Push(MoveTemp(Request));
		return Ticket;
	}

	TSharedPtr<FDreamBuffer> FindLive(const FGuid& Id) const
	{
		const FLive* Entry = Live.Find(Id);
		return Entry ? Entry->Buffer.Pin() : nullptr;
	}

	dream::json::Value BuildSession() const
	{
		using dream::json::Value;
		Value Session = Value::MakeObject();
		Value Tabs = Value::MakeArray();
		for (const FGuid& Id : TabOrder)
		{
			TSharedPtr<FDreamBuffer> Buffer = FindLive(Id);
			if (!Buffer.IsValid())
			{
				continue;
			}
			const FDreamBufferViewState& View = Buffer->GetViewState();
			Value Tab = Value::MakeObject();
			Tab.Set("uuid", Value::MakeString(DreamText::ToUtf8(Buffer->GetIdHex())));
			Tab.Set("kind", Value::MakeString(DreamJournalLocal::KindName(Buffer->GetKind())));
			Tab.Set("path", Value::MakeString(DreamText::ToUtf8(Buffer->GetPath())));
			Tab.Set("display", Value::MakeString(DreamText::ToUtf8(Buffer->GetDisplayName())));
			Value Caret = Value::MakeArray();
			Caret.Append(Value::MakeInt(View.CaretLine));
			Caret.Append(Value::MakeInt(View.CaretColumn));
			Tab.Set("caret", MoveTemp(Caret));
			Tab.Set("first", Value::MakeInt(View.FirstVisibleLine));
			Tab.Set("isolated", Value::MakeBool(View.Namespace == EDreamNamespaceMode::Isolated));
			Tab.Set("one_undo", Value::MakeBool(View.bOneUndoStepPerRun));
			Tab.Set("plain", Value::MakeBool(true));
			Tabs.Append(MoveTemp(Tab));
		}
		Session.Set("tabs", MoveTemp(Tabs));
		Session.Set("active", Value::MakeString(ActiveTab.IsValid() ? DreamText::ToUtf8(DreamText::GuidToHex(ActiveTab)) : std::string()));
		// last_record_utc (§10.2.4 step 6): the banner's "ended unexpectedly at <time>" reads the newest one.
		Session.Set("utc", Value::MakeInt(DreamJournalLocal::ToUtcMs(FDateTime::UtcNow())));
		return Session;
	}

	/** Every live buffer at its current state (the writer skips versions it already holds). */
	void AddAllBuffers(DreamJournal::FRequest& Request) const
	{
		for (const TPair<FGuid, FLive>& Pair : Live)
		{
			if (TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin())
			{
				Request.Commit.Buffers.push_back(DreamJournalLocal::MakePart(*Buffer, Buffer->Snapshot()));
			}
		}
	}

	void ForgetPendingEdits()
	{
		for (const TPair<FGuid, FLive>& Pair : Live)
		{
			if (TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin())
			{
				Policy.OnCommitted(DreamText::ToUtf8(DreamJournalLocal::UuidOf(*Buffer)));
			}
		}
	}

	void Track(const TSharedRef<FDreamBuffer>& Buffer, FDreamJournal& Owner)
	{
		if (Live.Contains(Buffer->GetId()))
		{
			return;
		}
		FLive Entry;
		Entry.Buffer = Buffer;
		const FGuid Id = Buffer->GetId();
		Entry.ChangedHandle = Buffer->OnChanged().AddLambda([&Owner, Id](const dream::ChangeSet&) { Owner.OnBufferEdited(Id); });
		Live.Add(Id, Entry);
	}

	void Untrack(const FGuid& Id)
	{
		if (FLive* Entry = Live.Find(Id))
		{
			if (TSharedPtr<FDreamBuffer> Buffer = Entry->Buffer.Pin())
			{
				Buffer->OnChanged().Remove(Entry->ChangedHandle);
			}
			Live.Remove(Id);
		}
	}

	/** The 4 Hz scheduler (§10.2.5): due buffers by CommitPolicy, the session after its debounce. */
	void Tick()
	{
		if (!CanWrite())
		{
			return;
		}
		const double Now = FPlatformTime::Seconds();
		TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
		for (const std::string& Uuid : Policy.DueBuffers(Now))
		{
			for (const TPair<FGuid, FLive>& Pair : Live)
			{
				TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin();
				if (Buffer.IsValid() && DreamText::ToUtf8(DreamJournalLocal::UuidOf(*Buffer)) == Uuid)
				{
					Request->Commit.Buffers.push_back(DreamJournalLocal::MakePart(*Buffer, Buffer->Snapshot()));
					break;
				}
			}
			Policy.OnCommitted(Uuid);
		}
		// Typing alone also moves last_record_utc (SNAP and DELTA carry no time): the session rides along at most every 30 s.
		const bool bSessionForTime = !Request->Commit.Buffers.empty() && Now - LastSessionPushSeconds >= DreamJournalLocal::SessionTimeSeconds;
		if ((bSessionDirty && Now - SessionDirtySince >= DreamJournalLocal::SessionDebounceSeconds) || bSessionForTime)
		{
			Request->Commit.bHasSession = true;
			Request->Commit.Session = BuildSession();
			bSessionDirty = false;
			LastSessionPushSeconds = Now;
		}
		if (!Request->Commit.Buffers.empty() || Request->Commit.bHasSession)
		{
			Push(MoveTemp(Request));
		}
	}
};

FDreamJournal::FDreamJournal()
	: Impl(MakeUnique<FImpl>())
{
	Impl->SessionStartUtc = FDateTime::UtcNow();
	Impl->SessionStamp = Impl->SessionStartUtc.ToString(TEXT("%Y%m%d-%H%M%S"));
}

FDreamJournal::~FDreamJournal()
{
	Shutdown();
	for (const TPair<FGuid, FImpl::FLive>& Pair : Impl->Live)
	{
		if (TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin())
		{
			Buffer->OnChanged().Remove(Pair.Value.ChangedHandle);
		}
	}
}

bool FDreamJournal::Open(const FDreamStorageRoot& Root, EDreamInstanceRole& OutRole)
{
	OutRole = Impl->bSecondary ? EDreamInstanceRole::Secondary : EDreamInstanceRole::Primary;
	if (Impl->bOpen)
	{
		return true;
	}
	if (Root.Root.IsEmpty())
	{
		return false;
	}

	const bool bPrimary = !Impl->bSecondary && AcquireInstanceLock(Root, OutRole);
	if (!bPrimary && !Impl->bSecondary)
	{
		return false;   // the lock file could not be opened at all (not writable): no journal this session
	}

	// §10.5 "At tab spawn": the lock free, the primary journals in main; held by a live instance, this one journals in its
	// own namespace journal/i-<pid>-<process_start>/ (the id FDreamAudit's secondary file names use).
	FString Namespace = DreamJournalLocal::MainNamespace;
	if (!bPrimary)
	{
		Namespace = Impl->SecondaryIdForTest.IsEmpty() ? FDreamAudit::MakeSecondaryId() : Impl->SecondaryIdForTest;
		std::string LockText;
		FDreamWin32FileOps::ReadLockFile(Root.Root / TEXT("instance.lock"), LockText);
		Impl->PrimaryLockContent = DreamText::Utf8ToFString(LockText);
		UE_LOG(LogDream, Log, TEXT("Another editor holds %s; this session journals separately in %s (§10.5)."),
			*(Root.Root / TEXT("instance.lock")), *Root.JournalDir(Namespace));
	}

	FDreamWin32FileOps Ops;
	bool bHeldByOther = false;
	const FString NamespaceDir = Root.JournalDir(Namespace);
	Ops.MakeDirs(FDreamWin32FileOps::ToUtf8Path(NamespaceDir));
	Impl->NamespaceLock = FDreamWin32FileOps::AcquireLockFile(NamespaceDir / TEXT("ns.lock"), bHeldByOther, DreamJournalScan::MakeLockContent(Namespace));
	if (Impl->NamespaceLock == 0 && !bPrimary)
	{
		// A secondary's namespace is new and its own; a held lock there means another journal of this process has it.
		UE_LOG(LogDream, Warning, TEXT("Dream Python Editor could not lock %s; this session's tabs are not journaled."), *(NamespaceDir / TEXT("ns.lock")));
		return false;
	}

	Impl->Root = Root;
	Impl->Namespace = Namespace;
	Impl->SourceNamespace = Namespace;
	Impl->Fence = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	FDreamJournalWriterOptions Options;
	Options.bPrimary = bPrimary;
	Options.HistoryDir = bPrimary ? Root.HistoryDir() : NamespaceDir / TEXT("pending-history");
	Impl->Writer = MakeUnique<FDreamJournalWriter>(Root, Namespace, DreamJournalLocal::ToUtcMs(Impl->SessionStartUtc), Impl->Fence,
		Impl->HistorySink, Options);
	// Normal, not BelowNormal (architect ruling, review r1): the low-priority lane of §10.2.1 is logical (commits first, other
	// work in slices), not an OS priority; a below-normal writer let a shader or code compile starve the 2 s pre-run bound.
	Impl->Thread = FRunnableThread::Create(Impl->Writer.Get(), TEXT("DreamJournalWriter"), 0, TPri_Normal);
	if (Impl->Thread == nullptr)
	{
		Impl->Writer.Reset();
		FDreamWin32FileOps::ReleaseLockFile(Impl->NamespaceLock);
		FDreamWin32FileOps::ReleaseLockFile(Impl->InstanceLock);
		Impl->NamespaceLock = Impl->InstanceLock = 0;
		return false;
	}

	// A journal opened late starts unreconciled: its generation begins at the reconcile that follows its own scan (ADD1-01).
	Impl->bReconciled = false;
	Impl->CachedScan.Reset();
	TUniquePtr<DreamJournal::FRequest> Scan = MakeUnique<DreamJournal::FRequest>();
	Scan->Kind = DreamJournal::ERequestKind::Scan;
	Impl->Push(MoveTemp(Scan));

	FImpl* Self = Impl.Get();
	Impl->CommitTicker = FDreamDeferral::AddTicker(TEXT("DreamJournal"), EDreamDeferPolicy::DeferWhileExecuting,
		static_cast<float>(1.0 / TickHz), false, [Self](float) { Self->Tick(); return true; });
	Impl->bOpen = true;
	OutRole = bPrimary ? EDreamInstanceRole::Primary : EDreamInstanceRole::Secondary;
	return true;
}

bool FDreamJournal::AcquireInstanceLock(const FDreamStorageRoot& Root, EDreamInstanceRole& OutRole)
{
	OutRole = Impl->bSecondary ? EDreamInstanceRole::Secondary : EDreamInstanceRole::Primary;
	if (Impl->InstanceLock != 0)
	{
		return true;
	}
	if (Impl->bSecondary || Root.Root.IsEmpty())
	{
		return false;
	}
	FDreamWin32FileOps Ops;
	Ops.MakeDirs(FDreamWin32FileOps::ToUtf8Path(Root.Root));
	bool bHeldByOther = false;
	Impl->InstanceLock = FDreamWin32FileOps::AcquireLockFile(Root.Root / TEXT("instance.lock"), bHeldByOther,
		DreamJournalScan::MakeLockContent(DreamJournalLocal::MainNamespace));
	if (Impl->InstanceLock == 0)
	{
		// The role is fixed for the editor session (A39): a session that started second stays second.
		Impl->bSecondary = bHeldByOther;
		OutRole = bHeldByOther ? EDreamInstanceRole::Secondary : EDreamInstanceRole::Primary;
		return false;
	}
	return true;
}

bool FDreamJournal::IsOpen() const
{
	return Impl->bOpen;
}

const FDreamStorageRoot& FDreamJournal::GetStorageRoot() const
{
	return Impl->Root;
}

FString FDreamJournal::GetSessionStartStamp() const
{
	return Impl->SessionStamp;
}

EDreamInstanceRole FDreamJournal::GetRole() const
{
	return Impl->bSecondary ? EDreamInstanceRole::Secondary : EDreamInstanceRole::Primary;
}

FString FDreamJournal::GetNamespace() const
{
	return Impl->Writer.IsValid() ? Impl->Writer->GetNamespace() : Impl->Namespace;
}

FString FDreamJournal::GetNamespaceDir() const
{
	const FString Namespace = GetNamespace();
	return Namespace.IsEmpty() ? FString() : Impl->Root.JournalDir(Namespace);
}

FString FDreamJournal::GetStateDir() const
{
	// §10.5 table: the secondary keeps its own restore guard (and file state) in <sec>/state/.
	return Impl->bSecondary && !Impl->Namespace.IsEmpty() ? Impl->Root.JournalDir(Impl->Namespace) / TEXT("state") : Impl->Root.StateDir();
}

FString FDreamJournal::GetHistoryDir() const
{
	return Impl->bSecondary && !Impl->Namespace.IsEmpty() ? Impl->Root.JournalDir(Impl->Namespace) / TEXT("pending-history") : Impl->Root.HistoryDir();
}

FString FDreamJournal::GetPrimaryLockContent() const
{
	return Impl->PrimaryLockContent;
}

void FDreamJournal::SetSecondaryIdForTest(const FString& Id)
{
	Impl->SecondaryIdForTest = Id;
}

void FDreamJournal::ReleaseOrphanLocks(TArray<FDreamOrphanNamespace>& Orphans)
{
	for (FDreamOrphanNamespace& Orphan : Orphans)
	{
		FDreamWin32FileOps::ReleaseLockFile(Orphan.NamespaceLock);
		Orphan.NamespaceLock = 0;
	}
}

void FDreamJournal::RequestRecoveryScan(FDreamOnRecoveryScan OnGameThread)
{
	if (!Impl->bOpen)
	{
		// Disabled journal: nothing to recover; the tab restores from the ini alone.
		FDreamRecoveredSession Empty;
		if (OnGameThread.IsBound())
		{
			OnGameThread.Execute(Empty);
		}
		return;
	}
	Impl->ScanCallback = MoveTemp(OnGameThread);
	if (Impl->ScanTicker.IsValid())
	{
		return;
	}
	FImpl* Self = Impl.Get();
	Impl->ScanTicker = FDreamDeferral::AddTicker(TEXT("DreamJournalScan"), EDreamDeferPolicy::DeferWhileExecuting, 0.f, false,
		[Self](float) -> bool
		{
			if (!Self->Writer.IsValid())
			{
				Self->ScanTicker.Reset();
				return false;
			}
			if (!Self->CachedScan.IsSet())
			{
				FDreamRecoveredSession Session;
				if (!Self->Writer->TakeScanResult(Session))
				{
					return true;
				}
				Self->SourceGeneration = Session.SourceGeneration;
				Self->SourceNamespace = Session.SourceNamespace.IsEmpty() ? Self->Namespace : Session.SourceNamespace;
				Self->CompatLock = Self->Writer->TakeCompatLock();   // compat-* (§10.6): released with the other locks
				Self->CachedScan = MoveTemp(Session);
			}
			Self->ScanTicker.Reset();
			// The result is kept until a callback consumes it: one whose tab is gone leaves it for the next request.
			FDreamOnRecoveryScan Callback = MoveTemp(Self->ScanCallback);
			if (Callback.IsBound() && Callback.Execute(Self->CachedScan.GetValue()))
			{
				Self->CachedScan.Reset();
			}
			return false;
		});
}

bool FDreamJournal::IsReconciled() const
{
	return Impl->bReconciled;
}

void FDreamJournal::Reconcile(TConstArrayView<TSharedRef<FDreamBuffer>> LiveBuffers, const FDreamRecoveredSession* SkippedSession)
{
	for (const TSharedRef<FDreamBuffer>& Buffer : LiveBuffers)
	{
		Impl->Track(Buffer, *this);
	}
	if (!Impl->bOpen || Impl->bReconciled)
	{
		// A closed journal stays unreconciled (ADD1-01): if it opens later, its own scan and reconcile start the generation.
		return;
	}
	Impl->bReconciled = true;
	if (Impl->Writer.IsValid() && Impl->Writer->GetHealth() == EDreamJournalHealth::ReadOnly)
	{
		return;     // §10.6: a newer format's namespace is never written; this session is not journaled there
	}

	// The first commit of the session: a new generation's checkpoint with every live buffer, the pending recoveries and the
	// session (§10.2.3, §10.7.1).
	TUniquePtr<DreamJournal::FRequest> Start = MakeUnique<DreamJournal::FRequest>();
	Start->Kind = DreamJournal::ERequestKind::StartGeneration;
	Impl->AddAllBuffers(*Start);
	// §10.7.1: every live scratch buffer's text at the reconcile is its clean text (the restore just made it clean), unless
	// the restore gave one explicitly; it rides in every checkpoint from now on.
	for (DreamJournal::FBufferPart& Part : Start->Commit.Buffers)
	{
		FGuid Id;
		if (!FGuid::Parse(DreamText::Utf8ToFString(Part.Uuid), Id))
		{
			continue;
		}
		TSharedPtr<FDreamBuffer> Buffer = Impl->FindLive(Id);
		if (!Buffer.IsValid() || Buffer->GetKind() != EDreamBufferKind::Scratch)
		{
			continue;
		}
		const dream::TextSnapshot* Explicit = Impl->CleanTexts.Find(Id);
		Part.CleanText = Explicit != nullptr ? *Explicit : Part.Snapshot;
		Impl->CleanTexts.Add(Id, *Part.CleanText);
	}
	if (SkippedSession != nullptr)
	{
		// Safe mode (§10.7.1, §10.8): every buffer of the skipped session gets a record, so ignoring the banner loses nothing.
		// Each one the restore did not open is carried with its journal text (not in the tab list; the next start restores
		// every journaled buffer), and a counted one also stays a pending recovery until Restore all or Discard.
		for (const FDreamRecoveredBuffer& Rec : SkippedSession->Buffers)
		{
			const FString Uuid = DreamText::GuidToHex(Rec.Id);
			const bool bLive = Impl->Live.Contains(Rec.Id);
			if (!bLive)
			{
				DreamJournal::FBufferPart Part;
				Part.Uuid = DreamText::ToUtf8(Uuid);
				Part.Snapshot = DreamJournalLocal::ModelSnapshot(Rec.RecoveredText);
				Part.bHasSnapshot = true;
				if (Rec.Kind == EDreamBufferKind::Scratch && Rec.CleanText.IsSet())
				{
					Part.CleanText = DreamJournalLocal::ModelSnapshot(Rec.CleanText.GetValue());
				}
				Part.Open.Uuid = DreamText::ToUtf8(Uuid);
				Part.Open.Kind = DreamJournalLocal::KindName(Rec.Kind);
				Part.Open.Path = DreamText::ToUtf8(Rec.Path);
				Part.Open.Display = DreamText::ToUtf8(Rec.DisplayName);
				Part.Open.Encoding = dream::codec::EncodingName(Rec.Format.Enc);
				Part.Open.bBom = Rec.Format.bBom;
				Part.Open.Eol = DreamJournalLocal::EolName(Rec.Format.Dominant);
				Part.Open.Base = DreamJournalLocal::ToJournalBase(Rec.Base);
				Part.bDirty = Rec.bDiffersFromDisk;
				Start->Commit.Buffers.push_back(MoveTemp(Part));
			}
			if (Rec.bCounted && (Rec.Kind == EDreamBufferKind::Scratch || !Rec.Path.IsEmpty()))
			{
				dream::journal::PendingInfo Info;
				Info.SourceNamespace = DreamText::ToUtf8(Impl->SourceNamespace);
				Info.SourceGeneration = Impl->SourceGeneration;
				Info.Uuid = DreamText::ToUtf8(Uuid);
				Info.Kind = DreamJournalLocal::KindName(Rec.Kind);
				Info.Path = DreamText::ToUtf8(Rec.Path);
				Info.Base = DreamJournalLocal::ToJournalBase(Rec.Base);
				Info.Eol = DreamJournalLocal::EolName(Rec.Format.Dominant);
				Start->Pending.emplace_back(MoveTemp(Info), DreamText::ToUtf8(Rec.RecoveredText));
				FDreamHistoryNote Note;
				Note.BufferUuid = Uuid;
				Note.Kind = EDreamHistoryKind::Recovered;
				Note.Text = DreamJournalLocal::ModelSnapshot(Rec.RecoveredText);
				Note.Path = Rec.Path;
				Note.DisplayName = Rec.DisplayName;
				Note.UtcMs = DreamHistoryNoteUtcMs();
				Start->History.Add(MoveTemp(Note));
			}
		}
	}
	for (const TPair<FGuid, FImpl::FLive>& Pair : Impl->Live)
	{
		TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin();
		if (!Buffer.IsValid() || !Buffer->HasPendingRecovery())
		{
			continue;
		}
		const std::string PendingUuid = DreamText::ToUtf8(Buffer->GetIdHex());
		if (std::any_of(Start->Pending.begin(), Start->Pending.end(),
			[&PendingUuid](const std::pair<dream::journal::PendingInfo, std::string>& P) { return P.first.Uuid == PendingUuid; }))
		{
			continue;
		}
		dream::journal::PendingInfo Info;
		Info.SourceNamespace = DreamText::ToUtf8(Impl->SourceNamespace);
		Info.SourceGeneration = Impl->SourceGeneration;
		Info.Uuid = DreamText::ToUtf8(Buffer->GetIdHex());
		Info.Kind = DreamJournalLocal::KindName(Buffer->GetKind());
		Info.Path = DreamText::ToUtf8(Buffer->GetPath());
		Info.Base = DreamJournalLocal::ToJournalBase(Buffer->GetBase());
		Info.Eol = DreamJournalLocal::EolName(Buffer->GetFormat().Dominant);
		const FString RecoveredText = Buffer->GetPendingRecoveryText().GetValue();
		Start->Pending.emplace_back(MoveTemp(Info), DreamText::ToUtf8(RecoveredText));
		// §10.7.1: "the writer adds each pending text to local history as kind recovered" (once per text: the history skips
		// an entry whose buffer's newest entry is the same recovered text).
		Start->History.Add(DreamJournalLocal::MakeNote(*Buffer, EDreamHistoryKind::Recovered, DreamJournalLocal::ModelSnapshot(RecoveredText)));
	}
	Start->Commit.bHasSession = true;
	Start->Commit.Session = Impl->BuildSession();
	Impl->ForgetPendingEdits();
	Impl->bSessionDirty = false;
	Impl->LastSessionPushSeconds = FPlatformTime::Seconds();
	Impl->Push(MoveTemp(Start));
}

void FDreamJournal::OnBufferOpened(const TSharedRef<FDreamBuffer>& Buffer)
{
	Impl->Track(Buffer, *this);
	if (Impl->CanWrite())
	{
		TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
		DreamJournal::FBufferPart Part = DreamJournalLocal::MakePart(*Buffer, Buffer->Snapshot());   // BUF_OPEN + first SNAP
		Part.bOpenEvent = true;
		Request->Commit.Buffers.push_back(MoveTemp(Part));
		Impl->Push(MoveTemp(Request));
	}
}

void FDreamJournal::OnBufferEdited(const FGuid& Buffer)
{
	if (TSharedPtr<FDreamBuffer> Live = Impl->FindLive(Buffer))
	{
		Impl->Policy.OnEdit(DreamText::ToUtf8(DreamJournalLocal::UuidOf(*Live)), FPlatformTime::Seconds());
	}
}

void FDreamJournal::OnViewStateChanged(const FGuid& Buffer)
{
	if (!Impl->bSessionDirty)
	{
		Impl->bSessionDirty = true;
		Impl->SessionDirtySince = FPlatformTime::Seconds();
	}
}

void FDreamJournal::OnBufferSaved(const TSharedRef<FDreamBuffer>& Buffer)
{
	Impl->Track(Buffer, *this);
	if (!Impl->CanWrite())
	{
		return;
	}
	TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
	DreamJournal::FBufferPart Part = DreamJournalLocal::MakePart(*Buffer, Buffer->Snapshot());
	Part.bMeta = true;          // forced META commit with the new base (§10.14 step 6)
	Request->Commit.Buffers.push_back(MoveTemp(Part));
	// §10.4, PER-04: every save gets a history entry, after the META commit that records it (ten saves, ten entries).
	Request->History.Add(DreamJournalLocal::MakeNote(*Buffer, EDreamHistoryKind::Save, Buffer->Snapshot()));
	Impl->Policy.OnCommitted(DreamText::ToUtf8(DreamJournalLocal::UuidOf(*Buffer)));
	Impl->Push(MoveTemp(Request));
}

void FDreamJournal::OnBufferMetaChanged(const TSharedRef<FDreamBuffer>& Buffer)
{
	Impl->Track(Buffer, *this);
	if (!Impl->CanWrite())
	{
		return;
	}
	TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
	DreamJournal::FBufferPart Part = DreamJournalLocal::MakePart(*Buffer, Buffer->Snapshot());
	Part.bMeta = true;          // the new base and dirty flag (§10.15: reload, Keep mine, deleted on disk)
	Request->Commit.Buffers.push_back(MoveTemp(Part));
	Impl->Policy.OnCommitted(DreamText::ToUtf8(DreamJournalLocal::UuidOf(*Buffer)));
	Impl->Push(MoveTemp(Request));
}

void FDreamJournal::OnBufferClosed(const FGuid& Buffer, const TCHAR* Reason)
{
	TSharedPtr<FDreamBuffer> Live = Impl->FindLive(Buffer);
	if (Live.IsValid() && Impl->CanWrite())
	{
		TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
		DreamJournal::FBufferPart Part = DreamJournalLocal::MakePart(*Live, Live->Snapshot());
		Part.bClose = true;
		Part.CloseReason = DreamText::ToUtf8(Reason ? Reason : TEXT("closed"));
		Request->Commit.Buffers.push_back(MoveTemp(Part));
		// §10.2.5: closing a non-empty scratch buffer writes BUF_CLOSE and a history entry of kind closed.
		const bool bClosedByUser = Reason == nullptr || FCString::Strcmp(Reason, TEXT("closed")) == 0;
		if (bClosedByUser && Live->GetKind() == EDreamBufferKind::Scratch && Live->GetModel().Length() > 0)
		{
			Request->History.Add(DreamJournalLocal::MakeNote(*Live, EDreamHistoryKind::Closed, Live->Snapshot()));
		}
		Impl->Push(MoveTemp(Request));
	}
	Impl->CleanTexts.Remove(Buffer);
	if (Live.IsValid())
	{
		Impl->Policy.Forget(DreamText::ToUtf8(DreamJournalLocal::UuidOf(*Live)));
	}
	Impl->Untrack(Buffer);
}

void FDreamJournal::ResolveRecovery(const FGuid& Buffer, const TCHAR* Action)
{
	if (!Impl->CanWrite())
	{
		return;
	}
	TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
	Request->Commit.Extra.push_back(dream::journal::MakeRecoveryResolved(DreamText::ToUtf8(Impl->SourceNamespace), Impl->SourceGeneration,
		DreamText::ToUtf8(DreamText::GuidToHex(Buffer)), DreamText::ToUtf8(Action ? Action : TEXT("restored"))));
	Impl->AddAllBuffers(*Request);
	Impl->ForgetPendingEdits();
	Impl->Push(MoveTemp(Request));
}

void FDreamJournal::SetSessionState(TConstArrayView<FGuid> TabOrder, const FGuid& ActiveTab)
{
	Impl->TabOrder = TArray<FGuid>(TabOrder);
	Impl->ActiveTab = ActiveTab;
	if (!Impl->bSessionDirty)
	{
		Impl->bSessionDirty = true;
		Impl->SessionDirtySince = FPlatformTime::Seconds();
	}
}

void FDreamJournal::ForceCommit()
{
	if (!Impl->CanWrite())
	{
		return;
	}
	TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
	Impl->AddAllBuffers(*Request);
	Request->Commit.bHasSession = true;
	Request->Commit.Session = Impl->BuildSession();
	Impl->bSessionDirty = false;
	Impl->ForgetPendingEdits();
	Impl->Push(MoveTemp(Request));
}

void FDreamJournal::SetCleanText(const FGuid& Buffer, const dream::TextSnapshot& Clean)
{
	Impl->CleanTexts.Add(Buffer, Clean);
	TSharedPtr<FDreamBuffer> Live = Impl->FindLive(Buffer);
	if (!Live.IsValid() || !Impl->CanWrite())
	{
		return;   // before the reconcile: its checkpoint carries it
	}
	TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
	DreamJournal::FBufferPart Part = DreamJournalLocal::MakePart(*Live, Live->Snapshot());
	Part.CleanText = Clean;
	Request->Commit.Buffers.push_back(MoveTemp(Part));
	Impl->Push(MoveTemp(Request));
}

void FDreamJournal::SetHistorySink(FDreamHistorySink Sink)
{
	Impl->HistorySink = MoveTemp(Sink);
}

bool FDreamJournal::PostIo(FDreamIoSlice Work) const
{
	if (!Impl->bOpen || !Impl->Writer.IsValid())
	{
		return false;
	}
	Impl->Writer->PostIo(MoveTemp(Work));
	return true;
}

FDreamJournalTicket FDreamJournal::SnapshotForRun(FDreamRunId RunId, int32 RunNumber, const FGuid& BufferId, const dream::TextSnapshot& Snapshot,
	EDreamRunRoute Route, bool bDebug)
{
	FDreamJournalTicket Ticket;
	if (!Impl->bOpen || GetHealth() == EDreamJournalHealth::Disabled || GetHealth() == EDreamJournalHealth::ReadOnly)
	{
		return Ticket;   // durable at once (Dream.RunFile with the tab never spawned, WS-07)
	}
	if (!Impl->bReconciled && !Impl->ScanCallback.IsBound() && !Impl->ScanTicker.IsValid())
	{
		// Open, but no tab waits for the scan, so no reconcile is coming (the tab closed before the result arrived): the run
		// goes on without a snapshot, as with the journal closed, instead of waiting for a commit that is held forever.
		return Ticket;
	}

	TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
	TSharedPtr<FDreamBuffer> Buffer = BufferId.IsValid() ? Impl->FindLive(BufferId) : nullptr;
	if (Buffer.IsValid())
	{
		// The snapshot the request's text was cut from, so journal, compiled text and echo agree (§10.2.5 step 1); then the
		// buffer as it is now, when it was edited while the run waited in Starting, so the batch ends at the newest text and
		// a crash during the run recovers it (PER-03; the writer skips a part older than what it already holds).
		Request->Commit.Buffers.push_back(DreamJournalLocal::MakePart(*Buffer, Snapshot));
		const dream::TextSnapshot Now = Buffer->Snapshot();
		if (Now.Version() != Snapshot.Version())
		{
			Request->Commit.Buffers.push_back(DreamJournalLocal::MakePart(*Buffer, Now));
		}
		// §10.2.5 step 3: once the snapshot is durable the writer adds "Before run #14" with the text the run compiled.
		Request->History.Add(DreamJournalLocal::MakeNote(*Buffer, EDreamHistoryKind::Run, Snapshot, RunNumber));
	}
	// (slice B choice, instances: G6 driver B) Every other buffer's pending edits ride along too: while the run executes the
	// 4 Hz scheduler is deferred (RT-01), so an edit made in the second before a long run would otherwise wait for its end,
	// and a kill during the run would lose more than 2 s of typing (PER-01). Unchanged buffers add nothing to the batch.
	Impl->AddAllBuffers(*Request);
	Impl->ForgetPendingEdits();
	Request->Commit.bHasSession = true;
	Request->Commit.Session = Impl->BuildSession();
	Impl->bSessionDirty = false;

	dream::journal::RunBeginInfo Begin;
	Begin.RunId = RunId;
	Begin.Number = static_cast<uint32>(RunNumber);
	Begin.TabUuid = BufferId.IsValid() ? DreamText::ToUtf8(DreamText::GuidToHex(BufferId)) : std::string();
	Begin.BufferVersion = Snapshot.Version();
	Begin.Route = DreamRunText::RouteKey(Route) ? DreamText::ToUtf8(DreamRunText::RouteKey(Route)) : std::string("buffer");
	Begin.bDebug = bDebug;
	Begin.UtcMs = DreamJournalLocal::ToUtcMs(FDateTime::UtcNow());
	Request->Commit.Extra.push_back(dream::journal::MakeRunBegin(Begin));
	Request->Commit.RunBegin = Begin;

	Ticket.Seq = Impl->Push(MoveTemp(Request));
	return Ticket;
}

bool FDreamJournal::IsDurable(FDreamJournalTicket Ticket) const
{
	return Ticket.Seq == 0 || !Impl->Writer.IsValid() || Impl->Writer->IsDurable(Ticket.Seq);
}

bool FDreamJournal::HasFailed(FDreamJournalTicket Ticket, FText& OutReason) const
{
	FString Reason;
	if (Ticket.Seq != 0 && Impl->Writer.IsValid() && Impl->Writer->HasFailed(Ticket.Seq, Reason))
	{
		OutReason = FText::FromString(Reason);
		return true;
	}
	return false;
}

void FDreamJournal::MarkRunEnded(FDreamRunId RunId, EDreamRunOutcome Outcome)
{
	if (!Impl->CanWrite())
	{
		return;
	}
	TUniquePtr<DreamJournal::FRequest> Request = MakeUnique<DreamJournal::FRequest>();
	Impl->AddAllBuffers(*Request);
	Impl->ForgetPendingEdits();
	Request->Commit.Extra.push_back(dream::journal::MakeRunEnd(RunId, DreamJournalLocal::OutcomeKey(Outcome)));
	Request->Commit.RunEndId = RunId;
	Impl->Push(MoveTemp(Request));
}

EDreamJournalHealth FDreamJournal::GetHealth() const
{
	if (!Impl->bOpen || !Impl->Writer.IsValid())
	{
		return EDreamJournalHealth::Disabled;
	}
	return Impl->Writer->GetHealth();
}

FDateTime FDreamJournal::GetLastDurableUtc() const
{
	const int64 Ticks = Impl->Writer.IsValid() ? Impl->Writer->GetLastDurableUtcTicks() : 0;
	return Ticks > 0 ? FDateTime(Ticks) : FDateTime();
}

void FDreamJournal::Shutdown()
{
	if (!Impl->bOpen)
	{
		// The instance lock may be held without an open journal (AcquireInstanceLock alone).
		FDreamWin32FileOps::ReleaseLockFile(Impl->InstanceLock);
		Impl->InstanceLock = 0;
		return;
	}
	FDreamDeferral::RemoveTicker(Impl->CommitTicker);
	FDreamDeferral::RemoveTicker(Impl->ScanTicker);
	Impl->CommitTicker.Reset();
	Impl->ScanTicker.Reset();

	// Final snapshots and CLEAN, then wait at most 2 s for the writer to acknowledge it (§10.2.5).
	TUniquePtr<DreamJournal::FRequest> Clean = MakeUnique<DreamJournal::FRequest>();
	Clean->Kind = DreamJournal::ERequestKind::Clean;
	Impl->AddAllBuffers(*Clean);
	for (const DreamJournal::FBufferPart& Part : Clean->Commit.Buffers)
	{
		Clean->Commit.CleanVersions.emplace_back(Part.Uuid, Part.Snapshot.Version());
	}
	Clean->Commit.bHasSession = true;
	Clean->Commit.Session = Impl->BuildSession();
	Impl->Push(MoveTemp(Clean));

	const double Deadline = FPlatformTime::Seconds() + CleanWaitSeconds;
	while (!Impl->Writer->IsCleanAcknowledged() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.005f);
	}

	if (Impl->Writer->IsCleanAcknowledged())
	{
		Impl->Writer->RequestStop(Deadline);   // queued history work may use the rest of the 2 s (review r1), then the join
		Impl->Thread->WaitForCompletion();
		delete Impl->Thread;
		Impl->Thread = nullptr;
		Impl->CompatLock = Impl->CompatLock != 0 ? Impl->CompatLock : Impl->Writer->TakeCompatLock();
		Impl->Writer.Reset();
	}
	else
	{
		// Abandoned (§10.2.5): the fence first, so the writer performs no further file operation even if a blocked call
		// returns; then the locks. The writer's code and state stay alive until process exit (deliberately leaked).
		UE_LOG(LogDream, Warning, TEXT("The journal writer did not acknowledge CLEAN within %.0f s; it is abandoned."), CleanWaitSeconds);
		Impl->Fence->store(true, std::memory_order_release);
		Impl->Writer->RequestStop();
		Impl->CompatLock = Impl->CompatLock != 0 ? Impl->CompatLock : Impl->Writer->TakeCompatLock();
		(void)Impl->Writer.Release();
		Impl->Thread = nullptr;
	}

	Impl->ReleaseLocks();
	Impl->bOpen = false;
	Impl->bReconciled = false;
}

void FDreamJournal::SetFailCommitsForTest(bool bFail)
{
	if (Impl->Writer.IsValid())
	{
		Impl->Writer->SetFailCommitsForTest(bFail);
	}
}

int32 FDreamJournal::GetRetryQueueLengthForTest() const
{
	return Impl->Writer.IsValid() ? Impl->Writer->GetRetryQueueLengthForTest() : 0;
}

uint64 FDreamJournal::GetGenerationForTest() const
{
	return Impl->Writer.IsValid() ? Impl->Writer->GetGenerationForTest() : 0;
}

void FDreamJournal::ForceRotationForTest()
{
	if (Impl->Writer.IsValid())
	{
		Impl->Writer->ForceRotationForTest();
	}
}

void FDreamJournal::CloseWithoutCleanForTest()
{
	if (!Impl->bOpen)
	{
		return;
	}
	FDreamDeferral::RemoveTicker(Impl->CommitTicker);
	FDreamDeferral::RemoveTicker(Impl->ScanTicker);
	Impl->CommitTicker.Reset();
	Impl->ScanTicker.Reset();
	Impl->Writer->RequestStop();        // the writer drains what was queued, then stops: no CLEAN was asked for
	Impl->Thread->WaitForCompletion();
	delete Impl->Thread;
	Impl->Thread = nullptr;
	Impl->CompatLock = Impl->CompatLock != 0 ? Impl->CompatLock : Impl->Writer->TakeCompatLock();
	Impl->Writer.Reset();
	Impl->ReleaseLocks();
	Impl->bOpen = false;
	Impl->bReconciled = false;
}

#undef LOCTEXT_NAMESPACE
