// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::TextBuffer (TDD §4.1): line index, snapshots, ChangeSets, validation, the string oracle (T-CORE-001
// shape), snapshot isolation (T-CORE-003 shape), column conversions (T-CORE-007) and MapLine (T-CORE-040).
#include "dream_test.h"
#include "test_util.h"

#include "dream/TextBuffer.h"

#include <algorithm>
#include <vector>

using dream::ChangeSet;
using dream::EditBatch;
using dream::EditRange;
using dream::LineNo;
using dream::TextBuffer;
using dream::TextSnapshot;
using dream_test_util::Rng;
using dream_test_util::U;

namespace
{
	EditBatch One(dream::Offset At, int64_t Del, std::u16string Ins)
	{
		EditBatch B;
		B.Ranges.push_back(EditRange{At, Del, std::move(Ins)});
		return B;
	}

	/** Checks the snapshot's line index against a scan of Expected. */
	void CheckIndex(const TextSnapshot& S, const std::u16string& Expected)
	{
		CHECK_EQ(S.Length(), static_cast<int64_t>(Expected.size()));
		CHECK(S.Text() == Expected);
		std::vector<dream::Offset> Starts{0};
		for (size_t I = 0; I < Expected.size(); ++I)
		{
			if (Expected[I] == u'\n')
			{
				Starts.push_back(static_cast<dream::Offset>(I + 1));
			}
		}
		REQUIRE_EQ(S.LineCount(), static_cast<LineNo>(Starts.size()));
		for (size_t L = 0; L < Starts.size(); ++L)
		{
			const LineNo Line = static_cast<LineNo>(L);
			CHECK_EQ(S.LineStart(Line), Starts[L]);
			CHECK_EQ(S.LineOf(Starts[L]), Line);
			const dream::Offset End = L + 1 < Starts.size() ? Starts[L + 1] - 1 : static_cast<dream::Offset>(Expected.size());
			CHECK_EQ(S.LineEnd(Line), End);
			std::u16string Scratch;
			CHECK(S.LineView(Line, Scratch) == std::u16string_view(Expected).substr(static_cast<size_t>(Starts[L]), static_cast<size_t>(End - Starts[L])));
		}
	}
}

TEST(TextBuffer_EmptyAndInitial)
{
	const TextSnapshot Empty;
	CHECK_EQ(Empty.Version(), 0u);
	CHECK_EQ(Empty.LineCount(), 1);
	CHECK_EQ(Empty.Length(), 0);
	CHECK_EQ(Empty.LineOf(5), 0);
	TextBuffer B(u"ab\ncd\n");
	CHECK_EQ(B.Version(), 1u);
	CHECK_EQ(B.LineCount(), 3);
	CheckIndex(B.Snapshot(), u"ab\ncd\n");
	CHECK_EQ(B.Snapshot().LineLength(2), 0);
	CHECK_EQ(B.Snapshot().TextUtf8(), std::string("ab\ncd\n"));
	std::u16string Out;
	B.Snapshot().CopyRange(1, 3, Out);
	CHECK(Out == u"b\nc");
	CHECK(B.Snapshot().Chunk(4, 100) == u"d\n");
	CHECK(B.Snapshot().Chunk(99, 2).empty());
}

TEST(TextBuffer_ApplyReportsChangeSets)
{
	TextBuffer B(u"line one\nline two\nline three");
	EditBatch Batch;
	Batch.Origin = dream::EditOrigin::Command;
	Batch.Ranges.push_back(EditRange{18, 4, u"LINE"});          // "line" of line three
	Batch.Ranges.push_back(EditRange{0, 0, u"# \n"});           // a new first line
	Batch.Ranges.push_back(EditRange{5, 3, u"1"});              // "one"
	const ChangeSet C = B.Apply(Batch);
	CHECK_EQ(C.VersionBefore, 1u);
	CHECK_EQ(C.VersionAfter, 2u);
	CHECK(C.Origin == dream::EditOrigin::Command);
	REQUIRE_EQ(C.Ranges.size(), 3u);
	CHECK_EQ(C.Ranges[0].At, 0);
	CHECK_EQ(C.Ranges[0].NewLines, 1);
	CHECK_EQ(C.Ranges[1].At, 5);
	CHECK_EQ(C.Ranges[1].OldLen, 3);
	CHECK_EQ(C.Ranges[1].NewLen, 1);
	CHECK_EQ(C.Ranges[2].StartLine, 2);
	CHECK_EQ(C.Ranges[2].StartCol16, 0);
	CheckIndex(B.Snapshot(), u"# \nline 1\nline two\nLINE three");
	const ChangeSet Multi = B.Apply(One(0, 10, u""));
	REQUIRE_EQ(Multi.Ranges.size(), 1u);
	CHECK_EQ(Multi.Ranges[0].OldLines, 2);
	CheckIndex(B.Snapshot(), u"line two\nLINE three");
}

