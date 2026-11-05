// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::history (TDD §10.4, PER-04): the index codec in the journal's framing and generations (round trip,
// TOMBSTONE, CLEAR, fallback to the previous generation, a newer format read only), run-entry merging (three identical
// runs give one entry with three numbers), the SUMIBLOB header and chunks, and retention with a fake clock (T-PER-010:
// the 30-day and 500 MB limits, each buffer's newest entry and every entry under 24 h kept, also over the size limit, a
// timestamp three days in the future kept, an unreferenced blob under 24 h old surviving the sweep, no older orphan blobs).
#include "dream_test.h"

#include "dream/Checksum.h"
#include "dream/HistoryFormat.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace dream::history;
using dream::journal::FileOps;
using dream::journal::Record;
using dream::journal::Writer;

namespace
{
	class MemOps : public FileOps
	{
	public:
		std::map<std::string, std::string> Files;
		std::map<Handle, std::string> Open;
		std::map<std::string, int64_t> Mtimes;
		Handle Next = 1;
		bool bFailAppend = false;
		std::set<std::string> UnreadablePaths;   // review r1 persist-M2: a scanner holds the file through the retries

		bool CreateExclusive(std::string_view Path, Handle& Out) override
		{
			if (Files.count(std::string(Path)) != 0)
			{
				return false;
			}
			Files[std::string(Path)];
			Out = Next++;
			Open[Out] = std::string(Path);
			return true;
		}
		bool OpenAppend(std::string_view Path, Handle& Out) override
		{
			Files[std::string(Path)];
			Out = Next++;
			Open[Out] = std::string(Path);
			return true;
		}
		bool Append(Handle H, std::string_view Bytes) override
		{
			if (bFailAppend || Open.count(H) == 0)
			{
				return false;
			}
			Files[Open[H]].append(Bytes);
			return true;
		}
		bool Flush(Handle H) override { return Open.count(H) != 0; }
		void Close(Handle H) override { Open.erase(H); }
		bool Truncate(Handle H, uint64_t Size) override
		{
			Files[Open[H]].resize(static_cast<size_t>(Size));
			return true;
		}
		bool ReadAll(std::string_view Path, std::string& Out) override
		{
			if (UnreadablePaths.count(std::string(Path)) != 0)
			{
				return false;
			}
			auto It = Files.find(std::string(Path));
			if (It == Files.end())
			{
				return false;
			}
			Out = It->second;
			return true;
		}
		bool RenameNew(std::string_view From, std::string_view To) override
		{
			if (Files.count(std::string(From)) == 0 || Files.count(std::string(To)) != 0)
			{
				return false;
			}
			Files[std::string(To)] = std::move(Files[std::string(From)]);
			Files.erase(std::string(From));
			return true;
		}
		bool ReplaceAtomic(std::string_view, std::string_view, std::string_view) override { return false; }
		bool Remove(std::string_view Path) override { return Files.erase(std::string(Path)) != 0; }
		bool List(std::string_view Dir, std::vector<std::string>& OutNames) override
		{
			const std::string Prefix = std::string(Dir) + "/";
			for (const auto& [Path, Bytes] : Files)
			{
				if (Path.compare(0, Prefix.size(), Prefix) == 0 && Path.find('/', Prefix.size()) == std::string::npos)
				{
					OutNames.push_back(Path.substr(Prefix.size()));
				}
			}
			return true;
		}
		bool MakeDirs(std::string_view) override { return true; }
		bool FlushDir(std::string_view) override { return true; }
		bool Exists(std::string_view Path) override { return Files.count(std::string(Path)) != 0; }
		std::string LastError() const override { return "mem"; }
		bool Touch(std::string_view Path) override
		{
			Mtimes[std::string(Path)] = 1;
			return Files.count(std::string(Path)) != 0;
		}
	};

	const std::string Dir = "root/history";
	constexpr int64_t Day = DayMs;
	constexpr int64_t Now = 1800000000000ll;

