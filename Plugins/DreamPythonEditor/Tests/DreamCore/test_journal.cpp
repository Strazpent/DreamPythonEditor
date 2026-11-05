// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::journal (TDD §10.2): codec round trips (T-PER-001), torn tails (T-PER-002), salvage (T-PER-003),
// generation fallback (T-PER-004), a fuzz pass over the reader (T-PER-005 shape), the commit policy (T-PER-006),
// DELTA version checks (T-PER-008) and format versions (T-PER-009), with an in-memory FileOps. Its faults (a torn append,
// a flush that fails after the bytes reached the file, no truncate) drive the commit-failure cases of T-PER-037, and
// several sessions on one directory the generation retention of §10.2.3 and §10.2.4. Then B10-B13: the checkpoint written in
// slices with the catch-up step (T-PER-054 shape), the header check of generation g-1 before its delete, the older-major
// dispatch with a synthetic format 0 and its .v<major> files (T-PER-026 shape), the batch planner's whole-batch replay of a
// failed commit (P2-per-02) and the clean texts of scratch buffers (§10.7.1).
#include "dream_test.h"
#include "test_util.h"

#include "dream/Checksum.h"
#include "dream/JournalFormat.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace dream::journal;
using dream::json::Value;
using dream_test_util::Rng;

namespace
{
	class MemFileOps : public FileOps
	{
	public:
		std::map<std::string, std::string> Files;
		std::map<Handle, std::string> Open;
		Handle Next = 1;
		bool bFailAppend = false;
		int64_t FailAppendAfter = -1;  // >= 0: an append writes that many bytes, then fails (a torn write)
		bool bFailFlush = false;       // a flush fails; the appended bytes stay in the file
		bool bCanTruncate = true;
		std::map<std::string, int64_t> Mtimes;
		int64_t NowUtcMs = 0;          // the clock Touch stamps with
		std::vector<std::pair<std::string, size_t>> AppendLog;   // every successful append: path, bytes
		std::vector<std::string> FlushLog;                       // every successful flush: path
		// review r1 faults: a scanner holding a file without sharing (persist-M2: ReadAll, persist-M4: OpenAppend, a rename).
		std::set<std::string> UnreadablePaths;
		std::set<std::string> UnopenablePaths;
		std::set<std::string> UnrenamablePaths;

		bool CreateExclusive(std::string_view Path, Handle& Out) override
		{
			const std::string P(Path);
			if (Files.count(P) != 0)
			{
				return false;
			}
			Files[P];
			Out = Next++;
			Open[Out] = P;
			return true;
		}
		bool OpenAppend(std::string_view Path, Handle& Out) override
		{
			if (UnopenablePaths.count(std::string(Path)) != 0)
			{
				return false;
			}
			Files[std::string(Path)];
			Out = Next++;
			Open[Out] = std::string(Path);
			return true;
		}
		bool Append(Handle H, std::string_view Bytes) override
		{
			if (bFailAppend || Open.count(H) == 0)
			{
				return false;
			}
			if (FailAppendAfter >= 0)
			{
				Files[Open[H]].append(Bytes.substr(0, std::min(Bytes.size(), static_cast<size_t>(FailAppendAfter))));
				return false;
			}
			Files[Open[H]].append(Bytes);
			AppendLog.emplace_back(Open[H], Bytes.size());
			return true;
		}
		bool Flush(Handle H) override
		{
			if (bFailFlush || Open.count(H) == 0)
			{
				return false;
			}
			FlushLog.push_back(Open[H]);
			return true;
		}
		bool Truncate(Handle H, uint64_t Size) override
		{
			if (!bCanTruncate || Open.count(H) == 0 || Files[Open[H]].size() < Size)
			{
				return false;
			}
			Files[Open[H]].resize(static_cast<size_t>(Size));
			return true;
		}
		bool ModifiedUtcMs(std::string_view Path, int64_t& OutUtcMs) override
		{
			auto It = Mtimes.find(std::string(Path));
			if (It == Mtimes.end())
			{
				return false;
			}
			OutUtcMs = It->second;
			return true;
		}
		bool Touch(std::string_view Path) override
		{
			if (Files.count(std::string(Path)) == 0)
			{
				return false;
			}
			Mtimes[std::string(Path)] = NowUtcMs;
			return true;
		}
		void Close(Handle H) override { Open.erase(H); }
		// review r2: the next N header reads of a path fail; a path in PrefixOnlyPaths has a readable header but no ReadAll.
		std::map<std::string, int> FailPrefixReads;
		std::set<std::string> PrefixOnlyPaths;
		bool ReadPrefix(std::string_view Path, size_t MaxBytes, std::string& Out) override
		{
			const std::string P(Path);
			if (auto It = FailPrefixReads.find(P); It != FailPrefixReads.end() && It->second > 0)
			{
				--It->second;
				return false;
			}
			auto F = Files.find(P);
			if (F == Files.end() || (UnreadablePaths.count(P) != 0 && PrefixOnlyPaths.count(P) == 0))
			{
				return false;
			}
			Out = F->second.substr(0, std::min(MaxBytes, F->second.size()));
			return true;
		}
		bool ReadAll(std::string_view Path, std::string& Out) override
		{
			if (UnreadablePaths.count(std::string(Path)) != 0)
			{
				return false;
			}
			auto It = Files.find(std::string(Path));
			if (It == Files.end())
			{
				return false;
			}
			Out = It->second;
			return true;
		}
		bool RenameNew(std::string_view From, std::string_view To) override
		{
			const std::string F(From);
			const std::string T(To);
			if (Files.count(F) == 0 || Files.count(T) != 0 || UnrenamablePaths.count(F) != 0)
			{
				return false;
			}
			Files[T] = std::move(Files[F]);
			Files.erase(F);
			Mtimes.erase(T);
			if (auto It = Mtimes.find(F); It != Mtimes.end())
			{
				Mtimes[T] = It->second;   // a rename keeps the write time, as MoveFileExW does
				Mtimes.erase(It);
			}
			return true;
		}
		bool ReplaceAtomic(std::string_view Target, std::string_view Temp, std::string_view Backup) override
		{
			Files[std::string(Backup)] = Files[std::string(Target)];
			Files[std::string(Target)] = Files[std::string(Temp)];
			Files.erase(std::string(Temp));
			return true;
		}
		bool Remove(std::string_view Path) override
		{
			Mtimes.erase(std::string(Path));
			return Files.erase(std::string(Path)) != 0;
		}
		bool List(std::string_view Dir, std::vector<std::string>& OutNames) override
		{
			const std::string Prefix = std::string(Dir) + "/";
			for (const auto& [Path, Bytes] : Files)
			{
				if (Path.compare(0, Prefix.size(), Prefix) == 0 && Path.find('/', Prefix.size()) == std::string::npos)
				{
					OutNames.push_back(Path.substr(Prefix.size()));
				}
			}
			return true;
		}
		bool MakeDirs(std::string_view) override { return true; }
		bool FlushDir(std::string_view) override { return true; }
		bool Exists(std::string_view Path) override { return Files.count(std::string(Path)) != 0; }
		std::string LastError() const override { return "mem"; }
	};

	const std::string Dir = "root/journal/main";

	FileHeader Proto()
	{
		FileHeader H;
		H.InstancePid = 4242;
		H.InstanceStart = 777;
		return H;
	}

	BufferOpenInfo Open(const std::string& Uuid)
	{
		BufferOpenInfo O;
		O.Uuid = Uuid;
		O.Kind = "scratch";
		O.Display = "Untitled-1";
		O.Encoding = "utf-8";
		O.Eol = "lf";
		return O;
	}

	/** One DELTA from Old to New: the common prefix and suffix are kept (UTF-8 byte offsets). */
	Record Delta(const std::string& Uuid, uint64_t From, uint64_t To, const std::string& Old, const std::string& New)
	{
		size_t P = 0;
		while (P < Old.size() && P < New.size() && Old[P] == New[P])
		{
			++P;
		}
		size_t S = 0;
		while (S < Old.size() - P && S < New.size() - P && Old[Old.size() - 1 - S] == New[New.size() - 1 - S])
		{
			++S;
		}
		const DeltaOp Op{static_cast<int64_t>(P), static_cast<int64_t>(Old.size() - P - S), static_cast<int64_t>(New.size() - P - S)};
		return MakeDelta(Uuid, From, To, std::span<const DeltaOp>(&Op, 1), std::string_view(New).substr(P, New.size() - P - S), dream::Crc32c(New));
	}

	std::vector<Record> Checkpoint(const std::vector<std::pair<std::string, std::string>>& Buffers, uint64_t Version = 1)
	{
		std::vector<Record> Out;
		for (const auto& [Uuid, Text] : Buffers)
		{
			Out.push_back(MakeBufOpen(Open(Uuid)));
			for (Record& R : MakeSnap(Uuid, Version, Text))
			{
				Out.push_back(std::move(R));
			}
		}
		return Out;
	}

	const RecoveredBuffer* Find(const RecoveredNamespace& N, const std::string& Uuid)
	{
		for (const RecoveredBuffer& B : N.Buffers)
		{
			if (B.Open.Uuid == Uuid)
			{
				return &B;
			}
		}
		return nullptr;
	}

	std::string GenPath(uint64_t G)
	{
		return JoinPath(Dir, GenerationFileName(G));
	}

	/** The wal.<g>.sjl names in Dir, oldest first. */
	std::vector<std::string> GenerationFiles(MemFileOps& Ops)
	{
		std::vector<std::string> Names;
		std::vector<std::string> Out;
		Ops.List(Dir, Names);
		for (const std::string& Name : Names)
		{
			uint64_t G = 0;
			if (ParseGenerationFileName(Name, G))
			{
				Out.push_back(Name);
			}
		}
		return Out;
	}

