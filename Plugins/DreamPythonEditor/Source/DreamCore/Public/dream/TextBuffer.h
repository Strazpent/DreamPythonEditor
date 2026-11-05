// SPDX-License-Identifier: Apache-2.0
// Dream core: the text model (TDD §4.1). UTF-16 code units, LF-only lines, O(1) immutable snapshots, ChangeSets.
// Threads: TextBuffer is mutated on the game thread only; a TextSnapshot is immutable and may be read on any thread.
#pragma once

#include "dream/AnchorSet.h"
#include "dream/Types.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dream
{
	/** Where an edit came from (TDD §4.1.3). Undo grouping and the journal read it. */
	enum class EditOrigin : uint8_t
	{
		Typing,
		Command,
		Ime,
		Undo,
		Reload,
		Load,
	};

	/** One replacement: delete DeleteLen units at At, then insert Insert there. Insert must not contain '\r'. */
	struct EditRange
	{
		Offset At = 0;
		int64_t DeleteLen = 0;
		std::u16string Insert;
	};

	/** Non-overlapping ranges, in any order; Apply sorts them and applies from the highest offset down (§4.1.3). */
	struct EditBatch
	{
		std::vector<EditRange> Ranges;
		EditOrigin Origin = EditOrigin::Typing;
	};

	/** One applied range in pre-edit coordinates, plus its line facts (§4.1.3). */
	struct ChangeRange
	{
		Offset At = 0;                 // offset in the text before the edit
		int64_t OldLen = 0;
		int64_t NewLen = 0;
		LineNo StartLine = 0;          // line of At before the edit
		int32_t StartCol16 = 0;        // UTF-16 column of At
		LineNo OldLines = 0;           // '\n' count removed
		LineNo NewLines = 0;           // '\n' count inserted
	};

	/** The result of one Apply: one version step, ranges in ascending order of At. */
	struct ChangeSet
	{
		uint64_t VersionBefore = 0;
		uint64_t VersionAfter = 0;
		EditOrigin Origin = EditOrigin::Typing;
		std::vector<ChangeRange> Ranges;

		bool IsEmpty() const { return Ranges.empty(); }
	};

	namespace detail
	{
		/** The immutable representation behind a snapshot. Defined in Private/; its layout is the core's choice. */
		struct TextRep;
		/** One applied ChangeSet in the buffer's change log (ChangesBetween, MapLine). Defined in Private/. */
		struct ChangeNode;
		/** The buffer's append state: the add chunk typed text goes to and the live units per chunk. Game thread. */
		struct EditState;
	}

	/**
	 * An immutable view of one buffer version. Copying is a reference-count increment. Readable on any thread.
	 * The representation is private (detail::TextRep, A3): the §4.1.3 persistent piece tree (immutable chunks, a B+-tree
	 * of pieces with fan-out 32 caching (Len, Lf) per child, path-copied on every edit). Line and offset lookups are
	 * O(log pieces); text that spans pieces is copied only where a call asks for one contiguous view.
	 */
	class DREAMCORE_API TextSnapshot
	{
	public:
		TextSnapshot();                                 // the empty text, version 0, one empty line

		uint64_t Version() const;
		int64_t Length() const;                         // UTF-16 units, '\n' included
		LineNo LineCount() const;                       // >= 1; a text ending in '\n' has an empty last line
		Offset LineStart(LineNo Line) const;            // clamps Line to [0, LineCount)
		Offset LineEnd(LineNo Line) const;              // offset of the line's '\n', or Length() for the last line
		int32_t LineLength(LineNo Line) const;          // without the '\n'
		LineNo LineOf(Offset At) const;                 // clamps At to [0, Length]

		/** The line's text without '\n'. Zero-copy when contiguous; otherwise copied into Scratch. */
		std::u16string_view LineView(LineNo Line, std::u16string& Scratch) const;
		std::u16string LineText(LineNo Line) const;

		/** Appends units [At, At + Len) to Out, clamped to the text. */
		void CopyRange(Offset At, int64_t Len, std::u16string& Out) const;

		/** The longest contiguous run of units starting at At, at most MaxLen long (empty at the end). Loop to walk. */
		std::u16string_view Chunk(Offset At, int64_t MaxLen) const;

		/** Calls Visit(std::u16string_view) for each contiguous run of [At, At + Len), clamped, in order (§4.1.3). */
		template <class Fn>
		void ForEachSpan(Offset At, int64_t Len, Fn&& Visit) const
		{
			const Offset End = At + (Len > 0 ? Len : 0);
			while (At < End)
			{
				const std::u16string_view Run = Chunk(At, End - At);
				if (Run.empty())
				{
					break;
				}
				Visit(Run);
				At += static_cast<Offset>(Run.size());
			}
		}

		std::u16string Text() const;                    // whole-text copy
		std::string TextUtf8() const;                   // whole text as UTF-8 (journal SNAP body, §10.2.2)

		/** True when both snapshots name the same immutable representation (cheap identity test). */
		bool SameRepAs(const TextSnapshot& Other) const;

		/** Diagnostics (compaction decisions, T-CORE-001/-002/-004): pieces in the tree and its depth (0 = one leaf). */
		size_t PieceCount() const;
		int32_t TreeDepth() const;

	private:
		friend class TextBuffer;
		explicit TextSnapshot(std::shared_ptr<const detail::TextRep> InRep);
		std::shared_ptr<const detail::TextRep> Rep;
	};

	/**
	 * The mutable model of one buffer. Game thread only (TDD §4 conventions). Movable, not copyable (its add chunk is
	 * appended in place; copy the text with TextBuffer(Snapshot().Text()) or share a Snapshot()).
	 */
	class DREAMCORE_API TextBuffer
	{
	public:
		TextBuffer();
		/** Starts at version 1 with Initial, which must already be LF-only (TextCodec normalizes, §4.13). */
		explicit TextBuffer(std::u16string_view Initial);
		~TextBuffer();
		TextBuffer(TextBuffer&& Other) noexcept;
		TextBuffer& operator=(TextBuffer&& Other) noexcept;
		TextBuffer(const TextBuffer&) = delete;
		TextBuffer& operator=(const TextBuffer&) = delete;

		TextSnapshot Snapshot() const;                  // O(1)
		uint64_t Version() const;
		int64_t Length() const;
		LineNo LineCount() const;

		/** False when a range is out of bounds, ranges overlap, an insert holds '\r', or a range splits a surrogate pair. */
		bool Validate(const EditBatch& Batch) const;

		/**
		 * Applies a batch atomically: one new version and one ChangeSet. An invalid batch changes nothing and returns
		 * an empty ChangeSet with VersionBefore == VersionAfter. An empty batch also changes nothing. The buffer's
		 * AnchorSet is mapped through the ChangeSet before Apply returns.
		 */
		ChangeSet Apply(const EditBatch& Batch);

		/** Replaces the whole text as one batch (Load or Reload origin). */
		ChangeSet SetText(std::u16string_view NewText, EditOrigin Origin);

		/** Column conversions on one line of the current version (TDD §4.1.3, T-CORE-007). Clamp out-of-range input. */
		int32_t Utf8ColToUtf16(LineNo Line, int32_t ByteCol) const;
		int32_t Utf16ColToUtf8(LineNo Line, int32_t Col16) const;
		int32_t CodepointColToUtf16(LineNo Line, int32_t Col) const;

		/**
		 * Maps a line of an older version through the ChangeSets applied since (TDD §4.1.3, traceback links §6.4).
		 * Returns InvalidLine when that line was deleted or Version is older than the oldest kept ChangeSet.
		 * Version == Version() returns Line unchanged (clamped).
		 * (slice A rule, T-CORE-040) Per ChangeSet, a line maps to the line that holds its first surviving character; text
		 * inserted at its first column goes above it; it is deleted when the ChangeSet removes all of it, from its first
		 * column through its '\n'. The last line has no '\n' and is never deleted.
		 */
		LineNo MapLine(uint64_t FromVersion, LineNo Line) const;

		/** How many ChangeSets MapLine and ChangesBetween keep (proposed horizon, §4.1.3). Default 1000. */
		void SetHistoryLimit(size_t MaxChangeSets);
		uint64_t OldestMappableVersion() const;

		/**
		 * Any thread (the journal writer, §10.2.1): the edits that turn From into To, as one batch in From's
		 * coordinates (ranges ascending and non-overlapping, common prefix and suffix trimmed, Insert copied from To,
		 * Origin Command). False when the two are not versions of one buffer with From not newer than To, or when a
		 * ChangeSet between them is no longer kept (older than the history horizon): write a SNAP instead.
		 */
		static bool ChangesBetween(const TextSnapshot& From, const TextSnapshot& To, EditBatch& Out);

		/** The positions that move with this buffer's edits (§4.1.3); Apply maps them. */
		AnchorSet& Anchors() { return AnchorStore; }
		const AnchorSet& Anchors() const { return AnchorStore; }

		/**
		 * Compaction (§4.1.3): true when the tree holds more than 2 x lines + 4,096 pieces, or its chunks hold more
		 * than 2 x the text + 1 Mi units (deleted text kept alive). Schedule BuildCompacted on a worker after 2 s idle
		 * (proposed), then InstallCompacted on the game thread.
		 */
		bool NeedsCompaction() const;
		/** Any thread: the same text and version in one fresh chunk and one piece. */
		static TextSnapshot BuildCompacted(const TextSnapshot& From);
		/** Installs a BuildCompacted result if this buffer is still at BuiltFromVersion; false (and nothing changes) for a
		 *  stale build or a snapshot of another buffer (T-CORE-005). */
		bool InstallCompacted(TextSnapshot Built, uint64_t BuiltFromVersion);

	private:
		void PushHistory(const std::shared_ptr<const detail::ChangeNode>& Node);

		std::shared_ptr<const detail::TextRep> Current;
		std::deque<std::shared_ptr<const detail::ChangeNode>> History;   // oldest first, at most HistoryLimit
		size_t HistoryLimit = 1000;
		AnchorSet AnchorStore;
		std::unique_ptr<detail::EditState> Edit;
	};
}
