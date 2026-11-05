// SPDX-License-Identifier: Apache-2.0
// Dream core: the write-ahead journal format (TDD §10.2): header, SJRF frames with CRC32C, record heads, generations with
// sliced checkpoints and the catch-up step (§10.2.1, §10.2.3), the reader with batch atomicity, salvage, fallback and the
// older-major dispatch (§10.6), the commit policy and the batch planner. Data only (SEC-06); no I/O of its own.
#include "dream/JournalFormat.h"

#include "dream/Checksum.h"
#include "dream/Diff.h"
#include "dream/Utf.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <utility>

namespace dream::journal_detail
{
	using json::Value;
	using namespace dream::journal;

	inline constexpr char Sync[4] = {'S', 'J', 'R', 'F'};
	inline constexpr size_t FrameFixedBytes = 20;   // sync, payload_len, type, flags, seq
	inline constexpr size_t FrameCrcBytes = 4;

	void PutU16(std::string& Out, uint16_t V)
	{
		Out.push_back(static_cast<char>(V & 0xFF));
		Out.push_back(static_cast<char>(V >> 8));
	}

	void PutU32(std::string& Out, uint32_t V)
	{
		for (int I = 0; I < 4; ++I)
		{
			Out.push_back(static_cast<char>((V >> (8 * I)) & 0xFF));
		}
	}

	void PutU64(std::string& Out, uint64_t V)
	{
		for (int I = 0; I < 8; ++I)
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

	Value Str(std::string_view S)
	{
		return Value::MakeString(std::string(S));
	}

	Value BaseToJson(const std::optional<BaseVersion>& Base)
	{
		if (!Base)
		{
			return Value::MakeNull();
		}
		Value V = Value::MakeObject();
		V.Set("size", Value::MakeInt(Base->Size));
		V.Set("mtime_utc", Value::MakeInt(Base->MtimeUtcMs));
		V.Set("sha256", Str(Base->Sha256Hex));
		return V;
	}

	std::optional<BaseVersion> BaseFromJson(const Value& V)
	{
		if (!V.IsObject())
		{
			return std::nullopt;
		}
		BaseVersion B;
		B.Size = V.Get("size").AsInt();
		B.MtimeUtcMs = V.Get("mtime_utc").AsInt();
		B.Sha256Hex = V.Get("sha256").AsString();
		return B;
	}

	Value PendingToJson(const PendingInfo& P)
	{
		Value V = Value::MakeObject();
		V.Set("source_namespace", Str(P.SourceNamespace));
		V.Set("source_generation", Value::MakeUInt(P.SourceGeneration));
		V.Set("uuid", Str(P.Uuid));
		V.Set("kind", Str(P.Kind));
		V.Set("path", Str(P.Path));
		V.Set("base", BaseToJson(P.Base));
		V.Set("eol", Str(P.Eol));
		return V;
	}

	PendingInfo PendingFromJson(const Value& V)
	{
		PendingInfo P;
		P.SourceNamespace = V.Get("source_namespace").AsString();
		P.SourceGeneration = V.Get("source_generation").AsUInt();
		P.Uuid = V.Get("uuid").AsString();
		P.Kind = V.Get("kind").AsString();
		P.Path = V.Get("path").AsString();
		P.Base = BaseFromJson(V.Get("base"));
		P.Eol = V.Get("eol").AsString();
		return P;
	}

	BufferOpenInfo OpenFromJson(const Value& V)
	{
		BufferOpenInfo O;
		O.Uuid = V.Get("uuid").AsString();
		O.Kind = V.Get("kind").AsString();
		O.Path = V.Get("path").AsString();
		O.Display = V.Get("display").AsString();
		O.Encoding = V.Get("encoding").AsString();
		O.bBom = V.Get("bom").AsBool();
		O.Eol = V.Get("eol").AsString();
		O.Base = BaseFromJson(V.Get("base"));
		O.bReadOnly = V.Get("readonly").AsBool();
		return O;
	}

	RunBeginInfo RunFromJson(const Value& V)
	{
		RunBeginInfo R;
		R.RunId = V.Get("run_id").AsUInt();
		R.Number = static_cast<uint32_t>(V.Get("number").AsUInt());
		R.TabUuid = V.Get("tab_uuid").AsString();
		R.BufferVersion = V.Get("buffer_version").AsUInt();
		R.Route = V.Get("route").AsString();
		R.bDebug = V.Get("debug").AsBool();
		R.UtcMs = V.Get("utc").AsInt();
		return R;
	}

	Record MakeRecord(RecordType Type, Value Head, std::string Body = std::string(), uint16_t Flags = 0)
	{
		Record R;
		R.Type = Type;
		R.Flags = Flags;
		R.Head = std::move(Head);
		R.Body = std::move(Body);
		return R;
	}

	/** Checks the frame at Pos without decoding its head: sync, bound, size and CRC. */
	FrameStatus CheckFrame(std::string_view Bytes, size_t Pos, uint32_t& OutPayloadLen)
	{
		const size_t Remaining = Bytes.size() - Pos;
		if (Remaining < FrameFixedBytes)
		{
			const std::string_view Have = Bytes.substr(Pos, std::min<size_t>(Remaining, 4));
			return std::string_view(Sync, Have.size()) == Have ? FrameStatus::Truncated : FrameStatus::BadSync;
		}
		if (std::memcmp(Bytes.data() + Pos, Sync, 4) != 0)
		{
			return FrameStatus::BadSync;
		}
		const uint32_t PayloadLen = static_cast<uint32_t>(GetLE(Bytes, Pos + 4, 4));
		if (PayloadLen > MaxPayloadBytes || PayloadLen < 4)
		{
			return FrameStatus::BadLength;
		}
		if (Remaining < FrameFixedBytes + PayloadLen + FrameCrcBytes)
		{
			return FrameStatus::Truncated;
		}
		const uint32_t Stored = static_cast<uint32_t>(GetLE(Bytes, Pos + FrameFixedBytes + PayloadLen, 4));
		if (Crc32c(Bytes.data() + Pos + 8, static_cast<size_t>(12) + PayloadLen) != Stored)
		{
			return FrameStatus::BadCrc;
		}
		OutPayloadLen = PayloadLen;
		return FrameStatus::Ok;
	}

	struct SnapAssembly
	{
		uint64_t Version = 0;
		uint64_t Parts = 0;
		uint64_t Next = 0;
		uint32_t Crc = 0;
		std::string Text;
	};

	std::string PendingKey(const PendingInfo& P)
	{
		return P.SourceNamespace + '\n' + std::to_string(P.SourceGeneration) + '\n' + P.Uuid;
	}

	/** The reconstructed namespace while reading; batches apply atomically (§10.2.3 commit). */
	class ReadState
	{
	public:
		std::vector<RecoveredBuffer> Buffers;
		json::Value Session;
		std::vector<RecoveredRun> OpenRuns;
		std::vector<RecoveredPending> Pending;
		std::vector<std::pair<std::string, uint64_t>> CleanVersions;
		bool bLastBatchClean = false;
		int64_t LastUtcMs = 0;

		void NoteUtc(int64_t Utc)
		{
			LastUtcMs = std::max(LastUtcMs, Utc);
		}

		RecoveredBuffer& BufferFor(const std::string& Uuid)
		{
			for (RecoveredBuffer& B : Buffers)
			{
				if (B.Open.Uuid == Uuid)
				{
					return B;
				}
			}
			RecoveredBuffer B;
			B.Open.Uuid = Uuid;
			Buffers.push_back(std::move(B));
			return Buffers.back();
		}

		RecoveredBuffer* FindBuffer(const std::string& Uuid)
		{
			for (RecoveredBuffer& B : Buffers)
			{
				if (B.Open.Uuid == Uuid)
				{
					return &B;
				}
			}
			return nullptr;
		}

		RecoveredPending& PendingFor(const PendingInfo& Info)
		{
			const std::string Key = PendingKey(Info);
			for (RecoveredPending& P : Pending)
			{
				if (PendingKey(P.Info) == Key)
				{
					return P;
				}
			}
			RecoveredPending P;
			P.Info = Info;
			Pending.push_back(std::move(P));
			return Pending.back();
		}

		void ApplyBatch(const std::vector<Record>& Batch)
		{
			std::map<std::string, SnapAssembly> Snaps;   // per batch: SNAP parts by uuid (or pending key)
			bool bClean = false;
			for (const Record& R : Batch)
			{
				const Value& H = R.Head;
				switch (R.Type)
				{
				case RecordType::BufOpen:
				{
					BufferOpenInfo Info = OpenFromJson(H);
					RecoveredBuffer& B = BufferFor(Info.Uuid);
					if (B.bClosed)
					{
						B = RecoveredBuffer();   // reopened: start over
					}
					B.Open = std::move(Info);
					break;
				}
				case RecordType::Snap:
					ApplySnapPart(R, Snaps);
					break;
				case RecordType::Delta:
					ApplyDeltaRecord(R);
					break;
				case RecordType::Meta:
				{
					RecoveredBuffer& B = BufferFor(H.Get("uuid").AsString());
					B.bDirty = H.Get("dirty").AsBool();
					B.Open.Base = BaseFromJson(H.Get("base"));
					if (const Value* P = H.Find("path"))
					{
						B.Open.Path = P->AsString();
					}
					if (const Value* D = H.Find("display"))
					{
						B.Open.Display = D->AsString();
					}
					break;
				}
				case RecordType::BufClose:
				{
					RecoveredBuffer& B = BufferFor(H.Get("uuid").AsString());
					B.bClosed = true;
					B.CloseReason = H.Get("reason").AsString();
					break;
				}
				case RecordType::RecoveryPending:
					PendingFor(PendingFromJson(H)).Info = PendingFromJson(H);
					break;
				case RecordType::CleanText:
				{
					RecoveredBuffer* B = FindBuffer(H.Get("uuid").AsString());
					if (B == nullptr)
					{
						break;
					}
					std::string Clean = H.Get("same").AsBool() ? B->TextUtf8 : R.Body;
					if (Crc32c(Clean) == static_cast<uint32_t>(H.Get("text_crc32c").AsUInt()))
					{
						B->CleanTextUtf8 = std::move(Clean);   // a mismatch keeps the last verified clean text
					}
					break;
				}
				case RecordType::RecoveryResolved:
				{
					PendingInfo Key;
					Key.SourceNamespace = H.Get("source_namespace").AsString();
					Key.SourceGeneration = H.Get("source_generation").AsUInt();
					Key.Uuid = H.Get("uuid").AsString();
					const std::string K = PendingKey(Key);
					Pending.erase(std::remove_if(Pending.begin(), Pending.end(),
						[&K](const RecoveredPending& P) { return PendingKey(P.Info) == K; }), Pending.end());
					break;
				}
				case RecordType::Session:
					Session = H;
					if (const Value* U = H.Find("utc"))
					{
						NoteUtc(U->AsInt());
					}
					break;
				case RecordType::RunBegin:
				{
					RecoveredRun Run;
					Run.Begin = RunFromJson(H);
					NoteUtc(Run.Begin.UtcMs);
					OpenRuns.erase(std::remove_if(OpenRuns.begin(), OpenRuns.end(),
						[&Run](const RecoveredRun& O) { return O.Begin.RunId == Run.Begin.RunId; }), OpenRuns.end());
					OpenRuns.push_back(std::move(Run));
					break;
				}
				case RecordType::RunEnd:
				{
					const uint64_t Id = H.Get("run_id").AsUInt();
					OpenRuns.erase(std::remove_if(OpenRuns.begin(), OpenRuns.end(),
						[Id](const RecoveredRun& O) { return O.Begin.RunId == Id; }), OpenRuns.end());
					break;
				}
				case RecordType::Pause:
				case RecordType::Resume:
				{
					const uint64_t Id = H.Get("run_id").AsUInt();
					for (RecoveredRun& O : OpenRuns)
					{
						if (O.Begin.RunId == Id)
						{
							O.bPaused = R.Type == RecordType::Pause;
						}
					}
					break;
				}
				case RecordType::Clean:
				{
					bClean = true;
					NoteUtc(H.Get("utc").AsInt());
					CleanVersions.clear();
					const Value& Versions = H.Get("clean_versions");
					for (const Value::Member& M : Versions.Members())
					{
						CleanVersions.emplace_back(M.Key, M.Val.AsUInt());
					}
					break;
				}
				default:
					break;   // HEADER_EXT, checkpoint markers, and unknown (skippable) records carry no buffer state
				}
			}
			bLastBatchClean = bClean;
		}

	private:
		void ApplySnapPart(const Record& R, std::map<std::string, SnapAssembly>& Snaps)
		{
			const Value& H = R.Head;
			const Value& PendingHead = H.Get("pending");
			const bool bPending = PendingHead.IsObject();
			const std::string Uuid = H.Get("uuid").AsString();
			const std::string Key = bPending ? "p\n" + PendingKey(PendingFromJson(PendingHead)) : "b\n" + Uuid;
			const uint64_t Part = H.Get("part").AsUInt();
			const uint64_t Parts = std::max<uint64_t>(1, H.Get("parts").AsUInt(1));
			SnapAssembly& A = Snaps[Key];
			if (Part == 0)
			{
				A = SnapAssembly();
				A.Version = H.Get("version").AsUInt();
				A.Parts = Parts;
				A.Crc = static_cast<uint32_t>(H.Get("text_crc32c").AsUInt());
			}
			if (Part != A.Next || Parts != A.Parts)
			{
				Snaps.erase(Key);   // a part out of order: this SNAP is unusable
				return;
			}
			A.Text.append(R.Body);
			++A.Next;
			if (A.Next < A.Parts)
			{
				return;
			}
			const bool bCrcOk = Crc32c(A.Text) == A.Crc;
			if (bPending)
			{
				// Never live-buffer state (§10.2.2); a lost RECOVERY_PENDING is rebuilt from this head (§10.2.4 step 3).
				if (bCrcOk)
				{
					PendingFor(PendingFromJson(PendingHead)).TextUtf8 = std::move(A.Text);
				}
			}
			else if (bCrcOk)
			{
				RecoveredBuffer& B = BufferFor(Uuid);
				B.TextUtf8 = std::move(A.Text);
				B.Version = A.Version;
				B.bHasText = true;
				B.bVerified = true;
			}
			Snaps.erase(Key);
		}

		void ApplyDeltaRecord(const Record& R)
		{
			const Value& H = R.Head;
			RecoveredBuffer* B = FindBuffer(H.Get("uuid").AsString());
			if (B == nullptr || !B->bHasText || B->Version != H.Get("from").AsUInt())
			{
				return;   // a DELTA from another version waits for the next SNAP (T-PER-008)
			}
			std::vector<DeltaOp> Ops;
			const Value& List = H.Get("ops");
			for (size_t I = 0; I < List.Size(); ++I)
			{
				const Value& Op = List.At(I);
				Ops.push_back(DeltaOp{Op.At(0).AsInt(-1), Op.At(1).AsInt(-1), Op.At(2).AsInt(-1)});
			}
			std::string Text = B->TextUtf8;
			if (!ApplyDelta(Text, Ops, R.Body) || Crc32c(Text) != static_cast<uint32_t>(H.Get("text_crc32c").AsUInt()))
			{
				return;   // keep the last verified state (§10.2.4 step 5)
			}
			B->TextUtf8 = std::move(Text);
			B->Version = H.Get("to").AsUInt();
			B->bVerified = true;
		}
	};

	struct GenerationResult
	{
		bool bHeaderOk = false;
		bool bCheckpointComplete = false;
		bool bSalvage = false;
		FileHeader Header;
	};

	/** Applies one generation's complete batches to State; salvages past bad frames (§10.2.4 steps 2-3). */
	GenerationResult ReadGeneration(std::string_view Bytes, ReadState& State)
	{
		GenerationResult Result;
		size_t Pos = HeaderBytes;
		if (DecodeHeader(Bytes, Result.Header))
		{
			Result.bHeaderOk = true;
			State.NoteUtc(static_cast<int64_t>(Result.Header.CreatedUtcMs));
		}
		else
		{
			// A bad header: salvage what follows (used on top of the previous generation).
			Pos = std::min(Bytes.size(), HeaderBytes);
			const size_t Next = FindNextValidFrame(Bytes, 0);
			if (Next == std::string_view::npos)
			{
				return Result;
			}
			Pos = Next;
			Result.bSalvage = true;
		}
		std::vector<Record> Batch;
		uint64_t LastSeq = 0;
		bool bFirstBatch = true;
		while (Pos < Bytes.size())
		{
			Record Rec;
			uint64_t Seq = 0;
			bool bCommit = false;
			const size_t FramePos = Pos;
			const FrameStatus Status = DecodeFrame(Bytes, Pos, Rec, Seq, bCommit);
			if (Status == FrameStatus::End)
			{
				break;
			}
			if (Status != FrameStatus::Ok)
			{
				const size_t Next = FindNextValidFrame(Bytes, FramePos + 1);
				if (Next == std::string_view::npos)
				{
					break;   // a torn tail, the usual crash: the open batch is dropped
				}
				Result.bSalvage = true;
				Batch.clear();
				bFirstBatch = false;
				Pos = Next;
				continue;
			}
			if (Seq <= LastSeq)
			{
				continue;   // after a resync, only frames with a larger seq belong to this generation's history
			}
			LastSeq = Seq;
			Batch.push_back(std::move(Rec));
			if (!bCommit)
			{
				continue;
			}
			bool bHasEnd = false;
			for (const Record& R : Batch)
			{
				if (R.Type == RecordType::CheckpointEnd)
				{
					bHasEnd = true;
				}
			}
			if (bFirstBatch)
			{
				Result.bCheckpointComplete = bHasEnd && !Result.bSalvage;
			}
			State.ApplyBatch(Batch);
			bFirstBatch = false;
			Batch.clear();
		}
		return Result;
	}
}

namespace dream::journal
{
	using json::Value;

