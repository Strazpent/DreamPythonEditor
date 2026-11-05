// SPDX-License-Identifier: Apache-2.0
// Dream.Language.* for owner ruling PO-06 F4, live problems PyCharm style (lang group; INT-07, TDD §7.3.6, §7.8):
// - Dream.Language.SyntaxCheck (any editor): the in-process lexer check finds the syntax errors a lexer can see, with
//   CPython's messages and positions, and stays silent on valid code (FDreamSyntaxCheck).
// - Dream.Language.SyntaxVerdict (any editor): one syntax verdict per buffer, the one that read the newer text, dream-check on
//   a tie (FDreamProblemsModel::NoteAnalysed).
// - Dream.Language.Live (attended): a syntax error typed into a tab is listed and squiggled (red pixels under it) within about
//   1 s without the server; a dream-check warning typed in is listed and squiggled (warning-coloured pixels) within about 1 s;
//   the hover tooltip shows message and code; the inspection widget counts and its click is F2; F2 / Shift+F2 walk the file;
//   the scroll bar has marks; a Problems view follows the typing.
#include "Support/DreamTestSteps.h"
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "CodeView/SDreamDiagnostics.h"
#include "CodeView/DreamUndoHistory.h"
#include "Commands/DreamCommands.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Language/DreamLanguageServices.h"
#include "Language/DreamProblemsModel.h"
#include "Language/DreamSyntaxCheck.h"
#include "Misc/AutomationTest.h"
#include "Module/DreamEditorModule.h"
#include "Shell/SDreamProblemsView.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamTheme.h"
#include "Widgets/SWindow.h"

namespace DreamLanguageLiveLocal
{
	FDreamLanguageServices* Services()
	{
		return FDreamEditorModule::IsAvailable() ? &FDreamEditorModule::Get().GetLanguageServices() : nullptr;
	}

