// SPDX-License-Identifier: Apache-2.0
// The Stage 1 snapshot driver (TDD §7.4.2, §7.4.3). See DreamStubSnapshot.h.
#include "Language/DreamStubSnapshot.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Modules/ModuleManager.h"
#include "ProjectDescriptor.h"
#include "Python/DreamPython.h"
#include "Python/DreamPythonSnapshot.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Tasks/Pipe.h"
#include "Tasks/Task.h"
#include "dream/Checksum.h"
#include "dream/Json.h"

namespace DreamStubSnapshotLocal
{
	constexpr int32 FlushBytes = 1 << 20;       // buffered JSONL lines per append task
	constexpr const TCHAR* ProgressFile = TEXT("progress.json");
	constexpr const TCHAR* UsedMarker = TEXT(".used");

	bool WriteFileAtomically(const FString& Path, const TArray<uint8>& Bytes)
	{
		const FString Temp = Path + TEXT(".tmp");
		if (!FFileHelper::SaveArrayToFile(Bytes, *Temp))
		{
			return false;
		}
		return IFileManager::Get().Move(*Path, *Temp, /*Replace*/ true, /*EvenIfReadOnly*/ true);
	}

	TArray<uint8> ToBytes(const std::string& Text)
	{
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Text.data()), static_cast<int32>(Text.size()));
		return Bytes;
	}
}

/** The key task's inputs (gathered on the game thread) and outputs (written by the worker, then bDone). */
struct FDreamStubSnapshot::FKeyResult
{
	FString EngineVersion;
	FString PythonVersion;
	TArray<FString> Plugins;
	TArray<FString> ModuleFiles;
	FString StubsRoot;

	std::atomic<bool> bDone{ false };
	FString Key;
	FString SnapshotDir;
	bool bComplete = false;
	TArray<FString> DoneEntries;            // serialized manifest entries of modules an interrupted walk finished
};

/** State touched only by the pipe's tasks (one at a time), plus the flags the game thread polls. */
struct FDreamStubSnapshot::FWriter
{
	FString Dir;
	FString CurrentModule;
	TUniquePtr<FArchive> File;

	FCriticalSection Lock;                  // DoneEntries and Error (the game thread reads them)
	TArray<FString> DoneEntries;
	FString Error;
	std::atomic<bool> bFailed{ false };
	std::atomic<bool> bComplete{ false };

	void Fail(const FString& Why)
	{
		{
			FScopeLock Guard(&Lock);
			if (Error.IsEmpty())
			{
				Error = Why;
			}
		}
		bFailed = true;
	}

	FString ModulePath(const FString& Module, bool bTemp) const
	{
		return Dir / TEXT("modules") / (Module + (bTemp ? TEXT(".jsonl.tmp") : TEXT(".jsonl")));
	}

	void OpenModule(const FString& Module)
	{
		File.Reset();
		CurrentModule = Module;
		File.Reset(IFileManager::Get().CreateFileWriter(*ModulePath(Module, true)));     // truncates an interrupted one
		if (!File.IsValid())
		{
			Fail(FString::Printf(TEXT("cannot write %s"), *ModulePath(Module, true)));
		}
	}

	void Append(const FString& Module, TArray<uint8>&& Bytes)
	{
		if (bFailed)
		{
			return;
		}
		if (Module != CurrentModule || !File.IsValid())
		{
			OpenModule(Module);
		}
		if (File.IsValid() && Bytes.Num() > 0)
		{
			File->Serialize(Bytes.GetData(), Bytes.Num());
			if (File->IsError())
			{
				Fail(FString::Printf(TEXT("cannot write %s"), *ModulePath(Module, true)));
			}
		}
	}

	void FinishModule(const FString& Module, const FString& Entry)
	{
		if (bFailed)
		{
			return;
		}
		if (Module != CurrentModule || !File.IsValid())
		{
			OpenModule(Module);     // a module that produced no line before its end
		}
		// A short write (a full disk) must never be renamed into place and recorded as done.
		const bool bWritten = File.IsValid() && !File->IsError() && File->Close();
		File.Reset();
		CurrentModule.Reset();
		if (!bWritten)
		{
			Fail(FString::Printf(TEXT("cannot write %s"), *ModulePath(Module, true)));
			return;
		}
		if (!IFileManager::Get().Move(*ModulePath(Module, false), *ModulePath(Module, true), true, true))
		{
			Fail(FString::Printf(TEXT("cannot rename %s"), *ModulePath(Module, true)));
			return;
		}
		FString Progress;
		{
			FScopeLock Guard(&Lock);
			DoneEntries.Add(Entry);
			Progress = TEXT("[") + FString::Join(DoneEntries, TEXT(",")) + TEXT("]");
		}
		if (!DreamStubSnapshotLocal::WriteFileAtomically(Dir / DreamStubSnapshotLocal::ProgressFile,
			DreamStubSnapshotLocal::ToBytes(DreamText::ToUtf8(Progress))))
		{
			Fail(TEXT("cannot write progress.json"));
		}
	}

