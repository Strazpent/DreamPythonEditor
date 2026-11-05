// SPDX-License-Identifier: Apache-2.0
// The restore guard (TDD §10.8): two small JSON writes on the game thread at the spawn path's budget (§10.3), a 4 Hz
// FDreamDeferral ticker only between the restore and the step-3 clear.
#include "Persistence/DreamRestoreGuard.h"

#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamWin32FileOps.h"
#include "Run/DreamDeferral.h"
#include "dream/Json.h"

namespace DreamRestoreGuardLocal
{
	bool ParseGuard(const std::string& Bytes, dream::json::Value& Out)
	{
		return dream::json::Parse(Bytes, Out) && Out.IsObject();
	}
}

FDreamRestoreGuard::FDreamRestoreGuard(TFunction<bool()> InIsImportSettled)
	: IsImportSettled(MoveTemp(InIsImportSettled))
{
}

FDreamRestoreGuard::~FDreamRestoreGuard()
{
	if (Ticker.IsValid())
	{
		FDreamDeferral::RemoveTicker(Ticker);
		Ticker.Reset();
	}
}

FString FDreamRestoreGuard::GuardPath(const FString& StateDir)
{
	return StateDir / TEXT("restore-guard.json");
}

bool FDreamRestoreGuard::IsAutomationSession()
{
	FString Commands;
	return GIsAutomationTesting || (FParse::Value(FCommandLine::Get(), TEXT("ExecCmds="), Commands, false) && Commands.Contains(TEXT("Automation")));
}

bool FDreamRestoreGuard::ReadPending(const FString& InPath, int32& OutPending, bool& bOutNewerSchema)
{
	OutPending = 0;
	bOutNewerSchema = false;
	// Game thread (review r2): the FileOps sharing-violation retries run without their waits here, never a 0.9 s stall.
	const FDreamWin32FileOps::FScopedNoRetryWait NoWaits;
	FDreamWin32FileOps Ops;
	for (const FString& Candidate : { InPath, InPath + TEXT(".bak") })
	{
		std::string Bytes;
		dream::json::Value Root;
		if (!Ops.ReadAll(FDreamWin32FileOps::ToUtf8Path(Candidate), Bytes) || !DreamRestoreGuardLocal::ParseGuard(Bytes, Root))
		{
			continue;   // torn or missing: the .bak, then the default (§10.2.3 small files)
		}
		bOutNewerSchema = Root.Get("schema").AsInt(Schema) > Schema;
		OutPending = static_cast<int32>(FMath::Clamp<int64>(Root.Get("pending").AsInt(0), 0, 1000));
		return true;
	}
	return false;
}

bool FDreamRestoreGuard::WritePending(const FString& InPath, int32 InPending)
{
	const FDreamWin32FileOps::FScopedNoRetryWait NoWaits;   // game thread (review r2)
	FDreamWin32FileOps Ops;
	const std::string Target = FDreamWin32FileOps::ToUtf8Path(InPath);
	dream::json::Value Root = dream::json::Value::MakeObject();
	{
		std::string Bytes;
		dream::json::Value Existing;
		if (Ops.ReadAll(Target, Bytes) && DreamRestoreGuardLocal::ParseGuard(Bytes, Existing))
		{
			Root = Existing;   // writers keep unknown keys (§10.2.3)
		}
	}
	Root.Set("schema", dream::json::Value::MakeInt(Schema));
	Root.Set("pending", dream::json::Value::MakeInt(InPending));
	const std::string Json = dream::json::Serialize(Root, true) + "\n";
	const std::string Temp = Target + ".tmp";
	Ops.MakeDirs(FDreamWin32FileOps::ToUtf8Path(FPaths::GetPath(InPath)));
	if (Ops.Exists(Temp))
	{
		Ops.Remove(Temp);
	}
	dream::journal::FileOps::Handle H = dream::journal::FileOps::InvalidHandle;
	if (!Ops.CreateExclusive(Temp, H))
	{
		UE_LOG(LogDream, Warning, TEXT("The restore guard could not be written to %s: %s"), *InPath, *DreamText::Utf8ToFString(Ops.LastError()));
		return false;
	}
	const bool bWritten = Ops.Append(H, Json) && Ops.Flush(H);
	Ops.Close(H);
	if (!bWritten || !Ops.ReplaceAtomic(Target, Temp, Target + ".bak"))
	{
		UE_LOG(LogDream, Warning, TEXT("The restore guard could not be written to %s: %s"), *InPath, *DreamText::Utf8ToFString(Ops.LastError()));
		Ops.Remove(Temp);
		return false;
	}
	return true;
}