	std::string Sha(const std::string& Text)
	{
		return dream::Sha256::ToHex(dream::Sha256::Hash(Text));
	}

	Entry MakeTestEntry(const std::string& Id, const std::string& Uuid, EntryKind Kind, int64_t UtcMs, const std::string& Text)
	{
		Entry E;
		E.Id = Id;
		E.Uuid = Uuid;
		E.Path = Uuid == "scratch" ? std::string() : "c:/p/" + Uuid + ".py";
		E.Display = Uuid + ".py";
		E.Kind = Kind;
		E.UtcMs = UtcMs;
		E.Blob = Sha(Text);
		E.Size = static_cast<int64_t>(Text.size());
		E.Lines = 1 + static_cast<int64_t>(std::count(Text.begin(), Text.end(), '\n'));
		return E;
	}

	std::string IndexPath(uint64_t G)
	{
		return dream::journal::JoinPath(Dir, dream::journal::LogFileName(IndexLogFormat, G));
	}

	std::vector<std::string> Ids(const Index& I)
	{
		std::vector<std::string> Out;
		for (const Entry& E : I.Entries())
		{
			Out.push_back(E.Id);
		}
		return Out;
	}
}

TEST(History_IndexRoundTripsInJournalFraming)
{
	MemOps Ops;
	Writer W(Ops, Dir, dream::journal::FileHeader(), IndexLogFormat);
	Index Live;
	const Entry A = MakeTestEntry("a1", "u1", EntryKind::Save, Now - 3 * Day, "print(1)\n");
	const Entry B = MakeTestEntry("b1", "u2", EntryKind::Closed, Now - Day, "x = 2");
	Live.Apply(MakeEntry(A));
	Live.Apply(MakeEntry(B));
	REQUIRE(W.StartGeneration(1, Live.Checkpoint(), Now, 0.0));
	CHECK_EQ(Ops.Files[IndexPath(1)].substr(0, 8), std::string("SUMIHIDX"));

	Entry RunEntry = MakeTestEntry("c1", "u1", EntryKind::Run, Now, "print(1)\n");
	RunEntry.Runs = {14};
	REQUIRE(W.Commit(std::vector<Record>{MakeEntry(RunEntry), MakeTombstone("b1")}));
	RecoveredIndex R = ReadIndex(Ops, Dir, false);
	REQUIRE(R.bFound && !R.bUnreadable && !R.bNewerFormat);
	CHECK((Ids(R.Entries) == std::vector<std::string>{"a1", "c1"}));
	const Entry* C = R.Entries.Find("c1");
	REQUIRE(C != nullptr);
	CHECK(C->Kind == EntryKind::Run);
	CHECK((C->Runs == std::vector<uint32_t>{14}));
	CHECK_EQ(C->Blob, Sha("print(1)\n"));
	CHECK_EQ(C->Lines, 2);
	CHECK_EQ(C->Path, std::string("c:/p/u1.py"));
	CHECK_EQ(C->UtcMs, Now);

	// An ENTRY with a known id replaces it in place (a merged run entry); CLEAR empties the index.
	RunEntry.Runs = {14, 15};
	REQUIRE(W.Commit(std::vector<Record>{MakeEntry(RunEntry)}));
	R = ReadIndex(Ops, Dir, false);
	CHECK((Ids(R.Entries) == std::vector<std::string>{"a1", "c1"}));
	CHECK((R.Entries.Find("c1")->Runs == std::vector<uint32_t>{14, 15}));
	REQUIRE(W.Commit(std::vector<Record>{MakeClear(Now)}));
	CHECK(ReadIndex(Ops, Dir, false).Entries.Entries().empty());

	// The index rotates like the journal; a damaged newest generation falls back to the previous one, set aside .corrupt.
	REQUIRE(W.Commit(std::vector<Record>{MakeEntry(A)}));
	R = ReadIndex(Ops, Dir, false);
	REQUIRE(W.Rotate(R.Entries.Checkpoint(), Now, 0.0));
	REQUIRE(W.Commit(std::vector<Record>{MakeEntry(B)}));
	CHECK((Ids(ReadIndex(Ops, Dir, false).Entries) == std::vector<std::string>{"a1", "b1"}));
	Ops.Files[IndexPath(2)][20] ^= 0x40;   // the header CRC
	R = ReadIndex(Ops, Dir, true);
	CHECK(R.bFallbackUsed);
	CHECK((Ids(R.Entries) == std::vector<std::string>{"a1"}) || (Ids(R.Entries) == std::vector<std::string>{"a1", "b1"}));
	CHECK(Ops.Exists(IndexPath(2) + ".corrupt"));

	// A newer index format is read only: nothing removed, nothing renamed.
	MemOps Newer;
	Writer W2(Newer, Dir, dream::journal::FileHeader(), IndexLogFormat);
	REQUIRE(W2.StartGeneration(4, Live.Checkpoint(), Now, 0.0));
	{
		// keep the header CRC valid: re-encode it
		dream::journal::FileHeader H;
		H.Generation = 4;
		H.Minor = 9;
		std::string Header;
		dream::journal::EncodeHeader(H, Header, IndexLogFormat);
		Newer.Files[IndexPath(4)].replace(0, Header.size(), Header);
	}
	Newer.Files[IndexPath(3)] = "old";
	Newer.Files[IndexPath(5) + ".tmp"] = "stale";
	const RecoveredIndex NewerRead = ReadIndex(Newer, Dir, true);
	CHECK(NewerRead.bNewerFormat);
	CHECK(Newer.Exists(IndexPath(3)));
	CHECK(Newer.Exists(IndexPath(5) + ".tmp"));
}

