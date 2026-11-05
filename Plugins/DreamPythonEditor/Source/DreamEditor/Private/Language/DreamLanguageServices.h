// SPDX-License-Identifier: Apache-2.0
// FDreamLanguageServices (TDD §7.3, §7.8; D4, RT-01, RT-17, INT-07, INT-11): starts the language servers at the deferred
// start after a good boot (M1: dream-check only), keeps every open buffer in sync with them, supervises them with backoff
// (1, 2, 4 s up to 60 s; Paused after 5 crashes in 3 min), applies their results only in Tick while no run is on the stack
// (RT-01), owns the Problems model (dream-check, Parse and runtime items), the F8 / Shift+F8 walk and the Ctrl+. quick fixes
// (one undo step per buffer through FDreamUndoHistory's grouped edit, EDT-08), and drives the Stage 1 snapshot (§7.4.2,
// FDreamStubSnapshot), whose completion it announces to dream-check with dream/snapshotChanged.
// Never started in a headless editor (D16) or in safe mode (D17, §10.8); leaving safe mode starts it.
// Game thread only. Body: DreamLanguageServices.cpp (lang).
// (lang choice) M1 has one server, so health is one value; the per-role health of WS-05 and the status-bar item are M2.
// Deferred (M2): the type server and ruff (§7.3.2), hover and signature help (§7.7), formatting, the orphan sweep of
// lsp-children.json (§7.1.3: on Win64 the kill-on-close job already ends every child with the editor), the policy keys of
// §7.3.4 (M4) and the REPL prompt document __dream_repl__.py (its diagnostics are never shown, §7.2.3).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Tasks/Task.h"
#include "Language/DreamProblemsModel.h"
#include "dream/LspClient.h"

class FLspTransport;
class FDreamBuffer;
class FDreamBufferManager;
class FDreamJournal;
class FDreamPython;
class FDreamRestoreGuard;
class FDreamRunService;
class FDreamRuntimeProblems;
class FDreamStubSnapshot;
class SDreamTab;
class SWidget;
struct FLspEvent;
struct FDreamRunRecord;

/** §7.3.5 health (INT-11). */
enum class EDreamLanguageHealth : uint8
{
	Deferred,           // not started yet (RT-17)
	Disabled,           // headless (D16) or safe mode (D17)
	Starting,
	Ready,
	Restarting,         // in backoff or respawning after a crash
	Paused,             // 5 crashes within 3 minutes: [Restart] [View log]
	Stopped,            // editor exit
};

/** The answer to a code-action request: the server's actions for the range (empty on an error or timeout). */
using FDreamOnCodeActions = TFunction<void(TArray<dream::LspCodeAction>&& Actions)>;

class DREAMEDITOR_API FDreamLanguageServices
{
public:
	static constexpr const TCHAR* DreamCheckId = TEXT("dream-check");
	/** §7.3.6 drain budget per frame. */
	static constexpr double DrainBudgetSeconds = 0.001;

	FDreamLanguageServices();
	~FDreamLanguageServices();

	/** StartupModule: bindings only (no thread, process or Python, B1). */
	void Initialize(FDreamRunService& InRuns, FDreamPython& InPython, FDreamBufferManager& InBuffers, FDreamRestoreGuard& InGuard,
		FDreamRuntimeProblems& InRuntime, FDreamJournal& InJournal, bool bInHeadless);
	/**
	 * §3.4, §7.1.5: called after every successful boot (FDreamPython::Boot). The first call starts the services: the snapshot
	 * key is computed on a worker, then dream-check spawns from the next Tick (thread creation only on the game thread) and
	 * the Stage 1 walk resumes. Later calls (a Python restart) only resume the walk. No-op when headless or in safe mode.
	 */
	void OnDeferredStart();
	/** §5.5.2 run protections (FDreamRunProtections Install / Remove): no results apply and no snapshot slice runs. */
	void SetRunActive(bool bActive);
	/** OnPreExit and ShutdownModule: every server gets shutdown, then exit, then is killed (<= 1 s in total, §7.1.5). */
	void Shutdown();

	FDreamProblemsModel& GetProblems() { return Problems; }
	const FDreamProblemsModel& GetProblems() const { return Problems; }
	FDreamStubSnapshot& GetSnapshot() { return *Snapshot; }

