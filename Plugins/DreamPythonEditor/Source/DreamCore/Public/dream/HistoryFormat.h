// SPDX-License-Identifier: Apache-2.0
// Dream core: the local-history format (TDD §10.4, PER-04): the index records (ENTRY, TOMBSTONE, CLEAR) in the journal's
// framing and generation protocol (index.<g>.shi, dream::journal::LogFormat), the index model and its reader with fallback
// and salvage, run-entry merging, the SUMIBLOB blob header and chunk framing, and the retention planner (30 days and
// 500 MB, each buffer's newest entry and every entry under 24 h kept, RV-08). Data only (SEC-06); no I/O or compression
// of its own: DreamEditor's FDreamLocalHistory compresses each chunk with zlib and does the file work on the journal writer
// thread through dream::journal::FileOps.
#pragma once

#include "dream/JournalFormat.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dream::history
{
	inline constexpr uint16_t IndexMajor = 1;
	inline constexpr uint16_t IndexMinor = 0;

	/** Index record codes (the history's own range; HEADER_EXT and the checkpoint markers are the journal's). */
	enum class RecordType : uint16_t
	{
		Entry     = 0x50,
		Tombstone = 0x51,
		Clear     = 0x52,
	};

	inline constexpr journal::LogFormat IndexLogFormat{ "SUMIHIDX", IndexMajor, IndexMinor, "index.", ".shi",
		static_cast<uint16_t>(RecordType::Entry), "entries" };

	/** The entry kinds of §10.4. */
	enum class EntryKind : uint8_t
	{
		Save,
		Run,
		Reload,
		Closed,
		Recovered,
		Discarded,
		Restore,
	};

	DREAMCORE_API const char* KindName(EntryKind Kind);
	DREAMCORE_API bool ParseKind(std::string_view Name, EntryKind& Out);

	/** One index entry: {"id", "uuid", "path", "display", "kind", "runs", "utc", "blob", "size", "lines"} (§10.4). */
	struct Entry
	{
		std::string Id;                   // 32 lower-case hex digits, unique across instances (merged by id, §10.5)
		std::string Uuid;                 // the buffer
		std::string Path;                 // UTF-8, forward slashes; empty for scratch
		std::string Display;
		EntryKind Kind = EntryKind::Save;
		std::vector<uint32_t> Runs;       // run numbers of a "run" entry ("Before runs #14-#16, same text")
		int64_t UtcMs = 0;
		std::string Blob;                 // SHA-256 of the UTF-8 text, 64 hex digits
		int64_t Size = 0;                 // UTF-8 bytes of the text
		int64_t Lines = 0;
	};

	DREAMCORE_API journal::Record MakeEntry(const Entry& E);
	DREAMCORE_API journal::Record MakeTombstone(std::string_view Id);
	DREAMCORE_API journal::Record MakeClear(int64_t UtcMs);
	/** False when the head is not a usable entry (no id or blob). */
	DREAMCORE_API bool EntryFromHead(const json::Value& Head, Entry& Out);

	/**
	 * The live entries in the order they were first added; an ENTRY with a known id replaces that entry in place. Apply, Find
	 * and References are O(1) (an id map and a blob reference count); a TOMBSTONE leaves a hole that the next Entries() or a
	 * later Apply compacts, so reading n records or committing n tombstones is O(n), not O(n^2).
	 */
	class DREAMCORE_API Index
	{
	public:
		/** ENTRY, TOMBSTONE and CLEAR change the index; every other record is ignored. */
		void Apply(const journal::Record& R);
		const std::vector<Entry>& Entries() const;
		const Entry* Find(std::string_view Id) const;
		/** The newest entry of a buffer (by time, then by order), or null. */
		const Entry* NewestOf(std::string_view Uuid) const;
		bool References(std::string_view Blob) const;
		/** One ENTRY per live entry: the body of a new index generation. */
		std::vector<journal::Record> Checkpoint() const;
		void Clear();

	private:
		void Compact() const;

		mutable std::vector<Entry> List;                                // a tombstoned entry keeps an empty Id until compacted
		mutable std::unordered_map<std::string, size_t> Positions;      // id -> its place in List
		std::unordered_map<std::string, size_t> BlobRefs;               // blob -> live entries that name it
		mutable size_t Holes = 0;
	};

	/**
	 * §10.4 run-entry merging: when the buffer's entry just before New in time (the latest with utc <= New's; a tie goes to
	 * the later one in the index) is a "run" entry with the same blob, the new run joins its runs list (the same id, so the
	 * ENTRY written replaces it); otherwise New is returned as it is. "Consecutive" is decided by time, not by arrival: a run's
	 * entry waits for its pre-run commit while a reload, restore or discarded entry of the same buffer is queued at once, so
	 * a later entry can be indexed first (the editor gives its notes strictly increasing times for this).
	 */
	DREAMCORE_API Entry PlanRunEntry(const Index& I, Entry New);

	struct RecoveredIndex
	{
		bool bFound = false;
		bool bUnreadable = false;
		bool bNewerFormat = false;        // a newer major or minor: read only, never written (§10.6)
		bool bFallbackUsed = false;
		bool bSalvageUsed = false;
		/** persist-M2: a listed generation could not be read even after the FileOps retries (§10.11, a scanner's lock). It is
		 *  left in place (never set aside or deleted), and the caller must not write above it: the session's history is read
		 *  only, so the next session reads it again. */
		bool bSkippedUnreadable = false;
		uint64_t Generation = 0;          // the newest generation file name seen
		uint64_t UsedGeneration = 0;      // the generation whose entries were applied (0: none)
		Index Entries;
	};

	/**
	 * Reads <root>/history/ (§10.4 "crash-safe with fallback"): newest index generation first, complete batches only,
	 * salvage after a bad frame, fallback to the previous generation with the damaged one's ENTRY records on top. With
	 * bMayModify (and not a newer format) it removes stale *.tmp, sets a damaged generation aside as .corrupt (touched first)
	 * and deletes this format's generations older than the one read.
	 */
	DREAMCORE_API RecoveredIndex ReadIndex(journal::FileOps& Ops, std::string_view Dir, bool bMayModify = true);

	// ---- Blobs: <root>/history/objects/<aa>/<sha256>.z -----------------------------------------------------------

	inline constexpr uint16_t BlobVersion = 1;
	inline constexpr size_t BlobHeaderBytes = 32;
	/** Raw bytes per zlib chunk: one chunk is compressed in one slice of the writer's background work (§10.2.1). */
	inline constexpr uint32_t BlobChunkBytes = 1u << 20;

	/** {"SUMIBLOB", version, raw length, CRC32C of the raw bytes, chunk size} plus its own CRC (32 bytes, little-endian). */
	struct BlobHeader
	{
		uint16_t Version = BlobVersion;
		uint64_t RawLength = 0;
		uint32_t RawCrc32c = 0;
		uint32_t ChunkBytes = BlobChunkBytes;
	};

	DREAMCORE_API void EncodeBlobHeader(const BlobHeader& Header, std::string& Out);
	DREAMCORE_API bool DecodeBlobHeader(std::string_view Bytes, BlobHeader& Out);
	/** Appends one chunk: u32 compressed length, u32 raw length, the compressed bytes. */
	DREAMCORE_API void AppendBlobChunk(std::string_view Compressed, uint32_t RawLength, std::string& Out);
	/** The chunk at InOutPos (start at BlobHeaderBytes); false at the end or on a bad length. */
	DREAMCORE_API bool NextBlobChunk(std::string_view File, size_t& InOutPos, std::string_view& OutCompressed, uint32_t& OutRawLength);
	/** "objects/<aa>/<sha256>.z" for a 64-hex SHA-256; ParseBlobFileName reads "<sha256>.z" back. */
	DREAMCORE_API std::string BlobRelativePath(std::string_view Sha256Hex);
	DREAMCORE_API bool ParseBlobFileName(std::string_view Name, std::string& OutSha256Hex);

	// ---- Retention (§10.4, D17, PER-04, RV-08) --------------------------------------------------------------------

	inline constexpr int64_t DayMs = 24ll * 60 * 60 * 1000;

	struct RetentionLimits
	{
		int64_t MaxAgeMs = 30 * DayMs;
		int64_t MaxBytes = 500ll * 1024 * 1024;
	};

	struct BlobInfo
	{
		std::string Sha256Hex;
		int64_t Bytes = 0;                // on disk
		int64_t WrittenUtcMs = 0;         // its last write time; 0 when unknown (treated as young: never swept)
	};

	struct RetentionPlan
	{
		std::vector<std::string> Tombstones;   // entry ids, oldest first
		std::vector<std::string> Sweep;        // blob hashes no live entry references and older than 24 h
		int64_t BytesBefore = 0;
		int64_t BytesAfter = 0;
	};

	/**
	 * Steps 1-3 of §10.4's retention: tombstone entries older than the day limit, then the oldest while the blobs exceed the
	 * size limit, never a buffer's newest entry or one younger than 24 h (age = max(0, now - utc); a time more than a day in
	 * the future counts as now); then sweep the unreferenced blobs, never one younger than 24 h (a blob written just before
	 * its index record, or staged by a secondary instance, survives). Step 4, the index rotation, is the caller's.
	 */
	DREAMCORE_API RetentionPlan PlanRetention(const Index& I, std::span<const BlobInfo> Blobs, int64_t NowUtcMs, const RetentionLimits& Limits);
}
