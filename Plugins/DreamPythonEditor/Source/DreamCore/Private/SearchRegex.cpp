// SPDX-License-Identifier: Apache-2.0
// Dream core, private: the regex engine behind dream::SearchEngine (TDD §4.4). The parser follows CPython 3.11's
// Lib/re/_parser.py for str patterns (syntax, error wording, the octal/group-reference rules); the program is matched
// by a backtracking interpreter with an explicit stack, so a long line never overflows the C++ stack (§4.4's reason to
// reject std::regex), and every attempt is bounded by steps and stack entries.
#include "SearchRegex.h"

#include "dream/Utf.h"

#include <algorithm>
#include <limits>

namespace dream::regex_detail
{
	namespace tables
	{
#include "SearchTables.inc"
	}

	namespace
	{
		template <size_t N>
		bool InRanges(const uint32_t (&Pairs)[N], char32_t C)
		{
			size_t Lo = 0;
			size_t Hi = N / 2;
			while (Lo < Hi)
			{
				const size_t Mid = (Lo + Hi) / 2;
				if (C < Pairs[2 * Mid])
				{
					Hi = Mid;
				}
				else if (C > Pairs[2 * Mid + 1])
				{
					Lo = Mid + 1;
				}
				else
				{
					return true;
				}
			}
			return false;
		}

		template <size_t N>
		char32_t MapPair(const uint32_t (&Pairs)[N], char32_t C)
		{
			size_t Lo = 0;
			size_t Hi = N / 2;
			while (Lo < Hi)
			{
				const size_t Mid = (Lo + Hi) / 2;
				if (C < Pairs[2 * Mid])
				{
					Hi = Mid;
				}
				else if (C > Pairs[2 * Mid])
				{
					Lo = Mid + 1;
				}
				else
				{
					return Pairs[2 * Mid + 1];
				}
			}
			return C;
		}

		bool IsAsciiWord(char32_t C)
		{
			return (C >= u'a' && C <= u'z') || (C >= u'A' && C <= u'Z') || (C >= u'0' && C <= u'9') || C == u'_';
		}

		bool IsAsciiSpace(char32_t C)
		{
			return C == u' ' || (C >= 0x09 && C <= 0x0D);
		}

		bool WordAfter(std::u16string_view T, Offset Sp)
		{
			if (Sp < 0 || static_cast<size_t>(Sp) >= T.size())
			{
				return false;
			}
			size_t Units = 1;
			return IsWordCp(DecodeAt(T, static_cast<size_t>(Sp), Units));
		}
	}

	bool IsWordCp(char32_t C)
	{
		return C < 0x80 ? IsAsciiWord(C) : InRanges(tables::WordRanges, C);
	}

	bool IsDigitCp(char32_t C)
	{
		return C < 0x80 ? (C >= u'0' && C <= u'9') : InRanges(tables::DigitRanges, C);
	}

	bool IsSpaceCp(char32_t C)
	{
		return C < 0x80 ? (IsAsciiSpace(C) || (C >= 0x1C && C <= 0x1F)) : InRanges(tables::SpaceRanges, C);
	}

	char32_t FoldCp(char32_t C)
	{
		if (C < 0x80)
		{
			return (C >= u'A' && C <= u'Z') ? C + 32 : C;
		}
		return MapPair(tables::FoldMap, C);
	}

	void FoldMembers(char32_t C, std::vector<char32_t>& Out)
	{
		Out.clear();
		const char32_t Canon = FoldCp(C);
		constexpr size_t N = sizeof(tables::FoldMembers) / sizeof(tables::FoldMembers[0]) / 2;
		size_t Lo = 0;
		size_t Hi = N;
		while (Lo < Hi)
		{
			const size_t Mid = (Lo + Hi) / 2;
			if (tables::FoldMembers[2 * Mid] < Canon)
			{
				Lo = Mid + 1;
			}
			else
			{
				Hi = Mid;
			}
		}
		for (size_t K = Lo; K < N && tables::FoldMembers[2 * K] == Canon; ++K)
		{
			Out.push_back(tables::FoldMembers[2 * K + 1]);
		}
		if (Out.empty())
		{
			Out.push_back(C);
		}
	}

	char32_t UpperCp(char32_t C)
	{
		return MapPair(tables::UpperMap, C);
	}

	char32_t LowerCp(char32_t C)
	{
		return MapPair(tables::LowerMap, C);
	}

	char32_t SreLowerCp(char32_t C)
	{
		return MapPair(tables::SreLowerMap, C);
	}

	char32_t DecodeAt(std::u16string_view T, size_t I, size_t& Units)
	{
		const char16_t C = T[I];
		if (utf::IsHighSurrogate(C) && I + 1 < T.size() && utf::IsLowSurrogate(T[I + 1]))
		{
			Units = 2;
			return 0x10000 + ((static_cast<char32_t>(C) - 0xD800) << 10) + (static_cast<char32_t>(T[I + 1]) - 0xDC00);
		}
		Units = 1;
		return C;
	}

	char32_t DecodeBefore(std::u16string_view T, size_t I, size_t& Units)
	{
		const char16_t C = T[I - 1];
		if (utf::IsLowSurrogate(C) && I >= 2 && utf::IsHighSurrogate(T[I - 2]))
		{
			Units = 2;
			return 0x10000 + ((static_cast<char32_t>(T[I - 2]) - 0xD800) << 10) + (static_cast<char32_t>(C) - 0xDC00);
		}
		Units = 1;
		return C;
	}

	bool CharClass::Matches(char32_t C) const
	{
		auto Test = [this](char32_t X)
		{
			for (const auto& R : Ranges)
			{
				if (X >= R.first && X <= R.second)
				{
					return true;
				}
			}
			if (Categories != 0)
			{
				const bool bWord = bAscii ? IsAsciiWord(X) : IsWordCp(X);
				const bool bDigit = bAscii ? (X >= u'0' && X <= u'9') : IsDigitCp(X);
				const bool bSpace = bAscii ? IsAsciiSpace(X) : IsSpaceCp(X);
				if (((Categories & Cat_Word) && bWord) || ((Categories & Cat_NotWord) && !bWord) || ((Categories & Cat_Digit) && bDigit)
					|| ((Categories & Cat_NotDigit) && !bDigit) || ((Categories & Cat_Space) && bSpace)
					|| ((Categories & Cat_NotSpace) && !bSpace))
				{
					return true;
				}
			}
			return false;
		};
		bool bIn = Test(C);
		if (!bIn && bIgnoreCase && bAscii)
		{
			const char32_t Other = (C >= U'a' && C <= U'z') ? C - 32 : AsciiLowerCp(C);
			bIn = Other != C && Test(Other);
		}
		else if (!bIn && bIgnoreCase)
		{
			std::vector<char32_t> Members;
			FoldMembers(C, Members);
			for (char32_t M : Members)
			{
				if (M != C && Test(M))
				{
					bIn = true;
					break;
				}
			}
		}
		return bIn != bNegate;
	}

	// ---- Parser ---------------------------------------------------------------------------------------------

	namespace
	{
		enum class NodeKind : uint8_t
		{
			Empty,
			Char,
			Any,
			Class,
			Assert,
			Group,
			Concat,
			Alt,
			Repeat,
			Backref,
			Look,
			Atomic,
		};

