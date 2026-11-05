// SPDX-License-Identifier: Apache-2.0
// Dream core: positions that move with edits (TDD §4.1.3 dream::AnchorSet). Owned by a TextBuffer, which maps it through
// every ChangeSet it applies. Game thread only, like the buffer.
// Owners: breakpoints (§8), ran-line marks (§4.8.2, EDT-18), folds, line-ending exceptions (§4.13), diagnostics (§7).
#pragma once

#include "dream/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace dream
{
	struct ChangeSet;
	class TextSnapshot;

	/** Where a Point anchor at an insertion point goes: Before stays in front of the inserted text, After moves past it.
	 *  A LineStart anchor ignores it: text inserted at its line's first column goes above it (MapLine's rule). */
	enum class AnchorGravity : uint8_t
	{
		Before,
		After,
	};

	/** Point anchors keep an offset; LineStart anchors stay on the start of their line (§4.1.3). */
	enum class AnchorKind : uint8_t
	{
		Point,
		LineStart,
	};

	/** What happens when an edit deletes an anchor's position (a Point strictly inside the deleted text, a LineStart whose
	 *  whole line goes, MapLine's rule): Collapse moves it to the edit, Remove drops it (TakeRemoved reports it). */
	enum class AnchorDeletePolicy : uint8_t
	{
		Collapse,
		Remove,
	};

	/** A stable handle: slot in the low 32 bits, the slot's generation in the high 32 bits; 0 is never an anchor. */
	using AnchorId = uint64_t;
	inline constexpr AnchorId InvalidAnchor = 0;

	struct Anchor
	{
		AnchorId Id = InvalidAnchor;
		Offset At = 0;
		AnchorGravity Gravity = AnchorGravity::Before;
		AnchorKind Kind = AnchorKind::Point;
		AnchorDeletePolicy Policy = AnchorDeletePolicy::Collapse;
		uint32_t Owner = 0;          // the owner's own tag (breakpoint, mark, fold, ...); RemoveOwner drops them together
	};

	/**
	 * A sorted set of anchors. MapThrough costs O(log a + anchors after the first edited offset) (§4.1.3). Ids stay valid
	 * until the anchor is removed (by Remove, RemoveOwner, Clear or a Remove-policy delete); a stale id finds nothing.
	 */
	class DREAMCORE_API AnchorSet
	{
	public:
		/** Adds an anchor (At is clamped to >= 0; a LineStart anchor should be given a line start). */
		AnchorId Add(Offset At, AnchorKind Kind, AnchorGravity Gravity, AnchorDeletePolicy Policy, uint32_t Owner = 0);
		bool Remove(AnchorId Id);
		/** Removes every anchor of Owner; returns how many. */
		size_t RemoveOwner(uint32_t Owner);
		void Clear();

		/** The anchor's current offset, or -1 when the id is stale. */
		Offset Find(AnchorId Id) const;
		bool Contains(AnchorId Id) const { return Find(Id) >= 0; }
		/** The anchor, or nullptr when the id is stale. Valid until the next mutation. */
		const Anchor* Get(AnchorId Id) const;

		size_t Size() const { return Order.size(); }
		bool IsEmpty() const { return Order.empty(); }
		/** Every live anchor, sorted by offset (ties keep insertion order). Valid until the next mutation. */
		std::vector<Anchor> All() const;
		/** Live anchors with First <= At < Last, sorted by offset (a view for one paint or one line). */
		std::vector<Anchor> InRange(Offset First, Offset Last) const;

		/**
		 * Maps every anchor through one applied ChangeSet. Before and After are the snapshots on either side of it (a
		 * LineStart anchor's line is judged in Before and its new line start is read in After). TextBuffer::Apply calls
		 * this for the buffer's own set; call it yourself only for a set you keep outside a buffer.
		 */
		void MapThrough(const ChangeSet& Changes, const TextSnapshot& Before, const TextSnapshot& After);

		/** Ids that MapThrough removed (Remove policy) since the last call, oldest first; clears the list. */
		std::vector<AnchorId> TakeRemoved();

	private:
		struct Slot
		{
			Anchor Data;
			uint32_t Generation = 1;
			bool bLive = false;
		};

		uint32_t SlotOf(AnchorId Id) const;
		size_t OrderIndexOf(uint32_t SlotIndex) const;
		void Kill(uint32_t SlotIndex);

		std::vector<Slot> Slots;
		std::vector<uint32_t> FreeSlots;
		std::vector<uint32_t> Order;      // live slots sorted by offset
		std::vector<AnchorId> Removed;
	};
}
