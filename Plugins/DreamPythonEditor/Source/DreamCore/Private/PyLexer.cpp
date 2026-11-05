// SPDX-License-Identifier: Apache-2.0
// Dream core: incremental Python lexer for highlighting and line facts (TDD §4.3).
// Every cross-line dependency is carried in the interned end-of-line state (string frames, backslash join, and the
// docstring context), so an incremental relex that stops at an unchanged end state equals a full lex (T-LEX-007).
// The line array is a gap buffer spliced per ChangeSet (§4.3.3), so typing that adds or removes lines moves only the
// records between the last splice and this one, not every record below the edit (T-LEX-008 at 100k lines).
// Deferred (G1): the 128-bit XXH3 line hash that keys SCodeView's shaping cache (§4.3.3, A8).
#include "dream/PyLexer.h"

#include "GapArray.h"
#include "dream/Utf.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>

namespace dream::lexer_tables
{
#include "PyBuiltins.inc"

	inline constexpr const char16_t* HardKeywords[] = {
		u"False", u"None", u"True", u"and", u"as", u"assert", u"async", u"await", u"break", u"class", u"continue", u"def",
		u"del", u"elif", u"else", u"except", u"finally", u"for", u"from", u"global", u"if", u"import", u"in", u"is",
		u"lambda", u"nonlocal", u"not", u"or", u"pass", u"raise", u"return", u"try", u"while", u"with", u"yield",
	};
}

namespace dream::lexer_detail
{
	inline constexpr int MaxFrames = 8;   // §4.3.2 depth cap

	/** Where a string frame is: its literal text, a replacement field's expression, or a format spec (2 levels). */
	enum class Part : uint8_t
	{
		Literal,
		Expr,
		Spec,
		Expr2,   // a field nested in a format spec
		Spec2,
	};

	struct Frame
	{
		char16_t Quote = u'"';
		bool bTriple = false;
		bool bRaw = false;
		bool bF = false;
		bool bBytes = false;
		bool bDoc = false;
		Part P = Part::Literal;
		uint8_t Depth = 0;   // bracket depth inside an Expr part; 1 = directly inside the braces
	};

	/** The end-of-line state (§4.3.3 LexState). Bracket depth is deliberately not part of it. */
	struct State
	{
		std::array<Frame, MaxFrames> Frames{};
		uint8_t NumFrames = 0;
		bool bBackslash = false;    // the line ended with a backslash join outside strings
		bool bAfterColon = false;   // the last significant line ended with ':' (docstring context)
		bool bModuleTop = false;    // no significant token yet in the text (docstring context)

		bool IsPlainNormal() const { return NumFrames == 0 && !bBackslash && !bAfterColon && !bModuleTop; }

		std::string Key() const
		{
			std::string K;
			K.reserve(2 + NumFrames * 3);
			K.push_back(static_cast<char>((bBackslash ? 1 : 0) | (bAfterColon ? 2 : 0) | (bModuleTop ? 4 : 0)));
			K.push_back(static_cast<char>(NumFrames));
			for (int I = 0; I < NumFrames; ++I)
			{
				const Frame& F = Frames[static_cast<size_t>(I)];
				K.push_back(static_cast<char>((F.Quote == u'\'' ? 1 : 0) | (F.bTriple ? 2 : 0) | (F.bRaw ? 4 : 0) | (F.bF ? 8 : 0)
					| (F.bBytes ? 16 : 0) | (F.bDoc ? 32 : 0)));
				K.push_back(static_cast<char>(F.P));
				K.push_back(static_cast<char>(F.Depth));
			}
			return K;
		}
	};

	/** Hash-consed states (§4.3.3): equal states share an id; 0 is the plain Normal state. */
	class Interner
	{
	public:
		Interner()
		{
			States.emplace_back();
			Ids.emplace(States.front().Key(), NormalState);
		}

		LexStateId Intern(const State& S)
		{
			if (S.IsPlainNormal())
			{
				return NormalState;
			}
			std::string K = S.Key();
			auto It = Ids.find(K);
			if (It != Ids.end())
			{
				return It->second;
			}
			const LexStateId Id = static_cast<LexStateId>(States.size());
			States.push_back(S);
			Ids.emplace(std::move(K), Id);
			return Id;
		}

		const State& Get(LexStateId Id) const
		{
			return Id < States.size() ? States[Id] : States.front();
		}

	private:
		std::vector<State> States;
		std::unordered_map<std::string, LexStateId> Ids;
	};

	inline State InitialState()
	{
		State S;
		S.bModuleTop = true;
		return S;
	}

	inline bool IsBlank(char16_t C) { return utf::IsPyBlank(C); }
	inline bool IsDigit(char16_t C) { return C >= u'0' && C <= u'9'; }
	inline bool IsHexDigit(char16_t C) { return IsDigit(C) || (C >= u'a' && C <= u'f') || (C >= u'A' && C <= u'F'); }
	inline bool IsOctDigit(char16_t C) { return C >= u'0' && C <= u'7'; }
	inline bool IsQuote(char16_t C) { return C == u'\'' || C == u'"'; }

	inline char16_t Lower(char16_t C)
	{
		return (C >= u'A' && C <= u'Z') ? static_cast<char16_t>(C - u'A' + u'a') : C;
	}

	/** The 24 valid prefixes (§4.3.1): r, u, f, b, fr, rf, br, rb in any case. */
	bool ParsePrefix(std::u16string_view Word, Frame& Out)
	{
		if (Word.empty() || Word.size() > 2)
		{
			return false;
		}
		bool bR = false;
		bool bU = false;
		bool bF = false;
		bool bB = false;
		for (char16_t C : Word)
		{
			switch (Lower(C))
			{
			case u'r': if (bR) { return false; } bR = true; break;
			case u'u': if (bU) { return false; } bU = true; break;
			case u'f': if (bF) { return false; } bF = true; break;
			case u'b': if (bB) { return false; } bB = true; break;
			default: return false;
			}
		}
		if (bU && Word.size() != 1)
		{
			return false;
		}
		if (bF && bB)
		{
			return false;
		}
		Out.bRaw = bR;
		Out.bF = bF;
		Out.bBytes = bB;
		return true;
	}

	bool IsKeywordWord(std::u16string_view Word)
	{
		for (const char16_t* K : lexer_tables::HardKeywords)
		{
			if (Word == K)
			{
				return true;
			}
		}
		return false;
	}

	bool IsBuiltinWord(std::u16string_view Word)
	{
		const auto* Begin = std::begin(lexer_tables::BuiltinNames);
		const auto* End = std::end(lexer_tables::BuiltinNames);
		const auto* It = std::lower_bound(Begin, End, Word,
			[](const char16_t* Name, std::u16string_view W) { return std::u16string_view(Name) < W; });
		return It != End && std::u16string_view(*It) == Word;
	}

