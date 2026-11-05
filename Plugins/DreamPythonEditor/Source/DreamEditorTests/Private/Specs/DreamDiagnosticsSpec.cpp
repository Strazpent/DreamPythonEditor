// SPDX-License-Identifier: Apache-2.0
// Crash breadcrumbs and Insights channels (TDD §11.8.1, §10.9, §13.1, D26, TEL-03, TEL-04; run group, B31, B39):
//   Dream.Diagnostics.CrashContext (attended) every Dream.* SetGameData call of a REPL run and a buffer run, seen through
//       FGenericCrashContext::OnGameDataSetDelegate: made on the game thread only; Dream.State follows U2 (Starting,
//       Running, Idle); during the run Dream.Run is "<N>/<session prefix>", Dream.Route "user/<kind>", Dream.InUserCode 1 then
//       0, Dream.LastPumpMs written by a pump, Dream.TabHash the first 8 hex digits of SHA-256 over the tab ID; after the run
//       the run keys are gone and Dream.Version and Dream.Pause remain (T-TEL-001, T-API-015; the forced crash of T-TEL-002
//       is tier 6).
//   Dream.Diagnostics.Trace (any editor) the DreamPump and DreamRun trace channels exist (TEL-03; T-GAP-017's capture is the
//       nightly job).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Diagnostics/DreamAudit.h"
#include "Diagnostics/DreamCrashContext.h"
#include "Diagnostics/DreamTrace.h"
#include "GenericPlatform/GenericPlatformCrashContext.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Shell/SDreamTab.h"
#include "dream/Checksum.h"

namespace DreamDiagnosticsSpec
{
	struct FSet
	{
		FString Key;
		FString Value;
		bool bGameThread = true;
	};

	struct FRecorder
	{
		TArray<FSet> Sets;
		FDelegateHandle Handle;

		void Begin()
		{
			Handle = FGenericCrashContext::OnGameDataSetDelegate().AddLambda([this](const FString& Key, const FString& Value)
			{
				if (Key.StartsWith(TEXT("Dream.")))
				{
					Sets.Add(FSet{ Key, Value, IsInGameThread() });
				}
			});
		}

		void End()
		{
			FGenericCrashContext::OnGameDataSetDelegate().Remove(Handle);
			Handle.Reset();
		}

		TArray<FString> ValuesOf(const TCHAR* Key) const
		{
			TArray<FString> Values;
			for (const FSet& Set : Sets)
			{
				if (Set.Key == Key)
				{
					Values.Add(Set.Value);
				}
			}
			return Values;
		}
	};

	FString Current(const TCHAR* Key)
	{
		const FString* Value = FGenericCrashContext::GetGameData().Find(Key);
		return Value != nullptr ? *Value : FString();
	}

	FString TabHash(const FGuid& Id)
	{
		const std::string Text = DreamText::ToUtf8(DreamText::GuidToHex(Id));
		dream::Sha256 Hasher;
		Hasher.Update(Text.data(), Text.size());
		return DreamText::Utf8ToFString(dream::Sha256::ToHex(Hasher.Final())).Left(8);
	}
}