	/**
	 * One editor session on Dir as DreamJournalWriter runs it at NowUtcMs (the file system clock too): the scan that may
	 * modify (ProcessScan), a generation above every file name, damaged ones too (ProcessStartGeneration), the checkpoint
	 * of the recovered text, one DELTA that appends Suffix to buffer u1.
	 */
	void RunSession(MemFileOps& Ops, int64_t NowUtcMs, const std::string& Suffix)
	{
		Ops.NowUtcMs = NowUtcMs;
		const RecoveredNamespace N = ReadNamespace(Ops, Dir, true);
		uint64_t Highest = N.Generation;
		std::vector<std::string> Names;
		Ops.List(Dir, Names);
		for (const std::string& Name : Names)
		{
			uint64_t G = 0;
			const size_t Sjl = Name.find(".sjl");
			if (Sjl != std::string::npos && ParseGenerationFileName(Name.substr(0, Sjl + 4), G))
			{
				Highest = std::max(Highest, G);
			}
		}
		std::string Text;
		uint64_t Version = 1;
		if (const RecoveredBuffer* B = Find(N, "u1"))
		{
			Text = B->TextUtf8;
			Version = B->Version;
		}
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(Highest + 1, Checkpoint({{"u1", Text}}, Version), NowUtcMs, 0.0));
		REQUIRE(W.Commit(std::vector<Record>{Delta("u1", Version, Version + 1, Text, Text + Suffix)}));
	}

	std::string HeaderBytesFor(uint64_t Generation, uint16_t Major, int64_t CreatedUtcMs)
	{
		FileHeader H = Proto();
		H.Generation = Generation;
		H.Major = Major;
		H.CreatedUtcMs = static_cast<uint64_t>(CreatedUtcMs);
		std::string Out;
		EncodeHeader(H, Out);
		return Out;
	}

	using BufferStates = std::map<std::string, std::pair<uint64_t, std::string>>;   // uuid -> version, text

	/** BUF_OPEN and SNAP for each buffer at its own version. */
	std::vector<Record> CheckpointOf(const BufferStates& States)
	{
		std::vector<Record> Body;
		for (const auto& [Uuid, State] : States)
		{
			Body.push_back(MakeBufOpen(Open(Uuid)));
			for (Record& R : MakeSnap(Uuid, State.first, State.second))
			{
				Body.push_back(std::move(R));
			}
		}
		return Body;
	}

	/**
	 * FDreamJournalWriter's commit loop in miniature (DreamJournalWriter.cpp Process): every call is a new buffer version, its
	 * DELTA starts from the last durable text, which changes only when a commit succeeds, and a successful commit is
	 * followed by the checkpoint rotation whenever the writer asks for one.
	 */
	struct MiniJournal
	{
		Writer& W;
		BufferStates Durable;
		std::map<std::string, uint64_t> Latest;   // the buffer's version, which moves whether or not a commit succeeds

		bool Commit(const std::vector<std::pair<std::string, std::string>>& Edits)
		{
			std::vector<Record> Batch;
			std::vector<uint64_t> To;
			for (const auto& [Uuid, Text] : Edits)
			{
				const auto& [Version, Old] = Durable[Uuid];
				To.push_back(Latest[Uuid] = std::max(Latest[Uuid], Version) + 1);
				Batch.push_back(Delta(Uuid, Version, To.back(), Old, Text));
			}
			if (!W.Commit(Batch))
			{
				return false;
			}
			for (size_t K = 0; K < Edits.size(); ++K)
			{
				Durable[Edits[K].first] = {To[K], Edits[K].second};
			}
			if (W.NeedsRotation(0.0))
			{
				CHECK(W.Rotate(CheckpointOf(Durable), 0, 0.0));
				CHECK(!W.NeedsRotation(0.0));
			}
			return true;
		}
	};
}

TEST(Journal_HeaderAndFrameRoundTrips)
{
	FileHeader H = Proto();
	H.Generation = 9;
	H.CreatedUtcMs = 1730000000000ull;
	std::string Bytes;
	EncodeHeader(H, Bytes);
	REQUIRE_EQ(Bytes.size(), HeaderBytes);
	CHECK(Bytes.substr(0, 8) == "SUMIJRNL");
	FileHeader D;
	REQUIRE(DecodeHeader(Bytes, D));
	CHECK_EQ(D.Generation, 9u);
	CHECK_EQ(D.InstancePid, 4242u);
	CHECK_EQ(D.CreatedUtcMs, 1730000000000ull);
	std::string Bad = Bytes;
	Bad[13] ^= 1;
	CHECK(!DecodeHeader(Bad, D));
	CHECK(!DecodeHeader(Bytes.substr(0, 63), D));

	PendingInfo P;
	P.SourceNamespace = "main";
	P.SourceGeneration = 3;
	P.Uuid = "0123456789abcdef0123456789abcdef";
	P.Kind = "file";
	P.Path = "d:/x/y.py";
	P.Base = BaseVersion{12, 34, std::string(64, 'a')};
	P.Eol = "crlf";
	RunBeginInfo Run;
	Run.RunId = (5ull << 32) | 7;
	Run.Number = 14;
	Run.TabUuid = P.Uuid;
	Run.Route = "line";
	Run.UtcMs = 99;
	const std::pair<std::string, uint64_t> Clean[] = {{"u1", 4}};
	Value Fields = Value::MakeObject();
	Fields.Set("engine", Value::MakeString("5.8.4"));
	Value Session = Value::MakeObject();
	Session.Set("active", Value::MakeString("u1"));
	std::vector<Record> All = {
		MakeHeaderExt(Fields), MakeCheckpointBegin(2), MakeCheckpointEnd(), MakeBufOpen(Open("u1")),
		Delta("u1", 1, 2, "abc", "aXc"), MakeMeta("u1", true, P.Base, "d:/x/y.py", "y.py"), MakeBufClose("u1", "closed"),
		MakeRecoveryPending(P), MakeRecoveryResolved("main", 3, P.Uuid, "restored"), MakeSession(Session),
		MakeRunBegin(Run), MakeRunEnd(Run.RunId, "ok"), MakeClean(5, Clean),
	};
	for (Record& R : MakeSnap("u1", 3, "text \xC3\xA9", &P))
	{
		All.push_back(std::move(R));
	}
	uint64_t Seq = 1;
	for (const Record& R : All)
	{
		std::string Frame;
		EncodeFrame(R, Seq, Seq % 2 == 0, Frame);
		size_t Pos = 0;
		Record Back;
		uint64_t OutSeq = 0;
		bool bCommit = false;
		REQUIRE(DecodeFrame(Frame, Pos, Back, OutSeq, bCommit) == FrameStatus::Ok);
		CHECK_EQ(Pos, Frame.size());
		CHECK_EQ(OutSeq, Seq);
		CHECK_EQ(bCommit, Seq % 2 == 0);
		CHECK(Back.Type == R.Type);
		std::string Again;
		EncodeFrame(Back, Seq, bCommit, Again);
		CHECK(Again == Frame);   // byte-identical re-encode
		Pos = 0;
		CHECK(DecodeFrame(Frame.substr(0, Frame.size() - 1), Pos, Back, OutSeq, bCommit) == FrameStatus::Truncated);
		std::string Flipped = Frame;
		Flipped[Frame.size() / 2] ^= 0x20;
		Pos = 0;
		CHECK(DecodeFrame(Flipped, Pos, Back, OutSeq, bCommit) == FrameStatus::BadCrc);
		++Seq;
	}
	size_t Pos = 0;
	Record Rec;
	uint64_t S = 0;
	bool bC = false;
	CHECK(DecodeFrame("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", Pos, Rec, S, bC) == FrameStatus::BadSync);
	CHECK(DecodeFrame("", Pos, Rec, S, bC) == FrameStatus::End);
	CHECK_EQ(GenerationFileName(7), std::string("wal.0000000000000007.sjl"));
	uint64_t G = 0;
	CHECK(ParseGenerationFileName("wal.00000000000000ff.sjl", G));
	CHECK_EQ(G, 255u);
	CHECK(!ParseGenerationFileName("wal.00000000000000ff.sjl.corrupt", G));
	CHECK(!ParseGenerationFileName("wal.0000000000000g07.sjl", G));
	CHECK(!ParseGenerationFileName("wal.7.sjl", G));
}

TEST(Journal_WriterAndReaderBasics)
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	Value Ext = Value::MakeObject();
	Ext.Set("namespace", Value::MakeString("main"));
	W.SetHeaderExtFields(Ext);
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "hello"}, {"u2", "other"}}), 1000, 0.0));
	CHECK(Ops.Exists(GenPath(1)));
	CHECK(!Ops.Exists(GenPath(1) + ".tmp"));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 1, 2, "hello", "hello world")}));
	Value Session = Value::MakeObject();
	Session.Set("tabs", Value::MakeArray());
	RunBeginInfo Run;
	Run.RunId = 11;
	Run.Number = 3;
	Run.UtcMs = 5000;
	REQUIRE(W.Commit(std::vector<Record>{MakeSession(Session), MakeRunBegin(Run), MakeBufClose("u2", "closed")}));
	REQUIRE(W.Commit(std::vector<Record>{Record{RecordType::Pause, FF_Skippable, [] { Value V = Value::MakeObject(); V.Set("run_id", Value::MakeInt(11)); return V; }(), {}}}));
	CHECK(W.Commit({}));
	CHECK_EQ(W.Generation(), 1u);
	CHECK(W.LastSeq() > 5u);

	RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
	CHECK(N.bFound);
	CHECK(!N.bUnreadable);
	CHECK(!N.bFallbackUsed);
	CHECK(!N.bSalvageUsed);
	CHECK(!N.bClean);
	CHECK_EQ(N.Generation, 1u);
	CHECK_EQ(N.Header.InstancePid, 4242u);
	const RecoveredBuffer* U1 = Find(N, "u1");
	REQUIRE(U1 != nullptr);
	CHECK_EQ(U1->TextUtf8, std::string("hello world"));
	CHECK_EQ(U1->Version, 2u);
	CHECK(U1->bVerified);
	CHECK_EQ(U1->Open.Display, std::string("Untitled-1"));
	const RecoveredBuffer* U2 = Find(N, "u2");
	REQUIRE(U2 != nullptr);
	CHECK(U2->bClosed);
	CHECK_EQ(U2->CloseReason, std::string("closed"));
	CHECK(N.Session.IsObject());
	REQUIRE_EQ(N.OpenRuns.size(), 1u);
	CHECK_EQ(N.OpenRuns[0].Begin.Number, 3u);
	CHECK(N.OpenRuns[0].bPaused);
	CHECK_EQ(N.LastRecordUtcMs, 5000);

	// RUN_END and CLEAN: no open run, a clean last batch.
	const std::pair<std::string, uint64_t> Versions[] = {{"u1", 2}};
	REQUIRE(W.Commit(std::vector<Record>{MakeRunEnd(11, "ok")}));
	REQUIRE(W.Commit(std::vector<Record>{MakeClean(9000, Versions)}));
	N = ReadNamespace(Ops, Dir, false);
	CHECK(N.bClean);
	CHECK(N.OpenRuns.empty());
	REQUIRE_EQ(N.CleanVersions.size(), 1u);
	CHECK_EQ(N.CleanVersions[0].second, 2u);
	CHECK_EQ(N.LastRecordUtcMs, 9000);

	// A failing append is reported, not hidden.
	Ops.bFailAppend = true;
	CHECK(!W.Commit(std::vector<Record>{MakeRunEnd(12, "ok")}));
	CHECK(!W.LastError().empty());
	CHECK(ReadNamespace(Ops, "root/journal/none", false).bFound == false);
}

TEST(Journal_TornTailAtEveryOffset)
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "hello"}}), 0, 0.0));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 1, 2, "hello", "hello world")}));
	const uint64_t LastStart = W.FileBytes();
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 2, 3, "hello world", "hello world!"), MakeMeta("u1", true, std::nullopt, "", "x")}));
	const std::string Full = Ops.Files[GenPath(1)];
	REQUIRE_EQ(static_cast<uint64_t>(Full.size()), W.FileBytes());
	for (size_t Cut = static_cast<size_t>(LastStart); Cut < Full.size(); ++Cut)
	{
		Ops.Files[GenPath(1)] = Full.substr(0, Cut);
		const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
		const RecoveredBuffer* B = Find(N, "u1");
		REQUIRE(B != nullptr);
		CHECK_EQ(B->TextUtf8, std::string("hello world"));   // the state before the torn batch, never an error
		CHECK_EQ(B->Version, 2u);
		CHECK(!B->bDirty);
		CHECK(!N.bUnreadable);
	}
	Ops.Files[GenPath(1)] = Full;
	const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("hello world!"));
	CHECK(Find(N, "u1")->bDirty);
}