	/** Lexes one physical line from a start state: tokens, facts and the end state. */
	class LineLexer
	{
	public:
		LineLexer(std::u16string_view InLine, const State& InStart, LexGrammar InGrammar, std::vector<uint32_t>& OutTokens)
			: L(InLine), Len(InLine.size()), Start(InStart), S(InStart), bPy311(InGrammar == LexGrammar::Py311), Toks(OutTokens)
		{
			Toks.clear();
			StrStartCol.fill(-1);
			FieldStartCol.fill(-1);
		}

		void Run(LineFacts& OutFacts, State& OutEnd)
		{
			while (Pos < Len)
			{
				if (S.NumFrames > 0)
				{
					const Part P = Top().P;
					if (P == Part::Expr || P == Part::Expr2)
					{
						if (bPy311 && CloseIfQuoteEndsAFrame())
						{
							continue;
						}
						LexCodeToken(true);
					}
					else
					{
						LexStringPiece();
					}
				}
				else
				{
					LexCodeToken(false);
				}
			}
			EndOfLine();
			PostPassSoftKeywords();
			PostPassDocstring();
			if (Len > static_cast<size_t>(LongLineColumns) && (Toks.empty() || TokenStart(Toks.back()) < LongLineColumns))
			{
				Toks.push_back(PackToken(LongLineColumns, TokenKind::Text));
			}
			OutFacts = BuildFacts();
			OutEnd = BuildEndState();
		}

	private:
		enum class Ctx : uint8_t
		{
			None,
			Def,
			Class,
			Dot,
			At,
			DecoratorName,
			Other,
		};

		std::u16string_view L;
		size_t Len;
		size_t Pos = 0;
		const State& Start;
		State S;
		bool bPy311;
		std::vector<uint32_t>& Toks;

		int32_t LocalDepth = 0;              // line-local bracket depth outside strings (EndsWithColon)
		int32_t BracketDelta = 0;
		bool bLastSigColon = false;
		bool bAnySig = false;
		bool bHasComment = false;
		int32_t FirstSigTok = -1;
		bool bCodeBackslashEol = false;      // a backslash join outside strings ends the line
		bool bStrBackslashEol = false;       // the last unit escapes the newline inside a string
		uint16_t ExtraFlags = 0;
		std::array<int32_t, MaxFrames> StrStartCol{};    // where each frame's string began on this line; -1 = earlier
		std::array<int32_t, MaxFrames> FieldStartCol{};  // the '{' of the frame's open field on this line; -1 = earlier
		Ctx Prev = Ctx::None;
		int32_t DocTok = -1;                 // token index where a docstring candidate started on this line
		bool bDocClosed = false;
		size_t DocCloseTokEnd = 0;
		bool bSigAfterDoc = false;

		bool StartsAtLogicalLine() const { return Start.NumFrames == 0 && !Start.bBackslash; }
		Frame& Top() { return S.Frames[static_cast<size_t>(S.NumFrames) - 1]; }

		void Emit(size_t Col, TokenKind Kind)
		{
			if (Col >= static_cast<size_t>(LongLineColumns))
			{
				return;   // the rest of a long line is one Text token, added at the end (§4.3.1)
			}
			const int32_t C = static_cast<int32_t>(Col);
			if (!Toks.empty() && TokenStart(Toks.back()) == C)
			{
				Toks.back() = PackToken(C, Kind);
			}
			else
			{
				Toks.push_back(PackToken(C, Kind));
			}
		}

		/** Emits only when the previous token has another kind (literal runs). */
		void EmitRun(size_t Col, TokenKind Kind)
		{
			if (!Toks.empty() && TokenKindOf(Toks.back()) == Kind && Col < static_cast<size_t>(LongLineColumns))
			{
				return;
			}
			Emit(Col, Kind);
		}

		void NoteSig(bool bColonAtTop)
		{
			if (!bAnySig)
			{
				bAnySig = true;
			}
			if (FirstSigTok < 0)
			{
				FirstSigTok = Toks.empty() ? 0 : static_cast<int32_t>(Toks.size()) - 1;
			}
			if (bDocClosed)
			{
				bSigAfterDoc = true;
			}
			bLastSigColon = bColonAtTop;
		}

		/** Replaces every token from Col on with one Error token (unterminated strings, broken fields). */
		void ErrorFrom(int32_t Col)
		{
			Col = std::max(Col, 0);
			if (Col >= LongLineColumns)
			{
				return;
			}
			while (!Toks.empty() && TokenStart(Toks.back()) >= Col)
			{
				Toks.pop_back();
			}
			if (static_cast<size_t>(Col) < Len)
			{
				Toks.push_back(PackToken(Col, TokenKind::Error));   // an empty line keeps no token
			}
		}

		size_t TokenEnd(size_t Index) const
		{
			if (Index + 1 < Toks.size())
			{
				return static_cast<size_t>(TokenStart(Toks[Index + 1]));
			}
			return std::min(Len, static_cast<size_t>(LongLineColumns));
		}

		std::u16string_view TokenText(size_t Index) const
		{
			const size_t B = static_cast<size_t>(TokenStart(Toks[Index]));
			return L.substr(B, TokenEnd(Index) - B);
		}

		bool IsWhitespaceToken(size_t Index) const
		{
			if (TokenKindOf(Toks[Index]) != TokenKind::Text)
			{
				return false;
			}
			for (char16_t C : TokenText(Index))
			{
				if (!IsBlank(C))
				{
					return false;
				}
			}
			return true;
		}

		bool MatchesClose(const Frame& F, size_t At) const
		{
			const size_t N = F.bTriple ? 3 : 1;
			if (At + N > Len)
			{
				return false;
			}
			for (size_t I = 0; I < N; ++I)
			{
				if (L[At + I] != F.Quote)
				{
					return false;
				}
			}
			return true;
		}

		/** Py311: the tokenizer ends the outermost string at its own quote, whatever is open inside it (§4.3.2). */
		bool CloseIfQuoteEndsAFrame()
		{
			if (!IsQuote(L[Pos]))
			{
				return false;
			}
			for (int K = 0; K < S.NumFrames; ++K)
			{
				if (MatchesClose(S.Frames[static_cast<size_t>(K)], Pos))
				{
					CloseFrom(K);
					return true;
				}
			}
			return false;
		}

