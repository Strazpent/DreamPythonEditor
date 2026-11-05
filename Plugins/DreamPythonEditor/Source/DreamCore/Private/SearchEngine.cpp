// SPDX-License-Identifier: Apache-2.0
// Dream core: find and replace (TDD §4.4). Literal queries scan directly (folded code points when ignoring case);
// regex queries run the matcher of SearchRegex.cpp at each start position with Python's finditer rules and the
// per-line step budget. Replacement templates follow Python's re.sub syntax plus §4.4's $1, ${name} and $$.
#include "dream/SearchEngine.h"

#include "SearchRegex.h"

#include <algorithm>
#include <chrono>

namespace dream::search_detail
{
	using namespace regex_detail;
	using Clock = std::chrono::steady_clock;

	bool WordBefore(std::u16string_view T, Offset At)
	{
		if (At <= 0)
		{
			return false;
		}
		size_t Units = 1;
		return IsWordCp(DecodeBefore(T, static_cast<size_t>(At), Units));
	}

	bool WordAfter(std::u16string_view T, Offset At)
	{
		if (At < 0 || static_cast<size_t>(At) >= T.size())
		{
			return false;
		}
		size_t Units = 1;
		return IsWordCp(DecodeAt(T, static_cast<size_t>(At), Units));
	}

	Offset NextCodePoint(std::u16string_view T, Offset At)
	{
		if (static_cast<size_t>(At) >= T.size())
		{
			return At + 1;
		}
		size_t Units = 1;
		DecodeAt(T, static_cast<size_t>(At), Units);
		return At + static_cast<Offset>(Units);
	}

	Offset LineEndAt(std::u16string_view T, Offset At)
	{
		const size_t P = T.find(u'\n', static_cast<size_t>(std::max<Offset>(At, 0)));
		return P == std::u16string_view::npos ? static_cast<Offset>(T.size()) : static_cast<Offset>(P);
	}

	/** Length of the literal match at At (folded comparison), or -1. */
	int64_t LiteralAt(std::u16string_view T, const CompiledQuery& Q, Offset At)
	{
		if (Q.Query.bMatchCase)
		{
			if (T.substr(static_cast<size_t>(At), Q.Needle.size()) != Q.Needle)
			{
				return -1;
			}
			return static_cast<int64_t>(Q.Needle.size());
		}
		size_t P = static_cast<size_t>(At);
		for (char32_t F : Q.FoldedNeedle)
		{
			if (P >= T.size())
			{
				return -1;
			}
			size_t Units = 1;
			const char32_t C = DecodeAt(T, P, Units);
			if (FoldCp(C) != F)
			{
				return -1;
			}
			P += Units;
		}
		return static_cast<int64_t>(P) - At;
	}

