// SPDX-License-Identifier: Apache-2.0
// Dream core: the outline of one Python text (NAV-02, TDD §7.9.2 without a server, owner ruling PO-05 §3). The rules are
// in dream/Outline.h. One pass over the lines with a small scanner (strings, comments, brackets, backslash joins); another
// pass over the tail only when a triple-quoted string is still open at the end, at most MaxStringRecoveries times (lines
// like \""" open a string that their escaped quotes never close, so the recoveries are capped, not bounded by the text).
#include "dream/Outline.h"

#include "SearchRegex.h"   // regex_detail::FoldCp: the filter folds case as Python's re.IGNORECASE does
#include "dream/Utf.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::outline_detail
{
	bool IsBlankUnit(char16_t C)
	{
		return C == u' ' || C == u'\t' || C == u'\f';
	}

	/** A unit of a word: ASCII letters, digits and '_', and anything non-ASCII (identifiers, numbers, string prefixes). */
	bool IsWordUnit(char16_t C)
	{
		return (C >= u'a' && C <= u'z') || (C >= u'A' && C <= u'Z') || (C >= u'0' && C <= u'9') || C == u'_' || C >= 0x80;
	}

	size_t WordEnd(std::u16string_view L, size_t At)
	{
		while (At < L.size() && IsWordUnit(L[At]))
		{
			++At;
		}
		return At;
	}

	size_t SkipBlanks(std::u16string_view L, size_t At)
	{
		while (At < L.size() && IsBlankUnit(L[At]))
		{
			++At;
		}
		return At;
	}

	/** A first word that can never continue an expression: a line starting with it starts a logical line (broken code). */
	bool StartsStatementOnly(std::u16string_view L, size_t At)
	{
		const size_t End = WordEnd(L, At);
		const std::u16string_view W = L.substr(At, End - At);
		if (W == u"async")
		{
			// `async for` continues a comprehension; only `async def` cannot.
			const size_t Next = SkipBlanks(L, End);
			return L.substr(Next, WordEnd(L, Next) - Next) == u"def";
		}
		static constexpr std::u16string_view Words[] = {
			u"def", u"class", u"import", u"return", u"pass", u"raise", u"with", u"while", u"try", u"except", u"finally",
			u"del", u"global", u"nonlocal", u"assert", u"break", u"continue", u"elif",
		};
		for (const std::u16string_view Word : Words)
		{
			if (W == Word)
			{
				return true;
			}
		}
		return false;
	}

	/** The scanner's state between lines. */
	struct LineState
	{
		char16_t Quote = 0;      // the open string's quote, 0 outside strings
		bool bTriple = false;
		int32_t Depth = 0;       // open brackets
		bool bJoin = false;      // the last line ended with a backslash outside a string
	};

	class Scanner
	{
	public:
		std::vector<OutlineEntry> Out;

		template <class GetLineFn>
		void Run(LineNo Count, GetLineFn&& GetLine)
		{
			LineNo Line = 0;
			for (;;)
			{
				for (; Line < Count; ++Line)
				{
					ProcessLine(Line, GetLine(Line));
				}
				const bool bOpenAtEnd = S.Quote != 0 && S.bTriple && TripleOpenLine >= 0 && TripleOpenLine + 1 < Count;
				if (bOpenAtEnd && Recoveries < MaxStringRecoveries)
				{
					++Recoveries;
					// Broken code: the string never closes. Take it as closed at the end of its first line and scan the rest
					// again. Nothing was added or closed since that line (every line after it was inside the string).
					Line = TripleOpenLine + 1;
					LastSig = TripleOpenLine;
					LastNonBlank = TripleOpenLine;
					TrailingComments.clear();
					S = LineState();
					TripleOpenLine = -1;
					continue;
				}
				break;
			}
			CloseTo(-1);
			CloseCell(LastNonBlank);
		}

	private:
		struct OpenEntry
		{
			int32_t Entry = 0;
			int32_t Indent = 0;
		};

		LineState S;
		std::vector<OpenEntry> Stack;
		int32_t CurrentCell = -1;
		LineNo LastSig = -1;                                  // the last line of the last logical line
		LineNo LastNonBlank = -1;
		std::vector<std::pair<LineNo, int32_t>> TrailingComments;   // comment-only lines (line, indent) after LastSig
		LineNo DecoratorLine = -1;
		int32_t DecoratorIndent = 0;
		LineNo TripleOpenLine = -1;                           // the line of the last triple-quoted opener
		int32_t Recoveries = 0;                               // unclosed-string recoveries so far (capped)

		void MarkSig(LineNo Line)
		{
			LastSig = Line;
			TrailingComments.clear();
		}

		void ProcessLine(LineNo Line, std::u16string_view Text)
		{
			if (!Text.empty() && Text.back() == u'\r')
			{
				Text.remove_suffix(1);
			}
			// Indentation as the tokenizer counts it: tabs to the next multiple of 8, a form feed resets the column.
			int32_t Indent = 0;
			size_t P = 0;
			for (; P < Text.size(); ++P)
			{
				const char16_t C = Text[P];
				if (C == u' ')
				{
					++Indent;
				}
				else if (C == u'\t')
				{
					Indent = (Indent / 8 + 1) * 8;
				}
				else if (C == u'\f')
				{
					Indent = 0;
				}
				else
				{
					break;
				}
			}
			const bool bBlank = P == Text.size();
			const LineNo PrevNonBlank = LastNonBlank;
			if (!bBlank)
			{
				LastNonBlank = Line;
			}

			if (S.Quote != 0)
			{
				// Inside a string that spans lines (triple-quoted, or a backslash-newline in a single-quoted one).
				ScanRest(Line, Text, 0);
				MarkSig(Line);
				return;
			}
			if (bBlank)
			{
				S.bJoin = false;
				return;
			}
			const bool bContinuation = S.Depth > 0 || S.bJoin;
			if (bContinuation && !StartsStatementOnly(Text, P))
			{
				ScanRest(Line, Text, P);
				MarkSig(Line);
				return;
			}
			// A logical line starts here (after a recovery when the brackets or the join were still open).
			S.Depth = 0;
			S.bJoin = false;

			if (Text[P] == u'#')
			{
				const size_t Mark = SkipBlanks(Text, P + 1);
				if (Text.substr(Mark, 2) == u"%%")
				{
					AddCell(Line, P, Indent, Text.substr(Mark + 2), PrevNonBlank);
				}
				else
				{
					TrailingComments.emplace_back(Line, Indent);
				}
				return;
			}

			CloseTo(Indent);
			size_t After = P;
			if (Text[P] == u'@')
			{
				if (DecoratorLine < 0 || DecoratorIndent != Indent)
				{
					DecoratorLine = Line;
					DecoratorIndent = Indent;
				}
				After = P + 1;
			}
			else
			{
				After = TryAddEntry(Line, Text, P, Indent);
			}
			ScanRest(Line, Text, After);
			MarkSig(Line);
		}

		/** A def, class or async def at P: adds the entry and returns where its name ends; else P (no entry). */
		size_t TryAddEntry(LineNo Line, std::u16string_view Text, size_t P, int32_t Indent)
		{
			size_t End = WordEnd(Text, P);
			std::u16string_view Word = Text.substr(P, End - P);
			bool bAsync = false;
			if (Word == u"async")
			{
				const size_t Next = SkipBlanks(Text, End);
				const size_t NextEnd = WordEnd(Text, Next);
				if (Text.substr(Next, NextEnd - Next) == u"def")
				{
					bAsync = true;
					Word = u"def";
					End = NextEnd;
				}
			}
			const bool bClass = Word == u"class";
			if (!bClass && Word != u"def")
			{
				DecoratorLine = -1;   // any other statement ends a decorator run
				return P;
			}
			if (End >= Text.size() || !IsBlankUnit(Text[End]))
			{
				DecoratorLine = -1;
				return End;
			}
			const size_t NameStart = SkipBlanks(Text, End);
			size_t NameEnd = NameStart;
			while (NameEnd < Text.size())
			{
				size_t Units = 1;
				const char32_t C = utf::DecodeAt(Text, NameEnd, Units);
				const bool bOk = NameEnd == NameStart ? utf::IsIdentifierStart(C) : utf::IsIdentifierContinue(C);
				if (!bOk)
				{
					break;
				}
				NameEnd += Units;
			}
			if (NameEnd == NameStart)
			{
				DecoratorLine = -1;   // "def" with no name yet (typing): no entry
				return NameStart;
			}

			OutlineEntry E;
			E.Name.assign(Text.substr(NameStart, NameEnd - NameStart));
			E.Line = Line;
			E.Column = static_cast<int32_t>(NameStart);
			E.EndLine = Line;
			E.FirstLine = (DecoratorLine >= 0 && DecoratorIndent == Indent) ? DecoratorLine : Line;
			E.IndentCols = Indent;
			DecoratorLine = -1;
			bool bInClass = false;
			if (!Stack.empty())
			{
				E.Parent = Stack.back().Entry;
				E.Depth = Out[static_cast<size_t>(E.Parent)].Depth + 1;
				bInClass = Out[static_cast<size_t>(E.Parent)].Kind == OutlineKind::Class;
			}
			else if (CurrentCell >= 0)
			{
				E.Parent = CurrentCell;
				E.Depth = 1;
			}
			if (bClass)
			{
				E.Kind = OutlineKind::Class;
			}
			else if (bAsync)
			{
				E.Kind = bInClass ? OutlineKind::AsyncMethod : OutlineKind::AsyncFunction;
			}
			else
			{
				E.Kind = bInClass ? OutlineKind::Method : OutlineKind::Function;
			}
			Stack.push_back(OpenEntry{ static_cast<int32_t>(Out.size()), Indent });
			Out.push_back(std::move(E));
			return NameEnd;
		}

		void AddCell(LineNo Line, size_t Hash, int32_t Indent, std::u16string_view Title, LineNo PrevNonBlank)
		{
			CloseCell(PrevNonBlank);
			DecoratorLine = -1;   // a cell header ends a decorator run (so FirstLine never reaches back over a cell)
			size_t First = 0;
			size_t Last = Title.size();
			while (First < Last && IsBlankUnit(Title[First]))
			{
				++First;
			}
			while (Last > First && IsBlankUnit(Title[Last - 1]))
			{
				--Last;
			}
			OutlineEntry E;
			E.Kind = OutlineKind::Cell;
			E.Name.assign(Title.substr(First, Last - First));
			E.Line = Line;
			E.Column = static_cast<int32_t>(Hash);
			E.EndLine = Line;
			E.FirstLine = Line;
			E.IndentCols = Indent;
			CurrentCell = static_cast<int32_t>(Out.size());
			Out.push_back(std::move(E));
		}

		void CloseCell(LineNo LastLine)
		{
			if (CurrentCell >= 0)
			{
				OutlineEntry& Cell = Out[static_cast<size_t>(CurrentCell)];
				Cell.EndLine = std::max(Cell.Line, LastLine);
			}
		}

		/** Closes the open entries indented at least Indent deep (all of them for -1). */
		void CloseTo(int32_t Indent)
		{
			while (!Stack.empty() && Stack.back().Indent >= Indent)
			{
				const OpenEntry Open = Stack.back();
				Stack.pop_back();
				OutlineEntry& E = Out[static_cast<size_t>(Open.Entry)];
				LineNo End = std::max(LastSig, E.Line);
				for (auto It = TrailingComments.rbegin(); It != TrailingComments.rend(); ++It)
				{
					if (It->second > Open.Indent)
					{
						End = std::max(End, It->first);
						break;
					}
				}
				E.EndLine = End;
			}
		}

		/** Scans Text from From to the end of the line: strings, comments, brackets, a trailing backslash. */
		void ScanRest(LineNo Line, std::u16string_view Text, size_t From)
		{
			const size_t N = Text.size();
			size_t I = From;
			S.bJoin = false;
			bool bEndsInBackslash = false;
			while (I < N)
			{
				const char16_t C = Text[I];
				if (S.Quote != 0)
				{
					if (C == u'\\')
					{
						if (I + 1 >= N)
						{
							bEndsInBackslash = true;   // the string goes on on the next line
							break;
						}
						I += 2;
						continue;
					}
					if (C == S.Quote && (!S.bTriple || (I + 2 < N && Text[I + 1] == C && Text[I + 2] == C)))
					{
						I += S.bTriple ? 3 : 1;
						S.Quote = 0;
						S.bTriple = false;
						continue;
					}
					++I;
					continue;
				}
				switch (C)
				{
				case u'#':
					I = N;   // a comment runs to the end of the line
					continue;
				case u'\'':
				case u'"':
					S.Quote = C;
					S.bTriple = I + 2 < N && Text[I + 1] == C && Text[I + 2] == C;
					if (S.bTriple)
					{
						TripleOpenLine = Line;
					}
					I += S.bTriple ? 3 : 1;
					continue;
				case u'(':
				case u'[':
				case u'{':
					++S.Depth;
					++I;
					continue;
				case u')':
				case u']':
				case u'}':
					S.Depth = std::max(0, S.Depth - 1);
					++I;
					continue;
				case u'\\':
					if (I + 1 >= N)
					{
						S.bJoin = true;
					}
					++I;
					continue;
				default:
					break;
				}
				// Anything else, string prefixes included: the quote after a prefix opens the string on the next round (the
				// tokenizer also reads a quote right after any name as a string).
				++I;
			}
			if (S.Quote != 0 && !S.bTriple && !bEndsInBackslash)
			{
				S.Quote = 0;   // an unterminated single-quoted string ends with its line (the tokenizer's error)
			}
		}
	};

	std::u32string Fold(std::u16string_view Text)
	{
		std::u32string Out;
		Out.reserve(Text.size());
		for (size_t I = 0; I < Text.size();)
		{
			size_t Units = 1;
			Out.push_back(regex_detail::FoldCp(utf::DecodeAt(Text, I, Units)));
			I += Units;
		}
		return Out;
	}
}

