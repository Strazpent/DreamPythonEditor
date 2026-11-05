// SPDX-License-Identifier: Apache-2.0
// Dream.Language.*: the language services of TDD §7 at tier 4 (lang group, B52-B56).
// - Dream.Language.Model (any editor): the Problems model's dedup, severity, edits and the F8 neighbour (§7.8.1, §7.8.2).
// - Dream.Language.Transport (any editor): FLspTransport with a real dream-check child: contained in the job, initialize with
//   the editor's processId, a publish, shutdown -> exit within the grace (§7.1, T-LSP-009's processId, TEL-01's log).
// - Dream.Language.Headless (any editor): a headless editor never starts a server (D16).
// - Dream.Language.EscapeCheck (any editor, Win64): the escape check's PID-reuse guard with a real child process (lang-M1).
// - Dream.Language.SnapshotGates (attended): no snapshot slice within 300 ms of a keystroke (buffer or REPL), while a
//   modal is open, or inside a run that ticks the core ticker (T-STUB-028, lang-M2), on a fresh test walk.
// - Dream.Language.InstanceLogs (any editor): a secondary instance's server log goes to <sec>/Logs/LSP, the primary's to
//   <root>/Logs/LSP (TEL-01, §10.5), with two journals on a private root as Dream.Persistence.Instances opens them.
// - Dream.Language.Services (attended): the deferred start after a boot, diagnostics into Problems, close clears, no result
//   applies during a run (RT-01, T-LSP-020), a killed server restarts and its items go stale then fresh (T-DIAG-015,
//   INT-11), safe mode stops and leaving it restarts (T-LSP-028, §10.8).
// - Dream.Language.Problems (attended): Parse items (§7.8.1), runtime items (T-DIAG-016), the F8 / Shift+F8 walk (T-DIAG-017),
//   a quick fix as one undo step (T-DIAG-021, EDT-08).
// - Dream.Language.Snapshot (attended): the Stage 1 snapshot completes in gated idle slices and SUM101 fires on a misspelt
//   name (T-STUB-024, T-STUB-028); the SUM102 fix rewrites the assignment as one undo step (T-DIAG-002, T-DIAG-003).
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestSteps.h"
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "CodeView/DreamUndoHistory.h"
#include "Commands/DreamCommands.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Language/DreamLanguageServices.h"
#include "Language/DreamLspTransport.h"
#include "Language/DreamProblemsModel.h"
#include "Language/DreamStubSnapshot.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamRestoreGuard.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamTab.h"
#include "dream/JsonRpc.h"
#include "dream/TextBuffer.h"
#include "Widgets/SWindow.h"

namespace DreamLanguageSpecLocal
{
	FDreamLanguageServices* Services()
	{
		return FDreamEditorModule::IsAvailable() ? &FDreamEditorModule::Get().GetLanguageServices() : nullptr;
	}

	const TCHAR* HealthName(EDreamLanguageHealth Health)
	{
		switch (Health)
		{
		case EDreamLanguageHealth::Deferred: return TEXT("Deferred");
		case EDreamLanguageHealth::Disabled: return TEXT("Disabled");
		case EDreamLanguageHealth::Starting: return TEXT("Starting");
		case EDreamLanguageHealth::Ready: return TEXT("Ready");
		case EDreamLanguageHealth::Restarting: return TEXT("Restarting");
		case EDreamLanguageHealth::Paused: return TEXT("Paused");
		case EDreamLanguageHealth::Stopped: return TEXT("Stopped");
		}
		return TEXT("?");
	}

	/** The items of one source in one buffer with Code (any line when Line < 0). */
	TArray<FDreamProblem> Find(const FGuid& Buffer, EDreamProblemSource Source, const FString& Code, int32 Line = -1)
	{
		TArray<FDreamProblem> Out;
		if (FDreamLanguageServices* Lang = Services())
		{
			for (const FDreamProblem& Item : Lang->GetProblems().GetItems(Buffer, Source))
			{
				if (Item.Code == Code && (Line < 0 || Item.StartLine == Line))
				{
					Out.Add(Item);
				}
			}
		}
		return Out;
	}

	FDreamProblem MakeItem(const FGuid& Buffer, EDreamProblemSource Source, int32 Line, const FString& DedupClass,
		EDreamSeverity Severity = EDreamSeverity::Warning, const FString& Code = TEXT("X1"))
	{
		FDreamProblem Item;
		Item.Buffer = Buffer;
		Item.Source = Source;
		Item.Severity = Severity;
		Item.Code = Code;
		Item.Message = Code;
		Item.DedupClass = DedupClass;
		Item.StartLine = Item.EndLine = Line;
		Item.StartColumn = 0;
		Item.EndColumn = 1;
		return Item;
	}

	/** Boots dream_editor (the deferred start runs after the boot) and waits for the server to be Ready. */
	void AddStartAndWaitReady(FAutomationTestBase& Test, DreamTest::FDreamSteps& Steps, double TimeoutSeconds = 90.0)
	{
		TSharedRef<DreamTest::FRunResult> Boot = MakeShared<DreamTest::FRunResult>();
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a boot run"), []() { return DreamTest::MakeReplRequest(TEXT("_dream_lang_boot = 1")); }, Boot, 60.0);
		Steps.Poll(TEXT("dream-check ready"), [&Test]()
		{
			FDreamLanguageServices* Lang = Services();
			if (Lang == nullptr)
			{
				Test.AddError(TEXT("no language services"));
				return DreamTest::EStep::Fail;
			}
			const EDreamLanguageHealth Health = Lang->GetHealth();
			if (Health == EDreamLanguageHealth::Ready)
			{
				return DreamTest::EStep::Next;
			}
			if (Health == EDreamLanguageHealth::Paused || Health == EDreamLanguageHealth::Disabled || Health == EDreamLanguageHealth::Stopped)
			{
				Test.AddError(FString::Printf(TEXT("language services are %s: %s"), HealthName(Health), *Lang->GetHealthText().ToString()));
				return DreamTest::EStep::Fail;
			}
			return DreamTest::EStep::Wait;
		}, TimeoutSeconds);
	}

	/** Polls until Buffer has an item of Source with Code (at Line when >= 0). */
	void AddWaitForItem(FAutomationTestBase& Test, DreamTest::FDreamSteps& Steps, const FString& Name, TFunction<FGuid()> Buffer,
		EDreamProblemSource Source, const FString& Code, int32 Line, double TimeoutSeconds = 15.0)
	{
		Steps.Poll(Name, [&Test, Buffer, Source, Code, Line]()
		{
			const TArray<FDreamProblem> Items = Find(Buffer(), Source, Code, Line);
			return Items.Num() > 0 && !Items[0].bStale ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, TimeoutSeconds);
	}

	FString Line(const FString& Text, int32 Index)
	{
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty*/ false);
		return Lines.IsValidIndex(Index) ? Lines[Index] : FString();
	}
}

// ---- Dream.Language.Model --------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageModelSpec, "Dream.Language.Model", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamLanguageModelSpec)

