// SPDX-License-Identifier: Apache-2.0
// FDreamInstances (TDD §10.1, §10.5, §10.6). See DreamInstances.h. Every file operation here runs on the DreamStorageProbe
// thread (or, for local history, on the journal writer); the game thread only queues work, waits at most 2 s for the
// override probe, and applies results from the completion queue.
#include "Persistence/DreamInstances.h"

#include "Containers/Queue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ISettingsModule.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Output/DreamReplHistory.h"
#include "Persistence/DreamJournalWriter.h"
#include "Persistence/DreamLocalHistory.h"
#include "Persistence/DreamStorageProbe.h"
#include "Persistence/DreamStorageRoot.h"
#include "Persistence/DreamWin32FileOps.h"
#include "Run/DreamDeferral.h"
#include "Settings/DreamUserSettings.h"
#include "dream/StorageIndex.h"
#include "dream/TextBuffer.h"

#include <algorithm>
#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

/** State shared with the probe jobs; it outlives FDreamInstances while a job still holds it. */
struct FDreamInstances::FShared
{
	TQueue<TFunction<void()>, EQueueMode::Mpsc> Completions;
	std::atomic<bool> bAlive { true };

	// Written and read on the probe thread only (jobs run in order): what UpdateProjects learned for the cross-root scan.
	FString RecordedRoot;
	TArray<FString> SameIdentityRoots;

	void Complete(TFunction<void()> Work)
	{
		if (bAlive.load(std::memory_order_acquire))
		{
			Completions.Enqueue(MoveTemp(Work));
		}
	}
};

namespace DreamInstancesLocal
{
	using FSharedPtr = TSharedPtr<FDreamInstances::FShared, ESPMode::ThreadSafe>;

	int64 NowUtcMs()
	{
		return static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds());
	}

	dream::TextSnapshot ModelSnapshot(const FString& Text)
	{
		return dream::TextBuffer(DreamText::ToU16(DreamText::ToModelText(Text))).Snapshot();
	}

	FString BufferKey(const FString& Key, const FGuid& Buffer)
	{
		return Key + TEXT("|") + DreamText::GuidToHex(Buffer);
	}

	/** Small JSON files (§10.2.3): temp, flush, ReplaceFileW with .bak. */
	bool WriteSmallFile(FDreamWin32FileOps& Ops, const FString& Path, const std::string& Bytes)
	{
		const std::string Target = FDreamWin32FileOps::ToUtf8Path(Path);
		const std::string Temp = Target + ".tmp";
		Ops.Remove(Temp);
		dream::journal::FileOps::Handle H = dream::journal::FileOps::InvalidHandle;
		const bool bWritten = Ops.CreateExclusive(Temp, H) && Ops.Append(H, Bytes) && Ops.Flush(H);
		if (H != dream::journal::FileOps::InvalidHandle)
		{
			Ops.Close(H);
		}
		if (!bWritten || !Ops.ReplaceAtomic(Target, Temp, Target + ".bak"))
		{
			Ops.Remove(Temp);
			return false;
		}
		return true;
	}

	/** projects.json.lock created exclusively, with retries for 1 s (§10.1, proposed). (slice B choice) A lock file older
	 *  than 30 s is a crashed holder's: it is removed and taken once more. */
	bool LockProjects(FDreamWin32FileOps& Ops, const FString& ProjectsJson, dream::journal::FileOps::Handle& OutHandle)
	{
		const std::string LockPath = FDreamWin32FileOps::ToUtf8Path(ProjectsJson + TEXT(".lock"));
		Ops.MakeDirs(FDreamWin32FileOps::ToUtf8Path(FPaths::GetPath(ProjectsJson)));
		const double Deadline = FPlatformTime::Seconds() + FDreamInstances::ProjectsLockSeconds;
		bool bStaleRemoved = false;
		while (true)
		{
			if (Ops.CreateExclusive(LockPath, OutHandle))
			{
				return true;
			}
			if (FPlatformTime::Seconds() >= Deadline)
			{
				int64_t Written = 0;
				if (!bStaleRemoved && Ops.ModifiedUtcMs(LockPath, Written) && NowUtcMs() - Written > 30000)
				{
					bStaleRemoved = true;
					Ops.Remove(LockPath);
					continue;
				}
				return false;
			}
			FPlatformProcess::Sleep(0.05f);
		}
	}

	void UnlockProjects(FDreamWin32FileOps& Ops, const FString& ProjectsJson, dream::journal::FileOps::Handle Handle)
	{
		Ops.Close(Handle);
		Ops.Remove(FDreamWin32FileOps::ToUtf8Path(ProjectsJson + TEXT(".lock")));
	}

	/** projects.json, else its .bak, else a new index (§10.11 "projects.json corrupt: .bak, else rebuild with this project"). */
	dream::storage::ProjectsIndex ReadProjects(FDreamWin32FileOps& Ops, const FString& ProjectsJson)
	{
		dream::storage::ProjectsIndex Index;
		std::string Bytes;
		const std::string Path = FDreamWin32FileOps::ToUtf8Path(ProjectsJson);
		if (Ops.ReadAll(Path, Bytes) && dream::storage::ParseProjects(Bytes, Index))
		{
			return Index;
		}
		if (Ops.ReadAll(Path + ".bak", Bytes) && dream::storage::ParseProjects(Bytes, Index))
		{
			return Index;
		}
		return dream::storage::ProjectsIndex();
	}

	/** Moves OldHash from this project's pending offers to its decided ones (Import, Ignore), under the lock. */
	void RecordDecision(FDreamWin32FileOps& Ops, const FString& ProjectsJson, const FString& OurHash, const FString& OldHash)
	{
		dream::journal::FileOps::Handle Lock = dream::journal::FileOps::InvalidHandle;
		if (ProjectsJson.IsEmpty() || !LockProjects(Ops, ProjectsJson, Lock))
		{
			UE_LOG(LogDream, Warning, TEXT("Dream Python Editor could not lock %s; the answer for %s is not remembered."), *ProjectsJson, *OldHash);
			return;
		}
		dream::storage::ProjectsIndex Index = ReadProjects(Ops, ProjectsJson);
		if (dream::storage::ProjectEntry* Ours = Index.Find(DreamText::ToUtf8(OurHash)))
		{
			const std::string Old = DreamText::ToUtf8(OldHash);
			Ours->PendingImports.erase(std::remove(Ours->PendingImports.begin(), Ours->PendingImports.end(), Old), Ours->PendingImports.end());
			if (std::find(Ours->DecidedImports.begin(), Ours->DecidedImports.end(), Old) == Ours->DecidedImports.end())
			{
				Ours->DecidedImports.push_back(Old);
			}
			if (!Index.bNewerSchema)
			{
				WriteSmallFile(Ops, ProjectsJson, dream::storage::SerializeProjects(Index));
			}
		}
		UnlockProjects(Ops, ProjectsJson, Lock);
	}

	/** Every file and folder of a namespace except its ns.lock (held by us until the folder goes). */
	bool DeleteNamespaceContents(FDreamWin32FileOps& Ops, const FString& NsDir)
	{
		bool bOk = true;
		TArray<FString> Folders;
		FDreamWin32FileOps::ListDirs(NsDir, Folders);
		for (const FString& Folder : Folders)
		{
			if (Ops.IsFenced())
			{
				return false;   // the static helpers are not fenced: checked before each (DreamStorageProbe.h)
			}
			bOk = FDreamWin32FileOps::DeleteTree(NsDir / Folder) && bOk;
		}
		std::vector<std::string> Files;
		Ops.List(FDreamWin32FileOps::ToUtf8Path(NsDir), Files);
		for (const std::string& File : Files)
		{
			if (File != "ns.lock")
			{
				bOk = Ops.Remove(dream::journal::JoinPath(FDreamWin32FileOps::ToUtf8Path(NsDir), File)) && bOk;
			}
		}
		return bOk;
	}

	/** The rest once its ns.lock is released: the lock file and the folder. */
	void RemoveNamespaceFolder(FDreamWin32FileOps& Ops, const FString& NsDir)
	{
		Ops.Remove(FDreamWin32FileOps::ToUtf8Path(NsDir / TEXT("ns.lock")));
		FDreamWin32FileOps::DeleteTree(NsDir);
	}

	bool SameWalFiles(const TMap<FString, int64>& A, const TMap<FString, int64>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (const TPair<FString, int64>& Pair : A)
		{
			const int64* Other = B.Find(Pair.Key);
			if (Other == nullptr || *Other != Pair.Value)
			{
				return false;
			}
		}
		return true;
	}
}

