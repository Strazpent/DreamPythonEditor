// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::SearchEngine (TDD §4.4, EDT-09): literal, case and whole word against a naive oracle and regex plus
// replacement against Python `re` golden data from the engine interpreter (T-CORE-017; build.bat writes it with
// search_golden.py and names it in DREAM_SEARCH_GOLDEN), the step limit (T-CORE-018), multi-line matches, viewport
// order, the first batch and cancel (T-CORE-019), match maintenance (T-CORE-020) and Replace All (T-CORE-021).
#define _CRT_SECURE_NO_WARNINGS   // std::getenv
#include "dream_test.h"
#include "test_util.h"

#include "dream/SearchEngine.h"
#include "dream/TextBuffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using dream::CompileResult;
using dream::SearchEngine;
using dream::SearchMatch;
using dream::SearchQuery;
using dream::TextBuffer;
using dream::TextSnapshot;
using dream_test_util::Rng;

namespace
{
	CompileResult Compile(std::u16string_view Pattern, bool bRegex, bool bCase = true, bool bWord = false)
	{
		SearchQuery Q;
		Q.Pattern.assign(Pattern);
		Q.bRegex = bRegex;
		Q.bMatchCase = bCase;
		Q.bWholeWord = bWord;
		return SearchEngine::Compile(Q);
	}

	std::vector<SearchMatch> Find(std::u16string_view Pattern, std::u16string_view Text, bool bRegex = true, bool bCase = true, bool bWord = false)
	{
		const CompileResult C = Compile(Pattern, bRegex, bCase, bWord);
		if (!C.Ok())
		{
			return {SearchMatch{-1, -1}};
		}
		return SearchEngine::FindAll(Text, *C.Query);
	}

	std::string Spans(const std::vector<SearchMatch>& M)
	{
		std::string Out;
		for (const SearchMatch& X : M)
		{
			if (!Out.empty())
			{
				Out += ",";
			}
			Out += std::to_string(X.At) + "-" + std::to_string(X.At + X.Len);
		}
		return Out.empty() ? "." : Out;
	}

	char16_t Lower(char16_t C)
	{
		return (C >= u'A' && C <= u'Z') ? static_cast<char16_t>(C + 32) : C;
	}

	bool IsWordAscii(char16_t C)
	{
		return (C >= u'a' && C <= u'z') || (C >= u'A' && C <= u'Z') || (C >= u'0' && C <= u'9') || C == u'_';
	}

	/** Naive literal oracle over ASCII text: non-overlapping, left to right. */
	std::vector<SearchMatch> NaiveLiteral(const std::u16string& Text, const std::u16string& Needle, bool bCase, bool bWord)
	{
		std::vector<SearchMatch> Out;
		size_t I = 0;
		while (I + Needle.size() <= Text.size())
		{
			bool bEq = true;
			for (size_t K = 0; K < Needle.size() && bEq; ++K)
			{
				bEq = bCase ? Text[I + K] == Needle[K] : Lower(Text[I + K]) == Lower(Needle[K]);
			}
			if (bEq && bWord)
			{
				bEq = (I == 0 || !IsWordAscii(Text[I - 1])) && (I + Needle.size() >= Text.size() || !IsWordAscii(Text[I + Needle.size()]));
			}
			if (bEq)
			{
				Out.push_back(SearchMatch{static_cast<dream::Offset>(I), static_cast<int64_t>(Needle.size())});
				I += Needle.size();
			}
			else
			{
				++I;
			}
		}
		return Out;
	}

	std::u16string FromHex(std::string_view Hex)
	{
		std::u16string Out;
		auto Nib = [](char C) { return C <= '9' ? C - '0' : C - 'a' + 10; };
		for (size_t I = 0; I + 3 < Hex.size() + 1 && I + 4 <= Hex.size(); I += 4)
		{
			const int Lo = Nib(Hex[I]) * 16 + Nib(Hex[I + 1]);
			const int Hi = Nib(Hex[I + 2]) * 16 + Nib(Hex[I + 3]);
			Out.push_back(static_cast<char16_t>(Hi * 256 + Lo));
		}
		return Out;
	}

