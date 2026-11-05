// SPDX-License-Identifier: Apache-2.0
// Open buffers, Untitled-N numbering, pseudo filenames, load and save (TDD §4.13, §5.3.2 filename rules, §10.14,
// BUF-01, BUF-03, RUN-06), external changes (§10.15 M1, BUF-04, through FDreamFileWatch), the read-only and encoding save
// failures (BUF-05 M1, BUF-06), Recent Files (BUF-03 M1) and the per-buffer reopen of a closed tab (§6.4 Reopen tab).
// Game thread API; every save and every external-change check runs on a worker task (never the journal writer, §10.14).
// Body: DreamBufferManager.cpp, DreamFileSaver.cpp, DreamFileWatch.cpp (buffers).
// (slice A choice, A40) Plain mode holds files up to 1 MB (§4.15 row (3), BUF-09, T-VIEW-018): the stock widget lays out and
// shapes every line on the game thread, so a larger file is refused instead of opened. Deferred (G1): the progressive and
// mapped tiers of §4.13 (above 10 MB, above 50 MB). Deferred (M2): Save All, Revert and the Compare of the dirty bar.
// Deferred (M4): the async checkout (§11.6.5).
#pragma once

#include "CoreMinimal.h"
#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamFileSaver.h"

class FDreamFileWatch;
struct FDreamFileCheckResult;

DECLARE_DELEGATE_TwoParams(FDreamOnSaved, bool /*bOk*/, const FText& /*Error*/);

/** A dirty buffer whose file changed on disk (§10.15 Dirty buffer): the bar "<name> changed on disk. [Keep mine] [Reload]"
 *  (Compare is M2). Also a clean buffer whose new disk version cannot be loaded (bCanReload false: over 1 MB, binary). */
struct FDreamDiskChange
{
	FDreamBaseVersion NewBase;
	bool bCanReload = true;
	FText Error;                        // why the disk version cannot be loaded (bCanReload false)
	FString Text;                       // the decoded disk text (§4.13)
	dream::codec::FileFormat Format;
	bool bDecodedReadOnly = false;
	FText DecodedReadOnlyReason;
};

/** The last failed save of a buffer (§10.14 step 5): the tab's bar shows it until a save succeeds or it is dismissed. */
struct FDreamSaveFailureInfo
{
	EDreamSaveFailure Kind = EDreamSaveFailure::None;
	FText Error;
	FString Path;                       // the target of the failed save
	int32 BadLine = 0;                  // Encoding: the first line the encoding cannot hold
};

class DREAMEDITOR_API FDreamBufferManager
{
public:
	static constexpr int64 PlainModeMaxBytes = 1 << 20;
	static constexpr int64 ReadOnlyMaxBytes = 50ll << 20;
	/** File ▸ Recent Files keeps this many paths (proposed, BUF-03 M1). */
	static constexpr int32 MaxRecentFiles = 15;

	FDreamBufferManager();
	~FDreamBufferManager();

	/** ShutdownModule step 6: the file watch's directory registrations, tickers and bindings go while every service lives. */
	void Shutdown();

	/** A new scratch buffer "Untitled-N" (or Name), optionally with a fixed ID (restore, new_tab's pre-allocated ID). In memory
	 *  and journaled; (scratch, PO-05) the UI and Python entry points use CreateScratchFile instead, and this stays for internal
	 *  and test use and for the fallback when the scratch folder cannot be written. */
	TSharedRef<FDreamBuffer> CreateScratch(const FString& Text = FString(), const FString& Name = FString(), const FGuid& Id = FGuid());
	/** Opens a file, or returns the open buffer for it. Decodes with dream::codec (§4.13); error text on refusal (binary, UTF-32). */
	TSharedPtr<FDreamBuffer> OpenFile(const FString& Path, FText& OutError, const FGuid& Id = FGuid());
	/** A recovered or restored buffer whose text and metadata come from the journal (§10.3, §10.7). */
	TSharedRef<FDreamBuffer> AddRestored(const FGuid& Id, EDreamBufferKind Kind, const FString& DisplayName, const FString& Path,
		const FString& Text, const dream::codec::FileFormat& Format, const TOptional<FDreamBaseVersion>& Base);
	/** The REPL prompt's buffer (kind ReplInput, journaled as "repl-input", §6.5). */
	TSharedRef<FDreamBuffer> GetReplInputBuffer();

