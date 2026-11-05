// SPDX-License-Identifier: Apache-2.0
// Dream.Run.RunFile: Run File and dream_editor.run(<path>) (TDD §5.1.2 step 4, §5.2.1 Run File and API rows, §5.3.4, RUN-11,
// RUN-24, T-ENTRY-015, T-API-009). A file on a private temp folder, open in a tab:
//   1. Run File of the dirty tab saves it first, runs Isolated with sys.argv == [path] and the file's folder as sys.path[0]
//      during the run, removed again afterwards, and the footer says "no transaction (Run File)";
//   2. the same with the file read-only ends not executed ("Save failed"), numbered, without a transaction, still dirty;
//   3. an API run of the path (what _dream.enqueue_run calls) keeps Run File semantics although the file is open in a tab:
//      Isolated, no statement mode, the same sys.argv and sys.path (RUN-02, RUN-03);
//   4. a re-run of a Run File and of an API file run keeps those semantics: sys.argv is still [path] (RUN-02, RUN-12).
// The run file records what it saw on builtins; a REPL check run reads it back and prints one verdict per rule.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamOutputRows.h"

namespace DreamRunFileSpec
{
	struct FState
	{
		FString Dir;
		FString Path;
		FString Sfx;
		FGuid BufferId;
		bool bSettingSaved = false;
		bool bSavedRunFileTransaction = false;
	};

	/** The run file: it records sys.argv and sys.path[0] on builtins, so the check run can read them after the run. */
	FString FileText(const FString& Sfx, const FString& Marker)
	{
		return FString::Join(TArray<FString>{
			TEXT("import builtins, os, sys"),
			FString::Printf(TEXT("builtins._dream_t4_runfile_%s = (list(sys.argv), os.path.normcase(os.path.abspath(sys.path[0])), '%s')"), *Sfx, *Marker),
		}, TEXT("\n"));
	}

	/** One REPL run that prints "argv-ok", "path0-ok", "cleaned-ok" and "marker-ok", or the "-bad" line with what it saw. */
	FString CheckCode(const FString& Sfx, const FString& Path, const FString& Marker)
	{
		return FString::Join(TArray<FString>{
			FString::Printf(TEXT("import builtins as _b_%s, os as _o_%s, sys as _s_%s"), *Sfx, *Sfx, *Sfx),
			FString::Printf(TEXT("_r_%s = _b_%s.__dict__.pop('_dream_t4_runfile_%s', None)"), *Sfx, *Sfx, *Sfx),
			FString::Printf(TEXT("_p_%s = _o_%s.path.normcase(_o_%s.path.abspath(r'%s'))"), *Sfx, *Sfx, *Sfx, *Path),
			FString::Printf(TEXT("print('argv-ok' if _r_%s is not None and [_o_%s.path.normcase(_o_%s.path.abspath(a)) for a in _r_%s[0]] == [_p_%s] else 'argv-bad %%r' %% (_r_%s,))"),
				*Sfx, *Sfx, *Sfx, *Sfx, *Sfx, *Sfx),
			FString::Printf(TEXT("print('path0-ok' if _r_%s is not None and _r_%s[1] == _o_%s.path.dirname(_p_%s) else 'path0-bad %%r' %% (_r_%s,))"),
				*Sfx, *Sfx, *Sfx, *Sfx, *Sfx),
			FString::Printf(TEXT("print('cleaned-ok' if _o_%s.path.dirname(_p_%s) not in [_o_%s.path.normcase(_o_%s.path.abspath(p)) for p in _s_%s.path if p] else 'cleaned-bad')"),
				*Sfx, *Sfx, *Sfx, *Sfx, *Sfx),
			FString::Printf(TEXT("print('marker-ok' if _r_%s is not None and _r_%s[2] == '%s' else 'marker-bad')"), *Sfx, *Sfx, *Marker),
			FString::Printf(TEXT("del _b_%s, _o_%s, _s_%s, _r_%s, _p_%s"), *Sfx, *Sfx, *Sfx, *Sfx, *Sfx),
		}, TEXT("\n"));
	}

