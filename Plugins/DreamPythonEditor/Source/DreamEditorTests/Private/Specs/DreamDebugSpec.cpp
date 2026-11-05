// SPDX-License-Identifier: Apache-2.0
// Dream.Debug: the built-in debugger end to end (TDD §8.9.1 G4 fixture step tests that fit this round; DBG-01..DBG-06;
// debug group, PO-06 F5). Breakpoints set through FDreamDebugger, a Debug run submitted like F5 does, and the Paused
// window driven from FDreamDebugger::PausedTickForTest, which runs at each iteration of the Paused window's blocking modal
// loop (the FDreamPausedWindowTestDriver pattern of §8.9.1: latent commands and the core ticker do not run there).
// Breakpoints persist to a temporary store for the whole spec, never the user's.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/DreamEditGroup.h"
#include "CodeView/DreamUndoHistory.h"
#include "Debug/SDreamPausedWindow.h"
#include "Module/DreamEditorModule.h"
#include "Debug/DreamDebugger.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Shell/SDreamTab.h"
#include "Widgets/SWindow.h"

namespace DreamDebugSpecPrivate
{
	/** One expected stop and what to do there. */
	struct FStopScript
	{
		int32 Line = 0;                 // expected 1-based line
		FString Reason;                 // expected reason
		EDreamResumeKind Resume = EDreamResumeKind::Continue;
		FString Local;                  // a local to read from the Variables tree first ("" = none)
	};

	/** What the driver saw at each stop. */
	struct FSeenStop
	{
		int32 Line = 0;
		FString Reason;
		FString Frame;
		FString LocalValue;
		bool bWindowShown = false;
		FVector2D Position = FVector2D::ZeroVector;   // the Paused window at the stop
		int32 SourceLines = 0;
	};

	/** Drives the Paused window from inside its modal loop. */
	struct FDriver : public TSharedFromThis<FDriver>
	{
		TArray<FStopScript> Script;
		TArray<FSeenStop> Seen;
		int32 LastPause = -1;
		int32 Ticks = 0;
		bool bTimedOut = false;

		void Install()
		{
			LastPause = FDreamDebugger::Get().GetPauseCountForTest();
			TWeakPtr<FDriver> Weak = AsShared();
			FDreamDebugger::Get().PausedTickForTest = [Weak](FDreamDebugger& Debugger)
			{
				if (const TSharedPtr<FDriver> Self = Weak.Pin())
				{
					Self->Tick(Debugger);
				}
			};
		}

		static void Uninstall()
		{
			FDreamDebugger::Get().PausedTickForTest = nullptr;
		}

		void Tick(FDreamDebugger& Debugger)
		{
			const int32 Pause = Debugger.GetPauseCountForTest();
			if (Pause != LastPause)
			{
				LastPause = Pause;
				Ticks = 0;
				FSeenStop Stop;
				Stop.Line = Debugger.GetStop().Line;
				Stop.Reason = Debugger.GetStop().Reason;
				Stop.Frame = Debugger.GetStop().Frames.Num() > 0 ? Debugger.GetStop().Frames[0].Name : FString();
				Stop.bWindowShown = Debugger.GetPausedWindowForTest().IsValid() && Debugger.GetPausedWindowForTest()->IsVisible();
				if (Stop.bWindowShown)
				{
					Stop.Position = Debugger.GetPausedWindowForTest()->GetPositionInScreen();
				}
				Seen.Add(Stop);
			}
			++Ticks;
			const int32 Index = Seen.Num() - 1;
			const FStopScript Step = Script.IsValidIndex(Index) ? Script[Index] : FStopScript();
			if (Ticks > 900)
			{
				bTimedOut = true;
				Debugger.RequestResume(EDreamResumeKind::Stop);   // never leave a modal behind (about 15 s at 60 Hz)
				return;
			}
			const TSharedPtr<SDreamPausedWindow> Content = Debugger.GetPausedContentForTest();
			if (Content.IsValid())
			{
				Seen[Index].SourceLines = Content->GetSourceLineCountForTest();
			}
			if (!Step.Local.IsEmpty())
			{
				if (!Content.IsValid())
				{
					return;
				}
				const TSharedPtr<FDreamVariableNode>* Locals = Content->GetRootsForTest().FindByPredicate(
					[](const TSharedPtr<FDreamVariableNode>& Node) { return Node->Name == TEXT("Locals"); });
				if (Locals == nullptr || !(*Locals)->bLoaded)
				{
					return;     // the scopes and variables requests are answered over the next iterations
				}
				for (const TSharedPtr<FDreamVariableNode>& Child : (*Locals)->Children)
				{
					if (Child->Name == Step.Local)
					{
						Seen[Index].LocalValue = Child->Value;
					}
				}
			}
			if (Ticks < 3)
			{
				return;         // let the window paint a few frames first (the window repaints while paused)
			}
			if (Index == 0 && Debugger.GetPausedWindowForTest().IsValid())
			{
				// R-q1-6: a moved window opens where it was left at the next stop.
				Debugger.GetPausedWindowForTest()->MoveWindowTo(Seen[0].Position + FVector2D(40.0, 30.0));
			}
			Debugger.RequestResume(Step.Resume);
		}
	};

