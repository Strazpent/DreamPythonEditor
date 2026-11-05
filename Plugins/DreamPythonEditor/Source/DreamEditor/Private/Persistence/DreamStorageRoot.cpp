// SPDX-License-Identifier: Apache-2.0
// The per-user, per-project storage root (TDD §10.1, U29): %LOCALAPPDATA%\DreamPythonEditor\<EngineVer>\<Name>-<hex6>\, never under the
// engine or the project tree; the user temp folder is the fallback, with a persistent warning. FDreamJournal's resolver
// takes the platform default and the temp fallback on the game thread (local paths); storage.root_override, its owner.json
// and its bounded probe are FDreamInstances' (DreamInstances.cpp), which tries the override first and then calls this.
#include "Persistence/DreamStorageRoot.h"

#include "Buffers/DreamHash.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamWin32FileOps.h"
#include "dream/Json.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamStorageRootLocal
{
	bool IsUnder(const FString& Path, const FString& Dir)
	{
		FString FullDir = FPaths::ConvertRelativePathToFull(Dir);
		FPaths::NormalizeDirectoryName(FullDir);
		return !FullDir.IsEmpty() && FPaths::IsUnderDirectory(Path, FullDir);
	}
}

FString DreamStorageRoot::EngineVersionDir()
{
	return FString::Printf(TEXT("%d.%d"), FEngineVersion::Current().GetMajor(), FEngineVersion::Current().GetMinor());
}

FString DreamStorageRoot::SanitizeName(const FString& BaseName)
{
	FString Out;
	for (const TCHAR Ch : BaseName)
	{
		if ((FChar::IsAlnum(Ch) && Ch < 128) || Ch == TEXT('_') || Ch == TEXT('-'))
		{
			Out.AppendChar(Ch);
		}
	}
	if (Out.IsEmpty())
	{
		Out = TEXT("Project");
	}
	return Out.Left(40);
}

FString DreamStorageRoot::NormalizedProjectPath(const FString& UProjectPath)
{
	FString Final = UProjectPath;
	FDreamWin32FileOps::GetFinalPath(UProjectPath, Final);
	Final = FPaths::ConvertRelativePathToFull(Final);
	FPaths::NormalizeFilename(Final);
	FPaths::CollapseRelativeDirectories(Final);
	while (Final.Len() > 1 && Final.EndsWith(TEXT("/")))
	{
		Final.LeftChopInline(1);
	}
#if PLATFORM_WINDOWS
	Final.ToLowerInline();
#endif
	return Final;
}

FString DreamStorageRoot::PlatformDefaultBase()
{
	// FPlatformProcess::UserSettingsDir is %LOCALAPPDATA% on Windows (§10.1).
	const FString Settings = FPlatformProcess::UserSettingsDir();
	return Settings.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Settings / TEXT("DreamPythonEditor"));
}

FString DreamStorageRoot::TempBase()
{
	const FString Temp = FPlatformProcess::UserTempDir();
	return Temp.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Temp / TEXT("DreamPythonEditor"));
}

FString DreamStorageRoot::ProjectsJsonPath()
{
	const FString Base = PlatformDefaultBase();
	return Base.IsEmpty() ? FString() : Base / EngineVersionDir() / TEXT("projects.json");
}

FString DreamStorageRoot::MakeRootPath(const FString& Base, const FString& UProjectPath, int32 HexDigits)
{
	const FString Project = NormalizedProjectPath(UProjectPath);
	const FString Name = SanitizeName(FPaths::GetBaseFilename(UProjectPath));
	const FString Hash = DreamHash::Sha256Hex(DreamText::ToUtf8(Project));
	FString Root = FPaths::ConvertRelativePathToFull(Base / EngineVersionDir() / FString::Printf(TEXT("%s-%s"), *Name, *Hash.Left(HexDigits)));
	FPaths::NormalizeDirectoryName(Root);
	return Root;
}

bool DreamStorageRoot::IsAllowedRoot(const FString& Root)
{
	return !DreamStorageRootLocal::IsUnder(Root, FPaths::EngineDir()) && !DreamStorageRootLocal::IsUnder(Root, FPaths::ProjectDir());
}

bool DreamStorageRoot::PrepareWritable(const FString& Root)
{
	if (!IFileManager::Get().MakeDirectory(*Root, /*Tree*/ true) && !IFileManager::Get().DirectoryExists(*Root))
	{
		return false;
	}
	const FString Probe = Root / FString::Printf(TEXT(".probe-%u"), FPlatformProcess::GetCurrentProcessId());
	if (!FFileHelper::SaveStringToFile(TEXT("dream"), *Probe))
	{
		return false;
	}
	IFileManager::Get().Delete(*Probe, false, true, true);
	return true;
}

