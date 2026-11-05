// SPDX-License-Identifier: Apache-2.0
// Tier 1: the §4.1.3 piece tree behind dream::TextBuffer (B02): the string oracle over random edits with typing,
// pastes, chunk-sized pastes and Replace-All batches (T-CORE-001; DREAM_FULL_SCALE=1 runs 1M edits), typing coalescing
// (10,000 typed characters add <= 2 pieces), the line index at 100k lines (T-CORE-002), a reader thread holding a
// snapshot during 10k edits (T-CORE-003; TSan and ASan are not available with this MSVC), compaction and its version
// check (T-CORE-005), ChangeSet minimality (T-CORE-006), ChangesBetween, chunk release, and dream::AnchorSet against
// an oracle (T-CORE-008 shape at tier-1 size).
#define _CRT_SECURE_NO_WARNINGS   // std::getenv
#include "dream_test.h"
#include "test_util.h"

#include "dream/AnchorSet.h"
#include "dream/TextBuffer.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

using dream::AnchorDeletePolicy;
using dream::AnchorGravity;
using dream::AnchorId;
using dream::AnchorKind;
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
	bool FullScale()
	{
		const char* V = std::getenv("DREAM_FULL_SCALE");
		return V != nullptr && *V != '\0' && *V != '0';
	}

	EditBatch One(Offset At, int64_t Del, std::u16string Ins)
	{
		EditBatch B;
		B.Ranges.push_back(EditRange{At, Del, std::move(Ins)});
		return B;
	}

	std::u16string Lines(int Count, int Width)
	{
		std::u16string Out;
		for (int L = 0; L < Count; ++L)
		{
			for (int C = 0; C < (L * 7 + 3) % Width; ++C)
			{
				Out.push_back(static_cast<char16_t>(u'a' + (L + C) % 26));
			}
			Out.push_back(u'\n');
		}
		return Out;
	}

	/** Random model text: letters, blanks, '\n', 'é', a CJK unit and whole surrogate pairs. */
	std::u16string RandomText(Rng& R, size_t Len)
	{
		std::u16string Out;
		while (Out.size() < Len)
		{
			switch (R.Below(12))
			{
			case 0: Out.push_back(u'\n'); break;
			case 1: Out.push_back(u' '); break;
			case 2: Out.push_back(0xE9); break;
			case 3: Out.push_back(0x4E2D); break;
			case 4: Out += u"\xD83D\xDE00"; break;
			default: Out.push_back(static_cast<char16_t>(u'a' + R.Below(26))); break;
			}
		}
		return Out;
	}

	/** Moves an offset off the low half of a surrogate pair. */
	Offset OffPair(const std::u16string& Text, Offset At)
	{
		if (At > 0 && At < static_cast<Offset>(Text.size()) && dream::utf::IsLowSurrogate(Text[static_cast<size_t>(At)])
			&& dream::utf::IsHighSurrogate(Text[static_cast<size_t>(At - 1)]))
		{
			return At - 1;
		}
		return At;
	}

	/** The snapshot's text and line index against a scan of Expected. */
	bool SameAs(const TextSnapshot& S, const std::u16string& Expected)
	{
		if (S.Length() != static_cast<int64_t>(Expected.size()) || S.Text() != Expected)
		{
			return false;
		}
		LineNo Line = 0;
		Offset Start = 0;
		std::u16string Scratch;
		for (size_t I = 0; I <= Expected.size(); ++I)
		{
			if (I == Expected.size() || Expected[I] == u'\n')
			{
				if (S.LineStart(Line) != Start || S.LineEnd(Line) != static_cast<Offset>(I) || S.LineOf(Start) != Line
					|| S.LineView(Line, Scratch) != std::u16string_view(Expected).substr(static_cast<size_t>(Start), I - static_cast<size_t>(Start)))
				{
					return false;
				}
				++Line;
				Start = static_cast<Offset>(I + 1);
			}
		}
		return S.LineCount() == Line;
	}

	/** Applies ranges (any order, non-overlapping) to a string the way TextBuffer::Apply does. */
	void ApplyToString(std::u16string& Text, std::vector<EditRange> Ranges)
	{
		std::stable_sort(Ranges.begin(), Ranges.end(), [](const EditRange& A, const EditRange& B) { return A.At < B.At; });
		for (auto It = Ranges.rbegin(); It != Ranges.rend(); ++It)
		{
			Text.replace(static_cast<size_t>(It->At), static_cast<size_t>(It->DeleteLen), It->Insert);
		}
	}

	/** One random batch for the oracle runs: typing, deletes, pastes, multi-range and Replace-All batches. */
	EditBatch RandomBatch(Rng& R, const std::u16string& Text, Offset& Caret)
	{
		EditBatch B;
		const Offset Size = static_cast<Offset>(Text.size());
		const uint64_t Kind = R.Below(100);
		if (Kind < 60)
		{
			// Typing at a caret that mostly moves forward.
			if (R.OneIn(30) || Caret > Size)
			{
				Caret = static_cast<Offset>(R.Below(static_cast<uint64_t>(Size + 1)));
			}
			Caret = OffPair(Text, Caret);
			if (R.OneIn(8) && Caret > 0)
			{
				const Offset From = OffPair(Text, Caret - 1);
				B.Ranges.push_back(EditRange{From, Caret - From, u""});
				Caret = From;
			}
			else
			{
				const std::u16string Ins = R.OneIn(15) ? std::u16string(u"\n    ") : RandomText(R, 1);
				B.Ranges.push_back(EditRange{Caret, 0, Ins});
				Caret += static_cast<Offset>(Ins.size());
			}
			return B;
		}
		if (Kind < 75)
		{
			const Offset At = OffPair(Text, static_cast<Offset>(R.Below(static_cast<uint64_t>(Size + 1))));
			const Offset End = OffPair(Text, std::min<Offset>(Size, At + static_cast<Offset>(R.Below(200))));
			B.Ranges.push_back(EditRange{At, End - At, u""});
			return B;
		}
		if (Kind < 88)
		{
			const Offset At = OffPair(Text, static_cast<Offset>(R.Below(static_cast<uint64_t>(Size + 1))));
			B.Ranges.push_back(EditRange{At, 0, RandomText(R, 1 + R.Below(600))});
			return B;
		}
		if (Kind < 97)
		{
			// Several ranges in one batch (multi-caret, indent), in any order.
			Offset Cursor = 0;
			bool bTouch = false;   // the next range starts where this one ends (MapLine's adjacent ranges)
			const int Count = 2 + static_cast<int>(R.Below(4));
			for (int K = 0; K < Count && Cursor <= Size; ++K)
			{
				const Offset At = bTouch ? Cursor
					: OffPair(Text, Cursor + static_cast<Offset>(R.Below(static_cast<uint64_t>(std::min<Offset>(Size - Cursor, 400) + 1))));
				const Offset End = OffPair(Text, std::min<Offset>(Size, At + static_cast<Offset>(R.Below(6))));
				B.Ranges.push_back(EditRange{At, End - At, RandomText(R, R.Below(4))});
				bTouch = R.OneIn(3);
				Cursor = bTouch ? End : End + 1;
			}
			std::reverse(B.Ranges.begin(), B.Ranges.end());
			return B;
		}
		if (Kind < 99)
		{
			// Replace All: many ranges (the linear rebuild path).
			Offset Cursor = 0;
			const int Count = 20 + static_cast<int>(R.Below(60));
			for (int K = 0; K < Count && Cursor < Size; ++K)
			{
				const Offset At = OffPair(Text, Cursor + static_cast<Offset>(R.Below(static_cast<uint64_t>(std::min<Offset>(Size - Cursor, 120)))));
				const Offset End = OffPair(Text, std::min<Offset>(Size, At + 1 + static_cast<Offset>(R.Below(3))));
				B.Ranges.push_back(EditRange{At, End - At, RandomText(R, R.Below(5))});
				Cursor = End + 1;
			}
			return B;
		}
		// A chunk-sized paste (a chunk of its own) or a large delete.
		if (Size > 60000)
		{
			const Offset At = OffPair(Text, static_cast<Offset>(R.Below(static_cast<uint64_t>(Size / 2))));
			const Offset End = OffPair(Text, At + Size / 2);
			B.Ranges.push_back(EditRange{At, End - At, u""});
		}
		else
		{
			const Offset At = OffPair(Text, static_cast<Offset>(R.Below(static_cast<uint64_t>(Size + 1))));
			B.Ranges.push_back(EditRange{At, 0, RandomText(R, 33000 + R.Below(40000))});
		}
		return B;
	}

	void OracleRun(uint64_t Seed, int Steps)
	{
		Rng R(Seed);
		std::u16string Oracle = Lines(300, 50);
		TextBuffer B(Oracle);
		Offset Caret = 100;
		int Bad = 0;
		for (int Step = 0; Step < Steps && Bad < 3; ++Step)
		{
			const EditBatch Batch = RandomBatch(R, Oracle, Caret);
			if (!B.Validate(Batch))
			{
				continue;   // two inserts at one offset or a range that ended on a pair: the oracle skips it too
			}
			const ChangeSet C = B.Apply(Batch);
			ApplyToString(Oracle, Batch.Ranges);
			if (C.IsEmpty() && B.Length() != static_cast<int64_t>(Oracle.size()))
			{
				++Bad;
			}
			bool bOk = B.Length() == static_cast<int64_t>(Oracle.size());
			// Local check around every edit; the whole index at checkpoints.
			for (const dream::ChangeRange& G : C.Ranges)
			{
				const TextSnapshot S = B.Snapshot();
				(void)G;
				const Offset Probe = std::min<Offset>(static_cast<Offset>(Oracle.size()), static_cast<Offset>(R.Below(static_cast<uint64_t>(Oracle.size() + 1))));
				std::u16string Got;
				S.CopyRange(Probe - 40, 80, Got);
				const Offset From = std::max<Offset>(0, Probe - 40);
				bOk = bOk && Got == Oracle.substr(static_cast<size_t>(From), static_cast<size_t>(std::min<Offset>(Probe + 40, static_cast<Offset>(Oracle.size())) - From));
				const LineNo L = S.LineOf(Probe);
				bOk = bOk && S.LineStart(L) <= Probe && Probe <= S.LineEnd(L);
			}
			if (Step % 997 == 0 || Step + 1 == Steps)
			{
				bOk = bOk && SameAs(B.Snapshot(), Oracle);
			}
			if (!bOk)
			{
				std::fprintf(stderr, "  oracle mismatch at step %d (seed %llu)\n", Step, static_cast<unsigned long long>(Seed));
				++Bad;
			}
		}
		CHECK_EQ(Bad, 0);
		CHECK(SameAs(B.Snapshot(), Oracle));
		std::printf("  seed %llu: %d steps, %zu pieces, depth %d, %lld units\n", static_cast<unsigned long long>(Seed), Steps,
			B.Snapshot().PieceCount(), B.Snapshot().TreeDepth(), static_cast<long long>(B.Length()));
	}
}