		/** Frames K.. close at Pos with frame K's quote; any field still open in them becomes Error. */
		void CloseFrom(int K)
		{
			int32_t ErrCol = -1;
			for (int J = K; J < S.NumFrames; ++J)
			{
				if (S.Frames[static_cast<size_t>(J)].P != Part::Literal)
				{
					const int32_t Col = std::max(FieldStartCol[static_cast<size_t>(J)], 0);
					ErrCol = ErrCol < 0 ? Col : std::min(ErrCol, Col);
				}
			}
			if (ErrCol >= 0)
			{
				ErrorFrom(ErrCol);
			}
			const Frame& F = S.Frames[static_cast<size_t>(K)];
			const TokenKind Kind = F.bDoc ? TokenKind::Docstring : TokenKind::String;
			if (ErrCol >= 0)
			{
				Emit(Pos, Kind);
			}
			else
			{
				EmitRun(Pos, Kind);
			}
			NoteSig(false);
			if (F.bDoc && K == 0)
			{
				bDocClosed = true;
				DocCloseTokEnd = Toks.size();
			}
			Pos += F.bTriple ? 3 : 1;
			S.NumFrames = static_cast<uint8_t>(K);
			Prev = Ctx::Other;
		}

		void BeginString(size_t Col, Frame F)
		{
			if (S.NumFrames >= MaxFrames)
			{
				// Frame stack over the cap (§4.3.3 failure modes): Error to the end of the line, frames dropped.
				ErrorFrom(static_cast<int32_t>(Col));
				NoteSig(false);
				S.NumFrames = 0;
				Pos = Len;
				return;
			}
			F.Quote = L[Pos];
			F.bTriple = Pos + 2 < Len && L[Pos + 1] == F.Quote && L[Pos + 2] == F.Quote;
			F.P = Part::Literal;
			F.Depth = 0;
			const bool bDocContext = S.NumFrames == 0 && FirstSigTok < 0 && StartsAtLogicalLine()
				&& (Start.bAfterColon || Start.bModuleTop) && !F.bF && !F.bBytes;
			F.bDoc = bDocContext;
			const size_t Index = S.NumFrames;
			S.Frames[Index] = F;
			S.NumFrames = static_cast<uint8_t>(S.NumFrames + 1);
			StrStartCol[Index] = static_cast<int32_t>(Col);
			FieldStartCol[Index] = -1;
			Emit(Col, F.bDoc ? TokenKind::Docstring : TokenKind::String);
			if (F.bDoc)
			{
				DocTok = static_cast<int32_t>(Toks.size()) - 1;
			}
			NoteSig(false);
			Prev = Ctx::Other;
			Pos += F.bTriple ? 3 : 1;
		}

		void LexStringPiece()
		{
			Frame& F = Top();
			const size_t Index = static_cast<size_t>(S.NumFrames) - 1;
			const TokenKind LitKind = F.P == Part::Literal ? (F.bDoc ? TokenKind::Docstring : TokenKind::String) : TokenKind::FStringSpec;
			const char16_t C = L[Pos];
			if (IsQuote(C))
			{
				if (bPy311)
				{
					if (CloseIfQuoteEndsAFrame())
					{
						return;
					}
				}
				else if (MatchesClose(F, Pos))
				{
					CloseFrom(S.NumFrames - 1);
					return;
				}
				EmitRun(Pos, LitKind);
				NoteSig(false);
				++Pos;
				return;
			}
			if (C == u'\\')
			{
				if (F.bRaw)
				{
					EmitRun(Pos, LitKind);
					NoteSig(false);
					if (Pos + 1 == Len)
					{
						bStrBackslashEol = true;
						++Pos;
					}
					else
					{
						// A raw backslash still keeps the next quote (or backslash) inside the string; anything else, an
						// f-string's '{' included (rf"\{x}"), is read on its own. ((lang) PO-06 review lang-q1-m1)
						const char16_t Next = L[Pos + 1];
						Pos += IsQuote(Next) || Next == u'\\' ? 2 : 1;
					}
					return;
				}
				LexEscape(LitKind);
				return;
			}
			if (F.bF && (C == u'{' || C == u'}'))
			{
				LexFStringBrace(F, Index, LitKind);
				return;
			}
			EmitRun(Pos, LitKind);
			NoteSig(false);
			while (Pos < Len)
			{
				const char16_t D = L[Pos];
				if (IsQuote(D) || D == u'\\' || (F.bF && (D == u'{' || D == u'}')))
				{
					break;
				}
				++Pos;
			}
		}

		void LexFStringBrace(Frame& F, size_t Index, TokenKind LitKind)
		{
			const char16_t C = L[Pos];
			const bool bDouble = Pos + 1 < Len && L[Pos + 1] == C;
			if (C == u'{')
			{
				if (F.P == Part::Literal && bDouble)
				{
					EmitRun(Pos, LitKind);
					NoteSig(false);
					Pos += 2;
					return;
				}
				Emit(Pos, F.P == Part::Spec2 ? TokenKind::Error : TokenKind::FStringBrace);   // 3 levels: nested too deeply
				NoteSig(false);
				if (F.P == Part::Literal)
				{
					F.P = Part::Expr;
					F.Depth = 1;
					FieldStartCol[Index] = static_cast<int32_t>(Pos);
				}
				else if (F.P == Part::Spec)
				{
					F.P = Part::Expr2;
					F.Depth = 1;
				}
				++Pos;
				return;
			}
			// '}'
			if (F.P == Part::Literal)
			{
				if (bDouble)
				{
					EmitRun(Pos, LitKind);
					NoteSig(false);
					Pos += 2;
					return;
				}
				Emit(Pos, TokenKind::Error);   // single '}' is not allowed
				NoteSig(false);
				++Pos;
				return;
			}
			Emit(Pos, TokenKind::FStringBrace);
			NoteSig(false);
			if (F.P == Part::Spec)
			{
				F.P = Part::Literal;
				FieldStartCol[Index] = -1;
			}
			else
			{
				F.P = Part::Spec;   // Spec2 back to the outer field's spec
			}
			++Pos;
		}

		size_t CountWhile(size_t From, size_t Max, bool (*Pred)(char16_t)) const
		{
			size_t N = 0;
			while (N < Max && From + N < Len && Pred(L[From + N]))
			{
				++N;
			}
			return N;
		}

