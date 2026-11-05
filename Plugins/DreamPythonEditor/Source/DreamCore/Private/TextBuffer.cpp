// SPDX-License-Identifier: Apache-2.0
// Dream core: the text model (TDD §4.1.3). A persistent piece tree:
// - Chunks hold the text. The loaded text is one chunk; typed and pasted text is appended to add chunks of 65,536 units
//   (a paste over half of that gets a chunk of its own size). A chunk's units and its LF index (positions of '\n', in
//   fixed 1,024-entry blocks) are append-only and never move, so a reader on any thread sees exactly the units its pieces
//   name while the game thread appends past them.
// - A piece names a run of one chunk: {Chunk, Start, Len, Lf, LfIndex} (LfIndex: the chunk's LF index entry of its
//   first '\n', so line lookups inside a piece are O(1) and splits O(log Lf)).
// - A B+-tree of fan-out 32 orders the pieces; inner nodes cache (Len, Lf) per child. An edit copies only the nodes on
//   its root-to-leaf paths; snapshots share everything else. Lookups descend by Len (offset to line) or Lf (line to
//   offset).
// - Each snapshot names its chunk table, its buffer (an identity object) and the ChangeSet that made it; the ChangeSets
//   link backwards weakly, so ChangesBetween walks them on any thread while the buffer's history keeps them alive.
#include "dream/TextBuffer.h"

#include "dream/Utf.h"

#include <algorithm>
#include <array>
#include <utility>

namespace dream::detail
{
	inline constexpr uint32_t AddChunkUnits = 65536;
	inline constexpr uint32_t LfBlockShift = 10;
	inline constexpr uint32_t LfBlockSize = 1u << LfBlockShift;
	inline constexpr int32_t Fanout = 32;
	inline constexpr int32_t MinFill = Fanout / 4;
	inline constexpr uint32_t NoChunk = UINT32_MAX;

	struct Chunk
	{
		std::unique_ptr<char16_t[]> Units;
		uint32_t Capacity = 0;
		uint32_t Used = 0;                    // game thread only: the append position
		uint32_t LfCount = 0;                 // game thread only: LF index entries written
		std::unique_ptr<std::unique_ptr<uint32_t[]>[]> LfBlocks;

		explicit Chunk(uint32_t InCapacity)
			: Units(std::make_unique<char16_t[]>(InCapacity == 0 ? 1 : InCapacity))
			, Capacity(InCapacity)
			, LfBlocks(std::make_unique<std::unique_ptr<uint32_t[]>[]>(InCapacity / LfBlockSize + 1))
		{
		}

		uint32_t Lf(uint32_t Index) const { return LfBlocks[Index >> LfBlockShift][Index & (LfBlockSize - 1)]; }

		void PushLf(uint32_t Pos)
		{
			std::unique_ptr<uint32_t[]>& Block = LfBlocks[LfCount >> LfBlockShift];
			if (!Block)
			{
				Block = std::make_unique<uint32_t[]>(LfBlockSize);
			}
			Block[LfCount & (LfBlockSize - 1)] = Pos;
			++LfCount;
		}
	};

	struct ChunkTable
	{
		std::vector<std::shared_ptr<Chunk>> Chunks;   // index = chunk id; null once no tree of the buffer names it
	};

	struct Piece
	{
		uint32_t ChunkId = 0;
		uint32_t Start = 0;
		uint32_t Len = 0;
		uint32_t Lf = 0;
		uint32_t LfIndex = 0;
	};

	struct Node
	{
		int64_t TotalLen = 0;
		int64_t TotalLf = 0;
		size_t TotalPieces = 0;
		int32_t Count = 0;
	};

	using NodePtr = std::shared_ptr<const Node>;

	struct Leaf : Node
	{
		std::array<Piece, Fanout> Pieces;
	};

	struct Inner : Node
	{
		std::array<int64_t, Fanout> Len{};
		std::array<int64_t, Fanout> Lf{};
		std::array<NodePtr, Fanout> Kids;
	};

	/** Identity of one buffer: snapshots of the same buffer share it (ChangesBetween, InstallCompacted). */
	struct Lineage
	{
		uint8_t Unused = 0;
	};

	struct ChangeNode
	{
		ChangeSet Changes;
		std::weak_ptr<const ChangeNode> Prev;   // the ChangeSet before this one; alive while the buffer's history keeps it
	};

	struct TextRep
	{
		uint64_t Version = 0;
		NodePtr Root;                           // never null
		int32_t Height = 0;                     // 0: the root is a leaf
		std::shared_ptr<const ChunkTable> Chunks;
		std::shared_ptr<const Lineage> Owner;
		std::shared_ptr<const ChangeNode> Change;   // the ChangeSet that made this version (null for the first)
	};

	struct EditState
	{
		uint32_t Active = NoChunk;              // the add chunk typed text is appended to
		std::vector<int64_t> LiveUnits;         // per chunk id: units the current tree names
	};
}

namespace dream::textbuffer_detail
{
	using namespace dream::detail;

	const NodePtr& EmptyLeaf()
	{
		static const NodePtr Empty = std::make_shared<Leaf>();
		return Empty;
	}

	const TextRep& EmptyRep()
	{
		static const TextRep Empty = [] {
			TextRep R;
			R.Root = EmptyLeaf();
			R.Chunks = std::make_shared<ChunkTable>();
			return R;
		}();
		return Empty;
	}

	const Leaf& AsLeaf(const Node& N) { return static_cast<const Leaf&>(N); }
	const Inner& AsInner(const Node& N) { return static_cast<const Inner&>(N); }

	const char16_t* Data(const TextRep& R, const Piece& P)
	{
		return R.Chunks->Chunks[P.ChunkId]->Units.get() + P.Start;
	}

	/** LF entries of P before its unit Rel (binary search inside the piece's own entries). */
	uint32_t LfBefore(const ChunkTable& Table, const Piece& P, uint32_t Rel)
	{
		if (P.Lf == 0 || Rel == 0)
		{
			return 0;
		}
		if (Rel >= P.Len)
		{
			return P.Lf;
		}
		const Chunk& C = *Table.Chunks[P.ChunkId];
		const uint32_t Target = P.Start + Rel;
		uint32_t Lo = P.LfIndex;
		uint32_t Hi = P.LfIndex + P.Lf;
		while (Lo < Hi)
		{
			const uint32_t Mid = Lo + (Hi - Lo) / 2;
			if (C.Lf(Mid) < Target)
			{
				Lo = Mid + 1;
			}
			else
			{
				Hi = Mid;
			}
		}
		return Lo - P.LfIndex;
	}

	Piece LeftPart(const ChunkTable& Table, const Piece& P, uint32_t Rel)
	{
		return Piece{P.ChunkId, P.Start, Rel, LfBefore(Table, P, Rel), P.LfIndex};
	}

	Piece RightPart(const ChunkTable& Table, const Piece& P, uint32_t Rel)
	{
		const uint32_t Left = LfBefore(Table, P, Rel);
		return Piece{P.ChunkId, P.Start + Rel, P.Len - Rel, P.Lf - Left, P.LfIndex + Left};
	}

	/** Units of one chunk an edit took out of the tree (live-unit bookkeeping, Release). */
	struct Removal
	{
		uint32_t ChunkId;
		int64_t Units;
	};

