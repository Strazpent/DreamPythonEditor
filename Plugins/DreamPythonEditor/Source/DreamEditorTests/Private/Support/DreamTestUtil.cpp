// SPDX-License-Identifier: Apache-2.0
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Layout/WidgetPath.h"
#include "Misc/Guid.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamTestLogCapture.h"

namespace DreamTest
{
	namespace
	{
		FString JoinLines(const TArray<FString>& Lines, int32 First, int32 Last)
		{
			FString Out;
			for (int32 Index = First; Index <= Last && Index < Lines.Num(); ++Index)
			{
				if (Index > First)
				{
					Out += TEXT("\n");
				}
				Out += Lines[Index];
			}
			return Out;
		}

		bool IsContentType(dream::LineType Type)
		{
			switch (Type)
			{
			case dream::LineType::Echo:
			case dream::LineType::Stdout:
			case dream::LineType::Stderr:
			case dream::LineType::Result:
			case dream::LineType::Error:
				return true;
			default:
				return false;
			}
		}
	}

	// ---- Services and preconditions ----

	FDreamEditorModule* GetModule(FAutomationTestBase& Test)
	{
		if (!FDreamEditorModule::IsAvailable())
		{
			Test.AddError(TEXT("The DreamEditor module is not loaded"));
			return nullptr;
		}
		return &FDreamEditorModule::Get();
	}

	bool RequireRunnablePython(FAutomationTestBase& Test)
	{
		FDreamEditorModule* Module = GetModule(Test);
		if (!Module)
		{
			return false;
		}
		const FDreamPython& Python = Module->GetPython();
		const EDreamPythonState State = Python.GetState();
		if (Python.IsReady() || Python.CanQueueRuns())
		{
			return true;
		}
		Test.AddError(FString::Printf(TEXT("Dream Python Editor cannot run code in Python state %s (%s). Tier 4 needs an attended editor with an RHI: ")
			TEXT("Dream Python Editor is Headless under -nullrhi, -unattended or a commandlet (TDD 3.4); run DreamHost/Scripts/run_tier4.bat --attended"),
			PythonStateName(State), *Python.GetDisabledReason().ToString()));
		return false;
	}

	TSharedPtr<SDreamTab> RequireTab(FAutomationTestBase& Test)
	{
		FDreamEditorModule* Module = GetModule(Test);
		if (!Module)
		{
			return nullptr;
		}
		TSharedPtr<SDreamTab> Tab = Module->InvokeTab();
		if (!Tab.IsValid())
		{
			Test.AddError(TEXT("FDreamEditorModule::InvokeTab returned no tab (WS-01)"));
		}
		return Tab;
	}

	const TCHAR* PythonStateName(EDreamPythonState State)
	{
		switch (State)
		{
		case EDreamPythonState::Unavailable: return TEXT("Unavailable");
		case EDreamPythonState::Headless: return TEXT("Headless");
		case EDreamPythonState::Disabled: return TEXT("Disabled");
		case EDreamPythonState::WaitingForInit: return TEXT("WaitingForInit");
		case EDreamPythonState::Cold: return TEXT("Cold");
		case EDreamPythonState::Booting: return TEXT("Booting");
		case EDreamPythonState::Ready: return TEXT("Ready");
		case EDreamPythonState::BootFailed: return TEXT("BootFailed");
		case EDreamPythonState::ShuttingDown: return TEXT("ShuttingDown");
		case EDreamPythonState::ShutDown: return TEXT("ShutDown");
		case EDreamPythonState::Enabling: return TEXT("Enabling");
		}
		return TEXT("?");
	}

	FString OutcomeText(EDreamRunOutcome Outcome)
	{
		return Outcome == EDreamRunOutcome::None ? FString(TEXT("(none)")) : DreamRunText::OutcomeName(Outcome).ToString();
	}

	const TCHAR* LineTypeName(dream::LineType Type)
	{
		switch (Type)
		{
		case dream::LineType::Stdout: return TEXT("stdout");
		case dream::LineType::Stderr: return TEXT("stderr");
		case dream::LineType::Result: return TEXT("result");
		case dream::LineType::Notice: return TEXT("notice");
		case dream::LineType::Info: return TEXT("info");
		case dream::LineType::Warning: return TEXT("warning");
		case dream::LineType::Error: return TEXT("error");
		case dream::LineType::Agent: return TEXT("agent");
		case dream::LineType::Debugger: return TEXT("debugger");
		case dream::LineType::Logpoint: return TEXT("logpoint");
		case dream::LineType::Evaluate: return TEXT("evaluate");
		case dream::LineType::RunHeader: return TEXT("header");
		case dream::LineType::RunFooter: return TEXT("footer");
		case dream::LineType::Echo: return TEXT("echo");
		}
		return TEXT("?");
	}

