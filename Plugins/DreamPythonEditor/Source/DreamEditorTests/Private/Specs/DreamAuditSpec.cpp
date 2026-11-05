// SPDX-License-Identifier: Apache-2.0
// The audit record (TDD §11.4 M1, SEC-05, D20, §10.5, §10.7.4; run group, B30):
//   Dream.Audit.Files       (any editor) a private FDreamAudit on a temp folder with an injected clock: lines wait for the
//                          role, the secondary-style name, the primary name, the day roll and the size roll (no line split
//                          across files), the retention sweep (older than 30 days by the name's day, never today's, only
//                          the primary), the overflow count and audit.gap, and the GT cost of 10,000 records (T-AUD-004,
//                          T-AUD-005).
//   Dream.Audit.Runs        (attended) 100 runs across REPL, buffer, selection, Run File and a not executed run: exactly one
//                          run.end per run, valid against schema v1, run.start for the executed ones, code.sha256 of the
//                          compiled text and, for Run File, of the file's bytes (T-AUD-001, T-AUD-002, T-RUN-024).
//   Dream.Audit.Unwritable  (attended) an audit folder that cannot be created: runs execute, one banner (T-AUD-006).
//   Dream.Audit.LostRun     (any editor) FDreamRunService::RecordLostRun becomes run.end with outcome lost (§10.7.4; the
//                          kill -9 part, T-AUD-003, is the tier-6 G6 driver).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Diagnostics/DreamAudit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Run/DreamRunFile.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamTab.h"
#include "dream/Checksum.h"
#include "dream/Json.h"

#include <atomic>

namespace DreamAuditSpec
{
	using dream::json::Value;