	// ---- Lookups ---------------------------------------------------------------------------------------------

	struct Found
	{
		const Leaf* L = nullptr;
		int32_t Index = 0;
		Offset Start = 0;          // offset of the piece
		int64_t LinesBefore = 0;   // '\n's before the piece
	};

	/** The piece holding unit At, or the last piece when At is the length. L is null for the empty text. */
	Found FindByOffset(const TextRep& R, Offset At)
	{
		const Node* N = R.Root.get();
		Found F;
		for (int32_t H = R.Height; H > 0; --H)
		{
			const Inner& In = AsInner(*N);
			int32_t I = 0;
			for (; I + 1 < In.Count && At >= F.Start + In.Len[static_cast<size_t>(I)]; ++I)
			{
				F.Start += In.Len[static_cast<size_t>(I)];
				F.LinesBefore += In.Lf[static_cast<size_t>(I)];
			}
			N = In.Kids[static_cast<size_t>(I)].get();
		}
		const Leaf& L = AsLeaf(*N);
		if (L.Count == 0)
		{
			return F;
		}
		int32_t I = 0;
		for (; I + 1 < L.Count && At >= F.Start + L.Pieces[static_cast<size_t>(I)].Len; ++I)
		{
			F.Start += L.Pieces[static_cast<size_t>(I)].Len;
			F.LinesBefore += L.Pieces[static_cast<size_t>(I)].Lf;
		}
		F.L = &L;
		F.Index = I;
		return F;
	}

	/** The offset just past the K-th '\n' (1-based; 1 <= K <= total). */
	Offset OffsetAfterLf(const TextRep& R, int64_t K)
	{
		const Node* N = R.Root.get();
		Offset Base = 0;
		int64_t Seen = 0;
		for (int32_t H = R.Height; H > 0; --H)
		{
			const Inner& In = AsInner(*N);
			int32_t I = 0;
			for (; I + 1 < In.Count && Seen + In.Lf[static_cast<size_t>(I)] < K; ++I)
			{
				Seen += In.Lf[static_cast<size_t>(I)];
				Base += In.Len[static_cast<size_t>(I)];
			}
			N = In.Kids[static_cast<size_t>(I)].get();
		}
		const Leaf& L = AsLeaf(*N);
		int32_t I = 0;
		for (; I + 1 < L.Count && Seen + L.Pieces[static_cast<size_t>(I)].Lf < K; ++I)
		{
			Seen += L.Pieces[static_cast<size_t>(I)].Lf;
			Base += L.Pieces[static_cast<size_t>(I)].Len;
		}
		const Piece& P = L.Pieces[static_cast<size_t>(I)];
		const int64_t Within = std::clamp<int64_t>(K - Seen, 1, std::max<uint32_t>(P.Lf, 1u));
		const uint32_t Pos = R.Chunks->Chunks[P.ChunkId]->Lf(P.LfIndex + static_cast<uint32_t>(Within) - 1);
		return Base + static_cast<Offset>(Pos - P.Start) + 1;
	}

	/** '\n's before At (At clamped to [0, length]): the line of At. */
	int64_t LfBeforeOffset(const TextRep& R, Offset At)
	{
		const Found F = FindByOffset(R, At);
		if (F.L == nullptr)
		{
			return 0;
		}
		const Piece& P = F.L->Pieces[static_cast<size_t>(F.Index)];
		return F.LinesBefore + LfBefore(*R.Chunks, P, static_cast<uint32_t>(At - F.Start));
	}

	char16_t UnitAt(const TextRep& R, Offset At)
	{
		const Found F = FindByOffset(R, At);
		const Piece& P = F.L->Pieces[static_cast<size_t>(F.Index)];
		return Data(R, P)[At - F.Start];
	}

	/** Calls Visit(const char16_t*, size_t) for each run of [Begin, End) in order; Begin and End within the text. */
	template <class Fn>
	void VisitRuns(const TextRep& R, Offset Begin, Offset End, Fn&& Visit)
	{
		Offset At = Begin;
		while (At < End)
		{
			const Found F = FindByOffset(R, At);
			if (F.L == nullptr)
			{
				return;
			}
			Offset PieceStart = F.Start;
			for (int32_t I = F.Index; I < F.L->Count && At < End; ++I)
			{
				const Piece& P = F.L->Pieces[static_cast<size_t>(I)];
				const Offset Rel = At - PieceStart;
				const Offset Take = std::min<Offset>(static_cast<Offset>(P.Len) - Rel, End - At);
				Visit(Data(R, P) + Rel, static_cast<size_t>(Take));
				At += Take;
				PieceStart += P.Len;
			}
		}
	}

	/** Every piece in order (compaction of the piece list, linear rebuilds). */
	void CollectPieces(const Node& N, int32_t Height, std::vector<Piece>& Out)
	{
		if (Height == 0)
		{
			const Leaf& L = AsLeaf(N);
			Out.insert(Out.end(), L.Pieces.begin(), L.Pieces.begin() + L.Count);
			return;
		}
		const Inner& In = AsInner(N);
		for (int32_t I = 0; I < In.Count; ++I)
		{
			CollectPieces(*In.Kids[static_cast<size_t>(I)], Height - 1, Out);
		}
	}

	// ---- Building nodes --------------------------------------------------------------------------------------

	NodePtr MakeLeaf(const Piece* P, size_t Count)
	{
		auto L = std::make_shared<Leaf>();
		L->Count = static_cast<int32_t>(Count);
		for (size_t I = 0; I < Count; ++I)
		{
			L->Pieces[I] = P[I];
			L->TotalLen += P[I].Len;
			L->TotalLf += P[I].Lf;
		}
		L->TotalPieces = Count;
		return L;
	}

	NodePtr MakeInner(const NodePtr* Kids, size_t Count)
	{
		auto In = std::make_shared<Inner>();
		In->Count = static_cast<int32_t>(Count);
		for (size_t I = 0; I < Count; ++I)
		{
			In->Kids[I] = Kids[I];
			In->Len[I] = Kids[I]->TotalLen;
			In->Lf[I] = Kids[I]->TotalLf;
			In->TotalLen += Kids[I]->TotalLen;
			In->TotalLf += Kids[I]->TotalLf;
			In->TotalPieces += Kids[I]->TotalPieces;
		}
		return In;
	}

	/** Splits Count entries into the fewest groups of at most Fanout, sizes as equal as possible. */
	template <class Fn>
	void ForEachGroup(size_t Count, Fn&& Emit)
	{
		if (Count == 0)
		{
			return;
		}
		const size_t Groups = (Count + Fanout - 1) / Fanout;
		const size_t Base = Count / Groups;
		const size_t Extra = Count % Groups;
		size_t At = 0;
		for (size_t G = 0; G < Groups; ++G)
		{
			const size_t Size = Base + (G < Extra ? 1 : 0);
			Emit(At, Size);
			At += Size;
		}
	}

	void PackLeaves(const std::vector<Piece>& P, std::vector<NodePtr>& Out)
	{
		ForEachGroup(P.size(), [&](size_t At, size_t Size) { Out.push_back(MakeLeaf(P.data() + At, Size)); });
	}