bool DreamStorageRoot::ProjectJsonMatches(const FString& Root, const FString& NormalizedProject)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *(Root / TEXT("project.json"))))
	{
		return true;   // a new folder
	}
	dream::json::Value Value;
	if (!dream::json::Parse(DreamText::ToUtf8(Text), Value))
	{
		return true;
	}
	const std::string& Stored = Value.Get("uproject").AsString();
	return Stored.empty() || DreamText::Utf8ToFString(Stored) == NormalizedProject;
}

void DreamStorageRoot::WriteProjectJson(const FString& Root, const FString& NormalizedProject, const FString& Name)
{
	const FString Target = Root / TEXT("project.json");
	if (IFileManager::Get().FileExists(*Target))
	{
		return;
	}
	dream::json::Value Value = dream::json::Value::MakeObject();
	Value.Set("schema", dream::json::Value::MakeInt(1));
	Value.Set("uproject", dream::json::Value::MakeString(DreamText::ToUtf8(NormalizedProject)));
	Value.Set("name", dream::json::Value::MakeString(DreamText::ToUtf8(Name)));
	const std::string Bytes = dream::json::Serialize(Value, true);

	// Small JSON files: temp, flush, atomic replace (§10.2.3).
	FDreamWin32FileOps Ops;
	const std::string Temp = FDreamWin32FileOps::ToUtf8Path(Target + TEXT(".tmp"));
	dream::journal::FileOps::Handle H = dream::journal::FileOps::InvalidHandle;
	Ops.Remove(Temp);
	if (Ops.CreateExclusive(Temp, H) && Ops.Append(H, Bytes) && Ops.Flush(H))
	{
		Ops.Close(H);
		Ops.ReplaceAtomic(FDreamWin32FileOps::ToUtf8Path(Target), Temp, FDreamWin32FileOps::ToUtf8Path(Target + TEXT(".bak")));
	}
	else
	{
		Ops.Close(H);
		Ops.Remove(Temp);
	}
}

bool DreamStorageRoot::PrepareRoot(const FString& Base, const FString& UProjectPath, FDreamStorageRoot& OutRoot)
{
	if (Base.IsEmpty())
	{
		return false;
	}
	const FString Project = NormalizedProjectPath(UProjectPath);
	const FString Name = SanitizeName(FPaths::GetBaseFilename(UProjectPath));
	FString Root = MakeRootPath(Base, UProjectPath, 6);
	if (!ProjectJsonMatches(Root, Project))
	{
		Root = MakeRootPath(Base, UProjectPath, 8);   // a 24-bit collision: 8 hex digits (proposed)
	}
	if (!IsAllowedRoot(Root))
	{
		UE_LOG(LogDream, Warning, TEXT("Storage folder %s is under the engine or the project folder; it is not used (§10.1)."), *Root);
		return false;
	}
	const bool bExisted = IFileManager::Get().DirectoryExists(*Root);
	if (!PrepareWritable(Root))
	{
		UE_LOG(LogDream, Warning, TEXT("Storage folder %s is not writable."), *Root);
		return false;
	}
	WriteProjectJson(Root, Project, Name);
	OutRoot = FDreamStorageRoot();
	OutRoot.Root = Root;
	OutRoot.ProjectHash = FPaths::GetCleanFilename(Root);
	OutRoot.bIsRemote = FDreamWin32FileOps::IsRemotePath(Root);
	OutRoot.bIsNew = !bExisted;
	OutRoot.BaseDir = FPaths::ConvertRelativePathToFull(Base);
	FPaths::NormalizeDirectoryName(OutRoot.BaseDir);
	return true;
}

FString DreamStorageRoot::BaseOfRoot(const FString& Root)
{
	return FPaths::GetPath(FPaths::GetPath(Root));
}

TValueOrError<FDreamStorageRoot, FText> FDreamJournal::ResolveStorageRoot(const FString& UProjectPath)
{
	// Platform default first (FPlatformProcess::UserSettingsDir is %LOCALAPPDATA% on Windows), then the temp fallback. The
	// user's storage.root_override comes before both: FDreamInstances::ResolveStorageRoot probes it and then calls this.
	const FString Bases[] = { DreamStorageRoot::PlatformDefaultBase(), DreamStorageRoot::TempBase() };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Bases); ++Index)
	{
		FDreamStorageRoot Result;
		if (!DreamStorageRoot::PrepareRoot(Bases[Index], UProjectPath, Result))
		{
			continue;
		}
		Result.bIsFallback = Index > 0;
		if (Result.bIsFallback)
		{
			UE_LOG(LogDream, Warning, TEXT("Dream Python Editor stores this project's journal in the temp folder %s."), *Result.Root);
		}
		return MakeValue(MoveTemp(Result));
	}
	return MakeError(LOCTEXT("NoStorageRoot", "Dream Python Editor found no writable storage folder; the journal is off for this session."));
}

#undef LOCTEXT_NAMESPACE
