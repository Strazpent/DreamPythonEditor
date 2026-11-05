// SPDX-License-Identifier: Apache-2.0
// Dream.Supervisor.StopPress (run group; architect's ruling after PO-04, A216; TDD §5.8 Stop record, T-CAN-020's shape).
// The Run window's Stop button acts on the press. The bot presses Stop 0.5 s into a 3 s blocking native call and its job
// ends at once, so the cursor leaves Stop while the game thread is still blocked, as the T-SUP-002 bot script did before
// PO-04. Slate reads the click after the call returns, when its hover already follows the restored cursor: a
// release-click (the SButton default) needs hover and is lost there, which would leave the run going to its end. The
// press must still request Stop (from the Run window's button), and the record must say Stop came during the native call
// at line 4. If the Stop were lost, the script ends on its own about 3 s after the call, so the case fails and never hangs.
// A bot check that may query an editor window (the window under a point, a raise) waits for the game thread, so a bot that
// checked during the call would press only after it returned (tests, r11: comment). The checks run before the call; during it the bot only moves the cursor and calls SendInput.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Fixtures/DreamTestLatch.h"
#include "Fixtures/DreamTestProbes.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Python/DreamTestPython.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace DreamRunStopPressSpec
{
	constexpr double NativeSeconds = 3.0;       // the blocking call (well under the 5 s after which Windows ghosts a window)
	constexpr double PressAfterSeconds = 0.5;   // into the call
	constexpr int32 NativeLine = 4;
}