TEST(PieceTree_RandomEditsMatchStringOracle)   // T-CORE-001 (20k per commit; DREAM_FULL_SCALE=1: 1M)
{
	OracleRun(1001, FullScale() ? 1000000 : 20000);
	OracleRun(77, FullScale() ? 100000 : 4000);
}

TEST(PieceTree_TypingCoalesces)   // T-CORE-001: 10,000 typed characters add <= 2 pieces
{
	std::u16string Oracle = Lines(1000, 60);
	TextBuffer B(Oracle);
	const size_t Before = B.Snapshot().PieceCount();
	Offset Caret = B.Snapshot().LineStart(500) + 3;
	for (int I = 0; I < 10000; ++I)
	{
		const std::u16string Ins(1, I % 40 == 39 ? u'\n' : static_cast<char16_t>(u'a' + I % 26));
		B.Apply(One(Caret, 0, Ins));
		Oracle.insert(static_cast<size_t>(Caret), Ins);
		++Caret;
	}
	CHECK(B.Snapshot().PieceCount() <= Before + 2);
	CHECK(SameAs(B.Snapshot(), Oracle));
	// The typed run reads back zero-copy line by line.
	std::u16string Scratch;
	const LineNo L = B.Snapshot().LineOf(Caret - 100);
	const std::u16string_view View = B.Snapshot().LineView(L, Scratch);
	CHECK(View.data() != Scratch.data() || Scratch.empty());
}