	FString TempStorePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(),
			FString::Printf(TEXT("DreamDebugSpec-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)), TEXT("breakpoints.json")));
	}
}

BEGIN_DEFINE_SPEC(FDreamDebugSpec, "Dream.Debug", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	FString StorePath;
	TSharedPtr<FDreamBuffer> OpenBuffer(const FString& Text, const FString& Purpose);
	void RunDebug(const FDoneDelegate& Done, const FString& Text, const TArray<int32>& Breakpoints,
		const TArray<DreamDebugSpecPrivate::FStopScript>& Script, EDreamRunOutcome Expected,
		TFunction<void(const DreamTest::FRunResult&, const DreamDebugSpecPrivate::FDriver&)> Check,
		TFunction<void(const FDreamBuffer&)> BeforeRun = nullptr, TFunction<void(DreamTest::FDreamSteps&)> AfterRun = nullptr);
END_DEFINE_SPEC(FDreamDebugSpec)

TSharedPtr<FDreamBuffer> FDreamDebugSpec::OpenBuffer(const FString& Text, const FString& Purpose)
{
	const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Text, Purpose);
	if (Buffer.IsValid())
	{
		BufferId = Buffer->GetId();
	}
	return Buffer;
}

void FDreamDebugSpec::RunDebug(const FDoneDelegate& Done, const FString& Text, const TArray<int32>& Breakpoints,
	const TArray<DreamDebugSpecPrivate::FStopScript>& Script, EDreamRunOutcome Expected,
	TFunction<void(const DreamTest::FRunResult&, const DreamDebugSpecPrivate::FDriver&)> Check, TFunction<void(const FDreamBuffer&)> BeforeRun,
	TFunction<void(DreamTest::FDreamSteps&)> AfterRun)
{
	using namespace DreamDebugSpecPrivate;
	if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
	{
		Done.Execute();
		return;
	}
	const TSharedPtr<FDreamBuffer> Buffer = OpenBuffer(Text, TEXT("Debug"));
	if (!Buffer.IsValid())
	{
		Done.Execute();
		return;
	}
	for (const int32 Line : Breakpoints)
	{
		FDreamDebugger::Get().SetBreakpoint(*Buffer, Line - 1, true);
	}
	if (BeforeRun)
	{
		BeforeRun(*Buffer);
	}
	TSharedRef<FDriver> Driver = MakeShared<FDriver>();
	Driver->Script = Script;
	TSharedRef<DreamTest::FRunResult> Result = MakeShared<DreamTest::FRunResult>();
	const int32 LineCount = static_cast<int32>(Buffer->Snapshot().LineCount());

	DreamTest::FDreamSteps Steps(*this);
	DreamTest::AddWaitIdle(Steps);
	Steps.Do(TEXT("install the Paused-window driver"), [Driver]()
	{
		Driver->Install();
		return true;
	});
	DreamTest::AddRun(Steps, TEXT("the Debug run"), [Buffer, LineCount]()
	{
		FDreamRunRequest Request = DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, LineCount);
		Request.bDebug = true;
		return Request;
	}, Result, 90.0);
	Steps.Do(TEXT("check the stops"), [this, Driver, Result, Expected, Check]()
	{
		TestFalse(TEXT("the driver never had to stop a stuck pause"), Driver->bTimedOut);
		DreamTest::ExpectOutcome(*this, *Result, Expected);
		TestFalse(TEXT("no pause is left open"), FDreamDebugger::Get().IsPaused());
		if (Check)
		{
			Check(*Result, *Driver);
		}
		return true;
	});
	if (AfterRun)
	{
		AfterRun(Steps);
	}
	Steps.Finally(TEXT("remove the driver"), []()
	{
		FDriver::Uninstall();
	});
	Steps.Run(Done);
}

