// SPDX-License-Identifier: Apache-2.0
// Dream.Run.LineAdvance: Run line and advance (RUN-26, T-RUN-029's first case) through the real key path: Alt+Enter goes
// through FDreamKeyRouter::RoutePre with a focused buffer's context (§4.9.1, §4.9.4) into SDreamTab, which runs the
// statement extent at the caret (RUN-02) and moves the caret to the next statement when Submit accepts it.
// Five presses on a five-statement buffer give five numbered Session runs, one per statement, and the caret ends on the
// last line ("End of buffer"). A press while the previous run is still Starting is consumed, refused and leaves the caret.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamKeyRouter.h"
#include "GenericPlatform/GenericApplication.h"
#include "HAL/PlatformTime.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Module/DreamEditorModule.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamTab.h"

namespace DreamLineAdvanceSpec
{
	/** Alt+Enter as Slate delivers it: main Enter, left Alt down. */
	FReply PressAltEnter(SDreamTab& Tab)
	{
		const FModifierKeysState LeftAlt(false, false, false, false, /*bInIsLeftAltDown*/ true, false, false, false, false);
		const FKeyEvent AltEnter(EKeys::Enter, LeftAlt, static_cast<uint32>(0), /*bInIsRepeat*/ false, /*InCharacterCode*/ 13, /*InKeyCode*/ 13);
		// The context a focused, editable buffer view computes (SDreamCodeView::ComputeContextMask) with no selection and no run.
		const uint32 Context = dream::Ctx_Focused | dream::Ctx_Editable | dream::Ctx_Buffer;
		return FDreamKeyRouter::Get().RoutePre(AltEnter, Context, Tab);
	}

	/** One press and its expectations (0-based caret lines, 1-based run ranges). */
	struct FPress
	{
		int32 FirstLine = 0;
		int32 LastLine = 0;
		int32 CaretAfter = 0;
	};
}

BEGIN_DEFINE_SPEC(FDreamLineAdvanceSpec, "Dream.Run.LineAdvance", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamLineAdvanceSpec)

