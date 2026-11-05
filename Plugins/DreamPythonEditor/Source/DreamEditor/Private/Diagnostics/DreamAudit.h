// SPDX-License-Identifier: Apache-2.0
// FDreamAudit: the local JSONL record of runs (TDD §11.4 M1, SEC-05, D20, §10.5 secondary file names, §10.7.4 lost runs).
// A record, not an enforcement point: a write failure never blocks a run; an unwritable folder shows one banner.
// Game thread API (each record is built on the GT, ≤ 0.05 ms proposed, T-AUD-005); the files are written by the
// DreamAudit FRunnable, which flushes every 250 ms, rolls at 16 MB and at the UTC day, and (primary only) sweeps files
// older than retention.audit_days (30 days built in; the policy keys are M4). Body: DreamAudit.cpp (run group).
//
// Until the instance role is known (decided at the first tab spawn by FDreamJournal::Open, A39) lines wait in the queue:
// the primary writes audit-YYYY-MM-DD.jsonl, a secondary audit-YYYY-MM-DD.<sec-id>.jsonl (<sec-id> = i-<pid>-<start>).
// A session that never opens the tab writes its lines under the secondary-style name at shutdown, or once half the
// queue is used, because only a primary may write the shared name (B-note).
//
// (run group choice) Event fields are a dream::json::Value (dream/Json.h, A6), not an FJsonObject: DreamEditor has no Json
// module dependency. Instances: a test may construct its own FDreamAudit on a private folder; nothing here is static.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Run/DreamRunTypes.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

#include <string>

namespace dream::json
{
	class Value;
}

/** Who writes the shared files under <root> (§10.5). Unknown until the first tab spawn decides it. */
enum class EDreamAuditRole : uint8
{
	Unknown,
	Primary,
	Secondary,
};

class DREAMEDITOR_API FDreamAudit
{
public:
	struct FOptions
	{
		int64 RollBytes = 16ll * 1024 * 1024;       // §11.4: roll to -2, -3 at 16 MB (proposed)
		int32 RetentionDays = 30;                   // retention.audit_days built-in value (§11.2.2, proposed)
		int32 QueueCapacity = 10000;                // §11.4 bounded queue (proposed)
		double FlushSeconds = 0.25;                 // §11.4 I/O-thread flush period
		/** The clock for file names, timestamps and the sweep; FDateTime::UtcNow when unset (tests inject a day). */
		TFunction<FDateTime()> UtcNow;
		/** Show the unwritable-folder banner (a Slate notification); off for private test instances. */
		bool bShowBanner = true;
	};

	FDreamAudit();
	explicit FDreamAudit(FOptions InOptions);
	~FDreamAudit();

	/**
	 * OnFEngineLoopInitComplete (threads are allowed from there, B1): starts the writer on <root>/audit. An empty Root
	 * (no storage root could be resolved) means an unwritable folder: one banner, records are dropped. Game thread.
	 */
	void Start(const FString& Root, const FString& ProjectHash);
	/** The role decided at the first tab spawn (A39, §10.5); a layout-restored tab may spawn before Start, which then
	 *  applies it. SecondaryId is "i-<pid>-<process_start>". Game thread. */
	void SetRole(EDreamAuditRole Role, const FString& SecondaryId);
	/** OnPreExit / ShutdownModule: flush what is queued (an unknown role writes the secondary-style name), wait at most
	 *  1 s, then stop the writer; one that does not answer is abandoned behind its fence (§10.2.5 pattern). */
	void Shutdown();

	// ---- Records (game thread; never block; IsInGameThread asserted) ----

	/** run.start at Running, after the trust, parse, snapshot and save steps (§5.2.3, §11.4). */
	void RunStarted(const FDreamRunRecord& Run);
	/** run.end at finish, not executed runs included (§5.2.3, T-RUN-024). */
	void RunEnded(const FDreamRunRecord& Run);
	/** run.snapshot_skipped: [Run without snapshot] accepted NewRun as a re-run of OriginalRunNumber (§10.2.5, PER-09). */
	void SnapshotSkipped(const FDreamRunRecord& NewRun, int32 OriginalRunNumber);
	/** run.end with outcome lost for a run the previous session left open (§10.7.4). */
	void RecordLostRun(const FDreamLostRun& Lost);
	void RecordLostRuns(TConstArrayView<FDreamLostRun> Lost);
	/** Any other event: Type ("debug.server_started" M2, "trust.*" and "policy.*" M4) and its extra fields (an object). */
	void Event(const TCHAR* Type, const dream::json::Value& Fields);

	// ---- State ----

	/** One per editor session; the "session" field and the Dream.Run crash key's prefix (§11.8.1). */
	const FGuid& GetSessionId() const { return SessionId; }
	/** The first 8 lower-case hex digits of the session id. */
	FString GetSessionPrefix() const;
	/** True once the writer reported that the folder cannot be written (the banner was shown once). Any thread. */
	bool HasWriteFailed() const;
	/** "audit" folder in use, or empty before Start. */
	FString GetFolder() const;

	// ---- Shared vocabulary (§11.4 table), also used by FDreamCrashContext ----

	/** user, shelf, menu, api, console, bridge, agent, headless. */
	static const TCHAR* RouteName(const FDreamRunRecord& Run);
	static const TCHAR* RouteName(EDreamRunRoute Route);
	/** selection, line, cell, buffer, repl, file, snippet, reload, debug, task. */
	static const TCHAR* KindName(const FDreamRunRecord& Run);
	static const TCHAR* KindName(EDreamRunRoute Route, bool bDebug, bool bFileTarget);
	/** The secondary-instance id of this process: "i-<pid>-<process start, UTC ms>" (§10.5). */
	static FString MakeSecondaryId();

	// ---- Test hooks (Dream.Audit.*) ----

	/** Writes everything queued now (role Unknown: the secondary-style name) and waits for the writer, at most 5 s. */
	bool FlushForTest();
	/** Points the writer at another folder (Dream.Audit.Unwritable); empty restores the folder Start chose. */
	void SetFolderOverrideForTest(const FString& Folder);
	/** Runs the retention sweep now, as the primary would at startup and once a day. */
	void SweepNowForTest();
	/** The files the writer created or appended to, in order (absolute paths). */
	TArray<FString> GetWrittenFilesForTest() const;
	int32 GetBannerCountForTest() const { return BannerCount; }
	uint64 GetDroppedForTest() const;

private:
	struct FWriter;
	void Push(std::string&& Line);
	void PollFailure();
	void ShowBanner(const FString& Reason);
	FDateTime Now() const;

	FOptions Options;
	FGuid SessionId;
	FString ProjectHash;
	FString PluginVersion;
	FString UserName;
	TSharedPtr<FWriter, ESPMode::ThreadSafe> Writer;
	class FRunnableThread* Thread = nullptr;
	FTSTicker::FDelegateHandle FailureTicker;
	int32 BannerCount = 0;
	bool bStarted = false;
	EDreamAuditRole PendingRole = EDreamAuditRole::Unknown;     // SetRole before Start
	FString PendingSecondaryId;
};