	void EncodeHeader(const FileHeader& Header, std::string& Out)
	{
		EncodeHeader(Header, Out, JournalLogFormat);
	}

	bool DecodeHeader(std::string_view Bytes, FileHeader& Out)
	{
		return DecodeHeader(Bytes, Out, JournalLogFormat);
	}

	void EncodeHeader(const FileHeader& Header, std::string& Out, const LogFormat& Format)
	{
		std::string H;
		H.reserve(HeaderBytes);
		H.append(Format.Magic.substr(0, 8));
		H.resize(8, '\0');
		journal_detail::PutU16(H, Header.Major);
		journal_detail::PutU16(H, Header.Minor);
		journal_detail::PutU64(H, Header.Generation);
		journal_detail::PutU64(H, Header.CreatedUtcMs);
		journal_detail::PutU32(H, Header.InstancePid);
		journal_detail::PutU64(H, Header.InstanceStart);
		H.push_back(static_cast<char>(Header.TextUnit));
		H.resize(HeaderBytes - 4, '\0');
		journal_detail::PutU32(H, Crc32c(H.data(), H.size()));
		Out.append(H);
	}

	bool DecodeHeader(std::string_view Bytes, FileHeader& Out, const LogFormat& Format)
	{
		if (Bytes.size() < HeaderBytes || Format.Magic.size() != 8 || Bytes.substr(0, 8) != Format.Magic)
		{
			return false;
		}
		if (Crc32c(Bytes.data(), HeaderBytes - 4) != static_cast<uint32_t>(journal_detail::GetLE(Bytes, HeaderBytes - 4, 4)))
		{
			return false;
		}
		Out.Major = static_cast<uint16_t>(journal_detail::GetLE(Bytes, 8, 2));
		Out.Minor = static_cast<uint16_t>(journal_detail::GetLE(Bytes, 10, 2));
		Out.Generation = journal_detail::GetLE(Bytes, 12, 8);
		Out.CreatedUtcMs = journal_detail::GetLE(Bytes, 20, 8);
		Out.InstancePid = static_cast<uint32_t>(journal_detail::GetLE(Bytes, 28, 4));
		Out.InstanceStart = journal_detail::GetLE(Bytes, 32, 8);
		Out.TextUnit = static_cast<uint8_t>(Bytes[40]);
		return true;
	}

	void EncodeFrame(const Record& Rec, uint64_t Seq, bool bCommit, std::string& Out)
	{
		const std::string Head = json::Serialize(Rec.Head.IsNull() ? Value::MakeObject() : Rec.Head);
		const uint32_t PayloadLen = static_cast<uint32_t>(4 + Head.size() + Rec.Body.size());
		const size_t Start = Out.size();
		Out.append(journal_detail::Sync, 4);
		journal_detail::PutU32(Out, PayloadLen);
		journal_detail::PutU16(Out, static_cast<uint16_t>(Rec.Type));
		const uint16_t Flags = static_cast<uint16_t>((Rec.Flags & ~FF_Commit) | (bCommit ? FF_Commit : 0));
		journal_detail::PutU16(Out, Flags);
		journal_detail::PutU64(Out, Seq);
		journal_detail::PutU32(Out, static_cast<uint32_t>(Head.size()));
		Out.append(Head);
		Out.append(Rec.Body);
		journal_detail::PutU32(Out, Crc32c(Out.data() + Start + 8, Out.size() - Start - 8));
	}

