// SPDX-License-Identifier: Apache-2.0
// Dream core: completion merge and ranking, the fuzzy matcher, the keyword table and the chain extractor
// (TDD §7.5.3, §7.5.5, §7.6.1). See dream/CompletionMerge.h.
#include "dream/CompletionMerge.h"

#include "dream/PyLexer.h"
#include "dream/Utf.h"

#include <algorithm>
#include <climits>
#include <map>

namespace dream
{
	namespace completion_tables
	{
		// dir(builtins) of the shipped 3.11.8 (the lexer's generated table, read-only reuse).
#include "PyBuiltins.inc"

		// keyword.kwlist and keyword.softkwlist of 3.11.
		inline constexpr const char* HardKeywords[] = {
			"False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
			"elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
			"not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
		};
		inline constexpr const char* SoftKeywords[] = { "_", "case", "match" };
	}

	namespace
	{
		constexpr int32_t NegInf = INT32_MIN / 4;
		constexpr int32_t PrefixBonus = 32;
		constexpr int32_t WordStartBonus = 40;
		constexpr int32_t ConsecutiveBonus = 8;
		constexpr int32_t ExactCaseBonus = 4;

		char FoldAscii(char C)
		{
			return (C >= 'A' && C <= 'Z') ? static_cast<char>(C - 'A' + 'a') : C;
		}

		bool IsLowerAscii(char C) { return C >= 'a' && C <= 'z'; }
		bool IsUpperAscii(char C) { return C >= 'A' && C <= 'Z'; }
		bool IsDigitAscii(char C) { return C >= '0' && C <= '9'; }
		bool IsAlnumAscii(char C) { return IsLowerAscii(C) || IsUpperAscii(C) || IsDigitAscii(C); }

		/** String start, after '_' or any other separator, or a camelCase hump (upper case after lower case or a digit). */
		bool IsWordStart(std::string_view Label, size_t Index)
		{
			if (Index == 0)
			{
				return true;
			}
			const char Prev = Label[Index - 1];
			const char Cur = Label[Index];
			if (Prev == '_')
			{
				return Cur != '_';
			}
			if (IsUpperAscii(Cur) && (IsLowerAscii(Prev) || IsDigitAscii(Prev)))
			{
				return true;
			}
			// Other ASCII separators ('.', '/', '-', ' ') start a word too (the Go To palette scores paths).
			const bool bPrevSeparator = static_cast<unsigned char>(Prev) < 0x80 && !IsAlnumAscii(Prev);
			return bPrevSeparator && static_cast<unsigned char>(Cur) < 0x80 && IsAlnumAscii(Cur);
		}

		bool StartsWithFolded(std::string_view Text, std::string_view Prefix)
		{
			if (Prefix.size() > Text.size())
			{
				return false;
			}
			for (size_t Index = 0; Index < Prefix.size(); ++Index)
			{
				if (FoldAscii(Text[Index]) != FoldAscii(Prefix[Index]))
				{
					return false;
				}
			}
			return true;
		}

		bool IsDunder(std::string_view Label)
		{
			return Label.size() > 4 && Label.substr(0, 2) == "__" && Label.substr(Label.size() - 2) == "__";
		}

		/**
		 * 0 normal, 1 a leading '_', 2 a dunder (T-MRG-012). A prefix starting with '_' lifts the '_' names, and dunders
		 * still come after them; a prefix starting with "__" lifts dunders too.
		 */
		int32_t DemotionTier(std::string_view Label, std::string_view Prefix)
		{
			if (Prefix.size() >= 2 && Prefix.substr(0, 2) == "__")
			{
				return 0;
			}
			const bool bDunder = IsDunder(Label);
			if (!Prefix.empty() && Prefix.front() == '_')
			{
				return bDunder ? 1 : 0;
			}
			if (bDunder)
			{
				return 2;
			}
			return (!Label.empty() && Label.front() == '_') ? 1 : 0;
		}

		CompletionBadge BadgeOf(const MergedItem& Item)
		{
			if (Item.bHasStatic)
			{
				return CompletionBadge::Static;
			}
			if (Item.bHasLive)
			{
				return CompletionBadge::Runtime;
			}
			return Item.Item.Source == CompletionSource::Snippet ? CompletionBadge::Snippet : CompletionBadge::Keyword;
		}