	/**
	 * Calls OnMatch(At, End, Matcher*) for every match starting in [From, To], with finditer rules. Matcher is null for
	 * literal queries. OnMatch returns false to stop. Cancel is polled every 64 start positions.
	 */
	template <class Fn>
	void Scan(std::u16string_view T, const CompiledQuery& Q, Offset From, Offset To, const SearchLimits& Limits,
		const CancelToken* Cancel, SearchSummary& Sum, Fn&& OnMatch)
	{
		const Offset Size = static_cast<Offset>(T.size());
		From = std::clamp<Offset>(From, 0, Size);
		To = To < 0 ? Size : std::min(To, Size);
		uint32_t Poll = 0;
		if (Q.bLiteral)
		{
			const char16_t First = Q.Needle.empty() ? 0 : Q.Needle[0];
			const bool bAsciiFirst = First < 0x80;
			const char16_t FirstLower = static_cast<char16_t>(FoldCp(First));
			const char16_t FirstUpper = static_cast<char16_t>(FirstLower >= u'a' && FirstLower <= u'z' ? FirstLower - 32 : FirstLower);
			Offset Pos = From;
			while (Pos <= To)
			{
				if ((++Poll & 63) == 0 && IsCancelled(Cancel))
				{
					Sum.bCancelled = true;
					return;
				}
				// Jump to the next candidate.
				if (Q.Query.bMatchCase)
				{
					const size_t P = T.find(Q.Needle, static_cast<size_t>(Pos));
					if (P == std::u16string_view::npos || static_cast<Offset>(P) > To)
					{
						return;
					}
					Pos = static_cast<Offset>(P);
				}
				else if (bAsciiFirst)
				{
					size_t P = static_cast<size_t>(Pos);
					while (P < T.size() && T[P] != FirstLower && T[P] != FirstUpper && T[P] < 0x80)
					{
						++P;
					}
					if (P >= T.size() || static_cast<Offset>(P) > To)
					{
						return;
					}
					Pos = static_cast<Offset>(P);
				}
				const int64_t Len = LiteralAt(T, Q, Pos);
				if (Len > 0 && (!Q.Query.bWholeWord || (!WordBefore(T, Pos) && !WordAfter(T, Pos + Len))))
				{
					if (!OnMatch(Pos, Pos + Len, static_cast<Matcher*>(nullptr)))
					{
						return;
					}
					Pos += Len;
					continue;
				}
				Pos = NextCodePoint(T, Pos);
			}
			return;
		}

		Matcher M(Q.Prog, T, Limits);
		M.SetCancel(Cancel);
		const bool bWhole = Q.Query.bWholeWord;
		const int32_t FirstChar = Q.Prog.FirstChar;
		Offset Pos = From;
		bool bMustAdvance = false;
		Offset BudgetEnd = -1;
		while (Pos <= To)
		{
			if ((++Poll & 63) == 0 && IsCancelled(Cancel))
			{
				Sum.bCancelled = true;
				return;
			}
			if (FirstChar >= 0 && FirstChar < 0x10000 && !bMustAdvance)
			{
				const size_t P = T.find(static_cast<char16_t>(FirstChar), static_cast<size_t>(Pos));
				if (P == std::u16string_view::npos || static_cast<Offset>(P) > To)
				{
					return;
				}
				Pos = static_cast<Offset>(P);
			}
			if (Pos > BudgetEnd)
			{
				M.ResetSteps();   // the step budget is per line of start positions (§4.4 limits)
				BudgetEnd = LineEndAt(T, Pos);
			}
			Offset End = 0;
			const Status S = M.MatchAt(Pos, bMustAdvance, -1, End, bWhole);
			if (S == Status::Matched)
			{
				if (!OnMatch(Pos, End, &M))
				{
					return;
				}
				bMustAdvance = End == Pos;
				Pos = End;
				continue;
			}
			bMustAdvance = false;
			if (S == Status::Cancelled)
			{
				Sum.bCancelled = true;
				return;
			}
			if (S == Status::Limit)
			{
				++Sum.ComplexLines;
				if (BudgetEnd >= Size)
				{
					return;
				}
				Pos = BudgetEnd + 1;
				continue;
			}
			Pos = NextCodePoint(T, Pos);
		}
	}

	/** One parsed piece of a replacement template: literal text or a group reference. */
	struct TemplatePart
	{
		std::u16string Literal;
		int32_t Group = -1;
	};

	bool ParseGroupRef(std::u16string_view Body, const CompiledQuery& Q, int32_t& OutGroup)
	{
		if (Body.empty())
		{
			return false;
		}
		bool bDigits = true;
		for (char16_t C : Body)
		{
			bDigits = bDigits && C >= u'0' && C <= u'9';
		}
		if (bDigits)
		{
			if (Body.size() > 6)
			{
				return false;
			}
			int32_t G = 0;
			for (char16_t C : Body)
			{
				G = G * 10 + (C - u'0');
			}
			OutGroup = G;
			return G < Q.Prog.Groups;
		}
		for (const auto& N : Q.Prog.Names)
		{
			if (N.first == Body)
			{
				OutGroup = N.second;
				return true;
			}
		}
		return false;
	}

	bool TemplateError(SearchError* Err, size_t Pos, std::u16string Detail)
	{
		if (Err != nullptr)
		{
			Err->Code = SearchErrorCode::BadTemplate;
			Err->Position = static_cast<int32_t>(Pos);
			Err->Detail = std::move(Detail);
		}
		return false;
	}

