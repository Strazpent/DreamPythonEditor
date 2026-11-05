// SPDX-License-Identifier: Apache-2.0
// Dream core, private: the gap buffer behind PyLexer's line array (TDD §4.3.3). Header-only; tier 1 tests it directly.
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace dream::lexer_detail
{
	/**
	 * The §4.3.3 line array: a vector with a gap at the last splice. Splice moves the gap there (cost: the records
	 * between the two splices) and then erases or inserts records at the gap; indexing skips the gap.
	 */
	template <class T>
	class GapArray
	{
	public:
		size_t size() const { return Items.size() - GapLen; }
		T& operator[](size_t I) { return Items[I < GapStart ? I : I + GapLen]; }
		const T& operator[](size_t I) const { return Items[I < GapStart ? I : I + GapLen]; }

		void clear()
		{
			Items.clear();
			GapStart = 0;
			GapLen = 0;
		}

		/** Count default records, no gap. */
		void resize(size_t Count)
		{
			clear();
			Items.resize(Count);
		}

		/** Replaces Erase records at At with Insert default records. */
		void Splice(size_t At, size_t Erase, size_t Insert)
		{
			MoveGap(At);
			for (size_t K = 0; K < Erase; ++K)
			{
				Items[GapStart + GapLen + K] = T();   // frees the erased record's tokens
			}
			GapLen += Erase;
			if (Insert > GapLen)
			{
				const size_t Extra = Insert - GapLen + std::max<size_t>(Items.size() / 8, 64);
				Items.insert(Items.begin() + static_cast<std::ptrdiff_t>(GapStart + GapLen), Extra, T());
				GapLen += Extra;
			}
			for (size_t K = 0; K < Insert; ++K)
			{
				Items[GapStart + K] = T();
			}
			GapStart += Insert;
			GapLen -= Insert;
		}

	private:
		void MoveGap(size_t At)
		{
			if (GapLen == 0 || At == GapStart)
			{
				// An empty gap moves for free; moving records onto themselves would be undefined (std::move's ranges
				// must not overlap that way) and empties their token vectors with libc++ and libstdc++.
				GapStart = At;
				return;
			}
			if (At < GapStart)
			{
				std::move_backward(Items.begin() + static_cast<std::ptrdiff_t>(At), Items.begin() + static_cast<std::ptrdiff_t>(GapStart),
					Items.begin() + static_cast<std::ptrdiff_t>(GapStart + GapLen));
			}
			else if (At > GapStart)
			{
				std::move(Items.begin() + static_cast<std::ptrdiff_t>(GapStart + GapLen), Items.begin() + static_cast<std::ptrdiff_t>(At + GapLen),
					Items.begin() + static_cast<std::ptrdiff_t>(GapStart));
			}
			GapStart = At;
		}

		std::vector<T> Items;
		size_t GapStart = 0;
		size_t GapLen = 0;
	};
}
