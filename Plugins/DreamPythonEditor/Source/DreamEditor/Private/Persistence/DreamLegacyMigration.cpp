// SPDX-License-Identifier: Apache-2.0
// (rename, PO-06 phase 3, A246) The one-time copy of a user's "Sumi" data to Dream Python Editor (DreamLegacyMigration.h).
#include "Persistence/DreamLegacyMigration.h"

#include "Buffers/DreamHash.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamInstances.h"
#include "Persistence/DreamStorageRoot.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Settings/DreamProjectTreeSettings.h"
#include "Settings/DreamUserSettings.h"
#include "Tasks/Task.h"
#include "dream/Json.h"

#include <atomic>

namespace DreamLegacyMigrationLocal
{
	const TCHAR* const RootMarkerName = TEXT(".migrating-root");
	const TCHAR* const RunLogsMarker = TEXT(".migrating-runlogs");
	const TCHAR* const ScratchMarker = TEXT(".migrated-scratches");
	const TCHAR* const ThemesMarker = TEXT(".migrated-themes");

	// Set in StartupModule (MigrateConfig) before any worker starts; read-only afterwards (RemapLegacyPath, any thread).
	FString GOldScratch;
	FString GNewScratch;
	// The ini files MigrateConfig changed in the config cache; RunStartupMigration flushes them (game thread).
	TArray<FString> GDirtyIniFiles;

	std::atomic<int32> GRunLogCopies{ 0 };
	std::atomic<bool> GStopRunLogCopy{ false };

	FString Normalize(const FString& In)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Out.Len() > 1 && Out.EndsWith(TEXT("/")))
		{
			Out.LeftChopInline(1);
		}
		return Out;
	}

	bool IsBoundary(const FString& Text, int32 Index)
	{
		if (Index >= Text.Len())
		{
			return true;
		}
		const TCHAR Ch = Text[Index];
		return Ch == TEXT('/') || Ch == TEXT('\\') || Ch == TEXT('"') || Ch == TEXT(')') || Ch == TEXT(',');
	}

	/** Every Old (either slash style, any case) that ends at a path boundary becomes New in that slash style. */
	FString ReplaceFolder(const FString& Text, const FString& Old, const FString& New)
	{
		if (Old.IsEmpty() || Text.Len() < Old.Len())
		{
			return Text;
		}
		const FString OldBack = Old.Replace(TEXT("/"), TEXT("\\"));
		const FString NewBack = New.Replace(TEXT("/"), TEXT("\\"));
		FString Out;
		int32 Pos = 0;
		while (Pos < Text.Len())
		{
			const FString Rest = Text.Mid(Pos);
			int32 Hit = INDEX_NONE;
			bool bBack = false;
			const int32 Forward = Rest.Find(Old, ESearchCase::IgnoreCase);
			const int32 Backward = Rest.Find(OldBack, ESearchCase::IgnoreCase);
			if (Forward != INDEX_NONE && (Backward == INDEX_NONE || Forward <= Backward))
			{
				Hit = Forward;
			}
			else if (Backward != INDEX_NONE)
			{
				Hit = Backward;
				bBack = true;
			}
			if (Hit == INDEX_NONE)
			{
				Out += Rest;
				break;
			}
			Out += Rest.Left(Hit);
			if (IsBoundary(Rest, Hit + Old.Len()))
			{
				Out += bBack ? NewBack : New;
			}
			else
			{
				Out += Rest.Mid(Hit, Old.Len());
			}
			Pos += Hit + Old.Len();
		}
		return Out;
	}

	/** "(Path=\"...\")" -> the path. */
	FString DirectoryPathOf(const FString& Value)
	{
		FString Path = Value;
		Path.TrimStartAndEndInline();
		if (Path.StartsWith(TEXT("(Path=")) && Path.EndsWith(TEXT(")")))
		{
			Path = Path.Mid(6, Path.Len() - 7).TrimQuotes();
		}
		return Path;
	}

	bool IsSkippedRootFile(const FString& Relative)
	{
		const FString Name = FPaths::GetCleanFilename(Relative);
		return Name.EndsWith(TEXT(".lock")) || Relative.Equals(TEXT("project.json"), ESearchCase::IgnoreCase)
			|| Name.EndsWith(TEXT(".part")) || Name.StartsWith(TEXT(".migrat"));
	}

	/** Copies Source to Target through Target.part, never over an existing Target. True when copied. */
	bool CopyOne(const FString& Source, const FString& Target)
	{
		IFileManager& Files = IFileManager::Get();
		if (Files.FileExists(*Target))
		{
			return false;
		}
		Files.MakeDirectory(*FPaths::GetPath(Target), /*Tree*/ true);
		const FString Part = Target + TEXT(".part");
		if (Files.Copy(*Part, *Source, /*bReplace*/ true, /*bEvenIfReadOnly*/ true) != COPY_OK)
		{
			Files.Delete(*Part, false, true, true);
			UE_LOG(LogDream, Warning, TEXT("Could not copy %s to %s."), *Source, *Target);
			return false;
		}
		if (!Files.Move(*Target, *Part, /*bReplace*/ false, /*bEvenIfReadOnly*/ true))
		{
			Files.Delete(*Part, false, true, true);
			return false;
		}
		return true;
	}

	/** Copies the files under From that are missing under To (Skip filters relative paths). Returns the number copied. */
	int32 CopyTree(const FString& From, const FString& To, TFunctionRef<bool(const FString& Relative)> Skip,
		const std::atomic<bool>* Stop = nullptr)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *From, TEXT("*"), /*Files*/ true, /*Directories*/ false);
		Found.Sort();
		int32 Copied = 0;
		for (const FString& Source : Found)
		{
			if (Stop != nullptr && Stop->load(std::memory_order_relaxed))
			{
				break;
			}
			FString Relative = Source;
			if (!FPaths::MakePathRelativeTo(Relative, *(From / TEXT(""))) || Skip(Relative))
			{
				continue;
			}
			if (CopyOne(Source, To / Relative))
			{
				++Copied;
			}
		}
		return Copied;
	}

	FString ReadMarker(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text.TrimStartAndEnd();
	}
}