	bool ParseTemplate(const CompiledQuery& Q, std::u16string_view T, std::vector<TemplatePart>& Parts, SearchError* Err)
	{
		Parts.clear();
		auto Lit = [&](char32_t C)
		{
			if (Parts.empty() || Parts.back().Group >= 0)
			{
				Parts.emplace_back();
			}
			if (C >= 0x10000)
			{
				Parts.back().Literal.push_back(static_cast<char16_t>(0xD800 + ((C - 0x10000) >> 10)));
				Parts.back().Literal.push_back(static_cast<char16_t>(0xDC00 + ((C - 0x10000) & 0x3FF)));
			}
			else
			{
				Parts.back().Literal.push_back(static_cast<char16_t>(C));
			}
		};
		auto Ref = [&](int32_t G)
		{
			TemplatePart P;
			P.Group = G;
			Parts.push_back(std::move(P));
		};
		if (Q.bLiteral)
		{
			Parts.emplace_back();
			Parts.back().Literal.assign(T);
			return true;
		}
		size_t I = 0;
		while (I < T.size())
		{
			const char16_t C = T[I];
			const size_t Start = I;
			if (C == u'\\')
			{
				if (I + 1 >= T.size())
				{
					return TemplateError(Err, Start, u"bad escape (end of pattern)");
				}
				const char16_t N = T[I + 1];
				I += 2;
				if (N == u'g')
				{
					if (I >= T.size() || T[I] != u'<')
					{
						return TemplateError(Err, Start, u"missing <");
					}
					const size_t Close = T.find(u'>', I + 1);
					if (Close == std::u16string_view::npos)
					{
						return TemplateError(Err, Start, u"missing >, unterminated name");
					}
					const std::u16string_view Body = T.substr(I + 1, Close - I - 1);
					int32_t G = -1;
					if (!ParseGroupRef(Body, Q, G))
					{
						return TemplateError(Err, Start, u"unknown group name '" + std::u16string(Body) + u"'");
					}
					Ref(G);
					I = Close + 1;
				}
				else if (N == u'0')
				{
					// Octal: \0 plus up to two octal digits.
					char32_t V = 0;
					for (int K = 0; K < 2 && I < T.size() && T[I] >= u'0' && T[I] <= u'7'; ++K)
					{
						V = V * 8 + (T[I] - u'0');
						++I;
					}
					Lit(V);
				}
				else if (N >= u'1' && N <= u'9')
				{
					int32_t G = N - u'0';
					if (I < T.size() && T[I] >= u'0' && T[I] <= u'9')
					{
						const bool bOctal = N <= u'7' && T[I] <= u'7' && I + 1 < T.size() && T[I + 1] >= u'0' && T[I + 1] <= u'7';
						if (bOctal)
						{
							const char32_t V = static_cast<char32_t>((N - u'0') * 64 + (T[I] - u'0') * 8 + (T[I + 1] - u'0'));
							I += 2;
							if (V > 0377)
							{
								return TemplateError(Err, Start, u"octal escape value outside of range 0-0o377");
							}
							Lit(V);
							continue;
						}
						G = G * 10 + (T[I] - u'0');
						++I;
					}
					if (G >= Q.Prog.Groups)
					{
						return TemplateError(Err, Start, u"invalid group reference");
					}
					Ref(G);
				}
				else
				{
					switch (N)
					{
					case u'n': Lit(u'\n'); break;
					case u't': Lit(u'\t'); break;
					case u'r': Lit(u'\r'); break;
					case u'a': Lit(7); break;
					case u'b': Lit(8); break;
					case u'f': Lit(12); break;
					case u'v': Lit(11); break;
					case u'\\': Lit(u'\\'); break;
					default:
						if ((N >= u'a' && N <= u'z') || (N >= u'A' && N <= u'Z'))
						{
							return TemplateError(Err, Start, std::u16string(u"bad escape \\") + N);
						}
						Lit(u'\\');   // Python keeps other escapes as they are
						Lit(N);
						break;
					}
				}
				continue;
			}
			if (C == u'$' && I + 1 < T.size())
			{
				const char16_t N = T[I + 1];
				if (N == u'$')
				{
					Lit(u'$');
					I += 2;
					continue;
				}
				if (N == u'{')
				{
					const size_t Close = T.find(u'}', I + 2);
					if (Close == std::u16string_view::npos)
					{
						return TemplateError(Err, Start, u"missing }, unterminated name");
					}
					const std::u16string_view Body = T.substr(I + 2, Close - I - 2);
					int32_t G = -1;
					if (!ParseGroupRef(Body, Q, G))
					{
						return TemplateError(Err, Start, u"unknown group name '" + std::u16string(Body) + u"'");
					}
					Ref(G);
					I = Close + 1;
					continue;
				}
				if (N >= u'0' && N <= u'9')
				{
					int32_t G = N - u'0';
					I += 2;
					if (I < T.size() && T[I] >= u'0' && T[I] <= u'9' && G * 10 + (T[I] - u'0') < Q.Prog.Groups)
					{
						G = G * 10 + (T[I] - u'0');
						++I;
					}
					if (G >= Q.Prog.Groups)
					{
						return TemplateError(Err, Start, u"invalid group reference");
					}
					Ref(G);
					continue;
				}
			}
			size_t Units = 1;
			const char32_t Cp = DecodeAt(T, I, Units);
			Lit(Cp);
			I += Units;
		}
		return true;
	}

