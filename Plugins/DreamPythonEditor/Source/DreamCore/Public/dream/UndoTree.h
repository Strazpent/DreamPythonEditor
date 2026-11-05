// SPDX-License-Identifier: Apache-2.0
// Dream core: operation-based undo and redo per buffer (TDD §4.2, EDT-08): groups, typing coalescing (R1-R4), one group
// per command or explicit group (R5-R7), branches (an edit after an undo starts one; redo follows the newest), save
// points (dirty = not at the save node; survives save), run boundaries (R8) and a memory bound. Game thread.
//
// Typical use (plain mode, §4.15 row 3 "Ctrl+Z drives UndoTree"):
//   const TextSnapshot Before = Buffer.Snapshot();
//   const ChangeSet C = Buffer.Apply(Batch);
//   Undo.Record(C, Before, Buffer.Snapshot(), UndoKind::Typing, NowSec, CaretsBefore, CaretsAfter);
//   ...
//   UndoStep S = Undo.Undo(Buffer.Snapshot());           // Ctrl+Z
//   if (S.Status == UndoStatus::Applied) { Buffer.Apply(S.Batch); /* restore S.Carets */ }
// The batch an Undo or Redo returns must be applied right away (its origin is EditOrigin::Undo; passing that ChangeSet
// to Record is allowed and only re-syncs the version). An edit the tree never saw (a version gap) drops the history
// and sets WasReset() (the "Undo history reset" note of §4.2's failure modes); nothing is ever applied wrongly.
// (slice B choice) An op keeps one text: the removed text while its group is applied, the inserted text once it is
// undone; the other side is in the buffer at that moment and is read back from the snapshot Undo or Redo is given.
#pragma once

