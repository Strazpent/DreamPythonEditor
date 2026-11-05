// SPDX-License-Identifier: Apache-2.0
// Dream core: the undo tree (TDD §4.2). A group's ops are one batch in its parent's coordinates; records that join an
// open group are composed into it (a linear merge of the group's ops with the record's ranges), so Undo and Redo always
// return one batch (one version, one ChangeSet) however many records the group took.
#include "dream/UndoTree.h"

#include <algorithm>
#include <limits>

namespace dream::undotree_detail
{
	/** Fixed bookkeeping per group (the node, its map entry, op and caret vectors); proposed, counted in the cap. */
	constexpr size_t GroupOverhead = 256;

	bool IsBlankUnit(char16_t C)
	{
		return C == u' ' || C == u'\t';
	}
}

namespace dream
{
	using namespace undotree_detail;

	UndoTree::UndoTree(uint64_t Version)
	{
		ResetInternal(Version);
	}

	UndoTree::~UndoTree() = default;
	UndoTree::UndoTree(UndoTree&&) noexcept = default;
	UndoTree& UndoTree::operator=(UndoTree&&) noexcept = default;

	UndoTree::Node& UndoTree::At(UndoNodeId Id)
	{
		return Nodes.find(Id)->second;
	}

	const UndoTree::Node* UndoTree::Find(UndoNodeId Id) const
	{
		const auto It = Nodes.find(Id);
		return It == Nodes.end() ? nullptr : &It->second;
	}

	void UndoTree::ResetInternal(uint64_t Version)
	{
		Nodes.clear();
		RunBoundaries.clear();
		Root = NextId++;
		Node R;
		R.Bytes = GroupOverhead;
		Nodes.emplace(Root, std::move(R));
		Current = Root;
		SaveNode = Root;
		ExpectedVersion = Version;
		Retained = GroupOverhead;
		OpenNode = NoUndoNode;
	}

	void UndoTree::Reset(uint64_t Version)
	{
		ResetInternal(Version);
		ExplicitDepth = 0;
		ExplicitLabel.clear();
	}

	void UndoTree::SetLimits(const UndoLimits& InLimits)
	{
		Limits = InLimits;
		TrimIfNeeded();
	}

	const UndoLimits& UndoTree::GetLimits() const
	{
		return Limits;
	}

	size_t UndoTree::NodeBytes(const Node& N)
	{
		size_t Bytes = GroupOverhead + N.Ops.size() * sizeof(Op) + (N.CaretsBefore.size() + N.CaretsAfter.size()) * sizeof(CaretRange)
			+ N.Label.size() * sizeof(char16_t);
		for (const Op& O : N.Ops)
		{
			Bytes += O.Text.size() * sizeof(char16_t);
		}
		return Bytes;
	}

	void UndoTree::Recount(Node& N)
	{
		const size_t Bytes = NodeBytes(N);
		Retained = Retained - N.Bytes + Bytes;
		N.Bytes = Bytes;
	}

	void UndoTree::SyncVersion(uint64_t VersionBefore)
	{
		if (VersionBefore == ExpectedVersion)
		{
			return;
		}
		// An edit the tree never saw: older ops no longer fit the text. A tree with no history adopts the version.
		const bool bHadHistory = Nodes.size() > 1;
		ResetInternal(VersionBefore);
		SaveNode = NoUndoNode;   // the text changed behind the tree: never clean until the next save point
		bReset = bReset || bHadHistory;
	}