	/** Writes BUF_CLOSE; a non-empty scratch buffer stays reopenable (Ctrl+Shift+T, §10.2.5). True when the closed buffer
	 *  was kept for Reopen Closed Tab (an empty scratch buffer and a pending recovery are not). */
	bool Close(const FGuid& Id);
	/** Ctrl+Shift+T: the most recently closed buffer, reopened with its text; null when none. */
	TSharedPtr<FDreamBuffer> ReopenClosed();
	/**
	 * The per-buffer reopen (§6.4 Reopen tab, B86): the closed buffer Id with its text and its ID, so a traceback's pseudo
	 * filename names it again. A closed file buffer whose path is open in another tab meanwhile gives that tab instead.
	 * Null when Id is not among the closed buffers.
	 */
	TSharedPtr<FDreamBuffer> ReopenClosed(const FGuid& Id);
	/** A closed buffer by tab ID or pseudo-file prefix (the link's Reopen tab offer, §6.4). */
	TSharedPtr<FDreamBuffer> FindClosedByIdHex(const FString& HexOrPrefix) const;
	/** The closed buffer a "<dream:name#id>" pseudo filename names (not REPL or Console inputs); null when none (§6.4). */
	TSharedPtr<FDreamBuffer> FindClosedByFilename(const FString& PseudoFilename) const;

	TSharedPtr<FDreamBuffer> FindById(const FGuid& Id) const;
	/** By the 32-hex tab ID of the public API or a pseudo filename's short id prefix (first 4+ hex digits). */
	TSharedPtr<FDreamBuffer> FindByIdHex(const FString& HexOrPrefix) const;
	TSharedPtr<FDreamBuffer> FindByPath(const FString& Path) const;
	/** By display name (enqueue_run targets a tab by ID or name). */
	TSharedPtr<FDreamBuffer> FindByName(const FString& DisplayName) const;
	TArray<TSharedRef<FDreamBuffer>> GetOpenBuffers() const;
	/** (scratch) An open or closed (reopenable) buffer holds Path: scratch_N.py numbering skips it. */
	bool IsPathInUse(const FString& Path) const;
	/** (scratch-p2-m1) A buffer still needs the file at Path: an open one, a closed one with unsaved text, or one whose scratch
	 *  save is in flight or due. An empty scratch file with no such holder may be used again for a new tab. */
	bool IsPathBusy(const FString& Path) const;

	// ---- (scratch, PO-05) Scratch files: an unnamed tab is scratch_N.py in the scratch folder, saved as you type ----

	/** The auto-save's timing (the journal's rule, §10.2.1): 1 s after the last edit, never later than 2 s after the oldest
	 *  edit not yet saved. */
	static constexpr double ScratchQuietSeconds = 1.0;
	static constexpr double ScratchMaxDelaySeconds = 2.0;
	/** (R-p1-3) The timer, focus-loss and run-start saves add at most one local-history entry per buffer this often; a close,
	 *  the exit, Save As and the first save after a failure always add one. */
	static constexpr double HistoryThrottleSeconds = 300.0;

	/** Why a scratch file is saved (the test hook records the last one). */
	enum class EScratchSave : uint8
	{
		Timer,              // the 1 s / 2 s rule
		FocusLost,          // keyboard focus left its code view, or the editor lost the foreground
		RunStart,           // a run reached Running (after its journal snapshot)
		Close,              // its tab closed (the file is kept; text that cannot be written goes to local history)
		Exit,               // OnPreExit (synchronous)
		Other,              // Keep mine, a caller of SaveScratchNow (never throttled)
	};