TArrayView<const DreamLegacyMigration::FSectionPair> DreamLegacyMigration::SettingsSections()
{
	static const FSectionPair Pairs[] =
	{
		{ TEXT("/Script/SumiEditor.SumiUserSettings"), TEXT("/Script/DreamEditor.DreamUserSettings") },
		{ TEXT("/Script/SumiEditor.SumiPerProjectUserSettings"), TEXT("/Script/DreamEditor.DreamPerProjectUserSettings") },
		{ TEXT("/Script/SumiEditor.SumiProjectTreeSettings"), TEXT("/Script/DreamEditor.DreamProjectTreeSettings") },
	};
	return Pairs;
}

DreamLegacyMigration::FMigrationPaths DreamLegacyMigration::FMigrationPaths::ForThisUser()
{
	FMigrationPaths Out;
	const FString Settings = FPlatformProcess::UserSettingsDir();
	Out.OldBase = Settings.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Settings / TEXT("Sumi"));
	Out.NewBase = DreamStorageRoot::PlatformDefaultBase();
	Out.UProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	Out.ScratchSetting = UDreamUserSettings::Get()->ScratchFolder.Path;
	return Out;
}

int32 DreamLegacyMigration::CopySection(const FString& IniFile, const TCHAR* OldSection, const TCHAR* NewSection,
	TFunctionRef<FString(FName Key, const FString& Value)> Remap)
{
	if (GConfig == nullptr || IniFile.IsEmpty() || GConfig->DoesSectionExist(NewSection, IniFile))
	{
		return 0;
	}
	const FConfigSection* Old = GConfig->GetSection(OldSection, /*Force*/ false, IniFile);
	if (Old == nullptr || Old->Num() == 0)
	{
		return 0;
	}
	TArray<TPair<FName, FString>> Keys;   // copied first: adding to the cache may move the sections
	for (const TPair<FName, FConfigValue>& Pair : *Old)
	{
		Keys.Emplace(Pair.Key, Pair.Value.GetSavedValue());
	}
	for (const TPair<FName, FString>& Pair : Keys)
	{
		GConfig->AddToSection(NewSection, Pair.Key, Remap(Pair.Key, Pair.Value), IniFile);
	}
	return Keys.Num();
}