void FDreamLanguageModelSpec::Define()
{
	using namespace DreamLanguageSpecLocal;

	It("keeps one item per line and dedup class, Parse first for syntax, and names the merged sources (TDD 7.8.2)", [this]()   // (tests) ASCII test name
	{
		FDreamProblemsModel Model;
		const FGuid Buffer = FGuid::NewGuid();
		Model.ReplaceSource(Buffer, EDreamProblemSource::DreamCheck, { MakeItem(Buffer, EDreamProblemSource::DreamCheck, 4, TEXT("syntax"),
			EDreamSeverity::Error, TEXT("syntax-error")), MakeItem(Buffer, EDreamProblemSource::DreamCheck, 4, TEXT("dream-check:SUM202")) });
		Model.ReplaceSource(Buffer, EDreamProblemSource::Parse, { MakeItem(Buffer, EDreamProblemSource::Parse, 4, TEXT("syntax"),
			EDreamSeverity::Error, TEXT("SyntaxError")) });
		const TArray<FDreamProblem> Visible = Model.GetVisible();
		TestEqual(TEXT("the syntax pair merges; the SUM202 item stays apart"), Visible.Num(), 2);
		const FDreamProblem* Syntax = Visible.FindByPredicate([](const FDreamProblem& Item) { return Item.DedupClass == TEXT("syntax"); });
		if (TestNotNull(TEXT("the syntax item"), Syntax))
		{
			TestTrue(TEXT("Parse is kept for syntax"), Syntax->Source == EDreamProblemSource::Parse);
			TestTrue(TEXT("the tooltip names dream-check"), Syntax->MergedFrom.Contains(TEXT("dream-check")));
		}
	});

	It("leaves hints out and replaces a source's set on each publish (TDD 7.8.1, TDD 7.8.2)", [this]()   // (tests) ASCII test name
	{
		FDreamProblemsModel Model;
		const FGuid Buffer = FGuid::NewGuid();
		Model.ReplaceSource(Buffer, EDreamProblemSource::DreamCheck, { MakeItem(Buffer, EDreamProblemSource::DreamCheck, 1, TEXT("a"), EDreamSeverity::Hint),
			MakeItem(Buffer, EDreamProblemSource::DreamCheck, 2, TEXT("b")) });
		TestEqual(TEXT("the hint is not listed"), Model.GetVisible().Num(), 1);
		Model.ReplaceSource(Buffer, EDreamProblemSource::DreamCheck, { MakeItem(Buffer, EDreamProblemSource::DreamCheck, 7, TEXT("c")) });
		const TArray<FDreamProblem> Visible = Model.GetVisible();
		TestTrue(TEXT("the publish replaced the set"), Visible.Num() == 1 && Visible[0].StartLine == 7);
		Model.ClearBuffer(Buffer);
		TestEqual(TEXT("close clears"), Model.GetVisible().Num(), 0);
	});

	It("moves items below an edit and clears a Parse item on an edited line (TDD 7.8.1)", [this]()   // (tests) ASCII test name
	{
		FDreamProblemsModel Model;
		const FGuid Buffer = FGuid::NewGuid();
		Model.ReplaceSource(Buffer, EDreamProblemSource::Parse, { MakeItem(Buffer, EDreamProblemSource::Parse, 3, TEXT("syntax")) });
		Model.ReplaceSource(Buffer, EDreamProblemSource::DreamCheck, { MakeItem(Buffer, EDreamProblemSource::DreamCheck, 3, TEXT("lint")),
			MakeItem(Buffer, EDreamProblemSource::DreamCheck, 8, TEXT("lint2")) });
		// One line inserted at the start of line 1 (two '\n' before it in the old text at line 1).
		dream::ChangeSet Insert;
		dream::ChangeRange& Range = Insert.Ranges.emplace_back();
		Range.StartLine = 1;
		Range.OldLines = 0;
		Range.NewLines = 1;
		Range.NewLen = 1;
		Model.OnBufferEdited(Buffer, Insert);
		TArray<FDreamProblem> Lint = Model.GetItems(Buffer, EDreamProblemSource::DreamCheck);
		TestTrue(TEXT("lint items moved down one line"), Lint.Num() == 2 && Lint[0].StartLine == 4 && Lint[1].StartLine == 9);
		TestTrue(TEXT("the Parse item moved down one line"), Model.GetItems(Buffer, EDreamProblemSource::Parse).Num() == 1 &&
			Model.GetItems(Buffer, EDreamProblemSource::Parse)[0].StartLine == 4);
		dream::ChangeSet Edit;
		dream::ChangeRange& OnLine = Edit.Ranges.emplace_back();
		OnLine.StartLine = 4;
		OnLine.OldLen = 1;
		OnLine.NewLen = 2;
		Model.OnBufferEdited(Buffer, Edit);
		TestEqual(TEXT("an edit of its line clears the Parse item"), Model.GetItems(Buffer, EDreamProblemSource::Parse).Num(), 0);
		TestEqual(TEXT("the lint item on that line stays until the next publish"), Model.GetItems(Buffer, EDreamProblemSource::DreamCheck).Num(), 2);
	});

	It("finds the F8 / Shift+F8 neighbour after the caret and wraps (U13)", [this]()
	{
		FDreamProblemsModel Model;
		const FGuid Buffer = FGuid::NewGuid();
		Model.ReplaceSource(Buffer, EDreamProblemSource::DreamCheck, { MakeItem(Buffer, EDreamProblemSource::DreamCheck, 2, TEXT("a")),
			MakeItem(Buffer, EDreamProblemSource::DreamCheck, 5, TEXT("b")), MakeItem(Buffer, EDreamProblemSource::DreamCheck, 9, TEXT("c")) });
		const TArray<FDreamProblem> Visible = Model.GetVisible();
		auto LineAt = [&Visible](int32 Index) { return Visible.IsValidIndex(Index) ? Visible[Index].StartLine : -1; };
		TestEqual(TEXT("F8 from line 0"), LineAt(FDreamProblemsModel::FindNeighbour(Visible, Buffer, 0, 0, true)), 2);
		TestEqual(TEXT("F8 from line 5"), LineAt(FDreamProblemsModel::FindNeighbour(Visible, Buffer, 5, 0, true)), 9);
		TestEqual(TEXT("F8 from line 9 wraps"), LineAt(FDreamProblemsModel::FindNeighbour(Visible, Buffer, 9, 0, true)), 2);
		TestEqual(TEXT("Shift+F8 from line 5"), LineAt(FDreamProblemsModel::FindNeighbour(Visible, Buffer, 5, 0, false)), 2);
		TestEqual(TEXT("Shift+F8 from line 2 wraps"), LineAt(FDreamProblemsModel::FindNeighbour(Visible, Buffer, 2, 0, false)), 9);
	});

	It("keeps a crashed server's items stale for at most 10 s (TDD 7.3.5)", [this]()   // (tests) ASCII test name
	{
		FDreamProblemsModel Model;
		const FGuid Buffer = FGuid::NewGuid();
		Model.ReplaceSource(Buffer, EDreamProblemSource::DreamCheck, { MakeItem(Buffer, EDreamProblemSource::DreamCheck, 1, TEXT("a")) });
		Model.MarkSourceStale(EDreamProblemSource::DreamCheck, 100.0);
		TestTrue(TEXT("stale"), Model.GetVisible().Num() == 1 && Model.GetVisible()[0].bStale);
		Model.ExpireStale(105.0);
		TestEqual(TEXT("kept before 10 s"), Model.GetVisible().Num(), 1);
		Model.ExpireStale(110.5);
		TestEqual(TEXT("dropped after 10 s"), Model.GetVisible().Num(), 0);
	});

	It("visits every item at one position with F8 and Shift+F8 (review r1 minor: SUM900 and SUM901 both at 0:0)", [this]()
	{
		FDreamProblemsModel Model;
		const FGuid Buffer = FGuid::NewGuid();
		Model.ReplaceSource(Buffer, EDreamProblemSource::DreamCheck, { MakeItem(Buffer, EDreamProblemSource::DreamCheck, 0, TEXT("a"),
			EDreamSeverity::Info, TEXT("SUM900")), MakeItem(Buffer, EDreamProblemSource::DreamCheck, 0, TEXT("b"), EDreamSeverity::Info,
			TEXT("SUM901")), MakeItem(Buffer, EDreamProblemSource::DreamCheck, 4, TEXT("c")) });
		const TArray<FDreamProblem> Visible = Model.GetVisible();
		if (!TestEqual(TEXT("three items"), Visible.Num(), 3))
		{
			return;
		}
		// The caret sits on an item after the step that reached it (OpenProblem puts it at the item's start).
		const int32 First = FDreamProblemsModel::FindNeighbour(Visible, Buffer, 0, -1, true);
		TestEqual(TEXT("F8 from before line 0 reaches the first 0:0 item"), First, 0);
		const int32 Second = FDreamProblemsModel::FindNeighbour(Visible, Buffer, 0, 0, true, First);
		TestEqual(TEXT("F8 again reaches the second 0:0 item"), Second, 1);
		const int32 Third = FDreamProblemsModel::FindNeighbour(Visible, Buffer, 0, 0, true, Second);
		TestEqual(TEXT("then line 4"), Third, 2);
		const int32 Back = FDreamProblemsModel::FindNeighbour(Visible, Buffer, 4, 0, false, Third);
		TestEqual(TEXT("Shift+F8 comes back to the second 0:0 item"), Back, 1);
		TestEqual(TEXT("and then to the first"), FDreamProblemsModel::FindNeighbour(Visible, Buffer, 0, 0, false, Back), 0);
		TestEqual(TEXT("a caret moved elsewhere ignores the last step"), FDreamProblemsModel::FindNeighbour(Visible, Buffer, 2, 0, true, 0), 2);
	});
}