	/**
	 * A new unnamed tab's buffer: scratch_N.py made empty in the scratch folder (DreamScratch::CreateNextFile, lowest free N)
	 * and opened as a File buffer marked scratch, so its label is the file name and tracebacks name the real path. Text,
	 * when given, becomes its text at once (dirty, so the auto-save writes it within ScratchQuietSeconds; no undo step).
	 * The first one also seeds the samples (DreamScratch::SeedSamplesOnce). Null with OutError when the folder cannot be
	 * written: the caller falls back to CreateScratch, a journaled Untitled-N.
	 */
	TSharedPtr<FDreamBuffer> CreateScratchFile(const FString& Text, FText& OutError, const FGuid& Id = FGuid());
	/**
	 * Starts the save of a dirty scratch-file buffer now (through SaveAsAsync). False when there is nothing to do or it
	 * must not save now: not dirty, deleted on disk or a pending disk change (BUF-04 as for any file: the bars decide), a
	 * pending recovery, read-only, or a failed save whose bar is still shown (Exit and Other try again; Retry, the bar's x
	 * or the next successful save resume the auto-save). With a save of it in flight nothing starts: the newer text goes
	 * once that save lands (never two writes of one file at once).
	 */
	bool SaveScratchNow(const FGuid& Id, EScratchSave Reason);
	/** (R-p1-4) The auto-save keeps this buffer's text on disk now: a scratch file with no failure bar, no disk change, not
	 *  deleted on disk, not read-only, no pending recovery. Its tab shows no dirty mark then. */
	bool IsScratchAutoSaveArmed(const FDreamBuffer& Buffer) const;
	/** A scratch save of Id is running. For a caller that must not move the file meanwhile (nothing waits on it today: a
	 *  rename during a save is handled when the save lands, scratch-p1-m3). */
	bool IsSaveInFlight(const FGuid& Id) const;
	/** Test hook (scratch-p2-m3): scratch saves started for Id so far, close saves included; kept after the tab closes. */
	int32 GetSavesStartedForTest(const FGuid& Id) const;
	/** (scratch-p1-m2, p2-M1) A dirty scratch tab restored after a clean exit whose file changed since: the buffer takes the
	 *  journal's base (JournalBase), so every check of the watch (app activation, run end) finds the newer disk version and
	 *  keeps the [Keep mine] [Reload] bar; the disk text waits there now, and the auto-save is paused until it is answered. */
	void HoldRestoredDiskVersion(const FGuid& Id, const FString& DiskText, const TOptional<FDreamBaseVersion>& JournalBase);
	/** Test hook (the timing specs): the focus-loss saves (code view and editor deactivation) are skipped while off. */
	void SetFocusSavesForTest(bool bEnabled);
	bool AreFocusSavesEnabled() const;
	/** SaveScratchNow for every open scratch-file buffer. */
	void SaveAllScratchNow(EScratchSave Reason);
	/** OnPreExit, before the journal's CLEAN: saves still in flight finish (at most 2 s), then every dirty scratch file is
	 *  written synchronously on the game thread (an async result could not land after the exit) and the journal records it. */
	void SaveScratchFilesAtExit();
	/**
	 * The migration of a journaled Untitled-N (an earlier session's, or this session's fallback) to scratch_N.py: the text is
	 * saved there through Save As, which keeps the buffer's ID and retargets the tab. A failed write leaves the buffer as it
	 * was (its save failure is cleared, and the empty file it made is removed again). Refused for a buffer that is not a
	 * plain Scratch buffer (a pending recovery, a file, a recovered tab).
	 */
	void ConvertToScratchFile(const FGuid& Id, FDreamOnSaved OnDone);

	/** Test hook (Dream.Buffers.ScratchFiles): a scratch buffer's auto-save so far. Times are FPlatformTime::Seconds. */
	struct FScratchSaveStatsForTest
	{
		int32 Saves = 0;                    // saves started (Close and Exit included)
		int32 HistoryEntries = 0;           // of those, the ones that add a local-history entry (R-p1-3)
		EScratchSave LastReason = EScratchSave::Other;
		double LastSaveStartSeconds = 0.0;
		double LastFirstEditSeconds = 0.0;  // the oldest unsaved edit when the last save started (0: none, e.g. Close)
		double LastEditSeconds = 0.0;       // the newest edit when it started
		bool bSaving = false;
		bool bPending = false;              // an edit waits for the timer
	};
	FScratchSaveStatsForTest GetScratchSaveStatsForTest(const FGuid& Id) const;

	/**
	 * The compile filename of a buffer (§5.3.2): its normalized real path when it has one, else "<dream:Untitled-3#a41f>",
	 * the id being the first 4 hex digits of the FGuid extended until unique among known buffers (RUN-06), so links survive renames.
	 */
	FString GetCompileFilename(const FDreamBuffer& Buffer) const;
	/** A traceback filename back to a buffer: a real path or a "<dream:...#id>" pseudo file; null when unknown or closed. */
	TSharedPtr<FDreamBuffer> ResolveFilename(const FString& Filename) const;

	// ---- Saving (§10.14) ----

	/** §10.14 subset: snapshot on the GT, encode and write on a worker (temp file in the target dir, flush, ReplaceFileW or
	 *  MoveFileExW write-through), then on the GT: base, clean, journal META. Failure keeps the buffer dirty with the OS error
	 *  and records it (GetSaveFailure, OnFileStateChanged) for the tab's bar. The bytes follow the text's PEP 263 cookie when it
	 *  names another supported encoding than the buffer's (§4.13 step 4), so the cookie and the bytes always agree. */
	void SaveAsync(const FGuid& Id, FDreamOnSaved OnDone, const FDreamSaveOptions& Options = FDreamSaveOptions());
	/** Save As promotes a scratch buffer to a file buffer (BUF-01). */
	void SaveAsAsync(const FGuid& Id, const FString& NewPath, FDreamOnSaved OnDone, const FDreamSaveOptions& Options = FDreamSaveOptions());
	/** The last failed save of Id, null after a successful save or ClearSaveFailure. */
	const FDreamSaveFailureInfo* GetSaveFailure(const FGuid& Id) const;
	void ClearSaveFailure(const FGuid& Id);
	/**
	 * "Save as UTF-8?" (BUF-06, §4.13): the buffer's encoding becomes UTF-8 without a BOM (line endings kept). A PEP 263
	 * cookie on line 1 or 2 that names another encoding is rewritten to "utf-8" as one undoable edit, so the bytes and the
	 * cookie agree when Python or Dream reads the file again. The caller saves next. False with OutError when it cannot.
	 */
	bool ConvertToUtf8(const FGuid& Id, FText& OutError);

