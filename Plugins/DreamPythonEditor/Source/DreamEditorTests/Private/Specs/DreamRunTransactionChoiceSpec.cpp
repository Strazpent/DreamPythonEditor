// SPDX-License-Identifier: Apache-2.0
// Which runs open a transaction (TDD §5.9.1, §5.2.1 RT-08 column, U3, U6; run group, B38, B40):
//   Dream.Transactions.RunInTransaction  a Run File runs without a transaction (RT-08); its footer offers
//       [Run in transaction] (FDreamRunService::CanRerunInTransaction), and FDreamRunService::RerunWithTransaction makes a
//       new numbered run of the same snapshot that opens "Dream: <file> #N"; the re-run itself no longer offers it.
//   Dream.Transactions.ToggleSeed  the tab's One undo step per run toggle seeded per route (A56, P2-run-02): untouched, a
//       selection run follows bTransactionSelectionAndLine and a buffer run bTransactionBuffer; once the user sets the
//       toggle, it decides both. The user settings are restored afterwards (board hygiene rule).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Run/DreamRunFile.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"

BEGIN_DEFINE_SPEC(FDreamRunInTransactionSpec, "Dream.Transactions.RunInTransaction", DreamTest::AttendedFlags)
	FString Dir;
END_DEFINE_SPEC(FDreamRunInTransactionSpec)

