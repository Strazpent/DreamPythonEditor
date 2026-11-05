// SPDX-License-Identifier: Apache-2.0
// Global user settings (TDD §11.1.1 UDreamUserSettings, D21): config=EditorSettings, so EditorSettings.ini in the
// engine-version-wide user Saved/Config; Editor Preferences > Plugins > Dream. Game thread. Body: shell slice.
// (slice A choice) Slice A has the user layer only: no FDreamSettingKeys registry, no bOverride_ pattern and no policy
// (§11.1.2, §11.2 are M4); the transaction defaults per route live here instead of the project layer.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Run/DreamRunTypes.h"
#include "DreamUserSettings.generated.h"

/** layout.output_position (WS-03, PO-01): global user only, never a project default or policy key. */
UENUM()
enum class EDreamOutputPosition : uint8
{
	Top,
	Bottom,
};

/** run.presentation (RT-03). (slice A choice) Live (docked) Preview is M4: slice A runs it as the Run window. */
UENUM()
enum class EDreamRunPresentationSetting : uint8
{
	RunWindow UMETA(DisplayName = "Run window"),
	LiveDocked UMETA(DisplayName = "Live (docked) Preview"),
};

/** (chrome) The PO-04 View toggles by name (menus, the icon row, specs): UDreamUserSettings::Get/SetViewSetting. */
enum class EDreamViewSetting : uint8
{
	ReplTab,
	ProblemsTab,
	ReplPrompt,
	RunHeaders,
	StatusLine,
	LineNumbers,
	// (sidebar) PO-05: the side bar's two sections, both on by default.
	ProjectPanel,
	StructurePanel,
};