	/** The first item of Items, as "line:col message" for test output. */
	FString DescribeItems(const TArray<FDreamProblem>& Items)
	{
		TArray<FString> Parts;
		for (const FDreamProblem& Item : Items)
		{
			Parts.Add(FString::Printf(TEXT("%d:%d %s"), Item.StartLine, Item.StartColumn, *Item.Message));
		}
		return Parts.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Parts, TEXT(" | "));
	}

	/** The items of Buffer's merged list that are syntax errors (any source) or have Code. */
	TArray<FDreamProblem> Shown(const FGuid& Buffer, const FString& Code)
	{
		TArray<FDreamProblem> Out;
		if (FDreamLanguageServices* Lang = Services())
		{
			for (const FDreamProblem& Item : Lang->GetProblems().GetForBuffer(Buffer))
			{
				if (Code.IsEmpty() ? Item.DedupClass == TEXT("syntax") : Item.Code == Code)
				{
					Out.Add(Item);
				}
			}
		}
		return Out;
	}

	FColor Srgb(const FLinearColor& Color)
	{
		return Color.ToFColorSRGB();
	}

	int32 Distance(const FColor& A, const FColor& B)
	{
		return FMath::Abs(A.R - B.R) + FMath::Abs(A.G - B.G) + FMath::Abs(A.B - B.B);
	}

	/**
	 * Reads the code view's pixels (FSlateApplication::TakeScreenshot) and checks that the band under a squiggle of Severity
	 * holds pixels of the squiggle's colour (nearer to it than to the background, anti-aliasing allowed). Adds errors.
	 */
	void CheckSquigglePixels(FAutomationTestBase& Test, const TSharedPtr<SDreamCodeView>& View, EDreamSeverity Severity, EDreamThemeSlot Slot,
		const FString& Context)
	{
		if (!View.IsValid() || !FSlateApplication::IsInitialized())
		{
			Test.AddError(Context + TEXT(": no code view"));
			return;
		}
		TArray<FDreamSquiggle> Squiggles;
		View->GetDiagnostics()->GetSquigglesInViewSpaceForTest(Squiggles);
		const FDreamSquiggle* Squiggle = Squiggles.FindByPredicate([Severity](const FDreamSquiggle& S) { return S.Severity == Severity; });
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: a squiggle is laid out"), *Context), Squiggle))
		{
			return;
		}
		FSlateApplication& App = FSlateApplication::Get();
		const TSharedPtr<SWindow> Window = App.FindWidgetWindow(View.ToSharedRef());
		if (!Window.IsValid() || Window->IsWindowMinimized() || !Window->IsVisible())
		{
			Test.AddError(Context + TEXT(": the code view's window is not shown"));
			return;
		}
		TArray<FColor> Pixels;
		FIntVector Size(0, 0, 0);
		if (!App.TakeScreenshot(View.ToSharedRef(), Pixels, Size) || Size.X < 40 || Size.Y < 20 || Pixels.Num() < Size.X * Size.Y)
		{
			Test.AddError(FString::Printf(TEXT("%s: TakeScreenshot gave %d x %d"), *Context, Size.X, Size.Y));
			return;
		}
		const FVector2D Local = View->GetTickSpaceGeometry().GetLocalSize();
		const double Scale = Local.X > 0.0 ? static_cast<double>(Size.X) / Local.X : 1.0;
		const FColor Want = Srgb(FDreamThemes::LinearColor(Slot));
		const FColor Background = Srgb(FDreamThemes::LinearColor(EDreamThemeSlot::EditorBackground));
		const int32 X0 = FMath::Clamp(FMath::FloorToInt32(Squiggle->Rect.Left * Scale), 0, Size.X - 1);
		const int32 X1 = FMath::Clamp(FMath::CeilToInt32(Squiggle->Rect.Right * Scale), 0, Size.X - 1);
		const int32 Y0 = FMath::Clamp(FMath::FloorToInt32((Squiggle->Rect.Bottom - 6.0) * Scale), 0, Size.Y - 1);
		const int32 Y1 = FMath::Clamp(FMath::CeilToInt32(Squiggle->Rect.Bottom * Scale), 0, Size.Y - 1);
		int32 Hits = 0;
		int32 Best = MAX_int32;
		for (int32 Y = Y0; Y <= Y1; ++Y)
		{
			for (int32 X = X0; X <= X1; ++X)
			{
				const FColor Pixel = Pixels[Y * Size.X + X];
				const int32 ToWant = Distance(Pixel, Want);
				Best = FMath::Min(Best, ToWant);
				if (ToWant < 120 && ToWant * 2 < Distance(Pixel, Background))
				{
					++Hits;
				}
			}
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: the band under the squiggle (%d..%d x %d..%d px) holds its colour %s (%d pixels; nearest distance %d)"),
			*Context, X0, X1, Y0, Y1, *Want.ToHex(), Hits, Best), Hits >= 2);
	}
}

// ---- Dream.Language.SyntaxCheck ----------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageSyntaxCheckSpec, "Dream.Language.SyntaxCheck", DreamTest::AnyEditorFlags)
	void ExpectOne(const FString& Source, int32 Line, int32 Column, const FString& MessageStart);
END_DEFINE_SPEC(FDreamLanguageSyntaxCheckSpec)

