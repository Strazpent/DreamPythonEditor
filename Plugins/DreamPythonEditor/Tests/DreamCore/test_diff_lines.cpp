// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::Diff (TDD §4.6): Lines against an O(N^2) LCS oracle on random pairs and its cap on adversarial input
// (T-CORE-029), ToMinimalEdits (exact reconstruction, unchanged lines untouched), hunk sliding, intra-line ranges, the
// unified-diff formatter of the M1 Compare (§10.7.2) and cancellation.
#include "dream_test.h"
#include "test_util.h"

#include "dream/Diff.h"
#include "dream/TextBuffer.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using dream::Diff;
using dream::DiffHunk;
using dream::DiffOptions;
using dream::DiffResult;
using dream::TextBuffer;
using dream::TextSnapshot;
using dream_test_util::Rng;

namespace
{
	TextSnapshot Snap(std::u16string_view Text)
	{
		return TextBuffer(Text).Snapshot();
	}

	/** Diff lines as Diff::Lines cuts them: each with its '\n', no trailing empty line. */
	std::vector<std::u16string> DiffLines(const std::u16string& Text)
	{
		std::vector<std::u16string> Out;
		size_t Start = 0;
		for (size_t I = 0; I < Text.size(); ++I)
		{
			if (Text[I] == u'\n')
			{
				Out.push_back(Text.substr(Start, I + 1 - Start));
				Start = I + 1;
			}
		}
		if (Start < Text.size())
		{
			Out.push_back(Text.substr(Start));
		}
		return Out;
	}

	int32_t LcsLength(const std::vector<std::u16string>& A, const std::vector<std::u16string>& B)
	{
		std::vector<int32_t> Prev(B.size() + 1, 0);
		std::vector<int32_t> Cur(B.size() + 1, 0);
		for (size_t I = 1; I <= A.size(); ++I)
		{
			for (size_t J = 1; J <= B.size(); ++J)
			{
				Cur[J] = A[I - 1] == B[J - 1] ? Prev[J - 1] + 1 : std::max(Prev[J], Cur[J - 1]);
			}
			std::swap(Prev, Cur);
		}
		return Prev[B.size()];
	}

	/** Rebuilds New from Old and the hunks; false when an equal line differs or the hunks are malformed. */
	bool Reconstruct(const std::u16string& Old, const std::u16string& New, const DiffResult& R)
	{
		const std::vector<std::u16string> A = DiffLines(Old);
		const std::vector<std::u16string> B = DiffLines(New);
		if (R.OldLines != static_cast<int32_t>(A.size()) || R.NewLines != static_cast<int32_t>(B.size()))
		{
			return false;
		}
		int32_t I = 0;
		int32_t J = 0;
		for (const DiffHunk& H : R.Hunks)
		{
			if (H.OldFirst < I || H.NewFirst < J || H.OldFirst - I != H.NewFirst - J)
			{
				return false;
			}
			while (I < H.OldFirst)
			{
				if (A[static_cast<size_t>(I)] != B[static_cast<size_t>(J)])
				{
					return false;
				}
				++I;
				++J;
			}
			I += H.OldCount;
			J += H.NewCount;
			if (I > static_cast<int32_t>(A.size()) || J > static_cast<int32_t>(B.size()))
			{
				return false;
			}
		}
		if (static_cast<int32_t>(A.size()) - I != static_cast<int32_t>(B.size()) - J)
		{
			return false;
		}
		while (I < static_cast<int32_t>(A.size()))
		{
			if (A[static_cast<size_t>(I)] != B[static_cast<size_t>(J)])
			{
				return false;
			}
			++I;
			++J;
		}
		return true;
	}

	std::u16string ApplyBatch(std::u16string Text, const dream::EditBatch& Batch)
	{
		TextBuffer Buf(Text);
		const dream::ChangeSet C = Buf.Apply(Batch);
		(void)C;
		return Buf.Snapshot().Text();
	}

	std::u16string RandomLines(Rng& R, int32_t Count, int32_t Alphabet)
	{
		std::u16string Out;
		for (int32_t I = 0; I < Count; ++I)
		{
			const uint64_t K = R.Below(static_cast<uint64_t>(Alphabet));
			if (K == 0)
			{
				Out += u"\n";   // blank lines exercise sliding
				continue;
			}
			Out += u"line ";
			Out += static_cast<char16_t>(u'a' + (K % 26));
			if (K >= 26)
			{
				Out += static_cast<char16_t>(u'0' + (K / 26) % 10);
			}
			Out += u'\n';
		}
		if (R.OneIn(3) && !Out.empty())
		{
			Out.pop_back();   // no final newline
		}
		return Out;
	}

