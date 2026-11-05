// SPDX-License-Identifier: Apache-2.0
// Dream.Output.ConsoleExecutor: "Python (Dream)" in the Output Log console (§6.6, UEI-08, T-REPL-012 and T-REPL-014).
// Exec only submits and returns before _run starts; each line is a numbered Session run on the ConsoleExecutor route
// with the <dream:Console#N> filename, echoed once to LogPython (EchoNow), and a last expression gives "# Result:" in
// Dream's store and in the Output Log. The inputs join the executor's history.
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamConsoleExecutor.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamProjectSettings.h"

BEGIN_DEFINE_SPEC(FDreamConsoleExecutorSpec, "Dream.Output.ConsoleExecutor", DreamTest::AttendedFlags)
	/** Exec(Input) and the checks that hold right after it returns; the run number goes to Run. */
	bool ExecLine(const FString& Input, const TSharedRef<DreamTest::FRunResult>& Run);
END_DEFINE_SPEC(FDreamConsoleExecutorSpec)

bool FDreamConsoleExecutorSpec::ExecLine(const FString& Input, const TSharedRef<DreamTest::FRunResult>& Run)
{
	FDreamConsoleExecutor& Executor = FDreamEditorModule::Get().GetConsoleExecutor();
	Run->SubmitSeconds = FPlatformTime::Seconds();
	if (!TestTrue(*FString::Printf(TEXT("Exec(\"%s\") returns true"), *Input), Executor.Exec(*Input)))
	{
		return false;
	}
	const FDreamRunRecord* Active = FDreamEditorModule::Get().GetRunService().GetActiveRecord();
	if (!Active)
	{
		AddError(FString::Printf(TEXT("Exec(\"%s\") submitted no run"), *Input));
		return false;
	}
	Run->RunNumber = Active->RunNumber;
	TestTrue(TEXT("Exec returns before _run starts (the run is Starting)"), Active->State == EDreamRunState::Starting);
	TestTrue(TEXT("route ConsoleExecutor"), Active->Route == EDreamRunRoute::ConsoleExecutor);
	TestEqual(TEXT("compile filename"), Active->Filename, FString::Printf(TEXT("<dream:Console#%d>"), Active->RunNumber));
	return true;
}

void FDreamConsoleExecutorSpec::Define()
{
	LatentIt("runs a line as a numbered Session run with # Result: in Dream Python Editor and the Output Log", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const int32 Value = FRandomStream(static_cast<int32>(FPlatformTime::Cycles())).RandRange(100000000, 999999999);
		const FString Assign = FString::Printf(TEXT("cx_%s = %d"), *Sfx, Value);
		const FString Expression = FString::Printf(TEXT("cx_%s + 1"), *Sfx);
		const FString ResultText = FString::Printf(TEXT("%d"), Value + 1);

		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		TSharedRef<DreamTest::FRunResult> First = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Second = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);

		Steps.Do(TEXT("Exec the assignment"), [this, Assign, First]()
		{
			const FDreamConsoleExecutor& Executor = FDreamEditorModule::Get().GetConsoleExecutor();
			TestEqual(TEXT("display name"), Executor.GetDisplayName().ToString(), FString(TEXT("Python (Dream)")));
			if (UDreamProjectSettings::Get()->bRegisterConsoleExecutor)
			{
				TestTrue(TEXT("the executor is registered once Python is initialized (T-REPL-010)"), Executor.IsRegistered());
			}
			return ExecLine(Assign, First);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the assignment"), First, 60.0);
		Steps.Do(TEXT("Exec the expression"), [this, Expression, Second]() { return ExecLine(Expression, Second); });
		DreamTest::AddWaitForRun(Steps, TEXT("the expression"), Second);

		Steps.Do(TEXT("check the runs"), [this, Assign, Expression, ResultText, First, Second]()
		{
			if (!DreamTest::ExpectOutcome(*this, *First, EDreamRunOutcome::Ok) || !DreamTest::ExpectOutcome(*this, *Second, EDreamRunOutcome::Ok))
			{
				return false;
			}
			TestTrue(TEXT("two run numbers, in order (U8)"), Second->RunNumber > First->RunNumber);
			TestTrue(TEXT("console runs use the Session"), Second->Record.Namespace == EDreamNamespaceMode::Session);
			TestTrue(TEXT("console runs are statement mode (RUN-27)"), Second->Record.bStatementMode);
			DreamTest::ExpectContent(*this, TEXT("assignment block"), *First, { DreamTest::FExpectedLine::Echo(1, Assign) });
			DreamTest::ExpectContent(*this, TEXT("expression block"), *Second, {
				DreamTest::FExpectedLine::Echo(1, Expression),
				DreamTest::FExpectedLine::Result(ResultText),
			});
			for (const DreamTest::FOutputLine& Line : Second->OfType(dream::LineType::Echo))
			{
				TestTrue(TEXT("a console echo is marked for the run log's >>> prompt (OLF_ReplPrompt)"), (Line.Flags & dream::OLF_ReplPrompt) != 0);
			}

			TArray<FString> History;
			FDreamEditorModule::Get().GetConsoleExecutor().GetExecHistory(History);
			TestTrue(TEXT("both inputs are in the executor history, newest last (T-REPL-014)"),
				History.Num() >= 2 && History.Last() == Expression && History[History.Num() - 2] == Assign);
			return true;
		});

		Steps.Poll(TEXT("# Result: in the Output Log"), [Log, ResultText]()
		{
			return Log->CountEqual(FName(TEXT("LogPython")), FString::Printf(TEXT("# Result: %s"), *ResultText)) > 0
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("check the Output Log"), [this, Log, Assign, Expression, ResultText]()
		{
			const FName LogPython(TEXT("LogPython"));
			TestEqual(TEXT("one # Result: line"), Log->CountEqual(LogPython, FString::Printf(TEXT("# Result: %s"), *ResultText)), 1);
			TestEqual(*FString::Printf(TEXT("the assignment is echoed to LogPython once:%s"), *Log->Describe(LogPython)),
				Log->CountContaining(LogPython, Assign), 1);
			TestEqual(TEXT("the expression is echoed to LogPython once"), Log->CountContaining(LogPython, Expression), 1);
			return true;
		});
		Steps.Run(Done);
	});
}