	void UndoTree::Compose(std::vector<Op>& Ops, const std::vector<ChangeRange>& Ranges, const TextSnapshot& Before)
	{
		// The group's ops in its post (= the record's pre) coordinates.
		std::vector<Offset> Post(Ops.size());
		Offset Shift = 0;
		for (size_t I = 0; I < Ops.size(); ++I)
		{
			Post[I] = Ops[I].At + Shift;
			Shift += Ops[I].NewLen - Ops[I].OldLen;
		}
		std::vector<Op> Out;
		Out.reserve(Ops.size() + Ranges.size());
		size_t G = 0;
		size_t R = 0;
		Offset DeltaBefore = 0;   // NewLen - OldLen of the group's ops before the cluster
		while (G < Ops.size() || R < Ranges.size())
		{
			// A cluster: intervals that overlap or touch, started by the earliest one.
			const bool bStartG = R >= Ranges.size() || (G < Ops.size() && Post[G] < Ranges[R].At);
			const size_t G0 = G;
			Offset Lo;
			Offset Hi;
			int64_t RecordOld = 0;
			int64_t RecordNew = 0;
			bool bHasRecord = false;
			if (bStartG)
			{
				Lo = Post[G];
				Hi = Post[G] + Ops[G].NewLen;
				++G;
			}
			else
			{
				Lo = Ranges[R].At;
				Hi = Ranges[R].At + Ranges[R].OldLen;
				RecordOld += Ranges[R].OldLen;
				RecordNew += Ranges[R].NewLen;
				bHasRecord = true;
				++R;
			}
			for (;;)
			{
				if (G < Ops.size() && Post[G] <= Hi)
				{
					Hi = std::max(Hi, Post[G] + Ops[G].NewLen);
					++G;
					continue;
				}
				if (R < Ranges.size() && Ranges[R].At <= Hi)
				{
					Hi = std::max(Hi, Ranges[R].At + Ranges[R].OldLen);
					RecordOld += Ranges[R].OldLen;
					RecordNew += Ranges[R].NewLen;
					bHasRecord = true;
					++R;
					continue;
				}
				break;
			}
			if (!bHasRecord)
			{
				for (size_t K = G0; K < G; ++K)
				{
					DeltaBefore += Ops[K].NewLen - Ops[K].OldLen;
					Out.push_back(std::move(Ops[K]));
				}
				continue;
			}
			Op Merged;
			Merged.At = Lo - DeltaBefore;
			Offset Pos = Lo;
			for (size_t K = G0; K < G; ++K)
			{
				Before.CopyRange(Pos, Post[K] - Pos, Merged.Text);
				Merged.Text += Ops[K].Text;
				Pos = Post[K] + Ops[K].NewLen;
				DeltaBefore += Ops[K].NewLen - Ops[K].OldLen;
			}
			Before.CopyRange(Pos, Hi - Pos, Merged.Text);
			Merged.OldLen = static_cast<int64_t>(Merged.Text.size());
			Merged.NewLen = (Hi - Lo) - RecordOld + RecordNew;
			if (Merged.OldLen != 0 || Merged.NewLen != 0)
			{
				Out.push_back(std::move(Merged));
			}
		}
		Ops = std::move(Out);
	}

