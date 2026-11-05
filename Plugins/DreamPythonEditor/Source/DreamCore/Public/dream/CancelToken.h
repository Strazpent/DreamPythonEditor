// SPDX-License-Identifier: Apache-2.0
// Dream core: the cancellation flag workers poll (TDD §4 conventions: "DreamEditor launches them with UE::Tasks::Launch
// and passes a dream::CancelToken"). Header-only; any thread. The owner keeps it alive until the worker has returned.
#pragma once

#include "dream/Export.h"

#include <atomic>

namespace dream
{
	class CancelToken
	{
	public:
		CancelToken() = default;
		CancelToken(const CancelToken&) = delete;
		CancelToken& operator=(const CancelToken&) = delete;

		/** Any thread. Once set, it stays set until Reset. */
		void Cancel() { bCancelled.store(true, std::memory_order_relaxed); }
		void Reset() { bCancelled.store(false, std::memory_order_relaxed); }
		bool IsCancelled() const { return bCancelled.load(std::memory_order_relaxed); }

	private:
		std::atomic<bool> bCancelled{false};
	};

	/** Null-safe poll for APIs that take an optional token. */
	inline bool IsCancelled(const CancelToken* Token)
	{
		return Token != nullptr && Token->IsCancelled();
	}
}
