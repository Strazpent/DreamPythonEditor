// SPDX-License-Identifier: Apache-2.0
// (run, PO-06 F2) Exact selection runs and the owner's "import statements aren't evaluated when selecting the module and
// pressing enter".
//   Dream.Run.SelectionText: DreamRunSource::ExactSelection, the characters a selection runs (pure, any editor session).
//   Dream.Run.Selection: real key presses (SendInput through the input bot) on selections in a scratch tab: Numpad Enter
//     and Ctrl+Enter run exactly the selection (a whole import line, the module name inside it, several import lines, a
//     mid-line start of a multi-line statement, an indented block from its first non-blank character, a fragment that
//     raises with buffer carets), the names are bound in the Session afterwards, and the echo shows what ran. The bug:
//     with the module name of a typed "import math" selected, Numpad Enter ran the whole line again (RUN-01 extended a
//     selection to its line start), which prints nothing, so the selected name was never evaluated; now it prints
//     "# Result: <module 'math' ...>". The owner's answer (R-q1-2, "Main Enter, make it run"): main Enter over a selection
//     runs it too, by default (FDreamKeyRouter::SetMainEnterRunsSelection); without a selection it stays a newline. Both
//     states of the switch are covered.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamKeyRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Run/DreamRunService.h"
#include "Run/DreamRunSourceText.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"
#include "Support/DreamTestLogCapture.h"
#include "dream/TextBuffer.h"

namespace DreamSelectionRunSpec
{
	dream::TextSnapshot Snap(const FString& Text)
	{
		const dream::TextBuffer Buffer(DreamText::ToU16(Text));
		return Buffer.Snapshot();
	}

	int64 At(const dream::TextSnapshot& S, int32 Line, int32 Column)
	{
		return S.LineStart(Line) + Column;
	}

	int32 LastRunNumber()
	{
		const FDreamRunService& Runs = FDreamEditorModule::Get().GetRunService();
		const int32 Finished = Runs.GetLastFinished() ? Runs.GetLastFinished()->RunNumber : 0;
		return FMath::Max(Finished, DreamTest::GetActiveRunNumber());
	}

	using FKeys = TArray<DreamTest::FDreamBotKey>;
	FKeys NumpadEnter() { return { DreamTest::FDreamInputBot::VkNumpadEnter() }; }
	FKeys MainEnter() { return { DreamTest::FDreamInputBot::VkReturn() }; }
	FKeys CtrlEnter() { return { DreamTest::FDreamInputBot::VkControl(), DreamTest::FDreamInputBot::VkReturn() }; }
}

// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamSelectionTextSpec, "Dream.Run.SelectionText", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamSelectionTextSpec)

