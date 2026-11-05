// SPDX-License-Identifier: Apache-2.0
// FDreamOutputDevice (TDD §6.3 "Design: FDreamOutputDevice", D19, RT-07). See DreamOutputDevice.h.
// Serialize runs on whatever thread logs: no lock, no log, no Python, no thread table (attribution is the ingest
// thread's, §6.1); a filtered-out line costs a TLS read and one or two FName compares (T-OUT-022).
#include "Output/DreamOutputDevice.h"

#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Output/DreamLogForwarder.h"
#include "Output/DreamOutputStore.h"

#include <atomic>

struct FDreamOutputDevice::FImpl
{
	FName LogPythonName;
	FName LogDreamName;
	/** Published on the game thread; Serialize reads the pointer only. Old arrays stay alive until the device dies,
	 *  because a Serialize on another thread may still be reading one. */
	std::atomic<const TArray<FName>*> ExtraCategories{nullptr};
	TArray<TUniquePtr<TArray<FName>>> Published;
	std::atomic<bool> bRunActive{false};

	mutable FCriticalSection StartupLock;   // never taken by Serialize (A31)
	TArray<FDreamStartupLine> StartupRing;
};

FDreamOutputDevice::FDreamOutputDevice(FDreamOutputStore& InStore)
	: Store(InStore)
	, Impl(MakeUnique<FImpl>())
{
	Impl->LogPythonName = FDreamLogForwarder::LogPythonName();
	Impl->LogDreamName = FDreamLogForwarder::LogDreamName();
	Impl->StartupRing.Reserve(StartupRingCapacity);
	Store.SetOutputDevice(this);
}

FDreamOutputDevice::~FDreamOutputDevice()
{
	Store.SetOutputDevice(nullptr);
}

void FDreamOutputDevice::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	// 1. Our own echo (FDreamLogForwarder sets the guard on its thread): never captured twice (T-OUT-021).
	if (GDreamForwarding || V == nullptr)
	{
		return;
	}

	// 2. LogPython always; the RunLogCategories (OUT-01) only while a run is active; LogDream never.
	bool bExtraCategory = false;
	if (Category != Impl->LogPythonName)
	{
		if (!Impl->bRunActive.load(std::memory_order_relaxed) || Category == Impl->LogDreamName)
		{
			return;
		}
		const TArray<FName>* Extra = Impl->ExtraCategories.load(std::memory_order_acquire);
		if (Extra == nullptr || !Extra->Contains(Category))
		{
			return;
		}
		bExtraCategory = true;
	}

	const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
	if (Level == ELogVerbosity::NoLogging || Level == ELogVerbosity::SetColor)
	{
		return;
	}

	// 3. One record: origin LogPython, type from verbosity, run ID 0 and only the native thread ID (§6.3 step 3).
	FDreamRawRecord Record;
	Record.Kind = EDreamRawKind::Text;
	Record.Origin = EDreamOutputOrigin::LogPython;
	Record.Stream = Level <= ELogVerbosity::Error ? EDreamStream::Error
		: Level == ELogVerbosity::Warning ? EDreamStream::Warning
		: EDreamStream::Info;
	Record.Flags = dream::OLF_FromLogPython;
	Record.NativeThreadId = FPlatformTLS::GetCurrentThreadId();
	Record.Time = FPlatformTime::Seconds();
	if (bExtraCategory)
	{
		// The ring has no category field: an extra category names itself in the text, as the Output Log does.
		const FString Prefixed = Category.ToString() + TEXT(": ") + V;
		const FTCHARToUTF8 Utf8(*Prefixed);
		Record.Text.Append(reinterpret_cast<const UTF8CHAR*>(Utf8.Get()), Utf8.Length());
	}
	else
	{
		const FTCHARToUTF8 Utf8(V);
		Record.Text.Append(reinterpret_cast<const UTF8CHAR*>(Utf8.Get()), Utf8.Length());
	}
	Store.Push(MoveTemp(Record));
}

void FDreamOutputDevice::SetExtraRunCategories(const TArray<FName>& Categories)
{
	check(IsInGameThread());
	const TArray<FName>* Current = Impl->ExtraCategories.load(std::memory_order_relaxed);
	if (Current != nullptr ? *Current == Categories : Categories.IsEmpty())
	{
		return;
	}
	TUniquePtr<TArray<FName>> Next = MakeUnique<TArray<FName>>(Categories);
	Impl->ExtraCategories.store(Next.Get(), std::memory_order_release);
	Impl->Published.Add(MoveTemp(Next));
}

void FDreamOutputDevice::SetRunActive(bool bActive)
{
	Impl->bRunActive.store(bActive, std::memory_order_relaxed);
}

TArray<FDreamStartupLine> FDreamOutputDevice::CopyStartupRing() const
{
	FScopeLock Guard(&Impl->StartupLock);
	return Impl->StartupRing;
}

void FDreamOutputDevice::AppendStartupLine(FDreamStartupLine&& Line)
{
	FScopeLock Guard(&Impl->StartupLock);
	if (Impl->StartupRing.Num() < StartupRingCapacity)
	{
		Impl->StartupRing.Add(MoveTemp(Line));
	}
}