		struct Node
		{
			NodeKind Kind = NodeKind::Empty;
			char32_t Ch = 0;
			bool bIgnoreCase = false;
			bool bDotAll = false;
			bool bAscii = false;
			bool bGreedy = true;
			bool bBehind = false;
			bool bNegate = false;
			Op AssertOp = Op::LineStart;
			int32_t Index = -1;          // class index, group number
			int32_t Min = 1;
			int32_t Max = 1;             // -1: unbounded
			std::vector<int32_t> Kids;
		};

		struct ParseFlags
		{
			bool bIgnoreCase = false;
			bool bDotAll = false;
			bool bVerbose = false;
			bool bAscii = false;
		};

		constexpr int32_t NoItem = -2;
		constexpr int32_t MaxNesting = 200;
		constexpr int32_t MaxRepeat = 100000;
		constexpr size_t MaxProgram = 300000;
		// Generation work (one unit per node visited) is bounded too: a body of one real node and thousands of empty
		// ones repeated {100000} emits a small program from billions of visits (review r2). About 50 ms at most.
		constexpr size_t MaxGenWork = 4 * MaxProgram;

		std::u16string Ascii(const char* S)
		{
			std::u16string Out;
			for (; *S; ++S)
			{
				Out.push_back(static_cast<char16_t>(static_cast<unsigned char>(*S)));
			}
			return Out;
		}

		bool IsOct(char16_t C) { return C >= u'0' && C <= u'7'; }
		bool IsDec(char16_t C) { return C >= u'0' && C <= u'9'; }
		bool IsAsciiLetter(char32_t C) { return (C >= u'a' && C <= u'z') || (C >= u'A' && C <= u'Z'); }
		int HexVal(char16_t C)
		{
			if (C >= u'0' && C <= u'9')
			{
				return C - u'0';
			}
			if (C >= u'a' && C <= u'f')
			{
				return C - u'a' + 10;
			}
			if (C >= u'A' && C <= u'F')
			{
				return C - u'A' + 10;
			}
			return -1;
		}

		class Parser
		{
		public:
			Parser(std::u16string_view InPattern, bool bIgnoreCase, Program& InOut, SearchError& InErr)
				: P(InPattern)
				, Out(InOut)
				, Err(InErr)
			{
				Flags.bIgnoreCase = bIgnoreCase;
			}

			std::vector<Node> Nodes;

			int32_t ParseAll()
			{
				const int32_t Root = ParseAlt();
				if (bFailed)
				{
					return -1;
				}
				if (I < P.size())
				{
					// Only an unmatched ')' stops the top level early.
					Fail(SearchErrorCode::Syntax, I, Ascii("unbalanced parenthesis"));
					return -1;
				}
				return Root;
			}

			bool Failed() const { return bFailed; }
			int32_t GroupTotal() const { return Groups; }

		private:
			std::u16string_view P;
			size_t I = 0;
			Program& Out;
			SearchError& Err;
			ParseFlags Flags;
			int32_t Groups = 1;
			std::vector<bool> OpenGroups{false};
			int32_t Depth = 0;
			bool bFailed = false;
			bool bSeenItem = false;      // something other than a global flag group was parsed

			int32_t Fail(SearchErrorCode Code, size_t Pos, std::u16string Detail)
			{
				if (!bFailed)
				{
					bFailed = true;
					Err.Code = Code;
					Err.Position = static_cast<int32_t>(Pos);
					Err.Detail = std::move(Detail);
				}
				return -1;
			}

			int32_t Add(Node N)
			{
				Nodes.push_back(std::move(N));
				return static_cast<int32_t>(Nodes.size()) - 1;
			}

			int32_t AddChar(char32_t C)
			{
				Node N;
				N.Kind = NodeKind::Char;
				N.bIgnoreCase = Flags.bIgnoreCase;
				N.bAscii = Flags.bAscii;
				N.Ch = !Flags.bIgnoreCase ? C : Flags.bAscii ? AsciiLowerCp(C) : FoldCp(C);
				return Add(std::move(N));
			}

			int32_t AddClass(CharClass Cls)
			{
				Cls.bIgnoreCase = Flags.bIgnoreCase;
				Cls.bAscii = Flags.bAscii;
				Out.Classes.push_back(std::move(Cls));
				Node N;
				N.Kind = NodeKind::Class;
				N.Index = static_cast<int32_t>(Out.Classes.size()) - 1;
				return Add(std::move(N));
			}

			int32_t AddAssert(Op Code)
			{
				Node N;
				N.Kind = NodeKind::Assert;
				N.AssertOp = Code;
				N.bAscii = Flags.bAscii;
				return Add(std::move(N));
			}

			char32_t GetCp()
			{
				size_t Units = 1;
				const char32_t C = DecodeAt(P, I, Units);
				I += Units;
				return C;
			}

			void SkipVerbose()
			{
				if (!Flags.bVerbose)
				{
					return;
				}
				while (I < P.size())
				{
					const char16_t C = P[I];
					if (C == u' ' || C == u'\t' || C == u'\n' || C == u'\r' || C == u'\v' || C == u'\f')
					{
						++I;
					}
					else if (C == u'#')
					{
						while (I < P.size() && P[I] != u'\n')
						{
							++I;
						}
					}
					else
					{
						break;
					}
				}
			}

			/** Reads a repeat at I without consuming it when it is not one (Python treats a bad '{' as a literal). */
			bool TryRepeat(size_t& End, int32_t& Min, int32_t& Max, bool& bTooBig) const
			{
				bTooBig = false;
				if (I >= P.size())
				{
					return false;
				}
				const char16_t C = P[I];
				if (C == u'*' || C == u'+' || C == u'?')
				{
					Min = C == u'+' ? 1 : 0;
					Max = C == u'?' ? 1 : -1;
					End = I + 1;
					return true;
				}
				if (C != u'{' || (I + 1 < P.size() && P[I + 1] == u'}'))
				{
					return false;
				}
				size_t K = I + 1;
				int64_t Lo = -1;
				int64_t Hi = -1;
				while (K < P.size() && IsDec(P[K]))
				{
					Lo = (Lo < 0 ? 0 : Lo) * 10 + (P[K] - u'0');
					Lo = std::min<int64_t>(Lo, int64_t(1) << 40);
					++K;
				}
				bool bComma = false;
				if (K < P.size() && P[K] == u',')
				{
					bComma = true;
					++K;
					while (K < P.size() && IsDec(P[K]))
					{
						Hi = (Hi < 0 ? 0 : Hi) * 10 + (P[K] - u'0');
						Hi = std::min<int64_t>(Hi, int64_t(1) << 40);
						++K;
					}
				}
				else
				{
					Hi = Lo;
				}
				if (K >= P.size() || P[K] != u'}')
				{
					return false;
				}
				Min = Lo < 0 ? 0 : static_cast<int32_t>(std::min<int64_t>(Lo, MaxRepeat + 1));
				Max = Hi < 0 ? -1 : static_cast<int32_t>(std::min<int64_t>(Hi, MaxRepeat + 1));
				bTooBig = Lo > MaxRepeat || Hi > MaxRepeat;
				(void)bComma;
				End = K + 1;
				return true;
			}

