// SPDX-License-Identifier: Apache-2.0
// Dream core: output ingest helpers (TDD §6.1 line splitting, §6.2 ScanLinks, RUN-06 pseudo filenames).
#include "dream/OutputLines.h"

#include <algorithm>

namespace dream::outputlines_detail
{
	/** RUN-06 pseudo filenames start with this ("<dream:Tab Name#id>"). */
	constexpr std::string_view PseudoPrefix = "<dream:";

	inline bool IsUtf8Cont(char C)
	{
		return (static_cast<unsigned char>(C) & 0xC0u) == 0x80u;
	}

	inline bool IsDigit(char C) { return C >= '0' && C <= '9'; }
	inline bool IsAlpha(char C) { return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z'); }
	inline bool IsAlnum(char C) { return IsDigit(C) || IsAlpha(C); }
	inline bool IsHex(char C) { return IsDigit(C) || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F'); }
	inline bool IsSep(char C) { return C == '/' || C == '\\'; }

	/** Characters that end a path scanned backwards from "(N)" (whitespace too, handled by the caller). */
	inline bool IsPathDelimiter(char C)
	{
		switch (C)
		{
		case ' ': case '\t': case '"': case '\'': case '<': case '>': case '|': case '(': case ')': case '[': case ']':
		case ',': case ';': case '=': case '*': case '?': case '{': case '}': case '`':
			return true;
		default:
			return static_cast<unsigned char>(C) < 0x20;
		}
	}

	/** Parses up to 9 digits at Pos into Out (>= 1); advances Pos. */
	bool ParseLineNumber(std::string_view S, size_t& Pos, int32_t& Out)
	{
		const size_t Begin = Pos;
		int32_t V = 0;
		while (Pos < S.size() && IsDigit(S[Pos]) && Pos - Begin < 9)
		{
			V = V * 10 + (S[Pos] - '0');
			++Pos;
		}
		if (Pos == Begin || (Pos < S.size() && IsDigit(S[Pos])) || V < 1)
		{
			return false;
		}
		Out = V;
		return true;
	}

	bool HasDrivePrefix(std::string_view P)
	{
		return P.size() >= 3 && IsAlpha(P[0]) && P[1] == ':' && IsSep(P[2]);
	}

	/** A file path ending in an extension (1-10 alphanumerics after the last '.', after the last separator). */
	bool LooksLikeFilePath(std::string_view P)
	{
		const size_t Dot = P.find_last_of('.');
		const size_t Sep = P.find_last_of("/\\");
		if (Dot == std::string_view::npos || Dot == 0 || Dot + 1 >= P.size() || (Sep != std::string_view::npos && Dot < Sep))
		{
			return false;
		}
		if (IsSep(P[Dot - 1]) || P.size() - Dot - 1 > 10)
		{
			return false;
		}
		bool bLetter = false;   // "3/4.5(2)" is arithmetic, not a file
		for (size_t I = Dot + 1; I < P.size(); ++I)
		{
			if (!IsAlnum(P[I]))
			{
				return false;
			}
			bLetter = bLetter || IsAlpha(P[I]);
		}
		if (!bLetter)
		{
			return false;
		}
		return Sep != std::string_view::npos || HasDrivePrefix(P);
	}

	bool Overlaps(const std::vector<LinkSpan>& Spans, size_t From, uint32_t Start, uint32_t End)
	{
		for (size_t I = From; I < Spans.size(); ++I)
		{
			if (Start < Spans[I].Start + Spans[I].Len && Spans[I].Start < End)
			{
				return true;
			}
		}
		return false;
	}

	void ScanPythonFrames(std::string_view S, std::vector<LinkSpan>& Out)
	{
		size_t From = 0;
		for (;;)
		{
			const size_t P = S.find("File \"", From);
			if (P == std::string_view::npos)
			{
				return;
			}
			From = P + 1;
			if (P > 0 && (IsAlnum(S[P - 1]) || S[P - 1] == '_'))
			{
				continue;
			}
			const size_t PathStart = P + 6;
			const size_t Q = S.find('"', PathStart);
			if (Q == std::string_view::npos || Q == PathStart)
			{
				continue;
			}
			const std::string_view Path = S.substr(PathStart, Q - PathStart);
			if (S.substr(Q, 8) != "\", line ")
			{
				continue;
			}
			size_t Pos = Q + 8;
			int32_t Line = 0;
			if (!ParseLineNumber(S, Pos, Line))
			{
				continue;
			}
			LinkKind Kind = LinkKind::PythonFrame;
			if (Path.front() == '<')
			{
				if (Path.substr(0, PseudoPrefix.size()) != PseudoPrefix || Path.back() != '>')
				{
					continue;   // <string>, <stdin>, <frozen ...>: no file to open
				}
				Kind = LinkKind::PseudoFile;
			}
			LinkSpan Span;
			Span.Start = static_cast<uint32_t>(P);
			Span.Len = static_cast<uint32_t>(Pos - P);
			Span.Kind = Kind;
			Span.Line = Line;
			Span.Col = 0;
			Span.PathStart = static_cast<uint32_t>(PathStart);
			Span.PathLen = static_cast<uint32_t>(Q - PathStart);
			Out.push_back(Span);
			From = Pos;
		}
	}

	void ScanFileLines(std::string_view S, std::vector<LinkSpan>& Out)
	{
		for (size_t P = S.find('('); P != std::string_view::npos; P = S.find('(', P + 1))
		{
			size_t Pos = P + 1;
			int32_t Line = 0;
			int32_t Col = 0;
			if (!ParseLineNumber(S, Pos, Line))
			{
				continue;
			}
			if (Pos < S.size() && S[Pos] == ',')
			{
				++Pos;
				if (!ParseLineNumber(S, Pos, Col))
				{
					continue;
				}
			}
			if (Pos >= S.size() || S[Pos] != ')')
			{
				continue;
			}
			const size_t End = Pos + 1;
			size_t B = P;
			while (B > 0 && !IsPathDelimiter(S[B - 1]))
			{
				--B;
			}
			if (B == P)
			{
				continue;
			}
			std::string_view Path = S.substr(B, P - B);
			if (!(HasDrivePrefix(Path) || Path.front() == '/' || Path.front() == '\\'))
			{
				// A path with spaces ("C:\Program Files\x.py(3)"): extend back to a drive prefix over spaces only.
				size_t D = B;
				bool bFound = false;
				while (D > 0)
				{
					--D;
					const char C = S[D];
					if (C != ' ' && IsPathDelimiter(C))
					{
						break;
					}
					if (D + 2 < P && IsAlpha(S[D]) && S[D + 1] == ':' && IsSep(S[D + 2]) && (D == 0 || IsPathDelimiter(S[D - 1])))
					{
						bFound = true;
						break;
					}
				}
				if (bFound)
				{
					B = D;
					Path = S.substr(B, P - B);
				}
			}
			if (!LooksLikeFilePath(Path))
			{
				continue;
			}
			if (Overlaps(Out, 0, static_cast<uint32_t>(B), static_cast<uint32_t>(End)))
			{
				continue;
			}
			LinkSpan Span;
			Span.Start = static_cast<uint32_t>(B);
			Span.Len = static_cast<uint32_t>(End - B);
			Span.Kind = LinkKind::FileLine;
			Span.Line = Line;
			Span.Col = Col;
			Span.PathStart = static_cast<uint32_t>(B);
			Span.PathLen = static_cast<uint32_t>(P - B);
			Out.push_back(Span);
		}
	}
}

namespace dream
{
	void LineSplitter::Commit(std::vector<SplitLine>& Out, uint16_t Flags)
	{
		SplitLine Line;
		Line.Text = std::move(Partial);
		Line.Flags = static_cast<uint16_t>(Flags | (bContinuing ? OLF_Continued : 0));
		Out.push_back(std::move(Line));
		Partial.clear();
	}

	void LineSplitter::Append(std::string_view Text, std::vector<SplitLine>& Out)
	{
		while (!Text.empty())
		{
			const size_t Room = ContinuationBytes - Partial.size();
			if (Text.size() <= Room)
			{
				Partial.append(Text);
				return;
			}
			size_t Take = Room;
			while (Take > 0 && outputlines_detail::IsUtf8Cont(Text[Take]))
			{
				--Take;   // never split a UTF-8 sequence
			}
			if (Take == 0 && Partial.empty())
			{
				Take = Room;
			}
			Partial.append(Text.substr(0, Take));
			Text.remove_prefix(Take);
			Commit(Out, 0);
			bContinuing = true;
		}
	}

	void LineSplitter::Feed(std::string_view Utf8, std::vector<SplitLine>& Out)
	{
		auto EndLine = [this, &Out]() {
			if (!(Partial.empty() && bContinuing))
			{
				Commit(Out, 0);
			}
			bContinuing = false;
		};
		size_t I = 0;
		if (bHeldCr && !Utf8.empty())
		{
			bHeldCr = false;
			if (Utf8[0] == '\n')
			{
				EndLine();   // "x\r" then "\n" is one line end (print('x\r'))
				I = 1;
			}
			else
			{
				Partial.clear();   // a carriage return: the open text is redrawn
				bContinuing = false;
			}
		}
		while (I < Utf8.size())
		{
			const size_t J = Utf8.find_first_of("\r\n", I);
			if (J == std::string_view::npos)
			{
				Append(Utf8.substr(I), Out);
				break;
			}
			Append(Utf8.substr(I, J - I), Out);
			if (Utf8[J] == '\n')
			{
				EndLine();
				I = J + 1;
				continue;
			}
			if (J + 1 >= Utf8.size())
			{
				bHeldCr = true;   // decided by the next write
				I = J + 1;
				continue;
			}
			if (Utf8[J + 1] == '\n')
			{
				EndLine();
				I = J + 2;
			}
			else
			{
				Partial.clear();
				bContinuing = false;
				I = J + 1;
			}
		}
		if (Partial.size() >= PartialCommitBytes)
		{
			if (bHeldCr)
			{
				bHeldCr = false;
				EndLine();
			}
			else
			{
				Commit(Out, OLF_Partial);   // bounds the per-key memory (§6.1)
				bContinuing = true;
			}
		}
	}

	void LineSplitter::Flush(std::vector<SplitLine>& Out, bool bBeforeEcho)
	{
		if (bHeldCr)
		{
			bHeldCr = false;
			if (!(Partial.empty() && bContinuing))
			{
				Commit(Out, 0);   // a held CR ends the line
			}
			bContinuing = false;
			return;
		}
		if (!Partial.empty())
		{
			Commit(Out, static_cast<uint16_t>(OLF_Partial | (bBeforeEcho ? OLF_EchoSplit : 0)));
		}
		bContinuing = false;
	}

	bool LineSplitter::HasPartial() const
	{
		return !Partial.empty() || bHeldCr;
	}

	std::string_view LineSplitter::PartialText() const
	{
		return Partial;
	}

	bool ScanLinks(std::string_view Utf8Line, std::vector<LinkSpan>& Out)
	{
		const size_t Before = Out.size();
		std::vector<LinkSpan> Found;
		outputlines_detail::ScanPythonFrames(Utf8Line, Found);
		outputlines_detail::ScanFileLines(Utf8Line, Found);
		std::sort(Found.begin(), Found.end(), [](const LinkSpan& A, const LinkSpan& B) { return A.Start < B.Start; });
		Out.insert(Out.end(), Found.begin(), Found.end());
		return Out.size() > Before;
	}

	bool ParsePseudoFilename(std::string_view Filename, std::string_view& OutName, std::string_view& OutId)
	{
		if (Filename.size() < outputlines_detail::PseudoPrefix.size() + 3 || Filename.substr(0, outputlines_detail::PseudoPrefix.size()) != outputlines_detail::PseudoPrefix || Filename.back() != '>')
		{
			return false;
		}
		const std::string_view Inner = Filename.substr(outputlines_detail::PseudoPrefix.size(), Filename.size() - outputlines_detail::PseudoPrefix.size() - 1);
		const size_t Hash = Inner.rfind('#');
		if (Hash == std::string_view::npos || Hash == 0 || Hash + 1 >= Inner.size())
		{
			return false;
		}
		const std::string_view Id = Inner.substr(Hash + 1);
		for (char C : Id)
		{
			if (!outputlines_detail::IsHex(C))
			{
				return false;
			}
		}
		OutName = Inner.substr(0, Hash);
		OutId = Id;
		return true;
	}
}
