// SPDX-License-Identifier: Apache-2.0
// FDreamAudit (TDD §11.4 M1, SEC-05, D20, §10.5, §10.7.4). See DreamAudit.h. The game thread builds each JSON line; the
// DreamAudit thread hashes the buffer text a dirty source needs (so the game thread never hashes a whole buffer), writes
// the lines, rolls the files and sweeps old ones. A write failure drops the lines it held (counted for audit.gap) and is
// shown once; runs are never blocked.
#include "Diagnostics/DreamAudit.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Run/DreamDeferral.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "dream/Checksum.h"
#include "dream/Json.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#include <atomic>
#include <string>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamAuditLocal
{
	using dream::json::Value;

	/** The placeholder a source.sha256 carries until the writer has hashed the buffer text (never a real digest). */
	constexpr const char* PendingSha = "#dream-pending-sha256#";

	std::string Utf8(const FString& Text)
	{
		return DreamText::ToUtf8(Text);
	}

	Value Str(const FString& Text)
	{
		return Value::MakeString(Utf8(Text));
	}

	Value StrOrNull(const FString& Text)
	{
		return Text.IsEmpty() ? Value::MakeNull() : Str(Text);
	}

	/** "2026-10-28T14:03:11.204Z" (§11.4 schema). */
	Value Iso(const FDateTime& Utc)
	{
		return Utc.GetTicks() > 0 ? Str(Utc.ToIso8601()) : Value::MakeNull();
	}

	const char* OutcomeKey(EDreamRunOutcome Outcome)
	{
		switch (Outcome)
		{
		case EDreamRunOutcome::Ok:
			return "ok";
		case EDreamRunOutcome::Error:
			return "error";
		case EDreamRunOutcome::Cancelled:
			return "cancelled";
		case EDreamRunOutcome::NotExecuted:
			return "not executed";
		case EDreamRunOutcome::Lost:
			return "lost";
		case EDreamRunOutcome::None:
			break;
		}
		return "error";
	}

	const char* NamespaceKey(EDreamNamespaceMode Mode)
	{
		switch (Mode)
		{
		case EDreamNamespaceMode::Session:
			return "session";
		case EDreamNamespaceMode::Isolated:
			return "isolated";
		case EDreamNamespaceMode::Agent:
			return "agent";
		}
		return "session";
	}

	/** error_type holds the class name only, never the message (§11.4): the detail's text before the first ": " (A51). */
	Value ErrorType(const FDreamRunRecord& Run)
	{
		if (Run.Outcome != EDreamRunOutcome::Error)
		{
			return Value::MakeNull();
		}
		if (Run.ExitCode.IsSet())
		{
			return Value::MakeString("SystemExit");
		}
		if (Run.bInternalError || Run.Detail.IsEmpty())
		{
			return Value::MakeNull();
		}
		FString Type = Run.Detail;
		int32 Colon = INDEX_NONE;
		if (Type.FindChar(TEXT(':'), Colon) && Colon > 0)
		{
			Type.LeftInline(Colon);
		}
		return Str(Type.TrimStartAndEnd());
	}

	std::string Sha256Of(const dream::TextSnapshot& Snapshot)
	{
		const std::string Text = DreamText::ToUtf8(DreamText::ToView(Snapshot.Text()));
		dream::Sha256 Hasher;
		Hasher.Update(Text.data(), Text.size());
		return dream::Sha256::ToHex(Hasher.Final());
	}

	/** The YYYY-MM-DD at the start of an audit file name, or false. */
	bool ParseDay(const FString& Name, FDateTime& OutDay)
	{
		// audit-YYYY-MM-DD[-n][.<sec-id>].jsonl
		if (!Name.StartsWith(TEXT("audit-")) || Name.Len() < 16 || !Name.EndsWith(TEXT(".jsonl")))
		{
			return false;
		}
		const FString Day = Name.Mid(6, 10);
		int32 Year = 0;
		int32 Month = 0;
		int32 DayOfMonth = 0;
		if (Day[4] != TEXT('-') || Day[7] != TEXT('-'))
		{
			return false;
		}
		Year = FCString::Atoi(*Day.Left(4));
		Month = FCString::Atoi(*Day.Mid(5, 2));
		DayOfMonth = FCString::Atoi(*Day.Mid(8, 2));
		if (!FDateTime::Validate(Year, Month, DayOfMonth, 0, 0, 0, 0))
		{
			return false;
		}
		OutDay = FDateTime(Year, Month, DayOfMonth);
		return true;
	}
}

// ---- The writer thread --------------------------------------------------------------------------------------------

