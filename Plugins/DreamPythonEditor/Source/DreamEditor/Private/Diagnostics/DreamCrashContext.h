// SPDX-License-Identifier: Apache-2.0
// FDreamCrashContext: Dream's crash breadcrumbs (TDD §11.8.1 M1, §10.9, D26, TEL-04). Every call writes
// FGenericCrashContext::SetGameData (GenericPlatformCrashContext.h:647), which updates an unlocked map
// (GenericPlatformCrashContext.cpp:1611-1633), so every call is made on the game thread and asserts IsInGameThread().
// Body: DreamCrashContext.cpp (run group).
//
// | Key              | Value                                                   | Set when                     |
// | Dream.Version     | the plugin version                                      | Initialize (StartupModule)   |
// | Dream.State       | the U2 state ("Idle", "Starting", "Running", ...)       | each transition              |
// | Dream.Run         | "<run number>/<session-id prefix>", e.g. "42/9b1e3c4d" | run start                    |
// | Dream.Route       | "<route>/<kind>" in the audit vocabulary, "user/line"   | run start                    |
// | Dream.TabHash     | the first 8 hex digits of SHA-256 over the tab ID       | run start (tab runs only)    |
// | Dream.InUserCode  | "1" from _run entry until _dream.user_code_done, then "0"| twice per run                |
// | Dream.LastPumpMs  | milliseconds since the run's entry at the last pump     | each pump, at most 1 Hz      |
// | Dream.Pause       | "none" (M1 has no pause; "paused", "evaluating" are M3) | Initialize                   |
// Run keys are removed at run end by writing an empty value (:1613-1623). InUserCode=1 with State=Running attributes a
// crash to user code (§11.8.1).
#pragma once

#include "CoreMinimal.h"
#include "Run/DreamRunTypes.h"

class DREAMEDITOR_API FDreamCrashContext
{
public:
	static constexpr double PumpKeyPeriodSeconds = 1.0;     // Dream.LastPumpMs at most 1 Hz (proposed)

	/** StartupModule: Dream.Version, Dream.State = Idle, Dream.Pause = none. SessionPrefix names this editor session. */
	void Initialize(const FString& PluginVersion, const FString& SessionPrefix);
	/** ShutdownModule: every Dream key removed. */
	void Shutdown();

	/** Dream.State on each U2 transition. */
	void OnStateChanged(EDreamRunState State);
	/** Running: Dream.Run, Dream.Route, Dream.TabHash, Dream.InUserCode = 1, Dream.LastPumpMs = 0. */
	void OnRunStarted(const FDreamRunRecord& Run);
	/** user_code_begin / user_code_done (a second window reopens it). */
	void OnUserCode(bool bInUserCode);
	/** Each pump: Dream.LastPumpMs, written at most once per PumpKeyPeriodSeconds. */
	void OnPump(double SecondsSinceEntry);
	/** Finish: the run keys are removed. */
	void OnRunEnded();

	/** The U2 state as the crash key writes it (English, never localized). */
	static const TCHAR* StateKey(EDreamRunState State);

	/** Every key this class writes (T-TEL-001, T-API-015). */
	static TConstArrayView<const TCHAR*> GetKeys();

private:
	void Set(const TCHAR* Key, const FString& Value);

	FString SessionPrefix;
	double LastPumpWrite = -1.0;
	bool bRunKeys = false;
};
