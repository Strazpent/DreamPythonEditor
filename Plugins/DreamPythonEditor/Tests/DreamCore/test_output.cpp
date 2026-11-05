// SPDX-License-Identifier: Apache-2.0
// Tier 1: OutputRing (TDD §6.2, T-OUT-001), LineSplitter (§6.1, T-OUT-002), ScanLinks and pseudo filenames
// (§6.2, T-OUT-004, RUN-06), VisibleIndex (§6.4, T-OUT-006).
#include "dream_test.h"
#include "test_util.h"

#include "dream/OutputLines.h"
#include "dream/OutputRing.h"
#include "dream/VisibleIndex.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace dream;
using dream_test_util::Rng;

namespace
{
	LineInfo Info(uint32_t Run, LineType Type = LineType::Stdout)
	{
		LineInfo I;
		I.RunNumber = Run;
		I.Type = Type;
		return I;
	}

	std::vector<SplitLine> Feed(LineSplitter& S, std::initializer_list<std::string_view> Writes)
	{
		std::vector<SplitLine> Out;
		for (std::string_view W : Writes)
		{
			S.Feed(W, Out);
		}
		return Out;
	}
}

TEST(OutputRing_AppendGetAndSpans)
{
	OutputRing Ring;
	LinkSpan Link;
	Link.Start = 2;
	Link.Len = 5;
	Link.Line = 88;
	const StyleSpan Style{0, 3, 7};
	const uint64_t A = Ring.Append(Info(1), "hello", std::span<const StyleSpan>(&Style, 1), std::span<const LinkSpan>(&Link, 1));
	const uint64_t B = Ring.Append(Info(1, LineType::Echo), "", {}, {});
	CHECK_EQ(A, 0u);
	CHECK_EQ(B, 1u);
	LineView V;
	REQUIRE(Ring.GetLine(A, V));
	CHECK(V.Text == "hello");
	CHECK_EQ(V.Info.RunNumber, 1u);
	CHECK((V.Info.Flags & OLF_HasLinks) != 0);
	REQUIRE_EQ(V.Links.size(), 1u);
	CHECK_EQ(V.Links[0].Line, 88);
	REQUIRE_EQ(V.Styles.size(), 1u);
	CHECK_EQ(V.Styles[0].Style, 7u);
	const char* Before = V.Text.data();
	for (int I = 0; I < 1000; ++I)
	{
		Ring.Append(Info(2), "more text that fills the chunk", {}, {});
	}
	LineView Again;
	REQUIRE(Ring.GetLine(A, Again));
	CHECK(Again.Text.data() == Before);   // views stay valid while the line lives
	CHECK(!Ring.GetLine(Ring.EndSeq(), V));
	REQUIRE(Ring.GetLine(B, V));
	CHECK(V.Info.Type == LineType::Echo);
	CHECK(V.Text.empty());
}

TEST(OutputRing_CapsEvictWholeChunksAndReport)
{
	OutputRingConfig Config;
	Config.MaxLines = 80;
	Config.MaxBytes = 1u << 20;
	OutputRing Ring(Config);
	std::vector<EvictionInfo> Events;
	Ring.SetEvictionListener([&Events](const EvictionInfo& E) { Events.push_back(E); });
	for (uint32_t I = 0; I < 300; ++I)
	{
		Ring.Append(Info(1 + I / 25), "line " + std::to_string(I), {}, {});
		CHECK(Ring.LineCount() <= 80u);
	}
	CHECK_EQ(Ring.EndSeq(), 300u);
	REQUIRE(!Events.empty());
	uint64_t Expected = 0;
	for (const EvictionInfo& E : Events)
	{
		CHECK_EQ(E.FirstEvicted, Expected);   // exact, contiguous ranges
		Expected = E.LastEvicted + 1;
		CHECK(!E.RunNumbers.empty());
		CHECK(std::is_sorted(E.RunNumbers.begin(), E.RunNumbers.end()));
		CHECK_EQ(E.RunNumbers.front(), 1 + static_cast<uint32_t>(E.FirstEvicted / 25));
		CHECK_EQ(E.RunNumbers.back(), 1 + static_cast<uint32_t>(E.LastEvicted / 25));
	}
	CHECK_EQ(Ring.FirstSeq(), Expected);
	LineView V;
	CHECK(!Ring.GetLine(Ring.FirstSeq() - 1, V));
	REQUIRE(Ring.GetLine(Ring.FirstSeq(), V));
	CHECK(V.Text == "line " + std::to_string(Ring.FirstSeq()));

	OutputRingConfig Bytes;
	Bytes.MaxBytes = 1000;
	OutputRing Small(Bytes);
	for (int I = 0; I < 100; ++I)
	{
		Small.Append(Info(1), std::string(100, 'x'), {}, {});
		CHECK(Small.TextBytes() <= 1000u);
	}
	CHECK_EQ(Small.EndSeq(), 100u);
}