		void LexEscape(TokenKind LitKind)
		{
			const bool bBytes = Top().bBytes;
			if (Pos + 1 == Len)
			{
				Emit(Pos, TokenKind::StringEscape);   // backslash-newline continues the string
				NoteSig(false);
				bStrBackslashEol = true;
				++Pos;
				return;
			}
			const char16_t N = L[Pos + 1];
			size_t Length = 0;
			bool bValid = true;
			switch (N)
			{
			case u'\\': case u'\'': case u'"': case u'a': case u'b': case u'f': case u'n': case u'r': case u't': case u'v':
				Length = 2;
				break;
			case u'0': case u'1': case u'2': case u'3': case u'4': case u'5': case u'6': case u'7':
				Length = 2 + CountWhile(Pos + 2, 2, IsOctDigit);
				break;
			case u'x':
			{
				const size_t H = CountWhile(Pos + 2, 2, IsHexDigit);
				Length = 2 + H;
				bValid = H == 2;
				break;
			}
			case u'u':
			case u'U':
			{
				if (bBytes)
				{
					break;
				}
				const size_t Want = N == u'u' ? 4 : 8;
				const size_t H = CountWhile(Pos + 2, Want, IsHexDigit);
				Length = 2 + H;
				bValid = H == Want;
				break;
			}
			case u'N':
			{
				if (bBytes)
				{
					break;
				}
				Length = 2;
				bValid = false;
				if (Pos + 2 < Len && L[Pos + 2] == u'{')
				{
					const size_t Close = L.find(u'}', Pos + 3);
					if (Close != std::u16string_view::npos && Close > Pos + 3)
					{
						Length = Close + 1 - Pos;
						bValid = true;
					}
				}
				break;
			}
			default:
				break;
			}
			if (Length == 0)
			{
				// Not an escape (an invalid escape is only a DeprecationWarning in 3.11): the backslash is literal text.
				EmitRun(Pos, LitKind);
				NoteSig(false);
				++Pos;
				return;
			}
			Emit(Pos, bValid ? TokenKind::StringEscape : TokenKind::Error);
			NoteSig(false);
			Pos += Length;
		}

		void LexComment()
		{
			TokenKind Kind = TokenKind::Comment;
			if (FirstSigTok < 0 && S.NumFrames == 0 && StartsAtLogicalLine())
			{
				size_t I = Pos + 1;
				while (I < Len && (L[I] == u' ' || L[I] == u'\t'))
				{
					++I;
				}
				const std::u16string_view Rest = L.substr(I);
				auto IsWordEnd = [&Rest](size_t At) { return At >= Rest.size() || IsBlank(Rest[At]); };
				if (Rest.substr(0, 2) == u"%%")
				{
					Kind = TokenKind::CellMarker;
					ExtraFlags |= LF_CellMarker;
				}
				else if ((Rest.substr(0, 6) == u"region" && IsWordEnd(6)) || (Rest.substr(0, 9) == u"endregion" && IsWordEnd(9)))
				{
					Kind = TokenKind::RegionMarker;
					ExtraFlags |= LF_Region;
				}
			}
			Emit(Pos, Kind);
			bHasComment = true;
			Pos = Len;
		}

		/** Digits with single underscores between them, from Pos; false on a bad underscore. */
		bool ScanDigits(bool (*IsDigitFn)(char16_t), bool bRequireOne)
		{
			const size_t Begin = Pos;
			bool bOk = true;
			bool bPrevUnderscore = false;
			while (Pos < Len && (IsDigitFn(L[Pos]) || L[Pos] == u'_'))
			{
				if (L[Pos] == u'_')
				{
					if (bPrevUnderscore || Pos == Begin)
					{
						bOk = false;
					}
					bPrevUnderscore = true;
				}
				else
				{
					bPrevUnderscore = false;
				}
				++Pos;
			}
			if (bPrevUnderscore || (bRequireOne && Pos == Begin))
			{
				bOk = false;
			}
			return bOk;
		}

		bool FollowsKeywordAfterNumber() const
		{
			static constexpr std::u16string_view Words[] = {u"and", u"else", u"for", u"if", u"in", u"is", u"not", u"or"};
			const std::u16string_view Rest = L.substr(Pos);
			for (std::u16string_view W : Words)
			{
				if (Rest.substr(0, W.size()) == W)
				{
					return true;
				}
			}
			return false;
		}

		bool IdentifierAt(size_t At) const
		{
			if (At >= Len)
			{
				return false;
			}
			size_t Units = 1;
			return utf::IsIdentifierContinue(utf::DecodeAt(L, At, Units));
		}

		void SkipIdentifier()
		{
			while (Pos < Len)
			{
				size_t Units = 1;
				if (!utf::IsIdentifierContinue(utf::DecodeAt(L, Pos, Units)))
				{
					break;
				}
				Pos += Units;
			}
		}

		void LexNumber()
		{
			const size_t Col = Pos;
			bool bOk = true;
			const char16_t C0 = L[Pos];
			const char16_t C1 = Pos + 1 < Len ? Lower(L[Pos + 1]) : 0;
			if (C0 == u'0' && (C1 == u'x' || C1 == u'o' || C1 == u'b'))
			{
				Pos += 2;
				bool (*Fn)(char16_t) = C1 == u'x' ? IsHexDigit : (C1 == u'o' ? IsOctDigit : +[](char16_t C) { return C == u'0' || C == u'1'; });
				if (Pos < Len && L[Pos] == u'_')
				{
					++Pos;   // 0x_1f is valid: one underscore may follow the prefix
				}
				bOk = ScanDigits(Fn, true);
			}
			else
			{
				bool bFloatOrImag = false;
				bool bNonZero = false;
				if (C0 != u'.')
				{
					const size_t IntBegin = Pos;
					bOk = ScanDigits(IsDigit, true);
					for (size_t I = IntBegin; I < Pos; ++I)
					{
						if (L[I] >= u'1' && L[I] <= u'9')
						{
							bNonZero = true;
						}
					}
				}
				if (Pos < Len && L[Pos] == u'.')
				{
					bFloatOrImag = true;
					++Pos;
					if (Pos < Len && IsDigit(L[Pos]))
					{
						bOk = ScanDigits(IsDigit, true) && bOk;
					}
				}
				if (Pos < Len && (L[Pos] == u'e' || L[Pos] == u'E'))
				{
					size_t J = Pos + 1;
					if (J < Len && (L[J] == u'+' || L[J] == u'-'))
					{
						++J;
					}
					if (J < Len && IsDigit(L[J]))
					{
						bFloatOrImag = true;
						Pos = J;
						bOk = ScanDigits(IsDigit, true) && bOk;
					}
					else if (!(L.substr(Pos, 4) == u"else"))
					{
						bOk = false;   // "1e", "1e+": invalid decimal literal
						Pos = J;
					}
				}
				if (Pos < Len && (L[Pos] == u'j' || L[Pos] == u'J'))
				{
					bFloatOrImag = true;
					++Pos;
				}
				if (C0 == u'0' && bNonZero && !bFloatOrImag)
				{
					bOk = false;   // leading zeros in decimal integer literals are not permitted
				}
			}
			if (IdentifierAt(Pos) && !FollowsKeywordAfterNumber())
			{
				bOk = false;       // "1abc": invalid decimal literal; the name chars join the error
				SkipIdentifier();
			}
			Emit(Col, bOk ? TokenKind::Number : TokenKind::Error);
			NoteSig(false);
			Prev = Ctx::Other;
		}

