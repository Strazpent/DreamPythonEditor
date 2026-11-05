// SPDX-License-Identifier: Apache-2.0
// Dream core: shared scalar types (TDD §4 conventions). Standard library only; no engine types (D7).
#pragma once

#include "dream/Export.h"

#include <cstddef>
#include <cstdint>

namespace dream
{
	/** A UTF-16 code-unit position in a buffer. Inside the model every line ends with one '\n' (TDD §4.1.2). */
	using Offset = int64_t;

	/** A 0-based line number. The UI shows lines 1-based (TDD §4 conventions). */
	using LineNo = int32_t;

	/** Returned by line lookups that have no answer (deleted line, version gone, §4.1.3 MapLine). */
	inline constexpr LineNo InvalidLine = -1;

	/** An inclusive range of 0-based lines, First <= Last (TDD §4.5). */
	struct LineRange
	{
		LineNo First = 0;
		LineNo Last = 0;

		bool Contains(LineNo Line) const { return Line >= First && Line <= Last; }
		int32_t Count() const { return Last - First + 1; }
		bool operator==(const LineRange&) const = default;
	};
}
