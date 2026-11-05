// SPDX-License-Identifier: Apache-2.0
// Dream.Run.Session: the Session namespace across runs and routes (D8, RUN-10, T-RUN-011 at tier 4), Isolated runs that
// neither see nor leave Session names (RUN-21, T-GAP-015), and auto-print (RUN-08, §5.3.6, T-RUN-009): the last
// expression of a Session run gives one result line, "# Result: <repr>" in the Unreal log (§6.3), binds `_` in the
// Session, and None prints nothing and leaves `_` alone.
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Shell/SDreamTab.h"

BEGIN_DEFINE_SPEC(FDreamSessionSpec, "Dream.Run.Session", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamSessionSpec)

void FDreamSessionSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		Tab.Reset();
	});

	LatentIt("shares the Session between a REPL run and a buffer run; an Isolated run sees and leaves nothing", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const TSharedPtr<FDreamBuffer> Reader = DreamTest::OpenScratch(*this, *Tab,
			FString::Printf(TEXT("print(shared_%s + 1)"), *Sfx), TEXT("SessionRead"));
		const TSharedPtr<FDreamBuffer> Isolated = DreamTest::OpenScratch(*this, *Tab,
			FString::Printf(TEXT("iso_%s = 1\nprint(shared_%s)"), *Sfx, *Sfx), TEXT("Isolated"));
		if (!Reader.IsValid() || !Isolated.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds = { Reader->GetId(), Isolated->GetId() };

		// The Isolated run's NameError traceback reaches LogPython at Error (§6.3), which this spec expects.
		const FString IsolatedError = FString::Printf(TEXT("NameError: name 'shared_%s' is not defined"), *Sfx);
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		DreamTest::ExpectPythonErrorBlock(*this, DreamTest::MakeBufferRequest(*Isolated, EDreamRunRoute::Buffer, 1, 2).Filename, IsolatedError,
			{ FString::Printf(TEXT("print(shared_%s)"), *Sfx) });

		TSharedRef<DreamTest::FRunResult> Defined = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Read = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Iso = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Leak = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);

		DreamTest::AddRun(Steps, TEXT("the REPL definition"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("shared_%s = 41"), *Sfx));
		}, Defined, 60.0);
		DreamTest::AddRun(Steps, TEXT("the buffer read"), [Reader]()
		{
			return DreamTest::MakeBufferRequest(*Reader, EDreamRunRoute::Buffer, 1, 1);
		}, Read);
		Steps.Do(TEXT("check the Session read"), [this, Defined, Read]()
		{
			DreamTest::ExpectOutcome(*this, *Defined, EDreamRunOutcome::Ok);
			if (DreamTest::ExpectOutcome(*this, *Read, EDreamRunOutcome::Ok))
			{
				TestTrue(TEXT("the buffer run read the REPL run's name"), Read->HasLineContaining(dream::LineType::Stdout, TEXT("42")));
				TestTrue(TEXT("run numbers increase"), Read->RunNumber > Defined->RunNumber);
			}
			return true;
		});

		DreamTest::AddRun(Steps, TEXT("the Isolated run"), [Isolated]()
		{
			FDreamRunRequest Request = DreamTest::MakeBufferRequest(*Isolated, EDreamRunRoute::Buffer, 1, 2);
			Request.Namespace = EDreamNamespaceMode::Isolated;
			return Request;
		}, Iso);
		DreamTest::AddRun(Steps, TEXT("the leak probe"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("print('iso_%s' in globals())"), *Sfx));
		}, Leak);
		Steps.Do(TEXT("check Isolated"), [this, Iso, Leak]()
		{
			if (DreamTest::ExpectOutcome(*this, *Iso, EDreamRunOutcome::Error))
			{
				TestTrue(*FString::Printf(TEXT("the Isolated run cannot see the Session name: %s"), *Iso->Describe()),
					Iso->HasLineContaining(dream::LineType::Error, TEXT("NameError")));
				TestTrue(TEXT("an Isolated run is not in statement mode (RUN-27 routes)"), !Iso->Record.bStatementMode);
				TestEqual(TEXT("an Isolated run echoes nothing"), Iso->OfType(dream::LineType::Echo).Num(), 0);
				TestEqual(TEXT("the error is at line 2 of the buffer"), Iso->Record.Location.Line, 2);
			}
			if (DreamTest::ExpectOutcome(*this, *Leak, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("the Isolated assignment did not reach the Session: %s"), *Leak->Describe()),
					Leak->HasLineContaining(dream::LineType::Stdout, TEXT("False")));
			}
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, IsolatedError);
		Steps.Finally(TEXT("release the log capture"), [Log]() {});
		Steps.Run(Done);
	});

	LatentIt("auto-prints the last expression as a result line and binds _ in the Session (RUN-08)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		// A value no other output of the session prints, so the log check is about this run only.
		const int32 Value = FRandomStream(static_cast<int32>(FPlatformTime::Cycles())).RandRange(100000000, 999999999);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
			FString::Printf(TEXT("v_%s = 2\nv_%s * 10\nv_%s * 21"), *Sfx, *Sfx, *Sfx), TEXT("AutoPrint"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds = { Buffer->GetId() };

		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		TSharedRef<DreamTest::FRunResult> First = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Underscore = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> NoneRun = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Kept = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> BufferRun = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);

		DreamTest::AddRun(Steps, TEXT("the expression"), [Value]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("%d + 0"), Value));
		}, First, 60.0);
		DreamTest::AddRun(Steps, TEXT("_ + 1"), []() { return DreamTest::MakeReplRequest(TEXT("_ + 1")); }, Underscore);
		DreamTest::AddRun(Steps, TEXT("None"), []() { return DreamTest::MakeReplRequest(TEXT("None")); }, NoneRun);
		DreamTest::AddRun(Steps, TEXT("print(_)"), []() { return DreamTest::MakeReplRequest(TEXT("print(_)")); }, Kept);
		DreamTest::AddRun(Steps, TEXT("the buffer"), [Buffer]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 3);
		}, BufferRun);

		Steps.Do(TEXT("check the result lines"), [this, Value, First, Underscore, NoneRun, Kept, BufferRun, Sfx]()
		{
			const FString Repr = FString::Printf(TEXT("%d"), Value);
			const FString Next = FString::Printf(TEXT("%d"), Value + 1);
			if (DreamTest::ExpectOutcome(*this, *First, EDreamRunOutcome::Ok))
			{
				// The stored text has no prefix: the view draws "# Result: " and the forwarder prepends it (§6.3, §6.4).
				DreamTest::ExpectContent(*this, TEXT("REPL expression"), *First, {
					DreamTest::FExpectedLine::Echo(1, FString::Printf(TEXT("%d + 0"), Value)),
					DreamTest::FExpectedLine::Result(Repr),
				});
				const TArray<DreamTest::FOutputLine> Echo = First->OfType(dream::LineType::Echo);
				TestTrue(TEXT("a REPL echo is marked for the run log's >>> prompt (OLF_ReplPrompt)"), Echo.Num() == 1 && (Echo[0].Flags & dream::OLF_ReplPrompt) != 0);
			}
			if (DreamTest::ExpectOutcome(*this, *Underscore, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("_ held the previous result: %s"), *Underscore->Describe()),
					Underscore->OfType(dream::LineType::Result).Num() == 1 && Underscore->OfType(dream::LineType::Result)[0].Text == Next);
			}
			if (DreamTest::ExpectOutcome(*this, *NoneRun, EDreamRunOutcome::Ok))
			{
				TestEqual(TEXT("None prints no result line"), NoneRun->OfType(dream::LineType::Result).Num(), 0);
			}
			if (DreamTest::ExpectOutcome(*this, *Kept, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("None did not rebind _: %s"), *Kept->Describe()), Kept->HasLineContaining(dream::LineType::Stdout, Next));
			}
			if (DreamTest::ExpectOutcome(*this, *BufferRun, EDreamRunOutcome::Ok))
			{
				// Only the last top-level statement is displayed, after its own echo (§5.3.6, RUN-27).
				DreamTest::ExpectContent(*this, TEXT("buffer with two expressions"), *BufferRun, {
					DreamTest::FExpectedLine::Echo(1, FString::Printf(TEXT("v_%s = 2"), *Sfx)),
					DreamTest::FExpectedLine::Echo(2, FString::Printf(TEXT("v_%s * 10"), *Sfx)),
					DreamTest::FExpectedLine::Echo(3, FString::Printf(TEXT("v_%s * 21"), *Sfx)),
					DreamTest::FExpectedLine::Result(TEXT("42")),
				});
			}
			return true;
		});

		// The forwarder writes LogPython from the DreamOutput thread, batched; poll for it.
		Steps.Poll(TEXT("# Result: in the Unreal log"), [this, Log, Value]()
		{
			return Log->CountEqual(FName(TEXT("LogPython")), FString::Printf(TEXT("# Result: %d"), Value)) > 0
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("check the log"), [this, Log, Value]()
		{
			const FName LogPython(TEXT("LogPython"));
			TestEqual(TEXT("one '# Result:' LogPython line for the first run"), Log->CountEqual(LogPython, FString::Printf(TEXT("# Result: %d"), Value)), 1);
			TestEqual(TEXT("echo lines are never forwarded to LogPython (§6.3)"),
				Log->CountContaining(LogPython, FString::Printf(TEXT("%d + 0"), Value)), 0);
			return true;
		});
		Steps.Finally(TEXT("release the log capture"), [Log]() {});
		Steps.Run(Done);
	});

	LatentIt("marks >>> on a statement's own first line and # Result: on a repr's first line only (OUT-06, OUT-07)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const int32 Value = FRandomStream(static_cast<int32>(FPlatformTime::Cycles())).RandRange(100000000, 999999999);
		const FString First = FString::Printf(TEXT("ra_%d"), Value);
		const FString Second = FString::Printf(TEXT("rb_%d"), Value);
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		TSharedRef<DreamTest::FRunResult> Split = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Repr = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);

		// Two statements on line 1; the second goes on to line 2, so its first echoed line (line 2) is not its first line.
		DreamTest::AddRun(Steps, TEXT("two statements over two lines"), []()
		{
			return DreamTest::MakeReplRequest(TEXT("pa_ = 1; pb_ = (2,\n 3)"));
		}, Split, 60.0);
		DreamTest::AddRun(Steps, TEXT("a two-line repr"), [First, Second]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(
				TEXT("type('TwoLines', (), {'__repr__': lambda self: '%s\\n%s'})()"), *First, *Second));
		}, Repr);

		Steps.Do(TEXT("check the prompt and result flags"), [this, Split, Repr, First, Second]()
		{
			if (DreamTest::ExpectOutcome(*this, *Split, EDreamRunOutcome::Ok))
			{
				const TArray<DreamTest::FOutputLine> Echo = Split->OfType(dream::LineType::Echo);
				if (TestEqual(*FString::Printf(TEXT("each line echoed once: %s"), *Split->Describe()), Echo.Num(), 2))
				{
					TestTrue(TEXT("line 1 starts statement 1: '>>>' (OLF_PromptFirst)"), (Echo[0].Flags & dream::OLF_PromptFirst) != 0);
					TestTrue(TEXT("line 2 is the first line echoed for statement 2 (OLF_EchoFirst)"), (Echo[1].Flags & dream::OLF_EchoFirst) != 0);
					TestTrue(TEXT("line 2 continues statement 2: '...', not '>>>'"), (Echo[1].Flags & dream::OLF_PromptFirst) == 0);
				}
			}
			if (DreamTest::ExpectOutcome(*this, *Repr, EDreamRunOutcome::Ok))
			{
				const TArray<DreamTest::FOutputLine> Results = Repr->OfType(dream::LineType::Result);
				if (TestEqual(*FString::Printf(TEXT("two result lines: %s"), *Repr->Describe()), Results.Num(), 2))
				{
					TestEqual(TEXT("first result line"), Results[0].Text, First);
					TestEqual(TEXT("second result line"), Results[1].Text, Second);
					TestTrue(TEXT("the first line carries # Result:"), (Results[0].Flags & dream::OLF_ResultCont) == 0);
					TestTrue(TEXT("the second line does not (OLF_ResultCont)"), (Results[1].Flags & dream::OLF_ResultCont) != 0);
				}
			}
			return true;
		});
		Steps.Poll(TEXT("the repr in the Unreal log"), [Log, Second]()
		{
			return Log->CountEqual(FName(TEXT("LogPython")), Second) > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("check the log"), [this, Log, First, Second]()
		{
			const FName LogPython(TEXT("LogPython"));
			TestEqual(TEXT("'# Result: ' before the first line"), Log->CountEqual(LogPython, TEXT("# Result: ") + First), 1);
			TestEqual(TEXT("the second line without the prefix"), Log->CountEqual(LogPython, Second), 1);
			TestEqual(TEXT("no '# Result: ' before the second line"), Log->CountEqual(LogPython, TEXT("# Result: ") + Second), 0);
			return true;
		});
		Steps.Finally(TEXT("release the log capture"), [Log]() {});
		Steps.Run(Done);
	});
}