		/** Snippets match by prefix only (§7.6.1); everything else through the fuzzy matcher. */
		bool Matches(const CompletionCandidate& Candidate, CompletionSource Source, std::string_view Prefix, int32_t& OutScore)
		{
			const std::string& Filter = Candidate.GetFilterText();
			if (Source == CompletionSource::Snippet && !StartsWithFolded(Filter, Prefix))
			{
				return false;
			}
			return FuzzyScore(Prefix, Filter, OutScore);
		}

		ScopeGroup BetterGroup(ScopeGroup A, ScopeGroup B, const MergeContext& Context)
		{
			return GroupRank(A, Context.Kind, Context.bRootBoundInSession) <= GroupRank(B, Context.Kind, Context.bRootBoundInSession) ? A : B;
		}

		void TakeContent(CompletionCandidate& To, const CompletionCandidate& From)
		{
			if (!From.FilterText.empty())
			{
				To.FilterText = From.FilterText;
			}
			if (!From.SortText.empty())
			{
				To.SortText = From.SortText;
			}
			if (!From.Detail.empty())
			{
				To.Detail = From.Detail;
			}
			if (!From.Documentation.empty())
			{
				To.Documentation = From.Documentation;
			}
			if (!From.InsertText.empty())
			{
				To.InsertText = From.InsertText;
				To.bInsertIsSnippet = From.bInsertIsSnippet;
			}
			if (From.ResolveHandle != 0)
			{
				To.ResolveHandle = From.ResolveHandle;
			}
			To.Kind = From.Kind;
		}

		/** Folds an incoming candidate into the item that already has its label (§7.6.1 key rule). */
		void MergeInto(MergedItem& Existing, const CompletionCandidate& Incoming, CompletionSource Source, const MergeContext& Context)
		{
			CompletionCandidate& Item = Existing.Item;
			const ScopeGroup Group = BetterGroup(Item.Group, Incoming.Group, Context);
			switch (Source)
			{
			case CompletionSource::Static:
				// The static item supplies detail, documentation and insert text; the live type stays (T-MRG-002).
				TakeContent(Item, Incoming);
				Item.Source = CompletionSource::Static;
				Existing.bHasStatic = true;
				break;
			case CompletionSource::Live:
				if (Existing.bHasStatic)
				{
					if (!Incoming.RuntimeType.empty())
					{
						Item.RuntimeType = Incoming.RuntimeType;
					}
				}
				else
				{
					// A newer live answer, or live replacing a keyword or snippet entry of the same name.
					const std::string Label = Item.Label;
					Item = Incoming;
					Item.Label = Label;
					Item.Source = CompletionSource::Live;
				}
				Existing.bHasLive = true;
				break;
			case CompletionSource::Keyword:
			case CompletionSource::Snippet:
				if (!Existing.bHasLive && !Existing.bHasStatic && Item.Source == Source)
				{
					const std::string Label = Item.Label;
					Item = Incoming;
					Item.Label = Label;
					Item.Source = Source;
				}
				break;
			}
			Item.Group = Group;
			Existing.Badge = BadgeOf(Existing);
		}

		struct RankKey
		{
			int32_t Group = 0;
			int32_t Demotion = 0;
			int32_t Score = 0;
			const std::string* Sort = nullptr;
			const std::string* Label = nullptr;
		};

		bool RanksBefore(const RankKey& A, const RankKey& B)
		{
			if (A.Group != B.Group)
			{
				return A.Group < B.Group;
			}
			if (A.Demotion != B.Demotion)
			{
				return A.Demotion < B.Demotion;
			}
			if (A.Score != B.Score)
			{
				return A.Score > B.Score;
			}
			// An item with a sortText ranks by it; one without sorts after those that have one.
			const bool bASort = !A.Sort->empty();
			const bool bBSort = !B.Sort->empty();
			if (bASort != bBSort)
			{
				return bASort;
			}
			if (bASort && *A.Sort != *B.Sort)
			{
				return *A.Sort < *B.Sort;
			}
			return *A.Label < *B.Label;
		}
	}

	int32_t GroupRank(ScopeGroup Group, BufferKind Kind, bool bRootBoundInSession)
	{
		const bool bFileOrder = Kind == BufferKind::File && !bRootBoundInSession;
		switch (Group)
		{
		case ScopeGroup::Scope:
			return 0;
		case ScopeGroup::LiveMembers:
			return bFileOrder ? 2 : 1;
		case ScopeGroup::StaticMembers:
			return bFileOrder ? 1 : 2;
		case ScopeGroup::KeywordsAndSnippets:
		default:
			return 3;
		}
	}

