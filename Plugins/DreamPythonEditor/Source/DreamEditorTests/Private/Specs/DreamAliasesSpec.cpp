// SPDX-License-Identifier: Apache-2.0
// Dream.Run.Aliases: the selection aliases in the editor (TDD §5.3.8, §5.3.3, §3.7, §3.8 UObject rules, UEI-03, U19, D3;
// run group, B35, B36). With level actors selected, a REPL line sees `sel`, `world` and `subsys` (UEI-03 "a REPL line
// sees the current selection"); a selection change while idle rewrites `sel` (the session epoch moves, §7.5.3); after
// `sel = 5` a selection change leaves it alone; Reset Session clears the ownership table, so `sel` is Dream's again
// (T-RUN-017 at tier 4); a map change releases Dream's aliases and the last-run namespace through its deferred helper
// (the epoch moves; T-LIFE-014's GC of the old world needs a real map load and is not run in the shared session).
// The rule itself, name by name, is tier 2 (Tests/python/test_aliases.py).
#include "Support/DreamTestUtil.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Module/DreamEditorModule.h"
#include "Python/DreamPython.h"
#include "Run/DreamRunService.h"
#include "Selection.h"
#include "Shell/SDreamTab.h"

namespace DreamAliasesSpec
{
	/** Up to Count actors of the editor world that the user could select (not transient, not hidden from the outliner). */
	TArray<TWeakObjectPtr<AActor>> FindActors(int32 Count)
	{
		TArray<TWeakObjectPtr<AActor>> Found;
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World == nullptr)
		{
			return Found;
		}
		for (TActorIterator<AActor> It(World); It && Found.Num() < Count; ++It)
		{
			AActor* Actor = *It;
			if (Actor != nullptr && !Actor->HasAnyFlags(RF_Transient) && Actor->IsListedInSceneOutliner() && !Actor->IsHiddenEd()
				&& GEditor->CanSelectActor(Actor, /*bInSelected*/ true, /*bSelectEvenIfHidden*/ false))
			{
				Found.Add(Actor);
			}
		}
		return Found;
	}

	TArray<TWeakObjectPtr<AActor>> CurrentSelection()
	{
		TArray<TWeakObjectPtr<AActor>> Selected;
		if (GEditor != nullptr)
		{
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				if (AActor* Actor = Cast<AActor>(*It))
				{
					Selected.Add(Actor);
				}
			}
		}
		return Selected;
	}

	void Select(const TArray<TWeakObjectPtr<AActor>>& Actors)
	{
		if (GEditor == nullptr)
		{
			return;
		}
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		for (const TWeakObjectPtr<AActor>& Actor : Actors)
		{
			if (Actor.IsValid())
			{
				GEditor->SelectActor(Actor.Get(), /*bInSelected*/ true, /*bNotify*/ false);
			}
		}
		GEditor->NoteSelectionChange();
	}
}