	void PackInners(const std::vector<NodePtr>& Kids, std::vector<NodePtr>& Out)
	{
		ForEachGroup(Kids.size(), [&](size_t At, size_t Size) { Out.push_back(MakeInner(Kids.data() + At, Size)); });
	}

	/** Drops empty pieces and joins neighbours that continue each other in one chunk (typing makes one piece). */
	void Coalesce(std::vector<Piece>& P)
	{
		size_t Out = 0;
		for (size_t I = 0; I < P.size(); ++I)
		{
			if (P[I].Len == 0)
			{
				continue;
			}
			if (Out > 0)
			{
				Piece& Last = P[Out - 1];
				if (Last.ChunkId == P[I].ChunkId && Last.Start + Last.Len == P[I].Start
					&& static_cast<uint64_t>(Last.Len) + P[I].Len <= UINT32_MAX)
				{
					Last.Len += P[I].Len;
					Last.Lf += P[I].Lf;
					continue;
				}
			}
			P[Out++] = P[I];
		}
		P.resize(Out);
	}

	/** One root from the nodes of one level (Height = their height); Height is updated. */
	NodePtr Grow(std::vector<NodePtr>& Level, int32_t& Height)
	{
		if (Level.empty())
		{
			Height = 0;
			return EmptyLeaf();
		}
		while (Level.size() > 1)
		{
			std::vector<NodePtr> Up;
			PackInners(Level, Up);
			Level.swap(Up);
			++Height;
		}
		NodePtr Root = Level.front();
		while (Height > 0 && Root->Count == 1)
		{
			Root = AsInner(*Root).Kids[0];
			--Height;
		}
		return Root;
	}

	NodePtr BuildTree(std::vector<Piece>& P, int32_t& Height)
	{
		Coalesce(P);
		std::vector<NodePtr> Level;
		PackLeaves(P, Level);
		Height = 0;
		return Grow(Level, Height);
	}

	// ---- Editing (game thread) -------------------------------------------------------------------------------

	struct EditCtx
	{
		const ChunkTable* Table = nullptr;
		std::vector<Removal>* Removed = nullptr;
		std::vector<Piece> Scratch;
	};

	void CollectRemoved(const Node& N, int32_t Height, EditCtx& Ctx)
	{
		if (Height == 0)
		{
			const Leaf& L = AsLeaf(N);
			for (int32_t I = 0; I < L.Count; ++I)
			{
				Ctx.Removed->push_back(Removal{L.Pieces[static_cast<size_t>(I)].ChunkId, L.Pieces[static_cast<size_t>(I)].Len});
			}
			return;
		}
		const Inner& In = AsInner(N);
		for (int32_t I = 0; I < In.Count; ++I)
		{
			CollectRemoved(*In.Kids[static_cast<size_t>(I)], Height - 1, Ctx);
		}
	}

	/** Joins two neighbouring nodes of one height into one or two, mending the seam below for inner nodes. */
	void Join2(const NodePtr& A, const NodePtr& B, int32_t Height, std::vector<NodePtr>& Out)
	{
		if (Height == 0)
		{
			std::vector<Piece> P;
			P.reserve(static_cast<size_t>(A->Count + B->Count));
			const Leaf& LA = AsLeaf(*A);
			const Leaf& LB = AsLeaf(*B);
			P.insert(P.end(), LA.Pieces.begin(), LA.Pieces.begin() + LA.Count);
			P.insert(P.end(), LB.Pieces.begin(), LB.Pieces.begin() + LB.Count);
			Coalesce(P);
			PackLeaves(P, Out);
			return;
		}
		const Inner& IA = AsInner(*A);
		const Inner& IB = AsInner(*B);
		std::vector<NodePtr> Kids;
		Kids.reserve(static_cast<size_t>(IA.Count + IB.Count));
		Kids.insert(Kids.end(), IA.Kids.begin(), IA.Kids.begin() + IA.Count);
		Kids.insert(Kids.end(), IB.Kids.begin(), IB.Kids.begin() + IB.Count);
		const size_t Seam = static_cast<size_t>(IA.Count);
		if (Kids[Seam - 1]->Count < MinFill || Kids[Seam]->Count < MinFill)
		{
			std::vector<NodePtr> Mended;
			Join2(Kids[Seam - 1], Kids[Seam], Height - 1, Mended);
			Kids.erase(Kids.begin() + static_cast<std::ptrdiff_t>(Seam - 1), Kids.begin() + static_cast<std::ptrdiff_t>(Seam + 1));
			Kids.insert(Kids.begin() + static_cast<std::ptrdiff_t>(Seam - 1), Mended.begin(), Mended.end());
		}
		PackInners(Kids, Out);
	}

	/** Merges underfull nodes in Kids[Lo - 1 .. Hi] (the new ones and their neighbours) into a sibling. */
	void Normalize(std::vector<NodePtr>& Kids, int32_t Height, size_t Lo, size_t Hi)
	{
		size_t I = Lo > 0 ? Lo - 1 : 0;
		size_t End = std::min(Hi + 1, Kids.size());
		while (I < End && Kids.size() > 1)
		{
			if (Kids[I]->Count >= MinFill)
			{
				++I;
				continue;
			}
			const size_t Left = I + 1 < Kids.size() ? I : I - 1;
			std::vector<NodePtr> Joined;
			Join2(Kids[Left], Kids[Left + 1], Height, Joined);
			Kids.erase(Kids.begin() + static_cast<std::ptrdiff_t>(Left), Kids.begin() + static_cast<std::ptrdiff_t>(Left + 2));
			Kids.insert(Kids.begin() + static_cast<std::ptrdiff_t>(Left), Joined.begin(), Joined.end());
			End = std::min(End + Joined.size() - 2 + 1, Kids.size());   // the joined nodes replace two
			I = Left;
			if (Joined.size() > 1 || Joined.front()->Count >= MinFill)
			{
				I = Left + Joined.size();
			}
		}
	}

	void Splice(const Node& N, int32_t Height, Offset From, Offset To, const std::vector<Piece>& Ins, std::vector<NodePtr>& Out, EditCtx& Ctx);

	void SpliceLeaf(const Leaf& L, Offset From, Offset To, const std::vector<Piece>& Ins, std::vector<NodePtr>& Out, EditCtx& Ctx)
	{
		std::vector<Piece>& P = Ctx.Scratch;
		P.clear();
		P.reserve(static_cast<size_t>(L.Count) + Ins.size() + 2);
		bool bInserted = false;
		Offset Pos = 0;
		for (int32_t I = 0; I < L.Count; ++I)
		{
			const Piece& Q = L.Pieces[static_cast<size_t>(I)];
			const Offset QS = Pos;
			const Offset QE = Pos + Q.Len;
			Pos = QE;
			if (QE <= From)
			{
				P.push_back(Q);
				continue;
			}
			if (QS >= To)
			{
				if (!bInserted)
				{
					P.insert(P.end(), Ins.begin(), Ins.end());
					bInserted = true;
				}
				P.push_back(Q);
				continue;
			}
			if (QS < From)
			{
				P.push_back(LeftPart(*Ctx.Table, Q, static_cast<uint32_t>(From - QS)));
			}
			if (!bInserted)
			{
				P.insert(P.end(), Ins.begin(), Ins.end());
				bInserted = true;
			}
			const Offset Gone = std::min(QE, To) - std::max(QS, From);
			if (Gone > 0)
			{
				Ctx.Removed->push_back(Removal{Q.ChunkId, Gone});
			}
			if (QE > To)
			{
				P.push_back(RightPart(*Ctx.Table, Q, static_cast<uint32_t>(To - QS)));
			}
		}
		if (!bInserted)
		{
			P.insert(P.end(), Ins.begin(), Ins.end());
		}
		Coalesce(P);
		PackLeaves(P, Out);
	}

