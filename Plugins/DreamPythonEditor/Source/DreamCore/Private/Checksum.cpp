// SPDX-License-Identifier: Apache-2.0
// Dream core: CRC32C (TDD §10.2.2) and SHA-256 (FIPS 180-4; §10.1, §10.14, §5.2.3).
#include "dream/Checksum.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace dream::checksum_detail
{
	using CrcTables = std::array<std::array<uint32_t, 256>, 8>;

	// Slicing-by-8 tables for the reflected Castagnoli polynomial, built at compile time (no mutable globals).
	constexpr CrcTables MakeCrcTables()
	{
		CrcTables T{};
		for (uint32_t I = 0; I < 256; ++I)
		{
			uint32_t C = I;
			for (int K = 0; K < 8; ++K)
			{
				C = (C & 1u) ? (C >> 1) ^ 0x82F63B78u : (C >> 1);
			}
			T[0][I] = C;
		}
		for (uint32_t I = 0; I < 256; ++I)
		{
			for (int S = 1; S < 8; ++S)
			{
				T[S][I] = (T[S - 1][I] >> 8) ^ T[0][T[S - 1][I] & 0xFFu];
			}
		}
		return T;
	}

	inline constexpr CrcTables Crc = MakeCrcTables();

	inline constexpr uint32_t K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};

	inline uint32_t Rotr(uint32_t X, int N) { return (X >> N) | (X << (32 - N)); }

	void Compress(uint32_t State[8], const uint8_t* Block)
	{
		uint32_t W[64];
		for (int I = 0; I < 16; ++I)
		{
			W[I] = (static_cast<uint32_t>(Block[I * 4]) << 24) | (static_cast<uint32_t>(Block[I * 4 + 1]) << 16)
				| (static_cast<uint32_t>(Block[I * 4 + 2]) << 8) | static_cast<uint32_t>(Block[I * 4 + 3]);
		}
		for (int I = 16; I < 64; ++I)
		{
			const uint32_t S0 = Rotr(W[I - 15], 7) ^ Rotr(W[I - 15], 18) ^ (W[I - 15] >> 3);
			const uint32_t S1 = Rotr(W[I - 2], 17) ^ Rotr(W[I - 2], 19) ^ (W[I - 2] >> 10);
			W[I] = W[I - 16] + S0 + W[I - 7] + S1;
		}
		uint32_t A = State[0], B = State[1], C = State[2], D = State[3];
		uint32_t E = State[4], F = State[5], G = State[6], H = State[7];
		for (int I = 0; I < 64; ++I)
		{
			const uint32_t S1 = Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25);
			const uint32_t Ch = (E & F) ^ (~E & G);
			const uint32_t T1 = H + S1 + Ch + K[I] + W[I];
			const uint32_t S0 = Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22);
			const uint32_t Maj = (A & B) ^ (A & C) ^ (B & C);
			const uint32_t T2 = S0 + Maj;
			H = G;
			G = F;
			F = E;
			E = D + T1;
			D = C;
			C = B;
			B = A;
			A = T1 + T2;
		}
		State[0] += A;
		State[1] += B;
		State[2] += C;
		State[3] += D;
		State[4] += E;
		State[5] += F;
		State[6] += G;
		State[7] += H;
	}
}

namespace dream
{
	uint32_t Crc32c(const void* Data, size_t Size, uint32_t Previous)
	{
		const auto& T = checksum_detail::Crc;
		const uint8_t* P = static_cast<const uint8_t*>(Data);
		uint32_t C = ~Previous;
		while (Size >= 8)
		{
			const uint32_t Lo = C ^ (static_cast<uint32_t>(P[0]) | (static_cast<uint32_t>(P[1]) << 8)
				| (static_cast<uint32_t>(P[2]) << 16) | (static_cast<uint32_t>(P[3]) << 24));
			C = T[7][Lo & 0xFF] ^ T[6][(Lo >> 8) & 0xFF] ^ T[5][(Lo >> 16) & 0xFF] ^ T[4][Lo >> 24]
				^ T[3][P[4]] ^ T[2][P[5]] ^ T[1][P[6]] ^ T[0][P[7]];
			P += 8;
			Size -= 8;
		}
		while (Size-- > 0)
		{
			C = T[0][(C ^ *P++) & 0xFFu] ^ (C >> 8);
		}
		return ~C;
	}

	Sha256::Sha256()
	{
		Reset();
	}

	void Sha256::Reset()
	{
		static constexpr uint32_t Init[8] = {
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
		};
		std::memcpy(State, Init, sizeof(State));
		TotalBytes = 0;
		BlockUsed = 0;
		std::memset(Block, 0, sizeof(Block));
	}

	void Sha256::Update(const void* Data, size_t Size)
	{
		const uint8_t* P = static_cast<const uint8_t*>(Data);
		TotalBytes += Size;
		if (BlockUsed > 0)
		{
			const size_t Take = std::min<size_t>(64 - BlockUsed, Size);
			std::memcpy(Block + BlockUsed, P, Take);
			BlockUsed += static_cast<uint32_t>(Take);
			P += Take;
			Size -= Take;
			if (BlockUsed == 64)
			{
				checksum_detail::Compress(State, Block);
				BlockUsed = 0;
			}
		}
		while (Size >= 64)
		{
			checksum_detail::Compress(State, P);
			P += 64;
			Size -= 64;
		}
		if (Size > 0)
		{
			std::memcpy(Block, P, Size);
			BlockUsed = static_cast<uint32_t>(Size);
		}
	}

	Sha256Digest Sha256::Final()
	{
		const uint64_t BitLength = TotalBytes * 8;
		uint8_t Pad[72] = {0x80};
		const size_t PadLen = (BlockUsed < 56) ? (56 - BlockUsed) : (120 - BlockUsed);
		for (int I = 0; I < 8; ++I)
		{
			Pad[PadLen + I] = static_cast<uint8_t>(BitLength >> (56 - 8 * I));
		}
		const uint64_t Saved = TotalBytes;
		Update(Pad, PadLen + 8);
		TotalBytes = Saved;
		Sha256Digest Out{};
		for (int I = 0; I < 8; ++I)
		{
			Out[I * 4] = static_cast<uint8_t>(State[I] >> 24);
			Out[I * 4 + 1] = static_cast<uint8_t>(State[I] >> 16);
			Out[I * 4 + 2] = static_cast<uint8_t>(State[I] >> 8);
			Out[I * 4 + 3] = static_cast<uint8_t>(State[I]);
		}
		return Out;
	}

	Sha256Digest Sha256::Hash(std::string_view Bytes)
	{
		Sha256 H;
		H.Update(Bytes);
		return H.Final();
	}

	std::string Sha256::ToHex(const Sha256Digest& Digest)
	{
		static constexpr char Hex[] = "0123456789abcdef";
		std::string Out;
		Out.reserve(64);
		for (uint8_t B : Digest)
		{
			Out.push_back(Hex[B >> 4]);
			Out.push_back(Hex[B & 0xF]);
		}
		return Out;
	}
}
