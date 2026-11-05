// SPDX-License-Identifier: Apache-2.0
// Tier 1: CRC32C and SHA-256 known-answer vectors (RFC 3720 B.4, FIPS 180-4 examples).
#include "dream_test.h"

#include "dream/Checksum.h"

#include <string>

TEST(Checksum_Crc32cKnownVectors)
{
	CHECK_EQ(dream::Crc32c(std::string_view("123456789")), 0xE3069283u);
	CHECK_EQ(dream::Crc32c(std::string_view("")), 0u);
	const std::string Zeros(32, '\0');
	CHECK_EQ(dream::Crc32c(Zeros), 0x8A9136AAu);          // RFC 3720 B.4: 32 bytes of zeros
	const std::string Ones(32, '\xFF');
	CHECK_EQ(dream::Crc32c(Ones), 0x62A8AB43u);           // 32 bytes of 0xFF
	std::string Inc;
	for (int I = 0; I < 32; ++I)
	{
		Inc.push_back(static_cast<char>(I));
	}
	CHECK_EQ(dream::Crc32c(Inc), 0x46DD794Eu);            // 32 incrementing bytes
}

TEST(Checksum_Crc32cChainsAcrossSplits)
{
	std::string Data;
	for (int I = 0; I < 1000; ++I)
	{
		Data.push_back(static_cast<char>((I * 37 + 11) & 0xFF));
	}
	const uint32_t Whole = dream::Crc32c(Data);
	for (size_t Split : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(9), size_t(500), size_t(999), size_t(1000)})
	{
		const std::string_view A = std::string_view(Data).substr(0, Split);
		const std::string_view B = std::string_view(Data).substr(Split);
		CHECK_EQ(dream::Crc32c(B, dream::Crc32c(A)), Whole);
	}
}

TEST(Checksum_Sha256KnownVectors)
{
	CHECK_EQ(dream::Sha256::ToHex(dream::Sha256::Hash("")), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	CHECK_EQ(dream::Sha256::ToHex(dream::Sha256::Hash("abc")), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	CHECK_EQ(dream::Sha256::ToHex(dream::Sha256::Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
		std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	CHECK_EQ(dream::Sha256::ToHex(dream::Sha256::Hash(
		"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
		std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
	dream::Sha256 H;
	const std::string Chunk(1000, 'a');
	for (int I = 0; I < 1000; ++I)
	{
		H.Update(Chunk);
	}
	CHECK_EQ(dream::Sha256::ToHex(H.Final()), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(Checksum_Sha256IncrementalEqualsOneShot)
{
	std::string Data;
	for (int I = 0; I < 777; ++I)
	{
		Data.push_back(static_cast<char>(I * 13));
	}
	const dream::Sha256Digest Whole = dream::Sha256::Hash(Data);
	for (size_t Step : {size_t(1), size_t(3), size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(200)})
	{
		dream::Sha256 H;
		for (size_t I = 0; I < Data.size(); I += Step)
		{
			H.Update(std::string_view(Data).substr(I, Step));
		}
		CHECK(H.Final() == Whole);
		H.Reset();
		H.Update(Data);
		CHECK(H.Final() == Whole);
	}
}