	bool FuzzyScore(std::string_view Query, std::string_view Label, int32_t& OutScore)
	{
		OutScore = 0;
		if (Query.empty())
		{
			return true;
		}
		const size_t L = std::min(Label.size(), static_cast<size_t>(FuzzyMaxLabelBytes));
		const size_t Q = Query.size();
		if (Q > L)
		{
			return false;
		}
		{
			// Cheap reject first: most labels of a 20,000-name list do not contain the query as a subsequence.
			size_t Matched = 0;
			for (size_t J = 0; J < L && Matched < Q; ++J)
			{
				if (FoldAscii(Label[J]) == FoldAscii(Query[Matched]))
				{
					++Matched;
				}
			}
			if (Matched < Q)
			{
				return false;
			}
		}
		if (StartsWithFolded(Label.substr(0, L), Query))
		{
			// A whole-label prefix scores along its contiguous alignment: a later word-start jump inside the same label
			// ("EditorAss" -> the 'S' of EditorAssetSubsystem) must not outrank another label with the same typed prefix.
			int32_t Total = PrefixBonus;
			for (size_t I = 0; I < Q; ++I)
			{
				Total += (IsWordStart(Label, I) ? WordStartBonus : 0) + (Label[I] == Query[I] ? ExactCaseBonus : 0)
					+ (I > 0 ? ConsecutiveBonus : 0);
			}
			OutScore = Total;
			return true;
		}

		// Best[j]: the best score with the current query character matched at label byte j (NegInf: impossible).
		int32_t Prev[FuzzyMaxLabelBytes];
		int32_t Cur[FuzzyMaxLabelBytes];
		for (size_t I = 0; I < Q; ++I)
		{
			const char QC = Query[I];
			const char QF = FoldAscii(QC);
			int32_t RunningMax = NegInf;    // max over j' < j - 1 of Prev[j'] + j' (the skip penalty is j' + 1 - j)
			for (size_t J = 0; J < L; ++J)
			{
				if (I > 0 && J >= 2 && Prev[J - 2] > NegInf)
				{
					RunningMax = std::max(RunningMax, Prev[J - 2] + static_cast<int32_t>(J - 2));
				}
				Cur[J] = NegInf;
				const char LC = Label[J];
				if (FoldAscii(LC) != QF)
				{
					continue;
				}
				const int32_t Bonus = (IsWordStart(Label, J) ? WordStartBonus : 0) + (LC == QC ? ExactCaseBonus : 0);
				if (I == 0)
				{
					Cur[J] = Bonus;     // skips before the first match are free
					continue;
				}
				int32_t Best = NegInf;
				if (J >= 1 && Prev[J - 1] > NegInf)
				{
					Best = Prev[J - 1] + ConsecutiveBonus;
				}
				if (RunningMax > NegInf)
				{
					Best = std::max(Best, RunningMax + 1 - static_cast<int32_t>(J));
				}
				if (Best > NegInf)
				{
					Cur[J] = Best + Bonus;
				}
			}
			std::copy(Cur, Cur + L, Prev);
		}
		int32_t Best = NegInf;
		for (size_t J = 0; J < L; ++J)
		{
			Best = std::max(Best, Prev[J]);
		}
		if (Best <= NegInf)
		{
			return false;
		}
		OutScore = Best;
		return true;
	}

	bool CanRefilterLocally(bool bIncomplete, std::string_view OldPrefix, std::string_view NewPrefix)
	{
		return !bIncomplete && NewPrefix.size() >= OldPrefix.size() && NewPrefix.substr(0, OldPrefix.size()) == OldPrefix;
	}

	const char* BadgeName(CompletionBadge Badge)
	{
		switch (Badge)
		{
		case CompletionBadge::Runtime: return "runtime";
		case CompletionBadge::Static: return "static";
		case CompletionBadge::Keyword: return "keyword";
		case CompletionBadge::Snippet: return "snippet";
		default: return "";
		}
	}

	std::string DisplayDetail(const MergedItem& Item)
	{
		std::string Text = Item.Item.Detail;
		if (Item.bHasStatic && Item.bHasLive && !Item.Item.RuntimeType.empty())
		{
			Text += Text.empty() ? "runtime: " : " \xC2\xB7 runtime: ";     // " · runtime: " (U+00B7)
			Text += Item.Item.RuntimeType;
		}
		return Text;
	}

