// SPDX-License-Identifier: Apache-2.0
// Dream.Supervisor.SourceText: the lexical handler search behind the §5.7.4 swallow report ("The script caught Stop at line
// 57", T-CAN-022) and the cleanup timer ("cleanup is running (finally at line 88)", T-CAN-006), read from a source
// snapshot by indentation (DreamRunSourceText.h). Pure functions over a dream::TextSnapshot: no Python, no Slate, so the
// spec runs in a headless session too. Includes except* (3.11: it catches a naked KeyboardInterrupt, ADD2-01).
#include "Support/DreamTestUtil.h"

#include "Module/DreamText.h"
#include "Run/DreamRunSourceText.h"
#include "dream/TextBuffer.h"

namespace DreamSourceTextSpec
{
	dream::TextSnapshot Snap(const TArray<FString>& Lines)
	{
		const dream::TextBuffer Buffer(DreamText::ToU16(FString::Join(Lines, TEXT("\n"))));
		return Buffer.Snapshot();
	}

	TArray<FString> TryAround(const FString& ExceptClause, const FString& Indent = TEXT("    "))
	{
		return {
			TEXT("for i in range(3):"),                     // 1
			Indent + TEXT("try:"),                          // 2
			Indent + Indent + TEXT("work()"),               // 3
			Indent + ExceptClause,                          // 4
			Indent + Indent + TEXT("pass"),                 // 5
		};
	}
}

BEGIN_DEFINE_SPEC(FDreamSourceTextSpec, "Dream.Supervisor.SourceText", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamSourceTextSpec)