	std::vector<std::string_view> SplitTabs(std::string_view Line)
	{
		std::vector<std::string_view> Out;
		size_t Start = 0;
		for (;;)
		{
			const size_t Tab = Line.find('\t', Start);
			Out.push_back(Line.substr(Start, Tab == std::string_view::npos ? std::string_view::npos : Tab - Start));
			if (Tab == std::string_view::npos)
			{
				return Out;
			}
			Start = Tab + 1;
		}
	}
}

TEST(Search_LiteralAgainstNaiveOracle)   // T-CORE-017: literal, case, whole word
{
	Rng R(17);
	const char16_t Alpha[] = {u'a', u'b', u'A', u'B', u' ', u'_', u'\n', u'.'};
	for (int Case = 0; Case < 400; ++Case)
	{
		std::u16string Text;
		const size_t Len = static_cast<size_t>(R.Below(300));
		for (size_t I = 0; I < Len; ++I)
		{
			Text.push_back(Alpha[R.Below(sizeof(Alpha) / sizeof(Alpha[0]))]);
		}
		std::u16string Needle;
		const size_t NLen = 1 + static_cast<size_t>(R.Below(3));
		for (size_t I = 0; I < NLen; ++I)
		{
			Needle.push_back(Alpha[R.Below(6)]);
		}
		const bool bCase = R.OneIn(2);
		const bool bWord = R.OneIn(3);
		const std::vector<SearchMatch> Got = Find(Needle, Text, false, bCase, bWord);
		const std::vector<SearchMatch> Want = NaiveLiteral(Text, Needle, bCase, bWord);
		CHECK(Got == Want);
		if (Got != Want)
		{
			break;
		}
	}
	// Folding beyond ASCII: Kelvin sign and long s fold like Python.
	CHECK_EQ(Spans(Find(u"k", u"k K \x212A", false, false)), std::string("0-1,2-3,4-5"));
	CHECK_EQ(Spans(Find(u"\x00C9t\x00E9", u"\x00E9T\x00C9!", false, false)), std::string("0-3"));
	CHECK_EQ(Spans(Find(u"x", u"\xD83D\xDE00x", false)), std::string("2-3"));
	CHECK(!Compile(u"", false).Ok());
	CHECK_EQ(Compile(u"", false).Error.Code, dream::SearchErrorCode::EmptyPattern);
}