	FrameStatus DecodeFrame(std::string_view Bytes, size_t& InOutPos, Record& Out, uint64_t& OutSeq, bool& OutCommit)
	{
		const size_t Pos = InOutPos;
		if (Pos >= Bytes.size())
		{
			return FrameStatus::End;
		}
		uint32_t PayloadLen = 0;
		const FrameStatus Check = journal_detail::CheckFrame(Bytes, Pos, PayloadLen);
		if (Check != FrameStatus::Ok)
		{
			return Check;
		}
		const size_t Payload = Pos + journal_detail::FrameFixedBytes;
		const uint32_t HeadLen = static_cast<uint32_t>(journal_detail::GetLE(Bytes, Payload, 4));
		if (HeadLen > PayloadLen - 4)
		{
			return FrameStatus::BadHead;
		}
		Value Head;
		if (!json::Parse(Bytes.substr(Payload + 4, HeadLen), Head))
		{
			return FrameStatus::BadHead;
		}
		const uint16_t Flags = static_cast<uint16_t>(journal_detail::GetLE(Bytes, Pos + 10, 2));
		Out.Type = static_cast<RecordType>(journal_detail::GetLE(Bytes, Pos + 8, 2));
		Out.Flags = Flags;
		Out.Head = std::move(Head);
		Out.Body.assign(Bytes.substr(Payload + 4 + HeadLen, PayloadLen - 4 - HeadLen));
		OutSeq = journal_detail::GetLE(Bytes, Pos + 12, 8);
		OutCommit = (Flags & FF_Commit) != 0;
		InOutPos = Payload + PayloadLen + journal_detail::FrameCrcBytes;
		return FrameStatus::Ok;
	}

	size_t FindNextValidFrame(std::string_view Bytes, size_t From)
	{
		while (From < Bytes.size())
		{
			const size_t At = Bytes.find(std::string_view(journal_detail::Sync, 4), From);
			if (At == std::string_view::npos)
			{
				return std::string_view::npos;
			}
			uint32_t PayloadLen = 0;
			if (journal_detail::CheckFrame(Bytes, At, PayloadLen) == FrameStatus::Ok)
			{
				return At;
			}
			From = At + 1;
		}
		return std::string_view::npos;
	}

	std::string GenerationFileName(uint64_t Generation)
	{
		return LogFileName(JournalLogFormat, Generation);
	}

	bool ParseGenerationFileName(std::string_view Name, uint64_t& OutGeneration)
	{
		return ParseLogFileName(JournalLogFormat, Name, OutGeneration);
	}

	std::string LogFileName(const LogFormat& Format, uint64_t Generation)
	{
		char Buffer[24];
		std::snprintf(Buffer, sizeof(Buffer), "%016llx", static_cast<unsigned long long>(Generation));
		std::string Out(Format.Prefix);
		Out.append(Buffer);
		Out.append(Format.Suffix);
		return Out;
	}

	std::string MigratedFileName(std::string_view GenerationName, uint16_t Major)
	{
		std::string Out(GenerationName);
		Out.append(".v");
		Out.append(std::to_string(Major));
		return Out;
	}

	bool ParseMigratedFileName(const LogFormat& Format, std::string_view Name, uint64_t& OutGeneration, uint16_t& OutMajor)
	{
		const size_t Base = Format.Prefix.size() + 16 + Format.Suffix.size();
		if (Name.size() < Base + 3 || Name.substr(Base, 2) != ".v" || !ParseLogFileName(Format, Name.substr(0, Base), OutGeneration))
		{
			return false;
		}
		uint32_t Major = 0;
		for (size_t I = Base + 2; I < Name.size(); ++I)
		{
			if (Name[I] < '0' || Name[I] > '9' || Major > 6553)
			{
				return false;
			}
			Major = Major * 10 + static_cast<uint32_t>(Name[I] - '0');
		}
		if (Major > 0xFFFF)
		{
			return false;
		}
		OutMajor = static_cast<uint16_t>(Major);
		return true;
	}

	bool ParseLogFileName(const LogFormat& Format, std::string_view Name, uint64_t& OutGeneration)
	{
		const size_t P = Format.Prefix.size();
		if (Name.size() != P + 16 + Format.Suffix.size() || Name.substr(0, P) != Format.Prefix || Name.substr(P + 16) != Format.Suffix)
		{
			return false;
		}
		uint64_t V = 0;
		for (size_t I = P; I < P + 16; ++I)
		{
			const char C = Name[I];
			uint64_t D = 0;
			if (C >= '0' && C <= '9')
			{
				D = static_cast<uint64_t>(C - '0');
			}
			else if (C >= 'a' && C <= 'f')
			{
				D = static_cast<uint64_t>(C - 'a' + 10);
			}
			else if (C >= 'A' && C <= 'F')
			{
				D = static_cast<uint64_t>(C - 'A' + 10);
			}
			else
			{
				return false;
			}
			V = (V << 4) | D;
		}
		OutGeneration = V;
		return true;
	}

	// ---- Record heads ---------------------------------------------------------------------------------------

	Record MakeHeaderExt(const json::Value& Fields)
	{
		return journal_detail::MakeRecord(RecordType::HeaderExt, Fields.IsObject() ? Fields : Value::MakeObject());
	}

	Record MakeCheckpointBegin(uint32_t BufferCount)
	{
		Value H = Value::MakeObject();
		H.Set("buffers", Value::MakeInt(BufferCount));
		return journal_detail::MakeRecord(RecordType::CheckpointBegin, std::move(H));
	}

	Record MakeCheckpointEnd()
	{
		return journal_detail::MakeRecord(RecordType::CheckpointEnd, Value::MakeObject());
	}

	Record MakeBufOpen(const BufferOpenInfo& Info)
	{
		using journal_detail::Str;
		Value H = Value::MakeObject();
		H.Set("uuid", Str(Info.Uuid));
		H.Set("kind", Str(Info.Kind));
		H.Set("path", Str(Info.Path));
		H.Set("display", Str(Info.Display));
		H.Set("encoding", Str(Info.Encoding));
		H.Set("bom", Value::MakeBool(Info.bBom));
		H.Set("eol", Str(Info.Eol));
		H.Set("base", journal_detail::BaseToJson(Info.Base));
		H.Set("readonly", Value::MakeBool(Info.bReadOnly));
		return journal_detail::MakeRecord(RecordType::BufOpen, std::move(H));
	}

	std::vector<Record> MakeSnap(std::string_view Uuid, uint64_t Version, std::string_view TextUtf8, const PendingInfo* Pending)
	{
		const uint32_t Crc = Crc32c(TextUtf8);
		auto MakeHead = [&](uint64_t Part, uint64_t Parts) {
			Value H = Value::MakeObject();
			H.Set("uuid", journal_detail::Str(Uuid));
			H.Set("version", Value::MakeUInt(Version));
			H.Set("part", Value::MakeUInt(Part));
			H.Set("parts", Value::MakeUInt(Parts));
			H.Set("text_crc32c", Value::MakeUInt(Crc));
			H.Set("pending", Pending != nullptr ? journal_detail::PendingToJson(*Pending) : Value::MakeNull());
			return H;
		};
		// Each part's payload (u32 head length, head, body) stays within MaxPayloadBytes.
		const size_t HeadBytes = json::Serialize(MakeHead(UINT32_MAX, UINT32_MAX)).size();
		const size_t PartBody = MaxPayloadBytes - 4 - HeadBytes - 64;
		const size_t Parts = std::max<size_t>(1, (TextUtf8.size() + PartBody - 1) / PartBody);
		std::vector<Record> Out;
		Out.reserve(Parts);
		for (size_t P = 0; P < Parts; ++P)
		{
			const std::string_view Body = TextUtf8.substr(std::min(TextUtf8.size(), P * PartBody), PartBody);
			Out.push_back(journal_detail::MakeRecord(RecordType::Snap, MakeHead(P, Parts), std::string(Body)));
		}
		return Out;
	}

	Record MakeDelta(std::string_view Uuid, uint64_t FromVersion, uint64_t ToVersion, std::span<const DeltaOp> Ops,
		std::string_view InsertedBytes, uint32_t ResultTextCrc32c)
	{
		Value H = Value::MakeObject();
		H.Set("uuid", journal_detail::Str(Uuid));
		H.Set("from", Value::MakeUInt(FromVersion));
		H.Set("to", Value::MakeUInt(ToVersion));
		Value List = Value::MakeArray();
		for (const DeltaOp& Op : Ops)
		{
			Value Item = Value::MakeArray();
			Item.Append(Value::MakeInt(Op.Offset));
			Item.Append(Value::MakeInt(Op.Removed));
			Item.Append(Value::MakeInt(Op.InsertedLen));
			List.Append(std::move(Item));
		}
		H.Set("ops", std::move(List));
		H.Set("text_crc32c", Value::MakeUInt(ResultTextCrc32c));
		return journal_detail::MakeRecord(RecordType::Delta, std::move(H), std::string(InsertedBytes));
	}

	Record MakeMeta(std::string_view Uuid, bool bDirty, const std::optional<BaseVersion>& Base, std::string_view Path, std::string_view Display)
	{
		Value H = Value::MakeObject();
		H.Set("uuid", journal_detail::Str(Uuid));
		H.Set("dirty", Value::MakeBool(bDirty));
		H.Set("base", journal_detail::BaseToJson(Base));
		H.Set("path", journal_detail::Str(Path));
		H.Set("display", journal_detail::Str(Display));
		return journal_detail::MakeRecord(RecordType::Meta, std::move(H));
	}

	Record MakeBufClose(std::string_view Uuid, std::string_view Reason)
	{
		Value H = Value::MakeObject();
		H.Set("uuid", journal_detail::Str(Uuid));
		H.Set("reason", journal_detail::Str(Reason));
		return journal_detail::MakeRecord(RecordType::BufClose, std::move(H));
	}

	Record MakeRecoveryPending(const PendingInfo& Info)
	{
		return journal_detail::MakeRecord(RecordType::RecoveryPending, journal_detail::PendingToJson(Info));
	}

