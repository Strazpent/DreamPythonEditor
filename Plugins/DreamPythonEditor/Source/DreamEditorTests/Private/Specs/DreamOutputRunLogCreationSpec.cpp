// SPDX-License-Identifier: Apache-2.0
// Dream.Output.RunLogCreation: when a run gets a run log (TDD §6.2 "Design: run logs (spill)", OUT-08, D17; the creation
// rows of T-OUT-023; slice B item B93):
//   1. with "Always write run logs" on (the default), a run with one output line makes <root>/RunLogs/<session>/run-<N>.log,
//      and a run with no output makes none;
//   2. with it off, a run of 10,001 lines (over the 10,000-line spill threshold) makes a file holding every line, and a
//      run of 9,999 lines makes none.
// The echo cap is lifted for the runs, because a window that holds lines back forces a run log too (§6.2).
// Not covered here: the eviction marker naming the folder, the synthetic header after an evicted header (both need a
// full 200,000-line ring) and the gap line of a throttled disk (needs a FileOps fixture in the run-log pipe).
#include "Support/DreamTestUtil.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamOutputStore.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"

namespace DreamOutputRunLogCreationSpec
{
	FString RunLogPath(const DreamTest::FRunResult& Run)
	{
		return FDreamEditorModule::Get().GetOutputStore().GetRunLogPath(static_cast<uint32>(Run.RunNumber));
	}

	/** Lines of the run log that are Marker's output lines (not its echo). */
	int32 CountOutputLines(const FString& Text, const FString& Marker)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		int32 Count = 0;
		for (const FString& Line : Lines)
		{
			Count += Line.Contains(Marker) && !Line.Contains(TEXT("echo [")) ? 1 : 0;
		}
		return Count;
	}

	FString PrintLoop(const FString& Marker, int32 Count)
	{
		return FString::Printf(TEXT("for _rl_i in range(%d):\n    print(f'%s {_rl_i}')"), Count, *Marker);
	}

	struct FState
	{
		FString Sfx;
		bool bSettingsSaved = false;
		bool bSavedAlways = true;
		FString LogText;
	};
}

BEGIN_DEFINE_SPEC(FDreamOutputRunLogCreationSpec, "Dream.Output.RunLogCreation", DreamTest::AttendedFlags)
END_DEFINE_SPEC(FDreamOutputRunLogCreationSpec)

void FDreamOutputRunLogCreationSpec::Define()
{
	LatentIt("makes a run log only for runs with output, or past 10,000 lines with the setting off (T-OUT-023)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamOutputRunLogCreationSpec;
		if (!DreamTest::RequireTab(*this).IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		TSharedRef<FState> State = MakeShared<FState>();
		State->Sfx = DreamTest::UniqueSuffix();
		const FString One = FString::Printf(TEXT("rl_one_%s"), *State->Sfx);
		const FString Over = FString::Printf(TEXT("rl_over_%s"), *State->Sfx);
		const FString Under = FString::Printf(TEXT("rl_under_%s"), *State->Sfx);
		TSharedRef<DreamTest::FRunResult> OneLine = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> NoOutput = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Above = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Below = MakeShared<DreamTest::FRunResult>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("always write run logs; no echo cap"), [State]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedAlways = Settings->bAlwaysWriteRunLogs;
			State->bSettingsSaved = true;
			Settings->bAlwaysWriteRunLogs = true;
			FDreamEditorModule::Get().GetOutputStore().SetEchoCap(0);
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("a run with one output line"), [One]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("print('%s')"), *One));
		}, OneLine);
		Steps.Poll(TEXT("its run log is complete"), [State, OneLine]()
		{
			const FString Path = RunLogPath(*OneLine);
			State->LogText.Reset();
			return !Path.IsEmpty() && FFileHelper::LoadFileToString(State->LogText, *Path) && State->LogText.Contains(TEXT("# end"))
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("check the one-line run's log"), [this, State, OneLine, One]()
		{
			DreamTest::ExpectOutcome(*this, *OneLine, EDreamRunOutcome::Ok);
			const FString Path = FPaths::ConvertRelativePathToFull(RunLogPath(*OneLine));
			TestTrue(*FString::Printf(TEXT("the file is <root>/RunLogs/<session>/run-<N>.log: %s"), *Path),
				Path.Replace(TEXT("\\"), TEXT("/")).Contains(TEXT("/RunLogs/"))
				&& FPaths::GetCleanFilename(Path) == FString::Printf(TEXT("run-%d.log"), OneLine->RunNumber));
			TestEqual(TEXT("it holds the output line"), CountOutputLines(State->LogText, One), 1);
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("a run with no output"), [State]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("rl_none_%s = 1"), *State->Sfx));
		}, NoOutput);
		Steps.WaitFrames(10);
		Steps.Do(TEXT("check there is no log; then turn the setting off"), [this, NoOutput]()
		{
			DreamTest::ExpectOutcome(*this, *NoOutput, EDreamRunOutcome::Ok);
			TestTrue(*FString::Printf(TEXT("a run with no output makes no run log (%s)"), *RunLogPath(*NoOutput)), RunLogPath(*NoOutput).IsEmpty());
			UDreamUserSettings::GetMutable()->bAlwaysWriteRunLogs = false;
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("10,001 lines with the setting off"), [Over]()
		{
			return DreamTest::MakeReplRequest(PrintLoop(Over, 10001));
		}, Above, 60.0);
		Steps.Poll(TEXT("the 10,001-line run's log is complete"), [State, Above]()
		{
			const FString Path = RunLogPath(*Above);
			State->LogText.Reset();
			return !Path.IsEmpty() && FFileHelper::LoadFileToString(State->LogText, *Path) && State->LogText.Contains(TEXT("# end"))
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 15.0);
		Steps.Do(TEXT("check it holds every line"), [this, State, Above, Over]()
		{
			DreamTest::ExpectOutcome(*this, *Above, EDreamRunOutcome::Ok);
			TestEqual(TEXT("over 10,000 lines the buffered lines and the rest are all in the file (§6.2 spill)"),
				CountOutputLines(State->LogText, Over), 10001);
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("9,999 lines with the setting off"), [Under]()
		{
			return DreamTest::MakeReplRequest(PrintLoop(Under, 9999));
		}, Below, 60.0);
		Steps.WaitFrames(10);
		Steps.Do(TEXT("check there is no log"), [this, Below]()
		{
			DreamTest::ExpectOutcome(*this, *Below, EDreamRunOutcome::Ok);
			TestEqual(TEXT("the run's 9,999 lines are in the view"), Below->OfType(dream::LineType::Stdout).Num(), 9999);
			TestTrue(*FString::Printf(TEXT("under 10,000 lines a run makes no run log with the setting off (%s)"), *RunLogPath(*Below)),
				RunLogPath(*Below).IsEmpty());
			return true;
		});
		Steps.Finally(TEXT("restore the setting and the project's echo cap"), [State]()
		{
			if (State->bSettingsSaved)
			{
				UDreamUserSettings::GetMutable()->bAlwaysWriteRunLogs = State->bSavedAlways;
			}
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetOutputStore().ClearEchoCap();
			}
		});
		Steps.Run(Done);
	});
}