			int32_t ParseAlt()
			{
				std::vector<int32_t> Alts;
				for (;;)
				{
					const int32_t Seq = ParseSeq();
					if (bFailed)
					{
						return -1;
					}
					Alts.push_back(Seq);
					if (I < P.size() && P[I] == u'|')
					{
						++I;
						continue;
					}
					break;
				}
				if (Alts.size() == 1)
				{
					return Alts[0];
				}
				Node N;
				N.Kind = NodeKind::Alt;
				N.Kids = std::move(Alts);
				return Add(std::move(N));
			}

			int32_t ParseSeq()
			{
				std::vector<int32_t> Items;
				for (;;)
				{
					SkipVerbose();
					if (I >= P.size() || P[I] == u'|' || P[I] == u')')
					{
						break;
					}
					const size_t ItemPos = I;
					bool bAnchor = false;
					int32_t Atom = ParseAtom(bAnchor);
					if (bFailed)
					{
						return -1;
					}
					if (Atom == NoItem)
					{
						continue;
					}
					bSeenItem = true;
					SkipVerbose();
					size_t End = 0;
					int32_t Min = 0;
					int32_t Max = 0;
					bool bTooBig = false;
					if (TryRepeat(End, Min, Max, bTooBig))
					{
						if (bAnchor)
						{
							return Fail(SearchErrorCode::Syntax, I, Ascii("nothing to repeat"));
						}
						if (bTooBig)
						{
							return Fail(SearchErrorCode::TooComplex, I, Ascii("the repetition number is too large"));
						}
						if (Max >= 0 && Max < Min)
						{
							return Fail(SearchErrorCode::Syntax, I, Ascii("min repeat greater than max repeat"));
						}
						I = End;
						bool bGreedy = true;
						bool bPossessive = false;
						if (I < P.size() && P[I] == u'?')
						{
							bGreedy = false;
							++I;
						}
						else if (I < P.size() && P[I] == u'+')
						{
							bPossessive = true;
							++I;
						}
						Node R;
						R.Kind = NodeKind::Repeat;
						R.Min = Min;
						R.Max = Max;
						R.bGreedy = bGreedy;
						R.Kids.push_back(Atom);
						Atom = Add(std::move(R));
						if (bPossessive)
						{
							Node A;
							A.Kind = NodeKind::Atomic;
							A.Kids.push_back(Atom);
							Atom = Add(std::move(A));
						}
						SkipVerbose();
						size_t End2 = 0;
						int32_t Min2 = 0;
						int32_t Max2 = 0;
						bool bBig2 = false;
						if (TryRepeat(End2, Min2, Max2, bBig2))
						{
							return Fail(SearchErrorCode::Syntax, I, Ascii("multiple repeat"));
						}
					}
					(void)ItemPos;
					Items.push_back(Atom);
				}
				if (Items.empty())
				{
					return Add(Node());
				}
				if (Items.size() == 1)
				{
					return Items[0];
				}
				Node N;
				N.Kind = NodeKind::Concat;
				N.Kids = std::move(Items);
				return Add(std::move(N));
			}

			int32_t ParseAtom(bool& bAnchor)
			{
				const char16_t C = P[I];
				switch (C)
				{
				case u'(':
					return ParseGroup();
				case u'[':
					return ParseClass();
				case u'.':
				{
					++I;
					Node N;
					N.Kind = NodeKind::Any;
					N.bDotAll = Flags.bDotAll;
					return Add(std::move(N));
				}
				case u'^':
					++I;
					bAnchor = true;
					return AddAssert(Op::LineStart);
				case u'$':
					++I;
					bAnchor = true;
					return AddAssert(Op::LineEnd);
				case u'\\':
					return ParseEscapeAtom(bAnchor);
				case u'*':
				case u'+':
				case u'?':
					return Fail(SearchErrorCode::Syntax, I, Ascii("nothing to repeat"));
				case u'{':
				{
					size_t End = 0;
					int32_t Min = 0;
					int32_t Max = 0;
					bool bBig = false;
					if (TryRepeat(End, Min, Max, bBig))
					{
						return Fail(SearchErrorCode::Syntax, I, Ascii("nothing to repeat"));
					}
					++I;
					return AddChar(u'{');
				}
				default:
					return AddChar(GetCp());
				}
			}

			bool ParseName(char16_t Terminator, std::u16string& Name, size_t& NamePos)
			{
				NamePos = I;
				while (I < P.size() && P[I] != Terminator)
				{
					Name.push_back(P[I]);
					++I;
				}
				if (I >= P.size())
				{
					Fail(SearchErrorCode::Syntax, NamePos, Ascii(Terminator == u'>' ? "missing >, unterminated name" : "missing ), unterminated name"));
					return false;
				}
				++I;
				if (Name.empty())
				{
					Fail(SearchErrorCode::Syntax, NamePos, Ascii("missing group name"));
					return false;
				}
				if (!utf::IsIdentifier(Name))
				{
					Fail(SearchErrorCode::Syntax, NamePos, u"bad character in group name '" + Name + u"'");
					return false;
				}
				return true;
			}

			int32_t FindName(std::u16string_view Name) const
			{
				for (const auto& N : Out.Names)
				{
					if (N.first == Name)
					{
						return N.second;
					}
				}
				return -1;
			}

			int32_t ParseSub(size_t OpenPos, int32_t& OutNode)
			{
				if (++Depth > MaxNesting)
				{
					return Fail(SearchErrorCode::TooComplex, OpenPos, Ascii("too many nested groups"));
				}
				OutNode = ParseAlt();
				--Depth;
				if (bFailed)
				{
					return -1;
				}
				if (I >= P.size() || P[I] != u')')
				{
					return Fail(SearchErrorCode::Syntax, OpenPos, Ascii("missing ), unterminated subpattern"));
				}
				++I;
				return 0;
			}

			int32_t NewCaptureGroup(size_t OpenPos, std::u16string Name)
			{
				const int32_t Index = Groups++;
				if (Groups > 1000)
				{
					return Fail(SearchErrorCode::TooComplex, OpenPos, Ascii("too many groups"));
				}
				OpenGroups.push_back(true);
				if (!Name.empty())
				{
					Out.Names.emplace_back(std::move(Name), Index);
				}
				int32_t Body = -1;
				if (ParseSub(OpenPos, Body) < 0)
				{
					return -1;
				}
				OpenGroups[static_cast<size_t>(Index)] = false;
				Node N;
				N.Kind = NodeKind::Group;
				N.Index = Index;
				N.Kids.push_back(Body);
				return Add(std::move(N));
			}

			int32_t Backref(int32_t Group, size_t Pos)
			{
				if (Group <= 0 || Group >= Groups)
				{
					return Fail(SearchErrorCode::Syntax, Pos, u"invalid group reference " + Ascii(std::to_string(Group).c_str()));
				}
				if (OpenGroups[static_cast<size_t>(Group)])
				{
					return Fail(SearchErrorCode::Syntax, Pos, Ascii("cannot refer to an open group"));
				}
				Node N;
				N.Kind = NodeKind::Backref;
				N.Index = Group;
				N.bIgnoreCase = Flags.bIgnoreCase;
				N.bAscii = Flags.bAscii;
				return Add(std::move(N));
			}