void FDreamDebugSpec::Define()
{
	using namespace DreamDebugSpecPrivate;

	BeforeEach([this]()
	{
		StorePath = TempStorePath();
		FDreamDebugger::Get().SetStorePathForTest(StorePath);
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		FDriver::Uninstall();
		if (BufferId.IsValid() && FDreamEditorModule::IsAvailable())
		{
			if (const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(BufferId))
			{
				FDreamDebugger::Get().ClearBreakpoints(*Buffer);
			}
		}
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		FDreamDebugger::Get().FlushStoreForTest();
		FDreamDebugger::Get().SetStorePathForTest(FString());
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(StorePath), /*RequireExists=*/false, /*Tree=*/true);
		Tab.Reset();
	});

	Describe("Breakpoints", [this]()
	{
		LatentIt("toggle on a line, move with edits above them and persist outside the file", DreamTest::LatentTimeout(),
			[this](const FDoneDelegate& Done)
		{
			if (!Tab.IsValid())
			{
				Done.Execute();
				return;
			}
			const TSharedPtr<FDreamBuffer> Buffer = OpenBuffer(TEXT("a = 1\nb = 2\nc = 3\n"), TEXT("Bp"));
			if (!Buffer.IsValid())
			{
				Done.Execute();
				return;
			}
			FDreamDebugger& Debugger = FDreamDebugger::Get();
			TestTrue(TEXT("F9 sets"), Debugger.ToggleBreakpoint(*Buffer, 1));
			TestEqual(TEXT("one breakpoint on line 2"), Debugger.GetBreakpointLines(*Buffer), TArray<int32>{ 1 });
			// Three lines inserted above it: the marker moves with its line (§8.2.1).
			FDreamEditGroup Group;
			Group.Label = FText::GetEmpty();
			FDreamTextEdit Insert;
			Insert.Text = TEXT("x = 0\ny = 0\nz = 0\n");
			Group.Edits.Add(Insert);
			TestTrue(TEXT("the edit applied"), FDreamUndoHistory::ApplyEditGroup(*Buffer, Group));
			TestEqual(TEXT("moved by three lines"), Debugger.GetBreakpointLines(*Buffer), TArray<int32>{ 4 });

			// Review q1 M1: the moved line reaches the store by itself (the debounced save after the edit), no flush helper.
			TSharedRef<FString> Last = MakeShared<FString>();
			DreamTest::FDreamSteps Steps(*this);
			Steps.Poll(TEXT("the store names line 5 for the tab"), [this, Buffer, Last]()
			{
				// A scratch tab is untitled until its first auto-save gives it a file: its key follows (buffer:<id>, then the path).
				const FString Key = Buffer->HasPath() ? FPaths::ConvertRelativePathToFull(Buffer->GetPath()).Replace(TEXT("\\"), TEXT("/"))
					: FString(TEXT("buffer:")) + Buffer->GetIdHex();
				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *StorePath))
				{
					return DreamTest::EStep::Wait;
				}
				*Last = Text;
				dream::json::Value Root;
				const FTCHARToUTF8 Utf8(*Text);
				if (!dream::json::Parse(std::string_view(Utf8.Get(), Utf8.Length()), Root))
				{
					return DreamTest::EStep::Wait;
				}
				for (const dream::json::Value::Member& Member : Root.Get("files").Members())
				{
					if (FString(UTF8_TO_TCHAR(Member.Key.c_str())).Equals(Key, ESearchCase::IgnoreCase)
						&& Member.Val.Size() == 1 && Member.Val.At(0).AsInt() == 5)
					{
						return DreamTest::EStep::Next;
					}
				}
				return DreamTest::EStep::Wait;
			}, 15.0);
			Steps.Do(TEXT("check the text and clear"), [this, Buffer, Last]()
			{
				TestFalse(TEXT("nothing is written into the file's text"), Buffer->GetTextAsString().Contains(TEXT("breakpoint")));
				TestFalse(TEXT("F9 again clears"), FDreamDebugger::Get().ToggleBreakpoint(*Buffer, 4));
				TestEqual(TEXT("no breakpoints"), FDreamDebugger::Get().GetBreakpointLines(*Buffer).Num(), 0);
				return true;
			});
			Steps.Finally(TEXT("show the store"), [this, Last]()
			{
				AddInfo(FString::Printf(TEXT("store file as last read: %s"), Last->IsEmpty() ? TEXT("(none)") : **Last));
			});
			Steps.Run(Done);
		});
	});

	Describe("Steps", [this]()
	{
		LatentIt("pauses at a breakpoint, reads a local, steps into, over and out, and continues", DreamTest::LatentTimeout(),
			[this](const FDoneDelegate& Done)
		{
			const FString Sfx = DreamTest::UniqueSuffix();
			const FString Text = FString::Printf(TEXT(
				"def helper_%s(x):\n"          // 1
				"    y = x * 2\n"              // 2
				"    return y\n"               // 3
				"a_%s = 21\n"                  // 4
				"b_%s = helper_%s(a_%s)\n"     // 5
				"c_%s = b_%s + 1\n"),          // 6
				*Sfx, *Sfx, *Sfx, *Sfx, *Sfx, *Sfx, *Sfx);
			TArray<FStopScript> Script;
			Script.Add({ 5, TEXT("breakpoint"), EDreamResumeKind::StepInto, FString::Printf(TEXT("a_%s"), *Sfx) });
			Script.Add({ 2, TEXT("step"), EDreamResumeKind::StepOver, TEXT("x") });
			Script.Add({ 3, TEXT("step"), EDreamResumeKind::StepOut, TEXT("y") });
			Script.Add({ 6, TEXT("step"), EDreamResumeKind::Continue, FString() });
			RunDebug(Done, Text, { 5 }, Script, EDreamRunOutcome::Ok,
				[this, Script](const DreamTest::FRunResult& Run, const FDriver& Driver)
			{
				TestEqual(TEXT("four stops"), Driver.Seen.Num(), Script.Num());
				for (int32 Index = 0; Index < FMath::Min(Driver.Seen.Num(), Script.Num()); ++Index)
				{
					const FSeenStop& Seen = Driver.Seen[Index];
					TestEqual(*FString::Printf(TEXT("stop %d line"), Index + 1), Seen.Line, Script[Index].Line);
					TestEqual(*FString::Printf(TEXT("stop %d reason"), Index + 1), Seen.Reason, Script[Index].Reason);
					TestTrue(*FString::Printf(TEXT("stop %d: the Paused window is up"), Index + 1), Seen.bWindowShown);
				}
				if (Driver.Seen.Num() >= 3)
				{
					TestEqual(TEXT("local a at the breakpoint"), Driver.Seen[0].LocalValue, FString(TEXT("21")));
					TestEqual(TEXT("local x in the helper"), Driver.Seen[1].LocalValue, FString(TEXT("21")));
					TestEqual(TEXT("local y after step over"), Driver.Seen[2].LocalValue, FString(TEXT("42")));
					TestTrue(TEXT("the helper frame is innermost"), Driver.Seen[1].Frame.StartsWith(TEXT("helper_")));
					TestEqual(TEXT("the module frame after step out"), Driver.Seen[3].Frame, FString(TEXT("<module>")));
					TestTrue(TEXT("the source view loaded the buffer"), Driver.Seen[0].SourceLines >= 6);
					TestTrue(*FString::Printf(TEXT("the moved window reopens moved (%s then %s)"), *Driver.Seen[0].Position.ToString(), *Driver.Seen[1].Position.ToString()),
						Driver.Seen[1].Position.Equals(Driver.Seen[0].Position + FVector2D(40.0, 30.0), 2.0));
				}
			});
		});

		LatentIt("stops the run while paused, running finally blocks", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
		{
			const FString Sfx = DreamTest::UniqueSuffix();
			const FString Text = FString::Printf(TEXT(
				"flag_%s = []\n"                       // 1
				"try:\n"                               // 2
				"    p_%s = 1\n"                       // 3
				"    q_%s = 2\n"                       // 4
				"finally:\n"                           // 5
				"    flag_%s.append('finally')\n"),     // 6
				*Sfx, *Sfx, *Sfx, *Sfx);
			TArray<FStopScript> Script;
			Script.Add({ 3, TEXT("breakpoint"), EDreamResumeKind::Stop, FString() });
			TSharedRef<DreamTest::FRunResult> Probe = MakeShared<DreamTest::FRunResult>();
			RunDebug(Done, Text, { 3 }, Script, EDreamRunOutcome::Cancelled,
				[this](const DreamTest::FRunResult& Run, const FDriver& Driver)
			{
				TestEqual(TEXT("one stop"), Driver.Seen.Num(), 1);
			}, nullptr, [this, Sfx, Probe](DreamTest::FDreamSteps& Steps)
			{
				// Review q1 M2: ScriptCancelled raised from the paused line ran the finally block, and line 3 never finished.
				DreamTest::AddRun(Steps, TEXT("the Session probe"), [Sfx]()
				{
					return DreamTest::MakeReplRequest(FString::Printf(
						TEXT("print('dream-stop-probe', flag_%s, 'p_%s' in globals(), 'q_%s' in globals())"), *Sfx, *Sfx, *Sfx));
				}, Probe);
				Steps.Do(TEXT("check the Session"), [this, Probe]()
				{
					if (DreamTest::ExpectOutcome(*this, *Probe, EDreamRunOutcome::Ok))
					{
						TestTrue(*FString::Printf(TEXT("finally ran, lines 3 and 4 never did: %s"), *Probe->Describe()),
							Probe->HasLineContaining(dream::LineType::Stdout, TEXT("dream-stop-probe ['finally'] False False")));
					}
					return true;
				});
			});
		});

		LatentIt("pauses at an uncaught exception on its raising line; Continue ends the run as error", DreamTest::LatentTimeout(),
			[this](const FDoneDelegate& Done)
		{
			const FString Sfx = DreamTest::UniqueSuffix();
			const FString Text = FString::Printf(TEXT(
				"data_%s = {}\n"                       // 1
				"def read_%s():\n"                     // 2
				"    return data_%s['missing']\n"      // 3
				"value_%s = read_%s()\n"),             // 4
				*Sfx, *Sfx, *Sfx, *Sfx, *Sfx);
			TArray<FStopScript> Script;
			Script.Add({ 3, TEXT("exception"), EDreamResumeKind::StepOver, FString() });   // a step at an exception stop continues
			RunDebug(Done, Text, {}, Script, EDreamRunOutcome::Error,
				[this, Sfx](const DreamTest::FRunResult& Run, const FDriver& Driver)
			{
				TestEqual(TEXT("one stop"), Driver.Seen.Num(), 1);
				if (Driver.Seen.Num() == 1)
				{
					TestEqual(TEXT("in the raising function"), Driver.Seen[0].Frame, FString::Printf(TEXT("read_%s"), *Sfx));
				}
			}, [this, Sfx](const FDreamBuffer& Buffer)
			{
				// The run fails on purpose: its traceback reaches LogPython (declared, section 6.3).
				DreamTest::ExpectPythonErrorBlock(*this, FDreamEditorModule::Get().GetBufferManager().GetCompileFilename(Buffer),
					TEXT("KeyError: 'missing'"), { FString::Printf(TEXT("data_%s"), *Sfx), FString::Printf(TEXT("read_%s"), *Sfx) });
			});
		});

		LatentIt("has no trace function in a normal run or in a Debug run without breakpoints (DBG-06)", DreamTest::LatentTimeout(),
			[this](const FDoneDelegate& Done)
		{
			if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
			{
				Done.Execute();
				return;
			}
			const TSharedPtr<FDreamBuffer> Buffer = OpenBuffer(TEXT("import sys\nprint('dream-trace', sys.gettrace(), sys.breakpointhook.__name__)\n"), TEXT("Trace"));
			if (!Buffer.IsValid())
			{
				Done.Execute();
				return;
			}
			TSharedRef<DreamTest::FRunResult> Plain = MakeShared<DreamTest::FRunResult>();
			TSharedRef<DreamTest::FRunResult> Debug = MakeShared<DreamTest::FRunResult>();
			DreamTest::FDreamSteps Steps(*this);
			DreamTest::AddWaitIdle(Steps);
			DreamTest::AddRun(Steps, TEXT("a normal run"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 2); }, Plain);
			DreamTest::AddRun(Steps, TEXT("a Debug run without breakpoints"), [Buffer]()
			{
				FDreamRunRequest Request = DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 2);
				Request.bDebug = true;
				return Request;
			}, Debug);
			Steps.Do(TEXT("check"), [this, Plain, Debug]()
			{
				if (DreamTest::ExpectOutcome(*this, *Plain, EDreamRunOutcome::Ok))
				{
					TestTrue(*FString::Printf(TEXT("normal run: %s"), *Plain->Describe()),
						Plain->HasLineContaining(dream::LineType::Stdout, TEXT("dream-trace None breakpointhook")));
				}
				if (DreamTest::ExpectOutcome(*this, *Debug, EDreamRunOutcome::Ok))
				{
					TestTrue(*FString::Printf(TEXT("debug run: %s"), *Debug->Describe()),
						Debug->HasLineContaining(dream::LineType::Stdout, TEXT("dream-trace None breakpointhook")));
				}
				return true;
			});
			Steps.Run(Done);
		});
	});
}
