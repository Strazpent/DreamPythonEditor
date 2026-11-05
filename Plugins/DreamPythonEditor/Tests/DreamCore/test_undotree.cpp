// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::UndoTree (TDD §4.2, EDT-08): the round trip with random grouping and branches (T-CORE-010, as the
// deterministic tier-1 form of fuzz_undo_roundtrip), rules R1-R9 (T-CORE-011 shape), branches (T-CORE-012 shape), the
// dirty flag (T-CORE-013), the cap (T-CORE-014), run boundaries and the recovery group (T-CORE-015), the IME group
// (T-CORE-016), the version-gap reset and the process cap. DREAM_FULL_SCALE=1 runs the round trip at 1M edits.
#define _CRT_SECURE_NO_WARNINGS   // std::getenv
#include "dream_test.h"
#include "test_util.h"

#include "dream/Diff.h"
#include "dream/PyLexer.h"
#include "dream/UndoTree.h"

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using dream::CaretRange;
using dream::ChangeSet;
using dream::EditBatch;
using dream::EditRange;
using dream::Offset;
using dream::TextBuffer;
using dream::TextSnapshot;
using dream::UndoKind;
using dream::UndoNodeId;
using dream::UndoStatus;
using dream::UndoStep;
using dream::UndoTree;
using dream_test_util::Rng;

namespace
{
	/** FNV-1a over the units plus the length: what the round trip keeps per node (1M nodes of text would not fit). */
	uint64_t Fingerprint(const std::u16string& Text)
	{
		uint64_t H = 0xcbf29ce484222325ull;
		for (char16_t C : Text)
		{
			H = (H ^ static_cast<uint64_t>(C)) * 0x100000001b3ull;
		}
		return H ^ (static_cast<uint64_t>(Text.size()) << 40);
	}

	bool FullScale()
	{
		const char* V = std::getenv("DREAM_FULL_SCALE");
		return V != nullptr && *V != '\0' && *V != '0';
	}

	/** A buffer with its undo tree and incremental lexer, applying edits the way plain mode does. */
	struct Doc
	{
		TextBuffer Buffer;
		UndoTree Tree;
		dream::PyLexer Lexer;
		double Now = 100.0;

		explicit Doc(std::u16string_view Initial)
			: Buffer(Initial)
			, Tree(Buffer.Version())
		{
			Lexer.Reset(Buffer.Snapshot());
			Lexer.Relex(Buffer.Snapshot(), 0.0, 0, 0);
		}

		/** The lexer follows every change with a small budget, so its frontier is often behind (idle slicing). */
		void Lex(const ChangeSet& C)
		{
			Lexer.OnChange(C, Buffer.Snapshot());
			Lexer.Relex(Buffer.Snapshot(), 0.002, 0, 0);
		}

		/** The incremental tokens, once relexed through, equal a full lex (T-CORE-010 "text and tokens"). */
		bool TokensMatchFullLex()
		{
			Lexer.RelexThrough(Buffer.Snapshot(), Buffer.LineCount() - 1);
			std::vector<std::vector<uint32_t>> Full;
			dream::PyLexer::LexFull(Buffer.Snapshot(), dream::LexGrammar::Py311, Full);
			if (Full.size() != static_cast<size_t>(Lexer.LineCount()))
			{
				return false;
			}
			for (dream::LineNo L = 0; L < Lexer.LineCount(); ++L)
			{
				const auto T = Lexer.Tokens(L);
				if (std::vector<uint32_t>(T.begin(), T.end()) != Full[static_cast<size_t>(L)])
				{
					return false;
				}
			}
			return true;
		}

		std::u16string Text() const { return Buffer.Snapshot().Text(); }

		ChangeSet Edit(Offset At, int64_t Del, std::u16string Ins, UndoKind Kind, double Advance = 0.1)
		{
			EditBatch B;
			B.Ranges.push_back(EditRange{At, Del, std::move(Ins)});
			return EditBatch_(B, Kind, Advance);
		}