	MergedList MergeCompletions(const MergedList& Previous, std::span<const CompletionCandidate> Incoming,
		CompletionSource IncomingSource, const MergeContext& Context)
	{
		MergedList Out;
		Out.bIncomplete = Previous.bIncomplete || Context.bIncomingIncomplete;

		std::vector<MergedItem> Items = Previous.Items;
		int32_t Pinned = Context.SelectedIndex;
		if (Pinned >= static_cast<int32_t>(Items.size()))
		{
			Pinned = static_cast<int32_t>(Items.size()) - 1;
		}
		uint32_t NextId = 1;
		std::map<std::string, size_t, std::less<>> ByLabel;
		for (size_t Index = 0; Index < Items.size(); ++Index)
		{
			NextId = std::max(NextId, Items[Index].Id + 1);
			ByLabel.emplace(Items[Index].Item.Label, Index);
		}

		for (const CompletionCandidate& Candidate : Incoming)
		{
			if (Candidate.Label.empty())
			{
				continue;
			}
			int32_t Score = 0;
			if (!Matches(Candidate, IncomingSource, Context.Prefix, Score))
			{
				continue;
			}
			const auto Found = ByLabel.find(Candidate.Label);
			if (Found != ByLabel.end())
			{
				MergedItem& Existing = Items[Found->second];
				MergeInto(Existing, Candidate, IncomingSource, Context);
				int32_t Rescored = 0;
				if (FuzzyScore(Context.Prefix, Existing.Item.GetFilterText(), Rescored))
				{
					Existing.Score = Rescored;
				}
				continue;
			}
			MergedItem Item;
			Item.Item = Candidate;
			Item.Item.Source = IncomingSource;
			Item.bHasLive = IncomingSource == CompletionSource::Live;
			Item.bHasStatic = IncomingSource == CompletionSource::Static;
			Item.Badge = BadgeOf(Item);
			Item.Score = Score;
			Item.Id = NextId++;
			ByLabel.emplace(Item.Item.Label, Items.size());
			Items.push_back(std::move(Item));
		}

		// Items 0..S keep identity and order; the rest is ranked below them (the no-reorder rule, D6).
		const size_t FirstRanked = Pinned >= 0 ? static_cast<size_t>(Pinned) + 1 : 0;
		std::vector<RankKey> Keys(Items.size());
		for (size_t Index = FirstRanked; Index < Items.size(); ++Index)
		{
			const CompletionCandidate& Item = Items[Index].Item;
			RankKey& Key = Keys[Index];
			Key.Group = GroupRank(Item.Group, Context.Kind, Context.bRootBoundInSession);
			Key.Demotion = DemotionTier(Item.Label, Context.Prefix);
			Key.Score = Items[Index].Score;
			Key.Sort = &Item.SortText;
			Key.Label = &Item.Label;
		}
		std::vector<size_t> Order;
		Order.reserve(Items.size() - FirstRanked);
		for (size_t Index = FirstRanked; Index < Items.size(); ++Index)
		{
			Order.push_back(Index);
		}
		std::sort(Order.begin(), Order.end(), [&Keys](size_t A, size_t B) { return RanksBefore(Keys[A], Keys[B]); });

		Out.Items.reserve(Items.size());
		for (size_t Index = 0; Index < FirstRanked; ++Index)
		{
			Out.Items.push_back(std::move(Items[Index]));
		}
		for (size_t Index : Order)
		{
			Out.Items.push_back(std::move(Items[Index]));
		}
		Out.SelectedIndex = Pinned >= 0 ? Pinned : 0;
		return Out;
	}