FString DreamLegacyMigration::RemapPath(const FString& Path, const FString& OldScratch, const FString& NewScratch)
{
	using namespace DreamLegacyMigrationLocal;
	return ReplaceFolder(Path, Normalize(OldScratch), Normalize(NewScratch));
}

FString DreamLegacyMigration::RemapLegacyPath(const FString& Path)
{
	using namespace DreamLegacyMigrationLocal;
	return GOldScratch.IsEmpty() ? Path : ReplaceFolder(Path, GOldScratch, GNewScratch);
}

int32 DreamLegacyMigration::MigrateConfig()
{
	using namespace DreamLegacyMigrationLocal;
	const FMigrationPaths Paths = FMigrationPaths::ForThisUser();
	const FString OldScratch = Normalize(Paths.OldScratch());
	const FString NewScratch = Normalize(Paths.NewScratch());
	int32 Sections = 0;

	// 1. Per user: a ScratchFolder that names the old default folder becomes empty, so it follows the new default.
	const FString UserIni = UDreamUserSettings::StaticClass()->GetConfigName();
	const TArrayView<const FSectionPair> Pairs = SettingsSections();
	if (CopySection(UserIni, Pairs[0].Old, Pairs[0].New, [&OldScratch](FName Key, const FString& Value)
		{
			if (Key == GET_MEMBER_NAME_CHECKED(UDreamUserSettings, ScratchFolder) && !OldScratch.IsEmpty()
				&& Normalize(DirectoryPathOf(Value)).Equals(OldScratch, ESearchCase::IgnoreCase))
			{
				return FString(TEXT("(Path=\"\")"));
			}
			return Value;
		}) > 0)
	{
		++Sections;
		GDirtyIniFiles.AddUnique(UserIni);
		UDreamUserSettings::GetMutable()->ReloadConfig();
	}

	// 2. Saved scratch paths follow the scratch files while the setting is the default folder.
	if (UDreamUserSettings::Get()->ScratchFolder.Path.IsEmpty() && !OldScratch.IsEmpty())
	{
		GOldScratch = OldScratch;
		GNewScratch = NewScratch;
	}

	// 3. Per project: the session state and the Project tree's roots, their paths remapped.
	const FString ProjectIni = UDreamPerProjectUserSettings::StaticClass()->GetConfigName();
	for (int32 Index = 1; Index < Pairs.Num(); ++Index)
	{
		if (CopySection(ProjectIni, Pairs[Index].Old, Pairs[Index].New,
			[](FName, const FString& Value) { return RemapLegacyPath(Value); }) > 0)
		{
			++Sections;
			GDirtyIniFiles.AddUnique(ProjectIni);
		}
	}
	if (GDirtyIniFiles.Contains(ProjectIni))
	{
		UDreamPerProjectUserSettings::GetMutable()->ReloadConfig();
		UDreamProjectTreeSettings::GetMutable()->ReloadConfig();
	}
	return Sections;
}

FString DreamLegacyMigration::LegacyProjectPath(const FString& NormalizedProject)
{
	// Only the host project itself: <...>/DreamHost.uproject, and its folder when that is named DreamHost too.
	TArray<FString> Parts;
	NormalizedProject.ParseIntoArray(Parts, TEXT("/"), /*CullEmpty*/ false);
	if (Parts.Num() == 0 || !Parts.Last().Equals(TEXT("DreamHost.uproject"), ESearchCase::IgnoreCase))
	{
		return NormalizedProject;
	}
	Parts.Last() = Parts.Last() == TEXT("dreamhost.uproject") ? TEXT("sumihost.uproject") : TEXT("SumiHost.uproject");
	if (Parts.Num() >= 2 && Parts[Parts.Num() - 2].Equals(TEXT("DreamHost"), ESearchCase::IgnoreCase))
	{
		FString& Folder = Parts[Parts.Num() - 2];
		Folder = Folder == TEXT("dreamhost") ? TEXT("sumihost") : TEXT("SumiHost");
	}
	return FString::Join(Parts, TEXT("/"));
}

FString DreamLegacyMigration::RootNameFor(const FString& NormalizedProject, const FString& BaseName, int32 HexDigits)
{
	const FString Hash = DreamHash::Sha256Hex(DreamText::ToUtf8(NormalizedProject));
	return FString::Printf(TEXT("%s-%s"), *DreamStorageRoot::SanitizeName(BaseName), *Hash.Left(HexDigits));
}

