// SPDX-License-Identifier: Apache-2.0
// Dream core: the local-history format (TDD §10.4): index records, the index model and reader, run-entry merging, the
// blob header and chunks, and the retention planner. Data only (SEC-06); no I/O of its own.
#include "dream/HistoryFormat.h"

#include "dream/Checksum.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <utility>

namespace dream::history_detail
{
	using json::Value;
	using namespace dream::history;

	inline constexpr char BlobMagic[8] = {'S', 'U', 'M', 'I', 'B', 'L', 'O', 'B'};

	void PutLE(std::string& Out, uint64_t V, int Bytes)
	{
		for (int I = 0; I < Bytes; ++I)
		{
			Out.push_back(static_cast<char>((V >> (8 * I)) & 0xFF));
		}
	}

	uint64_t GetLE(std::string_view B, size_t At, int Bytes)
	{
		uint64_t V = 0;
		for (int I = 0; I < Bytes; ++I)
		{
			V |= static_cast<uint64_t>(static_cast<uint8_t>(B[At + static_cast<size_t>(I)])) << (8 * I);
		}
		return V;
	}

	journal::Record MakeRecord(RecordType Type, Value Head)
	{
		journal::Record R;
		R.Type = static_cast<journal::RecordType>(Type);
		R.Head = std::move(Head);
		return R;
	}

	bool IsHex(std::string_view S, size_t Len)
	{
		if (S.size() != Len)
		{
			return false;
		}
		for (const char C : S)
		{
			if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f')))
			{
				return false;
			}
		}
		return true;
	}

	struct GenerationRead
	{
		bool bHeaderOk = false;
		bool bComplete = false;
		bool bSalvage = false;
		journal::FileHeader Header;
	};

	/** One index generation's complete batches applied to Into, salvaging past bad frames (as §10.2.4 steps 2-3). */
	GenerationRead ReadGeneration(std::string_view Bytes, Index& Into)
	{
		GenerationRead Result;
		size_t Pos = journal::HeaderBytes;
		if (journal::DecodeHeader(Bytes, Result.Header, IndexLogFormat))
		{
			Result.bHeaderOk = true;
		}
		else
		{
			const size_t Next = journal::FindNextValidFrame(Bytes, 0);
			if (Next == std::string_view::npos)
			{
				return Result;
			}
			Pos = Next;
			Result.bSalvage = true;
		}
		std::vector<journal::Record> Batch;
		uint64_t LastSeq = 0;
		bool bFirstBatch = true;
		while (Pos < Bytes.size())
		{
			journal::Record Rec;
			uint64_t Seq = 0;
			bool bCommit = false;
			const size_t FramePos = Pos;
			const journal::FrameStatus Status = journal::DecodeFrame(Bytes, Pos, Rec, Seq, bCommit);
			if (Status == journal::FrameStatus::End)
			{
				break;
			}
			if (Status != journal::FrameStatus::Ok)
			{
				const size_t Next = journal::FindNextValidFrame(Bytes, FramePos + 1);
				if (Next == std::string_view::npos)
				{
					break;   // a torn tail: the open batch is dropped
				}
				Result.bSalvage = true;
				Batch.clear();
				bFirstBatch = false;
				Pos = Next;
				continue;
			}
			if (Seq <= LastSeq)
			{
				continue;
			}
			LastSeq = Seq;
			Batch.push_back(std::move(Rec));
			if (!bCommit)
			{
				continue;
			}
			if (bFirstBatch)
			{
				const bool bHasEnd = std::any_of(Batch.begin(), Batch.end(),
					[](const journal::Record& R) { return R.Type == journal::RecordType::CheckpointEnd; });
				Result.bComplete = bHasEnd && !Result.bSalvage;
			}
			for (const journal::Record& R : Batch)
			{
				Into.Apply(R);
			}
			bFirstBatch = false;
			Batch.clear();
		}
		return Result;
	}