	std::u16string Mutate(Rng& R, const std::u16string& Text, int32_t Alphabet)
	{
		std::vector<std::u16string> Lines = DiffLines(Text);
		const int Edits = 1 + static_cast<int>(R.Below(8));
		for (int E = 0; E < Edits; ++E)
		{
			const size_t At = static_cast<size_t>(R.Below(Lines.size() + 1));
			switch (R.Below(3))
			{
			case 0:
				Lines.insert(Lines.begin() + static_cast<std::ptrdiff_t>(At), RandomLines(R, 1 + static_cast<int32_t>(R.Below(4)), Alphabet));
				break;
			case 1:
				if (At < Lines.size())
				{
					const size_t N = std::min<size_t>(Lines.size() - At, 1 + R.Below(3));
					Lines.erase(Lines.begin() + static_cast<std::ptrdiff_t>(At), Lines.begin() + static_cast<std::ptrdiff_t>(At + N));
				}
				break;
			default:
				if (At < Lines.size())
				{
					Lines[At] = RandomLines(R, 1, Alphabet);
				}
				break;
			}
		}
		std::u16string Out;
		for (const std::u16string& L : Lines)
		{
			Out += L;
			if (!Out.empty() && Out.back() != u'\n')
			{
				Out += u'\n';
			}
		}
		if (R.OneIn(3) && !Out.empty() && Out.back() == u'\n')
		{
			Out.pop_back();
		}
		return Out;
	}
}

TEST(Diff_LinesMinimalAgainstLcsOracle)   // T-CORE-029, random pairs up to 200 lines
{
	Rng R(29);
	DiffOptions Exact;
	Exact.TimeLimitMs = 0.0;
	int Bad = 0;
	for (int Case = 0; Case < 600 && Bad < 5; ++Case)
	{
		const int32_t Alphabet = 2 + static_cast<int32_t>(R.Below(40));
		const std::u16string Old = RandomLines(R, static_cast<int32_t>(R.Below(200)), Alphabet);
		const std::u16string New = R.OneIn(4) ? RandomLines(R, static_cast<int32_t>(R.Below(200)), Alphabet) : Mutate(R, Old, Alphabet);
		DiffOptions O = Exact;
		O.bSlideHunks = R.OneIn(2);
		O.bIntraLine = R.OneIn(2);
		const DiffResult D = Diff::Lines(Snap(Old), Snap(New), O);
		const std::vector<std::u16string> A = DiffLines(Old);
		const std::vector<std::u16string> B = DiffLines(New);
		const int32_t Oracle = static_cast<int32_t>(A.size() + B.size()) - 2 * LcsLength(A, B);
		const bool bValid = Reconstruct(Old, New, D);
		const bool bMinimal = D.AddedLines() + D.RemovedLines() == Oracle;
		CHECK(bValid);
		CHECK(bMinimal);
		CHECK(!D.bApproximate);
		CHECK_EQ(D.AddedLines(), static_cast<int32_t>(B.size()) - LcsLength(A, B));
		const std::u16string Rebuilt = ApplyBatch(Old, Diff::ToMinimalEdits(Snap(Old), Snap(New), D));
		CHECK(Rebuilt == New);
		if (!bValid || !bMinimal || Rebuilt != New)
		{
			++Bad;
			std::fprintf(stderr, "  case %d: %zu -> %zu lines, oracle %d, got %d\n", Case, A.size(), B.size(), Oracle,
				D.AddedLines() + D.RemovedLines());
		}
		// Hunks are separated by at least one equal line.
		for (size_t H = 1; H < D.Hunks.size(); ++H)
		{
			CHECK(D.Hunks[H].OldFirst > D.Hunks[H - 1].OldFirst + D.Hunks[H - 1].OldCount);
		}
	}
}