TEST(Journal_SalvageAfterMidFileCorruption)
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "a"}, {"u2", "b"}}), 0, 0.0));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 1, 2, "a", "a2")}));
	const uint64_t BadAt = W.FileBytes() + 30;
	REQUIRE(W.Commit(std::vector<Record>{Delta("u2", 1, 2, "b", "b2")}));
	std::vector<Record> Snap = MakeSnap("u1", 3, "u1 newest");
	REQUIRE(W.Commit(Snap));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u2", 2, 3, "b2", "b3")}));
	std::string& File = Ops.Files[GenPath(1)];
	File[static_cast<size_t>(BadAt)] ^= 0x5A;
	const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
	CHECK(N.bSalvageUsed);
	CHECK(!N.bFallbackUsed);
	const RecoveredBuffer* U1 = Find(N, "u1");
	const RecoveredBuffer* U2 = Find(N, "u2");
	REQUIRE(U1 != nullptr);
	REQUIRE(U2 != nullptr);
	CHECK_EQ(U1->TextUtf8, std::string("u1 newest"));   // a later SNAP: newest state
	CHECK_EQ(U1->Version, 3u);
	CHECK_EQ(U2->TextUtf8, std::string("b"));           // its DELTA after the gap does not apply: pre-gap state
	CHECK_EQ(U2->Version, 1u);
}

TEST(Journal_LostRecoveryPendingIsRebuiltFromItsSnap)
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "disk text"}}), 0, 0.0));
	PendingInfo P;
	P.SourceNamespace = "main";
	P.SourceGeneration = 4;
	P.Uuid = "u1";
	P.Kind = "file";
	std::vector<Record> Batch{MakeRecoveryPending(P)};
	for (Record& R : MakeSnap("u1", 9, "recovered text", &P))
	{
		Batch.push_back(std::move(R));
	}
	Batch.push_back(MakeMeta("u1", false, std::nullopt, "", "y.py"));
	const uint64_t PendingAt = W.FileBytes() + 25;
	REQUIRE(W.Commit(Batch));
	{
		const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
		REQUIRE_EQ(N.Pending.size(), 1u);
		CHECK_EQ(N.Pending[0].TextUtf8, std::string("recovered text"));
		CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("disk text"));   // a pending SNAP is never live state
	}
	Ops.Files[GenPath(1)][static_cast<size_t>(PendingAt)] ^= 0x11;
	{
		const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
		CHECK(N.bSalvageUsed);
		REQUIRE_EQ(N.Pending.size(), 1u);
		CHECK_EQ(N.Pending[0].Info.SourceGeneration, 4u);
		CHECK_EQ(N.Pending[0].TextUtf8, std::string("recovered text"));
		CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("disk text"));
	}
	Ops.Files.clear();
	Writer W2(Ops, Dir, Proto());
	REQUIRE(W2.StartGeneration(2, Batch, 0, 0.0));
	REQUIRE(W2.Commit(std::vector<Record>{MakeRecoveryResolved("main", 4, "u1", "restored")}));
	CHECK(ReadNamespace(Ops, Dir, false).Pending.empty());
}

TEST(Journal_GenerationFallbackAndRotation)
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "gen1"}}), 0, 0.0));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 1, 2, "gen1", "gen1 edited")}));
	CHECK(!W.NeedsRotation(1.0));
	CHECK(W.NeedsRotation(RotateSeconds + 1.0));   // 10 minutes after the checkpoint with something committed
	REQUIRE(W.Rotate(Checkpoint({{"u1", "gen2"}}, 3), 0, 700.0));
	CHECK_EQ(W.Generation(), 2u);
	CHECK(!W.NeedsRotation(701.0));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 3, 4, "gen2", "gen2 edited")}));
	CHECK(Ops.Exists(GenPath(1)));
	CHECK(Ops.Exists(GenPath(2)));

	RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("gen2 edited"));
	CHECK_EQ(N.Generation, 2u);

	// An incomplete checkpoint in G: G-1 in full, G renamed .corrupt and kept.
	const std::string Gen2 = Ops.Files[GenPath(2)];
	std::string Header;
	FileHeader H = Proto();
	H.Generation = 2;
	EncodeHeader(H, Header);
	std::string Partial = Header;
	EncodeFrame(MakeHeaderExt(Value::MakeObject()), 1, false, Partial);
	EncodeFrame(MakeCheckpointBegin(1), 2, false, Partial);
	Ops.Files[GenPath(2)] = Partial;
	N = ReadNamespace(Ops, Dir, true);
	CHECK(N.bFallbackUsed);
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("gen1 edited"));
	CHECK(!Ops.Exists(GenPath(2)));
	CHECK(Ops.Exists(GenPath(2) + ".corrupt"));
	CHECK_EQ(N.Generation, 2u);   // the caller's next generation stays above the damaged one

	// A bad header in G: G-1, plus whatever salvage finds in G (here, all of it).
	Ops.Files.erase(GenPath(2) + ".corrupt");
	std::string BadHeader = Gen2;
	BadHeader[3] = 'X';
	Ops.Files[GenPath(2)] = BadHeader;
	N = ReadNamespace(Ops, Dir, false);
	CHECK(N.bFallbackUsed);
	CHECK(N.bSalvageUsed);
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("gen2 edited"));

	// Rotation keeps one previous generation.
	MemFileOps Ops2;
	Writer W2(Ops2, Dir, Proto());
	REQUIRE(W2.StartGeneration(5, Checkpoint({{"u1", "x"}}), 0, 0.0));
	REQUIRE(W2.Rotate(Checkpoint({{"u1", "y"}}), 0, 1.0));
	REQUIRE(W2.Rotate(Checkpoint({{"u1", "z"}}), 0, 2.0));
	CHECK(!Ops2.Exists(GenPath(5)));
	CHECK(Ops2.Exists(GenPath(6)));
	CHECK(Ops2.Exists(GenPath(7)));
	CHECK_EQ(Find(ReadNamespace(Ops2, Dir, false), "u1")->TextUtf8, std::string("z"));

	// Every generation unusable: unreadable, nothing invented.
	MemFileOps Ops3;
	Ops3.Files[GenPath(1)] = "garbage";
	const RecoveredNamespace U = ReadNamespace(Ops3, Dir, true);
	CHECK(U.bFound);
	CHECK(U.bUnreadable);
	CHECK(U.Buffers.empty());
	// Stale .tmp files are removed only when the reader may modify.
	Ops3.Files[GenPath(9) + ".tmp"] = "x";
	ReadNamespace(Ops3, Dir, false);
	CHECK(Ops3.Exists(GenPath(9) + ".tmp"));
	ReadNamespace(Ops3, Dir, true);
	CHECK(!Ops3.Exists(GenPath(9) + ".tmp"));
}

TEST(Journal_DeltaVersionAndCrcChecks)
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "one"}}), 0, 0.0));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 7, 8, "one", "skipped")}));         // from does not match
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("one"));
	Record Wrong = Delta("u1", 1, 2, "one", "two");
	Wrong.Head.Set("text_crc32c", Value::MakeInt(12345));
	REQUIRE(W.Commit(std::vector<Record>{Wrong}));                                       // CRC mismatch: last verified state
	const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("one"));
	CHECK_EQ(Find(N, "u1")->Version, 1u);
	std::vector<Record> Snap = MakeSnap("u1", 5, "five");
	Snap[0].Head.Set("text_crc32c", Value::MakeInt(1));
	REQUIRE(W.Commit(Snap));
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("one"));
	REQUIRE(W.Commit(MakeSnap("u1", 6, "six")));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 6, 7, "six", "seven")}));
	const RecoveredNamespace M = ReadNamespace(Ops, Dir, false);
	CHECK_EQ(Find(M, "u1")->TextUtf8, std::string("seven"));
	CHECK(Find(M, "u1")->bVerified);

	std::string T = "hello world";
	const DeltaOp Ops2[] = {{0, 1, 1}, {6, 5, 2}};
	CHECK(ApplyDelta(T, Ops2, "Jhi"));
	CHECK_EQ(T, std::string("Jello hi"));
	const DeltaOp Unsorted[] = {{6, 1, 0}, {0, 1, 0}};
	CHECK(!ApplyDelta(T, Unsorted, ""));
	const DeltaOp Past[] = {{100, 0, 0}};
	CHECK(!ApplyDelta(T, Past, ""));
	const DeltaOp Short[] = {{0, 0, 3}};
	CHECK(!ApplyDelta(T, Short, "ab"));
	CHECK_EQ(T, std::string("Jello hi"));
}

TEST(Journal_FormatVersionsAndLargeSnaps)
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "v"}}), 0, 0.0));
	Value Head = Value::MakeObject();
	Head.Set("future", Value::MakeBool(true));
	REQUIRE(W.Commit(std::vector<Record>{Record{static_cast<RecordType>(0x50), FF_Skippable, Head, "body"}, Delta("u1", 1, 2, "v", "v2")}));
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("v2"));   // unknown skippable: skipped

	std::string& File = Ops.Files[GenPath(1)];
	FileHeader H;
	REQUIRE(DecodeHeader(File, H));
	H.Major = FormatMajor + 1;
	std::string NewHeader;
	EncodeHeader(H, NewHeader);
	File.replace(0, HeaderBytes, NewHeader);
	const RecoveredNamespace N = ReadNamespace(Ops, Dir, true);
	CHECK(N.bNewerFormat);
	CHECK(N.Buffers.empty());
	CHECK(Ops.Exists(GenPath(1)));   // never renamed or deleted

	// A text over 16 MiB becomes SNAP parts in one batch.
	std::string Big(MaxPayloadBytes + 1000, 'x');
	Big[12345] = 'y';
	const std::vector<Record> Parts = MakeSnap("big", 1, Big);
	CHECK_EQ(Parts.size(), 2u);
	MemFileOps Ops2;
	Writer W2(Ops2, Dir, Proto());
	std::vector<Record> Cp{MakeBufOpen(Open("big"))};
	Cp.insert(Cp.end(), Parts.begin(), Parts.end());
	REQUIRE(W2.StartGeneration(1, Cp, 0, 0.0));
	const RecoveredNamespace BigNs = ReadNamespace(Ops2, Dir, false);
	const RecoveredBuffer* B = Find(BigNs, "big");
	REQUIRE(B != nullptr);
	CHECK(B->TextUtf8 == Big);
}