TEST(TextBuffer_InvalidBatchesChangeNothing)
{
	TextBuffer B(U("a\xF0\x9F\x98\x80" "b\n"));
	const uint64_t V = B.Version();
	const EditBatch Bad[] = {
		One(-1, 0, u"x"), One(0, 99, u""), One(7, 0, u"x"), One(0, 0, u"a\rb"), One(2, 0, u"x"), One(1, 1, u""),
	};
	for (const EditBatch& E : Bad)
	{
		CHECK(!B.Validate(E));
		const ChangeSet C = B.Apply(E);
		CHECK(C.IsEmpty());
		CHECK_EQ(C.VersionAfter, V);
	}
	EditBatch Overlap;
	Overlap.Ranges.push_back(EditRange{0, 3, u""});
	Overlap.Ranges.push_back(EditRange{2, 1, u"y"});
	CHECK(!B.Validate(Overlap));
	CHECK(B.Apply(EditBatch()).IsEmpty());
	CHECK(B.Apply(One(1, 0, u"")).IsEmpty());   // a no-op range is an empty batch
	CHECK_EQ(B.Version(), V);
	CHECK(B.Apply(One(1, 2, u"")).Ranges.size() == 1);   // the whole pair is fine
	CHECK(B.Snapshot().Text() == u"ab\n");
}

TEST(TextBuffer_SnapshotsAreImmutable)
{
	TextBuffer B(u"base");
	const TextSnapshot S1 = B.Snapshot();
	const TextSnapshot S1Copy = S1;
	CHECK(S1.SameRepAs(S1Copy));
	for (int I = 0; I < 1000; ++I)
	{
		B.Apply(One(B.Length(), 0, u"x\n"));
	}
	CHECK(S1.Text() == u"base");
	CHECK_EQ(S1.Version(), 1u);
	CHECK_EQ(S1.LineCount(), 1);
	CHECK_EQ(B.LineCount(), 1001);
	CHECK(!S1.SameRepAs(B.Snapshot()));
}

TEST(TextBuffer_RandomEditsMatchStringOracle)
{
	Rng R(42);
	std::u16string Oracle = u"start\n";
	TextBuffer B(Oracle);
	const char16_t Alphabet[] = {u'a', u'b', u' ', u'\n', u'\n', u'(', u'"', 0xE9, 0x4E2D};
	for (int Step = 0; Step < 5000; ++Step)
	{
		EditBatch Batch;
		const int Ranges = 1 + static_cast<int>(R.Below(3));
		int64_t Cursor = 0;
		std::vector<EditRange> Picked;
		for (int K = 0; K < Ranges; ++K)
		{
			const int64_t Size = static_cast<int64_t>(Oracle.size());
			if (Cursor > Size)
			{
				break;
			}
			const int64_t At = Cursor + static_cast<int64_t>(R.Below(static_cast<uint64_t>(Size - Cursor + 1)));
			const int64_t Del = std::min<int64_t>(static_cast<int64_t>(R.Below(4)), Size - At);
			std::u16string Ins;
			const int Len = static_cast<int>(R.Below(5));
			for (int L = 0; L < Len; ++L)
			{
				Ins.push_back(Alphabet[R.Below(sizeof(Alphabet) / sizeof(Alphabet[0]))]);
			}
			Picked.push_back(EditRange{At, Del, Ins});
			Cursor = At + Del + 1;
		}
		std::u16string Expected = Oracle;
		for (auto It = Picked.rbegin(); It != Picked.rend(); ++It)
		{
			Expected.replace(static_cast<size_t>(It->At), static_cast<size_t>(It->DeleteLen), It->Insert);
		}
		std::reverse(Picked.begin(), Picked.end());   // any order is accepted
		Batch.Ranges = Picked;
		B.Apply(Batch);
		Oracle = Expected;
		if (Step % 250 == 0)
		{
			CheckIndex(B.Snapshot(), Oracle);
		}
	}
	CheckIndex(B.Snapshot(), Oracle);
}

TEST(TextBuffer_ColumnConversions)
{
	TextBuffer B(U("x\n\xC3\xA9 = 1; y = 2\n\xF0\x9F\x98\x80z"));
	CHECK_EQ(B.Utf8ColToUtf16(1, 8), 7);
	CHECK_EQ(B.Utf16ColToUtf8(1, 7), 8);
	CHECK_EQ(B.CodepointColToUtf16(2, 1), 2);
	CHECK_EQ(B.Utf8ColToUtf16(2, 4), 2);
	CHECK_EQ(B.Utf8ColToUtf16(0, 50), 1);
}