void FDreamRunInTransactionSpec::Define()
{
	AfterEach([this]()
	{
		if (!Dir.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		}
		Dir.Reset();
	});

	LatentIt("offers Run in transaction for a Run File and re-runs it with one undo step (RT-08)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"), TEXT("RunInTxn_") + Sfx));
		FPaths::NormalizeDirectoryName(Dir);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		const FString Path = Dir / FString::Printf(TEXT("txn_%s.py"), *Sfx);
		if (!TestTrue(TEXT("the run file was written"), FFileHelper::SaveStringToFile(FString::Printf(TEXT("txn_%s = 1\n"), *Sfx), *Path)))
		{
			Done.Execute();
			return;
		}
		FDreamRunService* Runs = &FDreamEditorModule::Get().GetRunService();
		TSharedRef<DreamTest::FRunResult> File = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Again = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("the Run File"), [Path]()
		{
			FDreamRunRequest Request;
			FText Error;
			DreamRunFile::MakeFileRequest(Path, EDreamRunRoute::RunFile, Request, Error);
			return Request;
		}, File, 60.0);
		Steps.Do(TEXT("check the Run File"), [this, File, Runs]()
		{
			if (DreamTest::ExpectOutcome(*this, *File, EDreamRunOutcome::Ok))
			{
				TestTrue(TEXT("Run File opens no transaction by default (RT-08)"), File->Record.Txn.Status == EDreamTxnStatus::Off);
				TestTrue(TEXT("its footer offers [Run in transaction]"), Runs->CanRerunInTransaction(File->RunNumber));
			}
			return true;
		});
		Steps.Do(TEXT("click [Run in transaction]"), [this, File, Again, Runs]()
		{
			Again->SubmitSeconds = FPlatformTime::Seconds();
			const FDreamSubmitResult Result = Runs->RerunWithTransaction(File->RunNumber);
			Again->RunNumber = Result.RunNumber;
			return TestTrue(*FString::Printf(TEXT("the re-run was accepted: %s"), *Result.Reason.ToString()), Result.bAccepted);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the re-run in a transaction"), Again);
		Steps.Do(TEXT("check the re-run"), [this, File, Again, Runs]()
		{
			if (DreamTest::ExpectOutcome(*this, *Again, EDreamRunOutcome::Ok))
			{
				TestTrue(TEXT("a new number"), Again->RunNumber > File->RunNumber);
				TestTrue(TEXT("the re-run opened a transaction"), Again->Record.Txn.Status != EDreamTxnStatus::Off);
				TestTrue(*FString::Printf(TEXT("titled Dream: <file> #N: %s"), *Again->Record.Txn.Title.ToString()),
					Again->Record.Txn.Title.ToString().EndsWith(FString::Printf(TEXT("#%d"), Again->RunNumber)));
				TestTrue(TEXT("the same route"), Again->Record.Route == EDreamRunRoute::RunFile);
				TestFalse(TEXT("a run that had a transaction does not offer it"), Runs->CanRerunInTransaction(Again->RunNumber));
			}
			return true;
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================

namespace DreamToggleSeedSpec
{
	struct FSaved
	{
		bool bSelection = true;
		bool bBuffer = true;
	};
}

BEGIN_DEFINE_SPEC(FDreamToggleSeedSpec, "Dream.Transactions.ToggleSeed", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	DreamToggleSeedSpec::FSaved Saved;
	bool bSaved = false;
END_DEFINE_SPEC(FDreamToggleSeedSpec)

void FDreamToggleSeedSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		UDreamUserSettings* Settings = GetMutableDefault<UDreamUserSettings>();
		Saved.bSelection = Settings->bTransactionSelectionAndLine;
		Saved.bBuffer = Settings->bTransactionBuffer;
		bSaved = true;
	});

	AfterEach([this]()
	{
		if (bSaved)
		{
			// Board hygiene: the user-global section keeps its values after a pass.
			UDreamUserSettings* Settings = GetMutableDefault<UDreamUserSettings>();
			Settings->bTransactionSelectionAndLine = Saved.bSelection;
			Settings->bTransactionBuffer = Saved.bBuffer;
			bSaved = false;
		}
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		Tab.Reset();
	});

	It("decides from the per-route settings until the toggle is set, then from the toggle (A56, P2-run-02)", [this]()
	{
		UDreamUserSettings* Settings = GetMutableDefault<UDreamUserSettings>();
		Settings->bTransactionSelectionAndLine = false;
		Settings->bTransactionBuffer = true;
		FDreamBufferViewState View;
		View.bOneUndoStepPerRun = true;         // what a new tab shows: the buffer-run seed
		View.bOneUndoStepPerRunSet = false;
		TestFalse(TEXT("untouched: a selection run follows bTransactionSelectionAndLine"), FDreamRunService::ResolveTabTransaction(View, EDreamRunRoute::Selection));
		TestFalse(TEXT("untouched: a line run too"), FDreamRunService::ResolveTabTransaction(View, EDreamRunRoute::Line));
		TestTrue(TEXT("untouched: a buffer run follows bTransactionBuffer"), FDreamRunService::ResolveTabTransaction(View, EDreamRunRoute::Buffer));
		TestFalse(TEXT("Run File keeps its own default, off (SHL-05)"), FDreamRunService::ResolveTabTransaction(View, EDreamRunRoute::RunFile));
		View.bOneUndoStepPerRunSet = true;      // the user set it (on): it decides every tab run
		TestTrue(TEXT("set on: a selection run opens one"), FDreamRunService::ResolveTabTransaction(View, EDreamRunRoute::Selection));
		View.bOneUndoStepPerRun = false;
		TestFalse(TEXT("set off: a buffer run opens none"), FDreamRunService::ResolveTabTransaction(View, EDreamRunRoute::Buffer));
		TestFalse(TEXT("GetTransactionDefault(Selection) reads bTransactionSelectionAndLine"), Settings->GetTransactionDefault(EDreamRunRoute::Selection));

		// The toolbar tells the truth (review r1): untouched with differing route settings it is mixed, and its tooltip names
		// both; untouched with agreeing settings it shows them; once set it shows the toggle.
		FDreamBufferViewState Untouched;
		Untouched.bOneUndoStepPerRun = true;
		Untouched.bOneUndoStepPerRunSet = false;
		TestFalse(TEXT("untouched, settings differ: the toggle shows a mixed state"), FDreamRunService::TabToggleShown(Untouched).IsSet());
		const FString Tip = FDreamRunService::TabToggleTooltip(Untouched).ToString();
		TestTrue(*FString::Printf(TEXT("the mixed tooltip names both kinds of run: %s"), *Tip),
			Tip.Contains(TEXT("buffer runs record one undo step")) && Tip.Contains(TEXT("selection, line and cell runs record no undo step")));
		Settings->bTransactionSelectionAndLine = true;
		TestTrue(TEXT("untouched, settings agree: the toggle shows them (on)"), FDreamRunService::TabToggleShown(Untouched).Get(false));
		Untouched.bOneUndoStepPerRunSet = true;
		Untouched.bOneUndoStepPerRun = false;
		Settings->bTransactionSelectionAndLine = false;
		TestTrue(TEXT("set off: the toggle shows off whatever the settings say"),
			FDreamRunService::TabToggleShown(Untouched).IsSet() && !FDreamRunService::TabToggleShown(Untouched).GetValue());
	});

	LatentIt("opens the transaction each route asks for in a real tab, and the toggle's once set", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, FString::Printf(TEXT("seed_%s = 1\nseed2_%s = 2"), *Sfx, *Sfx),
			TEXT("ToggleSeed"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		UDreamUserSettings* Settings = GetMutableDefault<UDreamUserSettings>();
		Settings->bTransactionSelectionAndLine = false;
		Settings->bTransactionBuffer = true;
		Buffer->GetViewState().bOneUndoStepPerRun = true;
		Buffer->GetViewState().bOneUndoStepPerRunSet = false;

		TSharedRef<DreamTest::FRunResult> Selection = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Whole = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> SetSelection = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a selection run"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Selection, 1, 1); }, Selection, 60.0);
		DreamTest::AddRun(Steps, TEXT("a buffer run"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 2); }, Whole);
		Steps.Do(TEXT("the user turns the toggle on (ToggleOneUndoStepPerRun)"), [Buffer]()
		{
			Buffer->GetViewState().bOneUndoStepPerRun = true;
			Buffer->GetViewState().bOneUndoStepPerRunSet = true;
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("a selection run with the toggle set"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Selection, 2, 2); }, SetSelection);
		Steps.Do(TEXT("check"), [this, Selection, Whole, SetSelection]()
		{
			if (DreamTest::ExpectOutcome(*this, *Selection, EDreamRunOutcome::Ok))
			{
				TestTrue(TEXT("untouched: the selection run follows bTransactionSelectionAndLine (off)"), Selection->Record.Txn.Status == EDreamTxnStatus::Off);
			}
			if (DreamTest::ExpectOutcome(*this, *Whole, EDreamRunOutcome::Ok))
			{
				TestTrue(TEXT("untouched: the buffer run follows bTransactionBuffer (on)"), Whole->Record.Txn.Status != EDreamTxnStatus::Off);
			}
			if (DreamTest::ExpectOutcome(*this, *SetSelection, EDreamRunOutcome::Ok))
			{
				TestTrue(TEXT("set: the toggle decides the selection run too"), SetSelection->Record.Txn.Status != EDreamTxnStatus::Off);
			}
			return true;
		});
		Steps.Run(Done);
	});
}