struct FDreamAudit::FWriter : public FRunnable
{
	struct FItem
	{
		std::string Line;
		dream::TextSnapshot HashSource;      // hashed into the PendingSha placeholder when bHashSource
		bool bHashSource = false;
	};

	// Shared with the game thread (Lock).
	mutable FCriticalSection Lock;
	TArray<FItem> Pending;
	uint64 Dropped = 0;
	FString Folder;
	FString FolderOverride;
	EDreamAuditRole Role = EDreamAuditRole::Unknown;
	FString SecondaryId;
	FString FailureReason;
	TArray<FString> WrittenFiles;
	bool bForceNext = false;                // FlushForTest and shutdown: an unknown role writes the secondary-style name
	bool bSweepNext = false;
	std::string GapPrefix;                  // "session", "user", "project", "plugin" of the audit.gap record (JSON members)

	std::atomic<bool> bStop{ false };
	std::atomic<bool> bExited{ false };
	std::atomic<bool> bFence{ false };      // abandoned: no further file operation
	std::atomic<bool> bFailed{ false };
	std::atomic<uint64> FlushesDone{ 0 };
	FEvent* Wake = nullptr;

	FOptions Options;

	// Writer-only state.
	FString CurrentDay;
	FString CurrentBase;                    // the folder and the name tail the current part belongs to
	int32 CurrentPart = 1;
	int64 CurrentSize = 0;
	FDateTime LastSweepUtc;
	bool bSwept = false;

	explicit FWriter(const FOptions& InOptions)
		: Options(InOptions)
	{
		Wake = FPlatformProcess::GetSynchEventFromPool(false);
	}

	virtual ~FWriter() override
	{
		if (Wake != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(Wake);
			Wake = nullptr;
		}
	}

	FDateTime Now() const
	{
		return Options.UtcNow ? Options.UtcNow() : FDateTime::UtcNow();
	}

	virtual uint32 Run() override
	{
		const uint32 WaitMs = static_cast<uint32>(FMath::Max(1.0, Options.FlushSeconds * 1000.0));
		while (!bStop.load())
		{
			Wake->Wait(WaitMs);
			FlushOnce(/*bFinal*/ false);
		}
		FlushOnce(/*bFinal*/ true);
		bExited.store(true);
		return 0;
	}

	virtual void Stop() override
	{
		bStop.store(true);
		if (Wake != nullptr)
		{
			Wake->Trigger();
		}
	}

	void Fail(const FString& Reason)
	{
		FScopeLock Guard(&Lock);
		if (FailureReason.IsEmpty())
		{
			FailureReason = Reason;
		}
		bFailed.store(true);
	}

	FString FileName(const FString& Day, int32 Part, EDreamAuditRole InRole, const FString& InSecondaryId) const
	{
		const FString PartText = Part > 1 ? FString::Printf(TEXT("-%d"), Part) : FString();
		return InRole == EDreamAuditRole::Primary
			? FString::Printf(TEXT("audit-%s%s.jsonl"), *Day, *PartText)
			: FString::Printf(TEXT("audit-%s%s.%s.jsonl"), *Day, *PartText, *InSecondaryId);
	}

	void FlushOnce(bool bFinal)
	{
		TArray<FItem> Items;
		uint64 DroppedNow = 0;
		FString TargetFolder;
		EDreamAuditRole TargetRole = EDreamAuditRole::Unknown;
		FString TargetSecondary;
		bool bSweep = false;
		{
			FScopeLock Guard(&Lock);
			TargetFolder = FolderOverride.IsEmpty() ? Folder : FolderOverride;
			TargetRole = Role;
			TargetSecondary = SecondaryId;
			const bool bForce = bFinal || bForceNext;
			bForceNext = false;
			bSweep = bSweepNext;
			bSweepNext = false;
			// A39: until the role is known only a forced flush (or a queue half full) writes, under the secondary-style name.
			const bool bMayWrite = TargetRole != EDreamAuditRole::Unknown || bForce || Pending.Num() >= Options.QueueCapacity / 2;
			if (bMayWrite && !TargetFolder.IsEmpty())
			{
				Items = MoveTemp(Pending);
				Pending.Reset();
				DroppedNow = Dropped;
				Dropped = 0;
			}
		}
		if (bFence.load())
		{
			return;
		}
		if (TargetRole == EDreamAuditRole::Unknown)
		{
			TargetRole = EDreamAuditRole::Secondary;     // the process-unique name: never another instance's file
		}
		if (TargetSecondary.IsEmpty())
		{
			TargetSecondary = FDreamAudit::MakeSecondaryId();
		}

		if (!Items.IsEmpty() || DroppedNow > 0)
		{
			Write(TargetFolder, TargetRole, TargetSecondary, Items, DroppedNow);
		}
		if (TargetRole == EDreamAuditRole::Primary && !TargetFolder.IsEmpty()
			&& (bSweep || !bSwept || (Now() - LastSweepUtc).GetTotalDays() >= 1.0))
		{
			// §11.4: at startup and once a day; only the primary sweeps (§10.5).
			Sweep(TargetFolder);
		}
		FlushesDone.fetch_add(1);
	}