	void SpliceInner(const Inner& N, int32_t Height, Offset From, Offset To, const std::vector<Piece>& Ins, std::vector<NodePtr>& Out, EditCtx& Ctx)
	{
		int32_t A = 0;
		Offset SA = 0;
		if (From == To)
		{
			// An insert goes to the child it ends (left bias), so typing continues the piece it extends.
			for (; A + 1 < N.Count && From > SA + N.Len[static_cast<size_t>(A)]; ++A)
			{
				SA += N.Len[static_cast<size_t>(A)];
			}
		}
		else
		{
			for (; A + 1 < N.Count && From >= SA + N.Len[static_cast<size_t>(A)]; ++A)
			{
				SA += N.Len[static_cast<size_t>(A)];
			}
		}
		int32_t B = A;
		Offset SB = SA;
		while (B + 1 < N.Count && To > SB + N.Len[static_cast<size_t>(B)])
		{
			SB += N.Len[static_cast<size_t>(B)];
			++B;
		}
		std::vector<NodePtr> Kids;
		Kids.reserve(static_cast<size_t>(N.Count) + 4);
		Kids.insert(Kids.end(), N.Kids.begin(), N.Kids.begin() + A);
		const size_t Lo = Kids.size();
		if (A == B)
		{
			Splice(*N.Kids[static_cast<size_t>(A)], Height - 1, From - SA, To - SA, Ins, Kids, Ctx);
		}
		else
		{
			Splice(*N.Kids[static_cast<size_t>(A)], Height - 1, From - SA, N.Len[static_cast<size_t>(A)], Ins, Kids, Ctx);
			for (int32_t I = A + 1; I < B; ++I)
			{
				CollectRemoved(*N.Kids[static_cast<size_t>(I)], Height - 1, Ctx);
			}
			static const std::vector<Piece> None;
			Splice(*N.Kids[static_cast<size_t>(B)], Height - 1, 0, To - SB, None, Kids, Ctx);
		}
		const size_t Hi = Kids.size();
		Kids.insert(Kids.end(), N.Kids.begin() + B + 1, N.Kids.begin() + N.Count);
		Normalize(Kids, Height - 1, Lo, Hi);
		PackInners(Kids, Out);
	}

	void Splice(const Node& N, int32_t Height, Offset From, Offset To, const std::vector<Piece>& Ins, std::vector<NodePtr>& Out, EditCtx& Ctx)
	{
		if (Height == 0)
		{
			SpliceLeaf(AsLeaf(N), From, To, Ins, Out, Ctx);
		}
		else
		{
			SpliceInner(AsInner(N), Height, From, To, Ins, Out, Ctx);
		}
	}

	/** Replaces [From, To) of the tree with Ins, copying only the nodes on the paths. */
	void SpliceRoot(NodePtr& Root, int32_t& Height, Offset From, Offset To, const std::vector<Piece>& Ins, EditCtx& Ctx)
	{
		std::vector<NodePtr> Top;
		Splice(*Root, Height, From, To, Ins, Top, Ctx);
		Normalize(Top, Height, 0, Top.size());
		Root = Grow(Top, Height);
	}

	/** A new chunk in the table (a free slot when there is one); returns its id. */
	uint32_t AddChunk(EditState& E, std::shared_ptr<const ChunkTable>& Table, uint32_t Capacity)
	{
		auto Next = std::make_shared<ChunkTable>(*Table);
		uint32_t Id = 0;
		while (Id < Next->Chunks.size() && Next->Chunks[Id])
		{
			++Id;
		}
		if (Id == Next->Chunks.size())
		{
			Next->Chunks.emplace_back();
		}
		Next->Chunks[Id] = std::make_shared<Chunk>(Capacity);
		if (E.LiveUnits.size() < Next->Chunks.size())
		{
			E.LiveUnits.resize(Next->Chunks.size(), 0);
		}
		E.LiveUnits[Id] = 0;
		Table = std::move(Next);
		return Id;
	}

	Piece WriteUnits(Chunk& C, uint32_t Id, std::u16string_view Text)
	{
		Piece P;
		P.ChunkId = Id;
		P.Start = C.Used;
		P.Len = static_cast<uint32_t>(Text.size());
		P.LfIndex = C.LfCount;
		char16_t* Out = C.Units.get() + C.Used;
		for (size_t I = 0; I < Text.size(); ++I)
		{
			Out[I] = Text[I];
			if (Text[I] == u'\n')
			{
				C.PushLf(C.Used + static_cast<uint32_t>(I));
			}
		}
		P.Lf = C.LfCount - P.LfIndex;
		C.Used += P.Len;
		return P;
	}

	/**
	 * Appends Text to the add chunks (or a chunk of its own when it is large) and returns its pieces. An add chunk that
	 * fills up is reported in Removed with 0 units, so Release drops it once no tree of the buffer names it.
	 */
	void AppendText(EditState& E, std::shared_ptr<const ChunkTable>& Table, std::u16string_view Text, std::vector<Piece>& Out,
		std::vector<Removal>& Removed)
	{
		while (!Text.empty())
		{
			Chunk* Active = E.Active != NoChunk ? Table->Chunks[E.Active].get() : nullptr;
			const uint32_t Free = Active != nullptr ? Active->Capacity - Active->Used : 0;
			if (Text.size() > Free && Text.size() > AddChunkUnits / 2)
			{
				const uint32_t Id = AddChunk(E, Table, static_cast<uint32_t>(Text.size()));
				const Piece P = WriteUnits(*Table->Chunks[Id], Id, Text);
				E.LiveUnits[Id] += P.Len;
				Out.push_back(P);
				return;
			}
			size_t Take = std::min<size_t>(Text.size(), Free);
			if (Take < Text.size() && Take > 0 && utf::IsHighSurrogate(Text[Take - 1]))
			{
				--Take;   // never split a surrogate pair across chunks
			}
			if (Take == 0)
			{
				if (E.Active != NoChunk)
				{
					Removed.push_back(Removal{E.Active, 0});
				}
				E.Active = AddChunk(E, Table, AddChunkUnits);
				continue;
			}
			const Piece P = WriteUnits(*Active, E.Active, Text.substr(0, Take));
			E.LiveUnits[E.Active] += P.Len;
			Out.push_back(P);
			Text.remove_prefix(Take);
		}
	}