TEST(Search_AgainstPythonGolden)   // T-CORE-017: Python `re` on the engine interpreter
{
	const char* Golden = std::getenv("DREAM_SEARCH_GOLDEN");
	if (Golden == nullptr || *Golden == '\0')
	{
		std::printf("  (skipped: DREAM_SEARCH_GOLDEN is not set; build.bat sets it when the engine's python.exe exists)\n");
		return;
	}
	std::ifstream In(Golden, std::ios::binary);
	REQUIRE(In.good());
	std::string Line;
	size_t Rows = 0;
	size_t Bad = 0;
	std::u16string LastPattern;
	std::string LastFlags;
	CompileResult C;
	while (std::getline(In, Line))
	{
		const std::vector<std::string_view> F = SplitTabs(Line);
		if (F.size() != 6)
		{
			continue;
		}
		++Rows;
		const std::u16string Pattern = FromHex(F[0]);
		const std::string Flags(F[1]);
		if (Pattern != LastPattern || Flags != LastFlags)
		{
			C = Compile(Pattern, true, Flags != "i");
			LastPattern = Pattern;
			LastFlags = Flags;
		}
		auto Report = [&](const char* What, const std::string& Got, std::string_view Want)
		{
			if (++Bad <= 25)
			{
				std::fprintf(stderr, "  %s: pattern %s flags %s subject %s: got %s want %.*s (%s)\n", What,
					dream_test_util::N(Pattern).c_str(), Flags.c_str(), dream_test_util::N(FromHex(F[2])).c_str(), Got.c_str(),
					static_cast<int>(Want.size()), Want.data(), C.Ok() ? "" : dream_test_util::N(C.Error.Detail).c_str());
			}
		};
		if (F[4] == "E")
		{
			if (C.Ok())
			{
				Report("compiles", "ok", "error");
			}
			continue;
		}
		if (!C.Ok())
		{
			Report("refused", "error", F[4]);
			continue;
		}
		const std::u16string Subject = FromHex(F[2]);
		if (F[3] == "-")
		{
			const std::string Got = Spans(SearchEngine::FindAll(Subject, *C.Query));
			if (Got != F[4])
			{
				Report("spans", Got, F[4]);
			}
			continue;
		}
		const std::u16string Template = FromHex(F[3]);
		const dream::EditBatch Batch = SearchEngine::BuildReplaceAll(TextBuffer(Subject).Snapshot(), *C.Query, Template, false);
		const bool bTemplateOk = SearchEngine::ValidateTemplate(*C.Query, Template);
		if (F[5] == "X")
		{
			if (bTemplateOk)
			{
				Report("template accepted", "ok", "X");
			}
			continue;
		}
		if (!bTemplateOk)
		{
			Report("template refused", "X", "ok");
			continue;
		}
		TextBuffer B(Subject);
		if (!Batch.Ranges.empty())
		{
			B.Apply(Batch);
		}
		const std::u16string Want = FromHex(F[5]);
		if (B.Snapshot().Text() != Want)
		{
			Report("sub", dream_test_util::N(B.Snapshot().Text()), dream_test_util::N(Want));
		}
	}
	std::printf("  %zu golden rows, %zu mismatches\n", Rows, Bad);
	CHECK(Rows > 10000u);
	CHECK_EQ(Bad, 0u);
}

TEST(Search_RegexBasicsAndErrors)
{
	CHECK_EQ(Spans(Find(u"^\\w+", u"ab cd\nef")), std::string("0-2,6-8"));     // MULTILINE always on
	CHECK_EQ(Spans(Find(u"\\w+$", u"ab cd\nef")), std::string("3-5,6-8"));
	CHECK_EQ(Spans(Find(u"a*", u"baa")), std::string("0-0,1-3,3-3"));         // Python 3.7+ empty-match rules
	CHECK_EQ(Spans(Find(u"foo", u"Foo foo FOO", true, false)), std::string("0-3,4-7,8-11"));
	CHECK_EQ(Spans(Find(u"fo+", u"foo xfoo foo_ foo", true, true, true)), std::string("0-3,14-17"));   // whole word
	CHECK_EQ(Spans(Find(u"a|ab", u"ab", true, true, true)), std::string("0-2"));  // whole word backtracks into the second branch
	const CompileResult E = Compile(u"a**", true);
	CHECK(!E.Ok());
	CHECK_EQ(E.Error.Code, dream::SearchErrorCode::Syntax);
	CHECK(E.Error.Detail == u"multiple repeat");
	CHECK_EQ(Compile(u"\\N{EM DASH}", true).Error.Code, dream::SearchErrorCode::Unsupported);
	CHECK_EQ(Compile(u"(a{1000}){1000}", true).Error.Code, dream::SearchErrorCode::TooComplex);
	CHECK(Compile(u"(?<=a+)b", true).Error.Detail == u"look-behind requires fixed-width pattern");
	CHECK(SearchEngine::CanMatchNewline(*Compile(u"a\\sb", true).Query));
	CHECK(!SearchEngine::CanMatchNewline(*Compile(u"a[ \\t]b", true).Query));
	CHECK(SearchEngine::CanMatchNewline(*Compile(u"[^x]", true).Query));
	CHECK_EQ(SearchEngine::GroupCount(*Compile(u"(a)(?:b)(?P<n>c)", true).Query), 3);
}

