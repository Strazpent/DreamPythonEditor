// SPDX-License-Identifier: Apache-2.0
// Dream core: text differences (TDD §4.6). DiffSingleRange (A4) plus the line diff: lines interned by a 64-bit hash
// (collisions resolved by comparing text), common prefix and suffix trimmed, Myers O(ND) with linear-space
// middle-snake division, the D = 4,000 / 50 ms cap turning the remaining middle into one approximate replace hunk,
// hunk sliding to blank-line or indent boundaries, and word-token Myers for the intra-line ranges.
#include "dream/Diff.h"

#include "dream/Utf.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>

namespace dream::diff_detail
{
	using Clock = std::chrono::steady_clock;

	/** The shared budget of one Lines call: the deadline and the token are sticky once they fire. */
	struct Budget
	{
		int32_t MaxD = 4000;
		bool bTimed = false;
		Clock::time_point Deadline{};
		const CancelToken* Cancel = nullptr;
		bool bExpired = false;       // deadline passed or cancelled
		bool bCancelled = false;
		uint32_t Tick = 0;

		/** Cheap poll; the clock and the token are read every 4,096 calls. */
		bool Poll()
		{
			if (bExpired)
			{
				return true;
			}
			if ((++Tick & 0xFFFu) != 0)
			{
				return false;
			}
			return CheckNow();
		}

		bool CheckNow()
		{
			if (IsCancelled(Cancel))
			{
				bCancelled = true;
				bExpired = true;
			}
			else if (bTimed && Clock::now() >= Deadline)
			{
				bExpired = true;
			}
			return bExpired;
		}
	};

	enum class BisectResult : uint8_t
	{
		Split,
		NoCommon,
		Capped,
	};

	struct ApproxRange
	{
		int32_t A0, A1, B0, B1;
	};

	/**
	 * Myers' O(ND) difference with linear-space division (the middle snake). Marks DelA[i] for removed A elements and
	 * InsB[j] for inserted B elements; the unmarked elements of A and B pair up in order and are equal.
	 */
	struct Myers
	{
		const uint32_t* A = nullptr;
		const uint32_t* B = nullptr;
		std::vector<uint8_t>* DelA = nullptr;
		std::vector<uint8_t>* InsB = nullptr;
		std::vector<ApproxRange>* Approx = nullptr;
		Budget* Bud = nullptr;
		std::vector<int32_t> V1;
		std::vector<int32_t> V2;

		void MarkAll(int32_t A0, int32_t A1, int32_t B0, int32_t B1, bool bApprox)
		{
			for (int32_t I = A0; I < A1; ++I)
			{
				(*DelA)[static_cast<size_t>(I)] = 1;
			}
			for (int32_t J = B0; J < B1; ++J)
			{
				(*InsB)[static_cast<size_t>(J)] = 1;
			}
			if (bApprox && (A0 < A1 || B0 < B1))
			{
				Approx->push_back(ApproxRange{A0, A1, B0, B1});
			}
		}

		void Compare(int32_t A0, int32_t A1, int32_t B0, int32_t B1)
		{
			while (A0 < A1 && B0 < B1 && A[A0] == B[B0])
			{
				++A0;
				++B0;
			}
			while (A0 < A1 && B0 < B1 && A[A1 - 1] == B[B1 - 1])
			{
				--A1;
				--B1;
			}
			if (A0 == A1 || B0 == B1)
			{
				MarkAll(A0, A1, B0, B1, false);
				return;
			}
			if (Bud->bExpired)
			{
				MarkAll(A0, A1, B0, B1, true);   // the remaining middle becomes one replace hunk (§4.6 step 5)
				return;
			}
			int32_t X = 0;
			int32_t Y = 0;
			const BisectResult Found = Bisect(A0, A1, B0, B1, X, Y);
			if (Found == BisectResult::NoCommon)
			{
				MarkAll(A0, A1, B0, B1, false);   // nothing in common: one exact replace
				return;
			}
			if (Found == BisectResult::Capped || (X == A0 && Y == B0) || (X == A1 && Y == B1))
			{
				MarkAll(A0, A1, B0, B1, true);
				return;
			}
			Compare(A0, X, B0, Y);
			Compare(X, A1, Y, B1);
		}