void FDreamSourceTextSpec::Define()
{
	using namespace DreamSourceTextSpec;
	using DreamRunSource::EHandlerKind;

	It("names the except clause that catches Stop, and none that cannot (T-CAN-022)", [this]()
	{
		struct FCase
		{
			const TCHAR* Clause;
			bool bCatches;
		};
		const FCase Cases[] = {
			{ TEXT("except KeyboardInterrupt:"), true },
			{ TEXT("except:"), true },
			{ TEXT("except BaseException as e:"), true },
			{ TEXT("except (KeyboardInterrupt, Exception):"), true },
			{ TEXT("except dream_editor.ScriptCancelled:"), true },
			{ TEXT("except* KeyboardInterrupt:"), true },           // 3.11 wraps the naked exception in a group (ADD2-01)
			{ TEXT("except* BaseException:"), true },
			{ TEXT("except* BaseExceptionGroup:"), false },         // TypeError at run time (PEP 654), catches nothing
			{ TEXT("except* (ValueError, BaseExceptionGroup):"), false },
			{ TEXT("except BaseExceptionGroup:"), false },          // a naked KeyboardInterrupt is no group
			{ TEXT("except MyKeyboardInterruptLike:"), false },     // whole names only
			{ TEXT("except Exception:"), false },
			{ TEXT("except ValueError:"), false },
			{ TEXT("except* ValueError:"), false },
			// Only the names before the clause's ':' count, never a comment or a one-line body (P3-run-05).
			{ TEXT("except* KeyboardInterrupt:  # Stop arrives wrapped in an ExceptionGroup"), true },
			{ TEXT("except* KeyboardInterrupt: print(ExceptionGroup)"), true },
			{ TEXT("except ValueError:  # not KeyboardInterrupt"), false },
			{ TEXT("except ValueError: raise KeyboardInterrupt"), false },
		};
		for (const FCase& Case : Cases)
		{
			const dream::TextSnapshot S = Snap(TryAround(Case.Clause));
			TestEqual(*FString::Printf(TEXT("FindCatchingExcept for \"%s\""), Case.Clause), DreamRunSource::FindCatchingExcept(S, 3), Case.bCatches ? 4 : 0);
		}
	});

	It("walks indentation with tabs and through a try nested in a finally", [this]()
	{
		const dream::TextSnapshot Tabs = Snap(TryAround(TEXT("except KeyboardInterrupt:"), TEXT("\t")));
		TestEqual(TEXT("tab indentation: the except at line 4"), DreamRunSource::FindCatchingExcept(Tabs, 3), 4);

		const dream::TextSnapshot Nested = Snap({
			TEXT("try:"),                                   // 1
			TEXT("    a()"),                                // 2
			TEXT("finally:"),                               // 3
			TEXT("    try:"),                               // 4
			TEXT("        b()"),                            // 5
			TEXT("    except BaseException:"),              // 6
			TEXT("        pass"),                           // 7
		});
		TestEqual(TEXT("the inner try's except catches line 5"), DreamRunSource::FindCatchingExcept(Nested, 5), 6);
		TestEqual(TEXT("line 2 has no except that catches"), DreamRunSource::FindCatchingExcept(Nested, 2), 0);
		EHandlerKind Kind = EHandlerKind::None;
		TestEqual(TEXT("line 7 is handled by the except at line 6"), DreamRunSource::FindEnclosingHandler(Nested, 7, Kind), 6);
		TestTrue(TEXT("... an except"), Kind == EHandlerKind::Except);
		TestEqual(TEXT("line 5 runs in the finally at line 3"), DreamRunSource::FindEnclosingHandler(Nested, 5, Kind), 3);
		TestTrue(TEXT("... a finally"), Kind == EHandlerKind::Finally);
	});

	It("finds the cleanup block: a finally body, a with body and a header line itself (T-CAN-006)", [this]()
	{
		const dream::TextSnapshot S = Snap({
			TEXT("try:"),                                   // 1
			TEXT("    work()"),                             // 2
			TEXT("finally:"),                               // 3
			TEXT("    # tidy up"),                          // 4
			TEXT("    cleanup()"),                          // 5
			TEXT("with open(p) as f:"),                     // 6
			TEXT("    data = f.read()"),                    // 7
			TEXT("x = 1"),                                  // 8
		});
		EHandlerKind Kind = EHandlerKind::None;
		TestEqual(TEXT("a finally body"), DreamRunSource::FindEnclosingHandler(S, 5, Kind), 3);
		TestTrue(TEXT("... is Finally"), Kind == EHandlerKind::Finally);
		TestEqual(TEXT("a with body"), DreamRunSource::FindEnclosingHandler(S, 7, Kind), 6);
		TestTrue(TEXT("... is With"), Kind == EHandlerKind::With);
		TestEqual(TEXT("the with header itself (an __exit__ call)"), DreamRunSource::FindEnclosingHandler(S, 6, Kind), 6);
		TestTrue(TEXT("... is With"), Kind == EHandlerKind::With);
		TestEqual(TEXT("top-level code has no handler"), DreamRunSource::FindEnclosingHandler(S, 8, Kind), 0);
		TestTrue(TEXT("... None"), Kind == EHandlerKind::None);
		TestEqual(TEXT("a try body is no handler"), DreamRunSource::FindEnclosingHandler(S, 2, Kind), 0);
	});

	It("classifies block headers", [this]()
	{
		TestTrue(TEXT("except*"), DreamRunSource::ClassifyHeader(TEXT("    except* KeyboardInterrupt:")) == EHandlerKind::Except);
		TestTrue(TEXT("finally"), DreamRunSource::ClassifyHeader(TEXT("finally:")) == EHandlerKind::Finally);
		TestTrue(TEXT("async with"), DreamRunSource::ClassifyHeader(TEXT("async with lock:")) == EHandlerKind::With);
		TestTrue(TEXT("a name that starts like a keyword"), DreamRunSource::ClassifyHeader(TEXT("exceptions = []")) == EHandlerKind::None);
		TestTrue(TEXT("plain code"), DreamRunSource::ClassifyHeader(TEXT("x = 1")) == EHandlerKind::None);
	});
}
