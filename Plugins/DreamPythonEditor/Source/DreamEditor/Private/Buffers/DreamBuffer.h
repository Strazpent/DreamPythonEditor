// SPDX-License-Identifier: Apache-2.0
// One editable buffer: identity, file metadata, the dream::TextBuffer model and its dream::PyLexer (TDD §4.1, §4.13,
// §10.2, BUF-01, BUF-02, BUF-06). Game thread. Body: DreamBuffer.cpp (shell slice).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Run/DreamRunTypes.h"
#include "dream/PyLexer.h"
#include "dream/TextBuffer.h"
#include "dream/TextCodec.h"

enum class EDreamBufferKind : uint8
{
	Scratch,            // Untitled-N, journaled from its first character (BUF-01)
	File,
	Recovered,          // "name (recovered)" from the §10.7.2 conflict rule
	ReplInput,          // the REPL prompt text, journaled as the special buffer "repl-input" (§6.5)
};

/** A file's size, mtime and SHA-256 when loaded or last saved (§10.14 FDreamBaseVersion; journal "base"). */
struct FDreamBaseVersion
{
	int64 Size = 0;
	FDateTime MtimeUtc;
	FString Sha256Hex;
};

/** Per-tab state kept with the session (§10.3 FDreamTabState fields that slice A uses). */
struct FDreamBufferViewState
{
	int32 CaretLine = 0;
	int32 CaretColumn = 0;
	int32 FirstVisibleLine = 0;
	bool bOneUndoStepPerRun = true;                     // U3 per-tab toggle
	/** (run group, B40) The user set the toggle on this tab; until then a run follows the per-route setting it was seeded
	 *  from (FDreamRunService::ResolveTabTransaction, A56, P2-run-02). */
	bool bOneUndoStepPerRunSet = false;
	EDreamNamespaceMode Namespace = EDreamNamespaceMode::Session;   // the split Run button (U6)
	int32 ZoomPercent = 100;                            // EDT-10: per tab, 50..300 in 10% steps
};

class DREAMEDITOR_API FDreamBuffer : public TSharedFromThis<FDreamBuffer>
{
public:
	FDreamBuffer(const FGuid& InId, EDreamBufferKind InKind, const FString& InDisplayName);

	const FGuid& GetId() const { return Id; }
	/** 32 lower-case hex digits: the journal uuid, the tab ID of the public API (§12.2). */
	FString GetIdHex() const;
	EDreamBufferKind GetKind() const { return Kind; }
	const FString& GetDisplayName() const { return DisplayName; }
	/** Real path for File buffers (normalized absolute, forward slashes); empty otherwise. */
	const FString& GetPath() const { return Path; }
	bool HasPath() const { return !Path.IsEmpty(); }
	/** (scratch, PO-05) A File buffer whose path lies in the scratch folder (DreamScratch::IsScratchPath): an unnamed tab's
	 *  scratch_N.py, a sample, any file opened from there. It saves itself as you type (FDreamBufferManager's auto-save), never
	 *  asks on close, and its tab keeps the file name. Follows the path (Save As out of the folder clears it) and the folder
	 *  setting (RefreshScratchFlag). Tracebacks and compile filenames use the real path, as for any file. */
	bool IsScratchFile() const { return bScratchFile; }
	/** Recomputes IsScratchFile from the path and the folder now; true when it changed. */
	bool RefreshScratchFlag();

	// ---- Model ----

	dream::TextBuffer& GetModel() { return Model; }
	const dream::TextBuffer& GetModel() const { return Model; }
	dream::PyLexer& GetLexer() { return Lexer; }
	dream::TextSnapshot Snapshot() const { return Model.Snapshot(); }

	/** Applies an edit to the model and the lexer and broadcasts OnChanged (the code view calls it from OnTextChanged). */
	dream::ChangeSet ApplyEdit(const dream::EditBatch& Batch);
	/** Load and reload: replaces the whole text (origin Load or Reload). */
	void ResetText(const FString& Text, dream::EditOrigin Origin);
	FString GetTextAsString() const;

	// ---- File state ----

	const dream::codec::FileFormat& GetFormat() const { return Format; }
	void SetFormat(const dream::codec::FileFormat& InFormat) { Format = InFormat; }
	const TOptional<FDreamBaseVersion>& GetBase() const { return Base; }

	/** Dirty: the text differs from the last load or save (BUF-02 dirty marker), or the file was deleted on disk (§10.15: the
	 *  buffer "becomes dirty and stays journaled"). (slice A choice) Tracked by the model version of the last save, with a
	 *  text compare when the version moved but the length matches (undo back to the saved text). */
	bool IsDirty() const;
	/** Only the text part of IsDirty: the text differs from the last load or save (§10.15 decides clean reloads on it). */
	bool IsTextDirty() const { return bDirty; }
	/** After a successful save or load: base, path, display name, clean. */
	void MarkSaved(const FString& NewPath, const FDreamBaseVersion& NewBase);
	/** A save finished: the written snapshot is the clean text (the user may have typed since the save started, §10.14). */
	void MarkSaved(const FString& NewPath, const FDreamBaseVersion& NewBase, const dream::TextSnapshot& WrittenText);
	/** The current text is the clean text (load, reload, a new scratch tab). */
	void MarkClean();
	/** The clean text is CleanText, which may differ from the current text (restore of a dirty buffer, §10.3). */
	void SetCleanText(const FString& CleanText);
	/** Restore: path and base of a file buffer without changing its clean state (§10.3, §10.7). */
	void SetFileIdentity(const FString& NewPath, const TOptional<FDreamBaseVersion>& NewBase);
	/** Turns a recovered "(recovered)" tab into a plain buffer of the given kind (§10.7.2 Restore). */
	void SetKind(EDreamBufferKind NewKind) { Kind = NewKind; RefreshScratchFlag(); }