		ChangeSet EditBatch_(const EditBatch& B, UndoKind Kind, double Advance = 0.1)
		{
			Now += Advance;
			const TextSnapshot Before = Buffer.Snapshot();
			const ChangeSet C = Buffer.Apply(B);
			Tree.Record(C, Before, Buffer.Snapshot(), Kind, Now);
			Lex(C);
			return C;
		}

		void Type(std::u16string_view Chars, double Gap = 0.1)
		{
			for (char16_t C : Chars)
			{
				Edit(Buffer.Length(), 0, std::u16string(1, C), UndoKind::Typing, Gap);
			}
		}

		void Backspace(int Count)
		{
			for (int I = 0; I < Count; ++I)
			{
				Edit(Buffer.Length() - 1, 1, u"", UndoKind::Delete);
			}
		}

		UndoStep Undo()
		{
			UndoStep S = Tree.Undo(Buffer.Snapshot());
			if (S.Status == UndoStatus::Applied)
			{
				const TextSnapshot Before = Buffer.Snapshot();
				const ChangeSet C = Buffer.Apply(S.Batch);
				CHECK(!C.IsEmpty());
				Tree.Record(C, Before, Buffer.Snapshot(), UndoKind::Typing, Now);   // an Undo origin only re-syncs
				Lex(C);
			}
			return S;
		}

		UndoStep Redo()
		{
			UndoStep S = Tree.Redo(Buffer.Snapshot());
			if (S.Status == UndoStatus::Applied)
			{
				const ChangeSet C = Buffer.Apply(S.Batch);
				CHECK(!C.IsEmpty());
				Lex(C);
			}
			return S;
		}

		int UndoAll()
		{
			int Steps = 0;
			while (Undo().Status == UndoStatus::Applied)
			{
				++Steps;
			}
			return Steps;
		}
	};

	std::u16string RandomInsert(Rng& R)
	{
		static const char16_t* const Pieces[] = {u"a", u"b", u" ", u"\n", u"xy", u"\xE9", u"\xD83D\xDE00", u"(", u"):\n    ", u"\t"};
		std::u16string S;
		const int N = 1 + static_cast<int>(R.Below(3));
		for (int I = 0; I < N; ++I)
		{
			S += Pieces[R.Below(sizeof(Pieces) / sizeof(Pieces[0]))];
		}
		return S;
	}

	/** A random valid offset that does not split a surrogate pair. */
	Offset SafeOffset(Rng& R, const TextSnapshot& S)
	{
		Offset At = static_cast<Offset>(R.Below(static_cast<uint64_t>(S.Length() + 1)));
		std::u16string Two;
		if (At > 0 && At < S.Length())
		{
			S.CopyRange(At - 1, 2, Two);
			if (Two.size() == 2 && dream::utf::IsHighSurrogate(Two[0]) && dream::utf::IsLowSurrogate(Two[1]))
			{
				--At;
			}
		}
		return At;
	}

	int64_t SafeLength(Rng& R, const TextSnapshot& S, Offset At)
	{
		int64_t Len = std::min<int64_t>(static_cast<int64_t>(R.Below(5)), S.Length() - At);
		std::u16string Two;
		if (Len > 0 && At + Len < S.Length())
		{
			S.CopyRange(At + Len - 1, 2, Two);
			if (Two.size() == 2 && dream::utf::IsHighSurrogate(Two[0]) && dream::utf::IsLowSurrogate(Two[1]))
			{
				++Len;
			}
		}
		return Len;
	}

