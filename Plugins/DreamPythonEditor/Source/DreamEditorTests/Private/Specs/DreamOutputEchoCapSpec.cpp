// SPDX-License-Identifier: Apache-2.0
// Dream.Output.EchoCap: T-OUT-020, the Output Log echo cap (TDD §6.3 rule 5, D19, OUT-01; slice B item B93). A REPL run
// prints 20,000 numbered lines per second for 5 s with the cap at 5,000 lines/s:
//   - at most 5,000 of its lines reach LogPython in each 1 s window, and each window that held lines back ends with one
//     LogPython warning "[Dream] Run #N: n lines not echoed in the last second (echo cap 5,000/s). Full output: <path>";
//   - the echoed and the summarised lines account for all 100,000; every summary names run N and the run's log, which
//     exists and holds all 100,000 lines (suppression forces a run log, §6.2);
//   - with the cap at 0 (unlimited) a run of 20,000 lines in one second echoes every line and no summary.
// A probe device on GLog sees the forwarder's lines in order, on the DreamOutput thread. The forwarder's windows are in
// forward time: each lasts at least 1 s, the one a summary closes starts no later than 1 s before it, and a window that
// held nothing back ends without a summary. So the echo between two summaries Dt seconds apart spans at most floor(Dt)
// windows (one when the ingest keeps up with the run), and is checked against the cap times that many.
#include "Support/DreamTestUtil.h"

#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamOutputStore.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamTab.h"

namespace DreamOutputEchoCapSpec
{
	constexpr int32 Cap = 5000;
	constexpr int32 LinesPerSecond = 20000;
	constexpr int32 Seconds = 5;
	constexpr int32 Total = LinesPerSecond * Seconds;

	/** Counts the forwarder's LogPython lines that start with Marker, and cuts them into the runs between two summaries. */
	class FProbe : public FOutputDevice
	{
	public:
		explicit FProbe(const FString& InMarker)
			: Marker(InMarker)
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FProbe() override
		{
			if (GLog != nullptr)
			{
				GLog->RemoveOutputDevice(this);
			}
		}

		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			static const FName LogPython(TEXT("LogPython"));
			if (Text == nullptr || Category != LogPython)
			{
				return;
			}
			const FStringView View(Text);
			if (View.StartsWith(Marker))
			{
				FScopeLock Guard(&Lock);
				++Echoed;
				++Current;
			}
			else if (View.Contains(TEXT("lines not echoed in the last second (echo cap")))
			{
				const double Now = FPlatformTime::Seconds();
				FScopeLock Guard(&Lock);
				Segments.Add(Current);
				SummaryTimes.Add(Now);
				Current = 0;
				Summaries.Add(FString(View));
			}
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		void Read(int32& OutEchoed, TArray<int32>& OutSegments, int32& OutTrailing, TArray<FString>& OutSummaries) const
		{
			FScopeLock Guard(&Lock);
			OutEchoed = Echoed;
			OutSegments = Segments;
			OutTrailing = Current;
			OutSummaries = Summaries;
		}

		/** The most windows the echo before summary Index (Index == summary count: after the last one, until now) spans. */
		int32 WindowsBefore(int32 Index) const
		{
			FScopeLock Guard(&Lock);
			const double End = SummaryTimes.IsValidIndex(Index) ? SummaryTimes[Index] : FPlatformTime::Seconds();
			if (Index == 0 || !SummaryTimes.IsValidIndex(Index - 1))
			{
				// From the probe's start, which may fall inside a window that started earlier: one more.
				return FMath::FloorToInt(End - ArmSeconds) + 1;
			}
			const double Gap = End - SummaryTimes[Index - 1];
			return SummaryTimes.IsValidIndex(Index) ? FMath::Max(1, FMath::FloorToInt(Gap + 0.01)) : FMath::FloorToInt(Gap) + 1;
		}

	private:
		FString Marker;
		mutable FCriticalSection Lock;
		int32 Echoed = 0;
		int32 Current = 0;
		TArray<int32> Segments;
		TArray<double> SummaryTimes;
		TArray<FString> Summaries;
		double ArmSeconds = FPlatformTime::Seconds();
	};