	bool IsReadOnly() const { return bReadOnly; }
	const FText& GetReadOnlyReason() const { return ReadOnlyReason; }
	void SetReadOnly(bool bInReadOnly, const FText& Reason);

	/** (buffers, B80/B81) The file has the read-only attribute: the tab shows the lock badge (BUF-02, §10.15 "only the
	 *  lock badge changes") and a save offers Make writable / Save As (BUF-05, §11.6.5). The text stays editable. */
	bool IsReadOnlyOnDisk() const { return bReadOnlyOnDisk; }
	void SetReadOnlyOnDisk(bool bInReadOnlyOnDisk);
	/** (buffers, B80) The file was deleted on disk (§10.15): the buffer stays open, dirty and journaled until Save recreates
	 *  the file, the file comes back with the base hash, or the tab closes. */
	bool IsDeletedOnDisk() const { return bDeletedOnDisk; }
	void SetDeletedOnDisk(bool bInDeleted);

	/** "Not restored yet" (§10.7.1): the buffer shows disk/clean text, is read-only, and holds the recovered text. */
	bool HasPendingRecovery() const { return PendingRecoveryText.IsSet(); }
	const TOptional<FString>& GetPendingRecoveryText() const { return PendingRecoveryText; }
	void SetPendingRecovery(TOptional<FString> RecoveredText);

	FDreamBufferViewState& GetViewState() { return ViewState; }
	const FDreamBufferViewState& GetViewState() const { return ViewState; }

	// ---- (buffers) Compaction of the piece tree (TDD §4.1.3) ----

	/** Quiet time after the last model change before a compaction is considered (§4.1.3 "after 2 s idle"). */
	static constexpr double CompactionIdleSeconds = 2.0;
	/** Compactions installed since the buffer was created (Dream.Buffers.Compaction). */
	int32 GetCompactionsForTest() const { return Compactions; }

	void SetDisplayName(const FString& Name) { DisplayName = Name; }

	/** After every model change, game thread. The journal and the code view's highlighter listen. */
	DECLARE_EVENT_OneParam(FDreamBuffer, FOnChanged, const dream::ChangeSet&);
	FOnChanged& OnChanged() { return ChangedEvent; }
	DECLARE_EVENT(FDreamBuffer, FOnStateChanged);
	/** Dirty, read-only (buffer or disk), deleted on disk, path, name or pending-recovery change (tab label and tooltip). */
	FOnStateChanged& OnStateChanged() { return StateChangedEvent; }

private:
	/** Recomputes the cached dirty flag and broadcasts OnStateChanged when it flipped. */
	void UpdateDirty();
	/**
	 * After a model change: arms the compaction ticker (FDreamDeferral, deferred while a run is active) unless it is armed.
	 * The ticker waits CompactionIdleSeconds after the last change; then, when NeedsCompaction(), BuildCompacted runs on a
	 * worker over a snapshot and InstallCompacted applies it on the game thread (refused when the buffer changed meanwhile:
	 * the next quiet period tries again). Text, version, undo and snapshots held elsewhere stay as they are.
	 */
	void ScheduleCompaction();
	bool TickCompaction();
	struct FCompactJob;

	FGuid Id;
	EDreamBufferKind Kind;
	FString DisplayName;
	FString Path;
	bool bScratchFile = false;          // (scratch) see IsScratchFile
	dream::TextBuffer Model;
	dream::PyLexer Lexer;
	dream::codec::FileFormat Format;
	TOptional<FDreamBaseVersion> Base;
	uint64 SavedVersion = 0;
	int64 SavedLength = 0;
	dream::TextSnapshot SavedSnapshot;
	bool bDirty = false;
	bool bReadOnly = false;
	FText ReadOnlyReason;
	bool bReadOnlyOnDisk = false;
	bool bDeletedOnDisk = false;
	bool bReadOnlyBeforeRecovery = false;
	FText ReadOnlyReasonBeforeRecovery;
	TOptional<FString> PendingRecoveryText;
	FDreamBufferViewState ViewState;
	FOnChanged ChangedEvent;
	FOnStateChanged StateChangedEvent;
	double LastChangeSeconds = 0.0;
	FTSTicker::FDelegateHandle CompactTicker;
	TSharedPtr<FCompactJob, ESPMode::ThreadSafe> CompactJob;
	int32 Compactions = 0;
};
