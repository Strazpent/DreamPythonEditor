// SPDX-License-Identifier: Apache-2.0
// Dream core: the output view's row <-> sequence map (TDD §6.4 "Visible index", T-OUT-006).
#include "dream/VisibleIndex.h"

#include <algorithm>
#include <utility>

namespace dream::visibleindex_detail
{
	void Rebuild(const std::vector<SeqRange>& Ranges, std::vector<uint64_t>& RowsBefore)
	{
		RowsBefore.resize(Ranges.size());
		uint64_t Rows = 0;
		for (size_t I = 0; I < Ranges.size(); ++I)
		{
			RowsBefore[I] = Rows;
			Rows += Ranges[I].End - Ranges[I].Begin;
		}
	}

	/** The first range whose End is above Seq. */
	size_t FirstEndingAfter(const std::vector<SeqRange>& Ranges, uint64_t Seq)
	{
		return static_cast<size_t>(std::upper_bound(Ranges.begin(), Ranges.end(), Seq,
			[](uint64_t S, const SeqRange& R) { return S < R.End; }) - Ranges.begin());
	}
}

namespace dream
{
	void VisibleIndex::Clear()
	{
		RangeList.clear();
		RowsBefore.clear();
	}

	void VisibleIndex::Append(uint64_t Begin, uint64_t End)
	{
		if (!RangeList.empty())
		{
			Begin = std::max(Begin, RangeList.back().End);
		}
		if (End <= Begin)
		{
			return;
		}
		if (!RangeList.empty() && RangeList.back().End == Begin)
		{
			RangeList.back().End = End;
			return;
		}
		const uint64_t Rows = RangeList.empty() ? 0 : RowsBefore.back() + (RangeList.back().End - RangeList.back().Begin);
		RangeList.push_back(SeqRange{Begin, End});
		RowsBefore.push_back(Rows);
	}

	void VisibleIndex::Assign(std::vector<SeqRange> Ranges)
	{
		std::sort(Ranges.begin(), Ranges.end(), [](const SeqRange& A, const SeqRange& B) { return A.Begin < B.Begin; });
		RangeList.clear();
		for (const SeqRange& R : Ranges)
		{
			if (R.End <= R.Begin)
			{
				continue;
			}
			if (!RangeList.empty() && R.Begin <= RangeList.back().End)
			{
				RangeList.back().End = std::max(RangeList.back().End, R.End);
			}
			else
			{
				RangeList.push_back(R);
			}
		}
		visibleindex_detail::Rebuild(RangeList, RowsBefore);
	}

	void VisibleIndex::DropBefore(uint64_t Seq)
	{
		const size_t Keep = visibleindex_detail::FirstEndingAfter(RangeList, Seq);
		if (Keep == 0 && (RangeList.empty() || RangeList.front().Begin >= Seq))
		{
			return;
		}
		RangeList.erase(RangeList.begin(), RangeList.begin() + static_cast<std::ptrdiff_t>(Keep));
		if (!RangeList.empty() && RangeList.front().Begin < Seq)
		{
			RangeList.front().Begin = Seq;
		}
		visibleindex_detail::Rebuild(RangeList, RowsBefore);
	}

	uint64_t VisibleIndex::RowCount() const
	{
		if (RangeList.empty())
		{
			return 0;
		}
		return RowsBefore.back() + (RangeList.back().End - RangeList.back().Begin);
	}

	std::optional<uint64_t> VisibleIndex::SeqAtRow(uint64_t Row) const
	{
		if (Row >= RowCount())
		{
			return std::nullopt;
		}
		const size_t I = static_cast<size_t>(std::upper_bound(RowsBefore.begin(), RowsBefore.end(), Row) - RowsBefore.begin()) - 1;
		return RangeList[I].Begin + (Row - RowsBefore[I]);
	}

	std::optional<uint64_t> VisibleIndex::RowAtOrAfter(uint64_t Seq) const
	{
		const size_t I = visibleindex_detail::FirstEndingAfter(RangeList, Seq);
		if (I >= RangeList.size())
		{
			return std::nullopt;
		}
		const SeqRange& R = RangeList[I];
		return RowsBefore[I] + (Seq >= R.Begin ? Seq - R.Begin : 0);
	}

	bool VisibleIndex::Contains(uint64_t Seq) const
	{
		const size_t I = visibleindex_detail::FirstEndingAfter(RangeList, Seq);
		return I < RangeList.size() && RangeList[I].Begin <= Seq;
	}
}