BEGIN_DEFINE_SPEC(FDreamAliasesSpec, "Dream.Run.Aliases", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<TWeakObjectPtr<AActor>> SavedSelection;
END_DEFINE_SPEC(FDreamAliasesSpec)

void FDreamAliasesSpec::Define()
{
	using namespace DreamAliasesSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		SavedSelection = CurrentSelection();
	});

	AfterEach([this]()
	{
		Select(SavedSelection);
		SavedSelection.Reset();
		Tab.Reset();
	});

	LatentIt("keeps sel, world and subsys current while idle, never touches a rebound name, and releases them on a map change",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TArray<TWeakObjectPtr<AActor>> Actors = FindActors(2);
		const bool bHaveActors = Actors.Num() == 2;
		if (!bHaveActors)
		{
			AddInfo(TEXT("the editor world has fewer than two selectable actors: the selection steps are skipped"));
		}
		FDreamPython* Python = &FDreamEditorModule::Get().GetPython();
		TSharedRef<uint32> Epoch = MakeShared<uint32>(0);
		TSharedRef<DreamTest::FRunResult> First = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Second = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Rebind = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Owned = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> AfterReset = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Isolated = MakeShared<DreamTest::FRunResult>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("select one actor"), [Actors, bHaveActors]()
		{
			Select(bHaveActors ? TArray<TWeakObjectPtr<AActor>>{ Actors[0] } : TArray<TWeakObjectPtr<AActor>>{});
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the first REPL line"), []()
		{
			return DreamTest::MakeReplRequest(TEXT("print('alias-first', len(sel), type(world).__name__, callable(subsys))"));
		}, First, 60.0);
		Steps.Do(TEXT("check the first line"), [this, First, bHaveActors]()
		{
			if (DreamTest::ExpectOutcome(*this, *First, EDreamRunOutcome::Ok))
			{
				const FString Expected = FString::Printf(TEXT("alias-first %d World True"), bHaveActors ? 1 : 0);
				TestTrue(*FString::Printf(TEXT("a REPL line sees the aliases (%s): %s"), *Expected, *First->Describe()),
					First->HasLineContaining(dream::LineType::Stdout, Expected));
			}
			return true;
		});

		if (bHaveActors)
		{
			// UEI-03: a selection change while idle refreshes sel (one deferred tick, 250 ms debounce).
			Steps.Do(TEXT("select both actors"), [Python, Epoch, Actors]()
			{
				*Epoch = Python->GetSessionEpoch();
				Select(Actors);
				return true;
			});
			Steps.Poll(TEXT("the idle refresh rewrote sel"), [Python, Epoch]()
			{
				return Python->GetSessionEpoch() > *Epoch ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
			}, 10.0);
			DreamTest::AddRun(Steps, TEXT("the second REPL line"), []()
			{
				return DreamTest::MakeReplRequest(TEXT("print('alias-second', len(sel))"));
			}, Second);
			Steps.Do(TEXT("check the second line"), [this, Second]()
			{
				if (DreamTest::ExpectOutcome(*this, *Second, EDreamRunOutcome::Ok))
				{
					TestTrue(*FString::Printf(TEXT("sel holds both actors: %s"), *Second->Describe()),
						Second->HasLineContaining(dream::LineType::Stdout, TEXT("alias-second 2")));
				}
				return true;
			});
		}

		// U19: once user code rebinds a name, Dream leaves it for the session.
		DreamTest::AddRun(Steps, TEXT("the rebinding line"), []() { return DreamTest::MakeReplRequest(TEXT("sel = 5")); }, Rebind);
		Steps.Do(TEXT("change the selection"), [Actors, bHaveActors]()
		{
			Select(bHaveActors ? TArray<TWeakObjectPtr<AActor>>{ Actors[1] } : TArray<TWeakObjectPtr<AActor>>{});
			return true;
		});
		Steps.WaitFrames(20);
		DreamTest::AddRun(Steps, TEXT("the owned check"), []() { return DreamTest::MakeReplRequest(TEXT("print('alias-owned', sel)")); }, Owned);
		Steps.Do(TEXT("check the rebound name"), [this, Rebind, Owned]()
		{
			DreamTest::ExpectOutcome(*this, *Rebind, EDreamRunOutcome::Ok);
			if (DreamTest::ExpectOutcome(*this, *Owned, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("sel is still the user's 5: %s"), *Owned->Describe()),
					Owned->HasLineContaining(dream::LineType::Stdout, TEXT("alias-owned 5")));
			}
			return true;
		});

		// Reset Session clears the ownership table (§5.3.3): sel is Dream's again at the next prepare.
		Steps.Do(TEXT("Reset Session"), [this, Python, Epoch]()
		{
			*Epoch = Python->GetSessionEpoch();
			FText Reason;
			return TestTrue(*FString::Printf(TEXT("Reset Session was accepted: %s"), *Reason.ToString()), Python->RequestResetSession(Reason));
		});
		Steps.Poll(TEXT("the reset ran"), [Python, Epoch]()
		{
			return Python->GetSessionEpoch() > *Epoch ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		DreamTest::AddRun(Steps, TEXT("the line after the reset"), []()
		{
			return DreamTest::MakeReplRequest(TEXT("print('alias-after-reset', isinstance(sel, int))"));
		}, AfterReset);
		Steps.Do(TEXT("check sel after the reset"), [this, AfterReset]()
		{
			if (DreamTest::ExpectOutcome(*this, *AfterReset, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("sel holds the selection again: %s"), *AfterReset->Describe()),
					AfterReset->HasLineContaining(dream::LineType::Stdout, TEXT("alias-after-reset False")));
			}
			return true;
		});

		// D3, §3.8: a map change releases Dream's aliases (and the last-run namespace) through the deferred helper.
		DreamTest::AddRun(Steps, TEXT("an Isolated run (the last-run namespace)"), []()
		{
			FDreamRunRequest Request = DreamTest::MakeReplRequest(TEXT("alias_isolated = 1"));
			Request.Namespace = EDreamNamespaceMode::Isolated;
			return Request;
		}, Isolated);
		Steps.Do(TEXT("signal a map change"), [Python, Epoch]()
		{
			*Epoch = Python->GetSessionEpoch();
			Python->SimulateMapChangeForTest();
			return true;
		});
		Steps.Poll(TEXT("the map-change helper released the aliases"), [Python, Epoch]()
		{
			return Python->GetSessionEpoch() > *Epoch ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("check the Isolated run"), [this, Isolated]()
		{
			DreamTest::ExpectOutcome(*this, *Isolated, EDreamRunOutcome::Ok);
			return true;
		});
		Steps.Run(Done);
	});
}
