// SPDX-License-Identifier: Apache-2.0
// Dream.Output.MayaStyle: the Maya-style history of owner ruling PO-04 point 2 in the docked Output view (SDreamOutputView,
// DreamOutputRows):
//   1. A buffer run's echo shows each source line as it is, with no line-number gutter and no '›'; a multi-line repr reads
//      "# Result: <first line>" ... "<last line> #" (A63: one decoration); a warnings.warn warning reads
//      "# Warning: <text>" with " #" on its last line; stdout and stderr lines are unprefixed; no shown row draws an icon
//      (no SImage, no glyph-only text such as ✖ ⚠ ↪ ◆ …), a shown row draws exactly its decorated text, and with Echo on
//      the echo is drawn in the theme's echo colour, visible on the history background.
//   2. A chained traceback shows every line prefixed "# ": the chain's first exception is a plain "# " line and the final
//      line reads "# Error: ValueError: <message> #"; an unreal.log_error line reads "# Error: <text> #"; the traceback's
//      File "<dream:…>", line N text is still a link, and its click opens the run's tab at that line.
//   3. Run headers and footers are no rows while View ▸ Run headers is off (the default), and ScrollToRun then selects
//      the run's first row; turned on, the run gets one header and one footer row, each one muted line with no link.
//      The REPL run's echo rows read and draw their source lines with no '>>> ' / '... ' prompt (A226).
//   4. The context menu on a line of a run (not its header) offers Copy, Copy run output, Re-run and, for a run made
//      without a transaction, Run in transaction; Re-run runs the same code again, Copy puts the raw line on the clipboard
//      (only when it holds text, A177), and Run in transaction runs it again with a transaction.
//   5. (review r6 M1) With headers off, a run that ends other than ok with no error line (SystemExit(3), a Stop before
//      its code started) shows one line, "# Error: Run #N <outcome> #" or "# Warning: … #", which ScrollToRun, a REPL link
//      and the context menu target; with headers on the footer stands instead.
//   6. (R1, r6 m8) A traceback that arrives over two drains: the rows made in the first are drawn again when the
//      exception line takes "# Error: " and the " #".
//   7. (RT-08, r6 m8) A partial run's Undo this run asks for a confirm first; the confirm undoes the recorded part.
//   8. (r6 M2, m7) In GitHub Light the REPL prompt strip's rules and the history popup use the theme's brushes.
//   9. (ruling on r7 m1) Only the run's own shown exception block hides the outcome line: after unreal.log_error, a
//      SystemExit(3) run still shows "# Error: Run #N … #" and a stopped run "# Warning: … #"; a traceback run shows none,
//      unless the Errors filter hides its traceback.
// Traceback rows are checked in the theme's error colour, the header and footer in its muted header colour. Each case
// starts with run headers off and every line type shown, and restores the user's values afterwards.
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Algo/Count.h"
#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Run/DreamTransactions.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamOutputRows.h"
#include "Shell/DreamTheme.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"   // the clipboard format check (IsPlainTextClipboard)
#endif

namespace DreamOutputMayaStyleSpec
{
	void Collect(const TSharedRef<SWidget>& Root, TFunctionRef<bool(const TSharedRef<SWidget>&)> Match, TArray<TSharedRef<SWidget>>& Out)
	{
		if (Match(Root))
		{
			Out.Add(Root);
		}
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			Collect(Children->GetChildAt(Index), Match, Out);
		}
	}

	bool IsTextBlock(const TSharedRef<SWidget>& Widget)
	{
		return Widget->GetType() == FName(TEXT("STextBlock"));
	}

	FString TextOf(const TSharedRef<SWidget>& Widget)
	{
		return StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
	}

	/** Every text block of a row widget, in order, joined: what the row draws. */
	FString DrawnText(const TSharedRef<SWidget>& Row)
	{
		TArray<TSharedRef<SWidget>> Texts;
		Collect(Row, IsTextBlock, Texts);
		FString Joined;
		for (const TSharedRef<SWidget>& Text : Texts)
		{
			Joined += TextOf(Text);
		}
		return Joined;
	}

	/** True when the clipboard is empty or holds only text, the only kind FPlatformApplicationMisc saves and restores (the
	 *  rule of Dream.Keys.G5, A177): a clipboard with an image, files or rich text is not touched. */
	bool IsPlainTextClipboard()
	{
#if PLATFORM_WINDOWS
		int32 TextFormats = 0;
		for (const UINT Format : { UINT(CF_TEXT), UINT(CF_UNICODETEXT), UINT(CF_OEMTEXT), UINT(CF_LOCALE) })
		{
			TextFormats += ::IsClipboardFormatAvailable(Format) ? 1 : 0;
		}
		return ::CountClipboardFormats() == TextFormats;
#else
		return false;
#endif
	}

	/** The colour a text block draws in (transparent when it inherits one). */
	FLinearColor ColorOf(const TSharedRef<SWidget>& TextBlock)
	{
		const FSlateColor Color = StaticCastSharedRef<STextBlock>(TextBlock)->GetColorAndOpacity();
		return Color.IsColorSpecified() ? Color.GetSpecifiedColor() : FLinearColor::Transparent;
	}

	/** Every text block of a row draws in Expected (bFirstOnly: only the row's first text block, e.g. a decoration
	 *  before a link); OutWhat names the first that does not. */
	bool TextsInColor(const TSharedRef<SWidget>& Row, const FLinearColor& Expected, bool bFirstOnly, FString& OutWhat)
	{
		TArray<TSharedRef<SWidget>> Texts;
		Collect(Row, IsTextBlock, Texts);
		if (Texts.IsEmpty())
		{
			OutWhat = TEXT("no text");
			return false;
		}
		for (int32 Index = 0; Index < (bFirstOnly ? 1 : Texts.Num()); ++Index)
		{
			if (!ColorOf(Texts[Index]).Equals(Expected, 0.001f))
			{
				OutWhat = FString::Printf(TEXT("\"%s\" in %s, expected %s"), *TextOf(Texts[Index]), *ColorOf(Texts[Index]).ToString(), *Expected.ToString());
				return false;
			}
		}
		return true;
	}

	/** A text block that is an icon or a gutter: the slice-B glyphs, a lone '›' or "<n> ›". */
	bool IsIconText(const FString& Text)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		static const TCHAR* Glyphs[] = { TEXT("✖"), TEXT("⚠"), TEXT("◆"), TEXT("↪"), TEXT("…"), TEXT("›"), TEXT("❌") };
		for (const TCHAR* Glyph : Glyphs)
		{
			if (Trimmed == Glyph)
			{
				return true;
			}
		}
		return Trimmed.EndsWith(TEXT(" ›"));
	}

	/** The stored line of a run with this type whose text contains Needle; 0 when none. */
	uint64 SeqOf(const DreamTest::FRunResult& Run, dream::LineType Type, const FString& Needle)
	{
		for (const DreamTest::FOutputLine& Line : Run.Lines)
		{
			if (Line.Type == Type && Line.Text.Contains(Needle))
			{
				return Line.Seq;
			}
		}
		return 0;
	}

	const DreamTest::FOutputLine* LineOf(const DreamTest::FRunResult& Run, uint64 Seq)
	{
		return Run.Lines.FindByPredicate([Seq](const DreamTest::FOutputLine& Line) { return Line.Seq == Seq; });
	}

	/** The rows the Output view has for a run. */
	struct FRows
	{
		TArray<EDreamOutputRowKind> Kinds;
		TArray<FString> Texts;
		TArray<uint64> Seqs;

		int32 IndexOfSeq(uint64 Seq) const { return Seq != 0 ? Seqs.IndexOfByKey(Seq) : INDEX_NONE; }
		int32 Count(EDreamOutputRowKind Kind) const
		{
			return static_cast<int32>(Algo::CountIf(Kinds, [Kind](EDreamOutputRowKind Each) { return Each == Kind; }));
		}
		FString Describe() const { return FString::Join(Texts, TEXT(" | ")); }
	};
}

