// SPDX-License-Identifier: Apache-2.0
// Catches Python output that bypasses Dream's writers (TDD §6.3, D19): pre-bound unreal.log, stale stock streams, engine
// tracebacks, foreign LogPython while idle. Registered with GLog->AddOutputDevice in StartupModule; touches no Python.
// Serialize runs synchronously on whatever thread logs (CanBeUsedOnMultipleThreads true), takes no lock other than the
// store's lock-free push, never logs, and costs <= 100 ns for a filtered-out line. Body: DreamOutputDevice.cpp (output slice).
#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "Output/DreamOutputTypes.h"

class FDreamOutputStore;

class DREAMEDITOR_API FDreamOutputDevice final : public FOutputDevice
{
public:
	static constexpr int32 StartupRingCapacity = 2000;   // proposed, §6.3

	explicit FDreamOutputDevice(FDreamOutputStore& InStore);
	/** Out of line: TUniquePtr<FImpl> needs FImpl complete where the (exported) destructor is defined. */
	virtual ~FDreamOutputDevice() override;

	/** 1. return if GDreamForwarding; 2. return unless Category is LogPython, or a RunLogCategories category while a run is
	 *  active; 3. push a record (origin LogPython, type from verbosity, run ID 0, native thread ID only). */
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
	virtual bool CanBeUsedOnAnyThread() const override { return true; }

	/** Settings: UDreamUserSettings::RunLogCategories (default empty, OUT-01). Game thread; published atomically. */
	void SetExtraRunCategories(const TArray<FName>& Categories);
	/** Set by FDreamRunService at entry and finish (atomic), for the extra categories rule. */
	void SetRunActive(bool bActive);

	/**
	 * The first 2,000 LogPython lines of the session (§6.3 startup ring). (slice A choice) The DreamOutput thread fills it from
	 * device records, so Serialize stays lock-free; this copy takes a short lock that Serialize never takes. Any thread.
	 */
	TArray<FDreamStartupLine> CopyStartupRing() const;
	/** Called by the ingest thread for each device record while the ring has room. */
	void AppendStartupLine(FDreamStartupLine&& Line);

private:
	FDreamOutputStore& Store;
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
