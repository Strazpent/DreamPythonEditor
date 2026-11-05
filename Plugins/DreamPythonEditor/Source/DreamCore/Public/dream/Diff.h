// SPDX-License-Identifier: Apache-2.0
// Dream core: text differences (TDD §4.6). Stateless; every function is pure and may run on any thread (the snapshots
// it reads are immutable).
//  - DiffSingleRange: one common-prefix/common-suffix range. Plain mode reflects SMultiLineEditableText's whole-text
//    OnTextChanged with it (§4.15 row 3), and the journal writer builds DELTA ops with it (§10.2.2, A4).
//  - Diff::Lines: the line diff (Myers with linear-space division, the D = 4,000 / 50 ms cap, hunk sliding and
//    word-level intra-line ranges) for local history, the recovery Compare and change bars.
//  - Diff::ToMinimalEdits: the edit batch that turns Old into New touching only changed lines (reload in place, BUF-04
//    §10.15; recovery and history restore, U30; format).
//  - Diff::Unified / FormatUnified: the read-only unified diff of the M1 Compare (§10.7.2).
#pragma once

#include "dream/CancelToken.h"
#include "dream/TextBuffer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dream
{
	/**
	 * The one range that turns Old into New: the longest common prefix and then the longest common suffix are kept.
	 * Never splits a surrogate pair. Old == New gives DeleteLen 0 and an empty Insert at Old.size().
	 */
	DREAMCORE_API EditRange DiffSingleRange(std::u16string_view Old, std::u16string_view New);

	/** Same over two snapshots, without copying either text when both are contiguous. Any thread. */
	DREAMCORE_API EditRange DiffSingleRange(const TextSnapshot& Old, const TextSnapshot& New);

	/** A run of UTF-16 units in one of the two texts (absolute offsets). */
	struct DiffSpan
	{
		Offset At = 0;
		int64_t Len = 0;

		bool operator==(const DiffSpan&) const = default;
	};

	/**
	 * One changed region in diff lines. A diff line is a model line with its '\n'; a text that ends in '\n' has no
	 * trailing empty diff line, so the lines are those of a unified diff and line i is model line i. OldCount == 0 is a
	 * pure insertion before old line OldFirst; NewCount == 0 a pure deletion before new line NewFirst.
	 */
	struct DiffHunk
	{
		LineNo OldFirst = 0;
		LineNo OldCount = 0;
		LineNo NewFirst = 0;
		LineNo NewCount = 0;
		/** The cap (D or time) turned this middle into one replace hunk; it is valid but may not be minimal. */
		bool bApproximate = false;
		/** Changed character runs inside a replace hunk's lines (word-token Myers, §4.6 step 6), ascending. */
		std::vector<DiffSpan> OldIntra;
		std::vector<DiffSpan> NewIntra;
	};

	struct DiffOptions
	{
		/** §4.6 step 5 (proposed): a middle whose edit distance passes this becomes one replace hunk. */
		int32_t MaxEditDistance = 4000;
		/** §4.6 step 5 (proposed): wall-clock cap for the whole call; <= 0 means none (deterministic tests). */
		double TimeLimitMs = 50.0;
		/** §4.6 step 4: slide insert-only and delete-only hunks to blank-line or indent boundaries. */
		bool bSlideHunks = true;
		/** §4.6 step 6: word-level intra-line ranges in replace hunks. */
		bool bIntraLine = true;
		/** Replace hunks with more word tokens than this on either side get no intra-line ranges. */
		int32_t MaxIntraTokens = 20000;
	};

	struct DiffResult
	{
		std::vector<DiffHunk> Hunks;     // ascending, non-overlapping, separated by at least one equal line
		bool bApproximate = false;       // some hunk is approximate
		bool bCancelled = false;         // stopped by the token: Hunks is empty and nothing may be applied
		LineNo OldLines = 0;             // diff-line counts of the two texts
		LineNo NewLines = 0;

		bool IsIdentical() const { return Hunks.empty() && !bCancelled; }
		/** "+12 −3 lines" (§10.7.1): lines added and removed over all hunks. */
		int32_t AddedLines() const
		{
			int32_t N = 0;
			for (const DiffHunk& H : Hunks)
			{
				N += H.NewCount;
			}
			return N;
		}
		int32_t RemovedLines() const
		{
			int32_t N = 0;
			for (const DiffHunk& H : Hunks)
			{
				N += H.OldCount;
			}
			return N;
		}
	};

	/** One line of a unified diff. Text includes the prefix ("--- ", "@@ ", ' ', '-', '+', '\'), without '\n'. */
	struct UnifiedLine
	{
		enum class Kind : uint8_t
		{
			FileHeader,      // "--- <old>" or "+++ <new>"
			HunkHeader,      // "@@ -l,s +l,s @@"
			Context,         // " text"
			Removed,         // "-text"
			Added,           // "+text"
			NoNewline,       // "\ No newline at end of file" after the line it describes
		};
		Kind Type = Kind::Context;
		std::u16string Text;
		LineNo OldLine = InvalidLine;    // 0-based line in Old for Context and Removed, else InvalidLine
		LineNo NewLine = InvalidLine;    // 0-based line in New for Context and Added, else InvalidLine
	};

	struct UnifiedOptions
	{
		int32_t Context = 3;             // equal lines around each hunk; hunks closer than 2 x Context merge
		/** File header labels; both empty: no "---" / "+++" lines (the Compare view names the texts itself). */
		std::u16string OldLabel;
		std::u16string NewLabel;
	};

	struct DREAMCORE_API Diff
	{
		/** The line diff of §4.6. Cancel may be null. Old == New gives no hunks. */
		static DiffResult Lines(const TextSnapshot& Old, const TextSnapshot& New, const DiffOptions& Options,
			const CancelToken* Cancel = nullptr);
		static DiffResult Lines(const TextSnapshot& Old, const TextSnapshot& New);

		/**
		 * The batch that turns Old into New from a Lines result of those two snapshots (§4.6 Users): one range per hunk,
		 * narrowed to the hunk's common prefix and suffix (never splitting a surrogate pair), so lines outside the hunks
		 * and the unchanged ends of changed lines keep their anchors (BUF-04: carets, breakpoints and folds stay put).
		 * A cancelled result gives an empty batch. Ranges are ascending, in Old's coordinates; Apply it to a buffer whose
		 * snapshot is Old.
		 */
		static EditBatch ToMinimalEdits(const TextSnapshot& Old, const TextSnapshot& New, const DiffResult& Result,
			EditOrigin Origin = EditOrigin::Reload);
		/** Lines with no time limit, sliding or intra-line pass, then ToMinimalEdits (game-thread reloads of < 1 MB). */
		static EditBatch ToMinimalEdits(const TextSnapshot& Old, const TextSnapshot& New,
			EditOrigin Origin = EditOrigin::Reload);

		/** The unified diff of a Lines result (GNU format: "@@ -l,s +l,s @@", ",1" omitted, "\ No newline..."). */
		static std::vector<UnifiedLine> Unified(const TextSnapshot& Old, const TextSnapshot& New, const DiffResult& Result,
			const UnifiedOptions& Options);
		/** Unified joined with '\n' (a final '\n' after the last line); empty when the texts are equal. */
		static std::u16string FormatUnified(const TextSnapshot& Old, const TextSnapshot& New, const DiffResult& Result,
			const UnifiedOptions& Options);
	};
}