TEST(Search_StepLimitSkipsTheLine)   // T-CORE-018
{
	std::u16string Text = u"aaa\n";
	Text += std::u16string(100000, u'a');
	Text += u"b\naaa\n";
	const CompileResult C = Compile(u"(a+)+$", true);
	REQUIRE(C.Ok());
	dream::SearchSummary Sum;
	const auto Began = std::chrono::steady_clock::now();
	const std::vector<SearchMatch> M = SearchEngine::FindAll(Text, *C.Query, 0, -1, &Sum);
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Began).count();
	std::printf("  (a+)+$ over a 100k-unit line: %.1f ms, %d complex lines\n", Ms, Sum.ComplexLines);
	CHECK_EQ(Sum.ComplexLines, 1);
	CHECK(Ms <= 50.0);
	REQUIRE_EQ(M.size(), 2u);                          // the lines around it are still searched
	CHECK_EQ(M[0].At, 0);
	CHECK_EQ(M[1].At, static_cast<dream::Offset>(Text.size() - 4));
	// A plain line of 100k 'a' matches at once.
	const std::u16string Plain(100000, u'a');
	CHECK_EQ(SearchEngine::FindAll(Plain, *C.Query).size(), 1u);
	// Deep greedy loops over a long line hit the stack limit, not the C++ stack.
	const CompileResult D = Compile(u"(?:a|b)*c", true);
	dream::SearchSummary Sum2;
	SearchEngine::FindAll(std::u16string(3000000, u'a'), *D.Query, 0, -1, &Sum2);
	CHECK_EQ(Sum2.ComplexLines, 1);
	// Backreference comparisons are steps too (review r1 core-formats-M1): (a+)\1b and (.+)\1 on 100k-unit lines end
	// within the per-line bound instead of costing O(n^2) uncounted work.
	const std::u16string Long(100000, u'a');
	for (const char16_t* Pattern : {u"(a+)\\1b", u"(.+)\\1"})
	{
		const CompileResult B = Compile(Pattern, true);
		REQUIRE(B.Ok());
		dream::SearchSummary Sum3;
		const auto T0 = std::chrono::steady_clock::now();
		SearchEngine::FindAll(Long, *B.Query, 0, -1, &Sum3);
		const double BMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
		std::string Name;
		dream::utf::Utf16ToUtf8(Pattern, Name);
		std::printf("  %s over a 100k-unit line: %.1f ms, %d complex lines\n", Name.c_str(), BMs, Sum3.ComplexLines);
		CHECK(BMs <= 50.0);
	}
	const CompileResult B2 = Compile(u"(a+)\\1b", true);
	dream::SearchSummary Sum4;
	SearchEngine::FindAll(Long, *B2.Query, 0, -1, &Sum4);
	CHECK_EQ(Sum4.ComplexLines, 1);   // reported as too complex, like other work over the limit
}