// ---- Dream.Language.Transport ----------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageTransportSpec, "Dream.Language.Transport", DreamTest::AnyEditorFlags)
	TUniquePtr<FLspTransport> Transport;
	FString LogPath;
END_DEFINE_SPEC(FDreamLanguageTransportSpec)

void FDreamLanguageTransportSpec::Define()
{
	AfterEach([this]()
	{
		if (Transport.IsValid())
		{
			Transport->RequestStop(FTimespan::Zero());
			Transport->Join();
			Transport.Reset();
		}
	});

	LatentIt("runs dream-check contained, publishes for an opened document and stops on shutdown then exit (TDD 7.1)",   // (tests) ASCII test name
		FTimespan::FromSeconds(90.0), [this](const FDoneDelegate& Done)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor"));
		const FString Python = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
		if (!TestTrue(TEXT("the plugin and the engine python exist"), Plugin.IsValid() && FPaths::FileExists(Python)))
		{
			Done.Execute();
			return;
		}
		const FString Launcher = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()) / TEXT("Tools/Python/dream_launch.py");
		LogPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Dream") / TEXT("Tests") /
			FString::Printf(TEXT("lsp-%s.log"), *DreamTest::UniqueSuffix()));
		FDreamLaunchSpec Spec;
		Spec.ServerId = TEXT("dream-check-test");
		Spec.Executable = Python;
		Spec.Arguments = FString::Printf(TEXT("-I -B \"%s\" check --parent-pid %u"), *Launcher, FPlatformProcess::GetCurrentProcessId());
		Spec.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		Spec.StderrLogPath = LogPath;
		Spec.ParentPid = FPlatformProcess::GetCurrentProcessId();
		dream::LspClientOptions Options;
		Options.ServerId = "dream-check-test";
		Transport = FLspTransport::Start(Spec, MoveTemp(Options));

		struct FState
		{
			bool bReady = false;
			bool bDiagnostics = false;
			bool bExited = false;
			int32 ExitCode = -1;
			uint32 Pid = 0;
			double StopAt = 0.0;
			double ExitedAt = 0.0;
			TArray<FString> Codes;
		};
		TSharedRef<FState> State = MakeShared<FState>();
		auto Pump = [this, State]()
		{
			TArray<FLspEvent> Events;
			Transport->Drain(Events, 256);
			for (FLspEvent& Event : Events)
			{
				if (Event.Kind == FLspEvent::EKind::Spawned)
				{
					State->Pid = Event.Pid;
				}
				else if (Event.Kind == FLspEvent::EKind::Exited)
				{
					State->bExited = true;
					State->ExitCode = Event.ExitCode;
					State->ExitedAt = FPlatformTime::Seconds();
				}
				else if (Event.Kind == FLspEvent::EKind::SpawnFailed || Event.Kind == FLspEvent::EKind::ProtocolError)
				{
					AddError(FString::Printf(TEXT("transport event: %s"), *Event.Text));
				}
				else if (Event.Kind == FLspEvent::EKind::Client)
				{
					if (Event.Lsp.Kind == dream::LspEventKind::StateChanged && Event.Lsp.State == dream::LspState::Ready)
					{
						State->bReady = true;
					}
					if (Event.Lsp.Kind == dream::LspEventKind::Diagnostics && Event.Lsp.Doc == 7)
					{
						State->bDiagnostics = true;
						State->Codes.Reset();
						for (const dream::LspDiagnostic& Diagnostic : Event.Lsp.Diagnostics)
						{
							State->Codes.Add(DreamText::Utf8ToFString(Diagnostic.Code));
						}
					}
				}
			}
		};

		DreamTest::FDreamSteps Steps(*this);
		Steps.Poll(TEXT("initialize answered"), [Pump, State]()
		{
			Pump();
			return State->bReady ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Do(TEXT("the child is in the kill-on-close job"), [this, State]()
		{
#if PLATFORM_WINDOWS
			FProcHandle Handle = FPlatformProcess::OpenProcess(State->Pid);
			TestTrue(TEXT("the server process opens"), Handle.IsValid());
			if (Handle.IsValid())
			{
				TestTrue(TEXT("the server is in Dream's job object (§7.1.3)"), FDreamChildJob::IsProcessInJob(Handle.Get()));
				FPlatformProcess::CloseProc(Handle);
			}
#endif
			return true;
		});
		Steps.Do(TEXT("open a document that calls input()"), [this]()
		{
			FLspCommand Open;
			Open.Kind = FLspCommand::EKind::Open;
			Open.Doc = 7;
			Open.Uri = dream::JsonRpc::PathToFileUri(DreamText::ToUtf8(FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) /
				TEXT("Dream/Scratch/transport-test.py")));
			Open.LanguageId = "python";
			Open.Text = dream::TextBuffer(u"name = input('Name? ')\n").Snapshot();
			Transport->Post(MoveTemp(Open));
			return true;
		});
		Steps.Poll(TEXT("SUM204 published"), [Pump, State]()
		{
			Pump();
			return State->bDiagnostics ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 15.0);
		Steps.Do(TEXT("check the publish and stop"), [this, State]()
		{
			TestTrue(*FString::Printf(TEXT("SUM204 in [%s]"), *FString::Join(State->Codes, TEXT(", "))), State->Codes.Contains(TEXT("SUM204")));
			State->StopAt = FPlatformTime::Seconds();
			Transport->RequestStop(FTimespan::FromMilliseconds(500.0));
			return true;
		});
		Steps.Poll(TEXT("the server exits"), [Pump, State]()
		{
			Pump();
			return State->bExited ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("check the exit"), [this, State]()
		{
			TestEqual(TEXT("shutdown then exit: exit code 0 (§7.1.5)"), State->ExitCode, 0);
			TestTrue(*FString::Printf(TEXT("stopped within 1.5 s (took %.2f s)"), State->ExitedAt - State->StopAt), State->ExitedAt - State->StopAt < 1.5);
			Transport->Join();
			Transport.Reset();
			const int64 LogBytes = IFileManager::Get().FileSize(*LogPath);
			TestTrue(*FString::Printf(TEXT("the server's stderr reached %s (TEL-01)"), *LogPath), LogBytes > 0);
			FString Log;
			FFileHelper::LoadFileToString(Log, *LogPath);
			TestTrue(TEXT("the log holds dream-check's own lines"), Log.Contains(TEXT("started (python")));
			IFileManager::Get().Delete(*LogPath, false, true, true);
			return true;
		});
		Steps.Run(Done);
	});
}

// ---- Dream.Language.EscapeCheck --------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageEscapeSpec, "Dream.Language.EscapeCheck", DreamTest::AnyEditorFlags)
	FProcHandle Child;
END_DEFINE_SPEC(FDreamLanguageEscapeSpec)