		void LexName()
		{
			const size_t Col = Pos;
			SkipIdentifier();
			const std::u16string_view Word = L.substr(Col, Pos - Col);
			if (Pos < Len && IsQuote(L[Pos]))
			{
				Frame F;
				if (ParsePrefix(Word, F))
				{
					BeginString(Col, F);
					return;
				}
			}
			TokenKind Kind = TokenKind::Text;
			Ctx Next = Ctx::Other;
			if (IsKeywordWord(Word))
			{
				Kind = (Word == u"True" || Word == u"False" || Word == u"None") ? TokenKind::KeywordConstant : TokenKind::Keyword;
				if (Word == u"def")
				{
					Next = Ctx::Def;
				}
				else if (Word == u"class")
				{
					Next = Ctx::Class;
				}
			}
			else if (Prev == Ctx::Def)
			{
				Kind = TokenKind::FunctionName;
			}
			else if (Prev == Ctx::Class)
			{
				Kind = TokenKind::ClassName;
			}
			else if (Prev == Ctx::At)
			{
				Kind = TokenKind::Decorator;
				Next = Ctx::DecoratorName;
			}
			else if (Word == u"self" || Word == u"cls")
			{
				Kind = TokenKind::SelfParam;
			}
			else if (Prev != Ctx::Dot && IsBuiltinWord(Word))
			{
				Kind = TokenKind::Builtin;
			}
			Emit(Col, Kind);
			NoteSig(false);
			Prev = Next;
		}

		void LexOperator(bool bInField)
		{
			const size_t Col = Pos;
			const char16_t C = L[Pos];
			const char16_t N1 = Pos + 1 < Len ? L[Pos + 1] : 0;
			const char16_t N2 = Pos + 2 < Len ? L[Pos + 2] : 0;
			switch (C)
			{
			case u'(':
			case u'[':
			case u'{':
				if (bInField)
				{
					Frame& F = Top();
					F.Depth = static_cast<uint8_t>(std::min(255, F.Depth + 1));
				}
				else
				{
					++LocalDepth;
					++BracketDelta;
				}
				Emit(Col, TokenKind::Bracket);
				++Pos;
				NoteSig(false);
				Prev = Ctx::Other;
				return;
			case u')':
			case u']':
			case u'}':
			{
				TokenKind Kind = TokenKind::Bracket;
				if (bInField)
				{
					Frame& F = Top();
					if (F.Depth > 1)
					{
						--F.Depth;
					}
					else
					{
						Kind = TokenKind::Error;   // a closer with no opener inside the field
					}
				}
				else
				{
					--LocalDepth;
					--BracketDelta;
				}
				Emit(Col, Kind);
				++Pos;
				NoteSig(false);
				Prev = Ctx::Other;
				return;
			}
			case u'@':
				if (!bInField && FirstSigTok < 0 && StartsAtLogicalLine() && N1 != u'=')
				{
					Emit(Col, TokenKind::Decorator);
					ExtraFlags |= LF_Decorator;
					++Pos;
					NoteSig(false);
					Prev = Ctx::At;
					return;
				}
				break;
			case u'.':
				if (N1 == u'.' && N2 == u'.')
				{
					Emit(Col, TokenKind::Operator);
					Pos += 3;
					NoteSig(false);
					Prev = Ctx::Other;
					return;
				}
				if (Prev == Ctx::DecoratorName)
				{
					Emit(Col, TokenKind::Decorator);
					++Pos;
					NoteSig(false);
					Prev = Ctx::At;
					return;
				}
				Emit(Col, TokenKind::Operator);
				++Pos;
				NoteSig(false);
				Prev = Ctx::Dot;
				return;
			case u':':
				if (N1 == u'=')
				{
					Emit(Col, TokenKind::Operator);
					Pos += 2;
					NoteSig(false);
					Prev = Ctx::Other;
					return;
				}
				Emit(Col, TokenKind::Operator);
				++Pos;
				NoteSig(!bInField && LocalDepth <= 0);
				Prev = Ctx::Other;
				return;
			default:
				break;
			}

			size_t OpLen = 0;
			if ((C == u'*' && N1 == u'*' && N2 == u'=') || (C == u'/' && N1 == u'/' && N2 == u'=')
				|| (C == u'>' && N1 == u'>' && N2 == u'=') || (C == u'<' && N1 == u'<' && N2 == u'='))
			{
				OpLen = 3;
			}
			else if ((N1 == u'=' && (C == u'!' || C == u'%' || C == u'&' || C == u'*' || C == u'+' || C == u'-' || C == u'/'
						|| C == u'<' || C == u'=' || C == u'>' || C == u'@' || C == u'^' || C == u'|'))
				|| (C == u'*' && N1 == u'*') || (C == u'/' && N1 == u'/') || (C == u'<' && N1 == u'<') || (C == u'>' && N1 == u'>')
				|| (C == u'-' && N1 == u'>'))
			{
				OpLen = 2;
			}
			else if (C == u'%' || C == u'&' || C == u'*' || C == u'+' || C == u',' || C == u'-' || C == u'/' || C == u';'
				|| C == u'<' || C == u'=' || C == u'>' || C == u'@' || C == u'^' || C == u'|' || C == u'~')
			{
				OpLen = 1;
			}
			if (OpLen > 0)
			{
				Emit(Col, TokenKind::Operator);
				Pos += OpLen;
				NoteSig(false);
				Prev = Ctx::Other;
				return;
			}
			// Not Python: '!', '$', '?', '`', NUL, non-identifier code points, lone surrogates.
			size_t Units = 1;
			utf::DecodeAt(L, Pos, Units);
			Emit(Col, TokenKind::Error);
			Pos += Units;
			NoteSig(false);
			Prev = Ctx::Other;
		}