	// ---- External changes (§10.15 M1, BUF-04) ----

	/** The pending disk change of a dirty buffer (or a disk version that cannot be loaded); null when none. */
	const FDreamDiskChange* GetDiskChange(const FGuid& Id) const;
	/** Keep mine: the new disk stamp becomes the base, so the bar does not return for that version and the next Save
	 *  overwrites it (§10.15). */
	void KeepMine(const FGuid& Id);
	/** Reload: the buffer's text goes to local history, then the disk text applies as one undo group "Reloaded from disk",
	 *  so Ctrl+Z brings the buffer's text back (§10.15). False when there is no loadable disk change or a run is active. */
	bool ReloadFromDisk(const FGuid& Id);
	FDreamFileWatch& GetFileWatch() const;

	// ---- Recent Files (BUF-03 M1) ----

	/** Most recent first; normalized paths, kept per project (UDreamPerProjectUserSettings::RecentFiles). */
	TArray<FString> GetRecentFiles() const;
	void AddRecentFile(const FString& Path);
	void RemoveRecentFile(const FString& Path);
	void ClearRecentFiles();

	DECLARE_EVENT_OneParam(FDreamBufferManager, FOnBufferEvent, const TSharedRef<FDreamBuffer>&);
	FOnBufferEvent& OnBufferOpened() { return OpenedEvent; }
	FOnBufferEvent& OnBufferClosed() { return ClosedEvent; }
	FOnBufferEvent& OnBufferSaved() { return SavedEvent; }
	/** A clean buffer was reloaded from disk (one undo group), or a dirty one by Reload (§10.15). */
	FOnBufferEvent& OnBufferReloaded() { return ReloadedEvent; }
	/** The disk side of a buffer changed: a pending disk change came or went, the file was deleted or came back, a save
	 *  failed or its failure was cleared (the tab rebuilds its file bar). The lock badge follows OnStateChanged. */
	FOnBufferEvent& OnFileStateChanged() { return FileStateChangedEvent; }

	// ---- Test hooks (Dream.Buffers.*) ----

	/** Reloads applied so far (clean reloads and Reload), and external changes seen on dirty buffers. */
	int32 GetReloadCountForTest() const;
	int32 GetDiskChangeCountForTest() const;

private:
	void HandleFileChecks(TArray<FDreamFileCheckResult>&& Results);
	/** Applies a new disk version to Buffer as one undo group (§10.15 Clean buffer); the old text goes to local history. */
	bool ApplyDiskVersion(const TSharedRef<FDreamBuffer>& Buffer, const FString& Text, const dream::codec::FileFormat& Format,
		const FDreamBaseVersion& NewBase, bool bDecodedReadOnly, const FText& DecodedReadOnlyReason);
	/** The file watch follows the open file buffers (open, close, Save As, kind changes). */
	void SyncWatch();
	// (scratch) The auto-save: every open buffer's OnChanged is followed (only a scratch file acts on it), one ticker polls.
	void TrackBuffer(const TSharedRef<FDreamBuffer>& Buffer);
	void UntrackBuffer(const FGuid& Id);
	void HandleScratchChanged(const FGuid& Id);
	/** A scratch save landed: the closed buffer's newer text, the save due again, the typing during the save. */
	void FinishScratchSave(const FGuid& Id, bool bOk);
	/** (scratch-p1-M1) Close of a dirty scratch file: saved now, after the in-flight save, or kept in local history. */
	void CloseScratch(const TSharedRef<FDreamBuffer>& Buffer);
	/** The close save (the tracked entry lives until it lands); false when it cannot be written now. */
	bool StartClosedSave(const TSharedRef<FDreamBuffer>& Buffer);
	/** A "closed" local-history entry of a closed scratch file's text that could not be saved. */
	void KeepClosedTextInHistory(const FDreamBuffer& Buffer);
	/** A scratch buffer that is still dirty is due again (Keep mine, a cleared failure): its oldest unsaved edit is now. */
	void MarkScratchPending(const FGuid& Id);
	void ArmScratchTicker();
	bool TickScratch();
	void EnsureScratchBindings();
	void HandleScratchFolderChanged();

	FOnBufferEvent OpenedEvent;
	FOnBufferEvent ClosedEvent;
	FOnBufferEvent SavedEvent;
	FOnBufferEvent ReloadedEvent;
	FOnBufferEvent FileStateChangedEvent;

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