	void ExpectVerdicts(FAutomationTestBase& Test, const TCHAR* What, const DreamTest::FRunResult& Check)
	{
		for (const TCHAR* Verdict : { TEXT("argv-ok"), TEXT("path0-ok"), TEXT("cleaned-ok"), TEXT("marker-ok") })
		{
			Test.TestTrue(*FString::Printf(TEXT("%s: %s (%s)"), What, Verdict, *Check.Describe()), Check.HasLineContaining(dream::LineType::Stdout, Verdict));
		}
	}

	/** Runs that check the file run's verdicts: wait for Run, then the REPL check run, then its verdicts. */
	void AddCheckedRun(DreamTest::FDreamSteps& Steps, FAutomationTestBase& Test, const TSharedRef<FState>& State, const TCHAR* What,
		const TSharedRef<DreamTest::FRunResult>& Run, const FString& Marker)
	{
		DreamTest::AddWaitForRun(Steps, What, Run);
		TSharedRef<DreamTest::FRunResult> Check = MakeShared<DreamTest::FRunResult>();
		DreamTest::AddRun(Steps, FString::Printf(TEXT("the check run after %s"), What), [State, Marker]()
		{
			return DreamTest::MakeReplRequest(CheckCode(State->Sfx, State->Path, Marker));
		}, Check);
		Steps.Do(FString::Printf(TEXT("check %s"), What), [&Test, Run, Check, What]()
		{
			if (DreamTest::ExpectOutcome(Test, *Run, EDreamRunOutcome::Ok) && DreamTest::ExpectOutcome(Test, *Check, EDreamRunOutcome::Ok))
			{
				ExpectVerdicts(Test, What, *Check);
			}
			return true;
		});
	}

	/** One typed edit at the end of the buffer: the tab is dirty, its text differs from the disk. */
	bool MakeDirty(FAutomationTestBase& Test, FDreamBuffer& Buffer, const FString& Text)
	{
		dream::EditBatch Batch;
		Batch.Origin = dream::EditOrigin::Typing;
		dream::EditRange& Range = Batch.Ranges.emplace_back();
		Range.At = 0;
		Range.DeleteLen = Buffer.GetModel().Length();
		Range.Insert = std::u16string(DreamText::ToU16(Text));
		const dream::ChangeSet Change = Buffer.ApplyEdit(Batch);
		return Test.TestFalse(TEXT("the edit applied"), Change.IsEmpty()) && Test.TestTrue(TEXT("the tab is dirty"), Buffer.IsDirty());
	}
}