	FString TempFolder(const TCHAR* Purpose)
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"),
			FString::Printf(TEXT("%s_%s"), Purpose, *DreamTest::UniqueSuffix())));
		FPaths::NormalizeDirectoryName(Dir);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		return Dir;
	}

	FString Sha256Utf8(const FString& Text)
	{
		const std::string Utf8 = DreamText::ToUtf8(Text);
		dream::Sha256 Hasher;
		Hasher.Update(Utf8.data(), Utf8.size());
		return DreamText::Utf8ToFString(dream::Sha256::ToHex(Hasher.Final()));
	}

	FString Sha256Bytes(const TArray<uint8>& Bytes)
	{
		dream::Sha256 Hasher;
		Hasher.Update(Bytes.GetData(), static_cast<size_t>(Bytes.Num()));
		return DreamText::Utf8ToFString(dream::Sha256::ToHex(Hasher.Final()));
	}

	FString Str(const Value& V)
	{
		return DreamText::Utf8ToFString(V.AsString());
	}

	/** Every line of the files as JSON objects; a line that does not parse is an error on Test. */
	TArray<Value> ReadLines(FAutomationTestBase& Test, const TArray<FString>& Files)
	{
		TArray<Value> Records;
		for (const FString& File : Files)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *File))
			{
				continue;
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, /*InCullEmpty*/ true);
			for (const FString& Line : Lines)
			{
				Value Parsed;
				std::string Error;
				if (!dream::json::Parse(DreamText::ToUtf8(Line), Parsed, &Error) || !Parsed.IsObject())
				{
					Test.AddError(FString::Printf(TEXT("%s: a line is not a JSON object (%hs): %s"), *FPaths::GetCleanFilename(File), Error.c_str(), *Line.Left(200)));
					continue;
				}
				Records.Add(MoveTemp(Parsed));
			}
		}
		return Records;
	}

	bool IsHex64(const Value& V)
	{
		if (!V.IsString() || V.AsString().size() != 64)
		{
			return false;
		}
		for (const char Ch : V.AsString())
		{
			if (!((Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f')))
			{
				return false;
			}
		}
		return true;
	}

	bool OneOf(const Value& V, std::initializer_list<const char*> Allowed)
	{
		if (!V.IsString())
		{
			return false;
		}
		for (const char* Item : Allowed)
		{
			if (V.AsString() == Item)
			{
				return true;
			}
		}
		return false;
	}

	/** Schema v1 of a run.end record (§11.4): every field, with its type and vocabulary. Empty when valid. */
	FString ValidateRunEnd(const Value& R)
	{
		TArray<FString> Problems;
		auto Need = [&Problems](bool bOk, const TCHAR* What)
		{
			if (!bOk)
			{
				Problems.Add(What);
			}
		};
		const bool bLost = R.Get("outcome").AsString() == "lost";
		Need(R.Get("v").IsInteger() && R.Get("v").AsInt() == 1, TEXT("v == 1"));
		Need(R.Get("ts").IsString(), TEXT("ts"));
		Need(R.Get("event").AsString() == "run.end", TEXT("event run.end"));
		Need(R.Get("session").IsString() && R.Get("session").AsString().size() == 36, TEXT("session"));
		Need(R.Get("run").IsInteger(), TEXT("run"));
		Need(OneOf(R.Get("route"), { "user", "shelf", "menu", "api", "console", "bridge", "agent", "headless" }), TEXT("route"));
		Need(OneOf(R.Get("kind"), { "selection", "line", "cell", "buffer", "repl", "file", "snippet", "reload", "debug", "task" }), TEXT("kind"));
		Need(R.Get("badge").IsNull() || R.Get("badge").IsString(), TEXT("badge"));
		Need(OneOf(R.Get("namespace"), { "session", "isolated", "agent" }) || (bLost && R.Get("namespace").IsNull()), TEXT("namespace"));
		Need(OneOf(R.Get("transaction"), { "on", "off" }) || (bLost && R.Get("transaction").IsNull()), TEXT("transaction"));
		const Value& Source = R.Get("source");
		if (!(bLost && Source.IsNull()))
		{
			Need(Source.IsObject(), TEXT("source object"));
			Need(Source.Get("path").IsNull() || Source.Get("path").IsString(), TEXT("source.path"));
			Need(IsHex64(Source.Get("sha256")), TEXT("source.sha256"));
			Need(Source.Get("dirty").GetKind() == dream::json::Kind::Bool, TEXT("source.dirty"));
			Need(OneOf(Source.Get("trust"), { "trusted", "run_once", "n/a", "untrusted", "blocked" }), TEXT("source.trust"));
		}
		const Value& Code = R.Get("code");
		if (!(bLost && Code.IsNull()))
		{
			Need(Code.IsObject(), TEXT("code object"));
			Need(IsHex64(Code.Get("sha256")), TEXT("code.sha256"));
			Need(Code.Get("bytes").IsInteger() && Code.Get("bytes").AsInt() >= 0, TEXT("code.bytes"));
			Need(Code.Get("lines").IsInteger() && Code.Get("lines").AsInt() >= 0, TEXT("code.lines"));
		}
		Need(R.Get("start").IsString() || R.Get("start").IsNull(), TEXT("start"));
		Need(R.Get("end").IsString() || R.Get("end").IsNull(), TEXT("end"));
		Need(R.Get("duration_ms").IsInteger() || (bLost && R.Get("duration_ms").IsNull()), TEXT("duration_ms"));
		Need(OneOf(R.Get("outcome"), { "ok", "error", "cancelled", "not executed", "lost" }), TEXT("outcome"));
		Need(R.Get("error_type").IsNull() || R.Get("error_type").IsString(), TEXT("error_type"));
		Need(R.Get("user").IsString() && R.Get("project").IsString() && R.Get("plugin").IsString(), TEXT("user, project, plugin"));
		return FString::Join(Problems, TEXT(", "));
	}

	FDreamRunRecord MakeRecord(int32 Number, const FDateTime& Utc)
	{
		FDreamRunRecord Rec;
		Rec.RunNumber = Number;
		Rec.RunIds.Add(DreamRunId::Make(7, static_cast<uint32>(Number)));
		Rec.Route = EDreamRunRoute::Buffer;
		Rec.TabTitle = TEXT("audit.py");
		Rec.FilePath = TEXT("C:/scripts/audit.py");
		Rec.Filename = Rec.FilePath;
		Rec.FirstLine = 1;
		Rec.LastLine = 3;
		Rec.SourceSha256 = Sha256Utf8(FString::Printf(TEXT("print(%d)"), Number));
		Rec.FileSha256 = Rec.SourceSha256;
		Rec.CodeBytes = 9;
		Rec.StartUtc = Utc;
		Rec.EndUtc = Utc;
		Rec.StartSeconds = 10.0;
		Rec.EnterSeconds = 10.1;
		Rec.EndSeconds = 10.4;
		Rec.State = EDreamRunState::Idle;
		Rec.Outcome = (Number % 3 == 0) ? EDreamRunOutcome::Error : EDreamRunOutcome::Ok;
		Rec.Detail = Rec.Outcome == EDreamRunOutcome::Error ? TEXT("ZeroDivisionError: division by zero") : TEXT("");
		return Rec;
	}

	TArray<FString> FilesIn(const FString& Folder)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Folder / TEXT("*")), /*Files*/ true, /*Directories*/ false);
		Names.Sort();
		return Names;
	}

	bool Touch(const FString& Path)
	{
		return FFileHelper::SaveStringToFile(TEXT("{\"v\":1,\"event\":\"run.end\"}\n"), *Path);
	}
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamAuditFilesSpec, "Dream.Audit.Files", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamAuditFilesSpec)