	void ApplyCase(std::u16string_view Matched, std::u16string& Out)
	{
		// All cased letters upper: upper; first cased letter upper: capitalise; else as typed (§4.4 preserve case).
		bool bAnyCased = false;
		bool bAllUpper = true;
		bool bFirstUpper = false;
		bool bFirstSeen = false;
		size_t I = 0;
		while (I < Matched.size())
		{
			size_t Units = 1;
			const char32_t C = DecodeAt(Matched, I, Units);
			I += Units;
			const bool bUpper = LowerCp(C) != C;
			const bool bLower = UpperCp(C) != C;
			if (!bUpper && !bLower)
			{
				continue;
			}
			bAnyCased = true;
			if (!bFirstSeen)
			{
				bFirstSeen = true;
				bFirstUpper = bUpper;
			}
			bAllUpper = bAllUpper && bUpper;
		}
		if (!bAnyCased)
		{
			return;
		}
		std::u16string Result;
		bool bDoneFirst = false;
		I = 0;
		while (I < Out.size())
		{
			size_t Units = 1;
			char32_t C = DecodeAt(Out, I, Units);
			I += Units;
			if (bAllUpper && Matched.size() > 1)
			{
				C = UpperCp(C);
			}
			else if (bFirstUpper && !bDoneFirst && (UpperCp(C) != C || LowerCp(C) != C))
			{
				C = UpperCp(C);
				bDoneFirst = true;
			}
			else if (UpperCp(C) != C || LowerCp(C) != C)
			{
				bDoneFirst = true;
			}
			if (C >= 0x10000)
			{
				Result.push_back(static_cast<char16_t>(0xD800 + ((C - 0x10000) >> 10)));
				Result.push_back(static_cast<char16_t>(0xDC00 + ((C - 0x10000) & 0x3FF)));
			}
			else
			{
				Result.push_back(static_cast<char16_t>(C));
			}
		}
		Out = std::move(Result);
	}

	void Expand(std::u16string_view Text, const std::vector<TemplatePart>& Parts, const Matcher* M, Offset At, Offset End,
		bool bPreserveCase, std::u16string& Out)
	{
		Out.clear();
		for (const TemplatePart& P : Parts)
		{
			if (P.Group < 0)
			{
				Out += P.Literal;
				continue;
			}
			Offset S = -1;
			Offset E = -1;
			if (P.Group == 0)
			{
				S = At;
				E = End;
			}
			else if (M != nullptr)
			{
				S = M->Slots()[static_cast<size_t>(2 * P.Group)];
				E = M->Slots()[static_cast<size_t>(2 * P.Group + 1)];
			}
			if (S >= 0 && E >= S)
			{
				Out.append(Text.substr(static_cast<size_t>(S), static_cast<size_t>(E - S)));
			}
		}
		if (bPreserveCase)
		{
			ApplyCase(Text.substr(static_cast<size_t>(At), static_cast<size_t>(End - At)), Out);
		}
	}