bool FDreamRestoreGuard::BeginRestore(const FString& StateDir, bool bAutomationSession)
{
	Path = GuardPath(StateDir);
	bRestoreApplied = false;
	bCleared = false;
	SettledAt = -1.0;
	bool bNewerSchema = false;
	ReadPending(Path, Pending, bNewerSchema);
	bReadOnly = bNewerSchema;
	if (Pending >= SafeModeThreshold)
	{
		if (!bAutomationSession)
		{
			UE_LOG(LogDream, Warning, TEXT("The last %d starts ended during the session restore; Dream Python Editor opens in safe mode (%s)."), Pending, *Path);
			return true;
		}
		// (slice B choice) An automation pass ends with a forced Quit, never a clean shutdown: it never enters safe mode by
		// the guard, and the count restarts so it cannot grow across passes.
		UE_LOG(LogDream, Log, TEXT("The restore guard reads %d in an automation session; safe mode is not entered and the count restarts."), Pending);
		Pending = 0;
	}
	// Step 2: flushed before any restore work.
	if (!bReadOnly && WritePending(Path, Pending + 1))
	{
		Pending += 1;
	}
	return false;
}

void FDreamRestoreGuard::OnRestoreApplied()
{
	if (Path.IsEmpty() || bCleared)
	{
		return;
	}
	bRestoreApplied = true;
	if (!Ticker.IsValid())
	{
		Ticker = FDreamDeferral::AddTicker(TEXT("DreamRestoreGuard"), EDreamDeferPolicy::DeferWhileExecuting, 0.25f, false,
			[this](float) { return Tick(); });
	}
}

bool FDreamRestoreGuard::Tick()
{
	if (bCleared || !bRestoreApplied)
	{
		Ticker.Reset();
		return false;
	}
	const double Now = FPlatformTime::Seconds();
	if (SettledAt < 0.0)
	{
		if (IsImportSettled && !IsImportSettled())
		{
			return true;   // the deferred import has not completed yet
		}
		SettledAt = Now;
	}
	if (Now - SettledAt < ClearDelay)
	{
		return true;
	}
	Ticker.Reset();
	Clear();
	return false;
}

void FDreamRestoreGuard::Clear()
{
	if (Path.IsEmpty() || bCleared)
	{
		return;
	}
	bCleared = true;
	if (Ticker.IsValid())
	{
		FDreamDeferral::RemoveTicker(Ticker);
		Ticker.Reset();
	}
	if (!bReadOnly && Pending != 0 && WritePending(Path, 0))
	{
		Pending = 0;
	}
}

void FDreamRestoreGuard::EnterSafeMode(EDreamSafeModeReason InReason)
{
	if (InReason == EDreamSafeModeReason::None || Reason == InReason)
	{
		return;
	}
	Reason = InReason;
	SafeModeChanged.Broadcast();
}

void FDreamRestoreGuard::LeaveSafeMode()
{
	const bool bWasSafe = IsSafeMode();
	Reason = EDreamSafeModeReason::None;
	// §10.8 Leaving: pending = 0; the skipped restore is applied by the tab, the deferred services start from the event.
	if (!Path.IsEmpty())
	{
		bCleared = false;
		Clear();
	}
	if (bWasSafe)
	{
		SafeModeChanged.Broadcast();
	}
}
