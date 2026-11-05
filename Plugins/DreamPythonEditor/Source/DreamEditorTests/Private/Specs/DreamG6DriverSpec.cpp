// SPDX-License-Identifier: Apache-2.0
// G6 driver B prototype (TDD §10.10, T-PER-036, T-E2E-010): one editor session of DreamHost/Scripts/run_g6.ps1. The
// driver starts the DreamHost editor once per step with
//   -ExecCmds="Dream.Test.G6Step" -DreamG6Step=<k> -DreamG6Kills=<n> -DreamG6RunStep=<r> -DreamG6Dir=<dir>
//   -DreamStorageRoot=<a private folder outside the project>
// (the user's own DreamHost journal and storage settings are never touched) and kills the editor with TerminateProcess
// (kill -9) while step k <= n types, or, at step r, while a long run executes. The console command, not an automation
// test, runs the step: the automation framework's tier-4 pass hygiene discards the pending recoveries a pass starts with,
// which are exactly what this driver checks. Each step:
//  1. opens the Dream tab: FDreamJournal::Open, SDreamTab::RestoreSession and the recovery scan, as a user's start does;
//  2. from step 2 on, checks the previous step's buffer against its sidecar and kill time and writes verify<k>.txt:
//     I1 the journal was read and the session restored, I3 the buffer is listed (pending under "Not restored yet"),
//     I2/I4 its recovered text equals exactly one typed state that was current within 2 s of the kill, and after the run
//     step the run is reported lost (I5, §10.7.4); then Restores it (one undoable edit, PER-03) and closes it;
//  3. while k <= n: opens a scratch tab "DreamG6-<k>" and types into it through FSlateApplication::ProcessKeyCharEvent at
//     about 10 keys/s, writing the expected text and time to sidecar<k>.txt and flushing it (FlushFileBuffers) before
//     every keystroke, and signals typing<k>.txt; at step r it then starts a 90 s REPL run whose first statement writes
//     run<k>.txt, so the kill lands inside the run;
//  4. at step n + 1 it only verifies, writes final.txt and asks for a normal exit.
// Dream.Persistence.G6Driver, in the shared tier-4 pass, only says how to run it.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Persistence/DreamInstances.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamRestoreGuard.h"
#include "Persistence/DreamWin32FileOps.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"

namespace DreamG6Driver
{
	/** The typed text: words, spaces, '=' and line breaks only, so smart indent and auto-close never change what is typed. */
	const TCHAR* Corpus = TEXT("alpha = 1\nbeta = alpha plus 2\ngamma = beta times 3\ndelta = gamma minus 4\nepsilon = 5\n");