	/** fuzz_undo_roundtrip: random edits, kinds, gaps, groups, undos and redos against the text of every node. */
	void RoundTrip(uint64_t Seed, int Steps, bool bSmallCap)
	{
		Rng R(Seed);
		const std::u16string Initial = u"def f():\n    return 1\n";
		Doc D(Initial);
		if (bSmallCap)
		{
			dream::UndoLimits L;
			L.MinBytes = 64 * 1024;
			L.KeepGroups = 50;
			D.Tree.SetLimits(L);
		}
		std::map<UndoNodeId, uint64_t> StateOf;
		StateOf[D.Tree.RootNode()] = Fingerprint(Initial);
		int Bad = 0;
		int Groups = 0;
		for (int Step = 0; Step < Steps && Bad < 3; ++Step)
		{
			const uint64_t Action = R.Below(20);
			bool bMoved = false;   // an undo or redo: the node reached must hold the text it had when it was last current
			if (Action < 3 && D.Tree.CanUndo())
			{
				D.Undo();
				bMoved = true;
			}
			else if (Action < 5 && D.Tree.CanRedo())
			{
				if (R.OneIn(4) && D.Tree.Branches().size() > 1)
				{
					const std::vector<UndoNodeId> B = D.Tree.Branches();
					CHECK(D.Tree.SelectBranch(B[R.Below(B.size())]));
				}
				D.Redo();
				bMoved = true;
			}
			else if (Action == 5)
			{
				D.Tree.CloseGroup();
			}
			else if (Action == 6 && !D.Tree.IsInExplicitGroup())
			{
				D.Tree.BeginGroup(UndoKind::Command, u"group");
				++Groups;
			}
			else if (Action == 7 && D.Tree.IsInExplicitGroup())
			{
				D.Tree.EndGroup();
			}
			else
			{
				const TextSnapshot S = D.Buffer.Snapshot();
				EditBatch B;
				const int Ranges = R.OneIn(6) ? 2 + static_cast<int>(R.Below(3)) : 1;
				Offset Floor = 0;
				for (int K = 0; K < Ranges; ++K)
				{
					Offset At = SafeOffset(R, S);
					if (At < Floor)
					{
						continue;
					}
					// Keep the text near 3,000 units so every step stays cheap: long texts get deletions.
					const bool bShrink = S.Length() > 3000;
					int64_t Del = R.OneIn(2) || bShrink ? SafeLength(R, S, At) : 0;
					if (bShrink)
					{
						At = std::min<Offset>(At, S.Length() - 300);
						std::u16string Probe;
						S.CopyRange(At, 260, Probe);
						Del = 200;
						while (Del < static_cast<int64_t>(Probe.size()) && dream::utf::IsLowSurrogate(Probe[static_cast<size_t>(Del)]))
						{
							++Del;
						}
						if (At < Floor || (At > 0 && dream::utf::IsLowSurrogate(Probe[0])))
						{
							continue;
						}
					}
					std::u16string Ins = bShrink || (R.OneIn(3) && Del > 0) ? std::u16string() : RandomInsert(R);
					B.Ranges.push_back(EditRange{At, Del, std::move(Ins)});
					Floor = At + Del + 1;
				}
				const UndoKind Kinds[] = {UndoKind::Typing, UndoKind::Typing, UndoKind::Delete, UndoKind::Command, UndoKind::Ime};
				const UndoKind Kind = Kinds[R.Below(5)];
				const double Gap = R.OneIn(8) ? 2.0 : 0.05;
				D.EditBatch_(B, Kind, Gap);
			}
			const uint64_t Print = Fingerprint(D.Text());
			const auto It = StateOf.find(D.Tree.CurrentNode());
			if (bMoved && (It == StateOf.end() || It->second != Print))
			{
				++Bad;
				std::fprintf(stderr, "  step %d: node %u text differs\n", Step, D.Tree.CurrentNode());
			}
			StateOf[D.Tree.CurrentNode()] = Print;
			if (Step % 4999 == 4998 && !D.TokensMatchFullLex())
			{
				++Bad;
				std::fprintf(stderr, "  step %d: incremental tokens differ from a full lex\n", Step);
			}
			// Every step's node text must hold for nodes reached by undo and redo: checked when they are reached.
			if (Action < 5)
			{
				CHECK(StateOf.count(D.Tree.CurrentNode()) == 1);
			}
			CHECK(!D.Tree.WasReset());
			if (bSmallCap)
			{
				CHECK(D.Tree.RetainedBytes() <= D.Tree.CapBytes() + 4096 * 3 || D.Tree.GroupCount() <= 1);
			}
		}
		if (D.Tree.IsInExplicitGroup())
		{
			D.Tree.EndGroup();
		}
		// Undo all gives the root's text (the initial text unless trimming moved the root); redo all the newest path.
		const std::u16string Final = D.Text();
		const UndoNodeId Last = D.Tree.CurrentNode();
		CHECK(D.TokensMatchFullLex());
		D.UndoAll();
		CHECK(Fingerprint(D.Text()) == StateOf[D.Tree.RootNode()]);
		CHECK(D.TokensMatchFullLex());
		if (!D.Tree.WasTrimmed())
		{
			CHECK(D.Text() == Initial);
		}
		while (D.Redo().Status == UndoStatus::Applied)
		{
			CHECK(Fingerprint(D.Text()) == StateOf[D.Tree.CurrentNode()]);
		}
		(void)Last;
		(void)Final;
		// The incremental lexer followed every edit, undo and redo with a stale frontier; its tokens of the round-tripped
		// text equal a full lex (T-CORE-010 "text and tokens"; review r1: the check compared two full lexes before).
		CHECK(D.TokensMatchFullLex());
		CHECK_EQ(Bad, 0);
		std::printf("  seed %llu: %d steps, %zu groups kept, %zu bytes retained, %d explicit groups\n",
			static_cast<unsigned long long>(Seed), Steps, D.Tree.GroupCount(), D.Tree.RetainedBytes(), Groups);
	}
}