	/** Subtracts removed units; a chunk no tree of the buffer names any more leaves the table (old snapshots keep theirs). */
	void Release(EditState& E, std::shared_ptr<const ChunkTable>& Table, const std::vector<Removal>& Removed)
	{
		std::shared_ptr<ChunkTable> Next;
		for (const Removal& R : Removed)
		{
			int64_t& Live = E.LiveUnits[R.ChunkId];
			Live -= R.Units;
			if (Live <= 0 && R.ChunkId != E.Active && Table->Chunks[R.ChunkId] && (!Next || Next->Chunks[R.ChunkId]))
			{
				if (!Next)
				{
					Next = std::make_shared<ChunkTable>(*Table);
				}
				Next->Chunks[R.ChunkId].reset();
				Live = 0;
			}
		}
		if (Next)
		{
			Table = std::move(Next);
		}
	}

	bool SplitsPair(const TextRep& R, Offset At)
	{
		return At > 0 && At < R.Root->TotalLen && utf::IsHighSurrogate(UnitAt(R, At - 1)) && utf::IsLowSurrogate(UnitAt(R, At));
	}

	/** The batch's effective ranges (no-op ranges dropped), stably sorted by At. */
	std::vector<const EditRange*> SortedRanges(const EditBatch& Batch)
	{
		std::vector<const EditRange*> Out;
		Out.reserve(Batch.Ranges.size());
		for (const EditRange& R : Batch.Ranges)
		{
			if (R.DeleteLen != 0 || !R.Insert.empty())
			{
				Out.push_back(&R);
			}
		}
		std::stable_sort(Out.begin(), Out.end(), [](const EditRange* A, const EditRange* B) { return A->At < B->At; });
		return Out;
	}

	LineNo CountNewlines(std::u16string_view Text)
	{
		return static_cast<LineNo>(std::count(Text.begin(), Text.end(), u'\n'));
	}

	/** A batch with many ranges (Replace All) rebuilds the piece list in one pass instead of one path copy per range. */
	void ApplyLinear(const TextRep& Old, const std::vector<const EditRange*>& Sorted, EditState& E, std::shared_ptr<const ChunkTable>& Table,
		std::vector<Removal>& Removed, NodePtr& Root, int32_t& Height)
	{
		std::vector<Piece> All;
		All.reserve(Old.Root->TotalPieces);
		CollectPieces(*Old.Root, Old.Height, All);
		std::vector<Piece> Out;
		Out.reserve(All.size() + Sorted.size() * 3);
		size_t Next = 0;
		Piece Cur;
		bool bHave = false;
		Offset CurStart = 0;
		const auto Advance = [&] {
			if (bHave)
			{
				CurStart += Cur.Len;
			}
			bHave = Next < All.size();
			if (bHave)
			{
				Cur = All[Next++];
			}
		};
		Advance();
		for (const EditRange* R : Sorted)
		{
			while (bHave && CurStart + Cur.Len <= R->At)
			{
				Out.push_back(Cur);
				Advance();
			}
			if (bHave && CurStart < R->At)
			{
				const uint32_t Rel = static_cast<uint32_t>(R->At - CurStart);
				Out.push_back(LeftPart(*Table, Cur, Rel));
				Cur = RightPart(*Table, Cur, Rel);
				CurStart = R->At;
			}
			AppendText(E, Table, R->Insert, Out, Removed);
			const Offset DelEnd = R->At + R->DeleteLen;
			while (bHave && CurStart + Cur.Len <= DelEnd)
			{
				Removed.push_back(Removal{Cur.ChunkId, Cur.Len});
				Advance();
			}
			if (bHave && CurStart < DelEnd)
			{
				const uint32_t Rel = static_cast<uint32_t>(DelEnd - CurStart);
				Removed.push_back(Removal{Cur.ChunkId, Rel});
				Cur = RightPart(*Table, Cur, Rel);
				CurStart = DelEnd;
			}
		}
		while (bHave)
		{
			Out.push_back(Cur);
			Advance();
		}
		Root = BuildTree(Out, Height);
	}

	std::shared_ptr<const TextRep> MakeInitialRep(std::u16string_view Text, EditState& E, const std::shared_ptr<const Lineage>& Owner)
	{
		auto Rep = std::make_shared<TextRep>();
		Rep->Version = 1;
		Rep->Owner = Owner;
		std::shared_ptr<const ChunkTable> Table = std::make_shared<ChunkTable>();
		std::vector<Piece> Pieces;
		if (!Text.empty())
		{
			const uint32_t Id = AddChunk(E, Table, static_cast<uint32_t>(Text.size()));
			Pieces.push_back(WriteUnits(*Table->Chunks[Id], Id, Text));
			E.LiveUnits[Id] = static_cast<int64_t>(Text.size());
		}
		Rep->Chunks = std::move(Table);
		Rep->Root = BuildTree(Pieces, Rep->Height);
		return Rep;
	}

	// ---- ChangesBetween --------------------------------------------------------------------------------------

	/** A changed region: Cur* in the newer text, Orig* in From's text; outside regions the texts are equal. */
	struct Region
	{
		Offset CurStart = 0;
		int64_t CurLen = 0;
		Offset OrigStart = 0;
		int64_t OrigLen = 0;
	};

	void ComposeRange(std::vector<Region>& Regions, Offset A, int64_t D, int64_t I)
	{
		// Regions touching [A, A + D] merge with the range; the ones after it shift.
		size_t First = 0;
		int64_t DeltaBefore = 0;
		while (First < Regions.size() && Regions[First].CurStart + Regions[First].CurLen < A)
		{
			DeltaBefore += Regions[First].CurLen - Regions[First].OrigLen;
			++First;
		}
		size_t Last = First;
		int64_t DeltaInside = 0;
		while (Last < Regions.size() && Regions[Last].CurStart <= A + D)
		{
			DeltaInside += Regions[Last].CurLen - Regions[Last].OrigLen;
			++Last;
		}
		Region New;
		Offset S = A;
		Offset E = A + D;
		Offset OS = A - DeltaBefore;
		Offset OE = A + D - DeltaBefore - DeltaInside;
		if (Last > First)
		{
			const Region& F = Regions[First];
			const Region& L = Regions[Last - 1];
			if (F.CurStart < S)
			{
				S = F.CurStart;
				OS = F.OrigStart;
			}
			if (L.CurStart + L.CurLen > E)
			{
				E = L.CurStart + L.CurLen;
				OE = L.OrigStart + L.OrigLen;
			}
		}
		New.CurStart = S;
		New.CurLen = (E - S) - D + I;
		New.OrigStart = OS;
		New.OrigLen = OE - OS;
		Regions.erase(Regions.begin() + static_cast<std::ptrdiff_t>(First), Regions.begin() + static_cast<std::ptrdiff_t>(Last));
		Regions.insert(Regions.begin() + static_cast<std::ptrdiff_t>(First), New);
		for (size_t K = First + 1; K < Regions.size(); ++K)
		{
			Regions[K].CurStart += I - D;
		}
	}
}

namespace dream
{
	namespace tb = textbuffer_detail;

	// ---- TextSnapshot ---------------------------------------------------------------------------------------

	TextSnapshot::TextSnapshot() = default;

	TextSnapshot::TextSnapshot(std::shared_ptr<const detail::TextRep> InRep)
		: Rep(std::move(InRep))
	{
	}

	uint64_t TextSnapshot::Version() const
	{
		return Rep ? Rep->Version : 0;
	}