	/** Age by §10.4: max(0, now - utc); a time more than a day in the future counts as now. */
	int64_t AgeMs(int64_t NowUtcMs, int64_t UtcMs)
	{
		// The difference in unsigned arithmetic: a corrupt utc (INT64_MIN, INT64_MAX) never overflows signed math (review r1).
		// A time at or after now is age 0 (more than a day ahead counts as now, less is in the future: 0 either way).
		if (UtcMs >= NowUtcMs)
		{
			return 0;
		}
		const uint64_t Age = static_cast<uint64_t>(NowUtcMs) - static_cast<uint64_t>(UtcMs);
		constexpr int64_t Max = std::numeric_limits<int64_t>::max();
		return Age > static_cast<uint64_t>(Max) ? Max : static_cast<int64_t>(Age);
	}
}

namespace dream::history
{
	using json::Value;

	namespace
	{
		struct KindNameEntry
		{
			EntryKind Kind;
			const char* Name;
		};

		constexpr KindNameEntry KindNames[] = {
			{ EntryKind::Save, "save" }, { EntryKind::Run, "run" }, { EntryKind::Reload, "reload" }, { EntryKind::Closed, "closed" },
			{ EntryKind::Recovered, "recovered" }, { EntryKind::Discarded, "discarded" }, { EntryKind::Restore, "restore" },
		};
	}

	const char* KindName(EntryKind Kind)
	{
		for (const KindNameEntry& E : KindNames)
		{
			if (E.Kind == Kind)
			{
				return E.Name;
			}
		}
		return "save";
	}

	bool ParseKind(std::string_view Name, EntryKind& Out)
	{
		for (const KindNameEntry& E : KindNames)
		{
			if (Name == E.Name)
			{
				Out = E.Kind;
				return true;
			}
		}
		return false;
	}

	journal::Record MakeEntry(const Entry& E)
	{
		Value H = Value::MakeObject();
		H.Set("id", Value::MakeString(E.Id));
		H.Set("uuid", Value::MakeString(E.Uuid));
		H.Set("path", Value::MakeString(E.Path));
		H.Set("display", Value::MakeString(E.Display));
		H.Set("kind", Value::MakeString(KindName(E.Kind)));
		Value Runs = Value::MakeArray();
		for (const uint32_t Run : E.Runs)
		{
			Runs.Append(Value::MakeUInt(Run));
		}
		H.Set("runs", std::move(Runs));
		H.Set("utc", Value::MakeInt(E.UtcMs));
		H.Set("blob", Value::MakeString(E.Blob));
		H.Set("size", Value::MakeInt(E.Size));
		H.Set("lines", Value::MakeInt(E.Lines));
		return history_detail::MakeRecord(RecordType::Entry, std::move(H));
	}

	journal::Record MakeTombstone(std::string_view Id)
	{
		Value H = Value::MakeObject();
		H.Set("id", Value::MakeString(std::string(Id)));
		return history_detail::MakeRecord(RecordType::Tombstone, std::move(H));
	}

	journal::Record MakeClear(int64_t UtcMs)
	{
		Value H = Value::MakeObject();
		H.Set("utc", Value::MakeInt(UtcMs));
		return history_detail::MakeRecord(RecordType::Clear, std::move(H));
	}

	bool EntryFromHead(const json::Value& H, Entry& Out)
	{
		Entry E;
		E.Id = H.Get("id").AsString();
		E.Uuid = H.Get("uuid").AsString();
		E.Path = H.Get("path").AsString();
		E.Display = H.Get("display").AsString();
		if (!ParseKind(H.Get("kind").AsString(), E.Kind))
		{
			E.Kind = EntryKind::Save;   // an unknown kind of a newer minor still names a text
		}
		const Value& Runs = H.Get("runs");
		for (size_t I = 0; I < Runs.Size(); ++I)
		{
			E.Runs.push_back(static_cast<uint32_t>(Runs.At(I).AsUInt()));
		}
		E.UtcMs = H.Get("utc").AsInt();
		E.Blob = H.Get("blob").AsString();
		E.Size = H.Get("size").AsInt();
		E.Lines = H.Get("lines").AsInt();
		if (E.Id.empty() || !history_detail::IsHex(E.Blob, 64))
		{
			return false;
		}
		Out = std::move(E);
		return true;
	}