TEST(UndoTree_RoundTripFuzz)   // T-CORE-010, fuzz_undo_roundtrip (100k per commit; DREAM_FULL_SCALE=1: 1M)
{
	const int Steps = FullScale() ? 1000000 : 100000;
	RoundTrip(10, Steps, false);
	RoundTrip(11, Steps / 4, true);
}

TEST(UndoTree_TypingCoalescing)   // R1, R2, R3, R4
{
	Doc D(u"");
	D.Type(u"foo bar");
	D.Type(u"\n");
	D.Type(u"baz");
	CHECK(D.Text() == u"foo bar\nbaz");
	D.Undo();
	CHECK(D.Text() == u"foo bar\n");       // "baz"
	D.Undo();
	CHECK(D.Text() == u"foo bar");         // the newline is its own group (R3)
	D.Undo();
	CHECK(D.Text() == u"foo");             // " bar": whitespace after a word closed it (R2)
	D.Undo();
	CHECK(D.Text() == u"");
	CHECK(!D.Tree.CanUndo());
	D.Redo();
	D.Redo();
	CHECK(D.Text() == u"foo bar");

	// R1: a pause longer than 1 s splits a burst.
	Doc P(u"");
	P.Type(u"ab");
	P.Type(u"cd", 1.5);
	P.Undo();
	CHECK(P.Text() == u"abc");             // 'd' came 1.5 s after 'c'
	// R4: a kind change and an explicit close split; a non-contiguous insert splits.
	Doc K(u"hello");
	K.Backspace(2);
	K.Type(u"p!");
	CHECK(K.Text() == u"help!");
	K.Undo();
	CHECK(K.Text() == u"hel");
	K.Undo();
	CHECK(K.Text() == u"hello");           // both backspaces were one burst
	Doc C(u"");
	C.Type(u"ab");
	C.Tree.CloseGroup();                   // the caret was moved by a click
	C.Type(u"cd");
	C.Undo();
	CHECK(C.Text() == u"ab");
	C.Edit(0, 0, u"x", UndoKind::Typing);  // typing elsewhere
	C.Edit(2, 0, u"y", UndoKind::Typing);  // not where the burst left off
	C.Undo();
	CHECK(C.Text() == u"xab");
	// Forward Delete bursts.
	Doc F(u"abcdef");
	F.Edit(1, 1, u"", UndoKind::Delete);
	F.Edit(1, 1, u"", UndoKind::Delete);
	F.Edit(1, 1, u"", UndoKind::Delete);
	CHECK(F.Text() == u"aef");
	F.Undo();
	CHECK(F.Text() == u"abcdef");
	CHECK_EQ(F.Tree.GroupCount(), 1u);
}

