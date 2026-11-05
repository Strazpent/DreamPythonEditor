// SPDX-License-Identifier: Apache-2.0
// The write-ahead journal and recovery (TDD §10.1-§10.3, §10.7 basic, D17, U7, U29, U30, PER-01..PER-03).
// Game thread API (snapshots <= 0.2 ms, never touches disk); file I/O on the DreamJournalWriter FRunnable through a
// Win64 dream::journal::FileOps (Persistence/DreamWin32FileOps.cpp). Body: DreamJournal.cpp, DreamJournalWriter.cpp,
// DreamStorageRoot.cpp (shell slice).
// The writer thread also runs the background work of §10.2.1 between commits: rotation checkpoints in slices and the
// local-history lane (PostIo, FDreamLocalHistory), whose entries a durable commit can carry (FDreamHistoryNote).
// The multi-instance work (B20-B22) replaces slice A choice A39 (a second instance with its journal Disabled): a second
// instance journals in its own namespace journal/i-<pid>-<process_start>/ with its own ns.lock and stages its history
// entries, REPL history and restore guard there (§10.5); an older build on newer data journals in
// journal/compat-<format>/ (§10.6). The primary's scan also reads the other namespaces of its root (orphans, §10.5);
// FDreamInstances (DreamInstances.h) lists them in Review, merges what they staged and deletes them, and runs
// storage.root_override's probe and the cross-root scan on its DreamStorageProbe thread (§10.1).
//
// Instances (CR-32): FDreamJournal holds no process-wide singleton or static state, and neither do DreamJournalWriter.cpp,
// DreamStorageRoot.cpp and DreamWin32FileOps.cpp (their only statics are constant tables and stateless helpers). Every
// instance owns its writer thread, its fence, its tickers and the locks of its own root, so a second FDreamJournal opened
// on another FDreamStorageRoot (Dream.Persistence.Journal's private temp root) runs beside the module's journal without
// touching it. Keep it so: per-instance state goes in FImpl.
#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Templates/ValueOrError.h"
#include "Run/DreamRunTypes.h"
#include "Buffers/DreamBuffer.h"
#include "Persistence/DreamHistoryTypes.h"

/** The per-user, per-project storage root (§10.1): %LOCALAPPDATA%\DreamPythonEditor\<EngineVer>\<Project>-<hex6>\ on Windows. */
struct FDreamStorageRoot
{
	FString Root;                       // absolute, no trailing slash
	FString ProjectHash;                // "<Name>-<hex6>" (U29)
	bool bIsFallback = false;           // the temp-dir fallback: persistent warning
	bool bIsRemote = false;
	bool bIsOverride = false;           // under storage.root_override (§10.1, PER-08 user layer)
	bool bIsNew = false;                // the hash folder was created at this start (the moved-project offer, §10.1)
	FString BaseDir;                    // the base folder <base>/<EngineVer>/<hash> was built on (an override's owner.json)

	FString JournalDir(const FString& Namespace) const { return Root / TEXT("journal") / Namespace; }
	FString RunLogsDir() const { return Root / TEXT("RunLogs"); }
	FString ReplDir() const { return Root / TEXT("Repl"); }
	FString StateDir() const { return Root / TEXT("state"); }
	FString HistoryDir() const { return Root / TEXT("history"); }
};

enum class EDreamInstanceRole : uint8
{
	Primary,                            // holds <root>/instance.lock; journals in journal/main (or compat-*, §10.6)
	Secondary,                          // another editor holds it: journal/i-<pid>-<process_start>/ (§10.5)
};

enum class EDreamJournalHealth : uint8
{
	Ok,
	Degraded,                           // commits failing, retrying (status strip shows the age turning amber)
	ReadOnly,                           // a newer format on disk (§10.6)
	Disabled,                           // no root, or the journal could not open
};

struct FDreamJournalTicket
{
	uint64 Seq = 0;                     // 0 = no ticket (the snapshot step was skipped or failed)
};

/** One recovered buffer offered by the banner (§10.7.1). */
struct FDreamRecoveredBuffer
{
	FGuid Id;
	EDreamBufferKind Kind = EDreamBufferKind::Scratch;
	FString DisplayName;
	FString Path;
	FString RecoveredText;
	dream::codec::FileFormat Format;
	TOptional<FDreamBaseVersion> Base;
	bool bConflict = false;             // disk SHA-256 differs from the journal's base, or the file is missing (§10.7.2)
	bool bCounted = false;              // counts toward n (§10.7.1)
	bool bDiffersFromDisk = false;      // file buffers: the journal text is not the disk text (dirty at a clean exit, §10.3)
	int32 LinesAdded = 0;               // +12 -3 against the text shown while pending (dream::Diff, T-PER-011)
	int32 LinesRemoved = 0;
	FDateTime LastEditUtc;
	/** Scratch buffers: the clean text (§10.7.1), the text this buffer had when the crashed session restored it (empty for one
	 *  created in that session). A counted scratch buffer shows it while its recovery is pending. */
	TOptional<FString> CleanText;
	/** The recovery was already pending before the session read (an unresolved RECOVERY_PENDING, §10.7.1). */
	bool bFromEarlierStart = false;
};