TEST(History_RunEntriesMergeWhenTheTextIsTheSame)   // §10.4, T-PER-017: three identical runs, one entry
{
	Index I;
	auto Run = [&I](const std::string& Id, uint32_t Number, int64_t Utc, const std::string& Text)
	{
		Entry E = MakeTestEntry(Id, "u1", EntryKind::Run, Utc, Text);
		E.Runs = {Number};
		const Entry Planned = PlanRunEntry(I, E);
		I.Apply(MakeEntry(Planned));
		return Planned;
	};
	Run("r14", 14, Now, "same");
	Run("r15", 15, Now + 1, "same");
	const Entry Third = Run("r16", 16, Now + 2, "same");
	CHECK_EQ(Third.Id, std::string("r14"));
	REQUIRE_EQ(I.Entries().size(), 1u);
	CHECK((I.Entries()[0].Runs == std::vector<uint32_t>{14, 15, 16}));
	CHECK_EQ(I.Entries()[0].UtcMs, Now + 2);

	// A save in between, or another text, starts a new entry; ten saves are ten entries even with one blob (PER-04).
	Run("r17", 17, Now + 3, "other");
	CHECK_EQ(I.Entries().size(), 2u);
	I.Apply(MakeEntry(MakeTestEntry("s1", "u1", EntryKind::Save, Now + 4, "other")));
	Run("r18", 18, Now + 5, "other");
	CHECK_EQ(I.Entries().size(), 4u);
	for (int K = 0; K < 10; ++K)
	{
		I.Apply(MakeEntry(MakeTestEntry("save" + std::to_string(K), "u1", EntryKind::Save, Now + 10 + K, "other")));
	}
	CHECK_EQ(I.Entries().size(), 14u);
	// Another buffer's runs never merge into this one's.
	Entry Other = MakeTestEntry("o1", "u2", EntryKind::Run, Now + 30, "other");
	Other.Runs = {19};
	CHECK_EQ(PlanRunEntry(I, Other).Id, std::string("o1"));
}