		/**
		 * Finds a split point on an optimal path through the middle snake (Myers 1986 §4b). Capped when the
		 * edit distance passes the cap or the budget expires; NoCommon when the two ranges share no element. Both
		 * ranges are non-empty and trimmed.
		 */
		BisectResult Bisect(int32_t A0, int32_t A1, int32_t B0, int32_t B1, int32_t& OutX, int32_t& OutY)
		{
			const int32_t N = A1 - A0;
			const int32_t M = B1 - B0;
			const int32_t MaxD = (N + M + 1) / 2;
			const int32_t Limit = std::min(MaxD, Bud->MaxD / 2 + 1);   // d counts each half of the D-path
			const int32_t Offset = Limit + 1;
			const int32_t Length = 2 * Limit + 3;
			V1.assign(static_cast<size_t>(Length), -1);
			V2.assign(static_cast<size_t>(Length), -1);
			V1[static_cast<size_t>(Offset + 1)] = 0;
			V2[static_cast<size_t>(Offset + 1)] = 0;
			const int32_t Delta = N - M;
			const bool bFront = (Delta % 2) != 0;   // odd: overlap is checked on the forward pass
			int32_t K1Start = 0;
			int32_t K1End = 0;
			int32_t K2Start = 0;
			int32_t K2End = 0;
			for (int32_t D = 0; D < Limit; ++D)
			{
				if (Bud->CheckNow())
				{
					return BisectResult::Capped;
				}
				for (int32_t K1 = -D + K1Start; K1 <= D - K1End; K1 += 2)
				{
					const int32_t K1Off = Offset + K1;
					int32_t X1;
					if (K1 == -D || (K1 != D && V1[static_cast<size_t>(K1Off - 1)] < V1[static_cast<size_t>(K1Off + 1)]))
					{
						X1 = V1[static_cast<size_t>(K1Off + 1)];
					}
					else
					{
						X1 = V1[static_cast<size_t>(K1Off - 1)] + 1;
					}
					int32_t Y1 = X1 - K1;
					while (X1 < N && Y1 < M && A[A0 + X1] == B[B0 + Y1])
					{
						++X1;
						++Y1;
						if (Bud->Poll())
						{
							return BisectResult::Capped;
						}
					}
					V1[static_cast<size_t>(K1Off)] = X1;
					if (X1 > N)
					{
						K1End += 2;    // ran off the right of the graph
					}
					else if (Y1 > M)
					{
						K1Start += 2;  // ran off the bottom
					}
					else if (bFront)
					{
						const int32_t K2Off = Offset + Delta - K1;
						if (K2Off >= 0 && K2Off < Length && V2[static_cast<size_t>(K2Off)] != -1)
						{
							const int32_t X2 = N - V2[static_cast<size_t>(K2Off)];
							if (X1 >= X2)
							{
								OutX = A0 + X1;
								OutY = B0 + Y1;
								return BisectResult::Split;
							}
						}
					}
				}
				for (int32_t K2 = -D + K2Start; K2 <= D - K2End; K2 += 2)
				{
					const int32_t K2Off = Offset + K2;
					int32_t X2;
					if (K2 == -D || (K2 != D && V2[static_cast<size_t>(K2Off - 1)] < V2[static_cast<size_t>(K2Off + 1)]))
					{
						X2 = V2[static_cast<size_t>(K2Off + 1)];
					}
					else
					{
						X2 = V2[static_cast<size_t>(K2Off - 1)] + 1;
					}
					int32_t Y2 = X2 - K2;
					while (X2 < N && Y2 < M && A[A0 + N - X2 - 1] == B[B0 + M - Y2 - 1])
					{
						++X2;
						++Y2;
						if (Bud->Poll())
						{
							return BisectResult::Capped;
						}
					}
					V2[static_cast<size_t>(K2Off)] = X2;
					if (X2 > N)
					{
						K2End += 2;
					}
					else if (Y2 > M)
					{
						K2Start += 2;
					}
					else if (!bFront)
					{
						const int32_t K1Off = Offset + Delta - K2;
						if (K1Off >= 0 && K1Off < Length && V1[static_cast<size_t>(K1Off)] != -1)
						{
							const int32_t X1 = V1[static_cast<size_t>(K1Off)];
							const int32_t Y1 = Offset + X1 - K1Off;
							if (X1 >= N - X2)
							{
								OutX = A0 + X1;
								OutY = B0 + Y1;
								return BisectResult::Split;
							}
						}
					}
				}
			}
			// The loop ran to (N + M + 1) / 2 without an overlap: no common element. Stopped by the cap instead: approximate.
			return Limit < MaxD ? BisectResult::Capped : BisectResult::NoCommon;
		}
	};