TEST(UndoTree_CommandsAndExplicitGroups)   // R5, R7, labels
{
	Doc D(u"line\n");
	D.Edit(5, 0, u"paste1", UndoKind::Command);
	D.Edit(11, 0, u"paste2", UndoKind::Command);
	D.Undo();
	CHECK(D.Text() == u"line\npaste1");    // each command is one group
	// An explicit group over several batches, with several ranges, is one step with its label.
	D.Tree.BeginGroup(UndoKind::Reload, u"Reloaded from disk");
	D.Edit(0, 4, u"LINE", UndoKind::Command);
	EditBatch Two;
	Two.Ranges.push_back(EditRange{0, 0, u"# "});
	Two.Ranges.push_back(EditRange{4, 0, u"!"});
	D.EditBatch_(Two, UndoKind::Command);
	D.Tree.BeginGroup(UndoKind::Command, u"inner");   // nests: only the outermost counts
	D.Edit(0, 0, u">", UndoKind::Typing);
	D.Tree.EndGroup();
	D.Tree.EndGroup();
	CHECK(D.Text() == u"># LINE!\npaste1");
	const UndoStep S = D.Undo();
	CHECK(S.Status == UndoStatus::Applied);
	CHECK(S.Label == u"Reloaded from disk");
	CHECK(S.Kind == UndoKind::Reload);
	CHECK_EQ(S.Batch.Origin, dream::EditOrigin::Undo);
	CHECK(D.Text() == u"line\npaste1");
	const UndoStep R = D.Redo();
	CHECK(R.Label == u"Reloaded from disk");
	CHECK(D.Text() == u"># LINE!\npaste1");
	// An empty explicit group makes no step; a group whose edits cancel out is dropped.
	const size_t Groups = D.Tree.GroupCount();
	D.Tree.BeginGroup(UndoKind::Command, u"nothing");
	D.Tree.EndGroup();
	D.Tree.BeginGroup(UndoKind::Command, u"cancels");
	D.Edit(0, 0, u"zz", UndoKind::Command);
	D.Edit(0, 2, u"", UndoKind::Command);
	D.Tree.EndGroup();
	CHECK_EQ(D.Tree.GroupCount(), Groups);
	D.Undo();
	CHECK(D.Text() == u"line\npaste1");
	// Multi-range typing (R7) is one group even without an explicit group.
	Doc M(u"a\nb\nc");
	EditBatch Carets;
	for (Offset At : {0, 2, 4})
	{
		Carets.Ranges.push_back(EditRange{At, 0, u"x"});
	}
	M.EditBatch_(Carets, UndoKind::Typing);
	M.Type(u"y");
	M.Undo();
	CHECK(M.Text() == u"xa\nxb\nxc");
	M.Undo();
	CHECK(M.Text() == u"a\nb\nc");
}