void FDreamSelectionTextSpec::Define()
{
	using namespace DreamSelectionRunSpec;

	It("runs a fragment inside one line from its start character to its end character", [this]()
	{
		const dream::TextSnapshot S = Snap(TEXT("import math\nr = math.floor(2.5) + 10\n"));
		const DreamRunSource::FSelectionText Module = DreamRunSource::ExactSelection(S, At(S, 0, 7), At(S, 0, 11));
		TestEqual(TEXT("the module name alone"), Module.Text, FString(TEXT("math")));
		TestEqual(TEXT("its line"), Module.FirstLine, 1);
		TestEqual(TEXT("one line"), Module.LastLine, 1);
		TestEqual(TEXT("start column"), Module.StartColumn.Get(-1), 7);
		TestFalse(TEXT("no end column: the selection reaches the line end"), Module.EndColumn.IsSet());

		// The direction of the selection does not matter (the caret before the anchor).
		const DreamRunSource::FSelectionText Back = DreamRunSource::ExactSelection(S, At(S, 1, 19), At(S, 1, 4));
		TestEqual(TEXT("a backwards fragment"), Back.Text, FString(TEXT("math.floor(2.5)")));
		TestEqual(TEXT("its line"), Back.FirstLine, 2);
		TestEqual(TEXT("its start"), Back.StartColumn.Get(-1), 4);
	});

	It("keeps whole lines whole and leaves no start column for a selection from column 0", [this]()
	{
		const dream::TextSnapshot S = Snap(TEXT("import math\nimport json\nx = 1\n"));
		const DreamRunSource::FSelectionText Line = DreamRunSource::ExactSelection(S, At(S, 0, 0), At(S, 0, 11));
		TestEqual(TEXT("one whole line"), Line.Text, FString(TEXT("import math")));
		TestFalse(TEXT("no start column"), Line.StartColumn.IsSet());
		TestFalse(TEXT("no end column (the line end)"), Line.EndColumn.IsSet());

		// Shift+Down twice from line 1: the selection ends at column 0 of line 3, so it ends on line 2 (§4.5).
		const DreamRunSource::FSelectionText Two = DreamRunSource::ExactSelection(S, At(S, 0, 0), At(S, 2, 0));
		TestEqual(TEXT("two import lines"), Two.Text, FString(TEXT("import math\nimport json")));
		TestEqual(TEXT("first line"), Two.FirstLine, 1);
		TestEqual(TEXT("last line"), Two.LastLine, 2);
		TestFalse(TEXT("no end column"), Two.EndColumn.IsSet());
	});

	It("runs a multi-line selection from its start character and stops at its end character", [this]()
	{
		const dream::TextSnapshot S = Snap(TEXT("skip = 1; vals = [1,\n    2, 3]  # tail\nafter = 2"));
		const DreamRunSource::FSelectionText Sel = DreamRunSource::ExactSelection(S, At(S, 0, 10), At(S, 1, 9));
		TestEqual(TEXT("from 'vals' to ']'"), Sel.Text, FString(TEXT("vals = [1,\n    2, 3]")));
		TestEqual(TEXT("start column"), Sel.StartColumn.Get(-1), 10);
		TestEqual(TEXT("end column"), Sel.EndColumn.Get(-1), 9);
		TestEqual(TEXT("lines"), Sel.LastLine - Sel.FirstLine, 1);

		const FString Echo1(DreamRunSource::CutRunLine(TEXT("skip = 1; vals = [1,"), 1, 1, 2, Sel.StartColumn, Sel.EndColumn));
		const FString Echo2(DreamRunSource::CutRunLine(TEXT("    2, 3]  # tail"), 2, 1, 2, Sel.StartColumn, Sel.EndColumn));
		TestEqual(TEXT("the echo of line 1 starts at the selection"), Echo1, FString(TEXT("vals = [1,")));
		TestEqual(TEXT("the echo of line 2 stops at the selection"), Echo2, FString(TEXT("    2, 3]")));
		const FString One(DreamRunSource::CutRunLine(TEXT("import math"), 4, 4, 4, TOptional<int32>(7), TOptional<int32>(11)));
		TestEqual(TEXT("a one-line fragment is cut at both ends"), One, FString(TEXT("math")));
	});

	It("keeps the indentation of a block selected from its first non-blank character", [this]()
	{
		const dream::TextSnapshot S = Snap(TEXT("if True:\n    a = 1\n    b = a + 1\nc = 3"));
		const DreamRunSource::FSelectionText Block = DreamRunSource::ExactSelection(S, At(S, 1, 4), At(S, 2, 13));
		TestEqual(TEXT("the block with its indentation (the RT-14 wrapper removes it)"), Block.Text, FString(TEXT("    a = 1\n    b = a + 1")));
		TestFalse(TEXT("no start column"), Block.StartColumn.IsSet());
		TestEqual(TEXT("first line"), Block.FirstLine, 2);

		// A start inside the indentation is the same.
		const DreamRunSource::FSelectionText Inside = DreamRunSource::ExactSelection(S, At(S, 1, 2), At(S, 2, 13));
		TestEqual(TEXT("a start inside the indentation"), Inside.Text, Block.Text);

		// A start after code on its line is a fragment, blanks after the code included.
		const dream::TextSnapshot T = Snap(TEXT("a = 1;   b = 2"));
		const DreamRunSource::FSelectionText After = DreamRunSource::ExactSelection(T, At(T, 0, 6), At(T, 0, 14));
		TestEqual(TEXT("blanks after code"), After.Text, FString(TEXT("   b = 2")));
		TestEqual(TEXT("its start column"), After.StartColumn.Get(-1), 6);
	});

	It("starts at the next line when only the first line's line break is selected", [this]()
	{
		const dream::TextSnapshot S = Snap(TEXT("first = 1\nsecond = 2\nthird = 3"));
		const DreamRunSource::FSelectionText Sel = DreamRunSource::ExactSelection(S, At(S, 0, 9), At(S, 1, 10));
		TestEqual(TEXT("line 2 only"), Sel.Text, FString(TEXT("second = 2")));
		TestEqual(TEXT("first line"), Sel.FirstLine, 2);
		TestFalse(TEXT("no start column"), Sel.StartColumn.IsSet());
	});

	It("counts columns in UTF-16 units", [this]()
	{
		const FString Line = FString(TEXT("s = '")) + TEXT("é") + TEXT("'; t = 2");
		const dream::TextSnapshot S = Snap(Line);
		const int32 Start = Line.Find(TEXT("t ="));
		const DreamRunSource::FSelectionText Sel = DreamRunSource::ExactSelection(S, At(S, 0, Start), At(S, 0, Line.Len()));
		TestEqual(TEXT("the fragment"), Sel.Text, FString(TEXT("t = 2")));
		TestEqual(TEXT("its UTF-16 start"), Sel.StartColumn.Get(-1), Start);
	});
}

// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamSelectionRunSpec, "Dream.Run.Selection", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	/** Focuses Buffer's code view, sets Selection, brings its window to the front for SendInput. */
	bool PrepareKeys(const TSharedPtr<FDreamBuffer>& Buffer, const FDreamTextSelection& Selection);
	/** Steps: PrepareKeys with Selection(), the bot presses Keys, and (bExpectRun) the run Result is waited for. */
	void AddKeyCase(DreamTest::FDreamSteps& Steps, const FString& Name, const TSharedPtr<FDreamBuffer>& Buffer,
		TFunction<FDreamTextSelection()> Selection, const DreamSelectionRunSpec::FKeys& Keys, bool bExpectRun,
		TSharedRef<DreamTest::FRunResult> Result);
	/** Steps: a REPL run of Code whose stdout must contain Needle. */
	void AddProbe(DreamTest::FDreamSteps& Steps, const FString& Name, const FString& Code, const FString& Needle);
END_DEFINE_SPEC(FDreamSelectionRunSpec)

bool FDreamSelectionRunSpec::PrepareKeys(const TSharedPtr<FDreamBuffer>& Buffer, const FDreamTextSelection& Selection)
{
	Tab->ActivateBuffer(Buffer->GetId());
	const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
	if (!TestTrue(TEXT("the scratch tab's code view"), View.IsValid()))
	{
		return false;
	}
	void* Hwnd = DreamTest::FDreamTestProbes::GetOsWindowHandle(FSlateApplication::Get().FindWidgetWindow(View.ToSharedRef()));
	if (!TestTrue(TEXT("the Dream tab's window is in front for SendInput"), DreamTest::FDreamInputBot::BringEditorToFront(Hwnd)))
	{
		return false;
	}
	View->FocusEditor();
	View->SetSelection(Selection);
	return TestTrue(TEXT("the code view has keyboard focus"), View->HasEditorFocus());
}