TEST(PieceTree_LineIndexAt100kLines)   // T-CORE-002: LineOf(LineStart(l)) == l on 100k lines, many pieces
{
	TextBuffer B(Lines(100000, 40));
	Rng R(5);
	for (int I = 0; I < 25000; ++I)
	{
		B.Apply(One(static_cast<Offset>(R.Below(static_cast<uint64_t>(B.Length()))), 0, u"x"));
	}
	const TextSnapshot S = B.Snapshot();
	CHECK(S.PieceCount() > 40000u);
	// Every lookup visits one node per level: depth + 1 <= 4 x depth (T-CORE-002) holds for any depth >= 1.
	CHECK(S.TreeDepth() >= 1);
	CHECK(S.TreeDepth() <= 4);
	int Bad = 0;
	for (LineNo L = 0; L < S.LineCount(); ++L)
	{
		const Offset Start = S.LineStart(L);
		Bad += (S.LineOf(Start) != L || S.LineOf(S.LineEnd(L)) != L || (L > 0 && S.LineEnd(L - 1) != Start - 1)) ? 1 : 0;
	}
	CHECK_EQ(Bad, 0);
	CHECK_EQ(S.LineCount(), 100001);
	std::printf("  %zu pieces, depth %d\n", S.PieceCount(), S.TreeDepth());
}