TEST(UndoTree_BranchesAndCarets)   // T-CORE-012 shape
{
	Doc D(u"base");
	D.Edit(4, 0, u" one", UndoKind::Command);
	D.Undo();
	D.Edit(4, 0, u" two", UndoKind::Command);   // an edit after an undo makes a branch; nothing is lost
	CHECK(D.Text() == u"base two");
	D.Undo();
	CHECK_EQ(D.Tree.Branches().size(), 2u);
	D.Redo();
	CHECK(D.Text() == u"base two");              // redo follows the newest branch
	D.Undo();
	CHECK(D.Tree.SelectBranch(D.Tree.Branches()[0]));
	D.Redo();
	CHECK(D.Text() == u"base one");
	CHECK(!D.Tree.SelectBranch(12345));
	// Carets recorded with the group come back: before on undo, after on redo.
	Doc C(u"abc");
	const TextSnapshot Before = C.Buffer.Snapshot();
	EditBatch B;
	B.Ranges.push_back(EditRange{1, 1, u"XYZ"});
	const ChangeSet Changes = C.Buffer.Apply(B);
	const CaretRange CB[] = {CaretRange{1, 2}, CaretRange{0, 0}};
	const CaretRange CA[] = {CaretRange{4, 4}};
	C.Tree.Record(Changes, Before, C.Buffer.Snapshot(), UndoKind::Command, 1.0, CB, CA);
	UndoStep U = C.Undo();
	REQUIRE_EQ(U.Carets.size(), 2u);
	CHECK(U.Carets[0] == (CaretRange{1, 2}));
	UndoStep R = C.Redo();
	REQUIRE_EQ(R.Carets.size(), 1u);
	CHECK(R.Carets[0] == (CaretRange{4, 4}));
	// Without recorded carets: the end of the restored text.
	Doc N(u"hello world");
	N.Edit(6, 5, u"there", UndoKind::Command);
	U = N.Undo();
	REQUIRE_EQ(U.Carets.size(), 1u);
	CHECK_EQ(U.Carets[0].Active, 11);
	R = N.Redo();
	CHECK_EQ(R.Carets[0].Active, 11);
}

TEST(UndoTree_DirtyFlagAcrossSaveAndUndo)   // T-CORE-013
{
	Doc D(u"x");
	CHECK(D.Tree.IsAtSavePoint());
	D.Type(u"abc");
	CHECK(!D.Tree.IsAtSavePoint());
	D.Tree.MarkSavePoint();                    // R9: closes the group
	CHECK(D.Tree.IsAtSavePoint());
	D.Type(u"d");
	CHECK(!D.Tree.IsAtSavePoint());
	D.Undo();
	CHECK(D.Tree.IsAtSavePoint());             // undo back to the save node: clean
	CHECK(D.Text() == u"xabc");
	D.Undo();
	CHECK(!D.Tree.IsAtSavePoint());
	D.Redo();
	CHECK(D.Tree.IsAtSavePoint());
	D.Redo();
	CHECK(!D.Tree.IsAtSavePoint());
	D.Tree.ClearSavePoint();
	D.Undo();
	CHECK(!D.Tree.IsAtSavePoint());
	// A trimmed save node: any edit leaves the buffer dirty, even undoing back to the same text.
	Doc P(u"");
	dream::UndoLimits L;
	L.MinBytes = 4096;
	L.KeepGroups = 2;
	P.Tree.SetLimits(L);
	P.Tree.MarkSavePoint();
	for (int I = 0; I < 40; ++I)
	{
		P.Edit(0, P.Buffer.Length(), std::u16string(100, static_cast<char16_t>(u'a' + I % 26)), UndoKind::Command);
	}
	CHECK(P.Tree.WasTrimmed());
	P.UndoAll();
	CHECK(!P.Tree.IsAtSavePoint());
}

TEST(UndoTree_CapWithLargePastes)   // T-CORE-014: 2,000 x 1 MB pastes
{
	Doc D(u"");
	const int Pastes = 2000;
	const size_t Units = size_t(512) * 1024;   // 1 MB of UTF-16
	std::u16string Text(Units, u'a');
	size_t MaxRetained = 0;
	for (int I = 0; I < Pastes; ++I)
	{
		Text[static_cast<size_t>(I) % Units] = static_cast<char16_t>(u'b' + I % 20);   // each paste differs
		D.Edit(0, D.Buffer.Length(), Text, UndoKind::Command);                          // select all, paste
		MaxRetained = std::max(MaxRetained, D.Tree.RetainedBytes());
		if (D.Tree.RetainedBytes() > D.Tree.CapBytes())
		{
			break;
		}
	}
	CHECK(D.Tree.RetainedBytes() <= D.Tree.CapBytes());
	CHECK(MaxRetained <= D.Tree.CapBytes());
	CHECK(D.Tree.WasTrimmed());
	CHECK(D.Tree.GroupCount() >= 1u);
	// The newest groups are the ones kept: undo steps back through the most recent pastes.
	const std::u16string Newest = D.Text();
	UndoStep S = D.Undo();
	CHECK(S.Status == UndoStatus::Applied);
	CHECK(D.Text() != Newest);
	CHECK_EQ(D.Text()[static_cast<size_t>(Pastes - 1) % Units], u'a');
	std::printf("  %zu groups kept, %zu of %zu bytes\n", D.Tree.GroupCount() + 1, D.Tree.RetainedBytes(), D.Tree.CapBytes());
}

