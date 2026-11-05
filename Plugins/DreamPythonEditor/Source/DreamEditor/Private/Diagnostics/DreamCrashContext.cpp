// SPDX-License-Identifier: Apache-2.0
// FDreamCrashContext (TDD §11.8.1, §10.9, D26, TEL-04). See DreamCrashContext.h.
#include "Diagnostics/DreamCrashContext.h"

#include "Diagnostics/DreamAudit.h"
#include "GenericPlatform/GenericPlatformCrashContext.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamText.h"
#include "dream/Checksum.h"

namespace DreamCrashContextLocal
{
	constexpr const TCHAR* Version = TEXT("Dream.Version");
	constexpr const TCHAR* State = TEXT("Dream.State");
	constexpr const TCHAR* Run = TEXT("Dream.Run");
	constexpr const TCHAR* Route = TEXT("Dream.Route");
	constexpr const TCHAR* TabHash = TEXT("Dream.TabHash");
	constexpr const TCHAR* InUserCode = TEXT("Dream.InUserCode");
	constexpr const TCHAR* LastPumpMs = TEXT("Dream.LastPumpMs");
	constexpr const TCHAR* Pause = TEXT("Dream.Pause");

	const TCHAR* const AllKeys[] = { Version, State, Run, Route, TabHash, InUserCode, LastPumpMs, Pause };

	/** The first 8 hex digits of SHA-256 over the tab ID (its 32 lower-case hex digits, the public API's tab ID). */
	FString HashTab(const FGuid& Tab)
	{
		const std::string Id = DreamText::ToUtf8(DreamText::GuidToHex(Tab));
		dream::Sha256 Hasher;
		Hasher.Update(Id.data(), Id.size());
		return DreamText::Utf8ToFString(dream::Sha256::ToHex(Hasher.Final())).Left(8);
	}
}

void FDreamCrashContext::Set(const TCHAR* Key, const FString& Value)
{
	// SetGameData updates an unlocked map: game thread only (§11.8.1).
	check(IsInGameThread());
	FGenericCrashContext::SetGameData(Key, Value);
}

void FDreamCrashContext::Initialize(const FString& PluginVersion, const FString& InSessionPrefix)
{
	using namespace DreamCrashContextLocal;
	SessionPrefix = InSessionPrefix;
	Set(Version, PluginVersion);
	Set(State, StateKey(EDreamRunState::Idle));
	// Deferred (M3): "paused" and "evaluating" with the Paused window (§8.3); M1 has no pause.
	Set(Pause, TEXT("none"));
}

void FDreamCrashContext::Shutdown()
{
	for (const TCHAR* Key : DreamCrashContextLocal::AllKeys)
	{
		Set(Key, FString());
	}
	bRunKeys = false;
}

const TCHAR* FDreamCrashContext::StateKey(EDreamRunState State)
{
	switch (State)
	{
	case EDreamRunState::Idle:
		return TEXT("Idle");
	case EDreamRunState::Starting:
		return TEXT("Starting");
	case EDreamRunState::Running:
		return TEXT("Running");
	case EDreamRunState::Stopping:
		return TEXT("Stopping");
	case EDreamRunState::Paused:
		return TEXT("Paused");
	case EDreamRunState::TaskRunning:
		return TEXT("Task running");
	}
	return TEXT("Idle");
}

void FDreamCrashContext::OnStateChanged(EDreamRunState NewState)
{
	Set(DreamCrashContextLocal::State, StateKey(NewState));
}

void FDreamCrashContext::OnRunStarted(const FDreamRunRecord& Rec)
{
	using namespace DreamCrashContextLocal;
	Set(Run, FString::Printf(TEXT("%d/%s"), Rec.RunNumber, *SessionPrefix));
	Set(Route, FString::Printf(TEXT("%s/%s"), FDreamAudit::RouteName(Rec), FDreamAudit::KindName(Rec)));
	Set(TabHash, Rec.BufferId.IsValid() ? HashTab(Rec.BufferId) : FString());
	Set(InUserCode, TEXT("1"));
	Set(LastPumpMs, TEXT("0"));
	LastPumpWrite = -1.0;
	bRunKeys = true;
}

void FDreamCrashContext::OnUserCode(bool bInUserCode)
{
	if (bRunKeys)
	{
		Set(DreamCrashContextLocal::InUserCode, bInUserCode ? TEXT("1") : TEXT("0"));
	}
}

void FDreamCrashContext::OnPump(double SecondsSinceEntry)
{
	if (!bRunKeys)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (LastPumpWrite >= 0.0 && Now - LastPumpWrite < PumpKeyPeriodSeconds)
	{
		return;
	}
	LastPumpWrite = Now;
	Set(DreamCrashContextLocal::LastPumpMs, FString::Printf(TEXT("%lld"), static_cast<int64>(SecondsSinceEntry * 1000.0)));
}

void FDreamCrashContext::OnRunEnded()
{
	using namespace DreamCrashContextLocal;
	// Run keys are removed at run end by writing an empty value (§11.8.1).
	for (const TCHAR* Key : { Run, Route, TabHash, InUserCode, LastPumpMs })
	{
		Set(Key, FString());
	}
	bRunKeys = false;
}

TConstArrayView<const TCHAR*> FDreamCrashContext::GetKeys()
{
	return MakeArrayView(DreamCrashContextLocal::AllKeys);
}
