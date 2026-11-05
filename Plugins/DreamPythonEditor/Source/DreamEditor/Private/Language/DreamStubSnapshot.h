// SPDX-License-Identifier: Apache-2.0
// The Stage 1 snapshot of the unreal module (TDD §7.4.2, §7.4.3, RT-13, D5): the cache key, the cache folder
// <Project>/Intermediate/Dream/Stubs/<key>/snapshot, the idle-slice driver of dream_editor._impl.stubs and the file writes.
// - Key (§7.4.3): the first 16 hex digits of SHA-256 over the engine version and changelist, the Python version, the sorted
//   enabled plugins as name@VersionName, the project module stamps (size and mtime of each project module's DLL), the
//   snapshot format version and the stubgen version. Stamps, the status scan and retention (the 2 newest keys) run on a
//   worker task; the game thread does no snapshot I/O.
// - Slices: an FDreamDeferral idle ticker (bNeedsPython, DeferWhileExecuting) calls stubs.step(4 ms) at most once per frame,
//   never while a run is active or a pump is on the stack (the gate), a modal window is open, or within 300 ms of a Dream
//   keystroke (proposed). Output goes to one UE::Tasks pipe: lines append to modules/<m>.jsonl.tmp, a finished module is
//   renamed into place and recorded in progress.json, and the end writes manifest.json, then .complete (§7.4.2 order).
// - An interrupted walk leaves no .complete; the next start passes progress.json's finished modules to stubs.start, which
//   skips them (T-STUB-025). Python shutdown (§3.8 step 3) stops the walk.
// Game thread, except the worker tasks. Owned by FDreamLanguageServices. Body: DreamStubSnapshot.cpp (lang).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

#include <atomic>

class FDreamPython;
namespace UE::Tasks { class FPipe; }

class DREAMEDITOR_API FDreamStubSnapshot
{
public:
	static constexpr int64 SliceBudgetNs = 4'000'000;          // §7.4.2, D5
	static constexpr double KeystrokeQuietSeconds = 0.3;        // §7.4.2 (proposed)
	static constexpr int32 KeysKept = 2;                        // §7.4.3 retention (proposed)
	/** §7.4.3 key inputs that are ours: the snapshot format (stubs.py FORMAT / SCHEMA) and the stubgen version (none in M1). */
	static constexpr const TCHAR* FormatVersion = TEXT("dream.stubsnap/1");
	static constexpr const TCHAR* StubgenVersion = TEXT("none");
	/** (lang choice) The walker and parser revision, part of the snapshot format for the key: a fix that changes what a
	 *  walk records (2: the two-space editor-property docs of PyGenUtil) makes every older snapshot stale. */
	static constexpr const TCHAR* WalkerVersion = TEXT("2");

	FDreamStubSnapshot();
	~FDreamStubSnapshot();

	/** Binds FDreamPython::OnShutdown (the walk stops with Python). */
	void Initialize(FDreamPython& InPython);
	/**
	 * Deferred start (and every later boot): the first call computes the key on a worker; then, unless the snapshot is
	 * complete, the walk starts or resumes in idle slices. Idempotent while a walk or the key task is running.
	 */
	void Start();
	/** Module shutdown or OnPreExit: the ticker stops; queued writes finish (bounded wait). */
	void Shutdown();
	/** A Dream keystroke (a buffer edit): no slice within KeystrokeQuietSeconds of it. */
	void NoteKeystroke();

