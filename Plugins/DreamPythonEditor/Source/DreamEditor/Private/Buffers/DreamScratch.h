// SPDX-License-Identifier: Apache-2.0
// (scratch, PO-05) The scratch folder: unnamed tabs are files scratch_N.py there, auto-saved (BUF-01 as amended by PO-05),
// and the curated samples are seeded into <folder>/Samples once. Per user, shared by every project: the test override
// (SetFolderForTest) wins, then -DreamScratchFolder=<dir>, then UDreamUserSettings::ScratchFolder (when allowed, R-p1-2),
// then %LOCALAPPDATA%\DreamPythonEditor\Scratches. The folder is created on first use (EnsureFolder), never in StartupModule (B1).
// Game thread unless a function says otherwise. Body: DreamScratch.cpp (scratch).
#pragma once

#include "CoreMinimal.h"

namespace DreamScratch
{
	// ---- API for the side bar (sidebar's Project tree, Scratches section) ----

	/** The scratch folder: normalized absolute, forward slashes, no trailing slash. No I/O (it may not exist yet). Any
	 *  thread: a cached copy under a lock, computed and refreshed on the game thread (a worker that finds the cache cold
	 *  gets an unchecked answer that is not cached). */
	DREAMEDITOR_API FString GetFolder();
	/** True when Path is the scratch folder or lies under it (subfolders such as Samples included; case-insensitive on
	 *  Windows). No I/O. Any thread. */
	DREAMEDITOR_API bool IsScratchPath(const FString& Path);
	/** Fires on the game thread after the folder's path or existence changed: the setting was edited, the test override
	 *  changed, or EnsureFolder created the folder (R-p1-7). */
	DREAMEDITOR_API FSimpleMulticastDelegate& OnFolderChanged();
	/** Test hook: the folder is Folder from now on (empty = no override; -DreamScratchFolder still wins over the setting, the
	 *  test override wins over both). Broadcasts OnFolderChanged when the answer changes. Specs put back what they found. */
	DREAMEDITOR_API void SetFolderForTest(const FString& Folder);
	DREAMEDITOR_API FString GetFolderForTest();

	// ---- Scratch files (buffers, SDreamTab) ----

	/** <folder>/Samples. */
	DREAMEDITOR_API FString GetSamplesFolder();
	/** Creates the folder when missing and proves it writable (a probe file made and removed; a success is remembered per
	 *  folder for the session unless bReprobe, a failure is probed again on the next call). False with OutError when it
	 *  cannot; the caller then falls back to a journaled Untitled-N. */
	DREAMEDITOR_API bool EnsureFolder(FText& OutError, bool bReprobe = false);
	/** A new unnamed tab's file: the lowest empty scratch_N.py that this editor made and then closed unused this session and
	 *  that no buffer needs (R-p1-8, scratch-p3-m1: never a file of another editor or an earlier session), else a new one with
	 *  the lowest free N (a name taken by a file, or by the path of an open or closed tab, is not free), made empty at once
	 *  (CREATE_NEW, so it shows in the tree and a second editor cannot take the same name). Its path; empty with OutError
	 *  when the folder cannot be written. */
	DREAMEDITOR_API FString CreateNextFile(FText& OutError);
	/** A scratch file's tab closed clean and empty: when this editor made the file this session, CreateNextFile may use it
	 *  again (scratch-p3-m1). FDreamBufferManager::Close calls it. */
	DREAMEDITOR_API void NoteClosedUnused(const FString& Path);
	/** scratch_<N>.py directly in the folder (not in a subfolder). */
	DREAMEDITOR_API bool IsScratchFileName(const FString& Path);
	/** The folder comes from -DreamScratchFolder (and no test override): a session folder such as the tier-4 runner's. The
	 *  startup migration of the developer's restored Untitled-N tabs waits for a session that uses their own folder. */
	DREAMEDITOR_API bool IsCommandLineFolder();
	/** "Scratch folder is not writable: <path> (<why>)": logged once per folder per session; true when this call logged
	 *  it (the caller shows it in the tab too). */
	DREAMEDITOR_API bool WarnNotWritableOnce(const FText& Why);
	/** (R-p1-2) The setting's refusal ("Scratch folder <path> is not allowed: <why>; the default folder is used"), once per
	 *  refused value: the text to show, then empty. */
	DREAMEDITOR_API FText TakeSettingWarning();
	/** Test hook: the once-per-folder and once-per-value warnings show again. */
	DREAMEDITOR_API void ResetWarningsForTest();
	/** (R-p1-2) Why Folder may not be the scratch folder (empty when it may): not absolute, a drive or share root
	 *  ("//server/share"), the user's profile folder or a folder holding it (C:/Users), the project folder or a folder
	 *  holding it, the engine folder (or inside or holding it), or a Project tree root (or a folder holding one). The roots
	 *  are checked on the game thread only; a worker's cold-cache answer gets every other check. */
	DREAMEDITOR_API FText WhyNotAllowed(const FString& Folder);

	// ---- Samples (Resources/Samples/*.py -> <folder>/Samples) ----

	/** The plugin's Resources/Samples folder. */
	DREAMEDITOR_API FString GetBundledSamplesFolder();
	/** Copies the bundled samples that are missing in <folder>/Samples; never overwrites. Returns the number copied, -1 when
	 *  the folder cannot be written or there is nothing bundled (OutError says why); OutFailed counts the copies that
	 *  failed (each also logged). Any thread (file I/O only; Help > Restore Sample Scripts runs it on the game thread). */
	DREAMEDITOR_API int32 CopyMissingSamples(FText& OutError, int32* OutFailed = nullptr);
	/** The first-use seeding: once per user (UDreamUserSettings::bSamplesSeeded, set only once every sample is there), so
	 *  deleted samples do not come back. Called when a scratch file is made; the copies run on a worker (m7) and the flag is
	 *  written on the game thread after. A folder of a session override (the test override, -DreamScratchFolder) keeps a
	 *  marker file of its own instead and never touches the user's flag. */
	DREAMEDITOR_API void SeedSamplesOnce();
	/** Test hook: a seeding started by SeedSamplesOnce is still running. */
	DREAMEDITOR_API bool IsSeedingForTest();
	/** Test hook (R-p1-12): when the seeding last wrote bSamplesSeeded to EditorSettings.ini (FPlatformTime::Seconds; 0 =
	 *  never): the one write of the user settings file the scratch files make. */
	DREAMEDITOR_API double GetLastFlagWriteSecondsForTest();
}