	void Complete(TArray<uint8>&& Manifest)
	{
		if (bFailed)
		{
			return;
		}
		// §7.4.2: manifest.json, then .complete, last.
		if (!DreamStubSnapshotLocal::WriteFileAtomically(Dir / TEXT("manifest.json"), Manifest) ||
			!FFileHelper::SaveStringToFile(FString(TEXT("complete\n")), *(Dir / TEXT(".complete"))))
		{
			Fail(TEXT("cannot write manifest.json or .complete"));
			return;
		}
		IFileManager::Get().Delete(*(Dir / DreamStubSnapshotLocal::ProgressFile), false, true, true);
		bComplete = true;
	}

	void Reset()
	{
		File.Reset();
		CurrentModule.Reset();
	}
};

FDreamStubSnapshot::FDreamStubSnapshot() = default;

FDreamStubSnapshot::~FDreamStubSnapshot()
{
	Shutdown();
}

void FDreamStubSnapshot::Initialize(FDreamPython& InPython)
{
	Python = &InPython;
	PythonShutdownHandle = InPython.OnShutdown().AddLambda([this]()
	{
		// §3.8 step 3 forgot the Python side of the walk; its buffered lines go too (the next walk rewrites the module).
		if (bWalking)
		{
			StopWalk(FString());
		}
	});
}

FString FDreamStubSnapshot::GetStubsRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("Dream") / TEXT("Stubs"));
}

