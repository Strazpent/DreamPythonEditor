// SPDX-License-Identifier: Apache-2.0
// Dream.Transactions.NoChange (run group; TDD §5.9.2 "Empty", D8, SHL-14, T-KEY-009's map probe): a run that changes nothing
// in the level leaves the map package clean and the undo queue as it was. The fixture is G5's: transient actors in the
// editor world, some selected (the selection aliases have something to read at prepare, §5.3.8), the map's dirty flag
// cleared, and a file buffer whose whole text runs through the tab's own Ctrl+Enter path (RunSelectionOrBuffer with no
// selection: the Buffer route, one undo step by default): once through the tab command after an idle second, once as
// G5 sends it, a Slate key event a few frames after the fixture. UPackage::PackageMarkedDirtyEvent records who dirties
// the map, with a stack, so a failure names the culprit. Every mark of the map during the run counts, also on a map that
// is dirty already, and the run fails on any mark not made by the engine's FUnsavedAssetsTracker sync (which re-dirties a
// never-saved '/Temp/Untitled_N' while its built-data or an external package is dirty: the editor's state, not the
// run's). The dirty flag itself is judged only when the map stayed clean while settling.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamCommands.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Selection.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformStackWalk.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamTab.h"
#include "UObject/Package.h"

namespace DreamRunNoChangeSpec
{
	struct FState
	{
		TArray<TWeakObjectPtr<AActor>> Actors;
		TArray<TWeakObjectPtr<AActor>> PreviousSelection;
		TWeakObjectPtr<UPackage> Map;
		bool bMapWasDirty = false;
		FDelegateHandle DirtyHandle;
		FString Phase = TEXT("setup");
		TArray<FString> DirtyEvents;        // "<phase>: <package>\n<stack>"
		int32 RunMarks = 0;                 // PackageMarkedDirtyEvent on the map during the run, bWasDirty included
		int32 RunOwnMarks = 0;              // ... of them not from the engine's FUnsavedAssetsTracker sync
		double IdleFrom = 0.0;
		bool bDirtyBeforeRun = false;
		int32 QueueBefore = 0;
		int32 UndoBefore = 0;
		FString FilePath;
		FGuid BufferId;
	};

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	FString Stack()
	{
		constexpr SIZE_T Size = 16 * 1024;
		TArray<ANSICHAR> Buffer;
		Buffer.SetNumZeroed(Size);
		FPlatformStackWalk::StackWalkAndDump(Buffer.GetData(), Size, /*IgnoreCount*/ 2);
		return FString(UTF8_TO_TCHAR(Buffer.GetData())).Left(6000);
	}
}

BEGIN_DEFINE_SPEC(FDreamRunNoChangeSpec, "Dream.Transactions.NoChange", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	void DefineCase(const TCHAR* Name, bool bKeyRightAway);
END_DEFINE_SPEC(FDreamRunNoChangeSpec)

void FDreamRunNoChangeSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		Tab.Reset();
	});

	DefineCase(TEXT("leaves the map clean and the undo queue unchanged after a Buffer run that changes nothing (TDD 5.9.2)"), false);
	DefineCase(TEXT("leaves the map clean when Ctrl+Enter comes as a Slate key event, as in G5 (T-KEY-009)"), true);
}