	// ---- Index ----------------------------------------------------------------------------------------------

	void Index::Apply(const journal::Record& R)
	{
		switch (static_cast<RecordType>(static_cast<uint16_t>(R.Type)))
		{
		case RecordType::Entry:
		{
			Entry E;
			if (!EntryFromHead(R.Head, E))
			{
				return;
			}
			++BlobRefs[E.Blob];
			const auto It = Positions.find(E.Id);
			if (It != Positions.end())
			{
				Entry& Existing = List[It->second];
				if (--BlobRefs[Existing.Blob] == 0)
				{
					BlobRefs.erase(Existing.Blob);
				}
				Existing = std::move(E);
				return;
			}
			Positions.emplace(E.Id, List.size());
			List.push_back(std::move(E));
			return;
		}
		case RecordType::Tombstone:
		{
			const auto It = Positions.find(R.Head.Get("id").AsString());
			if (It == Positions.end())
			{
				return;
			}
			Entry& Gone = List[It->second];
			if (--BlobRefs[Gone.Blob] == 0)
			{
				BlobRefs.erase(Gone.Blob);
			}
			Gone = Entry();   // an empty Id marks the hole
			Positions.erase(It);
			++Holes;
			if (Holes > 64 && Holes * 2 > List.size())
			{
				Compact();
			}
			return;
		}
		case RecordType::Clear:
			Clear();
			return;
		default:
			return;
		}
	}

	void Index::Compact() const
	{
		if (Holes == 0)
		{
			return;
		}
		List.erase(std::remove_if(List.begin(), List.end(), [](const Entry& E) { return E.Id.empty(); }), List.end());
		Positions.clear();
		for (size_t K = 0; K < List.size(); ++K)
		{
			Positions.emplace(List[K].Id, K);
		}
		Holes = 0;
	}

	const std::vector<Entry>& Index::Entries() const
	{
		Compact();
		return List;
	}

	const Entry* Index::Find(std::string_view Id) const
	{
		const auto It = Positions.find(std::string(Id));
		return It != Positions.end() ? &List[It->second] : nullptr;
	}

	const Entry* Index::NewestOf(std::string_view Uuid) const
	{
		const Entry* Newest = nullptr;
		for (const Entry& E : List)
		{
			if (!E.Id.empty() && E.Uuid == Uuid && (Newest == nullptr || E.UtcMs >= Newest->UtcMs))
			{
				Newest = &E;
			}
		}
		return Newest;
	}

	bool Index::References(std::string_view Blob) const
	{
		return BlobRefs.find(std::string(Blob)) != BlobRefs.end();
	}

	std::vector<journal::Record> Index::Checkpoint() const
	{
		std::vector<journal::Record> Out;
		Out.reserve(List.size());
		for (const Entry& E : List)
		{
			if (!E.Id.empty())
			{
				Out.push_back(MakeEntry(E));
			}
		}
		return Out;
	}

	void Index::Clear()
	{
		List.clear();
		Positions.clear();
		BlobRefs.clear();
		Holes = 0;
	}

	Entry PlanRunEntry(const Index& I, Entry New)
	{
		if (New.Kind != EntryKind::Run)
		{
			return New;
		}
		const Entry* Previous = nullptr;
		for (const Entry& E : I.Entries())
		{
			if (E.Uuid == New.Uuid && E.UtcMs <= New.UtcMs && (Previous == nullptr || E.UtcMs >= Previous->UtcMs))
			{
				Previous = &E;
			}
		}
		if (Previous == nullptr || Previous->Kind != EntryKind::Run || Previous->Blob != New.Blob)
		{
			return New;
		}
		Entry Merged = *Previous;
		for (const uint32_t Run : New.Runs)
		{
			if (std::find(Merged.Runs.begin(), Merged.Runs.end(), Run) == Merged.Runs.end())
			{
				Merged.Runs.push_back(Run);
			}
		}
		Merged.UtcMs = std::max(Merged.UtcMs, New.UtcMs);
		Merged.Path = New.Path;
		Merged.Display = New.Display;
		return Merged;
	}

