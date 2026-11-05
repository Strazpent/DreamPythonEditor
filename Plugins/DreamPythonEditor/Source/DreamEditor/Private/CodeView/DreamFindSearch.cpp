// SPDX-License-Identifier: Apache-2.0
// The code view's search on dream::SearchEngine (TDD §4.4, EDT-09 M1): Python `re` syntax for regexes, match case, whole
// word, the 100,000-match cap and the per-line complexity limit (§4.4 limits).
#include "CodeView/DreamFindSearch.h"

#include "Module/DreamText.h"
#include "dream/CancelToken.h"
#include "dream/SearchEngine.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamFindSearchLocal
{
	dream::SearchQuery ToCore(const FDreamFindQuery& Query)
	{
		dream::SearchQuery Out;
		Out.Pattern = std::u16string(DreamText::ToU16(Query.Text));
		Out.bRegex = Query.bRegex;
		Out.bMatchCase = Query.bMatchCase;
		Out.bWholeWord = Query.bWholeWord;
		return Out;
	}

	/** The error under the find box (§4.4 "Unsupported Python constructs show an error under the find box"). */
	FText ErrorText(const dream::SearchError& Error)
	{
		const FText Detail = FText::FromString(DreamText::ToFString(Error.Detail));
		const FText At = Error.Position >= 0
			? FText::Format(LOCTEXT("CodeView.FindErrorAt", " (at {0})"), FText::AsNumber(Error.Position + 1, &FNumberFormattingOptions::DefaultNoGrouping()))
			: FText::GetEmpty();
		switch (Error.Code)
		{
		case dream::SearchErrorCode::Syntax:
			return FText::Format(LOCTEXT("CodeView.FindSyntax", "Invalid pattern: {0}{1}"), Detail, At);
		case dream::SearchErrorCode::Unsupported:
			return FText::Format(LOCTEXT("CodeView.FindUnsupported", "Not supported in Dream's find: {0}{1}"), Detail, At);
		case dream::SearchErrorCode::TooComplex:
			return LOCTEXT("CodeView.FindTooComplex", "Pattern too complex");
		case dream::SearchErrorCode::BadTemplate:
			return FText::Format(LOCTEXT("CodeView.FindBadTemplate", "Invalid replacement: {0}{1}"), Detail, At);
		case dream::SearchErrorCode::EmptyPattern:
		case dream::SearchErrorCode::None:
		default:
			return FText::GetEmpty();
		}
	}

	bool CompileQuery(const FDreamFindQuery& Query, dream::CompileResult& Out, FText& OutError)
	{
		Out = dream::SearchEngine::Compile(ToCore(Query));
		if (!Out.Ok())
		{
			OutError = ErrorText(Out.Error);
			return false;
		}
		return true;
	}
}

FDreamFindResult DreamFindSearch::Search(const dream::TextSnapshot& Snapshot, const FDreamFindQuery& Query)
{
	FDreamFindResult Result;
	if (Query.Text.IsEmpty())
	{
		return Result;
	}
	dream::CompileResult Compiled;
	if (!DreamFindSearchLocal::CompileQuery(Query, Compiled, Result.Error))
	{
		return Result;
	}
	dream::SearchSummary Summary;
	const std::vector<dream::SearchMatch> Matches = dream::SearchEngine::FindAll(Snapshot, *Compiled.Query, &Summary);
	Result.Matches.Reserve(static_cast<int32>(Matches.size()));
	for (const dream::SearchMatch& Match : Matches)
	{
		FDreamFindMatch& Out = Result.Matches.AddDefaulted_GetRef();
		Out.Start = Match.At;
		Out.Length = Match.Len;
	}
	Result.bCapped = Summary.bCapped;
	Result.SkippedLines = Summary.ComplexLines;
	return Result;
}

bool DreamFindSearch::BuildReplaceOne(const dream::TextSnapshot& Snapshot, const FDreamFindQuery& Query, const FDreamFindMatch& Match,
	const FString& Template, dream::EditBatch& OutBatch, FText& OutError)
{
	dream::CompileResult Compiled;
	if (!DreamFindSearchLocal::CompileQuery(Query, Compiled, OutError))
	{
		return false;
	}
	const std::u16string Text = Snapshot.Text();
	std::u16string Replacement;
	dream::SearchError Error;
	if (!dream::SearchEngine::ExpandReplacement(Text, *Compiled.Query, dream::SearchMatch{ Match.Start, Match.Length },
		DreamText::ToU16(Template), /*bPreserveCase*/ false, Replacement, &Error))
	{
		OutError = DreamFindSearchLocal::ErrorText(Error);
		if (OutError.IsEmpty())
		{
			OutError = LOCTEXT("CodeView.ReplaceStale", "The selected text is no longer a match");
		}
		return false;
	}
	OutBatch = dream::EditBatch();
	OutBatch.Origin = dream::EditOrigin::Command;
	dream::EditRange& Range = OutBatch.Ranges.emplace_back();
	Range.At = Match.Start;
	Range.DeleteLen = Match.Length;
	Range.Insert = MoveTemp(Replacement);
	return true;
}