	std::vector<CompletionCandidate> KeywordCandidates()
	{
		std::vector<CompletionCandidate> Out;
		auto Add = [&Out](std::string Label, const char* Detail)
		{
			CompletionCandidate Candidate;
			Candidate.Label = std::move(Label);
			Candidate.Detail = Detail;
			Candidate.Kind = CompletionKind::Keyword;
			Candidate.Source = CompletionSource::Keyword;
			Candidate.Group = ScopeGroup::KeywordsAndSnippets;
			Out.push_back(std::move(Candidate));
		};
		for (const char* Word : completion_tables::HardKeywords)
		{
			Add(Word, "keyword");
		}
		for (const char* Word : completion_tables::SoftKeywords)
		{
			Add(Word, "soft keyword");
		}
		for (const char16_t* Name : completion_tables::BuiltinNames)
		{
			std::string Label;
			for (const char16_t* P = Name; *P != 0; ++P)
			{
				Label.push_back(static_cast<char>(*P));   // the table is ASCII
			}
			bool bKeyword = false;
			for (const char* Word : completion_tables::HardKeywords)
			{
				bKeyword = bKeyword || Label == Word;      // True, False, None are both
			}
			if (!bKeyword)
			{
				Add(std::move(Label), "builtin");
			}
		}
		return Out;
	}

	// ---- Chain extraction -----------------------------------------------------------------------------------------

	std::string CompletionChain::Key() const
	{
		std::string Out;
		switch (Context)
		{
		case ChainContext::Name: Out = "n"; break;
		case ChainContext::Member: Out = "m"; break;
		case ChainContext::StringArg: Out = "s"; break;
		default: Out = "-"; break;
		}
		for (const ChainStep& Step : Steps)
		{
			switch (Step.Kind)
			{
			case ChainStepKind::Name: Out += "|n:"; Out += Step.Text; break;
			case ChainStepKind::Attr: Out += "|a:"; Out += Step.Text; break;
			case ChainStepKind::Call: Out += "|c"; break;
			case ChainStepKind::Sub:
				if (Step.bIntKey)
				{
					Out += "|i:";
					Out += std::to_string(Step.Int);
				}
				else
				{
					Out += "|s:";
					Out += Step.Text;
				}
				break;
			}
		}
		if (Context == ChainContext::StringArg)
		{
			Out += "|f:";
			Out += Func;
			Out += "#";
			Out += std::to_string(ArgIndex);
		}
		return Out;
	}

	namespace
	{
		bool IsStringKind(TokenKind Kind)
		{
			return Kind == TokenKind::String || Kind == TokenKind::Docstring || Kind == TokenKind::StringEscape
				|| Kind == TokenKind::FStringBrace || Kind == TokenKind::FStringConversion || Kind == TokenKind::FStringSpec;
		}

		bool IsIdentUnit(char16_t C)
		{
			// ASCII fast path; other BMP letters through the XID tables (astral identifiers are not completed in M1).
			if (C < 0x80)
			{
				return (C >= u'a' && C <= u'z') || (C >= u'A' && C <= u'Z') || (C >= u'0' && C <= u'9') || C == u'_';
			}
			if (utf::IsHighSurrogate(C) || utf::IsLowSurrogate(C))
			{
				return false;
			}
			return utf::IsIdentifierContinue(static_cast<char32_t>(C));
		}

		bool IsIdentStartUnit(char16_t C)
		{
			if (C < 0x80)
			{
				return (C >= u'a' && C <= u'z') || (C >= u'A' && C <= u'Z') || C == u'_';
			}
			return IsIdentUnit(C) && utf::IsIdentifierStart(static_cast<char32_t>(C));
		}

		std::string ToUtf8(std::u16string_view Text)
		{
			std::string Out;
			utf::Utf16ToUtf8(Text, Out);
			return Out;
		}

		bool IsHardKeyword(std::string_view Word)
		{
			for (const char* Keyword : completion_tables::HardKeywords)
			{
				if (Word == Keyword)
				{
					return true;
				}
			}
			return false;
		}

		/** The text before Caret with a token kind per column (PyLexer, §7.5.3). */
		struct LineScan
		{
			std::u16string_view Text;
			std::vector<TokenKind> Kinds;
			std::vector<int32_t> TokenStarts;   // start column of the token covering each column

