// SPDX-License-Identifier: Apache-2.0
// A frame-polled step sequence for the tier-4 specs (TDD §14.2 synchronisation rule: specs wait on run-state probes and
// store contents, never on fixed sleeps). A LatentIt builds an FDreamSteps list and hands it its FDoneDelegate; the steps
// are polled once per frame from the core ticker until each reports Next, and Done fires after the last one.
// Game thread. The core ticker does not tick while a run executes (LRS-01), so a step never observes a run mid-flight:
// it sees the frame before Submit's OnEndFrame start and the frames after the finish.
#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Templates/Function.h"

namespace DreamTest
{
	enum class EStep : uint8
	{
		Wait,       // poll again next frame
		Next,       // this step is complete
		Fail,       // abort the remaining steps (the step adds its own error); Finally steps still run
	};

	/** Latent-It timeout of every Dream spec; FDreamSteps stops before it so a late step never reports into the next spec. */
	inline FTimespan LatentTimeout() { return FTimespan::FromSeconds(180.0); }
	inline constexpr double StepsDeadlineSeconds = 160.0;

	class FDreamSteps
	{
	public:
		explicit FDreamSteps(FAutomationTestBase& InTest);

		/** Polled once per frame until it returns Next or Fail; Timeout fails the step. */
		FDreamSteps& Poll(const FString& Name, TFunction<EStep()> Fn, double TimeoutSeconds = 10.0);
		/** Runs once; false fails the sequence. */
		FDreamSteps& Do(const FString& Name, TFunction<bool()> Fn);
		/** Runs once, also after a failure or the deadline: cleanup (restore settings, close test buffers, remove hooks). */
		FDreamSteps& Finally(const FString& Name, TFunction<void()> Fn);
		/** Lets Slate tick, lay out and paint before the next step. */
		FDreamSteps& WaitFrames(int32 Frames);
		/** A longer deadline than StepsDeadlineSeconds for a long case (the G2 cycles); its LatentIt timeout must exceed it. */
		FDreamSteps& SetDeadline(double Seconds) { DeadlineSeconds = Seconds; return *this; }

		/** Starts polling from the core ticker (the first poll happens now); Done is executed after the last step. */
		void Run(const FDoneDelegate& Done);

		FAutomationTestBase& GetTest() const { return Test; }

	private:
		friend class FDreamStepRunner;

		struct FStep
		{
			FString Name;
			TFunction<EStep()> Fn;
			double TimeoutSeconds = 10.0;
			bool bAlways = false;
		};

		FAutomationTestBase& Test;
		TArray<FStep> Steps;
		double DeadlineSeconds = StepsDeadlineSeconds;
	};
}

class SDreamTab;

namespace DreamTest
{
	// ---- Tier-4 hygiene (architect rulings 01:29 and on reviews tests-M1 / tests-r2-M1, tests B-note) ----
	// Automation Quit writes no CLEAN, so every pass ends as an unclean exit: a tab a spec leaves open, dirty, comes back
	// read-only under "Not restored yet" in every later pass (B16, TDD 10.7.1). Two shared guards keep passes independent,
	// and both touch only the buffers the tests made, never the developer's own tabs on this shared storage root: a buffer
	// the session restore opened (whenever it lands, also inside the first test) is the developer's unless it carries the
	// tests' scratch name (ScratchName, IsTestScratchName). Restore opens are told apart by SDreamTab::IsApplyingRestoreForTest
	// (set for the duration of ApplyRestore: the startup restore, Leave safe mode, persist's ApplyRecoveryForTest).

	/** The ids of the Dream tab's open tabs now (empty when there is no module or tab). Game thread. */
	TSet<FGuid> SnapshotOpenTabs();
	/**
	 * Closes every tab a test opened since Before (BUF_CLOSE: a non-empty scratch stays reopenable, nothing is deleted); a
	 * tab the session restore opened stays. The tests module calls it at the end of every Dream test
	 * (FAutomationTestFramework::OnTestEndEvent, failure paths included) with the snapshot taken at its start; a spec may
	 * call it in AfterEach too. Skipped while a run is active. Game thread.
	 */
	void CloseTabsOpenedSince(const TSet<FGuid>& Before);
	/** "DreamT4-<purpose>-<4 lower-case hex digits>", the display name ScratchName gives every scratch tab a spec opens. */
	bool IsTestScratchName(const FString& DisplayName);