	/** A text cut into diff lines: each with its '\n', no trailing empty line (unified-diff lines). */
	struct SideText
	{
		std::u16string Text;
		std::vector<Offset> Starts;   // Count() + 1 entries; the last is Text.size()

		explicit SideText(const TextSnapshot& S)
			: Text(S.Text())
		{
			Starts.push_back(0);
			for (size_t I = 0; I < Text.size(); ++I)
			{
				if (Text[I] == u'\n')
				{
					Starts.push_back(static_cast<Offset>(I + 1));
				}
			}
			if (Starts.back() != static_cast<Offset>(Text.size()))
			{
				Starts.push_back(static_cast<Offset>(Text.size()));
			}
		}

		int32_t Count() const { return static_cast<int32_t>(Starts.size()) - 1; }
		Offset Start(int32_t Line) const { return Starts[static_cast<size_t>(Line)]; }
		std::u16string_view Line(int32_t I) const
		{
			return std::u16string_view(Text).substr(static_cast<size_t>(Starts[static_cast<size_t>(I)]),
				static_cast<size_t>(Starts[static_cast<size_t>(I) + 1] - Starts[static_cast<size_t>(I)]));
		}
		bool HasNewline(int32_t I) const
		{
			const std::u16string_view L = Line(I);
			return !L.empty() && L.back() == u'\n';
		}
		std::u16string_view Content(int32_t I) const
		{
			std::u16string_view L = Line(I);
			if (!L.empty() && L.back() == u'\n')
			{
				L.remove_suffix(1);
			}
			return L;
		}
	};

	uint64_t HashUnits(std::u16string_view S)
	{
		uint64_t H = 1469598103934665603ull;
		for (char16_t C : S)
		{
			H ^= static_cast<uint64_t>(C);
			H *= 1099511628211ull;
		}
		return H ^ (static_cast<uint64_t>(S.size()) * 0x9E3779B97F4A7C15ull);
	}

	/** Interns strings to ids; equal text gives an equal id (hash collisions are resolved by comparing text). */
	struct Interner
	{
		std::unordered_map<uint64_t, uint32_t> FirstByHash;
		std::vector<std::u16string_view> Texts;
		std::vector<uint32_t> NextSameHash;

		uint32_t Intern(std::u16string_view S)
		{
			const uint64_t H = HashUnits(S);
			const auto It = FirstByHash.find(H);
			if (It == FirstByHash.end())
			{
				const uint32_t Id = static_cast<uint32_t>(Texts.size());
				FirstByHash.emplace(H, Id);
				Texts.push_back(S);
				NextSameHash.push_back(UINT32_MAX);
				return Id;
			}
			uint32_t Id = It->second;
			for (;;)
			{
				if (Texts[Id] == S)
				{
					return Id;
				}
				if (NextSameHash[Id] == UINT32_MAX)
				{
					const uint32_t NewId = static_cast<uint32_t>(Texts.size());
					NextSameHash[Id] = NewId;
					Texts.push_back(S);
					NextSameHash.push_back(UINT32_MAX);
					return NewId;
				}
				Id = NextSameHash[Id];
			}
		}
	};

	/** Runs Myers over two id sequences; returns the marks and the approximate ranges. */
	void RunMyers(const std::vector<uint32_t>& A, const std::vector<uint32_t>& B, Budget& Bud, std::vector<uint8_t>& DelA,
		std::vector<uint8_t>& InsB, std::vector<ApproxRange>& Approx)
	{
		DelA.assign(A.size(), 0);
		InsB.assign(B.size(), 0);
		Myers M;
		M.A = A.data();
		M.B = B.data();
		M.DelA = &DelA;
		M.InsB = &InsB;
		M.Approx = &Approx;
		M.Bud = &Bud;
		M.Compare(0, static_cast<int32_t>(A.size()), 0, static_cast<int32_t>(B.size()));
	}

