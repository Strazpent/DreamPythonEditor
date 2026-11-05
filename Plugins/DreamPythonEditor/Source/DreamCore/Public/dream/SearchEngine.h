// SPDX-License-Identifier: Apache-2.0
// Dream core: in-buffer find and replace (TDD §4.4, EDT-09 M1: find, replace, match case, regex; whole word too).
// Compile once (any thread), then search snapshots on a worker or on the game thread; a CompiledQuery is immutable and
// shared. Matches are UTF-16 offsets.
// (slice B choice) The regex engine is DreamCore's own bounded backtracking matcher with Python `re` syntax and
// semantics for str patterns (classes and IGNORECASE folding generated from the engine's CPython 3.11.8), MULTILINE
// always on as §4.4 compiles PCRE2, and PCRE2-like limits: 10^6 steps per line of start positions and a bounded
// backtracking stack; a line over a limit is skipped and counted ("Pattern too complex on n lines"). PCRE2 is not
// vendored and DreamCore cannot throw, so std::regex is out; UE's FRegexMatcher (ICU) has no step limit or error
// reporting. PCRE2 can replace the matcher behind CompiledQuery without changing this interface.
#pragma once

#include "dream/CancelToken.h"
#include "dream/TextBuffer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream
{
	struct SearchQuery
	{
		std::u16string Pattern;
		bool bRegex = false;           // Python `re` syntax; else the pattern is literal text
		bool bMatchCase = false;       // off: IGNORECASE (Python's Unicode folding)
		bool bWholeWord = false;       // the match is not preceded or followed by a word character (\w)
	};

	enum class SearchErrorCode : uint8_t
	{
		None,
		EmptyPattern,                  // nothing to find
		Syntax,                        // the pattern is not valid Python `re` syntax (Detail says what)
		Unsupported,                   // valid Python, not supported here: \N{...}, (?(group)...), (?L)
		TooComplex,                    // the compiled pattern would be too large (huge counted repeats, deep nesting)
		BadTemplate,                   // the replacement template refers to a missing group or has a bad escape
	};

	struct SearchError
	{
		SearchErrorCode Code = SearchErrorCode::None;
		int32_t Position = -1;         // UTF-16 index in the pattern or template, -1 when none
		std::u16string Detail;         // English, as Python words it ("nothing to repeat", "bad escape \q")
	};

	/** A compiled query. Opaque and immutable; any thread. */
	class CompiledQuery;

	struct CompileResult
	{
		std::shared_ptr<const CompiledQuery> Query;   // null on error
		SearchError Error;

		bool Ok() const { return Query != nullptr; }
	};

	struct SearchMatch
	{
		Offset At = 0;
		int64_t Len = 0;               // may be 0 for patterns that match empty text

		bool operator==(const SearchMatch&) const = default;
	};

	struct SearchLimits
	{
		size_t MaxMatches = 100000;           // §4.4 cap (proposed); the count then reads "m+"
		uint64_t StepLimit = 1000000;         // §4.4 match limit, per line of start positions
		size_t MaxBacktrack = size_t(1) << 20; // backtracking entries (about 24 MB), §4.4 heap limit
	};

	struct SearchSummary
	{
		size_t Count = 0;
		bool bCapped = false;          // MaxMatches reached: more matches exist
		int32_t ComplexLines = 0;      // lines skipped over a limit
		bool bCancelled = false;
	};

	class DREAMCORE_API SearchEngine
	{
	public:
		static CompileResult Compile(const SearchQuery& Query);

		/** True when a match may contain or depend on a '\n' (incremental maintenance then rescans everything). */
		static bool CanMatchNewline(const CompiledQuery& Query);
		/** Groups in the pattern (group 0 is the whole match). */
		static int32_t GroupCount(const CompiledQuery& Query);

		/**
		 * Every match that starts in [From, To] of Text, in order, with Python's finditer rules (an empty match may
		 * follow a non-empty one; never two at the same place). To < 0 means the end. Text is the whole buffer, so
		 * anchors and lookarounds see the real context.
		 */
		static std::vector<SearchMatch> FindAll(std::u16string_view Text, const CompiledQuery& Query, Offset From = 0,
			Offset To = -1, SearchSummary* OutSummary = nullptr, const CancelToken* Cancel = nullptr,
			const SearchLimits& Limits = SearchLimits());
		static std::vector<SearchMatch> FindAll(const TextSnapshot& Snapshot, const CompiledQuery& Query,
			SearchSummary* OutSummary = nullptr, const CancelToken* Cancel = nullptr);

		/**
		 * The incremental search of §4.4 (worker): the lines of [PriorityFirst, PriorityLast] first (the visible range),
		 * then the rest from there to the end and from the start. Sink receives batches about every 5 ms (proposed) and
		 * once more at the end; batches after the first are not in document order. A pattern that can match '\n'
		 * searches in document order from the start. Stops at the cap or when Cancel is set (checked about every
		 * 0.1 ms of work, so cancel takes effect well within 1 ms).
		 */
		static SearchSummary Run(const TextSnapshot& Snapshot, const CompiledQuery& Query, Offset PriorityFirst,
			Offset PriorityLast, const CancelToken* Cancel, const std::function<void(std::span<const SearchMatch>)>& Sink,
			const SearchLimits& Limits = SearchLimits());

		/** F3 / Shift+F3: the first match starting at or after From (forward), or the last one ending at or before
		 *  From (backward), wrapping around the text when bWrap. Game thread on plain-mode buffers (< 1 MB). */
		static std::optional<SearchMatch> FindNext(const TextSnapshot& Snapshot, const CompiledQuery& Query, Offset From,
			bool bBackward, bool bWrap = true);

		/**
		 * Match maintenance after an edit (§4.4 "After edits"): matches shift through the ChangeSet, matches that
		 * overlap or touch an edited range drop, and the edited lines +-1 are searched again; a pattern that can match
		 * '\n' searches the whole text again. Matches stay sorted. The result equals a full search of After.
		 */
		static void UpdateAfterEdit(std::vector<SearchMatch>& Matches, const ChangeSet& Changes, const TextSnapshot& After,
			const CompiledQuery& Query);

		/**
		 * Replacement text for one match (§4.4 Replace): regex templates take \1..\99, \g<1>, \g<name>, $1, ${name}, $$,
		 * \n, \t, \\ and Python's other escapes (an unmatched group is empty); a literal query's template is literal.
		 * bPreserveCase: an all-upper match gives upper-case text, a capitalised one a capitalised text. False (with
		 * OutError) for a bad template or a match the query does not make at that place.
		 */
		static bool ExpandReplacement(std::u16string_view Text, const CompiledQuery& Query, const SearchMatch& Match,
			std::u16string_view Template, bool bPreserveCase, std::u16string& Out, SearchError* OutError = nullptr);
		/** Checks a template against the query's groups before a Replace (shown under the replace box). */
		static bool ValidateTemplate(const CompiledQuery& Query, std::u16string_view Template, SearchError* OutError = nullptr);

		/**
		 * Replace All as one batch (origin Command; the caller records it as one undo group, §4.2 R5) over the matches
		 * that start in [From, To] (in-selection), To < 0 meaning the end. Empty when nothing matches or the template is
		 * bad. Empty matches are replaced as Python's re.sub does.
		 */
		static EditBatch BuildReplaceAll(const TextSnapshot& Snapshot, const CompiledQuery& Query, std::u16string_view Template,
			bool bPreserveCase, Offset From = 0, Offset To = -1, SearchError* OutError = nullptr);
	};
}
