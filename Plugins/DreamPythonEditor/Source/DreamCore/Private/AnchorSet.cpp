// SPDX-License-Identifier: Apache-2.0
// Dream core: dream::AnchorSet (TDD §4.1.3). Slots give stable ids; Order keeps the live slots sorted by offset, so an
// edit touches only the anchors at or after its first offset.
#include "dream/AnchorSet.h"

#include "dream/TextBuffer.h"

#include <algorithm>

namespace dream
{
	namespace
	{
		constexpr uint32_t AnchorNoSlot = UINT32_MAX;

		AnchorId MakeAnchorId(uint32_t SlotIndex, uint32_t Generation)
		{
			return (static_cast<AnchorId>(Generation) << 32) | SlotIndex;
		}
	}

	uint32_t AnchorSet::SlotOf(AnchorId Id) const
	{
		const uint32_t Index = static_cast<uint32_t>(Id & 0xFFFFFFFFu);
		const uint32_t Generation = static_cast<uint32_t>(Id >> 32);
		if (Index >= Slots.size() || !Slots[Index].bLive || Slots[Index].Generation != Generation)
		{
			return AnchorNoSlot;
		}
		return Index;
	}

	size_t AnchorSet::OrderIndexOf(uint32_t SlotIndex) const
	{
		const Offset At = Slots[SlotIndex].Data.At;
		auto It = std::lower_bound(Order.begin(), Order.end(), At, [this](uint32_t S, Offset V) { return Slots[S].Data.At < V; });
		while (It != Order.end() && *It != SlotIndex)
		{
			++It;
		}
		return static_cast<size_t>(It - Order.begin());
	}

	void AnchorSet::Kill(uint32_t SlotIndex)
	{
		Slot& S = Slots[SlotIndex];
		S.bLive = false;
		S.Generation = S.Generation == UINT32_MAX ? 1 : S.Generation + 1;
		FreeSlots.push_back(SlotIndex);
	}

	AnchorId AnchorSet::Add(Offset At, AnchorKind Kind, AnchorGravity Gravity, AnchorDeletePolicy Policy, uint32_t Owner)
	{
		uint32_t Index;
		if (!FreeSlots.empty())
		{
			Index = FreeSlots.back();
			FreeSlots.pop_back();
		}
		else
		{
			Index = static_cast<uint32_t>(Slots.size());
			Slots.emplace_back();
		}
		Slot& S = Slots[Index];
		S.bLive = true;
		S.Data.Id = MakeAnchorId(Index, S.Generation);
		S.Data.At = std::max<Offset>(At, 0);
		S.Data.Kind = Kind;
		S.Data.Gravity = Gravity;
		S.Data.Policy = Policy;
		S.Data.Owner = Owner;
		const auto It = std::upper_bound(Order.begin(), Order.end(), S.Data.At, [this](Offset V, uint32_t O) { return V < Slots[O].Data.At; });
		Order.insert(It, Index);
		return S.Data.Id;
	}

	bool AnchorSet::Remove(AnchorId Id)
	{
		const uint32_t Index = SlotOf(Id);
		if (Index == AnchorNoSlot)
		{
			return false;
		}
		Order.erase(Order.begin() + static_cast<std::ptrdiff_t>(OrderIndexOf(Index)));
		Kill(Index);
		return true;
	}

	size_t AnchorSet::RemoveOwner(uint32_t Owner)
	{
		size_t Count = 0;
		std::vector<uint32_t> Kept;
		Kept.reserve(Order.size());
		for (const uint32_t Index : Order)
		{
			if (Slots[Index].Data.Owner == Owner)
			{
				Kill(Index);
				++Count;
			}
			else
			{
				Kept.push_back(Index);
			}
		}
		Order.swap(Kept);
		return Count;
	}

	void AnchorSet::Clear()
	{
		for (const uint32_t Index : Order)
		{
			Kill(Index);
		}
		Order.clear();
		Removed.clear();
	}

	Offset AnchorSet::Find(AnchorId Id) const
	{
		const uint32_t Index = SlotOf(Id);
		return Index == AnchorNoSlot ? Offset(-1) : Slots[Index].Data.At;
	}

	const Anchor* AnchorSet::Get(AnchorId Id) const
	{
		const uint32_t Index = SlotOf(Id);
		return Index == AnchorNoSlot ? nullptr : &Slots[Index].Data;
	}

	std::vector<Anchor> AnchorSet::All() const
	{
		std::vector<Anchor> Out;
		Out.reserve(Order.size());
		for (const uint32_t Index : Order)
		{
			Out.push_back(Slots[Index].Data);
		}
		return Out;
	}

	std::vector<Anchor> AnchorSet::InRange(Offset First, Offset Last) const
	{
		std::vector<Anchor> Out;
		auto It = std::lower_bound(Order.begin(), Order.end(), First, [this](uint32_t S, Offset V) { return Slots[S].Data.At < V; });
		for (; It != Order.end() && Slots[*It].Data.At < Last; ++It)
		{
			Out.push_back(Slots[*It].Data);
		}
		return Out;
	}

