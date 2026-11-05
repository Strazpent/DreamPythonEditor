// SPDX-License-Identifier: Apache-2.0
// Tier 1 timing gates (B06, built /O2 like the rest of tier 1):
// - T-CORE-004: TextBuffer::Snapshot on 100k lines with 50k pieces, p99 <= 2 us (D17 allows 0.2 ms).
// - T-CORE-009: TextBuffer::Apply on 100k lines, p95 <= 0.1 ms.
// - T-LEX-008: 100k lines, a 5,000-edit trace of typing, pastes, `"""` toggles and undo. Per edit at p95:
//   TextBuffer::Apply + UndoTree::Record <= 0.15 ms, AnchorSet::MapThrough <= 0.10 ms, the keystroke relex (1 ms
//   budget, visible lines first) <= 1.00 ms, and their sum <= 2 ms (the §4 keystroke split; Keymap, IME, LSP and view
//   invalidation are SCodeView's stages, measured by Dream.Keystroke under G1).
// The corpus is generated Python (classes, decorators, docstrings, f-strings, comments), so the gates need no file.
// Each gate keeps the best of three attempts: tier 1 runs on shared developer machines where other builds preempt
// the process, and noise only ever adds time. DREAM_SKIP_TIMING=1 skips them (debugger, coverage or sanitizer builds).
#define _CRT_SECURE_NO_WARNINGS   // std::getenv
#include "dream_test.h"
#include "test_util.h"

#include "dream/AnchorSet.h"
#include "dream/PyLexer.h"
#include "dream/TextBuffer.h"
#include "dream/UndoTree.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

using dream::ChangeSet;
using dream::EditBatch;
using dream::EditRange;
using dream::LineNo;
using dream::Offset;
using dream::TextBuffer;
using dream::TextSnapshot;
using dream_test_util::Rng;

namespace
{
	using Clock = std::chrono::steady_clock;

	bool SkipTiming()
	{
		const char* V = std::getenv("DREAM_SKIP_TIMING");
		return V != nullptr && *V != '\0' && *V != '0';
	}

	double Ms(Clock::time_point A, Clock::time_point B)
	{
		return std::chrono::duration<double, std::milli>(B - A).count();
	}

	double Percentile(std::vector<double> V, double P)
	{
		if (V.empty())
		{
			return 0.0;
		}
		std::sort(V.begin(), V.end());
		const size_t Index = std::min(V.size() - 1, static_cast<size_t>(P * static_cast<double>(V.size())));
		return V[Index];
	}

	std::u16string Ascii(const std::string& S)
	{
		return std::u16string(S.begin(), S.end());
	}

	/** About Lines lines of ordinary Python in 21-line blocks. */
	std::u16string Corpus(int Lines)
	{
		std::string Out;
		Out.reserve(static_cast<size_t>(Lines) * 36);
		int Made = 0;
		for (int N = 0; Made < Lines; ++N)
		{
			const std::string I = std::to_string(N);
			Out += "@decorator_" + I + "\n";
			Out += "class Widget" + I + "(Base):\n";
			Out += "    \"\"\"Docstring for widget " + I + ".\n";
			Out += "\n";
			Out += "    More text about it.\n";
			Out += "    \"\"\"\n";
			Out += "    count = " + I + "\n";
			Out += "\n";
			Out += "    def method_" + I + "(self, value, *args, **kwargs):\n";
			Out += "        # compute something\n";
			Out += "        result = [x * " + I + " for x in range(value) if x % 3]\n";
			Out += "        text = f\"value={value!r:>10} n={self.count}\"\n";
			Out += "        data = {'key': value, \"other\": (1, 2, 3)}\n";
			Out += "        if result and value > 0x1F:\n";
			Out += "            return result\n";
			Out += "        return None\n";
			Out += "\n";
			Out += "def helper_" + I + "(a, b=None):\n";
			Out += "    '''Helper.'''\n";
			Out += "    return a + (b or 0)  # trailing comment\n";
			Out += "\n";
			Made += 21;
		}
		return Ascii(Out);
	}

	/** Runs Attempt up to three times; passes when one attempt passes. */
	template <class Fn>
	bool BestOfThree(Fn&& Attempt)
	{
		for (int Try = 1; Try <= 3; ++Try)
		{
			if (Attempt(Try))
			{
				return true;
			}
		}
		return false;
	}