TEST(Journal_NewerFormatNamespaceIsNeverModified)   // §10.6, PER-11: the newest header decides before any delete
{
	for (const bool bNewerMajor : {false, true})
	{
		MemFileOps Ops;
		{
			Writer W(Ops, Dir, Proto());
			REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "old"}}), 0, 0.0));
		}
		{
			Writer W(Ops, Dir, Proto());
			REQUIRE(W.StartGeneration(2, Checkpoint({{"u1", "new"}}), 0, 0.0));
		}
		std::string& File = Ops.Files[GenPath(2)];
		FileHeader H;
		REQUIRE(DecodeHeader(File, H));
		if (bNewerMajor)
		{
			H.Major = static_cast<uint16_t>(FormatMajor + 1);
		}
		else
		{
			H.Minor = static_cast<uint16_t>(FormatMinor + 1);
		}
		std::string NewHeader;
		EncodeHeader(H, NewHeader);
		File.replace(0, HeaderBytes, NewHeader);
		const std::string Stale = GenPath(3) + ".tmp";
		Ops.Files[Stale] = "unfinished checkpoint";

		const RecoveredNamespace N = ReadNamespace(Ops, Dir, /*bMayModify*/ true);
		CHECK_EQ(N.bNewerFormat, bNewerMajor);
		CHECK_EQ(N.bNewerMinor, !bNewerMajor);
		CHECK(Ops.Exists(Stale));          // no *.tmp deleted
		CHECK(Ops.Exists(GenPath(1)));     // no older generation pruned
		CHECK(Ops.Exists(GenPath(2)));     // nothing renamed .corrupt
		CHECK(!Ops.Exists(GenPath(2) + ".corrupt"));
		if (!bNewerMajor)
		{
			const RecoveredBuffer* B = Find(N, "u1");
			REQUIRE(B != nullptr);
			CHECK_EQ(B->TextUtf8, std::string("new"));   // a newer minor is still read in full
		}

		// A damaged header above it does not hide the newer format (P2-per-04): the next header that decodes decides.
		Ops.Files[GenPath(3)] = "garbage, no header";
		const RecoveredNamespace D = ReadNamespace(Ops, Dir, /*bMayModify*/ true);
		CHECK_EQ(D.bNewerFormat, bNewerMajor);
		CHECK_EQ(D.bNewerMinor, !bNewerMajor);
		CHECK(Ops.Exists(Stale));
		CHECK(Ops.Exists(GenPath(1)));
		CHECK(Ops.Exists(GenPath(3)));     // the damaged one is not renamed .corrupt either
		CHECK(!Ops.Exists(GenPath(3) + ".corrupt"));
		if (!bNewerMajor)
		{
			const RecoveredBuffer* B = Find(D, "u1");
			REQUIRE(B != nullptr);
			CHECK_EQ(B->TextUtf8, std::string("new"));
		}
	}

	// This format's own namespace is still cleaned up, also below a damaged newest header.
	MemFileOps Ops;
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "a"}}), 0, 0.0));
	}
	Ops.Files[GenPath(2) + ".tmp"] = "unfinished checkpoint";
	Ops.Files[GenPath(3)] = "garbage, no header";
	const RecoveredNamespace N = ReadNamespace(Ops, Dir, true);
	CHECK(!N.bNewerMinor);
	CHECK(!N.bNewerFormat);
	CHECK(!Ops.Exists(GenPath(2) + ".tmp"));
	CHECK(Ops.Exists(GenPath(3) + ".corrupt"));
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("a"));

	// A newer minor read below a damaged generation of this format: the damaged one is set aside before the newer
	// minor is found, then the newer minor is read and nothing more is modified.
	MemFileOps Ops2;
	{
		Writer W(Ops2, Dir, Proto());
		REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "old"}}), 0, 0.0));
	}
	{
		Writer W(Ops2, Dir, Proto());
		REQUIRE(W.StartGeneration(2, Checkpoint({{"u1", "newer minor"}}), 0, 0.0));
	}
	{
		Writer W(Ops2, Dir, Proto());
		REQUIRE(W.StartGeneration(3, Checkpoint({{"u1", "this format"}}), 0, 0.0));
	}
	std::string& Minor = Ops2.Files[GenPath(2)];
	FileHeader H;
	REQUIRE(DecodeHeader(Minor, H));
	H.Minor = static_cast<uint16_t>(FormatMinor + 1);
	std::string MinorHeader;
	EncodeHeader(H, MinorHeader);
	Minor.replace(0, HeaderBytes, MinorHeader);
	Ops2.Files[GenPath(3)].resize(HeaderBytes + 10);   // its checkpoint is incomplete
	const RecoveredNamespace M = ReadNamespace(Ops2, Dir, true);
	CHECK(M.bNewerMinor);
	CHECK(M.bFallbackUsed);
	CHECK_EQ(Find(M, "u1")->TextUtf8, std::string("newer minor"));
	CHECK(Ops2.Exists(GenPath(3) + ".corrupt"));   // set aside before the newer minor was found (A62)
	CHECK(Ops2.Exists(GenPath(1)));    // not pruned
}

TEST(Journal_ReaderFuzzNeverCrashes)
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "alpha"}, {"u2", "beta"}}), 0, 0.0));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 1, 2, "alpha", "alphabet")}));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u2", 1, 2, "beta", "betamax"), MakeBufClose("u1", "closed")}));
	const std::string Good = Ops.Files[GenPath(1)];
	Rng R(31337);
	for (int I = 0; I < 3000; ++I)
	{
		std::string Bad = Good;
		const int Mutations = 1 + static_cast<int>(R.Below(6));
		for (int M = 0; M < Mutations; ++M)
		{
			const size_t At = R.Below(Bad.size());
			switch (R.Below(3))
			{
			case 0: Bad[At] = static_cast<char>(R.Below(256)); break;
			case 1: Bad.erase(At, R.Below(8)); break;
			default: Bad.insert(At, std::string(R.Below(8), static_cast<char>(R.Below(256)))); break;
			}
		}
		Ops.Files[GenPath(1)] = Bad;
		const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
		for (const RecoveredBuffer& B : N.Buffers)
		{
			if (B.bHasText)
			{
				CHECK(B.bVerified);   // every accepted state passed its CRC
			}
		}
	}
}

TEST(Journal_CommitPolicyBoundsLatency)
{
	CommitPolicy P;
	CHECK(!P.AnyPending());
	std::vector<double> Pending;   // edit times not yet committed
	double Worst = 0.0;
	double Tick = 0.25;
	for (int Key = 0; Key < 150; ++Key)   // 15 keys/s for 10 s, continuous
	{
		const double Now = Key / 15.0;
		while (Tick <= Now)
		{
			if (P.Due("u1", Tick))
			{
				for (double T : Pending)
				{
					Worst = std::max(Worst, Tick - T);
				}
				Pending.clear();
				P.OnCommitted("u1");
			}
			Tick += 0.25;   // the 4 Hz FDreamJournal ticker
		}
		P.OnEdit("u1", Now);
		Pending.push_back(Now);
	}
	for (; !Pending.empty(); Tick += 0.25)
	{
		if (P.Due("u1", Tick))
		{
			for (double T : Pending)
			{
				Worst = std::max(Worst, Tick - T);
			}
			Pending.clear();
			P.OnCommitted("u1");
		}
	}
	CHECK(Worst <= 1.25 + 1e-9);
	CHECK(!P.AnyPending());

	P.OnViewState("u2", 0.0);
	P.OnViewState("u2", 1.5);
	CHECK(!P.Due("u2", 1.9));
	CHECK(P.Due("u2", 2.0));
	CHECK_EQ(P.DueBuffers(2.0).size(), 1u);
	P.OnEdit("u3", 0.0);
	CHECK(!P.Due("u3", 0.5));
	CHECK(P.Due("u3", 1.0));
	P.Forget("u3");
	CHECK(!P.Due("u3", 5.0));
	CHECK(!P.Due("never", 5.0));
}

TEST(Journal_TornCommitThenRetryKeepsLaterCommits)   // T-PER-037: a write that fails part-way, then the retry
{
	for (const bool bCanTruncate : {true, false})
	{
		MemFileOps Ops;
		Ops.bCanTruncate = bCanTruncate;
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "a"}, {"u2", "x"}}), 0, 0.0));
		const uint64_t SeqBefore = W.LastSeq();
		const uint64_t BytesBefore = W.FileBytes();
		const std::vector<Record> Batch{Delta("u1", 1, 2, "a", "ab"), Delta("u2", 1, 2, "x", "xy")};
		// Torn inside the second frame: the first one is complete in the file, with its seq.
		std::string FirstFrame;
		EncodeFrame(Batch[0], SeqBefore + 1, false, FirstFrame);
		Ops.FailAppendAfter = static_cast<int64_t>(FirstFrame.size() + 9);
		CHECK(!W.Commit(Batch));
		Ops.FailAppendAfter = -1;
		CHECK(!W.LastError().empty());
		CHECK_EQ(W.LastSeq(), SeqBefore + 2);   // spent with the batch
		CHECK_EQ(Ops.Files[GenPath(1)].size() == BytesBefore, bCanTruncate);
		CHECK_EQ(W.NeedsCheckpoint(), !bCanTruncate);
		CHECK_EQ(W.NeedsRotation(0.0), !bCanTruncate);

		// The retry of the same states (the durable versions did not move), then the next edit.
		REQUIRE(W.Commit(Batch));
		RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
		CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("ab"));
		CHECK_EQ(Find(N, "u2")->TextUtf8, std::string("xy"));
		CHECK_EQ(N.bSalvageUsed, !bCanTruncate);   // the torn bytes are still there only when they could not be cut
		REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 2, 3, "ab", "abc")}));
		N = ReadNamespace(Ops, Dir, false);
		CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("abc"));
		CHECK_EQ(Find(N, "u1")->Version, 3u);
		CHECK_EQ(Find(N, "u2")->TextUtf8, std::string("xy"));

		// A new checkpoint replaces the generation that holds the torn bytes.
		if (!bCanTruncate)
		{
			REQUIRE(W.Rotate(CheckpointOf({{"u1", {3, "abc"}}, {"u2", {2, "xy"}}}), 0, 0.0));
			CHECK(!W.NeedsCheckpoint());
			CHECK(!W.NeedsRotation(0.0));
			REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 3, 4, "abc", "abcd")}));
			N = ReadNamespace(Ops, Dir, false);
			CHECK(!N.bSalvageUsed);
			CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("abcd"));
		}
	}
}

TEST(Journal_FailedFlushThenLaterDeltasRecoverTheLatest)   // T-PER-037: the bytes reached the file, the flush failed
{
	for (const bool bCanTruncate : {true, false})
	{
		MemFileOps Ops;
		Ops.bCanTruncate = bCanTruncate;
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "a"}}), 0, 0.0));
		MiniJournal J{W, {{"u1", {1, "a"}}}};
		Ops.bFailFlush = true;
		CHECK(!J.Commit({{"u1", "ab"}}));   // DELTA 1->2 is in the file with its commit frame
		Ops.bFailFlush = false;
		CHECK_EQ(W.NeedsCheckpoint(), !bCanTruncate);
		REQUIRE(J.Commit({{"u1", "abc"}}));    // DELTA 1->3: version 2 never became durable for the writer
		REQUIRE(J.Commit({{"u1", "abcd"}}));   // DELTA 3->4
		const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
		CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("abcd"));
		CHECK_EQ(Find(N, "u1")->Version, 4u);
		CHECK_EQ(W.Generation(), bCanTruncate ? 1u : 2u);   // without truncate, the first success was followed by a checkpoint
		CHECK(!W.NeedsCheckpoint());
	}

	// While a checkpoint is due, a SNAP is right whatever version the reader rebuilt (the caller's rule until Rotate).
	MemFileOps Ops;
	Ops.bCanTruncate = false;
	Writer W(Ops, Dir, Proto());
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "a"}}), 0, 0.0));
	Ops.bFailFlush = true;
	CHECK(!W.Commit(std::vector<Record>{Delta("u1", 1, 2, "a", "ab")}));
	Ops.bFailFlush = false;
	REQUIRE(W.NeedsCheckpoint());
	REQUIRE(W.Commit(MakeSnap("u1", 3, "abc")));
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("abc"));
	CHECK(W.NeedsCheckpoint());   // still: the failed batch is in the file

	// A later failure never cuts at a length that is no longer exact.
	const size_t Size = Ops.Files[GenPath(1)].size();
	Ops.bCanTruncate = true;
	Ops.bFailFlush = true;
	CHECK(!W.Commit(std::vector<Record>{Delta("u1", 3, 4, "abc", "abcd")}));
	Ops.bFailFlush = false;
	CHECK(Ops.Files[GenPath(1)].size() > Size);
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("abcd"));
}