	void Write(const FString& TargetFolder, EDreamAuditRole TargetRole, const FString& TargetSecondary, TArray<FItem>& Items, uint64 DroppedNow)
	{
		std::string Block;
		if (DroppedNow > 0)
		{
			// §11.4: lines dropped on overflow (or by a failed write) are counted; the next flush writes audit.gap.
			std::string Gap = "{\"v\":1,\"ts\":";
			Gap += dream::json::Serialize(DreamAuditLocal::Iso(Now()));
			Gap += ",\"event\":\"audit.gap\",\"dropped\":";
			Gap += std::to_string(DroppedNow);
			Gap += GapPrefix;
			Gap += "}\n";
			Block += Gap;
		}
		for (FItem& Item : Items)
		{
			if (Item.bHashSource)
			{
				const size_t At = Item.Line.find(DreamAuditLocal::PendingSha);
				if (At != std::string::npos)
				{
					Item.Line.replace(At, std::char_traits<char>::length(DreamAuditLocal::PendingSha), DreamAuditLocal::Sha256Of(Item.HashSource));
				}
			}
			Block += Item.Line;
			Block += '\n';
		}

		// The lines a failed write loses, from byte From of Block: the audit.gap line at its start stands for DroppedNow lines,
		// so the next audit.gap counts them all again (and a partly written block only what it did not write).
		auto Unwritten = [&Block, DroppedNow](size_t From) -> uint64
		{
			uint64 Lines = 0;
			for (size_t Index = From; Index < Block.size(); ++Index)
			{
				Lines += Block[Index] == '\n' ? 1 : 0;
			}
			if (From == 0 && DroppedNow > 0 && Lines > 0)
			{
				Lines = Lines - 1 + DroppedNow;
			}
			return Lines;
		};

		IFileManager& Files = IFileManager::Get();
		if (!Files.DirectoryExists(*TargetFolder) && !Files.MakeDirectory(*TargetFolder, /*Tree*/ true))
		{
			Fail(TEXT("the folder cannot be created"));
			Recount(Unwritten(0));
			return;
		}

		const FString Day = Now().ToString(TEXT("%Y-%m-%d"));
		const FString Base = TargetFolder / FileName(Day, 1, TargetRole, TargetSecondary);
		if (Day != CurrentDay || Base != CurrentBase)
		{
			// A new day (or folder or role): the first part that is not full yet.
			CurrentDay = Day;
			CurrentBase = Base;
			CurrentPart = 1;
			CurrentSize = 0;
			for (;;)
			{
				const FString Candidate = TargetFolder / FileName(Day, CurrentPart, TargetRole, TargetSecondary);
				const int64 Size = Files.FileSize(*Candidate);
				if (Size < 0 || Size < Options.RollBytes)
				{
					CurrentSize = FMath::Max<int64>(Size, 0);
					break;
				}
				++CurrentPart;
			}
		}

		// Lines are never split across files; a part rolls before a line that would take it past RollBytes.
		size_t Begin = 0;
		while (Begin < Block.size())
		{
			if (bFence.load())
			{
				return;
			}
			size_t End = Begin;
			int64 Size = CurrentSize;
			while (End < Block.size())
			{
				size_t LineEnd = Block.find('\n', End);
				LineEnd = LineEnd == std::string::npos ? Block.size() : LineEnd + 1;
				const int64 LineBytes = static_cast<int64>(LineEnd - End);
				if (Size > 0 && Size + LineBytes > Options.RollBytes)
				{
					break;
				}
				Size += LineBytes;
				End = LineEnd;
			}
			if (End == Begin)
			{
				++CurrentPart;      // this part is full: the next one
				CurrentSize = 0;
				continue;
			}
			const FString Path = TargetFolder / FileName(Day, CurrentPart, TargetRole, TargetSecondary);
			TUniquePtr<FArchive> File(Files.CreateFileWriter(*Path, FILEWRITE_Append | FILEWRITE_AllowRead | FILEWRITE_Silent));
			if (!File.IsValid())
			{
				Fail(FString::Printf(TEXT("%s cannot be opened for writing"), *FPaths::GetCleanFilename(Path)));
				Recount(Unwritten(Begin));
				return;
			}
			File->Serialize(const_cast<char*>(Block.data() + Begin), static_cast<int64>(End - Begin));
			const bool bError = File->IsError();
			File->Close();
			if (bError || File->IsError())
			{
				Fail(FString::Printf(TEXT("writing %s failed"), *FPaths::GetCleanFilename(Path)));
				Recount(Unwritten(Begin));
				return;
			}
			CurrentSize = Size;
			{
				FScopeLock Guard(&Lock);
				WrittenFiles.AddUnique(Path);
			}
			Begin = End;
		}
	}

