// SPDX-License-Identifier: Apache-2.0
// Dream.Output.EchoGuard: no line reaches the Unreal log twice and no echo of ours comes back into the output (TDD §6.3
// "never echo a line twice", the GDreamForwarding guard of FDreamOutputDevice::Serialize, §6.7 "Echo loop"; the "none of
// our echoed lines appear twice" row of T-OUT-021, which STATUS lists as T-OUT-023; slice B item B93). On the module's
// own store and device, with no run, no Python and no RHI, so it also runs headless:
//   1. a stdout line written as _dream.write writes it is forwarded to LogPython exactly once, and the ring holds it once,
//      as a Python line, never also as a line FDreamOutputDevice captured from LogPython;
//   2. FDreamLogForwarder::EchoNow (the console executor's input echo, §6.6) reaches LogPython once and never the ring;
//   3. a foreign LogPython line (not ours: no guard) is in the log once and is never forwarded back: the ring holds it at
//      most once, as a captured line ("Outside runs"; none while that capture is off or before the first Python start,
//      when it feeds the startup ring instead).
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Misc/OutputDeviceRedirector.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamLogForwarder.h"
#include "Output/DreamOutputStore.h"

namespace DreamOutputEchoGuardSpec
{
	struct FFound
	{
		int32 Count = 0;
		uint16 Flags = 0;
		dream::LineType Type = dream::LineType::Stdout;
	};

	/** Ring lines whose text contains Needle (the module's store, drained first). */
	FFound FindInRing(const FString& Needle)
	{
		FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		Store.Drain(0.01);
		FFound Found;
		dream::LineView View;
		for (uint64 Seq = Store.GetFirstSeq(); Seq < Store.GetEndSeq(); ++Seq)
		{
			if (Store.GetLine(Seq, View) && DreamText::Utf8ToFString(View.Text).Contains(Needle))
			{
				++Found.Count;
				Found.Flags = View.Info.Flags;
				Found.Type = View.Info.Type;
			}
		}
		return Found;
	}

	struct FState
	{
		TSharedPtr<DreamTest::FDreamTestLogCapture> Log;
		FString Written;
		FString Echoed;
		FString Foreign;
	};
}

BEGIN_DEFINE_SPEC(FDreamOutputEchoGuardSpec, "Dream.Output.EchoGuard", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamOutputEchoGuardSpec)

void FDreamOutputEchoGuardSpec::Define()
{
	LatentIt("echoes each line once and never captures its own echo (B93, T-OUT-021)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamOutputEchoGuardSpec;
		if (DreamTest::GetModule(*this) == nullptr)
		{
			Done.Execute();
			return;
		}
		TSharedRef<FState> State = MakeShared<FState>();
		const FString Sfx = DreamTest::UniqueSuffix();
		State->Written = FString::Printf(TEXT("guard_written_%s"), *Sfx);
		State->Echoed = FString::Printf(TEXT("guard_echonow_%s"), *Sfx);
		State->Foreign = FString::Printf(TEXT("guard_foreign_%s"), *Sfx);
		const FName LogPython(TEXT("LogPython"));

		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("write a stdout line, echo one line with EchoNow, log one foreign LogPython line"), [State, LogPython]()
		{
			State->Log = MakeShared<DreamTest::FDreamTestLogCapture>();
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			// 1. As _dream.write(STREAM_STDOUT, ...) pushes it (run 0: no run is bound, so it is tagged Background).
			const std::string Utf8 = DreamText::ToUtf8(State->Written + TEXT("\n"));
			Store.PushText(0, EDreamStream::Stdout, EDreamOutputOrigin::Python, reinterpret_cast<const UTF8CHAR*>(Utf8.data()),
				static_cast<int32>(Utf8.size()));
			// 2. The console executor's input echo path.
			FDreamForwardLine Line;
			Line.Category = LogPython;
			Line.Verbosity = ELogVerbosity::Log;
			Line.Text = State->Echoed;
			FDreamLogForwarder::EchoNow(Line);
			// 3. A foreign tool logging to LogPython (an Editor Utility Widget's print while idle).
			GLog->Log(LogPython, ELogVerbosity::Log, *State->Foreign);
			return true;
		});
		Steps.Poll(TEXT("the forwarder echoed the written line"), [State, LogPython]()
		{
			FDreamEditorModule::Get().GetOutputStore().Drain(0.01);
			return State->Log->CountContaining(LogPython, State->Written) > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		// Anything that would come back (a captured copy of our echo, a forwarded copy of the foreign line) has had time to
		// go round the ingest thread several times (its window is 5 ms) and to drain.
		Steps.WaitFrames(20);
		Steps.Do(TEXT("count every line in the log and in the ring"), [this, State, LogPython]()
		{
			const FString LogLines = State->Log->Describe(LogPython);
			TestEqual(*FString::Printf(TEXT("the written line is in LogPython once:%s"), *LogLines), State->Log->CountContaining(LogPython, State->Written), 1);
			const FFound Written = FindInRing(State->Written);
			TestEqual(TEXT("the ring holds the written line once"), Written.Count, 1);
			TestTrue(TEXT("... as stdout"), Written.Type == dream::LineType::Stdout);
			TestTrue(TEXT("... never as a line captured from LogPython (the device skipped our echo)"), (Written.Flags & dream::OLF_FromLogPython) == 0);

			TestEqual(TEXT("the EchoNow line is in LogPython once"), State->Log->CountContaining(LogPython, State->Echoed), 1);
			TestEqual(TEXT("the EchoNow line never reaches the ring (GDreamForwarding was set)"), FindInRing(State->Echoed).Count, 0);

			TestEqual(TEXT("the foreign line is in LogPython once (never forwarded back)"), State->Log->CountContaining(LogPython, State->Foreign), 1);
			const FFound Foreign = FindInRing(State->Foreign);
			TestTrue(*FString::Printf(TEXT("the ring holds the foreign line at most once (%d)"), Foreign.Count), Foreign.Count <= 1);
			if (Foreign.Count == 1)
			{
				TestTrue(TEXT("... as a line captured from LogPython"), (Foreign.Flags & dream::OLF_FromLogPython) != 0);
			}
			else
			{
				AddInfo(TEXT("the foreign line went to the startup ring or outside-runs capture is off"));
			}
			return true;
		});
		Steps.Finally(TEXT("release the log capture"), [State]()
		{
			State->Log.Reset();
		});
		Steps.Run(Done);
	});
}