	/** The snapshot as one contiguous run: a view when it already is one, else a copy kept in Storage (§4.4 Run). */
	std::u16string_view Contiguous(const TextSnapshot& Snapshot, std::u16string& Storage)
	{
		const std::u16string_view View = Snapshot.Chunk(0, Snapshot.Length());
		if (static_cast<int64_t>(View.size()) == Snapshot.Length())
		{
			return View;
		}
		Storage = Snapshot.Text();
		return Storage;
	}
}

namespace dream
{
	using namespace search_detail;

	CompileResult SearchEngine::Compile(const SearchQuery& Query)
	{
		CompileResult R;
		if (Query.Pattern.empty())
		{
			R.Error.Code = SearchErrorCode::EmptyPattern;
			return R;
		}
		auto Q = std::make_shared<CompiledQuery>();
		Q->Query = Query;
		Q->bLiteral = !Query.bRegex;
		if (Q->bLiteral)
		{
			Q->Needle = Query.Pattern;
			size_t I = 0;
			while (I < Query.Pattern.size())
			{
				size_t Units = 1;
				Q->FoldedNeedle.push_back(FoldCp(DecodeAt(Query.Pattern, I, Units)));
				I += Units;
			}
			CompileLiteral(Query.Pattern, !Query.bMatchCase, Q->Prog);
			Q->Prog.bCanMatchNewline = Query.Pattern.find(u'\n') != std::u16string::npos;
		}
		else if (!CompileRegex(Query.Pattern, !Query.bMatchCase, Q->Prog, R.Error))
		{
			return R;
		}
		R.Query = std::move(Q);
		return R;
	}

	bool SearchEngine::CanMatchNewline(const CompiledQuery& Query)
	{
		return Query.Prog.bCanMatchNewline;
	}

	int32_t SearchEngine::GroupCount(const CompiledQuery& Query)
	{
		return Query.Prog.Groups;
	}

	std::vector<SearchMatch> SearchEngine::FindAll(std::u16string_view Text, const CompiledQuery& Query, Offset From, Offset To,
		SearchSummary* OutSummary, const CancelToken* Cancel, const SearchLimits& Limits)
	{
		std::vector<SearchMatch> Out;
		SearchSummary Sum;
		Scan(Text, Query, From, To, Limits, Cancel, Sum, [&](Offset At, Offset End, const Matcher*)
		{
			if (Out.size() >= Limits.MaxMatches)
			{
				Sum.bCapped = true;
				return false;
			}
			Out.push_back(SearchMatch{At, End - At});
			return true;
		});
		Sum.Count = Out.size();
		if (OutSummary != nullptr)
		{
			*OutSummary = Sum;
		}
		return Out;
	}

	std::vector<SearchMatch> SearchEngine::FindAll(const TextSnapshot& Snapshot, const CompiledQuery& Query, SearchSummary* OutSummary,
		const CancelToken* Cancel)
	{
		std::u16string Storage;
		return FindAll(Contiguous(Snapshot, Storage), Query, 0, -1, OutSummary, Cancel);
	}