FString DreamLegacyMigration::FindLegacyRoot(const FMigrationPaths& Paths)
{
	if (Paths.OldBase.IsEmpty() || Paths.UProject.IsEmpty())
	{
		return FString();
	}
	const FString Legacy = LegacyProjectPath(DreamStorageRoot::NormalizedProjectPath(Paths.UProject));
	const FString BaseName = FPaths::GetBaseFilename(Paths.UProject).Replace(TEXT("DreamHost"), TEXT("SumiHost"));
	for (const int32 Digits : { 6, 8 })
	{
		const FString Root = Paths.OldBase / DreamStorageRoot::EngineVersionDir() / RootNameFor(Legacy, BaseName, Digits);
		if (!IFileManager::Get().DirectoryExists(*Root))
		{
			continue;
		}
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *(Root / TEXT("project.json"))))
		{
			return Root;   // no project.json: as DreamStorageRoot::ProjectJsonMatches, a folder of this project
		}
		dream::json::Value Value;
		if (!dream::json::Parse(DreamText::ToUtf8(Text), Value))
		{
			return Root;
		}
		const std::string& Stored = Value.Get("uproject").AsString();
		if (Stored.empty() || DreamText::Utf8ToFString(Stored) == Legacy)
		{
			return Root;
		}
	}
	return FString();
}