void FDreamLanguageSyntaxCheckSpec::ExpectOne(const FString& Source, int32 Line, int32 Column, const FString& MessageStart)
{
	using namespace DreamLanguageLiveLocal;
	const TArray<FDreamProblem> Items = FDreamSyntaxCheck::CheckText(Source);
	if (!TestEqual(*FString::Printf(TEXT("one item for %s (got %s)"), *Source.ReplaceCharWithEscapedChar(), *DescribeItems(Items)), Items.Num(), 1))
	{
		return;
	}
	const FDreamProblem& Item = Items[0];
	TestTrue(*FString::Printf(TEXT("'%s' starts with '%s'"), *Item.Message, *MessageStart), Item.Message.StartsWith(MessageStart));
	TestEqual(TEXT("line"), Item.StartLine, Line);
	if (Column >= 0)
	{
		TestEqual(TEXT("column"), Item.StartColumn, Column);
	}
	TestTrue(TEXT("an error"), Item.Severity == EDreamSeverity::Error);
	TestEqual(TEXT("dedup class syntax"), Item.DedupClass, FString(TEXT("syntax")));
	TestEqual(TEXT("code"), Item.Code, FString(FDreamSyntaxCheck::Code));
	TestTrue(TEXT("source Syntax"), Item.Source == EDreamProblemSource::Syntax);
	TestTrue(TEXT("at least one character wide"), Item.EndLine > Item.StartLine || Item.EndColumn > Item.StartColumn);
}