	/** Hunks from the marks: maximal runs of marked elements between equal pairs. */
	std::vector<DiffHunk> HunksFromMarks(const std::vector<uint8_t>& DelA, const std::vector<uint8_t>& InsB)
	{
		std::vector<DiffHunk> Hunks;
		const int32_t N = static_cast<int32_t>(DelA.size());
		const int32_t M = static_cast<int32_t>(InsB.size());
		int32_t I = 0;
		int32_t J = 0;
		while (I < N || J < M)
		{
			if (I < N && J < M && !DelA[static_cast<size_t>(I)] && !InsB[static_cast<size_t>(J)])
			{
				++I;
				++J;
				continue;
			}
			const int32_t I0 = I;
			const int32_t J0 = J;
			for (;;)
			{
				bool bMoved = false;
				while (I < N && DelA[static_cast<size_t>(I)])
				{
					++I;
					bMoved = true;
				}
				while (J < M && InsB[static_cast<size_t>(J)])
				{
					++J;
					bMoved = true;
				}
				if (!bMoved)
				{
					break;
				}
			}
			if (I == I0 && J == J0)
			{
				break;   // unpaired tail without marks cannot happen for a valid script; stop rather than loop
			}
			DiffHunk H;
			H.OldFirst = I0;
			H.OldCount = I - I0;
			H.NewFirst = J0;
			H.NewCount = J - J0;
			Hunks.push_back(std::move(H));
		}
		return Hunks;
	}

	bool IsBlankLine(std::u16string_view Content)
	{
		for (char16_t C : Content)
		{
			if (!utf::IsPyBlank(C))
			{
				return false;
			}
		}
		return true;
	}

	int32_t IndentOf(std::u16string_view Content)
	{
		int32_t Cols = 0;
		for (char16_t C : Content)
		{
			if (C == u' ')
			{
				++Cols;
			}
			else if (C == u'\t')
			{
				Cols = (Cols / 8 + 1) * 8;
			}
			else
			{
				return Cols;
			}
		}
		return -1;   // blank
	}

	/**
	 * §4.6 step 4 (proposed rule): an insert-only or delete-only hunk slides while it stays equivalent and at least one
	 * equal line away from its neighbours. Among the positions it picks the one whose last line is blank, then whose
	 * first line is not blank, then whose first line has the smallest indentation; ties go to the lowest position.
	 */
	void SlideHunks(std::vector<DiffHunk>& Hunks, const std::vector<uint32_t>& A, const std::vector<uint32_t>& B,
		const SideText& OldSide, const SideText& NewSide)
	{
		for (size_t H = 0; H < Hunks.size(); ++H)
		{
			DiffHunk& Hk = Hunks[H];
			const bool bDelete = Hk.NewCount == 0 && Hk.OldCount > 0;
			const bool bInsert = Hk.OldCount == 0 && Hk.NewCount > 0;
			if (!bDelete && !bInsert)
			{
				continue;
			}
			const std::vector<uint32_t>& Ids = bDelete ? A : B;
			const SideText& Side = bDelete ? OldSide : NewSide;
			const int32_t Count = static_cast<int32_t>(Ids.size());
			const int32_t S = bDelete ? Hk.OldFirst : Hk.NewFirst;
			const int32_t C = bDelete ? Hk.OldCount : Hk.NewCount;
			const int32_t PrevEnd = H == 0 ? -1
				: (bDelete ? Hunks[H - 1].OldFirst + Hunks[H - 1].OldCount : Hunks[H - 1].NewFirst + Hunks[H - 1].NewCount);
			const int32_t NextStart = H + 1 < Hunks.size() ? (bDelete ? Hunks[H + 1].OldFirst : Hunks[H + 1].NewFirst) : Count + 1;
			int32_t Lo = S;
			while (Lo - 1 > PrevEnd && Lo - 1 >= 0 && Ids[static_cast<size_t>(Lo - 1)] == Ids[static_cast<size_t>(Lo + C - 1)])
			{
				--Lo;
			}
			int32_t Hi = S;
			while (Hi + C < Count && Hi + C + 1 < NextStart && Ids[static_cast<size_t>(Hi + C)] == Ids[static_cast<size_t>(Hi)])
			{
				++Hi;
			}
			if (Lo == Hi)
			{
				continue;
			}
			int32_t MinIndent = INT32_MAX;
			for (int32_t P = Lo; P <= Hi; ++P)
			{
				const int32_t Ind = IndentOf(Side.Content(P));
				if (Ind >= 0)
				{
					MinIndent = std::min(MinIndent, Ind);
				}
			}
			int32_t Best = S;
			int32_t BestScore = -1;
			for (int32_t P = Lo; P <= Hi; ++P)
			{
				const bool bLastBlank = IsBlankLine(Side.Content(P + C - 1));
				const bool bFirstBlank = IsBlankLine(Side.Content(P));
				const int32_t Ind = IndentOf(Side.Content(P));
				const int32_t Score = (bLastBlank ? 4 : 0) + (!bFirstBlank ? 2 : 0) + (Ind >= 0 && Ind == MinIndent ? 1 : 0);
				if (Score >= BestScore)
				{
					BestScore = Score;
					Best = P;
				}
			}
			const int32_t Shift = Best - S;
			Hk.OldFirst += Shift;
			Hk.NewFirst += Shift;
		}
	}