	FString UniqueSuffix()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	}

	FString ScratchName(const FString& Purpose)
	{
		return FString::Printf(TEXT("DreamT4-%s-%s"), *Purpose, *UniqueSuffix().Left(4));
	}

	TSharedPtr<FDreamBuffer> OpenScratch(FAutomationTestBase& Test, SDreamTab& Tab, const FString& Text, const FString& Purpose)
	{
		TSharedRef<FDreamBuffer> Buffer = Tab.NewScratchTab(Text, ScratchName(Purpose));
		Tab.ActivateBuffer(Buffer->GetId());
		if (Buffer->GetTextAsString() != Text)
		{
			Test.AddError(FString::Printf(TEXT("NewScratchTab: the buffer holds \"%s\", expected \"%s\""), *Buffer->GetTextAsString(), *Text));
			return nullptr;
		}
		return Buffer;
	}

	void CloseBuffer(const FGuid& BufferId)
	{
		if (BufferId.IsValid() && FDreamEditorModule::IsAvailable())
		{
			FDreamBufferManager& Buffers = FDreamEditorModule::Get().GetBufferManager();
			if (Buffers.FindById(BufferId).IsValid())
			{
				Buffers.Close(BufferId);
			}
		}
	}

	// ---- Requests ----

	FDreamRunRequest MakeReplRequest(const FString& Code)
	{
		FDreamRunRequest Request;
		Request.Route = EDreamRunRoute::Repl;
		Request.TabTitle = TEXT("REPL");
		Request.Text = Code;
		Request.FirstLine = 1;
		Request.Namespace = EDreamNamespaceMode::Session;
		return Request;
	}

	FDreamRunRequest MakeBufferRequest(const FDreamBuffer& Buffer, EDreamRunRoute Route, int32 FirstLine, int32 LastLine)
	{
		TArray<FString> Lines;
		Buffer.GetTextAsString().ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);

		FDreamRunRequest Request;
		Request.Route = Route;
		Request.BufferId = Buffer.GetId();
		Request.TabTitle = Buffer.GetDisplayName();
		Request.FilePath = Buffer.GetPath();
		Request.bDirty = Buffer.IsDirty();
		Request.SourceSnapshot = Buffer.Snapshot();
		Request.Text = JoinLines(Lines, FirstLine - 1, LastLine - 1);
		Request.FirstLine = FirstLine;
		Request.Filename = FDreamEditorModule::Get().GetBufferManager().GetCompileFilename(Buffer);
		Request.Namespace = Buffer.GetViewState().Namespace;
		return Request;
	}

	// ---- Runs and their output ----

	TArray<FOutputLine> FRunResult::Content() const
	{
		return Lines.FilterByPredicate([](const FOutputLine& Line) { return IsContentType(Line.Type); });
	}

	TArray<FOutputLine> FRunResult::OfType(dream::LineType Type) const
	{
		return Lines.FilterByPredicate([Type](const FOutputLine& Line) { return Line.Type == Type; });
	}

	bool FRunResult::HasLineContaining(dream::LineType Type, const FString& Needle) const
	{
		return Lines.ContainsByPredicate([Type, &Needle](const FOutputLine& Line) { return Line.Type == Type && Line.Text.Contains(Needle); });
	}

	FString FRunResult::Describe() const
	{
		FString Out = FString::Printf(TEXT("#%d %s"), RunNumber, *OutcomeText(Record.Outcome));
		if (!Record.Detail.IsEmpty())
		{
			Out += FString::Printf(TEXT(" (%s)"), *Record.Detail);
		}
		if (Record.Location.Line > 0)
		{
			Out += FString::Printf(TEXT(" at %s:%d"), *Record.Location.File, Record.Location.Line);
		}
		Out += TEXT(":");
		for (const FOutputLine& Line : Lines)
		{
			if (Line.Type == dream::LineType::RunHeader || Line.Type == dream::LineType::RunFooter)
			{
				continue;
			}
			Out += Line.Type == dream::LineType::Echo
				? FString::Printf(TEXT(" | [echo %d] %s"), Line.EchoLine, *Line.Text)
				: FString::Printf(TEXT(" | [%s] %s"), LineTypeName(Line.Type), *Line.Text);
		}
		return Out;
	}

	int32 Submit(FAutomationTestBase& Test, FDreamRunRequest&& Request)
	{
		const EDreamRunRoute Route = Request.Route;
		const FDreamSubmitResult Result = FDreamEditorModule::Get().GetRunService().Submit(MoveTemp(Request));
		if (!Result.bAccepted)
		{
			Test.AddError(FString::Printf(TEXT("Submit refused a %s run: %s"), DreamRunText::RouteKey(Route), *Result.Reason.ToString()));
			return 0;
		}
		return Result.RunNumber;
	}

	bool IsRunFinished(int32 RunNumber)
	{
		const FDreamRunService& RunService = FDreamEditorModule::Get().GetRunService();
		const FDreamRunRecord* Record = RunService.FindRecord(RunNumber);
		if (!Record || Record->Outcome == EDreamRunOutcome::None)
		{
			return false;
		}
		const FDreamRunRecord* Active = RunService.GetActiveRecord();
		return !Active || Active->RunNumber != RunNumber;
	}

	bool IsIdle()
	{
		return !FDreamEditorModule::Get().GetRunService().IsRunActive();
	}

	bool CollectRunLines(uint32 RunNumber, TArray<FOutputLine>& OutLines)
	{
		FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		Store.Drain(0.005);

		OutLines.Reset();
		bool bHasFooter = false;
		const FDreamRunSpan* Span = Store.FindRunSpan(RunNumber);
		const uint64 First = Span && Span->HeaderSeq >= Store.GetFirstSeq() ? Span->HeaderSeq : Store.GetFirstSeq();
		for (uint64 Seq = First; Seq < Store.GetEndSeq(); ++Seq)
		{
			dream::LineView View;
			if (!Store.GetLine(Seq, View) || View.Info.RunNumber != RunNumber)
			{
				continue;
			}
			FOutputLine& Line = OutLines.AddDefaulted_GetRef();
			Line.Seq = Seq;
			Line.Type = View.Info.Type;
			Line.Flags = View.Info.Flags;
			Line.EchoLine = View.Info.EchoLine;
			Line.RunNumber = View.Info.RunNumber;
			Line.Text = DreamText::Utf8ToFString(View.Text);
			bHasFooter |= Line.Type == dream::LineType::RunFooter;
		}
		return bHasFooter || (Span && Span->FooterSeq != 0);
	}

	void AddWaitIdle(FDreamSteps& Steps, double TimeoutSeconds)
	{
		// Also waits for the pass-start cleanup of earlier passes' pending recoveries (StartPassCleanup, DreamTestSteps.h).
		Steps.Poll(TEXT("wait until no run is active"), []() { return IsIdle() && IsPassCleanupDone() ? EStep::Next : EStep::Wait; },
			FMath::Max(TimeoutSeconds, 45.0));
	}

	void AddRun(FDreamSteps& Steps, const FString& Name, TFunction<FDreamRunRequest()> MakeRequest, TSharedRef<FRunResult> Result,
		double TimeoutSeconds)
	{
		FAutomationTestBase& Test = Steps.GetTest();
		Steps.Do(FString::Printf(TEXT("submit %s"), *Name), [&Test, MakeRequest = MoveTemp(MakeRequest), Result]()
		{
			Result->SubmitSeconds = FPlatformTime::Seconds();
			Result->RunNumber = Submit(Test, MakeRequest());
			return Result->RunNumber > 0;
		});
		AddWaitForRun(Steps, Name, Result, TimeoutSeconds);
	}

	void AddWaitForRun(FDreamSteps& Steps, const FString& Name, TSharedRef<FRunResult> Result, double TimeoutSeconds)
	{
		FAutomationTestBase& Test = Steps.GetTest();
		Steps.Poll(FString::Printf(TEXT("wait for %s to finish"), *Name), [&Test, Result]()
		{
			if (Result->RunNumber <= 0)
			{
				Test.AddError(TEXT("No run number to wait for"));
				return EStep::Fail;
			}
			if (!IsRunFinished(Result->RunNumber))
			{
				return EStep::Wait;
			}
			Result->Record = *FDreamEditorModule::Get().GetRunService().FindRecord(Result->RunNumber);
			Result->FinishSeenSeconds = FPlatformTime::Seconds();
			Result->bFinished = true;
			return EStep::Next;
		}, TimeoutSeconds);
		Steps.Poll(FString::Printf(TEXT("wait for the output block of %s"), *Name), [Result]()
		{
			return CollectRunLines(static_cast<uint32>(Result->RunNumber), Result->Lines) ? EStep::Next : EStep::Wait;
		}, 10.0);
	}

	bool ExpectOutcome(FAutomationTestBase& Test, const FRunResult& Run, EDreamRunOutcome Expected)
	{
		if (Run.Record.Outcome == Expected)
		{
			return true;
		}
		Test.AddError(FString::Printf(TEXT("Run outcome %s, expected %s. %s"), *OutcomeText(Run.Record.Outcome), *OutcomeText(Expected), *Run.Describe()));
		return false;
	}

	bool ExpectContent(FAutomationTestBase& Test, const TCHAR* What, const FRunResult& Run, const TArray<FExpectedLine>& Expected)
	{
		const TArray<FOutputLine> Actual = Run.Content();
		bool bEqual = Actual.Num() == Expected.Num();
		for (int32 Index = 0; bEqual && Index < Actual.Num(); ++Index)
		{
			const FOutputLine& Line = Actual[Index];
			const FExpectedLine& Want = Expected[Index];
			bEqual = Line.Type == Want.Type && Line.Text == Want.Text && (Want.Type != dream::LineType::Echo || Line.EchoLine == Want.EchoLine);
		}
		if (bEqual)
		{
			return true;
		}
		FString WantText;
		for (const FExpectedLine& Want : Expected)
		{
			WantText += Want.Type == dream::LineType::Echo
				? FString::Printf(TEXT(" | [echo %d] %s"), Want.EchoLine, *Want.Text)
				: FString::Printf(TEXT(" | [%s] %s"), LineTypeName(Want.Type), *Want.Text);
		}
		Test.AddError(FString::Printf(TEXT("%s: expected%s; got %s"), What, *WantText, *Run.Describe()));
		return false;
	}

	void ExpectPythonErrorBlock(FAutomationTestBase& Test, const FString& Filename, const FString& Summary, const TArray<FString>& SourceNeedles)
	{
		// Occurrences 0: at least once; below 0: any number. Each line is matched twice (the log filter and the
		// framework's log device), so no exact count is used.
		constexpr int32 AtLeastOnce = 0;
		constexpr int32 AnyNumber = -1;
		Test.AddExpectedErrorPlain(Summary, EAutomationExpectedErrorFlags::Contains, AtLeastOnce);
		Test.AddExpectedErrorPlain(TEXT("Traceback (most recent call last):"), EAutomationExpectedErrorFlags::Contains, AnyNumber);
		if (!Filename.IsEmpty())
		{
			Test.AddExpectedErrorPlain(FString::Printf(TEXT("File \"%s\""), *Filename), EAutomationExpectedErrorFlags::Contains, AnyNumber);
		}
		for (const FString& Needle : SourceNeedles)
		{
			Test.AddExpectedErrorPlain(Needle, EAutomationExpectedErrorFlags::Contains, AnyNumber);
		}
		// The caret line under the source (3.11 draws '^' and '~'), with or without the "LogPython: " prefix of the device.
		Test.AddExpectedError(TEXT("^(LogPython: )?\\s*[~^]+\\s*$"), EAutomationExpectedErrorFlags::Contains, AnyNumber);
	}

	void AddWaitForForwardedError(FDreamSteps& Steps, TSharedRef<FDreamTestLogCapture> Log, const FString& Summary, double TimeoutSeconds)
	{
		Steps.Poll(FString::Printf(TEXT("'%s' forwarded to LogPython"), *Summary), [Log, Summary]()
		{
			return Log->CountContaining(FName(TEXT("LogPython")), Summary) > 0 ? EStep::Next : EStep::Wait;
		}, TimeoutSeconds);
	}

	int32 GetActiveRunNumber()
	{
		const FDreamRunRecord* Active = FDreamEditorModule::Get().GetRunService().GetActiveRecord();
		return Active ? Active->RunNumber : 0;
	}

	bool MakePointerPath(const TSharedRef<SWidget>& Widget, FWidgetPath& OutPath)
	{
		FWidgetPath Found;
		if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().FindPathToWidget(Widget, Found))
		{
			return false;
		}
		// The routing reads a pointer position per widget (FWidgetPath::GetVirtualPointerPosition); a hit test fills them.
		TArray<FWidgetAndPointer> WithPointers;
		for (int32 Index = 0; Index < Found.Widgets.Num(); ++Index)
		{
			WithPointers.Add(FWidgetAndPointer(Found.Widgets[Index]));
		}
		OutPath = FWidgetPath(WithPointers);
		return OutPath.IsValid();
	}
}
