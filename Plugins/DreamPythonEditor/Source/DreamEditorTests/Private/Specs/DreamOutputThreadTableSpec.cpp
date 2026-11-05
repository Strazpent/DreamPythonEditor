// SPDX-License-Identifier: Apache-2.0
// Dream.Output.ThreadTable: thread attribution keyed by (native thread ID, PyThreadState*) and the DreamOutput counters
// (TDD §6.1 "thread attribution", §6.2 "Budgets", D24, TEL-03; slice B items B90 and B91). A private FDreamOutputStore
// (no run logs, its own DreamOutput thread) is fed synthetic records that name a worker by a native ID no real thread has
// and by a PyThreadState key, as _dream's writers do:
//   1. a worker labelled and first seen during run A is tagged A; the same pair writing during run B is tagged A and
//      Background (T-OUT-012's "a thread from run 5 writing during run 7");
//   2. the same native ID with another PyThreadState during run B is a new thread (the OS reused the ID): tagged B, not
//      Background, under a new badge tag that does not carry the old thread's label, while the old lines keep theirs;
//   3. a device record (no PyThreadState) from that ID follows the current thread, and a label made after the reuse names
//      the new tag;
//   4. a thread alive at capture_begin is tagged 0 and Background;
//   5. the counters count committed lines, report a drain's cost and an empty queue once drained, and count the lines the
//      echo cap kept out of the log; the cap they report follows SetEchoCap and ClearEchoCap.
// Needs no tab, Python or RHI, so it also runs headless.
#include "Support/DreamTestUtil.h"

#include "HAL/PlatformTime.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Settings/DreamProjectSettings.h"

namespace DreamOutputThreadTableSpec
{
	// Run numbers no real run of the session has (FDreamRunService::FindRecord(int32) finds none), so the header and footer
	// summaries the ingest thread logs read "Run #N" / "Run #N ended" at Log verbosity.
	constexpr uint32 RunA = 2000000101;
	constexpr uint32 RunB = 2000000102;
	constexpr FDreamRunId IdA = 0x7fff000000000101ull;
	constexpr FDreamRunId IdB = 0x7fff000000000102ull;
	constexpr uint32 Worker = 0x7ffffff1;           // a native thread ID no OS thread has
	constexpr uint32 AliveThread = 0x7ffffff2;
	constexpr uint64 StateOld = 0x1000;            // two PyThreadState keys under the same native ID
	constexpr uint64 StateNew = 0x2000;
	constexpr uint64 StateAlive = 0x3000;

	FDreamRawRecord MakeWrite(uint32 NativeId, uint64 ThreadState, const FString& Text, bool bDevice = false)
	{
		FDreamRawRecord Record;
		Record.Kind = EDreamRawKind::Text;
		// Notice lines go to LogDream (§6.3 table); a device line is never forwarded (it is already in the log).
		Record.Stream = bDevice ? EDreamStream::Info : EDreamStream::Notice;
		Record.Origin = bDevice ? EDreamOutputOrigin::LogPython : EDreamOutputOrigin::Python;
		Record.Flags = bDevice ? static_cast<uint16>(dream::OLF_FromLogPython) : 0;
		Record.NativeThreadId = NativeId;
		Record.ThreadState = ThreadState;
		Record.Time = FPlatformTime::Seconds();
		const std::string Utf8 = DreamText::ToUtf8(Text + TEXT("\n"));
		Record.Text.Append(reinterpret_cast<const UTF8CHAR*>(Utf8.data()), static_cast<int32>(Utf8.size()));
		return Record;
	}

	struct FSeen
	{
		bool bFound = false;
		uint32 RunNumber = 0;
		uint16 Flags = 0;
		uint32 ThreadTag = 0;
	};

	/** Drains the private store and finds the line whose text is Text. */
	FSeen Find(FDreamOutputStore& Store, const FString& Text)
	{
		Store.Drain(0.01);
		FSeen Seen;
		dream::LineView View;
		for (uint64 Seq = Store.GetFirstSeq(); Seq < Store.GetEndSeq(); ++Seq)
		{
			if (Store.GetLine(Seq, View) && DreamText::Utf8ToFString(View.Text) == Text)
			{
				Seen.bFound = true;
				Seen.RunNumber = View.Info.RunNumber;
				Seen.Flags = View.Info.Flags;
				Seen.ThreadTag = View.Info.ThreadTag;
			}
		}
		return Seen;
	}

	struct FState
	{
		TUniquePtr<FDreamOutputStore> Store;
		FString Sfx;
		FString Line(const TCHAR* Name) const { return FString::Printf(TEXT("tt_%s_%s"), Name, *Sfx); }
		int64 LinesBefore = 0;
		int64 SuppressedBefore = 0;
	};
}