void FDreamLanguageEscapeSpec::Define()
{
	AfterEach([this]()
	{
		if (Child.IsValid())
		{
			FPlatformProcess::TerminateProc(Child, /*KillTree*/ true);
			FPlatformProcess::CloseProc(Child);
		}
	});

	It("counts only processes created at or after the server (the PID-reuse guard, lang-M1)", [this]()
	{
		TestFalse(TEXT("an older process is never the server's child"), FLspTransport::IsEscapeCandidate(100, 200));
		TestTrue(TEXT("a process created after the server is"), FLspTransport::IsEscapeCandidate(300, 200));
		TestTrue(TEXT("the same instant counts"), FLspTransport::IsEscapeCandidate(200, 200));
		TestFalse(TEXT("an unknown child time never counts"), FLspTransport::IsEscapeCandidate(0, 200));
		TestFalse(TEXT("an unknown server time never counts"), FLspTransport::IsEscapeCandidate(300, 0));
	});

#if PLATFORM_WINDOWS
	LatentIt("never lists a real process older than the server, though its recorded parent is the server's PID (lang-M1, TDD 7.1.3)",   // (tests) ASCII test name
		FTimespan::FromSeconds(60.0), [this](const FDoneDelegate& Done)
	{
		// The editor stands in for "the server": its child below records the editor's PID as its parent. Created after the
		// editor, it is a real escaped child (outside the job); for a server created after it (a reused PID's new owner) it
		// must not be. Nothing is terminated: the scan runs with bTerminate false and the spec ends the child itself.
		struct FCase
		{
			uint32 EditorPid = 0;
			uint64 EditorCreated = 0;
			uint32 ChildPid = 0;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a child process outside the job"), [this, C]()
		{
			if (!TestTrue(TEXT("the job exists"), FDreamChildJob::EnsureCreated()))
			{
				return false;
			}
			C->EditorPid = FPlatformProcess::GetCurrentProcessId();
			C->EditorCreated = FLspTransport::GetProcessCreationTime(C->EditorPid);
			const FString Python = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
			Child = FPlatformProcess::CreateProc(*Python, TEXT("-I -c \"import time; time.sleep(30)\""), /*bLaunchDetached*/ true,
				/*bLaunchHidden*/ true, /*bLaunchReallyHidden*/ true, &C->ChildPid, 0, nullptr, nullptr);
			return TestTrue(TEXT("the child started"), Child.IsValid() && C->ChildPid != 0) && TestTrue(TEXT("the editor's creation time"), C->EditorCreated != 0);
		});
		Steps.WaitFrames(10);
		Steps.Do(TEXT("scan for it as the editor's and as a newer server's child"), [this, C]()
		{
			const uint64 ChildCreated = FLspTransport::GetProcessCreationTime(C->ChildPid);
			TestTrue(TEXT("the child is newer than the editor"), ChildCreated > C->EditorCreated);
			TArray<FLspTransport::FEscapedChild> AsEditorChild;
			FLspTransport::ScanEscapedChildren(C->EditorPid, C->EditorCreated, /*bTerminate*/ false, AsEditorChild);
			TestTrue(TEXT("found as a child created after its parent (the scan sees real candidates)"),
				AsEditorChild.ContainsByPredicate([C](const FLspTransport::FEscapedChild& Found) { return Found.Pid == C->ChildPid; }));
			TArray<FLspTransport::FEscapedChild> AsOlderProcess;
			FLspTransport::ScanEscapedChildren(C->EditorPid, ChildCreated + 1, /*bTerminate*/ false, AsOlderProcess);
			TestFalse(TEXT("not listed for a server created after it (a reused PID never makes it a child)"),
				AsOlderProcess.ContainsByPredicate([C](const FLspTransport::FEscapedChild& Found) { return Found.Pid == C->ChildPid; }));
			TestTrue(TEXT("the child still runs"), FPlatformProcess::IsProcRunning(Child));
			return true;
		});
		Steps.Run(Done);
	});
#endif
}

// ---- Dream.Language.SnapshotGates ------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageSnapshotGatesSpec, "Dream.Language.SnapshotGates", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	TSharedPtr<SWindow> Modal;
	FString WalkDir;
	/** The REPL prompt's text before the spec typed into it (an unsent prompt restored from the session, ADD3-01). */
	TOptional<FString> SavedReplText;
END_DEFINE_SPEC(FDreamLanguageSnapshotGatesSpec)

void FDreamLanguageSnapshotGatesSpec::Define()
{
	using namespace DreamLanguageSpecLocal;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		if (Modal.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().RequestDestroyWindow(Modal.ToSharedRef());
		}
		Modal.Reset();
		if (FDreamLanguageServices* Lang = Services())
		{
			Lang->GetSnapshot().EndTestWalkForTest();
			Lang->GetSnapshot().SetSliceBudgetForTest(0);
			// The prompt goes back to what it held before the spec typed (review r2: an unsent prompt is journaled, ADD3-01).
			if (SavedReplText.IsSet() && FDreamEditorModule::IsAvailable())
			{
				const TSharedRef<FDreamBuffer> Repl = FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer();
				if (Repl->GetTextAsString() != SavedReplText.GetValue())
				{
					FDreamUndoHistory::ReplaceAllText(*Repl, SavedReplText.GetValue(), FText::GetEmpty());
				}
			}
		}
		if (WalkDir.Contains(TEXT("DreamTier4")))
		{
			IFileManager::Get().DeleteDirectory(*WalkDir, /*RequireExists*/ false, /*Tree*/ true);
		}
		SavedReplText.Reset();
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
	});

	LatentIt("runs no slice within 300 ms of a keystroke, while a modal is open or inside a run (T-STUB-028, lang-M2)",
		FTimespan::FromSeconds(900.0), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("x = 1\n"), TEXT("LangGates"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		WalkDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"),
			TEXT("langwalk-") + DreamTest::UniqueSuffix()));
		struct FWindow
		{
			int32 Count = 0;
			int32 Frames = 0;
			double LastKey = 0.0;
			int32 Edits = 0;
		};
		TSharedRef<FWindow> W = MakeShared<FWindow>();
		TSharedRef<DreamTest::FRunResult> Ticking = MakeShared<DreamTest::FRunResult>();
		auto Snap = []() -> FDreamStubSnapshot& { return Services()->GetSnapshot(); };
		// Each "held" window: the slice count must not move for 30 frames; then slices must resume (or the walk be done).
		auto HoldWindow = [this, W, Snap](DreamTest::FDreamSteps& Steps, const FString& Name, TFunction<void()> EachFrame)
		{
			Steps.Poll(Name, [this, W, Snap, Name, EachFrame]()
			{
				if (EachFrame)
				{
					EachFrame();
				}
				if (Snap().GetSliceCount() != W->Count)
				{
					AddError(FString::Printf(TEXT("%s: a slice ran (%d -> %d)"), *Name, W->Count, Snap().GetSliceCount()));
					return DreamTest::EStep::Fail;
				}
				return ++W->Frames >= 30 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
			}, 20.0);
		};
		auto Resume = [W, Snap](DreamTest::FDreamSteps& Steps, const FString& Name)
		{
			Steps.Poll(Name, [W, Snap]()
			{
				return Snap().GetSliceCount() > W->Count || Snap().IsComplete() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
			}, 20.0);
		};
		auto Type = [W](FDreamBuffer& Target)
		{
			W->LastKey = FPlatformTime::Seconds();
			FDreamUndoHistory::ReplaceAllText(Target, (++W->Edits % 2) ? TEXT("x = 2\n") : TEXT("x = 1\n"), FText::GetEmpty());
		};

		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(880.0);
		AddStartAndWaitReady(*this, Steps);
		Steps.Poll(TEXT("this session's snapshot is complete"), [Snap]()
		{
			return Snap().IsComplete() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 840.0);
		Steps.Do(TEXT("a fresh test walk with 1 ms slices"), [this, W, Snap]()
		{
			Snap().SetSliceBudgetForTest(1'000'000);
			W->Count = Snap().GetSliceCount();
			return TestTrue(TEXT("the test walk starts"), Snap().BeginTestWalkForTest(WalkDir));
		});
		Steps.Poll(TEXT("the walk slices"), [W, Snap]()
		{
			return Snap().GetSliceCount() >= W->Count + 3 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);

		// 1. Typing in a buffer.
		Steps.Do(TEXT("type in the buffer"), [W, Snap, Buffer, Type]()
		{
			Type(*Buffer);
			W->Count = Snap().GetSliceCount();
			W->Frames = 0;
			return true;
		});
		HoldWindow(Steps, TEXT("buffer keystrokes every frame"), [Buffer, Type]() { Type(*Buffer); });
		Resume(Steps, TEXT("slices resume once typing stops"));
		Steps.Do(TEXT("not before 300 ms"), [this, W, Snap]()
		{
			const TArray<double>& Times = Snap().GetSliceTimesForTest();
			if (!Snap().IsComplete() && Times.Num() > 0)
			{
				TestTrue(*FString::Printf(TEXT("the first slice came %.0f ms after the last keystroke"), (Times.Last() - W->LastKey) * 1000.0),
					Times.Last() - W->LastKey >= FDreamStubSnapshot::KeystrokeQuietSeconds - 0.001);
			}
			return true;
		});

		// 2. Typing in the REPL prompt (the review r1 minor: the prompt is no server document but is a Dream keystroke).
		Steps.Do(TEXT("type in the REPL prompt"), [this, W, Snap, Type]()
		{
			SavedReplText = FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->GetTextAsString();
			Type(*FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer());
			W->Count = Snap().GetSliceCount();
			W->Frames = 0;
			return true;
		});
		HoldWindow(Steps, TEXT("REPL keystrokes every frame"), [Type]() { Type(*FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()); });
		Resume(Steps, TEXT("slices resume after the REPL typing"));

		// 3. A modal window (added as a slow-task modal: it is the active modal without a nested loop or any input).
		Steps.Do(TEXT("open a modal"), [this, W, Snap]()
		{
			Modal = SNew(SWindow).Title(FText::FromString(TEXT("Dream Python Editor T-STUB-028"))).ClientSize(FVector2D(200.0, 100.0));
			FSlateApplication::Get().AddModalWindow(Modal.ToSharedRef(), nullptr, /*bSlowTaskWindow*/ true);
			W->Count = Snap().GetSliceCount();
			W->Frames = 0;
			return TestTrue(TEXT("it is the active modal"), FSlateApplication::Get().GetActiveModalWindow() == Modal);
		});
		HoldWindow(Steps, TEXT("a modal is open"), nullptr);
		Steps.Do(TEXT("close the modal"), [this]()
		{
			FSlateApplication::Get().RequestDestroyWindow(Modal.ToSharedRef());
			return true;
		});
		Steps.Poll(TEXT("the modal is gone"), [this]()
		{
			return FSlateApplication::Get().GetActiveModalWindow() != Modal ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("forget the modal"), [this]()
		{
			Modal.Reset();
			return true;
		});
		Resume(Steps, TEXT("slices resume after the modal"));

		// 4. A run that ticks the core ticker re-entrantly and pumps (RT-01): FDreamDeferral defers the snapshot ticker.
		DreamTest::AddRun(Steps, TEXT("a run that ticks the core ticker"), []()
		{
			return DreamTest::MakeReplRequest(TEXT("import _dream_test, time\nfor _i in range(15):\n    _dream_test.tick_core_ticker(2)\n    time.sleep(0.05)"));
		}, Ticking, 60.0);
		Steps.Do(TEXT("no slice inside the run"), [this, Ticking, Snap]()
		{
			DreamTest::ExpectOutcome(*this, *Ticking, EDreamRunOutcome::Ok);
			const double Enter = Ticking->Record.EnterSeconds;
			const double End = Ticking->Record.EndSeconds;
			int32 Inside = 0;
			for (const double At : Snap().GetSliceTimesForTest())
			{
				Inside += At > Enter && At < End ? 1 : 0;
			}
			TestTrue(TEXT("the run's times are known"), Enter > 0.0 && End > Enter);
			TestEqual(TEXT("slices between entry and the end of the run"), Inside, 0);
			TestEqual(TEXT("no slice passed a gate (T-STUB-028)"), Snap().GetGateViolationCount(), 0);
			return true;
		});
		Steps.Run(Done);
	});
}