FDreamInstances::FDreamInstances(FDreamJournal& InJournal, FDreamLocalHistory& InHistory, FDreamReplHistory& InRepl)
	: Journal(InJournal)
	, History(InHistory)
	, Repl(InRepl)
	, Shared(MakeShared<FShared, ESPMode::ThreadSafe>())
{
}

FDreamInstances::~FDreamInstances()
{
	Shutdown();
	if (Probe != nullptr)
	{
		// Shutdown left it alive only when it could not be joined: it is leaked like an abandoned journal writer (§10.1).
		Probe = nullptr;
	}
}

FString FDreamInstances::GetConfiguredOverride()
{
	FString FromCommandLine;
	if (FParse::Value(FCommandLine::Get(), TEXT("DreamStorageRoot="), FromCommandLine) && !FromCommandLine.IsEmpty())
	{
		return FPaths::ConvertRelativePathToFull(FromCommandLine);   // this session only; the user setting is not touched
	}
	const FString Setting = UDreamUserSettings::Get()->StorageRootOverride.Path;
	return Setting.TrimStartAndEnd().IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Setting.TrimStartAndEnd());
}

void FDreamInstances::OpenStorageSettings()
{
	if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		Settings->ShowViewer(TEXT("Editor"), TEXT("Plugins"), TEXT("Dream"));
	}
}

bool FDreamInstances::HasSessionOverride()
{
	FString FromCommandLine;
	return FParse::Value(FCommandLine::Get(), TEXT("DreamStorageRoot="), FromCommandLine) && !FromCommandLine.IsEmpty();
}

bool FDreamInstances::PostProbe(TFunction<void(FDreamWin32FileOps&)> Work)
{
	if (!Shared->bAlive.load(std::memory_order_acquire))
	{
		return false;
	}
	if (Probe == nullptr)
	{
		Probe = new FDreamStorageProbe();
	}
	EnsureTicker();
	return Probe->Post(MoveTemp(Work));
}

void FDreamInstances::EnsureTicker()
{
	if (Ticker.IsValid() || !Shared->bAlive.load(std::memory_order_acquire))
	{
		return;
	}
	Ticker = FDreamDeferral::AddTicker(TEXT("DreamInstances"), EDreamDeferPolicy::DeferWhileExecuting, 0.25f, false,
		[this](float) { return Tick(); });
}

bool FDreamInstances::Tick()
{
	TFunction<void()> Work;
	while (Shared->Completions.Dequeue(Work))
	{
		if (Work)
		{
			Work();
		}
	}
	if (Journal.IsOpen() && Journal.GetRole() == EDreamInstanceRole::Secondary && !bPrimaryEnded && !bPollInFlight
		&& FPlatformTime::Seconds() >= NextPollSeconds)
	{
		Poll();
	}
	return true;
}

void FDreamInstances::BlockProbeForTest(double Seconds)
{
	PostProbe([Seconds](FDreamWin32FileOps&) { FPlatformProcess::Sleep(static_cast<float>(Seconds)); });
}

void FDreamInstances::PumpForTest()
{
	Tick();
}

bool FDreamInstances::IsIdleForTest() const
{
	return Finalizing.Num() == 0 && (Probe == nullptr || Probe->GetPendingForTest() == 0) && Shared->Completions.IsEmpty();
}

void FDreamInstances::Broadcast()
{
	Changed.Broadcast();
}

// ---------------------------------------------------------------------------------------------------------------------
// The storage root (§10.1)
// ---------------------------------------------------------------------------------------------------------------------