void FDreamRunNoChangeSpec::DefineCase(const TCHAR* Name, bool bKeyRightAway)
{
	LatentIt(Name, DreamTest::LatentTimeout(), [this, bKeyRightAway](const FDoneDelegate& Done)
	{
		using namespace DreamRunNoChangeSpec;
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this) || EditorWorld() == nullptr)
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<FState> State = MakeShared<FState>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedPtr<SDreamTab> TabRef = Tab;

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("spawn and select the fixture actors, open the file buffer, clear the map's dirty flag"), [this, State, Sfx, TabRef]()
		{
			UWorld* World = EditorWorld();
			State->Map = World->GetOutermost();
			State->bMapWasDirty = World->GetOutermost()->IsDirty();
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				if (AActor* Selected = Cast<AActor>(*It))
				{
					State->PreviousSelection.Add(Selected);
				}
			}
			for (int32 Index = 0; Index < 20; ++Index)
			{
				FActorSpawnParameters Params;
				Params.Name = MakeUniqueObjectName(World->PersistentLevel, AStaticMeshActor::StaticClass(),
					FName(*FString::Printf(TEXT("DreamNoChange_%s_%d"), *Sfx, Index)));
				Params.ObjectFlags = RF_Transient;
				AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),
					FTransform(FVector(200.0 * Index, 500.0, 100.0)), Params);
				if (!TestNotNull(TEXT("a fixture actor"), Actor))
				{
					return false;
				}
				Actor->SetActorLabel(Params.Name.ToString());
				State->Actors.Add(Actor);
			}
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
			for (int32 Index = 0; Index < 3; ++Index)
			{
				GEditor->SelectActor(State->Actors[Index].Get(), /*bInSelected*/ true, /*bNotify*/ Index == 2);
			}

			const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Dream"), TEXT("NoChange")));
			IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
			State->FilePath = FPaths::Combine(Dir, FString::Printf(TEXT("nochange_%s.py"), *Sfx));
			const FString Text = FString::Printf(TEXT("nochange_%s = globals().get('nochange_%s', 0) + 1"), *Sfx, *Sfx);
			if (!FFileHelper::SaveStringToFile(Text, *State->FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				AddError(FString::Printf(TEXT("could not write %s"), *State->FilePath));
				return false;
			}
			FText Error;
			const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().OpenFile(State->FilePath, Error);
			if (!TestTrue(FString::Printf(TEXT("open %s: %s"), *State->FilePath, *Error.ToString()), Buffer.IsValid()))
			{
				return false;
			}
			State->BufferId = Buffer->GetId();
			TabRef->ActivateBuffer(State->BufferId);

			// Every mark of the map counts during the run, also on a map that is dirty already (bWasDirty): an already dirty
			// never-saved map would otherwise hide a leak (round-2 review). The engine's FUnsavedAssetsTracker sync re-marks such a
			// map from its core ticker; a mark whose stack runs through that module is the editor's, any other is the run's.
			State->DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddLambda([State](UPackage* Package, bool bWasDirty)
			{
				if (Package == nullptr || Package != State->Map.Get())
				{
					return;
				}
				const bool bInRun = State->Phase == TEXT("the run");
				if (!bInRun && bWasDirty)
				{
					return;
				}
				const FString Where = Stack();
				const bool bTracker = Where.Contains(TEXT("UnsavedAssetsTracker"));
				if (bInRun)
				{
					++State->RunMarks;
					State->RunOwnMarks += bTracker ? 0 : 1;
				}
				State->DirtyEvents.Add(FString::Printf(TEXT("%s: %s marked dirty%s%s\n%s"), *State->Phase, *Package->GetName(),
					bWasDirty ? TEXT(" (already dirty)") : TEXT(""), bTracker ? TEXT(" by FUnsavedAssetsTracker") : TEXT(""), *Where));
			});
			World->GetOutermost()->SetDirtyFlag(false);
			State->Phase = TEXT("settling before the run");
			State->IdleFrom = FPlatformTime::Seconds();
			return true;
		});
		// Settle: clearing the dirty flag of a new map ('/Temp/Untitled_N', PKG_NewlyCreated) makes the engine's
		// FUnsavedAssetsTracker sync with the dirty-package list at its next core tick, and FEditorFileUtils::
		// EnumerateDirtyWorldPackages marks such a map dirty again while its built-data or an external package is dirty
		// (UnrealEd FileHelpers.cpp, "If world package does not have a name yet"). That is the editor's state, not a run's
		// doing, so the map is judged only when it stayed clean while settling (the G5 finding, run group).
		Steps.Poll(TEXT("settle: one second (the alias refresh after the selection change runs here), or a few frames"), [State, bKeyRightAway]()
		{
			return FPlatformTime::Seconds() - State->IdleFrom >= (bKeyRightAway ? 0.1 : 1.0) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.Do(TEXT("Ctrl+Enter in the file buffer (RunSelectionOrBuffer, no selection)"), [this, State, Run, TabRef, bKeyRightAway]()
		{
			State->bDirtyBeforeRun = State->Map.IsValid() && State->Map->IsDirty();     // the engine re-dirtied the new map
			State->QueueBefore = GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
			State->UndoBefore = GEditor->Trans ? GEditor->Trans->GetUndoCount() : 0;
			State->Phase = TEXT("the run");
			const TSharedPtr<SDreamCodeView> Code = TabRef->GetActiveCodeView();
			if (Code.IsValid())
			{
				Code->SetCaret(0, 0);
			}
			FText Reason;
			if (!TestTrue(FString::Printf(TEXT("RunSelectionOrBuffer can execute: %s"), *Reason.ToString()),
				TabRef->CanExecuteDreamCommand(EDreamCommand::RunSelectionOrBuffer, Reason)))
			{
				return false;
			}
			if (bKeyRightAway && Code.IsValid())
			{
				// G5's Send: key-down and key-up through Slate's real input path to the focused code view (no char with Ctrl).
				Code->FocusEditor();
				FSlateApplication& App = FSlateApplication::Get();
				const FModifierKeysState Ctrl(false, false, true, false, false, false, false, false, false);
				const uint32 User = App.GetUserIndexForKeyboard();
				TestTrue(TEXT("Dream Python Editor handled the Ctrl+Enter key-down"), App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter, Ctrl, User, false, 0, 0)));
				App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter, Ctrl, User, false, 0, 0));
			}
			else
			{
				TabRef->ExecuteDreamCommand(EDreamCommand::RunSelectionOrBuffer);
			}
			const FDreamRunRecord* Active = FDreamEditorModule::Get().GetRunService().GetActiveRecord();
			Run->RunNumber = Active != nullptr ? Active->RunNumber : 0;
			Run->SubmitSeconds = FPlatformTime::Seconds();
			return TestTrue(TEXT("the command submitted a run"), Run->RunNumber > 0);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the Buffer run"), Run, 60.0);
		Steps.WaitFrames(3);
		Steps.Do(TEXT("check"), [this, State, Run]()
		{
			State->Phase = TEXT("after the run");
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			TestTrue(TEXT("the run was a Buffer run"), Run->Record.Route == EDreamRunRoute::Buffer);
			AddInfo(FString::Printf(TEXT("transaction status %d, txn bytes %lld"), static_cast<int32>(Run->Record.Txn.Status), static_cast<long long>(Run->Record.Txn.LastSize)));
			const bool bDirty = State->Map.IsValid() && State->Map->IsDirty();
			FString Events = FString::Join(State->DirtyEvents, TEXT("\n----\n"));
			if (!Events.IsEmpty())
			{
				AddInfo(FString::Printf(TEXT("map dirty events:\n%s"), *Events));
			}
			// The run itself never marks the map, dirty or not (T-KEY-009 probe; G5 counts the same marks).
			AddInfo(FString::Printf(TEXT("map marks during the run: %d, %d of them not from FUnsavedAssetsTracker"), State->RunMarks, State->RunOwnMarks));
			TestEqual(TEXT("the run marked the map package dirty (marks not from the engine's FUnsavedAssetsTracker sync)"), State->RunOwnMarks, 0);
			if (State->bDirtyBeforeRun)
			{
				AddInfo(TEXT("the editor marked the new map dirty again while settling (FUnsavedAssetsTracker sync: its built-data or an external package is dirty), so the dirty flag cannot show a leak; the mark count above does"));
			}
			else
			{
				TestFalse(TEXT("the map package is not dirty after a run that changed nothing (T-KEY-009 probe)"), bDirty);
			}
			TestEqual(TEXT("undo queue length unchanged (an empty transaction is discarded, TDD 5.9.2)"),
				GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0, State->QueueBefore);
			TestEqual(TEXT("undo count unchanged"), GEditor->Trans ? GEditor->Trans->GetUndoCount() : 0, State->UndoBefore);
			return true;
		});
		Steps.Finally(TEXT("remove the fixture"), [State]()
		{
			UPackage::PackageMarkedDirtyEvent.Remove(State->DirtyHandle);
			DreamTest::CloseBuffer(State->BufferId);
			if (!State->FilePath.IsEmpty())
			{
				IFileManager::Get().Delete(*State->FilePath, false, false, true);
			}
			UWorld* World = EditorWorld();
			if (World != nullptr && GEditor != nullptr)
			{
				GEditor->SelectNone(false, true);
				for (const TWeakObjectPtr<AActor>& Actor : State->Actors)
				{
					if (Actor.IsValid())
					{
						World->DestroyActor(Actor.Get());
					}
				}
				for (const TWeakObjectPtr<AActor>& Selected : State->PreviousSelection)
				{
					if (Selected.IsValid())
					{
						GEditor->SelectActor(Selected.Get(), true, false);
					}
				}
				GEditor->NoteSelectionChange();
				if (State->Map.IsValid())
				{
					State->Map->SetDirtyFlag(State->bMapWasDirty);
				}
			}
		});
		Steps.Run(Done);
	});
}