	EditBatch One(Offset At, int64_t Del, std::u16string Ins, dream::EditOrigin Origin = dream::EditOrigin::Typing)
	{
		EditBatch B;
		B.Origin = Origin;
		B.Ranges.push_back(EditRange{At, Del, std::move(Ins)});
		return B;
	}
}

TEST(Perf_SnapshotCost)   // T-CORE-004
{
	if (SkipTiming())
	{
		std::printf("  (skipped: DREAM_SKIP_TIMING)\n");
		return;
	}
	TextBuffer B(Corpus(100000));
	Rng R(4);
	while (B.Snapshot().PieceCount() < 50000)
	{
		B.Apply(One(static_cast<Offset>(R.Below(static_cast<uint64_t>(B.Length()))), 0, u"x"));
	}
	REQUIRE(B.LineCount() >= 100000);
	const bool bPass = BestOfThree([&](int Try) {
		std::vector<double> Us;
		Us.reserve(100000);
		int64_t Sink = 0;
		for (int I = 0; I < 100000; ++I)
		{
			const Clock::time_point T0 = Clock::now();
			const TextSnapshot S = B.Snapshot();
			const Clock::time_point T1 = Clock::now();
			Sink += S.Length();
			Us.push_back(Ms(T0, T1) * 1000.0);
		}
		const double P99 = Percentile(Us, 0.99);
		std::printf("  try %d: %zu pieces, %d lines: snapshot p50 %.3f us, p99 %.3f us (gate 2 us)%s\n", Try, B.Snapshot().PieceCount(),
			B.LineCount(), Percentile(Us, 0.5), P99, Sink == 0 ? " (no text?)" : "");
		return P99 <= 2.0;
	});
	CHECK(bPass);
}

TEST(Perf_ApplyBudget)   // T-CORE-009
{
	if (SkipTiming())
	{
		std::printf("  (skipped: DREAM_SKIP_TIMING)\n");
		return;
	}
	const std::u16string Text = Corpus(100000);
	const bool bPass = BestOfThree([&](int Try) {
		TextBuffer B(Text);
		Rng R(static_cast<uint64_t>(90 + Try));
		Offset Caret = B.Snapshot().LineStart(50000) + 8;
		std::vector<double> Times;
		Times.reserve(10000);
		for (int I = 0; I < 10000; ++I)
		{
			EditBatch E;
			const uint64_t Kind = R.Below(100);
			if (Kind < 75)
			{
				E = One(Caret, 0, std::u16string(1, I % 30 == 29 ? u'\n' : static_cast<char16_t>(u'a' + I % 26)));
				++Caret;
			}
			else if (Kind < 85)
			{
				E = One(Caret - 1, 1, u"");
				--Caret;
			}
			else if (Kind < 95)
			{
				const Offset At = static_cast<Offset>(R.Below(static_cast<uint64_t>(B.Length())));
				E = One(At, 0, u"    value = compute(x)\n    if value:\n        return value\n", dream::EditOrigin::Command);
			}
			else
			{
				const LineNo L = static_cast<LineNo>(R.Below(static_cast<uint64_t>(B.LineCount() - 1)));
				const TextSnapshot S = B.Snapshot();
				E = One(S.LineStart(L), S.LineStart(L + 1) - S.LineStart(L), u"", dream::EditOrigin::Command);
				if (S.LineStart(L) < Caret)
				{
					Caret = std::max<Offset>(S.LineStart(L), Caret - (S.LineStart(L + 1) - S.LineStart(L)));
				}
			}
			if (R.OneIn(200))
			{
				Caret = static_cast<Offset>(R.Below(static_cast<uint64_t>(B.Length()))) + 1;
			}
			Caret = std::clamp<Offset>(Caret, 1, B.Length());
			const Clock::time_point T0 = Clock::now();
			const ChangeSet C = B.Apply(E);
			const Clock::time_point T1 = Clock::now();
			if (!C.IsEmpty())
			{
				Times.push_back(Ms(T0, T1));
			}
		}
		const double P95 = Percentile(Times, 0.95);
		std::printf("  try %d: %zu edits on %d lines, %zu pieces: Apply p50 %.4f ms, p95 %.4f ms, p99 %.4f ms (gate 0.1 ms)\n", Try,
			Times.size(), B.LineCount(), B.Snapshot().PieceCount(), Percentile(Times, 0.5), P95, Percentile(Times, 0.99));
		return P95 <= 0.1 && Times.size() > 9000;
	});
	CHECK(bPass);
}

