// SPDX-License-Identifier: Apache-2.0
// Dream core: the output store's bounded line history (TDD §6.2 "dream::OutputRing", D19, OUT-04).
// Text and spans live in fixed-capacity chunks that never reallocate, so LineView pointers stay valid until the line is
// evicted or cleared. Sealed chunks are shared with snapshots; a snapshot copies the open chunk's used part, so every
// chunk a snapshot holds is immutable and readable on any thread without locks.
// Deferred (M2): ParseSgr and stored style spans (OUT-03); Append keeps spans it is given, ingest passes none yet.
#include "dream/OutputRing.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <utility>

namespace dream::outputring_detail
{
	struct LineMeta
	{
		LineInfo Info;
		uint32_t TextOff = 0;
		uint32_t TextLen = 0;
		uint32_t StyleOff = 0;
		uint32_t LinkOff = 0;
		uint16_t StyleCount = 0;
		uint16_t LinkCount = 0;
	};

	struct Chunk
	{
		uint64_t FirstSeq = 0;
		uint32_t Capacity = 0;
		uint32_t Used = 0;
		uint64_t TextBytes = 0;            // text bytes of the lines in this chunk
		std::unique_ptr<unsigned char[]> Bytes;
		std::vector<LineMeta> Lines;

		uint64_t EndSeq() const { return FirstSeq + Lines.size(); }
	};

	inline uint32_t AlignUp(uint32_t V, uint32_t A)
	{
		return (V + A - 1) & ~(A - 1);
	}

	/** Longest prefix of S of at most Max bytes that does not end inside a UTF-8 sequence. */
	std::string_view CutUtf8(std::string_view S, size_t Max)
	{
		if (S.size() <= Max)
		{
			return S;
		}
		size_t N = Max;
		while (N > 0 && (static_cast<unsigned char>(S[N]) & 0xC0u) == 0x80u)
		{
			--N;
		}
		return S.substr(0, N);
	}

	bool ReadLine(const Chunk& C, uint64_t Seq, LineView& Out)
	{
		if (Seq < C.FirstSeq || Seq >= C.EndSeq())
		{
			return false;
		}
		const LineMeta& M = C.Lines[static_cast<size_t>(Seq - C.FirstSeq)];
		const unsigned char* Base = C.Bytes.get();
		Out.Seq = Seq;
		Out.Info = M.Info;
		Out.Text = std::string_view(reinterpret_cast<const char*>(Base + M.TextOff), M.TextLen);
		Out.Styles = std::span<const StyleSpan>(M.StyleCount ? reinterpret_cast<const StyleSpan*>(Base + M.StyleOff) : nullptr, M.StyleCount);
		Out.Links = std::span<const LinkSpan>(M.LinkCount ? reinterpret_cast<const LinkSpan*>(Base + M.LinkOff) : nullptr, M.LinkCount);
		return true;
	}

	template <typename ChunkPtr>
	const Chunk* FindChunk(const std::deque<ChunkPtr>& Chunks, uint64_t Seq)
	{
		auto It = std::upper_bound(Chunks.begin(), Chunks.end(), Seq,
			[](uint64_t S, const ChunkPtr& C) { return S < C->FirstSeq; });
		if (It == Chunks.begin())
		{
			return nullptr;
		}
		return (*(It - 1)).get();
	}

	std::shared_ptr<Chunk> CopyChunk(const Chunk& C)
	{
		auto Copy = std::make_shared<Chunk>();
		Copy->FirstSeq = C.FirstSeq;
		Copy->Capacity = C.Used;
		Copy->Used = C.Used;
		Copy->TextBytes = C.TextBytes;
		Copy->Bytes = std::make_unique<unsigned char[]>(std::max<uint32_t>(C.Used, 1));
		std::memcpy(Copy->Bytes.get(), C.Bytes.get(), C.Used);
		Copy->Lines = C.Lines;
		return Copy;
	}
}

namespace dream
{
	using outputring_detail::Chunk;

	struct OutputSnapshot::Data
	{
		std::deque<std::shared_ptr<const Chunk>> Chunks;
		uint64_t First = 0;
		uint64_t End = 0;
	};

	OutputSnapshot::OutputSnapshot() = default;

	uint64_t OutputSnapshot::FirstSeq() const
	{
		return State ? State->First : 0;
	}

	uint64_t OutputSnapshot::EndSeq() const
	{
		return State ? State->End : 0;
	}

	bool OutputSnapshot::GetLine(uint64_t Seq, LineView& Out) const
	{
		if (!State || Seq < State->First || Seq >= State->End)
		{
			return false;
		}
		const Chunk* C = outputring_detail::FindChunk(State->Chunks, Seq);
		return C != nullptr && outputring_detail::ReadLine(*C, Seq, Out);
	}

