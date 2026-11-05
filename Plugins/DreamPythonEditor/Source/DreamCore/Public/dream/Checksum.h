// SPDX-License-Identifier: Apache-2.0
// Dream core: CRC32C (journal frames, TDD §10.2.2) and SHA-256 (base versions §10.14, project hash §10.1,
// run-record source hash §5.2.3). The engine's Core has no SHA-256, so DreamCore carries one. Any thread.
#pragma once

#include "dream/Export.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dream
{
	/** CRC-32C (Castagnoli, reflected polynomial 0x82F63B78), chainable: Crc32c(B, Crc32c(A)) == Crc32c(A + B). */
	DREAMCORE_API uint32_t Crc32c(const void* Data, size_t Size, uint32_t Previous = 0);
	inline uint32_t Crc32c(std::string_view Bytes, uint32_t Previous = 0) { return Crc32c(Bytes.data(), Bytes.size(), Previous); }

	using Sha256Digest = std::array<uint8_t, 32>;
	// Needs the complete type, so std::array<uint8_t, 32> is instantiated before the exported functions below return
	// it. Without that, a dllimport caller that reaches Sha256::Hash or Final first gets MSVC C4686 (a UDT return
	// convention that could differ between the declaration and the call).
	static_assert(sizeof(Sha256Digest) == 32, "dream::Sha256Digest is 32 bytes");

	/** Incremental SHA-256 (FIPS 180-4). */
	class DREAMCORE_API Sha256
	{
	public:
		Sha256();
		void Update(const void* Data, size_t Size);
		void Update(std::string_view Bytes) { Update(Bytes.data(), Bytes.size()); }
		/** Finishes the hash; the object must be Reset before reuse. */
		Sha256Digest Final();
		void Reset();

		static Sha256Digest Hash(std::string_view Bytes);
		/** Lower-case hex, 64 characters. */
		static std::string ToHex(const Sha256Digest& Digest);

	private:
		uint32_t State[8];
		uint64_t TotalBytes;
		uint8_t Block[64];
		uint32_t BlockUsed;
	};
}