			explicit LineScan(std::u16string_view InText)
				: Text(InText)
			{
				Kinds.assign(Text.size(), TokenKind::Text);
				TokenStarts.assign(Text.size(), 0);
				std::vector<std::vector<uint32_t>> Tokens;
				std::vector<LineFacts> Facts;
				LexStateId EndState = NormalState;
				LexStateInfo EndInfo;
				PyLexer::LexText(Text, LexGrammar::Py311, Tokens, Facts, EndState, EndInfo);
				if (Tokens.empty())
				{
					return;
				}
				const std::vector<uint32_t>& Line = Tokens.front();
				for (size_t Index = 0; Index < Line.size(); ++Index)
				{
					const int32_t Start = std::min<int32_t>(TokenStart(Line[Index]), static_cast<int32_t>(Text.size()));
					const int32_t End = Index + 1 < Line.size()
						? std::min<int32_t>(TokenStart(Line[Index + 1]), static_cast<int32_t>(Text.size()))
						: static_cast<int32_t>(Text.size());
					for (int32_t Col = Start; Col < End; ++Col)
					{
						Kinds[static_cast<size_t>(Col)] = TokenKindOf(Line[Index]);
						TokenStarts[static_cast<size_t>(Col)] = Start;
					}
				}
			}

			bool IsCode(int32_t Col) const
			{
				const TokenKind Kind = Kinds[static_cast<size_t>(Col)];
				return Kind != TokenKind::Comment && !IsStringKind(Kind) && Kind != TokenKind::Error;
			}

			int32_t SkipBlanksBack(int32_t Col) const
			{
				while (Col >= 0 && utf::IsPyBlank(Text[static_cast<size_t>(Col)]))
				{
					--Col;
				}
				return Col;
			}

			/** The identifier ending at Col (inclusive): its start column, or -1 when Col is not in an identifier. */
			int32_t IdentifierStart(int32_t Col) const
			{
				if (Col < 0 || !IsCode(Col) || !IsIdentUnit(Text[static_cast<size_t>(Col)]))
				{
					return -1;
				}
				int32_t Start = Col;
				while (Start > 0 && IsCode(Start - 1) && IsIdentUnit(Text[static_cast<size_t>(Start - 1)]))
				{
					--Start;
				}
				return IsIdentStartUnit(Text[static_cast<size_t>(Start)]) ? Start : -1;
			}

			/** The bracket that opens the code bracket Close at Col; -1 when unbalanced on this line. */
			int32_t MatchOpen(int32_t Col, char16_t Open, char16_t Close) const
			{
				int32_t Depth = 0;
				for (int32_t Index = Col; Index >= 0; --Index)
				{
					if (!IsCode(Index))
					{
						continue;
					}
					const char16_t C = Text[static_cast<size_t>(Index)];
					if (C == Close)
					{
						++Depth;
					}
					else if (C == Open)
					{
						if (--Depth == 0)
						{
							return Index;
						}
					}
				}
				return -1;
			}
		};

		/** A literal subscript key between '[' (Open) and ']' (Close): an int, a negative int or a plain quoted string. */
		bool ParseLiteralKey(const LineScan& Scan, int32_t Open, int32_t Close, ChainStep& OutStep)
		{
			std::u16string_view Inner = Scan.Text.substr(static_cast<size_t>(Open + 1), static_cast<size_t>(Close - Open - 1));
			while (!Inner.empty() && utf::IsPyBlank(Inner.front()))
			{
				Inner.remove_prefix(1);
			}
			while (!Inner.empty() && utf::IsPyBlank(Inner.back()))
			{
				Inner.remove_suffix(1);
			}
			if (Inner.empty())
			{
				return false;
			}
			OutStep.Kind = ChainStepKind::Sub;
			const char16_t First = Inner.front();
			if (First == u'\'' || First == u'"')
			{
				if (Inner.size() < 2 || Inner.back() != First)
				{
					return false;
				}
				const std::u16string_view Value = Inner.substr(1, Inner.size() - 2);
				for (char16_t C : Value)
				{
					if (C == First || C == u'\\')
					{
						return false;       // escapes and nested quotes are not literal keys here
					}
				}
				OutStep.Text = ToUtf8(Value);
				OutStep.bIntKey = false;
				return true;
			}
			size_t Index = 0;
			bool bNegative = false;
			if (Inner[0] == u'-')
			{
				bNegative = true;
				Index = 1;
			}
			if (Index >= Inner.size() || Inner.size() - Index > 18)
			{
				return false;
			}
			int64_t Value = 0;
			for (; Index < Inner.size(); ++Index)
			{
				const char16_t C = Inner[Index];
				if (C < u'0' || C > u'9')
				{
					return false;
				}
				Value = Value * 10 + (C - u'0');
			}
			OutStep.Int = bNegative ? -Value : Value;
			OutStep.bIntKey = true;
			return true;
		}