TEST(PieceTree_ReaderThreadHoldsSnapshot)   // T-CORE-003: the reader sees V only while the buffer takes 10k edits
{
	TextBuffer B(Lines(200, 40));
	// Typed text first, so V's pieces share the add chunk the edits below keep appending to.
	for (int I = 0; I < 500; ++I)
	{
		B.Apply(One(B.Snapshot().LineStart(100) + I, 0, u"t"));
	}
	const TextSnapshot V = B.Snapshot();
	const std::u16string Expected = V.Text();
	std::atomic<bool> bStop{false};
	std::atomic<int> Reads{0};
	std::atomic<int> Wrong{0};
	std::thread Reader([&] {
		std::u16string Scratch;
		while (!bStop.load(std::memory_order_acquire) || Reads.load() == 0)
		{
			if (V.Text() != Expected || V.LineCount() != 201 || V.LineView(100, Scratch).substr(0, 500) != std::u16string(500, u't'))
			{
				Wrong.fetch_add(1);
			}
			Reads.fetch_add(1);
		}
	});
	Rng R(3);
	std::u16string Oracle = Expected;
	Offset Caret = 0;
	TextSnapshot Mid;
	std::u16string MidText;
	for (int I = 0; I < 10000; ++I)
	{
		const EditBatch Batch = RandomBatch(R, Oracle, Caret);
		if (B.Validate(Batch))
		{
			B.Apply(Batch);
			ApplyToString(Oracle, Batch.Ranges);
		}
		if (I == 5000)
		{
			Mid = B.Snapshot();
			MidText = Oracle;
		}
	}
	bStop.store(true, std::memory_order_release);
	Reader.join();
	CHECK_EQ(Wrong.load(), 0);
	CHECK(Reads.load() > 0);
	CHECK(SameAs(V, Expected));
	CHECK(SameAs(Mid, MidText));
	CHECK(SameAs(B.Snapshot(), Oracle));
	CHECK_EQ(V.Version(), 501u);
}

TEST(PieceTree_CompactionVersionCheck)   // T-CORE-005
{
	TextBuffer B(Lines(100, 30));
	Rng R(9);
	for (int I = 0; I < 6000; ++I)
	{
		B.Apply(One(static_cast<Offset>(R.Below(static_cast<uint64_t>(B.Length()))), 0, u"y"));
	}
	std::printf("  %zu pieces, %d lines\n", B.Snapshot().PieceCount(), B.LineCount());
	CHECK(B.NeedsCompaction());
	const TextSnapshot S = B.Snapshot();
	const uint64_t V = S.Version();
	const TextSnapshot Built = TextBuffer::BuildCompacted(S);
	CHECK(Built.Text() == S.Text());
	CHECK_EQ(Built.Version(), V);
	CHECK_EQ(Built.PieceCount(), 1u);
	CHECK(SameAs(Built, S.Text()));

	// A stale build is refused and changes nothing.
	B.Apply(One(0, 0, u"#"));
	const std::u16string AfterEdit = B.Snapshot().Text();
	CHECK(!B.InstallCompacted(Built, V));
	CHECK(B.Snapshot().Text() == AfterEdit);
	// Only a BuildCompacted layout installs: the buffer's own many-piece snapshot is refused (review r1 minor).
	CHECK(!B.InstallCompacted(B.Snapshot(), B.Version()));

	// A fresh one installs: same text and version, one piece, and the buffer keeps working.
	const TextSnapshot S2 = B.Snapshot();
	CHECK(B.InstallCompacted(TextBuffer::BuildCompacted(S2), S2.Version()));
	CHECK_EQ(B.Version(), S2.Version());
	CHECK(SameAs(B.Snapshot(), AfterEdit));
	CHECK_EQ(B.Snapshot().PieceCount(), 1u);
	CHECK(!B.NeedsCompaction());
	CHECK(SameAs(S, S.Text()));   // older snapshots are untouched
	B.Apply(One(1, 0, u"typed"));
	std::u16string Oracle = AfterEdit;
	Oracle.insert(1, u"typed");
	CHECK(SameAs(B.Snapshot(), Oracle));
	CHECK_EQ(B.MapLine(V, 50), 50);   // the change log survives an install
	EditBatch Delta;
	CHECK(TextBuffer::ChangesBetween(S, B.Snapshot(), Delta));
	std::u16string Rebuilt = S.Text();
	ApplyToString(Rebuilt, Delta.Ranges);
	CHECK(Rebuilt == Oracle);

	// Another buffer's build at the same version is refused.
	TextBuffer Other(Oracle);
	while (Other.Version() < B.Version())
	{
		Other.Apply(One(0, 0, u"z"));
	}
	CHECK(!B.InstallCompacted(TextBuffer::BuildCompacted(Other.Snapshot()), B.Version()));
	CHECK(SameAs(B.Snapshot(), Oracle));
}