	void UndoTree::Record(const ChangeSet& Changes, const TextSnapshot& Before, const TextSnapshot& After, UndoKind Kind,
		double NowSec, std::span<const CaretRange> CaretsBefore, std::span<const CaretRange> CaretsAfter)
	{
		if (Changes.IsEmpty() || Changes.VersionAfter == Changes.VersionBefore)
		{
			return;   // nothing applied (also a refused undo batch: the version did not move)
		}
		if (Changes.Origin == EditOrigin::Undo)
		{
			ExpectedVersion = Changes.VersionAfter;
			TextUnits = After.Length();
			return;
		}
		SyncVersion(Changes.VersionBefore);

		const bool bExplicit = ExplicitDepth > 0;
		const bool bSingle = Changes.Ranges.size() == 1;
		const ChangeRange& First = Changes.Ranges.front();
		bool bNewline = false;
		for (const ChangeRange& R : Changes.Ranges)
		{
			bNewline = bNewline || R.NewLines > 0;
		}
		const bool bCoalescing = !bExplicit && bSingle && (Kind == UndoKind::Typing || Kind == UndoKind::Delete) && !bNewline;

		bool bMerge = false;
		if (bExplicit)
		{
			bMerge = OpenNode != NoUndoNode && OpenNode == Current;
		}
		else if (bCoalescing && OpenNode != NoUndoNode && OpenNode == Current && OpenKind == Kind
			&& NowSec - LastRecordTime <= Limits.CoalesceGapSec && NowSec >= LastRecordTime)
		{
			if (Kind == UndoKind::Typing)
			{
				// R1 contiguous typing; R2: whitespace after non-whitespace closes the word first.
				bMerge = First.OldLen == 0 && First.At == ContinueAt;
				if (bMerge && First.NewLen > 0 && First.At > 0)
				{
					std::u16string Units;
					After.CopyRange(First.At, 1, Units);
					Before.CopyRange(First.At - 1, 1, Units);
					if (Units.size() == 2 && IsBlankUnit(Units[0]) && !IsBlankUnit(Units[1]) && Units[1] != u'\n')
					{
						bMerge = false;
					}
				}
			}
			else
			{
				// Backspace ends where the burst left off; Delete starts there.
				bMerge = First.NewLen == 0 && (First.At + First.OldLen == ContinueAt || First.At == ContinueAt);
			}
		}

		if (bMerge)
		{
			Node& N = At(OpenNode);
			Compose(N.Ops, Changes.Ranges, Before);
			N.CaretsAfter.assign(CaretsAfter.begin(), CaretsAfter.end());
			N.LastTime = NowSec;
			Recount(N);
		}
		else
		{
			if (!bExplicit)
			{
				CloseOpen();
			}
			// A new node under the current one; an edit after an undo starts a branch (§4.2 Branches).
			const UndoNodeId Id = NextId++;
			Node N;
			N.Parent = Current;
			N.Kind = bExplicit ? ExplicitKind : Kind;
			if (bExplicit)
			{
				N.Label = ExplicitLabel;
			}
			N.FirstTime = NowSec;
			N.LastTime = NowSec;
			N.CaretsBefore.assign(CaretsBefore.begin(), CaretsBefore.end());
			N.CaretsAfter.assign(CaretsAfter.begin(), CaretsAfter.end());
			Compose(N.Ops, Changes.Ranges, Before);
			N.Bytes = 0;
			Node& Parent = At(Current);
			Parent.Children.push_back(Id);
			Parent.RedoChild = Id;
			auto Inserted = Nodes.emplace(Id, std::move(N));
			Recount(Inserted.first->second);
			Current = Id;
			OpenNode = (bExplicit || bCoalescing) ? Id : NoUndoNode;
			OpenKind = Kind;
		}
		LastRecordTime = NowSec;
		ExpectedVersion = Changes.VersionAfter;
		TextUnits = After.Length();
		if (!bExplicit)
		{
			if (bNewline || !bSingle || (Kind != UndoKind::Typing && Kind != UndoKind::Delete))
			{
				CloseOpen();   // R3: a newline is its own group; R5, R7: one group per command or multi-range edit
			}
			else if (Kind == UndoKind::Typing)
			{
				ContinueAt = First.At + First.NewLen;
			}
			else
			{
				ContinueAt = First.At;
			}
		}
		TrimIfNeeded();
	}

	void UndoTree::DropIfEmpty(UndoNodeId Id)
	{
		const Node* N = Find(Id);
		if (N == nullptr || Id == Root || !N->Ops.empty() || !N->Children.empty() || Id != Current)
		{
			return;
		}
		// A group whose edits cancelled out: the text is its parent's again.
		Current = N->Parent;
		if (SaveNode == Id)
		{
			SaveNode = Current;
		}
		RemoveNodeOnly(Id);
	}

	void UndoTree::CloseOpen()
	{
		if (OpenNode != NoUndoNode)
		{
			DropIfEmpty(OpenNode);
		}
		OpenNode = NoUndoNode;
	}

	void UndoTree::BeginGroup(UndoKind Kind, std::u16string_view Label)
	{
		if (ExplicitDepth++ > 0)
		{
			return;
		}
		CloseOpen();
		ExplicitKind = Kind;
		ExplicitLabel.assign(Label);
	}

	void UndoTree::EndGroup()
	{
		if (ExplicitDepth <= 0)
		{
			return;
		}
		if (--ExplicitDepth == 0)
		{
			CloseOpen();
		}
	}