	/** A failed write: its lines, and the earlier gap its audit.gap line carried, are counted for the next audit.gap. */
	void Recount(uint64 Lost)
	{
		FScopeLock Guard(&Lock);
		Dropped += Lost;
	}

	void Sweep(const FString& TargetFolder)
	{
		bSwept = true;
		LastSweepUtc = Now();
		if (bFence.load())
		{
			return;
		}
		const FDateTime Today = FDateTime(LastSweepUtc.GetYear(), LastSweepUtc.GetMonth(), LastSweepUtc.GetDay());
		const FDateTime Oldest = Today - FTimespan::FromDays(Options.RetentionDays);
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(TargetFolder / TEXT("audit-*.jsonl")), /*Files*/ true, /*Directories*/ false);
		for (const FString& Name : Names)
		{
			FDateTime Day;
			// Older than retention.audit_days by the day in its name (the day its records were written); never today's.
			if (DreamAuditLocal::ParseDay(Name, Day) && Day < Oldest && Day < Today)
			{
				IFileManager::Get().Delete(*(TargetFolder / Name), /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
			}
		}
	}
};

// ---- FDreamAudit -----------------------------------------------------------------------------------------------------

FDreamAudit::FDreamAudit()
	: FDreamAudit(FOptions())
{
}

FDreamAudit::FDreamAudit(FOptions InOptions)
	: Options(MoveTemp(InOptions))
	, SessionId(FGuid::NewGuid())
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor"));
	PluginVersion = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString(TEXT("unknown"));
	UserName = FPlatformProcess::UserName(/*bOnlyAlphaNumeric*/ false);
}

FDreamAudit::~FDreamAudit()
{
	if (bStarted)
	{
		Shutdown();
	}
}

FDateTime FDreamAudit::Now() const
{
	return Options.UtcNow ? Options.UtcNow() : FDateTime::UtcNow();
}

FString FDreamAudit::GetSessionPrefix() const
{
	return SessionId.ToString(EGuidFormats::Digits).ToLower().Left(8);
}

void FDreamAudit::Start(const FString& Root, const FString& InProjectHash)
{
	check(IsInGameThread());
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	ProjectHash = InProjectHash.IsEmpty() ? FString(FApp::GetProjectName()) : InProjectHash;
	Writer = MakeShared<FWriter, ESPMode::ThreadSafe>(Options);
	{
		using dream::json::Value;
		Value Tail = Value::MakeObject();
		Tail.Set("session", DreamAuditLocal::Str(SessionId.ToString(EGuidFormats::DigitsWithHyphensLower)));
		Tail.Set("user", DreamAuditLocal::Str(UserName));
		Tail.Set("project", DreamAuditLocal::Str(ProjectHash));
		Tail.Set("plugin", DreamAuditLocal::Str(PluginVersion));
		std::string Members = dream::json::Serialize(Tail);      // {"session":...}
		Members.front() = ',';
		Members.pop_back();
		Writer->GapPrefix = MoveTemp(Members);
	}
	if (Root.IsEmpty())
	{
		Writer->Fail(TEXT("no storage folder could be resolved"));
	}
	else
	{
		Writer->Folder = FPaths::ConvertRelativePathToFull(Root / TEXT("audit"));
	}
	Writer->Role = PendingRole;
	Writer->SecondaryId = PendingSecondaryId;
	Thread = FRunnableThread::Create(Writer.Get(), TEXT("DreamAudit"), 0, TPri_BelowNormal);
	// One banner per session for an unwritable folder (§11.4); the writer reports it, the game thread shows it.
	FailureTicker = FDreamDeferral::AddTicker(TEXT("DreamAuditFailure"), EDreamDeferPolicy::DeferWhileExecuting, 0.5f, false,
		[this](float)
		{
			PollFailure();
			return true;
		});
	PollFailure();
}