void FDreamLineAdvanceSpec::Define()
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

	LatentIt("walks a five-statement buffer with five Alt+Enter presses and ends on the last statement (RUN-26)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		// Five statements: a line, a for loop with its body, a call over two lines, a line, the last line (no final newline).
		const TArray<FString> Source = {
			FString::Printf(TEXT("adv_%s = []"), *Sfx),
			FString::Printf(TEXT("for i_%s in range(2):"), *Sfx),
			FString::Printf(TEXT("    adv_%s.append(i_%s)"), *Sfx, *Sfx),
			FString::Printf(TEXT("adv_%s.append("), *Sfx),
			TEXT("    10)"),
			FString::Printf(TEXT("adv_%s.append(3)"), *Sfx),
			FString::Printf(TEXT("print(adv_%s)"), *Sfx),
		};
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, FString::Join(Source, TEXT("\n")), TEXT("Advance"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const TArray<DreamLineAdvanceSpec::FPress> Presses = {
			{ 1, 1, 1 },    // the first line; the caret moves to the for header
			{ 2, 3, 3 },    // the whole loop; the caret lands after its body
			{ 4, 5, 5 },    // the whole call
			{ 6, 6, 6 },    // a line; the caret moves to the last statement
			{ 7, 7, 6 },    // the last statement; the caret stays: End of buffer
		};

		TArray<TSharedRef<DreamTest::FRunResult>> Runs;
		for (int32 Index = 0; Index < Presses.Num(); ++Index)
		{
			Runs.Add(MakeShared<DreamTest::FRunResult>());
		}

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("caret on line 1"), [this]()
		{
			Tab->ActivateBuffer(BufferId);
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(BufferId);
			if (!View.IsValid() || Tab->GetActiveCodeView() != View)
			{
				AddError(TEXT("the scratch buffer is not the active code view"));
				return false;
			}
			View->SetCaret(0, 0);
			return true;
		});

		for (int32 Index = 0; Index < Presses.Num(); ++Index)
		{
			const DreamLineAdvanceSpec::FPress Press = Presses[Index];
			const TSharedRef<DreamTest::FRunResult> Run = Runs[Index];
			const TSharedRef<DreamTest::FRunResult> Previous = Runs[FMath::Max(Index - 1, 0)];
			Steps.Do(FString::Printf(TEXT("press %d"), Index + 1), [this, Index, Run, Previous]()
			{
				const FReply Reply = DreamLineAdvanceSpec::PressAltEnter(*Tab);
				if (!TestTrue(*FString::Printf(TEXT("press %d: Alt+Enter is a bound Dream Python Editor chord in a buffer (consumed)"), Index + 1), Reply.IsEventHandled()))
				{
					return false;
				}
				Run->SubmitSeconds = FPlatformTime::Seconds();
				Run->RunNumber = DreamTest::GetActiveRunNumber();
				if (Run->RunNumber <= 0)
				{
					AddError(FString::Printf(TEXT("press %d: no run was submitted"), Index + 1));
					return false;
				}
				if (Index > 0)
				{
					TestEqual(*FString::Printf(TEXT("press %d: the next run number"), Index + 1), Run->RunNumber, Previous->RunNumber + 1);
				}
				return true;
			});

			if (Index == 0)
			{
				// The first run is still Starting (it enters at this frame's OnEndFrame): a second press is refused.
				Steps.Do(TEXT("press while the run is active"), [this, Run]()
				{
					const int32 HistoryBefore = FDreamEditorModule::Get().GetRunService().GetHistory().Num();
					int32 LineBefore = -1;
					int32 ColumnBefore = -1;
					Tab->FindCodeView(BufferId)->GetCaret(LineBefore, ColumnBefore);
					const FReply Reply = DreamLineAdvanceSpec::PressAltEnter(*Tab);
					int32 LineAfter = -1;
					int32 ColumnAfter = -1;
					Tab->FindCodeView(BufferId)->GetCaret(LineAfter, ColumnAfter);
					TestTrue(TEXT("a refused press is still consumed (§4.9.4)"), Reply.IsEventHandled());
					TestEqual(TEXT("the refused press submits nothing"), DreamTest::GetActiveRunNumber(), Run->RunNumber);
					TestEqual(TEXT("the refused press adds no history record"), FDreamEditorModule::Get().GetRunService().GetHistory().Num(), HistoryBefore);
					TestEqual(TEXT("the refused press leaves the caret"), LineAfter, LineBefore);
					return true;
				});
			}

			DreamTest::AddWaitForRun(Steps, FString::Printf(TEXT("run %d"), Index + 1), Run, Index == 0 ? 60.0 : 30.0);
			Steps.Do(FString::Printf(TEXT("check run %d"), Index + 1), [this, Index, Press, Run]()
			{
				if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
				{
					return false;
				}
				const FDreamRunRecord& Record = Run->Record;
				TestTrue(*FString::Printf(TEXT("run %d is a line run"), Index + 1), Record.Route == EDreamRunRoute::Line);
				TestTrue(*FString::Printf(TEXT("run %d runs in the Session"), Index + 1), Record.Namespace == EDreamNamespaceMode::Session);
				TestEqual(*FString::Printf(TEXT("run %d first line"), Index + 1), Record.FirstLine, Press.FirstLine);
				TestEqual(*FString::Printf(TEXT("run %d last line"), Index + 1), Record.LastLine, Press.LastLine);
				TestEqual(*FString::Printf(TEXT("run %d echoes from its first line"), Index + 1),
					Run->OfType(dream::LineType::Echo).Num() > 0 ? Run->OfType(dream::LineType::Echo)[0].EchoLine : 0, Press.FirstLine);

				// The caret moved when Submit accepted the press and stayed through the run (a run makes the view read-only).
				int32 CaretLine = -1;
				int32 CaretColumn = -1;
				Tab->FindCodeView(BufferId)->GetCaret(CaretLine, CaretColumn);
				TestEqual(*FString::Printf(TEXT("press %d: caret line (0-based) after the press"), Index + 1), CaretLine, Press.CaretAfter);
				TestEqual(*FString::Printf(TEXT("press %d: caret at the next statement's first non-blank column"), Index + 1), CaretColumn, 0);
				return true;
			});
		}

		Steps.Do(TEXT("check the walk"), [this, Runs]()
		{
			TestTrue(*FString::Printf(TEXT("the statements ran once each, in order: %s"), *Runs.Last()->Describe()),
				Runs.Last()->HasLineContaining(dream::LineType::Stdout, TEXT("[0, 1, 10, 3]")));
			return true;
		});
		Steps.Run(Done);
	});
}