	bool UndoTree::IsInExplicitGroup() const
	{
		return ExplicitDepth > 0;
	}

	void UndoTree::CloseGroup()
	{
		if (ExplicitDepth == 0)
		{
			CloseOpen();
		}
	}

	bool UndoTree::CanUndo() const
	{
		return Current != Root;
	}

	bool UndoTree::CanRedo() const
	{
		const Node* N = Find(Current);
		return N != nullptr && N->RedoChild != NoUndoNode;
	}

	UndoStep UndoTree::Undo(const TextSnapshot& Snapshot)
	{
		ExplicitDepth = 0;
		CloseOpen();
		UndoStep Step;
		if (Current == Root)
		{
			return Step;
		}
		if (Snapshot.Version() != ExpectedVersion)
		{
			ResetInternal(Snapshot.Version());
			SaveNode = NoUndoNode;   // reset on a mismatch: the text's saved state is unknown
			bReset = true;
			Step.Status = UndoStatus::Reset;
			return Step;
		}
		Node& N = At(Current);
		const Offset Length = Snapshot.Length();
		Offset Shift = 0;
		for (const Op& O : N.Ops)
		{
			const Offset P = O.At + Shift;
			if (P < 0 || P + O.NewLen > Length)
			{
				ResetInternal(Snapshot.Version());
				SaveNode = NoUndoNode;   // reset on a mismatch: the text's saved state is unknown
				bReset = true;
				Step.Status = UndoStatus::Reset;
				return Step;
			}
			Shift += O.NewLen - O.OldLen;
		}
		Step.Batch.Origin = EditOrigin::Undo;
		Step.Batch.Ranges.reserve(N.Ops.size());
		Shift = 0;
		for (Op& O : N.Ops)
		{
			const Offset P = O.At + Shift;
			Shift += O.NewLen - O.OldLen;
			std::u16string Inserted;
			Snapshot.CopyRange(P, O.NewLen, Inserted);
			Step.Batch.Ranges.push_back(EditRange{P, O.NewLen, std::move(O.Text)});
			O.Text = std::move(Inserted);
		}
		Recount(N);
		if (!N.CaretsBefore.empty())
		{
			Step.Carets = N.CaretsBefore;
		}
		else if (!N.Ops.empty())
		{
			const Offset End = N.Ops.back().At + N.Ops.back().OldLen;
			Step.Carets.push_back(CaretRange{End, End});
		}
		Step.Kind = N.Kind;
		Step.Label = N.Label;
		Step.Status = UndoStatus::Applied;
		const UndoNodeId Parent = N.Parent;
		At(Parent).RedoChild = Current;
		Current = Parent;
		ExpectedVersion = Snapshot.Version() + 1;
		TextUnits = Length - Shift;
		return Step;
	}

	UndoStep UndoTree::Redo(const TextSnapshot& Snapshot)
	{
		ExplicitDepth = 0;
		CloseOpen();
		UndoStep Step;
		const UndoNodeId ChildId = At(Current).RedoChild;
		if (ChildId == NoUndoNode)
		{
			return Step;
		}
		if (Snapshot.Version() != ExpectedVersion)
		{
			ResetInternal(Snapshot.Version());
			SaveNode = NoUndoNode;   // reset on a mismatch: the text's saved state is unknown
			bReset = true;
			Step.Status = UndoStatus::Reset;
			return Step;
		}
		Node& N = At(ChildId);
		const Offset Length = Snapshot.Length();
		for (const Op& O : N.Ops)
		{
			if (O.At < 0 || O.At + O.OldLen > Length)
			{
				ResetInternal(Snapshot.Version());
				SaveNode = NoUndoNode;   // reset on a mismatch: the text's saved state is unknown
				bReset = true;
				Step.Status = UndoStatus::Reset;
				return Step;
			}
		}
		Step.Batch.Origin = EditOrigin::Undo;
		Step.Batch.Ranges.reserve(N.Ops.size());
		Offset Shift = 0;
		for (Op& O : N.Ops)
		{
			std::u16string Removed;
			Snapshot.CopyRange(O.At, O.OldLen, Removed);
			Step.Batch.Ranges.push_back(EditRange{O.At, O.OldLen, std::move(O.Text)});
			O.Text = std::move(Removed);
			Shift += O.NewLen - O.OldLen;
		}
		Recount(N);
		if (!N.CaretsAfter.empty())
		{
			Step.Carets = N.CaretsAfter;
		}
		else if (!N.Ops.empty())
		{
			const Op& Last = N.Ops.back();
			const Offset End = Last.At + (Shift - (Last.NewLen - Last.OldLen)) + Last.NewLen;
			Step.Carets.push_back(CaretRange{End, End});
		}
		Step.Kind = N.Kind;
		Step.Label = N.Label;
		Step.Status = UndoStatus::Applied;
		Current = ChildId;
		ExpectedVersion = Snapshot.Version() + 1;
		TextUnits = Length + Shift;
		return Step;
	}