		/**
		 * The receiver chain that ends at Col (inclusive, the character before the '.' or the '(' of a string argument),
		 * root first. False when the expression there is not a supported chain.
		 */
		bool ParseReceiver(const LineScan& Scan, int32_t Col, std::vector<ChainStep>& OutSteps)
		{
			std::vector<ChainStep> Reversed;
			int32_t Pos = Scan.SkipBlanksBack(Col);
			while (true)
			{
				if (Pos < 0 || static_cast<int32_t>(Reversed.size()) >= MaxChainSteps)
				{
					return false;
				}
				const char16_t C = Scan.Text[static_cast<size_t>(Pos)];
				if (Scan.IsCode(Pos) && (C == u')' || C == u']'))
				{
					const bool bCall = C == u')';
					const int32_t Open = Scan.MatchOpen(Pos, bCall ? u'(' : u'[', C);
					if (Open < 0)
					{
						return false;
					}
					ChainStep Step;
					if (bCall)
					{
						Step.Kind = ChainStepKind::Call;
					}
					else if (!ParseLiteralKey(Scan, Open, Pos, Step))
					{
						return false;
					}
					Reversed.push_back(std::move(Step));
					// A call or subscript needs a callee directly before it (no blank: "a [0]" is rare and "x (y)" is a call
					// of whatever x is either way).
					Pos = Open - 1;
					if (Pos < 0 || !Scan.IsCode(Pos))
					{
						return false;
					}
					const char16_t Before = Scan.Text[static_cast<size_t>(Pos)];
					if (Before != u')' && Before != u']' && !IsIdentUnit(Before))
					{
						return false;
					}
					continue;
				}
				const int32_t Start = Scan.IdentifierStart(Pos);
				if (Start < 0)
				{
					return false;
				}
				ChainStep Step;
				Step.Kind = ChainStepKind::Attr;
				Step.Text = ToUtf8(Scan.Text.substr(static_cast<size_t>(Start), static_cast<size_t>(Pos - Start + 1)));
				if (IsDigitAscii(Step.Text.front()))
				{
					return false;
				}
				Reversed.push_back(std::move(Step));
				const int32_t Dot = Scan.SkipBlanksBack(Start - 1);
				if (Dot >= 0 && Scan.IsCode(Dot) && Scan.Text[static_cast<size_t>(Dot)] == u'.')
				{
					Pos = Scan.SkipBlanksBack(Dot - 1);
					continue;
				}
				break;      // the root identifier
			}
			ChainStep& Root = Reversed.back();
			if (Root.Kind != ChainStepKind::Attr || IsHardKeyword(Root.Text))
			{
				return false;
			}
			Root.Kind = ChainStepKind::Name;
			OutSteps.assign(Reversed.rbegin(), Reversed.rend());
			return true;
		}

		/** True when the word before Col (skipping blanks) names something being defined or imported. */
		bool FollowsDefiningKeyword(const LineScan& Scan, int32_t Col)
		{
			const int32_t End = Scan.SkipBlanksBack(Col);
			const int32_t Start = Scan.IdentifierStart(End);
			if (Start < 0)
			{
				return false;
			}
			const std::string Word = ToUtf8(Scan.Text.substr(static_cast<size_t>(Start), static_cast<size_t>(End - Start + 1)));
			return Word == "def" || Word == "class" || Word == "import" || Word == "from" || Word == "as" || Word == "global"
				|| Word == "nonlocal";
		}