BEGIN_DEFINE_SPEC(FDreamRunStopPressSpec, "Dream.Supervisor.StopPress", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	FString Sfx;
	bool bSettingsSaved = false;
	bool bSavedRunPump = true;
	EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
END_DEFINE_SPEC(FDreamRunStopPressSpec)

void FDreamRunStopPressSpec::Define()
{
	BeforeEach([this]()
	{
		Sfx = DreamTest::UniqueSuffix();
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		if (bSettingsSaved)
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Settings->bRunPump = bSavedRunPump;
			Settings->RunPresentation = SavedPresentation;
			bSettingsSaved = false;
		}
		DreamTest::FDreamTestLatch::ForgetPrefix(TEXT("ssp_"));
		Tab.Reset();
	});

	LatentIt(TEXT("requests and records a Stop pressed during a blocking native call, the cursor leaving Stop at once (A216)"),
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamRunStopPressSpec;
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this)
			|| !TestTrue(TEXT("the _dream_test fixture module exists"), DreamTestPython::IsModuleCreated())
			|| !DreamTest::RequireEditorInFront(*this))
		{
			Done.Execute();
			return;
		}
		const FString Started = FString::Printf(TEXT("ssp_native_%s"), *Sfx);
		const FString Source = FString::Printf(TEXT(
			"import _dream_test, time\n"
			"for _ in range(60): time.sleep(0.01)\n"
			"_dream_test.mark('%s')\n"
			"_dream_test.sleep_native(%d)\n"
			"for _ in range(300): time.sleep(0.01)"), *Started, static_cast<int32>(NativeSeconds));
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Source, TEXT("StopPress"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<TSharedPtr<DreamTest::FDreamInputBotTicket>> Ticket = MakeShared<TSharedPtr<DreamTest::FDreamInputBotTicket>>();

		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(90.0);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("submit the run and the bot's press"), [this, Buffer, Run, Ticket, Started]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			bSavedRunPump = Settings->bRunPump;
			SavedPresentation = Settings->RunPresentation;
			bSettingsSaved = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 5));
			if (Run->RunNumber <= 0)
			{
				return false;
			}
			const int32 Number = Run->RunNumber;
			*Ticket = DreamTest::FDreamInputBot::Submit([Number, Started](DreamTest::FDreamInputBotContext& Bot)
			{
				if (!Bot.WaitFor([Number](const DreamTest::FDreamProbeSnapshot& S)
					{
						return S.ActiveRunNumber == Number && S.Modal == DreamTest::EDreamModalKind::RunWindow && S.StopButton.GetSize().X >= 1.f;
					}, 15.0, TEXT("the Run window with a painted Stop button")))
				{
					return;
				}
				// Before the call (the game thread still pumps): the foreground and an uncovered point on Stop. The cursor stays
				// where it is, so no pump sees it over Stop.
				const DreamTest::FDreamProbeSnapshot S = DreamTest::FDreamTestProbes::GetSnapshot();
				FVector2D Point(S.StopButton.GetCenter());
				if (!Bot.RequireForeground())
				{
					return;
				}
				if (!Bot.FindUncoveredPointRaising(S.StopButton, S.RunWindowHwnd, Point, TEXT("Stop")))   // (tests, r11) raise fallback
				{
					Bot.Fail(TEXT("input bot: another window covers the Run window's Stop button"));
					return;
				}
				if (!Bot.WaitUntil([Started]() { return DreamTest::FDreamTestLatch::GetMark(Started) > 0.0; }, 10.0, TEXT("the native call to start")))
				{
					return;
				}
				FPlatformProcess::Sleep(static_cast<float>(PressAfterSeconds));
#if PLATFORM_WINDOWS
				if (!::SetCursorPos(FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y)))
				{
					Bot.Fail(TEXT("input bot: SetCursorPos onto Stop failed"));
					return;
				}
#endif
				const double Pressed = Bot.LeftClick();
				// No wait: the job ends here and puts the cursor back while the call still blocks the game thread.
				Bot.Note(FString::Printf(TEXT("@Pressed=%.9f"), Pressed));
				Bot.Note(FString::Printf(TEXT("@Left=%.9f"), FPlatformTime::Seconds()));
			});
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("StopPress"), Run, 60.0);
		Steps.Poll(TEXT("the bot's job"), [Ticket]()
		{
			return !Ticket->IsValid() || DreamTest::IsTicketDone(Ticket->ToSharedRef()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Do(TEXT("check"), [this, Run, Ticket, Started]()
		{
			if (!Ticket->IsValid() || !DreamTest::ReportTicket(*this, Ticket->ToSharedRef(), TEXT("input bot")))
			{
				return false;
			}
			const double Mark = DreamTest::FDreamTestLatch::GetMark(Started);
			const double* Pressed = (*Ticket)->Times.Find(FName(TEXT("Pressed")));
			const double* Left = (*Ticket)->Times.Find(FName(TEXT("Left")));
			if (!TestTrue(TEXT("the bot pressed Stop and ended its job"), Pressed != nullptr && Left != nullptr && *Pressed > 0.0))
			{
				return false;
			}
			AddInfo(FString::Printf(TEXT("pressed %.2f s and left %.2f s into the %.0f s native call"), *Pressed - Mark, *Left - Mark, NativeSeconds));
			TestTrue(TEXT("the press and the cursor's return both happened while the native call blocked the game thread"),
				Mark > 0.0 && *Left - Mark < NativeSeconds - 0.5);
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Cancelled))
			{
				return true;
			}
			const FDreamRunRecord& Record = Run->Record;
			const FDreamRunStats& Stats = Record.Stats;
			AddInfo(FString::Printf(TEXT("Stop source %s; Stop during native: %d, %.2f s at line %d"), DreamRunText::StopSourceText(Record.StopSource),
				Stats.bStopDuringNative ? 1 : 0, Stats.StopNativeSeconds, Stats.StopNativeLine));
			TestTrue(TEXT("Stop was requested from the Run window's Stop button"), Record.bStopRequested && Record.StopSource == EDreamStopSource::RunWindowButton);
			TestTrue(TEXT("the record says Stop came during a native call (TDD 5.8 footer)"), Stats.bStopDuringNative);
			TestEqual(TEXT("the native call's line"), Stats.StopNativeLine, NativeLine);
			TestTrue(*FString::Printf(TEXT("about %.0f s"), NativeSeconds), FMath::Abs(Stats.StopNativeSeconds - NativeSeconds) <= 0.25);
			TestTrue(TEXT("delivered at the return: the run ended after the native call"), Stats.StopEffectiveAt >= Mark + NativeSeconds - 0.1);
			return true;
		});
		Steps.Run(Done);
	});
}