namespace
{
	/**
	 * Line-tracking oracle for MapLine, one edit batch at a time: every character of the text before the batch carries
	 * its line (the end anchor carries the last line and is never deleted); after the batch, a line maps to the line of
	 * its first surviving character, or is deleted when none survives. A recorded line's map is the composition of the
	 * per-batch maps.
	 */
	std::vector<LineNo> StepMap(const std::u16string& Before, const std::vector<EditRange>& Descending)
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
			if (C == u'\n')
			{
				++Line;
			}
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
			if (U.C == u'\n')
			{
				++NewLine;
			}
		}
		return Map;
	}
}

TEST(TextBuffer_MapLineMatchesLineTrackingOracle)
{
	Rng R(7);
	for (int Round = 0; Round < 20; ++Round)
	{
		std::u16string Text;
		for (int L = 0; L < 30; ++L)
		{
			Text += (L % 4 == 0) ? std::u16string() : u"line" + std::u16string(1, static_cast<char16_t>(u'a' + L % 26));
			Text += u'\n';
		}
		TextBuffer B(Text);
		const uint64_t Recorded = B.Version();
		const LineNo RecordedLines = B.LineCount();
		std::vector<LineNo> Oracle(static_cast<size_t>(RecordedLines));
		for (LineNo L = 0; L < RecordedLines; ++L)
		{
			Oracle[static_cast<size_t>(L)] = L;
		}
		std::u16string Current = Text;
		for (int Step = 0; Step < 50; ++Step)
		{
			std::vector<EditRange> Picked;
			int64_t Cursor = 0;
			const int Ranges = 1 + static_cast<int>(R.Below(3));
			for (int K = 0; K < Ranges; ++K)
			{
				const int64_t Size = static_cast<int64_t>(Current.size());
				if (Cursor > Size)
				{
					break;
				}
				const int64_t At = Cursor + static_cast<int64_t>(R.Below(static_cast<uint64_t>(std::min<int64_t>(Size - Cursor, 12) + 1)));
				const int64_t Del = std::min<int64_t>(static_cast<int64_t>(R.Below(9)), Size - At);
				std::u16string Ins;
				const int Len = static_cast<int>(R.Below(4));
				for (int L = 0; L < Len; ++L)
				{
					Ins.push_back(R.OneIn(3) ? u'\n' : u'q');
				}
				Picked.push_back(EditRange{At, Del, Ins});
				Cursor = At + Del + static_cast<int64_t>(R.Below(2));   // adjacent ranges too
			}
			std::vector<EditRange> Descending(Picked.rbegin(), Picked.rend());
			EditBatch Batch;
			Batch.Ranges = Picked;
			if (!B.Validate(Batch))
			{
				continue;   // two inserts at one offset: skip
			}
			B.Apply(Batch);
			const std::vector<LineNo> Step1 = StepMap(Current, Descending);
			for (LineNo& M : Oracle)
			{
				M = M == dream::InvalidLine ? dream::InvalidLine : Step1[static_cast<size_t>(M)];
			}
			for (const EditRange& E : Descending)
			{
				Current.replace(static_cast<size_t>(E.At), static_cast<size_t>(E.DeleteLen), E.Insert);
			}
			for (LineNo L = 0; L < RecordedLines; ++L)
			{
				CHECK_EQ(B.MapLine(Recorded, L), Oracle[static_cast<size_t>(L)]);
			}
		}
	}
}

TEST(TextBuffer_MapLineSimpleCasesAndHorizon)
{
	TextBuffer B(u"a\nb\nc\nd\n");
	const uint64_t V1 = B.Version();
	B.Apply(One(0, 0, u"new\n"));          // insert above line 0
	CHECK_EQ(B.MapLine(V1, 0), 1);
	CHECK_EQ(B.MapLine(V1, 3), 4);
	const uint64_t V2 = B.Version();
	B.Apply(One(4, 2, u""));               // delete line "a\n" (now line 1)
	CHECK_EQ(B.MapLine(V2, 1), dream::InvalidLine);
	CHECK_EQ(B.MapLine(V1, 0), dream::InvalidLine);
	CHECK_EQ(B.MapLine(V1, 1), 1);
	CHECK_EQ(B.MapLine(B.Version(), 2), 2);
	CHECK_EQ(B.MapLine(B.Version() + 5, 0), dream::InvalidLine);
	B.SetHistoryLimit(1);
	CHECK_EQ(B.OldestMappableVersion(), V2);
	CHECK_EQ(B.MapLine(V1, 1), dream::InvalidLine);   // older than the kept ChangeSets
	CHECK_EQ(B.MapLine(V2, 2), 1);
}