TEST(OutputRing_SnapshotsSurviveEvictionAndClear)
{
	OutputRingConfig Config;
	Config.MaxLines = 40;
	OutputRing Ring(Config);
	for (int I = 0; I < 30; ++I)
	{
		Ring.Append(Info(1), "a" + std::to_string(I), {}, {});
	}
	const OutputSnapshot Snap = Ring.TakeSnapshot();
	for (int I = 0; I < 200; ++I)
	{
		Ring.Append(Info(2), "b" + std::to_string(I), {}, {});
	}
	CHECK_EQ(Snap.FirstSeq(), 0u);
	CHECK_EQ(Snap.EndSeq(), 30u);
	for (uint64_t S = 0; S < 30; ++S)
	{
		LineView V;
		REQUIRE(Snap.GetLine(S, V));
		CHECK(V.Text == "a" + std::to_string(S));
	}
	LineView V;
	CHECK(!Snap.GetLine(30, V));
	CHECK(!Ring.GetLine(0, V));

	const uint64_t Keep = Ring.EndSeq() - 5;
	const uint64_t BytesBefore = Ring.TextBytes();
	Ring.ClearBefore(Keep);
	CHECK_EQ(Ring.FirstSeq(), Keep);
	CHECK_EQ(Ring.LineCount(), 5u);
	CHECK(Ring.TextBytes() < BytesBefore);
	CHECK(!Ring.GetLine(Keep - 1, V));
	CHECK(Ring.GetLine(Keep, V));
	CHECK(OutputSnapshot().GetLine(0, V) == false);
}

TEST(OutputRing_OverlongLineIsCutOnUtf8Boundary)
{
	OutputRingConfig Config;
	Config.ChunkBytes = 1024;
	OutputRing Ring(Config);
	std::string Long;
	for (int I = 0; I < 1000; ++I)
	{
		Long += "\xC3\xA9";
	}
	const uint64_t S = Ring.Append(Info(1), Long, {}, {});
	LineView V;
	REQUIRE(Ring.GetLine(S, V));
	CHECK(V.Text.size() < 1024u);
	CHECK(V.Text.size() % 2 == 0);
	CHECK(V.Text.size() > 900u);
}

TEST(LineSplitter_Basics)
{
	LineSplitter S;
	std::vector<SplitLine> L = Feed(S, {"a\nb\n", "\n\n"});
	REQUIRE_EQ(L.size(), 4u);
	CHECK_EQ(L[0].Text, std::string("a"));
	CHECK_EQ(L[2].Text, std::string(""));
	CHECK_EQ(L[0].Flags, 0);
	L = Feed(S, {"a\r\nb"});
	REQUIRE_EQ(L.size(), 1u);
	CHECK(S.HasPartial());
	CHECK(S.PartialText() == "b");
	std::vector<SplitLine> F;
	S.Flush(F);
	REQUIRE_EQ(F.size(), 1u);
	CHECK_EQ(F[0].Text, std::string("b"));
	CHECK_EQ(F[0].Flags, static_cast<uint16_t>(OLF_Partial));
	CHECK(!S.HasPartial());
	F.clear();
	S.Flush(F);
	CHECK(F.empty());
}

TEST(LineSplitter_CarriageReturns)
{
	LineSplitter S;
	// print('x\r') writes "x\r" then "\n": one line "x".
	std::vector<SplitLine> L = Feed(S, {"x\r", "\n"});
	REQUIRE_EQ(L.size(), 1u);
	CHECK_EQ(L[0].Text, std::string("x"));
	// A CRLF split across two writes.
	L = Feed(S, {"line\r", "\nnext\n"});
	REQUIRE_EQ(L.size(), 2u);
	CHECK_EQ(L[0].Text, std::string("line"));
	// Progress bars: a lone CR replaces the open text.
	L = Feed(S, {"10%\r20%\r", "done\n"});
	REQUIRE_EQ(L.size(), 1u);
	CHECK_EQ(L[0].Text, std::string("done"));
	// A held CR at run end ends its line.
	L = Feed(S, {"last\r"});
	CHECK(L.empty());
	std::vector<SplitLine> F;
	S.Flush(F);
	REQUIRE_EQ(F.size(), 1u);
	CHECK_EQ(F[0].Text, std::string("last"));
	CHECK_EQ(F[0].Flags, 0);
}