TEST(PieceTree_ChangeSetsAreMinimal)   // T-CORE-006: exact spans, never the whole document
{
	TextBuffer B(Lines(100000, 40));
	const ChangeSet One1 = B.Apply(One(B.Snapshot().LineStart(50000) + 2, 0, u"q"));
	REQUIRE_EQ(One1.Ranges.size(), 1u);
	CHECK_EQ(One1.Ranges[0].OldLen, 0);
	CHECK_EQ(One1.Ranges[0].NewLen, 1);
	CHECK_EQ(One1.Ranges[0].StartLine, 50000);
	CHECK_EQ(One1.Ranges[0].StartCol16, 2);
	CHECK_EQ(One1.Ranges[0].OldLines, 0);
	EditBatch Batch;
	for (int K = 0; K < 100; ++K)   // 100 ranges: the linear rebuild path
	{
		Batch.Ranges.push_back(EditRange{B.Snapshot().LineStart(K * 1000 + 7), 1, u"Z\n"});
	}
	const TextSnapshot Before = B.Snapshot();
	const ChangeSet Many = B.Apply(Batch);
	REQUIRE_EQ(Many.Ranges.size(), 100u);
	bool bExact = true;
	for (int K = 0; K < 100; ++K)
	{
		const dream::ChangeRange& G = Many.Ranges[static_cast<size_t>(K)];
		bExact = bExact && G.At == Before.LineStart(K * 1000 + 7) && G.OldLen == 1 && G.NewLen == 2 && G.StartLine == K * 1000 + 7
			&& G.StartCol16 == 0 && G.NewLines == 1 && G.OldLines == (Before.LineLength(K * 1000 + 7) == 0 ? 1 : 0);
	}
	CHECK(bExact);
	CHECK_EQ(B.LineCount(), 100101);
}

TEST(PieceTree_ChangesBetweenRebuildsTheNewerText)
{
	Rng R(21);
	std::u16string Oracle = Lines(200, 40);
	TextBuffer B(Oracle);
	Offset Caret = 0;
	int Bad = 0;
	for (int Round = 0; Round < 300; ++Round)
	{
		const TextSnapshot From = B.Snapshot();
		const int Steps = static_cast<int>(R.Below(25));
		for (int I = 0; I < Steps; ++I)
		{
			const EditBatch Batch = RandomBatch(R, Oracle, Caret);
			if (B.Validate(Batch))
			{
				B.Apply(Batch);
				ApplyToString(Oracle, Batch.Ranges);
			}
		}
		if (Oracle.size() > 50000)
		{
			B.Apply(One(0, static_cast<int64_t>(Oracle.size() / 2) - 1, u""));
			Oracle.erase(0, Oracle.size() / 2 - 1);
		}
		const TextSnapshot To = B.Snapshot();
		EditBatch Out;
		if (!TextBuffer::ChangesBetween(From, To, Out))
		{
			++Bad;
			continue;
		}
		TextBuffer Replay(From.Text());
		bool bOk = Replay.Validate(Out) && (Out.Ranges.empty() || !Replay.Apply(Out).IsEmpty());
		bOk = bOk && Replay.Snapshot().Text() == To.Text();
		for (size_t K = 1; K < Out.Ranges.size(); ++K)
		{
			bOk = bOk && Out.Ranges[K - 1].At + Out.Ranges[K - 1].DeleteLen < Out.Ranges[K].At;   // ascending, never touching
		}
		bOk = bOk && (From.Version() != To.Version() || Out.Ranges.empty());
		Bad += bOk ? 0 : 1;
	}
	CHECK_EQ(Bad, 0);

	// One typed character between two snapshots is one exact range.
	const TextSnapshot A = B.Snapshot();
	B.Apply(One(10, 0, u"k"));
	EditBatch Out;
	REQUIRE(TextBuffer::ChangesBetween(A, B.Snapshot(), Out));
	REQUIRE_EQ(Out.Ranges.size(), 1u);
	CHECK_EQ(Out.Ranges[0].At, 10);
	CHECK_EQ(Out.Ranges[0].DeleteLen, 0);
	CHECK(Out.Ranges[0].Insert == u"k");
	// Typing a character and deleting it again leaves nothing.
	const TextSnapshot A2 = B.Snapshot();
	B.Apply(One(20, 0, u"w"));
	B.Apply(One(20, 1, u""));
	REQUIRE(TextBuffer::ChangesBetween(A2, B.Snapshot(), Out));
	CHECK(Out.Ranges.empty());

	// Refusals: newer From, another buffer, an empty snapshot, the history horizon.
	CHECK(!TextBuffer::ChangesBetween(B.Snapshot(), A, Out));
	TextBuffer Other(B.Snapshot().Text());
	CHECK(!TextBuffer::ChangesBetween(Other.Snapshot(), B.Snapshot(), Out));
	CHECK(!TextBuffer::ChangesBetween(TextSnapshot(), B.Snapshot(), Out));
	B.SetHistoryLimit(5);
	const TextSnapshot Old = B.Snapshot();
	for (int I = 0; I < 10; ++I)
	{
		B.Apply(One(0, 0, u"h"));
	}
	CHECK(!TextBuffer::ChangesBetween(Old, B.Snapshot(), Out));
	CHECK(TextBuffer::ChangesBetween(B.Snapshot(), B.Snapshot(), Out));
	CHECK(Out.Ranges.empty());
}