	struct OutputRing::Impl
	{
		OutputRingConfig Config;
		std::deque<std::shared_ptr<Chunk>> Chunks;   // oldest first; the back one is open
		uint64_t First = 0;                          // first live sequence
		uint64_t End = 0;                            // next sequence to assign
		uint64_t LiveTextBytes = 0;
		uint32_t LinesPerChunk = 0;
		std::function<void(const EvictionInfo&)> Listener;

		explicit Impl(const OutputRingConfig& InConfig)
			: Config(InConfig)
		{
			Config.MaxLines = std::max<uint32_t>(Config.MaxLines, 1);
			Config.ChunkBytes = std::max<uint32_t>(Config.ChunkBytes, 256);
			// Eviction drops whole chunks; a line cap per chunk keeps that granularity at 1/8 of MaxLines.
			LinesPerChunk = std::max<uint32_t>(1, Config.MaxLines / 8);
		}

		std::shared_ptr<Chunk> NewChunk()
		{
			auto C = std::make_shared<Chunk>();
			C->FirstSeq = End;
			C->Capacity = Config.ChunkBytes;
			C->Bytes = std::make_unique<unsigned char[]>(C->Capacity);
			Chunks.push_back(C);
			return C;
		}

		/** Text bytes of the live lines of a chunk (lines before First were already subtracted by ClearBefore). */
		uint64_t LiveBytesOf(const Chunk& C) const
		{
			if (C.FirstSeq >= First)
			{
				return C.TextBytes;
			}
			uint64_t Bytes = 0;
			for (uint64_t S = std::max(First, C.FirstSeq); S < C.EndSeq(); ++S)
			{
				Bytes += C.Lines[static_cast<size_t>(S - C.FirstSeq)].TextLen;
			}
			return Bytes;
		}

		void EvictOldest()
		{
			std::shared_ptr<Chunk> Old = Chunks.front();
			Chunks.pop_front();
			const uint64_t FromSeq = std::max(First, Old->FirstSeq);
			const uint64_t ToSeq = Old->EndSeq();
			if (FromSeq < ToSeq)
			{
				EvictionInfo Info;
				Info.FirstEvicted = FromSeq;
				Info.LastEvicted = ToSeq - 1;
				for (uint64_t S = FromSeq; S < ToSeq; ++S)
				{
					const uint32_t Run = Old->Lines[static_cast<size_t>(S - Old->FirstSeq)].Info.RunNumber;
					auto It = std::lower_bound(Info.RunNumbers.begin(), Info.RunNumbers.end(), Run);
					if (It == Info.RunNumbers.end() || *It != Run)
					{
						Info.RunNumbers.insert(It, Run);
					}
				}
				LiveTextBytes -= std::min(LiveTextBytes, LiveBytesOf(*Old));
				First = ToSeq;
				if (Listener)
				{
					Listener(Info);
				}
			}
			First = std::max(First, ToSeq);
		}
	};

	OutputRing::OutputRing(const OutputRingConfig& Config)
		: State(std::make_unique<Impl>(Config))
	{
	}

	OutputRing::~OutputRing() = default;