void FDreamSelectionRunSpec::AddKeyCase(DreamTest::FDreamSteps& Steps, const FString& Name, const TSharedPtr<FDreamBuffer>& Buffer,
	TFunction<FDreamTextSelection()> Selection, const DreamSelectionRunSpec::FKeys& Keys, bool bExpectRun,
	TSharedRef<DreamTest::FRunResult> Result)
{
	TSharedRef<TSharedPtr<DreamTest::FDreamInputBotTicket>> Ticket = MakeShared<TSharedPtr<DreamTest::FDreamInputBotTicket>>();
	TSharedRef<int32> Before = MakeShared<int32>(0);
	DreamTest::AddWaitIdle(Steps);
	Steps.Do(FString::Printf(TEXT("%s: select"), *Name), [this, Buffer, Selection]()
	{
		return PrepareKeys(Buffer, Selection());
	});
	Steps.WaitFrames(2);
	Steps.Do(FString::Printf(TEXT("%s: the bot presses the keys"), *Name), [this, Keys, Ticket, Before, Result, Name]()
	{
		const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
		if (!TestTrue(FString::Printf(TEXT("%s: focus is still in the code view"), *Name), View.IsValid() && View->HasEditorFocus()))
		{
			return false;
		}
		*Before = DreamSelectionRunSpec::LastRunNumber();
		Result->RunNumber = *Before + 1;
		*Ticket = DreamTest::FDreamInputBot::Submit([Keys](DreamTest::FDreamInputBotContext& Bot)
		{
			if (Bot.RequireForeground())
			{
				Bot.Chord(Keys);
			}
		});
		return true;
	});
	Steps.Poll(FString::Printf(TEXT("%s: the key press"), *Name), [Ticket]()
	{
		return DreamTest::IsTicketDone(Ticket->ToSharedRef()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
	}, 20.0);
	Steps.Do(FString::Printf(TEXT("%s: the bot's report"), *Name), [this, Ticket, Name]()
	{
		return DreamTest::ReportTicket(*this, Ticket->ToSharedRef(), Name);
	});
	if (bExpectRun)
	{
		Steps.Poll(FString::Printf(TEXT("%s: the key started a run"), *Name), [Result]()
		{
			return DreamSelectionRunSpec::LastRunNumber() >= Result->RunNumber ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		DreamTest::AddWaitForRun(Steps, Name, Result, 30.0);
	}
	else
	{
		Steps.WaitFrames(4);
		Steps.Do(FString::Printf(TEXT("%s: no run"), *Name), [this, Before, Name]()
		{
			TestEqual(FString::Printf(TEXT("%s submitted no run"), *Name), DreamSelectionRunSpec::LastRunNumber(), *Before);
			return true;
		});
	}
}

void FDreamSelectionRunSpec::AddProbe(DreamTest::FDreamSteps& Steps, const FString& Name, const FString& Code, const FString& Needle)
{
	TSharedRef<DreamTest::FRunResult> Probe = MakeShared<DreamTest::FRunResult>();
	DreamTest::AddRun(Steps, Name, [Code]() { return DreamTest::MakeReplRequest(Code); }, Probe);
	Steps.Do(FString::Printf(TEXT("check %s"), *Name), [this, Probe, Name, Needle]()
	{
		if (DreamTest::ExpectOutcome(*this, *Probe, EDreamRunOutcome::Ok))
		{
			TestTrue(FString::Printf(TEXT("%s prints '%s': %s"), *Name, *Needle, *Probe->Describe()),
				Probe->HasLineContaining(dream::LineType::Stdout, Needle));
		}
		return true;
	});
}

void FDreamSelectionRunSpec::Define()
{
	using namespace DreamSelectionRunSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		Tab.Reset();
	});

	LatentIt("runs exactly the selection on Numpad Enter and Ctrl+Enter, imports included, and binds the names",
		FTimespan::FromSeconds(300.0), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this) || !DreamTest::RequireEditorInFront(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const TArray<FString> Source = {
			TEXT("import math"),                                                                        // 1
			FString::Printf(TEXT("import json as sel_json_%s"), *Sfx),                                  // 2
			FString::Printf(TEXT("from colorsys import rgb_to_hsv as sel_hsv_%s"), *Sfx),               // 3
			FString::Printf(TEXT("sel_skip_%s = 1; sel_vals_%s = [1,"), *Sfx, *Sfx),                    // 4
			TEXT("    2, 3]"),                                                                          // 5
			TEXT("if True:"),                                                                           // 6
			FString::Printf(TEXT("    sel_blk_a_%s = 1"), *Sfx),                                        // 7
			FString::Printf(TEXT("    sel_blk_b_%s = sel_blk_a_%s + 1"), *Sfx, *Sfx),                   // 8
			FString::Printf(TEXT("sel_tot_%s = 1 + zz_missing_%s * 2"), *Sfx, *Sfx),                    // 9
		};
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, FString::Join(Source, TEXT("\n")), TEXT("SelRun"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FString Filename = FDreamEditorModule::Get().GetBufferManager().GetCompileFilename(*Buffer);
		auto Line = [Source](int32 Index, int32 From = 0, int32 To = -1)
		{
			return FDreamTextSelection{ Index, From, Index, To < 0 ? Source[Index].Len() : To };
		};

		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(290.0);
		AddProbe(Steps, TEXT("forget math"), TEXT("globals().pop('math', None); print('math' in globals())"), TEXT("False"));

		// The owner's case with Maya's execute key: the selected import line runs and math is bound afterwards.
		TSharedRef<DreamTest::FRunResult> NumpadLine = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("Numpad Enter on 'import math'"), Buffer, [Line]() { return Line(0); }, NumpadEnter(), true, NumpadLine);
		Steps.Do(TEXT("check the Numpad Enter run"), [this, NumpadLine, Source]()
		{
			if (DreamTest::ExpectOutcome(*this, *NumpadLine, EDreamRunOutcome::Ok))
			{
				TestTrue(TEXT("a selection run"), NumpadLine->Record.Route == EDreamRunRoute::Selection);
				DreamTest::ExpectContent(*this, TEXT("the import line"), *NumpadLine, { DreamTest::FExpectedLine::Echo(1, Source[0]) });
			}
			return true;
		});
		AddProbe(Steps, TEXT("math after Numpad Enter"), TEXT("print('math' in globals(), math.floor(2.5))"), TEXT("True 2"));

		// Ctrl+Enter on the same line.
		AddProbe(Steps, TEXT("forget math again"), TEXT("globals().pop('math', None); print('math' in globals())"), TEXT("False"));
		TSharedRef<DreamTest::FRunResult> CtrlLine = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("Ctrl+Enter on 'import math'"), Buffer, [Line]() { return Line(0); }, CtrlEnter(), true, CtrlLine);
		Steps.Do(TEXT("check the Ctrl+Enter run"), [this, CtrlLine]()
		{
			DreamTest::ExpectOutcome(*this, *CtrlLine, EDreamRunOutcome::Ok);
			return true;
		});
		AddProbe(Steps, TEXT("math after Ctrl+Enter"), TEXT("print('math' in globals())"), TEXT("True"));

		// Only the module name inside the import line: the expression runs and prints its result (RUN-08).
		TSharedRef<DreamTest::FRunResult> Word = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("Numpad Enter on 'math' inside 'import math'"), Buffer, [Line]() { return Line(0, 7, 11); }, NumpadEnter(), true, Word);
		Steps.Do(TEXT("check the module name run"), [this, Word]()
		{
			if (DreamTest::ExpectOutcome(*this, *Word, EDreamRunOutcome::Ok))
			{
				TestEqual(TEXT("one statement"), Word->Record.Statements.Num(), 1);
				TestTrue(*FString::Printf(TEXT("the echo is the word alone: %s"), *Word->Describe()),
					Word->OfType(dream::LineType::Echo).Num() == 1 && Word->OfType(dream::LineType::Echo)[0].Text == TEXT("math"));
				TestTrue(*FString::Printf(TEXT("# Result: <module 'math' ...>: %s"), *Word->Describe()),
					Word->HasLineContaining(dream::LineType::Result, TEXT("<module 'math'")));
			}
			return true;
		});

		// Several import lines.
		TSharedRef<DreamTest::FRunResult> Imports = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("Ctrl+Enter on two import lines"), Buffer,
			[Source]() { return FDreamTextSelection{ 1, 0, 2, Source[2].Len() }; }, CtrlEnter(), true, Imports);
		Steps.Do(TEXT("check the import lines run"), [this, Imports, Source]()
		{
			if (DreamTest::ExpectOutcome(*this, *Imports, EDreamRunOutcome::Ok))
			{
				DreamTest::ExpectContent(*this, TEXT("two imports"), *Imports,
					{ DreamTest::FExpectedLine::Echo(2, Source[1]), DreamTest::FExpectedLine::Echo(3, Source[2]) });
			}
			return true;
		});

		// A multi-line statement selected from mid-line: from its start character to its end character.
		TSharedRef<DreamTest::FRunResult> MidLine = MakeShared<DreamTest::FRunResult>();
		const int32 ValsColumn = Source[3].Find(TEXT("sel_vals_"));
		AddKeyCase(Steps, TEXT("Ctrl+Enter from mid-line over two lines"), Buffer,
			[Source, ValsColumn]() { return FDreamTextSelection{ 3, ValsColumn, 4, Source[4].Len() }; }, CtrlEnter(), true, MidLine);
		Steps.Do(TEXT("check the mid-line run"), [this, MidLine, Source, ValsColumn]()
		{
			if (DreamTest::ExpectOutcome(*this, *MidLine, EDreamRunOutcome::Ok))
			{
				TestEqual(TEXT("start column recorded"), MidLine->Record.SelectionStartColumn.Get(-1), ValsColumn);
				DreamTest::ExpectContent(*this, TEXT("the echo starts at the selection"), *MidLine,
					{ DreamTest::FExpectedLine::Echo(4, Source[3].Mid(ValsColumn)), DreamTest::FExpectedLine::Echo(5, Source[4]) });
			}
			return true;
		});

		// q1-m2: re-running the fragment's record (RUN-12) runs the same characters from the recorded snapshot.
		AddProbe(Steps, TEXT("forget the mid-line names"),
			FString::Printf(TEXT("globals().pop('sel_vals_%s', None); print('sel_vals_%s' in globals())"), *Sfx, *Sfx), TEXT("False"));
		TSharedRef<DreamTest::FRunResult> Again = MakeShared<DreamTest::FRunResult>();
		Steps.Do(TEXT("re-run the mid-line record"), [this, MidLine, Again]()
		{
			const FDreamSubmitResult Submitted = FDreamEditorModule::Get().GetRunService().Rerun(MidLine->Record.RunNumber);
			Again->RunNumber = Submitted.RunNumber;
			return TestTrue(*FString::Printf(TEXT("the re-run is accepted: %s"), *Submitted.Reason.ToString()), Submitted.bAccepted);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the re-run"), Again, 30.0);
		Steps.Do(TEXT("check the re-run"), [this, MidLine, Again, Source, ValsColumn]()
		{
			if (DreamTest::ExpectOutcome(*this, *Again, EDreamRunOutcome::Ok))
			{
				TestEqual(TEXT("the re-run keeps the start column"), Again->Record.SelectionStartColumn.Get(-1), ValsColumn);
				TestEqual(TEXT("the re-run keeps the lines"), Again->Record.FirstLine, MidLine->Record.FirstLine);
				TestEqual(TEXT("the re-run runs the same text"), Again->Record.SourceSha256, MidLine->Record.SourceSha256);
				DreamTest::ExpectContent(*this, TEXT("the re-run's echo starts at the selection"), *Again,
					{ DreamTest::FExpectedLine::Echo(4, Source[3].Mid(ValsColumn)), DreamTest::FExpectedLine::Echo(5, Source[4]) });
			}
			return true;
		});

		// An indented block from its first non-blank character (common indentation removed).
		TSharedRef<DreamTest::FRunResult> Block = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("Ctrl+Enter on an indented block"), Buffer,
			[Source]() { return FDreamTextSelection{ 6, 4, 7, Source[7].Len() }; }, CtrlEnter(), true, Block);
		Steps.Do(TEXT("check the block run"), [this, Block]()
		{
			if (DreamTest::ExpectOutcome(*this, *Block, EDreamRunOutcome::Ok))
			{
				TestEqual(TEXT("two statements"), Block->Record.Statements.Num(), 2);
			}
			return true;
		});

		// A fragment that raises: the traceback names buffer line 9 and its carets sit under the name in the buffer line.
		TSharedRef<DreamTest::FRunResult> Raises = MakeShared<DreamTest::FRunResult>();
		const int32 MissingColumn = Source[8].Find(TEXT("zz_missing_"));
		const FString Missing = FString::Printf(TEXT("zz_missing_%s"), *Sfx);
		const FString Summary = FString::Printf(TEXT("NameError: name '%s' is not defined"), *Missing);
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		Steps.Do(TEXT("declare the NameError block"), [this, Filename, Summary, Source]()
		{
			DreamTest::ExpectPythonErrorBlock(*this, Filename, Summary, { Source[8] });
			return true;
		});
		AddKeyCase(Steps, TEXT("Numpad Enter on a fragment that raises"), Buffer,
			[Source, MissingColumn]() { return FDreamTextSelection{ 8, MissingColumn, 8, Source[8].Len() }; }, NumpadEnter(), true, Raises);
		DreamTest::AddWaitForForwardedError(Steps, Log, Summary);
		Steps.Do(TEXT("check the traceback"), [this, Raises, Source, MissingColumn, Missing]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Raises, EDreamRunOutcome::Error))
			{
				return true;
			}
			TestTrue(*FString::Printf(TEXT("the traceback names line 9: %s"), *Raises->Describe()),
				Raises->HasLineContaining(dream::LineType::Error, TEXT(", line 9, in <module>")));
			const FString Caret = FString::ChrN(4 + MissingColumn, TEXT(' ')) + FString::ChrN(Missing.Len(), TEXT('^'));
			bool bCaret = false;
			for (const DreamTest::FOutputLine& Out : Raises->OfType(dream::LineType::Error))
			{
				TArray<FString> Parts;
				Out.Text.ParseIntoArrayLines(Parts, /*bCullEmpty*/ false);
				for (const FString& Part : Parts)
				{
					bCaret |= Part.TrimEnd() == Caret;
				}
			}
			TestTrue(*FString::Printf(TEXT("the carets sit under the name in the buffer line ('%s'): %s"), *Caret, *Raises->Describe()), bCaret);
			return true;
		});

		AddProbe(Steps, TEXT("the Session afterwards"),
			FString::Printf(TEXT("print(sel_json_%s.__name__, sel_hsv_%s.__name__, 'sel_skip_%s' in globals(), sel_vals_%s, sel_blk_b_%s, 'sel_tot_%s' in globals())"),
				*Sfx, *Sfx, *Sfx, *Sfx, *Sfx, *Sfx),
			TEXT("json rgb_to_hsv False [1, 2, 3] 2 False"));
		// The spec's names leave the Session (q1 nit); math stays imported, as a user's own import would.
		AddProbe(Steps, TEXT("clean the Session"),
			FString::Printf(TEXT("[globals().pop(n) for n in [k for k in globals() if k.startswith('sel_') and k.endswith('_%s')]]; globals().pop('_', None); print('sel clean', [k for k in globals() if k.endswith('_%s')])"), *Sfx, *Sfx),
			TEXT("sel clean []"));
		Steps.Run(Done);
	});

	LatentIt("types import math by keys and selects the module name: Numpad Enter prints the module, main Enter runs it by default and is a newline with the switch off (the import bug)",
		FTimespan::FromSeconds(200.0), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this) || !DreamTest::RequireEditorInFront(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, FString(), TEXT("SelImport"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FString Import = TEXT("import math");
		TSharedRef<TSharedPtr<DreamTest::FDreamInputBotTicket>> Typing = MakeShared<TSharedPtr<DreamTest::FDreamInputBotTicket>>();

		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(190.0);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("the bot types 'import math'"), [this, Buffer, Typing, Import]()
		{
			if (!PrepareKeys(Buffer, FDreamTextSelection::Caret(0, 0)))
			{
				return false;
			}
			*Typing = DreamTest::FDreamInputBot::Submit([Import](DreamTest::FDreamInputBotContext& Bot)
			{
				if (Bot.RequireForeground())
				{
					Bot.TypeUnicode(Import);
				}
			});
			return true;
		});
		Steps.Poll(TEXT("the typing"), [Typing]()
		{
			return DreamTest::IsTicketDone(Typing->ToSharedRef()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 20.0);
		Steps.Do(TEXT("the bot's typing report"), [this, Typing]()
		{
			return DreamTest::ReportTicket(*this, Typing->ToSharedRef(), TEXT("typing"));
		});
		// SendInput returns before the game thread has pumped the characters: wait for them to land in the buffer.
		TSharedRef<double> TypedWaitStart = MakeShared<double>(0.0);
		Steps.Poll(TEXT("the typed text reaches the buffer"), [this, Buffer, Import, TypedWaitStart]()
		{
			if (Buffer->GetTextAsString() == Import)
			{
				return DreamTest::EStep::Next;
			}
			*TypedWaitStart = *TypedWaitStart == 0.0 ? FPlatformTime::Seconds() : *TypedWaitStart;
			if (FPlatformTime::Seconds() - *TypedWaitStart > 8.0)
			{
				AddError(FString::Printf(TEXT("the buffer holds \"%s\", not the typed \"%s\""), *Buffer->GetTextAsString().ReplaceCharWithEscapedChar(), *Import));
				return DreamTest::EStep::Fail;
			}
			return DreamTest::EStep::Wait;
		}, 10.0);

		// The owner's case: the module name selected (what a double-click does), Maya's execute key. Before PO-06 the run
		// was extended to the line start, so 'import math' ran again and printed nothing: the name was never evaluated.
		TSharedRef<DreamTest::FRunResult> Numpad = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("Numpad Enter on the typed module name"), Buffer,
			[]() { return FDreamTextSelection{ 0, 7, 0, 11 }; }, NumpadEnter(), true, Numpad);
		Steps.Do(TEXT("check Numpad Enter"), [this, Numpad, Buffer, Import]()
		{
			if (DreamTest::ExpectOutcome(*this, *Numpad, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("the module name was evaluated: # Result: <module 'math' ...>: %s"), *Numpad->Describe()),
					Numpad->HasLineContaining(dream::LineType::Result, TEXT("<module 'math'")));
			}
			TestEqual(TEXT("the buffer is unchanged (RUN-04)"), Buffer->GetTextAsString(), Import);
			return true;
		});

		// Ctrl+Enter on the whole typed line binds math in the Session.
		AddProbe(Steps, TEXT("forget math"), TEXT("globals().pop('math', None); print('math' in globals())"), TEXT("False"));
		TSharedRef<DreamTest::FRunResult> Ctrl = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("Ctrl+Enter on the typed import line"), Buffer,
			[]() { return FDreamTextSelection{ 0, 0, 0, 11 }; }, CtrlEnter(), true, Ctrl);
		Steps.Do(TEXT("check Ctrl+Enter"), [this, Ctrl]()
		{
			DreamTest::ExpectOutcome(*this, *Ctrl, EDreamRunOutcome::Ok);
			return true;
		});
		AddProbe(Steps, TEXT("math is bound"), TEXT("print('math' in globals(), math.pi > 3)"), TEXT("True True"));

		// Main Enter with the switch OFF (the behaviour before the owner's answer): it never runs; over a selection it
		// types a newline, as in Maya and every editor.
		Steps.Do(TEXT("the main-Enter switch off"), [this]()
		{
			FDreamKeyRouter::Get().SetMainEnterRunsSelection(false);
			return TestFalse(TEXT("main Enter runs no selection"), FDreamKeyRouter::Get().IsMainEnterRunsSelection());
		});
		TSharedRef<DreamTest::FRunResult> NoRun = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("main Enter (switch off) on the selected module name"), Buffer,
			[]() { return FDreamTextSelection{ 0, 7, 0, 11 }; }, MainEnter(), false, NoRun);
		Steps.Do(TEXT("check main Enter with the switch off"), [this, Buffer, Import]()
		{
			const FString Text = Buffer->GetTextAsString();
			TestTrue(*FString::Printf(TEXT("main Enter replaced the selected word with a newline: \"%s\""), *Text.ReplaceCharWithEscapedChar()),
				Text.StartsWith(TEXT("import")) && Text.Contains(TEXT("\n")) && !Text.Contains(TEXT("math")));
			if (const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView())
			{
				View->SetTextNoUndo(Import);
			}
			return true;
		});

		// The switch ON, the default since the owner's answer (R-q1-2): main Enter over a selection runs exactly the
		// selection, and main Enter without a selection is still a newline.
		Steps.Do(TEXT("the main-Enter switch on (the default)"), [this]()
		{
			TestTrue(TEXT("main Enter runs a selection by default (R-q1-2)"), FDreamKeyRouter::bMainEnterRunsSelectionDefault);
			FDreamKeyRouter::Get().SetMainEnterRunsSelection(true);
			return TestTrue(TEXT("main Enter runs a selection"), FDreamKeyRouter::Get().IsMainEnterRunsSelection());
		});
		TSharedRef<DreamTest::FRunResult> MainRun = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("main Enter (switch on) on the selected module name"), Buffer,
			[]() { return FDreamTextSelection{ 0, 7, 0, 11 }; }, MainEnter(), true, MainRun);
		Steps.Do(TEXT("check main Enter with the switch on"), [this, MainRun, Buffer, Import]()
		{
			if (DreamTest::ExpectOutcome(*this, *MainRun, EDreamRunOutcome::Ok))
			{
				TestTrue(TEXT("a selection run"), MainRun->Record.Route == EDreamRunRoute::Selection);
				TestTrue(*FString::Printf(TEXT("main Enter evaluated the module name: %s"), *MainRun->Describe()),
					MainRun->HasLineContaining(dream::LineType::Result, TEXT("<module 'math'")));
			}
			TestEqual(TEXT("main Enter left the buffer unchanged (RUN-04)"), Buffer->GetTextAsString(), Import);
			return true;
		});
		TSharedRef<DreamTest::FRunResult> CaretOnly = MakeShared<DreamTest::FRunResult>();
		AddKeyCase(Steps, TEXT("main Enter (switch on) without a selection"), Buffer,
			[]() { return FDreamTextSelection::Caret(0, 11); }, MainEnter(), false, CaretOnly);
		Steps.Do(TEXT("check main Enter without a selection"), [this, Buffer, Import]()
		{
			TestEqual(TEXT("without a selection main Enter is a newline"), Buffer->GetTextAsString(), Import + TEXT("\n"));
			return true;
		});
		Steps.Finally(TEXT("the main-Enter switch back to its default"), []()
		{
			FDreamKeyRouter::Get().SetMainEnterRunsSelection(FDreamKeyRouter::bMainEnterRunsSelectionDefault);
		});
		Steps.Run(Done);
	});
}