void FDreamStubSnapshot::Start()
{
	check(IsInGameThread());
	bWantWalk = true;
	if (!bKeyRequested)
	{
		bKeyRequested = true;
		TSharedPtr<FKeyResult> Result = MakeShared<FKeyResult>();
		Result->EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Changelist);
		Result->PythonVersion = DreamPythonSnapshot::GetPythonVersion();
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
		{
			Result->Plugins.Add(Plugin->GetName() + TEXT("@") + Plugin->GetDescriptor().VersionName);
		}
		Result->Plugins.Sort();
		if (const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject())
		{
			for (const FModuleDescriptor& Module : Project->Modules)
			{
				Result->ModuleFiles.Add(FModuleManager::Get().GetModuleFilename(Module.Name));
			}
		}
		Result->ModuleFiles.Sort();
		Result->StubsRoot = GetStubsRoot();
		KeyTask = Result;
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [Result]()
		{
			using namespace DreamStubSnapshotLocal;
			IFileManager& Files = IFileManager::Get();
			FString Text = FString::Printf(TEXT("engine=%s\npython=%s\n"), *Result->EngineVersion, *Result->PythonVersion);
			for (const FString& Plugin : Result->Plugins)
			{
				Text += TEXT("plugin=") + Plugin + TEXT("\n");
			}
			for (const FString& File : Result->ModuleFiles)
			{
				// §7.4.3 (proposed): the size and mtime, never a hash of a large DLL.
				const FFileStatData Stat = Files.GetStatData(*File);
				Text += FString::Printf(TEXT("module=%s|%lld|%lld\n"), *FPaths::GetCleanFilename(File), Stat.FileSize,
					Stat.ModificationTime.GetTicks());
			}
			Text += FString::Printf(TEXT("format=%s\nwalker=%s\nstubgen=%s\n"), FDreamStubSnapshot::FormatVersion,
				FDreamStubSnapshot::WalkerVersion, FDreamStubSnapshot::StubgenVersion);
			const std::string Hex = dream::Sha256::ToHex(dream::Sha256::Hash(DreamText::ToUtf8(Text)));
			Result->Key = DreamText::Utf8ToFString(std::string_view(Hex).substr(0, 16));
			const FString KeyDir = Result->StubsRoot / Result->Key;
			Result->SnapshotDir = KeyDir / TEXT("snapshot");
			Files.MakeDirectory(*(Result->SnapshotDir / TEXT("modules")), /*Tree*/ true);
			FFileHelper::SaveStringToFile(Result->EngineVersion, *(KeyDir / UsedMarker));
			Result->bComplete = Files.FileExists(*(Result->SnapshotDir / TEXT(".complete"))) &&
				Files.FileExists(*(Result->SnapshotDir / TEXT("manifest.json")));
			if (!Result->bComplete)
			{
				// T-STUB-025: modules an interrupted walk finished, while their files are still there.
				FString Progress;
				dream::json::Value Entries;
				if (FFileHelper::LoadFileToString(Progress, *(Result->SnapshotDir / ProgressFile)) &&
					dream::json::Parse(DreamText::ToUtf8(Progress), Entries) && Entries.IsArray())
				{
					for (size_t Index = 0; Index < Entries.Size(); ++Index)
					{
						const dream::json::Value& Entry = Entries.At(Index);
						const std::string& File = Entry.Get("file").AsString();
						if (Entry.IsObject() && !File.empty() && Files.FileExists(*(Result->SnapshotDir / DreamText::Utf8ToFString(File))))
						{
							Result->DoneEntries.Add(DreamText::Utf8ToFString(dream::json::Serialize(Entry)));
						}
					}
				}
			}
			// §7.4.3 retention (proposed): this key and the newest other one stay; older keys are deleted.
			TArray<TPair<FDateTime, FString>> Others;
			Files.IterateDirectory(*Result->StubsRoot, [&](const TCHAR* Path, bool bIsDirectory)
			{
				const FString Dir(Path);
				if (bIsDirectory && FPaths::GetCleanFilename(Dir) != Result->Key)
				{
					FDateTime Used = Files.GetTimeStamp(*(Dir / UsedMarker));
					if (Used == FDateTime::MinValue())
					{
						Used = Files.GetTimeStamp(*Dir);
					}
					Others.Emplace(Used, Dir);
				}
				return true;
			});
			Others.Sort([](const TPair<FDateTime, FString>& A, const TPair<FDateTime, FString>& B) { return A.Key > B.Key; });
			for (int32 Index = FDreamStubSnapshot::KeysKept - 1; Index < Others.Num(); ++Index)
			{
				Files.DeleteDirectory(*Others[Index].Value, /*RequireExists*/ false, /*Tree*/ true);
			}
			Result->bDone = true;
		});
	}
	EnsureTicker();
}