	bool IsKeyReady() const { return bKeyReady; }
	/** The real snapshot's key, also while a tier-4 test walk runs (a dream-check spawned then gets the real --key). */
	const FString& GetKey() const { return bTestWalk ? SavedForTest.Key : Key; }
	/** <Project>/Intermediate/Dream/Stubs/<key>/snapshot (absolute); empty before the key is known. The real snapshot's folder,
	 *  also while a test walk runs (review r2: StartServer passes it as --data). */
	const FString& GetSnapshotDir() const { return bTestWalk ? SavedForTest.SnapshotDir : SnapshotDir; }
	bool IsComplete() const { return bComplete; }
	bool IsWalking() const { return bWalking; }
	/** The last walk error (a helper exception or a failed write); empty when none. */
	const FString& GetLastError() const { return LastError; }
	int32 GetSliceCount() const { return SliceCount; }
	double GetMaxSliceSeconds() const { return MaxSliceSeconds; }
	/** Slices that ran while a run was active, a pump was on the stack, a modal was open or a keystroke was recent: counted
	 *  right before each stubs.step from the same conditions the gates read (T-STUB-028: must stay 0). */
	int32 GetGateViolationCount() const { return GateViolations; }
	/** FPlatformTime::Seconds() of the last slices (at most 4096, oldest first; tier 4 checks none fell inside a run). */
	const TArray<double>& GetSliceTimesForTest() const { return SliceTimes; }
	/**
	 * Tier 4 (T-STUB-028): a fresh walk into Dir under a test key, so slices run even when this session's snapshot is
	 * complete. Its completion is not announced (dream-check keeps the real snapshot). False when the key is not known yet
	 * or a real walk is running. EndTestWalkForTest stops it and restores the real snapshot's state.
	 */
	bool BeginTestWalkForTest(const FString& Dir);
	void EndTestWalkForTest();
	/** Tier 4 only: a larger slice budget lets a spec finish the walk in fewer frames (tier 2 T-STUB-002 measures the 4 ms
	 *  budget); 0 restores SliceBudgetNs. */
	void SetSliceBudgetForTest(int64 BudgetNs) { SliceBudget = BudgetNs > 0 ? BudgetNs : SliceBudgetNs; }

	/** <Project>/Intermediate/Dream/Stubs (absolute). */
	static FString GetStubsRoot();

	/** The key is known (dream-check needs it for --data and --key). */
	DECLARE_EVENT(FDreamStubSnapshot, FOnKeyReady);
	FOnKeyReady& OnKeyReady() { return KeyReadyEvent; }
	/** manifest.json and .complete are on disk: (snapshot folder, key). */
	DECLARE_EVENT_TwoParams(FDreamStubSnapshot, FOnComplete, const FString& /*SnapshotDir*/, const FString& /*Key*/);
	FOnComplete& OnComplete() { return CompleteEvent; }

private:
	struct FKeyResult;
	struct FWriter;

	bool Tick(float DeltaTime);
	void AdoptKey();
	void StartWalk();
	void StopWalk(const FString& Why);
	void FlushLines(bool bForce);
	void HandleOutput(struct FDreamStubOutput& Output);
	void EnsureTicker();
	bool IsGated(double Now) const;

	FDreamPython* Python = nullptr;
	FDelegateHandle PythonShutdownHandle;
	FTSTicker::FDelegateHandle TickHandle;

	TSharedPtr<FKeyResult> KeyTask;         // shared with the worker
	bool bKeyRequested = false;
	bool bKeyReady = false;
	FString Key;
	FString SnapshotDir;
	FString EngineJson;
	FString DoneJson;
	bool bComplete = false;

	bool bWalking = false;                  // stubs.start called for this Python generation; outputs pending
	bool bWantWalk = false;
	bool bAwaitingWrites = false;           // kind 2 seen; waiting for the pipe to write manifest and .complete
	double LastKeystrokeSeconds = -1.0;
	FString LastError;
	int32 SliceCount = 0;
	double MaxSliceSeconds = 0.0;
	int32 GateViolations = 0;
	TArray<double> SliceTimes;
	bool bTestWalk = false;
	struct FSaved { FString Key; FString SnapshotDir; bool bComplete = false; TSharedPtr<FWriter> Writer; } SavedForTest;
	int64 SliceBudget = SliceBudgetNs;

	FString LinesModule;                    // the module whose lines are buffered
	TArray<uint8> Lines;
	TUniquePtr<UE::Tasks::FPipe> Pipe;
	TSharedPtr<FWriter> Writer;             // shared with the pipe's tasks

	FOnKeyReady KeyReadyEvent;
	FOnComplete CompleteEvent;
};