TEST(History_BlobHeaderAndChunks)
{
	BlobHeader H;
	H.RawLength = 3u << 20;
	H.RawCrc32c = 0xDEADBEEF;
	std::string File;
	EncodeBlobHeader(H, File);
	CHECK_EQ(File.size(), BlobHeaderBytes);
	CHECK_EQ(File.substr(0, 8), std::string("SUMIBLOB"));
	AppendBlobChunk("abc", 1000, File);
	AppendBlobChunk("defgh", 2000, File);
	BlobHeader Back;
	REQUIRE(DecodeBlobHeader(File, Back));
	CHECK_EQ(Back.RawLength, H.RawLength);
	CHECK_EQ(Back.RawCrc32c, H.RawCrc32c);
	CHECK_EQ(Back.ChunkBytes, BlobChunkBytes);
	size_t Pos = BlobHeaderBytes;
	std::string_view Chunk;
	uint32_t Raw = 0;
	REQUIRE(NextBlobChunk(File, Pos, Chunk, Raw));
	CHECK(Chunk == "abc" && Raw == 1000);
	REQUIRE(NextBlobChunk(File, Pos, Chunk, Raw));
	CHECK(Chunk == "defgh" && Raw == 2000);
	CHECK(!NextBlobChunk(File, Pos, Chunk, Raw));
	// A torn chunk and a damaged header are refused.
	std::string Torn = File.substr(0, File.size() - 2);
	Pos = BlobHeaderBytes;
	CHECK(NextBlobChunk(Torn, Pos, Chunk, Raw));
	CHECK(!NextBlobChunk(Torn, Pos, Chunk, Raw));
	File[13] ^= 1;
	CHECK(!DecodeBlobHeader(File, Back));

	const std::string Hash = Sha("text");
	CHECK_EQ(BlobRelativePath(Hash), "objects/" + Hash.substr(0, 2) + "/" + Hash + ".z");
	std::string Parsed;
	CHECK(ParseBlobFileName(Hash + ".z", Parsed) && Parsed == Hash);
	CHECK(!ParseBlobFileName(Hash + ".z.tmp", Parsed));
	CHECK(!ParseBlobFileName("xyz.z", Parsed));
}