// ---- Dream.Language.InstanceLogs -------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageInstanceLogsSpec, "Dream.Language.InstanceLogs", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamLanguageInstanceLogsSpec)

void FDreamLanguageInstanceLogsSpec::Define()
{
	LatentIt("writes a secondary instance's server log under <sec>/Logs/LSP and the primary's under <root>/Logs/LSP (TEL-01, TDD 10.5)",   // (tests) ASCII test name
		FTimespan::FromSeconds(90.0), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Base;
			FDreamStorageRoot Root;
			TSharedPtr<FDreamJournal> Primary;
			TSharedPtr<FDreamJournal> Secondary;
			FString SecondaryLog;
			TUniquePtr<FLspTransport> Transport;
			bool bReady = false;
			bool bExited = false;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Base = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"),
			TEXT("langlogs-") + DreamTest::UniqueSuffix()));
		C->Root.Root = C->Base / TEXT("5.8") / TEXT("DreamT4Lang-a00001");
		C->Root.ProjectHash = TEXT("DreamT4Lang-a00001");
		C->Root.BaseDir = C->Base;
		IFileManager::Get().MakeDirectory(*C->Root.Root, /*Tree*/ true);

		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a primary and a secondary journal on one root"), [this, C]()
		{
			EDreamInstanceRole Role = EDreamInstanceRole::Primary;
			C->Primary = MakeShared<FDreamJournal>();
			if (!TestTrue(TEXT("the primary opens"), C->Primary->Open(C->Root, Role)) || !TestTrue(TEXT("primary"), Role == EDreamInstanceRole::Primary))
			{
				return false;
			}
			C->Secondary = MakeShared<FDreamJournal>();
			C->Secondary->SetSecondaryIdForTest(TEXT("i-4343-2000"));
			if (!TestTrue(TEXT("the secondary opens"), C->Secondary->Open(C->Root, Role)) ||
				!TestTrue(TEXT("secondary"), Role == EDreamInstanceRole::Secondary))
			{
				return false;
			}
			const FString PrimaryLog = FDreamLanguageServices::MakeLogPath(*C->Primary, TEXT("dream-check"));
			C->SecondaryLog = FDreamLanguageServices::MakeLogPath(*C->Secondary, TEXT("dream-check"));
			TestEqual(TEXT("the primary logs under the root"), PrimaryLog, C->Root.Root / TEXT("Logs/LSP/dream-check.log"));
			TestEqual(TEXT("the secondary logs under <sec>"), C->SecondaryLog,
				C->Root.JournalDir(TEXT("i-4343-2000")) / TEXT("Logs/LSP/dream-check.log"));
			FDreamJournal Closed;
			TestTrue(TEXT("no path while the journal is not open"), FDreamLanguageServices::MakeLogPath(Closed, TEXT("dream-check")).IsEmpty());
			return true;
		});
		Steps.Do(TEXT("a dream-check child with the secondary's log path"), [this, C]()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor"));
			FDreamLaunchSpec Spec;
			Spec.ServerId = TEXT("dream-check");
			Spec.Executable = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
			Spec.Arguments = FString::Printf(TEXT("-I -B \"%s\" check --parent-pid %u"),
				*(FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()) / TEXT("Tools/Python/dream_launch.py")), FPlatformProcess::GetCurrentProcessId());
			Spec.StderrLogPath = C->SecondaryLog;
			Spec.ParentPid = FPlatformProcess::GetCurrentProcessId();
			dream::LspClientOptions Options;
			Options.ServerId = "dream-check";
			C->Transport = FLspTransport::Start(Spec, MoveTemp(Options));
			return true;
		});
		auto Pump = [C]()
		{
			TArray<FLspEvent> Events;
			C->Transport->Drain(Events, 256);
			for (const FLspEvent& Event : Events)
			{
				C->bReady |= Event.Kind == FLspEvent::EKind::Client && Event.Lsp.Kind == dream::LspEventKind::StateChanged &&
					Event.Lsp.State == dream::LspState::Ready;
				C->bExited |= Event.Kind == FLspEvent::EKind::Exited;
			}
		};
		Steps.Poll(TEXT("initialize answered"), [Pump, C]()
		{
			Pump();
			return C->bReady ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Do(TEXT("stop it"), [C]()
		{
			C->Transport->RequestStop(FTimespan::FromMilliseconds(500.0));
			return true;
		});
		Steps.Poll(TEXT("it exits"), [Pump, C]()
		{
			Pump();
			return C->bExited ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("the log is in <sec>/Logs/LSP"), [this, C]()
		{
			C->Transport->Join();
			C->Transport.Reset();
			FString Log;
			TestTrue(*FString::Printf(TEXT("%s holds dream-check's own lines"), *C->SecondaryLog),
				FFileHelper::LoadFileToString(Log, *C->SecondaryLog) && Log.Contains(TEXT("started (python")));
			TestFalse(TEXT("nothing under <root>/Logs/LSP"), FPaths::FileExists(C->Root.Root / TEXT("Logs/LSP/dream-check.log")));
			return true;
		});
		Steps.Finally(TEXT("close both journals and remove the private root"), [C]()
		{
			if (C->Transport.IsValid())
			{
				C->Transport->RequestStop(FTimespan::Zero());
				C->Transport->Join();
				C->Transport.Reset();
			}
			for (TSharedPtr<FDreamJournal>* Journal : { &C->Secondary, &C->Primary })
			{
				if (Journal->IsValid() && (*Journal)->IsOpen())
				{
					(*Journal)->Shutdown();
				}
				Journal->Reset();
			}
			if (C->Base.Contains(TEXT("DreamTier4")))
			{
				IFileManager::Get().DeleteDirectory(*C->Base, /*RequireExists*/ false, /*Tree*/ true);
			}
		});
		Steps.Run(Done);
	});
}

// ---- Dream.Language.Headless -----------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageHeadlessSpec, "Dream.Language.Headless", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamLanguageHeadlessSpec)

