// SPDX-License-Identifier: Apache-2.0
// Dream.Run.SyntaxError: a compile error anywhere in the run ends it `not executed` before any statement runs (RUN-16,
// RUN-27 whole-module compile, T-RUN-032 at tier 4): numbered (U8), the SyntaxError block at its buffer line in the
// output, no echo line, no output, and the marker that statement 1 would write never reaches the Session.
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Shell/SDreamTab.h"

BEGIN_DEFINE_SPEC(FDreamSyntaxErrorSpec, "Dream.Run.SyntaxError", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;

	/** Runs Source as a buffer run, expects `not executed` at ErrorLine with Needle in the error block, then probes the marker. */
	void DefineCase(const FString& CaseName, const FString& Purpose, TFunction<FString(const FString& Sfx)> MakeSource, int32 ErrorLine,
		const FString& Needle);
END_DEFINE_SPEC(FDreamSyntaxErrorSpec)

void FDreamSyntaxErrorSpec::DefineCase(const FString& CaseName, const FString& Purpose, TFunction<FString(const FString& Sfx)> MakeSource,
	int32 ErrorLine, const FString& Needle)
{
	LatentIt(CaseName, DreamTest::LatentTimeout(), [this, Purpose, MakeSource, ErrorLine, Needle](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, MakeSource(Sfx), Purpose);
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FString Filename = DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 1).Filename;
		const int32 LastLine = Buffer->Snapshot().LineCount();

		// The SyntaxError block reaches LogPython at Error (§6.3), which this spec expects: File line, the source line at
		// ErrorLine, the caret line and "SyntaxError: <message>".
		TArray<FString> SourceLines;
		Buffer->GetTextAsString().ParseIntoArrayLines(SourceLines, /*InCullEmpty*/ false);
		const FString Summary = FString::Printf(TEXT("SyntaxError: %s"), *Needle);
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		DreamTest::ExpectPythonErrorBlock(*this, Filename, Summary,
			{ SourceLines.IsValidIndex(ErrorLine - 1) ? SourceLines[ErrorLine - 1].TrimStartAndEnd() : Summary });

		TSharedRef<DreamTest::FRunResult> Broken = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Probe = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("the broken buffer"), [Buffer, LastLine]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, LastLine);
		}, Broken, 60.0);

		Steps.Do(TEXT("check not executed"), [this, Broken, ErrorLine, Needle, Filename]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Broken, EDreamRunOutcome::NotExecuted))
			{
				return false;
			}
			const FDreamRunRecord& Record = Broken->Record;
			TestTrue(TEXT("the not executed run keeps its number (U8)"), Broken->RunNumber > 0 && Record.RunNumber == Broken->RunNumber);
			TestEqual(TEXT("the SyntaxError is at its buffer line"), Record.Location.Line, ErrorLine);
			TestEqual(TEXT("the location names the compile filename"), Record.Location.File, Filename);
			TestEqual(TEXT("no statement started"), Record.StatementsStarted, 0);
			TestTrue(TEXT("no transaction was opened (U7)"), Record.Txn.Status == EDreamTxnStatus::Off || Record.Txn.Status == EDreamTxnStatus::NotStarted);
			TestEqual(TEXT("no echo line (T-RUN-032)"), Broken->OfType(dream::LineType::Echo).Num(), 0);
			TestEqual(TEXT("no stdout"), Broken->OfType(dream::LineType::Stdout).Num(), 0);
			TestEqual(TEXT("no result"), Broken->OfType(dream::LineType::Result).Num(), 0);
			TestTrue(*FString::Printf(TEXT("the error block says SyntaxError: %s"), *Broken->Describe()),
				Broken->HasLineContaining(dream::LineType::Error, TEXT("SyntaxError")));
			TestTrue(*FString::Printf(TEXT("the error block names the message '%s'"), *Needle), Broken->HasLineContaining(dream::LineType::Error, Needle));
			TestTrue(*FString::Printf(TEXT("the error block names line %d"), ErrorLine),
				Broken->HasLineContaining(dream::LineType::Error, FString::Printf(TEXT("line %d"), ErrorLine)));
			return true;
		});

		DreamTest::AddRun(Steps, TEXT("the marker probe"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("print('marker_%s' in globals())"), *Sfx));
		}, Probe);
		Steps.Do(TEXT("check the marker"), [this, Probe]()
		{
			if (DreamTest::ExpectOutcome(*this, *Probe, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("statement 1 never ran: %s"), *Probe->Describe()), Probe->HasLineContaining(dream::LineType::Stdout, TEXT("False")));
			}
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, Summary);
		Steps.Finally(TEXT("release the log capture"), [Log]() {});
		Steps.Run(Done);
	});
}

void FDreamSyntaxErrorSpec::Define()
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

	DefineCase(TEXT("ends not executed with nothing run for an unclosed bracket in statement 3 of 3"), TEXT("Syntax"),
		[](const FString& Sfx)
		{
			return FString::Printf(TEXT("marker_%s = 'ran'\nprint('dream-must-not-print-%s')\nbroken_%s = (1,"), *Sfx, *Sfx, *Sfx);
		}, 3, TEXT("'(' was never closed"));

	DefineCase(TEXT("ends not executed with nothing run for a module-level return in statement 3 of 3"), TEXT("Return"),
		[](const FString& Sfx)
		{
			return FString::Printf(TEXT("marker_%s = 'ran'\ny_%s = 2\nreturn y_%s"), *Sfx, *Sfx, *Sfx);
		}, 3, TEXT("'return' outside function"));
}