void FDreamLanguageSyntaxCheckSpec::Define()
{
	using namespace DreamLanguageLiveLocal;

	It("reports nothing for valid code with brackets, strings, f-strings, lambdas, decorators and blocks (PO-06 F4)", [this]()
	{
		const FString Source = TEXT(
			"import unreal\n"
			"\n"
			"@decorator(1)\n"
			"class Thing(object):\n"
			"    \"\"\"A docstring\n"
			"    over two lines.\"\"\"\n"
			"    def run(self, x=lambda: 1, *args, **kw) -> dict[str, int]:\n"
			"        data = {'a': [1, 2, (3, 4)],\n"
			"                'b': f\"{x!r:>{10}} {{literal}}\"}\n"
			"        if (n := len(data)) > 1: return data\n"
			"        elif x:\n"
			"            pass  # comment with ( and :\n"
			"        else:\n"
			"            y = x if x else \\\n"
			"                None\n"
			"        try:\n"
			"            pass\n"
			"        except (ValueError, KeyError) as error:\n"
			"            raise\n"
			"        finally:\n"
			"            pass\n"
			"        while chunk := next(iter([]), None):\n"
			"            break\n"
			"        with open(__file__) as f, open(__file__) as g:\n"
			"            print(f, g, sep=':')\n"
			"        for i in range(3): print(i)\n"
			"        return [i for i in range(3) if i]\n"
			"\n"
			"async def go():\n"
			"    async with lock:\n"
			"        await go()\n"
			"match command:\n"
			"    case [x, y]:\n"
			"        pass\n");
		const TArray<FDreamProblem> Items = FDreamSyntaxCheck::CheckText(Source);
		TestEqual(*FString::Printf(TEXT("no item (got %s)"), *DescribeItems(Items)), Items.Num(), 0);
	});

	It("reports an unclosed, an unmatched and a mismatched bracket where CPython does (PO-06 F4)", [this]()
	{
		ExpectOne(TEXT("a = 1\nx = (1,\n"), 1, 4, TEXT("'(' was never closed"));
		ExpectOne(TEXT("x = 1)\n"), 0, 5, TEXT("unmatched ')'"));
		ExpectOne(TEXT("x = [1, 2)\n"), 0, 9, TEXT("closing parenthesis ')' does not match opening parenthesis '['"));
	});

	It("reports a missing colon after a block header and the indentation errors (PO-06 F4)", [this]()
	{
		ExpectOne(TEXT("x = 1\nif x\n    pass\n"), 1, 3, TEXT("expected ':'"));
		ExpectOne(TEXT("def f(a, b) -> int\n    return a\n"), 0, 17, TEXT("expected ':'"));
		ExpectOne(TEXT("def f():\nreturn 1\n"), 1, 0, TEXT("expected an indented block after function definition on line 1"));
		ExpectOne(TEXT("x = 1\n    y = 2\n"), 1, 0, TEXT("unexpected indent"));
		ExpectOne(TEXT("if x:\n        a = 1\n    b = 2\n"), 2, 0, TEXT("unindent does not match any outer indentation level"));
		ExpectOne(TEXT("for i in range(3):\n"), 0, 18, TEXT("expected an indented block after 'for' statement on line 1"));
	});

	It("reports nothing for a valid line past 20000 columns or for raw f-strings with a backslash before a brace (review lang-q1-M1, m1)", [this]()
	{
		// Past column 20,000 the lexer keeps no tokens; the brackets there must still balance the ones before it.
		FString Long = TEXT("values = [");
		for (int32 Index = 0; Index < 12000; ++Index)
		{
			Long += TEXT("1, ");
		}
		Long += TEXT("]\nif values:\n    total = (len(values) +\n             1)\n");
		TArray<FDreamProblem> Items = FDreamSyntaxCheck::CheckText(Long);
		TestEqual(*FString::Printf(TEXT("a %d-column line: no item (got %s)"), Long.Find(TEXT("\n")), *DescribeItems(Items)), Items.Num(), 0);
		FString Opened = TEXT("call(");
		for (int32 Index = 0; Index < 21000; ++Index)
		{
			Opened += TEXT(" ");
		}
		Opened += TEXT(")\n");
		Items = FDreamSyntaxCheck::CheckText(Opened);
		TestEqual(*FString::Printf(TEXT("an opener closed past column 20000: no item (got %s)"), *DescribeItems(Items)), Items.Num(), 0);
		Items = FDreamSyntaxCheck::CheckText(TEXT("a = rf\"\\{{\"\nb = rf\"\\{a}\"\nc = r\"\\\\\"\n"));
		TestEqual(*FString::Printf(TEXT("raw f-strings: no item (got %s)"), *DescribeItems(Items)), Items.Num(), 0);
	});

	It("words its messages as CPython 3.11 does (review lang-q1-m3)", [this]()
	{
		ExpectOne(TEXT("print('abc\n"), 0, 6, TEXT("unterminated string literal"));
		ExpectOne(TEXT("s = \"\\x4\"\n"), 0, -1, TEXT("(unicode error) 'unicodeescape' codec can't decode bytes: truncated \\xXX escape"));
		ExpectOne(TEXT("s = f\"{x!z}\"\n"), 0, -1, TEXT("f-string: invalid conversion character 'z'"));
		const TArray<FDreamProblem> Wide = FDreamSyntaxCheck::CheckText(TEXT("x = 1 \U0001F600\n"));
		if (TestEqual(TEXT("one item for a character outside the BMP"), Wide.Num(), 1))
		{
			TestTrue(*FString::Printf(TEXT("'%s' names U+1F600"), *Wide[0].Message), Wide[0].Message.Contains(TEXT("U+1F600")));
			TestEqual(TEXT("two UTF-16 units wide"), Wide[0].EndColumn - Wide[0].StartColumn, 2);
		}
	});

	It("reports unterminated strings and invalid characters (PO-06 F4)", [this]()
	{
		ExpectOne(TEXT("s = 'abc\n"), 0, 4, TEXT("unterminated string literal (detected at line 1)"));
		ExpectOne(TEXT("x = 1\ns = \"\"\"doc\nmore\n"), 1, 4, TEXT("unterminated triple-quoted string literal"));
		ExpectOne(TEXT("price = $5\n"), 0, 8, TEXT("invalid character '$'"));
	});
}

// ---- Dream.Language.SyntaxVerdict --------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageSyntaxVerdictSpec, "Dream.Language.SyntaxVerdict", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamLanguageSyntaxVerdictSpec)

void FDreamLanguageSyntaxVerdictSpec::Define()
{
	It("shows the syntax verdict of the newer analysis, dream-check on a tie, and keeps dream-check's other items (PO-06 F4)", [this]()
	{
		FDreamProblemsModel Model;
		const FGuid Buffer = FGuid::NewGuid();
		auto Item = [&Buffer](EDreamProblemSource Source, int32 Line, const FString& DedupClass, const FString& Code)
		{
			FDreamProblem P;
			P.Buffer = Buffer;
			P.Source = Source;
			P.Severity = DedupClass == TEXT("syntax") ? EDreamSeverity::Error : EDreamSeverity::Warning;
			P.Code = Code;
			P.Message = Code;
			P.DedupClass = DedupClass;
			P.StartLine = P.EndLine = Line;
			P.EndColumn = 1;
			return P;
		};
		auto SyntaxSources = [&Model, &Buffer]()
		{
			TArray<EDreamProblemSource> Out;
			for (const FDreamProblem& P : Model.GetForBuffer(Buffer))
			{
				if (P.DedupClass == TEXT("syntax"))
				{
					Out.Add(P.Source);
				}
			}
			return Out;
		};
		// The in-process check alone (dream-check not started): its item shows.
		Model.NoteAnalysed(Buffer, EDreamProblemSource::Syntax, 5);
		Model.ReplaceSource(Buffer, EDreamProblemSource::Syntax, { Item(EDreamProblemSource::Syntax, 3, TEXT("syntax"), TEXT("syntax-error")) });
		TestEqual(TEXT("the in-process item alone"), SyntaxSources(), TArray<EDreamProblemSource>{ EDreamProblemSource::Syntax });
		// dream-check analysed the same version, on another line: its verdict wins, the in-process item hides.
		const uint64 RevisionBefore = Model.GetRevision();
		Model.NoteAnalysed(Buffer, EDreamProblemSource::DreamCheck, 5);
		TestTrue(TEXT("a verdict change moves the revision"), Model.GetRevision() != RevisionBefore);
		Model.ReplaceSource(Buffer, EDreamProblemSource::DreamCheck, { Item(EDreamProblemSource::DreamCheck, 2, TEXT("syntax"), TEXT("syntax-error")),
			Item(EDreamProblemSource::DreamCheck, 7, TEXT("dream-check:SUM204"), TEXT("SUM204")) });
		TestEqual(TEXT("dream-check's item on a tie"), SyntaxSources(), TArray<EDreamProblemSource>{ EDreamProblemSource::DreamCheck });
		// The text changed and only the in-process check has read it: its verdict (no error) wins; dream-check's older syntax
		// item hides, its SUM204 stays.
		Model.NoteAnalysed(Buffer, EDreamProblemSource::Syntax, 6);
		Model.ReplaceSource(Buffer, EDreamProblemSource::Syntax, {});
		TestEqual(TEXT("no syntax item once the newer analysis found none"), SyntaxSources().Num(), 0);
		TestTrue(TEXT("dream-check's warning stays"), Model.GetForBuffer(Buffer).ContainsByPredicate([](const FDreamProblem& P) { return P.Code == TEXT("SUM204"); }));
		TestEqual(TEXT("GetVisible agrees"), Model.GetVisible().Num(), 1);
		// A crash greys dream-check's items: the in-process verdict wins even on a tie.
		Model.NoteAnalysed(Buffer, EDreamProblemSource::DreamCheck, 6);
		Model.MarkSourceStale(EDreamProblemSource::DreamCheck, 0.0);
		Model.NoteAnalysed(Buffer, EDreamProblemSource::Syntax, 6);
		Model.ReplaceSource(Buffer, EDreamProblemSource::Syntax, { Item(EDreamProblemSource::Syntax, 4, TEXT("syntax"), TEXT("syntax-error")) });
		TestEqual(TEXT("the in-process item while dream-check's are stale"), SyntaxSources(), TArray<EDreamProblemSource>{ EDreamProblemSource::Syntax });
		Model.ClearBuffer(Buffer);
		TestEqual(TEXT("ClearBuffer clears everything"), Model.GetForBuffer(Buffer).Num(), 0);
	});
}

// ---- Dream.Language.Live -----------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamLanguageLiveSpec, "Dream.Language.Live", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamLanguageLiveSpec)