TEST(PieceTree_ReleasesDeletedChunksAndMoves)
{
	TextBuffer B(u"abc\n");
	B.Apply(One(2, 0, std::u16string(2000000, u'x')));   // a chunk of its own
	CHECK(!B.NeedsCompaction());
	B.Apply(One(2, 2000000, u""));
	CHECK(!B.NeedsCompaction());   // the 2M-unit chunk left the table (else allocated > 2 x text + 1 Mi)
	for (int I = 0; I < 2000; ++I)   // 2M typed units fill 31 add chunks
	{
		B.Apply(One(B.Length(), 0, std::u16string(1000, static_cast<char16_t>(u'a' + I % 26))));
	}
	B.Apply(One(4, B.Length() - 4, u""));
	CHECK(!B.NeedsCompaction());   // filled add chunks are released once nothing names them
	CHECK(B.Snapshot().Text() == u"abc\n");

	// A pair split across pieces still converts whole; moving a buffer keeps its state.
	B.Apply(One(4, 0, u"\xD83D"));
	B.Apply(One(5, 0, u"\xDE00!"));
	CHECK_EQ(B.Snapshot().TextUtf8(), std::string("abc\n\xF0\x9F\x98\x80!"));
	TextBuffer Moved(std::move(B));
	Moved.Apply(One(0, 1, u"A"));
	CHECK(Moved.Snapshot().Text() == u"Abc\n\xD83D\xDE00!");
	std::u16string Spans;
	Moved.Snapshot().ForEachSpan(1, 100, [&Spans](std::u16string_view Run) { Spans += Run; });
	CHECK(Spans == u"bc\n\xD83D\xDE00!");
}

// ---- AnchorSet -------------------------------------------------------------------------------------------------

namespace
{
	/** Line-tracking oracle for one batch (as test_textbuffer.cpp's): new line of each old line, or -1 when deleted. */
	std::vector<LineNo> LineMap(const std::u16string& Before, const std::vector<EditRange>& Descending)
	{
		struct Unit
		{
			char16_t C;
			LineNo Tag;
		};
		std::vector<Unit> Units;
		LineNo Line = 0;
		for (char16_t C : Before)
		{
			Units.push_back(Unit{C, Line});
			Line += C == u'\n' ? 1 : 0;
		}
		Units.push_back(Unit{0, Line});
		for (const EditRange& R : Descending)
		{
			Units.erase(Units.begin() + static_cast<std::ptrdiff_t>(R.At), Units.begin() + static_cast<std::ptrdiff_t>(R.At + R.DeleteLen));
			std::vector<Unit> Ins;
			for (char16_t C : R.Insert)
			{
				Ins.push_back(Unit{C, -1});
			}
			Units.insert(Units.begin() + static_cast<std::ptrdiff_t>(R.At), Ins.begin(), Ins.end());
		}
		std::vector<LineNo> Map(static_cast<size_t>(Line) + 1, dream::InvalidLine);
		LineNo NewLine = 0;
		for (const Unit& U : Units)
		{
			if (U.Tag >= 0 && Map[static_cast<size_t>(U.Tag)] == dream::InvalidLine)
			{
				Map[static_cast<size_t>(U.Tag)] = NewLine;
			}
			NewLine += U.C == u'\n' ? 1 : 0;
		}
		return Map;
	}

	struct OracleAnchor
	{
		AnchorId Id = 0;
		Offset At = 0;
		AnchorKind Kind = AnchorKind::Point;
		AnchorGravity Gravity = AnchorGravity::Before;
		AnchorDeletePolicy Policy = AnchorDeletePolicy::Collapse;
		bool bLive = true;
	};

	/** A Point anchor through one range (A, D, I): the rule of §4.1.3 written out. Returns false when it is removed. */
	bool MapPoint(OracleAnchor& O, const EditRange& G)
	{
		const Offset A = G.At;
		const Offset D = G.DeleteLen;
		const Offset I = static_cast<Offset>(G.Insert.size());
		if (O.At < A)
		{
			return true;
		}
		if (O.At > A + D || (D > 0 && O.At == A + D))
		{
			O.At += I - D;
			return true;
		}
		if (O.At > A && O.Policy == AnchorDeletePolicy::Remove)
		{
			return false;
		}
		O.At = O.Gravity == AnchorGravity::After ? A + I : A;
		return true;
	}
}