void FDreamAudit::SetRole(EDreamAuditRole Role, const FString& SecondaryId)
{
	check(IsInGameThread());
	PendingRole = Role;
	PendingSecondaryId = SecondaryId.IsEmpty() ? MakeSecondaryId() : SecondaryId;
	if (!Writer.IsValid())
	{
		return;     // Start applies it (a tab restored with the layout can spawn before OnFEngineLoopInitComplete)
	}
	{
		FScopeLock Guard(&Writer->Lock);
		Writer->Role = PendingRole;
		Writer->SecondaryId = PendingSecondaryId;
	}
	Writer->Wake->Trigger();
}

void FDreamAudit::Shutdown()
{
	if (!bStarted)
	{
		return;
	}
	bStarted = false;
	if (FailureTicker.IsValid())
	{
		FDreamDeferral::RemoveTicker(FailureTicker);
		FailureTicker.Reset();
	}
	if (!Writer.IsValid())
	{
		return;
	}
	Writer->Stop();
	const double Deadline = FPlatformTime::Seconds() + 1.0;
	while (!Writer->bExited.load() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.005f);
	}
	if (Writer->bExited.load() && Thread != nullptr)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
		Writer.Reset();
	}
	else
	{
		// Abandoned (the §10.2.5 pattern): the fence first, then the writer's code and state stay alive until exit.
		UE_LOG(LogDream, Warning, TEXT("The audit writer did not finish within 1 s; it is abandoned."));
		Writer->bFence.store(true);
		new TSharedPtr<FWriter, ESPMode::ThreadSafe>(Writer);    // deliberately leaked: the thread still runs its code
		Thread = nullptr;
		Writer.Reset();
	}
}

void FDreamAudit::Push(std::string&& Line)
{
	if (!Writer.IsValid())
	{
		return;     // before Start (no storage root yet) or after Shutdown: a record, not an enforcement point
	}
	FScopeLock Guard(&Writer->Lock);
	if (Writer->Pending.Num() >= Options.QueueCapacity)
	{
		++Writer->Dropped;      // §11.4 overflow: dropped and counted; the next flush writes audit.gap
		return;
	}
	FWriter::FItem& Item = Writer->Pending.AddDefaulted_GetRef();
	Item.Line = MoveTemp(Line);
}

namespace DreamAuditLocal
{
	/** The members every record starts with: v, ts, event, session (§11.4 schema v1). */
	Value Head(const char* EventName, const FDateTime& NowUtc, const FGuid& Session)
	{
		Value Record = Value::MakeObject();
		Record.Set("v", Value::MakeInt(1));
		Record.Set("ts", Iso(NowUtc));
		Record.Set("event", Value::MakeString(EventName));
		Record.Set("session", Str(Session.ToString(EGuidFormats::DigitsWithHyphensLower)));
		return Record;
	}

	void Tail(Value& Record, const FString& User, const FString& Project, const FString& Plugin)
	{
		Record.Set("user", Str(User));
		Record.Set("project", Str(Project));
		Record.Set("plugin", Str(Plugin));
	}

	/** run.start / run.end of a run of this session. Returns whether source.sha256 still needs the buffer hash. */
	bool RunFields(Value& Record, const FDreamRunRecord& Run, bool bEnd)
	{
		Record.Set("run", Value::MakeInt(Run.RunNumber));
		Record.Set("run_id", Run.RunIds.IsEmpty() ? Value::MakeNull() : Value::MakeUInt(Run.RunIds.Last()));
		Record.Set("route", Value::MakeString(DreamText::ToUtf8(FDreamAudit::RouteName(Run))));
		Record.Set("kind", Value::MakeString(DreamText::ToUtf8(FDreamAudit::KindName(Run))));
		Record.Set("badge", StrOrNull(Run.Badge));
		Record.Set("namespace", Value::MakeString(NamespaceKey(Run.Namespace)));
		Record.Set("transaction", Value::MakeString(Run.Txn.Status != EDreamTxnStatus::Off ? "on" : "off"));

		const bool bRunFile = DreamRunText::HasRunFileSemantics(Run.Route, Run.bFileTarget);
		// Run File saves a dirty buffer at Starting step 4: the run's source is the saved file.
		const bool bDirty = Run.bDirty && !bRunFile;
		bool bNeedsHash = false;
		Value Source = Value::MakeObject();
		Source.Set("path", StrOrNull(Run.FilePath));
		if (!bDirty && !Run.FileSha256.IsEmpty())
		{
			Source.Set("sha256", Str(Run.FileSha256));      // the file on disk when the buffer is clean
		}
		else if (Run.Route == EDreamRunRoute::Buffer || Run.Route == EDreamRunRoute::Repl || Run.Route == EDreamRunRoute::ConsoleExecutor
			|| Run.SourceSnapshot.Length() == 0)
		{
			Source.Set("sha256", Str(Run.SourceSha256));    // the run's text is the whole buffer (or prompt input)
		}
		else
		{
			Source.Set("sha256", Value::MakeString(PendingSha));    // the whole buffer text, hashed on the writer
			bNeedsHash = true;
		}
		Source.Set("dirty", Value::MakeBool(bDirty));
		Source.Set("trust", Value::MakeString("n/a"));     // Deferred (M4): FDreamTrust verdicts and the library block
		Record.Set("source", MoveTemp(Source));

		Value Code = Value::MakeObject();
		// code.sha256: the exact UTF-8 text the runner compiles; for Run File the saved file's bytes (T-AUD-002).
		Code.Set("sha256", Str(bRunFile && !Run.FileSha256.IsEmpty() ? Run.FileSha256 : Run.SourceSha256));
		Code.Set("bytes", Value::MakeInt(Run.CodeBytes));
		Code.Set("lines", Value::MakeInt(FMath::Max(Run.LastLine - Run.FirstLine + 1, 0)));
		Record.Set("code", MoveTemp(Code));

		Record.Set("start", Iso(Run.StartUtc));
		if (bEnd)
		{
			Record.Set("end", Iso(Run.EndUtc));
			const double From = Run.EnterSeconds > 0.0 ? Run.EnterSeconds : Run.StartSeconds;
			Record.Set("duration_ms", Value::MakeInt(static_cast<int64>(FMath::RoundToDouble(FMath::Max(Run.EndSeconds - From, 0.0) * 1000.0))));
			Record.Set("outcome", Value::MakeString(OutcomeKey(Run.Outcome)));
			Record.Set("error_type", ErrorType(Run));
		}
		return bNeedsHash;
	}
}

