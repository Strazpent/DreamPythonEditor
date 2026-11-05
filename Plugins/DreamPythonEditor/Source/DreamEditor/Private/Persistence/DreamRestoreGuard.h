// SPDX-License-Identifier: Apache-2.0
// The restore guard and the safe-mode state (TDD §10.8, D17, PER-06): <root>/state/restore-guard.json {"schema", "pending"}.
//  1. At the tab spawn it is read; pending >= 2 enters safe mode.
//  2. Otherwise pending += 1, written and flushed before any restore work (a small JSON file: .tmp, flush, ReplaceFileW
//     with .bak, §10.2.3; readers try the file, then .bak, then defaults; unknown keys are kept; a newer schema is never
//     rewritten).
//  3. pending = 0 ten seconds after the deferred dream_editor import completes with every tab restored, and at clean
//     shutdown. (slice B choice) When Python never imports in this session (disabled, unavailable, a failed boot), the ten
//     seconds count from the restore.
// Safe mode is also entered by -DreamSafeMode and by Shift held while the tab opens; Leave safe mode sets pending = 0.
// (slice B choice) An editor started to run automation (-ExecCmds="Automation ...", which ends with a forced Quit that
// skips every clean-shutdown path) keeps counting but never enters safe mode by the guard: a guard at 2 there is reset to 1
// with a log line, so repeated tier-4 passes cannot push a profile into safe mode.
// Game thread. The module owns one for its storage root; tests make their own on a private root. Body: DreamRestoreGuard.cpp.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

enum class EDreamSafeModeReason : uint8
{
	None,
	Switch,             // -DreamSafeMode
	Shift,              // Shift held while the tab opened
	Guard,              // two crashes during restore (pending >= 2)
};

class DREAMEDITOR_API FDreamRestoreGuard
{
public:
	static constexpr int32 SafeModeThreshold = 2;
	static constexpr double ClearDelaySeconds = 10.0;
	static constexpr int32 Schema = 1;

	/** IsImportSettled: true once the deferred dream_editor import has completed, or will never run this session. */
	explicit FDreamRestoreGuard(TFunction<bool()> InIsImportSettled);
	~FDreamRestoreGuard();

	/** <StateDir>/restore-guard.json. */
	static FString GuardPath(const FString& StateDir);
	/** Steps 1-2 at the spawn: true when the guard says safe mode (pending >= 2); otherwise pending += 1 is on disk. */
	bool BeginRestore(const FString& StateDir, bool bAutomationSession);
	/** Every tab is restored (ApplyRestore done): the step-3 timer starts once the import has settled too. */
	void OnRestoreApplied();
	/** pending = 0 now (Leave safe mode, clean shutdown); a no-op when BeginRestore never ran. */
	void Clear();
	/** The value on disk as this guard last read or wrote it; -1 before BeginRestore. */
	int32 GetPending() const { return Pending; }

	void EnterSafeMode(EDreamSafeModeReason Reason);
	/** Leave safe mode (§10.8): pending = 0, and listeners start the deferred services. */
	void LeaveSafeMode();
	bool IsSafeMode() const { return Reason != EDreamSafeModeReason::None; }
	EDreamSafeModeReason GetSafeModeReason() const { return Reason; }
	/** Entered or left; the language services and the deferred start listen (§10.8 "Skips"). */
	FSimpleMulticastDelegate& OnSafeModeChanged() { return SafeModeChanged; }

	/** True for an editor started with -ExecCmds="Automation ..." (tier 4) or running automation now. */
	static bool IsAutomationSession();

	/** The pending count in a guard file: from the file, else its .bak, else 0; false when neither parses. */
	static bool ReadPending(const FString& Path, int32& OutPending, bool& bOutNewerSchema);
	/** Writes pending (keeping unknown keys) through .tmp, flush and ReplaceFileW with .bak. */
	static bool WritePending(const FString& Path, int32 InPending);

	/** Test hook: the step-3 clear happens after this many seconds instead of ClearDelaySeconds. */
	void SetClearDelayForTest(double Seconds) { ClearDelay = Seconds; }
	bool IsClearArmedForTest() const { return bRestoreApplied && !bCleared; }

private:
	bool Tick();

	TFunction<bool()> IsImportSettled;
	FString Path;
	int32 Pending = -1;
	bool bReadOnly = false;            // a newer schema: used, never rewritten (§10.6)
	bool bRestoreApplied = false;
	bool bCleared = false;
	double SettledAt = -1.0;
	double ClearDelay = ClearDelaySeconds;
	EDreamSafeModeReason Reason = EDreamSafeModeReason::None;
	FSimpleMulticastDelegate SafeModeChanged;
	FTSTicker::FDelegateHandle Ticker;
};