TEST(UndoTree_RunBoundaryAndRecoveryGroup)   // T-CORE-015
{
	Doc D(u"print(1)\n");
	D.Type(u"print(2)\n");
	D.Tree.MarkRunBoundary(7);                 // R8: the text run #7 started from
	const std::u16string Ran = D.Text();
	D.Type(u"x = 3");
	D.Edit(0, 0, u"# ", UndoKind::Command);
	const UndoNodeId Boundary = D.Tree.RunBoundaryNode(7);
	CHECK(Boundary != dream::NoUndoNode);
	CHECK(D.Tree.RunBoundaryNode(8) == dream::NoUndoNode);
	while (D.Tree.CurrentNode() != Boundary && D.Undo().Status == UndoStatus::Applied)
	{
	}
	CHECK(D.Text() == Ran);

	// Recovery (§4.2 journal interplay): the disk text is the root, the journal text one group; one undo shows disk.
	const std::u16string Disk = u"a = 1\nb = 2\nc = 3\n";
	const std::u16string Journal = u"a = 1\nb = 20\nc = 3\nd = 4\n";
	Doc R(Disk);
	R.Tree.BeginGroup(UndoKind::Recovery, u"Recovered unsaved changes");
	const TextSnapshot DiskSnap = R.Buffer.Snapshot();
	const EditBatch Minimal = dream::Diff::ToMinimalEdits(DiskSnap, TextBuffer(Journal).Snapshot());
	CHECK_EQ(Minimal.Ranges.size(), 2u);
	R.EditBatch_(Minimal, UndoKind::Recovery);
	R.Tree.EndGroup();
	R.Tree.ClearSavePoint();                   // the recovered text is dirty
	CHECK(R.Text() == Journal);
	const UndoStep S = R.Undo();
	CHECK(S.Label == u"Recovered unsaved changes");
	CHECK(R.Text() == Disk);
	CHECK(!R.Tree.CanUndo());
}

TEST(UndoTree_ImeCompositionIsOneStep)   // T-CORE-016
{
	Doc D(u"x");
	D.Tree.BeginGroup(UndoKind::Ime);
	Offset At = 1;
	int64_t Composed = 0;
	for (int I = 0; I < 50; ++I)
	{
		// Each update replaces the whole composition string.
		std::u16string Comp(static_cast<size_t>(1 + I % 4), static_cast<char16_t>(0x3042 + I % 10));
		D.Edit(At, Composed, Comp, UndoKind::Ime, 0.03);
		Composed = static_cast<int64_t>(Comp.size());
	}
	D.Edit(At, Composed, u"\x65E5\x672C", UndoKind::Ime);   // commit
	D.Tree.EndGroup();
	CHECK(D.Text() == u"x\x65E5\x672C");
	CHECK_EQ(D.Tree.GroupCount(), 1u);
	D.Undo();
	CHECK(D.Text() == u"x");
}