TValueOrError<FDreamStorageRoot, FText> FDreamInstances::ResolveStorageRoot(const FString& UProjectPath, const FString& InOverride, double WaitSeconds,
	bool bStrict)
{
	ResolvedRoot.Reset();
	UProject = UProjectPath;
	OverrideBase = InOverride.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(InOverride);
	FPaths::NormalizeDirectoryName(OverrideBase);
	StorageNotices.Reset();

	enum class EOverride : uint8 { None, Used, Rejected, Unwritable, OtherAccount, Silent };
	EOverride Outcome = EOverride::None;
	FString OverrideRoot;
	TOptional<FDreamStorageRoot> Chosen;
	if (!OverrideBase.IsEmpty())
	{
		OverrideRoot = DreamStorageRoot::MakeRootPath(OverrideBase, UProject, 6);
		if (!DreamStorageRoot::IsAllowedRoot(OverrideRoot))
		{
			Outcome = EOverride::Rejected;
		}
		else
		{
			// §10.1: never a blocking call on the game thread for the override. The probe creates the folders, checks
			// <override>/owner.json (writing it at first use) and proves the root writable; the wait is bounded.
			struct FProbeState
			{
				std::atomic<int32> State { 0 };    // 0 pending, 1 used, 2 another account, 3 not writable
				FDreamStorageRoot Root;
			};
			TSharedRef<FProbeState, ESPMode::ThreadSafe> State = MakeShared<FProbeState, ESPMode::ThreadSafe>();
			const FString Sid = FDreamWin32FileOps::GetCurrentUserSid();
			const FString Base = OverrideBase;
			const FString Project = UProject;
			const bool bPosted = PostProbe([State, Sid, Base, Project](FDreamWin32FileOps& Ops)
			{
				const std::string OwnerPath = FDreamWin32FileOps::ToUtf8Path(Base / TEXT("owner.json"));
				Ops.MakeDirs(FDreamWin32FileOps::ToUtf8Path(Base));
				std::string Bytes;
				const bool bExists = Ops.Exists(OwnerPath);
				if (bExists)
				{
					Ops.ReadAll(OwnerPath, Bytes);
				}
				const dream::storage::OwnerCheck Check = dream::storage::CheckOwner(Bytes, bExists, DreamText::ToUtf8(Sid));
				if (Check == dream::storage::OwnerCheck::Other || Check == dream::storage::OwnerCheck::Unreadable || Sid.IsEmpty())
				{
					State->State.store(2, std::memory_order_release);
					return;
				}
				if (Check == dream::storage::OwnerCheck::Missing
					&& !DreamInstancesLocal::WriteSmallFile(Ops, Base / TEXT("owner.json"),
						dream::storage::SerializeOwner(DreamText::ToUtf8(Sid), dream::storage::FormatIsoUtc(DreamInstancesLocal::NowUtcMs()))))
				{
					State->State.store(3, std::memory_order_release);
					return;
				}
				FDreamStorageRoot Root;
				if (!DreamStorageRoot::PrepareRoot(Base, Project, Root))
				{
					State->State.store(3, std::memory_order_release);
					return;
				}
				Root.bIsOverride = true;
				State->Root = MoveTemp(Root);
				State->State.store(1, std::memory_order_release);
			});
			const double Deadline = FPlatformTime::Seconds() + WaitSeconds;
			while (bPosted && State->State.load(std::memory_order_acquire) == 0 && FPlatformTime::Seconds() < Deadline)
			{
				FPlatformProcess::Sleep(0.005f);
			}
			switch (bPosted ? State->State.load(std::memory_order_acquire) : 0)
			{
			case 1:
				Outcome = EOverride::Used;
				Chosen = State->Root;
				break;
			case 2:
				Outcome = EOverride::OtherAccount;
				break;
			case 3:
				Outcome = EOverride::Unwritable;
				break;
			default:
				// It counts as not writable for this session; a probe that completes later opened nothing that stays open.
				Outcome = EOverride::Silent;
				break;
			}
		}
	}

	if (bStrict && !OverrideBase.IsEmpty() && Outcome != EOverride::Used)
	{
		// (review r1, persist-M3) -DreamStorageRoot names this session's only root: when it cannot be used the session runs
		// without a journal, and the platform default (the user's own profile) is never used instead.
		StorageNotices.Add(FText::Format(LOCTEXT("Storage.SessionRootUnusable", "Storage folder {0} (-DreamStorageRoot) cannot be used ({1}); this session runs without a journal."),
			FText::FromString(OverrideBase),
			Outcome == EOverride::Silent ? LOCTEXT("Storage.WhySilent", "no answer within the wait")
				: (Outcome == EOverride::OtherAccount ? LOCTEXT("Storage.WhyOwner", "its owner.json names another account")
					: (Outcome == EOverride::Rejected ? LOCTEXT("Storage.WhyTree", "inside the engine or project folder")
						: LOCTEXT("Storage.WhyUnwritable", "it cannot be written")))));
		UE_LOG(LogDream, Warning, TEXT("%s"), *StorageNotices.Last().ToString());
		return MakeError(StorageNotices.Last());
	}
	if (!Chosen.IsSet())
	{
		TValueOrError<FDreamStorageRoot, FText> Fallback = FDreamJournal::ResolveStorageRoot(UProject);
		if (Fallback.HasValue())
		{
			Chosen = Fallback.StealValue();
		}
	}
	const FText Using = Chosen.IsSet() ? FText::FromString(Chosen->Root) : LOCTEXT("Storage.NoFolder", "no storage folder (the journal is off)");
	switch (Outcome)
	{
	case EOverride::Rejected:
		StorageNotices.Add(FText::Format(LOCTEXT("Storage.OverrideTree", "Storage folder {0} is under the engine or the project folder and is not used; this session uses {1}."),
			FText::FromString(OverrideRoot), Using));
		break;
	case EOverride::OtherAccount:
		StorageNotices.Add(FText::Format(LOCTEXT("Storage.OverrideOwner", "Storage folder {0} belongs to another account (owner.json) and is not used; this session uses {1}."),
			FText::FromString(OverrideBase), Using));
		break;
	case EOverride::Unwritable:
		StorageNotices.Add(FText::Format(LOCTEXT("Storage.OverrideUnwritable", "Storage folder {0} is not writable; this session uses {1}."),
			FText::FromString(OverrideRoot), Using));
		break;
	case EOverride::Silent:
		StorageNotices.Add(FText::Format(LOCTEXT("Storage.OverrideSilent", "Storage folder {0} did not answer; this session uses {1}."),
			FText::FromString(OverrideRoot), Using));
		break;
	default:
		break;
	}
	if (Outcome != EOverride::None && Outcome != EOverride::Used)
	{
		UE_LOG(LogDream, Warning, TEXT("%s"), *StorageNotices.Last().ToString());
	}
	if (Chosen.IsSet() && Chosen->bIsFallback)
	{
		StorageNotices.Add(FText::Format(LOCTEXT("Storage.TempRoot", "Dream Python Editor stores this project's journal in the temp folder {0}."), FText::FromString(Chosen->Root)));
	}
	if (!Chosen.IsSet())
	{
		return MakeError(LOCTEXT("Storage.NoStorageRoot", "Dream Python Editor found no writable storage folder; the journal is off for this session."));
	}
	ResolvedRoot = Chosen;
	return MakeValue(Chosen.GetValue());
}

// ---------------------------------------------------------------------------------------------------------------------
// projects.json (§10.1)
// ---------------------------------------------------------------------------------------------------------------------