void FDreamAuditFilesSpec::Define()
{
	using namespace DreamAuditSpec;

	It("waits for the role, then writes the secondary and primary names and rolls at the day and at the size (T-AUD-004)", [this]()
	{
		const FString Root = TempFolder(TEXT("AuditFiles"));
		const FString Folder = Root / TEXT("audit");
		TSharedRef<std::atomic<int64>> Ticks = MakeShared<std::atomic<int64>>(FDateTime(2026, 10, 30, 23, 59, 0).GetTicks());
		FDreamAudit::FOptions Options;
		Options.RollBytes = 4096;
		Options.FlushSeconds = 0.05;
		Options.bShowBanner = false;
		Options.UtcNow = [Ticks]() { return FDateTime(Ticks->load()); };
		{
			FDreamAudit Audit(Options);
			Audit.Start(Root, TEXT("AuditSpec-abc123"));

			// A39: lines wait while the role is unknown (the first tab spawn decides it).
			Audit.RunEnded(MakeRecord(1, FDateTime(Ticks->load())));
			Audit.RunEnded(MakeRecord(2, FDateTime(Ticks->load())));
			FPlatformProcess::Sleep(0.3f);
			TestEqual(TEXT("nothing is written before the role is known"), Audit.GetWrittenFilesForTest().Num(), 0);

			// A forced flush (shutdown, a session that never opens the tab) uses the process-unique secondary-style name.
			TestTrue(TEXT("the forced flush finished"), Audit.FlushForTest());
			TArray<FString> Names = FilesIn(Folder);
			const FRegexPattern Secondary(TEXT("^audit-2026-10-30\\.i-[0-9]+-[0-9]+\\.jsonl$"));
			TestEqual(*FString::Printf(TEXT("one file: %s"), *FString::Join(Names, TEXT(", "))), Names.Num(), 1);
			if (Names.Num() == 1)
			{
				FRegexMatcher Matcher(Secondary, Names[0]);
				TestTrue(*FString::Printf(TEXT("the secondary-style name audit-YYYY-MM-DD.<sec-id>.jsonl: %s"), *Names[0]), Matcher.FindNext());
			}

			// The primary writes the shared name.
			Audit.SetRole(EDreamAuditRole::Primary, FDreamAudit::MakeSecondaryId());
			Audit.RunEnded(MakeRecord(3, FDateTime(Ticks->load())));
			TestTrue(TEXT("flushed"), Audit.FlushForTest());
			TestTrue(TEXT("the primary file of the day"), IFileManager::Get().FileExists(*(Folder / TEXT("audit-2026-10-30.jsonl"))));

			// The UTC day boundary starts a new file.
			Ticks->store(FDateTime(2026, 10, 31, 0, 0, 30).GetTicks());
			Audit.RunEnded(MakeRecord(4, FDateTime(Ticks->load())));
			TestTrue(TEXT("flushed"), Audit.FlushForTest());
			TestTrue(TEXT("a new file at the day boundary"), IFileManager::Get().FileExists(*(Folder / TEXT("audit-2026-10-31.jsonl"))));

			// 16 MB in production, 4 KB here: the next part, -2, -3, never splitting a line.
			for (int32 Number = 5; Number < 45; ++Number)
			{
				Audit.RunEnded(MakeRecord(Number, FDateTime(Ticks->load())));
			}
			TestTrue(TEXT("flushed"), Audit.FlushForTest());
			const FString Part2 = Folder / TEXT("audit-2026-10-31-2.jsonl");
			const FString Part3 = Folder / TEXT("audit-2026-10-31-3.jsonl");
			TestTrue(TEXT("the second part exists"), IFileManager::Get().FileExists(*Part2));
			TestTrue(TEXT("the third part exists"), IFileManager::Get().FileExists(*Part3));
			for (const FString& Part : { Folder / TEXT("audit-2026-10-31.jsonl"), Part2 })
			{
				TestTrue(*FString::Printf(TEXT("%s stays within the roll size"), *FPaths::GetCleanFilename(Part)), IFileManager::Get().FileSize(*Part) <= Options.RollBytes);
			}
			const TArray<Value> Records = ReadLines(*this, Audit.GetWrittenFilesForTest());
			TestEqual(TEXT("every record is written once"), Records.Num(), 44);
			for (const Value& Record : Records)
			{
				const FString Problems = ValidateRunEnd(Record);
				if (!Problems.IsEmpty())
				{
					AddError(FString::Printf(TEXT("run %lld is not valid against schema v1: %s"), static_cast<long long>(Record.Get("run").AsInt()), *Problems));
					break;
				}
			}
			Audit.Shutdown();
		}
		IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true);
	});

	It("sweeps files older than retention.audit_days, never today's, and only as the primary (TDD 11.4, TDD 10.5)", [this]()
	{
		const FString Root = TempFolder(TEXT("AuditSweep"));
		const FString Folder = Root / TEXT("audit");
		IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);
		const FDateTime Today(2026, 10, 30, 12, 0, 0);
		auto Day = [&Today](int32 Back) { return (Today - FTimespan::FromDays(Back)).ToString(TEXT("%Y-%m-%d")); };
		const TArray<FString> Old = {
			FString::Printf(TEXT("audit-%s.jsonl"), *Day(40)),
			FString::Printf(TEXT("audit-%s-2.jsonl"), *Day(31)),
			FString::Printf(TEXT("audit-%s.i-11-22.jsonl"), *Day(31)),
		};
		const TArray<FString> Kept = {
			FString::Printf(TEXT("audit-%s.jsonl"), *Day(30)),
			FString::Printf(TEXT("audit-%s.jsonl"), *Day(29)),
			FString::Printf(TEXT("audit-%s.jsonl"), *Day(0)),
			TEXT("notes.txt"),
		};
		for (const FString& Name : Old)
		{
			Touch(Folder / Name);
		}
		for (const FString& Name : Kept)
		{
			Touch(Folder / Name);
		}

		FDreamAudit::FOptions Options;
		Options.bShowBanner = false;
		Options.UtcNow = [Today]() { return Today; };
		{
			// A secondary never sweeps (§10.5).
			FDreamAudit Secondary(Options);
			Secondary.Start(Root, TEXT("AuditSpec-abc123"));
			Secondary.SetRole(EDreamAuditRole::Secondary, TEXT("i-11-22"));
			Secondary.SweepNowForTest();
			for (const FString& Name : Old)
			{
				TestTrue(*FString::Printf(TEXT("a secondary leaves %s"), *Name), IFileManager::Get().FileExists(*(Folder / Name)));
			}
			Secondary.Shutdown();
		}
		{
			FDreamAudit Primary(Options);
			Primary.Start(Root, TEXT("AuditSpec-abc123"));
			Primary.SetRole(EDreamAuditRole::Primary, TEXT("i-1-2"));
			Primary.SweepNowForTest();
			for (const FString& Name : Old)
			{
				TestFalse(*FString::Printf(TEXT("the primary deletes %s (older than 30 days)"), *Name), IFileManager::Get().FileExists(*(Folder / Name)));
			}
			for (const FString& Name : Kept)
			{
				TestTrue(*FString::Printf(TEXT("the primary keeps %s"), *Name), IFileManager::Get().FileExists(*(Folder / Name)));
			}
			Primary.Shutdown();
		}
		IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true);
	});

	It("counts the lines a full queue drops and writes audit.gap before the next lines (TDD 11.4)", [this]()
	{
		const FString Root = TempFolder(TEXT("AuditGap"));
		FDreamAudit::FOptions Options;
		Options.QueueCapacity = 6;
		Options.FlushSeconds = 30.0;      // only the forced flushes below write
		Options.bShowBanner = false;
		{
			FDreamAudit Audit(Options);
			Audit.Start(Root, TEXT("AuditSpec-abc123"));
			Audit.SetRole(EDreamAuditRole::Primary, TEXT("i-1-2"));
			TestTrue(TEXT("the role's flush is done"), Audit.FlushForTest());
			for (int32 Number = 1; Number <= 10; ++Number)
			{
				Audit.RunEnded(MakeRecord(Number, FDateTime::UtcNow()));
			}
			TestEqual(TEXT("four lines were dropped"), Audit.GetDroppedForTest(), static_cast<uint64>(4));
			TestTrue(TEXT("flushed"), Audit.FlushForTest());
			const TArray<Value> Records = ReadLines(*this, Audit.GetWrittenFilesForTest());
			if (TestEqual(TEXT("audit.gap and the six queued records"), Records.Num(), 7))
			{
				TestEqual(TEXT("audit.gap first"), Str(Records[0].Get("event")), FString(TEXT("audit.gap")));
				TestEqual(TEXT("it counts the dropped lines"), Records[0].Get("dropped").AsInt(), static_cast<int64_t>(4));
				TestEqual(TEXT("then the queued runs"), Records[1].Get("run").AsInt(), static_cast<int64_t>(1));
			}
			Audit.Shutdown();
		}
		IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true);
	});

	It("builds a record on the game thread in at most 0.05 ms at p95 over 10,000 runs (T-AUD-005, proposed)", [this]()
	{
		const FString Root = TempFolder(TEXT("AuditCost"));
		FDreamAudit::FOptions Options;
		Options.bShowBanner = false;
		{
			FDreamAudit Audit(Options);
			Audit.Start(Root, TEXT("AuditSpec-abc123"));
			Audit.SetRole(EDreamAuditRole::Primary, TEXT("i-1-2"));
			TArray<double> Costs;
			Costs.Reserve(10000);
			const FDateTime Now = FDateTime::UtcNow();
			for (int32 Number = 1; Number <= 10000; ++Number)
			{
				const FDreamRunRecord Rec = MakeRecord(Number, Now);
				const uint64 Start = FPlatformTime::Cycles64();
				Audit.RunEnded(Rec);
				Costs.Add(FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - Start));
			}
			Costs.Sort();
			const double P50 = Costs[Costs.Num() / 2];
			const double P95 = Costs[Costs.Num() * 95 / 100];
			AddInfo(FString::Printf(TEXT("audit record cost: p50 %.4f ms, p95 %.4f ms, max %.4f ms"), P50, P95, Costs.Last()));
			TestTrue(*FString::Printf(TEXT("p95 %.4f ms <= 0.05 ms"), P95), P95 <= 0.05);
			TestTrue(TEXT("flushed"), Audit.FlushForTest());
			const TArray<Value> Records = ReadLines(*this, Audit.GetWrittenFilesForTest());
			TestEqual(TEXT("all 10,000 records were written"), Records.Num() + static_cast<int32>(Audit.GetDroppedForTest()), 10000);
			Audit.Shutdown();
		}
		IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true);
	});
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamAuditRunsSpec, "Dream.Audit.Runs", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	FString Dir;
END_DEFINE_SPEC(FDreamAuditRunsSpec)

