// SPDX-License-Identifier: Apache-2.0
// Shared helpers for the tier-1 DreamCore suites: UTF-8 literals to model text, and a deterministic PRNG.
#pragma once

#include "dream/Utf.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace dream_test_util
{
	/** Model text from a UTF-8 literal (tests write u8-free ASCII or escaped UTF-8 for readability). */
	inline std::u16string U(std::string_view Utf8)
	{
		std::u16string Out;
		dream::utf::Utf8ToUtf16(Utf8, Out);
		return Out;
	}

	inline std::string N(std::u16string_view Text)
	{
		std::string Out;
		dream::utf::Utf16ToUtf8(Text, Out);
		return Out;
	}

	/** xorshift64: reproducible across toolchains (std::uniform_int_distribution is not). */
	struct Rng
	{
		uint64_t S;

		explicit Rng(uint64_t Seed) : S(Seed ? Seed : 0x9E3779B97F4A7C15ull) {}

		uint64_t Next()
		{
			S ^= S << 13;
			S ^= S >> 7;
			S ^= S << 17;
			return S;
		}

		uint64_t Below(uint64_t Bound) { return Bound == 0 ? 0 : Next() % Bound; }
		bool OneIn(uint64_t K) { return Below(K) == 0; }
	};
}