	EDreamLanguageHealth GetHealth() const { return Health; }
	/** "Lint: ready", "Lint: restarting (2)", "Lint: paused after 5 crashes", ... (INT-11). */
	FText GetHealthText() const;
	int32 GetRestartCount() const { return RestartCount; }
	uint32 GetServerPid() const;
	/** The running server's log (TEL-01): <root>/Logs/LSP/dream-check.log, or <sec>/Logs/LSP/... in a secondary instance;
	 *  empty before the start. */
	const FString& GetLogPath() const { return LogPath; }
	/**
	 * TEL-01, §10.5: a server's log for the journal's role: <root>/Logs/LSP/<ServerId>.log for the primary, <sec>/Logs/LSP/
	 * <ServerId>.log for a secondary (<root>/journal/i-<pid>-<start>); empty when the journal is not open. Each spawn uses it.
	 */
	static FString MakeLogPath(const FDreamJournal& InJournal, const FString& ServerId);
	/** The user's Restart from Paused (or a manual restart): the backoff resets and the server spawns now. */
	void RestartServers();

	/**
	 * textDocument/codeAction for Range (0-based lines, UTF-16 columns of the buffer's current text) at the server. False
	 * when no server can answer now (OnDone is then never called); otherwise OnDone runs later in Tick, on the game thread.
	 */
	bool RequestCodeActions(const FGuid& BufferId, const dream::LspRange& Range, FDreamOnCodeActions OnDone);
	/** Applies one code action as one undo step of its buffer (§7.8.3, EDT-08); false with a reason when it is stale. */
	bool ApplyCodeAction(const dream::LspCodeAction& Action, FText& OutError);
	/** The actions as a menu under Anchor at ScreenPosition (Ctrl+. and the Problems view's context menu). */
	void ShowCodeActionMenu(const TSharedRef<SWidget>& Anchor, const FVector2D& ScreenPosition, TArray<dream::LspCodeAction>&& Actions);

	/** F8 / Shift+F8 (U13, §7.8.2): the next or previous problem after the caret of the tab's active buffer. */
	void WalkProblems(SDreamTab& Tab, int32 Direction);
	/**
	 * (lang) PO-06 F4, F2 / Shift+F2 (PyCharm's Next / Previous Highlighted Error): the next or previous problem of the active
	 * buffer after the caret, by position, wrapping inside the buffer; infos and hints are skipped. The inspection widget's click too.
	 */
	void WalkProblemsInBuffer(SDreamTab& Tab, int32 Direction);
	/** Ctrl+. (U22): the quick fixes on the caret line of the active buffer, as a menu at the caret. */
	void ShowQuickFixes(SDreamTab& Tab);
	/** Moves the caret to a problem (opening its tab or file). False when it cannot be shown. */
	bool OpenProblem(SDreamTab& Tab, const FDreamProblem& Problem);

	/** True while the buffer is a document of the running server (tests). */
	bool IsDocumentOpen(const FGuid& BufferId) const;
	/** (lang) Tests: keeps a buffer off dream-check (closed at the server, its dream-check items and verdict dropped) until
	 *  cleared, so a spec proves the in-process syntax check alone (review lang-q1-M2). */
	void SetServerExcludedForTest(const FGuid& BufferId, bool bExcluded);
	/** Kills the running server process as a crash would (tests: supervision and stale items). */
	void KillServerForTest();

	/** FDreamDeferral body (DeferWhileExecuting): drains server events and applies results; first statement is the RT-01 check. */
	bool Tick(float DeltaTime);

	/** (lang) PO-06 F4: the in-process syntax check runs this long after the last edit (INT-07's debounce, as dream-check). */
	static constexpr double SyntaxCheckDelaySeconds = 0.3;
	/** (lang) True while Buffer is tracked by the in-process syntax check (tests). */
	bool IsSyntaxCheckTracked(const FGuid& BufferId) const { return SyntaxChecks.Contains(BufferId); }
	/** (lang) Starts every pending syntax check, waits for the workers and applies them (tests; game thread, outside runs). */
	void RunSyntaxChecksForTest() { TickSyntaxChecks(0.f, /*bAll*/ true); }

private:
	struct FDoc
	{
		dream::LspDocId Id = 0;
		TWeakPtr<FDreamBuffer> Buffer;
		std::string Uri;
		std::string ConfigKey;          // the serialized "dream" section last sent
		FDelegateHandle ChangedHandle;
		FDelegateHandle StateHandle;
	};
	struct FPendingActions
	{
		FDreamOnCodeActions OnDone;
	};

	void Begin();
	void StartServer();
	void StopServers(int64 GraceMs);
	void HandleEvent(FLspEvent& Event, int64 NowMs);
	void HandleCrash(const FString& Why, int64 NowMs);
	void ApplyDiagnostics(const dream::LspEvent& Event);
	void ReapDying(bool bWait);
	FString ResolveLogPath();
	void FailPendingActions();