	uint64_t OutputRing::Append(const LineInfo& Info, std::string_view Utf8, std::span<const StyleSpan> Styles, std::span<const LinkSpan> Links)
	{
		Impl& I = *State;
		const uint32_t Cap = I.Config.ChunkBytes;
		const size_t MaxSpans = 0xFFFF;
		Styles = Styles.subspan(0, std::min(Styles.size(), MaxSpans));
		Links = Links.subspan(0, std::min(Links.size(), MaxSpans));
		const uint32_t SpanBytes = static_cast<uint32_t>(Styles.size() * sizeof(StyleSpan) + Links.size() * sizeof(LinkSpan)) + 8;
		if (SpanBytes >= Cap)
		{
			Styles = {};
			Links = {};
		}
		const uint32_t SpanNeed = static_cast<uint32_t>(Styles.size() * sizeof(StyleSpan) + Links.size() * sizeof(LinkSpan)) + 8;
		// A line longer than a chunk is the caller's bug (ingest splits at 64 KB, §6.1): cut it on a UTF-8 boundary.
		Utf8 = outputring_detail::CutUtf8(Utf8, Cap - SpanNeed);
		const uint32_t Need = static_cast<uint32_t>(Utf8.size()) + SpanNeed;

		// Caps (D19, OUT-04): evict the oldest chunks first. The open chunk is sealed first if it is the only one.
		while ((I.End - I.First + 1 > I.Config.MaxLines || I.LiveTextBytes + Utf8.size() > I.Config.MaxBytes) && I.End > I.First)
		{
			if (I.Chunks.size() == 1)
			{
				I.NewChunk();
			}
			I.EvictOldest();
		}

		std::shared_ptr<Chunk> Open = I.Chunks.empty() ? nullptr : I.Chunks.back();
		if (!Open || Open->Used + Need > Open->Capacity || Open->Lines.size() >= I.LinesPerChunk)
		{
			Open = I.NewChunk();
		}

		outputring_detail::LineMeta M;
		M.Info = Info;
		M.TextOff = Open->Used;
		M.TextLen = static_cast<uint32_t>(Utf8.size());
		unsigned char* Base = Open->Bytes.get();
		if (!Utf8.empty())
		{
			std::memcpy(Base + M.TextOff, Utf8.data(), Utf8.size());
		}
		uint32_t Cursor = outputring_detail::AlignUp(M.TextOff + M.TextLen, 8);
		if (!Styles.empty())
		{
			M.StyleOff = Cursor;
			M.StyleCount = static_cast<uint16_t>(Styles.size());
			std::uninitialized_copy(Styles.begin(), Styles.end(), reinterpret_cast<StyleSpan*>(Base + Cursor));
			Cursor += static_cast<uint32_t>(Styles.size() * sizeof(StyleSpan));
		}
		if (!Links.empty())
		{
			Cursor = outputring_detail::AlignUp(Cursor, 8);
			M.LinkOff = Cursor;
			M.LinkCount = static_cast<uint16_t>(Links.size());
			std::uninitialized_copy(Links.begin(), Links.end(), reinterpret_cast<LinkSpan*>(Base + Cursor));
			Cursor += static_cast<uint32_t>(Links.size() * sizeof(LinkSpan));
		}
		if (M.LinkCount > 0)
		{
			M.Info.Flags = static_cast<uint16_t>(M.Info.Flags | OLF_HasLinks);
		}
		Open->Used = std::min(Cursor, Open->Capacity);
		Open->TextBytes += M.TextLen;
		Open->Lines.push_back(M);
		I.LiveTextBytes += M.TextLen;
		return I.End++;
	}

	bool OutputRing::GetLine(uint64_t Seq, LineView& Out) const
	{
		const Impl& I = *State;
		if (Seq < I.First || Seq >= I.End)
		{
			return false;
		}
		const Chunk* C = outputring_detail::FindChunk(I.Chunks, Seq);
		return C != nullptr && outputring_detail::ReadLine(*C, Seq, Out);
	}

	uint64_t OutputRing::FirstSeq() const
	{
		return State->First;
	}

	uint64_t OutputRing::EndSeq() const
	{
		return State->End;
	}

	uint32_t OutputRing::LineCount() const
	{
		return static_cast<uint32_t>(State->End - State->First);
	}

	uint64_t OutputRing::TextBytes() const
	{
		return State->LiveTextBytes;
	}

	OutputSnapshot OutputRing::TakeSnapshot() const
	{
		const Impl& I = *State;
		OutputSnapshot Snap;
		auto D = std::make_shared<OutputSnapshot::Data>();
		D->First = I.First;
		D->End = I.End;
		for (size_t K = 0; K < I.Chunks.size(); ++K)
		{
			const bool bOpen = K + 1 == I.Chunks.size();
			if (bOpen)
			{
				D->Chunks.push_back(outputring_detail::CopyChunk(*I.Chunks[K]));   // the open chunk keeps growing
			}
			else
			{
				D->Chunks.push_back(I.Chunks[K]);
			}
		}
		Snap.State = std::move(D);
		return Snap;
	}

	void OutputRing::ClearBefore(uint64_t Seq)
	{
		Impl& I = *State;
		Seq = std::min(Seq, I.End);
		if (Seq <= I.First)
		{
			return;
		}
		for (uint64_t S = I.First; S < Seq; ++S)
		{
			const Chunk* C = outputring_detail::FindChunk(I.Chunks, S);
			if (C != nullptr && S >= C->FirstSeq && S < C->EndSeq())
			{
				I.LiveTextBytes -= std::min<uint64_t>(I.LiveTextBytes, C->Lines[static_cast<size_t>(S - C->FirstSeq)].TextLen);
			}
		}
		I.First = Seq;
		// Chunks that hold no live line any more are released (the open chunk always stays).
		while (I.Chunks.size() > 1 && I.Chunks.front()->EndSeq() <= I.First)
		{
			I.Chunks.pop_front();
		}
	}

	void OutputRing::SetEvictionListener(std::function<void(const EvictionInfo&)> Listener)
	{
		State->Listener = std::move(Listener);
	}
}