void FDreamLanguageLiveSpec::Define()
{
	using namespace DreamLanguageLiveLocal;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		if (FDreamLanguageServices* Lang = Services())
		{
			Lang->SetServerExcludedForTest(BufferId, false);
		}
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
	});

	LatentIt("lists and squiggles a typed syntax error within about 1 s, without the server, with tooltip, marks and counts (PO-06 F4)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || Services() == nullptr)
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("first = 1\n"), TEXT("LiveSyntax"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		// Review lang-q1-M2: dream-check stays off this document, so only the in-process check can report.
		Services()->SetServerExcludedForTest(Id, true);
		const TSharedRef<SDreamProblemsView> ProblemsView = SNew(SDreamProblemsView);   // a Problems view of its own: it must follow
		TSharedRef<double> TypedAt = MakeShared<double>(0.0);
		DreamTest::FDreamSteps Steps(*this);
		Steps.WaitFrames(2);
		Steps.Do(TEXT("type a broken line at the end"), [this, Id, TypedAt]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the code view"), View.IsValid()))
			{
				return false;
			}
			TestTrue(TEXT("the buffer is tracked by the in-process check"), Services()->IsSyntaxCheckTracked(Id));
			TestFalse(TEXT("dream-check does not have the document"), Services()->IsDocumentOpen(Id));
			TestEqual(TEXT("no syntax error before the edit"), Shown(Id, FString()).Num(), 0);
			View->SetCaret(1, 0);
			View->InsertAtCaret(TEXT("value = (2,"));     // typed text: the '(' is never closed
			*TypedAt = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("the syntax error is listed"), [Id]()
		{
			return Shown(Id, FString()).Num() > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("within about 1 s, at the bracket, in the Problems view too"), [this, Id, TypedAt, ProblemsView]()
		{
			const double Elapsed = FPlatformTime::Seconds() - *TypedAt;
			AddInfo(FString::Printf(TEXT("syntax error listed %.0f ms after the edit"), Elapsed * 1000.0));
			TestTrue(*FString::Printf(TEXT("listed within 1.2 s of the edit (took %.0f ms; INT-07: 300 ms debounce)"), Elapsed * 1000.0), Elapsed <= 1.2);
			const TArray<FDreamProblem> Items = Shown(Id, FString());
			if (TestEqual(*FString::Printf(TEXT("one syntax item (got %s)"), *DescribeItems(Items)), Items.Num(), 1))
			{
				TestEqual(TEXT("on the typed line"), Items[0].StartLine, 1);
				TestTrue(TEXT("an error"), Items[0].Severity == EDreamSeverity::Error);
				TestTrue(TEXT("from the in-process check (Source == Syntax)"), Items[0].Source == EDreamProblemSource::Syntax);
			}
			TestTrue(TEXT("GetItems(Id, Syntax) holds it"), Services()->GetProblems().GetItems(Id, EDreamProblemSource::Syntax).Num() > 0);
			TestEqual(TEXT("dream-check reported nothing for it"), Services()->GetProblems().GetItems(Id, EDreamProblemSource::DreamCheck).Num(), 0);
			TestFalse(TEXT("dream-check still does not have the document"), Services()->IsDocumentOpen(Id));
			TestTrue(TEXT("the Problems view lists it (it follows the model live)"), ProblemsView->GetItemsForTest().ContainsByPredicate(
				[Id](const FDreamProblem& Item) { return Item.Buffer == Id && Item.DedupClass == TEXT("syntax"); }));
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("squiggle pixels, tooltip, counts and marks"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the code view"), View.IsValid()))
			{
				return false;
			}
			CheckSquigglePixels(*this, View, EDreamSeverity::Error, EDreamThemeSlot::SynError, TEXT("syntax error"));
			const TArray<FDreamProblem> Items = Shown(Id, FString());
			if (Items.Num() > 0)
			{
				const FString Tip = View->GetDiagnostics()->GetToolTipAt(Items[0].StartLine, Items[0].StartColumn).ToString();
				TestTrue(*FString::Printf(TEXT("the tooltip '%s' holds the message"), *Tip), Tip.Contains(Items[0].Message));
				TestTrue(*FString::Printf(TEXT("the tooltip '%s' holds the code"), *Tip), Tip.Contains(TEXT("(syntax-error)")));
				TestTrue(TEXT("no tooltip on the clean first line"), View->GetDiagnostics()->GetToolTipAt(0, 2).IsEmpty());
			}
			int32 Errors = 0;
			int32 Warnings = 0;
			View->GetDiagnostics()->GetCounts(Errors, Warnings);
			TestEqual(TEXT("the inspection widget counts one error"), Errors, 1);
			TestTrue(TEXT("the inspection widget shows"), View->GetDiagnostics()->IsInspectionWidgetShown());
			TestTrue(TEXT("the scroll bar has a mark"), View->GetDiagnostics()->HasMarks());
			return true;
		});
		Steps.Do(TEXT("fixing the line clears it"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (View.IsValid())
			{
				View->InsertAtCaret(TEXT(")"));
			}
			return true;
		});
		Steps.Poll(TEXT("the syntax error is gone"), [Id]()
		{
			return Shown(Id, FString()).Num() == 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("nothing shows on a clean buffer"), [this, Id, ProblemsView]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (View.IsValid())
			{
				TestFalse(TEXT("no inspection widget"), View->GetDiagnostics()->IsInspectionWidgetShown());
				TestFalse(TEXT("no marks"), View->GetDiagnostics()->HasMarks());
			}
			TestFalse(TEXT("the Problems view dropped it"), ProblemsView->GetItemsForTest().ContainsByPredicate(
				[Id](const FDreamProblem& Item) { return Item.Buffer == Id; }));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("lists and squiggles a typed dream-check warning within about 1 s, walks it with F2 and Shift+F2 and the inspection click (PO-06 F4)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || Services() == nullptr || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("first = 1\nsecond = 2\n"), TEXT("LiveWarning"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const FGuid Id = BufferId;
		TSharedRef<double> TypedAt = MakeShared<double>(0.0);
		DreamTest::FDreamSteps Steps(*this);
		// Boot dream_editor so the deferred start runs, then wait for dream-check (Dream.Language.Services covers the start).
		TSharedRef<DreamTest::FRunResult> Boot = MakeShared<DreamTest::FRunResult>();
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a boot run"), []() { return DreamTest::MakeReplRequest(TEXT("_dream_live_boot = 1")); }, Boot, 60.0);
		Steps.Poll(TEXT("dream-check ready and the buffer open at the server"), [this, Id]()
		{
			FDreamLanguageServices* Lang = Services();
			const EDreamLanguageHealth Health = Lang->GetHealth();
			if (Health == EDreamLanguageHealth::Paused || Health == EDreamLanguageHealth::Disabled || Health == EDreamLanguageHealth::Stopped)
			{
				AddError(FString::Printf(TEXT("language services: %s"), *Lang->GetHealthText().ToString()));
				return DreamTest::EStep::Fail;
			}
			return Health == EDreamLanguageHealth::Ready && Lang->IsDocumentOpen(Id) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 90.0);
		Steps.WaitFrames(30);   // the open's first analysis (no item) has been published
		Steps.Do(TEXT("type two input() calls"), [this, Id, TypedAt]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the code view"), View.IsValid()))
			{
				return false;
			}
			TestEqual(TEXT("no SUM204 before the edit"), Shown(Id, TEXT("SUM204")).Num(), 0);
			View->SetCaret(0, 0);
			View->InsertAtCaret(TEXT("name = input()\n"));
			View->SetCaret(2, 0);
			View->InsertAtCaret(TEXT("other = input()\n"));
			*TypedAt = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("both SUM204 warnings are listed"), [Id]()
		{
			return Shown(Id, TEXT("SUM204")).Num() >= 2 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("within about 1 s, as warnings"), [this, Id, TypedAt]()
		{
			const double Elapsed = FPlatformTime::Seconds() - *TypedAt;
			AddInfo(FString::Printf(TEXT("dream-check warnings listed %.0f ms after the edit"), Elapsed * 1000.0));
			TestTrue(*FString::Printf(TEXT("listed within 1.2 s of the edit (took %.0f ms; INT-07, PERF-08)"), Elapsed * 1000.0), Elapsed <= 1.2);
			const TArray<FDreamProblem> Items = Shown(Id, TEXT("SUM204"));
			TestTrue(TEXT("warnings"), Items.Num() >= 2 && Items[0].Severity == EDreamSeverity::Warning);
			TestTrue(TEXT("on lines 1 and 3"), Items.Num() >= 2 && Items[0].StartLine == 0 && Items[1].StartLine == 2);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("warning squiggle pixels and tooltip"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			CheckSquigglePixels(*this, View, EDreamSeverity::Warning, EDreamThemeSlot::SynWarning, TEXT("SUM204 warning"));
			const TArray<FDreamProblem> Items = Shown(Id, TEXT("SUM204"));
			if (View.IsValid() && Items.Num() > 0)
			{
				const FString Tip = View->GetDiagnostics()->GetToolTipAt(Items[0].StartLine, Items[0].StartColumn).ToString();
				TestTrue(*FString::Printf(TEXT("the tooltip '%s' holds the message and (SUM204)"), *Tip), Tip.Contains(Items[0].Message) && Tip.Contains(TEXT("(SUM204)")));
				int32 Errors = 0;
				int32 Warnings = 0;
				View->GetDiagnostics()->GetCounts(Errors, Warnings);
				TestEqual(TEXT("the inspection widget counts two warnings"), Warnings, 2);
				TestEqual(TEXT("and no error"), Errors, 0);
			}
			return true;
		});
		Steps.Do(TEXT("F2, Shift+F2 and the inspection widget's click"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the code view"), View.IsValid()))
			{
				return false;
			}
			Tab->ActivateBuffer(Id);
			const TSharedPtr<FUICommandInfo> Next = FDreamCommands::Get().GetInfo(EDreamCommand::NextProblemInFile);
			const TSharedPtr<FUICommandInfo> Prev = FDreamCommands::Get().GetInfo(EDreamCommand::PrevProblemInFile);
			TestTrue(TEXT("F2 is Next Problem in File"), Next.IsValid() && Next->GetDefaultChord(EMultipleKeyBindingIndex::Primary) == FInputChord(EKeys::F2));
			TestTrue(TEXT("Shift+F2 is Previous Problem in File"), Prev.IsValid()
				&& Prev->GetDefaultChord(EMultipleKeyBindingIndex::Primary) == FInputChord(EModifierKey::Shift, EKeys::F2));
			View->SetCaret(1, 0);
			Tab->ExecuteDreamCommand(EDreamCommand::NextProblemInFile);
			TestEqual(TEXT("F2 from line 2 goes to line 3"), View->GetSelection().CaretLine, 2);
			Tab->ExecuteDreamCommand(EDreamCommand::NextProblemInFile);
			TestEqual(TEXT("F2 again wraps to line 1"), View->GetSelection().CaretLine, 0);
			Tab->ExecuteDreamCommand(EDreamCommand::PrevProblemInFile);
			TestEqual(TEXT("Shift+F2 wraps back to line 3"), View->GetSelection().CaretLine, 2);
			View->SetCaret(1, 0);
			View->GetDiagnostics()->ClickInspectionWidgetForTest();
			TestEqual(TEXT("the inspection widget's click is F2"), View->GetSelection().CaretLine, 2);
			return true;
		});
		Steps.Run(Done);
	});
}