			int32_t ParseGroup()
			{
				const size_t OpenPos = I;
				++I;   // '('
				if (I >= P.size() || P[I] != u'?')
				{
					return NewCaptureGroup(OpenPos, std::u16string());
				}
				++I;   // '?'
				if (I >= P.size())
				{
					return Fail(SearchErrorCode::Syntax, I, Ascii("unexpected end of pattern"));
				}
				const char16_t C = P[I];
				if (C == u'P')
				{
					++I;
					if (I < P.size() && P[I] == u'<')
					{
						++I;
						std::u16string Name;
						size_t NamePos = 0;
						if (!ParseName(u'>', Name, NamePos))
						{
							return -1;
						}
						if (FindName(Name) >= 0)
						{
							return Fail(SearchErrorCode::Syntax, NamePos, u"redefinition of group name '" + Name + u"'");
						}
						return NewCaptureGroup(OpenPos, std::move(Name));
					}
					if (I < P.size() && P[I] == u'=')
					{
						++I;
						std::u16string Name;
						size_t NamePos = 0;
						if (!ParseName(u')', Name, NamePos))
						{
							return -1;
						}
						const int32_t G = FindName(Name);
						if (G < 0)
						{
							return Fail(SearchErrorCode::Syntax, NamePos, u"unknown group name '" + Name + u"'");
						}
						return Backref(G, NamePos);
					}
					return Fail(SearchErrorCode::Syntax, I, Ascii("unknown extension ?P"));
				}
				if (C == u':' || C == u'>')
				{
					++I;
					int32_t Body = -1;
					if (ParseSub(OpenPos, Body) < 0)
					{
						return -1;
					}
					if (C == u':')
					{
						return Body;
					}
					Node N;
					N.Kind = NodeKind::Atomic;
					N.Kids.push_back(Body);
					return Add(std::move(N));
				}
				if (C == u'=' || C == u'!' || C == u'<')
				{
					bool bBehind = false;
					char16_t Kind = C;
					if (C == u'<')
					{
						++I;
						if (I >= P.size() || (P[I] != u'=' && P[I] != u'!'))
						{
							return Fail(SearchErrorCode::Syntax, I, Ascii("unknown extension ?<"));
						}
						bBehind = true;
						Kind = P[I];
					}
					++I;
					int32_t Body = -1;
					if (ParseSub(OpenPos, Body) < 0)
					{
						return -1;
					}
					Node N;
					N.Kind = NodeKind::Look;
					N.bBehind = bBehind;
					N.bNegate = Kind == u'!';
					N.Kids.push_back(Body);
					if (bBehind)
					{
						int32_t Lo = 0;
						int32_t Hi = 0;
						Width(Body, Lo, Hi);
						if (Lo != Hi || Hi < 0)
						{
							return Fail(SearchErrorCode::Syntax, OpenPos, Ascii("look-behind requires fixed-width pattern"));
						}
						N.Min = Lo;
					}
					return Add(std::move(N));
				}
				if (C == u'#')
				{
					while (I < P.size() && P[I] != u')')
					{
						++I;
					}
					if (I >= P.size())
					{
						return Fail(SearchErrorCode::Syntax, OpenPos, Ascii("missing ), unterminated comment"));
					}
					++I;
					return NoItem;
				}
				if (C == u'(')
				{
					return Fail(SearchErrorCode::Unsupported, OpenPos, Ascii("conditional groups (?(group)yes|no) are not supported"));
				}
				return ParseFlagsGroup(OpenPos);
			}

			int32_t ParseFlagsGroup(size_t OpenPos)
			{
				ParseFlags On = Flags;
				bool bAnyOn = false;
				bool bUnicode = false;
				bool bAsciiSet = false;
				auto Apply = [&](char16_t F, bool bValue) -> bool
				{
					switch (F)
					{
					case u'i': On.bIgnoreCase = bValue; return true;
					case u's': On.bDotAll = bValue; return true;
					case u'x': On.bVerbose = bValue; return true;
					case u'm': return true;   // MULTILINE is always on (§4.4)
					case u'a':
						if (!bValue)
						{
							return false;
						}
						On.bAscii = true;
						bAsciiSet = true;
						return true;
					case u'u':
						if (!bValue)
						{
							return false;
						}
						bUnicode = true;
						return true;
					default: return false;
					}
				};
				while (I < P.size() && P[I] != u')' && P[I] != u':' && P[I] != u'-')
				{
					const char16_t F = P[I];
					if (F == u'L')
					{
						return Fail(SearchErrorCode::Syntax, I, Ascii("bad inline flag: cannot use 'L' flag with a str pattern"));
					}
					if (!Apply(F, true))
					{
						return Fail(SearchErrorCode::Syntax, I, Ascii(IsAsciiLetter(F) ? "unknown flag" : "missing -, : or )"));
					}
					bAnyOn = true;
					++I;
				}
				if (bUnicode && bAsciiSet)
				{
					return Fail(SearchErrorCode::Syntax, I, Ascii("bad inline flag: flags 'a', 'u' and 'L' are incompatible"));
				}
				if (I >= P.size())
				{
					return Fail(SearchErrorCode::Syntax, I, Ascii("missing -, : or )"));
				}
				if (P[I] == u')')
				{
					// A global flag group: 3.11 accepts it only at the start of the pattern.
					if (!bAnyOn)
					{
						return Fail(SearchErrorCode::Syntax, I, Ascii("missing flag"));
					}
					if (bSeenItem || Depth > 0)
					{
						return Fail(SearchErrorCode::Syntax, OpenPos, Ascii("global flags not at the start of the expression"));
					}
					++I;
					Flags = On;
					return NoItem;
				}
				if (P[I] == u'-')
				{
					++I;
					bool bAnyOff = false;
					while (I < P.size() && P[I] != u':')
					{
						const char16_t F = P[I];
						if (F != u'i' && F != u's' && F != u'x' && F != u'm')
						{
							return Fail(SearchErrorCode::Syntax, I, Ascii(IsAsciiLetter(F) ? "bad inline flag: cannot turn off flags 'a', 'u' and 'L'" : "missing :"));
						}
						Apply(F, false);
						bAnyOff = true;
						++I;
					}
					if (!bAnyOff)
					{
						return Fail(SearchErrorCode::Syntax, I, Ascii("missing flag"));
					}
					if (I >= P.size())
					{
						return Fail(SearchErrorCode::Syntax, I, Ascii("missing :"));
					}
				}
				++I;   // ':'
				const ParseFlags Saved = Flags;
				Flags = On;
				int32_t Body = -1;
				const int32_t R = ParseSub(OpenPos, Body);
				Flags = Saved;
				return R < 0 ? -1 : Body;
			}

			/** \x.., \u...., \U........ with exactly Digits hex digits after I (which is after the letter). */
			bool ParseHex(int Digits, size_t EscPos, char32_t& Value)
			{
				char32_t V = 0;
				for (int K = 0; K < Digits; ++K)
				{
					if (I >= P.size() || HexVal(P[I]) < 0)
					{
						Fail(SearchErrorCode::Syntax, EscPos, Ascii("incomplete escape"));
						return false;
					}
					V = V * 16 + static_cast<char32_t>(HexVal(P[I]));
					++I;
				}
				if (V > 0x10FFFF)
				{
					Fail(SearchErrorCode::Syntax, EscPos, Ascii("bad escape"));
					return false;
				}
				Value = V;
				return true;
			}