void FDreamInstances::UpdateProjects(const FString& InProjectsJson, const FString& UProjectPath, const FDreamStorageRoot& Root)
{
	ProjectsJson = InProjectsJson;
	if (ProjectsJson.IsEmpty() || Root.Root.IsEmpty())
	{
		return;
	}
	const FString Hash = Root.ProjectHash;
	const FString RootDir = Root.Root;
	const bool bNewFolder = Root.bIsNew;
	const FString Name = DreamStorageRoot::SanitizeName(FPaths::GetBaseFilename(UProjectPath));
	const FString Normalized = DreamStorageRoot::NormalizedProjectPath(UProjectPath);
	const FString UProjectFile = UProjectPath;
	const FString Path = ProjectsJson;
	DreamInstancesLocal::FSharedPtr SharedRef = Shared;
	FDreamInstances* Self = this;   // dereferenced only by completions, which only this object's Tick runs
	PostProbe([Self, SharedRef, Path, Hash, RootDir, bNewFolder, Name, Normalized, UProjectFile](FDreamWin32FileOps& Ops)
	{
		const FString FileId = FDreamWin32FileOps::GetIdentity(UProjectFile);
		dream::journal::FileOps::Handle Lock = dream::journal::FileOps::InvalidHandle;
		if (!DreamInstancesLocal::LockProjects(Ops, Path, Lock))
		{
			UE_LOG(LogDream, Warning, TEXT("Dream Python Editor could not lock %s within 1 s; it is not updated this session."), *Path);
			return;
		}
		dream::storage::ProjectsIndex Index = DreamInstancesLocal::ReadProjects(Ops, Path);
		const std::string HashUtf8 = DreamText::ToUtf8(Hash);
		const std::vector<dream::storage::ProjectEntry> Moved = dream::storage::FindMovedProjects(Index, HashUtf8, DreamText::ToUtf8(Name), bNewFolder,
			[&Ops](std::string_view P) { return Ops.Exists(P); });
		const std::vector<dream::storage::ProjectEntry> Same = dream::storage::FindSameIdentity(Index, HashUtf8, DreamText::ToUtf8(FileId));

		dream::storage::ProjectEntry Ours;
		if (const dream::storage::ProjectEntry* Existing = Index.Find(HashUtf8))
		{
			Ours = *Existing;
		}
		const FString PreviousRoot = DreamText::Utf8ToFString(Ours.Root);
		Ours.Hash = HashUtf8;
		Ours.UProject = DreamText::ToUtf8(Normalized);
		Ours.Name = DreamText::ToUtf8(Name);
		Ours.FileId = DreamText::ToUtf8(FileId);
		Ours.Root = DreamText::ToUtf8(RootDir);
		Ours.LastSeenUtc = dream::storage::FormatIsoUtc(DreamInstancesLocal::NowUtcMs());
		for (const dream::storage::ProjectEntry& M : Moved)
		{
			if (std::find(Ours.PendingImports.begin(), Ours.PendingImports.end(), M.Hash) == Ours.PendingImports.end())
			{
				Ours.PendingImports.push_back(M.Hash);   // offered until Import or Ignore (slice B choice)
			}
		}
		Index.Upsert(Ours);
		if (!Index.bNewerSchema && !DreamInstancesLocal::WriteSmallFile(Ops, Path, dream::storage::SerializeProjects(Index)))
		{
			UE_LOG(LogDream, Warning, TEXT("Dream Python Editor could not write %s."), *Path);
		}
		DreamInstancesLocal::UnlockProjects(Ops, Path, Lock);

		// For the cross-root scan, which runs after this on the same thread.
		SharedRef->RecordedRoot = PreviousRoot;
		SharedRef->SameIdentityRoots.Reset();
		for (const dream::storage::ProjectEntry& S : Same)
		{
			if (!S.Root.empty())
			{
				SharedRef->SameIdentityRoots.Add(DreamText::Utf8ToFString(S.Root));
			}
		}

		TArray<FDreamMovedProject> Offers;
		for (const dream::storage::ProjectEntry& M : Moved)
		{
			FDreamMovedProject& Offer = Offers.AddDefaulted_GetRef();
			Offer.OldHash = DreamText::Utf8ToFString(M.Hash);
			Offer.Name = DreamText::Utf8ToFString(M.Name);
			Offer.OldProjectPath = DreamText::Utf8ToFString(M.UProject);
			Offer.OldRoot = DreamText::Utf8ToFString(M.Root);
			int64_t SeenMs = 0;
			if (dream::storage::ParseIsoUtc(M.LastSeenUtc, SeenMs))
			{
				Offer.LastSeenUtc = FDateTime(1970, 1, 1) + FTimespan::FromMilliseconds(static_cast<double>(SeenMs));
			}
		}
		SharedRef->Complete([Self, Offers = MoveTemp(Offers)]() mutable
		{
			Self->MovedProjects = MoveTemp(Offers);
			Self->Broadcast();
		});
	});
}

void FDreamInstances::ImportMovedProject(const FString& OldHash)
{
	const int32 Index = MovedProjects.IndexOfByPredicate([&OldHash](const FDreamMovedProject& M) { return M.OldHash == OldHash; });
	if (Index == INDEX_NONE || !Journal.IsOpen() || Journal.GetRole() != EDreamInstanceRole::Primary)
	{
		return;
	}
	const FDreamMovedProject Moved = MovedProjects[Index];
	if (dream::storage::MakeImportedNamespace(DreamText::ToUtf8(Moved.OldHash), dream::storage::MainNamespace).empty())
	{
		// The old hash is a projects.json key nobody validated; it never becomes a folder name (review r1).
		UE_LOG(LogDream, Warning, TEXT("Dream Python Editor does not import %s: its projects.json key is not a project hash."), *Moved.OldHash);
		return;
	}
	MovedProjects.RemoveAt(Index);
	Broadcast();
	const FString OurRoot = Journal.GetStorageRoot().Root;
	const FString OurHash = Journal.GetStorageRoot().ProjectHash;
	const FString Path = ProjectsJson;
	DreamInstancesLocal::FSharedPtr SharedRef = Shared;
	FDreamInstances* Self = this;
	PostProbe([Self, SharedRef, Moved, OurRoot, OurHash, Path](FDreamWin32FileOps& Ops)
	{
		// §10.1 Import: its journal namespaces are copied (never moved: the old root stays as it was) to
		// <root>/journal/imported-<oldhash>[.<ns>]/; only namespaces nobody holds, only generation files, then an ns.lock
		// file, so the next start's scan lists them even when this session ends before it does.
		TArray<FString> Copied;
		TArray<FString> Names;
		FDreamWin32FileOps::ListDirs(Moved.OldRoot / TEXT("journal"), Names);
		for (const FString& Name : Names)
		{
			if (Ops.IsFenced())
			{
				return;   // abandoned at OnPreExit (DreamStorageProbe.h): no further file operation
			}
			const dream::storage::NamespaceName Parsed = dream::storage::ParseNamespace(DreamText::ToUtf8(Name));
			const FString Target = DreamText::Utf8ToFString(dream::storage::MakeImportedNamespace(DreamText::ToUtf8(Moved.OldHash), DreamText::ToUtf8(Name)));
			if (Parsed.Kind == dream::storage::NamespaceKind::Invalid || Target.IsEmpty()
				|| FDreamWin32FileOps::IsLockHeld(Moved.OldRoot / TEXT("journal") / Name / TEXT("ns.lock")))
			{
				continue;
			}
			const FString TargetDir = OurRoot / TEXT("journal") / Target;
			const FString SourceDir = Moved.OldRoot / TEXT("journal") / Name;
			std::vector<std::string> Files;
			Ops.List(FDreamWin32FileOps::ToUtf8Path(SourceDir), Files);
			bool bAny = false;
			for (const std::string& File : Files)
			{
				uint64 Generation = 0;
				if (!dream::journal::ParseGenerationFileName(File, Generation))
				{
					continue;
				}
				Ops.MakeDirs(FDreamWin32FileOps::ToUtf8Path(TargetDir));
				const FString FileName = DreamText::Utf8ToFString(File);
				bAny = FDreamWin32FileOps::CopyNew(SourceDir / FileName, TargetDir / FileName) || bAny;
			}
			if (bAny && !Ops.IsFenced())
			{
				bool bHeld = false;
				FDreamWin32FileOps::ReleaseLockFile(FDreamWin32FileOps::AcquireLockFile(TargetDir / TEXT("ns.lock"), bHeld));
				Copied.Add(Target);
			}
		}
		if (Ops.IsFenced())
		{
			return;
		}
		DreamInstancesLocal::RecordDecision(Ops, Path, OurHash, Moved.OldHash);
		const FString OldHistory = Moved.OldRoot / TEXT("history");
		SharedRef->Complete([Self, Copied, OldHistory]()
		{
			// Its history joins ours (blobs by SHA-256, entries by id), then its namespaces are listed as orphans.
			Self->History.MergeFrom(OldHistory, [](bool bOk, int32 Entries)
			{
				UE_LOG(LogDream, Log, TEXT("Imported %d local-history entries%s."), Entries, bOk ? TEXT("") : TEXT(" (some could not be read)"));
			});
			Self->ScanImported(Copied);
		});
	});
}