/** Where an orphaned namespace comes from (§10.5, §10.6, §10.1); Review gives each its own unticked section. */
enum class EDreamOrphanSource : uint8
{
	SecondaryInstance,                  // journal/i-<pid>-<start>/ of a second editor that ended (§10.5)
	Compat,                             // journal/compat-*/ of an older build (§10.6)
	Imported,                           // journal/imported-<oldhash>/ of a moved project (§10.1)
	OtherNamespace,                     // any other namespace folder of this root
	OtherRoot,                          // a namespace under another candidate storage root (§10.1 cross-root scan)
	SameProject,                        // main of a projects.json entry with this .uproject's file identity (§10.1)
	PrimaryView,                        // the live primary's tabs, read-only for a secondary (§10.5 View its tabs)
};

/** One namespace another session left (§10.5 orphans) or the live primary's, as Review lists it. */
struct FDreamOrphanNamespace
{
	FString Key;                        // "<root>|<namespace>": unique among the listed ones
	FString Root;                       // the storage root that holds it
	FString Namespace;
	EDreamOrphanSource Source = EDreamOrphanSource::OtherNamespace;
	bool bClean = false;                // its last batch is CLEAN
	FDateTime EndUtc;                   // its last record
	uint32 Pid = 0;                     // a secondary's or the live primary's process id; 0 when unknown
	FString Detail;                     // imported: the old project path; compat: the format; other root: that root
	bool bReadOnly = false;             // PrimaryView: [Open a copy] only; never written, resolved or deleted
	/** What it offers, never counted in n (§10.7.1): a non-empty scratch buffer, a file buffer whose text differs from the
	 *  disk (or whose file is gone), and every unresolved RECOVERY_PENDING (§10.5, §10.1); the live primary's view
	 *  offers every open buffer. Its texts go to local history before the namespace is deleted (§10.5). */
	TArray<FDreamRecoveredBuffer> Buffers;
	/** wal.*.sjl names and sizes when it was read: a namespace written since is never deleted (§10.1). */
	TMap<FString, int64> WalFiles;
	/** Same root: its ns.lock, held while it is listed; FDreamInstances releases it. */
	uint64 NamespaceLock = 0;
};

/** The result of the startup recovery scan, delivered on the game thread (§10.3 step H). */
struct FDreamRecoveredSession
{
	bool bPreviousExitClean = true;
	bool bUnreadable = false;
	/** A newer Dream wrote the namespace (a newer major or minor, §10.6): read only; this session journals nothing there. */
	bool bNewerFormat = false;
	/** The newest generation was damaged and the previous one was used, or salvage read past a bad frame (§10.2.4, §10.11). */
	bool bFallbackUsed = false;
	bool bSalvageUsed = false;
	FDateTime PreviousEndUtc;
	/** The tab list to restore: the journal SESSION after a crash, else empty (the ini wins after a clean exit, §10.3). */
	TArray<FGuid> SessionTabOrder;
	FGuid SessionActiveTab;
	/** Text of every journaled buffer (dirty and scratch buffers restore from here; clean files from disk). */
	TArray<FDreamRecoveredBuffer> Buffers;
	TArray<FDreamLostRun> LostRuns;
	bool bPausedAtExit = false;
	int32 CountedBuffers = 0;           // n of the banner
	/** Buffers whose last record is BUF_CLOSE: never restored, even when a stale ini tab list still names them (VIS-07). */
	TSet<FGuid> ClosedIds;
	/** Per-tab view state from the SESSION record (carets, first visible line, U3 toggle, U6 namespace). */
	TMap<FGuid, FDreamBufferViewState> SessionViewStates;
	/** The REPL prompt's journaled text ("repl-input", §6.5). */
	TOptional<FString> ReplInputText;
	/** The generation the scan read; RECOVERY_RESOLVED names it (§10.2.2). */
	uint64 SourceGeneration = 0;
	/** Previous session's buffers whose journal text was dropped because it could not be verified (§10.2.4 step 5). */
	int32 UnverifiedBuffers = 0;
	/** The namespace the scan read and this session's recoveries come from: "main", a secondary's own, or compat-* (§10.6). */
	FString SourceNamespace;
	/** §10.6: the newer format found in journal/main (bNewerFormat); this session journals in compat-<format> instead. */
	bool bCompat = false;
	int32 NewerMajor = 0;
	int32 NewerMinor = 0;
	/** A primary's scan of its own root: the other namespaces with a free ns.lock (secondaries, compat-*, imported-*, §10.5),
	 *  each holding its ns.lock until FDreamInstances resolves or releases it. Never counted in n (§10.7.1). */
	TArray<FDreamOrphanNamespace> Orphans;
};