			/**
			 * Escapes shared by atoms and classes. Returns 1 with Cp for a character, 2 with Cat for a category, 3 for
			 * a non-class item handled by the caller (anchors, group references: only outside classes), 0 on error.
			 */
			int ParseCommonEscape(bool bInClass, size_t EscPos, char32_t& Cp, uint8_t& Cat)
			{
				if (I >= P.size())
				{
					Fail(SearchErrorCode::Syntax, EscPos, Ascii("bad escape (end of pattern)"));
					return 0;
				}
				const char32_t C = GetCp();
				switch (C)
				{
				case u'd': Cat = Cat_Digit; return 2;
				case u'D': Cat = Cat_NotDigit; return 2;
				case u's': Cat = Cat_Space; return 2;
				case u'S': Cat = Cat_NotSpace; return 2;
				case u'w': Cat = Cat_Word; return 2;
				case u'W': Cat = Cat_NotWord; return 2;
				case u'a': Cp = 7; return 1;
				case u'f': Cp = 12; return 1;
				case u'n': Cp = 10; return 1;
				case u'r': Cp = 13; return 1;
				case u't': Cp = 9; return 1;
				case u'v': Cp = 11; return 1;
				case u'\\': Cp = u'\\'; return 1;
				case u'x': return ParseHex(2, EscPos, Cp) ? 1 : 0;
				case u'u': return ParseHex(4, EscPos, Cp) ? 1 : 0;
				case u'U': return ParseHex(8, EscPos, Cp) ? 1 : 0;
				case u'N':
					Fail(SearchErrorCode::Unsupported, EscPos, Ascii("\\N{...} named characters are not supported"));
					return 0;
				case u'b':
					if (bInClass)
					{
						Cp = 8;
						return 1;
					}
					return 3;
				default:
					break;
				}
				if (C == u'0' || (bInClass && C < 0x80 && IsOct(static_cast<char16_t>(C))))
				{
					// Octal: up to three digits in a class or after \0.
					char32_t V = C - u'0';
					for (int K = 0; K < 2 && I < P.size() && IsOct(P[I]); ++K)
					{
						V = V * 8 + (P[I] - u'0');
						++I;
					}
					if (V > 0377)
					{
						Fail(SearchErrorCode::Syntax, EscPos, Ascii("octal escape value outside of range 0-0o377"));
						return 0;
					}
					Cp = V;
					return 1;
				}
				if (C < 0x80 && IsDec(static_cast<char16_t>(C)))
				{
					if (bInClass)
					{
						Fail(SearchErrorCode::Syntax, EscPos, Ascii("bad escape"));
						return 0;
					}
					return 3;
				}
				if (IsAsciiLetter(C))
				{
					std::u16string D = Ascii("bad escape \\");
					D.push_back(static_cast<char16_t>(C));
					Fail(SearchErrorCode::Syntax, EscPos, std::move(D));
					return 0;
				}
				Cp = C;
				return 1;
			}

			int32_t ParseEscapeAtom(bool& bAnchor)
			{
				const size_t EscPos = I;
				++I;   // '\'
				if (I < P.size())
				{
					const char16_t C = P[I];
					if (C == u'A' || C == u'Z' || C == u'B')
					{
						++I;
						bAnchor = true;
						return AddAssert(C == u'A' ? Op::TextStart : (C == u'Z' ? Op::TextEnd : Op::NotWordBoundary));
					}
				}
				char32_t Cp = 0;
				uint8_t Cat = 0;
				const int Kind = ParseCommonEscape(false, EscPos, Cp, Cat);
				if (Kind == 0)
				{
					return -1;
				}
				if (Kind == 1)
				{
					return AddChar(Cp);
				}
				if (Kind == 2)
				{
					CharClass Cls;
					Cls.Categories = Cat;
					return AddClass(std::move(Cls));
				}
				// Kind 3: \b, or digits (an octal escape of three octal digits, else a group reference).
				const char16_t First = P[I - 1];
				if (First == u'b')
				{
					bAnchor = true;
					return AddAssert(Op::WordBoundary);
				}
				const size_t DigitsPos = I - 1;
				int32_t Group = First - u'0';
				if (I < P.size() && IsDec(P[I]))
				{
					const char16_t Second = P[I];
					if (IsOct(First) && IsOct(Second) && I + 1 < P.size() && IsOct(P[I + 1]))
					{
						const int32_t V = (First - u'0') * 64 + (Second - u'0') * 8 + (P[I + 1] - u'0');
						I += 2;
						if (V > 0377)
						{
							return Fail(SearchErrorCode::Syntax, EscPos, Ascii("octal escape value outside of range 0-0o377"));
						}
						return AddChar(static_cast<char32_t>(V));
					}
					Group = Group * 10 + (Second - u'0');
					++I;
				}
				return Backref(Group, DigitsPos);
			}

			int32_t ParseClass()
			{
				const size_t OpenPos = I;
				++I;   // '['
				CharClass Cls;
				if (I < P.size() && P[I] == u'^')
				{
					Cls.bNegate = true;
					++I;
				}
				bool bFirst = true;
				for (;;)
				{
					if (I >= P.size())
					{
						return Fail(SearchErrorCode::Syntax, OpenPos, Ascii("unterminated character set"));
					}
					if (P[I] == u']' && !bFirst)
					{
						++I;
						break;
					}
					bFirst = false;
					const size_t ItemPos = I;
					char32_t Lo = 0;
					uint8_t Cat = 0;
					if (P[I] == u'\\')
					{
						++I;
						if (ParseCommonEscape(true, ItemPos, Lo, Cat) == 0)
						{
							return -1;
						}
					}
					else
					{
						Lo = GetCp();
					}
					const bool bRange = I + 1 < P.size() && P[I] == u'-' && P[I + 1] != u']';
					if (Cat != 0)
					{
						if (bRange)
						{
							return Fail(SearchErrorCode::Syntax, ItemPos, Ascii("bad character range"));
						}
						Cls.Categories |= Cat;
						continue;
					}
					if (!bRange)
					{
						Cls.Ranges.emplace_back(Lo, Lo);
						continue;
					}
					++I;   // '-'
					char32_t Hi = 0;
					if (P[I] == u'\\')
					{
						const size_t HiPos = I;
						++I;
						uint8_t Cat2 = 0;
						if (ParseCommonEscape(true, HiPos, Hi, Cat2) == 0)
						{
							return -1;
						}
						if (Cat2 != 0)
						{
							return Fail(SearchErrorCode::Syntax, ItemPos, Ascii("bad character range"));
						}
					}
					else
					{
						Hi = GetCp();
					}
					if (Hi < Lo)
					{
						return Fail(SearchErrorCode::Syntax, ItemPos, Ascii("bad character range"));
					}
					Cls.Ranges.emplace_back(Lo, Hi);
				}
				return AddClass(std::move(Cls));
			}