TEST(Journal_SessionsKeepOneClosedGeneration)   // §10.2.3 retention, §10.2.4 .corrupt kept 30 days
{
	const int64_t Day = 24ll * 60 * 60 * 1000;
	const int64_t Now = 1800000000000ll;
	MemFileOps Ops;
	RunSession(Ops, Now, "a");
	RunSession(Ops, Now, "b");
	RunSession(Ops, Now, "c");
	CHECK(GenerationFiles(Ops) == (std::vector<std::string>{GenerationFileName(2), GenerationFileName(3)}));
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("abc"));

	// The scan that may not modify (the cross-root scan) deletes nothing.
	Ops.Files[GenPath(1)] = Ops.Files[GenPath(2)];
	ReadNamespace(Ops, Dir, false);
	CHECK(Ops.Exists(GenPath(1)));
	Ops.Files.erase(GenPath(1));

	// A damaged newest generation: renamed .corrupt, the one before it is the fallback and is kept.
	Ops.Files[GenPath(3)].resize(HeaderBytes + 10);
	RunSession(Ops, Now, "d");
	CHECK(GenerationFiles(Ops) == (std::vector<std::string>{GenerationFileName(2), GenerationFileName(4)}));
	CHECK(Ops.Exists(GenPath(3) + ".corrupt"));
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("abd"));

	// Older files: another major is kept (§10.6), a bad header becomes .corrupt, this format's are deleted.
	Ops.Files[GenPath(1)] = HeaderBytesFor(1, FormatMajor + 1, Now);
	Ops.Files[GenPath(2) + ".bak"] = "unrelated";
	Ops.Files[GenPath(0)] = "garbage, no header";
	RunSession(Ops, Now, "e");
	CHECK(GenerationFiles(Ops) == (std::vector<std::string>{GenerationFileName(1), GenerationFileName(4), GenerationFileName(5)}));
	CHECK(Ops.Exists(GenPath(0) + ".corrupt"));
	CHECK(Ops.Exists(GenPath(2) + ".bak"));
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("abde"));

	// .corrupt files are kept 30 days by their write time, which the scan set when it renamed them (the header's time is
	// never used); for ever when it is unknown or in the future.
	const std::string Gen3Corrupt = GenPath(3) + ".corrupt";   // set aside at Now
	const std::string Gen0Corrupt = GenPath(0) + ".corrupt";   // set aside at Now
	const std::string OldByMtime = GenPath(0x20) + ".corrupt";
	const std::string NewByMtime = GenPath(0x21) + ".corrupt";
	const std::string NoMtime = GenPath(0x22) + ".corrupt";
	const std::string FutureMtime = GenPath(0x23) + ".corrupt";
	for (const std::string& SetAside : {Gen3Corrupt, Gen0Corrupt})
	{
		int64_t SetAsideUtcMs = 0;
		CHECK(Ops.ModifiedUtcMs(SetAside, SetAsideUtcMs));
		CHECK_EQ(SetAsideUtcMs, Now);
	}
	Ops.Files[OldByMtime] = "x";
	Ops.Mtimes[OldByMtime] = Now - 40 * Day;
	Ops.Files[NewByMtime] = HeaderBytesFor(0x21, FormatMajor, Now - 40 * Day);
	Ops.Mtimes[NewByMtime] = Now + 28 * Day;   // set aside lately: its older header time does not count
	Ops.Files[NoMtime] = HeaderBytesFor(0x22, FormatMajor, Now - 40 * Day);
	Ops.Files[FutureMtime] = "x";
	Ops.Mtimes[FutureMtime] = Now + 90 * Day;
	RunSession(Ops, Now + 29 * Day, "f");
	CHECK(Ops.Exists(Gen3Corrupt));
	CHECK(Ops.Exists(Gen0Corrupt));
	CHECK(!Ops.Exists(OldByMtime));
	CHECK(Ops.Exists(NewByMtime));
	RunSession(Ops, Now + 31 * Day, "g");
	CHECK(!Ops.Exists(Gen3Corrupt));
	CHECK(!Ops.Exists(Gen0Corrupt));
	CHECK(Ops.Exists(NewByMtime));
	CHECK(Ops.Exists(NoMtime));
	CHECK(Ops.Exists(FutureMtime));
	CHECK_EQ(GenerationFiles(Ops).size(), 3u);   // generation 1 (another major), the closed one and the session's
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("abdefg"));
}

TEST(Journal_CorruptKeptThirtyDaysFromSetAside)   // §10.2.4, §10.11 (P2-per-01): a rename keeps the old write time
{
	const int64_t Day = 24ll * 60 * 60 * 1000;
	const int64_t Now = 1800000000000ll;
	MemFileOps Ops;
	RunSession(Ops, Now - 41 * Day, "a");
	RunSession(Ops, Now - 40 * Day, "b");
	REQUIRE(GenerationFiles(Ops) == (std::vector<std::string>{GenerationFileName(1), GenerationFileName(2)}));
	Ops.Mtimes[GenPath(1)] = Now - 41 * Day;   // last written by those sessions
	Ops.Mtimes[GenPath(2)] = Now - 40 * Day;

	// Both damaged since (a bad header, a bad CRC in the checkpoint): nothing is usable, and the user is told the files
	// were kept. The session start that set them aside must not delete them by their old write times.
	Ops.Files[GenPath(2)][3] = 'X';
	Ops.Files[GenPath(1)][HeaderBytes + 30] ^= 0x01;
	CHECK(ReadNamespace(Ops, Dir, false).bUnreadable);
	RunSession(Ops, Now, "c");
	for (const uint64_t G : {1u, 2u})
	{
		int64_t SetAsideUtcMs = 0;
		CHECK(Ops.ModifiedUtcMs(GenPath(G) + ".corrupt", SetAsideUtcMs));
		CHECK_EQ(SetAsideUtcMs, Now);
	}
	RunSession(Ops, Now + 29 * Day, "d");
	CHECK(Ops.Exists(GenPath(1) + ".corrupt"));
	CHECK(Ops.Exists(GenPath(2) + ".corrupt"));
	RunSession(Ops, Now + 30 * Day, "e");   // 30 days after they were set aside
	CHECK(!Ops.Exists(GenPath(1) + ".corrupt"));
	CHECK(!Ops.Exists(GenPath(2) + ".corrupt"));
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, std::string("cde"));
}

// ---- B10-B13: sliced checkpoints, the g-1 header check, the older-major dispatch, the batch planner, clean texts -----

namespace
{
	dream::TextSnapshot Snap(const std::u16string& Text)
	{
		return dream::TextBuffer(Text).Snapshot();
	}

	/** A buffer part as FDreamJournal builds one: the snapshot and its open info. */
	BufferPart Part(const std::string& Uuid, const dream::TextSnapshot& S)
	{
		BufferPart P;
		P.Uuid = Uuid;
		P.Snapshot = S;
		P.bHasSnapshot = true;
		P.Open = Open(Uuid);
		return P;
	}

	/** Bytes the writer appended to Path, append by append. */
	std::vector<size_t> AppendsTo(const MemFileOps& Ops, const std::string& Path)
	{
		std::vector<size_t> Out;
		for (const auto& [P, Bytes] : Ops.AppendLog)
		{
			if (P == Path)
			{
				Out.push_back(Bytes);
			}
		}
		return Out;
	}

	size_t CountFrames(const std::string& Bytes, RecordType Type)
	{
		size_t Count = 0;
		size_t Pos = HeaderBytes;
		Record R;
		uint64_t Seq = 0;
		bool bCommit = false;
		while (DecodeFrame(Bytes, Pos, R, Seq, bCommit) == FrameStatus::Ok)
		{
			Count += R.Type == Type ? 1 : 0;
		}
		return Count;
	}

	// ---- A synthetic older major (§10.6): "format 0" wrote a buffer's text inside the head of its own record 0x61 ------

	constexpr uint16_t V0SnapType = 0x61;

	/** Writes a format-0 generation: header Major 0, a checkpoint with each buffer's BUF_OPEN and a 0x61 {uuid, version,
	 *  text}, then one batch that replaces u1's text. */
	std::string WriteFormat0Generation(uint64_t Generation, const std::string& CheckpointText, const std::string& LaterText)
	{
		FileHeader H = Proto();
		H.Major = 0;
		H.Minor = 3;
		H.Generation = Generation;
		std::string Out;
		EncodeHeader(H, Out);
		auto V0Snap = [](const std::string& Uuid, uint64_t Version, const std::string& Text)
		{
			Record R;
			R.Type = static_cast<RecordType>(V0SnapType);
			R.Head = Value::MakeObject();
			R.Head.Set("uuid", Value::MakeString(Uuid));
			R.Head.Set("version", Value::MakeUInt(Version));
			R.Head.Set("text", Value::MakeString(Text));
			return R;
		};
		uint64_t S = 0;
		EncodeFrame(MakeCheckpointBegin(1), ++S, false, Out);
		EncodeFrame(MakeBufOpen(Open("u1")), ++S, false, Out);
		EncodeFrame(V0Snap("u1", 4, CheckpointText), ++S, false, Out);
		EncodeFrame(MakeCheckpointEnd(), ++S, true, Out);
		EncodeFrame(V0Snap("u1", 5, LaterText), ++S, true, Out);
		return Out;
	}

	/** The format-0 reader: its 0x61 records become this format's SNAPs. */
	bool UpgradeFormat0(std::string_view Bytes, UpgradedGeneration& Out)
	{
		if (!DecodeHeader(Bytes, Out.Header) || Out.Header.Major != 0)
		{
			return false;
		}
		size_t Pos = HeaderBytes;
		std::vector<Record> Batch;
		bool bFirst = true;
		for (;;)
		{
			Record R;
			uint64_t Seq = 0;
			bool bCommit = false;
			if (DecodeFrame(Bytes, Pos, R, Seq, bCommit) != FrameStatus::Ok)
			{
				break;   // the end, or a torn tail: the open batch is dropped
			}
			if (static_cast<uint16_t>(R.Type) == V0SnapType)
			{
				for (Record& S : MakeSnap(R.Head.Get("uuid").AsString(), R.Head.Get("version").AsUInt(), R.Head.Get("text").AsString()))
				{
					Batch.push_back(std::move(S));
				}
			}
			else
			{
				Batch.push_back(std::move(R));
			}
			if (!bCommit)
			{
				continue;
			}
			if (bFirst)
			{
				Out.bCheckpointComplete = std::any_of(Batch.begin(), Batch.end(), [](const Record& Rec) { return Rec.Type == RecordType::CheckpointEnd; });
				bFirst = false;
			}
			Out.Batches.push_back(std::move(Batch));
			Batch.clear();
		}
		return true;
	}