/** The scan result, on the game thread. Returns true when it was consumed (the callee may then move from it); false keeps it
 *  for the next RequestRecoveryScan, so a tab closed before the result arrived never loses the restore (PER-02). */
DECLARE_DELEGATE_RetVal_OneParam(bool, FDreamOnRecoveryScan, FDreamRecoveredSession& /*Session*/);

class DREAMEDITOR_API FDreamJournal
{
public:
	static constexpr double TickHz = 4.0;               // §10.2.5
	static constexpr double CleanWaitSeconds = 2.0;     // OnPreExit wait for CLEAN

	FDreamJournal();
	~FDreamJournal();

	/** §10.1 rules: platform default root, then the temp fallback; never under the engine or project tree. */
	static TValueOrError<FDreamStorageRoot, FText> ResolveStorageRoot(const FString& UProjectPath);

	/**
	 * Tab spawn (B2): root, instance lock, writer thread; queues the recovery scan. The lock decides the role (§10.3 step B,
	 * §10.5): free, the primary journals in journal/main (compat-<format> when main is newer, §10.6) and its scan also reads
	 * the root's other namespaces (orphans); held, the secondary journals in journal/i-<pid>-<process_start>/ under its own
	 * ns.lock. False only when no journal could open (no root, the namespace lock or the writer thread failed). Game thread.
	 */
	bool Open(const FDreamStorageRoot& Root, EDreamInstanceRole& OutRole);
	/**
	 * Only <root>/instance.lock (§10.5), held from then on; Open takes it at the tab spawn, where the role is decided
	 * (§10.3 step B), and reuses a lock already held. The role is fixed for the session: false and Secondary while another
	 * editor holds the lock. Game thread.
	 */
	bool AcquireInstanceLock(const FDreamStorageRoot& Root, EDreamInstanceRole& OutRole);
	bool IsOpen() const;
	const FDreamStorageRoot& GetStorageRoot() const;
	/** "<session start>" folder name for run logs: the editor session's UTC start as yyyymmdd-hhmmss (OUT-08). */
	FString GetSessionStartStamp() const;

	// ---- Role and namespace (§10.5, §10.6) ----

	/** The role Open decided (Primary before Open). */
	EDreamInstanceRole GetRole() const;
	/** The namespace this session writes: "main", "i-<pid>-<process_start>" or "compat-<format>" (empty before Open). The
	 *  writer moves a primary to compat-* when its scan finds main newer (§10.6); this follows once the scan arrived. */
	FString GetNamespace() const;
	/** <root>/journal/<namespace>. */
	FString GetNamespaceDir() const;
	/** The restore guard's folder (§10.8): <root>/state for the primary, <sec>/state for a secondary (§10.5). */
	FString GetStateDir() const;
	/** Where this session's history entries go (§10.4, §10.5): <root>/history, or a secondary's <sec>/pending-history. */
	FString GetHistoryDir() const;
	/** The instance.lock content of this session's root as another editor wrote it (§10.5; empty while this is the primary). */
	FString GetPrimaryLockContent() const;
	/** Test hook: the secondary namespace id Open uses instead of FDreamAudit::MakeSecondaryId() (several journals of one
	 *  process on one private root). */
	void SetSecondaryIdForTest(const FString& Id);
	/** Takes the same-root orphans' ns.lock handles out of a scan result: the caller releases them (FDreamInstances). */
	static void ReleaseOrphanLocks(TArray<FDreamOrphanNamespace>& Orphans);

	/** The scan result arrives on the game thread; the first commit of the session (a new generation's checkpoint) waits for
	 *  Reconcile, which the shell calls after it has applied the restore (§10.2.3, §10.3). A result the callback did not
	 *  consume is kept and delivered to the next request. */
	void RequestRecoveryScan(FDreamOnRecoveryScan OnGameThread);
	/**
	 * Starts this session's generation with every live buffer and their pending recoveries. In safe mode (§10.7.1, §10.8)
	 * SkippedSession is the session the restore skipped: every buffer of it that is not live is carried in the checkpoint
	 * (its journal text, open info and kind) and its counted recoveries as RECOVERY_PENDING, so ignoring the banner loses
	 * nothing and a later Restore tabs still finds them. Only while the journal is open: a closed journal stays
	 * unreconciled, so a later Open starts its generation normally.
	 */
	void Reconcile(TConstArrayView<TSharedRef<FDreamBuffer>> LiveBuffers, const FDreamRecoveredSession* SkippedSession = nullptr);
	bool IsReconciled() const;