TEST(History_RetentionKeepsNewestAndYoungEntries)   // T-PER-010, RV-08
{
	Index I;
	// u1: an old entry, an older one, and its newest (also old): the newest stays whatever its age.
	I.Apply(MakeEntry(MakeTestEntry("u1-old", "u1", EntryKind::Save, Now - 40 * Day, "a")));
	I.Apply(MakeEntry(MakeTestEntry("u1-older", "u1", EntryKind::Save, Now - 50 * Day, "b")));
	I.Apply(MakeEntry(MakeTestEntry("u1-newest", "u1", EntryKind::Save, Now - 35 * Day, "c")));
	// u2: one inside the 30 days, one younger than 24 h, one three days in the future (counts as now), and an old one.
	I.Apply(MakeEntry(MakeTestEntry("u2-recent", "u2", EntryKind::Save, Now - 10 * Day, "d")));
	I.Apply(MakeEntry(MakeTestEntry("u2-young", "u2", EntryKind::Save, Now - 2 * 60 * 60 * 1000, "e")));
	I.Apply(MakeEntry(MakeTestEntry("u2-future", "u2", EntryKind::Save, Now + 3 * Day, "f")));
	I.Apply(MakeEntry(MakeTestEntry("u2-old", "u2", EntryKind::Run, Now - 31 * Day, "d")));   // shares d's blob

	std::vector<BlobInfo> Blobs;
	for (const char* T : {"a", "b", "c", "d", "e", "f"})
	{
		Blobs.push_back(BlobInfo{ Sha(T), 1000, Now - 60 * Day });
	}
	Blobs.push_back(BlobInfo{ Sha("orphan-old"), 1000, Now - 2 * Day });     // nothing references it: swept
	Blobs.push_back(BlobInfo{ Sha("orphan-young"), 1000, Now - 60 * 1000 });   // written a minute ago: survives
	Blobs.push_back(BlobInfo{ Sha("orphan-untimed"), 1000, 0 });               // no write time: never swept

	RetentionPlan Plan = PlanRetention(I, Blobs, Now, RetentionLimits());
	std::sort(Plan.Tombstones.begin(), Plan.Tombstones.end());
	CHECK((Plan.Tombstones == std::vector<std::string>{"u1-old", "u1-older", "u2-old"}));
	std::vector<std::string> Swept = Plan.Sweep;
	std::sort(Swept.begin(), Swept.end());
	std::vector<std::string> ExpectSwept = {Sha("a"), Sha("b"), Sha("orphan-old")};
	std::sort(ExpectSwept.begin(), ExpectSwept.end());
	CHECK(Swept == ExpectSwept);   // d stays: u2-recent still references it
	CHECK_EQ(Plan.BytesBefore, 9000);
	CHECK_EQ(Plan.BytesAfter, 6000);

	// The size limit: the oldest entries go first while the blobs exceed it, with the same two exceptions, so a limit
	// below what the protected entries need stops at them.
	RetentionLimits Small;
	Small.MaxBytes = 4500;
	Plan = PlanRetention(I, Blobs, Now, Small);
	std::sort(Plan.Tombstones.begin(), Plan.Tombstones.end());
	CHECK((Plan.Tombstones == std::vector<std::string>{"u1-old", "u1-older", "u2-old", "u2-recent"}));
	CHECK_EQ(Plan.BytesAfter, 5000);   // c, e, f (protected) + the two orphans that are too young or untimed
	RetentionLimits Tiny;
	Tiny.MaxBytes = 0;
	Plan = PlanRetention(I, Blobs, Now, Tiny);
	CHECK(std::find(Plan.Tombstones.begin(), Plan.Tombstones.end(), "u1-newest") == Plan.Tombstones.end());
	CHECK(std::find(Plan.Tombstones.begin(), Plan.Tombstones.end(), "u2-young") == Plan.Tombstones.end());
	CHECK(std::find(Plan.Tombstones.begin(), Plan.Tombstones.end(), "u2-future") == Plan.Tombstones.end());

	// A shorter day limit (a policy value from M4) applies the same way.
	RetentionLimits Week;
	Week.MaxAgeMs = 7 * Day;
	Plan = PlanRetention(I, Blobs, Now, Week);
	CHECK(std::find(Plan.Tombstones.begin(), Plan.Tombstones.end(), "u2-recent") != Plan.Tombstones.end());
}