	bool IsEligible(const FDreamBuffer& Buffer) const;
	void AddDocument(const TSharedRef<FDreamBuffer>& Buffer);
	void RemoveDocument(const FGuid& BufferId);
	void OpenOnServer(const FDoc& Doc, const TSharedRef<FDreamBuffer>& Buffer);
	void HandleBufferChanged(const FGuid& BufferId, const dream::ChangeSet& Changes);
	void HandleBufferStateChanged(const FGuid& BufferId);
	void SyncConfiguration(FDoc& Doc, const FDreamBuffer& Buffer, bool bNotify);
	std::string MakeUri(const FDreamBuffer& Buffer) const;
	dream::json::Value MakeConfiguration(const FDreamBuffer& Buffer) const;

	void HandleRunStateChanged(const FDreamRunRecord& Record);
	void HandleRunFinished(const FDreamRunRecord& Record);
	void HandleSafeModeChanged();
	void HandleSnapshotComplete(const FString& SnapshotDir, const FString& Key);

	static int64 NowMs();

	/** (lang) PO-06 F4: one buffer of the in-process syntax check. */
	struct FSyntaxResult
	{
		bool bChecked = false;              // false: the text was too long, no verdict
		TArray<FDreamProblem> Items;
	};
	struct FSyntaxCheck
	{
		TWeakPtr<FDreamBuffer> Buffer;
		FDelegateHandle ChangedHandle;
		double DueSeconds = 0.0;            // 0: nothing due
		/** Ruling R-q1-5: the check runs on a UE::Tasks worker over an immutable snapshot of TaskVersion; the result
		 *  applies in TickSyntaxChecks only while the buffer is still at that version (an edit has scheduled the next one). */
		UE::Tasks::TTask<FSyntaxResult> Task;
		uint64 TaskVersion = 0;
		bool bInFlight = false;
	};
	void TrackSyntaxCheck(const TSharedRef<FDreamBuffer>& Buffer);
	void UntrackSyntaxCheck(const FGuid& BufferId);
	/** FDreamDeferral body (DeferWhileExecuting, no Python): applies finished checks by version and starts the due ones on a
	 *  worker; bAll (tests) starts every pending check and waits for it. */
	bool TickSyntaxChecks(float DeltaTime, bool bAll = false);

	FDreamRunService* Runs = nullptr;
	FDreamPython* Python = nullptr;
	FDreamBufferManager* Buffers = nullptr;
	FDreamRestoreGuard* Guard = nullptr;
	FDreamJournal* Journal = nullptr;
	bool bHeadless = false;

	FDreamProblemsModel Problems;
	TUniquePtr<FDreamStubSnapshot> Snapshot;

	EDreamLanguageHealth Health = EDreamLanguageHealth::Deferred;
	bool bDeferredStartSeen = false;        // a boot asked for the start (kept through safe mode)
	bool bStarted = false;                  // Begin ran
	bool bServerWanted = false;             // spawn when the key is known and no backoff is pending
	bool bStopping = false;
	bool bRunActive = false;
	int32 RestartCount = 0;
	int32 LastWalkIndex = INDEX_NONE;     // the Problems item the last F8 / Shift+F8 reached
	int64 NextSpawnMs = 0;
	int64 LastHealthyCheckMs = 0;
	int64 LastConfigCheckMs = 0;
	FString LogPath;

	TUniquePtr<FLspTransport> Transport;
	TArray<TUniquePtr<FLspTransport>> Dying;
	dream::LspBackoff Backoff;

	TMap<FGuid, FDoc> Docs;
	TMap<dream::LspDocId, FGuid> BufferByDoc;
	dream::LspDocId NextDocId = 1;
	uint64 NextTag = 1;
	TMap<uint64, FPendingActions> PendingActions;

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle BufferOpenedHandle;
	FDelegateHandle BufferClosedHandle;
	FDelegateHandle RunStateHandle;
	FDelegateHandle RunFinishedHandle;
	FDelegateHandle SafeModeHandle;
	FDelegateHandle SnapshotHandle;
	TWeakPtr<FDreamBuffer> ReplInput;
	FDelegateHandle ReplInputHandle;

	TMap<FGuid, FSyntaxCheck> SyntaxChecks;             // (lang) PO-06 F4
	TSet<FGuid> ServerExcludedForTest;                  // (lang)
	FTSTicker::FDelegateHandle SyntaxTickHandle;        // (lang)
};