void FDreamAuditRunsSpec::Define()
{
	using namespace DreamAuditSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		if (!Dir.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		}
		Dir.Reset();
		Tab.Reset();
	});

	LatentIt("writes one valid run.end per run of 100 across routes, with the compiled text's SHA-256 (T-AUD-001, T-AUD-002, T-RUN-024)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
			FString::Printf(TEXT("audit_a_%s = 1\naudit_b_%s = 2\naudit_c_%s = 3"), *Sfx, *Sfx, *Sfx), TEXT("Audit"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());

		// A file with a BOM and CRLF: its bytes are not the text the runner compiles (T-AUD-002, "Run File (the file bytes)").
		Dir = TempFolder(TEXT("AuditRuns"));
		const FString FilePath = Dir / FString::Printf(TEXT("audit_%s.py"), *Sfx);
		TArray<uint8> FileBytes = { 0xEF, 0xBB, 0xBF };
		const std::string Body = DreamText::ToUtf8(FString::Printf(TEXT("audit_file_%s = 1\r\naudit_file2_%s = 2\r\n"), *Sfx, *Sfx));
		FileBytes.Append(reinterpret_cast<const uint8*>(Body.data()), static_cast<int32>(Body.size()));
		if (!TestTrue(TEXT("the run file was written"), FFileHelper::SaveArrayToFile(FileBytes, *FilePath)))
		{
			Done.Execute();
			return;
		}

		struct FExpected
		{
			TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
			FString CodeSha;
			bool bExecuted = true;
			const TCHAR* What = TEXT("");
		};
		TSharedRef<TArray<TSharedRef<FExpected>>> Runs = MakeShared<TArray<TSharedRef<FExpected>>>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);

		auto Add = [this, &Steps, Runs](const TCHAR* What, TFunction<FDreamRunRequest()> Make, const FString& CodeSha)
		{
			TSharedRef<FExpected> Entry = MakeShared<FExpected>();
			Entry->What = What;
			Entry->CodeSha = CodeSha;
			Runs->Add(Entry);
			DreamTest::AddRun(Steps, What, MoveTemp(Make), Entry->Run, 60.0);
		};

		// Run File of a file that is not open in a tab: code.sha256 is the file's bytes.
		Add(TEXT("the Run File"), [FilePath]()
		{
			FDreamRunRequest Request;
			FText Error;
			DreamRunFile::MakeFileRequest(FilePath, EDreamRunRoute::RunFile, Request, Error);
			Request.TransactionOverride = false;
			return Request;
		}, Sha256Bytes(FileBytes));
		const FDreamRunRequest Selection = DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Selection, 2, 3);
		Add(TEXT("the selection run"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Selection, 2, 3); },
			Sha256Utf8(Selection.Text));
		const FDreamRunRequest Whole = DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 3);
		Add(TEXT("the buffer run"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 3); },
			Sha256Utf8(Whole.Text));

		// A not executed run (Stop before user code started, §5.1.2 step 5): still one run.end, no run.start (T-RUN-024).
		{
			TSharedRef<FExpected> Entry = MakeShared<FExpected>();
			Entry->What = TEXT("the run stopped before entry");
			Entry->bExecuted = false;
			const FString Code = FString::Printf(TEXT("audit_never_%s = 1"), *Sfx);
			Entry->CodeSha = Sha256Utf8(Code);
			Runs->Add(Entry);
			Steps.Do(TEXT("submit and stop a run in Starting"), [this, Entry, Code]()
			{
				Entry->Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(Code));
				FDreamEditorModule::Get().GetRunService().RequestStopActive(EDreamStopSource::Api);
				return Entry->Run->RunNumber > 0;
			});
			DreamTest::AddWaitForRun(Steps, Entry->What, Entry->Run);
		}

		for (int32 Index = 0; Index < 96; ++Index)
		{
			const FString Code = FString::Printf(TEXT("audit_repl_%s = %d"), *Sfx, Index);
			Add(TEXT("a REPL run"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Sha256Utf8(Code));
		}

		Steps.Do(TEXT("read the audit files"), [this, Runs]()
		{
			FDreamAudit& Audit = FDreamEditorModule::Get().GetAudit();
			TestTrue(TEXT("the audit writer flushed"), Audit.FlushForTest());
			const std::string Session = DreamText::ToUtf8(Audit.GetSessionId().ToString(EGuidFormats::DigitsWithHyphensLower));
			TMap<int64, TArray<const Value*>> Ends;
			TMap<int64, int32> Starts;
			const TArray<Value> Records = ReadLines(*this, Audit.GetWrittenFilesForTest());
			for (const Value& Record : Records)
			{
				if (Record.Get("session").AsString() != Session)
				{
					continue;
				}
				if (Record.Get("event").AsString() == "run.end")
				{
					Ends.FindOrAdd(Record.Get("run").AsInt()).Add(&Record);
				}
				else if (Record.Get("event").AsString() == "run.start")
				{
					++Starts.FindOrAdd(Record.Get("run").AsInt());
				}
			}
			TestEqual(TEXT("100 runs"), Runs->Num(), 100);
			for (const TSharedRef<FExpected>& Entry : *Runs)
			{
				const int64 Number = Entry->Run->RunNumber;
				const TArray<const Value*>* Found = Ends.Find(Number);
				if (!TestTrue(*FString::Printf(TEXT("%s (#%lld) has a run.end"), Entry->What, static_cast<long long>(Number)), Found != nullptr)
					|| !TestEqual(*FString::Printf(TEXT("%s (#%lld) has exactly one run.end"), Entry->What, static_cast<long long>(Number)), Found->Num(), 1))
				{
					continue;
				}
				const Value& End = *(*Found)[0];
				const FString Problems = ValidateRunEnd(End);
				TestTrue(*FString::Printf(TEXT("%s (#%lld) is valid against schema v1: %s"), Entry->What, static_cast<long long>(Number), *Problems), Problems.IsEmpty());
				TestEqual(*FString::Printf(TEXT("%s (#%lld) code.sha256"), Entry->What, static_cast<long long>(Number)),
					Str(End.Get("code").Get("sha256")), Entry->CodeSha);
				TestEqual(*FString::Printf(TEXT("%s (#%lld) run.start"), Entry->What, static_cast<long long>(Number)), Starts.FindRef(Number), Entry->bExecuted ? 1 : 0);
				if (!Entry->bExecuted)
				{
					TestEqual(TEXT("the stopped run is not executed"), Str(End.Get("outcome")), FString(TEXT("not executed")));
				}
			}
			// The kinds and routes of the first three (§11.4 table).
			if (Runs->Num() >= 4)
			{
				auto Kind = [&Ends](int64 Number) -> FString
				{
					const TArray<const Value*>* Found = Ends.Find(Number);
					return Found && Found->Num() > 0 ? Str((*Found)[0]->Get("kind")) + TEXT("/") + Str((*Found)[0]->Get("route")) : FString();
				};
				TestEqual(TEXT("Run File: file/user"), Kind((*Runs)[0]->Run->RunNumber), FString(TEXT("file/user")));
				TestEqual(TEXT("selection/user"), Kind((*Runs)[1]->Run->RunNumber), FString(TEXT("selection/user")));
				TestEqual(TEXT("buffer/user"), Kind((*Runs)[2]->Run->RunNumber), FString(TEXT("buffer/user")));
				TestEqual(TEXT("REPL: repl/user"), Kind((*Runs)[4]->Run->RunNumber), FString(TEXT("repl/user")));
				const TArray<const Value*>* RunFile = Ends.Find((*Runs)[0]->Run->RunNumber);
				if (RunFile && RunFile->Num() > 0)
				{
					TestEqual(TEXT("Run File has no transaction by default (RT-08)"), Str((*RunFile)[0]->Get("transaction")), FString(TEXT("off")));
					TestEqual(TEXT("Run File's source is the file on disk"), Str((*RunFile)[0]->Get("source").Get("sha256")), (*Runs)[0]->CodeSha);
				}
			}
			return true;
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamAuditUnwritableSpec, "Dream.Audit.Unwritable", DreamTest::AttendedFlags)
	FString Dir;
END_DEFINE_SPEC(FDreamAuditUnwritableSpec)

void FDreamAuditUnwritableSpec::Define()
{
	using namespace DreamAuditSpec;

	AfterEach([this]()
	{
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetAudit().SetFolderOverrideForTest(FString());
		}
		if (!Dir.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		}
		Dir.Reset();
	});

	LatentIt("keeps runs going and shows one banner when the audit folder cannot be written (T-AUD-006)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		// A file where the folder should be: the writer can neither create the folder nor open a file in it.
		Dir = TempFolder(TEXT("AuditUnwritable"));
		const FString Blocker = Dir / TEXT("blocker");
		FFileHelper::SaveStringToFile(TEXT("not a folder"), *Blocker);
		FDreamAudit* Audit = &FDreamEditorModule::Get().GetAudit();
		const int32 BannersBefore = Audit->GetBannerCountForTest();
		const bool bFailedBefore = Audit->HasWriteFailed();
		Audit->SetFolderOverrideForTest(Blocker / TEXT("audit"));

		TSharedRef<DreamTest::FRunResult> First = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Second = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Third = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("the first run"), []() { return DreamTest::MakeReplRequest(TEXT("print('audit-unwritable-1')")); }, First, 60.0);
		DreamTest::AddRun(Steps, TEXT("the second run"), []() { return DreamTest::MakeReplRequest(TEXT("print('audit-unwritable-2')")); }, Second);
		Steps.Poll(TEXT("the writer reports the folder"), [Audit]()
		{
			return Audit->HasWriteFailed() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Poll(TEXT("the banner is shown"), [Audit, BannersBefore, bFailedBefore]()
		{
			return Audit->GetBannerCountForTest() > BannersBefore || bFailedBefore ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		DreamTest::AddRun(Steps, TEXT("a run after the failure"), []() { return DreamTest::MakeReplRequest(TEXT("print('audit-unwritable-3')")); }, Third);
		Steps.WaitFrames(30);
		Steps.Do(TEXT("check"), [this, First, Second, Third, Audit, BannersBefore, bFailedBefore]()
		{
			for (const TSharedRef<DreamTest::FRunResult>& Run : { First, Second, Third })
			{
				DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			}
			if (bFailedBefore || BannersBefore > 0)
			{
				AddInfo(TEXT("the audit folder had already failed in this session; its one banner was shown before this spec"));
			}
			else
			{
				TestEqual(TEXT("one banner for the session"), Audit->GetBannerCountForTest(), BannersBefore + 1);
			}
			return true;
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamAuditLostRunSpec, "Dream.Audit.LostRun", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamAuditLostRunSpec)

void FDreamAuditLostRunSpec::Define()
{
	using namespace DreamAuditSpec;

	It("writes a lost run of the previous session as run.end with outcome lost (TDD 10.7.4, TDD 11.4)", [this]()
	{
		FDreamEditorModule* Module = DreamTest::GetModule(*this);
		if (Module == nullptr)
		{
			return;
		}
		FDreamLostRun Lost;
		Lost.PreviousSessionUtc = FDateTime::UtcNow() - FTimespan::FromMinutes(5);
		Lost.StartUtc = Lost.PreviousSessionUtc - FTimespan::FromSeconds(30);
		Lost.RunNumber = 90000 + FMath::RandRange(0, 9999);
		Lost.RunId = DreamRunId::Make(3, 41);
		Lost.Route = EDreamRunRoute::Selection;
		Lost.TabTitle = TEXT("rig_fix.py");
		Module->GetRunService().RecordLostRun(Lost);

		FDreamAudit& Audit = Module->GetAudit();
		TestTrue(TEXT("flushed"), Audit.FlushForTest());
		const std::string Session = DreamText::ToUtf8(Audit.GetSessionId().ToString(EGuidFormats::DigitsWithHyphensLower));
		int32 Found = 0;
		for (const Value& Record : ReadLines(*this, Audit.GetWrittenFilesForTest()))
		{
			if (Record.Get("session").AsString() == Session && Record.Get("run").AsInt() == Lost.RunNumber
				&& Record.Get("previous_session").AsBool(false))
			{
				++Found;
				const FString Problems = ValidateRunEnd(Record);
				TestTrue(*FString::Printf(TEXT("valid against schema v1: %s"), *Problems), Problems.IsEmpty());
				TestEqual(TEXT("outcome lost"), Str(Record.Get("outcome")), FString(TEXT("lost")));
				TestEqual(TEXT("its run ID"), Record.Get("run_id").AsUInt(), static_cast<uint64_t>(Lost.RunId));
				TestEqual(TEXT("kind selection"), Str(Record.Get("kind")), FString(TEXT("selection")));
			}
		}
		TestEqual(TEXT("one lost record"), Found, 1);
	});
}
