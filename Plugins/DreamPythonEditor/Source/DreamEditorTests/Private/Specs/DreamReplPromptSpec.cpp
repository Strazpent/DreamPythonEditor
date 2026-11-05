// SPDX-License-Identifier: Apache-2.0
// Dream.Repl.Prompt: the REPL prompt strip's submission rules (TDD §6.5, REPL-01, T-REPL-004). A forced submission of an
// incomplete statement is accepted, ends not executed, and its text leaves the prompt for the history; a submission refused
// because a run is active keeps its text in the prompt. The Enter completeness decision and type-ahead are the G5 suite
// (slice B).
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamReplHistory.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/SDreamTab.h"

namespace DreamReplPromptSpec
{
	FString PromptText()
	{
		return FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->GetTextAsString();
	}
}

BEGIN_DEFINE_SPEC(FDreamReplPromptSpec, "Dream.Repl.Prompt", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
END_DEFINE_SPEC(FDreamReplPromptSpec)

void FDreamReplPromptSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->ResetText(FString(), dream::EditOrigin::Load);
		}
		Tab.Reset();
	});

	LatentIt("moves a forced incomplete submission to history and ends it not executed; a refused one keeps its text (T-REPL-004)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this) || !Tab->GetOutputArea().IsValid())
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<SDreamReplStrip> Strip = Tab->GetOutputArea()->GetReplStrip();
		if (!TestTrue(TEXT("the REPL prompt strip exists"), Strip.IsValid()))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Incomplete = FString::Printf(TEXT("incomplete_%s = (1,"), *Sfx);
		const FString Kept = FString::Printf(TEXT("kept_%s = 2"), *Sfx);
		TSharedRef<DreamTest::FRunResult> Forced = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Blocker = MakeShared<DreamTest::FRunResult>();
		// The not executed block reaches LogPython at Error (§6.3), which this spec expects.
		const FString Summary = TEXT("was never closed");
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		DreamTest::ExpectPythonErrorBlock(*this, FString(), Summary, { TEXT("<dream:REPL#"), Incomplete });

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("Ctrl+Enter on an incomplete statement"), [this, Strip, Incomplete, Forced]()
		{
			Strip->PutInPrompt(Incomplete);
			TestEqual(TEXT("the prompt holds the statement"), DreamReplPromptSpec::PromptText(), Incomplete);
			Strip->Submit(EDreamReplSubmit::Force);
			Forced->SubmitSeconds = FPlatformTime::Seconds();
			Forced->RunNumber = DreamTest::GetActiveRunNumber();
			return TestTrue(TEXT("the forced submission was accepted"), Forced->RunNumber > 0);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the forced run"), Forced);
		Steps.Do(TEXT("check the forced run"), [this, Incomplete, Forced]()
		{
			DreamTest::ExpectOutcome(*this, *Forced, EDreamRunOutcome::NotExecuted);
			TestTrue(TEXT("the prompt is empty after an accepted submission"), DreamReplPromptSpec::PromptText().IsEmpty());
			const TArray<FDreamHistoryEntry> Found = FDreamEditorModule::Get().GetReplHistory().Query(Incomplete, EDreamHistoryMatch::Prefix, 5);
			TestTrue(TEXT("the submitted text is in the REPL history (also for not executed)"),
				Found.ContainsByPredicate([&Incomplete](const FDreamHistoryEntry& E) { return E.Text == Incomplete; }));
			return true;
		});
		Steps.Do(TEXT("a submission while a run is active"), [this, Strip, Kept, Blocker]()
		{
			Strip->PutInPrompt(Kept);       // typed before the other run started (the prompt is read-only during a run)
			Blocker->SubmitSeconds = FPlatformTime::Seconds();
			Blocker->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(TEXT("pass")));
			if (Blocker->RunNumber <= 0)
			{
				return false;
			}
			const int32 Before = FDreamEditorModule::Get().GetRunService().GetHistory().Num();
			Strip->Submit(EDreamReplSubmit::Force);         // refused: one run at a time (D14(4))
			TestEqual(TEXT("the refused submission ran nothing"), FDreamEditorModule::Get().GetRunService().GetHistory().Num(), Before);
			TestEqual(TEXT("a refused submission keeps its text in the prompt"), DreamReplPromptSpec::PromptText(), Kept);
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the active run"), Blocker);
		Steps.Do(TEXT("the kept text is still there"), [this, Kept]()
		{
			TestEqual(TEXT("the prompt still holds the refused text"), DreamReplPromptSpec::PromptText(), Kept);
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, Summary);
		Steps.Finally(TEXT("release the log capture"), [Log]() {});
		Steps.Run(Done);
	});
}