	// ---- Buffers (game thread, O(1)) ----

	void OnBufferOpened(const TSharedRef<FDreamBuffer>& Buffer);     // BUF_OPEN + first SNAP
	void OnBufferEdited(const FGuid& Buffer);                       // CommitPolicy::OnEdit
	void OnViewStateChanged(const FGuid& Buffer);
	void OnBufferSaved(const TSharedRef<FDreamBuffer>& Buffer);      // forced META commit (§10.14 step 6) and a history save entry
	/** A forced META commit with the buffer's current base, path, display and dirty flag, and no history entry: an external
	 *  reload, Keep mine, a file deleted on disk (§10.15, BUF-04). Game thread. */
	void OnBufferMetaChanged(const TSharedRef<FDreamBuffer>& Buffer);
	void OnBufferClosed(const FGuid& Buffer, const TCHAR* Reason);  // BUF_CLOSE ("closed", "discarded", "restored")
	/** RECOVERY_RESOLVED for a pending recovery (Restore all / Discard, §10.7.3). */
	void ResolveRecovery(const FGuid& Buffer, const TCHAR* Action);
	/**
	 * A scratch buffer's clean text (§10.7.1, CLEAN_TEXT): the text it had when this session restored it, kept in every
	 * checkpoint so that after a crash its recovery can wait over that text. Reconcile records it for every live scratch
	 * buffer; a buffer restored later (Leave safe mode) calls this, which commits it at once.
	 */
	void SetCleanText(const FGuid& Buffer, const dream::TextSnapshot& Clean);
	/** Session state for the SESSION record (tab order, active tab, per-tab view state). Called when it changes. */
	void SetSessionState(TConstArrayView<FGuid> TabOrder, const FGuid& ActiveTab);
	/** Focus leaving Dream, tab close: force a commit now (§10.2.5). */
	void ForceCommit();

	// ---- Runs (FDreamRunService, game thread) ----

	/**
	 * The pre-run snapshot (U7 step 3, §10.2.5): queues a forced commit with Snapshot's DELTA or SNAP (BufferId invalid for
	 * REPL and console: only RUN_BEGIN), the SESSION and RUN_BEGIN. <= 0.2 ms; never waits on disk.
	 * While the journal is not open (the Dream tab has never been spawned this session, as for Dream.RunFile or an API run
	 * with the tab closed, WS-07) or is Disabled, it returns a zero ticket and writes nothing.
	 */
	FDreamJournalTicket SnapshotForRun(FDreamRunId RunId, int32 RunNumber, const FGuid& BufferId, const dream::TextSnapshot& Snapshot,
		EDreamRunRoute Route, bool bDebug);
	/** Atomic load; the run service polls it from its Starting handler. A zero ticket (journal not open or Disabled) is
	 *  durable at once. */
	bool IsDurable(FDreamJournalTicket Ticket) const;
	/** The commit failed (not executed: "Journal snapshot failed: <reason>"). */
	bool HasFailed(FDreamJournalTicket Ticket, FText& OutReason) const;
	/** RUN_END with a forced commit. */
	void MarkRunEnded(FDreamRunId RunId, EDreamRunOutcome Outcome);

	// ---- Status ----

	EDreamJournalHealth GetHealth() const;
	/** UTC of the last durable commit, for the status strip's journal age (WS-05). */
	FDateTime GetLastDurableUtc() const;

	/** OnPreExit: final snapshots, CLEAN, wait at most 2 s, release locks; a writer that has not acknowledged is abandoned
	 *  behind the FileOps fence (§10.2.5). */
	void Shutdown();

	// ---- Background work (§10.2.1, §10.4) ----

	/** Game thread, before Open: turns a durable commit's history notes into background work (FDreamLocalHistory). */
	void SetHistorySink(FDreamHistorySink Sink);
	/** Any thread: queues background work on the writer thread; false (and the work dropped) when the journal is not open. */
	bool PostIo(FDreamIoSlice Work) const;

	// ---- Test hooks (Dream.Persistence.Journal, on its private root; per instance, CR-32) ----

	/** Every commit of this instance's writer fails, as a full disk would, until turned off; the retry then replays. */
	void SetFailCommitsForTest(bool bFail);
	/** Ends the session as a crash would: the writer finishes what it was given, no final snapshots and no CLEAN; the locks
	 *  are released so another instance can open the root. */
	void CloseWithoutCleanForTest();
	/** The commits waiting behind a failed one in the writer's retry queue (P2-per-02: plain text commits fold in). */
	int32 GetRetryQueueLengthForTest() const;
	/** The writer's current generation, and a rotation forced at the next commit (sliced checkpoint, §10.2.3). */
	uint64 GetGenerationForTest() const;
	void ForceRotationForTest();

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