void FDreamAudit::RunStarted(const FDreamRunRecord& Run)
{
	check(IsInGameThread());
	using namespace DreamAuditLocal;
	if (!Writer.IsValid())
	{
		return;
	}
	Value Record = Head("run.start", Now(), SessionId);
	const bool bHash = RunFields(Record, Run, /*bEnd*/ false);
	Tail(Record, UserName, ProjectHash, PluginVersion);
	std::string Line = dream::json::Serialize(Record);
	FScopeLock Guard(&Writer->Lock);
	if (Writer->Pending.Num() >= Options.QueueCapacity)
	{
		++Writer->Dropped;
		return;
	}
	FWriter::FItem& Item = Writer->Pending.AddDefaulted_GetRef();
	Item.Line = MoveTemp(Line);
	Item.bHashSource = bHash;
	if (bHash)
	{
		Item.HashSource = Run.SourceSnapshot;
	}
}

void FDreamAudit::RunEnded(const FDreamRunRecord& Run)
{
	check(IsInGameThread());
	using namespace DreamAuditLocal;
	if (!Writer.IsValid())
	{
		return;
	}
	Value Record = Head("run.end", Now(), SessionId);
	const bool bHash = RunFields(Record, Run, /*bEnd*/ true);
	Tail(Record, UserName, ProjectHash, PluginVersion);
	std::string Line = dream::json::Serialize(Record);
	FScopeLock Guard(&Writer->Lock);
	if (Writer->Pending.Num() >= Options.QueueCapacity)
	{
		++Writer->Dropped;
		return;
	}
	FWriter::FItem& Item = Writer->Pending.AddDefaulted_GetRef();
	Item.Line = MoveTemp(Line);
	Item.bHashSource = bHash;
	if (bHash)
	{
		Item.HashSource = Run.SourceSnapshot;
	}
}

void FDreamAudit::SnapshotSkipped(const FDreamRunRecord& NewRun, int32 OriginalRunNumber)
{
	check(IsInGameThread());
	using namespace DreamAuditLocal;
	Value Record = Head("run.snapshot_skipped", Now(), SessionId);
	Record.Set("run", Value::MakeInt(NewRun.RunNumber));
	Record.Set("rerun_of", Value::MakeInt(OriginalRunNumber));
	Record.Set("route", Value::MakeString(DreamText::ToUtf8(RouteName(NewRun))));
	Record.Set("kind", Value::MakeString(DreamText::ToUtf8(KindName(NewRun))));
	Value Source = Value::MakeObject();
	Source.Set("path", StrOrNull(NewRun.FilePath));
	Record.Set("source", MoveTemp(Source));
	Tail(Record, UserName, ProjectHash, PluginVersion);
	Push(dream::json::Serialize(Record));
}