	/** Word tokens for the intra-line pass: identifier runs, blank runs, '\n', and single other code points. */
	void WordTokens(std::u16string_view Text, std::vector<std::u16string_view>& Out)
	{
		size_t I = 0;
		while (I < Text.size())
		{
			const size_t Start = I;
			size_t Units = 1;
			const char32_t Cp = utf::DecodeAt(Text, I, Units);
			if (utf::IsPyBlank(Text[I]))
			{
				while (I < Text.size() && utf::IsPyBlank(Text[I]))
				{
					++I;
				}
			}
			else if (utf::IsIdentifierContinue(Cp))
			{
				I += Units;
				while (I < Text.size())
				{
					size_t U = 1;
					const char32_t Next = utf::DecodeAt(Text, I, U);
					if (!utf::IsIdentifierContinue(Next))
					{
						break;
					}
					I += U;
				}
			}
			else
			{
				I += Units;
			}
			Out.push_back(Text.substr(Start, I - Start));
		}
	}

	void AddSpan(std::vector<DiffSpan>& Spans, Offset At, int64_t Len)
	{
		if (Len <= 0)
		{
			return;
		}
		if (!Spans.empty() && Spans.back().At + Spans.back().Len == At)
		{
			Spans.back().Len += Len;
			return;
		}
		Spans.push_back(DiffSpan{At, Len});
	}

	void IntraLine(DiffHunk& Hk, const SideText& OldSide, const SideText& NewSide, const DiffOptions& Options, Budget& Bud)
	{
		const Offset OldAt = OldSide.Start(Hk.OldFirst);
		const Offset NewAt = NewSide.Start(Hk.NewFirst);
		const std::u16string_view OldText = std::u16string_view(OldSide.Text).substr(static_cast<size_t>(OldAt),
			static_cast<size_t>(OldSide.Start(Hk.OldFirst + Hk.OldCount) - OldAt));
		const std::u16string_view NewText = std::u16string_view(NewSide.Text).substr(static_cast<size_t>(NewAt),
			static_cast<size_t>(NewSide.Start(Hk.NewFirst + Hk.NewCount) - NewAt));
		std::vector<std::u16string_view> OldToks;
		std::vector<std::u16string_view> NewToks;
		WordTokens(OldText, OldToks);
		WordTokens(NewText, NewToks);
		if (static_cast<int64_t>(OldToks.size()) > Options.MaxIntraTokens || static_cast<int64_t>(NewToks.size()) > Options.MaxIntraTokens)
		{
			return;
		}
		Interner Words;
		std::vector<uint32_t> A;
		std::vector<uint32_t> B;
		A.reserve(OldToks.size());
		B.reserve(NewToks.size());
		for (std::u16string_view T : OldToks)
		{
			A.push_back(Words.Intern(T));
		}
		for (std::u16string_view T : NewToks)
		{
			B.push_back(Words.Intern(T));
		}
		std::vector<uint8_t> DelA;
		std::vector<uint8_t> InsB;
		std::vector<ApproxRange> Approx;
		RunMyers(A, B, Bud, DelA, InsB, Approx);
		if (!Approx.empty() || Bud.bCancelled)
		{
			return;   // no intra ranges for a middle the cap cut short
		}
		Offset Pos = OldAt;
		for (size_t I = 0; I < OldToks.size(); ++I)
		{
			if (DelA[I])
			{
				AddSpan(Hk.OldIntra, Pos, static_cast<int64_t>(OldToks[I].size()));
			}
			Pos += static_cast<Offset>(OldToks[I].size());
		}
		Pos = NewAt;
		for (size_t J = 0; J < NewToks.size(); ++J)
		{
			if (InsB[J])
			{
				AddSpan(Hk.NewIntra, Pos, static_cast<int64_t>(NewToks[J].size()));
			}
			Pos += static_cast<Offset>(NewToks[J].size());
		}
	}