	RecoveredIndex ReadIndex(journal::FileOps& Ops, std::string_view Dir, bool bMayModify)
	{
		RecoveredIndex R;
		std::vector<std::string> Names;
		if (!Ops.List(Dir, Names))
		{
			return R;
		}
		std::vector<uint64_t> Generations;
		std::vector<std::string> Stale;
		for (const std::string& Name : Names)
		{
			uint64_t G = 0;
			if (Name.size() > 4 && Name.compare(Name.size() - 4, 4, ".tmp") == 0 && Name.compare(0, IndexLogFormat.Prefix.size(), IndexLogFormat.Prefix) == 0)
			{
				Stale.push_back(Name);
			}
			else if (journal::ParseLogFileName(IndexLogFormat, Name, G))
			{
				Generations.push_back(G);
			}
		}
		std::sort(Generations.begin(), Generations.end(), std::greater<uint64_t>());
		for (const uint64_t G : Generations)
		{
			std::string Prefix;
			journal::FileHeader Newest;
			if (Ops.ReadPrefix(journal::JoinPath(Dir, journal::LogFileName(IndexLogFormat, G)), journal::HeaderBytes, Prefix)
				&& journal::DecodeHeader(Prefix, Newest, IndexLogFormat))
			{
				if (Newest.Major > IndexMajor || (Newest.Major == IndexMajor && Newest.Minor > IndexMinor))
				{
					R.bNewerFormat = true;
					bMayModify = false;
				}
				break;
			}
		}
		if (bMayModify)
		{
			for (const std::string& Name : Stale)
			{
				Ops.Remove(journal::JoinPath(Dir, Name));
			}
		}
		if (Generations.empty())
		{
			return R;
		}
		R.bFound = true;
		R.Generation = Generations.front();

		std::vector<std::string> Damaged;
		for (size_t K = 0; K < Generations.size(); ++K)
		{
			const std::string Path = journal::JoinPath(Dir, journal::LogFileName(IndexLogFormat, Generations[K]));
			std::string Bytes;
			if (!Ops.ReadAll(Path, Bytes))
			{
				// persist-M2: not readable even after the FileOps retries. Kept in place (never set aside or deleted) so that the
				// next session can read it; the caller writes nothing above it this session (bSkippedUnreadable).
				R.bSkippedUnreadable = true;
				continue;
			}
			journal::FileHeader Header;
			const bool bHeaderOk = journal::DecodeHeader(Bytes, Header, IndexLogFormat);
			if (bHeaderOk && Header.Major > IndexMajor)
			{
				R.bNewerFormat = true;   // header only
				return R;
			}
			Index Entries;
			history_detail::GenerationRead Read;
			if (bHeaderOk && Header.Major == IndexMajor)
			{
				Read = history_detail::ReadGeneration(Bytes, Entries);
			}
			if (!bHeaderOk || Header.Major != IndexMajor || !Read.bComplete)
			{
				if (!bHeaderOk || Header.Major == IndexMajor)
				{
					Damaged.push_back(std::move(Bytes));
					if (bMayModify)
					{
						Ops.Touch(Path);
						Ops.RenameNew(Path, Path + ".corrupt");
					}
				}
				continue;
			}
			R.bFallbackUsed = !Damaged.empty() || R.bSkippedUnreadable;
			R.bSalvageUsed = Read.bSalvage;
			R.UsedGeneration = Generations[K];
			for (auto It = Damaged.rbegin(); It != Damaged.rend(); ++It)
			{
				history_detail::ReadGeneration(*It, Entries);   // entries are self-contained: newer ones apply on top
				R.bSalvageUsed = true;
			}
			R.Entries = std::move(Entries);
			if (bMayModify)
			{
				for (size_t Older = K + 1; Older < Generations.size(); ++Older)
				{
					const std::string OldPath = journal::JoinPath(Dir, journal::LogFileName(IndexLogFormat, Generations[Older]));
					std::string Prefix;
					journal::FileHeader OldHeader;
					if (Ops.ReadPrefix(OldPath, journal::HeaderBytes, Prefix) && journal::DecodeHeader(Prefix, OldHeader, IndexLogFormat)
						&& OldHeader.Major == IndexMajor && OldHeader.Minor <= IndexMinor)
					{
						Ops.Remove(OldPath);
					}
				}
			}
			return R;
		}
		R.bUnreadable = true;
		// No complete generation: the damaged ones still hold self-contained entries. Salvaged oldest first, so a session that
		// writes a new generation keeps referencing their blobs instead of letting retention sweep them (§10.11).
		Index Salvaged;
		for (auto It = Damaged.rbegin(); It != Damaged.rend(); ++It)
		{
			history_detail::ReadGeneration(*It, Salvaged);
			R.bSalvageUsed = true;
		}
		R.Entries = std::move(Salvaged);
		return R;
	}