namespace dream
{
	std::vector<OutlineEntry> BuildOutline(const TextSnapshot& Snapshot)
	{
		outline_detail::Scanner Scan;
		std::u16string Scratch;
		Scan.Run(Snapshot.LineCount(), [&Snapshot, &Scratch](LineNo Line) { return Snapshot.LineView(Line, Scratch); });
		return std::move(Scan.Out);
	}

	std::vector<OutlineEntry> BuildOutline(std::u16string_view Text)
	{
		std::vector<size_t> Starts;
		Starts.push_back(0);
		for (size_t I = 0; I < Text.size(); ++I)
		{
			if (Text[I] == u'\n')
			{
				Starts.push_back(I + 1);
			}
		}
		outline_detail::Scanner Scan;
		const LineNo Count = static_cast<LineNo>(Starts.size());
		Scan.Run(Count, [&Text, &Starts, Count](LineNo Line)
		{
			const size_t Begin = Starts[static_cast<size_t>(Line)];
			const size_t End = Line + 1 < Count ? Starts[static_cast<size_t>(Line) + 1] - 1 : Text.size();
			return Text.substr(Begin, End - Begin);
		});
		return std::move(Scan.Out);
	}

	int32_t FindOutlineEntryAt(const std::vector<OutlineEntry>& Entries, LineNo Line)
	{
		int32_t Best = -1;
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			const OutlineEntry& E = Entries[I];
			if (E.FirstLine > Line)
			{
				break;   // Line strictly increases, and an entry's FirstLine is after the previous entry's Line
			}
			if (Line > E.EndLine)
			{
				continue;
			}
			if (Best < 0 || E.Depth >= Entries[static_cast<size_t>(Best)].Depth)
			{
				Best = static_cast<int32_t>(I);
			}
		}
		return Best;
	}

	std::vector<OutlineMatch> FilterOutline(const std::vector<OutlineEntry>& Entries, std::u16string_view Query,
		std::u16string_view UntitledCellName)
	{
		std::vector<OutlineMatch> Result(Entries.size(), Query.empty() ? OutlineMatch::Match : OutlineMatch::Hidden);
		if (Query.empty())
		{
			return Result;
		}
		const std::u32string Needle = outline_detail::Fold(Query);
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			const OutlineEntry& E = Entries[I];
			const std::u16string_view Shown = (E.IsCell() && E.Name.empty()) ? UntitledCellName : std::u16string_view(E.Name);
			const std::u32string Name = outline_detail::Fold(Shown);
			if (Name.find(Needle) == std::u32string::npos)
			{
				continue;
			}
			Result[I] = OutlineMatch::Match;
			// Parents come before their children, so a parent's own result is final here.
			for (int32_t Parent = Entries[I].Parent; Parent >= 0; Parent = Entries[static_cast<size_t>(Parent)].Parent)
			{
				if (Result[static_cast<size_t>(Parent)] != OutlineMatch::Hidden)
				{
					break;
				}
				Result[static_cast<size_t>(Parent)] = OutlineMatch::Ancestor;
			}
		}
		return Result;
	}
}