#include "dream/TextBuffer.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream
{
	/** A node of the tree: 0 is "none"; the root (the base text) is a node with no ops. Ids grow with creation. */
	using UndoNodeId = uint32_t;
	inline constexpr UndoNodeId NoUndoNode = 0;

	/** What made a group (§4.2 rules). Only Typing and Delete coalesce across records. */
	enum class UndoKind : uint8_t
	{
		Typing,      // characters typed (R1-R3)
		Delete,      // Backspace / Delete bursts (R1, R4)
		Command,     // R5: paste, cut, comment, indent, line commands, auto-close, completion, format, Replace All, drop
		Ime,         // R6: one group per composition (BeginGroup at start, EndGroup at EndComposition)
		Reload,      // "Reloaded from disk" (BUF-04, §10.15)
		Restore,     // history restore, Restore of a recovered buffer (U30)
		Recovery,    // "Recovered unsaved changes" (§4.2 journal interplay)
	};

	/** A selection in offsets (UTF-16 units of the text it belongs to). */
	struct CaretRange
	{
		Offset Anchor = 0;
		Offset Active = 0;

		bool operator==(const CaretRange&) const = default;
	};

	/** EDT-08's proposed numbers. */
	struct UndoLimits
	{
		size_t MinBytes = size_t(32) << 20;   // per-buffer cap: max(MinBytes, TextMultiple x text bytes)
		uint32_t TextMultiple = 4;
		size_t KeepGroups = 1000;             // the newest groups kept while they fit (the cap wins over them)
		double CoalesceGapSec = 1.0;          // R1: a pause longer than this splits a typing burst
	};

	enum class UndoStatus : uint8_t
	{
		Applied,     // Batch holds the step: apply it now, then restore Carets
		Nothing,     // nothing to undo or redo
		Reset,       // the buffer was not at the version the tree expected: history dropped, nothing to apply
	};

	struct UndoStep
	{
		UndoStatus Status = UndoStatus::Nothing;
		EditBatch Batch;                      // origin Undo, ranges in the coordinates of the snapshot passed in
		std::vector<CaretRange> Carets;       // after applying: the group's carets before (undo) or after (redo)
		UndoKind Kind = UndoKind::Typing;
		std::u16string Label;                 // the group's label ("Reloaded from disk"); empty for typing
	};

	class DREAMCORE_API UndoTree
	{
	public:
		/** A tree whose root is the buffer text at Version (TextBuffer starts at 1). At its save point. */
		explicit UndoTree(uint64_t Version = 1);
		~UndoTree();
		UndoTree(UndoTree&&) noexcept;
		UndoTree& operator=(UndoTree&&) noexcept;
		UndoTree(const UndoTree&) = delete;
		UndoTree& operator=(const UndoTree&) = delete;

		/** Drops all history: the root becomes the text at Version, which is the save point (call ClearSavePoint for a
		 *  dirty text). Load, a whole-text replacement nobody may undo, a restored session. */
		void Reset(uint64_t Version);
		void SetLimits(const UndoLimits& Limits);
		const UndoLimits& GetLimits() const;

		/**
		 * Records one applied ChangeSet. Before and After are the snapshots on both sides of it (O(1) to take); only the
		 * removed text is copied. Kind decides coalescing: a Typing or Delete record merges into the open group when it
		 * continues it (same kind, at the place the burst left off, within CoalesceGapSec, one range); whitespace after
		 * non-whitespace starts a new group (R2); a newline is a group of its own (R3); any other kind is one group (R5)
		 * unless an explicit group is open, which takes every record until its EndGroup. Carets may be empty.
		 * A ChangeSet with origin Undo only re-syncs the version; an empty one is ignored.
		 */
		void Record(const ChangeSet& Changes, const TextSnapshot& Before, const TextSnapshot& After, UndoKind Kind,
			double NowSec, std::span<const CaretRange> CaretsBefore = {}, std::span<const CaretRange> CaretsAfter = {});

		/** Explicit group: every Record until the matching EndGroup is one undo step with this kind and label. Nests
		 *  (only the outermost counts). An explicit group with no record makes no step; one whose edits cancel out
		 *  is dropped at EndGroup. */
		void BeginGroup(UndoKind Kind, std::u16string_view Label = {});
		void EndGroup();
		bool IsInExplicitGroup() const;

		/** Closes the open typing or delete burst (R4: the caret moved by navigation, a click or find; focus loss).
		 *  Leaves an explicit group open. */
		void CloseGroup();

		bool CanUndo() const;
		bool CanRedo() const;
		/** Ctrl+Z / Ctrl+Y. Close any open group first (an open explicit group ends here). Current must be the buffer's
		 *  snapshot now; the returned batch is in its coordinates. */
		UndoStep Undo(const TextSnapshot& Current);
		UndoStep Redo(const TextSnapshot& Current);

		/** R9: closes the open group and makes the current node the save node. */
		void MarkSavePoint();
		/** The buffer holds no save point (a text loaded dirty, a deleted file). */
		void ClearSavePoint();
		/** Clean exactly at the save node; never again once the save node was trimmed, until the next save. */
		bool IsAtSavePoint() const;

		/** R8: closes the open group and marks the current node as the text run #Run started from. */
		void MarkRunBoundary(uint32_t Run);
		/** The node marked for Run, or NoUndoNode when none or trimmed. */
		UndoNodeId RunBoundaryNode(uint32_t Run) const;

		UndoNodeId CurrentNode() const;
		UndoNodeId RootNode() const;
		/** The current node's children, oldest first (branches), and the one Redo follows. */
		std::vector<UndoNodeId> Branches() const;
		UndoNodeId RedoTarget() const;
		/** Makes Child (a child of the current node) the branch Redo follows. False when it is not one. */
		bool SelectBranch(UndoNodeId Child);

		/** Groups trimmed by the memory bound since the last ClearNotices ("Older undo history trimmed"). */
		bool WasTrimmed() const;
		/** History dropped because the buffer changed behind the tree's back ("Undo history reset"). */
		bool WasReset() const;
		void ClearNotices();

		/** Bytes held only by undo history (op texts plus a fixed per-group overhead). */
		size_t RetainedBytes() const;
		/** Undo steps kept (nodes besides the root). */
		size_t GroupCount() const;
		/** The per-buffer cap for the current text length. */
		size_t CapBytes() const;
		/**
		 * Trims oldest groups first, other branches before the current one, until RetainedBytes <= TargetBytes. With
		 * bKeepNewest the newest KeepGroups groups stay; without it only the current group stays undoable (the cap wins
		 * over the floor, T-CORE-014). Returns the bytes freed. Record calls it on its own when the cap is passed.
		 */
		size_t TrimTo(size_t TargetBytes, bool bKeepNewest);

	private:
		struct Op
		{
			Offset At = 0;              // in the parent node's text
			int64_t OldLen = 0;
			int64_t NewLen = 0;
			std::u16string Text;        // removed text while applied, inserted text while undone
		};

		struct Node
		{
			UndoNodeId Parent = NoUndoNode;
			std::vector<UndoNodeId> Children;
			UndoNodeId RedoChild = NoUndoNode;
			std::vector<Op> Ops;        // ascending, non-overlapping
			std::vector<CaretRange> CaretsBefore;
			std::vector<CaretRange> CaretsAfter;
			UndoKind Kind = UndoKind::Typing;
			std::u16string Label;
			double FirstTime = 0.0;
			double LastTime = 0.0;
			size_t Bytes = 0;
		};

		Node& At(UndoNodeId Id);
		const Node* Find(UndoNodeId Id) const;
		void ResetInternal(uint64_t Version);
		void SyncVersion(uint64_t VersionBefore);
		void CloseOpen();
		void DropIfEmpty(UndoNodeId Id);
		static void Compose(std::vector<Op>& Ops, const std::vector<ChangeRange>& Ranges, const TextSnapshot& Before);
		static size_t NodeBytes(const Node& N);
		void Recount(Node& N);
		void RemoveSubtree(UndoNodeId Id);
		void RemoveNodeOnly(UndoNodeId Id);
		void AdvanceRoot(UndoNodeId Next);
		void TrimIfNeeded();

		std::map<UndoNodeId, Node> Nodes;
		UndoNodeId Root = NoUndoNode;
		UndoNodeId Current = NoUndoNode;
		UndoNodeId NextId = 1;
		UndoNodeId SaveNode = NoUndoNode;
		std::vector<std::pair<uint32_t, UndoNodeId>> RunBoundaries;
		uint64_t ExpectedVersion = 0;
		int64_t TextUnits = 0;
		size_t Retained = 0;
		UndoLimits Limits;

		// The open group: the node that still takes records, how the burst continues, the explicit group.
		UndoNodeId OpenNode = NoUndoNode;
		UndoKind OpenKind = UndoKind::Typing;
		Offset ContinueAt = 0;
		double LastRecordTime = 0.0;
		int32_t ExplicitDepth = 0;
		UndoKind ExplicitKind = UndoKind::Command;
		std::u16string ExplicitLabel;

		bool bTrimmed = false;
		bool bReset = false;
	};

	/**
	 * EDT-08's process cap (proposed 256 MB): DreamCore holds no globals, so the editor passes every live tree. Trims the
	 * tree holding the most bytes first, keeping each tree's newest groups while possible. Returns the bytes freed.
	 */
	DREAMCORE_API size_t EnforceUndoProcessCap(std::span<UndoTree* const> Trees, size_t CapBytes);
}