TEST(Diff_LinesAdversarialHitsTheCap)   // T-CORE-029, adversarial inputs: <= 50 ms, approximate, still valid
{
	// Two-letter alphabets in shifted order make Myers walk long snakes on every diagonal.
	std::u16string Old;
	std::u16string New;
	Rng R(7);
	for (int I = 0; I < 60000; ++I)
	{
		Old += (I % 2 == 0) ? u"a\n" : u"b\n";
		New += R.OneIn(2) ? u"a\n" : u"b\n";
	}
	const TextSnapshot SOld = Snap(Old);
	const TextSnapshot SNew = Snap(New);
	const auto Began = std::chrono::steady_clock::now();
	const DiffResult D = Diff::Lines(SOld, SNew);
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Began).count();
	std::printf("  adversarial 60k x 60k lines: %.1f ms, %zu hunks, approximate %d\n", Ms, D.Hunks.size(), D.bApproximate ? 1 : 0);
	CHECK(D.bApproximate);
	CHECK(Ms <= 50.0 + 25.0);   // 50 ms cap plus the setup (interning, hunks) outside the Myers loop
	CHECK(Reconstruct(Old, New, D));
	CHECK(ApplyBatch(Old, Diff::ToMinimalEdits(SOld, SNew, D)) == New);

	// No common lines at all: D = N + M passes 4,000 at once.
	std::u16string X;
	std::u16string Y;
	for (int I = 0; I < 5000; ++I)
	{
		X += u"x" + std::u16string(1, static_cast<char16_t>(u'A' + I % 20)) + u"\n";
		Y += u"y" + std::u16string(1, static_cast<char16_t>(u'A' + I % 20)) + u"\n";
	}
	DiffOptions NoClock;
	NoClock.TimeLimitMs = 0.0;
	const DiffResult D2 = Diff::Lines(Snap(X), Snap(Y), NoClock);
	CHECK(D2.bApproximate);
	REQUIRE_EQ(D2.Hunks.size(), 1u);
	CHECK(D2.Hunks[0].bApproximate);
	CHECK(Reconstruct(X, Y, D2));

	// The time cap: a deadline that has passed before the first middle snake leaves one approximate replace hunk.
	DiffOptions Tiny;
	Tiny.TimeLimitMs = 0.000001;
	std::u16string P = Old.substr(0, 4000);
	std::u16string Q = New.substr(0, 4000);
	const DiffResult D3 = Diff::Lines(Snap(P), Snap(Q), Tiny);
	CHECK(D3.bApproximate);
	CHECK(!D3.bCancelled);
	CHECK(Reconstruct(P, Q, D3));
	CHECK(ApplyBatch(P, Diff::ToMinimalEdits(Snap(P), Snap(Q), D3)) == Q);
}

TEST(Diff_ToMinimalEditsLeavesUnchangedLines)
{
	const std::u16string Old = u"def f():\n    return 1\n\nx = f()\nprint(x)\n";
	const std::u16string New = u"def f():\n    return 2\n\nx = f()\ny = x\nprint(x)\n";
	const dream::EditBatch Batch = Diff::ToMinimalEdits(Snap(Old), Snap(New));
	REQUIRE_EQ(Batch.Ranges.size(), 2u);
	// "1" -> "2" on line 1 only; "y = x\n" inserted before print(x): nothing else is touched.
	CHECK_EQ(Batch.Ranges[0].At, static_cast<dream::Offset>(Old.find(u'1')));
	CHECK_EQ(Batch.Ranges[0].DeleteLen, 1);
	CHECK(Batch.Ranges[0].Insert == u"2");
	CHECK_EQ(Batch.Ranges[1].DeleteLen, 0);
	CHECK(Batch.Ranges[1].Insert == u"y = x\n");
	CHECK(ApplyBatch(Old, Batch) == New);
	CHECK_EQ(Batch.Origin, dream::EditOrigin::Reload);

	// A line that only gains a final newline, an emptied text, an equal text.
	CHECK(ApplyBatch(u"a\nb", Diff::ToMinimalEdits(Snap(u"a\nb"), Snap(u"a\nb\n"))) == u"a\nb\n");
	CHECK(ApplyBatch(u"a\nb\n", Diff::ToMinimalEdits(Snap(u"a\nb\n"), Snap(u""))) == u"");
	CHECK(Diff::ToMinimalEdits(Snap(u"same\n"), Snap(u"same\n")).Ranges.empty());
	// A narrowed range never splits a surrogate pair.
	const std::u16string P1 = u"x\xD83D\xDE00y\n";
	const std::u16string P2 = u"x\xD83D\xDE01y\n";
	const dream::EditBatch Pair = Diff::ToMinimalEdits(Snap(P1), Snap(P2));
	REQUIRE_EQ(Pair.Ranges.size(), 1u);
	CHECK_EQ(Pair.Ranges[0].At, 1);
	CHECK_EQ(Pair.Ranges[0].DeleteLen, 2);
	// The result of other texts is refused.
	const DiffResult Other = Diff::Lines(Snap(u"a\nb\nc\n"), Snap(u"a\nc\n"));
	CHECK(Diff::ToMinimalEdits(Snap(u"q\n"), Snap(u"r\n"), Other).Ranges.empty());
}

TEST(Diff_SlidesToBlankAndIndentBoundaries)
{
	// A function added between two others: the hunk is "def c():", "    pass", "" (it ends with the blank line).
	const std::u16string Old = u"def a():\n    pass\n\ndef b():\n    pass\n";
	const std::u16string New = u"def a():\n    pass\n\ndef c():\n    pass\n\ndef b():\n    pass\n";
	const DiffResult D = Diff::Lines(Snap(Old), Snap(New));
	REQUIRE_EQ(D.Hunks.size(), 1u);
	CHECK_EQ(D.Hunks[0].OldCount, 0);
	CHECK_EQ(D.Hunks[0].NewFirst, 3);
	CHECK_EQ(D.Hunks[0].NewCount, 3);
	CHECK(Reconstruct(Old, New, D));
	DiffOptions NoSlide;
	NoSlide.bSlideHunks = false;
	CHECK(Reconstruct(Old, New, Diff::Lines(Snap(Old), Snap(New), NoSlide)));
}