	constexpr OlderMajorReader Format0Readers[] = { OlderMajorReader{ 0, &UpgradeFormat0 } };
}

TEST(Journal_RotationInSlicesWithCatchUp)   // B10: §10.2.1, §10.2.3 steps 1-2, T-PER-054 shape
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	// Three buffers whose checkpoint is about 1.5 MiB; the slices below are 256 KiB, so the checkpoint takes several.
	const std::string Big(512 * 1024, 'x');
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", Big + "1"}, {"u2", Big + "2"}, {"u3", Big + "3"}}), 0, 0.0));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 1, 2, Big + "1", Big + "1a")}));
	const std::string Tmp = GenPath(2) + ".tmp";

	REQUIRE(W.BeginRotation(Checkpoint({{"u1", Big + "1a"}, {"u2", Big + "2"}, {"u3", Big + "3"}}, 2), 0));
	CHECK(W.IsRotating());
	CHECK(!W.NeedsRotation(1e9));   // one rotation at a time
	CHECK(Ops.Exists(Tmp));
	const size_t Slice = 256 * 1024;
	std::string U1 = Big + "1a";
	uint64_t U1Version = 2;
	int Units = 0;
	int Writes = 0;
	RotationStep Step = RotationStep::More;
	while (Step == RotationStep::More && Units < 200)
	{
		const size_t AppendsBefore = AppendsTo(Ops, Tmp).size();
		const size_t FlushesBefore = Ops.FlushLog.size();
		Step = W.StepRotation(Slice);
		++Units;
		const size_t AppendsAfter = AppendsTo(Ops, Tmp).size();
		// Each unit is a write of at most one slice or the flush of the write before it, never both (§10.2.3 step 1).
		CHECK((AppendsAfter - AppendsBefore) + (Ops.FlushLog.size() - FlushesBefore) <= 1);
		if (AppendsAfter > AppendsBefore)
		{
			++Writes;
			CHECK(AppendsTo(Ops, Tmp).back() <= Slice);
		}
		// Commits go on between the units, to generation 1 (the .tmp is not appended to by them).
		const std::string Next = U1 + "b";
		REQUIRE(W.Commit(std::vector<Record>{Delta("u1", U1Version, U1Version + 1, U1, Next)}));
		U1 = Next;
		++U1Version;
	}
	REQUIRE(Step == RotationStep::Ready);
	CHECK(Writes >= 6);
	CHECK_EQ(W.RotationCatchUpBatches(), static_cast<size_t>(Units));
	CHECK_EQ(W.Generation(), 1u);
	// A commit that fails while the checkpoint is written never joins the catch-up (the caller treats it as not written).
	Ops.bFailAppend = true;
	CHECK(!W.Commit(std::vector<Record>{Delta("u2", 1, 2, Big + "2", Big + "2-lost")}));
	Ops.bFailAppend = false;
	CHECK_EQ(W.RotationCatchUpBatches(), static_cast<size_t>(Units));

	// Step 2: the catch-up joins the .tmp with one flush; step 3: the rename; step 4: appends go to generation 2.
	const size_t FlushesBeforeFinish = Ops.FlushLog.size();
	REQUIRE(W.FinishRotation(0.0));
	CHECK_EQ(Ops.FlushLog.size() - FlushesBeforeFinish, 1u);
	CHECK(!W.IsRotating());
	CHECK_EQ(W.Generation(), 2u);
	CHECK(!Ops.Exists(Tmp));
	CHECK(Ops.Exists(GenPath(1)));    // the kept previous generation
	CHECK(!W.NeedsCheckpoint());      // the failed batch's bytes may be in generation 1, never in 2
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", U1Version, U1Version + 1, U1, U1 + "z")}));

	// The new generation alone recovers the newest text: the checkpoint, then every batch committed while it was written.
	Ops.Files.erase(GenPath(1));
	const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
	REQUIRE(!N.bFallbackUsed && !N.bSalvageUsed && !N.bUnreadable);
	CHECK_EQ(N.Generation, 2u);
	CHECK_EQ(Find(N, "u1")->TextUtf8, U1 + "z");
	CHECK_EQ(Find(N, "u2")->TextUtf8, Big + "2");
	CHECK_EQ(Find(N, "u3")->TextUtf8, Big + "3");

	// A rotation that fails part-way is abandoned: its .tmp goes, appends stay on the current generation.
	REQUIRE(W.BeginRotation(Checkpoint({{"u1", U1 + "z"}}, U1Version + 1), 0));
	Ops.bFailAppend = true;
	CHECK(W.StepRotation(Slice) == RotationStep::Failed);
	Ops.bFailAppend = false;
	CHECK(!W.IsRotating());
	CHECK(!Ops.Exists(GenPath(3) + ".tmp"));
	CHECK_EQ(W.Generation(), 2u);
	CHECK(W.Commit(std::vector<Record>{Delta("u1", U1Version + 1, U1Version + 2, U1 + "z", U1 + "zz")}));
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, U1 + "zz");

	// Rotate is the same protocol in one call.
	CHECK(W.Rotate(Checkpoint({{"u1", U1 + "zz"}}, U1Version + 2), 0, 0.0));
	CHECK_EQ(W.Generation(), 3u);
	CHECK_EQ(Find(ReadNamespace(Ops, Dir, false), "u1")->TextUtf8, U1 + "zz");
}

TEST(Journal_RotateLooksAtPreviousHeaderBeforeDeleting)   // B12: §10.2.3 step 5, §10.6
{
	const int64_t Now = 1800000000000ll;
	struct Case
	{
		const char* Name;
		std::string PreviousBytes;
		bool bDeleted;
		std::string KeptAs;
	};
	std::vector<Case> Cases;
	Cases.push_back({ "this format", HeaderBytesFor(1, FormatMajor, Now), true, "" });
	Cases.push_back({ "an older major", HeaderBytesFor(1, 0, Now), false, GenPath(1) + ".v0" });
	Cases.push_back({ "a newer major", HeaderBytesFor(1, FormatMajor + 1, Now), false, GenPath(1) });
	Cases.push_back({ "a bad header", std::string("no header here"), false, GenPath(1) + ".corrupt" });
	for (const Case& C : Cases)
	{
		MemFileOps Ops;
		Ops.NowUtcMs = Now;
		Ops.Files[GenPath(1)] = C.PreviousBytes;
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(2, Checkpoint({{"u1", "a"}}), Now, 0.0));
		REQUIRE(W.Rotate(Checkpoint({{"u1", "a"}}), Now, 0.0));
		CHECK_EQ(W.Generation(), 3u);
		CHECK(Ops.Exists(GenPath(2)));
		if (C.bDeleted)
		{
			CHECK(!Ops.Exists(GenPath(1)));
		}
		else
		{
			CHECK(Ops.Exists(C.KeptAs));
			CHECK_EQ(Ops.Files[C.KeptAs], C.PreviousBytes);
			if (C.KeptAs != GenPath(1))
			{
				int64_t SetAside = 0;
				CHECK(Ops.ModifiedUtcMs(C.KeptAs, SetAside));   // touched before the rename: its 30 days count from now
				CHECK_EQ(SetAside, Now);
			}
		}
	}
}

TEST(Journal_OlderMajorIsReadAndMigrated)   // B11: §10.6, T-PER-026 shape, with a synthetic format 0
{
	const int64_t Day = 24ll * 60 * 60 * 1000;
	const int64_t Now = 1800000000000ll;
	MemFileOps Ops;
	Ops.NowUtcMs = Now;
	Ops.Files[GenPath(5)] = WriteFormat0Generation(5, "old checkpoint", "old later");
	Ops.Files[GenPath(4)] = WriteFormat0Generation(4, "older", "older still");

	// A build that knows no older major skips it and changes nothing.
	const RecoveredNamespace Unknown = ReadNamespace(Ops, Dir, true);
	CHECK(Unknown.bUnreadable);
	CHECK(Ops.Exists(GenPath(5)) && Ops.Exists(GenPath(4)));
	CHECK(!Ops.Exists(GenPath(5) + ".corrupt"));

	// With the format-0 reader the namespace recovers normally: the checkpoint and the later batch.
	const RecoveredNamespace N = ReadNamespace(Ops, Dir, true, Format0Readers);
	REQUIRE(!N.bUnreadable && !N.bNewerFormat && !N.bNewerMinor);
	CHECK_EQ(N.SourceMajor, 0u);
	CHECK_EQ(N.Generation, 5u);
	REQUIRE(Find(N, "u1") != nullptr);
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("old later"));
	CHECK_EQ(Find(N, "u1")->Version, 5u);
	// Older than the generation read and another major: kept for a downgrade as .v0, touched first.
	CHECK(!Ops.Exists(GenPath(4)));
	CHECK(Ops.Exists(GenPath(4) + ".v0"));
	CHECK(Ops.Exists(GenPath(5)));    // the generation read stays as the fallback

	// The session's first generation writes the new format; the next scan reads it and sets generation 5 aside as .v0.
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(6, Checkpoint({{"u1", Find(N, "u1")->TextUtf8}}, 5), Now, 0.0));
		REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 5, 6, "old later", "old later, new")}));
	}
	FileHeader H6;
	CHECK(DecodeHeader(Ops.Files[GenPath(6)], H6) && H6.Major == FormatMajor);
	const RecoveredNamespace After = ReadNamespace(Ops, Dir, true, Format0Readers);
	CHECK_EQ(After.SourceMajor, FormatMajor);
	CHECK_EQ(Find(After, "u1")->TextUtf8, std::string("old later, new"));
	CHECK(!Ops.Exists(GenPath(5)));
	CHECK(Ops.Exists(GenPath(5) + ".v0"));
	uint64_t G = 0;
	uint16_t Major = 99;
	CHECK(ParseMigratedFileName(JournalLogFormat, GenerationFileName(5) + ".v0", G, Major) && G == 5 && Major == 0);
	CHECK(!ParseMigratedFileName(JournalLogFormat, GenerationFileName(5) + ".corrupt", G, Major));

	// The .v<major> files are kept 30 days from their set-aside, then the next session start deletes them.
	Ops.NowUtcMs = Now + 29 * Day;
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(7, Checkpoint({{"u1", "x"}}), Now + 29 * Day, 0.0));
	}
	CHECK(Ops.Exists(GenPath(5) + ".v0") && Ops.Exists(GenPath(4) + ".v0"));
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(8, Checkpoint({{"u1", "x"}}), Now + 30 * Day, 0.0));
	}
	CHECK(!Ops.Exists(GenPath(5) + ".v0") && !Ops.Exists(GenPath(4) + ".v0"));
}

