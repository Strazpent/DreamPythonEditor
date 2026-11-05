// SPDX-License-Identifier: Apache-2.0
// Dream core: the write-ahead journal format (TDD §10.2): header, SJRF frames with CRC32C, record heads, the generation
// protocol, the reader with fallback and salvage, the commit policy, and the batch planner of the writer thread. No engine
// types; the G6 host links it (§10.10). The local-history index (§10.4, dream/HistoryFormat.h) shares the framing and the
// generation protocol through LogFormat.
// Threads: Writer, BatchPlanner and ReadNamespace run on the journal writer thread (DreamJournalWriter); CommitPolicy on the
// game thread.
// (slice A choice) DreamCore does no I/O itself (§2.1): FileOps is an interface here; DreamEditor implements it for Win64
// in Private/Persistence (MoveFileExW write-through, ReplaceFileW, FlushFileBuffers) and tier 1 uses an in-memory one.
#pragma once

#include "dream/Json.h"
#include "dream/TextBuffer.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::journal
{
	inline constexpr uint16_t FormatMajor = 1;
	inline constexpr uint16_t FormatMinor = 0;
	inline constexpr size_t HeaderBytes = 64;
	inline constexpr uint32_t MaxPayloadBytes = 16u << 20;   // larger texts become SNAP parts in one batch
	inline constexpr uint64_t RotateBytes = 16ull << 20;     // proposed, §10.2.3
	inline constexpr double RotateSeconds = 600.0;           // proposed: 10 minutes after the last checkpoint
	inline constexpr int64_t CorruptRetentionMs = 30ll * 24 * 60 * 60 * 1000;   // wal.<g>.sjl.corrupt kept 30 days (proposed, §10.2.4)
	/** An older major's generation renamed wal.<g>.sjl.v<major> is kept as long (proposed, §10.6). */
	inline constexpr int64_t MigratedRetentionMs = CorruptRetentionMs;
	/** Each slice of a checkpoint writes at most this much to the .tmp before its flush (proposed, §10.2.3 step 1). */
	inline constexpr size_t RotationSliceBytes = 4u << 20;

	/** Record codes (§10.2.2). */
	enum class RecordType : uint16_t
	{
		HeaderExt        = 0x01,
		CheckpointBegin  = 0x02,
		CheckpointEnd    = 0x03,
		BufOpen          = 0x10,
		Snap             = 0x11,
		Delta            = 0x12,
		Meta             = 0x13,
		BufClose         = 0x14,
		RecoveryPending  = 0x15,
		/**
		 * (slice B choice) A scratch buffer's clean text, the text it had when this session restored it (§10.7.1: a counted
		 * scratch buffer shows its clean text while its recovery is pending). Head {uuid, text_crc32c, same}; "same" means the
		 * text equals the buffer's state just before this record (the checkpoint's SNAP), else the body holds it. Written in
		 * every checkpoint and when the clean text is set. Skippable: an older reader ignores it.
		 */
		CleanText        = 0x16,
		Session          = 0x20,
		RunBegin         = 0x30,
		RunEnd           = 0x31,
		Pause            = 0x32,
		Resume           = 0x33,
		RecoveryResolved = 0x40,
		Clean            = 0x7F,
	};

	enum FrameFlag : uint16_t
	{
		FF_Skippable = 1 << 0,
		FF_Commit    = 1 << 1,
	};

	/** The 64-byte little-endian file header (§10.2.2). */
	struct FileHeader
	{
		uint16_t Major = FormatMajor;
		uint16_t Minor = FormatMinor;
		uint64_t Generation = 0;
		uint64_t CreatedUtcMs = 0;     // display only
		uint32_t InstancePid = 0;
		uint64_t InstanceStart = 0;    // OS process creation time
		uint8_t TextUnit = 1;          // 1 = UTF-8
	};

	/** One record: a JSON head and a raw body (UTF-8 text for SNAP, inserted bytes for DELTA). */
	struct Record
	{
		RecordType Type = RecordType::Session;
		uint16_t Flags = 0;            // FF_Skippable; FF_Commit is set by the writer on a batch's last frame
		json::Value Head;
		std::string Body;
	};

	/**
	 * A family of generation files that share the journal's 64-byte header layout, SJRF framing and generation protocol
	 * (§10.2.2-§10.2.4): the journal's wal.<g>.sjl and the local-history index's index.<g>.shi (§10.4).
	 */
	struct LogFormat
	{
		std::string_view Magic;        // 8 bytes
		uint16_t Major = 0;
		uint16_t Minor = 0;
		std::string_view Prefix;       // "wal."
		std::string_view Suffix;       // ".sjl"
		uint16_t CountedType = 0;      // the record code CHECKPOINT_BEGIN counts
		std::string_view CountKey;     // its head key
	};

	inline constexpr LogFormat JournalLogFormat{ "SUMIJRNL", FormatMajor, FormatMinor, "wal.", ".sjl",
		static_cast<uint16_t>(RecordType::BufOpen), "buffers" };

	// ---- Codec (pure) ----------------------------------------------------------------------------------------

	DREAMCORE_API void EncodeHeader(const FileHeader& Header, std::string& Out);
	/** False on a bad magic, size or header CRC. A newer Major still decodes (the reader then opens read-only). */
	DREAMCORE_API bool DecodeHeader(std::string_view Bytes, FileHeader& Out);
	/** The same for another log family (its magic). */
	DREAMCORE_API void EncodeHeader(const FileHeader& Header, std::string& Out, const LogFormat& Format);
	DREAMCORE_API bool DecodeHeader(std::string_view Bytes, FileHeader& Out, const LogFormat& Format);

	/** Appends one frame: "SJRF", payload_len, type, flags, seq, payload (u32-prefixed JSON head + body), CRC32C. */
	DREAMCORE_API void EncodeFrame(const Record& Rec, uint64_t Seq, bool bCommit, std::string& Out);

	enum class FrameStatus : uint8_t
	{
		Ok,
		End,                           // InOutPos is at the end of Bytes
		Truncated,                     // a torn tail
		BadSync,
		BadLength,
		BadCrc,
		BadHead,                       // the JSON head does not parse
	};

	/** Decodes the frame at InOutPos and advances it on Ok. OutCommit reports the commit flag. */
	DREAMCORE_API FrameStatus DecodeFrame(std::string_view Bytes, size_t& InOutPos, Record& Out, uint64_t& OutSeq, bool& OutCommit);

	/** Salvage: the position of the next "SJRF" at or after From whose frame has a valid CRC; npos when none. */
	DREAMCORE_API size_t FindNextValidFrame(std::string_view Bytes, size_t From);

	/** "wal.0000000000000007.sjl" (16 hex digits) and its parse. */
	DREAMCORE_API std::string GenerationFileName(uint64_t Generation);
	DREAMCORE_API bool ParseGenerationFileName(std::string_view Name, uint64_t& OutGeneration);
	/** The same for any log family: Prefix, 16 hex digits, Suffix. */
	DREAMCORE_API std::string LogFileName(const LogFormat& Format, uint64_t Generation);
	DREAMCORE_API bool ParseLogFileName(const LogFormat& Format, std::string_view Name, uint64_t& OutGeneration);
	/** §10.6: an older major's generation kept for a downgrade, "wal.<g>.sjl.v<major>"; ParseMigratedFileName reads it back. */
	DREAMCORE_API std::string MigratedFileName(std::string_view GenerationName, uint16_t Major);
	DREAMCORE_API bool ParseMigratedFileName(const LogFormat& Format, std::string_view Name, uint64_t& OutGeneration, uint16_t& OutMajor);

	// ---- Record heads (§10.2.2) ---------------------------------------------------------------------------------

	/** A file's size, mtime and SHA-256 when its buffer was loaded or last saved ("base", §10.2.2, §10.14). */
	struct BaseVersion
	{
		int64_t Size = 0;
		int64_t MtimeUtcMs = 0;
		std::string Sha256Hex;
	};

	struct BufferOpenInfo
	{
		std::string Uuid;              // FGuid as 32 lower-case hex digits (slice A choice)
		std::string Kind;              // "scratch", "file", "recovered"
		std::string Path;              // UTF-8, forward slashes; empty for scratch
		std::string Display;           // "Untitled-3", "rig_fix.py"
		std::string Encoding;          // codec::EncodingName
		bool bBom = false;
		std::string Eol;               // "lf", "crlf", "cr"
		std::optional<BaseVersion> Base;
		bool bReadOnly = false;
	};

	/** A pending recovery (§10.7.1): the RECOVERY_PENDING head, also written as the SNAP's "pending" field. */
	struct PendingInfo
	{
		std::string SourceNamespace;
		uint64_t SourceGeneration = 0;
		std::string Uuid;
		std::string Kind;
		std::string Path;
		std::optional<BaseVersion> Base;
		std::string Eol;
	};

	/** UTF-8 byte offsets into the buffer's UTF-8 text before the edit (text_unit 1). */
	struct DeltaOp
	{
		int64_t Offset = 0;
		int64_t Removed = 0;
		int64_t InsertedLen = 0;
	};

	struct RunBeginInfo
	{
		uint64_t RunId = 0;            // FDreamRunId (§12.1.2)
		uint32_t Number = 0;
		std::string TabUuid;
		uint64_t BufferVersion = 0;
		std::string Route;             // "selection", "line", "buffer", "repl", "console", "run_file", "api"
		bool bDebug = false;
		int64_t UtcMs = 0;
	};

	DREAMCORE_API Record MakeHeaderExt(const json::Value& Fields);          // engine and plugin versions, project hash, namespace, previous generation, host
	DREAMCORE_API Record MakeCheckpointBegin(uint32_t BufferCount);
	DREAMCORE_API Record MakeCheckpointEnd();
	DREAMCORE_API Record MakeBufOpen(const BufferOpenInfo& Info);
	/** One or more SNAP parts of at most MaxPayloadBytes; text_crc32c over the whole text on every part. */
	DREAMCORE_API std::vector<Record> MakeSnap(std::string_view Uuid, uint64_t Version, std::string_view TextUtf8, const PendingInfo* Pending = nullptr);
	DREAMCORE_API Record MakeDelta(std::string_view Uuid, uint64_t FromVersion, uint64_t ToVersion, std::span<const DeltaOp> Ops,
		std::string_view InsertedBytes, uint32_t ResultTextCrc32c);
	DREAMCORE_API Record MakeMeta(std::string_view Uuid, bool bDirty, const std::optional<BaseVersion>& Base, std::string_view Path, std::string_view Display);
	DREAMCORE_API Record MakeBufClose(std::string_view Uuid, std::string_view Reason);      // "closed", "discarded", "restored"
	DREAMCORE_API Record MakeRecoveryPending(const PendingInfo& Info);
	/** CLEAN_TEXT (skippable): bSameAsLive writes no body (the text is the buffer's state just before this record). */
	DREAMCORE_API Record MakeCleanText(std::string_view Uuid, std::string_view CleanTextUtf8, bool bSameAsLive);
	DREAMCORE_API Record MakeRecoveryResolved(std::string_view SourceNamespace, uint64_t SourceGeneration, std::string_view Uuid, std::string_view Action);
	DREAMCORE_API Record MakeSession(json::Value SessionState);               // tabs, order, active, per-tab view state (§10.2.2)
	DREAMCORE_API Record MakeRunBegin(const RunBeginInfo& Info);
	DREAMCORE_API Record MakeRunEnd(uint64_t RunId, std::string_view Outcome); // "ok", "error", "cancelled", "not executed"
	DREAMCORE_API Record MakeClean(int64_t UtcMs, std::span<const std::pair<std::string, uint64_t>> CleanVersions);

	// ---- File operations (implemented by the host) -----------------------------------------------------------

	/** Paths are UTF-8 with forward slashes. Every call returns false on failure and LastError() says why. */
	class FileOps
	{
	public:
		using Handle = uint64_t;
		static constexpr Handle InvalidHandle = 0;

		virtual ~FileOps() = default;
		virtual bool CreateExclusive(std::string_view Path, Handle& Out) = 0;   // fails when the file exists
		virtual bool OpenAppend(std::string_view Path, Handle& Out) = 0;
		virtual bool Append(Handle H, std::string_view Bytes) = 0;
		virtual bool Flush(Handle H) = 0;                                       // FlushFileBuffers / fsync
		virtual void Close(Handle H) = 0;
		virtual bool ReadAll(std::string_view Path, std::string& Out) = 0;
		virtual bool RenameNew(std::string_view From, std::string_view To) = 0;  // write-through rename (§10.2.3 step 3)
		virtual bool ReplaceAtomic(std::string_view Target, std::string_view Temp, std::string_view Backup) = 0; // small JSON files
		virtual bool Remove(std::string_view Path) = 0;
		virtual bool List(std::string_view Dir, std::vector<std::string>& OutNames) = 0; // file names, no directories
		virtual bool MakeDirs(std::string_view Dir) = 0;
		virtual bool FlushDir(std::string_view Dir) = 0;                        // no-op on Windows
		virtual bool Exists(std::string_view Path) = 0;
		virtual std::string LastError() const = 0;

		// Optional, with correct but costlier defaults: ReadPrefix reads the whole file, without Truncate the writer needs a
		// new checkpoint after a failed commit, and without ModifiedUtcMs a .corrupt file is never deleted. Touch has no
		// safe default: without it a damaged generation is dated by its last write, which a rename keeps (§10.2.4).

		/** Cuts an open file back to Size bytes (a failed commit's bytes). The default cannot: the writer then asks for a new
		 *  checkpoint instead (Writer::NeedsCheckpoint). */
		virtual bool Truncate(Handle /*H*/, uint64_t /*Size*/)
		{
			return false;
		}

		/** At most MaxBytes from the start of a file (generation headers). The default reads the whole file. */
		virtual bool ReadPrefix(std::string_view Path, size_t MaxBytes, std::string& Out)
		{
			if (!ReadAll(Path, Out))
			{
				return false;
			}
			if (Out.size() > MaxBytes)
			{
				Out.resize(MaxBytes);
			}
			return true;
		}

		/** The file's last write time in UTC milliseconds. The default does not know (a .corrupt file is then kept). */
		virtual bool ModifiedUtcMs(std::string_view /*Path*/, int64_t& /*OutUtcMs*/)
		{
			return false;
		}

		/** Sets the file's last write time to now. The reader touches a damaged generation just before it renames it
		 *  .corrupt, so its 30 days count from then (§10.2.4). The default cannot. */
		virtual bool Touch(std::string_view /*Path*/)
		{
			return false;
		}
	};

	DREAMCORE_API std::string JoinPath(std::string_view Dir, std::string_view Name);

	// ---- Writer (journal writer thread) ----------------------------------------------------------------------

	enum class RotationStep : uint8_t
	{
		More,                          // call StepRotation again (after the due commits)
		Ready,                         // the checkpoint is on disk: FinishRotation
		Failed,                        // abandoned (the .tmp removed); the current generation goes on
	};

	/**
	 * Appends batches to the current generation of one namespace directory and rotates (§10.2.3).
	 * Rotation is sliced (§10.2.1, §10.2.3 steps 1-2): BeginRotation starts wal.<g+1>.sjl.tmp, each StepRotation is one
	 * unit of background work (a write of at most RotationSliceBytes, or the flush that follows it), and commits keep
	 * appending to g between them; the batches committed meanwhile are kept and FinishRotation appends them to the .tmp
	 * (the catch-up), flushes, renames, switches to g+1 and then looks at g-1's header before it deletes it.
	 * Rotate does all of it in one call. The session's first generation (StartGeneration) is written in one go: every commit
	 * waits for it anyway (§10.2.3).
	 * The successful rename is the switch (persist-M4): from then on nothing is appended to g, even when opening g+1 for
	 * append fails (a scanner holding the new file). The generation, seq and size move to g+1 with no handle open; the next
	 * Commit opens it again first, and while that fails each Commit fails (the caller's retry queue) and NeedsRotation is true,
	 * so the next rotation goes past the existing g+1.
	 */
	class DREAMCORE_API Writer
	{
	public:
		Writer(FileOps& InOps, std::string InDir, FileHeader InProto);   // Proto supplies pid and instance start
		/** Another log family (the history index, §10.4). Format must outlive the writer. */
		Writer(FileOps& InOps, std::string InDir, FileHeader InProto, const LogFormat& InFormat);
		~Writer();
		Writer(const Writer&) = delete;
		Writer& operator=(const Writer&) = delete;

		/**
		 * The HEADER_EXT fields (engine and plugin versions, project hash, namespace, host) written first in every
		 * generation; the writer adds "previous_generation". A HeaderExt record inside a CheckpointBody is used instead
		 * when present (it is never written twice).
		 */
		void SetHeaderExtFields(json::Value Fields);

		/**
		 * Starts this session's own generation (Generation > every existing one) with a checkpoint: header, HEADER_EXT,
		 * CHECKPOINT_BEGIN, the given records, CHECKPOINT_END with commit, then flush (§10.2.3). Then deletes the
		 * wal.<g>.sjl.corrupt files whose write time (when the reader set them aside) is CorruptRetentionMs or more before
		 * NowUtcMs (§10.2.4; nothing when NowUtcMs is 0; a file whose write time is unknown or in the future is kept).
		 */
		bool StartGeneration(uint64_t Generation, std::span<const Record> CheckpointBody, int64_t NowUtcMs, double NowSec);

		/**
		 * Appends a batch, sets FF_Commit on the last frame, flushes. True means the batch is durable. A false return means
		 * the caller must treat the batch as not written (its next DELTA starts from the last durable version), so the
		 * writer makes the file agree: the batch's seqs are spent either way, and its bytes are cut back off the file
		 * (FileOps::Truncate). When they cannot be, NeedsCheckpoint() turns true until the next Rotate (§10.11).
		 */
		bool Commit(std::span<const Record> Batch);

		/** The size, age, or NeedsCheckpoint() rule of §10.2.3; false while a rotation is in progress. */
		bool NeedsRotation(double NowSec) const;
		/**
		 * True after a failed commit whose bytes may still be in the file: a reader could apply that batch, then skip every
		 * later DELTA based on the version before it. Only a new checkpoint (Rotate) clears it; until then the caller writes
		 * SNAPs, not DELTAs.
		 */
		bool NeedsCheckpoint() const;
		/** Steps 1-5 of §10.2.3 in one call: BeginRotation, every StepRotation, FinishRotation. */
		bool Rotate(std::span<const Record> CheckpointBody, int64_t NowUtcMs, double NowSec);

		/** Step 1 begins: creates wal.<g+1>.sjl.tmp and queues its header, HEADER_EXT and the checkpoint. False on failure. */
		bool BeginRotation(std::vector<Record> CheckpointBody, int64_t NowUtcMs);
		bool IsRotating() const;
		/** One unit of the checkpoint: a write of at most MaxBytes to the .tmp, or the flush after it. */
		RotationStep StepRotation(size_t MaxBytes = RotationSliceBytes);
		/**
		 * Steps 2-5 once StepRotation said Ready: appends the batches committed to g since BeginRotation (the catch-up),
		 * flushes and closes the .tmp, renames it with write-through, appends to g+1 from then on, and handles g-1 by its
		 * header: this format's is deleted, an older major's is renamed wal.<g-1>.sjl.v<major> (§10.6), a newer format's is
		 * never touched, and one whose header does not decode is set aside as .corrupt. False when nothing was renamed
		 * (appends stay on g), or when the rename succeeded but g+1 could not be opened: the rename is still the switch
		 * (Generation() is g+1, nothing goes to g again, and the next Commit opens g+1 first, persist-M4).
		 */
		bool FinishRotation(double NowSec);
		/** Drops a rotation in progress (its .tmp removed). */
		void AbandonRotation();
		/** Bytes the checkpoint in progress has written to its .tmp, and the catch-up batches it holds (tests). */
		uint64_t RotationBytesWritten() const;
		size_t RotationCatchUpBatches() const;

		uint64_t Generation() const;
		uint64_t FileBytes() const;
		uint64_t LastSeq() const;
		const std::string& LastError() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};

	// ---- Reader (journal writer thread, or the cross-root scan worker with bMayModify false) ------------------

	struct RecoveredBuffer
	{
		BufferOpenInfo Open;
		std::string TextUtf8;
		uint64_t Version = 0;
		bool bHasText = false;
		bool bVerified = false;        // text_crc32c matched; otherwise the last verified state is returned
		bool bDirty = false;           // from the last META
		bool bClosed = false;
		std::string CloseReason;
		/** CLEAN_TEXT: the clean text of a scratch buffer this session's predecessor restored (§10.7.1); none for a buffer
		 *  created in the session read. */
		std::optional<std::string> CleanTextUtf8;
	};

	struct RecoveredPending
	{
		PendingInfo Info;
		std::string TextUtf8;
	};

	struct RecoveredRun
	{
		RunBeginInfo Begin;
		bool bPaused = false;          // a PAUSE without RESUME
	};

	struct RecoveredNamespace
	{
		bool bFound = false;           // any generation file existed
		bool bUnreadable = false;      // no usable generation
		bool bNewerFormat = false;     // header Major above FormatMajor: open read-only (§10.6)
		bool bNewerMinor = false;      // the newest decodable header, or the one read, is a newer minor: read-only (§10.6)
		bool bFallbackUsed = false;    // the previous generation was used (§10.2.4 step 4)
		bool bSalvageUsed = false;
		bool bClean = false;           // the last batch is CLEAN
		uint64_t Generation = 0;       // newest generation file listed
		/** The generation whose state was applied (0: none). Every listed generation above it was unusable: damaged, or not
		 *  readable even after the FileOps retries (persist-M2); each counts as damaged (set aside as .corrupt, never deleted). */
		uint64_t UsedGeneration = 0;
		/** (instances, review r2) A listed generation could not be read even after the FileOps retries (a scanner's lock):
		 *  what was read comes from an older one. An orphan namespace in this state is neither listed nor merged nor deleted
		 *  this session (§10.5). */
		bool bSkippedUnreadable = false;
		FileHeader Header;
		std::vector<RecoveredBuffer> Buffers;           // open at the end (BUF_CLOSE'd ones have bClosed set)
		json::Value Session;                            // last SESSION head
		std::vector<RecoveredRun> OpenRuns;             // RUN_BEGIN without RUN_END
		std::vector<RecoveredPending> Pending;          // RECOVERY_PENDING without RECOVERY_RESOLVED
		std::vector<std::pair<std::string, uint64_t>> CleanVersions;
		int64_t LastRecordUtcMs = 0;
		/** The format major of the generation read: below FormatMajor when an older major was migrated on read (§10.6). */
		uint16_t SourceMajor = FormatMajor;
	};

	/** One generation of an older major, read as this format's records (§10.6). */
	struct UpgradedGeneration
	{
		FileHeader Header;
		bool bCheckpointComplete = false;   // its first batch is a complete checkpoint
		bool bSalvage = false;
		std::vector<std::vector<Record>> Batches;   // complete batches, in order, as this format's records
	};

	/** The reader of one older major (§10.6: "ReadNamespace dispatches on every older major it knows"). */
	struct OlderMajorReader
	{
		uint16_t Major = 0;
		/** False when the bytes are not a usable generation of that major. */
		bool (*Upgrade)(std::string_view FileBytes, UpgradedGeneration& Out) = nullptr;
	};

	/** The older majors this build reads. Format 1 is the first, so the list is empty; tier 1 passes a synthetic one. */
	DREAMCORE_API std::span<const OlderMajorReader> KnownOlderMajors();

	/**
	 * Reads one namespace directory (§10.2.4): newest generation first, complete batches only, salvage after a bad frame,
	 * fallback to the previous generation, per-buffer CRC checks. With bMayModify it deletes stale *.tmp, touches and
	 * renames a damaged generation to wal.<g>.sjl.corrupt, and handles the generations older than the complete one it read:
	 * this format's are deleted, an older major's are touched and renamed wal.<g>.sjl.v<major> (kept 30 days for a
	 * downgrade, §10.6), and one with a bad header becomes .corrupt, so the namespace keeps one closed generation besides
	 * the session's own (§10.2.3). The cross-root scan passes false (§10.1). The newest header that decodes is checked first
	 * (a damaged one above it does not hide it): when it is a newer major or a newer minor of this major, nothing is
	 * modified whatever bMayModify says, and nothing more once the generation read turns out to be one (§10.6). A
	 * generation of an older major is read through its OlderMajorReader (none known: it is skipped, never modified).
	 */
	DREAMCORE_API RecoveredNamespace ReadNamespace(FileOps& Ops, std::string_view Dir, bool bMayModify = true);
	DREAMCORE_API RecoveredNamespace ReadNamespace(FileOps& Ops, std::string_view Dir, bool bMayModify,
		std::span<const OlderMajorReader> OlderMajors);

	/**
	 * Before a session writes its first generation above the one its read used (persist-M2, §10.2.4, §10.11): every listed
	 * generation of this family above UsedGeneration that is this format (or whose header cannot be read or decoded) was
	 * unusable to the reader and must outlive the session's own generation, which the next read would otherwise treat as
	 * newer and delete it under. Each is touched and renamed .corrupt now (kept 30 days, never deleted); an older or newer
	 * major is left as it is. False when one is still in place (its rename failed): the caller must not start a generation.
	 * A generation whose header decodes as a newer major or a newer minor of this major is never renamed (§10.6: read only,
	 * never modified); it also returns false, with bOutNewerFormat set: the session journals nothing in that namespace.
	 */
	DREAMCORE_API bool SetAsideUnusedNewer(FileOps& Ops, std::string_view Dir, const LogFormat& Format, uint64_t UsedGeneration,
		bool* bOutNewerFormat = nullptr);

	/** Applies DELTA ops to a UTF-8 text (reader and tests). False when an op is out of range. */
	DREAMCORE_API bool ApplyDelta(std::string& InOutTextUtf8, std::span<const DeltaOp> Ops, std::string_view InsertedBytes);

	// ---- Commit policy (game thread) --------------------------------------------------------------------------

	/** When a buffer's commit is due (§10.2.5): 1.0 s idle debounce or 1.0 s since the oldest uncommitted edit. */
	class DREAMCORE_API CommitPolicy
	{
	public:
		static constexpr double IdleDebounceSec = 1.0;
		static constexpr double MaxLatencySec = 1.0;
		static constexpr double ViewStateDebounceSec = 2.0;

		void OnEdit(std::string_view Buffer, double NowSec);
		void OnViewState(std::string_view Buffer, double NowSec);
		void OnCommitted(std::string_view Buffer);       // clears the buffer's pending state
		void Forget(std::string_view Buffer);
		bool Due(std::string_view Buffer, double NowSec) const;
		std::vector<std::string> DueBuffers(double NowSec) const;
		bool AnyPending() const;

	private:
		struct Entry
		{
			std::string Buffer;
			double FirstEdit = -1.0;
			double LastEdit = -1.0;
			double FirstViewState = -1.0;
		};
		std::vector<Entry> Entries;
	};

	// ---- Batch planner (journal writer thread) ------------------------------------------------------------------

	/** One buffer in a commit request: its snapshot and, when it changed, its open info (BUF_OPEN / META fields). */
	struct BufferPart
	{
		std::string Uuid;                        // 32 hex, or "repl-input"
		TextSnapshot Snapshot;
		bool bHasSnapshot = false;
		BufferOpenInfo Open;                     // always filled: a buffer the planner has not seen gets BUF_OPEN
		bool bMeta = false;                      // META (save, Save As)
		bool bDirty = false;
		bool bClose = false;
		std::string CloseReason;
		/** BUF_OPEN and a SNAP even when the planner already holds the uuid (a late safe-mode restore opens a buffer over
		 *  the skipped session's carried part, which has the old open info). */
		bool bOpenEvent = false;
		/** A new clean text (CLEAN_TEXT, §10.7.1): written after the part's text, then kept for every checkpoint. */
		std::optional<TextSnapshot> CleanText;
	};

	/** One commit request of the game thread (§10.2.5). */
	struct CommitRequest
	{
		uint64_t Ticket = 0;
		std::vector<BufferPart> Buffers;
		bool bHasSession = false;
		json::Value Session;
		std::vector<Record> Extra;               // RUN_BEGIN / RUN_END / RECOVERY_RESOLVED, in order
		std::optional<RunBeginInfo> RunBegin;    // kept for checkpoints until its RUN_END
		uint64_t RunEndId = 0;
		bool bClean = false;                     // CLEAN after everything else (orderly shutdown)
		int64_t CleanUtcMs = 0;
		std::vector<std::pair<std::string, uint64_t>> CleanVersions;
	};

	/** A buffer as the planner holds it: its durable state. */
	struct PlannedBuffer
	{
		BufferOpenInfo Open;
		TextSnapshot Committed;                  // the base of the next DELTA
		bool bDirty = false;
		std::optional<TextSnapshot> CleanText;
	};

	/** The records of one batch and the state changes the planner applies once the batch is durable. */
	struct BatchPlan
	{
		std::vector<Record> Records;
		std::vector<uint64_t> Tickets;           // the requests in the batch, in order
		bool bReplay = false;                    // the whole retry queue
		bool bClean = false;
		std::map<std::string, std::optional<PlannedBuffer>> States;   // planned buffer states; unset = closed
		std::vector<std::string> Resolved;       // RECOVERY_RESOLVED uuids
		std::optional<json::Value> Session;
		std::vector<RunBeginInfo> RunBegins;
		std::vector<uint64_t> RunEnds;
	};

	/**
	 * The commit side of the journal writer thread (§10.2.5), the seam tier 1 tests (P2-per-02): it holds what is durable,
	 * turns a request into SNAP / DELTA / META / BUF_CLOSE / CLEAN_TEXT records against it (a DELTA from the durable text, a
	 * SNAP for a new buffer, a delta over half the text, or while the generation may hold a failed batch), and keeps the
	 * retry queue. A failed commit changes nothing, so its retry writes the same records again (BUF_OPEN, BUF_CLOSE, META,
	 * RUN_END, RECOVERY_RESOLVED); the retry replays the whole queue as one batch and one flush.
	 */
	class DREAMCORE_API BatchPlanner
	{
	public:
		BatchPlanner();
		~BatchPlanner();
		BatchPlanner(const BatchPlanner&) = delete;
		BatchPlanner& operator=(const BatchPlanner&) = delete;

		// ---- Durable state ----

		/** The generation's starting state (StartGeneration): a buffer as given, pending recoveries, the session. */
		void SeedBuffer(const BufferPart& Part);
		void SetPendingRecoveries(std::vector<std::pair<PendingInfo, std::string>> Pending);
		void SetSession(json::Value Session);
		/**
		 * The checkpoint body (§10.2.3 step 1, §10.7.1): per buffer BUF_OPEN, SNAP, META when dirty and CLEAN_TEXT when it has
		 * one; every unresolved RECOVERY_PENDING with its SNAP; the SESSION; RUN_BEGIN of every open run.
		 */
		std::vector<Record> BuildCheckpoint() const;
		size_t BufferCount() const;
		const PlannedBuffer* FindBuffer(std::string_view Uuid) const;
		size_t PendingRecoveryCount() const;
		size_t OpenRunCount() const;

		// ---- Planning ----

		/** One request against the durable state; nothing changes until Committed. bSnapOnly: Writer::NeedsCheckpoint(). */
		BatchPlan Plan(const CommitRequest& Request, bool bSnapOnly) const;
		/** Every request of the retry queue, in order, as one batch (P2-per-02); no records when the queue is empty. */
		BatchPlan PlanReplay(bool bSnapOnly) const;
		/** The batch is durable: its state changes apply, and a replay plan empties the retry queue. */
		void Committed(BatchPlan&& Plan);

		// ---- Retry queue: a failed commit and everything after it (§10.2.5) ----

		/** The request's own commit failed: it waits at the back of the queue, whole. */
		void QueueFailed(CommitRequest&& Request);
		/**
		 * A request made while the queue holds a failed one. Its plain text parts fold into the newest kept request (the
		 * newest text wins; a part's planning re-diffs against the durable state anyway), so the queue grows only by the
		 * order-sensitive records made during the outage; a buffer with an open, META or close part keeps its parts in order.
		 */
		void QueueBehindFailure(CommitRequest&& Request);
		size_t RetryQueueLength() const;
		bool HasRetryQueue() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