TEST(History_IndexAndBlobReadersSurviveMutations)   // T-PER-005 shape for the history index and blobs (§10.4, SEC-06)
{
	MemOps Ops;
	Writer W(Ops, Dir, dream::journal::FileHeader(), IndexLogFormat);
	Index Live;
	Live.Apply(MakeEntry(MakeTestEntry("a1", "u1", EntryKind::Save, Now - 3 * Day, "print(1)\n")));
	Live.Apply(MakeEntry(MakeTestEntry("b1", "scratch", EntryKind::Closed, Now - Day, "x = 2")));
	REQUIRE(W.StartGeneration(1, Live.Checkpoint(), Now, 0.0));
	Entry Run = MakeTestEntry("c1", "u1", EntryKind::Run, Now, "print(1)\n");
	Run.Runs = {3, 4};
	REQUIRE(W.Commit(std::vector<Record>{MakeEntry(Run), MakeTombstone("b1")}));
	REQUIRE(W.Commit(std::vector<Record>{MakeClear(Now), MakeEntry(Run)}));
	const std::string Good = Ops.Files[IndexPath(1)];

	std::string GoodBlob;
	BlobHeader Header;
	Header.RawLength = 9;
	EncodeBlobHeader(Header, GoodBlob);
	AppendBlobChunk("compressed", 4, GoodBlob);
	AppendBlobChunk("more", 5, GoodBlob);

	uint64_t State = 0x9E3779B97F4A7C15ull;
	auto Below = [&State](uint64_t N) -> uint64_t
	{
		State ^= State << 13;
		State ^= State >> 7;
		State ^= State << 17;
		return N == 0 ? 0 : State % N;
	};
	auto Mutate = [&Below](std::string Bytes)
	{
		const int Mutations = 1 + static_cast<int>(Below(6));
		for (int M = 0; M < Mutations && !Bytes.empty(); ++M)
		{
			const size_t At = static_cast<size_t>(Below(Bytes.size()));
			switch (Below(3))
			{
			case 0: Bytes[At] = static_cast<char>(Below(256)); break;
			case 1: Bytes.erase(At, static_cast<size_t>(Below(8))); break;
			default: Bytes.insert(At, std::string(static_cast<size_t>(Below(8)), static_cast<char>(Below(256)))); break;
			}
		}
		return Bytes;
	};
	for (int K = 0; K < 3000; ++K)
	{
		Ops.Files[IndexPath(1)] = Mutate(Good);
		const RecoveredIndex R = ReadIndex(Ops, Dir, false);
		for (const Entry& E : R.Entries.Entries())
		{
			// Only usable entries survive: an id, and a blob that is a 64-hex hash (it becomes a file path, §10.4).
			CHECK(!E.Id.empty());
			CHECK_EQ(E.Blob.size(), static_cast<size_t>(64));
			CHECK(E.Blob.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos);
		}

		const std::string Blob = Mutate(GoodBlob);
		BlobHeader Back;
		if (DecodeBlobHeader(Blob, Back))
		{
			size_t Pos = BlobHeaderBytes;
			std::string_view Chunk;
			uint32_t Raw = 0;
			int Chunks = 0;
			while (NextBlobChunk(Blob, Pos, Chunk, Raw) && Chunks < 1000)
			{
				CHECK(Chunk.data() >= Blob.data() && Chunk.data() + Chunk.size() <= Blob.data() + Blob.size());
				++Chunks;
			}
			CHECK(Chunks < 1000);
		}
	}
}

TEST(History_UnreadableGenerationIsKeptAndTheSessionWritesNothingAboveIt)   // review r1 persist-M2, §10.4, §10.11
{
	MemOps Ops;
	{
		Writer W(Ops, Dir, dream::journal::FileHeader(), IndexLogFormat);
		Index Live;
		Live.Apply(MakeEntry(MakeTestEntry("a1", "u1", EntryKind::Save, Now - 3 * Day, "print(1)\n")));
		REQUIRE(W.StartGeneration(1, Live.Checkpoint(), Now, 0.0));
	}
	{
		Writer W(Ops, Dir, dream::journal::FileHeader(), IndexLogFormat);
		Index Live;
		Live.Apply(MakeEntry(MakeTestEntry("a1", "u1", EntryKind::Save, Now - 3 * Day, "print(1)\n")));
		REQUIRE(W.StartGeneration(2, Live.Checkpoint(), Now, 0.0));
		REQUIRE(W.Commit(std::vector<Record>{MakeEntry(MakeTestEntry("b1", "scratch", EntryKind::Closed, Now - 2 * Day, "only copy"))}));
	}

	// Generation 2 cannot be read: the reader falls back, says so, and leaves it exactly where it is (no .corrupt, no delete),
	// so the next session reads it again. The caller opens history read-only (bSkippedUnreadable).
	Ops.UnreadablePaths = { IndexPath(2) };
	RecoveredIndex R = ReadIndex(Ops, Dir, true);
	CHECK(R.bSkippedUnreadable);
	CHECK(R.bFallbackUsed);
	CHECK(!R.bUnreadable);
	CHECK_EQ(R.UsedGeneration, 1u);
	CHECK((Ids(R.Entries) == std::vector<std::string>{"a1"}));
	CHECK(Ops.Exists(IndexPath(2)));
	CHECK(!Ops.Exists(IndexPath(2) + ".corrupt"));
	CHECK(Ops.Exists(IndexPath(1)));

	// The lock is gone at the next session: every entry is back, the closed scratch text's entry included.
	Ops.UnreadablePaths.clear();
	R = ReadIndex(Ops, Dir, true);
	CHECK(!R.bSkippedUnreadable);
	CHECK(!R.bFallbackUsed);
	CHECK_EQ(R.UsedGeneration, 2u);
	CHECK((Ids(R.Entries) == std::vector<std::string>{"a1", "b1"}));
	CHECK(!Ops.Exists(IndexPath(1)));

	// No complete generation at all: the damaged one's entries are salvaged, so their blobs stay referenced.
	std::string& Bytes = Ops.Files[IndexPath(2)];
	Bytes.resize(Bytes.size() - 3);   // the last batch (b1) and nothing else torn: the checkpoint is complete
	R = ReadIndex(Ops, Dir, false);
	CHECK((Ids(R.Entries) == std::vector<std::string>{"a1"}));
	MemOps Torn;
	{
		Writer W(Torn, Dir, dream::journal::FileHeader(), IndexLogFormat);
		Index Live;
		Live.Apply(MakeEntry(MakeTestEntry("a1", "u1", EntryKind::Save, Now - 3 * Day, "print(1)\n")));
		Live.Apply(MakeEntry(MakeTestEntry("c1", "u2", EntryKind::Discarded, Now - 4 * Day, "discarded")));
		REQUIRE(W.StartGeneration(1, Live.Checkpoint(), Now, 0.0));
	}
	std::string& Only = Torn.Files[IndexPath(1)];
	Only[20] ^= 0x40;   // the header CRC: no complete generation, its frames still salvageable
	R = ReadIndex(Torn, Dir, true);
	CHECK(R.bUnreadable);
	CHECK(R.bSalvageUsed);
	CHECK((Ids(R.Entries) == std::vector<std::string>{"a1", "c1"}));
	CHECK(R.Entries.References(Sha("discarded")));
	CHECK(Torn.Exists(IndexPath(1) + ".corrupt"));   // set aside, never deleted
}