TEST(Journal_BatchPlannerReplaysFailedBatchWhole)   // B13: P2-per-02, §10.2.5
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	BatchPlanner P;
	// The generation's start: buffer u1, an open run 7 and a pending recovery of u9 (§10.7.1).
	P.SeedBuffer(Part("u1", Snap(u"one")));
	PendingInfo Pending;
	Pending.SourceNamespace = "main";
	Pending.SourceGeneration = 3;
	Pending.Uuid = "u9";
	Pending.Kind = "file";
	Pending.Path = "c:/p/x.py";
	P.SetPendingRecoveries({ { Pending, std::string("recovered x") } });
	{
		CommitRequest Begin;
		Begin.Ticket = 1;
		RunBeginInfo Run;
		Run.RunId = 7;
		Run.Number = 1;
		Run.Route = "buffer";
		Begin.Extra.push_back(MakeRunBegin(Run));
		Begin.RunBegin = Run;
		REQUIRE(W.StartGeneration(1, P.BuildCheckpoint(), 0, 0.0));
		BatchPlan Plan = P.Plan(Begin, W.NeedsCheckpoint());
		REQUIRE(W.Commit(Plan.Records));
		P.Committed(std::move(Plan));
	}
	CHECK_EQ(P.OpenRunCount(), 1u);
	CHECK_EQ(P.PendingRecoveryCount(), 1u);

	// The disk fails. The request carries a new buffer's BUF_OPEN, RUN_END of run 7 and RECOVERY_RESOLVED of u9.
	dream::TextBuffer U1(u"one");
	dream::TextBuffer U2(u"two");
	CommitRequest Failing;
	Failing.Ticket = 2;
	BufferPart Opened = Part("u2", U2.Snapshot());
	Opened.bOpenEvent = true;
	Failing.Buffers.push_back(Opened);
	Failing.Extra.push_back(MakeRunEnd(7, "ok"));
	Failing.RunEndId = 7;
	Failing.Extra.push_back(MakeRecoveryResolved("main", 3, "u9", "restored"));
	Ops.bFailAppend = true;
	{
		BatchPlan Plan = P.Plan(Failing, W.NeedsCheckpoint());
		CHECK(!W.Commit(Plan.Records));
		P.QueueFailed(std::move(Failing));
	}
	CHECK_EQ(P.RetryQueueLength(), 1u);
	CHECK_EQ(P.OpenRunCount(), 1u);                  // nothing of a failed plan applies
	CHECK_EQ(P.PendingRecoveryCount(), 1u);
	CHECK(P.FindBuffer("u2") == nullptr);

	// During the outage: typing in u1 and u2 (plain text: folds into the waiting request), then a run's RUN_BEGIN (waits).
	for (int K = 0; K < 5; ++K)
	{
		U1.SetText(U1.Snapshot().Text() + u"+", dream::EditOrigin::Typing);
		U2.SetText(U2.Snapshot().Text() + u"*", dream::EditOrigin::Typing);
		CommitRequest Typing;
		Typing.Ticket = 10 + static_cast<uint64_t>(K);
		Typing.Buffers.push_back(Part("u1", U1.Snapshot()));
		Typing.Buffers.push_back(Part("u2", U2.Snapshot()));
		P.QueueBehindFailure(std::move(Typing));
	}
	CHECK_EQ(P.RetryQueueLength(), 1u);
	CommitRequest NextRun;
	NextRun.Ticket = 20;
	RunBeginInfo Run8;
	Run8.RunId = 8;
	Run8.Number = 2;
	Run8.Route = "line";
	NextRun.Extra.push_back(MakeRunBegin(Run8));
	NextRun.RunBegin = Run8;
	NextRun.Buffers.push_back(Part("u1", U1.Snapshot()));
	P.QueueBehindFailure(std::move(NextRun));
	CHECK_EQ(P.RetryQueueLength(), 2u);

	// A replay while the disk still fails changes nothing and keeps the queue whole.
	{
		BatchPlan Replay = P.PlanReplay(W.NeedsCheckpoint());
		CHECK(!W.Commit(Replay.Records));
	}
	CHECK_EQ(P.RetryQueueLength(), 2u);

	// The disk recovers: the whole queue is one batch, one append and one flush, with every order-sensitive record in order.
	Ops.bFailAppend = false;
	const size_t AppendsBefore = Ops.AppendLog.size();
	const size_t FlushesBefore = Ops.FlushLog.size();
	BatchPlan Replay = P.PlanReplay(W.NeedsCheckpoint());
	CHECK((Replay.Tickets == std::vector<uint64_t>{2, 20}));
	std::vector<RecordType> Order;
	for (const Record& R : Replay.Records)
	{
		if (R.Type == RecordType::BufOpen || R.Type == RecordType::RunEnd || R.Type == RecordType::RecoveryResolved || R.Type == RecordType::RunBegin)
		{
			Order.push_back(R.Type);
		}
	}
	CHECK((Order == std::vector<RecordType>{RecordType::BufOpen, RecordType::RunEnd, RecordType::RecoveryResolved, RecordType::RunBegin}));
	REQUIRE(W.Commit(Replay.Records));
	CHECK_EQ(Ops.AppendLog.size() - AppendsBefore, 1u);
	CHECK_EQ(Ops.FlushLog.size() - FlushesBefore, 1u);
	P.Committed(std::move(Replay));
	CHECK(!P.HasRetryQueue());
	CHECK_EQ(P.PendingRecoveryCount(), 0u);
	CHECK_EQ(P.OpenRunCount(), 1u);                  // run 8 only
	REQUIRE(P.FindBuffer("u2") != nullptr);
	CHECK(P.FindBuffer("u2")->Committed.Text() == U2.Snapshot().Text());

	// What the reader sees after the replay, and after a checkpoint of the planner's state.
	auto CheckRecovered = [&](const RecoveredNamespace& N)
	{
		REQUIRE(Find(N, "u1") != nullptr && Find(N, "u2") != nullptr);
		CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("one+++++"));
		CHECK_EQ(Find(N, "u2")->TextUtf8, std::string("two*****"));
		CHECK(N.Pending.empty());
		REQUIRE_EQ(N.OpenRuns.size(), 1u);
		CHECK_EQ(N.OpenRuns[0].Begin.RunId, 8u);
	};
	CheckRecovered(ReadNamespace(Ops, Dir, false));
	CHECK(W.Rotate(P.BuildCheckpoint(), 0, 0.0));
	Ops.Files.erase(GenPath(1));
	CheckRecovered(ReadNamespace(Ops, Dir, false));

	// A request whose own records are older than the durable state is covered and adds no text record.
	CommitRequest Stale;
	Stale.Ticket = 30;
	Stale.Buffers.push_back(Part("u1", Snap(u"one")));
	const BatchPlan StalePlan = P.Plan(Stale, false);
	CHECK(StalePlan.Records.empty());
}

TEST(Journal_CleanTextOfScratchBuffers)   // §10.7.1 (B16): a counted scratch buffer shows its clean text
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	BatchPlanner P;
	dream::TextBuffer Scratch(u"restored at start");
	BufferPart Seed = Part("u1", Scratch.Snapshot());
	Seed.CleanText = Scratch.Snapshot();
	P.SeedBuffer(Seed);
	P.SeedBuffer(Part("u2", Snap(u"created later")));   // no clean text: created in this session
	REQUIRE(W.StartGeneration(1, P.BuildCheckpoint(), 0, 0.0));
	CHECK_EQ(CountFrames(Ops.Files[GenPath(1)], RecordType::CleanText), 1u);

	Scratch.SetText(u"restored at start, then typed", dream::EditOrigin::Typing);
	CommitRequest Typing;
	Typing.Buffers.push_back(Part("u1", Scratch.Snapshot()));
	BatchPlan Plan = P.Plan(Typing, false);
	REQUIRE(W.Commit(Plan.Records));
	P.Committed(std::move(Plan));

	auto CheckClean = [](const RecoveredNamespace& N)
	{
		REQUIRE(Find(N, "u1") != nullptr && Find(N, "u2") != nullptr);
		CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("restored at start, then typed"));
		REQUIRE(Find(N, "u1")->CleanTextUtf8.has_value());
		CHECK_EQ(*Find(N, "u1")->CleanTextUtf8, std::string("restored at start"));
		CHECK(!Find(N, "u2")->CleanTextUtf8.has_value());
	};
	CheckClean(ReadNamespace(Ops, Dir, false));
	// Every checkpoint carries it, now with its own body (the live text moved on).
	REQUIRE(W.Rotate(P.BuildCheckpoint(), 0, 0.0));
	Ops.Files.erase(GenPath(1));
	CheckClean(ReadNamespace(Ops, Dir, false));

	// A clean text set later (Leave safe mode restores the buffer) is written with that commit.
	CommitRequest Later;
	BufferPart WithClean = Part("u2", Snap(u"created later"));
	WithClean.CleanText = Snap(u"its clean text");
	Later.Buffers.push_back(WithClean);
	Plan = P.Plan(Later, false);
	REQUIRE(W.Commit(Plan.Records));
	P.Committed(std::move(Plan));
	const RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
	REQUIRE(Find(N, "u2") != nullptr && Find(N, "u2")->CleanTextUtf8.has_value());
	CHECK_EQ(*Find(N, "u2")->CleanTextUtf8, std::string("its clean text"));

	// A CLEAN_TEXT whose CRC does not match is ignored (a reader keeps the last verified clean text).
	Record Bad = MakeCleanText("u1", "tampered", false);
	Bad.Body = "tamperex";
	REQUIRE(W.Commit(std::vector<Record>{Bad}));
	CHECK_EQ(*Find(ReadNamespace(Ops, Dir, false), "u1")->CleanTextUtf8, std::string("restored at start"));
}

TEST(Journal_UnreadableGenerationCountsAsDamagedAndIsNeverDeleted)   // review r1 persist-M2, §10.2.4, §10.11
{
	const int64_t Now = 1800000000000ll;
	MemFileOps Ops;
	Ops.NowUtcMs = Now;
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "one"}}), Now, 0.0));
	}
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(2, Checkpoint({{"u1", "two"}}), Now, 0.0));
		REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 1, 2, "two", "two, the last session")}));
	}
	const std::string Gen2 = Ops.Files[GenPath(2)];

	// A scanner holds generation 2 (no read, no rename) through the FileOps retries: it counts as damaged, the previous
	// generation is used and says so, and generation 2 stays where it is.
	Ops.UnreadablePaths = { GenPath(2) };
	Ops.UnrenamablePaths = { GenPath(2) };
	RecoveredNamespace N = ReadNamespace(Ops, Dir, true);
	REQUIRE(!N.bUnreadable);
	CHECK(N.bFallbackUsed);
	CHECK_EQ(N.UsedGeneration, 1u);
	CHECK_EQ(N.Generation, 2u);
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("one"));
	CHECK(Ops.Exists(GenPath(2)));
	// Still in place: no session generation may start above it (the next read would delete it under that generation).
	CHECK(!SetAsideUnusedNewer(Ops, Dir, JournalLogFormat, N.UsedGeneration));
	CHECK(Ops.Exists(GenPath(2)));

	// The scanner lets go: generation 2 is set aside, kept 30 days from now, and a session generation 3 starts.
	Ops.UnreadablePaths.clear();
	Ops.UnrenamablePaths.clear();
	Ops.Files[GenPath(9)] = HeaderBytesFor(9, 0, Now);   // an older major above: left for the §10.6 migration
	CHECK(SetAsideUnusedNewer(Ops, Dir, JournalLogFormat, N.UsedGeneration));
	CHECK(!Ops.Exists(GenPath(2)));
	REQUIRE(Ops.Exists(GenPath(2) + ".corrupt"));
	CHECK_EQ(Ops.Files[GenPath(2) + ".corrupt"], Gen2);
	CHECK_EQ(Ops.Mtimes[GenPath(2) + ".corrupt"], Now);
	CHECK(Ops.Exists(GenPath(9)));
	Ops.Files.erase(GenPath(9));
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(3, Checkpoint({{"u1", "one"}}, 1), Now, 0.0));
	}
	const RecoveredNamespace After = ReadNamespace(Ops, Dir, true);
	CHECK_EQ(After.UsedGeneration, 3u);
	CHECK(!After.bFallbackUsed);
	CHECK(!Ops.Exists(GenPath(1)));
	CHECK(Ops.Exists(GenPath(2) + ".corrupt"));   // the reader never deletes it

	// The reader never renames a generation it cannot read (its header is unknown, review r3); the set-aside before the
	// session's generation does, once it knows the header is this format or cannot be read at all.
	MemFileOps Ops2;
	{
		Writer W(Ops2, Dir, Proto());
		REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "a"}}), Now, 0.0));
	}
	{
		Writer W(Ops2, Dir, Proto());
		REQUIRE(W.StartGeneration(2, Checkpoint({{"u1", "b"}}), Now, 0.0));
	}
	Ops2.UnreadablePaths = { GenPath(2) };
	const RecoveredNamespace N2 = ReadNamespace(Ops2, Dir, true);
	CHECK(N2.bFallbackUsed);
	CHECK(N2.bSkippedUnreadable);
	CHECK_EQ(N2.UsedGeneration, 1u);
	CHECK(Ops2.Exists(GenPath(2)));
	CHECK(!Ops2.Exists(GenPath(2) + ".corrupt"));
	CHECK(SetAsideUnusedNewer(Ops2, Dir, JournalLogFormat, N2.UsedGeneration));
	CHECK(Ops2.Exists(GenPath(2) + ".corrupt"));

	// A read that may not modify (the cross-root scan) changes nothing.
	MemFileOps Ops3;
	Ops3.Files = Ops.Files;
	Ops3.Files[GenPath(4)] = Ops3.Files[GenPath(3)];
	Ops3.UnreadablePaths = { GenPath(4) };
	const RecoveredNamespace N3 = ReadNamespace(Ops3, Dir, false);
	CHECK(N3.bFallbackUsed);
	CHECK_EQ(N3.UsedGeneration, 3u);
	CHECK(Ops3.Exists(GenPath(4)));
}