		/** The string-argument context (§7.5.4): Caret inside an unterminated plain string that is argument 0 of
		 *  get_editor_property / set_editor_property on a receiver chain. */
		CompletionChain StringArgContext(const LineScan& Scan, int32_t Caret)
		{
			CompletionChain Out;
			int32_t QuoteCol = Scan.TokenStarts[static_cast<size_t>(Caret - 1)];
			const char16_t Quote = Scan.Text[static_cast<size_t>(QuoteCol)];
			if (Quote != u'\'' && Quote != u'"')
			{
				return Out;         // prefixed (b'', f'') and broken strings are not completed
			}
			const std::u16string_view Typed = Scan.Text.substr(static_cast<size_t>(QuoteCol + 1), static_cast<size_t>(Caret - QuoteCol - 1));
			for (char16_t C : Typed)
			{
				if (C == Quote || C == u'\\')
				{
					return Out;     // closed, triple-quoted or escaped: not an editor-property name being typed
				}
			}
			const int32_t Paren = Scan.SkipBlanksBack(QuoteCol - 1);
			if (Paren < 0 || !Scan.IsCode(Paren) || Scan.Text[static_cast<size_t>(Paren)] != u'(')
			{
				return Out;
			}
			const int32_t FuncEnd = Scan.SkipBlanksBack(Paren - 1);
			const int32_t FuncStart = Scan.IdentifierStart(FuncEnd);
			if (FuncStart < 0)
			{
				return Out;
			}
			const std::string Func = ToUtf8(Scan.Text.substr(static_cast<size_t>(FuncStart), static_cast<size_t>(FuncEnd - FuncStart + 1)));
			if (Func != "get_editor_property" && Func != "set_editor_property")
			{
				return Out;
			}
			const int32_t Dot = Scan.SkipBlanksBack(FuncStart - 1);
			if (Dot < 0 || !Scan.IsCode(Dot) || Scan.Text[static_cast<size_t>(Dot)] != u'.')
			{
				return Out;
			}
			std::vector<ChainStep> Steps;
			if (!ParseReceiver(Scan, Dot - 1, Steps))
			{
				return Out;
			}
			Out.Context = ChainContext::StringArg;
			Out.Steps = std::move(Steps);
			Out.Prefix = ToUtf8(Typed);
			Out.PrefixStart = QuoteCol + 1;
			Out.Func = Func;
			Out.ArgIndex = 0;
			Out.Quote = Quote;
			return Out;
		}
	}

	CompletionChain ExtractCompletionChain(std::u16string_view Line, int32_t Caret, bool bLineStartsInString)
	{
		CompletionChain Out;
		if (bLineStartsInString)
		{
			return Out;
		}
		Caret = std::clamp<int32_t>(Caret, 0, static_cast<int32_t>(Line.size()));
		if (Caret > LongLineColumns)
		{
			return Out;
		}
		const LineScan Scan(Line.substr(0, static_cast<size_t>(Caret)));
		if (Caret == 0)
		{
			Out.Context = ChainContext::Name;   // an explicit Ctrl+Space on an empty line
			return Out;
		}

		const TokenKind Last = Scan.Kinds[static_cast<size_t>(Caret - 1)];
		if (Last == TokenKind::Comment)
		{
			return Out;
		}
		if (IsStringKind(Last) || Last == TokenKind::Error)
		{
			return StringArgContext(Scan, Caret);
		}

		// The identifier being typed (possibly empty).
		int32_t PrefixStart = Caret;
		while (PrefixStart > 0 && Scan.IsCode(PrefixStart - 1) && IsIdentUnit(Scan.Text[static_cast<size_t>(PrefixStart - 1)]))
		{
			--PrefixStart;
		}
		if (PrefixStart < Caret && !IsIdentStartUnit(Scan.Text[static_cast<size_t>(PrefixStart)]))
		{
			return Out;             // a number such as 12 or 3e5
		}
		const std::u16string_view Prefix = Scan.Text.substr(static_cast<size_t>(PrefixStart), static_cast<size_t>(Caret - PrefixStart));

		const int32_t Before = Scan.SkipBlanksBack(PrefixStart - 1);
		if (Before >= 0 && Scan.IsCode(Before) && Scan.Text[static_cast<size_t>(Before)] == u'.')
		{
			std::vector<ChainStep> Steps;
			if (!ParseReceiver(Scan, Before - 1, Steps))
			{
				return Out;         // "1.", "(a + b).", "x[i]." (a non-literal key), "from . import"
			}
			Out.Context = ChainContext::Member;
			Out.Steps = std::move(Steps);
			Out.Prefix = ToUtf8(Prefix);
			Out.PrefixStart = PrefixStart;
			return Out;
		}
		if (FollowsDefiningKeyword(Scan, PrefixStart - 1))
		{
			return Out;
		}
		if (Prefix.empty() && Before >= 0)
		{
			// Nothing typed after a closed bracket or a string: no name can start here ("f() if" still completes once a
			// letter is typed, for the keyword).
			const char16_t C = Scan.Text[static_cast<size_t>(Before)];
			if (!Scan.IsCode(Before) || C == u')' || C == u']' || C == u'}')
			{
				return Out;
			}
		}
		Out.Context = ChainContext::Name;
		Out.Prefix = ToUtf8(Prefix);
		Out.PrefixStart = PrefixStart;
		return Out;
	}
}