void FDreamAudit::RecordLostRun(const FDreamLostRun& Lost)
{
	check(IsInGameThread());
	using namespace DreamAuditLocal;
	// §10.7.4: a RUN_BEGIN without RUN_END in the recovered namespace becomes run.end with outcome lost.
	Value Record = Head("run.end", Now(), SessionId);
	Record.Set("run", Value::MakeInt(Lost.RunNumber));
	Record.Set("run_id", Lost.RunId != 0 ? Value::MakeUInt(Lost.RunId) : Value::MakeNull());
	Record.Set("previous_session", Value::MakeBool(true));
	Record.Set("route", Value::MakeString(DreamText::ToUtf8(RouteName(Lost.Route))));
	Record.Set("kind", Value::MakeString(DreamText::ToUtf8(KindName(Lost.Route, false, false))));
	Record.Set("badge", Value::MakeNull());
	Record.Set("namespace", Value::MakeNull());
	Record.Set("transaction", Value::MakeNull());
	Record.Set("source", Value::MakeNull());
	Record.Set("code", Value::MakeNull());
	Record.Set("start", Iso(Lost.StartUtc));
	Record.Set("end", Iso(Lost.PreviousSessionUtc));
	Record.Set("duration_ms", Value::MakeNull());
	Record.Set("outcome", Value::MakeString("lost"));
	Record.Set("error_type", Value::MakeNull());
	Record.Set("paused", Value::MakeBool(Lost.bPaused));
	Tail(Record, UserName, ProjectHash, PluginVersion);
	Push(dream::json::Serialize(Record));
}

void FDreamAudit::RecordLostRuns(TConstArrayView<FDreamLostRun> Lost)
{
	for (const FDreamLostRun& Run : Lost)
	{
		RecordLostRun(Run);
	}
}

void FDreamAudit::Event(const TCHAR* Type, const dream::json::Value& Fields)
{
	check(IsInGameThread());
	using namespace DreamAuditLocal;
	const std::string TypeUtf8 = DreamText::ToUtf8(Type);
	Value Record = Head(TypeUtf8.c_str(), Now(), SessionId);
	if (Fields.IsObject())
	{
		for (const Value::Member& Member : Fields.Members())
		{
			if (Member.Key != "v" && Member.Key != "ts" && Member.Key != "event" && Member.Key != "session")
			{
				Record.Set(Member.Key, Member.Val);
			}
		}
	}
	Tail(Record, UserName, ProjectHash, PluginVersion);
	Push(dream::json::Serialize(Record));
}

bool FDreamAudit::HasWriteFailed() const
{
	return Writer.IsValid() && Writer->bFailed.load();
}

FString FDreamAudit::GetFolder() const
{
	if (!Writer.IsValid())
	{
		return FString();
	}
	FScopeLock Guard(&Writer->Lock);
	return Writer->FolderOverride.IsEmpty() ? Writer->Folder : Writer->FolderOverride;
}

void FDreamAudit::PollFailure()
{
	if (!Writer.IsValid() || BannerCount > 0 || !Writer->bFailed.load())
	{
		return;
	}
	FString Reason;
	{
		FScopeLock Guard(&Writer->Lock);
		Reason = Writer->FailureReason;
	}
	ShowBanner(Reason);
}

