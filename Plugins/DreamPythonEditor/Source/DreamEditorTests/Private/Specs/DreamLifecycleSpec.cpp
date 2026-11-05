// SPDX-License-Identifier: Apache-2.0
// Dream.Lifecycle: the headless rule of TDD §3.4 / §3.5 in whatever session the suite runs in (T-LIFE-009's editor case).
// Runs in every editor session, so the -nullrhi -unattended pass of run_tier4.bat checks something real while the
// attended specs are filtered out by their NonNullRHI flag.
#include "Support/DreamTestUtil.h"

#include "Features/IModularFeatures.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamConsoleExecutor.h"
#include "Python/DreamPython.h"
#include "Settings/DreamProjectSettings.h"

BEGIN_DEFINE_SPEC(FDreamLifecycleSpec, "Dream.Lifecycle", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamLifecycleSpec)

void FDreamLifecycleSpec::Define()
{
	Describe("Headless rule", [this]()
	{
		It("is Headless exactly when the session is a commandlet, unattended or has no RHI (TDD 3.4)", [this]()
		{
			FDreamEditorModule* Module = DreamTest::GetModule(*this);
			if (!Module)
			{
				return;
			}
			// The session's -unattended flag, not FApp::IsUnattended(): that is also true while any automation test runs
			// (GIsAutomationTesting, App.cpp), so inside this spec it would call every attended session Headless.
			const bool bExpectHeadless = IsRunningCommandlet() || FParse::Param(FCommandLine::Get(), TEXT("unattended")) || !FApp::CanEverRender();
			const EDreamPythonState State = Module->GetPython().GetState();
			const FString StateText = DreamTest::PythonStateName(State);

			if (bExpectHeadless)
			{
				TestTrue(*FString::Printf(TEXT("Python state is Headless (it is %s)"), *StateText), State == EDreamPythonState::Headless);
			}
			else
			{
				TestTrue(*FString::Printf(TEXT("Python state is not Headless in an attended session (it is %s)"), *StateText),
					State != EDreamPythonState::Headless);
			}
		});

		It("registers the tab spawner and the Python (Dream) executor only outside Headless (TDD 3.5 table)", [this]()
		{
			FDreamEditorModule* Module = DreamTest::GetModule(*this);
			if (!Module)
			{
				return;
			}
			const EDreamPythonState State = Module->GetPython().GetState();
			const bool bHeadless = State == EDreamPythonState::Headless;
			const bool bHasSpawner = FGlobalTabmanager::Get()->HasTabSpawner(FName(FDreamEditorModule::TabIdName));
			TestTrue(*FString::Printf(TEXT("DreamEditor.Main spawner registered: %s, expected %s"),
				bHasSpawner ? TEXT("yes") : TEXT("no"), bHeadless ? TEXT("no") : TEXT("yes")), bHasSpawner == !bHeadless);

			const FDreamConsoleExecutor& Executor = Module->GetConsoleExecutor();
			if (bHeadless)
			{
				TestFalse(TEXT("The Python (Dream) executor is not registered while Headless"), Executor.IsRegistered());
				return;
			}
			if (State != EDreamPythonState::Cold && State != EDreamPythonState::Booting && State != EDreamPythonState::Ready)
			{
				AddInfo(FString::Printf(TEXT("Python state %s: executor registration not checked"), DreamTest::PythonStateName(State)));
				return;
			}
			if (!UDreamProjectSettings::Get()->bRegisterConsoleExecutor)
			{
				AddInfo(TEXT("UDreamProjectSettings::bRegisterConsoleExecutor is off: executor registration not checked"));
				return;
			}
			TArray<IConsoleCommandExecutor*> Executors = IModularFeatures::Get().GetModularFeatureImplementations<IConsoleCommandExecutor>(
				IConsoleCommandExecutor::ModularFeatureName());
			const bool bListed = Executors.ContainsByPredicate([](const IConsoleCommandExecutor* Each)
			{
				return Each && Each->GetName() == FDreamConsoleExecutor::StaticName();
			});
			TestTrue(TEXT("Python (Dream) is listed as a console executor after Python init (T-REPL-010)"), Executor.IsRegistered() && bListed);
		});
	});
}