	int64_t TextSnapshot::Length() const
	{
		return Rep ? Rep->Root->TotalLen : 0;
	}

	LineNo TextSnapshot::LineCount() const
	{
		return Rep ? static_cast<LineNo>(Rep->Root->TotalLf + 1) : 1;
	}

	Offset TextSnapshot::LineStart(LineNo Line) const
	{
		const detail::TextRep& R = Rep ? *Rep : tb::EmptyRep();
		Line = std::clamp<LineNo>(Line, 0, static_cast<LineNo>(R.Root->TotalLf));
		return Line == 0 ? 0 : tb::OffsetAfterLf(R, Line);
	}

	Offset TextSnapshot::LineEnd(LineNo Line) const
	{
		const detail::TextRep& R = Rep ? *Rep : tb::EmptyRep();
		const LineNo Last = static_cast<LineNo>(R.Root->TotalLf);
		Line = std::clamp<LineNo>(Line, 0, Last);
		return Line < Last ? tb::OffsetAfterLf(R, static_cast<int64_t>(Line) + 1) - 1 : R.Root->TotalLen;
	}

	int32_t TextSnapshot::LineLength(LineNo Line) const
	{
		return static_cast<int32_t>(LineEnd(Line) - LineStart(Line));
	}

	LineNo TextSnapshot::LineOf(Offset At) const
	{
		const detail::TextRep& R = Rep ? *Rep : tb::EmptyRep();
		At = std::clamp<Offset>(At, 0, R.Root->TotalLen);
		return static_cast<LineNo>(tb::LfBeforeOffset(R, At));
	}

	std::u16string_view TextSnapshot::LineView(LineNo Line, std::u16string& Scratch) const
	{
		const detail::TextRep& R = Rep ? *Rep : tb::EmptyRep();
		const Offset Start = LineStart(Line);
		const Offset End = LineEnd(Line);
		if (End <= Start)
		{
			return std::u16string_view();
		}
		const tb::Found F = tb::FindByOffset(R, Start);
		const detail::Piece& P = F.L->Pieces[static_cast<size_t>(F.Index)];
		if (End <= F.Start + P.Len)
		{
			return std::u16string_view(tb::Data(R, P) + (Start - F.Start), static_cast<size_t>(End - Start));
		}
		Scratch.clear();
		Scratch.reserve(static_cast<size_t>(End - Start));
		tb::VisitRuns(R, Start, End, [&Scratch](const char16_t* D, size_t N) { Scratch.append(D, N); });
		return Scratch;
	}

	std::u16string TextSnapshot::LineText(LineNo Line) const
	{
		std::u16string Scratch;
		return std::u16string(LineView(Line, Scratch));
	}

	void TextSnapshot::CopyRange(Offset At, int64_t Len, std::u16string& Out) const
	{
		const detail::TextRep& R = Rep ? *Rep : tb::EmptyRep();
		const Offset Size = R.Root->TotalLen;
		const Offset Begin = std::clamp<Offset>(At, 0, Size);
		const Offset End = std::clamp<Offset>(At + std::max<int64_t>(Len, 0), Begin, Size);
		Out.reserve(Out.size() + static_cast<size_t>(End - Begin));
		tb::VisitRuns(R, Begin, End, [&Out](const char16_t* D, size_t N) { Out.append(D, N); });
	}

	std::u16string_view TextSnapshot::Chunk(Offset At, int64_t MaxLen) const
	{
		const detail::TextRep& R = Rep ? *Rep : tb::EmptyRep();
		const Offset Size = R.Root->TotalLen;
		const Offset Begin = std::clamp<Offset>(At, 0, Size);
		const Offset End = std::clamp<Offset>(Begin + std::max<int64_t>(MaxLen, 0), Begin, Size);
		if (End <= Begin)
		{
			return std::u16string_view();
		}
		const tb::Found F = tb::FindByOffset(R, Begin);
		const detail::Piece& P = F.L->Pieces[static_cast<size_t>(F.Index)];
		const Offset RunEnd = std::min<Offset>(End, F.Start + P.Len);
		return std::u16string_view(tb::Data(R, P) + (Begin - F.Start), static_cast<size_t>(RunEnd - Begin));
	}

	std::u16string TextSnapshot::Text() const
	{
		std::u16string Out;
		if (Rep)
		{
			Out.reserve(static_cast<size_t>(Rep->Root->TotalLen));
			tb::VisitRuns(*Rep, 0, Rep->Root->TotalLen, [&Out](const char16_t* D, size_t N) { Out.append(D, N); });
		}
		return Out;
	}

	std::string TextSnapshot::TextUtf8() const
	{
		std::string Out;
		if (!Rep)
		{
			return Out;
		}
		Out.reserve(static_cast<size_t>(Rep->Root->TotalLen));
		char16_t Carry = 0;   // a high surrogate at the end of a run, joined with the next run's first unit
		tb::VisitRuns(*Rep, 0, Rep->Root->TotalLen, [&Out, &Carry](const char16_t* D, size_t N) {
			std::u16string_view Run(D, N);
			if (Carry != 0)
			{
				const char16_t Pair[2] = {Carry, Run.front()};
				const bool bJoin = utf::IsLowSurrogate(Run.front());
				utf::Utf16ToUtf8(std::u16string_view(Pair, bJoin ? 2 : 1), Out);
				Carry = 0;
				if (bJoin)
				{
					Run.remove_prefix(1);
				}
			}
			if (!Run.empty() && utf::IsHighSurrogate(Run.back()))
			{
				Carry = Run.back();
				Run.remove_suffix(1);
			}
			utf::Utf16ToUtf8(Run, Out);
		});
		if (Carry != 0)
		{
			utf::Utf16ToUtf8(std::u16string_view(&Carry, 1), Out);
		}
		return Out;
	}

	bool TextSnapshot::SameRepAs(const TextSnapshot& Other) const
	{
		return Rep.get() == Other.Rep.get();
	}

	size_t TextSnapshot::PieceCount() const
	{
		return Rep ? Rep->Root->TotalPieces : 0;
	}

	int32_t TextSnapshot::TreeDepth() const
	{
		return Rep ? Rep->Height : 0;
	}

	// ---- TextBuffer -----------------------------------------------------------------------------------------

	TextBuffer::TextBuffer()
		: TextBuffer(std::u16string_view())
	{
	}

	TextBuffer::TextBuffer(std::u16string_view Initial)
		: Edit(std::make_unique<detail::EditState>())
	{
		Current = tb::MakeInitialRep(Initial, *Edit, std::make_shared<detail::Lineage>());
	}

	TextBuffer::~TextBuffer() = default;
	TextBuffer::TextBuffer(TextBuffer&& Other) noexcept = default;
	TextBuffer& TextBuffer::operator=(TextBuffer&& Other) noexcept = default;

	TextSnapshot TextBuffer::Snapshot() const
	{
		return TextSnapshot(Current);
	}

	uint64_t TextBuffer::Version() const
	{
		return Current->Version;
	}

	int64_t TextBuffer::Length() const
	{
		return Current->Root->TotalLen;
	}

	LineNo TextBuffer::LineCount() const
	{
		return static_cast<LineNo>(Current->Root->TotalLf + 1);
	}