	/**
	 * The pass-start cleanup: once per editor session, at the first Dream.* test start, the pending recoveries of test buffers
	 * are discarded through the tab's own Discard (SDreamTab::DiscardRecovered: each text becomes a "discarded" local-history
	 * entry first, nothing is deleted), and their scratch tabs are closed. A test buffer is one that the ledger
	 * DreamHost/Saved/Automation/dream-tier4-buffers.txt lists (every buffer a Dream test opens after the restore is appended at
	 * the open) or that carries the tests' scratch name. Any other pending recovery, such as one of the developer's own
	 * sessions, is left alone (and removed from the ledger); the processed entries are dropped afterwards. It runs from the
	 * core ticker once the journal has reconciled the restore (or 20 s passed); AddWaitIdle waits for it. A spec that tests
	 * recovery creates and resolves its own state.
	 */
	void StartPassCleanup();
	bool IsPassCleanupDone();
	/** The tests module's hooks: StartPassCleanup at the first Dream test start, the tab snapshot and close around each, and
	 *  at each Dream test start the Dream tab's window activated again (the automation controller's Message Log takes the
	 *  activation after every test; see ActivateDreamWindow in the .cpp), and the user-settings guard: UDreamUserSettings'
	 *  config keys read from the config cache and from EditorSettings.ini on disk at the first Dream test of each
	 *  automation run (also kept in Saved/DreamTests/settings-snapshot.json until the run ends) and compared at each Dream
	 *  test end; a changed value is logged at Error ("user setting <Key> changed by <test>: <old> -> <new>"), put back and
	 *  re-applied. */
	void RegisterHygieneHooks();
	void UnregisterHygieneHooks();

	/** For the running Dream test only, the user-settings guard neither flags nor puts back Key (a UDreamUserSettings
	 *  property name); the value the test leaves becomes the baseline for the rest of the run. For a spec that persists
	 *  a setting on purpose and restores it some other way (Dream.Restart.Layout's steps: run_restart.ps1 backs the key up
	 *  and restores it). Game thread. */
	void AllowSettingChange(FAutomationTestBase& Test, FName Key);

	/** (scratch, PO-05) The tab Id stays open after the running Dream test: the per-test close skips it. For
	 *  Dream.Restart.Layout's steps, whose tabs must still be open when the editor exits so that the next session restores
	 *  them (PER-02); the next step closes them itself. Cleared at each Dream test end. Game thread. */
	void KeepTabOpenAfterTest(const FGuid& Id);

	/** Dream.Hygiene.Ledger's view of the guards above. Game thread. */
	namespace HygieneForTest
	{
		bool IsInLedger(const FGuid& Id);
		/** What the next pass's cleanup would discard among Pending, with the ledger as it is on disk now. */
		TArray<FGuid> SelectAtPassStart(const TArray<FGuid>& Pending);
		/** The pass-start cleanup's own two halves, run now on Tab with the ledger on disk: the discard (returns the ids),
		 *  then, once those are resolved, the close of their scratch tabs and the pruning of the ledger. */
		TArray<FGuid> BeginPassStartDiscard(SDreamTab& Tab);
		int32 FinishPassStartDiscard(SDreamTab& Tab, const TArray<FGuid>& Discarded);
		/** The user-settings guard's end-of-test check, run now with TestName as the test's name: every key whose value
		 *  differs from the pass's snapshot is logged at Error and put back. False when there is no snapshot yet (no Dream
		 *  test has started in this pass). */
		bool CheckUserSettings(const FString& TestName);
		/** Where the guard keeps the run's baseline while the run lasts. */
		FString SettingsSnapshotPath();
		/** Writes a baseline file at Path as a run that did not finish would leave it: the settings as they are now, with Key's
		 *  value replaced by Value (ini text), stamped with EditorSettings.ini's Dream section as it is now (bStampCurrentIni)
		 *  or with a stamp no file has (the ini "written after" it). */
		void PlantLeftoverSnapshotForTest(const FString& Path, FName Key, const FString& Value, bool bStampCurrentIni);
		/** Resolves the baseline file at Path as the first Dream test of a run resolves a leftover one, then deletes it. */
		void ResolveLeftoverSnapshotForTest(const FString& Path);
		/** The stamp of EditorSettings.ini's Dream section kept in the baseline file at Path ("" when unreadable), and the stamp
		 *  of that section as the file is now. */
		FString SnapshotStampForTest(const FString& Path);
		FString DreamSectionStampForTest();
		/** The guard's settled read: true when EditorSettings.ini on disk agrees with the config cache afterwards. */
		bool SettleSettingsFileForTest();
	}
}