void FDreamAudit::ShowBanner(const FString& Reason)
{
	// §11.4: an unwritable folder means one banner (and, from M4, a doctor item); runs continue.
	++BannerCount;
	const FString Folder = GetFolder();
	const FText Text = FText::Format(LOCTEXT("Audit.Unwritable", "Dream Python Editor cannot write its audit log{0}: {1}. Runs continue without audit records."),
		Folder.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("Audit.UnwritableIn", " in {0}"), FText::FromString(Folder)),
		FText::FromString(Reason));
	if (!Options.bShowBanner)
	{
		UE_LOG(LogDream, Verbose, TEXT("%s"), *Text.ToString());
		return;
	}
	if (FDreamEditorModule::IsAvailable())
	{
		// Run 0's warning line reaches LogDream through the forwarder (A46) and stays in the output.
		FDreamEditorModule::Get().GetOutputStore().PushDreamLine(0, EDreamStream::Warning, Text.ToString());
	}
	if (FSlateApplication::IsInitialized() && !FApp::IsUnattended() && !IsRunningCommandlet())
	{
		// (run group choice) The banner is a notification: the tab's inline banner host belongs to SDreamTab.
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 12.f;
		Info.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

// ---- Vocabulary ----------------------------------------------------------------------------------------------------

const TCHAR* FDreamAudit::RouteName(EDreamRunRoute Route)
{
	switch (Route)
	{
	case EDreamRunRoute::ConsoleExecutor:
		return TEXT("console");
	case EDreamRunRoute::Shelf:
	case EDreamRunRoute::Snippet:
		return TEXT("shelf");
	case EDreamRunRoute::Api:
		return TEXT("api");
	case EDreamRunRoute::Bridge:
		return TEXT("bridge");
	case EDreamRunRoute::Mcp:
		return TEXT("agent");
	default:
		return TEXT("user");
	}
}

const TCHAR* FDreamAudit::RouteName(const FDreamRunRecord& Run)
{
	return RouteName(Run.Route);
}

const TCHAR* FDreamAudit::KindName(EDreamRunRoute Route, bool bDebug, bool bFileTarget)
{
	if (bDebug)
	{
		return TEXT("debug");
	}
	switch (Route)
	{
	case EDreamRunRoute::Selection:
		return TEXT("selection");
	case EDreamRunRoute::Line:
		return TEXT("line");
	case EDreamRunRoute::Cell:
		return TEXT("cell");
	case EDreamRunRoute::Repl:
	case EDreamRunRoute::ConsoleExecutor:
		return TEXT("repl");
	case EDreamRunRoute::RunFile:
	case EDreamRunRoute::EntryPoint:
		return TEXT("file");
	case EDreamRunRoute::Shelf:
	case EDreamRunRoute::Snippet:
		return TEXT("snippet");
	case EDreamRunRoute::Reload:
	case EDreamRunRoute::ReloadOnSave:
	case EDreamRunRoute::Revert:
		return TEXT("reload");
	case EDreamRunRoute::TaskStep:
		return TEXT("task");
	case EDreamRunRoute::Api:
		return bFileTarget ? TEXT("file") : TEXT("buffer");
	default:
		return TEXT("buffer");
	}
}

const TCHAR* FDreamAudit::KindName(const FDreamRunRecord& Run)
{
	return KindName(Run.Route, /*bDebug*/ false, Run.bFileTarget);
}

FString FDreamAudit::MakeSecondaryId()
{
	const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
	int64 StartMs = 0;
#if PLATFORM_WINDOWS
	FILETIME Creation;
	FILETIME ExitTime;
	FILETIME Kernel;
	FILETIME User;
	if (::GetProcessTimes(::GetCurrentProcess(), &Creation, &ExitTime, &Kernel, &User))
	{
		// FILETIME: 100 ns since 1601-01-01; the id uses UTC milliseconds since 1970 (§10.5 process_start).
		const uint64 Ticks = (static_cast<uint64>(Creation.dwHighDateTime) << 32) | Creation.dwLowDateTime;
		StartMs = static_cast<int64>(Ticks / 10000ull) - 11644473600000ll;
	}
#endif
	if (StartMs <= 0)
	{
		static const int64 ModuleLoadMs = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond;
		StartMs = ModuleLoadMs;
	}
	return FString::Printf(TEXT("i-%u-%lld"), Pid, StartMs);
}

// ---- Test hooks -----------------------------------------------------------------------------------------------------

bool FDreamAudit::FlushForTest()
{
	if (!Writer.IsValid())
	{
		return false;
	}
	const uint64 Before = Writer->FlushesDone.load();
	{
		FScopeLock Guard(&Writer->Lock);
		Writer->bForceNext = true;
	}
	Writer->Wake->Trigger();
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	// Two flushes: one may already be past its snapshot of the queue when the request arrives.
	while (Writer->FlushesDone.load() < Before + 2 && FPlatformTime::Seconds() < Deadline)
	{
		Writer->Wake->Trigger();
		FPlatformProcess::Sleep(0.002f);
		FScopeLock Guard(&Writer->Lock);
		Writer->bForceNext = true;
	}
	return Writer->FlushesDone.load() >= Before + 2;
}

void FDreamAudit::SetFolderOverrideForTest(const FString& Folder)
{
	if (!Writer.IsValid())
	{
		return;
	}
	FScopeLock Guard(&Writer->Lock);
	Writer->FolderOverride = Folder;
	if (Folder.IsEmpty())
	{
		Writer->bFailed.store(false);
		Writer->FailureReason.Reset();
	}
}

void FDreamAudit::SweepNowForTest()
{
	if (!Writer.IsValid())
	{
		return;
	}
	{
		FScopeLock Guard(&Writer->Lock);
		Writer->bSweepNext = true;
	}
	FlushForTest();
}

TArray<FString> FDreamAudit::GetWrittenFilesForTest() const
{
	if (!Writer.IsValid())
	{
		return {};
	}
	FScopeLock Guard(&Writer->Lock);
	return Writer->WrittenFiles;
}

uint64 FDreamAudit::GetDroppedForTest() const
{
	if (!Writer.IsValid())
	{
		return 0;
	}
	FScopeLock Guard(&Writer->Lock);
	return Writer->Dropped;
}

#undef LOCTEXT_NAMESPACE
