// SPDX-License-Identifier: Apache-2.0
// Python highlighting for plain mode (TDD §4.15 row 3): one FSlateTextRun per lexer token (§4.3.3 packing: a token runs from
// its start column to the next token's start, the last to the line end).
#include "CodeView/DreamPythonMarshaller.h"

#include "Buffers/DreamBuffer.h"
#include "Framework/Text/IRun.h"
#include "Framework/Text/SlateTextRun.h"
#include "Framework/Text/TextLayout.h"
#include "Module/DreamText.h"
#include "Shell/DreamEditorStyle.h"

#include <vector>

namespace DreamPythonMarshallerLocal
{
	/** The model text equals the widget's text, compared chunk by chunk without a whole-text copy (SHL-13). */
	bool SameText(const dream::TextSnapshot& Snapshot, const FString& Source)
	{
		static_assert(sizeof(TCHAR) == sizeof(char16_t), "the model and FString share UTF-16 units on this platform");
		if (Snapshot.Length() != static_cast<int64>(Source.Len()))
		{
			return false;
		}
		const TCHAR* Data = *Source;
		int64 At = 0;
		while (At < Snapshot.Length())
		{
			const std::u16string_view Chunk = Snapshot.Chunk(At, 1 << 16);
			if (Chunk.empty() || FMemory::Memcmp(Chunk.data(), Data + At, Chunk.size() * sizeof(char16_t)) != 0)
			{
				return false;
			}
			At += static_cast<int64>(Chunk.size());
		}
		return true;
	}
}

TSharedRef<FDreamPythonMarshaller> FDreamPythonMarshaller::Create(const TSharedPtr<FDreamBuffer>& InBuffer, int32 InFontSize)
{
	return MakeShareable(new FDreamPythonMarshaller(InBuffer, InFontSize));
}

FDreamPythonMarshaller::FDreamPythonMarshaller(const TSharedPtr<FDreamBuffer>& InBuffer, int32 InFontSize)
	: Buffer(InBuffer)
	, FontSize(InFontSize)
{
	RebuildStyles();
}

void FDreamPythonMarshaller::SetFontSize(int32 InFontSize)
{
	if (FontSize != InFontSize)
	{
		FontSize = InFontSize;
		RebuildStyles();
		MakeDirty();
	}
}

void FDreamPythonMarshaller::RefreshStyles()
{
	// (theme) the colours come from FDreamEditorStyle::GetTokenColor, i.e. the current theme
	RebuildStyles();
	MakeDirty();
}

void FDreamPythonMarshaller::RebuildStyles()
{
	const FTextBlockStyle& Base = FDreamEditorStyle::GetCodeTextStyle();
	Styles.Reset(static_cast<int32>(dream::TokenKind::Count));
	for (int32 Kind = 0; Kind < static_cast<int32>(dream::TokenKind::Count); ++Kind)
	{
		const dream::TokenKind TokenKind = static_cast<dream::TokenKind>(Kind);
		Styles.Add(FTextBlockStyle(Base)
			.SetFont(FDreamEditorStyle::GetTokenFont(TokenKind, FontSize))
			.SetColorAndOpacity(FDreamEditorStyle::GetTokenColor(TokenKind)));
	}
}

void FDreamPythonMarshaller::SetText(const FString& SourceString, FTextLayout& TargetTextLayout)
{
	Layout = &TargetTextLayout;

	// Lines exactly as the model splits them: '\n' only (the model is LF-only; a stray CR is shown, never a break).
	// (FString::ParseIntoArray drops a trailing empty line, so the split is done here: N '\n' give N + 1 lines.)
	TArray<FString> Lines;
	{
		int32 LineStart = 0;
		const int32 Len = SourceString.Len();
		for (int32 Index = 0; Index <= Len; ++Index)
		{
			if (Index == Len || SourceString[Index] == TEXT('\n'))
			{
				Lines.Add(SourceString.Mid(LineStart, Index - LineStart));
				LineStart = Index + 1;
			}
		}
	}

	// The buffer's incremental lexer when the source is the model text (the usual case); a full lex of the source otherwise.
	TSharedPtr<FDreamBuffer> Pinned = Buffer.Pin();
	std::vector<std::vector<uint32_t>> OwnTokens;
	bool bUseBufferLexer = false;
	if (Pinned.IsValid() && Pinned->GetModel().Length() == SourceString.Len())
	{
		const dream::TextSnapshot Snapshot = Pinned->Snapshot();
		if (Snapshot.LineCount() == Lines.Num() && DreamPythonMarshallerLocal::SameText(Snapshot, SourceString))
		{
			Pinned->GetLexer().RelexThrough(Snapshot, Snapshot.LineCount() - 1);
			bUseBufferLexer = true;
		}
	}
	if (!bUseBufferLexer)
	{
		std::vector<dream::LineFacts> Facts;
		dream::LexStateId EndState = dream::NormalState;
		dream::LexStateInfo EndInfo;
		dream::PyLexer::LexText(DreamText::ToU16(SourceString), dream::LexGrammar::Py311, OwnTokens, Facts, EndState, EndInfo);
	}

	TArray<FTextLayout::FNewLineData> NewLines;
	NewLines.Reserve(Lines.Num());
	for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
	{
		TSharedRef<FString> LineText = MakeShared<FString>(MoveTemp(Lines[LineIndex]));
		const int32 LineLength = LineText->Len();

		std::span<const uint32_t> Tokens;
		if (bUseBufferLexer)
		{
			Tokens = Pinned->GetLexer().Tokens(LineIndex);
		}
		else if (LineIndex < static_cast<int32>(OwnTokens.size()))
		{
			Tokens = std::span<const uint32_t>(OwnTokens[LineIndex]);
		}

		TArray<TSharedRef<IRun>> Runs;
		int32 Covered = 0;
		for (size_t TokenIndex = 0; TokenIndex < Tokens.size(); ++TokenIndex)
		{
			const int32 Start = FMath::Clamp(dream::TokenStart(Tokens[TokenIndex]), Covered, LineLength);
			const int32 End = TokenIndex + 1 < Tokens.size()
				? FMath::Clamp(dream::TokenStart(Tokens[TokenIndex + 1]), Start, LineLength)
				: LineLength;
			if (End <= Start)
			{
				continue;
			}
			const int32 Kind = FMath::Clamp(static_cast<int32>(dream::TokenKindOf(Tokens[TokenIndex])), 0, Styles.Num() - 1);
			if (Start > Covered)
			{
				Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, Styles[0], FTextRange(Covered, Start)));
			}
			Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, Styles[Kind], FTextRange(Start, End)));
			Covered = End;
		}
		if (Covered < LineLength || Runs.Num() == 0)
		{
			Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, Styles[0], FTextRange(Covered, LineLength)));
		}
		NewLines.Emplace(MoveTemp(LineText), MoveTemp(Runs));
	}
	TargetTextLayout.AddLines(NewLines);
}

void FDreamPythonMarshaller::GetText(FString& TargetString, const FTextLayout& SourceTextLayout)
{
	const TArray<FTextLayout::FLineModel>& LineModels = SourceTextLayout.GetLineModels();
	int32 Length = 0;
	for (const FTextLayout::FLineModel& Line : LineModels)
	{
		Length += Line.Text->Len() + 1;
	}
	TargetString.Reset(Length);
	for (int32 Index = 0; Index < LineModels.Num(); ++Index)
	{
		if (Index > 0)
		{
			TargetString.AppendChar(TEXT('\n'));
		}
		TargetString.Append(*LineModels[Index].Text);
	}
}
