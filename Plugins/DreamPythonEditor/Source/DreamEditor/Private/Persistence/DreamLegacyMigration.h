// SPDX-License-Identifier: Apache-2.0
// (rename, PO-06 phase 3, A246) The one-time migration of a user's data from the product's former name "Sumi" to
// Dream Python Editor. It copies, never moves or deletes: every old path stays as a backup, and a second start copies
// nothing.
//  StartupModule (no file I/O, B1): MigrateConfig copies the ini sections [/Script/SumiEditor.SumiUserSettings]
//    (EditorSettings), SumiPerProjectUserSettings and SumiProjectTreeSettings (EditorPerProjectUserSettings) key by key
//    in the config cache into the Dream sections, only when the Dream section is absent; the settings objects reload;
//    a ScratchFolder equal to the old default becomes empty (the new default); saved scratch paths are remapped.
//  The first ResolveStorageRoot (game thread, before the root is prepared and the journal opens): RunStartupMigration
//    copies the old storage root %LOCALAPPDATA%\Sumi\<EngineVer>\<Project>-<hex6> into the new root (the renamed host
//    project DreamHost maps to SumiHost's old hash folder), the default scratch folder (Samples included) and the user
//    themes, and flushes the ini files MigrateConfig changed. RunLogs copy on a worker afterwards, resumed at the next
//    start if the editor exits first.
// Nothing is logged when there is nothing to copy; a start that copied logs one Display line.
// Owner: rename. Body: DreamLegacyMigration.cpp.
#pragma once

#include "CoreMinimal.h"

namespace DreamLegacyMigration
{
	/** The old and new names of the three per-user settings sections (class paths). */
	struct FSectionPair
	{
		const TCHAR* Old;
		const TCHAR* New;
	};
	DREAMEDITOR_API TArrayView<const FSectionPair> SettingsSections();

	/** Where the migration reads and writes. ForThisUser() is the real one (no I/O); specs build theirs under Saved/. */
	struct FMigrationPaths
	{
		FString OldBase;                // %LOCALAPPDATA%\Sumi
		FString NewBase;                // %LOCALAPPDATA%\DreamPythonEditor
		FString UProject;               // the .uproject of this editor session
		/** The scratch folder setting after MigrateConfig: empty = the default, the only folder that moves. */
		FString ScratchSetting;

		DREAMEDITOR_API static FMigrationPaths ForThisUser();
		FString OldScratch() const { return OldBase / TEXT("Scratches"); }
		FString NewScratch() const { return NewBase / TEXT("Scratches"); }
	};

	/** What a migration copied (zero everywhere on a second start). */
	struct FResult
	{
		int32 SectionsCopied = 0;
		FString OldRoot;                // the storage root copied from (empty: none)
		FString NewRoot;
		int32 RootFiles = 0;            // copied now (RunLogs excluded)
		int32 RunLogFiles = 0;          // copied now (inline only)
		bool bRunLogsPending = false;   // left to StartRunLogCopy (marker <new root>/.migrating-runlogs)
		int32 ScratchFiles = 0;
		int32 ThemeFiles = 0;
		bool bAnyCopied() const
		{
			return SectionsCopied > 0 || RootFiles > 0 || RunLogFiles > 0 || bRunLogsPending || ScratchFiles > 0
				|| ThemeFiles > 0;
		}
	};

	// ---- config (no file I/O) ----

	/**
	 * Copies OldSection of the config cache's IniFile into NewSection key by key (repeated array keys kept, in order),
	 * each value through Remap, when NewSection is absent there and OldSection has keys. Returns the number of keys
	 * copied (0: the new section existed, or nothing to copy). OldSection is left as it was. Game thread.
	 */
	DREAMEDITOR_API int32 CopySection(const FString& IniFile, const TCHAR* OldSection, const TCHAR* NewSection,
		TFunctionRef<FString(FName Key, const FString& Value)> Remap);

	/** StartupModule: the three settings sections of this user and project, then the settings objects reload. Returns
	 *  the sections copied (RunStartupMigration flushes them). Sets the scratch path remap (RemapLegacyPath). */
	DREAMEDITOR_API int32 MigrateConfig();

	/** A path saved under the old default scratch folder, moved to the new default one while the scratch folder
	 *  setting is empty; any other path unchanged. Pure string work, any thread (set in StartupModule, before any
	 *  worker starts). */
	DREAMEDITOR_API FString RemapLegacyPath(const FString& Path);
	/** The same rule for given folders (specs): Path under OldScratch (case-insensitive, either slash) -> NewScratch. */
	DREAMEDITOR_API FString RemapPath(const FString& Path, const FString& OldScratch, const FString& NewScratch);

	// ---- files (blocking file I/O) ----

	/** The .uproject path this project had before the rename: <...>/DreamHost/DreamHost.uproject becomes
	 *  <...>/SumiHost/SumiHost.uproject (the file, and its folder when named DreamHost); any other project keeps its path.
	 *  Input and output normalized like the project hash input. */
	DREAMEDITOR_API FString LegacyProjectPath(const FString& NormalizedProject);
	/** "<Name>-<hex Digits>" of a normalized project path and its .uproject base name, as MakeRootPath names a root. */
	DREAMEDITOR_API FString RootNameFor(const FString& NormalizedProject, const FString& BaseName, int32 HexDigits);
	/** The old root of this project under Paths.OldBase whose project.json names the legacy project path (or has none);
	 *  empty when there is none. */
	DREAMEDITOR_API FString FindLegacyRoot(const FMigrationPaths& Paths);

	/**
	 * Copies what is missing, never overwriting: the old storage root into the new root when the new root does not exist
	 * yet, or while its marker .migrating-root says an earlier copy was cut short (journal, history, Repl, audit, state
	 * with the breakpoints, Logs; never a *.lock file or project.json), the
	 * default scratch folder when the setting is empty (once: marker <NewBase>/.migrated-scratches) and the user themes
	 * (once: marker .migrated-themes). RunLogs: inline when bRunLogsInline, else left to StartRunLogCopy. Blocking.
	 */
	DREAMEDITOR_API FResult MigrateFiles(const FMigrationPaths& Paths, bool bRunLogsInline);
	/** Copies <old root>/RunLogs into <new root>/RunLogs on a worker (missing files only, each through a temp file),
	 *  then removes the marker. Nothing when the marker is absent. */
	DREAMEDITOR_API void StartRunLogCopy(const FString& OldRoot, const FString& NewRoot);
	/** The editor is exiting: a running run-log copy stops after its current file (the marker stays, so the next start
	 *  resumes it). OnPreExit. */
	DREAMEDITOR_API void StopRunLogCopy();
	/** No run-log copy is running (specs). */
	DREAMEDITOR_API bool IsRunLogCopyIdle();

	/** The module's call at the first ResolveStorageRoot: MigrateFiles on this user's paths (none under
	 *  -DreamStorageRoot), the flush of the ini files MigrateConfig changed, the run-log copy, the one log line. */
	DREAMEDITOR_API void RunStartupMigration(int32 SectionsCopied);

	/** The old tab ID, registered as a legacy tab type so a saved layout reopens the tab where it was. */
	inline constexpr const TCHAR* LegacyTabId = TEXT("SumiEditor.Main");
}