	/** The hunk's range narrowed to its common prefix and suffix, never splitting a surrogate pair. */
	EditRange NarrowRange(std::u16string_view Old, std::u16string_view New, Offset OldAt)
	{
		EditRange R = DiffSingleRange(Old, New);
		R.At += OldAt;
		return R;
	}

	void AppendNumber(std::u16string& Out, int64_t V)
	{
		const std::string S = std::to_string(V);
		for (char C : S)
		{
			Out.push_back(static_cast<char16_t>(C));
		}
	}

	void AppendRange(std::u16string& Out, int32_t Start, int32_t Count)
	{
		if (Count == 1)
		{
			AppendNumber(Out, Start + 1);
			return;
		}
		AppendNumber(Out, Count == 0 ? Start : Start + 1);
		Out += u',';
		AppendNumber(Out, Count);
	}
}

namespace dream
{
	using namespace diff_detail;

	EditRange DiffSingleRange(std::u16string_view Old, std::u16string_view New)
	{
		EditRange Range;
		if (Old == New)
		{
			Range.At = static_cast<Offset>(Old.size());
			return Range;
		}
		const size_t MinLen = std::min(Old.size(), New.size());
		size_t Prefix = 0;
		while (Prefix < MinLen && Old[Prefix] == New[Prefix])
		{
			++Prefix;
		}
		// Never end the prefix between the halves of a surrogate pair.
		if (Prefix > 0 && utf::IsHighSurrogate(Old[Prefix - 1]))
		{
			--Prefix;
		}
		size_t Suffix = 0;
		const size_t MaxSuffix = MinLen - Prefix;
		while (Suffix < MaxSuffix && Old[Old.size() - 1 - Suffix] == New[New.size() - 1 - Suffix])
		{
			++Suffix;
		}
		// Never start the suffix on the low half of a surrogate pair.
		if (Suffix > 0 && utf::IsLowSurrogate(Old[Old.size() - Suffix]))
		{
			--Suffix;
		}
		Range.At = static_cast<Offset>(Prefix);
		Range.DeleteLen = static_cast<int64_t>(Old.size() - Prefix - Suffix);
		Range.Insert.assign(New.substr(Prefix, New.size() - Prefix - Suffix));
		return Range;
	}

	EditRange DiffSingleRange(const TextSnapshot& Old, const TextSnapshot& New)
	{
		// A contiguous snapshot returns its text whole from Chunk without a copy; a split one falls back to copies.
		const std::u16string_view OldView = Old.Chunk(0, Old.Length());
		const std::u16string_view NewView = New.Chunk(0, New.Length());
		if (static_cast<int64_t>(OldView.size()) == Old.Length() && static_cast<int64_t>(NewView.size()) == New.Length())
		{
			return DiffSingleRange(OldView, NewView);
		}
		return DiffSingleRange(Old.Text(), New.Text());
	}

	DiffResult Diff::Lines(const TextSnapshot& Old, const TextSnapshot& New)
	{
		return Lines(Old, New, DiffOptions());
	}