UCLASS(config = EditorSettings, meta = (DisplayName = "Dream Python Editor"))
class DREAMEDITOR_API UDreamUserSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:

	static const UDreamUserSettings* Get() { return GetDefault<UDreamUserSettings>(); }
	static UDreamUserSettings* GetMutable() { return GetMutableDefault<UDreamUserSettings>(); }

	// UDeveloperSettings: Editor Preferences > Plugins > Dream
	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Dream"); }

	/** Bumped when keys are migrated in PostInitProperties (§12.4 settings ini). */
	UPROPERTY(config)
	int32 SettingsVersion = 1;

	/** layout.output_position: where the output area sits (WS-03). Applies at once. */
	UPROPERTY(config, EditAnywhere, Category = "Layout", meta = (DisplayName = "Output position"))
	EDreamOutputPosition OutputPosition = EDreamOutputPosition::Top;

	/** run.pump: the D2 kill switch. Off: no watchdog, pending calls, pump, pre-processor or Run window (§5.5.5). */
	UPROPERTY(config, EditAnywhere, Category = "Runs", meta = (DisplayName = "Keep the editor live during runs"))
	bool bRunPump = true;

	/** run.presentation (RT-03). */
	UPROPERTY(config, EditAnywhere, Category = "Runs", meta = (DisplayName = "Run presentation"))
	EDreamRunPresentationSetting RunPresentation = EDreamRunPresentationSetting::RunWindow;

	/** run.cleanup_interrupt_s: Interrupt cleanup appears this long after the first Stop delivery (RT-06). */
	UPROPERTY(config, EditAnywhere, Category = "Runs", meta = (DisplayName = "Offer Interrupt cleanup after (s)", ClampMin = "1", ClampMax = "600"))
	float CleanupInterruptSeconds = 10.f;

	/** run.transaction_defaults.<route> (RT-08, §5.2.1): One undo step per run, per route. (run group, B40, P2-run-02) A
	 *  tab's selection, line and cell runs follow this until the user sets the tab's toggle, which then decides every tab
	 *  run (FDreamRunService::ResolveTabTransaction, A56); a re-run whose tab is closed follows it too. */
	UPROPERTY(config, EditAnywhere, Category = "Undo", meta = (DisplayName = "One undo step per run: selection, line and cell runs"))
	bool bTransactionSelectionAndLine = true;
	/** The same for a tab's buffer runs (Ctrl+Shift+Enter), and the value a new tab's toggle shows; Run File also needs
	 *  the Run File default (§5.9.1, A56). */
	UPROPERTY(config, EditAnywhere, Category = "Undo", meta = (DisplayName = "One undo step per run: buffer runs"))
	bool bTransactionBuffer = true;
	UPROPERTY(config, EditAnywhere, Category = "Undo", meta = (DisplayName = "One undo step per run: REPL"))
	bool bTransactionRepl = true;
	UPROPERTY(config, EditAnywhere, Category = "Undo", meta = (DisplayName = "One undo step per run: Python (Dream) console"))
	bool bTransactionConsole = true;
	UPROPERTY(config, EditAnywhere, Category = "Undo", meta = (DisplayName = "One undo step per run: Run File"))
	bool bTransactionRunFile = false;

	/** (run group, B35) UEI-03 "can be disabled": the selection aliases sel, sel_assets, world and subsys are written into
	 *  the Session while idle under the U19 rule (§5.3.8). Off: Dream writes none (names already written stay). */
	UPROPERTY(config, EditAnywhere, Category = "Runs", meta = (DisplayName = "Selection aliases (sel, sel_assets, world, subsys)"))
	bool bSelectionAliases = true;

	/** The Echo input toggle (OUT-12): show each statement's source before its output. */
	UPROPERTY(config, EditAnywhere, Category = "Output", meta = (DisplayName = "Echo input"))
	bool bEchoInput = true;

	/** Normal preset folding: a statement's echo shows this many lines, then "... n more lines" (§6.4, proposed 6). */
	UPROPERTY(config, EditAnywhere, Category = "Output", meta = (DisplayName = "Echo lines before folding", ClampMin = "1", ClampMax = "100"))
	int32 EchoFoldLines = 6;

	/** OUT-08: every run that produces output gets a run log under <root>/RunLogs. */
	UPROPERTY(config, EditAnywhere, Category = "Output", meta = (DisplayName = "Always write run logs"))
	bool bAlwaysWriteRunLogs = true;

	/** OUT-01: extra log categories captured into the run while a run is active (default none). */
	UPROPERTY(config, EditAnywhere, Category = "Output", meta = (DisplayName = "Extra log categories during runs"))
	TArray<FName> RunLogCategories;

	/** §6.3: LogPython output of foreign Python while idle lands in an "Outside runs" block. */
	UPROPERTY(config, EditAnywhere, Category = "Output", meta = (DisplayName = "Capture LogPython outside runs"))
	bool bCaptureLogPythonOutsideRuns = true;

	/** editor.font_size in points (Ctrl+0 resets the zoom to it, EDT-10). */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (DisplayName = "Font size", ClampMin = "6", ClampMax = "48"))
	int32 FontSize = 10;

	/** WS-01 toolbar button in LevelEditor.LevelEditorToolBar.User. */
	UPROPERTY(config, EditAnywhere, Category = "Editor", meta = (DisplayName = "Show toolbar button"))
	bool bShowToolbarButton = true;

	/** (instances, B22) storage.root_override, the user layer (PER-08 M1; the machine and studio layers are M4): replaces the
	 *  base folder %LOCALAPPDATA%\DreamPythonEditor; <EngineVer>\<Project>-<hex6> is always appended, the result passes the §10.1 checks,
	 *  and the folder's owner.json must name this account. Empty: the platform default. Applies at the next editor start. */
	UPROPERTY(config, EditAnywhere, Category = "Storage", meta = (DisplayName = "Storage folder override",
		ToolTip = "A folder that replaces %LOCALAPPDATA%\\DreamPythonEditor for the journal, local history, run logs, REPL history and audit log. Applies at the next editor start."))
	FDirectoryPath StorageRootOverride;

	// ---- (chrome) PO-04 view settings: the Maya-minimal tab. Every hidden part is one View toggle away; per user, persisted.

	/** The theme of the whole Dream tab (PO-04 point 3): a built-in id (Darcula, Dracula, Maya, Monokai, One Dark, Nord,
	 *  Gruvbox Dark, Solarized Light, GitHub Light, Unreal) or a user theme's id. FDreamThemes (Shell/DreamTheme.h) reads it
	 *  and applies it live; View ▸ Theme writes it. (theme) Its Editor Preferences dropdown of theme ids is a detail
	 *  customization (Shell/DreamThemeDetails.h): GetOptions would need a UFUNCTION, which DreamEditor has none of (rule 11). */
	UPROPERTY(config, EditAnywhere, Category = "View", meta = (DisplayName = "Theme"))
	FString ThemeId = TEXT("Darcula");

	/** View ▸ Status line: the strip under the code area (run state, namespace, caret, journal age). Off: the caret's
	 *  line:col shows at the right of the input tab row, and transient messages beside it. */
	UPROPERTY(config, EditAnywhere, Category = "View", meta = (DisplayName = "Status line"))
	bool bShowStatusLine = false;
	/** View ▸ REPL prompt: the one-line prompt under the history. Off: Focus REPL still shows it for this editor session. */
	UPROPERTY(config, EditAnywhere, Category = "View", meta = (DisplayName = "REPL prompt"))
	bool bShowReplPrompt = false;
	/** View ▸ REPL tab: the output area's REPL tab. With it and the Problems tab off there is no tab row. */
	UPROPERTY(config, EditAnywhere, Category = "View", meta = (DisplayName = "REPL tab"))
	bool bShowReplTab = false;
	/** View ▸ Problems tab: the output area's Problems tab (F8 / Shift+F8 walk the problems either way). */
	UPROPERTY(config, EditAnywhere, Category = "View", meta = (DisplayName = "Problems tab"))
	bool bShowProblemsTab = false;
	/** View ▸ Run headers: each run's header and footer line in the history. */
	UPROPERTY(config, EditAnywhere, Category = "View", meta = (DisplayName = "Run headers and footers"))
	bool bShowRunHeaders = false;
	/** View ▸ Line numbers: the code view's line-number gutter. */
	UPROPERTY(config, EditAnywhere, Category = "View", meta = (DisplayName = "Line numbers"))
	bool bShowLineNumbers = true;
	/** (sidebar) View ▸ Project: the side bar's Project tree (PO-05: on by default, the exception to PO-04's defaults). With
	 *  it and Structure off there is no side bar. */
	UPROPERTY(config, EditAnywhere, Category = "View", meta = (DisplayName = "Project"))
	bool bShowProjectPanel = true;
	/** (sidebar) View ▸ Structure: the side bar's outline of the active tab (PO-05: on by default). */
	UPROPERTY(config, EditAnywhere, Category = "View", meta = (DisplayName = "Structure"))
	bool bShowStructurePanel = true;

	// ---- (scratch) PO-05 scratch files: per user, shared by every project (Buffers/DreamScratch.h).

	/** The folder of the scratch files: every new unnamed tab is scratch_N.py there, saved as you type (BUF-01 as amended by
	 *  PO-05). Empty: %LOCALAPPDATA%\DreamPythonEditor\Scratches. Created on first use; -DreamScratchFolder=<dir> overrides it for a
	 *  session. Applies to new tabs at once. */
	UPROPERTY(config, EditAnywhere, Category = "Scratch Files", meta = (DisplayName = "Scratch folder",
		ToolTip = "Where new unnamed tabs are saved as scratch_N.py files. Empty: %LOCALAPPDATA%\\DreamPythonEditor\\Scratches. Applies to new tabs at once."))
	FDirectoryPath ScratchFolder;
	/** The sample scripts were copied into <Scratch folder>/Samples once (so deleted samples do not come back; Help ▸
	 *  Restore Sample Scripts copies the missing ones again). Not shown. */
	UPROPERTY(config)
	bool bSamplesSeeded = false;

	/** Fires on the game thread after a view setting above (the theme id included) changed, from the View menu or the icon
	 *  row (SetViewSetting, SetThemeId) or from Editor Preferences (PostEditChangeProperty). Widgets that show or hide a part
	 *  bind to it; OnSettingChanged() still fires for Editor Preferences edits as before. */
	static FSimpleMulticastDelegate& OnViewSettingsChanged();

	bool GetViewSetting(EDreamViewSetting Setting) const;
	/** Sets a toggle, saves the config at once and broadcasts OnViewSettingsChanged (nothing when unchanged). GT. */
	static void SetViewSetting(EDreamViewSetting Setting, bool bValue);
	/** Writes ThemeId, saves it and broadcasts OnViewSettingsChanged (nothing when unchanged). FDreamThemes::SetCurrent is
	 *  the caller that also applies the colours. GT. */
	static void SetThemeId(const FString& Id);
	/** (chrome) Saves ONE property of this section to the user's EditorSettings.ini (config cache, then the file), so a
	 *  toggle never writes the other properties' in-memory values (a value a spec pinned for its steps, for example). */
	static void SaveKey(FName Property);
	/** (chrome) ShutdownModule: a write still pending gets one last checked flush (a Warning if it fails); the retry ends. */
	static void CancelSaveRetry();
	/** (chrome) Broadcast after every successful write of the settings file by SaveKey, its retry or the shutdown flush
	 *  (review r9: the tests' settings guard keeps its stamp current with it). Not for the engine's own saves. GT. */
	static FSimpleMulticastDelegate& OnFileWritten();
	/** Test hooks (Dream.Shell.Chrome): the next Count write attempts fail as a refused write does (the file is not touched);
	 *  the retry's state, with this pending episode's attempt times (FPlatformTime::Seconds). */
	struct FSaveRetryStateForTest
	{
		bool bPending = false;
		bool bWarned = false;
		double FirstFailureSeconds = 0.0;
		double NextAttemptSeconds = 0.0;
		TArray<double> AttemptStarts;
		TArray<double> AttemptEnds;
	};
	enum class ESaveFailureForTest : uint8 { Refused, ReadOnly };
	/** Kind Refused: as a held file refuses a write (retried); ReadOnly: as a read-only file does (one warning, no retry). */
	static void SetSaveFailuresForTest(int32 Count, ESaveFailureForTest Kind = ESaveFailureForTest::Refused);
	static FSaveRetryStateForTest GetSaveRetryStateForTest();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** The route default, the user setting and the U6 override combined (§5.9.1). */
	bool GetTransactionDefault(EDreamRunRoute Route) const;
};