TEST(Diff_IntraLineRanges)
{
	const std::u16string Old = u"x = compute(alpha, beta)\n";
	const std::u16string New = u"x = compute(alpha, gamma)\n";
	const DiffResult D = Diff::Lines(Snap(Old), Snap(New));
	REQUIRE_EQ(D.Hunks.size(), 1u);
	REQUIRE_EQ(D.Hunks[0].OldIntra.size(), 1u);
	REQUIRE_EQ(D.Hunks[0].NewIntra.size(), 1u);
	CHECK(D.Hunks[0].OldIntra[0] == (dream::DiffSpan{static_cast<dream::Offset>(Old.find(u"beta")), 4}));
	CHECK(D.Hunks[0].NewIntra[0] == (dream::DiffSpan{static_cast<dream::Offset>(New.find(u"gamma")), 5}));
	DiffOptions NoIntra;
	NoIntra.bIntraLine = false;
	CHECK(Diff::Lines(Snap(Old), Snap(New), NoIntra).Hunks[0].OldIntra.empty());
}

TEST(Diff_UnifiedFormat)   // the M1 Compare (§10.7.2): GNU unified format
{
	dream::UnifiedOptions O;
	auto Fmt = [&](std::u16string_view A, std::u16string_view B)
	{
		return Diff::FormatUnified(Snap(A), Snap(B), Diff::Lines(Snap(A), Snap(B)), O);
	};
	CHECK(Fmt(u"a\nb\nc\n", u"a\nB\nc\n") == u"@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n");
	CHECK(Fmt(u"a", u"b") == u"@@ -1 +1 @@\n-a\n\\ No newline at end of file\n+b\n\\ No newline at end of file\n");
	CHECK(Fmt(u"", u"x\n") == u"@@ -0,0 +1 @@\n+x\n");
	CHECK(Fmt(u"a\n", u"a") == u"@@ -1 +1 @@\n-a\n+a\n\\ No newline at end of file\n");
	CHECK(Fmt(u"same\n", u"same\n").empty());
	// Context of 3, and two hunks more than 6 lines apart stay separate.
	std::u16string Old;
	for (int I = 1; I <= 20; ++I)
	{
		Old += u"l" + std::u16string(1, static_cast<char16_t>(u'a' + I)) + u"\n";
	}
	std::u16string New = Old;
	New.replace(0, 3, u"X1\n");                 // line 1
	New.replace(New.size() - 3, 3, u"X20\n");   // line 20
	CHECK(Fmt(Old, New) == u"@@ -1,4 +1,4 @@\n-lb\n+X1\n lc\n ld\n le\n@@ -17,4 +17,4 @@\n lr\n ls\n lt\n-lu\n+X20\n");
	O.OldLabel = u"disk";
	O.NewLabel = u"recovered";
	const std::vector<dream::UnifiedLine> Lines = Diff::Unified(Snap(u"a\nb\n"), Snap(u"a\n"), Diff::Lines(Snap(u"a\nb\n"), Snap(u"a\n")), O);
	REQUIRE_EQ(Lines.size(), 5u);
	CHECK(Lines[0].Text == u"--- disk");
	CHECK(Lines[1].Text == u"+++ recovered");
	CHECK(Lines[2].Text == u"@@ -1,2 +1 @@");
	CHECK_EQ(Lines[3].Type, dream::UnifiedLine::Kind::Context);
	CHECK_EQ(Lines[3].OldLine, 0);
	CHECK_EQ(Lines[3].NewLine, 0);
	CHECK_EQ(Lines[4].Type, dream::UnifiedLine::Kind::Removed);
	CHECK_EQ(Lines[4].OldLine, 1);
	const DiffResult Stats = Diff::Lines(Snap(Old), Snap(New));
	CHECK_EQ(Stats.AddedLines(), 2);
	CHECK_EQ(Stats.RemovedLines(), 2);
}

TEST(Diff_Cancelled)
{
	dream::CancelToken Token;
	Token.Cancel();
	std::u16string Old;
	std::u16string New;
	for (int I = 0; I < 20000; ++I)
	{
		Old += (I % 3 == 0) ? u"a\n" : u"b\n";
		New += (I % 5 == 0) ? u"a\n" : u"b\n";
	}
	const DiffResult D = Diff::Lines(Snap(Old), Snap(New), DiffOptions(), &Token);
	CHECK(D.bCancelled);
	CHECK(D.Hunks.empty());
	CHECK(Diff::ToMinimalEdits(Snap(Old), Snap(New), D).Ranges.empty());
}