	DiffResult Diff::Lines(const TextSnapshot& Old, const TextSnapshot& New, const DiffOptions& Options, const CancelToken* Cancel)
	{
		DiffResult Result;
		Budget Bud;
		Bud.MaxD = std::max(Options.MaxEditDistance, 2);
		Bud.Cancel = Cancel;
		if (Options.TimeLimitMs > 0.0)
		{
			Bud.bTimed = true;
			Bud.Deadline = Clock::now() + std::chrono::microseconds(static_cast<int64_t>(Options.TimeLimitMs * 1000.0));
		}
		const SideText OldSide(Old);
		const SideText NewSide(New);
		Result.OldLines = OldSide.Count();
		Result.NewLines = NewSide.Count();
		if (OldSide.Text == NewSide.Text)
		{
			return Result;
		}

		Interner LineIds;
		std::vector<uint32_t> A(static_cast<size_t>(OldSide.Count()));
		std::vector<uint32_t> B(static_cast<size_t>(NewSide.Count()));
		for (int32_t I = 0; I < OldSide.Count(); ++I)
		{
			A[static_cast<size_t>(I)] = LineIds.Intern(OldSide.Line(I));
		}
		for (int32_t J = 0; J < NewSide.Count(); ++J)
		{
			B[static_cast<size_t>(J)] = LineIds.Intern(NewSide.Line(J));
		}
		std::vector<uint8_t> DelA;
		std::vector<uint8_t> InsB;
		std::vector<ApproxRange> Approx;
		RunMyers(A, B, Bud, DelA, InsB, Approx);
		if (Bud.bCancelled)
		{
			Result.bCancelled = true;
			return Result;
		}
		Result.Hunks = HunksFromMarks(DelA, InsB);
		for (DiffHunk& H : Result.Hunks)
		{
			for (const ApproxRange& R : Approx)
			{
				const bool bOld = H.OldFirst < R.A1 && R.A0 < H.OldFirst + H.OldCount;
				const bool bNew = H.NewFirst < R.B1 && R.B0 < H.NewFirst + H.NewCount;
				if (bOld || bNew)
				{
					H.bApproximate = true;
					Result.bApproximate = true;
				}
			}
		}
		if (Options.bSlideHunks)
		{
			SlideHunks(Result.Hunks, A, B, OldSide, NewSide);
		}
		if (Options.bIntraLine)
		{
			for (DiffHunk& H : Result.Hunks)
			{
				if (H.OldCount > 0 && H.NewCount > 0 && !H.bApproximate && !Bud.bExpired)
				{
					IntraLine(H, OldSide, NewSide, Options, Bud);
				}
			}
			if (Bud.bCancelled)
			{
				Result = DiffResult();
				Result.bCancelled = true;
				Result.OldLines = OldSide.Count();
				Result.NewLines = NewSide.Count();
			}
		}
		return Result;
	}

	EditBatch Diff::ToMinimalEdits(const TextSnapshot& Old, const TextSnapshot& New, EditOrigin Origin)
	{
		DiffOptions Options;
		Options.bSlideHunks = false;
		Options.bIntraLine = false;
		return ToMinimalEdits(Old, New, Lines(Old, New, Options), Origin);
	}

	EditBatch Diff::ToMinimalEdits(const TextSnapshot& Old, const TextSnapshot& New, const DiffResult& Result, EditOrigin Origin)
	{
		EditBatch Batch;
		Batch.Origin = Origin;
		if (Result.bCancelled || Result.Hunks.empty())
		{
			return Batch;
		}
		const SideText OldSide(Old);
		const SideText NewSide(New);
		if (Result.OldLines != OldSide.Count() || Result.NewLines != NewSide.Count())
		{
			return Batch;   // the result belongs to other texts
		}
		Batch.Ranges.reserve(Result.Hunks.size());
		for (const DiffHunk& H : Result.Hunks)
		{
			const Offset OldAt = OldSide.Start(H.OldFirst);
			const Offset OldEnd = OldSide.Start(H.OldFirst + H.OldCount);
			const Offset NewAt = NewSide.Start(H.NewFirst);
			const Offset NewEnd = NewSide.Start(H.NewFirst + H.NewCount);
			EditRange R = NarrowRange(
				std::u16string_view(OldSide.Text).substr(static_cast<size_t>(OldAt), static_cast<size_t>(OldEnd - OldAt)),
				std::u16string_view(NewSide.Text).substr(static_cast<size_t>(NewAt), static_cast<size_t>(NewEnd - NewAt)), OldAt);
			if (R.DeleteLen != 0 || !R.Insert.empty())
			{
				Batch.Ranges.push_back(std::move(R));
			}
		}
		return Batch;
	}