	bool TextBuffer::Validate(const EditBatch& Batch) const
	{
		const detail::TextRep& R = *Current;
		const Offset Size = R.Root->TotalLen;
		for (const EditRange& E : Batch.Ranges)
		{
			if (E.At < 0 || E.DeleteLen < 0 || E.At > Size || E.DeleteLen > Size - E.At)
			{
				return false;
			}
			if (E.Insert.find(u'\r') != std::u16string::npos)
			{
				return false;
			}
			if (tb::SplitsPair(R, E.At) || tb::SplitsPair(R, E.At + E.DeleteLen))
			{
				return false;
			}
		}
		const std::vector<const EditRange*> Sorted = tb::SortedRanges(Batch);
		for (size_t I = 1; I < Sorted.size(); ++I)
		{
			if (Sorted[I - 1]->At + Sorted[I - 1]->DeleteLen > Sorted[I]->At)
			{
				return false;
			}
		}
		return true;
	}

	ChangeSet TextBuffer::Apply(const EditBatch& Batch)
	{
		ChangeSet Changes;
		Changes.VersionBefore = Current->Version;
		Changes.VersionAfter = Current->Version;
		Changes.Origin = Batch.Origin;
		if (!Validate(Batch))
		{
			return Changes;
		}
		const std::vector<const EditRange*> Sorted = tb::SortedRanges(Batch);
		if (Sorted.empty())
		{
			return Changes;
		}

		const std::shared_ptr<const detail::TextRep> Old = Current;
		const TextSnapshot OldSnap(Old);
		Changes.Ranges.reserve(Sorted.size());
		for (const EditRange* E : Sorted)
		{
			ChangeRange C;
			C.At = E->At;
			C.OldLen = E->DeleteLen;
			C.NewLen = static_cast<int64_t>(E->Insert.size());
			C.StartLine = static_cast<LineNo>(tb::LfBeforeOffset(*Old, E->At));
			C.StartCol16 = static_cast<int32_t>(E->At - (C.StartLine == 0 ? 0 : tb::OffsetAfterLf(*Old, C.StartLine)));
			C.OldLines = E->DeleteLen == 0 ? 0 : static_cast<LineNo>(tb::LfBeforeOffset(*Old, E->At + E->DeleteLen)) - C.StartLine;
			C.NewLines = tb::CountNewlines(E->Insert);
			Changes.Ranges.push_back(C);
		}

		std::shared_ptr<const detail::ChunkTable> Table = Old->Chunks;
		detail::NodePtr Root = Old->Root;
		int32_t Height = Old->Height;
		std::vector<tb::Removal> Removed;
		if (Sorted.size() > 16 && Sorted.size() * 64 > Old->Root->TotalPieces)
		{
			tb::ApplyLinear(*Old, Sorted, *Edit, Table, Removed, Root, Height);
		}
		else
		{
			tb::EditCtx Ctx;
			Ctx.Removed = &Removed;
			std::vector<detail::Piece> Ins;
			for (auto It = Sorted.rbegin(); It != Sorted.rend(); ++It)
			{
				const EditRange& E = **It;
				Ins.clear();
				tb::AppendText(*Edit, Table, E.Insert, Ins, Removed);
				Ctx.Table = Table.get();
				tb::SpliceRoot(Root, Height, E.At, E.At + E.DeleteLen, Ins, Ctx);
			}
		}
		tb::Release(*Edit, Table, Removed);

		Changes.VersionAfter = Old->Version + 1;
		auto Node = std::make_shared<detail::ChangeNode>();
		Node->Changes = Changes;
		Node->Prev = Old->Change;
		auto New = std::make_shared<detail::TextRep>();
		New->Version = Changes.VersionAfter;
		New->Root = std::move(Root);
		New->Height = Height;
		New->Chunks = std::move(Table);
		New->Owner = Old->Owner;
		New->Change = Node;
		Current = std::move(New);
		PushHistory(Node);
		if (!AnchorStore.IsEmpty())
		{
			AnchorStore.MapThrough(Changes, OldSnap, TextSnapshot(Current));
		}
		return Changes;
	}

	ChangeSet TextBuffer::SetText(std::u16string_view NewText, EditOrigin Origin)
	{
		EditBatch Batch;
		Batch.Origin = Origin;
		Batch.Ranges.push_back(EditRange{0, Length(), std::u16string(NewText)});
		return Apply(Batch);
	}

	int32_t TextBuffer::Utf8ColToUtf16(LineNo Line, int32_t ByteCol) const
	{
		std::u16string Scratch;
		return utf::Utf8ColToUtf16(Snapshot().LineView(Line, Scratch), ByteCol);
	}

	int32_t TextBuffer::Utf16ColToUtf8(LineNo Line, int32_t Col16) const
	{
		std::u16string Scratch;
		return utf::Utf16ColToUtf8(Snapshot().LineView(Line, Scratch), Col16);
	}

	int32_t TextBuffer::CodepointColToUtf16(LineNo Line, int32_t Col) const
	{
		std::u16string Scratch;
		return utf::CodepointColToUtf16(Snapshot().LineView(Line, Scratch), Col);
	}

	void TextBuffer::PushHistory(const std::shared_ptr<const detail::ChangeNode>& Node)
	{
		History.push_back(Node);
		while (History.size() > HistoryLimit)
		{
			History.pop_front();
		}
	}

	LineNo TextBuffer::MapLine(uint64_t FromVersion, LineNo Line) const
	{
		// A line is deleted when one edit (or adjacent edits) removes all of it including its '\n' from its first
		// column; otherwise it maps to the line that holds its first surviving character, and text inserted at its
		// first column goes above it. The last line has no '\n' and is never deleted (slice A rule, T-CORE-040).
		const LineNo Count = LineCount();
		if (Line < 0)
		{
			return InvalidLine;
		}
		if (FromVersion == Version())
		{
			return std::min(Line, Count - 1);
		}
		if (FromVersion > Version() || History.empty() || FromVersion < History.front()->Changes.VersionBefore)
		{
			return InvalidLine;
		}
		size_t First = static_cast<size_t>(FromVersion - History.front()->Changes.VersionBefore);
		if (First >= History.size() || History[First]->Changes.VersionBefore != FromVersion)
		{
			First = 0;
			while (First < History.size() && History[First]->Changes.VersionBefore != FromVersion)
			{
				++First;
			}
			if (First == History.size())
			{
				return InvalidLine;
			}
		}
		LineNo L = Line;
		for (size_t H = First; H < History.size(); ++H)
		{
			const std::vector<ChangeRange>& Ranges = History[H]->Changes.Ranges;
			LineNo Delta = 0;
			LineNo Result = InvalidLine;
			bool bDone = false;
			for (size_t I = 0; I < Ranges.size() && !bDone; ++I)
			{
				const ChangeRange& R = Ranges[I];
				if (L < R.StartLine || (L == R.StartLine && R.StartCol16 > 0))
				{
					Result = L + Delta;          // the line starts before this range
					bDone = true;
					break;
				}
				const LineNo EndLine = R.StartLine + R.OldLines;
				if (L < EndLine)
				{
					return InvalidLine;          // its first column and its '\n' are inside the deleted text
				}
				Delta += R.NewLines - R.OldLines;
				if (L == EndLine)
				{
					// The first surviving character follows this range, unless an adjacent range removes more.
					size_t J = I;
					while (J + 1 < Ranges.size() && Ranges[J + 1].At == Ranges[J].At + Ranges[J].OldLen)
					{
						const ChangeRange& Next = Ranges[J + 1];
						if (L < Next.StartLine + Next.OldLines)
						{
							return InvalidLine;
						}
						Delta += Next.NewLines - Next.OldLines;
						++J;
					}
					Result = L + Delta;
					bDone = true;
				}
			}
			L = bDone ? Result : L + Delta;
		}
		return std::clamp<LineNo>(L, 0, Count - 1);
	}