TEST(History_IndexIsLinearAndAgeNeverOverflows)   // review r1 core-formats minors: O(n^2) Apply/tombstones, AgeMs overflow
{
	// Replace in place, tombstone, re-add, References by blob count, holes compacted in order.
	Index I;
	I.Apply(MakeEntry(MakeTestEntry("a", "u1", EntryKind::Save, Now - 3, "x")));
	I.Apply(MakeEntry(MakeTestEntry("b", "u1", EntryKind::Save, Now - 2, "y")));
	I.Apply(MakeEntry(MakeTestEntry("c", "u2", EntryKind::Save, Now - 1, "x")));
	CHECK(I.References(Sha("x")) && I.References(Sha("y")));
	I.Apply(MakeEntry(MakeTestEntry("b", "u1", EntryKind::Save, Now - 2, "z")));   // the same id: replaced in place
	CHECK(!I.References(Sha("y")) && I.References(Sha("z")));
	CHECK((Ids(I) == std::vector<std::string>{"a", "b", "c"}));
	I.Apply(MakeTombstone("a"));
	CHECK(I.Find("a") == nullptr);
	CHECK(I.References(Sha("x")));   // c still names it
	CHECK(I.NewestOf("u1") != nullptr && I.NewestOf("u1")->Id == "b");
	CHECK_EQ(I.Checkpoint().size(), 2u);
	I.Apply(MakeEntry(MakeTestEntry("a", "u1", EntryKind::Save, Now, "x")));   // re-added: at the end
	CHECK((Ids(I) == std::vector<std::string>{"b", "c", "a"}));
	CHECK(I.Find("c") != nullptr && I.Find("c")->Uuid == "u2");
	I.Apply(MakeClear(Now));
	CHECK(I.Entries().empty() && !I.References(Sha("x")) && I.Find("b") == nullptr);

	// 60,000 entries and 59,990 tombstones (one record each, as the index replays them): linear work.
	Index Big;
	const int Count = 60000;
	for (int K = 0; K < Count; ++K)
	{
		Big.Apply(MakeEntry(MakeTestEntry("id" + std::to_string(K), "u" + std::to_string(K % 7), EntryKind::Save, Now - K,
			"text " + std::to_string(K % 100))));
	}
	for (int K = 10; K < Count; ++K)
	{
		Big.Apply(MakeTombstone("id" + std::to_string(K)));
	}
	REQUIRE_EQ(Big.Entries().size(), 10u);
	CHECK_EQ(Big.Entries()[9].Id, std::string("id9"));
	CHECK(Big.Find("id9") != nullptr && Big.Find("id10") == nullptr);
	CHECK(Big.References(Sha("text 9")) && !Big.References(Sha("text 10")));

	// A corrupt utc never overflows the age arithmetic: INT64_MIN is very old (tombstoned), INT64_MAX is the future (kept).
	Index Ages;
	Ages.Apply(MakeEntry(MakeTestEntry("ancient", "u1", EntryKind::Save, std::numeric_limits<int64_t>::min(), "old")));
	Ages.Apply(MakeEntry(MakeTestEntry("future", "u1", EntryKind::Save, std::numeric_limits<int64_t>::max(), "new")));
	Ages.Apply(MakeEntry(MakeTestEntry("now", "u2", EntryKind::Save, Now, "now")));
	const RetentionPlan Plan = PlanRetention(Ages, {}, Now, RetentionLimits());
	CHECK((Plan.Tombstones == std::vector<std::string>{"ancient"}));
	const RetentionPlan AtMin = PlanRetention(Ages, {}, std::numeric_limits<int64_t>::min(), RetentionLimits());
	CHECK(AtMin.Tombstones.empty());
}