	// ---- Blobs ----------------------------------------------------------------------------------------------

	void EncodeBlobHeader(const BlobHeader& Header, std::string& Out)
	{
		std::string H;
		H.append(history_detail::BlobMagic, 8);
		history_detail::PutLE(H, Header.Version, 2);
		history_detail::PutLE(H, 0, 2);
		history_detail::PutLE(H, Header.RawLength, 8);
		history_detail::PutLE(H, Header.RawCrc32c, 4);
		history_detail::PutLE(H, Header.ChunkBytes, 4);
		H.resize(BlobHeaderBytes - 4, '\0');
		history_detail::PutLE(H, Crc32c(H.data(), H.size()), 4);
		Out.append(H);
	}

	bool DecodeBlobHeader(std::string_view Bytes, BlobHeader& Out)
	{
		if (Bytes.size() < BlobHeaderBytes || std::memcmp(Bytes.data(), history_detail::BlobMagic, 8) != 0
			|| Crc32c(Bytes.data(), BlobHeaderBytes - 4) != static_cast<uint32_t>(history_detail::GetLE(Bytes, BlobHeaderBytes - 4, 4)))
		{
			return false;
		}
		Out.Version = static_cast<uint16_t>(history_detail::GetLE(Bytes, 8, 2));
		Out.RawLength = history_detail::GetLE(Bytes, 12, 8);
		Out.RawCrc32c = static_cast<uint32_t>(history_detail::GetLE(Bytes, 20, 4));
		Out.ChunkBytes = static_cast<uint32_t>(history_detail::GetLE(Bytes, 24, 4));
		return Out.Version == BlobVersion && Out.ChunkBytes > 0;
	}

	void AppendBlobChunk(std::string_view Compressed, uint32_t RawLength, std::string& Out)
	{
		history_detail::PutLE(Out, static_cast<uint32_t>(Compressed.size()), 4);
		history_detail::PutLE(Out, RawLength, 4);
		Out.append(Compressed);
	}

	bool NextBlobChunk(std::string_view File, size_t& InOutPos, std::string_view& OutCompressed, uint32_t& OutRawLength)
	{
		if (InOutPos + 8 > File.size())
		{
			return false;
		}
		const uint64_t Len = history_detail::GetLE(File, InOutPos, 4);
		const uint64_t Raw = history_detail::GetLE(File, InOutPos + 4, 4);
		if (Len > File.size() - InOutPos - 8 || Raw > (64u << 20))
		{
			return false;
		}
		OutCompressed = File.substr(InOutPos + 8, static_cast<size_t>(Len));
		OutRawLength = static_cast<uint32_t>(Raw);
		InOutPos += 8 + static_cast<size_t>(Len);
		return true;
	}

	std::string BlobRelativePath(std::string_view Sha256Hex)
	{
		std::string Out = "objects/";
		Out.append(Sha256Hex.substr(0, 2));
		Out.push_back('/');
		Out.append(Sha256Hex);
		Out.append(".z");
		return Out;
	}

	bool ParseBlobFileName(std::string_view Name, std::string& OutSha256Hex)
	{
		if (Name.size() != 66 || Name.substr(64) != ".z" || !history_detail::IsHex(Name.substr(0, 64), 64))
		{
			return false;
		}
		OutSha256Hex.assign(Name.substr(0, 64));
		return true;
	}