	Record MakeCleanText(std::string_view Uuid, std::string_view CleanTextUtf8, bool bSameAsLive)
	{
		Value H = Value::MakeObject();
		H.Set("uuid", journal_detail::Str(Uuid));
		H.Set("text_crc32c", Value::MakeUInt(Crc32c(CleanTextUtf8)));
		H.Set("same", Value::MakeBool(bSameAsLive));
		return journal_detail::MakeRecord(RecordType::CleanText, std::move(H), bSameAsLive ? std::string() : std::string(CleanTextUtf8),
			FF_Skippable);
	}

	Record MakeRecoveryResolved(std::string_view SourceNamespace, uint64_t SourceGeneration, std::string_view Uuid, std::string_view Action)
	{
		Value H = Value::MakeObject();
		H.Set("source_namespace", journal_detail::Str(SourceNamespace));
		H.Set("source_generation", Value::MakeUInt(SourceGeneration));
		H.Set("uuid", journal_detail::Str(Uuid));
		H.Set("action", journal_detail::Str(Action));
		return journal_detail::MakeRecord(RecordType::RecoveryResolved, std::move(H));
	}

	Record MakeSession(json::Value SessionState)
	{
		return journal_detail::MakeRecord(RecordType::Session, SessionState.IsObject() ? std::move(SessionState) : Value::MakeObject());
	}

	Record MakeRunBegin(const RunBeginInfo& Info)
	{
		Value H = Value::MakeObject();
		H.Set("run_id", Value::MakeUInt(Info.RunId));
		H.Set("number", Value::MakeUInt(Info.Number));
		H.Set("tab_uuid", journal_detail::Str(Info.TabUuid));
		H.Set("buffer_version", Value::MakeUInt(Info.BufferVersion));
		H.Set("route", journal_detail::Str(Info.Route));
		H.Set("debug", Value::MakeBool(Info.bDebug));
		H.Set("utc", Value::MakeInt(Info.UtcMs));
		return journal_detail::MakeRecord(RecordType::RunBegin, std::move(H));
	}

	Record MakeRunEnd(uint64_t RunId, std::string_view Outcome)
	{
		Value H = Value::MakeObject();
		H.Set("run_id", Value::MakeUInt(RunId));
		H.Set("outcome", journal_detail::Str(Outcome));
		return journal_detail::MakeRecord(RecordType::RunEnd, std::move(H));
	}

	Record MakeClean(int64_t UtcMs, std::span<const std::pair<std::string, uint64_t>> CleanVersions)
	{
		Value H = Value::MakeObject();
		H.Set("utc", Value::MakeInt(UtcMs));
		Value Versions = Value::MakeObject();
		for (const auto& [Uuid, Version] : CleanVersions)
		{
			Versions.Set(Uuid, Value::MakeUInt(Version));
		}
		H.Set("clean_versions", std::move(Versions));
		return journal_detail::MakeRecord(RecordType::Clean, std::move(H));
	}

	std::string JoinPath(std::string_view Dir, std::string_view Name)
	{
		std::string Out(Dir);
		if (!Out.empty() && Out.back() != '/' && Out.back() != '\\')
		{
			Out.push_back('/');
		}
		Out.append(Name);
		return Out;
	}

	bool ApplyDelta(std::string& InOutTextUtf8, std::span<const DeltaOp> Ops, std::string_view InsertedBytes)
	{
		int64_t Cursor = 0;
		int64_t Inserted = 0;
		const int64_t Size = static_cast<int64_t>(InOutTextUtf8.size());
		for (const DeltaOp& Op : Ops)
		{
			if (Op.Offset < Cursor || Op.Removed < 0 || Op.InsertedLen < 0 || Op.Offset > Size || Op.Removed > Size - Op.Offset)
			{
				return false;
			}
			Cursor = Op.Offset + Op.Removed;
			Inserted += Op.InsertedLen;
		}
		if (Inserted != static_cast<int64_t>(InsertedBytes.size()))
		{
			return false;
		}
		std::string Out;
		Out.reserve(static_cast<size_t>(Size + Inserted));
		Cursor = 0;
		size_t From = 0;
		for (const DeltaOp& Op : Ops)
		{
			Out.append(InOutTextUtf8, static_cast<size_t>(Cursor), static_cast<size_t>(Op.Offset - Cursor));
			Out.append(InsertedBytes.substr(From, static_cast<size_t>(Op.InsertedLen)));
			From += static_cast<size_t>(Op.InsertedLen);
			Cursor = Op.Offset + Op.Removed;
		}
		Out.append(InOutTextUtf8, static_cast<size_t>(Cursor), std::string::npos);
		InOutTextUtf8 = std::move(Out);
		return true;
	}

	// ---- Writer ---------------------------------------------------------------------------------------------

	struct Writer::Impl
	{
		/** A generation being written through its .tmp (§10.2.3 steps 1-3): the session's first one, or a rotation. */
		struct Build
		{
			bool bActive = false;
			bool bRotation = false;
			uint64_t Gen = 0;
			std::string Temp;
			std::string Final;
			FileOps::Handle Tmp = FileOps::InvalidHandle;
			std::vector<Record> Body;      // without HEADER_EXT (it is written first)
			size_t NextRecord = 0;
			bool bEndEncoded = false;
			std::string Pending;           // encoded and not yet written
			size_t PendingPos = 0;
			bool bNeedsFlush = false;      // the last unit wrote bytes: the next one flushes them
			uint64_t Seq = 0;              // the last seq encoded
			uint64_t Written = 0;          // bytes written to the .tmp
			std::vector<std::vector<Record>> CatchUp;   // batches committed to g meanwhile (step 2)
		};

		FileOps& Ops;
		std::string Dir;
		FileHeader Proto;
		const LogFormat* Format = &JournalLogFormat;
		json::Value HeaderExtFields = json::Value::MakeObject();
		uint64_t Gen = 0;
		bool bHasGeneration = false;
		FileOps::Handle Handle = FileOps::InvalidHandle;
		uint64_t Bytes = 0;
		uint64_t Seq = 0;
		double LastCheckpointSec = 0.0;
		bool bCommittedSinceCheckpoint = false;
		bool bNeedsCheckpoint = false;   // the file may hold a failed batch; Bytes is then a lower bound
		/** persist-M4: the generation was renamed into place but could not be opened for append; Commit opens it first. */
		bool bHandleLost = false;
		std::string Error;
		Build B;

		Impl(FileOps& InOps, std::string InDir, FileHeader InProto, const LogFormat& InFormat)
			: Ops(InOps), Dir(std::move(InDir)), Proto(InProto), Format(&InFormat)
		{
		}

		bool Fail(const char* What)
		{
			Error = std::string(What) + ": " + Ops.LastError();
			return false;
		}

		std::string FileName(uint64_t G) const
		{
			return LogFileName(*Format, G);
		}

		/**
		 * After a failed append or flush: the caller treats the batch as not written, so its bytes (a torn prefix, or the
		 * whole batch with its commit frame when only the flush failed) are cut back off; the next successful flush makes
		 * the cut durable. When the cut fails, or Bytes is no longer exact, only a new checkpoint makes the file agree.
		 */
		void CutBackFailedBatch()
		{
			if (bNeedsCheckpoint || !Ops.Truncate(Handle, Bytes))
			{
				bNeedsCheckpoint = true;
			}
		}

		/** A file the reader or the writer sets aside: touched first, because a rename keeps the last write time (§10.2.4). */
		void SetAside(const std::string& Path, const std::string& NewPath)
		{
			Ops.Touch(Path);
			Ops.RenameNew(Path, NewPath);
		}

		/**
		 * §10.2.4, §10.6: a damaged generation (.corrupt) and an older major's (.v<major>) are kept 30 days after they were set
		 * aside, then deleted by the next session start. They were touched before the rename, so their write time is that
		 * moment; their header's time never is (it is older).
		 */
		void RemoveExpiredSetAside(int64_t NowUtcMs)
		{
			static constexpr std::string_view Corrupt = ".corrupt";
			std::vector<std::string> Names;
			if (NowUtcMs <= 0 || !Ops.List(Dir, Names))
			{
				return;
			}
			for (const std::string& Name : Names)
			{
				const std::string_view View(Name);
				uint64_t G = 0;
				uint16_t Major = 0;
				int64_t Retention = 0;
				if (View.size() > Corrupt.size() && View.substr(View.size() - Corrupt.size()) == Corrupt
					&& ParseLogFileName(*Format, View.substr(0, View.size() - Corrupt.size()), G))
				{
					Retention = CorruptRetentionMs;
				}
				else if (ParseMigratedFileName(*Format, View, G, Major))
				{
					Retention = MigratedRetentionMs;
				}
				else
				{
					continue;
				}
				const std::string Path = JoinPath(Dir, Name);
				int64_t WrittenUtcMs = 0;
				if (!Ops.ModifiedUtcMs(Path, WrittenUtcMs))
				{
					continue;   // no time to go by: kept
				}
				// A time in the future (a clock jump) keeps the file (§10.11: retention clamps future timestamps).
				if (WrittenUtcMs <= NowUtcMs && NowUtcMs - WrittenUtcMs >= Retention)
				{
					Ops.Remove(Path);
				}
			}
		}

		/** Step 1 begins: the .tmp, its header, HEADER_EXT and CHECKPOINT_BEGIN queued; the body is encoded slice by slice. */
		bool BeginBuild(uint64_t G, std::vector<Record> Body, int64_t NowUtcMs, uint64_t PreviousGeneration, bool bRotation)
		{
			AbandonBuild();
			Build N;
			N.Gen = G;
			N.bRotation = bRotation;
			N.Final = JoinPath(Dir, FileName(G));
			N.Temp = N.Final + ".tmp";
			if (!Ops.MakeDirs(Dir))
			{
				return Fail("journal folder");
			}
			if (Ops.Exists(N.Temp))
			{
				Ops.Remove(N.Temp);
			}
			FileHeader Header = Proto;
			Header.Major = Format->Major;
			Header.Minor = Format->Minor;
			Header.Generation = G;
			Header.CreatedUtcMs = static_cast<uint64_t>(std::max<int64_t>(NowUtcMs, 0));
			Header.TextUnit = 1;
			EncodeHeader(Header, N.Pending, *Format);

			Value Ext = HeaderExtFields;
			bool bExtFromBody = false;
			uint32_t Counted = 0;
			for (const Record& R : Body)
			{
				if (R.Type == RecordType::HeaderExt && !bExtFromBody && R.Head.IsObject())
				{
					Ext = R.Head;
					bExtFromBody = true;
				}
				if (static_cast<uint16_t>(R.Type) == Format->CountedType)
				{
					++Counted;
				}
			}
			if (Ext.Find("previous_generation") == nullptr)
			{
				Ext.Set("previous_generation", Value::MakeUInt(PreviousGeneration));
			}
			EncodeFrame(MakeHeaderExt(Ext), ++N.Seq, false, N.Pending);
			Value BeginHead = Value::MakeObject();
			BeginHead.Set(std::string(Format->CountKey), Value::MakeInt(Counted));
			EncodeFrame(journal_detail::MakeRecord(RecordType::CheckpointBegin, std::move(BeginHead)), ++N.Seq, false, N.Pending);
			Body.erase(std::remove_if(Body.begin(), Body.end(), [](const Record& R) { return R.Type == RecordType::HeaderExt; }), Body.end());
			N.Body = std::move(Body);
			if (!Ops.CreateExclusive(N.Temp, N.Tmp))
			{
				return Fail("create checkpoint");
			}
			N.bActive = true;
			B = std::move(N);
			return true;
		}

