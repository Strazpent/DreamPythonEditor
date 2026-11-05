// SPDX-License-Identifier: Apache-2.0
// FDreamInstances: the multi-instance and storage-root service (TDD §10.1, §10.5, §10.6; PER-05, PER-08 user layer):
//  - the storage root: storage.root_override probed on DreamStorageProbe (folders, owner.json with the account's SID, a
//    probe file), at most 2 s; then the platform default, then the temp fallback; the persistent notices;
//  - projects.json under its lock, the moved-project offer ([Import] [Ignore], remembered there) and the same-identity
//    entries;
//  - orphans: the namespaces of this root the primary's scan found (their ns.lock held while listed), the cross-root scan
//    after the first checkpoint, per-buffer resolution, then the history copy of their texts, the merge of what a
//    secondary staged (pending-history, repl-history.jsonl) and the delete under the §10.1 lock and size rules;
//  - a secondary's side: the primary's PID for the banner, "View its tabs" (its main namespace read-only), the 30 s poll
//    of the primary's lock and "The other editor instance ended unexpectedly".
// Game thread API; file work on the DreamStorageProbe thread (or the journal writer for history merges); results come back
// through an FDreamDeferral ticker (never inside a run, RT-01). The module owns one for its journal; tests make their own
// on private roots. Body: DreamInstances.cpp (instances).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Persistence/DreamJournal.h"
#include "Templates/ValueOrError.h"

class FDreamLocalHistory;
class FDreamReplHistory;
class FDreamStorageProbe;
class FDreamWin32FileOps;

/** A moved project's journal that can be imported (§10.1 "Moved projects"). */
struct FDreamMovedProject
{
	FString OldHash;
	FString Name;
	FString OldProjectPath;             // the .uproject that no longer exists
	FString OldRoot;                    // its storage root
	FDateTime LastSeenUtc;
};

class DREAMEDITOR_API FDreamInstances
{
public:
	static constexpr double ProbeWaitSeconds = 2.0;        // §10.1: the tab spawn waits for the override at most 2 s (proposed)
	static constexpr double PollSeconds = 30.0;            // §10.5: a secondary polls the primary's lock (proposed)
	static constexpr double ProjectsLockSeconds = 1.0;     // §10.1: projects.json.lock, created exclusively with retries

	FDreamInstances(FDreamJournal& InJournal, FDreamLocalHistory& InHistory, FDreamReplHistory& InRepl);
	~FDreamInstances();
	FDreamInstances(const FDreamInstances&) = delete;
	FDreamInstances& operator=(const FDreamInstances&) = delete;

	// ---- The storage root (§10.1) ----

	/**
	 * The root this session uses. OverrideBase (storage.root_override, the user layer in M1; empty for none) is probed on
	 * DreamStorageProbe for at most WaitSeconds: folders created, <override>/owner.json checked (written at first use; another
	 * account's is rejected), a probe file. Then the platform default, then the temp fallback (FDreamJournal::ResolveStorageRoot).
	 * A rejected or silent override and the temp fallback leave a persistent notice. Game thread.
	 */
	TValueOrError<FDreamStorageRoot, FText> ResolveStorageRoot(const FString& UProjectPath, const FString& OverrideBase,
		double WaitSeconds = ProbeWaitSeconds, bool bStrict = false);
	// bStrict (review r1): the override is this session's only root (-DreamStorageRoot): when it cannot be used, the result
	// is an error with a notice and no journal, never the platform default.
	/** The root the last ResolveStorageRoot chose (the G6 driver checks it before it opens the tab). */
	const TOptional<FDreamStorageRoot>& GetResolvedRoot() const { return ResolvedRoot; }
	/** The override this session would use: -DreamStorageRoot=<dir> (drivers and tests, this session only), else the user
	 *  setting storage.root_override. Game thread. */
	static FString GetConfiguredOverride();
	/** -DreamStorageRoot=<dir> names this session's root (drivers and tests): such a session leaves projects.json alone, so
	 *  a test root never becomes the project's recorded root (slice B choice). */
	static bool HasSessionOverride();
	/** Editor Preferences > Plugins > Dream, where storage.root_override is (B23: [Open storage settings]). Game thread. */
	static void OpenStorageSettings();
	/** The persistent storage notices (§10.1, §10.11): an override that was rejected or did not answer, the temp fallback. */
	const TArray<FText>& GetStorageNotices() const { return StorageNotices; }

	// ---- projects.json (§10.1) ----

	/**
	 * Reads, updates and atomically replaces ProjectsJson under projects.json.lock (probe thread): this project's entry
	 * (uproject, name, file_id, root, last_seen_utc), the moved-project offers and the same-identity entries. Root is the
	 * root this session uses. The offers arrive on the game thread (OnChanged). Game thread.
	 */
	void UpdateProjects(const FString& ProjectsJson, const FString& UProjectPath, const FDreamStorageRoot& Root);
	const TArray<FDreamMovedProject>& GetMovedProjects() const { return MovedProjects; }
	/** Import (§10.1): its journal namespaces are copied to <root>/journal/imported-<oldhash>[-<ns>]/ and its history merged
	 *  into ours; they are then listed as orphans. Remembered in projects.json. The primary only. */
	void ImportMovedProject(const FString& OldHash);
	/** Ignore: remembered in projects.json; never offered again. */
	void IgnoreMovedProject(const FString& OldHash);

	// ---- Orphans (§10.5, §10.1) ----

