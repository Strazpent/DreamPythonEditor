// SPDX-License-Identifier: Apache-2.0
// The code view's use of dream::SearchEngine (TDD §4.4, EDT-09 M1): one query over a buffer snapshot, the single and the
// Replace All edits. A literal search is linear (about a millisecond for 1 MB, A40) and runs on the game thread; a regex
// search and Replace All's enumeration run on a UE::Tasks worker over the snapshot with a dream::CancelToken (§4.4 Run):
// SearchEngine's limits bound one line, not the whole search (a bad pattern can cost seconds over a few thousand lines).
// Compile, ValidateTemplate and the FText messages are game thread; SearchCompiled and BuildReplaceAllCompiled run on
// any thread. Slice-internal (codeview).
#pragma once

#include "CoreMinimal.h"
#include "dream/TextBuffer.h"

#include <memory>
#include <string>

namespace dream
{
	class CancelToken;
	class CompiledQuery;
}

struct FDreamFindQuery
{
	FString Text;
	bool bMatchCase = false;
	bool bRegex = false;
	bool bWholeWord = false;

	bool operator==(const FDreamFindQuery& Other) const
	{
		return Text.Equals(Other.Text, ESearchCase::CaseSensitive) && bMatchCase == Other.bMatchCase && bRegex == Other.bRegex
			&& bWholeWord == Other.bWholeWord;
	}
};

/** One match in UTF-16 offsets of the snapshot it was found in. */
struct FDreamFindMatch
{
	dream::Offset Start = 0;
	int64 Length = 0;

	dream::Offset End() const { return Start + Length; }
};

struct FDreamFindResult
{
	TArray<FDreamFindMatch> Matches;   // ascending
	bool bCapped = false;             // stopped at the match cap: the count reads "m+"
	int32 SkippedLines = 0;           // "Pattern too complex on n lines" (§4.4 limits)
	bool bCancelled = false;          // the worker's token was set: the result is partial and never shown
	FText Error;                      // the pattern does not compile; Matches is empty (game thread only)
};

/** Replace All built on a worker: one batch for every match, or the reason there is none. */
struct FDreamReplaceAllResult
{
	dream::EditBatch Batch;
	int32 Count = 0;
	bool bCapped = false;             // more than MaxMatches matches: nothing is replaced
	bool bCancelled = false;
	bool bFailed = false;             // a replacement could not be expanded (the template or the match)
};

namespace DreamFindSearch
{
	/** Every match of Query in Snapshot (§4.4), capped at MaxMatches. An empty query finds nothing. */
	DREAMEDITOR_API FDreamFindResult Search(const dream::TextSnapshot& Snapshot, const FDreamFindQuery& Query);

	/** The edit that replaces Match with Template (expanded for a regex: \1, \g<name>, $1, ${name}, $$, \n, \t, \\). */
	DREAMEDITOR_API bool BuildReplaceOne(const dream::TextSnapshot& Snapshot, const FDreamFindQuery& Query, const FDreamFindMatch& Match,
		const FString& Template, dream::EditBatch& OutBatch, FText& OutError);

	/** Replace All as one batch (one undo group, §4.4); OutCount is the number of replaced matches. */
	DREAMEDITOR_API bool BuildReplaceAll(const dream::TextSnapshot& Snapshot, const FDreamFindQuery& Query, const FString& Template,
		dream::EditBatch& OutBatch, int32& OutCount, FText& OutError);

	/** §4.4 "the cap is 100,000 matches (proposed)". */
	inline constexpr int32 MaxMatches = 100000;

	/** Game thread: Query compiled for a worker, or false with the message under the find box. */
	DREAMEDITOR_API bool Compile(const FDreamFindQuery& Query, std::shared_ptr<const dream::CompiledQuery>& OutQuery, FText& OutError);
	/** Game thread: Template checked against Query's groups before a worker expands it. */
	DREAMEDITOR_API bool ValidateTemplate(const dream::CompiledQuery& Query, const FString& Template, FText& OutError);
	/** Any thread: every match of a compiled query in Snapshot, stopping soon after Cancel is set (no FText here). */
	DREAMEDITOR_API FDreamFindResult SearchCompiled(const dream::TextSnapshot& Snapshot, const dream::CompiledQuery& Query,
		const dream::CancelToken* Cancel);
	/** Any thread: Replace All as one batch over the matches of a compiled query (Python re.sub semantics). */
	DREAMEDITOR_API FDreamReplaceAllResult BuildReplaceAllCompiled(const dream::TextSnapshot& Snapshot, const dream::CompiledQuery& Query,
		const std::u16string& Template, const dream::CancelToken* Cancel);
}