void FDreamInstances::IgnoreMovedProject(const FString& OldHash)
{
	MovedProjects.RemoveAll([&OldHash](const FDreamMovedProject& M) { return M.OldHash == OldHash; });
	Broadcast();
	const FString Path = ProjectsJson;
	const FString OurHash = Journal.GetStorageRoot().ProjectHash;
	if (Path.IsEmpty() || OurHash.IsEmpty())
	{
		return;
	}
	PostProbe([Path, OurHash, OldHash](FDreamWin32FileOps& Ops) { DreamInstancesLocal::RecordDecision(Ops, Path, OurHash, OldHash); });
}

void FDreamInstances::ScanImported(const TArray<FString>& Namespaces)
{
	if (Namespaces.Num() == 0)
	{
		return;
	}
	const FString OurRoot = Journal.GetStorageRoot().Root;
	DreamInstancesLocal::FSharedPtr SharedRef = Shared;
	FDreamInstances* Self = this;
	PostProbe([Self, SharedRef, OurRoot, Namespaces](FDreamWin32FileOps& Ops)
	{
		TArray<FDreamOrphanNamespace> Found;
		FDreamReadOnlyFileOps ReadOps(Ops);
		for (const FString& Name : Namespaces)
		{
			if (Ops.IsFenced())
			{
				break;
			}
			bool bHeld = false;
			const uint64 Lock = FDreamWin32FileOps::AcquireLockFile(OurRoot / TEXT("journal") / Name / TEXT("ns.lock"), bHeld,
				DreamJournalScan::MakeLockContent(Name));
			if (Lock == 0)
			{
				continue;
			}
			FDreamOrphanNamespace Orphan;
			DreamJournalScan::ReadOrphan(ReadOps, Ops, OurRoot, Name, /*bAll*/ false, Orphan);
			if (Orphan.Key.IsEmpty())
			{
				FDreamWin32FileOps::ReleaseLockFile(Lock);
				continue;
			}
			Orphan.Source = EDreamOrphanSource::Imported;
			Orphan.NamespaceLock = Lock;
			Found.Add(MoveTemp(Orphan));
		}
		SharedRef->Complete([Self, Found = MoveTemp(Found)]() mutable { Self->AddSameRootOrphans(MoveTemp(Found)); });
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Orphans (§10.5, §10.1)
// ---------------------------------------------------------------------------------------------------------------------

const FDreamOrphanNamespace* FDreamInstances::FindOrphan(const FString& Key) const
{
	return Orphans.FindByPredicate([&Key](const FDreamOrphanNamespace& O) { return O.Key == Key; });
}

void FDreamInstances::AddSameRootOrphans(TArray<FDreamOrphanNamespace>&& InOrphans)
{
	TArray<FString> Empty;
	for (FDreamOrphanNamespace& Orphan : InOrphans)
	{
		if (FindOrphan(Orphan.Key) != nullptr)
		{
			FDreamWin32FileOps::ReleaseLockFile(Orphan.NamespaceLock);   // already listed (its own lock is held)
			continue;
		}
		if (Orphan.Buffers.Num() == 0)
		{
			Empty.Add(Orphan.Key);   // §10.5 "Fully resolved": merged and deleted without being listed
		}
		UE_LOG(LogDream, Log, TEXT("Orphaned journal namespace %s (%s, %d buffer(s) to offer)."), *Orphan.Namespace,
			Orphan.bClean ? TEXT("closed cleanly") : TEXT("ended unexpectedly"), Orphan.Buffers.Num());
		Orphans.Add(MoveTemp(Orphan));
	}
	for (const FString& Key : Empty)
	{
		Finalize(Key);
	}
	Broadcast();
}

void FDreamInstances::StartCrossRootScan(const TArray<FString>& ExtraRoots)
{
	if (!Journal.IsOpen() || Journal.GetRole() != EDreamInstanceRole::Primary)
	{
		return;
	}
	if (bCrossRootStarted && ExtraRoots.Num() == 0)
	{
		return;
	}
	bCrossRootStarted = true;
	const FString OurRoot = Journal.GetStorageRoot().Root;
	TArray<FString> Candidates = ExtraRoots;
	if (!UProject.IsEmpty())
	{
		if (!OverrideBase.IsEmpty())
		{
			Candidates.Add(DreamStorageRoot::MakeRootPath(OverrideBase, UProject, 6));
		}
		Candidates.Add(DreamStorageRoot::MakeRootPath(DreamStorageRoot::PlatformDefaultBase(), UProject, 6));
		Candidates.Add(DreamStorageRoot::MakeRootPath(DreamStorageRoot::TempBase(), UProject, 6));
	}
	const FString Sid = FDreamWin32FileOps::GetCurrentUserSid();
	DreamInstancesLocal::FSharedPtr SharedRef = Shared;
	FDreamInstances* Self = this;
	const bool bUseRecorded = ExtraRoots.Num() == 0;
	PostProbe([Self, SharedRef, OurRoot, Candidates, Sid, bUseRecorded](FDreamWin32FileOps& Ops)
	{
		TArray<FString> Roots = Candidates;
		TArray<FString> SameRoots;
		if (bUseRecorded)
		{
			Roots.Add(SharedRef->RecordedRoot);
			SameRoots = SharedRef->SameIdentityRoots;
		}
		TSet<FString> Seen;
		Seen.Add(FDreamWin32FileOps::GetIdentity(OurRoot));
		FDreamReadOnlyFileOps ReadOps(Ops);
		TArray<FDreamOrphanNamespace> Found;
		auto ScanRoot = [&](const FString& Root, bool bSameProject)
		{
			if (Root.IsEmpty() || Ops.IsFenced() || !DreamStorageRoot::IsAllowedRoot(Root))
			{
				return;
			}
			// By folder identity, never by path: this root under another path (a junction, a symlink) is skipped (§10.1).
			const FString Identity = FDreamWin32FileOps::GetIdentity(Root);
			if (Identity.IsEmpty() || Seen.Contains(Identity))
			{
				return;
			}
			Seen.Add(Identity);
			// A root whose owner.json names another account is never read, listed or deleted from (RV-08).
			const std::string Owner = FDreamWin32FileOps::ToUtf8Path(DreamStorageRoot::BaseOfRoot(Root) / TEXT("owner.json"));
			if (Ops.Exists(Owner))
			{
				std::string Bytes;
				Ops.ReadAll(Owner, Bytes);
				if (dream::storage::CheckOwner(Bytes, true, DreamText::ToUtf8(Sid)) != dream::storage::OwnerCheck::Mine)
				{
					return;
				}
			}
			if (FDreamWin32FileOps::IsLockHeld(Root / TEXT("instance.lock")))
			{
				return;   // that root's own live primary owns its namespaces (§10.5)
			}
			TArray<FString> Names;
			FDreamWin32FileOps::ListDirs(Root / TEXT("journal"), Names);
			for (const FString& Name : Names)
			{
				const dream::storage::NamespaceName Parsed = dream::storage::ParseNamespace(DreamText::ToUtf8(Name));
				if (Parsed.Kind == dream::storage::NamespaceKind::Invalid || (bSameProject && Parsed.Kind != dream::storage::NamespaceKind::Main)
					|| FDreamWin32FileOps::IsLockHeld(Root / TEXT("journal") / Name / TEXT("ns.lock")))
				{
					continue;
				}
				FDreamOrphanNamespace Orphan;
				DreamJournalScan::ReadOrphan(ReadOps, Ops, Root, Name, /*bAll*/ false, Orphan);
				// §10.1: without CLEAN, or with CLEAN but a non-empty scratch buffer, a file buffer that differs from the disk
				// or an unresolved RECOVERY_PENDING; the same project under another path only when its main lacks CLEAN.
				// (slice B choice) A namespace that offers no buffer is not listed.
				if (Orphan.Key.IsEmpty() || Orphan.Buffers.Num() == 0 || (bSameProject && Orphan.bClean))
				{
					continue;
				}
				Orphan.Source = bSameProject ? EDreamOrphanSource::SameProject : EDreamOrphanSource::OtherRoot;
				Orphan.Detail = Root;
				Found.Add(MoveTemp(Orphan));
			}
		};
		for (const FString& Root : Roots)
		{
			ScanRoot(Root, false);
		}
		for (const FString& Root : SameRoots)
		{
			ScanRoot(Root, true);
		}
		SharedRef->Complete([Self, Found = MoveTemp(Found)]() mutable
		{
			bool bAdded = false;
			for (FDreamOrphanNamespace& Orphan : Found)
			{
				if (Self->FindOrphan(Orphan.Key) == nullptr)
				{
					UE_LOG(LogDream, Log, TEXT("Tabs from another storage folder: %s (%d buffer(s))."), *Orphan.Key, Orphan.Buffers.Num());
					Self->Orphans.Add(MoveTemp(Orphan));
					bAdded = true;
				}
			}
			if (bAdded)
			{
				Self->Broadcast();
			}
		});
	});
}

bool FDreamInstances::IsOrphanBufferResolved(const FString& Key, const FGuid& Buffer) const
{
	return ResolvedBuffers.Contains(DreamInstancesLocal::BufferKey(Key, Buffer));
}

int32 FDreamInstances::CountOfferedBuffers() const
{
	int32 Count = 0;
	for (const FDreamOrphanNamespace& Orphan : Orphans)
	{
		for (const FDreamRecoveredBuffer& Buffer : Orphan.Buffers)
		{
			Count += IsOrphanBufferResolved(Orphan.Key, Buffer.Id) ? 0 : 1;
		}
	}
	return Count;
}

void FDreamInstances::ResolveOrphanBuffer(const FString& Key, const FGuid& Buffer, bool bDiscarded)
{
	const FDreamOrphanNamespace* Orphan = FindOrphan(Key);
	if (Orphan == nullptr || Orphan->bReadOnly)
	{
		return;
	}
	const FString BufferKey = DreamInstancesLocal::BufferKey(Key, Buffer);
	ResolvedBuffers.Add(BufferKey);
	if (bDiscarded)
	{
		DiscardedBuffers.Add(BufferKey);
	}
	const bool bAllResolved = !Orphan->Buffers.ContainsByPredicate([this, &Key](const FDreamRecoveredBuffer& B)
	{
		return !ResolvedBuffers.Contains(DreamInstancesLocal::BufferKey(Key, B.Id));
	});
	if (bAllResolved)
	{
		Finalize(Key);
	}
	Broadcast();
}

void FDreamInstances::Finalize(const FString& Key)
{
	const FDreamOrphanNamespace* Orphan = FindOrphan(Key);
	if (Orphan == nullptr || Orphan->bReadOnly || Finalizing.Contains(Key))
	{
		return;
	}
	if (!Journal.IsOpen() || Journal.GetRole() != EDreamInstanceRole::Primary)
	{
		return;   // only the primary merges and deletes (§10.5)
	}
	Finalizing.Add(Key, FPendingFinalize());
	FDreamInstances* Self = this;
	auto StepDone = [Self, Key](bool bOk)
	{
		FPendingFinalize* Pending = Self->Finalizing.Find(Key);
		if (Pending == nullptr)
		{
			return;
		}
		Pending->bFailed = Pending->bFailed || !bOk;
		if (--Pending->Outstanding == 0)
		{
			Self->FinishFinalize(Key);
		}
	};
	TSharedRef<TFunction<void(bool)>, ESPMode::ThreadSafe> Step = MakeShared<TFunction<void(bool)>, ESPMode::ThreadSafe>(StepDone);
	FPendingFinalize& Pending = Finalizing.FindChecked(Key);
	Pending.Outstanding = 1;   // held until every step is queued

	// 1. Its texts go to local history (§10.5 "deleted after its texts are copied into local history"); a discarded text has
	//    its "discarded" entry already. Each callback comes back through the history's own ticker on this thread.
	for (const FDreamRecoveredBuffer& Buffer : Orphan->Buffers)
	{
		if (Buffer.RecoveredText.IsEmpty() || DiscardedBuffers.Contains(DreamInstancesLocal::BufferKey(Key, Buffer.Id)))
		{
			continue;
		}
		++Pending.Outstanding;
		History.AddEntry(Buffer.Id, EDreamHistoryKind::Recovered, DreamInstancesLocal::ModelSnapshot(Buffer.RecoveredText), Buffer.Path,
			Buffer.DisplayName, TOptional<int32>(), [Step](bool bOk) { (*Step)(bOk); });
	}
	// 2. What a secondary staged (§10.5 Merge): its history entries and blobs, then its REPL entries in time order.
	const FString NsDir = Orphan->Root / TEXT("journal") / Orphan->Namespace;
	++Pending.Outstanding;
	History.MergeFrom(NsDir / TEXT("pending-history"), [Step](bool bOk, int32) { (*Step)(bOk); });
	++Pending.Outstanding;
	DreamInstancesLocal::FSharedPtr SharedRef = Shared;
	const FString ReplPath = NsDir / TEXT("repl-history.jsonl");
	if (!PostProbe([SharedRef, Self, Step, ReplPath](FDreamWin32FileOps& Ops)
	{
		// (review r1, persist-M1) A staged file that exists but cannot be read fails the step: the namespace, which holds
		// its only copy, is kept and listed again at the next start.
		TArray<FDreamHistoryEntry> Entries;
		const bool bPresent = Ops.Exists(FDreamWin32FileOps::ToUtf8Path(ReplPath));
		const bool bRead = !bPresent || FDreamReplHistory::ReadStagedFile(ReplPath, Entries);
		if (!bRead)
		{
			UE_LOG(LogDream, Warning, TEXT("REPL history: %s could not be read; its namespace is kept."), *ReplPath);
		}
		SharedRef->Complete([SharedRef, Self, Step, bRead, Entries = MoveTemp(Entries)]() mutable
		{
			if (!bRead)
			{
				(*Step)(false);
				return;
			}
			// Done only once the entries are on disk: MergeEntries answers after its rewrite landed (on its pipe), or false
			// when this editor cannot write history.jsonl (unreadable, OUT-03).
			TWeakPtr<FShared, ESPMode::ThreadSafe> WeakShared = SharedRef;
			Self->Repl.MergeEntries(MoveTemp(Entries), [WeakShared, Self, Step](bool bKept, int32 Joined)
			{
				if (TSharedPtr<FShared, ESPMode::ThreadSafe> Alive = WeakShared.Pin())
				{
					Alive->Complete([Step, bKept, Joined]()
					{
						if (Joined > 0)
						{
							UE_LOG(LogDream, Log, TEXT("Merged %d REPL history entries from another editor instance%s."), Joined,
								bKept ? TEXT("") : TEXT(" (not written: history.jsonl cannot be written now)"));
						}
						(*Step)(bKept);
					});
				}
				(void)Self;
			});
		});
	}))
	{
		(*Step)(false);
	}
	(*Step)(true);   // the hold
}

void FDreamInstances::FinishFinalize(const FString& Key)
{
	FPendingFinalize Pending = Finalizing.FindRef(Key);
	FDreamOrphanNamespace* Orphan = Orphans.FindByPredicate([&Key](const FDreamOrphanNamespace& O) { return O.Key == Key; });
	if (Orphan == nullptr)
	{
		Finalizing.Remove(Key);
		return;
	}
	if (Pending.bFailed)
	{
		// A text that could not be kept: nothing is deleted; the namespace is listed again at the next start.
		UE_LOG(LogDream, Warning, TEXT("Journal namespace %s was not deleted: its texts or staged files could not all be kept in local history."),
			*Orphan->Namespace);
		Finalizing.Remove(Key);
		return;
	}
	const FString NsDir = Orphan->Root / TEXT("journal") / Orphan->Namespace;
	const FString Root = Orphan->Root;
	const TMap<FString, int64> WalFiles = Orphan->WalFiles;
	const uint64 HeldLock = Orphan->NamespaceLock;
	Orphan->NamespaceLock = 0;   // the delete job owns it now
	DreamInstancesLocal::FSharedPtr SharedRef = Shared;
	FDreamInstances* Self = this;
	const bool bPosted = PostProbe([SharedRef, Self, Key, NsDir, Root, WalFiles, HeldLock](FDreamWin32FileOps& Ops)
	{
		enum class EResult : uint8 { Deleted, Live, Changed, Failed };
		EResult Result = EResult::Failed;
		if (Ops.IsFenced())
		{
			// Abandoned at OnPreExit (DreamStorageProbe.h): no file operation; only the held handle is closed.
			FDreamWin32FileOps::ReleaseLockFile(HeldLock);
			return;
		}
		if (HeldLock != 0)
		{
			// This root (§10.5): under its ns.lock, held since the scan.
			const bool bOk = DreamInstancesLocal::DeleteNamespaceContents(Ops, NsDir);
			FDreamWin32FileOps::ReleaseLockFile(HeldLock);
			if (bOk && !Ops.IsFenced())
			{
				DreamInstancesLocal::RemoveNamespaceFolder(Ops, NsDir);
			}
			Result = bOk ? EResult::Deleted : EResult::Failed;
		}
		else
		{
			// Another root (§10.1): that root's instance.lock, then the namespace's ns.lock; deleted only while both are held and
			// only when its wal files are as they were read (every session starts its own generation).
			bool bHeld = false;
			const uint64 InstanceLock = FDreamWin32FileOps::AcquireLockFile(Root / TEXT("instance.lock"), bHeld);
			// Never created in another root: a namespace whose ns.lock went away is not this scan's to delete.
			const uint64 NsLock = InstanceLock != 0
				? FDreamWin32FileOps::AcquireLockFile(NsDir / TEXT("ns.lock"), bHeld, std::string_view(), /*bOnlyIfExists*/ true) : 0;
			if (InstanceLock == 0 || NsLock == 0)
			{
				Result = EResult::Live;
			}
			else
			{
				TMap<FString, int64> Now;
				DreamJournalScan::RecordWalFiles(Ops, NsDir, Now);
				if (!DreamInstancesLocal::SameWalFiles(Now, WalFiles))
				{
					Result = EResult::Changed;
				}
				else
				{
					Result = DreamInstancesLocal::DeleteNamespaceContents(Ops, NsDir) ? EResult::Deleted : EResult::Failed;
				}
			}
			FDreamWin32FileOps::ReleaseLockFile(NsLock);
			if (Result == EResult::Deleted && !Ops.IsFenced())
			{
				DreamInstancesLocal::RemoveNamespaceFolder(Ops, NsDir);
			}
			FDreamWin32FileOps::ReleaseLockFile(InstanceLock);
		}
		TOptional<FDreamOrphanNamespace> Reread;
		if (Result == EResult::Changed)
		{
			// Written since the record: listed afresh with its new content (§10.1).
			FDreamReadOnlyFileOps ReadOps(Ops);
			FDreamOrphanNamespace Fresh;
			DreamJournalScan::ReadOrphan(ReadOps, Ops, Root, FPaths::GetCleanFilename(NsDir), false, Fresh);
			if (!Fresh.Key.IsEmpty() && Fresh.Buffers.Num() > 0)
			{
				Reread = MoveTemp(Fresh);
			}
		}
		SharedRef->Complete([Self, Key, Result, Reread = MoveTemp(Reread)]() mutable
		{
			Self->Finalizing.Remove(Key);
			const int32 Index = Self->Orphans.IndexOfByPredicate([&Key](const FDreamOrphanNamespace& O) { return O.Key == Key; });
			if (Index == INDEX_NONE)
			{
				return;
			}
			FDreamOrphanNamespace Old = Self->Orphans[Index];
			Self->Orphans.RemoveAt(Index);
			for (auto It = Self->ResolvedBuffers.CreateIterator(); It; ++It)
			{
				if (It->StartsWith(Key + TEXT("|")))
				{
					It.RemoveCurrent();
				}
			}
			switch (Result)
			{
			case EResult::Deleted:
				UE_LOG(LogDream, Log, TEXT("Journal namespace %s merged and deleted (§10.5)."), *Key);
				Self->Deleted.Add(Key);
				break;
			case EResult::Live:
				UE_LOG(LogDream, Log, TEXT("Journal namespace %s was not deleted: that storage folder now has a live editor."), *Key);
				break;
			case EResult::Changed:
				UE_LOG(LogDream, Log, TEXT("Journal namespace %s was written since it was read; it is listed again."), *Key);
				if (Reread.IsSet())
				{
					Reread->Source = Old.Source;
					Reread->Detail = Old.Detail;
					Self->Orphans.Add(MoveTemp(Reread.GetValue()));
				}
				break;
			default:
				UE_LOG(LogDream, Warning, TEXT("Journal namespace %s could not be deleted completely."), *Key);
				break;
			}
			Self->Broadcast();
		});
	});
	if (!bPosted)
	{
		Orphan->NamespaceLock = HeldLock;
		Finalizing.Remove(Key);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The secondary's side (§10.5)
// ---------------------------------------------------------------------------------------------------------------------

void FDreamInstances::OnJournalOpened()
{
	if (!Journal.IsOpen() || Journal.GetRole() != EDreamInstanceRole::Secondary)
	{
		return;
	}
	dream::storage::LockContent Lock;
	if (dream::storage::ParseLock(DreamText::ToUtf8(Journal.GetPrimaryLockContent()), Lock))
	{
		PrimaryPid = Lock.Pid;
	}
	else
	{
		// §10.5: a lock held with empty or unparseable content is read again after 200 ms, then treated as live (no PID).
		const FString LockPath = Journal.GetStorageRoot().Root / TEXT("instance.lock");
		DreamInstancesLocal::FSharedPtr SharedRef = Shared;
		FDreamInstances* Self = this;
		PostProbe([SharedRef, Self, LockPath](FDreamWin32FileOps&)
		{
			FPlatformProcess::Sleep(0.2f);
			std::string Text;
			FDreamWin32FileOps::ReadLockFile(LockPath, Text);
			dream::storage::LockContent Again;
			if (dream::storage::ParseLock(Text, Again))
			{
				const uint32 Pid = Again.Pid;
				SharedRef->Complete([Self, Pid]() { Self->PrimaryPid = Pid; Self->Broadcast(); });
			}
		});
	}
	// §10.5 table: the secondary appends its REPL entries to <sec>/repl-history.jsonl; history.jsonl stays the primary's.
	Repl.SetStagingFile(Journal.GetNamespaceDir() / TEXT("repl-history.jsonl"));
	NextPollSeconds = FPlatformTime::Seconds() + PollInterval;
	EnsureTicker();
}

void FDreamInstances::Poll()
{
	bPollInFlight = true;
	const FString Root = Journal.GetStorageRoot().Root;
	const uint32 Pid = PrimaryPid;
	DreamInstancesLocal::FSharedPtr SharedRef = Shared;
	FDreamInstances* Self = this;
	const bool bPosted = PostProbe([SharedRef, Self, Root, Pid](FDreamWin32FileOps& Ops)
	{
		// §10.5: a held lock on this host is live. Free: the primary is gone; main without CLEAN means it ended unexpectedly.
		if (FDreamWin32FileOps::IsLockHeld(Root / TEXT("instance.lock")))
		{
			SharedRef->Complete([Self]() { Self->bPollInFlight = false; Self->NextPollSeconds = FPlatformTime::Seconds() + Self->PollInterval; });
			return;
		}
		FDreamReadOnlyFileOps ReadOps(Ops);
		FDreamOrphanNamespace View;
		DreamJournalScan::ReadOrphan(ReadOps, Ops, Root, TEXT("main"), /*bAll*/ true, View);
		const bool bEnded = !View.Key.IsEmpty() && !View.bClean;
		View.Source = EDreamOrphanSource::PrimaryView;
		View.bReadOnly = true;
		View.Pid = Pid;
		SharedRef->Complete([Self, bEnded, View = MoveTemp(View)]() mutable
		{
			Self->bPollInFlight = false;
			Self->bPrimaryEnded = true;   // either way the poll stops: the primary is gone
			if (bEnded)
			{
				UE_LOG(LogDream, Warning, TEXT("The other editor instance ended unexpectedly; its buffers can be reviewed (§10.5)."));
				Self->PrimaryView = MoveTemp(View);
				Self->Broadcast();
			}
			else
			{
				Self->bPrimaryEnded = false;
				Self->NextPollSeconds = TNumericLimits<double>::Max();   // it exited cleanly: nothing to offer, no more polls
			}
		});
	});
	if (!bPosted)
	{
		bPollInFlight = false;
	}
}

void FDreamInstances::RequestPrimaryView()
{
	if (!Journal.IsOpen() || Journal.GetRole() != EDreamInstanceRole::Secondary)
	{
		return;
	}
	const FString Root = Journal.GetStorageRoot().Root;
	const uint32 Pid = PrimaryPid;
	DreamInstancesLocal::FSharedPtr SharedRef = Shared;
	FDreamInstances* Self = this;
	PostProbe([SharedRef, Self, Root, Pid](FDreamWin32FileOps& Ops)
	{
		// The tolerant reader copes with the primary's concurrent appends; nothing there is written, rotated or deleted.
		FDreamReadOnlyFileOps ReadOps(Ops);
		FDreamOrphanNamespace View;
		DreamJournalScan::ReadOrphan(ReadOps, Ops, Root, TEXT("main"), /*bAll*/ true, View);
		View.Source = EDreamOrphanSource::PrimaryView;
		View.bReadOnly = true;
		View.Pid = Pid;
		if (View.Key.IsEmpty())
		{
			View.Key = Root + TEXT("|main");
		}
		SharedRef->Complete([Self, View = MoveTemp(View)]() mutable
		{
			Self->PrimaryView = MoveTemp(View);
			Self->Broadcast();
		});
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------------

void FDreamInstances::Shutdown()
{
	if (Ticker.IsValid())
	{
		FDreamDeferral::RemoveTicker(Ticker);
		Ticker.Reset();
	}
	Shared->bAlive.store(false, std::memory_order_release);
	FDreamJournal::ReleaseOrphanLocks(Orphans);
	Finalizing.Reset();
	if (Probe != nullptr)
	{
		// §10.1: abandoned like the journal writer: the fence first, never joined; leaked when it does not finish at once.
		if (Probe->Abandon())
		{
			delete Probe;
		}
		Probe = nullptr;
	}
}

#undef LOCTEXT_NAMESPACE