BEGIN_DEFINE_SPEC(FDreamCrashContextSpec, "Dream.Diagnostics.CrashContext", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	TSharedPtr<DreamDiagnosticsSpec::FRecorder> Recorder;
END_DEFINE_SPEC(FDreamCrashContextSpec)

void FDreamCrashContextSpec::Define()
{
	using namespace DreamDiagnosticsSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		if (Recorder.IsValid())
		{
			Recorder->End();
			Recorder.Reset();
		}
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		Tab.Reset();
	});

	LatentIt("sets every breadcrumb during a run on the game thread and removes the run keys after it (T-TEL-001, T-API-015)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("import time\ntime.sleep(1.4)\nprint('crash-context-buffer')"),
			TEXT("CrashContext"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		Recorder = MakeShared<FRecorder>();
		TSharedRef<DreamTest::FRunResult> Repl = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> BufferRun = MakeShared<DreamTest::FRunResult>();
		const FString Prefix = FDreamEditorModule::Get().GetAudit().GetSessionPrefix();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("record the Dream Python Editor crash keys"), [this]()
		{
			Recorder->Begin();
			return true;
		});
		// Pumps begin about 300 ms into a run (§5.4.1); 1.4 s of sleep gives the 1 Hz LastPumpMs key a write.
		DreamTest::AddRun(Steps, TEXT("the REPL run"), []() { return DreamTest::MakeReplRequest(TEXT("import time\ntime.sleep(1.4)\nprint('crash-context-repl')")); }, Repl, 60.0);
		Steps.Do(TEXT("check the REPL run's keys"), [this, Repl, Prefix]()
		{
			DreamTest::ExpectOutcome(*this, *Repl, EDreamRunOutcome::Ok);
			for (const FSet& Set : Recorder->Sets)
			{
				if (!Set.bGameThread)
				{
					AddError(FString::Printf(TEXT("%s was set off the game thread"), *Set.Key));
				}
			}
			const FString Run = FString::Printf(TEXT("%d/%s"), Repl->RunNumber, *Prefix);
			TestTrue(*FString::Printf(TEXT("Dream.Run was %s"), *Run), Recorder->ValuesOf(TEXT("Dream.Run")).Contains(Run));
			TestTrue(TEXT("Dream.Route was user/repl"), Recorder->ValuesOf(TEXT("Dream.Route")).Contains(TEXT("user/repl")));
			const TArray<FString> InUserCode = Recorder->ValuesOf(TEXT("Dream.InUserCode"));
			const int32 One = InUserCode.Find(TEXT("1"));
			const int32 Zero = InUserCode.Find(TEXT("0"));
			TestTrue(*FString::Printf(TEXT("Dream.InUserCode 1 then 0: %s"), *FString::Join(InUserCode, TEXT(","))), One != INDEX_NONE && Zero != INDEX_NONE && One < Zero);
			const TArray<FString> Pumps = Recorder->ValuesOf(TEXT("Dream.LastPumpMs"));
			bool bPumped = false;
			for (const FString& Ms : Pumps)
			{
				bPumped |= FCString::Atoi(*Ms) > 0;
			}
			TestTrue(*FString::Printf(TEXT("Dream.LastPumpMs was written by a pump: %s"), *FString::Join(Pumps, TEXT(","))), bPumped);
			TestTrue(TEXT("at most 1 Hz: no more than three pump writes over 1.4 s"), Pumps.Num() <= 4);
			const TArray<FString> States = Recorder->ValuesOf(TEXT("Dream.State"));
			const int32 Starting = States.Find(TEXT("Starting"));
			const int32 Running = States.Find(TEXT("Running"));
			const int32 Idle = States.FindLast(TEXT("Idle"));
			TestTrue(*FString::Printf(TEXT("Dream.State Starting, Running, Idle: %s"), *FString::Join(States, TEXT(","))),
				Starting != INDEX_NONE && Running > Starting && Idle > Running);
			// After the run: the run keys are removed (§11.8.1); Version, State and Pause stay.
			for (const TCHAR* Key : { TEXT("Dream.Run"), TEXT("Dream.Route"), TEXT("Dream.TabHash"), TEXT("Dream.InUserCode"), TEXT("Dream.LastPumpMs") })
			{
				TestTrue(*FString::Printf(TEXT("%s is removed after the run"), Key), FGenericCrashContext::GetGameData().Find(Key) == nullptr);
			}
			TestFalse(TEXT("Dream.Version is set"), Current(TEXT("Dream.Version")).IsEmpty());
			TestEqual(TEXT("Dream.State is Idle"), Current(TEXT("Dream.State")), FString(TEXT("Idle")));
			TestEqual(TEXT("Dream.Pause is none (no pause in M1)"), Current(TEXT("Dream.Pause")), FString(TEXT("none")));
			Recorder->Sets.Reset();
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the buffer run"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 3); }, BufferRun);
		Steps.Do(TEXT("check the buffer run's keys"), [this, BufferRun, Buffer]()
		{
			DreamTest::ExpectOutcome(*this, *BufferRun, EDreamRunOutcome::Ok);
			const FString Hash = TabHash(Buffer->GetId());
			TestTrue(*FString::Printf(TEXT("Dream.TabHash was %s"), *Hash), Recorder->ValuesOf(TEXT("Dream.TabHash")).Contains(Hash));
			TestTrue(TEXT("Dream.Route was user/buffer"), Recorder->ValuesOf(TEXT("Dream.Route")).Contains(TEXT("user/buffer")));
			TestTrue(TEXT("Dream.TabHash is removed after the run"), FGenericCrashContext::GetGameData().Find(TEXT("Dream.TabHash")) == nullptr);
			return true;
		});
		Steps.Finally(TEXT("stop recording"), [this]()
		{
			if (Recorder.IsValid())
			{
				Recorder->End();
			}
		});
		Steps.Run(Done);
	});
}

BEGIN_DEFINE_SPEC(FDreamTraceSpec, "Dream.Diagnostics.Trace", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamTraceSpec)

void FDreamTraceSpec::Define()
{
	It("registers the DreamPump and DreamRun Insights channels (TEL-03, TDD 13.1)", [this]()
	{
		TestTrue(TEXT("DreamPump and DreamRun are trace channels"), DreamTrace::AreChannelsRegistered());
		// The breadcrumb key list is the §11.8.1 table.
		TestEqual(TEXT("eight crash keys"), FDreamCrashContext::GetKeys().Num(), 8);
	});
}
