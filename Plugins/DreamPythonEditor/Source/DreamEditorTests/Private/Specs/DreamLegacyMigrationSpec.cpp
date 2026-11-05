// SPDX-License-Identifier: Apache-2.0
// (rename, PO-06 phase 3, A246) Dream.Migration (any editor): the one-time copy of a user's "Sumi" data. Every case
// works on its own folder under <Project>/Saved/DreamTests/Migration-<id> with a fake old base, new base and project
// path, and on an ini file of its own loaded into the config cache; nothing reads or writes the user's real
// %LOCALAPPDATA%\Sumi, %LOCALAPPDATA%\DreamPythonEditor or settings files.
//  - CopySection: key by key with repeated keys in order, values remapped, only into an absent section, the old one kept;
//  - RemapPath: the old default scratch folder in bare paths and struct text, either slash, any case, boundaries;
//  - LegacyProjectPath and RootNameFor: DreamHost maps to SumiHost's hash folder, other projects keep theirs;
//  - MigrateFiles: the storage root (no lock files, no project.json), scratch files with Samples, themes, the old tree
//    byte for byte unchanged, a second run copying nothing, a deleted copy not coming back, a custom scratch folder left
//    alone, a copy cut short resumed, run logs on the worker;
//  - the old REPL history header and the old theme format still read; the CoreRedirects and the legacy tab ID.
#include "Support/DreamTestUtil.h"
#include "Buffers/DreamHash.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamReplHistory.h"
#include "Persistence/DreamLegacyMigration.h"
#include "Persistence/DreamStorageRoot.h"
#include "UObject/CoreRedirects.h"

namespace DreamLegacyMigrationSpec
{
	void WriteText(const FString& Path, const FString& Text)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString ReadText(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	/** Relative path -> content of every file under Dir (the old tree before and after a migration). */
	TMap<FString, FString> Manifest(const FString& Dir)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *Dir, TEXT("*"), true, false);
		TMap<FString, FString> Out;
		for (const FString& File : Found)
		{
			FString Relative = File;
			FPaths::MakePathRelativeTo(Relative, *(Dir / TEXT("")));
			TArray<uint8> Bytes;
			FFileHelper::LoadFileToArray(Bytes, *File);
			Out.Add(Relative, FString::Printf(TEXT("%d:%s"), Bytes.Num(), *DreamHash::Sha256Hex(std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num()))));
		}
		return Out;
	}
}

BEGIN_DEFINE_SPEC(FDreamLegacyMigrationSpec, "Dream.Migration", DreamTest::AnyEditorFlags)
	FString Dir;
	DreamLegacyMigration::FMigrationPaths Paths;
	FString OldRoot;
	void MakeOldTree();
END_DEFINE_SPEC(FDreamLegacyMigrationSpec)

void FDreamLegacyMigrationSpec::MakeOldTree()
{
	using namespace DreamLegacyMigrationSpec;
	const FString Legacy = DreamLegacyMigration::LegacyProjectPath(DreamStorageRoot::NormalizedProjectPath(Paths.UProject));
	OldRoot = Paths.OldBase / DreamStorageRoot::EngineVersionDir() / DreamLegacyMigration::RootNameFor(Legacy, TEXT("SumiHost"), 6);
	WriteText(OldRoot / TEXT("project.json"), FString::Printf(TEXT("{\"schema\": 1, \"uproject\": \"%s\", \"name\": \"SumiHost\"}"), *Legacy));
	WriteText(OldRoot / TEXT("instance.lock"), TEXT("{\"pid\": 1}"));
	WriteText(OldRoot / TEXT("journal/main/wal.1.sjl"), TEXT("SUMIJRNL journal bytes"));
	WriteText(OldRoot / TEXT("journal/main/ns.lock"), TEXT("lock"));
	WriteText(OldRoot / TEXT("history/index.1.shi"), TEXT("SUMIHIDX index"));
	WriteText(OldRoot / TEXT("Repl/history.jsonl"), TEXT("{\"format\":\"sumi.repl-history\",\"schema\":1}\n"));
	WriteText(OldRoot / TEXT("audit/audit-2026-11-04.jsonl"), TEXT("{}\n"));
	WriteText(OldRoot / TEXT("state/breakpoints.json"), TEXT("{\"breakpoints\": []}"));
	WriteText(OldRoot / TEXT("RunLogs/20261104-120000/run-1.log"), TEXT("# sumi-run-log 1\n"));
	WriteText(OldRoot / TEXT("RunLogs/20261104-120000/run-2.log"), TEXT("# sumi-run-log 1\n"));
	WriteText(Paths.OldScratch() / TEXT("scratch_1.py"), TEXT("import sumi_editor\n"));
	WriteText(Paths.OldScratch() / TEXT("Samples/demo.py"), TEXT("print('demo')\n"));
	WriteText(Paths.OldBase / TEXT("Themes/Mine.json"), TEXT("{\"format\": \"sumi-theme\", \"version\": 1, \"id\": \"Mine\", \"name\": \"Mine\"}"));
}