		public:
			/** Width in code points, as Python's getwidth: Hi -1 when unbounded. */
			void Width(int32_t Index, int32_t& Lo, int32_t& Hi) const
			{
				const Node& N = Nodes[static_cast<size_t>(Index)];
				switch (N.Kind)
				{
				case NodeKind::Empty:
				case NodeKind::Assert:
				case NodeKind::Look:
					Lo = Hi = 0;
					return;
				case NodeKind::Char:
				case NodeKind::Any:
				case NodeKind::Class:
					Lo = Hi = 1;
					return;
				case NodeKind::Backref:
					Lo = 0;
					Hi = -1;
					return;
				case NodeKind::Group:
				case NodeKind::Atomic:
					Width(N.Kids[0], Lo, Hi);
					return;
				case NodeKind::Concat:
				{
					Lo = 0;
					Hi = 0;
					for (int32_t K : N.Kids)
					{
						int32_t L = 0;
						int32_t H = 0;
						Width(K, L, H);
						Lo += L;
						Hi = (Hi < 0 || H < 0) ? -1 : Hi + H;
					}
					return;
				}
				case NodeKind::Alt:
				{
					Lo = std::numeric_limits<int32_t>::max();
					Hi = 0;
					for (int32_t K : N.Kids)
					{
						int32_t L = 0;
						int32_t H = 0;
						Width(K, L, H);
						Lo = std::min(Lo, L);
						Hi = (Hi < 0 || H < 0) ? -1 : std::max(Hi, H);
					}
					return;
				}
				case NodeKind::Repeat:
				{
					int32_t L = 0;
					int32_t H = 0;
					Width(N.Kids[0], L, H);
					Lo = static_cast<int32_t>(std::min<int64_t>(static_cast<int64_t>(L) * N.Min, 1 << 30));
					Hi = (H < 0 || N.Max < 0) ? -1 : static_cast<int32_t>(std::min<int64_t>(static_cast<int64_t>(H) * N.Max, 1 << 30));
					return;
				}
				}
				Lo = Hi = 0;
			}
		};

		bool Nullable(const std::vector<Node>& Nodes, int32_t Index)
		{
			const Node& N = Nodes[static_cast<size_t>(Index)];
			switch (N.Kind)
			{
			case NodeKind::Empty:
			case NodeKind::Assert:
			case NodeKind::Look:
			case NodeKind::Backref:
				return true;
			case NodeKind::Char:
			case NodeKind::Any:
			case NodeKind::Class:
				return false;
			case NodeKind::Group:
			case NodeKind::Atomic:
				return Nullable(Nodes, N.Kids[0]);
			case NodeKind::Concat:
				for (int32_t K : N.Kids)
				{
					if (!Nullable(Nodes, K))
					{
						return false;
					}
				}
				return true;
			case NodeKind::Alt:
				for (int32_t K : N.Kids)
				{
					if (Nullable(Nodes, K))
					{
						return true;
					}
				}
				return false;
			case NodeKind::Repeat:
				return N.Min == 0 || Nullable(Nodes, N.Kids[0]);
			}
			return true;
		}

		class Emitter
		{
		public:
			Emitter(Program& InProg, const std::vector<Node>& InNodes)
				: Prog(InProg)
				, Nodes(InNodes)
				, Silent(InNodes.size(), -1)
			{
			}

			bool bTooBig = false;
			size_t GenWork = 0;

			int32_t Emit(Op Code, uint8_t Flags = 0, int32_t A = 0, int32_t B = 0)
			{
				if (Prog.Code.size() >= MaxProgram)
				{
					bTooBig = true;
					return static_cast<int32_t>(Prog.Code.size()) - 1;
				}
				Prog.Code.push_back(Inst{Code, Flags, A, B});
				return static_cast<int32_t>(Prog.Code.size()) - 1;
			}

			int32_t Here() const { return static_cast<int32_t>(Prog.Code.size()); }

			void Patch(int32_t At, int32_t A, int32_t B)
			{
				if (!bTooBig && At >= 0 && static_cast<size_t>(At) < Prog.Code.size())
				{
					Prog.Code[static_cast<size_t>(At)].A = A;
					Prog.Code[static_cast<size_t>(At)].B = B;
				}
			}

			void Build(int32_t Root)
			{
				Emit(Op::Save, 0, 0);   // group 0 spans the whole match
				Gen(Root);
				Emit(Op::Save, 0, 1);
				Emit(Op::Match);
				for (size_t K = 0; K < Deferred.size() && !bTooBig; ++K)
				{
					const auto [InstAt, NodeIndex] = Deferred[K];
					const int32_t Start = Here();
					if (!bTooBig)
					{
						Prog.Code[static_cast<size_t>(InstAt)].A = Start;
					}
					Gen(Nodes[static_cast<size_t>(NodeIndex)].Kids[0]);
					Emit(Op::Match);
				}
			}

		private:
			/**
			 * True when Gen emits no instruction for the node: empty groups of no capture, nested empty or {0} repeats.
			 * Memoized per node (review r3), so a repeat generated 10^5 times does not walk its body 10^5 times; every
			 * node is walked once per compile.
			 */
			bool EmitsNothing(int32_t Index)
			{
				const size_t At = static_cast<size_t>(Index);
				if (Silent[At] >= 0)
				{
					return Silent[At] != 0;
				}
				const Node& N = Nodes[At];
				bool bSilent = false;
				switch (N.Kind)
				{
				case NodeKind::Empty:
					bSilent = true;
					break;
				case NodeKind::Concat:
					bSilent = true;
					for (int32_t K : N.Kids)
					{
						if (!EmitsNothing(K))
						{
							bSilent = false;
							break;
						}
					}
					break;
				case NodeKind::Repeat:
					bSilent = N.Max == 0 || EmitsNothing(N.Kids[0]);
					break;
				default:
					break;
				}
				Silent[At] = bSilent ? 1 : 0;
				return bSilent;
			}

			Program& Prog;
			const std::vector<Node>& Nodes;
			std::vector<std::pair<int32_t, int32_t>> Deferred;
			std::vector<int8_t> Silent;   // per node: -1 not known yet, 0 emits code, 1 emits nothing

			static uint8_t CaseFlags(const Node& N)
			{
				return static_cast<uint8_t>((N.bIgnoreCase ? Flag_Set : 0) | (N.bIgnoreCase && N.bAscii ? Flag_Ascii : 0));
			}

