// SPDX-License-Identifier: Apache-2.0
// DreamEditorTests module entry (repository only; never shipped, TDD §2.2). Tier-4 specs live in Specs/ (one spec per
// file, names under "Dream."), their shared helpers in Support/ (FDreamSteps, run submission and output collection, the
// log capture), and the §14.2 editor fixtures in Fixtures/ and Python/: DreamTestLatch and the _dream_test module (wait,
// held, release, marks, run-state probes; Python/DreamTestPython.h), the input-bot FRunnable with SendInput
// (Fixtures/DreamInputBot.h), the probes the game thread publishes for it (Fixtures/DreamTestProbes.h), TickCoreTicker,
// SleepNative, FillPendingCallQueue, PostSupervisorCall and QueueOsKeys (in _dream_test), CallPythonOverride
// (UDreamTestOverrideFixture) and the getter-counting fixture UObject (UDreamTestGetterFixture).
// DreamHost/Scripts/run_tier4.bat runs the specs.
// Deferred (M3): FDreamPausedWindowTestDriver (it drives the Paused window, which lands with the debugger).
#include "Modules/ModuleManager.h"

#include "Fixtures/DreamInputBot.h"
#include "Fixtures/DreamTestProbes.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/CoreDelegates.h"
#include "Python/DreamTestPython.h"
#include "Support/DreamTestSteps.h"

class FDreamEditorTestsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		DreamTestPython::Register();
		// Tier-4 hygiene: discard earlier passes' pending recoveries at pass start, close each test's tabs at its end.
		DreamTest::RegisterHygieneHooks();
		// Slate exists by PostEngineInit in an editor with an RHI; headless sessions have no Slate tick to publish from.
		if (FSlateApplication::IsInitialized())
		{
			DreamTest::FDreamTestProbes::Start();
		}
		else
		{
			PostInitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([]()
			{
				if (FSlateApplication::IsInitialized())
				{
					DreamTest::FDreamTestProbes::Start();
				}
			});
		}
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(PostInitHandle);
		DreamTest::FDreamInputBot::Shutdown();
		DreamTest::FDreamTestProbes::Stop();
		DreamTestPython::Unregister();
		DreamTest::UnregisterHygieneHooks();
	}

private:
	FDelegateHandle PostInitHandle;
};

IMPLEMENT_MODULE(FDreamEditorTestsModule, DreamEditorTests)