TEST(History_RunEntriesMergeByTimeNotByArrival)   // T-PER-017: a reload entry indexed before the runs it follows
{
	// Run #1's entry is indexed; then a reload entry of the same buffer (queued at once by its AddEntry, at time 400) lands
	// before the entries of runs #2 and #3, whose pre-run commits were still in flight (their notes are times 200 and 300).
	Index I;
	auto Add = [&I](Entry E) { I.Apply(MakeEntry(PlanRunEntry(I, std::move(E)))); };
	Entry Run1 = MakeTestEntry("r1", "u1", EntryKind::Run, Now + 100, "same text");
	Run1.Runs = {1};
	Add(Run1);
	Add(MakeTestEntry("reload", "u1", EntryKind::Reload, Now + 400, "same text"));
	for (uint32_t Number = 2; Number <= 3; ++Number)
	{
		Entry Run = MakeTestEntry("r" + std::to_string(Number), "u1", EntryKind::Run, Now + 100 * Number, "same text");
		Run.Runs = {Number};
		Add(Run);
	}
	REQUIRE_EQ(I.Entries().size(), 2u);   // one run entry and the reload: the three runs were consecutive in time
	const Entry* Runs = I.Find("r1");
	REQUIRE(Runs != nullptr);
	CHECK((Runs->Runs == std::vector<uint32_t>{1, 2, 3}));
	CHECK_EQ(Runs->UtcMs, Now + 300);

	// A run after the reload is not consecutive with the earlier runs: its own entry, even with the same text.
	Entry Run4 = MakeTestEntry("r4", "u1", EntryKind::Run, Now + 500, "same text");
	Run4.Runs = {4};
	Add(Run4);
	CHECK_EQ(I.Entries().size(), 3u);
	CHECK((I.Find("r4")->Runs == std::vector<uint32_t>{4}));
	// The next one with the same text merges into #4, and another buffer's run never joins.
	Entry Run5 = MakeTestEntry("r5", "u1", EntryKind::Run, Now + 600, "same text");
	Run5.Runs = {5};
	Add(Run5);
	CHECK((I.Find("r4")->Runs == std::vector<uint32_t>{4, 5}));
	Entry Other = MakeTestEntry("o1", "u2", EntryKind::Run, Now + 700, "same text");
	Other.Runs = {6};
	Add(Other);
	CHECK_EQ(I.Entries().size(), 4u);
}