		void LexCodeToken(bool bInField)
		{
			const size_t Col = Pos;
			const char16_t C = L[Pos];
			if (IsBlank(C))
			{
				while (Pos < Len && IsBlank(L[Pos]))
				{
					++Pos;
				}
				Emit(Col, TokenKind::Text);
				return;
			}
			if (C == u'#')
			{
				if (bInField && bPy311)
				{
					Emit(Col, TokenKind::Error);   // '#' in an f-string field is an error in 3.11
					++Pos;
					NoteSig(false);
					return;
				}
				LexComment();
				return;
			}
			if (C == u'\\')
			{
				if (Pos + 1 == Len)
				{
					if (bInField)
					{
						// Py311: an error inside the field, but the tokenizer still continues the string.
						Emit(Col, bPy311 ? TokenKind::Error : TokenKind::Text);
						bStrBackslashEol = true;
					}
					else
					{
						Emit(Col, TokenKind::Text);
						bCodeBackslashEol = true;
					}
					++Pos;
					return;
				}
				Emit(Col, TokenKind::Error);   // unexpected character after line continuation character
				++Pos;
				NoteSig(false);
				Prev = Ctx::Other;
				return;
			}
			if (IsDigit(C) || (C == u'.' && Pos + 1 < Len && IsDigit(L[Pos + 1])))
			{
				LexNumber();
				return;
			}
			if (IsQuote(C))
			{
				BeginString(Col, Frame());
				return;
			}
			size_t Units = 1;
			if (utf::IsIdentifierStart(utf::DecodeAt(L, Pos, Units)))
			{
				LexName();
				return;
			}
			if (bInField)
			{
				Frame& F = Top();
				if (F.Depth <= 1)
				{
					if (C == u'}')
					{
						Emit(Col, TokenKind::FStringBrace);
						++Pos;
						NoteSig(false);
						const size_t Index = static_cast<size_t>(S.NumFrames) - 1;
						if (F.P == Part::Expr)
						{
							F.P = Part::Literal;
							FieldStartCol[Index] = -1;
						}
						else
						{
							F.P = Part::Spec;
						}
						F.Depth = 0;
						Prev = Ctx::Other;
						return;
					}
					if (C == u':')
					{
						Emit(Col, TokenKind::Operator);
						++Pos;
						NoteSig(false);
						F.P = F.P == Part::Expr ? Part::Spec : Part::Spec2;
						Prev = Ctx::Other;
						return;
					}
					if (C == u'!' && Pos + 1 < Len && L[Pos + 1] != u'=')
					{
						const char16_t K = L[Pos + 1];
						const bool bConv = K == u'r' || K == u's' || K == u'a';
						Emit(Col, bConv ? TokenKind::FStringConversion : TokenKind::Error);
						Pos += bConv ? 2 : 1;
						NoteSig(false);
						Prev = Ctx::Other;
						return;
					}
				}
			}
			LexOperator(bInField);
		}

		void EndOfLine()
		{
			if (S.NumFrames == 0 || bStrBackslashEol)
			{
				return;
			}
			auto DropFrom = [this](int Index) {
				ErrorFrom(StrStartCol[static_cast<size_t>(Index)]);
				S.NumFrames = static_cast<uint8_t>(Index);
			};
			if (bPy311)
			{
				// Only the outermost string spans lines for the 3.11 tokenizer; a nested single-quoted one cannot.
				for (int J = 1; J < S.NumFrames; ++J)
				{
					if (!S.Frames[static_cast<size_t>(J)].bTriple)
					{
						DropFrom(J);
						break;
					}
				}
				if (!S.Frames[0].bTriple)
				{
					DropFrom(0);   // unterminated string literal
				}
				return;
			}
			// PEP 701: an expression part may span lines; an unterminated single-quoted literal may not.
			while (S.NumFrames > 0)
			{
				const Frame& F = Top();
				if (F.bTriple || F.P == Part::Expr || F.P == Part::Expr2)
				{
					break;
				}
				DropFrom(S.NumFrames - 1);
			}
		}

		void PostPassSoftKeywords()
		{
			// Soft keywords (§4.3.1): first token of a logical line, the physical line ends with ':' at depth 0, and the
			// next token is not '=', '.', ',' or ')'.
			if (FirstSigTok < 0 || !StartsAtLogicalLine() || !bLastSigColon || S.NumFrames != 0)
			{
				return;
			}
			const size_t First = static_cast<size_t>(FirstSigTok);
			if (First >= Toks.size() || TokenKindOf(Toks[First]) != TokenKind::Text)
			{
				return;
			}
			const std::u16string_view Word = TokenText(First);
			if (Word != u"match" && Word != u"case")
			{
				return;
			}
			size_t Next = First + 1;
			while (Next < Toks.size() && (IsWhitespaceToken(Next) || TokenKindOf(Toks[Next]) == TokenKind::Comment))
			{
				++Next;
			}
			if (Next < Toks.size())
			{
				const std::u16string_view T = TokenText(Next);
				if (T == u"=" || T == u"." || T == u"," || T == u")")
				{
					return;
				}
			}
			Toks[First] = PackToken(TokenStart(Toks[First]), TokenKind::SoftKeyword);
			if (Word == u"case")
			{
				for (size_t I = First + 1; I < Toks.size(); ++I)
				{
					if (TokenKindOf(Toks[I]) == TokenKind::Text && TokenText(I) == u"_")
					{
						Toks[I] = PackToken(TokenStart(Toks[I]), TokenKind::SoftKeyword);
					}
				}
			}
		}

		void PostPassDocstring()
		{
			// A docstring is a lone string on its line; a string that closes here and is followed by code is a String.
			if (DocTok < 0 || !bDocClosed || !bSigAfterDoc)
			{
				return;
			}
			for (size_t I = static_cast<size_t>(DocTok); I < DocCloseTokEnd && I < Toks.size(); ++I)
			{
				if (TokenKindOf(Toks[I]) == TokenKind::Docstring)
				{
					Toks[I] = PackToken(TokenStart(Toks[I]), TokenKind::String);
				}
			}
		}

		LineFacts BuildFacts() const
		{
			LineFacts F;
			int32_t Col = 0;
			size_t I = 0;
			for (; I < Len; ++I)
			{
				const char16_t C = L[I];
				if (C == u' ')
				{
					++Col;
				}
				else if (C == u'\t')
				{
					Col = (Col / 8 + 1) * 8;
				}
				else if (C == u'\f')
				{
					Col = 0;
				}
				else
				{
					break;
				}
			}
			F.IndentCols = Col;
			F.BracketDelta = BracketDelta;
			uint16_t Flags = ExtraFlags;
			if (I == Len)
			{
				Flags |= LF_Blank;
			}
			else
			{
				F.FirstTokenCol = static_cast<int32_t>(I);
				F.FirstTokenKind = TokenKind::Text;
				for (size_t T = 0; T < Toks.size(); ++T)
				{
					if (TokenStart(Toks[T]) > F.FirstTokenCol)
					{
						break;
					}
					F.FirstTokenKind = TokenKindOf(Toks[T]);
				}
			}
			if (!bAnySig && bHasComment)
			{
				Flags |= LF_CommentOnly;
			}
			if (Start.NumFrames > 0)
			{
				Flags |= LF_StartsInString;
			}
			if (Start.bBackslash)
			{
				Flags |= LF_StartsContinuation;
			}
			if (bLastSigColon && S.NumFrames == 0)
			{
				Flags |= LF_EndsWithColon;
			}
			if (S.NumFrames > 0)
			{
				Flags |= LF_EndsInString;
			}
			if (S.NumFrames == 0 && bCodeBackslashEol)
			{
				Flags |= LF_EndsWithBackslash;
			}
			F.Flags = Flags;
			return F;
		}