TEST(LineSplitter_EchoSplitAndLimits)
{
	LineSplitter S;
	std::vector<SplitLine> L = Feed(S, {"Working..."});
	CHECK(L.empty());
	std::vector<SplitLine> F;
	S.Flush(F, true);
	REQUIRE_EQ(F.size(), 1u);
	CHECK_EQ(F[0].Flags, static_cast<uint16_t>(OLF_Partial | OLF_EchoSplit));
	L = Feed(S, {" done\n"});
	REQUIRE_EQ(L.size(), 1u);
	CHECK_EQ(L[0].Text, std::string(" done"));                       // a later write starts a new line
	CHECK_EQ(L[0].Flags, 0);

	// 16 KB of open text at the end of a write is committed Partial; its terminator adds no empty line.
	L = Feed(S, {std::string(20000, 'p'), "\n"});
	REQUIRE_EQ(L.size(), 1u);
	CHECK_EQ(L[0].Text.size(), 20000u);
	CHECK_EQ(L[0].Flags, static_cast<uint16_t>(OLF_Partial));
	L = Feed(S, {std::string(20000, 'q') + "\n"});
	REQUIRE_EQ(L.size(), 1u);
	CHECK_EQ(L[0].Flags, 0);

	// Over 64 KB inside one write: continuation records, never splitting a UTF-8 sequence.
	std::string Big = "a";
	for (int I = 0; I < 80000; ++I)
	{
		Big += "\xC3\xA9";
	}
	L = Feed(S, {Big + "\n"});
	REQUIRE(L.size() >= 3u);
	std::string Joined;
	for (size_t I = 0; I < L.size(); ++I)
	{
		CHECK(L[I].Text.size() <= LineSplitter::ContinuationBytes);
		CHECK_EQ(L[I].Flags, static_cast<uint16_t>(I == 0 ? 0 : OLF_Continued));
		CHECK((static_cast<unsigned char>(L[I].Text[0]) & 0xC0u) != 0x80u);
		Joined += L[I].Text;
	}
	CHECK(Joined == Big);
}

TEST(ScanLinks_PositivesAndNegatives)
{
	std::vector<LinkSpan> Out;
	const std::string Frame = "  File \"D:\\dev\\studio\\importer.py\", line 88, in <module>";
	REQUIRE(ScanLinks(Frame, Out));
	REQUIRE_EQ(Out.size(), 1u);
	CHECK(Out[0].Kind == LinkKind::PythonFrame);
	CHECK_EQ(Out[0].Line, 88);
	CHECK(Frame.substr(Out[0].PathStart, Out[0].PathLen) == "D:\\dev\\studio\\importer.py");
	CHECK(Frame.substr(Out[0].Start, Out[0].Len) == "File \"D:\\dev\\studio\\importer.py\", line 88");

	Out.clear();
	REQUIRE(ScanLinks("  File \"<dream:Untitled-3#a41f>\", line 18", Out));
	CHECK(Out[0].Kind == LinkKind::PseudoFile);
	CHECK_EQ(Out[0].Line, 18);

	struct Pos
	{
		std::string Line;
		std::string Path;
		int32_t L;
		int32_t C;
	};
	const Pos Positives[] = {
		{"D:\\x\\y.py(88)", "D:\\x\\y.py", 88, 0},
		{"D:\\x\\y.py(88,5): error C2065: 'z'", "D:\\x\\y.py", 88, 5},
		{"/home/u/a.py(3) warning", "/home/u/a.py", 3, 0},
		{"see Content/Python/tools/a.py(12)", "Content/Python/tools/a.py", 12, 0},
		{"Warning: C:\\Program Files\\Epic\\x.py(10): msg", "C:\\Program Files\\Epic\\x.py", 10, 0},
		{"LogPython: Error: C:/p/m.py(7)", "C:/p/m.py", 7, 0},
	};
	for (const Pos& P : Positives)
	{
		Out.clear();
		REQUIRE(ScanLinks(P.Line, Out));
		REQUIRE_EQ(Out.size(), 1u);
		CHECK(Out[0].Kind == LinkKind::FileLine);
		CHECK_EQ(P.Line.substr(Out[0].PathStart, Out[0].PathLen), P.Path);
		CHECK_EQ(Out[0].Line, P.L);
		CHECK_EQ(Out[0].Col, P.C);
	}
	const char* Negatives[] = {
		"print(1)", "range(10)", "foo.bar(3)", "Vector(1.0, 2.0)", "x/y.py(0)", "file(abc)", "LogTemp: obj.method(5) returned",
		"[1, 2, 3] (4)", "a/b(3)", "  File \"<string>\", line 1", "  File \"<frozen importlib._bootstrap>\", line 241",
		"Traceback (most recent call last):", "x = f(1)(2)", "ratio 3/4.5(2)", "MyFile \"a.py\", line 3", "ok", "",
		"LogStreaming: Display: Took 0.02s to load (3 packages)", "C:(1)", "./(1)",
	};
	for (const char* N : Negatives)
	{
		Out.clear();
		CHECK(!ScanLinks(N, Out));
		CHECK(Out.empty());
	}
	Out.clear();
	CHECK(ScanLinks("  File \"a/b.py\", line 2 and D:\\c.py(3)", Out));
	REQUIRE_EQ(Out.size(), 2u);
	CHECK(Out[0].Start < Out[1].Start);
}

