// SPDX-License-Identifier: Apache-2.0
// Dream core: the output view's row <-> sequence map under filters and collapsed blocks (TDD §6.4 "Visible index",
// T-OUT-006, tier 1). Sorted [SeqBegin, SeqEnd) ranges with prefix sums; row lookup is O(log R). Game thread.
// (slice A choice) It lives in DreamCore as dream::VisibleIndex so tier 1 can test it; §6.4 calls it FDreamVisibleIndex.
#pragma once

#include "dream/Export.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace dream
{
	struct SeqRange
	{
		uint64_t Begin = 0;            // first visible sequence
		uint64_t End = 0;              // one past the last
	};

	class DREAMCORE_API VisibleIndex
	{
	public:
		void Clear();

		/** Appends a range after every existing one; merges with the last range when contiguous. Begin >= last End. */
		void Append(uint64_t Begin, uint64_t End);
		void AppendOne(uint64_t Seq) { Append(Seq, Seq + 1); }

		/** Replaces everything with sorted, non-overlapping ranges (a rebuilt filter). */
		void Assign(std::vector<SeqRange> Ranges);

		/** Drops every sequence below Seq (eviction, Clear). */
		void DropBefore(uint64_t Seq);

		uint64_t RowCount() const;
		std::optional<uint64_t> SeqAtRow(uint64_t Row) const;
		/** The row of Seq, or of the first visible sequence after it when Seq is hidden; nullopt when none follows. */
		std::optional<uint64_t> RowAtOrAfter(uint64_t Seq) const;
		bool Contains(uint64_t Seq) const;
		const std::vector<SeqRange>& Ranges() const { return RangeList; }

	private:
		std::vector<SeqRange> RangeList;
		std::vector<uint64_t> RowsBefore;  // prefix sums: rows before RangeList[i]
	};
}