TEST(Perf_KeystrokeBudget)   // T-LEX-008
{
	if (SkipTiming())
	{
		std::printf("  (skipped: DREAM_SKIP_TIMING)\n");
		return;
	}
	const std::u16string Text = Corpus(100000);
	const bool bPass = BestOfThree([&](int Try) {
		TextBuffer B(Text);
		dream::UndoTree Undo(B.Version());
		dream::PyLexer Lx(dream::LexGrammar::Py311);
		Lx.Reset(B.Snapshot());
		Lx.Relex(B.Snapshot(), 0.0, 0, 0);
		// Breakpoints, ran-line marks and folds: 1,000 anchors over the file, mapped through every edit.
		dream::AnchorSet Marks;
		for (int K = 0; K < 1000; ++K)
		{
			const TextSnapshot S = B.Snapshot();
			const LineNo L = static_cast<LineNo>(K * 97 % B.LineCount());
			if (K % 3 == 0)
			{
				Marks.Add(S.LineStart(L), dream::AnchorKind::LineStart, dream::AnchorGravity::Before, dream::AnchorDeletePolicy::Remove, 1);
			}
			else
			{
				Marks.Add(S.LineStart(L) + 4, dream::AnchorKind::Point, dream::AnchorGravity::After, dream::AnchorDeletePolicy::Collapse, 2);
			}
		}
		Rng R(static_cast<uint64_t>(800 + Try));
		LineNo Top = 40000;
		Offset Caret = B.Snapshot().LineStart(Top + 20) + 8;
		Offset QuoteAt = -1;   // an open `"""` the trace inserted, toggled off later
		double Now = 1000.0;
		std::vector<double> Model;
		std::vector<double> Map;
		std::vector<double> Lex;
		std::vector<double> Splice;
		std::vector<double> Total;
		int Undos = 0;
		int Toggles = 0;
		for (int I = 0; I < 5000; ++I)
		{
			const TextSnapshot Before = B.Snapshot();
			const uint64_t Kind = R.Below(100);
			EditBatch E;
			dream::UndoKind UKind = dream::UndoKind::Typing;
			bool bUndo = false;
			if (Kind < 78)
			{
				const bool bNewline = R.OneIn(25);
				E = One(Caret, 0, bNewline ? u"\n        " : std::u16string(1, static_cast<char16_t>(u'a' + R.Below(26))));
				Caret += bNewline ? 9 : 1;
			}
			else if (Kind < 84)
			{
				E = One(Caret - 1, 1, u"");
				UKind = dream::UndoKind::Delete;
				--Caret;
			}
			else if (Kind < 88)
			{
				E = One(Before.LineStart(Before.LineOf(Caret)), 0,
					u"for item in items:\n    total += item.value  # sum\n    print(f\"{item!r}\")\n", dream::EditOrigin::Command);
				UKind = dream::UndoKind::Command;
				Caret += 75;
			}
			else if (Kind < 92)
			{
				// Toggle a `"""` near the top of the visible lines: the cascade case.
				if (QuoteAt < 0)
				{
					QuoteAt = Before.LineStart(Top + 3);
					E = One(QuoteAt, 0, u"\"\"\"", dream::EditOrigin::Command);
					Caret += Caret >= QuoteAt ? 3 : 0;
				}
				else
				{
					E = One(QuoteAt, 3, u"", dream::EditOrigin::Command);
					Caret -= Caret >= QuoteAt + 3 ? 3 : 0;
					QuoteAt = -1;
				}
				UKind = dream::UndoKind::Command;
				++Toggles;
			}
			else
			{
				bUndo = true;
			}
			Now += 0.08;
			Clock::time_point T0 = Clock::now();
			ChangeSet C;
			if (bUndo)
			{
				const dream::UndoStep Step = Undo.Undo(Before);
				if (Step.Status != dream::UndoStatus::Applied)
				{
					continue;
				}
				C = B.Apply(Step.Batch);
				Undo.Record(C, Before, B.Snapshot(), Step.Kind, Now);
				++Undos;
				QuoteAt = -1;   // the undone step may have been the toggle; stop tracking it
			}
			else
			{
				C = B.Apply(E);
				Undo.Record(C, Before, B.Snapshot(), UKind, Now);
			}
			const TextSnapshot After = B.Snapshot();
			const Clock::time_point T1 = Clock::now();
			Marks.MapThrough(C, Before, After);
			const Clock::time_point T2 = Clock::now();
			// The lexer's share is 1 ms for the splice and the sync relex together: the relex gets what the splice left.
			Lx.OnChange(C, After);
			const Clock::time_point T2b = Clock::now();
			Lx.Relex(After, std::max(0.05, 1.0 - Ms(T2, T2b)), Top, Top + 60);
			const Clock::time_point T3 = Clock::now();
			if (C.IsEmpty())
			{
				continue;
			}
			Model.push_back(Ms(T0, T1));
			Map.push_back(Ms(T1, T2));
			Lex.push_back(Ms(T2, T3));
			Splice.push_back(Ms(T2, T2b));
			Total.push_back(Ms(T0, T3));
			// The frame's idle slice (untimed, shorter than §4.3.3's 1 ms so the gate stays quick).
			Lx.Relex(After, 0.25, Top, Top + 60);
			if (bUndo || C.Ranges.front().At > Caret + 4000 || C.Ranges.front().At + 4000 < Caret)
			{
				Caret = std::clamp<Offset>(C.Ranges.front().At + C.Ranges.front().NewLen, 1, B.Length());
			}
			Caret = std::clamp<Offset>(Caret, 1, B.Length());
			Top = std::max<LineNo>(0, B.Snapshot().LineOf(Caret) - 20);
		}
		const double PModel = Percentile(Model, 0.95);
		const double PMap = Percentile(Map, 0.95);
		const double PLex = Percentile(Lex, 0.95);
		const double PTotal = Percentile(Total, 0.95);
		std::printf("  try %d: %zu edits (%d undos, %d \"\"\" toggles) on %d lines, p95 / p99 ms: Apply+Record %.4f / %.4f (0.15), "
			"MapThrough %.4f / %.4f (0.10), splice+relex %.4f / %.4f (1.00; splice alone %.4f / %.4f), sum %.4f / %.4f (2.00)\n", Try,
			Total.size(), Undos, Toggles, B.LineCount(), PModel, Percentile(Model, 0.99), PMap, Percentile(Map, 0.99), PLex,
			Percentile(Lex, 0.99), Percentile(Splice, 0.95), Percentile(Splice, 0.99), PTotal, Percentile(Total, 0.99));
		// The incremental tokens still equal a full lex once the frontier is through (T-LEX-007's invariant).
		Lx.RelexThrough(B.Snapshot(), B.LineCount() - 1);
		std::vector<std::vector<uint32_t>> Full;
		dream::PyLexer::LexFull(B.Snapshot(), dream::LexGrammar::Py311, Full);
		bool bSame = Full.size() == static_cast<size_t>(Lx.LineCount());
		for (LineNo L = 0; bSame && L < Lx.LineCount(); L += 7)
		{
			const auto T = Lx.Tokens(L);
			bSame = std::vector<uint32_t>(T.begin(), T.end()) == Full[static_cast<size_t>(L)];
			if (!bSame)
			{
				std::string Line;
				dream::utf::Utf16ToUtf8(B.Snapshot().LineText(L), Line);
				std::fprintf(stderr, "  line %d differs (frontier %d, start state %u): %.100s\n", L, Lx.Frontier(), Lx.StartState(L), Line.c_str());
				for (uint32_t Tok : T)
				{
					std::fprintf(stderr, "    inc %d:%d\n", dream::TokenStart(Tok), static_cast<int>(dream::TokenKindOf(Tok)));
				}
				for (uint32_t Tok : Full[static_cast<size_t>(L)])
				{
					std::fprintf(stderr, "    full %d:%d\n", dream::TokenStart(Tok), static_cast<int>(dream::TokenKindOf(Tok)));
				}
			}
		}
		if (Full.size() != static_cast<size_t>(Lx.LineCount()))
		{
			std::fprintf(stderr, "  line counts differ: full %zu, incremental %d\n", Full.size(), Lx.LineCount());
		}
		CHECK(bSame);
		return Total.size() > 4500 && PModel <= 0.15 && PMap <= 0.10 && PLex <= 1.00 && PTotal <= 2.00;
	});
	CHECK(bPass);
}