	void UndoTree::MarkSavePoint()
	{
		ExplicitDepth = 0;
		CloseOpen();
		SaveNode = Current;
	}

	void UndoTree::ClearSavePoint()
	{
		SaveNode = NoUndoNode;
	}

	bool UndoTree::IsAtSavePoint() const
	{
		return SaveNode != NoUndoNode && SaveNode == Current;
	}

	void UndoTree::MarkRunBoundary(uint32_t Run)
	{
		ExplicitDepth = 0;
		CloseOpen();
		for (auto& Entry : RunBoundaries)
		{
			if (Entry.first == Run)
			{
				Entry.second = Current;
				return;
			}
		}
		RunBoundaries.emplace_back(Run, Current);
	}

	UndoNodeId UndoTree::RunBoundaryNode(uint32_t Run) const
	{
		for (const auto& Entry : RunBoundaries)
		{
			if (Entry.first == Run)
			{
				return Find(Entry.second) != nullptr ? Entry.second : NoUndoNode;
			}
		}
		return NoUndoNode;
	}

	UndoNodeId UndoTree::CurrentNode() const
	{
		return Current;
	}

	UndoNodeId UndoTree::RootNode() const
	{
		return Root;
	}

	std::vector<UndoNodeId> UndoTree::Branches() const
	{
		const Node* N = Find(Current);
		return N != nullptr ? N->Children : std::vector<UndoNodeId>();
	}

	UndoNodeId UndoTree::RedoTarget() const
	{
		const Node* N = Find(Current);
		return N != nullptr ? N->RedoChild : NoUndoNode;
	}

	bool UndoTree::SelectBranch(UndoNodeId Child)
	{
		Node& N = At(Current);
		if (std::find(N.Children.begin(), N.Children.end(), Child) == N.Children.end())
		{
			return false;
		}
		N.RedoChild = Child;
		return true;
	}

	bool UndoTree::WasTrimmed() const
	{
		return bTrimmed;
	}

	bool UndoTree::WasReset() const
	{
		return bReset;
	}

	void UndoTree::ClearNotices()
	{
		bTrimmed = false;
		bReset = false;
	}

	size_t UndoTree::RetainedBytes() const
	{
		return Retained;
	}

	size_t UndoTree::GroupCount() const
	{
		return Nodes.size() - 1;
	}

	size_t UndoTree::CapBytes() const
	{
		const size_t TextBytes = static_cast<size_t>(std::max<int64_t>(TextUnits, 0)) * sizeof(char16_t);
		return std::max(Limits.MinBytes, TextBytes * Limits.TextMultiple);
	}