TEST(UndoTree_ResetsNeverClaimCleanOrMisapply)   // review r1 minors
{
	// A reset for an edit the tree never saw leaves the save point unknown: never clean until the next save.
	Doc D(u"abc");
	CHECK(D.Tree.IsAtSavePoint());
	D.Type(u"d");
	EditBatch B;
	B.Ranges.push_back(EditRange{0, 1, u"Z"});
	D.Buffer.Apply(B);
	CHECK(D.Undo().Status == UndoStatus::Reset);
	CHECK(!D.Tree.IsAtSavePoint());
	D.Tree.MarkSavePoint();
	CHECK(D.Tree.IsAtSavePoint());
	Doc G(u"q");
	G.Buffer.Apply(B);
	G.Type(u"r");                              // Record across the gap
	G.Undo();
	CHECK(!G.Tree.IsAtSavePoint());

	// An undo batch the buffer refused (an empty ChangeSet with origin Undo) must not re-sync the version: the next
	// Undo resets instead of applying ops to a text they no longer fit.
	Doc U(u"abc");
	U.Type(u"X");
	U.Edit(0, 0, u"Y", UndoKind::Command);
	CHECK(U.Text() == u"YabcX");
	const UndoStep S = U.Tree.Undo(U.Buffer.Snapshot());
	REQUIRE(S.Status == UndoStatus::Applied);
	ChangeSet Refused;
	Refused.Origin = dream::EditOrigin::Undo;
	Refused.VersionBefore = U.Buffer.Version();
	Refused.VersionAfter = U.Buffer.Version();
	U.Tree.Record(Refused, U.Buffer.Snapshot(), U.Buffer.Snapshot(), UndoKind::Typing, U.Now);
	const UndoStep Next = U.Tree.Undo(U.Buffer.Snapshot());
	CHECK(Next.Status == UndoStatus::Reset);
	CHECK(U.Text() == u"YabcX");
}

TEST(UndoTree_VersionGapResets)
{
	Doc D(u"abc");
	D.Type(u"d");
	// An edit the tree never saw.
	EditBatch B;
	B.Ranges.push_back(EditRange{0, 1, u"Z"});
	D.Buffer.Apply(B);
	const UndoStep S = D.Undo();
	CHECK(S.Status == UndoStatus::Reset);
	CHECK(D.Tree.WasReset());
	CHECK(D.Text() == u"Zbcd");                // the text is intact
	CHECK(!D.Tree.CanUndo());
	D.Tree.ClearNotices();
	D.Type(u"e");                              // recording works again from the new version
	D.Undo();
	CHECK(D.Text() == u"Zbcd");
	CHECK(!D.Tree.WasReset());
	// A Record across a gap also resets (and records the new edit).
	Doc G(u"q");
	G.Type(u"r");
	G.Buffer.Apply(B);
	G.Type(u"s");
	CHECK(G.Tree.WasReset());
	CHECK_EQ(G.Tree.GroupCount(), 1u);
	// A fresh tree adopts the buffer's version without a reset notice.
	TextBuffer Buf(u"t");
	Buf.Apply(B);
	UndoTree T;
	const TextSnapshot Before = Buf.Snapshot();
	const ChangeSet C = Buf.Apply(B);
	T.Record(C, Before, Buf.Snapshot(), UndoKind::Command, 1.0);
	CHECK(!T.WasReset());
	CHECK(T.CanUndo());
}

TEST(UndoTree_ProcessCap)
{
	Doc A(u"");
	Doc B(u"");
	for (int I = 0; I < 30; ++I)
	{
		A.Edit(0, A.Buffer.Length(), std::u16string(20000, static_cast<char16_t>(u'a' + I % 26)), UndoKind::Command);
		B.Edit(0, B.Buffer.Length(), std::u16string(5000, static_cast<char16_t>(u'a' + I % 26)), UndoKind::Command);
	}
	UndoTree* Trees[] = {&A.Tree, &B.Tree};
	const size_t Before = A.Tree.RetainedBytes() + B.Tree.RetainedBytes();
	const size_t Cap = Before / 2;
	const size_t Freed = dream::EnforceUndoProcessCap(Trees, Cap);
	CHECK(Freed > 0u);
	CHECK(A.Tree.RetainedBytes() + B.Tree.RetainedBytes() <= Cap);
	CHECK(A.Tree.WasTrimmed());               // the biggest tree gave first
	CHECK(A.Tree.CanUndo());
}
