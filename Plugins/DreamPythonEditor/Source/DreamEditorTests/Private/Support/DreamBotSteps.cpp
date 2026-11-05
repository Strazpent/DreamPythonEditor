// SPDX-License-Identifier: Apache-2.0
#include "Support/DreamBotSteps.h"

#include "Fixtures/DreamTestLatch.h"
#include "Fixtures/DreamTestProbes.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"

namespace DreamTest
{
	const TCHAR* BotStopName(EBotStop Method)
	{
		switch (Method)
		{
		case EBotStop::Click: return TEXT("Stop click");
		case EBotStop::ShiftF5: return TEXT("Shift+F5");
		case EBotStop::Esc: return TEXT("Esc");
		case EBotStop::Api: return TEXT("API Stop");
		}
		return TEXT("?");
	}

	TSharedRef<FDreamInputBotTicket> SubmitBotStop(const FBotStopOptions& Options)
	{
		// The services outlive any job: the module is shut down only after the bot thread is joined.
		TSharedRef<FDreamRunService> RunService = FDreamEditorModule::Get().GetRunServiceRef();
		FRunSupervisor* Supervisor = &FDreamEditorModule::Get().GetSupervisor();
		TSharedRef<FDreamInputBotTicket> Ticket = FDreamInputBot::Submit([Options, RunService, Supervisor](FDreamInputBotContext& Bot)
		{
			const int32 Number = Options.RunNumber;
			const bool bShown = Bot.WaitFor([Number](const FDreamProbeSnapshot& S)
			{
				return S.ActiveRunNumber == Number && S.Modal == EDreamModalKind::RunWindow && S.StopButton.GetSize().X >= 1.f
					&& S.RunWindowHwnd != nullptr;
			}, Options.ShowTimeoutSeconds, FString::Printf(TEXT("run #%d's Run window with a painted Stop button"), Number));
			if (bShown && !Options.AwaitMark.IsEmpty())
			{
				const FString Mark = Options.AwaitMark;
				Bot.WaitUntil([Mark]() { return FDreamTestLatch::GetMark(Mark) > 0.0; }, Options.ShowTimeoutSeconds,
					FString::Printf(TEXT("the probe mark %s"), *Mark));
			}
			double Injected = 0.0;
			if (!Bot.HasFailed() && Options.Method == EBotStop::Api)
			{
				Injected = FPlatformTime::Seconds();
				RunService->RequestStopActive(EDreamStopSource::Api);
			}
			else if (!Bot.HasFailed() && Bot.RequireForeground())
			{
				const FDreamProbeSnapshot S = FDreamTestProbes::GetSnapshot();
				switch (Options.Method)
				{
				case EBotStop::Click:
					if (Bot.MoveTo(S.StopButton, S.RunWindowHwnd, TEXT("the Run window's Stop button")))
					{
						Injected = Bot.LeftClick();
					}
					break;
				case EBotStop::ShiftF5:
					Injected = Bot.Chord({ FDreamInputBot::VkShift(), FDreamInputBot::VkF5() });
					break;
				case EBotStop::Esc:
					Injected = Bot.Chord({ FDreamInputBot::VkEscape() });
					break;
				case EBotStop::Api:
					break;
				}
			}
			if (Injected > 0.0)
			{
				Bot.Note(FString::Printf(TEXT("injected %s at %.6f"), BotStopName(Options.Method), Injected));
			}
			const bool bSafety = Bot.HasFailed() && Options.bSafetyNet;
			if (bSafety)
			{
				// Any-thread Stop (the supervisor's atomics), so the case ends and the next can run.
				RunService->RequestStopActive(EDreamStopSource::Api);
			}
			if ((Injected > 0.0 || bSafety) && Options.DrainAfterSeconds > 0.0)
			{
				const double Deadline = FPlatformTime::Seconds() + Options.DrainAfterSeconds;
				while (RunService->GetActiveRunId() != 0 && FPlatformTime::Seconds() < Deadline)
				{
					FPlatformProcess::Sleep(0.005f);
				}
				if (RunService->GetActiveRunId() != 0)
				{
					// The run swallows Stop (§5.7.4). Keep the Interrupt cleanup flag set without pause: each pending call then
					// delivers once more, the fast-chain call right after a swallowed delivery included (§5.7.3).
					const double DrainedAt = FPlatformTime::Seconds();
					Bot.Note(FString::Printf(TEXT("@Drained=%.9f"), DrainedAt));
					const double DrainEnd = DrainedAt + 300.0;
					while (FPlatformTime::Seconds() < DrainEnd)
					{
						const FDreamRunId Id = RunService->GetActiveRunId();
						if (Id == 0)
						{
							break;
						}
						for (int32 Spin = 0; Spin < 4096; ++Spin)
						{
							Supervisor->RequestInterruptCleanup(Id);
						}
					}
					Bot.Note(FString::Printf(TEXT("the run did not end within %.0f s of the Stop and was drained with Interrupt cleanup for %.1f s"),
						Options.DrainAfterSeconds, FPlatformTime::Seconds() - DrainedAt));
				}
			}
			// Bot-thread write before bDone; the spec reads it after.
			Bot.Note(FString::Printf(TEXT("@Injected=%.9f"), Injected));
		});
		return Ticket;
	}