	void AnchorSet::MapThrough(const ChangeSet& Changes, const TextSnapshot& Before, const TextSnapshot& After)
	{
		if (Order.empty() || Changes.Ranges.empty())
		{
			return;
		}
		const std::vector<ChangeRange>& Ranges = Changes.Ranges;
		const Offset FirstAt = Ranges.front().At;
		const Offset LastEnd = Ranges.back().At + Ranges.back().OldLen;
		const auto BeginIt = std::lower_bound(Order.begin(), Order.end(), FirstAt, [this](uint32_t S, Offset V) { return Slots[S].Data.At < V; });
		const size_t Begin = static_cast<size_t>(BeginIt - Order.begin());

		size_t R = 0;
		int64_t Delta = 0;
		bool bDropped = false;
		Offset Previous = Begin > 0 ? Slots[Order[Begin - 1]].Data.At : Offset(-1);
		bool bSorted = true;
		for (size_t I = Begin; I < Order.size(); ++I)
		{
			Anchor& A = Slots[Order[I]].Data;
			const Offset X = A.At;
			// Pass the ranges that end before X: X is after their deleted text, or at its end when they delete. This part
			// grows with X, so the walk is shared by all anchors.
			while (R < Ranges.size()
				&& (X > Ranges[R].At + Ranges[R].OldLen || (X == Ranges[R].At + Ranges[R].OldLen && Ranges[R].OldLen > 0)))
			{
				Delta += Ranges[R].NewLen - Ranges[R].OldLen;
				++R;
			}
			// Pure inserts at X go in front of this anchor for After gravity (a LineStart anchor always lets text at its
			// column go above it); that depends on the anchor, so it is a local step.
			size_t LR = R;
			int64_t LDelta = Delta;
			if (A.Kind == AnchorKind::LineStart || A.Gravity == AnchorGravity::After)
			{
				while (LR < Ranges.size() && Ranges[LR].OldLen == 0 && Ranges[LR].At == X)
				{
					LDelta += Ranges[LR].NewLen;
					++LR;
				}
			}
			Offset Mapped = X + LDelta;
			if (LR < Ranges.size() && X >= Ranges[LR].At)
			{
				// X is at the start of range LR or strictly inside its deleted text.
				const ChangeRange& G = Ranges[LR];
				const Offset Start = G.At + LDelta;
				const Offset End = G.At + LDelta + G.NewLen;
				bool bDeleted = false;
				if (A.Kind == AnchorKind::LineStart)
				{
					// MapLine's rule: the line goes when the deleted text holds its '\n' and starts at or before its first
					// column; text inserted at its first column goes above it. Touching ranges delete as one (MapLine's
					// adjacent-range rule), and the line's first surviving character follows the last of them.
					size_t J = LR;
					int64_t ChainDelta = LDelta;
					while (J + 1 < Ranges.size() && Ranges[J + 1].At == Ranges[J].At + Ranges[J].OldLen)
					{
						ChainDelta += Ranges[J].NewLen - Ranges[J].OldLen;
						++J;
					}
					const Offset DeletedEnd = Ranges[J].At + Ranges[J].OldLen;
					bDeleted = DeletedEnd > G.At && Before.LineOf(DeletedEnd) > Before.LineOf(X);
					Mapped = Ranges[J].At + ChainDelta + Ranges[J].NewLen;
				}
				else
				{
					bDeleted = X > G.At;
					Mapped = A.Gravity == AnchorGravity::After ? End : Start;
				}
				if (bDeleted && A.Policy == AnchorDeletePolicy::Remove)
				{
					Removed.push_back(A.Id);
					Kill(Order[I]);
					Order[I] = AnchorNoSlot;
					bDropped = true;
					continue;
				}
			}
			if (A.Kind == AnchorKind::LineStart && X <= LastEnd)
			{
				Mapped = After.LineStart(After.LineOf(Mapped));
			}
			A.At = Mapped;
			bSorted = bSorted && Mapped >= Previous;
			Previous = Mapped;
		}
		if (bDropped)
		{
			Order.erase(std::remove(Order.begin() + static_cast<std::ptrdiff_t>(Begin), Order.end(), AnchorNoSlot), Order.end());
		}
		if (!bSorted)
		{
			// Rare: a collapse or a line-start snap (a deleted '\n' merges the line into the one above) moved an anchor
			// in front of one it followed.
			std::stable_sort(Order.begin(), Order.end(), [this](uint32_t L, uint32_t Rhs) { return Slots[L].Data.At < Slots[Rhs].Data.At; });
		}
	}

	std::vector<AnchorId> AnchorSet::TakeRemoved()
	{
		std::vector<AnchorId> Out;
		Out.swap(Removed);
		return Out;
	}
}