TEST(AnchorSet_BasicsAndPolicies)
{
	TextBuffer B(u"alpha\nbeta\ngamma\n");
	dream::AnchorSet& S = B.Anchors();
	const AnchorId Before = S.Add(6, AnchorKind::Point, AnchorGravity::Before, AnchorDeletePolicy::Collapse, 1);
	const AnchorId After = S.Add(6, AnchorKind::Point, AnchorGravity::After, AnchorDeletePolicy::Collapse, 1);
	const AnchorId Inside = S.Add(8, AnchorKind::Point, AnchorGravity::Before, AnchorDeletePolicy::Remove, 2);
	const AnchorId Line = S.Add(11, AnchorKind::LineStart, AnchorGravity::Before, AnchorDeletePolicy::Remove, 3);
	const AnchorId Fold = S.Add(11, AnchorKind::LineStart, AnchorGravity::Before, AnchorDeletePolicy::Collapse, 4);
	CHECK_EQ(S.Size(), 5u);
	B.Apply(One(6, 0, u"XY"));   // insert at 6
	CHECK_EQ(S.Find(Before), 6);
	CHECK_EQ(S.Find(After), 8);
	CHECK_EQ(S.Find(Inside), 10);
	CHECK_EQ(S.Find(Line), 13);
	B.Apply(One(9, 2, u""));     // deletes "et" around Inside (now at 10): removed
	CHECK(!S.Contains(Inside));
	const std::vector<AnchorId> Gone = S.TakeRemoved();
	REQUIRE_EQ(Gone.size(), 1u);
	CHECK_EQ(Gone[0], Inside);
	CHECK(S.TakeRemoved().empty());
	CHECK_EQ(S.Find(Line), 11);   // "alpha\nXYba\ngamma\n": gamma starts at 11
	B.Apply(One(11, 6, u""));     // deletes the whole line "gamma\n"
	CHECK(!S.Contains(Line));
	CHECK_EQ(S.Find(Fold), 11);   // collapsed onto the line the text joined
	B.Apply(One(0, 0, u"new\n"));   // text inserted at a line's first column goes above it
	CHECK_EQ(S.Find(Fold), 15);
	CHECK_EQ(S.RemoveOwner(1), 2u);
	CHECK_EQ(S.Size(), 1u);
	CHECK(S.Remove(Fold));
	CHECK(!S.Remove(Fold));
	CHECK_EQ(S.Find(Fold), -1);
	const AnchorId Reused = S.Add(0, AnchorKind::Point, AnchorGravity::Before, AnchorDeletePolicy::Collapse);
	CHECK(Reused != Fold);   // a reused slot gets a new generation
	CHECK_EQ(S.Find(Fold), -1);
	CHECK_EQ(S.InRange(0, 1).size(), 1u);

	// MapLine's adjacent-range rule (review r1 minor): two touching ranges that together delete a line from its first
	// column through its '\n' delete it, though neither does alone; a LineStart anchor on it goes like MapLine's -1.
	TextBuffer T(u"aa\nbb\ncc\n");
	const uint64_t V = T.Version();
	const AnchorId Gone2 = T.Anchors().Add(3, AnchorKind::LineStart, AnchorGravity::Before, AnchorDeletePolicy::Remove);
	const AnchorId Kept2 = T.Anchors().Add(3, AnchorKind::LineStart, AnchorGravity::Before, AnchorDeletePolicy::Collapse);
	EditBatch Touching;
	Touching.Ranges.push_back(EditRange{2, 2, u""});   // "\nb": the '\n' above the line and its first unit
	Touching.Ranges.push_back(EditRange{4, 2, u""});   // "b\n": the rest of the line with its '\n'
	T.Apply(Touching);
	CHECK(T.Snapshot().Text() == u"aacc\n");
	CHECK_EQ(T.MapLine(V, 1), dream::InvalidLine);
	CHECK(!T.Anchors().Contains(Gone2));
	CHECK_EQ(T.Anchors().Find(Kept2), 0);   // collapsed onto the line the text joined
}

