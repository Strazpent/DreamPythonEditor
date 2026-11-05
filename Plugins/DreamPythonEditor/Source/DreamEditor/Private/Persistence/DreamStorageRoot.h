// SPDX-License-Identifier: Apache-2.0
// The storage-root rules of TDD §10.1 (U29, D17, RV-08) as small helpers shared by FDreamJournal::ResolveStorageRoot (the
// platform default and the temp fallback, local, on the game thread) and FDreamInstances (storage.root_override, probed on
// its DreamStorageProbe thread; the cross-root candidates): the base folders, <base>/<EngineVer>/<Name>-<hex6> with the
// 8-digit collision rule, the engine and project tree refusals, the writability probe, project.json and owner.json.
// Stateless; any thread unless a function says otherwise. Body: DreamStorageRoot.cpp (instances).
#pragma once

#include "CoreMinimal.h"

struct FDreamStorageRoot;

namespace DreamStorageRoot
{
	/** "<major>.<minor>" of FEngineVersion::Current() (§10.1 <EngineVer>). */
	DREAMEDITOR_API FString EngineVersionDir();
	/** The normalised final .uproject path the hash is taken over (§10.1): subst, junctions and links give one hash. */
	DREAMEDITOR_API FString NormalizedProjectPath(const FString& UProjectPath);
	/** <Name>: the .uproject base name reduced to [A-Za-z0-9_-], at most 40 characters (U29). */
	DREAMEDITOR_API FString SanitizeName(const FString& BaseName);
	/** %LOCALAPPDATA%\DreamPythonEditor (FPlatformProcess::UserSettingsDir) and <user temp>\Dream: the bases storage.root_override replaces. */
	DREAMEDITOR_API FString PlatformDefaultBase();
	DREAMEDITOR_API FString TempBase();
	/** %LOCALAPPDATA%\DreamPythonEditor\<EngineVer>\projects.json: always under the platform default base, never an override (§10.1). */
	DREAMEDITOR_API FString ProjectsJsonPath();
	/** <Base>/<EngineVer>/<Name>-<hex Digits> for this project (no I/O). */
	DREAMEDITOR_API FString MakeRootPath(const FString& Base, const FString& UProjectPath, int32 HexDigits = 6);
	/** §10.1: never under the engine directory, never inside the project tree. */
	DREAMEDITOR_API bool IsAllowedRoot(const FString& Root);
	/** Creates the folder and proves it writable with a probe file (blocking file I/O: local roots, or the probe thread). */
	DREAMEDITOR_API bool PrepareWritable(const FString& Root);
	/** <root>/project.json names the full project path; another path there is a 24-bit collision (§10.1). */
	DREAMEDITOR_API bool ProjectJsonMatches(const FString& Root, const FString& NormalizedProject);
	/** Writes <root>/project.json once (temp, flush, atomic replace). */
	DREAMEDITOR_API void WriteProjectJson(const FString& Root, const FString& NormalizedProject, const FString& Name);
	/**
	 * The root under Base for this project after the collision rule, fully checked and prepared: allowed (§10.1), its
	 * folder created and writable, project.json written. False (OutRoot unset) when it cannot be used. bIsNew says whether
	 * the hash folder existed before. Blocking file I/O.
	 */
	DREAMEDITOR_API bool PrepareRoot(const FString& Base, const FString& UProjectPath, FDreamStorageRoot& OutRoot);
	/** The base folder a root was built on: <base> of <base>/<EngineVer>/<hash>. */
	DREAMEDITOR_API FString BaseOfRoot(const FString& Root);
}