TEST(ScanLinks_PseudoFilenames)
{
	std::string_view Name;
	std::string_view Id;
	REQUIRE(ParsePseudoFilename("<dream:Untitled-3#a41f>", Name, Id));
	CHECK(Name == "Untitled-3");
	CHECK(Id == "a41f");
	REQUIRE(ParsePseudoFilename("<dream:REPL#42>", Name, Id));
	CHECK(Name == "REPL");
	CHECK(Id == "42");
	REQUIRE(ParsePseudoFilename("<dream:My #Tab#1F>", Name, Id));
	CHECK(Name == "My #Tab");
	CHECK(Id == "1F");
	for (const char* Bad : {"<dream:#a>", "<dream:x#>", "<dream:x#zz>", "<other:x#1>", "<dream:x#1", "dream:x#1>", "<dream:>"})
	{
		CHECK(!ParsePseudoFilename(Bad, Name, Id));
	}
}

TEST(VisibleIndex_MatchesBruteForce)
{
	Rng R(5);
	VisibleIndex Index;
	std::set<uint64_t> Model;
	uint64_t Tail = 0;
	for (int Op = 0; Op < 20000; ++Op)
	{
		switch (R.Below(10))
		{
		case 0:
		{
			// A rebuilt filter: random sorted ranges.
			std::vector<SeqRange> Ranges;
			Model.clear();
			uint64_t At = R.Below(50);
			for (int K = 0; K < 20; ++K)
			{
				const uint64_t Len = R.Below(6);
				Ranges.push_back(SeqRange{At, At + Len});
				for (uint64_t S = At; S < At + Len; ++S)
				{
					Model.insert(S);
				}
				At += Len + R.Below(4);
			}
			Tail = At;
			Index.Assign(Ranges);
			break;
		}
		case 1:
		{
			const uint64_t Cut = R.Below(Tail + 1);
			Index.DropBefore(Cut);
			Model.erase(Model.begin(), Model.lower_bound(Cut));
			break;
		}
		default:
		{
			const uint64_t Begin = Tail + R.Below(3);
			const uint64_t End = Begin + R.Below(4);
			Index.Append(Begin, End);
			for (uint64_t S = Begin; S < End; ++S)
			{
				Model.insert(S);
			}
			Tail = std::max(Tail, End);
			break;
		}
		}
		REQUIRE_EQ(Index.RowCount(), static_cast<uint64_t>(Model.size()));
		if (!Model.empty())
		{
			const uint64_t Row = R.Below(Model.size());
			auto It = Model.begin();
			std::advance(It, static_cast<std::ptrdiff_t>(Row));
			CHECK(Index.SeqAtRow(Row) == std::optional<uint64_t>(*It));
			const uint64_t Probe = R.Below(Tail + 2);
			auto At = Model.lower_bound(Probe);
			const std::optional<uint64_t> WantRow = At == Model.end()
				? std::nullopt : std::optional<uint64_t>(static_cast<uint64_t>(std::distance(Model.begin(), At)));
			CHECK(Index.RowAtOrAfter(Probe) == WantRow);
			CHECK_EQ(Index.Contains(Probe), Model.count(Probe) == 1);
		}
		CHECK(!Index.SeqAtRow(Model.size()).has_value());
	}
	for (size_t I = 1; I < Index.Ranges().size(); ++I)
	{
		CHECK(Index.Ranges()[I - 1].End < Index.Ranges()[I].Begin);   // merged and sorted
	}
	Index.Clear();
	CHECK_EQ(Index.RowCount(), 0u);
}