			void Gen(int32_t Index)
			{
				if (bTooBig || ++GenWork > MaxGenWork)
				{
					bTooBig = true;
					return;
				}
				const Node& N = Nodes[static_cast<size_t>(Index)];
				switch (N.Kind)
				{
				case NodeKind::Empty:
					return;
				case NodeKind::Char:
					Emit(Op::Char, CaseFlags(N), static_cast<int32_t>(N.Ch));
					return;
				case NodeKind::Any:
					Emit(Op::Any, N.bDotAll ? Flag_Set : 0);
					return;
				case NodeKind::Class:
					Emit(Op::Class, 0, N.Index);
					return;
				case NodeKind::Assert:
					Emit(N.AssertOp, N.bAscii ? Flag_Set : 0);
					return;
				case NodeKind::Group:
					Emit(Op::Save, 0, 2 * N.Index);
					Gen(N.Kids[0]);
					Emit(Op::Save, 0, 2 * N.Index + 1);
					return;
				case NodeKind::Concat:
					for (int32_t K : N.Kids)
					{
						Gen(K);
					}
					return;
				case NodeKind::Alt:
				{
					std::vector<int32_t> Jumps;
					for (size_t K = 0; K < N.Kids.size(); ++K)
					{
						if (K + 1 < N.Kids.size())
						{
							const int32_t S = Emit(Op::Split);
							Gen(N.Kids[K]);
							Jumps.push_back(Emit(Op::Jmp));
							Patch(S, S + 1, Here());
						}
						else
						{
							Gen(N.Kids[K]);
						}
					}
					const int32_t End = Here();
					for (int32_t J : Jumps)
					{
						Patch(J, End, 0);
					}
					return;
				}
				case NodeKind::Repeat:
				{
					const int32_t Body = N.Kids[0];
					if (EmitsNothing(Body))
					{
						return;   // repeating nothing is nothing: (?:(?:){100000}){100000} must not loop 10^10 times
					}
					for (int32_t K = 0; K < N.Min && !bTooBig; ++K)
					{
						Gen(Body);
					}
					if (N.Max < 0)
					{
						const bool bNullable = Nullable(Nodes, Body);
						const int32_t Loop = Emit(Op::Split);
						int32_t Mark = -1;
						if (bNullable)
						{
							Mark = Prog.Slots++;
							Emit(Op::MarkSet, 0, Mark);
						}
						Gen(Body);
						int32_t Check = -1;
						if (bNullable)
						{
							Check = Emit(Op::MarkCheck, 0, Mark);
						}
						Emit(Op::Jmp, 0, Loop);
						const int32_t Exit = Here();
						if (N.bGreedy)
						{
							Patch(Loop, Loop + 1, Exit);
						}
						else
						{
							Patch(Loop, Exit, Loop + 1);
						}
						if (Check >= 0)
						{
							Patch(Check, Mark, Exit);
						}
						return;
					}
					std::vector<int32_t> Splits;
					for (int32_t K = N.Min; K < N.Max && !bTooBig; ++K)
					{
						Splits.push_back(Emit(Op::Split));
						Gen(Body);
					}
					const int32_t Exit = Here();
					for (int32_t S : Splits)
					{
						if (N.bGreedy)
						{
							Patch(S, S + 1, Exit);
						}
						else
						{
							Patch(S, Exit, S + 1);
						}
					}
					return;
				}
				case NodeKind::Backref:
					Emit(Op::Backref, CaseFlags(N), N.Index);
					return;
				case NodeKind::Look:
				{
					const uint8_t F = static_cast<uint8_t>((N.bBehind ? Flag_Behind : 0) | (N.bNegate ? Flag_Negate : 0));
					const int32_t At = Emit(Op::Look, F, 0, N.Min);
					Deferred.emplace_back(At, Index);
					return;
				}
				case NodeKind::Atomic:
				{
					const int32_t At = Emit(Op::Atomic);
					Deferred.emplace_back(At, Index);
					return;
				}
				}
			}
		};

		bool InstMatchesNewline(const Program& Prog, const Inst& In)
		{
			switch (In.Code)
			{
			case Op::Char:
				return In.A == u'\n';
			case Op::Any:
				return (In.Flags & Flag_Set) != 0;
			case Op::Class:
				return Prog.Classes[static_cast<size_t>(In.A)].Matches(u'\n');
			case Op::Backref:
				return true;
			default:
				return false;
			}
		}

		void Finish(Program& Prog)
		{
			Prog.bCanMatchNewline = false;
			for (const Inst& In : Prog.Code)
			{
				Prog.bCanMatchNewline = Prog.bCanMatchNewline || InstMatchesNewline(Prog, In);
			}
			Prog.FirstChar = -1;
			for (const Inst& In : Prog.Code)
			{
				if (In.Code == Op::Save)
				{
					continue;
				}
				if (In.Code == Op::Char && (In.Flags & Flag_Set) == 0)
				{
					Prog.FirstChar = In.A;
				}
				break;
			}
		}
	}

	bool CompileRegex(std::u16string_view Pattern, bool bIgnoreCase, Program& Out, SearchError& Err)
	{
		Out = Program();
		Err = SearchError();
		Parser Ps(Pattern, bIgnoreCase, Out, Err);
		const int32_t Root = Ps.ParseAll();
		if (Ps.Failed() || Root < 0)
		{
			return false;
		}
		Out.Groups = Ps.GroupTotal();
		Out.Slots = 2 * Out.Groups;
		Emitter E(Out, Ps.Nodes);
		E.Build(Root);
		if (E.bTooBig)
		{
			Err.Code = SearchErrorCode::TooComplex;
			Err.Position = 0;
			Err.Detail = u"the pattern is too large";
			return false;
		}
		Finish(Out);
		return true;
	}

	void CompileLiteral(std::u16string_view Literal, bool bIgnoreCase, Program& Out)
	{
		Out = Program();
		Out.Code.push_back(Inst{Op::Save, 0, 0, 0});
		size_t I = 0;
		while (I < Literal.size())
		{
			size_t Units = 1;
			const char32_t C = DecodeAt(Literal, I, Units);
			I += Units;
			Out.Code.push_back(Inst{Op::Char, static_cast<uint8_t>(bIgnoreCase ? Flag_Set : 0),
				static_cast<int32_t>(bIgnoreCase ? FoldCp(C) : C), 0});
		}
		Out.Code.push_back(Inst{Op::Save, 0, 1, 0});
		Out.Code.push_back(Inst{Op::Match, 0, 0, 0});
		Finish(Out);
	}

	// ---- Matcher --------------------------------------------------------------------------------------------

	Matcher::Matcher(const Program& InProg, std::u16string_view InText, const SearchLimits& InLimits)
		: Prog(InProg)
		, Text(InText)
		, Limits(InLimits)
	{
		Slot.assign(static_cast<size_t>(Prog.Slots), -1);
	}

	void Matcher::Unwind(size_t LogSize)
	{
		while (Log.size() > LogSize)
		{
			Slot[static_cast<size_t>(Log.back().first)] = Log.back().second;
			Log.pop_back();
		}
	}

	Status Matcher::MatchAt(Offset Start, bool bRejectEmpty, Offset RequiredEnd, Offset& OutEnd, bool bWholeWord)
	{
		bWhole = bWholeWord;
		if (bWhole && Start > 0)
		{
			size_t Units = 1;
			if (IsWordCp(DecodeBefore(Text, static_cast<size_t>(Start), Units)))
			{
				return Status::NoMatch;
			}
		}
		std::fill(Slot.begin(), Slot.end(), Offset(-1));
		Log.clear();
		Stack.clear();
		return Exec(0, Start, RequiredEnd, bRejectEmpty ? Start : -1, 0, OutEnd);
	}