DreamLegacyMigration::FResult DreamLegacyMigration::MigrateFiles(const FMigrationPaths& Paths, bool bRunLogsInline)
{
	using namespace DreamLegacyMigrationLocal;
	FResult Result;
	if (Paths.OldBase.IsEmpty() || Paths.NewBase.IsEmpty() || !IFileManager::Get().DirectoryExists(*Paths.OldBase))
	{
		return Result;
	}
	IFileManager& Files = IFileManager::Get();

	// 1. The storage root, when the new one does not exist yet. The marker <new root>/.migrating-root names the old root
	//    until the copy is complete, so a start cut short copies the missing files at the next start.
	const FString NewRoot = DreamStorageRoot::MakeRootPath(Paths.NewBase, Paths.UProject, 6);
	const FString RootMarker = NewRoot / RootMarkerName;
	FString OldRoot;
	if (!Files.DirectoryExists(*NewRoot))
	{
		OldRoot = FindLegacyRoot(Paths);
		if (!OldRoot.IsEmpty() && (!Files.MakeDirectory(*NewRoot, /*Tree*/ true)
			|| !FFileHelper::SaveStringToFile(OldRoot, *RootMarker, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
		{
			UE_LOG(LogDream, Warning, TEXT("Could not create %s; the Sumi storage folder %s is not copied."), *NewRoot, *OldRoot);
			OldRoot.Reset();
		}
	}
	else if (Files.FileExists(*RootMarker))
	{
		OldRoot = ReadMarker(RootMarker);
	}
	if (!OldRoot.IsEmpty() && Files.DirectoryExists(*OldRoot))
	{
		Result.RootFiles = CopyTree(OldRoot, NewRoot, [](const FString& Relative)
			{
				return IsSkippedRootFile(Relative) || Relative.StartsWith(TEXT("RunLogs/"));
			});
		if (Files.DirectoryExists(*(OldRoot / TEXT("RunLogs"))))
		{
			if (bRunLogsInline)
			{
				Result.RunLogFiles = CopyTree(OldRoot / TEXT("RunLogs"), NewRoot / TEXT("RunLogs"), &IsSkippedRootFile);
			}
			else
			{
				FFileHelper::SaveStringToFile(OldRoot, *(NewRoot / RunLogsMarker), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				Result.bRunLogsPending = true;
			}
		}
		Files.Delete(*RootMarker, false, true, true);
		Result.OldRoot = OldRoot;
		Result.NewRoot = NewRoot;
	}

	// 2. The default scratch folder, once, while the setting is empty (a custom folder keeps its files where they are).
	const FString OldScratch = Paths.OldScratch();
	if (Paths.ScratchSetting.IsEmpty() && Files.DirectoryExists(*OldScratch) && !Files.FileExists(*(Paths.NewBase / ScratchMarker)))
	{
		Result.ScratchFiles = CopyTree(OldScratch, Paths.NewScratch(), &IsSkippedRootFile);
		FFileHelper::SaveStringToFile(OldScratch, *(Paths.NewBase / ScratchMarker), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// 3. The user themes (and an exported Built-in folder), once.
	const FString OldThemes = Paths.OldBase / TEXT("Themes");
	if (Files.DirectoryExists(*OldThemes) && !Files.FileExists(*(Paths.NewBase / ThemesMarker)))
	{
		Result.ThemeFiles = CopyTree(OldThemes, Paths.NewBase / TEXT("Themes"), &IsSkippedRootFile);
		FFileHelper::SaveStringToFile(OldThemes, *(Paths.NewBase / ThemesMarker), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	return Result;
}

void DreamLegacyMigration::StartRunLogCopy(const FString& OldRoot, const FString& NewRoot)
{
	using namespace DreamLegacyMigrationLocal;
	const FString Marker = NewRoot / RunLogsMarker;
	if (OldRoot.IsEmpty() || !IFileManager::Get().FileExists(*Marker))
	{
		return;
	}
	GStopRunLogCopy.store(false);
	GRunLogCopies.fetch_add(1);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [OldRoot, NewRoot, Marker]()
	{
		const int32 Copied = CopyTree(OldRoot / TEXT("RunLogs"), NewRoot / TEXT("RunLogs"), &IsSkippedRootFile, &GStopRunLogCopy);
		if (!GStopRunLogCopy.load())
		{
			IFileManager::Get().Delete(*Marker, false, true, true);
			UE_LOG(LogDream, Log, TEXT("Copied %d Sumi run logs from %s."), Copied, *OldRoot);
		}
		GRunLogCopies.fetch_sub(1);
	});
}

void DreamLegacyMigration::StopRunLogCopy()
{
	DreamLegacyMigrationLocal::GStopRunLogCopy.store(true);
}

bool DreamLegacyMigration::IsRunLogCopyIdle()
{
	return DreamLegacyMigrationLocal::GRunLogCopies.load() == 0;
}

void DreamLegacyMigration::RunStartupMigration(int32 SectionsCopied)
{
	using namespace DreamLegacyMigrationLocal;
	for (const FString& Ini : GDirtyIniFiles)
	{
		if (GConfig != nullptr && !GConfig->Flush(/*bRemoveFromCache*/ false, Ini))
		{
			UE_LOG(LogDream, Warning, TEXT("Could not write %s; the Sumi settings copied into it apply to this session only."), *Ini);
		}
	}
	GDirtyIniFiles.Reset();

	FResult Result;
	Result.SectionsCopied = SectionsCopied;
	if (!FDreamInstances::HasSessionOverride())   // -DreamStorageRoot (the G6 driver) is a session of its own
	{
		const FMigrationPaths Paths = FMigrationPaths::ForThisUser();
		const FResult Files = MigrateFiles(Paths, /*bRunLogsInline*/ false);
		Result.OldRoot = Files.OldRoot;
		Result.NewRoot = Files.NewRoot;
		Result.RootFiles = Files.RootFiles;
		Result.bRunLogsPending = Files.bRunLogsPending;
		Result.ScratchFiles = Files.ScratchFiles;
		Result.ThemeFiles = Files.ThemeFiles;
		// A copy cut short by an earlier exit resumes here.
		const FString NewRoot = DreamStorageRoot::MakeRootPath(Paths.NewBase, Paths.UProject, 6);
		StartRunLogCopy(ReadMarker(NewRoot / RunLogsMarker), NewRoot);
	}
	if (Result.bAnyCopied())
	{
		UE_LOG(LogDream, Display, TEXT("Dream Python Editor copied your Sumi data (the old files stay as a backup): %d settings sections, storage folder %s (%d files%s), %d scratch files, %d themes."),
			Result.SectionsCopied, Result.OldRoot.IsEmpty() ? TEXT("none") : *FString::Printf(TEXT("%s -> %s"), *Result.OldRoot, *Result.NewRoot),
			Result.RootFiles, Result.bRunLogsPending ? TEXT("; run logs follow in the background") : TEXT(""), Result.ScratchFiles,
			Result.ThemeFiles);
	}
}