	void TextBuffer::SetHistoryLimit(size_t MaxChangeSets)
	{
		HistoryLimit = MaxChangeSets;
		while (History.size() > HistoryLimit)
		{
			History.pop_front();
		}
	}

	uint64_t TextBuffer::OldestMappableVersion() const
	{
		return History.empty() ? Version() : History.front()->Changes.VersionBefore;
	}

	bool TextBuffer::ChangesBetween(const TextSnapshot& From, const TextSnapshot& To, EditBatch& Out)
	{
		Out.Ranges.clear();
		Out.Origin = EditOrigin::Command;
		if (!From.Rep || !To.Rep || From.Rep->Owner != To.Rep->Owner || From.Version() > To.Version())
		{
			return false;
		}
		if (From.Version() == To.Version())
		{
			return true;
		}
		std::vector<std::shared_ptr<const detail::ChangeNode>> Chain;   // newest first
		std::shared_ptr<const detail::ChangeNode> Node = To.Rep->Change;
		while (Node && Node->Changes.VersionBefore > From.Version())
		{
			std::shared_ptr<const detail::ChangeNode> Prev = Node->Prev.lock();
			Chain.push_back(std::move(Node));
			Node = std::move(Prev);
		}
		if (!Node || Node->Changes.VersionBefore != From.Version())
		{
			return false;   // a ChangeSet in between is gone (history horizon)
		}
		Chain.push_back(std::move(Node));

		std::vector<tb::Region> Regions;
		for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
		{
			const std::vector<ChangeRange>& Ranges = (*It)->Changes.Ranges;
			for (auto R = Ranges.rbegin(); R != Ranges.rend(); ++R)
			{
				tb::ComposeRange(Regions, R->At, R->OldLen, R->NewLen);
			}
		}
		std::u16string OldText;
		for (const tb::Region& G : Regions)
		{
			EditRange E;
			To.CopyRange(G.CurStart, G.CurLen, E.Insert);
			OldText.clear();
			From.CopyRange(G.OrigStart, G.OrigLen, OldText);
			// Trim what both sides share, never between the halves of a surrogate pair.
			size_t Prefix = 0;
			const size_t Most = std::min(OldText.size(), E.Insert.size());
			while (Prefix < Most && OldText[Prefix] == E.Insert[Prefix])
			{
				++Prefix;
			}
			if (Prefix > 0 && Prefix < OldText.size() && utf::IsLowSurrogate(OldText[Prefix]))
			{
				--Prefix;
			}
			size_t Suffix = 0;
			while (Suffix < Most - Prefix && OldText[OldText.size() - 1 - Suffix] == E.Insert[E.Insert.size() - 1 - Suffix])
			{
				++Suffix;
			}
			if (Suffix > 0 && Suffix < OldText.size() && utf::IsHighSurrogate(OldText[OldText.size() - 1 - Suffix]))
			{
				--Suffix;
			}
			if (OldText.size() - Prefix - Suffix == 0 && E.Insert.size() - Prefix - Suffix == 0)
			{
				continue;
			}
			E.At = G.OrigStart + static_cast<Offset>(Prefix);
			E.DeleteLen = static_cast<int64_t>(OldText.size() - Prefix - Suffix);
			E.Insert = E.Insert.substr(Prefix, E.Insert.size() - Prefix - Suffix);
			Out.Ranges.push_back(std::move(E));
		}
		return true;
	}

	bool TextBuffer::NeedsCompaction() const
	{
		const detail::TextRep& R = *Current;
		if (R.Root->TotalPieces > 2 * static_cast<size_t>(R.Root->TotalLf + 1) + 4096)
		{
			return true;
		}
		uint64_t Allocated = 0;
		for (const std::shared_ptr<detail::Chunk>& C : R.Chunks->Chunks)
		{
			Allocated += C ? C->Capacity : 0;
		}
		return Allocated > 2 * static_cast<uint64_t>(R.Root->TotalLen) + (1u << 20);
	}

	TextSnapshot TextBuffer::BuildCompacted(const TextSnapshot& From)
	{
		if (!From.Rep)
		{
			return From;
		}
		const detail::TextRep& Src = *From.Rep;
		auto Rep = std::make_shared<detail::TextRep>();
		Rep->Version = Src.Version;
		Rep->Owner = Src.Owner;
		Rep->Change = Src.Change;
		auto Table = std::make_shared<detail::ChunkTable>();
		std::vector<detail::Piece> Pieces;
		const Offset Size = Src.Root->TotalLen;
		if (Size > 0)
		{
			auto C = std::make_shared<detail::Chunk>(static_cast<uint32_t>(Size));
			tb::VisitRuns(Src, 0, Size, [&C](const char16_t* D, size_t N) { tb::WriteUnits(*C, 0, std::u16string_view(D, N)); });
			Pieces.push_back(detail::Piece{0, 0, C->Used, C->LfCount, 0});
			Table->Chunks.push_back(std::move(C));
		}
		Rep->Chunks = std::move(Table);
		Rep->Root = tb::BuildTree(Pieces, Rep->Height);
		return TextSnapshot(std::move(Rep));
	}

	bool TextBuffer::InstallCompacted(TextSnapshot Built, uint64_t BuiltFromVersion)
	{
		if (!Built.Rep || Built.Rep->Owner != Current->Owner || Built.Rep->Version != BuiltFromVersion
			|| Current->Version != BuiltFromVersion || Built.Rep->Root->TotalLen != Current->Root->TotalLen)
		{
			return false;
		}
		// Only BuildCompacted's layout (at most one chunk, one piece starting at its unit 0) may reset the live-unit
		// bookkeeping below; any other snapshot (Snapshot() itself) would let later edits release chunks it still names.
		const std::vector<std::shared_ptr<detail::Chunk>>& BuiltChunks = Built.Rep->Chunks->Chunks;
		if (BuiltChunks.size() > 1 || Built.Rep->Root->TotalPieces > 1 || (BuiltChunks.size() == 1 && !BuiltChunks[0])
			|| (Built.Rep->Root->TotalPieces == 1 && (Built.Rep->Height != 0 || tb::AsLeaf(*Built.Rep->Root).Pieces[0].Start != 0)))
		{
			return false;
		}
		Current = std::move(Built.Rep);
		Edit->Active = detail::NoChunk;
		Edit->LiveUnits.assign(Current->Chunks->Chunks.size(), 0);
		if (!Edit->LiveUnits.empty())
		{
			Edit->LiveUnits[0] = Current->Root->TotalLen;
		}
		return true;
	}
}