	void UndoTree::RemoveNodeOnly(UndoNodeId Id)
	{
		const auto It = Nodes.find(Id);
		if (It == Nodes.end())
		{
			return;
		}
		Node& N = It->second;
		if (N.Parent != NoUndoNode)
		{
			Node& P = At(N.Parent);
			P.Children.erase(std::remove(P.Children.begin(), P.Children.end(), Id), P.Children.end());
			if (P.RedoChild == Id)
			{
				P.RedoChild = P.Children.empty() ? NoUndoNode : P.Children.back();
			}
		}
		for (UndoNodeId C : N.Children)
		{
			auto CIt = Nodes.find(C);
			if (CIt != Nodes.end())
			{
				CIt->second.Parent = NoUndoNode;
			}
		}
		if (SaveNode == Id)
		{
			SaveNode = NoUndoNode;
		}
		if (OpenNode == Id)
		{
			OpenNode = NoUndoNode;
		}
		Retained -= N.Bytes;
		Nodes.erase(It);
	}

	void UndoTree::RemoveSubtree(UndoNodeId Id)
	{
		std::vector<UndoNodeId> Stack{Id};
		std::vector<UndoNodeId> Order;
		while (!Stack.empty())
		{
			const UndoNodeId X = Stack.back();
			Stack.pop_back();
			Order.push_back(X);
			const Node* N = Find(X);
			if (N != nullptr)
			{
				Stack.insert(Stack.end(), N->Children.begin(), N->Children.end());
			}
		}
		for (auto It = Order.rbegin(); It != Order.rend(); ++It)
		{
			RemoveNodeOnly(*It);
		}
	}

	void UndoTree::AdvanceRoot(UndoNodeId Next)
	{
		// Next, the root's child on the path to the current node, becomes the root; it keeps its text state, not its ops.
		if (Next == NoUndoNode || Next == Root || At(Next).Parent != Root)
		{
			return;
		}
		Node& OldRoot = At(Root);
		const std::vector<UndoNodeId> Others = OldRoot.Children;
		for (UndoNodeId C : Others)
		{
			if (C != Next)
			{
				RemoveSubtree(C);
			}
		}
		const bool bSaveWasRoot = SaveNode == Root;
		RemoveNodeOnly(Root);
		if (bSaveWasRoot)
		{
			SaveNode = NoUndoNode;   // the save node is gone: never clean until the next save
		}
		Root = Next;
		Node& NewRoot = At(Root);
		NewRoot.Parent = NoUndoNode;
		NewRoot.Ops.clear();
		NewRoot.Ops.shrink_to_fit();
		NewRoot.CaretsBefore.clear();
		Recount(NewRoot);
	}

