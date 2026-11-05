// SPDX-License-Identifier: Apache-2.0
// Shared helpers of the tier-4 specs (TDD §14.1 tier 4, §14.2 editor test fixtures): service access through
// FDreamEditorModule, the precondition that Dream can run code at all, run requests and the steps that submit a run, wait
// for its finish (finish step 5, §5.2.4) and collect its lines from FDreamOutputStore once its footer is drained.
// Game thread.
#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Output/DreamOutputTypes.h"
#include "Python/DreamPython.h"
#include "Run/DreamRunTypes.h"
#include "Support/DreamTestSteps.h"

class FDreamBuffer;
class FDreamEditorModule;
class FWidgetPath;
class SDreamTab;
class SWidget;

namespace DreamTest
{
	class FDreamTestLogCapture;

	/**
	 * Specs that open the tab or run code: Dream is Headless under -nullrhi, -unattended or a commandlet (TDD §3.4, §3.5),
	 * so they carry NonNullRHI and the framework filters them out of a -nullrhi session (AutomationTest.cpp, feature
	 * flags). Tier 4 runs them in an attended editor (TDD §14.1); Dream.Lifecycle covers the headless session.
	 */
	inline constexpr EAutomationTestFlags AttendedFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::ProductFilter;
	inline constexpr EAutomationTestFlags AnyEditorFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	// ---- Services and preconditions ----

	/** The DreamEditor module, or null with an error on Test. */
	FDreamEditorModule* GetModule(FAutomationTestBase& Test);
	/** True when Dream accepts runs now or after its deferred boot; otherwise an error naming the state and the reason. */
	bool RequireRunnablePython(FAutomationTestBase& Test);
	/** Opens or focuses the Dream tab (Tools > Dream Python Editor, FDreamEditorModule::InvokeTab). */
	TSharedPtr<SDreamTab> RequireTab(FAutomationTestBase& Test);

	const TCHAR* PythonStateName(EDreamPythonState State);
	FString OutcomeText(EDreamRunOutcome Outcome);
	const TCHAR* LineTypeName(dream::LineType Type);

	/** Eight random lower-case hex digits: names a spec writes into the shared Session never collide across suite runs. */
	FString UniqueSuffix();
	/** Display name for a scratch buffer a spec opens ("DreamT4-<Purpose>-<suffix>"). */
	FString ScratchName(const FString& Purpose);
	/** Opens a scratch tab with Text in the Dream tab and makes it the active buffer. */
	TSharedPtr<FDreamBuffer> OpenScratch(FAutomationTestBase& Test, SDreamTab& Tab, const FString& Text, const FString& Purpose);
	/** Closes a buffer a spec opened (BUF_CLOSE; a non-empty scratch stays reopenable, §10.2.5). No-op when already closed. */
	void CloseBuffer(const FGuid& BufferId);

	// ---- Requests ----

	/** A REPL-route request (Session, statement mode, auto-print); the snapshot and <dream:REPL#N> are filled at Submit. */
	FDreamRunRequest MakeReplRequest(const FString& Code);
	/**
	 * A Buffer or Selection request over whole lines [FirstLine, LastLine] (1-based) of a buffer, as SDreamTab builds it:
	 * the buffer's snapshot, its compile filename from FDreamBufferManager and the tab title (§5.2.1, §5.3.2, RT-14).
	 */
	FDreamRunRequest MakeBufferRequest(const FDreamBuffer& Buffer, EDreamRunRoute Route, int32 FirstLine, int32 LastLine);

	// ---- Runs and their output ----

	/** One line of a run block copied out of the ring. */
	struct FOutputLine
	{
		uint64 Seq = 0;
		dream::LineType Type = dream::LineType::Stdout;
		uint16 Flags = 0;
		int32 EchoLine = 0;
		uint32 RunNumber = 0;
		FString Text;
	};

	/** What a submitted run produced; filled by the steps AddRun appends. */
	struct FRunResult
	{
		int32 RunNumber = 0;
		double SubmitSeconds = 0.0;
		double FinishSeenSeconds = 0.0;  // the first frame that saw finish step 5 done (an upper bound of the finish)
		bool bFinished = false;
		FDreamRunRecord Record;
		TArray<FOutputLine> Lines;      // the whole block, header and footer included

		/** Echo, stdout, stderr, result and error lines: what a spec compares in order. */
		TArray<FOutputLine> Content() const;
		TArray<FOutputLine> OfType(dream::LineType Type) const;
		bool HasLineContaining(dream::LineType Type, const FString& Needle) const;
		/** "#7 ok: [echo 2] a = 1 | [stdout] two ..." for failure messages. */
		FString Describe() const;
	};

