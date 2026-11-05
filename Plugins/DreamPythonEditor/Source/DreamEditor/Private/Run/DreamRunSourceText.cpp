// SPDX-License-Identifier: Apache-2.0
// Source-text helpers for the Location row and the §5.7.4 reports (supervisor slice). The handler search is a lexical
// reading of the run's source snapshot by indentation, which is what Python's block structure is; continuation lines
// inside brackets can mislead it, and then the report names the nearest header it found (a wording risk only).
#include "Run/DreamRunSourceText.h"

#include "Module/DreamText.h"
#include "dream/StatementExtent.h"   // (run) SelectionLines

namespace DreamRunSourceTextPrivate
{
	/** Python's indentation width of a line: tabs advance to the next multiple of 8 (tokenizer rule). */
	int32 IndentOf(const FString& Text)
	{
		int32 Column = 0;
		for (const TCHAR Ch : Text)
		{
			if (Ch == TEXT(' '))
			{
				++Column;
			}
			else if (Ch == TEXT('\t'))
			{
				Column = (Column / 8 + 1) * 8;
			}
			else if (Ch == TEXT('\f'))
			{
				Column = 0;
			}
			else
			{
				break;
			}
		}
		return Column;
	}

	bool IsBlankOrComment(const FString& Text)
	{
		for (const TCHAR Ch : Text)
		{
			if (Ch == TEXT('#'))
			{
				return true;
			}
			if (!FChar::IsWhitespace(Ch))
			{
				return false;
			}
		}
		return true;
	}

	/** True when Text (leading whitespace already removed) starts with the keyword Word as a whole token. */
	bool StartsWithKeyword(const FString& Text, const TCHAR* Word)
	{
		const int32 Len = FCString::Strlen(Word);
		if (!Text.StartsWith(Word, ESearchCase::CaseSensitive))
		{
			return false;
		}
		if (Text.Len() == Len)
		{
			return true;
		}
		const TCHAR Next = Text[Len];
		return !(FChar::IsAlnum(Next) || Next == TEXT('_'));
	}

	bool IsTryHeader(const FString& Trimmed)
	{
		return StartsWithKeyword(Trimmed, TEXT("try"));
	}