	bool IsTicketDone(const TSharedRef<FDreamInputBotTicket>& Ticket)
	{
		return Ticket->bDone.load();
	}

	bool ReportTicket(FAutomationTestBase& Test, const TSharedRef<FDreamInputBotTicket>& Ticket, const FString& What)
	{
		for (const FString& Note : Ticket->Notes)
		{
			if (Note.StartsWith(TEXT("@")))
			{
				FString Key, Value;
				if (Note.Mid(1).Split(TEXT("="), &Key, &Value))
				{
					Ticket->Times.Add(FName(*Key), FCString::Atod(*Value));
				}
			}
		}
		for (const FString& Error : Ticket->Errors)
		{
			Test.AddError(FString::Printf(TEXT("%s: %s"), *What, *Error));
		}
		return Ticket->Errors.Num() == 0;
	}

	int32 CommandLineInt(const TCHAR* Name, int32 Default)
	{
		int32 Value = Default;
		FParse::Value(FCommandLine::Get(), *FString::Printf(TEXT("-%s="), Name), Value);
		return Value;
	}

	FString CommandLineString(const TCHAR* Name)
	{
		FString Value;
		FParse::Value(FCommandLine::Get(), *FString::Printf(TEXT("-%s="), Name), Value);
		return Value;
	}

	double Percentile(TArray<double> Values, double P)
	{
		if (Values.Num() == 0)
		{
			return 0.0;
		}
		Values.Sort();
		const int32 Rank = FMath::Clamp(FMath::CeilToInt(P / 100.0 * Values.Num()), 1, Values.Num());
		return Values[Rank - 1];
	}

	FString DescribeMs(const TArray<double>& Seconds)
	{
		if (Seconds.Num() == 0)
		{
			return TEXT("n 0");
		}
		return FString::Printf(TEXT("n %d, p50 %.1f ms, p95 %.1f ms, p99 %.1f ms, max %.1f ms"), Seconds.Num(),
			Percentile(Seconds, 50.0) * 1000.0, Percentile(Seconds, 95.0) * 1000.0, Percentile(Seconds, 99.0) * 1000.0,
			Percentile(Seconds, 100.0) * 1000.0);
	}

	bool RequireEditorInFront(FAutomationTestBase& Test)
	{
		if (!FDreamInputBot::IsSupported())
		{
			Test.AddError(TEXT("The input bot needs Win64 (SendInput)"));
			return false;
		}
		if (!FDreamInputBot::IsDesktopInteractive())
		{
			Test.AddError(TEXT("The input desktop is locked or not interactive: SendInput needs an unlocked interactive session (TDD 14.1)"));
			return false;
		}
		FDreamTestProbes::PublishNow();
		if (FDreamInputBot::BringEditorToFront())
		{
			return true;
		}
		FString What;
		FDreamInputBot::IsEditorForeground(What);
		Test.AddError(FString::Printf(TEXT("The editor could not be brought to the front (the foreground window is %s). The input-bot specs "
			"send OS input with SendInput, which goes to the foreground window: click the DreamHost editor and keep the desktop free while "
			"run_tier4.bat --attended runs"), *What));
		return false;
	}
}