void FDreamLanguageHeadlessSpec::Define()
{
	It("never starts a server in a headless editor (D16, TDD 7.1.5)", [this]()   // (tests) ASCII test name
	{
		FDreamLanguageServices* Lang = DreamLanguageSpecLocal::Services();
		if (!TestNotNull(TEXT("language services"), Lang))
		{
			return;
		}
		if (FDreamEditorModule::Get().GetPython().GetState() == EDreamPythonState::Headless)
		{
			TestTrue(TEXT("disabled"), Lang->GetHealth() == EDreamLanguageHealth::Disabled);
			TestTrue(TEXT("no child process"), Lang->GetServerPid() == 0);
			Lang->OnDeferredStart();
			TestTrue(TEXT("a deferred start changes nothing"), Lang->GetHealth() == EDreamLanguageHealth::Disabled);
		}
		else
		{
			TestTrue(TEXT("an attended editor's services are not stopped"), Lang->GetHealth() != EDreamLanguageHealth::Stopped);
		}
	});
}

// ---- Dream.Language.Services -----------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageServicesSpec, "Dream.Language.Services", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamLanguageServicesSpec)

void FDreamLanguageServicesSpec::Define()
{
	using namespace DreamLanguageSpecLocal;

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

	LatentIt("starts dream-check after the boot, contained and logging, and lists its diagnostics until the tab closes (TDD 7.1, TDD 7.3, TDD 7.8)",   // (tests) ASCII test name
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("name = input('Name? ')\n"), TEXT("LangDiag"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		DreamTest::FDreamSteps Steps(*this);
		AddStartAndWaitReady(*this, Steps);
		Steps.Do(TEXT("the server is contained and logs"), [this]()
		{
			FDreamLanguageServices* Lang = Services();
			const uint32 Pid = Lang->GetServerPid();
			TestTrue(TEXT("a server process runs"), Pid != 0);
#if PLATFORM_WINDOWS
			FProcHandle Handle = FPlatformProcess::OpenProcess(Pid);
			if (TestTrue(TEXT("the server process opens"), Handle.IsValid()))
			{
				TestTrue(TEXT("it is in the kill-on-close job (§7.1.3)"), FDreamChildJob::IsProcessInJob(Handle.Get()));
				FPlatformProcess::CloseProc(Handle);
			}
#endif
			TestTrue(*FString::Printf(TEXT("the log %s exists (TEL-01)"), *Lang->GetLogPath()),
				!Lang->GetLogPath().IsEmpty() && IFileManager::Get().FileSize(*Lang->GetLogPath()) > 0);
			return true;
		});
		AddWaitForItem(*this, Steps, TEXT("SUM204 listed"), [Id]() { return Id; }, EDreamProblemSource::DreamCheck, TEXT("SUM204"), 0);
		Steps.Do(TEXT("the item is visible and closing the tab clears it"), [this, Id]()
		{
			FDreamLanguageServices* Lang = Services();
			const TArray<FDreamProblem> Visible = Lang->GetProblems().GetVisible();
			TestTrue(TEXT("Problems lists it"), Visible.ContainsByPredicate([Id](const FDreamProblem& Item)
			{
				return Item.Buffer == Id && Item.Code == TEXT("SUM204") && Item.Severity == EDreamSeverity::Warning;
			}));
			DreamTest::CloseBuffer(Id);
			BufferId.Invalidate();
			TestEqual(TEXT("document close clears its items (T-DIAG-015)"), Lang->GetProblems().GetItems(Id, EDreamProblemSource::DreamCheck).Num(), 0);
			TestFalse(TEXT("the document is closed at the server"), Lang->IsDocumentOpen(Id));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("applies no result while a run is active and applies it after the run (RT-01, T-LSP-020)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("x = 1\n"), TEXT("LangRunActive"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		struct FWatch
		{
			FDelegateHandle Handle;
			int32 ChangesDuringRun = 0;
			double FirstItemAt = 0.0;
		};
		TSharedRef<FWatch> Watch = MakeShared<FWatch>();
		TSharedRef<DreamTest::FRunResult> Sleeper = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		AddStartAndWaitReady(*this, Steps);
		Steps.Poll(TEXT("the document is open"), [Id]()
		{
			return Services()->IsDocumentOpen(Id) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.WaitFrames(30);
		Steps.Do(TEXT("watch the model, then edit in a SUM204 line"), [this, Buffer, Watch, Id]()
		{
			Watch->Handle = Services()->GetProblems().OnChanged().AddLambda([Watch, Id]()
			{
				if (FDreamEditorModule::Get().GetRunService().IsRunActive())
				{
					++Watch->ChangesDuringRun;
				}
				if (Watch->FirstItemAt == 0.0 && Find(Id, EDreamProblemSource::DreamCheck, TEXT("SUM204")).Num() > 0)
				{
					Watch->FirstItemAt = FPlatformTime::Seconds();
				}
			});
			return TestTrue(TEXT("the edit applies"), FDreamUndoHistory::ReplaceAllText(*Buffer, TEXT("x = 1\nname = input()\n"),
				FText::GetEmpty()));
		});
		// dream-check publishes about 300 ms after the edit, inside this 2 s run; the result must wait for its end.
		DreamTest::AddRun(Steps, TEXT("a 2 s run"), []() { return DreamTest::MakeReplRequest(TEXT("import time\ntime.sleep(2.0)")); },
			Sleeper, 60.0);
		AddWaitForItem(*this, Steps, TEXT("SUM204 listed after the run"), [Id]() { return Id; }, EDreamProblemSource::DreamCheck, TEXT("SUM204"), 1);
		Steps.Do(TEXT("check"), [this, Watch, Sleeper]()
		{
			DreamTest::ExpectOutcome(*this, *Sleeper, EDreamRunOutcome::Ok);
			TestEqual(TEXT("no Problems change while the run was active"), Watch->ChangesDuringRun, 0);
			TestTrue(TEXT("the item appeared after the run ended"), Watch->FirstItemAt >= Sleeper->Record.EndSeconds);
			return true;
		});
		Steps.Finally(TEXT("unwatch"), [Watch]()
		{
			if (FDreamLanguageServices* Lang = Services())
			{
				Lang->GetProblems().OnChanged().Remove(Watch->Handle);
			}
		});
		Steps.Run(Done);
	});

	LatentIt("restarts a killed server, greying its items until the new publish (TDD 7.3.5, T-DIAG-015, INT-11)", DreamTest::LatentTimeout(),   // (tests) ASCII test name
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("name = input()\n"), TEXT("LangCrash"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		TSharedRef<int32> RestartsBefore = MakeShared<int32>(0);
		TSharedRef<bool> bSawStale = MakeShared<bool>(false);
		DreamTest::FDreamSteps Steps(*this);
		AddStartAndWaitReady(*this, Steps);
		AddWaitForItem(*this, Steps, TEXT("SUM204 listed"), [Id]() { return Id; }, EDreamProblemSource::DreamCheck, TEXT("SUM204"), 0);
		Steps.Do(TEXT("kill the server"), [RestartsBefore]()
		{
			*RestartsBefore = Services()->GetRestartCount();
			Services()->KillServerForTest();
			return true;
		});
		Steps.Poll(TEXT("the crash is noticed"), [Id, bSawStale]()
		{
			const TArray<FDreamProblem> Items = Find(Id, EDreamProblemSource::DreamCheck, TEXT("SUM204"));
			*bSawStale = *bSawStale || (Items.Num() > 0 && Items[0].bStale);
			return Services()->GetHealth() != EDreamLanguageHealth::Ready ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("the item is stale"), [this, Id, bSawStale]()
		{
			const TArray<FDreamProblem> Items = Find(Id, EDreamProblemSource::DreamCheck, TEXT("SUM204"));
			TestTrue(TEXT("the old item stays, greyed (§7.3.5)"), *bSawStale || (Items.Num() > 0 && Items[0].bStale));
			TestTrue(*FString::Printf(TEXT("health says restarting (%s)"), HealthName(Services()->GetHealth())),
				Services()->GetHealth() == EDreamLanguageHealth::Restarting);
			return true;
		});
		Steps.Poll(TEXT("ready again"), []()
		{
			return Services()->GetHealth() == EDreamLanguageHealth::Ready ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		AddWaitForItem(*this, Steps, TEXT("a fresh SUM204"), [Id]() { return Id; }, EDreamProblemSource::DreamCheck, TEXT("SUM204"), 0);
		Steps.Do(TEXT("check the restart"), [this, RestartsBefore]()
		{
			TestTrue(TEXT("one more restart counted"), Services()->GetRestartCount() > *RestartsBefore);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("stops in safe mode and starts again when safe mode is left (T-LSP-028, TDD 10.8)", DreamTest::LatentTimeout(),   // (tests) ASCII test name
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		DreamTest::FDreamSteps Steps(*this);
		AddStartAndWaitReady(*this, Steps);
		Steps.Do(TEXT("enter safe mode"), [this]()
		{
			FDreamEditorModule::Get().GetRestoreGuard().EnterSafeMode(EDreamSafeModeReason::Switch);
			TestTrue(TEXT("disabled in safe mode"), Services()->GetHealth() == EDreamLanguageHealth::Disabled);
			TestTrue(TEXT("no server process"), Services()->GetServerPid() == 0);
			return true;
		});
		Steps.Do(TEXT("leave safe mode"), []()
		{
			FDreamEditorModule::Get().GetRestoreGuard().LeaveSafeMode();
			return true;
		});
		Steps.Poll(TEXT("ready again"), []()
		{
			return Services()->GetHealth() == EDreamLanguageHealth::Ready ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Finally(TEXT("never leave the profile in safe mode"), []()
		{
			if (FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetRestoreGuard().IsSafeMode())
			{
				FDreamEditorModule::Get().GetRestoreGuard().LeaveSafeMode();
			}
		});
		Steps.Run(Done);
	});
}

// ---- Dream.Language.Problems -----------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageProblemsSpec, "Dream.Language.Problems", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamLanguageProblemsSpec)

void FDreamLanguageProblemsSpec::Define()
{
	using namespace DreamLanguageSpecLocal;

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

	LatentIt("adds a Parse item for a not executed run and clears it when its line is edited (TDD 7.8.1, RUN-16)", DreamTest::LatentTimeout(),   // (tests) ASCII test name
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Source = FString::Printf(TEXT("a_%s = 1\nbroken_%s = (2,\n"), *Sfx, *Sfx);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Source, TEXT("LangParse"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		const FString Filename = DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 1).Filename;
		const FString Summary = TEXT("SyntaxError: '(' was never closed");
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		DreamTest::ExpectPythonErrorBlock(*this, Filename, Summary, { FString::Printf(TEXT("broken_%s"), *Sfx) });
		TSharedRef<DreamTest::FRunResult> Broken = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("the broken buffer"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 3); },
			Broken, 60.0);
		Steps.Do(TEXT("the Parse item is at the error line"), [this, Broken, Id]()
		{
			DreamTest::ExpectOutcome(*this, *Broken, EDreamRunOutcome::NotExecuted);
			const TArray<FDreamProblem> Items = Services()->GetProblems().GetItems(Id, EDreamProblemSource::Parse);
			if (TestEqual(TEXT("one Parse item"), Items.Num(), 1))
			{
				TestEqual(TEXT("at the SyntaxError's line"), Items[0].StartLine + 1, Broken->Record.Location.Line);
				TestEqual(TEXT("dedup class syntax"), Items[0].DedupClass, FString(TEXT("syntax")));
				TestTrue(TEXT("an error"), Items[0].Severity == EDreamSeverity::Error);
			}
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, Summary);
		Steps.Do(TEXT("edit the line"), [this, Buffer, Id, Sfx]()
		{
			TestTrue(TEXT("the edit applies"), FDreamUndoHistory::ReplaceAllText(*Buffer,
				FString::Printf(TEXT("a_%s = 1\nbroken_%s = (2,)\n"), *Sfx, *Sfx), FText::GetEmpty()));
			TestEqual(TEXT("an edit of the line clears the Parse item"), Services()->GetProblems().GetItems(Id, EDreamProblemSource::Parse).Num(), 0);
			return true;
		});
		Steps.Finally(TEXT("release the log capture"), [Log]() {});
		Steps.Run(Done);
	});

	LatentIt("adds a runtime item at the raising buffer line and walks lint and runtime items with F8 / Shift+F8 (T-DIAG-016, T-DIAG-017)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		// Line 0: SUM204 (not executed: input() is in a function); line 3: the raise.
		const FString Source = FString::Printf(TEXT("def ask_%s():\n    return input()\nvalue_%s = 1\nraise ValueError('dream-lang-%s')\n"),
			*Sfx, *Sfx, *Sfx);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Source, TEXT("LangWalk"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		const FString Summary = FString::Printf(TEXT("ValueError: dream-lang-%s"), *Sfx);
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		const FString Filename = DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 1).Filename;
		DreamTest::ExpectPythonErrorBlock(*this, Filename, Summary, { FString::Printf(TEXT("dream-lang-%s"), *Sfx) });
		TSharedRef<DreamTest::FRunResult> Raising = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		AddStartAndWaitReady(*this, Steps);
		AddWaitForItem(*this, Steps, TEXT("SUM204 listed"), [Id]() { return Id; }, EDreamProblemSource::DreamCheck, TEXT("SUM204"), 1);
		DreamTest::AddRun(Steps, TEXT("the raising buffer"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 4); },
			Raising, 60.0);
		DreamTest::AddWaitForForwardedError(Steps, Log, Summary);
		Steps.Do(TEXT("the runtime item is at line 4"), [this, Raising, Id]()
		{
			DreamTest::ExpectOutcome(*this, *Raising, EDreamRunOutcome::Error);
			const TArray<FDreamProblem> Visible = Services()->GetProblems().GetVisible();
			const FDreamProblem* Runtime = Visible.FindByPredicate([Id](const FDreamProblem& Item)
			{
				return Item.Buffer == Id && Item.Source == EDreamProblemSource::Runtime;
			});
			if (TestNotNull(TEXT("a runtime item in the buffer (T-DIAG-016)"), Runtime))
			{
				TestEqual(TEXT("at the raising line"), Runtime->StartLine, 3);
				TestEqual(TEXT("code = exception type"), Runtime->Code, FString(TEXT("ValueError")));
			}
			return true;
		});
		Steps.Do(TEXT("F8 / Shift+F8 walk"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the code view"), View.IsValid()))
			{
				return false;
			}
			Tab->ActivateBuffer(Id);
			View->SetCaret(0, 0);
			Tab->ExecuteDreamCommand(EDreamCommand::NextProblem);
			TestEqual(TEXT("F8 goes to the SUM204 line"), View->GetSelection().CaretLine, 1);
			Tab->ExecuteDreamCommand(EDreamCommand::NextProblem);
			TestEqual(TEXT("F8 again goes to the runtime item"), View->GetSelection().CaretLine, 3);
			Tab->ExecuteDreamCommand(EDreamCommand::PrevProblem);
			TestEqual(TEXT("Shift+F8 comes back"), View->GetSelection().CaretLine, 1);
			return true;
		});
		Steps.Do(TEXT("the next run of the buffer clears its runtime item"), [this, Id]()
		{
			// §7.8.1: the next run from the same source clears it; Problems > Clear runtime items does too.
			Services()->GetProblems().ClearRuntimeItems();
			const TArray<FDreamProblem> Visible = Services()->GetProblems().GetVisible();
			TestFalse(TEXT("Clear runtime items removed it"), Visible.ContainsByPredicate([Id](const FDreamProblem& Item)
			{
				return Item.Buffer == Id && Item.Source == EDreamProblemSource::Runtime;
			}));
			return true;
		});
		Steps.Finally(TEXT("release the log capture"), [Log]() {});
		Steps.Run(Done);
	});

	LatentIt("applies a quick fix as one undo step (TDD 7.8.3, T-DIAG-021, EDT-08)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)   // (tests) ASCII test name
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Source = TEXT("name = input()\nprint(name)\n");
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Source, TEXT("LangFix"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		struct FFix
		{
			bool bAnswered = false;
			TArray<dream::LspCodeAction> Actions;
		};
		TSharedRef<FFix> Fix = MakeShared<FFix>();
		DreamTest::FDreamSteps Steps(*this);
		AddStartAndWaitReady(*this, Steps);
		AddWaitForItem(*this, Steps, TEXT("SUM204 listed"), [Id]() { return Id; }, EDreamProblemSource::DreamCheck, TEXT("SUM204"), 0);
		Steps.Do(TEXT("ask for the code actions"), [this, Id, Fix]()
		{
			dream::LspRange Range;
			Range.EndCol = 14;
			return TestTrue(TEXT("the request is sent"), Services()->RequestCodeActions(Id, Range, [Fix](TArray<dream::LspCodeAction>&& Actions)
			{
				Fix->bAnswered = true;
				Fix->Actions = MoveTemp(Actions);
			}));
		});
		Steps.Poll(TEXT("the answer"), [Fix]() { return Fix->bAnswered ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("apply Suppress, then undo it in one step"), [this, Id, Fix, Source]()
		{
			const dream::LspCodeAction* Suppress = Fix->Actions.FindByPredicate([](const dream::LspCodeAction& Action)
			{
				return Action.Title == "Suppress SUM204 on this line";
			});
			if (!TestNotNull(TEXT("the Suppress SUM204 action"), Suppress))
			{
				return false;
			}
			FText Error;
			if (!TestTrue(TEXT("the fix applies"), Services()->ApplyCodeAction(*Suppress, Error)))
			{
				AddError(Error.ToString());
				return false;
			}
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			const FString Fixed = View.IsValid() ? View->GetText() : FString();
			TestTrue(*FString::Printf(TEXT("the line carries the noqa comment: \"%s\""), *Line(Fixed, 0)), Line(Fixed, 0).Contains(TEXT("# noqa: SUM204")));
			if (TestTrue(TEXT("one undo step exists"), View.IsValid() && View->CanUndo()))
			{
				View->Undo();
				TestEqual(TEXT("one Ctrl+Z restores the text"), View->GetText(), Source);
			}
			return true;
		});
		Steps.Run(Done);
	});
}

// ---- Dream.Language.Snapshot -----------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageSnapshotSpec, "Dream.Language.Snapshot", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamLanguageSnapshotSpec)

void FDreamLanguageSnapshotSpec::Define()
{
	using namespace DreamLanguageSpecLocal;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
		if (FDreamLanguageServices* Lang = Services())
		{
			Lang->GetSnapshot().SetSliceBudgetForTest(0);
		}
	});

	LatentIt("builds the snapshot in gated idle slices, then SUM101 fires on unreal.EditorAssetLibary (T-STUB-024, T-STUB-028)",
		FTimespan::FromSeconds(900.0), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("import unreal\nlib = unreal.EditorAssetLibary\n"),
			TEXT("LangSnap"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(880.0);
		AddStartAndWaitReady(*this, Steps);
		Steps.Do(TEXT("a larger slice budget for this spec"), []()
		{
			Services()->GetSnapshot().SetSliceBudgetForTest(40'000'000);
			return true;
		});
		Steps.Poll(TEXT("the snapshot completes"), [this]()
		{
			FDreamStubSnapshot& Snapshot = Services()->GetSnapshot();
			if (!Snapshot.GetLastError().IsEmpty())
			{
				AddError(FString::Printf(TEXT("the walk failed: %s"), *Snapshot.GetLastError()));
				return DreamTest::EStep::Fail;
			}
			return Snapshot.IsComplete() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 840.0);
		Steps.Do(TEXT("check the files"), [this]()
		{
			FDreamStubSnapshot& Snapshot = Services()->GetSnapshot();
			const FString Dir = Snapshot.GetSnapshotDir();
			TestTrue(TEXT("manifest.json written"), FPaths::FileExists(Dir / TEXT("manifest.json")));
			TestTrue(TEXT(".complete written"), FPaths::FileExists(Dir / TEXT(".complete")));
			TestTrue(TEXT("the unreal dict's records"), FPaths::FileExists(Dir / TEXT("modules/__unreal__.jsonl")));
			TestTrue(TEXT("the engine module's records"), FPaths::FileExists(Dir / TEXT("modules/engine.jsonl")));
			TestFalse(TEXT("no progress file left"), FPaths::FileExists(Dir / TEXT("progress.json")));
			TestTrue(TEXT("the cache sits under Intermediate/Dream/Stubs/<key> (§7.4.3)"),
				Dir.StartsWith(FDreamStubSnapshot::GetStubsRoot() / Snapshot.GetKey()));
			TestEqual(TEXT("the key has 16 hex digits"), Snapshot.GetKey().Len(), 16);
			TestEqual(TEXT("no slice ran past its gates (T-STUB-028)"), Snapshot.GetGateViolationCount(), 0);
			AddInfo(FString::Printf(TEXT("snapshot %s: %d slices this session, longest %.2f ms"), *Snapshot.GetKey(), Snapshot.GetSliceCount(),
				Snapshot.GetMaxSliceSeconds() * 1000.0));
			return true;
		});
		AddWaitForItem(*this, Steps, TEXT("SUM101 on the misspelt name"), [Id]() { return Id; }, EDreamProblemSource::DreamCheck, TEXT("SUM101"), 1, 30.0);
		Steps.Do(TEXT("no SUM900 once the data is loaded"), [this, Id]()
		{
			TestEqual(TEXT("SUM900 gone"), Find(Id, EDreamProblemSource::DreamCheck, TEXT("SUM900")).Num(), 0);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("rewrites an edit-only attribute assignment with set_editor_property as one undo step (T-DIAG-002, T-DIAG-003)",
		FTimespan::FromSeconds(900.0), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Source = TEXT("import unreal\nstatic_mesh: unreal.StaticMesh = None\nstatic_mesh.lod_group = \"LargeProp\"\n");
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Source, TEXT("LangSum102"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		struct FFix
		{
			bool bAnswered = false;
			TArray<dream::LspCodeAction> Actions;
		};
		TSharedRef<FFix> Fix = MakeShared<FFix>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(880.0);
		AddStartAndWaitReady(*this, Steps);
		Steps.Do(TEXT("a larger slice budget for this spec"), []()
		{
			Services()->GetSnapshot().SetSliceBudgetForTest(40'000'000);
			return true;
		});
		Steps.Poll(TEXT("the snapshot is complete"), []()
		{
			return Services()->GetSnapshot().IsComplete() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 840.0);
		AddWaitForItem(*this, Steps, TEXT("SUM102 on the assignment"), [Id]() { return Id; }, EDreamProblemSource::DreamCheck, TEXT("SUM102"), 2, 30.0);
		Steps.Do(TEXT("ask for the fixes"), [this, Id, Fix]()
		{
			const TArray<FDreamProblem> Items = Find(Id, EDreamProblemSource::DreamCheck, TEXT("SUM102"), 2);
			TestTrue(TEXT("the item offers a fix"), Items.Num() > 0 && Items[0].bHasQuickFix);
			TestTrue(*FString::Printf(TEXT("the edit-only message: %s"), Items.Num() > 0 ? *Items[0].Message : TEXT("")),
				Items.Num() > 0 && Items[0].Message.Contains(TEXT("Use set_editor_property")));
			dream::LspRange Range;
			Range.StartLine = Range.EndLine = 2;
			Range.EndCol = 35;
			return TestTrue(TEXT("the request is sent"), Services()->RequestCodeActions(Id, Range, [Fix](TArray<dream::LspCodeAction>&& Actions)
			{
				Fix->bAnswered = true;
				Fix->Actions = MoveTemp(Actions);
			}));
		});
		Steps.Poll(TEXT("the answer"), [Fix]() { return Fix->bAnswered ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("apply the preferred fix, then undo it in one step"), [this, Id, Fix, Source]()
		{
			const dream::LspCodeAction* Preferred = Fix->Actions.FindByPredicate([](const dream::LspCodeAction& Action) { return Action.bPreferred; });
			if (!TestNotNull(TEXT("a preferred SUM102 fix"), Preferred))
			{
				return false;
			}
			FText Error;
			if (!TestTrue(TEXT("the fix applies"), Services()->ApplyCodeAction(*Preferred, Error)))
			{
				AddError(Error.ToString());
				return false;
			}
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			const FString Fixed = View.IsValid() ? View->GetText() : FString();
			TestEqual(TEXT("the assignment is rewritten"), Line(Fixed, 2), FString(TEXT("static_mesh.set_editor_property(\"lod_group\", \"LargeProp\")")));
			if (TestTrue(TEXT("one undo step exists"), View.IsValid() && View->CanUndo()))
			{
				View->Undo();
				TestEqual(TEXT("one Ctrl+Z restores the text"), View->GetText(), Source);
			}
			return true;
		});
		Steps.Run(Done);
	});
}