TEST(Search_CompileIsBounded)   // review r1 minor: repeats of a body that emits nothing
{
	const auto T0 = std::chrono::steady_clock::now();
	const CompileResult A = Compile(u"(?:(?:){100000}){100000}", true);
	const CompileResult B = Compile(u"(?:(?:(?:){65535}){65535}){65535}x", true);
	const CompileResult C = Compile(u"(?:a{0}){100000}y", true);
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
	std::printf("  three empty-body repeat patterns compiled in %.2f ms\n", Ms);
	CHECK(Ms <= 50.0);
	REQUIRE(A.Ok());
	REQUIRE(B.Ok());
	REQUIRE(C.Ok());
	CHECK_EQ(SearchEngine::FindAll(std::u16string(u"ab"), *A.Query).size(), 3u);   // empty matches at 0, 1, 2 as in Python
	REQUIRE_EQ(SearchEngine::FindAll(std::u16string(u"axb"), *B.Query).size(), 1u);
	CHECK_EQ(SearchEngine::FindAll(std::u16string(u"axb"), *B.Query)[0].At, 1);
	CHECK_EQ(SearchEngine::FindAll(std::u16string(u"yy"), *C.Query).size(), 2u);

	// One real node among thousands of empty ones, repeated {100000}: a small program from 3x10^8 node visits. The
	// generation work is bounded (review r2): refused as too complex, quickly, instead of compiling for a second.
	std::u16string Padded = u"(?:";
	for (int K = 0; K < 3000; ++K)
	{
		Padded += u"(?:)";
	}
	Padded += u"a){100000}";
	const auto T1 = std::chrono::steady_clock::now();
	const CompileResult D = Compile(Padded, true);
	const double PaddedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T1).count();
	std::printf("  3,000 empty groups and one char repeated {100000}: %.2f ms\n", PaddedMs);
	CHECK(PaddedMs <= 50.0);
	CHECK(!D.Ok());
	CHECK_EQ(D.Error.Code, dream::SearchErrorCode::TooComplex);
	// A repeat around a {0} body of thousands of empty groups (review r3): the emptiness of a body is known once per
	// node, not walked on every one of the 10^5 generations. Each compiles quickly or is refused as too complex.
	std::u16string Empty10k;
	for (int K = 0; K < 10000; ++K)
	{
		Empty10k += u"(?:)";
	}
	const std::u16string Empty3k = Empty10k.substr(0, 3000 * 4);
	const std::u16string Bypass[] = {
		u"(?:b(?:" + Empty3k + u"a){0}){100000}",
		u"(?:b(?:" + Empty3k + u"a){0}){60000}",
		u"(?:b(?:" + Empty10k + u"a){0}){100000}",
		u"(?:(?:b(?:" + Empty3k + u"a){0}){100}){2900}",
		u"(?:(?:b(?:" + Empty10k + u"a){0}){100}){2900}",
	};
	for (const std::u16string& Pattern : Bypass)
	{
		const auto T2 = std::chrono::steady_clock::now();
		const CompileResult R = Compile(Pattern, true);
		const double Ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T2).count();
		std::printf("  %zu-unit {0}-body repeat pattern: %.2f ms, %s\n", Pattern.size(), Ms2, R.Ok() ? "compiled" : "too complex");
		CHECK(Ms2 <= 50.0);
		CHECK(R.Ok() || R.Error.Code == dream::SearchErrorCode::TooComplex);
	}
	CHECK(SearchEngine::FindAll(std::u16string(60000, u'b'), *Compile(Bypass[1], true).Query).size() == 1u);
	// Large but ordinary counted repeats still compile.
	CHECK(Compile(u"a{100000}", true).Ok());
	CHECK(Compile(u"(?:ab){60000}", true).Ok());
}

namespace
{
	/** DREAM_SKIP_TIMING=1 skips the timing gates (debugger, coverage or sanitizer builds), as test_perf.cpp's. */
	bool SkipTiming()
	{
		const char* V = std::getenv("DREAM_SKIP_TIMING");
		return V != nullptr && *V != '\0' && *V != '0';
	}
}