bool DreamFindSearch::BuildReplaceAll(const dream::TextSnapshot& Snapshot, const FDreamFindQuery& Query, const FString& Template,
	dream::EditBatch& OutBatch, int32& OutCount, FText& OutError)
{
	OutCount = 0;
	dream::CompileResult Compiled;
	if (!DreamFindSearchLocal::CompileQuery(Query, Compiled, OutError))
	{
		return false;
	}
	dream::SearchError Error;
	if (!dream::SearchEngine::ValidateTemplate(*Compiled.Query, DreamText::ToU16(Template), &Error))
	{
		OutError = DreamFindSearchLocal::ErrorText(Error);
		return false;
	}
	OutBatch = dream::SearchEngine::BuildReplaceAll(Snapshot, *Compiled.Query, DreamText::ToU16(Template), /*bPreserveCase*/ false,
		0, -1, &Error);
	if (Error.Code != dream::SearchErrorCode::None)
	{
		OutError = DreamFindSearchLocal::ErrorText(Error);
		return false;
	}
	OutCount = static_cast<int32>(OutBatch.Ranges.size());
	return true;
}

bool DreamFindSearch::Compile(const FDreamFindQuery& Query, std::shared_ptr<const dream::CompiledQuery>& OutQuery, FText& OutError)
{
	OutQuery.reset();
	dream::CompileResult Compiled;
	if (Query.Text.IsEmpty() || !DreamFindSearchLocal::CompileQuery(Query, Compiled, OutError))
	{
		return false;
	}
	OutQuery = Compiled.Query;
	return true;
}

bool DreamFindSearch::ValidateTemplate(const dream::CompiledQuery& Query, const FString& Template, FText& OutError)
{
	dream::SearchError Error;
	if (!dream::SearchEngine::ValidateTemplate(Query, DreamText::ToU16(Template), &Error))
	{
		OutError = DreamFindSearchLocal::ErrorText(Error);
		return false;
	}
	return true;
}

FDreamFindResult DreamFindSearch::SearchCompiled(const dream::TextSnapshot& Snapshot, const dream::CompiledQuery& Query,
	const dream::CancelToken* Cancel)
{
	FDreamFindResult Result;
	dream::SearchSummary Summary;
	const std::vector<dream::SearchMatch> Matches = dream::SearchEngine::FindAll(Snapshot, Query, &Summary, Cancel);
	Result.bCancelled = Summary.bCancelled || dream::IsCancelled(Cancel);
	if (Result.bCancelled)
	{
		return Result;
	}
	Result.Matches.Reserve(static_cast<int32>(Matches.size()));
	for (const dream::SearchMatch& Match : Matches)
	{
		FDreamFindMatch& Out = Result.Matches.AddDefaulted_GetRef();
		Out.Start = Match.At;
		Out.Length = Match.Len;
	}
	Result.bCapped = Summary.bCapped;
	Result.SkippedLines = Summary.ComplexLines;
	return Result;
}

FDreamReplaceAllResult DreamFindSearch::BuildReplaceAllCompiled(const dream::TextSnapshot& Snapshot, const dream::CompiledQuery& Query,
	const std::u16string& Template, const dream::CancelToken* Cancel)
{
	// The same set as re.sub: every finditer match, empty ones included (SearchEngine::BuildReplaceAll takes no token).
	FDreamReplaceAllResult Result;
	dream::SearchSummary Summary;
	const std::vector<dream::SearchMatch> Matches = dream::SearchEngine::FindAll(Snapshot, Query, &Summary, Cancel);
	if (Summary.bCancelled || dream::IsCancelled(Cancel))
	{
		Result.bCancelled = true;
		return Result;
	}
	if (Summary.bCapped)
	{
		Result.bCapped = true;
		return Result;
	}
	const std::u16string Text = Snapshot.Text();
	Result.Batch.Origin = dream::EditOrigin::Command;
	Result.Batch.Ranges.reserve(Matches.size());
	for (const dream::SearchMatch& Match : Matches)
	{
		std::u16string Replacement;
		if (!dream::SearchEngine::ExpandReplacement(Text, Query, Match, Template, /*bPreserveCase*/ false, Replacement, nullptr))
		{
			Result.bFailed = true;
			Result.Batch.Ranges.clear();
			return Result;
		}
		if (dream::IsCancelled(Cancel))
		{
			Result.bCancelled = true;
			Result.Batch.Ranges.clear();
			return Result;
		}
		if (Match.Len == 0 && Replacement.empty())
		{
			continue;   // an empty match replaced by nothing changes nothing
		}
		dream::EditRange& Range = Result.Batch.Ranges.emplace_back();
		Range.At = Match.At;
		Range.DeleteLen = Match.Len;
		Range.Insert = std::move(Replacement);
	}
	Result.Count = static_cast<int32>(Matches.size());
	return Result;
}

#undef LOCTEXT_NAMESPACE