	/** The primary's scan of its own root (FDreamRecoveredSession::Orphans): listed with their ns.lock held; a namespace with
	 *  nothing to offer is merged and deleted at once. Game thread. */
	void AddSameRootOrphans(TArray<FDreamOrphanNamespace>&& Orphans);
	/**
	 * §10.1 cross-root scan, after the primary's reconcile and first checkpoint: every other candidate root (the override,
	 * the platform default, the temp fallback, projects.json's recorded root, and main of the same-identity entries) that
	 * passes the §10.1 checks, is not this root by folder identity, names no other account in owner.json and has no live
	 * primary. ExtraRoots (tests) are scanned too. Nothing waits on it; what it finds joins GetOrphans (OnChanged).
	 */
	void StartCrossRootScan(const TArray<FString>& ExtraRoots = TArray<FString>());
	const TArray<FDreamOrphanNamespace>& GetOrphans() const { return Orphans; }
	const FDreamOrphanNamespace* FindOrphan(const FString& Key) const;
	/**
	 * One offered buffer of an orphan was restored or discarded in this session. When it was the last one, the namespace's
	 * texts go to local history ("recovered", discarded ones are there already), what it staged merges (history and REPL),
	 * and it is deleted: under its held ns.lock in this root; in another root only while that root's instance.lock and the
	 * namespace's ns.lock are both held and its wal files are as they were read (else it is listed again, §10.1).
	 */
	void ResolveOrphanBuffer(const FString& Key, const FGuid& Buffer, bool bDiscarded);
	/** Whether an orphan's buffer was restored or discarded in this session (Review hides it). */
	bool IsOrphanBufferResolved(const FString& Key, const FGuid& Buffer) const;
	/** The orphans' buffers still offered (the n = 0 banner, §10.7.1). */
	int32 CountOfferedBuffers() const;
	/** The namespaces deleted this session (tests and the log). */
	const TArray<FString>& GetDeletedForTest() const { return Deleted; }

	// ---- The secondary's side (§10.5) ----

	/** The primary's PID from instance.lock, 0 when unknown (a lock held with empty content: the banner shows no PID). */
	uint32 GetPrimaryPid() const { return PrimaryPid; }
	/** View its tabs: the primary's main namespace read read-only on the probe thread (the tolerant reader copes with its
	 *  appends); the result is GetPrimaryView. */
	void RequestPrimaryView();
	const TOptional<FDreamOrphanNamespace>& GetPrimaryView() const { return PrimaryView; }
	/** The poll found the primary's lock free and main without CLEAN: "The other editor instance ended unexpectedly". */
	bool HasPrimaryEndedUnexpectedly() const { return bPrimaryEnded; }

	// ---- Lifecycle ----

	/** After FDreamJournal::Open (the role is known): a secondary starts staging its REPL entries and polling. Game thread. */
	void OnJournalOpened();
	/** Anything listed, offered or found changed (banners and Review rebuild). Game thread. */
	FSimpleMulticastDelegate& OnChanged() { return Changed; }
	/** OnPreExit: tickers removed, held ns.locks released, the probe abandoned behind its fence (never joined). */
	void Shutdown();

	// ---- Test hooks ----

	void SetPollSecondsForTest(double Seconds) { PollInterval = Seconds; NextPollSeconds = 0.0; }
	/** Queues a probe job that sleeps this long (a storage folder that does not answer, T-PER-055). */
	void BlockProbeForTest(double Seconds);
	/** No probe job, history merge or finalization in flight. */
	bool IsIdleForTest() const;
	/** The completions now (specs poll this instead of waiting for the 4 Hz ticker). */
	void PumpForTest();

	struct FShared;

private:
	struct FPendingFinalize
	{
		int32 Outstanding = 0;          // history entries and merges not yet answered
		bool bFailed = false;
	};

	void EnsureTicker();
	bool Tick();
	void Broadcast();
	bool PostProbe(TFunction<void(FDreamWin32FileOps&)> Work);
	void Finalize(const FString& Key);
	void FinishFinalize(const FString& Key);
	void ScanImported(const TArray<FString>& Namespaces);
	void Poll();

	FDreamJournal& Journal;
	FDreamLocalHistory& History;
	FDreamReplHistory& Repl;
	TSharedPtr<FShared, ESPMode::ThreadSafe> Shared;
	FDreamStorageProbe* Probe = nullptr;             // created at first use; leaked when abandoned at OnPreExit (§10.1)
	FTSTicker::FDelegateHandle Ticker;
	FSimpleMulticastDelegate Changed;

	FString UProject;
	FString OverrideBase;
	TOptional<FDreamStorageRoot> ResolvedRoot;
	FString ProjectsJson;
	TArray<FText> StorageNotices;
	TArray<FDreamMovedProject> MovedProjects;
	TArray<FDreamOrphanNamespace> Orphans;
	TMap<FString, FPendingFinalize> Finalizing;
	TArray<FString> Deleted;
	TSet<FString> ResolvedBuffers;                  // "<key>|<buffer hex>": restored or discarded this session
	TSet<FString> DiscardedBuffers;
	bool bCrossRootStarted = false;
	int32 InFlight = 0;

	uint32 PrimaryPid = 0;
	TOptional<FDreamOrphanNamespace> PrimaryView;
	bool bPrimaryEnded = false;
	bool bPollInFlight = false;
	double PollInterval = PollSeconds;
	double NextPollSeconds = 0.0;
};