void FDreamStubSnapshot::EnsureTicker()
{
	if (TickHandle.IsValid())
	{
		return;
	}
	TickHandle = FDreamDeferral::AddTicker(TEXT("DreamStubSnapshot"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
		/*bNeedsPython*/ true, [this](float DeltaTime) { return Tick(DeltaTime); });
}

void FDreamStubSnapshot::AdoptKey()
{
	Key = KeyTask->Key;
	SnapshotDir = KeyTask->SnapshotDir;
	bComplete = KeyTask->bComplete;
	bKeyReady = true;
	Writer = MakeShared<FWriter>();
	Writer->Dir = SnapshotDir;
	Writer->DoneEntries = KeyTask->DoneEntries;
	dream::json::Value Engine = dream::json::Value::MakeObject();
	Engine.Set("version", dream::json::Value::MakeString(DreamText::ToUtf8(KeyTask->EngineVersion)));
	Engine.Set("python", dream::json::Value::MakeString(DreamText::ToUtf8(KeyTask->PythonVersion)));
	EngineJson = DreamText::Utf8ToFString(dream::json::Serialize(Engine));
	KeyTask.Reset();
	UE_LOG(LogDream, Log, TEXT("Stub snapshot %s: %s (%s)."), *Key, bComplete ? TEXT("complete") : TEXT("to be built"), *SnapshotDir);
	KeyReadyEvent.Broadcast();
}

bool FDreamStubSnapshot::Tick(float /*DeltaTime*/)
{
	if (!bKeyReady)
	{
		if (!KeyTask.IsValid() || !KeyTask->bDone.load())
		{
			return true;
		}
		AdoptKey();
	}
	if (Writer.IsValid() && Writer->bFailed.load())
	{
		FString Error;
		{
			FScopeLock Guard(&Writer->Lock);
			Error = Writer->Error;
		}
		StopWalk(Error);
		bAwaitingWrites = false;
		bWantWalk = false;
	}
	if (bAwaitingWrites)
	{
		if (!Writer->bComplete.load())
		{
			return true;
		}
		bAwaitingWrites = false;
		bComplete = true;
		bWantWalk = false;
		UE_LOG(LogDream, Log, TEXT("Stub snapshot %s complete: %d slices, longest %.2f ms."), *Key, SliceCount, MaxSliceSeconds * 1000.0);
		if (!bTestWalk)
		{
			CompleteEvent.Broadcast(SnapshotDir, Key);
		}
	}
	if (bComplete || !bWantWalk)
	{
		TickHandle.Reset();
		return false;
	}

	// §7.4.2 gates beyond FDreamDeferral's (run active, pump, Python Ready): no modal window, no recent Dream keystroke.
	const double Now = FPlatformTime::Seconds();
	if ((FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()) ||
		(LastKeystrokeSeconds >= 0.0 && Now - LastKeystrokeSeconds < KeystrokeQuietSeconds))
	{
		return true;
	}
	if (!bWalking)
	{
		StartWalk();
		return true;
	}

	TArray<FDreamStubOutput> Outputs;
	FString Error;
	const double Began = FPlatformTime::Seconds();
	if (IsGated(Began))
	{
		++GateViolations;       // T-STUB-028: a gate above (or FDreamDeferral's) let this slice through
	}
	if (SliceTimes.Num() >= 4096)
	{
		SliceTimes.RemoveAt(0, 1024, EAllowShrinking::No);
	}
	SliceTimes.Add(Began);
	const bool bOk = DreamPythonSnapshot::Step(SliceBudget, Outputs, Error);
	const double Took = FPlatformTime::Seconds() - Began;
	++SliceCount;
	MaxSliceSeconds = FMath::Max(MaxSliceSeconds, Took);
	if (!bOk)
	{
		UE_LOG(LogDream, Warning, TEXT("Stub snapshot stopped: %s"), *Error);
		StopWalk(Error);
		bWantWalk = false;      // one failure per session: dream-check keeps SUM900 ("unavailable") until the next start
		TickHandle.Reset();
		return false;
	}
	for (FDreamStubOutput& Output : Outputs)
	{
		HandleOutput(Output);
	}
	FlushLines(/*bForce*/ false);
	return true;
}

bool FDreamStubSnapshot::IsGated(double Now) const
{
	// The §7.4.2 conditions: a run active or a pump on the stack (FDreamDeferral's gate), a modal window, a recent keystroke.
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		if (Module.GetRunService().IsRunActive() || Module.GetSupervisor().GetPumpDepth() > 0)
		{
			return true;
		}
	}
	return (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()) ||
		(LastKeystrokeSeconds >= 0.0 && Now - LastKeystrokeSeconds < KeystrokeQuietSeconds);
}

bool FDreamStubSnapshot::BeginTestWalkForTest(const FString& Dir)
{
	if (!bKeyReady || bWalking || bAwaitingWrites || bTestWalk || !Writer.IsValid())
	{
		return false;
	}
	SavedForTest = FSaved{ Key, SnapshotDir, bComplete, Writer };
	bTestWalk = true;
	Key = TEXT("t4walk0000000000");
	SnapshotDir = Dir;
	bComplete = false;
	IFileManager::Get().MakeDirectory(*(Dir / TEXT("modules")), /*Tree*/ true);
	Writer = MakeShared<FWriter>();
	Writer->Dir = Dir;
	LastError.Reset();
	bWantWalk = true;
	EnsureTicker();
	return true;
}

void FDreamStubSnapshot::EndTestWalkForTest()
{
	if (!bTestWalk)
	{
		return;
	}
	StopWalk(FString());
	bAwaitingWrites = false;
	if (Pipe.IsValid())
	{
		Pipe->WaitUntilEmpty(FTimespan::FromSeconds(5.0));
	}
	Key = SavedForTest.Key;
	SnapshotDir = SavedForTest.SnapshotDir;
	bComplete = SavedForTest.bComplete;
	Writer = SavedForTest.Writer;
	SavedForTest = FSaved();
	bTestWalk = false;
	bWantWalk = !bComplete;
	LastError.Reset();
	if (bWantWalk)
	{
		EnsureTicker();
	}
}