	int64 NowUtcMs()
	{
		return static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds());
	}

	FString Escape(const FString& Text)
	{
		FString Out;
		for (const TCHAR Ch : Text)
		{
			if (Ch == TEXT('\\')) { Out += TEXT("\\\\"); }
			else if (Ch == TEXT('\n')) { Out += TEXT("\\n"); }
			else if (Ch == TEXT('\t')) { Out += TEXT("\\t"); }
			else { Out.AppendChar(Ch); }
		}
		return Out;
	}

	FString Unescape(const FString& Text)
	{
		FString Out;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			if (Text[Index] == TEXT('\\') && Index + 1 < Text.Len())
			{
				const TCHAR Next = Text[++Index];
				Out.AppendChar(Next == TEXT('n') ? TEXT('\n') : (Next == TEXT('t') ? TEXT('\t') : Next));
			}
			else
			{
				Out.AppendChar(Text[Index]);
			}
		}
		return Out;
	}

	struct FState
	{
		int64 UtcMs = 0;
		FString Text;
	};

	/** sidecar<k>.txt: "<utc ms>\t<buffer id>\t<escaped text>" per line, the expected text before each keystroke. */
	bool ReadSidecar(const FString& Path, FGuid& OutBuffer, TArray<FState>& OutStates)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *Path))
		{
			return false;
		}
		for (const FString& Line : Lines)
		{
			TArray<FString> Parts;
			Line.ParseIntoArray(Parts, TEXT("\t"), /*bCullEmpty*/ false);
			FGuid Id;
			if (Parts.Num() < 3 || !FGuid::Parse(Parts[1], Id))
			{
				continue;   // a torn last line (the kill landed in the write): that keystroke never happened
			}
			OutBuffer = Id;
			FState& State = OutStates.AddDefaulted_GetRef();
			LexFromString(State.UtcMs, *Parts[0]);
			State.Text = Unescape(Parts[2]);
		}
		return OutStates.Num() > 0;
	}

	/** One driver step, ticked from the core ticker; it lives until the process ends (the driver kills it or it exits). */
	class FStep
	{
	public:
		int32 Step = 0;
		int32 Kills = 0;
		int32 RunStep = 0;
		FString Dir;

		void Start()
		{
			StartedAt = FPlatformTime::Seconds();
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float) { return Tick(); }), 0.f);
		}

	private:
		enum class EPhase : uint8 { OpenTab, WaitRestore, Verify, WaitGuard, Typing, Done };

		EPhase Phase = EPhase::OpenTab;
		double StartedAt = 0.0;
		double PhaseAt = 0.0;
		/** Weak: a tab this object kept alive past Slate's shutdown would rebuild its banners at Python's exit. */
		TWeakPtr<SDreamTab> Tab;
		TArray<FString> Errors;
		TArray<FString> Notes;
		TSharedPtr<FDreamWin32FileOps> SidecarOps;
		uint64 SidecarHandle = 0;
		TSharedPtr<FDreamBuffer> Typed;
		int32 Cursor = 0;
		int32 Keystrokes = 0;
		int32 Fallbacks = 0;
		int32 Mismatches = 0;
		double NextKey = 0.0;
		bool bRunSubmitted = false;
		bool bRootChecked = false;

		void Fail(const FString& Error)
		{
			Errors.Add(Error);
			UE_LOG(LogDream, Warning, TEXT("G6 driver step %d: %s"), Step, *Error);
		}

		void Check(bool bOk, const FString& What)
		{
			if (!bOk)
			{
				Fail(What);
			}
		}

		void WriteResult(const FString& Name)
		{
			TArray<FString> Lines;
			Lines.Add(FString::Printf(TEXT("ok=%d"), Errors.Num() == 0 ? 1 : 0));
			Lines.Append(Notes);
			for (const FString& Error : Errors)
			{
				Lines.Add(TEXT("error=") + Error);
			}
			FFileHelper::SaveStringArrayToFile(Lines, *(Dir / Name));
		}

		void RequestExit()
		{
			Typed.Reset();
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				FPlatformMisc::RequestExit(/*Force*/ false, TEXT("Dream.Test.G6Step"));
				return false;
			}), 1.0f);
		}

		bool Tick()
		{
			if (!FDreamEditorModule::IsAvailable())
			{
				return false;
			}
			FDreamEditorModule& M = FDreamEditorModule::Get();
			const double Now = FPlatformTime::Seconds();
			switch (Phase)
			{
			case EPhase::OpenTab:
				if (!bRootChecked)
				{
					// (review r1, persist-M3) Before the tab opens the journal: the root must be the driver's own, under
					// -DreamStorageRoot; otherwise this session never types or waits to be killed (the user's profile).
					bRootChecked = true;
					FString Expected;
					FParse::Value(FCommandLine::Get(), TEXT("DreamStorageRoot="), Expected);
					Expected = FPaths::ConvertRelativePathToFull(Expected);
					FPaths::NormalizeDirectoryName(Expected);
					const TOptional<FDreamStorageRoot>& Root = M.GetInstances().GetResolvedRoot();
					const bool bOwnRoot = Root.IsSet() && Root->bIsOverride && !Expected.IsEmpty() && FPaths::IsUnderDirectory(Root->Root, Expected);
					Notes.Add(TEXT("resolved_root=") + (Root.IsSet() ? Root->Root : FString(TEXT("none"))));
					if (!bOwnRoot)
					{
						Fail(FString::Printf(TEXT("the storage root is not under -DreamStorageRoot=%s; nothing is typed"), *Expected));
						WriteResult(FString::Printf(TEXT("verify%d.txt"), Step));
						WriteResult(FString::Printf(TEXT("notkilled%d.txt"), Step));
						RequestExit();
						Phase = EPhase::Done;
						return false;
					}
				}
				Tab = M.InvokeTab();
				if (!Tab.Pin().IsValid())
				{
					if (Now - StartedAt > 60.0)
					{
						Fail(TEXT("the Dream tab did not open"));
						WriteResult(FString::Printf(TEXT("verify%d.txt"), Step));
						Phase = EPhase::Done;
					}
					return true;
				}
				Phase = EPhase::WaitRestore;
				PhaseAt = Now;
				return true;
			case EPhase::WaitRestore:
				if (!M.GetJournal().IsOpen())
				{
					Fail(TEXT("the journal did not open on the driver's storage root"));
					WriteResult(FString::Printf(TEXT("verify%d.txt"), Step));
					Phase = EPhase::Done;
					return false;
				}
				if (!M.GetJournal().IsReconciled() || Now - PhaseAt < 0.5)
				{
					return true;   // the restore and its reconcile; a few frames more so the tab is laid out
				}
				Phase = EPhase::Verify;
				return true;
			case EPhase::Verify:
			{
				Notes.Add(TEXT("root=") + M.GetJournal().GetStorageRoot().Root);
				Check(!M.GetRestoreGuard().IsSafeMode(), TEXT("the start is in safe mode (the restore guard counted a kill inside the restore window)"));
				if (Step > 1)
				{
					VerifyPrevious(M);
				}
				WriteResult(FString::Printf(TEXT("verify%d.txt"), Step));
				if (Step > Kills)
				{
					WriteResult(TEXT("final.txt"));
					RequestExit();
					Phase = EPhase::Done;
					return false;
				}
				Phase = EPhase::WaitGuard;
				PhaseAt = Now;
				return true;
			}
			case EPhase::WaitGuard:
				// §10.8: a kill inside the restore window counts toward safe mode (two in a row: the next start is in safe
				// mode, T-PER-034). The driver kills while typing, so it types only once the guard has cleared, 10 s after
				// the deferred import with every tab restored.
				if (M.GetRestoreGuard().GetPending() != 0 && Now - PhaseAt < 90.0)
				{
					return true;
				}
				Check(M.GetRestoreGuard().GetPending() == 0, TEXT("the restore guard did not clear within 90 s"));
				if (!Errors.IsEmpty() || !OpenTyping())
				{
					WriteResult(FString::Printf(TEXT("notkilled%d.txt"), Step));
					RequestExit();
					Phase = EPhase::Done;
					return false;
				}
				Phase = EPhase::Typing;
				PhaseAt = Now;
				NextKey = Now + 0.2;
				return true;
			case EPhase::Typing:
				if (Now - PhaseAt > 240.0)
				{
					Fail(TEXT("the driver did not kill the editor"));
					WriteResult(FString::Printf(TEXT("notkilled%d.txt"), Step));
					RequestExit();
					Phase = EPhase::Done;
					return false;
				}
				if (Now < NextKey || bRunSubmitted)
				{
					return true;
				}
				NextKey = Now + 0.1;   // about 10 keys/s (§10.10: 8-15)
				if (Step == RunStep && Keystrokes >= 25)
				{
					SubmitLongRun(M);
					return true;
				}
				TypeOne();
				return true;
			default:
				return false;
			}
		}

		void VerifyPrevious(FDreamEditorModule& M)
		{
			const int32 Previous = Step - 1;
			FGuid Id;
			TArray<FState> States;
			if (!ReadSidecar(Dir / FString::Printf(TEXT("sidecar%d.txt"), Previous), Id, States))
			{
				Fail(TEXT("I3: the previous step left no sidecar"));
				return;
			}
			FString KillText;
			int64 KillMs = 0;
			if (FFileHelper::LoadFileToString(KillText, *(Dir / FString::Printf(TEXT("kill%d.txt"), Previous))))
			{
				LexFromString(KillMs, *KillText.TrimStartAndEnd());
			}
			if (KillMs <= 0)
			{
				Fail(TEXT("the driver recorded no kill time"));
				return;
			}
			Check(M.GetJournal().IsOpen() && M.GetJournal().IsReconciled(), TEXT("I1: the journal was read and the session restored"));
			TSharedPtr<FDreamBuffer> Buffer = M.GetBufferManager().FindById(Id);
			if (!Buffer.IsValid())
			{
				Fail(TEXT("I3: the typed buffer is not listed after the kill"));
				return;
			}
			const bool bPending = Buffer->HasPendingRecovery();
			const TSharedPtr<SDreamTab> Live = Tab.Pin();
			const bool bListed = Live.IsValid() && Live->GetPendingRecoveries().Contains(Id);
			const FString Recovered = bPending ? Buffer->GetPendingRecoveryText().GetValue() : Buffer->GetTextAsString();
			// I2 and I4: exactly one typed state, current at some t with kill - t <= 2 s.
			int32 Match = INDEX_NONE;
			for (int32 Index = States.Num() - 1; Index >= 0; --Index)
			{
				if (States[Index].Text == Recovered)
				{
					Match = Index;
					break;
				}
			}
			const bool bCurrentNearKill = Match != INDEX_NONE && (Match == States.Num() - 1 || States[Match + 1].UtcMs > KillMs - 2000);
			const int64 LostMs = Match == INDEX_NONE ? -1 : (Match == States.Num() - 1 ? 0 : FMath::Max<int64>(0, KillMs - States[Match + 1].UtcMs));
			Notes.Add(FString::Printf(TEXT("states=%d"), States.Num()));
			Notes.Add(FString::Printf(TEXT("recovered_chars=%d"), Recovered.Len()));
			Notes.Add(FString::Printf(TEXT("typed_chars=%d"), States.Last().Text.Len()));
			Notes.Add(FString::Printf(TEXT("lost_ms=%lld"), LostMs));
			Notes.Add(FString::Printf(TEXT("pending=%d"), bPending ? 1 : 0));
			Check(bPending && bListed, TEXT("I3: the crashed session's buffer waits under \"Not restored yet\" (PER-03)"));
			Check(Match != INDEX_NONE, TEXT("I4: the recovered text equals none of the typed states"));
			Check(bCurrentNearKill, FString::Printf(TEXT("I2: more than 2 s of typing lost (%lld ms)"), LostMs));
			if (Previous == RunStep)
			{
				bool bLost = false;
				for (const FDreamRunRecord& Record : M.GetRunService().GetHistory())
				{
					bLost = bLost || Record.Outcome == EDreamRunOutcome::Lost;
				}
				Notes.Add(FString::Printf(TEXT("lost_run=%d"), bLost ? 1 : 0));
				Check(bLost, TEXT("I5: the run executing at the kill is not reported lost"));
			}
			if (bPending && Live.IsValid())
			{
				Live->RestoreRecovered(Id, false);   // PER-03: one undoable edit, dirty, nothing written to disk
				Check(!Buffer->HasPendingRecovery() && Buffer->GetTextAsString() == Recovered, TEXT("Restore did not apply the recovered text"));
			}
			M.GetBufferManager().Close(Id);
		}

		bool OpenTyping()
		{
			const TSharedPtr<SDreamTab> Live = Tab.Pin();
			if (!Live.IsValid())
			{
				Fail(TEXT("the Dream tab closed"));
				return false;
			}
			Typed = Live->NewScratchTab(FString(), FString::Printf(TEXT("DreamG6-%d"), Step));
			if (TSharedPtr<SDreamCodeView> View = Live->GetActiveCodeView())
			{
				View->FocusEditor();
			}
			SidecarOps = MakeShared<FDreamWin32FileOps>();
			const std::string Path = FDreamWin32FileOps::ToUtf8Path(Dir / FString::Printf(TEXT("sidecar%d.txt"), Step));
			SidecarOps->Remove(Path);
			if (!SidecarOps->OpenAppend(Path, SidecarHandle))
			{
				Fail(TEXT("the sidecar could not be opened"));
				return false;
			}
			WriteStatus();
			return true;
		}

		void WriteStatus()
		{
			FFileHelper::SaveStringArrayToFile({ TEXT("typing"), FString::Printf(TEXT("keystrokes=%d"), Keystrokes),
				FString::Printf(TEXT("fallbacks=%d"), Fallbacks), FString::Printf(TEXT("mismatches=%d"), Mismatches) },
				*(Dir / FString::Printf(TEXT("typing%d.txt"), Step)));
		}

		void AppendSidecar(const FString& Text)
		{
			// §10.10 driver B: the expected text and the time, flushed through FlushFileBuffers.
			const FString Line = FString::Printf(TEXT("%lld\t%s\t%s\n"), NowUtcMs(), *Typed->GetIdHex(), *Escape(Text));
			if (!SidecarOps->Append(SidecarHandle, DreamText::ToUtf8(Line)) || !SidecarOps->Flush(SidecarHandle))
			{
				Fail(TEXT("the sidecar write failed"));
			}
		}

		void TypeOne()
		{
			const FString Source(Corpus);
			const TCHAR Ch = Source[Cursor % Source.Len()];
			++Cursor;
			const FString Before = Typed->GetTextAsString();
			const FString Expected = Before + FString::ConstructFromPtrSize(&Ch, 1);
			AppendSidecar(Expected);   // before the keystroke
			FSlateApplication& App = FSlateApplication::Get();
			const uint32 User = App.GetUserIndexForKeyboard();
			if (Ch == TEXT('\n'))
			{
				App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter, FModifierKeysState(), User, false, 13, 13));
				App.ProcessKeyCharEvent(FCharacterEvent(TCHAR(13), FModifierKeysState(), User, false));
				App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter, FModifierKeysState(), User, false, 13, 13));
			}
			else
			{
				App.ProcessKeyCharEvent(FCharacterEvent(Ch, FModifierKeysState(), User, false));
			}
			++Keystrokes;
			FString After = Typed->GetTextAsString();
			if (After == Before)
			{
				// The key reached no focused text (the window lost focus): the same edit through the model, as typing makes it.
				++Fallbacks;
				dream::EditBatch Batch;
				Batch.Origin = dream::EditOrigin::Typing;
				dream::EditRange& Range = Batch.Ranges.emplace_back();
				Range.At = Typed->GetModel().Length();
				Range.Insert = std::u16string(1, static_cast<char16_t>(Ch));
				Typed->ApplyEdit(Batch);
				After = Typed->GetTextAsString();
			}
			if (After != Expected)
			{
				++Mismatches;
				AppendSidecar(After);   // recorded as the state it really is
			}
			if (Keystrokes % 10 == 0)
			{
				WriteStatus();
			}
		}

		void SubmitLongRun(FDreamEditorModule& M)
		{
			// T-E2E-010: the kill lands during a supervised long run; its first statement tells the driver it runs.
			bRunSubmitted = true;
			WriteStatus();
			const FString Signal = (Dir / FString::Printf(TEXT("run%d.txt"), Step)).Replace(TEXT("\\"), TEXT("/"));
			const FString Code = FString::Printf(TEXT("import time\nopen(r'%s', 'w').write('running')\nfor _ in range(900):\n    time.sleep(0.1)\n"), *Signal);
			const FDreamSubmitResult Result = M.GetRunService().Submit(DreamTest::MakeReplRequest(Code));
			if (!Result.bAccepted)
			{
				Fail(FString::Printf(TEXT("the long run was refused: %s"), *Result.Reason.ToString()));
				WriteResult(FString::Printf(TEXT("notkilled%d.txt"), Step));
			}
		}
	};

	/** The one step of this process (the console command runs once per editor session). */
	TUniquePtr<FStep> GStep;

	void StartStep()
	{
		if (GStep.IsValid())
		{
			return;
		}
		GStep = MakeUnique<FStep>();
		GStep->Step = DreamTest::CommandLineInt(TEXT("DreamG6Step"), 0);
		GStep->Kills = DreamTest::CommandLineInt(TEXT("DreamG6Kills"), 0);
		GStep->RunStep = DreamTest::CommandLineInt(TEXT("DreamG6RunStep"), 0);
		GStep->Dir = DreamTest::CommandLineString(TEXT("DreamG6Dir"));
		if (GStep->Step <= 0 || GStep->Dir.IsEmpty())
		{
			UE_LOG(LogDream, Warning, TEXT("Dream.Test.G6Step needs -DreamG6Step=<k> and -DreamG6Dir=<dir> (DreamHost/Scripts/run_g6.ps1)."));
			GStep.Reset();
			return;
		}
		GStep->Start();
	}

	FAutoConsoleCommand GG6StepCommand(TEXT("Dream.Test.G6Step"),
		TEXT("G6 driver B (DreamHost/Scripts/run_g6.ps1 only): restore, check the previous kill, then type until killed."),
		FConsoleCommandDelegate::CreateStatic(&StartStep));
}

BEGIN_DEFINE_SPEC(FDreamG6DriverSpec, "Dream.Persistence.G6Driver", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamG6DriverSpec)

void FDreamG6DriverSpec::Define()
{
	It("G6 driver B: kill -9 while typing and during a run, restore, I1-I5 (T-PER-036, T-E2E-010; driver-only)", [this]()
	{
		// A pass here is not a G6 run: the warning keeps the test index from reading T-PER-036 / T-E2E-010 as passed.
		AddWarning(TEXT("NOT RUN (T-PER-036 / T-E2E-010 are driver-only): run powershell -ExecutionPolicy Bypass -File DreamHost\\Scripts\\run_g6.ps1 (it kills and restarts the editor; each session runs the console command Dream.Test.G6Step)."));
		TestNotNull(TEXT("the driver's console command is registered"), IConsoleManager::Get().FindConsoleObject(TEXT("Dream.Test.G6Step")));
	});
}