		/** One unit of step 1: a write of at most MaxBytes (encoding records as needed), or the flush of the last write. */
		RotationStep StepBuild(size_t MaxBytes)
		{
			if (!B.bActive)
			{
				return RotationStep::Failed;
			}
			if (B.bNeedsFlush)
			{
				B.bNeedsFlush = false;
				if (!Ops.Flush(B.Tmp))
				{
					Fail("flush checkpoint");
					AbandonBuild();
					return RotationStep::Failed;
				}
				return B.PendingPos >= B.Pending.size() && B.bEndEncoded ? RotationStep::Ready : RotationStep::More;
			}
			MaxBytes = std::max<size_t>(MaxBytes, 1);
			while (B.Pending.size() - B.PendingPos < MaxBytes && !B.bEndEncoded)
			{
				if (B.NextRecord < B.Body.size())
				{
					EncodeFrame(B.Body[B.NextRecord], ++B.Seq, false, B.Pending);
					B.Body[B.NextRecord] = Record();   // its bytes are queued: the text it held is not needed any more
					++B.NextRecord;
				}
				else
				{
					EncodeFrame(MakeCheckpointEnd(), ++B.Seq, true, B.Pending);
					B.bEndEncoded = true;
				}
			}
			const size_t Chunk = std::min(MaxBytes, B.Pending.size() - B.PendingPos);
			if (Chunk == 0)
			{
				return RotationStep::Ready;   // everything is written and flushed
			}
			{
				if (!Ops.Append(B.Tmp, std::string_view(B.Pending).substr(B.PendingPos, Chunk)))
				{
					Fail("write checkpoint");
					AbandonBuild();
					return RotationStep::Failed;
				}
				B.PendingPos += Chunk;
				B.Written += Chunk;
				B.bNeedsFlush = true;
			}
			if (B.PendingPos >= (1u << 20) || B.PendingPos >= B.Pending.size())
			{
				B.Pending.erase(0, B.PendingPos);
				B.PendingPos = 0;
			}
			return RotationStep::More;   // a write is always followed by its flush, its own unit
		}

		/**
		 * Steps 2-3: the catch-up batches, flush, close, write-through rename, then open the new generation for append. The
		 * rename is the switch (persist-M4): once it succeeded bOutRenamed is set and OutBytes/OutSeq describe the new file,
		 * even when the open fails (false, OutHandle invalid).
		 */
		bool FinishBuild(FileOps::Handle& OutHandle, uint64_t& OutBytes, uint64_t& OutSeq, bool& bOutRenamed)
		{
			bOutRenamed = false;
			OutHandle = FileOps::InvalidHandle;
			if (!B.bActive || B.PendingPos < B.Pending.size() || !B.bEndEncoded || B.bNeedsFlush)
			{
				Error = "checkpoint not complete";
				AbandonBuild();
				return false;
			}
			std::string Tail;
			for (const std::vector<Record>& Batch : B.CatchUp)
			{
				for (size_t K = 0; K < Batch.size(); ++K)
				{
					EncodeFrame(Batch[K], ++B.Seq, K + 1 == Batch.size(), Tail);
				}
			}
			if (!Tail.empty() && (!Ops.Append(B.Tmp, Tail) || !Ops.Flush(B.Tmp)))
			{
				Fail("write catch-up");
				AbandonBuild();
				return false;
			}
			Ops.Close(B.Tmp);
			B.Tmp = FileOps::InvalidHandle;
			if (!Ops.RenameNew(B.Temp, B.Final))
			{
				Fail("rename checkpoint");
				AbandonBuild();
				return false;
			}
			Ops.FlushDir(Dir);
			bOutRenamed = true;
			OutBytes = B.Written + Tail.size();
			OutSeq = B.Seq;
			const std::string Final = B.Final;
			B = Build();
			FileOps::Handle H = FileOps::InvalidHandle;
			if (!Ops.OpenAppend(Final, H))
			{
				return Fail("open generation");
			}
			OutHandle = H;
			return true;
		}

		void AbandonBuild()
		{
			if (B.Tmp != FileOps::InvalidHandle)
			{
				Ops.Close(B.Tmp);
			}
			if (B.bActive && !B.Temp.empty())
			{
				Ops.Remove(B.Temp);
			}
			B = Build();
		}

		/** Writes the whole of a build in one call. */
		bool RunBuild(FileOps::Handle& OutHandle, uint64_t& OutBytes, uint64_t& OutSeq, bool& bOutRenamed)
		{
			bOutRenamed = false;
			for (;;)
			{
				const RotationStep Step = StepBuild(RotationSliceBytes);
				if (Step == RotationStep::Failed)
				{
					return false;
				}
				if (Step == RotationStep::Ready)
				{
					return FinishBuild(OutHandle, OutBytes, OutSeq, bOutRenamed);
				}
			}
		}

		/** The switch to a generation now on disk (persist-M4: also when it could not be opened; Commit opens it first). */
		void SwitchTo(uint64_t G, FileOps::Handle H, uint64_t InBytes, uint64_t InSeq, double NowSec, bool bCommitted)
		{
			CloseHandle();
			Handle = H;
			bHandleLost = H == FileOps::InvalidHandle;
			Gen = G;
			bHasGeneration = true;
			Bytes = InBytes;
			Seq = InSeq;
			LastCheckpointSec = NowSec;
			bCommittedSinceCheckpoint = bCommitted;
			bNeedsCheckpoint = false;
		}

		/**
		 * §10.2.3 step 5 with B12: generation g-1 is deleted only when its header says this format. An older major's is kept
		 * for a downgrade as .v<major> (§10.6), a newer format's is never touched (§10.6: an older plugin never writes,
		 * rotates or deletes there), and one whose header does not decode is set aside as .corrupt (kept 30 days).
		 */
		void RetirePrevious(uint64_t Previous)
		{
			const std::string Name = FileName(Previous);
			const std::string Path = JoinPath(Dir, Name);
			if (!Ops.Exists(Path))
			{
				return;
			}
			std::string Prefix;
			FileHeader Header;
			if (!Ops.ReadPrefix(Path, HeaderBytes, Prefix))
			{
				return;   // unreadable now (a sharing violation): the next session's reader deals with it
			}
			if (!DecodeHeader(Prefix, Header, *Format))
			{
				SetAside(Path, Path + ".corrupt");
			}
			else if (Header.Major == Format->Major && Header.Minor <= Format->Minor)
			{
				Ops.Remove(Path);
			}
			else if (Header.Major < Format->Major)
			{
				SetAside(Path, JoinPath(Dir, MigratedFileName(Name, Header.Major)));
			}
		}

		void CloseHandle()
		{
			if (Handle != FileOps::InvalidHandle)
			{
				Ops.Close(Handle);
				Handle = FileOps::InvalidHandle;
			}
		}
	};

	Writer::Writer(FileOps& InOps, std::string InDir, FileHeader InProto)
		: State(std::make_unique<Impl>(InOps, std::move(InDir), InProto, JournalLogFormat))
	{
	}

	Writer::Writer(FileOps& InOps, std::string InDir, FileHeader InProto, const LogFormat& InFormat)
		: State(std::make_unique<Impl>(InOps, std::move(InDir), InProto, InFormat))
	{
	}

	Writer::~Writer()
	{
		if (State->B.Tmp != FileOps::InvalidHandle)
		{
			State->Ops.Close(State->B.Tmp);   // the .tmp stays: the next reader removes it (§10.2.4 step 1)
		}
		State->CloseHandle();
	}

	void Writer::SetHeaderExtFields(json::Value Fields)
	{
		State->HeaderExtFields = Fields.IsObject() ? std::move(Fields) : Value::MakeObject();
	}

	bool Writer::StartGeneration(uint64_t Generation, std::span<const Record> CheckpointBody, int64_t NowUtcMs, double NowSec)
	{
		Impl& I = *State;
		const uint64_t Previous = Generation > 0 ? Generation - 1 : 0;
		if (!I.BeginBuild(Generation, std::vector<Record>(CheckpointBody.begin(), CheckpointBody.end()), NowUtcMs, Previous, false))
		{
			return false;
		}
		FileOps::Handle H = FileOps::InvalidHandle;
		uint64_t Bytes = 0;
		uint64_t Seq = 0;
		bool bRenamed = false;
		if (!I.RunBuild(H, Bytes, Seq, bRenamed) && !bRenamed)
		{
			return false;
		}
		// Renamed means started (persist-M4): the checkpoint is durable; with no handle the next Commit opens it first.
		I.SwitchTo(Generation, H, Bytes, Seq, NowSec, false);
		I.RemoveExpiredSetAside(NowUtcMs);
		return true;
	}

	bool Writer::Commit(std::span<const Record> Batch)
	{
		Impl& I = *State;
		if (Batch.empty())
		{
			return true;
		}
		if (I.Handle == FileOps::InvalidHandle && I.bHandleLost)
		{
			// persist-M4: the current generation is on disk but was not opened after its rename; never fall back to g-1.
			FileOps::Handle H = FileOps::InvalidHandle;
			if (!I.Ops.OpenAppend(JoinPath(I.Dir, I.FileName(I.Gen)), H))
			{
				return I.Fail("open generation");
			}
			I.Handle = H;
			I.bHandleLost = false;
		}
		if (I.Handle == FileOps::InvalidHandle)
		{
			I.Error = "no open generation";
			return false;
		}
		std::string Buf;
		uint64_t S = I.Seq;
		for (size_t K = 0; K < Batch.size(); ++K)
		{
			EncodeFrame(Batch[K], ++S, K + 1 == Batch.size(), Buf);
		}
		// The seqs are spent even when the batch fails: the complete frames of a torn batch keep theirs in the file, and the
		// reader drops every later frame whose seq is not above them (§10.2.4 step 3).
		I.Seq = S;
		if (!I.Ops.Append(I.Handle, Buf))
		{
			I.Fail("append");
			I.CutBackFailedBatch();
			return false;
		}
		if (!I.Ops.Flush(I.Handle))
		{
			I.Fail("flush");
			I.CutBackFailedBatch();
			return false;
		}
		I.Bytes += Buf.size();
		I.bCommittedSinceCheckpoint = true;
		if (I.B.bActive && I.B.bRotation)
		{
			// §10.2.3 step 2: a durable batch of g that the checkpoint in progress does not hold; FinishRotation appends it to
			// the .tmp. A failed batch never joins (the caller treats it as not written).
			I.B.CatchUp.emplace_back(Batch.begin(), Batch.end());
		}
		return true;
	}