TEST(Search_RunOrderMultilineAndCancel)   // T-CORE-019
{
	// 100k lines; the visible range comes first, and its first batch is fast.
	std::u16string Text;
	for (int I = 0; I < 100000; ++I)
	{
		Text += (I % 7 == 0) ? u"value = needle(1)\n" : u"other = thing(2)\n";
	}
	const TextBuffer B(Text);
	const TextSnapshot S = B.Snapshot();
	const CompileResult C = Compile(u"needle", false);
	const dream::Offset VisFirst = S.LineStart(50000);
	const dream::Offset VisLast = S.LineStart(50060);
	// Timing gates keep the best of three attempts, as test_perf.cpp's: other builds preempt the process, and noise
	// only ever adds time.
	std::vector<std::vector<SearchMatch>> Batches;
	double FirstMs = -1.0;
	dream::SearchSummary Sum;
	for (int Try = 1; Try <= 3; ++Try)
	{
		Batches.clear();
		FirstMs = -1.0;
		const auto Began = std::chrono::steady_clock::now();
		Sum = SearchEngine::Run(S, *C.Query, VisFirst, VisLast, nullptr, [&](std::span<const SearchMatch> M)
		{
			if (FirstMs < 0.0)
			{
				FirstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Began).count();
			}
			Batches.emplace_back(M.begin(), M.end());
		});
		std::printf("  try %d: first batch after %.2f ms, %zu batches, %zu matches\n", Try, FirstMs, Batches.size(), Sum.Count);
		if (FirstMs <= 16.0)
		{
			break;
		}
	}
	CHECK(FirstMs <= 16.0 || SkipTiming());
	REQUIRE(!Batches.empty());
	CHECK(!Batches[0].empty());
	for (const SearchMatch& M : Batches[0])
	{
		CHECK(M.At >= VisFirst && M.At <= S.LineEnd(S.LineOf(VisLast)));
	}
	std::vector<SearchMatch> All;
	for (const auto& Bt : Batches)
	{
		All.insert(All.end(), Bt.begin(), Bt.end());
	}
	std::sort(All.begin(), All.end(), [](const SearchMatch& A, const SearchMatch& Bm) { return A.At < Bm.At; });
	CHECK(All == SearchEngine::FindAll(S, *C.Query));
	CHECK_EQ(All.size(), static_cast<size_t>((100000 + 6) / 7));

	// Multi-line matches are found (and searched in document order).
	const CompileResult ML = Compile(u"needle\\(1\\)\\nother", true);
	REQUIRE(ML.Ok());
	CHECK(SearchEngine::CanMatchNewline(*ML.Query));
	CHECK_EQ(SearchEngine::FindAll(S, *ML.Query).size(), static_cast<size_t>((100000 + 6) / 7));

	// The cap.
	dream::SearchLimits Few;
	Few.MaxMatches = 10;
	dream::SearchSummary Capped;
	CHECK_EQ(SearchEngine::FindAll(Text, *C.Query, 0, -1, &Capped, nullptr, Few).size(), 10u);
	CHECK(Capped.bCapped);

	// Cancel from another thread takes effect within 1 ms.
	std::u16string Slow;
	for (int I = 0; I < 2000; ++I)
	{
		Slow += std::u16string(400, u'a') + u"b\n";
	}
	const TextBuffer SB(Slow);
	const CompileResult Bad = Compile(u"(a|aa)*c", true);
	bool bCancelInTime = false;
	for (int Try = 1; Try <= 3 && !bCancelInTime; ++Try)
	{
		dream::CancelToken Token;
		std::atomic<bool> bDone{false};
		std::chrono::steady_clock::time_point Finished;
		dream::SearchSummary CancelSum;
		std::thread Worker([&]()
		{
			CancelSum = SearchEngine::Run(SB.Snapshot(), *Bad.Query, 0, 0, &Token, [](std::span<const SearchMatch>) {});
			Finished = std::chrono::steady_clock::now();
			bDone = true;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		const bool bStillRunning = !bDone;
		const auto CancelAt = std::chrono::steady_clock::now();
		Token.Cancel();
		Worker.join();
		const double CancelMs = std::chrono::duration<double, std::milli>(Finished - CancelAt).count();
		std::printf("  try %d: cancel took %.3f ms (still running at cancel: %d)\n", Try, CancelMs, bStillRunning ? 1 : 0);
		if (bStillRunning)
		{
			CHECK(CancelSum.bCancelled);   // not a timing check: every attempt that was cancelled must say so
		}
		bCancelInTime = !bStillRunning || CancelMs <= 1.0;
	}
	CHECK(bCancelInTime || SkipTiming());
}

TEST(Search_CancelInsideBackreference)   // review r2: backreference steps poll the cancel token too
{
	// Per start position: 4,000 steps to capture (a{4000}), then 1,000 backreference compares of 4,000 units each,
	// 4,000,000 steps with only 1,000 instruction boundaries between them. The main loop polls the token only at those
	// boundaries, so without a poll inside the comparison loop a cancel waits milliseconds for the attempt to end. A
	// raised step limit keeps the one line going long enough to cancel it. Best of three attempts, as the other
	// timing gates; DREAM_SKIP_TIMING=1 skips the bound.
	const std::u16string Text(4010000, u'a');
	const CompileResult C = Compile(u"(a{4000})\\1{1000}x", true);
	REQUIRE(C.Ok());
	dream::SearchLimits Long;
	Long.StepLimit = 1000000000;
	bool bInTime = false;
	for (int Try = 1; Try <= 3 && !bInTime; ++Try)
	{
		dream::CancelToken Token;
		std::atomic<bool> bDone{false};
		std::chrono::steady_clock::time_point Finished;
		dream::SearchSummary Sum;
		std::thread Worker([&]()
		{
			SearchEngine::FindAll(Text, *C.Query, 0, -1, &Sum, &Token, Long);
			Finished = std::chrono::steady_clock::now();
			bDone = true;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
		const bool bStillRunning = !bDone;
		const auto CancelAt = std::chrono::steady_clock::now();
		Token.Cancel();
		Worker.join();
		const double CancelMs = std::chrono::duration<double, std::milli>(Finished - CancelAt).count();
		std::printf("  try %d: cancel inside backreferences took %.3f ms (still running at cancel: %d)\n", Try, CancelMs,
			bStillRunning ? 1 : 0);
		if (bStillRunning)
		{
			CHECK(Sum.bCancelled);
		}
		bInTime = !bStillRunning || CancelMs <= 1.0;
	}
	CHECK(bInTime || SkipTiming());
}

TEST(Search_MaintenanceEqualsRescan)   // T-CORE-020
{
	const char16_t* Patterns[][2] = {
		{u"ab", u"l"}, {u"Ab", u"li"}, {u"ab", u"lw"}, {u"a+b?", u"r"}, {u"\\bx\\w*", u"r"}, {u"^a", u"r"}, {u"b$", u"r"},
		{u"a\\nb", u"r"}, {u"(?<=a)b", u"r"}, {u"[ab]{2}", u"ri"},
	};
	const char16_t Alpha[] = {u'a', u'b', u'A', u'x', u' ', u'\n'};
	for (const auto& P : Patterns)
	{
		const std::u16string_view Kind(P[1]);
		const CompileResult C = Compile(P[0], Kind.find(u'r') != std::u16string_view::npos, Kind.find(u'i') == std::u16string_view::npos,
			Kind.find(u'w') != std::u16string_view::npos);
		REQUIRE(C.Ok());
		Rng R(20 + static_cast<uint64_t>(P[0][0]));
		TextBuffer B(u"ab ab\nxab b\naab");
		std::vector<SearchMatch> M = SearchEngine::FindAll(B.Snapshot(), *C.Query);
		int Bad = 0;
		for (int Step = 0; Step < 600 && Bad < 2; ++Step)
		{
			dream::EditBatch E;
			const int Ranges = 1 + static_cast<int>(R.Below(2));
			dream::Offset Floor = 0;
			for (int K = 0; K < Ranges; ++K)
			{
				const int64_t Size = B.Length();
				if (Floor > Size)
				{
					break;
				}
				const dream::Offset At = Floor + static_cast<dream::Offset>(R.Below(static_cast<uint64_t>(Size - Floor + 1)));
				const int64_t Del = std::min<int64_t>(static_cast<int64_t>(R.Below(3)), Size - At);
				std::u16string Ins;
				for (uint64_t L = R.Below(4); L > 0; --L)
				{
					Ins.push_back(Alpha[R.Below(sizeof(Alpha) / sizeof(Alpha[0]))]);
				}
				E.Ranges.push_back(dream::EditRange{At, Del, Ins});
				Floor = At + Del + 1;
			}
			const dream::ChangeSet Ch = B.Apply(E);
			SearchEngine::UpdateAfterEdit(M, Ch, B.Snapshot(), *C.Query);
			const std::vector<SearchMatch> Full = SearchEngine::FindAll(B.Snapshot(), *C.Query);
			if (M != Full)
			{
				++Bad;
				std::fprintf(stderr, "  %s step %d: got %s want %s\n", dream_test_util::N(P[0]).c_str(), Step, Spans(M).c_str(), Spans(Full).c_str());
			}
			CHECK(M == Full);
		}
	}
}

TEST(Search_ReplaceAllAndTemplates)   // T-CORE-021
{
	const TextBuffer B(u"foo Foo FOO food\nfoo");
	const CompileResult C = Compile(u"foo", false, false, true);
	// Preserve case: upper stays upper, capitalised stays capitalised, otherwise as typed.
	const dream::EditBatch E = SearchEngine::BuildReplaceAll(B.Snapshot(), *C.Query, u"bar", true);
	CHECK_EQ(E.Ranges.size(), 4u);
	CHECK_EQ(E.Origin, dream::EditOrigin::Command);
	TextBuffer Out(B.Snapshot().Text());
	Out.Apply(E);
	CHECK(Out.Snapshot().Text() == u"bar Bar BAR food\nbar");
	// In-selection: nothing changes outside [From, To].
	const dream::EditBatch Sel = SearchEngine::BuildReplaceAll(B.Snapshot(), *C.Query, u"X", false, 4, 11);
	TextBuffer SelOut(B.Snapshot().Text());
	SelOut.Apply(Sel);
	CHECK(SelOut.Snapshot().Text() == u"foo X X food\nfoo");
	// Templates: groups by number and name, $ forms, escapes, errors.
	const CompileResult R = Compile(u"(?P<k>\\w+)=(\\d+)", true);
	const std::u16string Text = u"a=1, bb=22";
	std::u16string Rep;
	const std::vector<SearchMatch> M = SearchEngine::FindAll(Text, *R.Query);
	REQUIRE_EQ(M.size(), 2u);
	CHECK(SearchEngine::ExpandReplacement(Text, *R.Query, M[1], u"\\2:\\g<k>|$1|${2}|$$|\\n|\\\\", false, Rep));
	CHECK(Rep == u"22:bb|bb|22|$|\n|\\");
	dream::SearchError Err;
	CHECK(!SearchEngine::ValidateTemplate(*R.Query, u"\\3", &Err));
	CHECK_EQ(Err.Code, dream::SearchErrorCode::BadTemplate);
	CHECK(!SearchEngine::ValidateTemplate(*R.Query, u"\\g<nope>", &Err));
	CHECK(!SearchEngine::ValidateTemplate(*R.Query, u"\\q", &Err));
	CHECK(SearchEngine::ValidateTemplate(*R.Query, u"$x \\& $", &Err));
	CHECK(!SearchEngine::ExpandReplacement(Text, *R.Query, SearchMatch{1, 2}, u"x", false, Rep));
	// A literal query's template is literal.
	const CompileResult L = Compile(u"a=1", false);
	CHECK(SearchEngine::ExpandReplacement(Text, *L.Query, SearchMatch{0, 3}, u"\\1$1", false, Rep));
	CHECK(Rep == u"\\1$1");
	// Replace All of a regex is one batch; applying it once equals re.sub.
	const CompileResult W = Compile(u"(\\w+)@(\\w+)", true);
	const TextBuffer Mail(u"a@b c@d");
	TextBuffer MailOut(Mail.Snapshot().Text());
	MailOut.Apply(SearchEngine::BuildReplaceAll(Mail.Snapshot(), *W.Query, u"\\2 at \\1", false));
	CHECK(MailOut.Snapshot().Text() == u"b at a d at c");
	// FindNext forward and backward with wrap.
	const TextBuffer N(u"x ab ab x");
	const CompileResult AB = Compile(u"ab", false);
	CHECK(SearchEngine::FindNext(N.Snapshot(), *AB.Query, 3, false) == (SearchMatch{5, 2}));
	CHECK(SearchEngine::FindNext(N.Snapshot(), *AB.Query, 6, false) == (SearchMatch{2, 2}));   // wraps
	CHECK(!SearchEngine::FindNext(N.Snapshot(), *AB.Query, 6, false, false).has_value());
	CHECK(SearchEngine::FindNext(N.Snapshot(), *AB.Query, 6, true) == (SearchMatch{2, 2}));
	CHECK(SearchEngine::FindNext(N.Snapshot(), *AB.Query, 1, true) == (SearchMatch{5, 2}));    // wraps backward
}
