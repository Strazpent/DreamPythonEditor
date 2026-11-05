// SPDX-License-Identifier: Apache-2.0
// Dream.Output.RunLog: the run log and the LogPython forwarding of a statement-mode run (TDD §6.1-§6.3, OUT-08, T-OUT-021,
// T-OUT-044). A buffer run of `print('Working...', end='')` / `print(' done')` forwards one joined line to LogPython and
// no echo text, while its run-<N>.log keeps the echo before each statement's output with the EchoSplit line split there;
// a REPL run that only assigns (echo alone) makes no run log (§6.2 "Input echo in run logs").
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Misc/FileHelper.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamOutputStore.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"

BEGIN_DEFINE_SPEC(FDreamRunLogSpec, "Dream.Output.RunLog", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamRunLogSpec)

void FDreamRunLogSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
	});

	LatentIt("joins a split print for LogPython and keeps the echo in the run log (T-OUT-021, T-OUT-044)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Working = FString::Printf(TEXT("Working_%s..."), *Sfx);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
			FString::Printf(TEXT("print('%s', end='')\nprint(' done')"), *Working), TEXT("RunLog"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<FString> LogText = MakeShared<FString>();
		TSharedRef<bool> bSavedAlways = MakeShared<bool>(true);

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("always write run logs"), [bSavedAlways]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			*bSavedAlways = Settings->bAlwaysWriteRunLogs;
			Settings->bAlwaysWriteRunLogs = true;
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the split print"), [Buffer]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 2);
		}, Run);
		Steps.Poll(TEXT("the run log is complete"), [Run, LogText]()
		{
			const FString Path = FDreamEditorModule::Get().GetOutputStore().GetRunLogPath(static_cast<uint32>(Run->RunNumber));
			LogText->Reset();
			return !Path.IsEmpty() && FFileHelper::LoadFileToString(*LogText, *Path) && LogText->Contains(TEXT("# end"))
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Poll(TEXT("LogPython has the joined line"), [Log, Working]()
		{
			return Log->CountEqual(FName(TEXT("LogPython")), Working + TEXT(" done")) > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("check LogPython and the run log"), [this, Run, Log, LogText, Working]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			const FName LogPython(TEXT("LogPython"));
			TestEqual(*FString::Printf(TEXT("one joined line in LogPython (§6.3 rule 1):%s"), *Log->Describe(LogPython)),
				Log->CountEqual(LogPython, Working + TEXT(" done")), 1);
			TestEqual(TEXT("no echo text in LogPython (a buffer run's echo is never forwarded)"), Log->CountContaining(LogPython, TEXT("print(")), 0);

			// The run log, in order: echo 1, the split part, echo 2, the rest, the end line (§6.2 format).
			const TCHAR* Order[] = { TEXT("echo [1]"), *Working, TEXT("echo [2]"), TEXT(" done"), TEXT("# end") };
			int32 From = 0;
			for (const TCHAR* Needle : Order)
			{
				const int32 At = LogText->Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
				if (!TestTrue(*FString::Printf(TEXT("the run log has \"%s\" in order:\n%s"), Needle, **LogText), At != INDEX_NONE))
				{
					break;
				}
				From = At + FCString::Strlen(Needle);
			}
			return true;
		});
		Steps.Finally(TEXT("restore the setting"), [bSavedAlways]()
		{
			UDreamUserSettings::GetMutable()->bAlwaysWriteRunLogs = *bSavedAlways;
		});
		Steps.Run(Done);
	});

	LatentIt("makes no run log for a REPL run that only echoes (TDD 6.2)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<bool> bSavedAlways = MakeShared<bool>(true);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		// With "always write run logs" on, only an echo line counted as output could create the file: the rule under test.
		Steps.Do(TEXT("always write run logs"), [bSavedAlways]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			*bSavedAlways = Settings->bAlwaysWriteRunLogs;
			Settings->bAlwaysWriteRunLogs = true;
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("an assignment"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("echo_only_%s = 1"), *Sfx));
		}, Run);
		Steps.WaitFrames(10);
		Steps.Do(TEXT("check there is no run log"), [this, Run]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			TestTrue(TEXT("the run echoed its input"), Run->OfType(dream::LineType::Echo).Num() > 0);
			const FString Path = FDreamEditorModule::Get().GetOutputStore().GetRunLogPath(static_cast<uint32>(Run->RunNumber));
			TestTrue(*FString::Printf(TEXT("no run log for an echo-only run (got \"%s\")"), *Path), Path.IsEmpty());
			return true;
		});
		Steps.Finally(TEXT("restore the setting"), [bSavedAlways]()
		{
			UDreamUserSettings::GetMutable()->bAlwaysWriteRunLogs = *bSavedAlways;
		});
		Steps.Run(Done);
	});
}