	Status Matcher::Exec(int32_t Pc, Offset Sp, Offset RequiredEnd, Offset RejectEmptyAt, int32_t Depth, Offset& OutEnd)
	{
		const size_t Base = Stack.size();
		const Offset Size = static_cast<Offset>(Text.size());
		const Inst* Code = Prog.Code.data();
		for (;;)
		{
			if (++Steps > Limits.StepLimit)
			{
				Stack.resize(Base);
				return Status::Limit;
			}
			if ((Steps & 0xFFF) == 0 && IsCancelled(Cancel))
			{
				Stack.resize(Base);
				return Status::Cancelled;
			}
			const Inst& In = Code[Pc];
			bool bOk = true;
			switch (In.Code)
			{
			case Op::Char:
			{
				if (Sp >= Size)
				{
					bOk = false;
					break;
				}
				size_t Units = 1;
				char32_t C = DecodeAt(Text, static_cast<size_t>(Sp), Units);
				if (In.Flags & Flag_Set)
				{
					C = (In.Flags & Flag_Ascii) ? AsciiLowerCp(C) : FoldCp(C);
				}
				if (C != static_cast<char32_t>(In.A))
				{
					bOk = false;
					break;
				}
				Sp += static_cast<Offset>(Units);
				++Pc;
				continue;
			}
			case Op::Any:
			{
				if (Sp >= Size || (Text[static_cast<size_t>(Sp)] == u'\n' && !(In.Flags & Flag_Set)))
				{
					bOk = false;
					break;
				}
				size_t Units = 1;
				DecodeAt(Text, static_cast<size_t>(Sp), Units);
				Sp += static_cast<Offset>(Units);
				++Pc;
				continue;
			}
			case Op::Class:
			{
				if (Sp >= Size)
				{
					bOk = false;
					break;
				}
				size_t Units = 1;
				const char32_t C = DecodeAt(Text, static_cast<size_t>(Sp), Units);
				if (!Prog.Classes[static_cast<size_t>(In.A)].Matches(C))
				{
					bOk = false;
					break;
				}
				Sp += static_cast<Offset>(Units);
				++Pc;
				continue;
			}
			case Op::Split:
				if (Stack.size() >= Limits.MaxBacktrack)
				{
					Stack.resize(Base);
					return Status::Limit;
				}
				Stack.push_back(Frame{In.B, Sp, Log.size()});
				Pc = In.A;
				continue;
			case Op::Jmp:
				Pc = In.A;
				continue;
			case Op::Save:
			case Op::MarkSet:
				Log.emplace_back(In.A, Slot[static_cast<size_t>(In.A)]);
				Slot[static_cast<size_t>(In.A)] = Sp;
				++Pc;
				continue;
			case Op::MarkCheck:
				Pc = Sp == Slot[static_cast<size_t>(In.A)] ? In.B : Pc + 1;
				continue;
			case Op::LineStart:
				bOk = Sp == 0 || Text[static_cast<size_t>(Sp - 1)] == u'\n';
				break;
			case Op::LineEnd:
				bOk = Sp == Size || Text[static_cast<size_t>(Sp)] == u'\n';
				break;
			case Op::TextStart:
				bOk = Sp == 0;
				break;
			case Op::TextEnd:
				bOk = Sp == Size;
				break;
			case Op::WordBoundary:
			case Op::NotWordBoundary:
			{
				const bool bAscii = (In.Flags & Flag_Set) != 0;
				size_t Units = 1;
				bool bBefore = false;
				bool bAfter = false;
				if (Sp > 0)
				{
					const char32_t C = DecodeBefore(Text, static_cast<size_t>(Sp), Units);
					bBefore = bAscii ? IsAsciiWord(C) : IsWordCp(C);
				}
				if (Sp < Size)
				{
					const char32_t C = DecodeAt(Text, static_cast<size_t>(Sp), Units);
					bAfter = bAscii ? IsAsciiWord(C) : IsWordCp(C);
				}
				bOk = (bBefore != bAfter) == (In.Code == Op::WordBoundary) && Size > 0;
				break;
			}
			case Op::Backref:
			{
				const Offset S = Slot[static_cast<size_t>(2 * In.A)];
				const Offset E = Slot[static_cast<size_t>(2 * In.A + 1)];
				if (S < 0 || E < 0)
				{
					bOk = false;
					break;
				}
				Offset P = S;
				Offset Q = Sp;
				const bool bFold = (In.Flags & Flag_Set) != 0;
				const bool bAsciiFold = (In.Flags & Flag_Ascii) != 0;
				while (P < E && bOk)
				{
					if (++Steps > Limits.StepLimit)
					{
						Stack.resize(Base);
						return Status::Limit;
					}
					if ((Steps & 0xFFF) == 0 && IsCancelled(Cancel))
					{
						Stack.resize(Base);
						return Status::Cancelled;   // the same poll as the main loop's, so a long comparison is cancellable
					}
					if (Q >= Size)
					{
						bOk = false;
						break;
					}
					size_t U1 = 1;
					size_t U2 = 1;
					char32_t A = DecodeAt(Text, static_cast<size_t>(P), U1);
					char32_t B = DecodeAt(Text, static_cast<size_t>(Q), U2);
					if (bFold)
					{
						A = bAsciiFold ? AsciiLowerCp(A) : SreLowerCp(A);
						B = bAsciiFold ? AsciiLowerCp(B) : SreLowerCp(B);
					}
					bOk = A == B;
					P += static_cast<Offset>(U1);
					Q += static_cast<Offset>(U2);
				}
				if (bOk)
				{
					Sp = Q;
					++Pc;
					continue;
				}
				break;
			}
			case Op::Look:
			{
				if (Depth >= 100)
				{
					Stack.resize(Base);
					return Status::Limit;
				}
				const size_t LogMark = Log.size();
				Status R = Status::NoMatch;
				Offset End = 0;
				if (!(In.Flags & Flag_Behind))
				{
					R = Exec(In.A, Sp, -1, -1, Depth + 1, End);
				}
				else
				{
					// Fixed width in code points: B units if every one is in the BMP, up to 2B with surrogate pairs.
					const Offset W = In.B;
					for (Offset S = Sp - W; S >= Sp - 2 * W && S >= 0; --S)
					{
						if (S > 0 && S < Size && utf::IsLowSurrogate(Text[static_cast<size_t>(S)])
							&& utf::IsHighSurrogate(Text[static_cast<size_t>(S - 1)]))
						{
							continue;
						}
						R = Exec(In.A, S, Sp, -1, Depth + 1, End);
						if (R != Status::NoMatch)
						{
							break;
						}
						if (W == 0)
						{
							break;
						}
					}
				}
				if (R == Status::Limit || R == Status::Cancelled)
				{
					Stack.resize(Base);
					return R;
				}
				const bool bMatched = R == Status::Matched;
				const bool bNegate = (In.Flags & Flag_Negate) != 0;
				if (bNegate || !bMatched)
				{
					Unwind(LogMark);   // a negative look keeps no groups
				}
				bOk = bMatched != bNegate;
				if (bOk)
				{
					++Pc;
					continue;
				}
				break;
			}
			case Op::Atomic:
			{
				if (Depth >= 100)
				{
					Stack.resize(Base);
					return Status::Limit;
				}
				const size_t LogMark = Log.size();
				Offset End = 0;
				const Status R = Exec(In.A, Sp, -1, -1, Depth + 1, End);
				if (R == Status::Limit || R == Status::Cancelled)
				{
					Stack.resize(Base);
					return R;
				}
				if (R == Status::NoMatch)
				{
					Unwind(LogMark);
					bOk = false;
					break;
				}
				Sp = End;
				++Pc;
				continue;
			}
			case Op::Match:
				if ((RequiredEnd >= 0 && Sp != RequiredEnd) || (RejectEmptyAt >= 0 && Sp == RejectEmptyAt)
					|| (bWhole && Depth == 0 && WordAfter(Text, Sp)))
				{
					bOk = false;
					break;
				}
				OutEnd = Sp;
				Stack.resize(Base);
				return Status::Matched;
			}
			if (bOk)
			{
				++Pc;   // assertions that held
				continue;
			}
			// Backtrack.
			if (Stack.size() <= Base)
			{
				return Status::NoMatch;
			}
			const Frame F = Stack.back();
			Stack.pop_back();
			Unwind(F.LogSize);
			Pc = F.Pc;
			Sp = F.Sp;
		}
	}
}