		State BuildEndState() const
		{
			State E = S;
			E.bBackslash = S.NumFrames == 0 && bCodeBackslashEol;
			if (bAnySig)
			{
				E.bAfterColon = bLastSigColon && S.NumFrames == 0 && !E.bBackslash;
				E.bModuleTop = false;
			}
			else
			{
				E.bAfterColon = Start.bAfterColon;
				E.bModuleTop = Start.bModuleTop;
			}
			return E;
		}
	};

	/** Lexes a whole snapshot or text with a private interner (the T-LEX-007 oracle and the REPL pre-check). */
	void LexLines(const TextSnapshot* Snapshot, std::u16string_view Text, LexGrammar Grammar, std::vector<std::vector<uint32_t>>& OutTokens,
		std::vector<LineFacts>* OutFacts, State& OutEnd)
	{
		OutTokens.clear();
		if (OutFacts != nullptr)
		{
			OutFacts->clear();
		}
		State Cur = InitialState();
		auto LexOne = [&](std::u16string_view Line) {
			std::vector<uint32_t> Tokens;
			LineFacts Facts;
			State End;
			LineLexer Lexer(Line, Cur, Grammar, Tokens);
			Lexer.Run(Facts, End);
			OutTokens.push_back(std::move(Tokens));
			if (OutFacts != nullptr)
			{
				OutFacts->push_back(Facts);
			}
			Cur = End;
		};
		if (Snapshot != nullptr)
		{
			std::u16string Scratch;
			const LineNo Count = Snapshot->LineCount();
			for (LineNo Line = 0; Line < Count; ++Line)
			{
				LexOne(Snapshot->LineView(Line, Scratch));
			}
		}
		else
		{
			size_t From = 0;
			for (;;)
			{
				const size_t Nl = Text.find(u'\n', From);
				if (Nl == std::u16string_view::npos)
				{
					LexOne(Text.substr(From));
					break;
				}
				LexOne(Text.substr(From, Nl - From));
				From = Nl + 1;
			}
		}
		OutEnd = Cur;
	}

	LexStateInfo Describe(const State& S)
	{
		LexStateInfo Info;
		Info.bInString = S.NumFrames > 0;
		Info.bTriple = S.NumFrames > 0 && S.Frames[0].bTriple;
		Info.bBackslashJoin = S.bBackslash;
		Info.FrameDepth = S.NumFrames;
		for (int I = 0; I < S.NumFrames; ++I)
		{
			if (S.Frames[static_cast<size_t>(I)].bF)
			{
				Info.bFString = true;
			}
		}
		return Info;
	}

	struct LineRec
	{
		std::vector<uint32_t> Tokens;
		LexStateId Start = NormalState;
		LexStateId End = NormalState;
		LineFacts Facts;
		bool bDirty = true;
	};
}

namespace dream
{
	struct PyLexer::Impl
	{
		LexGrammar Grammar = LexGrammar::Py311;
		lexer_detail::Interner States;
		LexStateId Initial = NormalState;
		lexer_detail::GapArray<lexer_detail::LineRec> Lines;
		LineNo FrontierLine = 0;   // every line above it is clean and consistent with the line before it

		explicit Impl(LexGrammar InGrammar)
			: Grammar(InGrammar)
		{
			Initial = States.Intern(lexer_detail::InitialState());
			Lines.resize(1);
		}

		void ResetLines(LineNo Count)
		{
			Lines.clear();
			Lines.resize(static_cast<size_t>(std::max<LineNo>(Count, 1)));
			FrontierLine = 0;
		}

		void LexLine(const TextSnapshot& Snapshot, LineNo Line, std::u16string& Scratch)
		{
			lexer_detail::LineRec& Rec = Lines[static_cast<size_t>(Line)];
			const LexStateId StartId = Line == 0 ? Initial : Lines[static_cast<size_t>(Line) - 1].End;
			lexer_detail::State End;
			lexer_detail::LineLexer Lexer(Snapshot.LineView(Line, Scratch), States.Get(StartId), Grammar, Rec.Tokens);
			Lexer.Run(Rec.Facts, End);
			Rec.Start = StartId;
			Rec.End = States.Intern(End);
			Rec.bDirty = false;
		}

		/**
		 * The time budget of one Relex call (§4.3.3 steps 3-4). After each line it predicts the next line's cost by the
		 * slowest line so far and reports the budget spent when that line would pass it, so a call ends within its
		 * budget unless one line costs more than all before it. BudgetMs <= 0: no budget.
		 */
		struct Budget
		{
			std::chrono::steady_clock::time_point Began = std::chrono::steady_clock::now();
			double BudgetMs = 0.0;
			double LastMs = 0.0;
			double SlowestLineMs = 0.0;

			bool SpentAfterLine()
			{
				if (BudgetMs <= 0.0)
				{
					return false;
				}
				const double Elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Began).count();
				SlowestLineMs = std::max(SlowestLineMs, Elapsed - LastMs);
				LastMs = Elapsed;
				return Elapsed + SlowestLineMs >= BudgetMs;
			}
		};