BEGIN_DEFINE_SPEC(FDreamOutputMayaStyleSpec, "Dream.Output.MayaStyle", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	bool bSavedRunHeaders = false;
	FDreamOutputVisibility SavedVisibility;
	/** Declares one log line of a run under test as expected (any number of times, or at least once when bRequired):
	 *  an anchored regex over the whole message, so an assertion message that quotes the line is never taken for it. */
	void ExpectLogLine(const FString& Regex, ELogVerbosity::Type Verbosity, bool bRequired = false);
	/** The docked Output view, active and following; null with an error otherwise. */
	TSharedPtr<SDreamOutputView> ShowOutputView();
	DreamOutputMayaStyleSpec::FRows RowsOf(const SDreamOutputView& View, int32 RunNumber) const;
	/** Every shown row: no SImage, no icon or gutter text. */
	void ExpectNoIcons(const SDreamOutputView& View);
END_DEFINE_SPEC(FDreamOutputMayaStyleSpec)

TSharedPtr<SDreamOutputView> FDreamOutputMayaStyleSpec::ShowOutputView()
{
	const TSharedPtr<SDreamOutputArea> Area = Tab.IsValid() ? Tab->GetOutputArea() : nullptr;
	if (!TestTrue(TEXT("the output area"), Area.IsValid()))
	{
		return nullptr;
	}
	Area->SetActiveTab(EDreamOutputTab::Output);
	const TSharedPtr<SDreamOutputView> View = Area->GetOutputView();
	TestTrue(TEXT("the Output view"), View.IsValid());
	return View;
}

DreamOutputMayaStyleSpec::FRows FDreamOutputMayaStyleSpec::RowsOf(const SDreamOutputView& View, int32 RunNumber) const
{
	DreamOutputMayaStyleSpec::FRows Rows;
	View.GetRowsForTest(RunNumber, Rows.Kinds, Rows.Texts, Rows.Seqs);
	return Rows;
}

void FDreamOutputMayaStyleSpec::ExpectNoIcons(const SDreamOutputView& View)
{
	using namespace DreamOutputMayaStyleSpec;
	const TArray<TSharedRef<SWidget>> Shown = View.GetShownRowWidgetsForTest();
	TestTrue(TEXT("the view shows rows"), Shown.Num() > 0);
	for (const TSharedRef<SWidget>& Row : Shown)
	{
		TArray<TSharedRef<SWidget>> Found;
		Collect(Row, [](const TSharedRef<SWidget>& Widget)
		{
			return Widget->GetType() == FName(TEXT("SImage")) || (IsTextBlock(Widget) && IsIconText(TextOf(Widget)));
		}, Found);
		if (Found.Num() > 0)
		{
			AddError(FString::Printf(TEXT("a row draws an icon or a gutter (%s): \"%s\""), *Found[0]->GetTypeAsString(), *DrawnText(Row)));
			return;
		}
	}
}

void FDreamOutputMayaStyleSpec::ExpectLogLine(const FString& Regex, ELogVerbosity::Type Verbosity, bool bRequired)
{
	// The framework matches both the raw line and "LogPython: <line>" (FAutomationTestOutputDevice adds the category).
	const FString Anchored = FString::Printf(TEXT("^(LogPython: )?%s$"), *Regex);
	if (Verbosity == ELogVerbosity::Error)
	{
		AddExpectedError(Anchored, EAutomationExpectedErrorFlags::Contains, bRequired ? 0 : -1);
	}
	else
	{
		AddExpectedMessage(Anchored, Verbosity, EAutomationExpectedMessageFlags::Contains, bRequired ? 0 : -1);
	}
}

void FDreamOutputMayaStyleSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		bSavedRunHeaders = UDreamUserSettings::Get()->bShowRunHeaders;
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::RunHeaders, false);     // PO-04: the default
		// Every line type shown (the developer's Echo input may be off), through the store as the Show menu sets it;
		// bEchoInput is not written. Restored in AfterEach.
		FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		SavedVisibility = Store.GetVisibility();
		FDreamOutputVisibility All = SavedVisibility;
		All.bStdout = All.bStderr = All.bResults = All.bInfo = All.bWarnings = All.bErrors = All.bEcho = All.bNotice = true;
		Store.SetVisibility(All);
	});

	AfterEach([this]()
	{
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::RunHeaders, bSavedRunHeaders);
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetOutputStore().SetVisibility(SavedVisibility);
		}
		if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
		{
			if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
			{
				View->ScrollToEnd();
			}
		}
		Tab.Reset();
	});

	LatentIt("draws echo without a gutter, a multi-line result, a warning and unprefixed stdout and stderr, with no icons",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputMayaStyleSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Code = FString::Printf(TEXT(
			"import sys, warnings\n"
			"class MayaRepr_%s:\n"
			"    def __repr__(self):\n"
			"        return 'first_%s\\nmiddle_%s\\nlast_%s'\n"
			"print('plain_%s')\n"
			"sys.stderr.write('stderr_%s\\n')\n"
			"warnings.warn('warn_%s')\n"
			"MayaRepr_%s()\n"), *Sfx, *Sfx, *Sfx, *Sfx, *Sfx, *Sfx, *Sfx, *Sfx);
		// The stderr line reaches LogPython at Error and the warning at Warning (§6.3): lines under test, not failures.
		ExpectLogLine(FString::Printf(TEXT("stderr_%s"), *Sfx), ELogVerbosity::Error, /*bRequired*/ true);
		ExpectLogLine(FString::Printf(TEXT("<dream:[^>]*>:7: UserWarning: warn_%s"), *Sfx), ELogVerbosity::Warning, /*bRequired*/ true);
		ExpectLogLine(FString::Printf(TEXT("\\s+warnings\\.warn\\('warn_%s'\\)"), *Sfx), ELogVerbosity::Warning);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Code, TEXT("MayaEcho"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid BufferId = Buffer->GetId();
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("the buffer run"), [Buffer]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 8);
		}, Run);
		Steps.Do(TEXT("follow the Output view"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (View.IsValid())
			{
				View->ScrollToEnd();
			}
			return View.IsValid();
		});
		Steps.WaitFrames(4);
		Steps.Do(TEXT("check the rows"), [this, Run, Sfx]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				return false;
			}
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			const FRows Rows = RowsOf(*View, Run->RunNumber);
			const FString All = Rows.Describe();
			AddInfo(FString::Printf(TEXT("rows: %s"), *All));
			AddInfo(FString::Printf(TEXT("lines: %s"), *Run->Describe()));
			TestEqual(*FString::Printf(TEXT("no header or footer row with run headers off: %s"), *All),
				Rows.Count(EDreamOutputRowKind::Header) + Rows.Count(EDreamOutputRowKind::Footer), 0);

			// Echo: the source line as it is (no "<n> ›" gutter, no prompt for a buffer run).
			int32 EchoRows = 0;
			for (int32 Index = 0; Index < Rows.Seqs.Num(); ++Index)
			{
				const DreamTest::FOutputLine* Line = LineOf(*Run, Rows.Seqs[Index]);
				if (Line != nullptr && Line->Type == dream::LineType::Echo)
				{
					++EchoRows;
					TestEqual(TEXT("an echo row draws its source line as it is"), Rows.Texts[Index], Line->Text);
				}
			}
			TestTrue(*FString::Printf(TEXT("the echo is shown: %s"), *All), EchoRows >= 1);
			const int32 FirstEcho = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Echo, TEXT("import sys, warnings")));
			if (TestTrue(*FString::Printf(TEXT("the first echo line has a row: %s"), *All), FirstEcho != INDEX_NONE))
			{
				TestEqual(TEXT("the first echo row"), Rows.Texts[FirstEcho], FString(TEXT("import sys, warnings")));
			}

			// stdout and stderr: unprefixed.
			const int32 Plain = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Stdout, TEXT("plain_") + Sfx));
			const int32 Stderr = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Stderr, TEXT("stderr_") + Sfx));
			if (TestTrue(*FString::Printf(TEXT("stdout and stderr rows: %s"), *All), Plain != INDEX_NONE && Stderr != INDEX_NONE))
			{
				TestEqual(TEXT("the stdout row is unprefixed"), Rows.Texts[Plain], TEXT("plain_") + Sfx);
				TestEqual(TEXT("the stderr row is unprefixed"), Rows.Texts[Stderr], TEXT("stderr_") + Sfx);
			}

			// The multi-line repr: one decoration (A63).
			const int32 First = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Result, TEXT("first_") + Sfx));
			const int32 Middle = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Result, TEXT("middle_") + Sfx));
			const int32 Last = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Result, TEXT("last_") + Sfx));
			if (TestTrue(*FString::Printf(TEXT("three result rows: %s"), *All), First != INDEX_NONE && Middle != INDEX_NONE && Last != INDEX_NONE))
			{
				TestEqual(TEXT("the repr's first row"), Rows.Texts[First], TEXT("# Result: first_") + Sfx);
				TestEqual(TEXT("its middle row has no decoration"), Rows.Texts[Middle], TEXT("middle_") + Sfx);
				TestEqual(TEXT("its last row ends with ' #'"), Rows.Texts[Last], TEXT("last_") + Sfx + TEXT(" #"));
			}

			// The warning: "# Warning: <text>", " #" on its last line (warnings.formatwarning adds the source line).
			TArray<int32> WarningRows;
			for (int32 Index = 0; Index < Rows.Seqs.Num(); ++Index)
			{
				const DreamTest::FOutputLine* Line = LineOf(*Run, Rows.Seqs[Index]);
				if (Line != nullptr && Line->Type == dream::LineType::Warning)
				{
					WarningRows.Add(Index);
				}
			}
			if (TestTrue(*FString::Printf(TEXT("warning rows: %s"), *Run->Describe()), WarningRows.Num() >= 1))
			{
				const FString& Head = Rows.Texts[WarningRows[0]];
				TestTrue(*FString::Printf(TEXT("the warning reads '# Warning: ...: UserWarning: warn_...': \"%s\""), *Head),
					Head.StartsWith(TEXT("# Warning: ")) && Head.Contains(TEXT("UserWarning: warn_") + Sfx));
				TestTrue(*FString::Printf(TEXT("the warning's last row ends with ' #': \"%s\""), *Rows.Texts[WarningRows.Last()]),
					Rows.Texts[WarningRows.Last()].EndsWith(TEXT(" #")));
				for (int32 Index = 1; Index < WarningRows.Num(); ++Index)
				{
					TestFalse(*FString::Printf(TEXT("a warning's later line has no decoration of its own: \"%s\""), *Rows.Texts[WarningRows[Index]]),
						Rows.Texts[WarningRows[Index]].StartsWith(TEXT("#")));
				}
				if (WarningRows.Num() > 1)
				{
					TestFalse(TEXT("the warning's first row has no ' #' when lines follow"), Head.EndsWith(TEXT(" #")));
				}
			}

			// What the shown rows draw: no icons, and the decorated text.
			ExpectNoIcons(*View);
			// Echo on: the echo is drawn in the theme's echo colour, which stands out from the history's background.
			if (FirstEcho != INDEX_NONE)
			{
				const TSharedPtr<SWidget> EchoRow = View->GetRunRowWidgetForTest(Run->RunNumber, FirstEcho);
				TArray<TSharedRef<SWidget>> Texts;
				if (TestTrue(TEXT("the first echo row is shown"), EchoRow.IsValid()))
				{
					// Everything the row draws, so a gutter ("1 ›", a line number) would fail here (tests-r6 m11).
					TestEqual(TEXT("the echo row draws its source line and nothing else"), DrawnText(EchoRow.ToSharedRef()),
						FString(TEXT("import sys, warnings")));
					Collect(EchoRow.ToSharedRef(), [](const TSharedRef<SWidget>& Widget)
					{
						return IsTextBlock(Widget) && TextOf(Widget) == TEXT("import sys, warnings");
					}, Texts);
				}
				if (TestEqual(TEXT("the echo row draws its source line in one text"), Texts.Num(), 1))
				{
					const FSlateColor Color = StaticCastSharedRef<STextBlock>(Texts[0])->GetColorAndOpacity();
					const FLinearColor Drawn = Color.IsColorSpecified() ? Color.GetSpecifiedColor() : FLinearColor::Transparent;
					const FLinearColor Echo = FDreamThemes::LinearColor(EDreamThemeSlot::OutEcho);
					const FLinearColor Background = FDreamThemes::LinearColor(EDreamThemeSlot::OutputBackground);
					TestTrue(*FString::Printf(TEXT("the echo is in the theme's echo colour (%s, expected %s)"), *Drawn.ToString(), *Echo.ToString()),
						Drawn.Equals(Echo, 0.001f));
					TestTrue(*FString::Printf(TEXT("the echo is visible on the history background (luminance %.3f on %.3f, alpha %.2f)"),
						Drawn.GetLuminance(), Background.GetLuminance(), Drawn.A), Drawn.A > 0.5f && FMath::Abs(Drawn.GetLuminance() - Background.GetLuminance()) > 0.05f);
				}
			}
			for (const int32 Index : { First, Last })
			{
				if (Index == INDEX_NONE)
				{
					continue;
				}
				const TSharedPtr<SWidget> Widget = View->GetRunRowWidgetForTest(Run->RunNumber, Index);
				if (TestTrue(TEXT("the result rows are shown (the run is the newest output)"), Widget.IsValid()))
				{
					TestEqual(TEXT("the row draws its decorated text"), DrawnText(Widget.ToSharedRef()), Rows.Texts[Index]);
				}
			}
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, TEXT("stderr_") + Sfx);
		Steps.Finally(TEXT("close the scratch tab"), [BufferId]()
		{
			DreamTest::CloseBuffer(BufferId);
		});
		Steps.Run(Done);
	});

	LatentIt("draws a chained traceback as # lines ending in # Error: <Type>: <message> #, and its links open the code",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputMayaStyleSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Code = FString::Printf(TEXT(
			"import unreal\n"
			"unreal.log_error('logerr_%s')\n"
			"def boom_%s():\n"
			"    return 1 / 0\n"
			"try:\n"
			"    boom_%s()\n"
			"except ZeroDivisionError as cause_%s:\n"
			"    raise ValueError('chained_%s') from cause_%s\n"), *Sfx, *Sfx, *Sfx, *Sfx, *Sfx, *Sfx);
		const FString Summary = FString::Printf(TEXT("ValueError: chained_%s"), *Sfx);
		// The run fails on purpose: its log_error line and its traceback reach LogPython at Error (§6.3), one log line per
		// block line, blank lines included. Anchored, so the assertions below that quote these lines still fail visibly.
		ExpectLogLine(FString::Printf(TEXT("logerr_%s"), *Sfx), ELogVerbosity::Error, /*bRequired*/ true);
		ExpectLogLine(FString::Printf(TEXT("ValueError: chained_%s"), *Sfx), ELogVerbosity::Error, /*bRequired*/ true);
		ExpectLogLine(TEXT("ZeroDivisionError: division by zero"), ELogVerbosity::Error, /*bRequired*/ true);
		ExpectLogLine(TEXT("Traceback \\(most recent call last\\):"), ELogVerbosity::Error);
		ExpectLogLine(TEXT("The above exception was the direct cause of the following exception:"), ELogVerbosity::Error);
		ExpectLogLine(TEXT("\\s+File \"<dream:[^\"]*>\", line \\d+, in \\S+"), ELogVerbosity::Error);
		ExpectLogLine(FString::Printf(TEXT("\\s+(boom_%s\\(\\)|return 1 / 0|raise ValueError\\('chained_%s'\\) from cause_%s)"), *Sfx, *Sfx, *Sfx),
			ELogVerbosity::Error);
		ExpectLogLine(TEXT("\\s*[~^]+\\s*"), ELogVerbosity::Error);       // the caret lines (3.11 draws '^' and '~')
		ExpectLogLine(TEXT("\\s*"), ELogVerbosity::Error);                // the blank lines around the chain separator
		// The run's one LogDream summary of a failed run (A44), at Warning.
		AddExpectedMessage(FString::Printf(TEXT("^(LogDream: )?Run #\\d+ error ValueError: chained_%s .*$"), *Sfx), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, -1);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Code, TEXT("MayaError"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid BufferId = Buffer->GetId();
		TSharedRef<FGuid> OtherId = MakeShared<FGuid>();
		TSharedRef<int32> LinkRow = MakeShared<int32>(INDEX_NONE);
		TSharedRef<int32> ExceptionRow = MakeShared<int32>(INDEX_NONE);
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("the failing buffer run"), [Buffer]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 8);
		}, Run);
		Steps.Do(TEXT("check the error rows"), [this, Run, Sfx, Summary, LinkRow, ExceptionRow]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Error))
			{
				return false;
			}
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ScrollToEnd();
			const FRows Rows = RowsOf(*View, Run->RunNumber);
			const FString All = Rows.Describe();

			const int32 LogError = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Error, TEXT("logerr_") + Sfx));
			if (TestTrue(*FString::Printf(TEXT("the log_error row: %s"), *All), LogError != INDEX_NONE))
			{
				TestEqual(TEXT("an unreal.log_error line reads '# Error: <text> #'"), Rows.Texts[LogError], FString::Printf(TEXT("# Error: logerr_%s #"), *Sfx));
			}

			// The traceback block: every Error row after the log_error line.
			const int32 TracebackStart = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Error, TEXT("Traceback (most recent call last):")));
			if (!TestTrue(*FString::Printf(TEXT("the traceback has rows: %s"), *Run->Describe()), TracebackStart != INDEX_NONE))
			{
				return false;
			}
			TArray<int32> Block;
			for (int32 Index = TracebackStart; Index < Rows.Seqs.Num(); ++Index)
			{
				const DreamTest::FOutputLine* Line = LineOf(*Run, Rows.Seqs[Index]);
				if (Line != nullptr && Line->Type == dream::LineType::Error)
				{
					Block.Add(Index);
				}
			}
			int32 ErrorLines = 0;
			for (const int32 Index : Block)
			{
				TestTrue(*FString::Printf(TEXT("every traceback row starts with '# ': \"%s\""), *Rows.Texts[Index]), Rows.Texts[Index].StartsWith(TEXT("# ")));
				ErrorLines += Rows.Texts[Index].StartsWith(TEXT("# Error: ")) ? 1 : 0;
			}
			TestEqual(*FString::Printf(TEXT("one '# Error:' line in the traceback: %s"), *All), ErrorLines, 1);
			TestEqual(TEXT("the first row"), Rows.Texts[Block[0]], FString(TEXT("# Traceback (most recent call last):")));
			TestEqual(TEXT("the final line reads '# Error: <Type>: <message> #'"), Rows.Texts[Block.Last()], FString::Printf(TEXT("# Error: %s #"), *Summary));
			*ExceptionRow = Block.Last();
			const int32 Cause = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Error, TEXT("ZeroDivisionError: division by zero")));
			if (TestTrue(TEXT("the chain's first exception has a row"), Cause != INDEX_NONE))
			{
				TestEqual(TEXT("the chain's first exception is a plain '# ' line"), Rows.Texts[Cause], FString(TEXT("# ZeroDivisionError: division by zero")));
			}
			const int32 Separator = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Error, TEXT("The above exception was the direct cause")));
			if (TestTrue(TEXT("the chain separator has a row"), Separator != INDEX_NONE))
			{
				TestTrue(TEXT("the chain separator is a '# ' line"), Rows.Texts[Separator].StartsWith(TEXT("# The above exception")));
			}
			// The frame of boom_<sfx> (line 4): its row keeps a link.
			const FString Frame = FString::Printf(TEXT("line 4, in boom_%s"), *Sfx);
			*LinkRow = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Error, Frame));
			TestTrue(*FString::Printf(TEXT("the frame row of boom_%s: %s"), *Sfx, *All), *LinkRow != INDEX_NONE);
			return *LinkRow != INDEX_NONE;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the exception row is in the theme's error colour"), [this, Run, ExceptionRow]()
		{
			// "All in the theme's error colour" (PO-04 point 2); the view follows the end, where the exception row is.
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const TSharedPtr<SWidget> Exception = View.IsValid() ? View->GetRunRowWidgetForTest(Run->RunNumber, *ExceptionRow) : nullptr;
			if (TestTrue(TEXT("the exception row is shown (the newest output)"), Exception.IsValid()))
			{
				FString What;
				TestTrue(*FString::Printf(TEXT("the exception row is in the theme's error colour (%s)"), *What),
					DreamOutputMayaStyleSpec::TextsInColor(Exception.ToSharedRef(), FDreamThemes::LinearColor(EDreamThemeSlot::OutError), false, What));
			}
			return true;
		});
		Steps.Do(TEXT("open another tab, then show the frame row"), [this, Run, LinkRow, OtherId]()
		{
			const TSharedPtr<FDreamBuffer> Other = DreamTest::OpenScratch(*this, *Tab, TEXT("pass\n"), TEXT("MayaOther"));
			if (!Other.IsValid())
			{
				return false;
			}
			*OtherId = Other->GetId();
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			return View.IsValid() && TestTrue(TEXT("the frame row can be selected"), View->SelectRunRowForTest(Run->RunNumber, *LinkRow));
		});
		Steps.WaitFrames(4);
		Steps.Do(TEXT("click the link: the run's tab opens at line 4"), [this, Run, LinkRow, BufferId]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const TSharedPtr<SWidget> Row = View.IsValid() ? View->GetRunRowWidgetForTest(Run->RunNumber, *LinkRow) : nullptr;
			if (!TestTrue(TEXT("the frame row is shown"), Row.IsValid()))
			{
				return false;
			}
			ExpectNoIcons(*View);
			// "All in the theme's error colour" (PO-04 point 2): the exception row, and a frame row's "# " before its link.
			const FLinearColor ErrorColor = FDreamThemes::LinearColor(EDreamThemeSlot::OutError);
			FString What;
			TestTrue(*FString::Printf(TEXT("the frame row's decoration is in the theme's error colour (%s)"), *What),
				DreamOutputMayaStyleSpec::TextsInColor(Row.ToSharedRef(), ErrorColor, /*bFirstOnly*/ true, What));
			TArray<TSharedRef<SWidget>> Links;
			DreamOutputMayaStyleSpec::Collect(Row.ToSharedRef(), [](const TSharedRef<SWidget>& Widget)
			{
				return Widget->GetType() == FName(TEXT("SHyperlink"));
			}, Links);
			if (!TestEqual(*FString::Printf(TEXT("the frame row has one link: \"%s\""), *DreamOutputMayaStyleSpec::DrawnText(Row.ToSharedRef())), Links.Num(), 1))
			{
				return false;
			}
			const FString LinkText = DreamOutputMayaStyleSpec::DrawnText(Links[0]);
			TestTrue(*FString::Printf(TEXT("the link is the File \"<dream:...>\", line 4 text: \"%s\""), *LinkText),
				LinkText.StartsWith(TEXT("File \"<dream:")) && LinkText.EndsWith(TEXT("line 4")));
			TestTrue(TEXT("the link is enabled (its tab is open)"), Links[0]->IsEnabled());
			StaticCastSharedRef<SButton>(Links[0])->SimulateClick();       // SHyperlink is an SButton: the click path
			const TSharedPtr<FDreamBuffer> Active = Tab->GetActiveBuffer();
			TestTrue(TEXT("the click activates the run's tab"), Active.IsValid() && Active->GetId() == BufferId);
			if (const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView())
			{
				int32 Line = 0;
				int32 Column = 0;
				Code->GetCaret(Line, Column);
				TestEqual(TEXT("the caret is on line 4 (0-based 3)"), Line, 3);
			}
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, Summary);
		Steps.Poll(TEXT("the run's LogDream summary is logged inside this test"), [Log, Summary]()
		{
			return Log->CountContaining(FName(TEXT("LogDream")), TEXT("error ") + Summary) > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Finally(TEXT("close the scratch tabs"), [BufferId, OtherId]()
		{
			DreamTest::CloseBuffer(*OtherId);
			DreamTest::CloseBuffer(BufferId);
		});
		Steps.Run(Done);
	});

	LatentIt("hides run headers and footers by default, and shows one muted line each when toggled on", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputMayaStyleSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a REPL run"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("headers_%s = 6\nheaders_%s * 7"), *Sfx, *Sfx));
		}, Run);
		// The echo rows' widgets are checked below, so let the view build the run's rows first, as case 1 does.
		Steps.Do(TEXT("follow the Output view"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (View.IsValid())
			{
				View->ScrollToEnd();
			}
			return View.IsValid();
		});
		Steps.WaitFrames(4);
		Steps.Do(TEXT("headers off: no header or footer row, and ScrollToRun selects the first row"), [this, Run, Sfx]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				return false;
			}
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			TestFalse(TEXT("run headers are off"), UDreamUserSettings::Get()->bShowRunHeaders);
			const FRows Rows = RowsOf(*View, Run->RunNumber);
			TestEqual(*FString::Printf(TEXT("no header row: %s"), *Rows.Describe()), Rows.Count(EDreamOutputRowKind::Header), 0);
			TestEqual(*FString::Printf(TEXT("no footer row: %s"), *Rows.Describe()), Rows.Count(EDreamOutputRowKind::Footer), 0);
			TestTrue(*FString::Printf(TEXT("the result reads '# Result: 42 #': %s"), *Rows.Describe()), Rows.Texts.Contains(TEXT("# Result: 42 #")));
			// (A226) Each statement's first line was a '>>> ' row before the owner dropped the prompts: now its source line.
			const FString EchoSources[] = { FString::Printf(TEXT("headers_%s = 6"), *Sfx), FString::Printf(TEXT("headers_%s * 7"), *Sfx) };
			for (const FString& Source : EchoSources)
			{
				const int32 Echo = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Echo, Source));
				if (!TestTrue(*FString::Printf(TEXT("the REPL echo '%s' has a row: %s"), *Source, *Rows.Describe()), Echo != INDEX_NONE))
				{
					continue;
				}
				TestEqual(TEXT("a REPL echo row reads its source line, with no prompt"), Rows.Texts[Echo], Source);
				const TSharedPtr<SWidget> EchoRow = View->GetRunRowWidgetForTest(Run->RunNumber, Echo);
				if (TestTrue(TEXT("the REPL echo row is shown"), EchoRow.IsValid()))
				{
					TestEqual(TEXT("a REPL echo row draws its source line and nothing else"), DrawnText(EchoRow.ToSharedRef()), Source);
				}
			}
			if (TestTrue(TEXT("the run has rows"), Rows.Seqs.Num() > 0) && TestTrue(TEXT("ScrollToRun finds the run"), View->ScrollToRun(Run->RunNumber)))
			{
				const DreamTest::FOutputLine* First = LineOf(*Run, Rows.Seqs[0]);
				TestTrue(TEXT("ScrollToRun selects the run's first row"), First != nullptr && View->GetSelectedText() == First->Text);
				TestEqual(TEXT("no header is selected"), View->GetSelectedHeaderRunForTest(), 0);
			}
			View->ScrollToEnd();
			return true;
		});
		Steps.Do(TEXT("turn View ▸ Run headers on"), []()
		{
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::RunHeaders, true);
			return UDreamUserSettings::Get()->bShowRunHeaders;
		});
		Steps.WaitFrames(4);
		Steps.Do(TEXT("headers on: one header and one footer row, each one muted line"), [this, Run]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			const FRows Rows = RowsOf(*View, Run->RunNumber);
			const FString All = Rows.Describe();
			if (!TestEqual(*FString::Printf(TEXT("one header row: %s"), *All), Rows.Count(EDreamOutputRowKind::Header), 1)
				|| !TestEqual(*FString::Printf(TEXT("one footer row: %s"), *All), Rows.Count(EDreamOutputRowKind::Footer), 1))
			{
				return false;
			}
			const FString RunTag = FString::Printf(TEXT("#%d"), Run->RunNumber);
			TestTrue(TEXT("the header comes first"), Rows.Kinds[0] == EDreamOutputRowKind::Header);
			TestTrue(TEXT("the footer comes last"), Rows.Kinds.Last() == EDreamOutputRowKind::Footer);
			TestTrue(*FString::Printf(TEXT("the header names the run: \"%s\""), *Rows.Texts[0]), Rows.Texts[0].Contains(RunTag + TEXT(" · REPL")));
			TestTrue(*FString::Printf(TEXT("the footer reads '└ #N ok ...': \"%s\""), *Rows.Texts.Last()), Rows.Texts.Last().StartsWith(TEXT("└ ") + RunTag + TEXT(" ok")));
			const int32 Result = Rows.Texts.IndexOfByKey(FString(TEXT("# Result: 42 #")));
			const TSharedPtr<SWidget> Header = View->GetRunRowWidgetForTest(Run->RunNumber, 0);
			const TSharedPtr<SWidget> Footer = View->GetRunRowWidgetForTest(Run->RunNumber, Rows.Kinds.Num() - 1);
			const TSharedPtr<SWidget> Line = Result != INDEX_NONE ? View->GetRunRowWidgetForTest(Run->RunNumber, Result) : nullptr;
			if (!TestTrue(TEXT("the header, footer and result rows are shown"), Header.IsValid() && Footer.IsValid() && Line.IsValid()))
			{
				return false;
			}
			const float LineHeight = static_cast<float>(Line->GetCachedGeometry().GetLocalSize().Y);
			const float HeaderHeight = static_cast<float>(Header->GetCachedGeometry().GetLocalSize().Y);
			const float FooterHeight = static_cast<float>(Footer->GetCachedGeometry().GetLocalSize().Y);
			AddInfo(FString::Printf(TEXT("row heights: line %.1f, header %.1f, footer %.1f px"), LineHeight, HeaderHeight, FooterHeight));
			TestTrue(TEXT("the header is one line"), LineHeight > 0.f && HeaderHeight > 0.f && HeaderHeight < LineHeight * 1.5f);
			TestTrue(TEXT("the footer is one line"), FooterHeight > 0.f && FooterHeight < LineHeight * 1.5f);
			const FLinearColor Muted = FDreamThemes::LinearColor(EDreamThemeSlot::OutHeader);
			FString What;
			TestTrue(*FString::Printf(TEXT("the header is in the theme's muted header colour (%s)"), *What),
				TextsInColor(Header.ToSharedRef(), Muted, /*bFirstOnly*/ false, What));
			What.Reset();
			TestTrue(*FString::Printf(TEXT("the footer is in the theme's muted header colour (%s)"), *What),
				TextsInColor(Footer.ToSharedRef(), Muted, /*bFirstOnly*/ false, What));
			TArray<TSharedRef<SWidget>> Links;
			DreamOutputMayaStyleSpec::Collect(Footer.ToSharedRef(), [](const TSharedRef<SWidget>& Widget)
			{
				return Widget->GetType() == FName(TEXT("SHyperlink"));
			}, Links);
			TestEqual(TEXT("the footer has no action links (they are in the context menu)"), Links.Num(), 0);
			ExpectNoIcons(*View);
			TestTrue(TEXT("ScrollToRun finds the run"), View->ScrollToRun(Run->RunNumber));
			TestEqual(TEXT("with headers on, ScrollToRun selects the header"), View->GetSelectedHeaderRunForTest(), Run->RunNumber);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("offers Re-run, Run in transaction and Copy in the context menu of a run's line, and Re-run runs it again",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputMayaStyleSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Again = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a REPL run without a transaction"), [Sfx]()
		{
			FDreamRunRequest Request = DreamTest::MakeReplRequest(FString::Printf(TEXT("print('rerun_%s')"), *Sfx));
			Request.TransactionOverride = false;        // U6 "Run without transaction": the menu then offers Run in transaction
			return Request;
		}, Run);
		Steps.Do(TEXT("select the run's stdout line and read the menu, then Re-run"), [this, Run, Again, Sfx]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				return false;
			}
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			const FRows Rows = RowsOf(*View, Run->RunNumber);
			const int32 Stdout = Rows.IndexOfSeq(SeqOf(*Run, dream::LineType::Stdout, TEXT("rerun_") + Sfx));
			if (!TestTrue(*FString::Printf(TEXT("the stdout row: %s"), *Rows.Describe()), Stdout != INDEX_NONE)
				|| !TestTrue(TEXT("the stdout row can be selected"), View->SelectRunRowForTest(Run->RunNumber, Stdout)))
			{
				return false;
			}
			TestEqual(TEXT("the selected row is a line, not a header"), View->GetSelectedHeaderRunForTest(), 0);
			const TArray<FString> Labels = View->GetContextMenuLabelsForTest();
			const FString Listed = FString::Join(Labels, TEXT(", "));
			for (const TCHAR* Expected : { TEXT("Copy"), TEXT("Copy run output"), TEXT("Re-run"), TEXT("Run in transaction") })
			{
				TestTrue(*FString::Printf(TEXT("the menu offers %s: %s"), Expected, *Listed), Labels.Contains(Expected));
			}
			TestFalse(*FString::Printf(TEXT("no Undo this run without a transaction: %s"), *Listed), Labels.Contains(TEXT("Undo this run")));
			if (!TestTrue(TEXT("Re-run runs"), View->InvokeContextMenuEntryForTest(TEXT("Re-run"))))
			{
				return false;
			}
			Again->RunNumber = DreamTest::GetActiveRunNumber();
			return TestTrue(*FString::Printf(TEXT("Re-run started a new numbered run (#%d after #%d)"), Again->RunNumber, Run->RunNumber),
				Again->RunNumber > Run->RunNumber);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the re-run"), Again);
		Steps.Do(TEXT("the re-run ran the same code"), [this, Run, Again, Sfx]()
		{
			DreamTest::ExpectOutcome(*this, *Again, EDreamRunOutcome::Ok);
			TestTrue(*FString::Printf(TEXT("the re-run printed again: %s"), *Again->Describe()),
				Again->HasLineContaining(dream::LineType::Stdout, TEXT("rerun_") + Sfx));
			TestEqual(TEXT("the same source"), DreamText::ToFString(Again->Record.SourceSnapshot.Text()), DreamText::ToFString(Run->Record.SourceSnapshot.Text()));
			return true;
		});
		TSharedRef<DreamTest::FRunResult> InTxn = MakeShared<DreamTest::FRunResult>();
		Steps.Do(TEXT("the first run's line again: Copy, then Run in transaction"), [this, Run, InTxn, Sfx]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const int32 Stdout = View.IsValid() ? RowsOf(*View, Run->RunNumber).IndexOfSeq(SeqOf(*Run, dream::LineType::Stdout, TEXT("rerun_") + Sfx)) : INDEX_NONE;
			if (!TestTrue(TEXT("the first run's stdout row can be selected again"), Stdout != INDEX_NONE && View->SelectRunRowForTest(Run->RunNumber, Stdout)))
			{
				return false;
			}
			// Copy: the raw line (§6.4). The clipboard is touched only when it is empty or holds text only, and restored (A177).
			if (IsPlainTextClipboard())
			{
				FString Saved;
				FPlatformApplicationMisc::ClipboardPaste(Saved);
				TestTrue(TEXT("Copy runs"), View->InvokeContextMenuEntryForTest(TEXT("Copy")));
				FString Copied;
				FPlatformApplicationMisc::ClipboardPaste(Copied);
				FPlatformApplicationMisc::ClipboardCopy(*Saved);
				TestTrue(TEXT("Copy put the selected line's raw text on the clipboard"), Copied == TEXT("rerun_") + Sfx);
			}
			else
			{
				AddInfo(TEXT("the clipboard holds more than text: Copy is left out so it stays as it is (A177)"));
			}
			if (!TestTrue(TEXT("Run in transaction runs"), View->InvokeContextMenuEntryForTest(TEXT("Run in transaction"))))
			{
				return false;
			}
			InTxn->RunNumber = DreamTest::GetActiveRunNumber();
			return TestTrue(TEXT("Run in transaction started a new numbered run"), InTxn->RunNumber > Run->RunNumber);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the run in a transaction"), InTxn);
		Steps.Do(TEXT("the run in a transaction had one"), [this, InTxn, Sfx]()
		{
			DreamTest::ExpectOutcome(*this, *InTxn, EDreamRunOutcome::Ok);
			TestTrue(*FString::Printf(TEXT("it ran the same code: %s"), *InTxn->Describe()), InTxn->HasLineContaining(dream::LineType::Stdout, TEXT("rerun_") + Sfx));
			TestTrue(*FString::Printf(TEXT("with a transaction this time (status %d)"), static_cast<int32>(InTxn->Record.Txn.Status)),
				InTxn->Record.Txn.Status != EDreamTxnStatus::Off);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("shows a run that ends other than ok with no error line as one Maya-style line while run headers are off (r6 M1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputMayaStyleSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		// Each run that does not end ok writes one LogDream summary at Warning (A44).
		AddExpectedMessage(TEXT("^(LogDream: )?Run #\\d+ (error|not executed|cancelled) .*$"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, -1);
		TSharedRef<DreamTest::FRunResult> Exited = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Stopped = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a run that exits with code 3 and writes no line"), []()
		{
			return DreamTest::MakeReplRequest(TEXT("raise SystemExit(3)"));
		}, Exited);
		Steps.Do(TEXT("submit a run and stop it before its code starts"), [this, Stopped, Sfx]()
		{
			Stopped->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(FString::Printf(TEXT("print('never_%s')"), *Sfx)));
			return Stopped->RunNumber > 0
				&& TestTrue(TEXT("Stop is taken while the run is starting"), FDreamEditorModule::Get().GetRunService().RequestStopActive(EDreamStopSource::Api));
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the stopped run"), Stopped);
		Steps.Do(TEXT("follow the Output view"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (View.IsValid())
			{
				View->ScrollToEnd();
			}
			return View.IsValid();
		});
		Steps.WaitFrames(4);
		Steps.Do(TEXT("each run shows one outcome line, the run's target"), [this, Exited, Stopped]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const FDreamRunService& Runs = FDreamEditorModule::Get().GetRunService();
			const FDreamRunRecord* ExitRec = Runs.FindRecord(Exited->RunNumber);
			const FDreamRunRecord* StopRec = Runs.FindRecord(Stopped->RunNumber);
			if (!View.IsValid() || !DreamTest::ExpectOutcome(*this, *Exited, EDreamRunOutcome::Error)
				|| !TestTrue(TEXT("both records"), ExitRec != nullptr && StopRec != nullptr)
				|| !TestTrue(*FString::Printf(TEXT("the second run was stopped: %s"), *Stopped->Describe()), DreamOutputRows::IsStoppedRun(*StopRec)))
			{
				return false;
			}
			const FRows ExitRows = RowsOf(*View, Exited->RunNumber);
			const FRows StopRows = RowsOf(*View, Stopped->RunNumber);
			AddInfo(FString::Printf(TEXT("exited: %s"), *ExitRows.Describe()));
			AddInfo(FString::Printf(TEXT("stopped: %s"), *StopRows.Describe()));
			TestEqual(TEXT("no header or footer row with headers off"), ExitRows.Count(EDreamOutputRowKind::Header) + ExitRows.Count(EDreamOutputRowKind::Footer), 0);
			if (TestEqual(TEXT("the exited run has one outcome line"), ExitRows.Count(EDreamOutputRowKind::RunOutcome), 1))
			{
				TestTrue(TEXT("it is the run's last row"), ExitRows.Kinds.Last() == EDreamOutputRowKind::RunOutcome);
				TestEqual(TEXT("it reads '# Error: Run #N <the footer's outcome text> #'"), ExitRows.Texts.Last(),
					FString::Printf(TEXT("# Error: %s #"), *DreamOutputRows::RunOutcomeText(ExitRec, static_cast<uint32>(Exited->RunNumber))));
				if (const TSharedPtr<SWidget> Widget = View->GetRunRowWidgetForTest(Exited->RunNumber, ExitRows.Kinds.Num() - 1))
				{
					FString What;
					TestTrue(*FString::Printf(TEXT("in the theme's error colour (%s)"), *What),
						TextsInColor(Widget.ToSharedRef(), FDreamThemes::LinearColor(EDreamThemeSlot::OutError), false, What));
				}
			}
			if (TestEqual(TEXT("the stopped run shows only its outcome line"), StopRows.Kinds.Num(), 1)
				&& TestTrue(TEXT("an outcome line"), StopRows.Kinds[0] == EDreamOutputRowKind::RunOutcome))
			{
				TestEqual(TEXT("a stopped run's line is a warning"), StopRows.Texts[0],
					FString::Printf(TEXT("# Warning: %s #"), *DreamOutputRows::RunOutcomeText(StopRec, static_cast<uint32>(Stopped->RunNumber))));
				if (const TSharedPtr<SWidget> Widget = View->GetRunRowWidgetForTest(Stopped->RunNumber, 0))
				{
					FString What;
					TestTrue(*FString::Printf(TEXT("in the theme's warning colour (%s)"), *What),
						TextsInColor(Widget.ToSharedRef(), FDreamThemes::LinearColor(EDreamThemeSlot::OutWarning), false, What));
				}
				else
				{
					AddError(TEXT("the stopped run's line is shown (the newest output)"));
				}
				const FString Raw = DreamOutputRows::RunOutcomeText(StopRec, static_cast<uint32>(Stopped->RunNumber));
				TestTrue(TEXT("ScrollToRun (RV-16) finds the stopped run"), View->ScrollToRun(Stopped->RunNumber));
				TestEqual(TEXT("and selects its outcome line (copied raw)"), View->GetSelectedText(), Raw);
				View->ResetSelectionForTest();
				TestTrue(TEXT("a REPL link to its line 1 falls back to the run"), View->ScrollToEchoLine(Stopped->RunNumber, 1));
				TestEqual(TEXT("and selects the outcome line too"), View->GetSelectedText(), Raw);
				const TArray<FString> Labels = View->GetContextMenuLabelsForTest();
				TestTrue(*FString::Printf(TEXT("its context menu offers Re-run: %s"), *FString::Join(Labels, TEXT(", "))), Labels.Contains(TEXT("Re-run")));
			}
			// The outcome line stands for the header: ScrollToRun selects it, not the run's echo above it (RV-16).
			View->ResetSelectionForTest();
			TestTrue(TEXT("ScrollToRun finds the exited run"), View->ScrollToRun(Exited->RunNumber));
			TestEqual(TEXT("and selects its outcome line, not its echo"), View->GetSelectedText(),
				DreamOutputRows::RunOutcomeText(ExitRec, static_cast<uint32>(Exited->RunNumber)));
			// Echo hidden: the exited run shows nothing but its outcome line, and that is what ScrollToRun selects.
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			FDreamOutputVisibility NoEcho = Store.GetVisibility();
			NoEcho.bEcho = false;
			Store.SetVisibility(NoEcho);
			const FRows Bare = RowsOf(*View, Exited->RunNumber);
			TestTrue(*FString::Printf(TEXT("with echo hidden only the outcome line is left: %s"), *Bare.Describe()),
				Bare.Kinds.Num() == 1 && Bare.Kinds[0] == EDreamOutputRowKind::RunOutcome);
			TestTrue(TEXT("ScrollToRun finds the exited run"), View->ScrollToRun(Exited->RunNumber));
			TestEqual(TEXT("and selects its outcome line"), View->GetSelectedText(), DreamOutputRows::RunOutcomeText(ExitRec, static_cast<uint32>(Exited->RunNumber)));
			NoEcho.bEcho = true;
			Store.SetVisibility(NoEcho);
			View->ScrollToEnd();
			return true;
		});
		Steps.Do(TEXT("turn View ▸ Run headers on"), []()
		{
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::RunHeaders, true);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("headers on: the footer, no outcome line"), [this, Exited]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const FRows Rows = View.IsValid() ? RowsOf(*View, Exited->RunNumber) : FRows();
			TestEqual(TEXT("no outcome line with headers on"), Rows.Count(EDreamOutputRowKind::RunOutcome), 0);
			TestEqual(TEXT("the footer instead"), Rows.Count(EDreamOutputRowKind::Footer), 1);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("hides the outcome line only behind the run's own shown exception block, not behind log_error lines (ruling on r7 m1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputMayaStyleSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		// unreal.log_error and the traceback reach LogPython at Error, each failed run's summary LogDream at Warning (§6.3, A44).
		ExpectLogLine(FString::Printf(TEXT("logexit_%s"), *Sfx), ELogVerbosity::Error, /*bRequired*/ true);
		ExpectLogLine(FString::Printf(TEXT("logstop_%s"), *Sfx), ELogVerbosity::Error, /*bRequired*/ true);
		ExpectLogLine(TEXT("ZeroDivisionError: division by zero"), ELogVerbosity::Error, /*bRequired*/ true);
		ExpectLogLine(TEXT("Traceback \\(most recent call last\\):"), ELogVerbosity::Error);
		ExpectLogLine(TEXT("\\s+File \"<dream:REPL#\\d+>\", line \\d+, in <module>"), ELogVerbosity::Error);
		ExpectLogLine(FString::Printf(TEXT("\\s+tb_%s = 1 / 0"), *Sfx), ELogVerbosity::Error);
		ExpectLogLine(TEXT("\\s*[~^]+\\s*"), ELogVerbosity::Error);
		AddExpectedMessage(TEXT("^(LogDream: )?Run #\\d+ (error|cancelled|not executed) .*$"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, -1);
		struct FStopState
		{
			bool bSaved = false;
			bool bRunPump = true;
			EDreamRunPresentationSetting Presentation = EDreamRunPresentationSetting::RunWindow;
			int32 RunNumber = 0;
			bool bStopIssued = false;
			FDelegateHandle PreTick;
		};
		TSharedRef<FStopState> Stop = MakeShared<FStopState>();
		TSharedRef<DreamTest::FRunResult> Exited = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Stopped = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Raised = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("unreal.log_error, then SystemExit(3)"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("import unreal\nunreal.log_error('logexit_%s')\nraise SystemExit(3)"), *Sfx));
		}, Exited);
		Steps.Do(TEXT("unreal.log_error, then a loop that a Stop from a pump ends"), [this, Stop, Stopped, Sfx]()
		{
			// A supervised presentation, so the run pumps and Slate ticks inside it (§5.5.3); the pre-tick probe presses Stop
			// once in a pump of this run, as Dream.Output.StatusItem does.
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Stop->bRunPump = Settings->bRunPump;
			Stop->Presentation = Settings->RunPresentation;
			Stop->bSaved = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			FDreamEditorModule& Module = FDreamEditorModule::Get();
			FRunSupervisor* Supervisor = &Module.GetSupervisor();
			FDreamRunService* Runs = &Module.GetRunService();
			Stop->PreTick = FSlateApplication::Get().OnPreTick().AddLambda([Stop, Supervisor, Runs](float)
			{
				const FDreamRunRecord* Active = Runs->GetActiveRecord();
				if (Stop->bStopIssued || Supervisor->GetPumpDepth() <= 0 || Active == nullptr || Active->RunNumber != Stop->RunNumber
					|| FPlatformTime::Seconds() - Active->StartSeconds < 0.3)
				{
					return;
				}
				Stop->bStopIssued = true;
				Runs->RequestStopActive(EDreamStopSource::Api);
			});
			Stopped->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(FString::Printf(
				TEXT("import unreal, time\nunreal.log_error('logstop_%s')\nfor _i in range(400):\n    time.sleep(0.01)"), *Sfx)));
			Stop->RunNumber = Stopped->RunNumber;
			return Stopped->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the stopped run"), Stopped, 60.0);
		DreamTest::AddRun(Steps, TEXT("a run whose own Python raises"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("tb_%s = 1 / 0"), *Sfx));
		}, Raised);
		Steps.Do(TEXT("follow the Output view"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (View.IsValid())
			{
				View->ScrollToEnd();
			}
			return View.IsValid();
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("log_error lines do not hide the outcome line; the run's shown traceback does"), [this, Exited, Stopped, Raised, Stop, Sfx]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const FDreamRunService& Runs = FDreamEditorModule::Get().GetRunService();
			const FDreamRunRecord* ExitRec = Runs.FindRecord(Exited->RunNumber);
			const FDreamRunRecord* StopRec = Runs.FindRecord(Stopped->RunNumber);
			if (!View.IsValid() || !TestTrue(TEXT("the records"), ExitRec != nullptr && StopRec != nullptr)
				|| !DreamTest::ExpectOutcome(*this, *Exited, EDreamRunOutcome::Error) || !DreamTest::ExpectOutcome(*this, *Raised, EDreamRunOutcome::Error)
				|| !TestTrue(TEXT("the probe pressed Stop inside a pump of the second run"), Stop->bStopIssued)
				|| !TestTrue(*FString::Printf(TEXT("the second run was stopped (outcome %d)"), static_cast<int32>(StopRec->Outcome)), DreamOutputRows::IsStoppedRun(*StopRec)))
			{
				return false;
			}
			const FRows ExitRows = RowsOf(*View, Exited->RunNumber);
			const FRows StopRows = RowsOf(*View, Stopped->RunNumber);
			const FRows RaiseRows = RowsOf(*View, Raised->RunNumber);
			AddInfo(FString::Printf(TEXT("exited: %s"), *ExitRows.Describe()));
			AddInfo(FString::Printf(TEXT("stopped: %s"), *StopRows.Describe()));
			AddInfo(FString::Printf(TEXT("raised: %s"), *RaiseRows.Describe()));
			// The exit-code run: its log_error line, then still its outcome line.
			TestTrue(TEXT("the exit-code run shows its log_error line"), ExitRows.Texts.Contains(FString::Printf(TEXT("# Error: logexit_%s #"), *Sfx)));
			TestTrue(TEXT("and after it the outcome line"), ExitRows.Kinds.Num() > 0 && ExitRows.Kinds.Last() == EDreamOutputRowKind::RunOutcome
				&& ExitRows.Texts.Last() == FString::Printf(TEXT("# Error: %s #"), *DreamOutputRows::RunOutcomeText(ExitRec, static_cast<uint32>(Exited->RunNumber))));
			// The stopped run: its log_error line, then the warning line.
			TestTrue(TEXT("the stopped run shows its log_error line"), StopRows.Texts.Contains(FString::Printf(TEXT("# Error: logstop_%s #"), *Sfx)));
			TestTrue(TEXT("and after it the warning line"), StopRows.Kinds.Num() > 0 && StopRows.Kinds.Last() == EDreamOutputRowKind::RunOutcome
				&& StopRows.Texts.Last() == FString::Printf(TEXT("# Warning: %s #"), *DreamOutputRows::RunOutcomeText(StopRec, static_cast<uint32>(Stopped->RunNumber))));
			// The traceback run: its exception block stands for the outcome.
			TestEqual(TEXT("the traceback run shows no outcome line"), RaiseRows.Count(EDreamOutputRowKind::RunOutcome), 0);
			TestTrue(TEXT("it shows its exception line instead"), RaiseRows.Texts.Contains(FString(TEXT("# Error: ZeroDivisionError: division by zero #"))));
			// A traceback the Errors filter hides does not hide the outcome line (and the hidden log_error lines change nothing).
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			FDreamOutputVisibility NoErrors = Store.GetVisibility();
			NoErrors.bErrors = false;
			Store.SetVisibility(NoErrors);
			TestEqual(TEXT("with Errors hidden the traceback run shows its outcome line"), RowsOf(*View, Raised->RunNumber).Count(EDreamOutputRowKind::RunOutcome), 1);
			TestEqual(TEXT("and the exit-code run still shows its own"), RowsOf(*View, Exited->RunNumber).Count(EDreamOutputRowKind::RunOutcome), 1);
			NoErrors.bErrors = true;
			Store.SetVisibility(NoErrors);
			View->ScrollToEnd();
			return true;
		});
		Steps.Finally(TEXT("the probe out, the run settings as they were"), [Stop]()
		{
			if (Stop->PreTick.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().OnPreTick().Remove(Stop->PreTick);
			}
			if (Stop->bSaved)
			{
				UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
				Settings->bRunPump = Stop->bRunPump;
				Settings->RunPresentation = Stop->Presentation;
			}
		});
		Steps.Run(Done);
	});

	LatentIt("redraws the rows made earlier when an error block grows across drains (R1, r6 m8)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputMayaStyleSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		// Dream-origin error lines reach LogPython at Error (A46): the lines under test.
		ExpectLogLine(TEXT("Traceback \\(most recent call last\\):"), ELogVerbosity::Error);
		ExpectLogLine(FString::Printf(TEXT("\\s+File \"drain_%s\\.py\", line 3, in <module>"), *Sfx), ELogVerbosity::Error);
		ExpectLogLine(FString::Printf(TEXT("\\s+boom_%s\\(\\)"), *Sfx), ELogVerbosity::Error);
		ExpectLogLine(FString::Printf(TEXT("ValueError: drain_%s"), *Sfx), ELogVerbosity::Error, /*bRequired*/ true);
		const FString Boom = FString::Printf(TEXT("    boom_%s()"), *Sfx);
		TSharedRef<int32> RebuildsBefore = MakeShared<int32>(0);
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		auto Push = [](EDreamStream Stream, const FString& Text)
		{
			FDreamEditorModule::Get().GetOutputStore().PushDreamLine(0, Stream, Text);
		};
		// The rows of run 0 (outside runs) by text: the boom row is the last one with that text, the traceback two above it.
		auto FindBoom = [Boom](const FRows& Rows)
		{
			for (int32 Index = Rows.Texts.Num() - 1; Index >= 2; --Index)
			{
				if (Rows.Texts[Index].Contains(Boom))
				{
					return Index;
				}
			}
			return int32(INDEX_NONE);
		};
		auto ExpectDrawn = [this](const SDreamOutputView& View, int32 Index, const FString& Expected, const TCHAR* What)
		{
			const TSharedPtr<SWidget> Widget = View.GetRunRowWidgetForTest(0, Index);
			if (TestTrue(*FString::Printf(TEXT("%s: the row is shown"), What), Widget.IsValid()))
			{
				TestEqual(*FString::Printf(TEXT("%s: what the row draws"), What), DrawnText(Widget.ToSharedRef()), Expected);
			}
		};
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("the first drain: a traceback without its exception line"), [this, Push, Sfx, Boom]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (View.IsValid())
			{
				View->ScrollToEnd();
			}
			Push(EDreamStream::Stdout, FString::Printf(TEXT("drain_start_%s"), *Sfx));     // ends any open error block of run 0
			Push(EDreamStream::Error, TEXT("Traceback (most recent call last):"));
			Push(EDreamStream::Error, FString::Printf(TEXT("  File \"drain_%s.py\", line 3, in <module>"), *Sfx));
			Push(EDreamStream::Error, Boom);
			return View.IsValid();
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("the rows drawn so far: the first line carries the error, the last the ' #'"), [this, Boom, FindBoom, ExpectDrawn, RebuildsBefore]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const FRows Rows = View.IsValid() ? RowsOf(*View, 0) : FRows();
			const int32 Index = FindBoom(Rows);
			if (!TestTrue(TEXT("the pushed rows are in the view"), Index != INDEX_NONE))
			{
				return false;
			}
			TestEqual(TEXT("without an exception line the first line takes '# Error: '"), Rows.Texts[Index - 2], FString(TEXT("# Error: Traceback (most recent call last):")));
			TestEqual(TEXT("the last line takes ' #'"), Rows.Texts[Index], TEXT("# ") + Boom + TEXT(" #"));
			ExpectDrawn(*View, Index - 2, Rows.Texts[Index - 2], TEXT("the traceback row"));
			ExpectDrawn(*View, Index, Rows.Texts[Index], TEXT("the source row"));
			*RebuildsBefore = View->GetRowRebuildsForTest();
			return true;
		});
		Steps.Do(TEXT("the second drain: the exception line"), [Push, Sfx]()
		{
			Push(EDreamStream::Error, FString::Printf(TEXT("ValueError: drain_%s"), *Sfx));
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("the rows made in the first drain are drawn again with the moved decorations"), [this, Boom, Sfx, FindBoom, ExpectDrawn, RebuildsBefore]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const FRows Rows = View.IsValid() ? RowsOf(*View, 0) : FRows();
			const int32 Index = FindBoom(Rows);
			if (!TestTrue(TEXT("the rows are still in the view"), Index != INDEX_NONE && Rows.Texts.IsValidIndex(Index + 1)))
			{
				return false;
			}
			TestEqual(TEXT("the first line is a plain '# ' line now"), Rows.Texts[Index - 2], FString(TEXT("# Traceback (most recent call last):")));
			TestEqual(TEXT("the source line lost its ' #'"), Rows.Texts[Index], TEXT("# ") + Boom);
			TestEqual(TEXT("the exception line"), Rows.Texts[Index + 1], FString::Printf(TEXT("# Error: ValueError: drain_%s #"), *Sfx));
			ExpectDrawn(*View, Index - 2, Rows.Texts[Index - 2], TEXT("the traceback row, made in the first drain"));
			ExpectDrawn(*View, Index, Rows.Texts[Index], TEXT("the source row, made in the first drain"));
			ExpectDrawn(*View, Index + 1, Rows.Texts[Index + 1], TEXT("the exception row"));
			TestTrue(TEXT("the shown rows were made again for it"), View->GetRowRebuildsForTest() > *RebuildsBefore);
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, FString::Printf(TEXT("ValueError: drain_%s"), *Sfx));
		Steps.Run(Done);
	});

	LatentIt("asks for a confirm, then undoes the recorded part of a partial run through the context menu (RT-08, r6 m8)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		if (!GEditor || !GEditor->Trans || !GEditor->GetEditorWorldContext().World())
		{
			AddError(TEXT("no editor world or transaction buffer"));
			Done.Execute();
			return;
		}
		using namespace DreamOutputMayaStyleSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Label = FString::Printf(TEXT("DreamT4Maya_%s"), *Sfx);
		struct FPinned
		{
			bool bSaved = false;
			bool bTransactionRepl = true;
			bool bRunPump = true;
			EDreamRunPresentationSetting Presentation = EDreamRunPresentationSetting::RunWindow;
			TOptional<SIZE_T> Budget;
			TWeakObjectPtr<AStaticMeshActor> Actor;
		};
		TSharedRef<FPinned> Pinned = MakeShared<FPinned>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		auto ActorAt = [Pinned]()
		{
			const AStaticMeshActor* Actor = Pinned->Actor.Get();
			return Actor != nullptr ? Actor->GetActorLocation() : FVector(-1.0);
		};
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("a fixture actor, an empty undo history and a 1-byte undo budget"), [this, Pinned, Label]()
		{
			// The pumping presentation and the REPL's one undo step, as Dream.Transactions.UndoLastRun pins them.
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Pinned->bTransactionRepl = Settings->bTransactionRepl;
			Pinned->bRunPump = Settings->bRunPump;
			Pinned->Presentation = Settings->RunPresentation;
			Pinned->bSaved = true;
			Settings->bTransactionRepl = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			UWorld* World = GEditor->GetEditorWorldContext().World();
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, FActorSpawnParameters());
			if (!TestTrue(TEXT("the fixture actor"), Actor != nullptr))
			{
				return false;
			}
			Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			Actor->SetActorLabel(Label);
			Pinned->Actor = Actor;
			GEditor->ResetTransaction(FText::FromString(TEXT("Dream.Output.MayaStyle: empty history")));
			FDreamTransactions& Transactions = FDreamEditorModule::Get().GetTransactions();
			Pinned->Budget = Transactions.GetBudgetBytes();
			Transactions.SetBudgetBytesForTest(1);         // the first pump after the move ends the record at 100% (Partial)
			return true;
		});
		Steps.Do(TEXT("a run that moves the actor, then pumps"), [this, Run, Label, Sfx]()
		{
			const TArray<FString> Lines = {
				FString::Printf(TEXT("import unreal, time as _t_%s"), *Sfx),
				TEXT("for _a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():"),
				FString::Printf(TEXT("    if _a.get_actor_label() == '%s':"), *Label),
				TEXT("        _a.root_component.modify()"),
				TEXT("        _a.set_actor_location(unreal.Vector(1.0, 2.0, 3.0), False, False)"),
				TEXT("for _ in range(80):"),
				FString::Printf(TEXT("    _t_%s.sleep(0.025)"), *Sfx),
				FString::Printf(TEXT("del _a, _t_%s"), *Sfx),
			};
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(FString::Join(Lines, TEXT("\n"))));
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the moving, pumping run"), Run, 60.0);
		Steps.Do(TEXT("Undo this run asks for a confirm first"), [this, Run, ActorAt]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok)
				|| !TestTrue(*FString::Printf(TEXT("the run's record is Partial (status %d)"), static_cast<int32>(Run->Record.Txn.Status)),
					Run->Record.Txn.Status == EDreamTxnStatus::Partial))
			{
				return false;
			}
			TestTrue(TEXT("the run moved the actor"), ActorAt().Equals(FVector(1.0, 2.0, 3.0), 0.01));
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (!View.IsValid() || !TestTrue(TEXT("a row of the run can be selected"), View->ScrollToRun(Run->RunNumber)))
			{
				return false;
			}
			TArray<FString> Labels = View->GetContextMenuLabelsForTest();
			if (!TestTrue(*FString::Printf(TEXT("the menu offers Undo this run: %s"), *FString::Join(Labels, TEXT(", "))), Labels.Contains(TEXT("Undo this run")))
				|| !TestTrue(TEXT("its first choice runs"), View->InvokeContextMenuEntryForTest(TEXT("Undo this run"))))
			{
				return false;
			}
			TestTrue(TEXT("the first choice only asks: the actor has not moved back"), ActorAt().Equals(FVector(1.0, 2.0, 3.0), 0.01));
			Labels = View->GetContextMenuLabelsForTest();
			TestTrue(*FString::Printf(TEXT("the menu now offers the confirm: %s"), *FString::Join(Labels, TEXT(", "))),
				Labels.Contains(TEXT("Confirm: undo the recorded part")) && !Labels.Contains(TEXT("Undo this run")));
			return TestTrue(TEXT("the confirm runs"), View->InvokeContextMenuEntryForTest(TEXT("Confirm: undo the recorded part")));
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the recorded part is undone"), [this, Run, ActorAt]()
		{
			TestTrue(*FString::Printf(TEXT("the actor is back where it was (%s)"), *ActorAt().ToString()), ActorAt().Equals(FVector::ZeroVector, 0.01));
			const FDreamRunRecord* Record = FDreamEditorModule::Get().GetRunService().FindRecord(Run->RunNumber);
			TestTrue(*FString::Printf(TEXT("the run's record reads Undone (status %d)"), Record ? static_cast<int32>(Record->Txn.Status) : -1),
				Record != nullptr && Record->Txn.Status == EDreamTxnStatus::Undone);
			return true;
		});
		Steps.Finally(TEXT("restore the budget and the settings, remove the fixture actor"), [Pinned]()
		{
			if (Pinned->Budget.IsSet() && FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetTransactions().SetBudgetBytesForTest(Pinned->Budget.GetValue());
			}
			if (AStaticMeshActor* Actor = Pinned->Actor.Get())
			{
				if (UWorld* World = Actor->GetWorld())
				{
					World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ false);
				}
			}
			if (Pinned->bSaved)
			{
				UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
				Settings->bTransactionRepl = Pinned->bTransactionRepl;
				Settings->bRunPump = Pinned->bRunPump;
				Settings->RunPresentation = Pinned->Presentation;
			}
		});
		Steps.Run(Done);
	});

	LatentIt("themes the REPL prompt strip and the history popup in a light theme (r6 M2, m7, arch-r6-m1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !Tab->GetOutputArea().IsValid())
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputMayaStyleSpec;
		TSharedRef<FString> SavedTheme = MakeShared<FString>(FDreamThemes::GetCurrentId());
		TSharedRef<bool> bSavedPrompt = MakeShared<bool>(UDreamUserSettings::Get()->bShowReplPrompt);
		TSharedRef<bool> bThemeSet = MakeShared<bool>(false);
		const TSharedPtr<SDreamReplStrip> Strip = Tab->GetOutputArea()->GetReplStrip();
		auto IsBorder = [](const TSharedRef<SWidget>& Widget) { return Widget->GetType() == FName(TEXT("SBorder")); };
		auto ImageOf = [](const TSharedRef<SWidget>& Border) { return StaticCastSharedRef<SBorder>(Border)->GetBorderImage(); };
		auto TintOf = [](EDreamThemeSlot Slot)
		{
			const FSlateBrush* Brush = FDreamThemes::Brush(Slot);
			return Brush != nullptr && Brush->TintColor.IsColorSpecified() ? Brush->TintColor.GetSpecifiedColor() : FLinearColor::Transparent;
		};
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("GitHub Light, with the REPL prompt shown"), [this, bThemeSet]()
		{
			*bThemeSet = TestTrue(TEXT("the GitHub Light theme applies"), FDreamThemes::SetCurrent(TEXT("GitHub Light"), /*bPersist*/ false));
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplPrompt, true);
			return *bThemeSet;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the prompt strip's rules and background are the theme's"), [this, Strip, IsBorder, ImageOf, TintOf]()
		{
			if (!TestTrue(TEXT("the REPL prompt strip"), Strip.IsValid()))
			{
				return false;
			}
			TArray<TSharedRef<SWidget>> Borders;
			Collect(Strip.ToSharedRef(), IsBorder, Borders);
			if (!TestTrue(TEXT("the strip has its two frame borders"), Borders.Num() >= 2))
			{
				return false;
			}
			const FSlateBrush* Group = FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder"));
			for (const TSharedRef<SWidget>& Border : Borders)
			{
				TestFalse(TEXT("no stock ToolPanel.GroupBorder band in the strip"), ImageOf(Border) == Group);
			}
			TestTrue(TEXT("the rules above and below the prompt are the theme's ChromeBorder brush"), ImageOf(Borders[0]) == FDreamThemes::Brush(EDreamThemeSlot::ChromeBorder));
			TestTrue(TEXT("on the input pane's EditorBackground"), ImageOf(Borders[1]) == FDreamThemes::Brush(EDreamThemeSlot::EditorBackground));
			const FLinearColor Rule = TintOf(EDreamThemeSlot::ChromeBorder);
			TestTrue(*FString::Printf(TEXT("the rule is tinted the light theme's border colour (%s)"), *Rule.ToString()),
				Rule.Equals(FDreamThemes::LinearColor(EDreamThemeSlot::ChromeBorder), 0.001f) && Rule.GetLuminance() > 0.4f);
			return true;
		});
		Steps.Do(TEXT("the history popup is the theme's chrome"), [this, Strip, IsBorder, ImageOf, TintOf]()
		{
			FDreamHistoryEntry Entry;
			Entry.Text = TEXT("maya_popup_entry = 1");
			const TSharedPtr<SWidget> Popup = Strip->OpenHistoryForTest({ Entry });
			if (!TestTrue(TEXT("the popup opens"), Popup.IsValid()))
			{
				return false;
			}
			TArray<TSharedRef<SWidget>> Borders;
			Collect(Popup.ToSharedRef(), IsBorder, Borders);
			const FSlateBrush* Menu = FAppStyle::GetBrush(TEXT("Menu.Background"));
			if (TestTrue(TEXT("the popup has its outline and its background"), Borders.Num() >= 2))
			{
				for (const TSharedRef<SWidget>& Border : Borders)
				{
					TestFalse(TEXT("no stock Menu.Background in the popup"), ImageOf(Border) == Menu);
				}
				TestTrue(TEXT("the outline is the theme's ChromeBorder brush"), ImageOf(Borders[0]) == FDreamThemes::Brush(EDreamThemeSlot::ChromeBorder));
				TestTrue(TEXT("the background is the theme's ChromeBackground brush"), ImageOf(Borders[1]) == FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground));
			}
			const FLinearColor Background = TintOf(EDreamThemeSlot::ChromeBackground);
			const FLinearColor Text = FDreamThemes::LinearColor(EDreamThemeSlot::ChromeText);
			TestTrue(*FString::Printf(TEXT("a light popup background (%s)"), *Background.ToString()), Background.GetLuminance() > 0.4f);
			TestTrue(*FString::Printf(TEXT("its text reads on it (luminance %.2f on %.2f)"), Text.GetLuminance(), Background.GetLuminance()),
				FMath::Abs(Background.GetLuminance() - Text.GetLuminance()) > 0.4f);
			Strip->CloseHistoryForTest();
			return true;
		});
		Steps.Finally(TEXT("the theme and the REPL prompt as they were"), [Strip, SavedTheme, bSavedPrompt, bThemeSet]()
		{
			if (Strip.IsValid())
			{
				Strip->CloseHistoryForTest();
			}
			if (*bThemeSet && FDreamThemes::GetCurrentId() != *SavedTheme)
			{
				FDreamThemes::SetCurrent(*SavedTheme, /*bPersist*/ false);
			}
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplPrompt, *bSavedPrompt);
		});
		Steps.Run(Done);
	});
}