void FDreamStubSnapshot::StartWalk()
{
	FString Done;
	{
		FScopeLock Guard(&Writer->Lock);
		Done = TEXT("[") + FString::Join(Writer->DoneEntries, TEXT(",")) + TEXT("]");
	}
	FString Error;
	if (!DreamPythonSnapshot::Start(Key, EngineJson, Done, Error))
	{
		UE_LOG(LogDream, Warning, TEXT("Stub snapshot could not start: %s"), *Error);
		LastError = Error;
		bWantWalk = false;
		return;
	}
	if (!Pipe.IsValid())
	{
		Pipe = MakeUnique<UE::Tasks::FPipe>(TEXT("DreamStubSnapshot"));
	}
	bWalking = true;
}

void FDreamStubSnapshot::StopWalk(const FString& Why)
{
	if (!Why.IsEmpty())
	{
		LastError = Why;
	}
	if (bWalking)
	{
		DreamPythonSnapshot::Stop();         // a no-op when Python is not Ready (shutdown already forgot the walk)
	}
	bWalking = false;
	Lines.Reset();
	LinesModule.Reset();
	if (Pipe.IsValid() && Writer.IsValid())
	{
		TSharedPtr<FWriter> State = Writer;
		Pipe->Launch(UE_SOURCE_LOCATION, [State]() { State->Reset(); });
	}
}

void FDreamStubSnapshot::HandleOutput(FDreamStubOutput& Output)
{
	TSharedPtr<FWriter> State = Writer;
	switch (Output.Kind)
	{
	case 0:
		if (Output.Module != LinesModule)
		{
			FlushLines(/*bForce*/ true);
			LinesModule = Output.Module;
		}
		Lines.Append(Output.Data);
		Lines.Add(static_cast<uint8>('\n'));
		break;
	case 1:
	{
		FlushLines(/*bForce*/ true);
		const FString Module = Output.Module;
		const FString Entry = DreamText::Utf8ToFString(std::string_view(reinterpret_cast<const char*>(Output.Data.GetData()),
			static_cast<size_t>(Output.Data.Num())));
		Pipe->Launch(UE_SOURCE_LOCATION, [State, Module, Entry]() { State->FinishModule(Module, Entry); });
		break;
	}
	case 2:
	{
		FlushLines(/*bForce*/ true);
		TArray<uint8> Manifest = MoveTemp(Output.Data);
		Pipe->Launch(UE_SOURCE_LOCATION, [State, Manifest = MoveTemp(Manifest)]() mutable { State->Complete(MoveTemp(Manifest)); });
		bWalking = false;
		bAwaitingWrites = true;
		break;
	}
	default:
		break;
	}
}

void FDreamStubSnapshot::FlushLines(bool bForce)
{
	if (Lines.IsEmpty() || (!bForce && Lines.Num() < DreamStubSnapshotLocal::FlushBytes))
	{
		return;
	}
	TSharedPtr<FWriter> State = Writer;
	const FString Module = LinesModule;
	TArray<uint8> Bytes = MoveTemp(Lines);
	Lines.Reset();
	Pipe->Launch(UE_SOURCE_LOCATION, [State, Module, Bytes = MoveTemp(Bytes)]() mutable { State->Append(Module, MoveTemp(Bytes)); });
}

void FDreamStubSnapshot::NoteKeystroke()
{
	LastKeystrokeSeconds = FPlatformTime::Seconds();
}

void FDreamStubSnapshot::Shutdown()
{
	FDreamDeferral::RemoveTicker(TickHandle);
	TickHandle.Reset();
	bWantWalk = false;
	bWalking = false;
	Lines.Reset();
	if (Python != nullptr)
	{
		Python->OnShutdown().Remove(PythonShutdownHandle);
		PythonShutdownHandle.Reset();
		Python = nullptr;
	}
	if (Pipe.IsValid())
	{
		// Queued writes finish (each is one file); a pipe that cannot drain is left alive rather than destroyed with work.
		if (!Pipe->WaitUntilEmpty(FTimespan::FromSeconds(5.0)))
		{
			UE_LOG(LogDream, Warning, TEXT("Stub snapshot: queued writes did not finish within 5 s."));
			(void)Pipe.Release();
		}
		Pipe.Reset();
	}
	if (KeyTask.IsValid() && !KeyTask->bDone.load())
	{
		// The key task holds its own reference to the result; nothing here waits for it.
		KeyTask.Reset();
	}
}