	SearchSummary SearchEngine::Run(const TextSnapshot& Snapshot, const CompiledQuery& Query, Offset PriorityFirst, Offset PriorityLast,
		const CancelToken* Cancel, const std::function<void(std::span<const SearchMatch>)>& Sink, const SearchLimits& Limits)
	{
		std::u16string Storage;
		const std::u16string_view Text = Contiguous(Snapshot, Storage);
		const Offset Size = static_cast<Offset>(Text.size());
		SearchSummary Sum;
		std::vector<SearchMatch> Batch;
		Clock::time_point LastFlush = Clock::now();
		auto Flush = [&]()
		{
			if (Sink)
			{
				Sink(std::span<const SearchMatch>(Batch.data(), Batch.size()));
			}
			Batch.clear();
			LastFlush = Clock::now();
		};
		auto Phase = [&](Offset From, Offset To)
		{
			if (From > To || Sum.bCapped || Sum.bCancelled)
			{
				return;
			}
			Scan(Text, Query, From, To, Limits, Cancel, Sum, [&](Offset At, Offset End, const Matcher*)
			{
				if (Sum.Count >= Limits.MaxMatches)
				{
					Sum.bCapped = true;
					return false;
				}
				Batch.push_back(SearchMatch{At, End - At});
				++Sum.Count;
				if ((Batch.size() & 255) == 0 && Clock::now() - LastFlush >= std::chrono::milliseconds(5))
				{
					Flush();
				}
				return true;
			});
		};
		if (!Query.Prog.bCanMatchNewline)
		{
			// Whole lines, so a match never straddles a phase boundary and the result equals one full pass.
			const Offset P0 = Snapshot.LineStart(Snapshot.LineOf(std::clamp<Offset>(PriorityFirst, 0, Size)));
			const Offset P1 = Snapshot.LineEnd(Snapshot.LineOf(std::clamp<Offset>(std::max(PriorityFirst, PriorityLast), 0, Size)));
			Phase(P0, P1);
			Flush();   // the visible lines go out at once
			Phase(P1 + 1, Size);
			Phase(0, P0 - 1);
		}
		else
		{
			Phase(0, Size);
		}
		Flush();
		return Sum;
	}

	std::optional<SearchMatch> SearchEngine::FindNext(const TextSnapshot& Snapshot, const CompiledQuery& Query, Offset From,
		bool bBackward, bool bWrap)
	{
		std::u16string Storage;
		const std::u16string_view Text = Contiguous(Snapshot, Storage);
		const Offset Size = static_cast<Offset>(Text.size());
		From = std::clamp<Offset>(From, 0, Size);
		SearchSummary Sum;
		std::optional<SearchMatch> Found;
		const SearchLimits Limits;
		if (!bBackward)
		{
			// Empty matches are skipped: the find bar steps between real matches.
			auto First = [&](Offset A, Offset B)
			{
				Scan(Text, Query, A, B, Limits, nullptr, Sum, [&](Offset At, Offset End, const Matcher*)
				{
					if (End == At)
					{
						return true;
					}
					Found = SearchMatch{At, End - At};
					return false;
				});
			};
			First(From, Size);
			if (!Found && bWrap && From > 0)
			{
				First(0, From - 1);
			}
			return Found;
		}
		auto Last = [&](Offset Limit)
		{
			Scan(Text, Query, 0, Limit, Limits, nullptr, Sum, [&](Offset At, Offset End, const Matcher*)
			{
				if (End > At && End <= Limit)
				{
					Found = SearchMatch{At, End - At};
				}
				return End <= Limit;
			});
		};
		Last(From);
		if (!Found && bWrap)
		{
			Last(Size);
		}
		return Found;
	}

