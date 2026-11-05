// SPDX-License-Identifier: Apache-2.0
// The DreamEditor module (TDD §2.3, §3.4 B1, §3.8, §11.6.1, §11.7): owns every service, the nomad tab spawner
// "DreamEditor.Main" (label Dream), the Tools and Window menu entries, the toolbar button, the WS-07 status-bar item, the
// console commands Dream.Open / Dream.RunFile / Dream.ClearOutput, the output device and the lifecycle bindings.
// StartupModule does no Python, file scan, child process or thread (B1). Game thread. Body: DreamEditorModule.cpp (shell).
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class FRunSupervisor;
class FDreamAudit;
class FDreamBuffer;
class FDreamBufferManager;
class FDreamCompletionService;
class FDreamLanguageServices;
class FDreamConsoleExecutor;
class FDreamCrashContext;
class FDreamInstances;
class FDreamJournal;
class FDreamLocalHistory;
class FDreamRestoreGuard;
class FDreamOutputDevice;
class FDreamOutputStore;
class FDreamPython;
class FDreamReplHistory;
class FDreamRunService;
class FDreamRuntimeProblems;
class FDreamTransactions;
class SDreamTab;

class DREAMEDITOR_API FDreamEditorModule : public IModuleInterface
{
public:
	/** The nomad tab ID (WS-01). A TCHAR literal, so no FName is constructed during static initialization. */
	static constexpr const TCHAR* TabIdName = TEXT("DreamEditor.Main");

	/** Valid while IsAvailable() is true. */
	static FDreamEditorModule& Get()
	{
		return FModuleManager::GetModuleChecked<FDreamEditorModule>(TEXT("DreamEditor"));
	}
	/**
	 * Any thread. True from the moment StartupModule has created the services until ShutdownModule has stopped them,
	 * before it destroys them. Late paths (FDreamDeferral bodies, async save and load results, widget destructors that
	 * unbind from a service, OnEndFrame) check it first and do nothing when it is false, so nothing reaches a service
	 * that is being torn down. A plain atomic read: safe even during static destruction.
	 */
	static bool IsAvailable();

	/** Out of line: TUniquePtr<FImpl> needs FImpl complete where the (exported) destructor is defined. */
	FDreamEditorModule();
	virtual ~FDreamEditorModule() override;

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// ---- Services (created in StartupModule, destroyed in ShutdownModule; never null while IsAvailable()) ----

	FDreamPython& GetPython() const;
	FDreamRunService& GetRunService() const;
	TSharedRef<FDreamRunService> GetRunServiceRef() const;
	FRunSupervisor& GetSupervisor() const;
	FDreamTransactions& GetTransactions() const;
	FDreamOutputStore& GetOutputStore() const;
	FDreamOutputDevice& GetOutputDevice() const;
	FDreamConsoleExecutor& GetConsoleExecutor() const;
	FDreamReplHistory& GetReplHistory() const;
	FDreamRuntimeProblems& GetRuntimeProblems() const;
	FDreamBufferManager& GetBufferManager() const;
	FDreamJournal& GetJournal() const;
	/** (persist) Local history (§10.4, Persistence/DreamLocalHistory.h) and the restore guard and safe mode (§10.8). */
	FDreamLocalHistory& GetLocalHistory() const;
	FDreamRestoreGuard& GetRestoreGuard() const;
	/** (instances) The storage root's override and probe, projects.json, orphans and the secondary's side (§10.1, §10.5, §10.6;
	 *  Persistence/DreamInstances.h). */
	FDreamInstances& GetInstances() const;
	/** (run group) The audit record (§11.4) and the crash breadcrumbs (§11.8.1), Private/Diagnostics. */
	FDreamAudit& GetAudit() const;
	FDreamCrashContext& GetCrashContext() const;
	/** (completion) Live completion: FUnrealNameIndex, the post-run Session cache and the resolver's idle ticks (§7.5.3,
	 *  Completion/DreamCompletionService.h). */
	FDreamCompletionService& GetCompletion() const;
	/** (lang) Language services: dream-check, the Problems model, the Stage 1 snapshot (§7.3, §7.4, §7.8,
	 *  Language/DreamLanguageServices.h). */
	FDreamLanguageServices& GetLanguageServices() const;

	// ---- The tab ----

	/** The live tab body, or null while the Dream tab is closed. */
	TSharedPtr<SDreamTab> GetTab() const;
	/** Opens or focuses the Dream tab (Tools > Dream Python Editor, Dream.Open, WS-07 click). */
	TSharedPtr<SDreamTab> InvokeTab();

	/**
	 * Opens a real path or "<dream:...#id>" pseudo file at a 1-based line and column, opening the tab first. Inside a run the
	 * request is queued and applied at run end (§11.7); returns the buffer's tab ID (32 hex) or empty when unresolved.
	 */
	FString OpenLocation(const FString& PathOrPseudoFile, int32 Line, int32 Column);
	/**
	 * dream_editor.open() (_dream.open_tab, §11.7): the same open, never inside the caller's Python call: queued to run end
	 * inside a run, else applied at the next idle tick. Returns the tab ID now (pre-allocated for a file not yet open, A20,
	 * the same one for a repeated open() before it is applied); empty when no tab can open (headless), for a missing file or
	 * one over 1 MB, and for a pseudo file that names neither an open nor a closed tab, so open() raises in both cases.
	 */
	FString OpenLocationFromApi(const FString& PathOrPseudoFile, int32 Line, int32 Column);
	/** new_tab(): a scratch tab; the ID is returned now and the tab appears at run end when called inside a run (§11.7).
	 *  (scratch, PO-05) Without a name it is a scratch file (SDreamTab::NewScratchTab). */
	FString NewScratchTab(const FString& Text, const FString& Name);

private:
	/** (scratch) new_tab() when no tab can show it: a scratch file for an unnamed tab, else the journaled scratch buffer. */
	TSharedRef<FDreamBuffer> CreateScratchWithoutTab(const FString& Text, const FString& Name, const FGuid& Id);

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