BEGIN_DEFINE_SPEC(FDreamOutputThreadTableSpec, "Dream.Output.ThreadTable", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamOutputThreadTableSpec)

void FDreamOutputThreadTableSpec::Define()
{
	LatentIt("attributes by (native ID, PyThreadState*) and detects a reused native ID (B90, T-OUT-012)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamOutputThreadTableSpec;
		TSharedRef<FState> State = MakeShared<FState>();
		State->Sfx = DreamTest::UniqueSuffix();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("start a private store and write run A"), [State]()
		{
			State->Store = MakeUnique<FDreamOutputStore>();
			FDreamOutputStore& Store = *State->Store;
			Store.Start(FString());             // no run logs
			Store.SetThreadLabel(Worker, TEXT("Worker-1"));
			Store.BeginRunBlock(RunA);
			Store.BindRunId(IdA, RunA);
			const uint64 Alive[] = { AliveThread };
			Store.CaptureBegin(IdA, Alive);
			Store.Push(MakeWrite(Worker, StateOld, State->Line(TEXT("a1"))));
			Store.Push(MakeWrite(AliveThread, StateAlive, State->Line(TEXT("pre"))));
			Store.CaptureEnd(IdA);
			Store.EndRunBlock(RunA);
			Store.BeginRunBlock(RunB);
			Store.BindRunId(IdB, RunB);
			Store.CaptureBegin(IdB, Alive);
			Store.Push(MakeWrite(Worker, StateOld, State->Line(TEXT("b_old"))));
			Store.Push(MakeWrite(Worker, StateNew, State->Line(TEXT("b_new"))));
			Store.Push(MakeWrite(Worker, 0, State->Line(TEXT("b_dev")), /*bDevice*/ true));
			return true;
		});
		Steps.Poll(TEXT("the ingest thread processed the writes"), [State]()
		{
			return Find(*State->Store, State->Line(TEXT("b_dev"))).bFound ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.Do(TEXT("label the new thread after the reuse, and write again"), [State]()
		{
			State->Store->SetThreadLabel(Worker, TEXT("Downloader"));
			State->Store->Push(MakeWrite(Worker, StateNew, State->Line(TEXT("b_named"))));
			State->Store->CaptureEnd(IdB);
			State->Store->EndRunBlock(RunB);
			return true;
		});
		Steps.Poll(TEXT("the last write is in the ring"), [State]()
		{
			return Find(*State->Store, State->Line(TEXT("b_named"))).bFound ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.Do(TEXT("check the attribution"), [this, State]()
		{
			FDreamOutputStore& Store = *State->Store;
			const FSeen A1 = Find(Store, State->Line(TEXT("a1")));
			const FSeen Pre = Find(Store, State->Line(TEXT("pre")));
			const FSeen BOld = Find(Store, State->Line(TEXT("b_old")));
			const FSeen BNew = Find(Store, State->Line(TEXT("b_new")));
			const FSeen BDev = Find(Store, State->Line(TEXT("b_dev")));
			const FSeen BNamed = Find(Store, State->Line(TEXT("b_named")));
			if (!TestTrue(TEXT("every line reached the ring"), A1.bFound && Pre.bFound && BOld.bFound && BNew.bFound && BDev.bFound && BNamed.bFound))
			{
				return false;
			}
			auto Background = [](const FSeen& Seen) { return (Seen.Flags & dream::OLF_Background) != 0; };

			TestEqual(TEXT("a worker started during run A is tagged A (§6.1)"), A1.RunNumber, RunA);
			TestFalse(TEXT("... and not Background while A runs"), Background(A1));
			TestEqual(TEXT("its label names its badge"), Store.GetThreadName(A1.ThreadTag), FString(TEXT("Worker-1")));

			TestEqual(TEXT("a thread alive at capture_begin is tagged 0 (§6.1)"), Pre.RunNumber, 0u);
			TestTrue(TEXT("... and Background"), Background(Pre));

			TestEqual(TEXT("the same pair writing during run B keeps its origin run A (T-OUT-012)"), BOld.RunNumber, RunA);
			TestTrue(TEXT("... and is Background (A ended)"), Background(BOld));
			TestEqual(TEXT("... under the same badge tag"), BOld.ThreadTag, A1.ThreadTag);

			TestEqual(TEXT("the same native ID with another PyThreadState is a new thread started during run B (B90)"), BNew.RunNumber, RunB);
			TestFalse(TEXT("... not Background"), Background(BNew));
			TestNotEqual(TEXT("... under a new badge tag"), BNew.ThreadTag, A1.ThreadTag);

			TestEqual(TEXT("a device record (no PyThreadState) follows the thread that has the ID now"), BDev.RunNumber, RunB);
			TestEqual(TEXT("... and its badge tag"), BDev.ThreadTag, BNew.ThreadTag);
			TestEqual(TEXT("a write after the label keeps the new thread's tag"), BNamed.ThreadTag, BNew.ThreadTag);
			TestEqual(TEXT("the label made after the reuse names the new tag"), Store.GetThreadName(BNew.ThreadTag), FString(TEXT("Downloader")));
			TestEqual(TEXT("the old thread's lines keep their name"), Store.GetThreadName(A1.ThreadTag), FString(TEXT("Worker-1")));
			return true;
		});
		Steps.Finally(TEXT("stop the private store"), [State]()
		{
			if (State->Store.IsValid())
			{
				State->Store->Shutdown();
				State->Store.Reset();
			}
		});
		Steps.Run(Done);
	});

	LatentIt("counts lines, drain cost and echo-suppressed lines (B91, TEL-03)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamOutputThreadTableSpec;
		// The cap's one summary per window is a LogPython warning (§6.3 rule 5): the line under test, not a failure.
		AddExpectedMessagePlain(TEXT("lines not echoed in the last second (echo cap 100/s)"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 0);
		TSharedRef<FState> State = MakeShared<FState>();
		State->Sfx = DreamTest::UniqueSuffix();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("start a private store with an echo cap of 100 lines/s"), [State]()
		{
			State->Store = MakeUnique<FDreamOutputStore>();
			State->Store->Start(FString());
			State->Store->SetEchoCap(100);
			return true;
		});
		Steps.Poll(TEXT("the forwarder applies the cap"), [State]()
		{
			State->Store->Drain(0.01);
			return State->Store->GetCounters().EchoCap == 100 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.Do(TEXT("push 1,000 lines in one burst"), [State]()
		{
			const FDreamOutputCounters Before = State->Store->GetCounters();
			State->LinesBefore = Before.LinesTotal;
			State->SuppressedBefore = Before.EchoSuppressedLines;
			for (int32 Index = 0; Index < 1000; ++Index)
			{
				State->Store->PushDreamLine(0, EDreamStream::Notice, FString::Printf(TEXT("tt_cap_%s %04d"), *State->Sfx, Index));
			}
			return true;
		});
		Steps.Poll(TEXT("the lines are drained and the counters published"), [State]()
		{
			State->Store->Drain(0.01);
			const FDreamOutputCounters Now = State->Store->GetCounters();
			return Now.LinesTotal - State->LinesBefore >= 1000 && Now.QueueBytes == 0 && Now.ViewBacklogLines == 0
				&& Now.EchoSuppressedLines > State->SuppressedBefore ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.Do(TEXT("check the counters"), [this, State]()
		{
			const FDreamOutputCounters Now = State->Store->GetCounters();
			AddInfo(FString::Printf(TEXT("lines %lld, suppressed %lld, drain %.3f ms, dropped %lld, %d lines/s"),
				Now.LinesTotal - State->LinesBefore, Now.EchoSuppressedLines - State->SuppressedBefore, Now.DrainMs,
				Now.DroppedLines, Now.LinesPerSecond));
			TestEqual(TEXT("every pushed line was committed once"), Now.LinesTotal - State->LinesBefore, int64(1000));
			// At most 100 lines per 1 s window reach the log; the burst may straddle one window boundary.
			TestTrue(*FString::Printf(TEXT("the cap kept at least 800 of the 1,000 lines out of the log (%lld)"),
				Now.EchoSuppressedLines - State->SuppressedBefore), Now.EchoSuppressedLines - State->SuppressedBefore >= 800);
			TestTrue(TEXT("a drain reports its cost"), Now.DrainMs >= 0.0);
			TestEqual(TEXT("nothing was dropped"), Now.DroppedLines, int64(0));
			State->Store->ClearEchoCap();
			return true;
		});
		Steps.Poll(TEXT("ClearEchoCap brings back the project's cap"), [State]()
		{
			State->Store->Drain(0.01);
			const int32 Project = FMath::Max(0, UDreamProjectSettings::Get()->EchoCapLinesPerSecond);
			return State->Store->GetCounters().EchoCap == Project ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.Poll(TEXT("lines per second covers the burst"), [State]()
		{
			// The rate is published per full second of the ingest thread's wakes (every 100 ms while idle).
			return State->Store->GetCounters().LinesPerSecond > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Finally(TEXT("stop the private store"), [State]()
		{
			if (State->Store.IsValid())
			{
				State->Store->Shutdown();
				State->Store.Reset();
			}
		});
		Steps.Run(Done);
	});
}