	/** Submits Request; 0 (and an error on Test) when it was refused. */
	int32 Submit(FAutomationTestBase& Test, FDreamRunRequest&& Request);
	/** Finish step 5 has run for RunNumber: it is in the history with an outcome and is no longer the active run. */
	bool IsRunFinished(int32 RunNumber);
	/** No run is active (Idle). */
	bool IsIdle();
	/** Drains the store and copies RunNumber's lines; true once the block's footer is in the ring. */
	bool CollectRunLines(uint32 RunNumber, TArray<FOutputLine>& OutLines);

	/** A step that waits until no run is active (an earlier spec's run, a boot in flight). */
	void AddWaitIdle(FDreamSteps& Steps, double TimeoutSeconds = 30.0);
	/**
	 * Appends three steps: submit MakeRequest() (refused = fail), wait for finish step 5 (copies the record), and wait for
	 * the run block's footer in FDreamOutputStore (copies the lines). The first run of a session also boots dream_editor.
	 */
	void AddRun(FDreamSteps& Steps, const FString& Name, TFunction<FDreamRunRequest()> MakeRequest, TSharedRef<FRunResult> Result,
		double TimeoutSeconds = 30.0);
	/** Appends the finish and output waits for a run that something else submitted (a tab command, the console executor). */
	void AddWaitForRun(FDreamSteps& Steps, const FString& Name, TSharedRef<FRunResult> Result, double TimeoutSeconds = 30.0);

	/** Test.AddError with the run's description unless its outcome is Expected. */
	bool ExpectOutcome(FAutomationTestBase& Test, const FRunResult& Run, EDreamRunOutcome Expected);

	/** One expected content line: type, exact stored text (no "# Result: " or '›' decoration, §6.4) and, for echo, its buffer line. */
	struct FExpectedLine
	{
		dream::LineType Type = dream::LineType::Stdout;
		FString Text;
		int32 EchoLine = 0;

		static FExpectedLine Echo(int32 BufferLine, const FString& Source) { return FExpectedLine{ dream::LineType::Echo, Source, BufferLine }; }
		static FExpectedLine Stdout(const FString& Text) { return FExpectedLine{ dream::LineType::Stdout, Text, 0 }; }
		static FExpectedLine Result(const FString& Repr) { return FExpectedLine{ dream::LineType::Result, Repr, 0 }; }
	};

	/** The run's content lines (FRunResult::Content) equal Expected in order; one error with both lists otherwise. */
	bool ExpectContent(FAutomationTestBase& Test, const TCHAR* What, const FRunResult& Run, const TArray<FExpectedLine>& Expected);

	/**
	 * A run that fails on purpose forwards its error block to LogPython at Error, one log line per block line, as a stock
	 * run does (§6.3 verbosity table: the runner traceback and the SyntaxError block); the automation framework counts each
	 * such line as a test error. This declares the block: Summary (its last line, for example "NameError: name 'x' is not
	 * defined") must reach the log at least once, and the other lines are accepted in any number: the traceback header,
	 * the File lines naming Filename, the source lines containing one of SourceNeedles and the caret lines.
	 * Call it before the run is submitted: the DreamOutput thread forwards while the steps poll.
	 */
	void ExpectPythonErrorBlock(FAutomationTestBase& Test, const FString& Filename, const FString& Summary, const TArray<FString>& SourceNeedles);
	/**
	 * A step that waits until the forwarder wrote Summary to LogPython (§6.3), so a declared error block is logged inside
	 * this spec and never in the next one. Log must exist before the run is submitted.
	 */
	void AddWaitForForwardedError(FDreamSteps& Steps, TSharedRef<FDreamTestLogCapture> Log, const FString& Summary, double TimeoutSeconds = 5.0);
	/** The SDreamTab-shaped number of the active (just submitted) run, or 0. */
	int32 GetActiveRunNumber();

	/**
	 * The path from Widget's window to Widget, shaped for FSlateApplication::Route*Event (a hit-test path: one pointer entry
	 * per widget, none of them virtual). The Process*Event entry points hit-test under the real OS cursor instead
	 * (LocateWindowUnderMouse asks the platform for the window under it), which a test cannot place. False when the widget
	 * is in no visible window.
	 */
	bool MakePointerPath(const TSharedRef<SWidget>& Widget, FWidgetPath& OutPath);
}