		/** The sequential pass of §4.3.3 step 2; stops at the budget (after at least one line) or once the frontier
		 *  passes StopAfter. */
		int32_t Sequential(const TextSnapshot& Snapshot, Budget& Clock, LineNo StopAfter)
		{
			std::u16string Scratch;
			int32_t Count = 0;
			const LineNo N = static_cast<LineNo>(Lines.size());
			LineNo Line = FrontierLine;
			while (Line < N && FrontierLine <= StopAfter)
			{
				lexer_detail::LineRec& Rec = Lines[static_cast<size_t>(Line)];
				const LexStateId StartId = Line == 0 ? Initial : Lines[static_cast<size_t>(Line) - 1].End;
				if (!Rec.bDirty && Rec.Start == StartId)
				{
					// Unchanged start state on a clean line: everything up to the next dirty line is current.
					LineNo Next = Line + 1;
					while (Next < N && !Lines[static_cast<size_t>(Next)].bDirty)
					{
						++Next;
					}
					Line = Next;
					FrontierLine = Next;
					continue;
				}
				LexLine(Snapshot, Line, Scratch);
				++Count;
				++Line;
				FrontierLine = Line;
				if (Clock.SpentAfterLine())
				{
					break;
				}
			}
			if (FrontierLine < N)
			{
				// Stopped early (budget, StopAfter): the lines from the frontier on were not checked against the lines above
				// them. Mark the first one dirty, so a later pass that starts higher (an edit above the frontier) cannot skip
				// past it as "clean"; the flag travels with the record through OnChange's splices.
				Lines[static_cast<size_t>(FrontierLine)].bDirty = true;
			}
			return Count;
		}
	};

	PyLexer::PyLexer(LexGrammar Grammar)
		: State(std::make_unique<Impl>(Grammar))
	{
	}

	PyLexer::~PyLexer() = default;
	PyLexer::PyLexer(PyLexer&& Other) noexcept = default;
	PyLexer& PyLexer::operator=(PyLexer&& Other) noexcept = default;

	LexGrammar PyLexer::GetGrammar() const
	{
		return State->Grammar;
	}

	void PyLexer::Reset(const TextSnapshot& Snapshot)
	{
		State->ResetLines(Snapshot.LineCount());
	}

	void PyLexer::OnChange(const ChangeSet& Changes, const TextSnapshot& After)
	{
		Impl& I = *State;
		if (Changes.Ranges.empty())
		{
			return;
		}
		for (auto It = Changes.Ranges.rbegin(); It != Changes.Ranges.rend(); ++It)
		{
			const size_t First = static_cast<size_t>(It->StartLine);
			const size_t OldLines = static_cast<size_t>(It->OldLines);
			const size_t NewLines = static_cast<size_t>(It->NewLines);
			if (First + OldLines >= I.Lines.size())
			{
				I.ResetLines(After.LineCount());   // the records do not match this ChangeSet: start over
				return;
			}
			if (OldLines != 0 || NewLines != 0)
			{
				I.Lines.Splice(First + 1, OldLines, NewLines);
			}
			I.Lines[First].bDirty = true;
		}
		I.FrontierLine = std::min(I.FrontierLine, Changes.Ranges.front().StartLine);
		if (static_cast<LineNo>(I.Lines.size()) != After.LineCount())
		{
			I.ResetLines(After.LineCount());
		}
	}

	int32_t PyLexer::Relex(const TextSnapshot& Snapshot, double BudgetMs, LineNo PriorityFirst, LineNo PriorityLast)
	{
		Impl& I = *State;
		Impl::Budget Clock;
		Clock.BudgetMs = BudgetMs;
		if (static_cast<LineNo>(I.Lines.size()) != Snapshot.LineCount())
		{
			I.ResetLines(Snapshot.LineCount());
		}
		int32_t Count = 0;
		const LineNo N = static_cast<LineNo>(I.Lines.size());
		if (BudgetMs > 0.0 && PriorityFirst > I.FrontierLine && PriorityFirst < N)
		{
			// Visible lines first (§4.3.3 step 4): a provisional lex from the stored state above. The lines stay dirty, so
			// the sequential pass relexes them once it reaches them with a verified start state.
			std::u16string Scratch;
			const LineNo Last = std::min(PriorityLast, N - 1);
			for (LineNo Line = PriorityFirst; Line <= Last; ++Line)
			{
				if (I.Lines[static_cast<size_t>(Line)].bDirty)
				{
					I.LexLine(Snapshot, Line, Scratch);
					I.Lines[static_cast<size_t>(Line)].bDirty = true;
					++Count;
					if (Clock.SpentAfterLine())
					{
						break;   // the sequential pass below still lexes its first line (progress), then stops
					}
				}
			}
		}
		Count += I.Sequential(Snapshot, Clock, N);
		return Count;
	}

	void PyLexer::RelexThrough(const TextSnapshot& Snapshot, LineNo Last)
	{
		Impl& I = *State;
		if (static_cast<LineNo>(I.Lines.size()) != Snapshot.LineCount())
		{
			I.ResetLines(Snapshot.LineCount());
		}
		Impl::Budget NoBudget;
		I.Sequential(Snapshot, NoBudget, Last);
	}

	LineNo PyLexer::Frontier() const
	{
		return std::min<LineNo>(State->FrontierLine, static_cast<LineNo>(State->Lines.size()));
	}

	bool PyLexer::IsClean(LineNo Line) const
	{
		return Line >= 0 && Line < Frontier();
	}

	LineNo PyLexer::LineCount() const
	{
		return static_cast<LineNo>(State->Lines.size());
	}

	std::span<const uint32_t> PyLexer::Tokens(LineNo Line) const
	{
		if (Line < 0 || Line >= LineCount())
		{
			return {};
		}
		const std::vector<uint32_t>& T = State->Lines[static_cast<size_t>(Line)].Tokens;
		return std::span<const uint32_t>(T.data(), T.size());
	}

	LineFacts PyLexer::Facts(LineNo Line) const
	{
		if (Line < 0 || Line >= LineCount())
		{
			return LineFacts();
		}
		return State->Lines[static_cast<size_t>(Line)].Facts;
	}

	LexStateId PyLexer::StartState(LineNo Line) const
	{
		if (Line < 0 || Line >= LineCount())
		{
			return NormalState;
		}
		return State->Lines[static_cast<size_t>(Line)].Start;
	}

	LexStateId PyLexer::EndState(LineNo Line) const
	{
		if (Line < 0 || Line >= LineCount())
		{
			return NormalState;
		}
		return State->Lines[static_cast<size_t>(Line)].End;
	}

	LexStateInfo PyLexer::DescribeState(LexStateId Id) const
	{
		return lexer_detail::Describe(State->States.Get(Id));
	}

	void PyLexer::LexFull(const TextSnapshot& Snapshot, LexGrammar Grammar, std::vector<std::vector<uint32_t>>& OutTokens)
	{
		lexer_detail::State End;
		lexer_detail::LexLines(&Snapshot, std::u16string_view(), Grammar, OutTokens, nullptr, End);
	}

	void PyLexer::LexText(std::u16string_view Text, LexGrammar Grammar, std::vector<std::vector<uint32_t>>& OutTokens,
		std::vector<LineFacts>& OutFacts, LexStateId& OutEndState, LexStateInfo& OutEndInfo)
	{
		lexer_detail::State End;
		lexer_detail::LexLines(nullptr, Text, Grammar, OutTokens, &OutFacts, End);
		lexer_detail::Interner Local;
		Local.Intern(lexer_detail::InitialState());
		OutEndState = Local.Intern(End);
		OutEndInfo = lexer_detail::Describe(End);
	}

	bool PyLexer::IsKeyword(std::u16string_view Word)
	{
		return lexer_detail::IsKeywordWord(Word);
	}

	bool PyLexer::IsBuiltin(std::u16string_view Word)
	{
		return lexer_detail::IsBuiltinWord(Word);
	}
}