	void SearchEngine::UpdateAfterEdit(std::vector<SearchMatch>& Matches, const ChangeSet& Changes, const TextSnapshot& After,
		const CompiledQuery& Query)
	{
		if (Changes.IsEmpty())
		{
			return;
		}
		std::u16string Storage;
		const std::u16string_view Text = Contiguous(After, Storage);
		if (Query.Prog.bCanMatchNewline)
		{
			Matches = FindAll(Text, Query);
			return;
		}
		// Shift through the ChangeSet; a match that overlaps or touches an edited range goes.
		std::vector<SearchMatch> Kept;
		Kept.reserve(Matches.size());
		size_t R = 0;
		int64_t Delta = 0;
		for (const SearchMatch& M : Matches)
		{
			while (R < Changes.Ranges.size() && Changes.Ranges[R].At + Changes.Ranges[R].OldLen < M.At)
			{
				Delta += Changes.Ranges[R].NewLen - Changes.Ranges[R].OldLen;
				++R;
			}
			bool bTouched = false;
			for (size_t K = R; K < Changes.Ranges.size() && Changes.Ranges[K].At <= M.At + M.Len; ++K)
			{
				bTouched = true;   // its range ends at or after the match start and starts at or before its end
			}
			if (!bTouched)
			{
				Kept.push_back(SearchMatch{M.At + Delta, M.Len});
			}
		}
		// The edited lines +-1, in After's coordinates, merged.
		std::vector<std::pair<LineNo, LineNo>> Dirty;
		Delta = 0;
		for (const ChangeRange& C : Changes.Ranges)
		{
			const Offset PostAt = C.At + Delta;
			const LineNo First = std::max<LineNo>(0, After.LineOf(PostAt) - 1);
			const LineNo Last = std::min<LineNo>(After.LineCount() - 1, After.LineOf(PostAt + C.NewLen) + 1);
			if (!Dirty.empty() && First <= Dirty.back().second + 1)
			{
				Dirty.back().second = std::max(Dirty.back().second, Last);
			}
			else
			{
				Dirty.emplace_back(First, Last);
			}
			Delta += C.NewLen - C.OldLen;
		}
		std::vector<SearchMatch> Out;
		Out.reserve(Kept.size() + 16);
		size_t K = 0;
		for (const auto& [First, Last] : Dirty)
		{
			const Offset A = After.LineStart(First);
			const Offset B = After.LineEnd(Last);
			while (K < Kept.size() && Kept[K].At < A)
			{
				Out.push_back(Kept[K++]);
			}
			while (K < Kept.size() && Kept[K].At <= B)
			{
				++K;   // rescanned below
			}
			const std::vector<SearchMatch> Fresh = FindAll(Text, Query, A, B);
			Out.insert(Out.end(), Fresh.begin(), Fresh.end());
		}
		while (K < Kept.size())
		{
			Out.push_back(Kept[K++]);
		}
		Matches = std::move(Out);
	}

	bool SearchEngine::ValidateTemplate(const CompiledQuery& Query, std::u16string_view Template, SearchError* OutError)
	{
		std::vector<TemplatePart> Parts;
		return ParseTemplate(Query, Template, Parts, OutError);
	}

	bool SearchEngine::ExpandReplacement(std::u16string_view Text, const CompiledQuery& Query, const SearchMatch& Match,
		std::u16string_view Template, bool bPreserveCase, std::u16string& Out, SearchError* OutError)
	{
		std::vector<TemplatePart> Parts;
		if (!ParseTemplate(Query, Template, Parts, OutError))
		{
			return false;
		}
		const Offset Size = static_cast<Offset>(Text.size());
		if (Match.At < 0 || Match.Len < 0 || Match.At + Match.Len > Size)
		{
			return TemplateError(OutError, 0, u"the match is outside the text");
		}
		if (Query.bLiteral)
		{
			if (LiteralAt(Text, Query, Match.At) != Match.Len)
			{
				return TemplateError(OutError, 0, u"no match at that place");
			}
			Expand(Text, Parts, nullptr, Match.At, Match.At + Match.Len, bPreserveCase, Out);
			return true;
		}
		Matcher M(Query.Prog, Text, SearchLimits());
		Offset End = 0;
		if (M.MatchAt(Match.At, false, Match.At + Match.Len, End, Query.Query.bWholeWord) != Status::Matched)
		{
			return TemplateError(OutError, 0, u"no match at that place");
		}
		Expand(Text, Parts, &M, Match.At, End, bPreserveCase, Out);
		return true;
	}

	EditBatch SearchEngine::BuildReplaceAll(const TextSnapshot& Snapshot, const CompiledQuery& Query, std::u16string_view Template,
		bool bPreserveCase, Offset From, Offset To, SearchError* OutError)
	{
		EditBatch Batch;
		Batch.Origin = EditOrigin::Command;
		std::vector<TemplatePart> Parts;
		if (!ParseTemplate(Query, Template, Parts, OutError))
		{
			return Batch;
		}
		std::u16string Storage;
		const std::u16string_view Text = Contiguous(Snapshot, Storage);
		SearchSummary Sum;
		std::u16string Replacement;
		Scan(Text, Query, From, To, SearchLimits(), nullptr, Sum, [&](Offset At, Offset End, const Matcher* M)
		{
			Expand(Text, Parts, M, At, End, bPreserveCase, Replacement);
			Batch.Ranges.push_back(EditRange{At, End - At, Replacement});
			return true;
		});
		return Batch;
	}
}