TEST(AnchorSet_MatchesOracle)   // T-CORE-008 shape (the M3 gate runs 10k anchors x 10k edits)
{
	Rng R(31);
	std::u16string Text = Lines(400, 30);
	TextBuffer B(Text);
	dream::AnchorSet& S = B.Anchors();
	std::vector<OracleAnchor> Oracle;
	for (int K = 0; K < 600; ++K)
	{
		OracleAnchor O;
		O.Kind = R.OneIn(2) ? AnchorKind::LineStart : AnchorKind::Point;
		O.Gravity = R.OneIn(2) ? AnchorGravity::After : AnchorGravity::Before;
		O.Policy = R.OneIn(2) ? AnchorDeletePolicy::Remove : AnchorDeletePolicy::Collapse;
		O.At = static_cast<Offset>(R.Below(Text.size() + 1));
		if (O.Kind == AnchorKind::LineStart)
		{
			O.At = B.Snapshot().LineStart(B.Snapshot().LineOf(O.At));
		}
		O.Id = S.Add(O.At, O.Kind, O.Gravity, O.Policy, static_cast<uint32_t>(K));
		Oracle.push_back(O);
	}
	Offset Caret = 0;
	int Bad = 0;
	for (int Step = 0; Step < 2000 && Bad < 5; ++Step)
	{
		EditBatch Batch = RandomBatch(R, Text, Caret);
		if (!B.Validate(Batch))
		{
			continue;
		}
		std::vector<EditRange> Desc = Batch.Ranges;
		std::stable_sort(Desc.begin(), Desc.end(), [](const EditRange& A, const EditRange& C) { return A.At < C.At; });
		std::reverse(Desc.begin(), Desc.end());
		const TextSnapshot Pre = B.Snapshot();
		const ChangeSet C = B.Apply(Batch);
		const std::vector<LineNo> Map = LineMap(Text, Desc);
		ApplyToString(Text, Batch.Ranges);
		const TextSnapshot Post = B.Snapshot();
		for (OracleAnchor& O : Oracle)
		{
			if (!O.bLive)
			{
				continue;
			}
			if (O.Kind == AnchorKind::Point)
			{
				for (const EditRange& G : Desc)
				{
					if (!MapPoint(O, G))
					{
						O.bLive = false;
						break;
					}
				}
				continue;
			}
			const LineNo NewLine = Map[static_cast<size_t>(Pre.LineOf(O.At))];
			if (NewLine != dream::InvalidLine)
			{
				O.At = Post.LineStart(NewLine);
			}
			else if (O.Policy == AnchorDeletePolicy::Remove)
			{
				O.bLive = false;
			}
			else
			{
				// Collapse: onto the line where the inserted text of the deleting range, and of the ranges touching it
				// after it, ends (where the line's first surviving character would have been).
				int64_t Delta = 0;
				for (size_t K = 0; K < C.Ranges.size(); ++K)
				{
					const dream::ChangeRange& G = C.Ranges[K];
					if (G.OldLen > 0 && O.At >= G.At && O.At < G.At + G.OldLen)
					{
						size_t J = K;
						while (J + 1 < C.Ranges.size() && C.Ranges[J + 1].At == C.Ranges[J].At + C.Ranges[J].OldLen)
						{
							Delta += C.Ranges[J].NewLen - C.Ranges[J].OldLen;
							++J;
						}
						O.At = Post.LineStart(Post.LineOf(C.Ranges[J].At + Delta + C.Ranges[J].NewLen));
						break;
					}
					Delta += G.NewLen - G.OldLen;
				}
			}
		}
		for (const OracleAnchor& O : Oracle)
		{
			const Offset Got = S.Find(O.Id);
			if ((O.bLive ? O.At : -1) != Got)
			{
				if (++Bad <= 5)
				{
					std::fprintf(stderr, "  step %d: anchor kind %d gravity %d policy %d: want %lld got %lld\n", Step, static_cast<int>(O.Kind),
						static_cast<int>(O.Gravity), static_cast<int>(O.Policy), static_cast<long long>(O.bLive ? O.At : -1), static_cast<long long>(Got));
					for (const EditRange& G : Desc)
					{
						std::fprintf(stderr, "    range %lld %lld %zu\n", static_cast<long long>(G.At), static_cast<long long>(G.DeleteLen), G.Insert.size());
					}
				}
			}
		}
		const std::vector<dream::Anchor> All = S.All();
		for (size_t K = 1; K < All.size(); ++K)
		{
			Bad += All[K - 1].At > All[K].At ? 1 : 0;
		}
		if (Text.size() > 40000)
		{
			Text = Lines(400, 30);
			B.SetText(Text, dream::EditOrigin::Reload);
			S.Clear();
			Oracle.clear();
			for (int K = 0; K < 600; ++K)
			{
				OracleAnchor O;
				O.Kind = R.OneIn(2) ? AnchorKind::LineStart : AnchorKind::Point;
				O.Gravity = R.OneIn(2) ? AnchorGravity::After : AnchorGravity::Before;
				O.Policy = R.OneIn(2) ? AnchorDeletePolicy::Remove : AnchorDeletePolicy::Collapse;
				O.At = static_cast<Offset>(R.Below(Text.size() + 1));
				if (O.Kind == AnchorKind::LineStart)
				{
					O.At = B.Snapshot().LineStart(B.Snapshot().LineOf(O.At));
				}
				O.Id = S.Add(O.At, O.Kind, O.Gravity, O.Policy, static_cast<uint32_t>(K));
				Oracle.push_back(O);
			}
		}
	}
	CHECK_EQ(Bad, 0);
	size_t Live = 0;
	for (const OracleAnchor& O : Oracle)
	{
		Live += O.bLive ? 1 : 0;
	}
	CHECK_EQ(S.Size(), Live);
	std::printf("  %zu of %zu anchors live after the edits\n", Live, Oracle.size());
}