TEST(Journal_OpenFailureAfterTheRenameNeverAppendsToTheOldGeneration)   // review r1 persist-M4 / core-formats-M2, §10.2.3
{
	MemFileOps Ops;
	Writer W(Ops, Dir, Proto());
	REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "a"}}), 0, 0.0));
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 1, 2, "a", "ab")}));

	// The rename to generation 2 succeeds, the open for append fails (a scanner): the rename is the switch.
	Ops.UnopenablePaths = { GenPath(2) };
	CHECK(!W.Rotate(Checkpoint({{"u1", "ab"}}, 2), 0, 0.0));
	CHECK(Ops.Exists(GenPath(2)));
	CHECK_EQ(W.Generation(), 2u);
	CHECK(W.NeedsRotation(0.0));
	const std::string Gen1 = Ops.Files[GenPath(1)];
	CHECK(!W.Commit(std::vector<Record>{Delta("u1", 2, 3, "ab", "abc")}));   // an ordinary failed commit (retry queue)
	CHECK_EQ(Ops.Files[GenPath(1)], Gen1);                                   // nothing goes to generation 1 again

	// The scanner lets go: the next commit opens generation 2 first and lands there.
	Ops.UnopenablePaths.clear();
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 2, 3, "ab", "abc")}));
	CHECK(!W.NeedsRotation(0.0));
	CHECK_EQ(Ops.Files[GenPath(1)], Gen1);
	RecoveredNamespace N = ReadNamespace(Ops, Dir, false);
	CHECK_EQ(N.UsedGeneration, 2u);
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("abc"));

	// A lasting failure: the next rotation goes past the existing generation 3, and the commits land in 4.
	Ops.UnopenablePaths = { GenPath(3) };
	CHECK(!W.Rotate(Checkpoint({{"u1", "abc"}}, 3), 0, 0.0));
	CHECK_EQ(W.Generation(), 3u);
	CHECK(!Ops.Exists(GenPath(1)));   // step 5 still retired g-1
	CHECK(!W.Commit(std::vector<Record>{Delta("u1", 3, 4, "abc", "abcd")}));
	REQUIRE(W.Rotate(Checkpoint({{"u1", "abc"}}, 3), 0, 0.0));
	CHECK_EQ(W.Generation(), 4u);
	REQUIRE(W.Commit(std::vector<Record>{Delta("u1", 3, 4, "abc", "abcd")}));
	N = ReadNamespace(Ops, Dir, false);
	CHECK_EQ(N.UsedGeneration, 4u);
	CHECK_EQ(Find(N, "u1")->TextUtf8, std::string("abcd"));

	// The session's first generation: renamed means started (its checkpoint is durable); the first commit opens it.
	MemFileOps Ops2;
	Ops2.UnopenablePaths = { GenPath(1) };
	Writer W2(Ops2, Dir, Proto());
	REQUIRE(W2.StartGeneration(1, Checkpoint({{"u1", "x"}}), 0, 0.0));
	CHECK(!W2.Commit(std::vector<Record>{Delta("u1", 1, 2, "x", "xy")}));
	Ops2.UnopenablePaths.clear();
	REQUIRE(W2.Commit(std::vector<Record>{Delta("u1", 1, 2, "x", "xy")}));
	CHECK_EQ(Find(ReadNamespace(Ops2, Dir, false), "u1")->TextUtf8, std::string("xy"));
}

TEST(Journal_SkippedUnreadableGenerationIsReported)   // (instances) review r2 persist-r2-M1, §10.5
{
	// An orphan read (bMayModify false) must know that what it read is an older generation than the newest one listed: such
	// a namespace is neither listed nor merged nor deleted this session.
	const int64_t Now = 1800000000000ll;
	MemFileOps Ops;
	Ops.NowUtcMs = Now;
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "older"}}), Now, 0.0));
	}
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(2, Checkpoint({{"u1", "newest, only here"}}), Now, 0.0));
	}
	RecoveredNamespace Clear = ReadNamespace(Ops, Dir, false);
	CHECK(!Clear.bSkippedUnreadable);
	CHECK_EQ(Find(Clear, "u1")->TextUtf8, std::string("newest, only here"));

	Ops.UnreadablePaths = { GenPath(2) };
	RecoveredNamespace Held = ReadNamespace(Ops, Dir, false);
	CHECK(Held.bSkippedUnreadable);
	CHECK(!Held.bUnreadable);
	CHECK(Held.bFallbackUsed);
	CHECK_EQ(Held.UsedGeneration, 1u);
	CHECK(Ops.Exists(GenPath(2)));        // read only: nothing set aside

	// Only the oldest generation unreadable while a newer one reads: nothing above the one read was skipped.
	Ops.UnreadablePaths = { GenPath(1) };
	RecoveredNamespace Older = ReadNamespace(Ops, Dir, false);
	CHECK_EQ(Older.UsedGeneration, 2u);
	CHECK(!Older.bSkippedUnreadable);
}

TEST(Journal_NewerMinorAboveIsNeverSetAside)   // review r2 (core minor 5), §10.6: a newer minor is read only, never modified
{
	const int64_t Now = 1800000000000ll;
	MemFileOps Ops;
	Ops.NowUtcMs = Now;
	{
		Writer W(Ops, Dir, Proto());
		REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "one"}}), Now, 0.0));
	}
	// Generation 2 was written by a newer minor of this format.
	FileHeader Newer = Proto();
	Newer.Generation = 2;
	Newer.Major = FormatMajor;
	Newer.Minor = static_cast<uint16_t>(FormatMinor + 1);
	std::string NewerBytes;
	EncodeHeader(Newer, NewerBytes);
	NewerBytes += "frames a newer minor wrote";
	Ops.Files[GenPath(2)] = NewerBytes;

	// Its header could not be read at the scan (the header read fails, ReadAll fails throughout): the reader uses generation 1
	// and leaves generation 2 exactly as it is, reading nothing more of it (one header read, review r3).
	Ops.UnreadablePaths = { GenPath(2) };
	Ops.PrefixOnlyPaths = { GenPath(2) };
	Ops.FailPrefixReads[GenPath(2)] = 1;
	const RecoveredNamespace N = ReadNamespace(Ops, Dir, true);
	CHECK(N.bSkippedUnreadable);
	CHECK(N.bFallbackUsed);
	CHECK_EQ(N.UsedGeneration, 1u);
	CHECK_EQ(Ops.FailPrefixReads[GenPath(2)], 0);
	CHECK(Ops.Exists(GenPath(2)));
	CHECK(!Ops.Exists(GenPath(2) + ".corrupt"));
	CHECK_EQ(Ops.Mtimes.count(GenPath(2)), 0u);   // not even touched
	CHECK(Ops.Exists(GenPath(1)));

	// Before the session's generation: the newer minor above the one read is never renamed, and nothing may start there.
	bool bNewerFormat = false;
	CHECK(!SetAsideUnusedNewer(Ops, Dir, JournalLogFormat, 1, &bNewerFormat));
	CHECK(bNewerFormat);
	CHECK(Ops.Exists(GenPath(2)));
	CHECK(!Ops.Exists(GenPath(2) + ".corrupt"));
	CHECK_EQ(Ops.Files[GenPath(2)], NewerBytes);

	// Once a newer format is found nothing is renamed, not even a damaged generation of this format beside it (review r3).
	Ops.Files[GenPath(5)] = "damaged, this format";
	CHECK(!SetAsideUnusedNewer(Ops, Dir, JournalLogFormat, 1, &bNewerFormat));
	CHECK(bNewerFormat);
	CHECK(Ops.Exists(GenPath(5)));
	CHECK(!Ops.Exists(GenPath(5) + ".corrupt"));
	Ops.Files.erase(GenPath(5));

	// An older major the reader cannot read is skipped as unreadable too.
	MemFileOps Older;
	{
		Writer W(Older, Dir, Proto());
		REQUIRE(W.StartGeneration(1, Checkpoint({{"u1", "one"}}), Now, 0.0));
	}
	Older.Files[GenPath(2)] = HeaderBytesFor(2, 0, Now);
	Older.UnreadablePaths = { GenPath(2) };
	const RecoveredNamespace NO = ReadNamespace(Older, Dir, true);
	CHECK(NO.bSkippedUnreadable);
	CHECK(Older.Exists(GenPath(2)));

	// A newer major above is treated the same; an older major above is left for the migration and does not block.
	Ops.Files.erase(GenPath(2));
	Ops.Files[GenPath(3)] = HeaderBytesFor(3, static_cast<uint16_t>(FormatMajor + 1), Now);
	CHECK(!SetAsideUnusedNewer(Ops, Dir, JournalLogFormat, 1, &bNewerFormat));
	CHECK(bNewerFormat);
	CHECK(Ops.Exists(GenPath(3)));
	Ops.Files.erase(GenPath(3));
	Ops.Files[GenPath(4)] = HeaderBytesFor(4, 0, Now);
	CHECK(SetAsideUnusedNewer(Ops, Dir, JournalLogFormat, 1, &bNewerFormat));
	CHECK(!bNewerFormat);
	CHECK(Ops.Exists(GenPath(4)));
}
