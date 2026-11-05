// SPDX-License-Identifier: Apache-2.0
// Dream.Run.SelectionEcho: a selection of three statements, statement 2 printing (T-OUT-041's first case, OUT-12,
// RUN-27). The run is statement mode with one echo per statement before that statement's output, numbered with the
// buffer lines, and the unselected lines around it are neither echoed nor run.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Shell/SDreamTab.h"

BEGIN_DEFINE_SPEC(FDreamSelectionEchoSpec, "Dream.Run.SelectionEcho", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamSelectionEchoSpec)

void FDreamSelectionEchoSpec::Define()
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

	LatentIt("echoes each statement of a three-statement selection before its output, in order", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}

		const FString Sfx = DreamTest::UniqueSuffix();
		const TArray<FString> Source = {
			FString::Printf(TEXT("sel_before_%s = 0"), *Sfx),                 // line 1: not selected
			FString::Printf(TEXT("sel_a_%s = 1"), *Sfx),                      // line 2
			FString::Printf(TEXT("print('dream-two-%s')"), *Sfx),              // line 3
			FString::Printf(TEXT("sel_b_%s = sel_a_%s + 1"), *Sfx, *Sfx),     // line 4
			FString::Printf(TEXT("sel_after_%s = 5"), *Sfx),                  // line 5: not selected
		};
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, FString::Join(Source, TEXT("\n")), TEXT("Echo"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();

		TSharedRef<DreamTest::FRunResult> Selection = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Probe = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("the selection of lines 2-4"), [Buffer]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Selection, 2, 4);
		}, Selection, 60.0);

		Steps.Do(TEXT("check the selection run"), [this, Selection, Source, Sfx]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Selection, EDreamRunOutcome::Ok))
			{
				return false;
			}
			const FDreamRunRecord& Record = Selection->Record;
			TestTrue(TEXT("route selection"), Record.Route == EDreamRunRoute::Selection);
			TestEqual(TEXT("executed range first line"), Record.FirstLine, 2);
			TestEqual(TEXT("executed range last line"), Record.LastLine, 4);
			TestTrue(TEXT("statement mode (RUN-27)"), Record.bStatementMode);
			TestEqual(TEXT("prepared statements"), Record.Statements.Num(), 3);
			TestEqual(TEXT("statements started"), Record.StatementsStarted, 3);
			TestEqual(TEXT("statements returned"), Record.StatementsReturned, 3);

			// Echo 1, echo 2, the printed line, echo 3; assignments and print() display nothing (None, RUN-08).
			DreamTest::ExpectContent(*this, TEXT("selection block"), *Selection, {
				DreamTest::FExpectedLine::Echo(2, Source[1]),
				DreamTest::FExpectedLine::Echo(3, Source[2]),
				DreamTest::FExpectedLine::Stdout(FString::Printf(TEXT("dream-two-%s"), *Sfx)),
				DreamTest::FExpectedLine::Echo(4, Source[3]),
			});
			for (const DreamTest::FOutputLine& Line : Selection->OfType(dream::LineType::Echo))
			{
				TestTrue(*FString::Printf(TEXT("echo of line %d starts its statement (OLF_EchoFirst)"), Line.EchoLine),
					(Line.Flags & dream::OLF_EchoFirst) != 0);
				TestTrue(*FString::Printf(TEXT("echo of line %d has no REPL prompt flag"), Line.EchoLine), (Line.Flags & dream::OLF_ReplPrompt) == 0);
			}
			return true;
		});

		// The unselected lines did not run, and the selected ones ran in the Session.
		DreamTest::AddRun(Steps, TEXT("the Session probe"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("print('sel_before_%s' in globals(), 'sel_after_%s' in globals(), sel_b_%s)"),
				*Sfx, *Sfx, *Sfx));
		}, Probe);
		Steps.Do(TEXT("check the Session"), [this, Probe]()
		{
			if (DreamTest::ExpectOutcome(*this, *Probe, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("lines 1 and 5 never ran and line 4 did: %s"), *Probe->Describe()),
					Probe->HasLineContaining(dream::LineType::Stdout, TEXT("False False 2")));
			}
			return true;
		});
		Steps.Run(Done);
	});
}
