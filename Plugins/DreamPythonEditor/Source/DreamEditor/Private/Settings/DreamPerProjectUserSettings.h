// SPDX-License-Identifier: Apache-2.0
// Per-project user settings: the clean-shutdown session (TDD §10.3, §11.1.1, D21): config=EditorPerProjectUserSettings,
// <Project>/Saved/Config/<Platform>/EditorPerProjectUserSettings.ini. SaveConfig at OnPreExit and after tab changes with a
// 30 s debounce, never per keystroke (§10.3). Game thread. Body: shell slice.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DreamPerProjectUserSettings.generated.h"

UENUM()
enum class EDreamTabKind : uint8
{
	Scratch,
	File,
	Recovered,
};

/** One open tab (§10.3 FDreamTabState, the slice-A fields). */
USTRUCT()
struct FDreamTabState
{
	GENERATED_BODY()

	/** The buffer FGuid as 32 lower-case hex digits (the journal uuid). */
	UPROPERTY()
	FString BufferId;

	UPROPERTY()
	EDreamTabKind Kind = EDreamTabKind::Scratch;

	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString DisplayName;

	UPROPERTY()
	int32 CaretLine = 0;

	UPROPERTY()
	int32 CaretColumn = 0;

	UPROPERTY()
	int32 FirstVisibleLine = 0;

	/** U3 per-tab toggle. */
	UPROPERTY()
	bool bOneUndoStepPerRun = true;

	/** (run group, B40) The user set the toggle on this tab: it decides every tab run; unset, runs follow the per-route
	 *  settings it is seeded from (A56, P2-run-02). */
	UPROPERTY()
	bool bOneUndoStepPerRunSet = false;

	/** U6 namespace: false = Session, true = Isolated. */
	UPROPERTY()
	bool bIsolated = false;

	/** Always true in slice A (plain mode is the only view). */
	UPROPERTY()
	bool bPlainMode = true;

	/** EDT-10 per-tab zoom, 50..300 (percent of the Font size setting). */
	UPROPERTY()
	int32 ZoomPercent = 100;
};

UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Dream Python Editor (this project)"))
class DREAMEDITOR_API UDreamPerProjectUserSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	static const UDreamPerProjectUserSettings* Get() { return GetDefault<UDreamPerProjectUserSettings>(); }
	static UDreamPerProjectUserSettings* GetMutable() { return GetMutableDefault<UDreamPerProjectUserSettings>(); }

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("DreamProject"); }
	/** (slice A choice) Session state only, nothing to edit: not shown in Editor Preferences. (sidebar, R-p1-10) The Project
	 *  Roots page is UDreamProjectTreeSettings (Settings/DreamProjectTreeSettings.h), so Reset to Defaults and Set as Default
	 *  there never touch this state. */
	virtual bool SupportsAutoRegistration() const override { return false; }

	UPROPERTY(config)
	int32 SettingsVersion = 1;

	UPROPERTY(config)
	TArray<FDreamTabState> OpenTabs;

	UPROPERTY(config)
	FString ActiveTab;

	/** The output area's share of the centre column (WS-02, proposed 40%); written by OnSlotResized. */
	UPROPERTY(config)
	float OutputAreaFraction = 0.4f;

	/** 0 Output, 1 REPL, 2 Problems. */
	UPROPERTY(config)
	int32 ActiveOutputTab = 0;

	/** Untitled-N continues across sessions. */
	UPROPERTY(config)
	int32 NextUntitledNumber = 1;

	/** The RUN-26 hint chip was dismissed for good (§4.9.4): set by the first RunLineAdvance or a click on the chip, saved at
	 *  once, so the chip never shows again in this project, also after a restart (T-KEY-018). */
	UPROPERTY(config)
	bool bRunLineAdvanceHintDismissed = false;

	/** (buffers, B83) File ▸ Recent Files (BUF-03 M1): normalized paths, most recent first, at most
	 *  FDreamBufferManager::MaxRecentFiles; a file opened, saved under a new name or closed moves to the front. */
	UPROPERTY(config)
	TArray<FString> RecentFiles;

	// ---- (sidebar) PO-05: the side bar and its Project tree's view state, per user and project. The roots themselves are in
	// UDreamProjectTreeSettings (R-p1-10). A secondary editor instance changes none of these (its session is not saved).

	/** Roots and the Scratches section start expanded; these (normalized absolute paths) were collapsed. */
	UPROPERTY(config)
	TArray<FString> CollapsedRoots;

	/** The folders below the roots that are expanded (normalized absolute paths, at most 1000, oldest dropped). */
	UPROPERTY(config)
	TArray<FString> ExpandedFolders;

	/** The side bar's width in Slate units (default 240, at least 160); written when its splitter is dragged. */
	UPROPERTY(config)
	float SideBarWidth = 240.f;

	/** The Project section's share of the side bar's height (default 0.6, Structure gets the rest). */
	UPROPERTY(config)
	float SideBarProjectFraction = 0.6f;
};