	bool Writer::NeedsRotation(double NowSec) const
	{
		const Impl& I = *State;
		return I.bHasGeneration && !I.B.bActive
			&& (I.bNeedsCheckpoint || I.bHandleLost || I.Bytes > RotateBytes
				|| (I.bCommittedSinceCheckpoint && NowSec - I.LastCheckpointSec >= RotateSeconds));
	}

	bool Writer::NeedsCheckpoint() const
	{
		return State->bHasGeneration && State->bNeedsCheckpoint;
	}

	bool Writer::BeginRotation(std::vector<Record> CheckpointBody, int64_t NowUtcMs)
	{
		Impl& I = *State;
		if (!I.bHasGeneration)
		{
			I.Error = "no open generation";
			return false;
		}
		if (I.B.bActive)
		{
			I.Error = "a rotation is in progress";
			return false;
		}
		return I.BeginBuild(I.Gen + 1, std::move(CheckpointBody), NowUtcMs, I.Gen, true);
	}

	bool Writer::IsRotating() const
	{
		return State->B.bActive && State->B.bRotation;
	}

	RotationStep Writer::StepRotation(size_t MaxBytes)
	{
		if (!IsRotating())
		{
			return RotationStep::Failed;
		}
		return State->StepBuild(MaxBytes);
	}

	bool Writer::FinishRotation(double NowSec)
	{
		Impl& I = *State;
		if (!IsRotating())
		{
			I.Error = "no rotation in progress";
			return false;
		}
		const uint64_t Old = I.Gen;
		const bool bCaughtUp = !I.B.CatchUp.empty();
		FileOps::Handle H = FileOps::InvalidHandle;
		uint64_t Bytes = 0;
		uint64_t Seq = 0;
		bool bRenamed = false;
		const bool bOpened = I.FinishBuild(H, Bytes, Seq, bRenamed);
		if (!bRenamed)
		{
			return false;   // nothing renamed: keep appending to the current generation
		}
		// The rename is the switch (persist-M4): g+1 holds only durable batches (a failed one of g never joined the catch-up),
		// and nothing is appended to g again, even when g+1 could not be opened (false; Commit opens it first).
		I.SwitchTo(Old + 1, H, Bytes, Seq, NowSec, bCaughtUp);
		if (Old > 0)
		{
			I.RetirePrevious(Old - 1);   // step 5: g stays as the kept previous generation
		}
		return bOpened;
	}

	void Writer::AbandonRotation()
	{
		if (IsRotating())
		{
			State->AbandonBuild();
		}
	}

	uint64_t Writer::RotationBytesWritten() const
	{
		return State->B.Written;
	}

	size_t Writer::RotationCatchUpBatches() const
	{
		return State->B.CatchUp.size();
	}

	bool Writer::Rotate(std::span<const Record> CheckpointBody, int64_t NowUtcMs, double NowSec)
	{
		if (!BeginRotation(std::vector<Record>(CheckpointBody.begin(), CheckpointBody.end()), NowUtcMs))
		{
			return false;
		}
		for (;;)
		{
			const RotationStep Step = StepRotation(RotationSliceBytes);
			if (Step == RotationStep::Failed)
			{
				return false;
			}
			if (Step == RotationStep::Ready)
			{
				return FinishRotation(NowSec);
			}
		}
	}

	uint64_t Writer::Generation() const
	{
		return State->Gen;
	}

	uint64_t Writer::FileBytes() const
	{
		return State->Bytes;
	}

	uint64_t Writer::LastSeq() const
	{
		return State->Seq;
	}

	const std::string& Writer::LastError() const
	{
		return State->Error;
	}

	// ---- Reader ---------------------------------------------------------------------------------------------

	std::span<const OlderMajorReader> KnownOlderMajors()
	{
		// Format 1 is the first journal format: no older major exists yet. A reader for major N-1 joins this list in the
		// build that introduces major N (§10.6).
		return {};
	}

	RecoveredNamespace ReadNamespace(FileOps& Ops, std::string_view Dir, bool bMayModify)
	{
		return ReadNamespace(Ops, Dir, bMayModify, KnownOlderMajors());
	}

	RecoveredNamespace ReadNamespace(FileOps& Ops, std::string_view Dir, bool bMayModify, std::span<const OlderMajorReader> OlderMajors)
	{
		RecoveredNamespace R;
		std::vector<std::string> Names;
		if (!Ops.List(Dir, Names))
		{
			return R;
		}
		std::vector<uint64_t> Generations;
		std::vector<std::string> Stale;
		for (const std::string& Name : Names)
		{
			if (Name.size() > 4 && Name.compare(Name.size() - 4, 4, ".tmp") == 0)
			{
				Stale.push_back(Name);
				continue;
			}
			uint64_t G = 0;
			if (ParseGenerationFileName(Name, G))
			{
				Generations.push_back(G);
			}
		}
		std::sort(Generations.begin(), Generations.end(), std::greater<uint64_t>());

		// §10.6: an older plugin never writes, rotates or deletes in a namespace that a newer format wrote, so the newest
		// header that decodes decides before anything is removed or renamed (a damaged newer header does not hide it). A
		// newer minor of this major is still read in full (unknown skippable records are skipped); a newer major is
		// header only (below).
		for (const uint64_t G : Generations)
		{
			std::string Prefix;
			FileHeader Newest;
			if (Ops.ReadPrefix(JoinPath(Dir, GenerationFileName(G)), HeaderBytes, Prefix) && DecodeHeader(Prefix, Newest))
			{
				R.bNewerMinor = Newest.Major == FormatMajor && Newest.Minor > FormatMinor;
				if (Newest.Major > FormatMajor || R.bNewerMinor)
				{
					bMayModify = false;
				}
				break;
			}
		}
		// §10.2.4: a damaged generation is kept 30 days from now. A rename keeps the last write time, so the file is
		// touched first (a crash between the two then cannot leave an old time on a .corrupt file).
		auto SetAsideAs = [&Ops](const std::string& Path, const std::string& NewPath) {
			Ops.Touch(Path);
			Ops.RenameNew(Path, NewPath);
		};
		auto SetAside = [&SetAsideAs](const std::string& Path) { SetAsideAs(Path, Path + ".corrupt"); };
		if (bMayModify)
		{
			for (const std::string& Name : Stale)
			{
				Ops.Remove(JoinPath(Dir, Name));   // step 1: a checkpoint that never finished
			}
		}
		if (Generations.empty())
		{
			return R;
		}
		R.bFound = true;
		R.Generation = Generations.front();

		std::vector<std::string> Damaged;   // newer generations that failed, newest first (salvaged on top)
		bool bSkippedNewer = false;         // a newer generation was damaged or unreadable: the fallback (§10.2.4 step 4)
		for (size_t K = 0; K < Generations.size(); ++K)
		{
			const uint64_t G = Generations[K];
			const std::string Path = JoinPath(Dir, GenerationFileName(G));
			std::string Bytes;
			if (!Ops.ReadAll(Path, Bytes))
			{
				// persist-M2: not readable even after the FileOps retries (§10.11, a scanner's lock). It counts as damaged and has
				// nothing to salvage: the previous generation is used. Review r2/r3 (§10.6): its header is unknown (it may be a
				// newer minor or major, which is never modified), so the reader leaves it exactly where it is and reads nothing
				// more of it; before a session's generation goes above it, SetAsideUnusedNewer reads its header first and sets it
				// aside only when it is this format or undecodable.
				bSkippedNewer = true;
				R.bSkippedUnreadable = true;   // (instances, review r2)
				continue;
			}
			FileHeader Header;
			const bool bHeaderOk = DecodeHeader(Bytes, Header);
			if (bHeaderOk && Header.Major > FormatMajor)
			{
				R.bNewerFormat = true;   // §10.6: header only, read-only
				R.Header = Header;
				return R;
			}
			if (bHeaderOk && Header.Major == FormatMajor && Header.Minor > FormatMinor)
			{
				R.bNewerMinor = true;   // §10.6, even below a damaged generation of this format: nothing more is modified
				bMayModify = false;
			}
			journal_detail::ReadState State;
			journal_detail::GenerationResult Result;
			if (bHeaderOk && Header.Major < FormatMajor)
			{
				// §10.6: a newer plugin on older data dispatches on every older major it knows; recovery is then normal, and
				// this session's first generation writes the new format. An older major this build cannot read is skipped and
				// kept as it is (never set aside: an older plugin may still read it).
				const OlderMajorReader* Reader = nullptr;
				for (const OlderMajorReader& Candidate : OlderMajors)
				{
					if (Candidate.Major == Header.Major && Candidate.Upgrade != nullptr)
					{
						Reader = &Candidate;
					}
				}
				UpgradedGeneration Up;
				if (Reader == nullptr || !Reader->Upgrade(Bytes, Up) || !Up.bCheckpointComplete)
				{
					continue;
				}
				for (const std::vector<Record>& Batch : Up.Batches)
				{
					State.ApplyBatch(Batch);
				}
				State.NoteUtc(static_cast<int64_t>(Header.CreatedUtcMs));
				Result.bHeaderOk = true;
				Result.bCheckpointComplete = true;
				Result.bSalvage = Up.bSalvage;
				Result.Header = Header;
				R.SourceMajor = Header.Major;
			}
			else if (bHeaderOk)
			{
				Result = journal_detail::ReadGeneration(Bytes, State);
			}
			if (!bHeaderOk || !Result.bCheckpointComplete)
			{
				bSkippedNewer = true;
				Damaged.push_back(std::move(Bytes));
				if (bMayModify)
				{
					SetAside(Path);   // kept, never deleted by the reader
				}
				continue;
			}
			// Step 4: the previous generation in full, then whatever newer per-buffer state salvage finds.
			R.bFallbackUsed = bSkippedNewer;
			R.bSalvageUsed = Result.bSalvage;
			R.Header = Header;
			R.UsedGeneration = G;
			for (auto It = Damaged.rbegin(); It != Damaged.rend(); ++It)
			{
				journal_detail::ReadGeneration(*It, State);
				R.bSalvageUsed = true;
			}
			R.Buffers = std::move(State.Buffers);
			R.Session = std::move(State.Session);
			R.OpenRuns = std::move(State.OpenRuns);
			R.Pending = std::move(State.Pending);
			R.CleanVersions = std::move(State.CleanVersions);
			R.bClean = State.bLastBatchClean;
			R.LastRecordUtcMs = State.LastUtcMs;
			if (bMayModify)
			{
				// (slice A choice) Every session starts a generation above this one, and G stays its fallback (§10.2.3: one
				// complete closed generation and the current one), so older generations are never read again. Only this
				// format's are deleted: an older major's is kept 30 days as wal.<g>.sjl.v<major> for a downgrade (§10.6), a
				// newer one is never touched, and a bad header becomes .corrupt (kept 30 days).
				for (size_t Older = K + 1; Older < Generations.size(); ++Older)
				{
					const std::string OldPath = JoinPath(Dir, GenerationFileName(Generations[Older]));
					std::string Prefix;
					FileHeader OldHeader;
					if (!Ops.ReadPrefix(OldPath, HeaderBytes, Prefix))
					{
						continue;
					}
					if (!DecodeHeader(Prefix, OldHeader))
					{
						SetAside(OldPath);
					}
					else if (OldHeader.Major == FormatMajor && OldHeader.Minor <= FormatMinor)
					{
						Ops.Remove(OldPath);
					}
					else if (OldHeader.Major < FormatMajor)
					{
						SetAsideAs(OldPath, JoinPath(Dir, MigratedFileName(GenerationFileName(Generations[Older]), OldHeader.Major)));
					}
				}
			}
			return R;
		}
		R.bUnreadable = true;
		return R;
	}