BEGIN_DEFINE_SPEC(FDreamRunFileSpec, "Dream.Run.RunFile", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TSharedPtr<DreamRunFileSpec::FState> State;
	bool PrepareFile(const FString& InitialText);
END_DEFINE_SPEC(FDreamRunFileSpec)

bool FDreamRunFileSpec::PrepareFile(const FString& InitialText)
{
	State = MakeShared<DreamRunFileSpec::FState>();
	State->Sfx = DreamTest::UniqueSuffix();
	State->Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"), TEXT("RunFile_") + State->Sfx));
	FPaths::NormalizeDirectoryName(State->Dir);
	State->Path = State->Dir / FString::Printf(TEXT("runfile_%s.py"), *State->Sfx);
	IFileManager::Get().MakeDirectory(*State->Dir, /*Tree*/ true);
	if (!FFileHelper::SaveStringToFile(InitialText, *State->Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		AddError(FString::Printf(TEXT("could not write %s"), *State->Path));
		return false;
	}
	if (!Tab->OpenLocation(State->Path, 0, 0))
	{
		AddError(FString::Printf(TEXT("could not open %s in a tab"), *State->Path));
		return false;
	}
	const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindByPath(State->Path);
	if (!TestTrue(TEXT("the file is open in a tab"), Buffer.IsValid()))
	{
		return false;
	}
	State->BufferId = Buffer->GetId();
	State->Path = Buffer->GetPath();     // the normalized path the run uses
	return true;
}

void FDreamRunFileSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		if (State.IsValid())
		{
			DreamTest::CloseBuffer(State->BufferId);
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*State->Path, false);
			IFileManager::Get().DeleteDirectory(*State->Dir, /*RequireExists*/ false, /*Tree*/ true);
			State.Reset();
		}
		Tab.Reset();
	});

	LatentIt("saves a dirty tab first and runs it with Run File semantics; the sys.path entry is gone afterwards (T-ENTRY-015, RUN-11)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this) || !PrepareFile(TEXT("# placeholder\n")))
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Check = MakeShared<DreamTest::FRunResult>();
		const TSharedRef<FString> Edited = MakeShared<FString>(DreamRunFileSpec::FileText(State->Sfx, TEXT("tab")));

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("edit the tab and press Run File"), [this, Run, Edited]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(State->BufferId);
			if (!Buffer.IsValid() || !DreamRunFileSpec::MakeDirty(*this, *Buffer, *Edited))
			{
				return false;
			}
			// RT-08: Run File's own default is off and the tab's toggle can only turn it off; both are user settings, so they
			// are pinned (the toggle on, so the default alone decides) and restored (A1-01).
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedRunFileTransaction = Settings->bTransactionRunFile;
			State->bSettingSaved = true;
			Settings->bTransactionRunFile = false;
			Buffer->GetViewState().bOneUndoStepPerRun = true;
			Tab->ActivateBuffer(State->BufferId);
			FText Reason;
			if (!Tab->CanExecuteDreamCommand(EDreamCommand::RunFile, Reason))
			{
				AddError(FString::Printf(TEXT("Run File cannot execute: %s"), *Reason.ToString()));
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::RunFile);
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::GetActiveRunNumber();
			return TestTrue(TEXT("Run File submitted a run"), Run->RunNumber > 0);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the Run File run"), Run);
		Steps.Do(TEXT("check the save and the record"), [this, Run, Edited]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				return false;
			}
			FString Disk;
			FFileHelper::LoadFileToString(Disk, *State->Path);
			TestEqual(TEXT("Starting step 4 saved the buffer's text first (§5.1.2)"), Disk.Replace(TEXT("\r\n"), TEXT("\n")), *Edited);
			const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(State->BufferId);
			TestTrue(TEXT("the tab is clean after the save"), Buffer.IsValid() && !Buffer->IsDirty());
			const FDreamRunRecord& Record = Run->Record;
			TestTrue(TEXT("route Run File"), Record.Route == EDreamRunRoute::RunFile);
			TestTrue(TEXT("Isolated (§5.2.1)"), Record.Namespace == EDreamNamespaceMode::Isolated);
			TestFalse(TEXT("one exec, no statement mode (RUN-27)"), Record.bStatementMode);
			TestTrue(TEXT("no transaction (RT-08)"), Record.Txn.Status == EDreamTxnStatus::Off);
			FDreamEditorModule& Module = FDreamEditorModule::Get();
			const FString Footer = DreamOutputRows::FooterText(Module.GetRunService().FindRecord(Run->RunNumber),
				static_cast<uint32>(Run->RunNumber), Module.GetOutputStore()).ToString();
			TestTrue(*FString::Printf(TEXT("the footer says no transaction (Run File): %s"), *Footer), Footer.Contains(TEXT("no transaction (Run File)")));
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the check run"), [this]()
		{
			return DreamTest::MakeReplRequest(DreamRunFileSpec::CheckCode(State->Sfx, State->Path, TEXT("tab")));
		}, Check);
		Steps.Do(TEXT("check sys.argv and sys.path"), [this, Check]()
		{
			if (DreamTest::ExpectOutcome(*this, *Check, EDreamRunOutcome::Ok))
			{
				DreamRunFileSpec::ExpectVerdicts(*this, TEXT("Run File"), *Check);
			}
			return true;
		});
		const TSharedRef<DreamRunFileSpec::FState> Pinned = State.ToSharedRef();
		Steps.Finally(TEXT("restore the Run File default"), [Pinned]()
		{
			if (Pinned->bSettingSaved)
			{
				UDreamUserSettings::GetMutable()->bTransactionRunFile = Pinned->bSavedRunFileTransaction;
			}
		});
		Steps.Run(Done);
	});

	LatentIt("ends a Run File not executed when the save fails: numbered, no transaction, still dirty (T-ENTRY-015)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this) || !PrepareFile(TEXT("# placeholder\n")))
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("make the file read-only, edit the tab and press Run File"), [this, Run]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(State->BufferId);
			if (!Buffer.IsValid() || !DreamRunFileSpec::MakeDirty(*this, *Buffer, DreamRunFileSpec::FileText(State->Sfx, TEXT("never"))))
			{
				return false;
			}
			if (!TestTrue(TEXT("the file is read-only now"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*State->Path, true)))
			{
				return false;
			}
			Tab->ActivateBuffer(State->BufferId);
			Tab->ExecuteDreamCommand(EDreamCommand::RunFile);
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::GetActiveRunNumber();
			return TestTrue(TEXT("Run File submitted a run"), Run->RunNumber > 0);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the Run File run"), Run);
		Steps.Do(TEXT("check the not executed run"), [this, Run]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::NotExecuted))
			{
				return false;
			}
			const FDreamRunRecord& Record = Run->Record;
			TestTrue(*FString::Printf(TEXT("the detail names the save failure: %s"), *Record.Detail), Record.Detail.StartsWith(TEXT("Save failed")));
			TestTrue(TEXT("the run took a number (U7, U8)"), Record.RunNumber > 0);
			TestTrue(TEXT("no transaction was opened (U7)"), Record.Txn.Status == EDreamTxnStatus::Off);
			const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(State->BufferId);
			TestTrue(TEXT("the tab stays dirty (§10.14)"), Buffer.IsValid() && Buffer->IsDirty());
			FString Disk;
			FFileHelper::LoadFileToString(Disk, *State->Path);
			TestEqual(TEXT("the original bytes on disk survive"), Disk.Replace(TEXT("\r\n"), TEXT("\n")), FString(TEXT("# placeholder\n")));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("keeps Run File semantics for an API run of a path that is open in a tab (RUN-02, RUN-03, T-API-009)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		if (!PrepareFile(DreamRunFileSpec::FileText(Sfx, TEXT("api"))))
		{
			Done.Execute();
			return;
		}
		State->Sfx = Sfx;
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Check = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("dream_editor.run(<path>)"), [this, Run]()
		{
			FText Error;
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = FDreamEditorModule::Get().GetRunService().EnqueueApiRun(State->Path, TOptional<EDreamNamespaceMode>(), TOptional<bool>(), Error);
			if (Run->RunNumber <= 0)
			{
				AddError(FString::Printf(TEXT("the API run was refused: %s"), *Error.ToString()));
				return false;
			}
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the API run"), Run);
		Steps.Do(TEXT("check the record"), [this, Run]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				return false;
			}
			const FDreamRunRecord& Record = Run->Record;
			TestTrue(TEXT("route Api"), Record.Route == EDreamRunRoute::Api);
			TestTrue(TEXT("a file target, though a tab has the file open"), Record.bFileTarget);
			TestTrue(TEXT("Isolated (§5.2.1 API row: as Run File)"), Record.Namespace == EDreamNamespaceMode::Isolated);
			TestFalse(TEXT("no statement mode for a file target (RUN-27)"), Record.bStatementMode);
			TestEqual(TEXT("no input echo for a file target"), Run->OfType(dream::LineType::Echo).Num(), 0);
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the check run"), [this]()
		{
			return DreamTest::MakeReplRequest(DreamRunFileSpec::CheckCode(State->Sfx, State->Path, TEXT("api")));
		}, Check);
		Steps.Do(TEXT("check sys.argv and sys.path"), [this, Check]()
		{
			if (DreamTest::ExpectOutcome(*this, *Check, EDreamRunOutcome::Ok))
			{
				DreamRunFileSpec::ExpectVerdicts(*this, TEXT("API run"), *Check);
			}
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("re-runs a Run File and an API file run with the same sys.argv and sys.path (RUN-02, RUN-12)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		if (!PrepareFile(DreamRunFileSpec::FileText(Sfx, TEXT("rerun"))))
		{
			Done.Execute();
			return;
		}
		State->Sfx = Sfx;
		const TSharedRef<DreamRunFileSpec::FState> Shared = State.ToSharedRef();
		TSharedRef<DreamTest::FRunResult> First = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Again = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Api = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> ApiAgain = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);

		Steps.Do(TEXT("Run File"), [this, First]()
		{
			Tab->ActivateBuffer(State->BufferId);
			Tab->ExecuteDreamCommand(EDreamCommand::RunFile);
			First->SubmitSeconds = FPlatformTime::Seconds();
			First->RunNumber = DreamTest::GetActiveRunNumber();
			return TestTrue(TEXT("Run File submitted a run"), First->RunNumber > 0);
		});
		DreamRunFileSpec::AddCheckedRun(Steps, *this, Shared, TEXT("the Run File run"), First, TEXT("rerun"));

		Steps.Do(TEXT("Rerun of the Run File"), [this, First, Again]()
		{
			const FDreamSubmitResult Result = FDreamEditorModule::Get().GetRunService().Rerun(First->RunNumber);
			Again->SubmitSeconds = FPlatformTime::Seconds();
			Again->RunNumber = Result.RunNumber;
			return TestTrue(*FString::Printf(TEXT("the re-run is accepted (%s)"), *Result.Reason.ToString()), Result.bAccepted && Result.RunNumber > 0);
		});
		DreamRunFileSpec::AddCheckedRun(Steps, *this, Shared, TEXT("the re-run of the Run File"), Again, TEXT("rerun"));

		Steps.Do(TEXT("dream_editor.run(<path>)"), [this, Api]()
		{
			FText Error;
			Api->SubmitSeconds = FPlatformTime::Seconds();
			Api->RunNumber = FDreamEditorModule::Get().GetRunService().EnqueueApiRun(State->Path, TOptional<EDreamNamespaceMode>(), TOptional<bool>(), Error);
			return TestTrue(*FString::Printf(TEXT("the API run is accepted (%s)"), *Error.ToString()), Api->RunNumber > 0);
		});
		DreamRunFileSpec::AddCheckedRun(Steps, *this, Shared, TEXT("the API run"), Api, TEXT("rerun"));

		Steps.Do(TEXT("Run without snapshot of the API run"), [this, Api, ApiAgain]()
		{
			const FDreamSubmitResult Result = FDreamEditorModule::Get().GetRunService().RerunWithoutSnapshot(Api->RunNumber);
			ApiAgain->SubmitSeconds = FPlatformTime::Seconds();
			ApiAgain->RunNumber = Result.RunNumber;
			return TestTrue(*FString::Printf(TEXT("the re-run is accepted (%s)"), *Result.Reason.ToString()), Result.bAccepted && Result.RunNumber > 0);
		});
		DreamRunFileSpec::AddCheckedRun(Steps, *this, Shared, TEXT("the re-run of the API run"), ApiAgain, TEXT("rerun"));
		Steps.Do(TEXT("the API re-run is still a file target"), [this, ApiAgain]()
		{
			TestTrue(TEXT("a file target (Old->bFileTarget carried over)"), ApiAgain->Record.bFileTarget);
			TestTrue(TEXT("Isolated"), ApiAgain->Record.Namespace == EDreamNamespaceMode::Isolated);
			return true;
		});
		Steps.Run(Done);
	});
}