	/** Name occurs in Text as a whole identifier: no identifier character before or after it ("unreal.X" is fine). */
	bool ContainsIdentifier(const FString& Text, const TCHAR* Name)
	{
		const int32 Len = FCString::Strlen(Name);
		auto IsIdentChar = [](TCHAR Ch) { return FChar::IsAlnum(Ch) || Ch == TEXT('_'); };
		for (int32 At = Text.Find(Name, ESearchCase::CaseSensitive); At != INDEX_NONE;
			At = Text.Find(Name, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
		{
			const bool bStartOk = At == 0 || !IsIdentChar(Text[At - 1]);
			const bool bEndOk = At + Len >= Text.Len() || !IsIdentChar(Text[At + Len]);
			if (bStartOk && bEndOk)
			{
				return true;
			}
		}
		return false;
	}

	/** The clause's names: Rest up to the ':' that ends the header (outside brackets) or a comment, so neither a trailing
	 *  comment nor a one-line body takes part in the match (P3-run-05). */
	FString ClauseNames(const FString& Rest)
	{
		int32 Depth = 0;
		for (int32 At = 0; At < Rest.Len(); ++At)
		{
			const TCHAR Ch = Rest[At];
			if (Ch == TEXT('(') || Ch == TEXT('[') || Ch == TEXT('{'))
			{
				++Depth;
			}
			else if (Ch == TEXT(')') || Ch == TEXT(']') || Ch == TEXT('}'))
			{
				Depth = FMath::Max(0, Depth - 1);
			}
			else if (Ch == TEXT('#') || (Ch == TEXT(':') && Depth == 0))
			{
				return Rest.Left(At);
			}
		}
		return Rest;
	}

	/** An except clause that can catch ScriptCancelled(KeyboardInterrupt): bare, or naming one of its bases. */
	bool CanCatchCancel(const FString& Trimmed)
	{
		FString Rest = Trimmed.Mid(6).TrimStart();     // after "except"
		if (Rest.StartsWith(TEXT("*")))
		{
			// except* (3.11) also catches a naked exception, wrapped in a group, so "except* KeyboardInterrupt:" catches Stop.
			// A bare "except*:" is a SyntaxError, and an except* naming an exception group type raises TypeError when it is
			// tested (PEP 654): neither catches anything.
			Rest = Rest.Mid(1).TrimStart();
			if (Rest.StartsWith(TEXT(":")))
			{
				return false;
			}
			Rest = ClauseNames(Rest);
			if (ContainsIdentifier(Rest, TEXT("BaseExceptionGroup")) || ContainsIdentifier(Rest, TEXT("ExceptionGroup")))
			{
				return false;
			}
		}
		else if (Rest.StartsWith(TEXT(":")))
		{
			return true;
		}
		else
		{
			Rest = ClauseNames(Rest);
		}
		// Whole names: "except BaseExceptionGroup:" lets a naked KeyboardInterrupt through.
		return ContainsIdentifier(Rest, TEXT("BaseException"))
			|| ContainsIdentifier(Rest, TEXT("KeyboardInterrupt"))
			|| ContainsIdentifier(Rest, TEXT("ScriptCancelled"));
	}
}

bool DreamRunSource::HasSource(const dream::TextSnapshot& Snapshot)
{
	return Snapshot.Version() != 0;
}

FString DreamRunSource::GetLine(const dream::TextSnapshot& Snapshot, int32 Line)
{
	if (!HasSource(Snapshot) || Line < 1 || Line > Snapshot.LineCount())
	{
		return FString();
	}
	return DreamText::ToFString(Snapshot.LineText(Line - 1));
}

FString DreamRunSource::ForLocation(const FString& LineText)
{
	constexpr int32 MaxChars = 80;
	FString Text = LineText.TrimStartAndEnd();
	if (Text.Len() > MaxChars)
	{
		Text.LeftInline(MaxChars);
		Text.AppendChar(TEXT('…'));
	}
	return Text;
}

DreamRunSource::EHandlerKind DreamRunSource::ClassifyHeader(const FString& LineText)
{
	using namespace DreamRunSourceTextPrivate;
	const FString Trimmed = LineText.TrimStart();
	if (StartsWithKeyword(Trimmed, TEXT("except")))
	{
		return EHandlerKind::Except;
	}
	if (StartsWithKeyword(Trimmed, TEXT("finally")))
	{
		return EHandlerKind::Finally;
	}
	if (StartsWithKeyword(Trimmed, TEXT("with")))
	{
		return EHandlerKind::With;
	}
	if (StartsWithKeyword(Trimmed, TEXT("async")) && StartsWithKeyword(Trimmed.Mid(5).TrimStart(), TEXT("with")))
	{
		return EHandlerKind::With;
	}
	return EHandlerKind::None;
}

int32 DreamRunSource::FindEnclosingHandler(const dream::TextSnapshot& Snapshot, int32 Line, EHandlerKind& OutKind)
{
	using namespace DreamRunSourceTextPrivate;
	OutKind = EHandlerKind::None;
	if (!HasSource(Snapshot) || Line < 1 || Line > Snapshot.LineCount())
	{
		return 0;
	}

	const FString Text = GetLine(Snapshot, Line);
	const EHandlerKind Own = ClassifyHeader(Text);
	if (Own != EHandlerKind::None)
	{
		OutKind = Own;
		return Line;
	}

	int32 Indent = IsBlankOrComment(Text) ? MAX_int32 : IndentOf(Text);
	for (int32 L = Line - 1; L >= 1 && Indent > 0; --L)
	{
		const FString Header = GetLine(Snapshot, L);
		if (IsBlankOrComment(Header))
		{
			continue;
		}
		const int32 HeaderIndent = IndentOf(Header);
		if (HeaderIndent >= Indent)
		{
			continue;
		}
		const EHandlerKind Kind = ClassifyHeader(Header);
		if (Kind != EHandlerKind::None)
		{
			OutKind = Kind;
			return L;
		}
		Indent = HeaderIndent;
	}
	return 0;
}

int32 DreamRunSource::FindCatchingExcept(const dream::TextSnapshot& Snapshot, int32 Line)
{
	using namespace DreamRunSourceTextPrivate;
	if (!HasSource(Snapshot) || Line < 1 || Line > Snapshot.LineCount())
	{
		return 0;
	}

	const int32 LineCount = Snapshot.LineCount();
	const FString Text = GetLine(Snapshot, Line);
	int32 Indent = IsBlankOrComment(Text) ? MAX_int32 : IndentOf(Text);

	// Walk outward through the enclosing blocks; each enclosing try is a candidate, innermost first.
	for (int32 L = Line - 1; L >= 1 && Indent > 0; --L)
	{
		const FString Header = GetLine(Snapshot, L);
		if (IsBlankOrComment(Header))
		{
			continue;
		}
		const int32 HeaderIndent = IndentOf(Header);
		if (HeaderIndent >= Indent)
		{
			continue;
		}
		Indent = HeaderIndent;
		if (!IsTryHeader(Header.TrimStart()))
		{
			continue;
		}

		// The try's clauses sit at its indentation after Line; the first one that can catch ScriptCancelled wins.
		for (int32 C = Line + 1; C <= LineCount; ++C)
		{
			const FString Clause = GetLine(Snapshot, C);
			if (IsBlankOrComment(Clause))
			{
				continue;
			}
			const int32 ClauseIndent = IndentOf(Clause);
			if (ClauseIndent < HeaderIndent)
			{
				break;
			}
			if (ClauseIndent > HeaderIndent)
			{
				continue;
			}
			const FString Trimmed = Clause.TrimStart();
			if (StartsWithKeyword(Trimmed, TEXT("except")))
			{
				if (CanCatchCancel(Trimmed))
				{
					return C;
				}
				continue;
			}
			break;                                      // else, finally or the next statement: this try did not catch it
		}
	}
	return 0;
}

// ---- (run, PO-06 F2) exact selection runs ---------------------------------------------------------------------------

DreamRunSource::FSelectionText DreamRunSource::ExactSelection(const dream::TextSnapshot& Snapshot, int64 SelStart, int64 SelEnd)
{
	const dream::Offset Length = Snapshot.Length();
	dream::Offset Lo = FMath::Clamp<dream::Offset>(FMath::Min(SelStart, SelEnd), 0, Length);
	const dream::Offset Hi = FMath::Clamp<dream::Offset>(FMath::Max(SelStart, SelEnd), 0, Length);
	dream::LineRange Lines = dream::SelectionLines(Snapshot, Lo, Hi);
	if (Lines.Last > Lines.First && Lo == Snapshot.LineEnd(Lines.First))
	{
		// Only the line break of the first line is selected there: the run starts at the next line.
		++Lines.First;
		Lo = Snapshot.LineStart(Lines.First);
	}

	const dream::Offset FirstLineStart = Snapshot.LineStart(Lines.First);
	dream::Offset TextBegin = Lo;
	if (Lo > FirstLineStart)
	{
		std::u16string Before;
		Snapshot.CopyRange(FirstLineStart, Lo - FirstLineStart, Before);
		bool bBlanksOnly = true;
		for (const char16_t Ch : Before)
		{
			if (Ch != u' ' && Ch != u'\t' && Ch != u'\f')
			{
				bBlanksOnly = false;
				break;
			}
		}
		if (bBlanksOnly)
		{
			TextBegin = FirstLineStart;   // an indented block keeps its indentation; the RT-14 wrapper removes it
		}
	}
	const dream::Offset LastLineEnd = Snapshot.LineEnd(Lines.Last);
	const dream::Offset TextEnd = FMath::Max(TextBegin, FMath::Min(Hi, LastLineEnd));

	FSelectionText Out;
	std::u16string Text;
	Snapshot.CopyRange(TextBegin, TextEnd - TextBegin, Text);
	Out.Text = DreamText::ToFString(Text);
	Out.FirstLine = Lines.First + 1;
	Out.LastLine = Lines.Last + 1;
	if (TextBegin > FirstLineStart)
	{
		Out.StartColumn = static_cast<int32>(TextBegin - FirstLineStart);
	}
	if (TextEnd < LastLineEnd)
	{
		Out.EndColumn = static_cast<int32>(TextEnd - Snapshot.LineStart(Lines.Last));
	}
	return Out;
}

FStringView DreamRunSource::CutRunLine(FStringView LineText, int32 Line, int32 FirstLine, int32 LastLine,
	const TOptional<int32>& StartColumn, const TOptional<int32>& EndColumn)
{
	FStringView Out = LineText;
	if (Line == LastLine && EndColumn.IsSet())
	{
		Out = Out.Left(FMath::Max(0, EndColumn.GetValue()));   // the end column is a column of the whole line
	}
	if (Line == FirstLine && StartColumn.IsSet())
	{
		Out = Out.RightChop(FMath::Max(0, StartColumn.GetValue()));
	}
	return Out;
}