	/** "[Dream] Run #22: 48,000 lines not echoed ... Full output: <path>" -> 22, 48000, <path>. */
	bool ParseSummary(const FString& Summary, int32& OutRun, int64& OutCount, FString& OutPath)
	{
		const int32 RunAt = Summary.Find(TEXT("Run #"));
		const int32 Colon = RunAt != INDEX_NONE ? Summary.Find(TEXT(": "), ESearchCase::CaseSensitive, ESearchDir::FromStart, RunAt) : INDEX_NONE;
		const int32 Lines = Colon != INDEX_NONE ? Summary.Find(TEXT(" lines not echoed"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Colon) : INDEX_NONE;
		const int32 PathAt = Summary.Find(TEXT("Full output: "));
		if (Lines == INDEX_NONE || PathAt == INDEX_NONE)
		{
			return false;
		}
		OutRun = FCString::Atoi(*Summary.Mid(RunAt + 5, Colon - RunAt - 5));
		const FString Count = Summary.Mid(Colon + 2, Lines - Colon - 2).Replace(TEXT(","), TEXT(""));
		OutCount = FCString::Atoi64(*Count);
		OutPath = Summary.Mid(PathAt + 13).TrimStartAndEnd();
		return true;
	}

	/** The REPL code: Count lines per burst, one burst per second (time.perf_counter pacing), Bursts bursts. */
	FString MakeCode(const FString& Marker, int32 Bursts)
	{
		return FString::Printf(TEXT(
			"import time as _ec_time\n"
			"_ec_t0 = _ec_time.perf_counter()\n"
			"for _ec_s in range(%d):\n"
			"    for _ec_i in range(%d):\n"
			"        print(f'%s {_ec_s * %d + _ec_i}')\n"
			"    _ec_left = _ec_t0 + _ec_s + 1 - _ec_time.perf_counter()\n"
			"    if _ec_left > 0:\n"
			"        _ec_time.sleep(_ec_left)\n"),
			Bursts, LinesPerSecond, *Marker, LinesPerSecond);
	}

	/**
	 * Submits the run and waits until it finished and its whole block is in the ring. (DreamTest::AddRun copies the block
	 * out of the ring every frame while it drains, which for 100,000 lines can outlast its fixed 10 s output wait.)
	 */
	void AddSubmitAndWait(DreamTest::FDreamSteps& Steps, const FString& Name, TFunction<FDreamRunRequest()> MakeRequest,
		TSharedRef<DreamTest::FRunResult> Run)
	{
		FAutomationTestBase& Test = Steps.GetTest();
		Steps.Do(FString::Printf(TEXT("submit %s"), *Name), [&Test, MakeRequest, Run]()
		{
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(Test, MakeRequest());
			return Run->RunNumber > 0;
		});
		Steps.Poll(FString::Printf(TEXT("%s finishes and its block is drained"), *Name), [Run]()
		{
			FDreamEditorModule& Module = FDreamEditorModule::Get();
			FDreamOutputStore& Store = Module.GetOutputStore();
			Store.Drain(0.02);
			if (!DreamTest::IsRunFinished(Run->RunNumber))
			{
				return DreamTest::EStep::Wait;
			}
			const FDreamRunSpan* Span = Store.FindRunSpan(static_cast<uint32>(Run->RunNumber));
			if (Span == nullptr || Span->FooterSeq == 0)
			{
				return DreamTest::EStep::Wait;
			}
			if (const FDreamRunRecord* Record = Module.GetRunService().FindRecord(Run->RunNumber))
			{
				Run->Record = *Record;
			}
			Run->bFinished = true;
			return DreamTest::EStep::Next;
		}, 90.0);
	}

	struct FState
	{
		TSharedPtr<FProbe> Probe;
		FString Marker;
		FString LogText;
		bool bCapSet = false;
	};
}

BEGIN_DEFINE_SPEC(FDreamOutputEchoCapSpec, "Dream.Output.EchoCap", DreamTest::AttendedFlags)
END_DEFINE_SPEC(FDreamOutputEchoCapSpec)

void FDreamOutputEchoCapSpec::Define()
{
	LatentIt("echoes at most 5,000 lines per second with one summary per window naming the full run log (T-OUT-020)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamOutputEchoCapSpec;
		if (!DreamTest::RequireTab(*this).IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		// The summaries are LogPython warnings: the lines under test.
		AddExpectedMessagePlain(TEXT("lines not echoed in the last second (echo cap"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 0);
		TSharedRef<FState> State = MakeShared<FState>();
		State->Marker = FString::Printf(TEXT("ec_%s "), *DreamTest::UniqueSuffix());
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("cap the echo at 5,000 lines/s and arm the probe"), [State]()
		{
			FDreamEditorModule::Get().GetOutputStore().SetEchoCap(Cap);
			State->bCapSet = true;
			State->Probe = MakeShared<FProbe>(State->Marker);
			return true;
		});
		Steps.Poll(TEXT("the forwarder applies the cap"), []()
		{
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			Store.Drain(0.002);
			return Store.GetCounters().EchoCap == Cap ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		AddSubmitAndWait(Steps, TEXT("20,000 lines per second for 5 s"), [State]()
		{
			return DreamTest::MakeReplRequest(MakeCode(State->Marker, Seconds));
		}, Run);
		Steps.Poll(TEXT("every line is echoed or summarised, and the run log is complete"), [State, Run]()
		{
			int32 Echoed = 0;
			int32 Trailing = 0;
			TArray<int32> Segments;
			TArray<FString> Summaries;
			State->Probe->Read(Echoed, Segments, Trailing, Summaries);
			int64 Suppressed = 0;
			for (const FString& Summary : Summaries)
			{
				int32 RunNumber = 0;
				int64 Count = 0;
				FString Path;
				if (ParseSummary(Summary, RunNumber, Count, Path) && RunNumber == Run->RunNumber)
				{
					Suppressed += Count;
				}
			}
			const FString Path = FDreamEditorModule::Get().GetOutputStore().GetRunLogPath(static_cast<uint32>(Run->RunNumber));
			State->LogText.Reset();
			const bool bLogDone = !Path.IsEmpty() && FFileHelper::LoadFileToString(State->LogText, *Path) && State->LogText.Contains(TEXT("# end"));
			return bLogDone && Echoed + Suppressed >= Total ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 20.0);
		Steps.Do(TEXT("check the windows, the summaries and the run log"), [this, State, Run]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			int32 Echoed = 0;
			int32 Trailing = 0;
			TArray<int32> Segments;
			TArray<FString> Summaries;
			State->Probe->Read(Echoed, Segments, Trailing, Summaries);
			AddInfo(FString::Printf(TEXT("echoed %d lines; %d summaries; per window: %s; after the last summary: %d"), Echoed, Summaries.Num(),
				*FString::JoinBy(Segments, TEXT(", "), [](int32 Value) { return FString::FromInt(Value); }), Trailing));
			for (int32 Index = 0; Index <= Segments.Num(); ++Index)
			{
				const int32 Lines = Index < Segments.Num() ? Segments[Index] : Trailing;
				const int32 Windows = State->Probe->WindowsBefore(Index);
				TestTrue(*FString::Printf(TEXT("the echo before summary %d (%d lines, at most %d window(s)) is within %d lines per window"),
					Index + 1, Lines, Windows, Cap), Lines <= Cap * Windows);
			}
			TestTrue(*FString::Printf(TEXT("the windows that held lines back ended with a summary (%d)"), Summaries.Num()), Summaries.Num() >= 1);

			const FString RunLog = FDreamEditorModule::Get().GetOutputStore().GetRunLogPath(static_cast<uint32>(Run->RunNumber));
			int64 Suppressed = 0;
			for (const FString& Summary : Summaries)
			{
				int32 RunNumber = 0;
				int64 Count = 0;
				FString Path;
				if (!TestTrue(*FString::Printf(TEXT("the summary reads as §6.3 words it: %s"), *Summary), ParseSummary(Summary, RunNumber, Count, Path)))
				{
					continue;
				}
				TestEqual(TEXT("the summary names the run"), RunNumber, Run->RunNumber);
				TestTrue(*FString::Printf(TEXT("the summary names the run's log (%s)"), *Path), FPaths::IsSamePath(Path, RunLog));
				TestTrue(*FString::Printf(TEXT("the summary says the cap: %s"), *Summary), Summary.Contains(TEXT("(echo cap 5,000/s)")));
				Suppressed += Count;
			}
			// The run's header and footer summaries go through the same cap (LogDream), so a few more may be counted.
			TestTrue(*FString::Printf(TEXT("echoed (%d) and summarised (%lld) lines account for all %d"), Echoed, Suppressed, Total),
				Echoed + Suppressed >= Total && Echoed + Suppressed <= Total + 5);
			TestTrue(TEXT("the cap held lines back"), Echoed < Total);

			TestTrue(*FString::Printf(TEXT("the run log exists: %s"), *RunLog), FPaths::FileExists(RunLog));
			TArray<FString> LogLines;
			State->LogText.ParseIntoArrayLines(LogLines);
			int32 InLog = 0;
			for (const FString& Line : LogLines)
			{
				InLog += Line.Contains(State->Marker) && !Line.Contains(TEXT("echo [")) ? 1 : 0;
			}
			TestEqual(TEXT("the run log holds every line (§6.2: suppression forces a run log)"), InLog, Total);
			return true;
		});
		Steps.Finally(TEXT("restore the project's cap and remove the probe"), [State]()
		{
			State->Probe.Reset();
			if (State->bCapSet && FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetOutputStore().ClearEchoCap();
			}
		});
		Steps.Run(Done);
	});

	LatentIt("echoes every line with the cap at 0 (T-OUT-020)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamOutputEchoCapSpec;
		if (!DreamTest::RequireTab(*this).IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		TSharedRef<FState> State = MakeShared<FState>();
		State->Marker = FString::Printf(TEXT("ec0_%s "), *DreamTest::UniqueSuffix());
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("no cap, and arm the probe"), [State]()
		{
			FDreamEditorModule::Get().GetOutputStore().SetEchoCap(0);
			State->bCapSet = true;
			State->Probe = MakeShared<FProbe>(State->Marker);
			return true;
		});
		Steps.Poll(TEXT("the forwarder applies no cap"), []()
		{
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			Store.Drain(0.002);
			return Store.GetCounters().EchoCap == 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		AddSubmitAndWait(Steps, TEXT("20,000 lines in one second"), [State]()
		{
			return DreamTest::MakeReplRequest(MakeCode(State->Marker, 1));
		}, Run);
		Steps.Poll(TEXT("every line is echoed"), [State]()
		{
			int32 Echoed = 0;
			int32 Trailing = 0;
			TArray<int32> Segments;
			TArray<FString> Summaries;
			State->Probe->Read(Echoed, Segments, Trailing, Summaries);
			return Echoed >= LinesPerSecond ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		// A window that held lines back would end with its summary within the next second.
		Steps.Poll(TEXT("a second for a summary to show"), [Start = MakeShared<double>(0.0)]()
		{
			const double Now = FPlatformTime::Seconds();
			*Start = *Start == 0.0 ? Now : *Start;
			return Now - *Start >= 1.2 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("check"), [this, State, Run]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			int32 Echoed = 0;
			int32 Trailing = 0;
			TArray<int32> Segments;
			TArray<FString> Summaries;
			State->Probe->Read(Echoed, Segments, Trailing, Summaries);
			TestEqual(TEXT("with cap 0 every line is echoed"), Echoed, LinesPerSecond);
			TestEqual(TEXT("... and nothing is summarised"), Summaries.Num(), 0);
			return true;
		});
		Steps.Finally(TEXT("restore the project's cap and remove the probe"), [State]()
		{
			State->Probe.Reset();
			if (State->bCapSet && FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetOutputStore().ClearEchoCap();
			}
		});
		Steps.Run(Done);
	});
}