	bool SetAsideUnusedNewer(FileOps& Ops, std::string_view Dir, const LogFormat& Format, uint64_t UsedGeneration, bool* bOutNewerFormat)
	{
		if (bOutNewerFormat != nullptr)
		{
			*bOutNewerFormat = false;
		}
		std::vector<std::string> Names;
		if (!Ops.List(Dir, Names))
		{
			return true;   // nothing listed: nothing above
		}
		// First every header (one read each), then the renames: once a newer format is found nothing is renamed (review r3,
		// §10.6: that namespace is not modified at all).
		std::vector<std::string> ToSetAside;
		for (const std::string& Name : Names)
		{
			uint64_t G = 0;
			if (!ParseLogFileName(Format, Name, G) || G <= UsedGeneration)
			{
				continue;
			}
			const std::string Path = JoinPath(Dir, Name);
			std::string Prefix;
			FileHeader Header;
			if (Ops.ReadPrefix(Path, HeaderBytes, Prefix) && DecodeHeader(Prefix, Header, Format)
				&& (Header.Major != Format.Major || Header.Minor > Format.Minor))
			{
				if (Header.Major < Format.Major)
				{
					continue;   // an older major: the next read migrates it (§10.6)
				}
				// A newer major or a newer minor of this one (review r2, §10.6): never renamed, and nothing starts above it.
				if (bOutNewerFormat != nullptr)
				{
					*bOutNewerFormat = true;
				}
				return false;
			}
			ToSetAside.push_back(Path);
		}
		bool bClear = true;
		for (const std::string& Path : ToSetAside)
		{
			Ops.Touch(Path);
			if (!Ops.RenameNew(Path, Path + ".corrupt") && Ops.Exists(Path))
			{
				bClear = false;   // still in place: no generation may start above it
			}
		}
		return bClear;
	}

	// ---- Commit policy --------------------------------------------------------------------------------------

	void CommitPolicy::OnEdit(std::string_view Buffer, double NowSec)
	{
		for (Entry& E : Entries)
		{
			if (E.Buffer == Buffer)
			{
				if (E.FirstEdit < 0.0)
				{
					E.FirstEdit = NowSec;
				}
				E.LastEdit = NowSec;
				return;
			}
		}
		Entries.push_back(Entry{std::string(Buffer), NowSec, NowSec, -1.0});
	}

	void CommitPolicy::OnViewState(std::string_view Buffer, double NowSec)
	{
		for (Entry& E : Entries)
		{
			if (E.Buffer == Buffer)
			{
				if (E.FirstViewState < 0.0)
				{
					E.FirstViewState = NowSec;
				}
				return;
			}
		}
		Entries.push_back(Entry{std::string(Buffer), -1.0, -1.0, NowSec});
	}

	void CommitPolicy::OnCommitted(std::string_view Buffer)
	{
		Forget(Buffer);
	}

	void CommitPolicy::Forget(std::string_view Buffer)
	{
		Entries.erase(std::remove_if(Entries.begin(), Entries.end(), [Buffer](const Entry& E) { return E.Buffer == Buffer; }),
			Entries.end());
	}

	bool CommitPolicy::Due(std::string_view Buffer, double NowSec) const
	{
		for (const Entry& E : Entries)
		{
			if (E.Buffer != Buffer)
			{
				continue;
			}
			if (E.FirstEdit >= 0.0 && (NowSec - E.LastEdit >= IdleDebounceSec || NowSec - E.FirstEdit >= MaxLatencySec))
			{
				return true;
			}
			// View-state-only changes coalesce for 2 s from the first one (§10.2.5), so scrolling cannot hold them back.
			return E.FirstViewState >= 0.0 && NowSec - E.FirstViewState >= ViewStateDebounceSec;
		}
		return false;
	}

	std::vector<std::string> CommitPolicy::DueBuffers(double NowSec) const
	{
		std::vector<std::string> Out;
		for (const Entry& E : Entries)
		{
			if (Due(E.Buffer, NowSec))
			{
				Out.push_back(E.Buffer);
			}
		}
		return Out;
	}

	bool CommitPolicy::AnyPending() const
	{
		return !Entries.empty();
	}

	// ---- Batch planner --------------------------------------------------------------------------------------

	struct BatchPlanner::Impl
	{
		std::map<std::string, PlannedBuffer> Buffers;
		std::vector<std::pair<PendingInfo, std::string>> Pending;
		json::Value Session;
		std::map<uint64_t, RunBeginInfo> OpenRuns;
		std::vector<CommitRequest> RetryQueue;

		/** UTF-8 byte length of UTF-16 text (a lone surrogate counts as U+FFFD, as the journal's UTF-8 text does). */
		static int64_t Utf8Length(std::u16string_view Text)
		{
			int64_t Bytes = 0;
			for (size_t Index = 0; Index < Text.size(); ++Index)
			{
				const char16_t C = Text[Index];
				if (C < 0x80)
				{
					Bytes += 1;
				}
				else if (C < 0x800)
				{
					Bytes += 2;
				}
				else if (C >= 0xD800 && C <= 0xDBFF && Index + 1 < Text.size() && Text[Index + 1] >= 0xDC00 && Text[Index + 1] <= 0xDFFF)
				{
					Bytes += 4;
					++Index;
				}
				else
				{
					Bytes += 3;
				}
			}
			return Bytes;
		}

		/** A clean text too large for one frame is not kept: the buffer then counts as created in its session (§10.7.1). */
		static bool CleanFits(const TextSnapshot& Clean)
		{
			return Clean.Length() * 3 < static_cast<int64_t>(MaxPayloadBytes) - 4096;
		}

		/** CLEAN_TEXT after a buffer's text: "same" when the clean text is that text. */
		static void AddCleanText(const std::string& Uuid, const TextSnapshot& Live, const TextSnapshot& Clean, std::vector<Record>& Out)
		{
			if (!CleanFits(Clean))
			{
				return;
			}
			// By identity or content: versions of two snapshots say nothing unless they come from one TextBuffer.
			const bool bSame = Clean.SameRepAs(Live) || (Clean.Length() == Live.Length() && Clean.Text() == Live.Text());
			Out.push_back(MakeCleanText(Uuid, Clean.TextUtf8(), bSame));
		}