	size_t UndoTree::TrimTo(size_t TargetBytes, bool bKeepNewest)
	{
		const size_t Start = Retained;
		if (Retained <= TargetBytes || Nodes.size() <= 1)
		{
			return 0;
		}
		// The newest KeepGroups groups (by creation) are protected while bKeepNewest.
		UndoNodeId Protect = std::numeric_limits<UndoNodeId>::max();
		if (bKeepNewest && Limits.KeepGroups > 0)
		{
			size_t Seen = 0;
			for (auto It = Nodes.rbegin(); It != Nodes.rend(); ++It)
			{
				if (It->first == Root)
				{
					continue;
				}
				Protect = It->first;
				if (++Seen >= Limits.KeepGroups)
				{
					break;
				}
			}
			if (Seen < Limits.KeepGroups)
			{
				Protect = 0;   // fewer groups than the floor: all protected
			}
		}
		auto IsProtected = [&](UndoNodeId Id) { return bKeepNewest && Id >= Protect; };

		// The current branch: the path from the root and the redo chain past the current node.
		std::vector<UndoNodeId> Main;
		for (UndoNodeId X = Current; X != NoUndoNode; X = At(X).Parent)
		{
			Main.push_back(X);
		}
		for (UndoNodeId X = At(Current).RedoChild; X != NoUndoNode; X = At(X).RedoChild)
		{
			Main.push_back(X);
		}
		std::sort(Main.begin(), Main.end());
		auto IsMain = [&](UndoNodeId Id) { return std::binary_search(Main.begin(), Main.end(), Id); };

		// 1. Other branches, whole subtrees, the one whose newest group is oldest first.
		struct Sub
		{
			UndoNodeId Top;
			UndoNodeId Newest;
		};
		std::vector<Sub> Subs;
		for (UndoNodeId M : Main)
		{
			for (UndoNodeId C : At(M).Children)
			{
				if (IsMain(C))
				{
					continue;
				}
				UndoNodeId Newest = C;
				std::vector<UndoNodeId> Stack{C};
				while (!Stack.empty())
				{
					const UndoNodeId X = Stack.back();
					Stack.pop_back();
					Newest = std::max(Newest, X);
					const Node& N = At(X);
					Stack.insert(Stack.end(), N.Children.begin(), N.Children.end());
				}
				Subs.push_back(Sub{C, Newest});
			}
		}
		std::sort(Subs.begin(), Subs.end(), [](const Sub& A, const Sub& B) { return A.Newest < B.Newest; });
		for (const Sub& S : Subs)
		{
			if (Retained <= TargetBytes)
			{
				break;
			}
			if (!IsProtected(S.Newest))
			{
				RemoveSubtree(S.Top);
			}
		}

		// 2. The oldest groups of the current branch: the root advances, never past the current node's parent, so the
		//    newest group stays undoable. The path is walked once (a trim at the cap may advance thousands of times).
		std::vector<UndoNodeId> Path;   // the root's descendants on the way to the current node, nearest the root first
		if (Retained > TargetBytes)
		{
			for (UndoNodeId X = Current; X != Root && X != NoUndoNode; X = At(X).Parent)
			{
				Path.push_back(X);
			}
			std::reverse(Path.begin(), Path.end());
		}
		for (size_t K = 0; K < Path.size() && Retained > TargetBytes && Root != Current; ++K)
		{
			const UndoNodeId Next = Path[K];
			if (Next == Current || IsProtected(Next))
			{
				break;
			}
			AdvanceRoot(Next);
		}

		// 3. Redo groups past the current node, from the far end.
		while (Retained > TargetBytes && !bKeepNewest)
		{
			UndoNodeId Tip = At(Current).RedoChild;
			if (Tip == NoUndoNode)
			{
				break;
			}
			while (At(Tip).RedoChild != NoUndoNode)
			{
				Tip = At(Tip).RedoChild;
			}
			RemoveSubtree(Tip);
		}

		RunBoundaries.erase(std::remove_if(RunBoundaries.begin(), RunBoundaries.end(),
								[this](const std::pair<uint32_t, UndoNodeId>& E) { return Find(E.second) == nullptr; }),
			RunBoundaries.end());
		if (Retained < Start)
		{
			bTrimmed = true;
		}
		return Start - Retained;
	}

	void UndoTree::TrimIfNeeded()
	{
		const size_t Cap = CapBytes();
		if (Retained <= Cap)
		{
			return;
		}
		// Hysteresis: trim to 90 % so a buffer at its cap does not trim on every keystroke (proposed).
		const size_t Target = Cap - Cap / 10;
		TrimTo(Target, true);
		if (Retained > Cap)
		{
			TrimTo(Target, false);   // the cap wins over the newest-groups floor (T-CORE-014)
		}
	}

	size_t EnforceUndoProcessCap(std::span<UndoTree* const> Trees, size_t CapBytes)
	{
		size_t Freed = 0;
		for (int Pass = 0; Pass < 2; ++Pass)
		{
			for (;;)
			{
				size_t Total = 0;
				UndoTree* Biggest = nullptr;
				for (UndoTree* T : Trees)
				{
					if (T == nullptr)
					{
						continue;
					}
					Total += T->RetainedBytes();
					if (Biggest == nullptr || T->RetainedBytes() > Biggest->RetainedBytes())
					{
						Biggest = T;
					}
				}
				if (Total <= CapBytes || Biggest == nullptr)
				{
					return Freed;
				}
				const size_t Over = Total - CapBytes;
				const size_t Target = Biggest->RetainedBytes() > Over ? Biggest->RetainedBytes() - Over : 0;
				const size_t Got = Biggest->TrimTo(Target, Pass == 0);
				Freed += Got;
				if (Got == 0)
				{
					break;   // the biggest cannot give more in this pass; the next pass ignores the floor
				}
			}
		}
		return Freed;
	}
}