	std::vector<UnifiedLine> Diff::Unified(const TextSnapshot& Old, const TextSnapshot& New, const DiffResult& Result,
		const UnifiedOptions& Options)
	{
		std::vector<UnifiedLine> Out;
		if (Result.bCancelled || Result.Hunks.empty())
		{
			return Out;
		}
		const SideText OldSide(Old);
		const SideText NewSide(New);
		if (Result.OldLines != OldSide.Count() || Result.NewLines != NewSide.Count())
		{
			return Out;
		}
		if (!Options.OldLabel.empty() || !Options.NewLabel.empty())
		{
			Out.push_back(UnifiedLine{UnifiedLine::Kind::FileHeader, u"--- " + Options.OldLabel, InvalidLine, InvalidLine});
			Out.push_back(UnifiedLine{UnifiedLine::Kind::FileHeader, u"+++ " + Options.NewLabel, InvalidLine, InvalidLine});
		}
		const int32_t Ctx = std::max(Options.Context, 0);
		const std::vector<DiffHunk>& Hunks = Result.Hunks;

		auto EmitLine = [&](UnifiedLine::Kind Kind, const SideText& Side, int32_t Line, LineNo OldLine, LineNo NewLine)
		{
			UnifiedLine U;
			U.Type = Kind;
			U.Text.push_back(Kind == UnifiedLine::Kind::Removed ? u'-' : (Kind == UnifiedLine::Kind::Added ? u'+' : u' '));
			U.Text.append(Side.Content(Line));
			U.OldLine = OldLine;
			U.NewLine = NewLine;
			Out.push_back(std::move(U));
			if (!Side.HasNewline(Line))
			{
				Out.push_back(UnifiedLine{UnifiedLine::Kind::NoNewline, u"\\ No newline at end of file", InvalidLine, InvalidLine});
			}
		};

		size_t First = 0;
		while (First < Hunks.size())
		{
			size_t Last = First;
			while (Last + 1 < Hunks.size()
				&& Hunks[Last + 1].OldFirst - (Hunks[Last].OldFirst + Hunks[Last].OldCount) <= 2 * Ctx)
			{
				++Last;
			}
			const DiffHunk& H0 = Hunks[First];
			const DiffHunk& H1 = Hunks[Last];
			const int32_t OldStart = std::max(0, H0.OldFirst - Ctx);
			const int32_t Lead = H0.OldFirst - OldStart;
			const int32_t NewStart = H0.NewFirst - Lead;
			const int32_t Trail = std::min(Ctx, OldSide.Count() - (H1.OldFirst + H1.OldCount));
			const int32_t OldEnd = H1.OldFirst + H1.OldCount + Trail;
			const int32_t NewEnd = H1.NewFirst + H1.NewCount + Trail;

			UnifiedLine Header;
			Header.Type = UnifiedLine::Kind::HunkHeader;
			Header.Text = u"@@ -";
			AppendRange(Header.Text, OldStart, OldEnd - OldStart);
			Header.Text += u" +";
			AppendRange(Header.Text, NewStart, NewEnd - NewStart);
			Header.Text += u" @@";
			Out.push_back(std::move(Header));

			int32_t I = OldStart;
			int32_t J = NewStart;
			for (size_t K = First; K <= Last; ++K)
			{
				const DiffHunk& H = Hunks[K];
				while (I < H.OldFirst)
				{
					EmitLine(UnifiedLine::Kind::Context, OldSide, I, I, J);
					++I;
					++J;
				}
				for (int32_t L = 0; L < H.OldCount; ++L)
				{
					EmitLine(UnifiedLine::Kind::Removed, OldSide, I, I, InvalidLine);
					++I;
				}
				for (int32_t L = 0; L < H.NewCount; ++L)
				{
					EmitLine(UnifiedLine::Kind::Added, NewSide, J, InvalidLine, J);
					++J;
				}
			}
			while (I < OldEnd)
			{
				EmitLine(UnifiedLine::Kind::Context, OldSide, I, I, J);
				++I;
				++J;
			}
			First = Last + 1;
		}
		return Out;
	}

	std::u16string Diff::FormatUnified(const TextSnapshot& Old, const TextSnapshot& New, const DiffResult& Result,
		const UnifiedOptions& Options)
	{
		std::u16string Out;
		for (const UnifiedLine& L : Unified(Old, New, Result, Options))
		{
			Out += L.Text;
			Out += u'\n';
		}
		return Out;
	}
}