		/** The records of one request into Plan, building on what earlier parts and requests of the same plan planned. */
		void PlanInto(const CommitRequest& Request, bool bSnapOnly, BatchPlan& Plan) const
		{
			auto Current = [this, &Plan](const std::string& Uuid) -> const PlannedBuffer*
			{
				const auto Planned = Plan.States.find(Uuid);
				if (Planned != Plan.States.end())
				{
					return Planned->second.has_value() ? &*Planned->second : nullptr;
				}
				const auto Durable = Buffers.find(Uuid);
				return Durable != Buffers.end() ? &Durable->second : nullptr;
			};
			Plan.Tickets.push_back(Request.Ticket);
			for (const BufferPart& Part : Request.Buffers)
			{
				const PlannedBuffer* Cur = Current(Part.Uuid);
				if (Cur == nullptr || (Part.bOpenEvent && Part.bHasSnapshot && !Part.bClose))
				{
					if (Part.bClose || !Part.bHasSnapshot)
					{
						continue;   // never journaled in this generation: nothing to close
					}
					PlannedBuffer New;
					New.Open = Part.Open;
					New.Committed = Part.Snapshot;
					New.bDirty = Cur != nullptr ? Cur->bDirty : false;
					New.CleanText = Cur != nullptr ? Cur->CleanText : std::nullopt;
					Plan.Records.push_back(MakeBufOpen(Part.Open));
					for (Record& Snap : MakeSnap(Part.Uuid, Part.Snapshot.Version(), Part.Snapshot.TextUtf8()))
					{
						Plan.Records.push_back(std::move(Snap));
					}
					Cur = &*Plan.States.insert_or_assign(Part.Uuid, std::move(New)).first->second;
				}
				else if (Part.bHasSnapshot && Part.Snapshot.Version() > Cur->Committed.Version())
				{
					// Versions of one buffer only grow, so an older snapshot (a run's submit-time text after the 4 Hz commit of a
					// newer one, PER-03) is already covered and never moves the journal back.
					const EditRange Range = DiffSingleRange(Cur->Committed, Part.Snapshot);
					const std::u16string OldText = Cur->Committed.Text();
					const std::string NewUtf8 = Part.Snapshot.TextUtf8();
					const std::u16string_view Old(OldText);
					const int64_t OffsetBytes = Utf8Length(Old.substr(0, static_cast<size_t>(Range.At)));
					const int64_t RemovedBytes = Utf8Length(Old.substr(static_cast<size_t>(Range.At), static_cast<size_t>(Range.DeleteLen)));
					std::string Inserted;
					utf::Utf16ToUtf8(Range.Insert, Inserted);
					// A delta larger than half of the text becomes a SNAP (proposed, §10.2.2), and so does every one while the
					// generation may hold a failed batch the reader rebuilt (Writer::NeedsCheckpoint, until the next rotation).
					if (bSnapOnly || static_cast<int64_t>(Inserted.size()) + RemovedBytes > static_cast<int64_t>(NewUtf8.size()) / 2 + 64)
					{
						for (Record& Snap : MakeSnap(Part.Uuid, Part.Snapshot.Version(), NewUtf8))
						{
							Plan.Records.push_back(std::move(Snap));
						}
					}
					else
					{
						const DeltaOp Op{ OffsetBytes, RemovedBytes, static_cast<int64_t>(Inserted.size()) };
						Plan.Records.push_back(MakeDelta(Part.Uuid, Cur->Committed.Version(), Part.Snapshot.Version(),
							std::span<const DeltaOp>(&Op, 1), Inserted, Crc32c(NewUtf8)));
					}
					PlannedBuffer Next = *Cur;
					Next.Committed = Part.Snapshot;
					Cur = &*Plan.States.insert_or_assign(Part.Uuid, std::move(Next)).first->second;
				}

				if (Part.CleanText.has_value())
				{
					PlannedBuffer Next = *Cur;
					Next.CleanText = Part.CleanText;
					AddCleanText(Part.Uuid, Next.Committed, *Part.CleanText, Plan.Records);
					Cur = &*Plan.States.insert_or_assign(Part.Uuid, std::move(Next)).first->second;
				}
				if (Part.bMeta)
				{
					PlannedBuffer Next = *Cur;
					Next.Open = Part.Open;
					Next.bDirty = Part.bDirty;
					Plan.Records.push_back(MakeMeta(Part.Uuid, Part.bDirty, Part.Open.Base, Part.Open.Path, Part.Open.Display));
					Cur = &*Plan.States.insert_or_assign(Part.Uuid, std::move(Next)).first->second;
				}
				if (Part.bClose)
				{
					Plan.Records.push_back(MakeBufClose(Part.Uuid, Part.CloseReason));
					Plan.States.insert_or_assign(Part.Uuid, std::optional<PlannedBuffer>());
				}
			}

			if (Request.bHasSession)
			{
				Plan.Records.push_back(MakeSession(Request.Session));
				Plan.Session = Request.Session;
			}
			for (const Record& Extra : Request.Extra)
			{
				if (Extra.Type == RecordType::RecoveryResolved)
				{
					Plan.Resolved.push_back(Extra.Head.Get("uuid").AsString());
				}
				Plan.Records.push_back(Extra);
			}
			if (Request.RunBegin.has_value())
			{
				Plan.RunBegins.push_back(*Request.RunBegin);
			}
			if (Request.RunEndId != 0)
			{
				Plan.RunEnds.push_back(Request.RunEndId);
			}
			if (Request.bClean)
			{
				Plan.Records.push_back(MakeClean(Request.CleanUtcMs, Request.CleanVersions));
				Plan.bClean = true;
			}
		}
	};

	BatchPlanner::BatchPlanner()
		: State(std::make_unique<Impl>())
	{
	}

	BatchPlanner::~BatchPlanner() = default;

	void BatchPlanner::SeedBuffer(const BufferPart& Part)
	{
		if (!Part.bHasSnapshot)
		{
			return;
		}
		PlannedBuffer& B = State->Buffers[Part.Uuid];
		B.Open = Part.Open;
		B.Committed = Part.Snapshot;
		B.bDirty = Part.bDirty;
		if (Part.CleanText.has_value())
		{
			B.CleanText = Part.CleanText;
		}
	}

	void BatchPlanner::SetPendingRecoveries(std::vector<std::pair<PendingInfo, std::string>> Pending)
	{
		State->Pending = std::move(Pending);
	}

	void BatchPlanner::SetSession(json::Value Session)
	{
		State->Session = std::move(Session);
	}

	std::vector<Record> BatchPlanner::BuildCheckpoint() const
	{
		std::vector<Record> Body;
		for (const auto& [Uuid, B] : State->Buffers)
		{
			Body.push_back(MakeBufOpen(B.Open));
			for (Record& Snap : MakeSnap(Uuid, B.Committed.Version(), B.Committed.TextUtf8()))
			{
				Body.push_back(std::move(Snap));
			}
			if (B.bDirty)
			{
				Body.push_back(MakeMeta(Uuid, true, B.Open.Base, B.Open.Path, B.Open.Display));
			}
			if (B.CleanText.has_value())
			{
				Impl::AddCleanText(Uuid, B.Committed, *B.CleanText, Body);
			}
		}
		// Every unresolved recovery lives in every checkpoint until RECOVERY_RESOLVED (§10.7.1).
		for (const auto& [Info, Text] : State->Pending)
		{
			Body.push_back(MakeRecoveryPending(Info));
			for (Record& Snap : MakeSnap(Info.Uuid, 0, Text, &Info))
			{
				Body.push_back(std::move(Snap));
			}
		}
		if (!State->Session.IsNull())
		{
			Body.push_back(MakeSession(State->Session));
		}
		for (const auto& [Id, Run] : State->OpenRuns)
		{
			Body.push_back(MakeRunBegin(Run));
		}
		return Body;
	}

	size_t BatchPlanner::BufferCount() const
	{
		return State->Buffers.size();
	}

	const PlannedBuffer* BatchPlanner::FindBuffer(std::string_view Uuid) const
	{
		const auto It = State->Buffers.find(std::string(Uuid));
		return It != State->Buffers.end() ? &It->second : nullptr;
	}

	size_t BatchPlanner::PendingRecoveryCount() const
	{
		return State->Pending.size();
	}

	size_t BatchPlanner::OpenRunCount() const
	{
		return State->OpenRuns.size();
	}

	BatchPlan BatchPlanner::Plan(const CommitRequest& Request, bool bSnapOnly) const
	{
		BatchPlan P;
		State->PlanInto(Request, bSnapOnly, P);
		return P;
	}

	BatchPlan BatchPlanner::PlanReplay(bool bSnapOnly) const
	{
		BatchPlan P;
		P.bReplay = true;
		for (const CommitRequest& Request : State->RetryQueue)
		{
			State->PlanInto(Request, bSnapOnly, P);
		}
		return P;
	}

	void BatchPlanner::Committed(BatchPlan&& P)
	{
		Impl& I = *State;
		for (auto& [Uuid, Planned] : P.States)
		{
			if (Planned.has_value())
			{
				I.Buffers.insert_or_assign(Uuid, std::move(*Planned));
			}
			else
			{
				I.Buffers.erase(Uuid);
			}
		}
		for (const std::string& Uuid : P.Resolved)
		{
			I.Pending.erase(std::remove_if(I.Pending.begin(), I.Pending.end(),
				[&Uuid](const std::pair<PendingInfo, std::string>& Entry) { return Entry.first.Uuid == Uuid; }), I.Pending.end());
		}
		if (P.Session.has_value())
		{
			I.Session = std::move(*P.Session);
		}
		for (const RunBeginInfo& Run : P.RunBegins)
		{
			I.OpenRuns[Run.RunId] = Run;
		}
		for (const uint64_t Id : P.RunEnds)
		{
			I.OpenRuns.erase(Id);
		}
		if (P.bReplay)
		{
			I.RetryQueue.clear();
		}
	}

	void BatchPlanner::QueueFailed(CommitRequest&& Request)
	{
		State->RetryQueue.push_back(std::move(Request));
	}

	void BatchPlanner::QueueBehindFailure(CommitRequest&& Request)
	{
		std::vector<CommitRequest>& Queue = State->RetryQueue;
		auto IsPlain = [](const BufferPart& Part)
		{
			return Part.bHasSnapshot && !Part.bOpenEvent && !Part.bMeta && !Part.bClose && !Part.CleanText.has_value();
		};
		CommitRequest* Newest = Queue.empty() ? nullptr : &Queue.back();
		if (Newest == nullptr || Newest->bClean || Request.bClean)
		{
			Queue.push_back(std::move(Request));
			return;
		}

		// Planning re-diffs every part against the durable state, so only the newest text of a buffer needs to wait. A plain
		// text part replaces the newest kept request's plain part of the same buffer, or joins that request after its other
		// records of the buffer; a buffer with an open, META, clean-text or close part in this request keeps its parts here.
		std::vector<std::string> OrderSensitive;
		for (const BufferPart& Part : Request.Buffers)
		{
			if (!IsPlain(Part))
			{
				OrderSensitive.push_back(Part.Uuid);
			}
		}
		auto IsOrderSensitive = [&OrderSensitive](const std::string& Uuid)
		{
			return std::find(OrderSensitive.begin(), OrderSensitive.end(), Uuid) != OrderSensitive.end();
		};
		std::vector<BufferPart> Kept;
		for (BufferPart& Part : Request.Buffers)
		{
			if (!IsPlain(Part) || IsOrderSensitive(Part.Uuid))
			{
				Kept.push_back(std::move(Part));
				continue;
			}
			BufferPart* Into = nullptr;
			for (size_t Index = Newest->Buffers.size(); Index-- > 0 && Into == nullptr;)
			{
				if (Newest->Buffers[Index].Uuid == Part.Uuid)
				{
					Into = &Newest->Buffers[Index];
				}
			}
			if (Into == nullptr || Into->bOpenEvent || Into->bMeta || Into->CleanText.has_value())
			{
				Newest->Buffers.push_back(std::move(Part));
			}
			else if (Into->bClose)
			{
				Kept.push_back(std::move(Part));   // never journaled after its close: as before, in its own request
			}
			else if (Part.Snapshot.Version() > Into->Snapshot.Version())
			{
				*Into = std::move(Part);           // both plain: the newer text and open info win (an older one is covered, PER-03)
			}
		}
		Request.Buffers = std::move(Kept);

		const bool bOnlyText = Request.Buffers.empty() && Request.Extra.empty() && !Request.RunBegin.has_value() && Request.RunEndId == 0;
		if (bOnlyText)
		{
			// Nothing order-sensitive is left: the newest SESSION record wins, so it rides on the newest kept request too.
			if (Request.bHasSession)
			{
				Newest->bHasSession = true;
				Newest->Session = std::move(Request.Session);
			}
			return;
		}
		Queue.push_back(std::move(Request));
	}

	size_t BatchPlanner::RetryQueueLength() const
	{
		return State->RetryQueue.size();
	}

	bool BatchPlanner::HasRetryQueue() const
	{
		return !State->RetryQueue.empty();
	}
}