	// ---- Retention ------------------------------------------------------------------------------------------

	RetentionPlan PlanRetention(const Index& I, std::span<const BlobInfo> Blobs, int64_t NowUtcMs, const RetentionLimits& Limits)
	{
		RetentionPlan Plan;
		const std::vector<Entry>& Entries = I.Entries();
		std::vector<bool> Removed(Entries.size(), false);

		// The two exceptions: each buffer's newest entry, and every entry younger than 24 h.
		std::vector<bool> Protected(Entries.size(), false);
		std::map<std::string, size_t> NewestByBuffer;
		for (size_t K = 0; K < Entries.size(); ++K)
		{
			auto [It, bNew] = NewestByBuffer.emplace(Entries[K].Uuid, K);
			if (!bNew && Entries[K].UtcMs >= Entries[It->second].UtcMs)
			{
				It->second = K;
			}
			if (history_detail::AgeMs(NowUtcMs, Entries[K].UtcMs) < DayMs)
			{
				Protected[K] = true;
			}
		}
		for (const auto& [Uuid, K] : NewestByBuffer)
		{
			Protected[K] = true;
		}

		// Oldest first, so both steps tombstone in age order.
		std::vector<size_t> Order(Entries.size());
		for (size_t K = 0; K < Order.size(); ++K)
		{
			Order[K] = K;
		}
		std::stable_sort(Order.begin(), Order.end(), [&Entries](size_t A, size_t B) { return Entries[A].UtcMs < Entries[B].UtcMs; });

		// Step 1: the day limit.
		for (const size_t K : Order)
		{
			if (!Protected[K] && history_detail::AgeMs(NowUtcMs, Entries[K].UtcMs) > Limits.MaxAgeMs)
			{
				Removed[K] = true;
				Plan.Tombstones.push_back(Entries[K].Id);
			}
		}

		// The bytes that stay: every blob a live entry references, and every unreferenced one too young to sweep.
		std::map<std::string, int64_t> References;   // blob -> live entries
		for (size_t K = 0; K < Entries.size(); ++K)
		{
			if (!Removed[K])
			{
				++References[Entries[K].Blob];
			}
		}
		auto Young = [NowUtcMs](const BlobInfo& B) { return B.WrittenUtcMs <= 0 || history_detail::AgeMs(NowUtcMs, B.WrittenUtcMs) < DayMs; };
		std::map<std::string, const BlobInfo*> BlobBySha;
		for (const BlobInfo& B : Blobs)
		{
			BlobBySha[B.Sha256Hex] = &B;
			Plan.BytesBefore += B.Bytes;
		}
		auto StayingBytes = [&]()
		{
			int64_t Bytes = 0;
			for (const BlobInfo& B : Blobs)
			{
				const auto Ref = References.find(B.Sha256Hex);
				if ((Ref != References.end() && Ref->second > 0) || Young(B))
				{
					Bytes += B.Bytes;
				}
			}
			return Bytes;
		};

		// Step 2: the size limit, oldest entries first, with the same exceptions.
		int64_t Staying = StayingBytes();
		for (const size_t K : Order)
		{
			if (Staying <= Limits.MaxBytes)
			{
				break;
			}
			if (Protected[K] || Removed[K])
			{
				continue;
			}
			Removed[K] = true;
			Plan.Tombstones.push_back(Entries[K].Id);
			int64_t& Count = References[Entries[K].Blob];
			if (--Count == 0)
			{
				const auto Blob = BlobBySha.find(Entries[K].Blob);
				if (Blob != BlobBySha.end() && !Young(*Blob->second))
				{
					Staying -= Blob->second->Bytes;
				}
			}
		}

		// Step 3: mark and sweep.
		for (const BlobInfo& B : Blobs)
		{
			const auto Ref = References.find(B.Sha256Hex);
			if ((Ref == References.end() || Ref->second <= 0) && !Young(B))
			{
				Plan.Sweep.push_back(B.Sha256Hex);
			}
		}
		Plan.BytesAfter = StayingBytes();
		return Plan;
	}
}