void FDreamLegacyMigrationSpec::Define()
{
	using namespace DreamLegacyMigrationSpec;

	BeforeEach([this]()
	{
		Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTests") / (TEXT("Migration-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
		Paths = DreamLegacyMigration::FMigrationPaths();
		Paths.OldBase = Dir / TEXT("Local/Sumi");
		Paths.NewBase = Dir / TEXT("Local/DreamPythonEditor");
		Paths.UProject = Dir / TEXT("Projects/DreamHost/DreamHost.uproject");
		OldRoot.Reset();
	});
	AfterEach([this]()
	{
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (!DreamLegacyMigration::IsRunLogCopyIdle() && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.02f);
		}
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	});

	It("copies an old ini section key by key into an absent new section and keeps the old one", [this]()
	{
		const FString Ini = Dir / TEXT("Settings.ini");
		WriteText(Ini, TEXT("[/Script/SumiEditor.SumiPerProjectUserSettings]\nOutputAreaFraction=0.5\nRecentFiles=C:/a.py\nRecentFiles=C:/Users/x/AppData/Local/Sumi/Scratches/scratch_2.py\nRecentFiles=C:/c.py\n"));
		GConfig->LoadFile(Ini);
		const FString OldScratch = TEXT("C:/Users/x/AppData/Local/Sumi/Scratches");
		const FString NewScratch = TEXT("C:/Users/x/AppData/Local/DreamPythonEditor/Scratches");
		auto Remap = [&](FName, const FString& Value) { return DreamLegacyMigration::RemapPath(Value, OldScratch, NewScratch); };
		const int32 Copied = DreamLegacyMigration::CopySection(Ini, TEXT("/Script/SumiEditor.SumiPerProjectUserSettings"),
			TEXT("/Script/DreamEditor.DreamPerProjectUserSettings"), Remap);
		TestEqual(TEXT("four keys copied"), Copied, 4);
		TArray<FString> Recent;
		GConfig->GetArray(TEXT("/Script/DreamEditor.DreamPerProjectUserSettings"), TEXT("RecentFiles"), Recent, Ini);
		const TArray<FString> Expected{ TEXT("C:/a.py"), NewScratch + TEXT("/scratch_2.py"), TEXT("C:/c.py") };
		TestTrue(*FString::Printf(TEXT("the repeated key keeps its three values in order (%s)"), *FString::Join(Recent, TEXT(", "))), Recent == Expected);
		FString Fraction;
		GConfig->GetString(TEXT("/Script/DreamEditor.DreamPerProjectUserSettings"), TEXT("OutputAreaFraction"), Fraction, Ini);
		TestEqual(TEXT("a plain key"), Fraction, FString(TEXT("0.5")));
		TArray<FString> OldRecent;
		GConfig->GetArray(TEXT("/Script/SumiEditor.SumiPerProjectUserSettings"), TEXT("RecentFiles"), OldRecent, Ini);
		TestEqual(TEXT("the old section is unchanged"), OldRecent.Num() == 3 && OldRecent[1].Contains(TEXT("/Sumi/Scratches/")), true);

		// Idempotent: the new section exists now, so a second copy (even after the old one changed) copies nothing.
		GConfig->SetString(TEXT("/Script/SumiEditor.SumiPerProjectUserSettings"), TEXT("OutputAreaFraction"), TEXT("0.9"), Ini);
		TestEqual(TEXT("a second copy copies nothing"), DreamLegacyMigration::CopySection(Ini, TEXT("/Script/SumiEditor.SumiPerProjectUserSettings"),
			TEXT("/Script/DreamEditor.DreamPerProjectUserSettings"), Remap), 0);
		GConfig->GetString(TEXT("/Script/DreamEditor.DreamPerProjectUserSettings"), TEXT("OutputAreaFraction"), Fraction, Ini);
		TestEqual(TEXT("the new value is kept"), Fraction, FString(TEXT("0.5")));
		TestEqual(TEXT("a missing old section copies nothing"), DreamLegacyMigration::CopySection(Ini, TEXT("/Script/SumiEditor.Nothing"),
			TEXT("/Script/DreamEditor.Nothing"), Remap), 0);

		// The flushed file holds both sections.
		GConfig->Flush(false, Ini);
		const FString Text = ReadText(Ini);
		TestTrue(TEXT("the file has the new section"), Text.Contains(TEXT("[/Script/DreamEditor.DreamPerProjectUserSettings]")));
		TestTrue(TEXT("the file still has the old section"), Text.Contains(TEXT("[/Script/SumiEditor.SumiPerProjectUserSettings]")));
		GConfig->UnloadFile(Ini);
	});

	It("remaps the old default scratch folder in paths and struct text only at a path boundary", [this]()
	{
		const FString Old = TEXT("C:/Users/x/AppData/Local/Sumi/Scratches");
		const FString New = TEXT("C:/Users/x/AppData/Local/DreamPythonEditor/Scratches");
		TestEqual(TEXT("a file"), DreamLegacyMigration::RemapPath(Old + TEXT("/scratch_1.py"), Old, New), New + TEXT("/scratch_1.py"));
		TestEqual(TEXT("the folder itself"), DreamLegacyMigration::RemapPath(Old, Old, New), New);
		TestEqual(TEXT("any case"), DreamLegacyMigration::RemapPath(TEXT("c:/users/x/appdata/local/sumi/scratches/Samples/demo.py"), Old, New),
			New + TEXT("/Samples/demo.py"));
		TestEqual(TEXT("backslashes"), DreamLegacyMigration::RemapPath(TEXT("C:\\Users\\x\\AppData\\Local\\Sumi\\Scratches\\a.py"), Old, New),
			FString(TEXT("C:\\Users\\x\\AppData\\Local\\DreamPythonEditor\\Scratches\\a.py")));
		TestEqual(TEXT("struct text"), DreamLegacyMigration::RemapPath(TEXT("(BufferId=\"1\",Path=\"C:/Users/x/AppData/Local/Sumi/Scratches/scratch_3.py\")"), Old, New),
			FString(TEXT("(BufferId=\"1\",Path=\"C:/Users/x/AppData/Local/DreamPythonEditor/Scratches/scratch_3.py\")")));
		TestEqual(TEXT("not a longer folder name"), DreamLegacyMigration::RemapPath(Old + TEXT("2/a.py"), Old, New), Old + TEXT("2/a.py"));
		TestEqual(TEXT("another folder"), DreamLegacyMigration::RemapPath(TEXT("D:/Work/a.py"), Old, New), FString(TEXT("D:/Work/a.py")));
	});

	It("maps DreamHost to SumiHost's old hash folder and keeps other projects' hashes", [this]()
	{
		const FString Host = TEXT("c:/users/anima/desktop/unrealpythoneditor/dreamhost/dreamhost.uproject");
		TestEqual(TEXT("the renamed host"), DreamLegacyMigration::LegacyProjectPath(Host),
			FString(TEXT("c:/users/anima/desktop/unrealpythoneditor/sumihost/sumihost.uproject")));
		TestEqual(TEXT("another project"), DreamLegacyMigration::LegacyProjectPath(TEXT("d:/games/mygame/mygame.uproject")),
			FString(TEXT("d:/games/mygame/mygame.uproject")));
		TestEqual(TEXT("a folder that only contains the name"), DreamLegacyMigration::LegacyProjectPath(TEXT("d:/dreamhosting/x.uproject")),
			FString(TEXT("d:/dreamhosting/x.uproject")));
		// The real host's old folder name, computed independently (SHA-256 of the lower-case path, 6 hex digits).
		TestEqual(TEXT("SumiHost's old hash folder"), DreamLegacyMigration::RootNameFor(
			TEXT("c:/users/anima/desktop/unrealpythoneditor/sumihost/sumihost.uproject"), TEXT("SumiHost"), 6), FString(TEXT("SumiHost-9eabca")));
		TestEqual(TEXT("the same rule as MakeRootPath"), FPaths::GetCleanFilename(DreamStorageRoot::MakeRootPath(Dir, Paths.UProject, 6)),
			DreamLegacyMigration::RootNameFor(DreamStorageRoot::NormalizedProjectPath(Paths.UProject), TEXT("DreamHost"), 6));
	});

	It("copies the storage root, scratch files and themes once and leaves the old tree unchanged", [this]()
	{
		MakeOldTree();
		const TMap<FString, FString> Before = Manifest(Paths.OldBase);
		const DreamLegacyMigration::FResult First = DreamLegacyMigration::MigrateFiles(Paths, /*bRunLogsInline*/ true);
		const FString NewRoot = DreamStorageRoot::MakeRootPath(Paths.NewBase, Paths.UProject, 6);
		TestEqual(TEXT("found the old root"), First.OldRoot, OldRoot);
		TestEqual(TEXT("into the new root"), First.NewRoot, NewRoot);
		TestEqual(TEXT("root files: journal, history, Repl, audit, state"), First.RootFiles, 5);
		TestEqual(TEXT("run logs inline"), First.RunLogFiles, 2);
		TestEqual(TEXT("scratch files with Samples"), First.ScratchFiles, 2);
		TestEqual(TEXT("themes"), First.ThemeFiles, 1);
		IFileManager& Files = IFileManager::Get();
		TestEqual(TEXT("the journal is copied byte for byte"), ReadText(NewRoot / TEXT("journal/main/wal.1.sjl")), FString(TEXT("SUMIJRNL journal bytes")));
		TestTrue(TEXT("the breakpoints are copied"), Files.FileExists(*(NewRoot / TEXT("state/breakpoints.json"))));
		TestTrue(TEXT("a run log is copied"), Files.FileExists(*(NewRoot / TEXT("RunLogs/20261104-120000/run-2.log"))));
		TestFalse(TEXT("no instance.lock"), Files.FileExists(*(NewRoot / TEXT("instance.lock"))));
		TestFalse(TEXT("no ns.lock"), Files.FileExists(*(NewRoot / TEXT("journal/main/ns.lock"))));
		TestFalse(TEXT("no old project.json (PrepareRoot writes the new one)"), Files.FileExists(*(NewRoot / TEXT("project.json"))));
		TestFalse(TEXT("no marker left"), Files.FileExists(*(NewRoot / TEXT(".migrating-root"))));
		TestTrue(TEXT("a sample is copied"), Files.FileExists(*(Paths.NewScratch() / TEXT("Samples/demo.py"))));
		TestTrue(TEXT("the theme is copied"), Files.FileExists(*(Paths.NewBase / TEXT("Themes/Mine.json"))));
		TestTrue(TEXT("the old tree is byte for byte unchanged"), Manifest(Paths.OldBase).OrderIndependentCompareEqual(Before));

		// Idempotent: nothing is copied again, and a copy the user deleted does not come back.
		Files.Delete(*(Paths.NewScratch() / TEXT("scratch_1.py")));
		const DreamLegacyMigration::FResult Second = DreamLegacyMigration::MigrateFiles(Paths, true);
		TestFalse(TEXT("a second run copies nothing"), Second.bAnyCopied());
		TestFalse(TEXT("the deleted scratch copy stays deleted"), Files.FileExists(*(Paths.NewScratch() / TEXT("scratch_1.py"))));
		TestTrue(TEXT("the old tree is still unchanged"), Manifest(Paths.OldBase).OrderIndependentCompareEqual(Before));
	});

	It("resumes a copy cut short, leaves a custom scratch folder alone and copies run logs on the worker", [this]()
	{
		MakeOldTree();
		Paths.ScratchSetting = Dir / TEXT("MyScratches");
		const FString NewRoot = DreamStorageRoot::MakeRootPath(Paths.NewBase, Paths.UProject, 6);
		// A start that died after the first file: the new root exists with its marker and one copied file.
		WriteText(NewRoot / TEXT(".migrating-root"), OldRoot);
		WriteText(NewRoot / TEXT("journal/main/wal.1.sjl"), TEXT("SUMIJRNL journal bytes"));
		const DreamLegacyMigration::FResult Result = DreamLegacyMigration::MigrateFiles(Paths, /*bRunLogsInline*/ false);
		TestEqual(TEXT("the missing root files are copied"), Result.RootFiles, 4);
		TestTrue(TEXT("run logs are left to the worker"), Result.bRunLogsPending);
		TestEqual(TEXT("a custom scratch folder is not copied"), Result.ScratchFiles, 0);
		TestFalse(TEXT("nothing in the new default scratch folder"), IFileManager::Get().DirectoryExists(*Paths.NewScratch()));
		TestFalse(TEXT("the root marker is gone"), IFileManager::Get().FileExists(*(NewRoot / TEXT(".migrating-root"))));
		TestTrue(TEXT("the run-log marker is there"), IFileManager::Get().FileExists(*(NewRoot / TEXT(".migrating-runlogs"))));

		DreamLegacyMigration::StartRunLogCopy(OldRoot, NewRoot);
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (!DreamLegacyMigration::IsRunLogCopyIdle() && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.02f);
		}
		TestTrue(TEXT("the worker finished"), DreamLegacyMigration::IsRunLogCopyIdle());
		TestTrue(TEXT("run log 1 copied"), IFileManager::Get().FileExists(*(NewRoot / TEXT("RunLogs/20261104-120000/run-1.log"))));
		TestTrue(TEXT("run log 2 copied"), IFileManager::Get().FileExists(*(NewRoot / TEXT("RunLogs/20261104-120000/run-2.log"))));
		TestFalse(TEXT("the run-log marker is removed"), IFileManager::Get().FileExists(*(NewRoot / TEXT(".migrating-runlogs"))));
		TestTrue(TEXT("the old run logs are still there"), IFileManager::Get().FileExists(*(OldRoot / TEXT("RunLogs/20261104-120000/run-1.log"))));
	});

	It("finds another project's old root under its own unchanged hash", [this]()
	{
		Paths.UProject = Dir / TEXT("Projects/MyGame/MyGame.uproject");
		const FString Normalized = DreamStorageRoot::NormalizedProjectPath(Paths.UProject);
		const FString Old = Paths.OldBase / DreamStorageRoot::EngineVersionDir() / DreamLegacyMigration::RootNameFor(Normalized, TEXT("MyGame"), 6);
		WriteText(Old / TEXT("journal/main/wal.1.sjl"), TEXT("x"));
		TestEqual(TEXT("the same hash folder"), DreamLegacyMigration::FindLegacyRoot(Paths), Old);
		WriteText(Old / TEXT("project.json"), TEXT("{\"uproject\": \"d:/another/project.uproject\"}"));
		TestEqual(TEXT("a folder of another project (a collision) is not used"), DreamLegacyMigration::FindLegacyRoot(Paths), FString());
	});

	It("reads a REPL history file with the old header", [this]()
	{
		const FString File = Dir / TEXT("repl-history.jsonl");
		WriteText(File, TEXT("{\"format\":\"sumi.repl-history\",\"schema\":1}\n{\"id\":\"a\",\"t\":1,\"src\":\"repl\",\"text\":\"x = 1\"}\n"));
		TArray<FDreamHistoryEntry> Entries;
		TestTrue(TEXT("the old header reads"), FDreamReplHistory::ReadStagedFile(File, Entries));
		TestEqual(TEXT("its entry"), Entries.Num(), 1);
		WriteText(File, TEXT("{\"format\":\"other\",\"schema\":1}\n"));
		TestFalse(TEXT("another format is still refused"), FDreamReplHistory::ReadStagedFile(File, Entries));
	});

	It("redirects the old reflected type names and the old tab ID", [this]()
	{
		const FCoreRedirectObjectName Old(TEXT("/Script/SumiEditor.SumiUserSettings"));
		TestEqual(TEXT("the settings class"), FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Class, Old).ToString(),
			FString(TEXT("/Script/DreamEditor.DreamUserSettings")));
		TestEqual(TEXT("the tab state struct"), FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Struct,
			FCoreRedirectObjectName(TEXT("/Script/SumiEditor.SumiTabState"))).ToString(), FString(TEXT("/Script/DreamEditor.DreamTabState")));
		if (FSlateApplication::IsInitialized() && !FApp::IsUnattended() && FApp::CanEverRender())
		{
			TestEqual(TEXT("the old tab ID opens the new tab"), FGlobalTabmanager::Get()->GetTabTypeForPotentiallyLegacyTab(FName(DreamLegacyMigration::LegacyTabId)),
				FName(FDreamEditorModule::TabIdName));
			// A session started on a layout that names only the old tab ID (the rename group's check, run alone with this
			// switch): the layout restore opened the tab before any test asked for it.
			if (FParse::Param(FCommandLine::Get(), TEXT("DreamExpectRestoredTab")))
			{
				TestTrue(TEXT("a layout with the old tab ID opened the tab at startup"), FDreamEditorModule::Get().GetTab().IsValid());
			}
		}
		else
		{
			AddInfo(TEXT("Headless: the tab spawner and its legacy tab ID are not registered (TDD 3.5)"));
		}
	});
}
