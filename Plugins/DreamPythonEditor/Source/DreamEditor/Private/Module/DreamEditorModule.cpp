// SPDX-License-Identifier: Apache-2.0
// The DreamEditor module (TDD §2.3, §3.4 B1, §3.8, §11.6.1, §11.7). It owns every service. Start-up order (ARCHITECTURE §6):
//  StartupModule ............... style, commands; the services (the output store before the device, CR-23), IsAvailable();
//                                key router, spawner, menus, console commands, output device; the bindings on FDreamPython;
//                                then FDreamPython::Initialize (which may call HandleInitialized at once on a late load, so
//                                every service exists first, CR-15) and FDreamRunService::Initialize. No Python, file scan or
//                                thread (B1).
//  OnFEngineLoopInitComplete ... storage root, FDreamOutputStore::Start, FDreamReplHistory::Open (memory-only),
//                                FDreamTransactions::Initialize, the 10 s deferred-start timer (RT-17)
//  tab spawn ................... FDreamJournal::Open: the instance lock decides the role (§10.3 step B, §10.5); the primary's
//                                REPL history starts writing its file (CR-24, OUT-05), a secondary journals in its own
//                                namespace and stages its REPL entries there (instances); SDreamTab::RestoreSession
//                                (recovery scan, restore, reconcile; orphans and the cross-root scan, instances)
//  OnPreExit ................... session ini, final journal snapshots and CLEAN (§10.2.5)
//  ShutdownModule .............. the reverse (§3.8 module shutdown; Python is already finalized): the module's own engine
//                                bindings and ticker; the session and CLEAN; the tab widgets, whose destructors unbind from
//                                the services (CR-23); the entry points; each service's Shutdown, the device before the
//                                store; then every gated ticker left is removed and IsAvailable() turns false, so a late
//                                ticker, async result or widget destructor does nothing; then the services are destroyed in
//                                reverse creation order.
#include "Module/DreamEditorModule.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamScratch.h"   // (scratch)
#include "Commands/DreamCommands.h"
#include "Commands/DreamKeyRouter.h"
#include "Commands/DreamNumpadEnter.h"
#include "Completion/DreamCompletionService.h"
#include "Diagnostics/DreamAudit.h"
#include "Diagnostics/DreamCrashContext.h"
#include "Interfaces/IPluginManager.h"
#include "Language/DreamLanguageServices.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Module/DreamLog.h"
#include "Module/DreamModuleInternal.h"
#include "Module/DreamText.h"
#include "Output/DreamConsoleExecutor.h"
#include "Output/DreamLogForwarder.h"
#include "Output/DreamOutputDevice.h"
#include "Output/DreamOutputStore.h"
#include "Output/DreamReplHistory.h"
#include "Output/DreamRuntimeProblems.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamLegacyMigration.h"   // (rename)
#include "Persistence/DreamInstances.h"
#include "Persistence/DreamLocalHistory.h"
#include "Persistence/DreamRestoreGuard.h"
#include "Persistence/DreamStorageRoot.h"
#include "Python/DreamPython.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Run/DreamTransactions.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Settings/DreamProjectSettings.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamEditorStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SNullWidget.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamModuleLocal
{
	constexpr float DeferredStartSeconds = 10.f;    // RT-17: 10 s after OnFEngineLoopInitComplete with a Dream tab open

	/** FDreamEditorModule::IsAvailable(): set once the services exist, cleared once they have stopped (any thread). */
	std::atomic<bool> GServicesAlive{ false };

	bool IsHeadless()
	{
		// §3.4 B1: commandlet, unattended or nullrhi: no spawner, pump, pre-processor or menus (T-LIFE-009).
		return IsRunningCommandlet() || FApp::IsUnattended() || !FApp::CanEverRender() || !FSlateApplication::IsInitialized();
	}
}

struct FDreamEditorModule::FImpl
{
	// Services, in creation order (destroyed in reverse)
	TUniquePtr<FDreamPython> Python;
	TSharedPtr<FDreamRunService> RunService;
	TUniquePtr<FRunSupervisor> Supervisor;
	TUniquePtr<FDreamTransactions> Transactions;
	TUniquePtr<FDreamOutputStore> OutputStore;
	TUniquePtr<FDreamOutputDevice> OutputDevice;
	TUniquePtr<FDreamConsoleExecutor> ConsoleExecutor;
	TUniquePtr<FDreamReplHistory> ReplHistory;
	TUniquePtr<FDreamRuntimeProblems> RuntimeProblems;
	TUniquePtr<FDreamBufferManager> BufferManager;
	TUniquePtr<FDreamJournal> Journal;
	TUniquePtr<FDreamLocalHistory> LocalHistory;     // (persist) §10.4; its I/O runs on the journal writer thread
	TUniquePtr<FDreamRestoreGuard> RestoreGuard;     // (persist) §10.8; game thread
	TUniquePtr<FDreamInstances> Instances;           // (instances) §10.1, §10.5, §10.6; its DreamStorageProbe starts at first use
	FDelegateHandle SafeModeHandle;
	TUniquePtr<FDreamCrashContext> CrashContext;     // (run group) §11.8.1; game thread only
	TUniquePtr<FDreamAudit> Audit;                   // (run group) §11.4; its writer thread starts at engine init
	TUniquePtr<FDreamCompletionService> Completion;  // (completion) §7.5.3 caches; its idle ticks start at the first boot
	TUniquePtr<FDreamLanguageServices> LanguageServices;    // (lang) §7.3; servers spawn at the deferred start after a boot

	TOptional<FDreamStorageRoot> StorageRoot;
	bool bStorageResolved = false;
	int32 LegacySectionsCopied = 0;     // (rename) settings sections MigrateConfig copied in StartupModule (A246)
	bool bEngineInitDone = false;
	bool bHeadless = false;
	bool bSpawnerRegistered = false;
	bool bPreExitDone = false;
	bool bJournalOpenAttempted = false;     // the journal opens at the first tab spawn only (A39, SHL-12)
	FTSTicker::FDelegateHandle OpenLocationTicker;

	TWeakPtr<SDreamTab> Tab;
	TWeakPtr<SDockTab> LiveDockTab;
	/** API calls made inside a run are applied in order at run end (§11.7). */
	TArray<TFunction<void()>> QueuedAtRunEnd;
	/** open() outside a run, applied at the next idle tick, in order (§11.7). */
	TArray<TFunction<void()>> QueuedApiOpens;
	/** The ID pre-allocated for a queued open() of a file not open yet, by its path key, so a repeated open() of the same
	 *  file before the queue runs returns the same ID (P2-shl-03). Removed when the open is applied. */
	TMap<FString, FGuid> PendingApiOpenIds;

	FDelegateHandle EngineInitHandle;
	FDelegateHandle PreExitHandle;
	FDelegateHandle PythonInitializedHandle;
	FDelegateHandle PythonShutdownHandle;
	FDelegateHandle RunFinishedHandle;
	FDelegateHandle BufferOpenedHandle;
	FDelegateHandle BufferClosedHandle;
	FDelegateHandle BufferSavedHandle;
	FDelegateHandle MenusStartupHandle;
	FTSTicker::FDelegateHandle DeferredStartTicker;

	void ResolveStorageRoot()
	{
		if (bStorageResolved)
		{
			return;
		}
		bStorageResolved = true;
		// (rename) A246: the one-time copy of the user's Sumi data, before the root is prepared and the journal opens.
		DreamLegacyMigration::RunStartupMigration(LegacySectionsCopied);
		const FString UProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		// (instances) §10.1: storage.root_override first (probed on DreamStorageProbe, at most 2 s), then the platform default,
		// then the temp fallback; projects.json is updated in the background.
		TValueOrError<FDreamStorageRoot, FText> Result = Instances->ResolveStorageRoot(UProject, FDreamInstances::GetConfiguredOverride(),
			FDreamInstances::ProbeWaitSeconds, /*bStrict: -DreamStorageRoot never falls back to the user profile*/ FDreamInstances::HasSessionOverride());
		if (Result.HasValue())
		{
			StorageRoot = Result.StealValue();
			if (!FDreamInstances::HasSessionOverride())
			{
				Instances->UpdateProjects(DreamStorageRoot::ProjectsJsonPath(), UProject, StorageRoot.GetValue());
			}
		}
		else
		{
			UE_LOG(LogDream, Warning, TEXT("%s"), *Result.GetError().ToString());
		}
	}

	/** §3.4 OnFEngineLoopInitComplete: storage, the output thread, REPL history, the transaction buffer, the RT-17 timer. */
	void OnEngineLoopInitComplete(FDreamEditorModule& Module)
	{
		if (bEngineInitDone)
		{
			return;
		}
		bEngineInitDone = true;
		ResolveStorageRoot();

		// The echo cap is set before the DreamOutput thread exists, so the forwarder is never touched from two threads.
		OutputStore->GetForwarder().SetEchoCap(UDreamProjectSettings::Get()->EchoCapLinesPerSecond);
		OutputStore->Start(StorageRoot.IsSet() ? StorageRoot->RunLogsDir() / Journal->GetSessionStartStamp() : FString());
		// (run group) The audit writer on <root>/audit (§11.4); its lines wait for the role the first tab spawn decides.
		Audit->Start(StorageRoot.IsSet() ? StorageRoot->Root : FString(), StorageRoot.IsSet() ? StorageRoot->ProjectHash : FString());
		if (StorageRoot.IsSet())
		{
			// §6.5 "Second instance": only the primary writes history.jsonl, and the instance lock that decides the role is
			// taken at the first tab spawn (§10.3 step B, §10.5), never by an editor that does not use Dream or a headless one.
			// Until then the history reads the file and keeps every entry in memory (the Python (Dream) executor's too); the
			// spawn of a primary appends the ones added meanwhile to the file as it is then (SetMemoryOnly(false), OUT-05).
			ReplHistory->SetMemoryOnly(true);
			ReplHistory->Open(StorageRoot->Root);
		}
		Transactions->Initialize();

		FDreamPython* PythonPtr = Python.Get();
		FDreamEditorModule* ModulePtr = &Module;
		DeferredStartTicker = FDreamDeferral::AddTicker(TEXT("DreamDeferredStart"), EDreamDeferPolicy::DeferWhileExecuting,
			DreamModuleLocal::DeferredStartSeconds, false, [this, PythonPtr, ModulePtr](float)
			{
				DeferredStartTicker.Reset();
				// (persist) §10.8: safe mode skips the startup paths that run without the user asking; Leave safe mode starts it.
				if (ModulePtr->GetTab().IsValid() && !RestoreGuard->IsSafeMode())
				{
					PythonPtr->RequestStart(EDreamStartReason::Timer);
				}
				return false;
			});
	}

	TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args)
	{
		TSharedRef<SDockTab> DockTab = SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("TabLabel", "Dream Python Editor"));

		// B2 (§3.4, §10.3): the journal opens at the tab spawn; the restore runs from its recovery scan.
		ResolveStorageRoot();
		if (StorageRoot.IsSet() && !bJournalOpenAttempted)
		{
			// Once per editor session (A39): a session that started as the second instance keeps its journal Disabled and its
			// REPL history in memory even after the other editor exits, so no journal opens behind a restore that already
			// ran (SHL-12, ADD1-01: it would never be reconciled).
			bJournalOpenAttempted = true;
			EDreamInstanceRole Role = EDreamInstanceRole::Primary;
			Journal->Open(StorageRoot.GetValue(), Role);
			// (run group) §10.5: a secondary writes its own audit-YYYY-MM-DD.<sec-id>.jsonl and never sweeps; (instances) its
			// id is the journal namespace's, journal/i-<pid>-<process_start>/.
			Audit->SetRole(Role == EDreamInstanceRole::Secondary ? EDreamAuditRole::Secondary : EDreamAuditRole::Primary,
				Role == EDreamInstanceRole::Secondary && Journal->IsOpen() ? Journal->GetNamespace() : FDreamAudit::MakeSecondaryId());
			// A36 (CR-24): only the journal's instance lock knows this is the second editor on the project.
			ReplHistory->SetMemoryOnly(Role == EDreamInstanceRole::Secondary);
			// (instances) §10.5: a secondary stages its REPL entries in <sec>/repl-history.jsonl and polls the primary's lock.
			Instances->OnJournalOpened();
			// (persist) §10.4, §10.5: only the primary runs history retention, 60 s from now and hourly; every role gets the
			// history ticker (results, the pause notice and its re-reads, review r3).
			if (Journal->IsOpen())
			{
				LocalHistory->OnJournalOpened(Role == EDreamInstanceRole::Primary);
			}
		}

		TSharedRef<SDreamTab> Body = SNew(SDreamTab, DockTab);
		DockTab->SetContent(Body);
		Tab = Body;
		LiveDockTab = DockTab;
		DockTab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateLambda([this](TSharedRef<SDockTab>)
		{
			if (TSharedPtr<SDreamTab> Closing = Tab.Pin())
			{
				Closing->SaveSessionState(true);
			}
			Journal->ForceCommit();   // tab close forces a commit (§10.2.5)
		}));
		Body->RestoreSession();
		return DockTab;
	}

	void HandlePreExit()
	{
		if (bPreExitDone)
		{
			return;
		}
		bPreExitDone = true;
		DreamLegacyMigration::StopRunLogCopy();     // (rename) a background copy of Sumi run logs resumes at the next start
		BufferManager->SaveScratchFilesAtExit();   // (scratch, PO-05) dirty scratch files to disk, before the journal's CLEAN
		if (TSharedPtr<SDreamTab> Live = Tab.Pin())
		{
			Live->SaveSessionState(true);   // SaveConfig at OnPreExit (§10.3)
		}
		LanguageServices->Shutdown();       // (lang) every server: shutdown, exit, then the kill, <= 1 s in all (§7.1.5)
		RestoreGuard->Clear();              // (persist) §10.8 step 3: pending = 0 at clean shutdown
		Instances->Shutdown();              // (instances) orphans' ns.locks released; DreamStorageProbe abandoned, never joined (§10.1)
		Journal->Shutdown();                // final snapshots, CLEAN, at most 2 s (§10.2.5)
		Audit->Shutdown();                  // (run group) the queued audit lines, at most 1 s (§11.4)
	}

	/**
	 * CR-23: the tab body, the output area, both views, the problems view, the REPL strip and its history list unbind from
	 * the store, run service, history and problems in their destructors, so they are destroyed while every service is
	 * alive. At editor exit Slate has destroyed them already (FSlateApplication::Shutdown runs before OnPreExit and the
	 * module unload, LaunchEngineLoop.cpp:5095, :5123, :5182); a module unloaded at runtime closes its tab here.
	 */
	void CloseTab()
	{
		TSharedPtr<SDockTab> Dock = LiveDockTab.Pin();
		LiveDockTab.Reset();
		Tab.Reset();
		if (!Dock.IsValid() || !FSlateApplication::IsInitialized())
		{
			return;
		}
		Dock->SetOnTabClosed(SDockTab::FOnTabClosedCallback());   // its handler belongs to this module, which is going away
		Dock->RequestCloseTab();
		Dock->SetContent(SNullWidget::NullWidget);                 // the tab manager may keep the dock tab; the body goes now
	}

	void FlushQueuedAtRunEnd()
	{
		TArray<TFunction<void()>> Queued = MoveTemp(QueuedAtRunEnd);
		for (TFunction<void()>& Action : Queued)
		{
			Action();
		}
	}
};

FDreamEditorModule::FDreamEditorModule() = default;

FDreamEditorModule::~FDreamEditorModule() = default;

void FDreamEditorModule::StartupModule()
{
	using namespace DreamModuleLocal;
	Impl = MakeUnique<FImpl>();
	Impl->bHeadless = IsHeadless();
	// (rename) A246: the Sumi settings sections copied in the config cache (no file I/O here), before any setting is read.
	Impl->LegacySectionsCopied = DreamLegacyMigration::MigrateConfig();

	if (!Impl->bHeadless)
	{
		FDreamEditorStyle::Initialize();
		FDreamCommands::Register();
	}

	// Services: constructed here, no thread and no Python (B1). The run service is shared (CR-15: the Run File save callback
	// and the run protections hold it by reference); the store comes before the device, which registers with the store in
	// its constructor (CR-23). ShutdownModule destroys them in the reverse order.
	Impl->Python = MakeUnique<FDreamPython>();
	Impl->RunService = MakeShared<FDreamRunService>();
	Impl->Supervisor = MakeUnique<FRunSupervisor>();
	Impl->Transactions = MakeUnique<FDreamTransactions>();
	Impl->OutputStore = MakeUnique<FDreamOutputStore>();
	Impl->OutputDevice = MakeUnique<FDreamOutputDevice>(*Impl->OutputStore);
	Impl->ConsoleExecutor = MakeUnique<FDreamConsoleExecutor>();
	Impl->ReplHistory = MakeUnique<FDreamReplHistory>();
	Impl->RuntimeProblems = MakeUnique<FDreamRuntimeProblems>();
	Impl->BufferManager = MakeUnique<FDreamBufferManager>();
	Impl->Journal = MakeUnique<FDreamJournal>();
	// (persist) After the journal, whose writer thread runs its I/O; the guard asks Python whether the deferred import settled.
	Impl->LocalHistory = MakeUnique<FDreamLocalHistory>(*Impl->Journal);
	{
		FDreamPython* PythonPtr = Impl->Python.Get();
		Impl->RestoreGuard = MakeUnique<FDreamRestoreGuard>([PythonPtr]()
		{
			const EDreamPythonState State = PythonPtr->GetState();
			return State != EDreamPythonState::WaitingForInit && State != EDreamPythonState::Cold && State != EDreamPythonState::Booting
				&& State != EDreamPythonState::Enabling;
		});
		// (instances) After the journal, the history and the REPL history it merges into; no thread until first use (B1).
		Impl->Instances = MakeUnique<FDreamInstances>(*Impl->Journal, *Impl->LocalHistory, *Impl->ReplHistory);
		Impl->SafeModeHandle = Impl->RestoreGuard->OnSafeModeChanged().AddLambda([this]()
		{
			// §10.8 Leaving: the deferred services start now (the RT-17 import here; the language services listen too).
			if (!Impl->RestoreGuard->IsSafeMode() && Impl->bEngineInitDone && GetTab().IsValid())
			{
				Impl->Python->RequestStart(EDreamStartReason::Timer);
			}
		});
	}
	// (run group) Diagnostics: no thread here (B1); the audit's writer starts at OnFEngineLoopInitComplete.
	Impl->CrashContext = MakeUnique<FDreamCrashContext>();
	Impl->Audit = MakeUnique<FDreamAudit>();
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor"));
		Impl->CrashContext->Initialize(Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString(TEXT("unknown")),
			Impl->Audit->GetSessionPrefix());
	}
	Impl->Completion = MakeUnique<FDreamCompletionService>();   // (completion) caches only: no thread, no Python (B1)
	Impl->LanguageServices = MakeUnique<FDreamLanguageServices>();   // (lang) bindings only until the deferred start (B1)
	GServicesAlive.store(true, std::memory_order_release);   // the menus' startup callback and HandleInitialized may run below

	if (!Impl->bHeadless)
	{
		// Key routing (§4.9.2, §4.9.3): the Numpad Enter hooks first, so Rebuild knows whether to bind it.
		FDreamNumpadEnter::Get().Register();
		FDreamKeyRouter::Get().Rebuild();

		// (rename) A246: a saved layout's old tab ID opens this tab in its place (FTabManager reads it as a legacy tab type).
		FGlobalTabmanager::Get()->AddLegacyTabType(FName(DreamLegacyMigration::LegacyTabId), FName(TabIdName));
		// The nomad tab (WS-01, §11.6.1): the Level Editor category gives the Window-menu mirror.
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(FName(TabIdName),
			FOnSpawnTab::CreateLambda([this](const FSpawnTabArgs& Args) { return Impl->SpawnTab(Args); }))
			.SetDisplayName(LOCTEXT("TabLabel", "Dream Python Editor"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Dream Python Editor for Unreal Engine"))
			.SetIcon(FSlateIcon(FDreamEditorStyle::GetStyleSetName(), DreamStyleKeys::TabIcon))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory());
		Impl->bSpawnerRegistered = true;

		Impl->MenusStartupHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
		{
			DreamMenus::Register(this);
		}));

		// The global Open Dream chord, unbound by default (§11.6.1): mapped into the main frame bindings.
		if (FModuleManager::Get().IsModuleLoaded(TEXT("MainFrame")))
		{
			IMainFrameModule& MainFrame = FModuleManager::GetModuleChecked<IMainFrameModule>(TEXT("MainFrame"));
			MainFrame.GetMainFrameCommandBindings()->MapAction(FDreamCommands::Get().GetInfo(EDreamCommand::OpenDream),
				FExecuteAction::CreateLambda([this]() { InvokeTab(); }));
		}
	}

	DreamConsoleCommands::Register();

	// The output device catches Python output that bypasses Dream's writers (§6.3); records wait in the queue until the
	// DreamOutput thread starts at OnFEngineLoopInitComplete (A29).
	Impl->OutputDevice->SetExtraRunCategories(UDreamUserSettings::Get()->RunLogCategories);
	{
		// The Echo input toggle persists in the user settings (OUT-12); every view reads the store's shared visibility.
		FDreamOutputVisibility Visibility = Impl->OutputStore->GetVisibility();
		Visibility.bEcho = UDreamUserSettings::Get()->bEchoInput;
		Impl->OutputStore->SetVisibility(Visibility);
	}
	if (GLog)
	{
		GLog->AddOutputDevice(Impl->OutputDevice.Get());
	}

	// §6.6: the console executor follows the Python generations. Bound before Initialize: on a late load (Python already
	// up) RegisterOnPythonInitialized calls HandleInitialized at once, inside Initialize (IPythonScriptPlugin.h:100).
	Impl->PythonInitializedHandle = Impl->Python->OnInitialized().AddLambda([this]() { Impl->ConsoleExecutor->Register(); });
	Impl->PythonShutdownHandle = Impl->Python->OnShutdown().AddLambda([this]() { Impl->ConsoleExecutor->Unregister(); });

	// Buffers -> journal (BUF_OPEN + SNAP, BUF_CLOSE, forced META commit, §10.2.2).
	Impl->BufferOpenedHandle = Impl->BufferManager->OnBufferOpened().AddLambda([this](const TSharedRef<FDreamBuffer>& Buffer)
	{
		Impl->Journal->OnBufferOpened(Buffer);
	});
	Impl->BufferClosedHandle = Impl->BufferManager->OnBufferClosed().AddLambda([this](const TSharedRef<FDreamBuffer>& Buffer)
	{
		Impl->Journal->OnBufferClosed(Buffer->GetId(), TEXT("closed"));
	});
	Impl->BufferSavedHandle = Impl->BufferManager->OnBufferSaved().AddLambda([this](const TSharedRef<FDreamBuffer>& Buffer)
	{
		Impl->Journal->OnBufferSaved(Buffer);
	});
	Impl->RunFinishedHandle = Impl->RunService->OnRunFinished().AddLambda([this](const FDreamRunRecord&) { Impl->FlushQueuedAtRunEnd(); });

	// (completion) Before Python's Initialize, so a late load's state changes reach it (it only binds events here).
	Impl->Completion->Initialize(*Impl->RunService, *Impl->Python);
	// (lang) Bindings on the buffers, runs and safe mode; FDreamPython::Boot calls OnDeferredStart after a good boot.
	Impl->LanguageServices->Initialize(*Impl->RunService, *Impl->Python, *Impl->BufferManager, *Impl->RestoreGuard,
		*Impl->RuntimeProblems, *Impl->Journal, Impl->bHeadless);

	// CR-15: every service exists and every binding is in place, so HandleInitialized (now, on a late load, or at the first
	// OnPythonInitialized) finds the supervisor, the run service and the store; the run service accepts runs after it.
	Impl->Python->Initialize();
	Impl->RunService->Initialize();

	// The module loads at PostEngineInit, before OnFEngineLoopInitComplete (LaunchEngineLoop.cpp:4863, :4951); a later
	// load (enabled at runtime) finds the engine loop already initialized.
	if (GIsRunning)
	{
		Impl->OnEngineLoopInitComplete(*this);
	}
	else
	{
		Impl->EngineInitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([this]() { Impl->OnEngineLoopInitComplete(*this); });
	}
	Impl->PreExitHandle = FCoreDelegates::OnPreExit.AddLambda([this]() { Impl->HandlePreExit(); });
}

void FDreamEditorModule::ShutdownModule()
{
	if (!Impl.IsValid())
	{
		return;
	}

	// 1. The module's own engine bindings and ticker first, so nothing of the module starts while it tears down (at editor
	//    exit the core ticker was reset already, LaunchEngineLoop.cpp:4995; a runtime unload still has it ticking).
	FCoreDelegates::OnFEngineLoopInitComplete.Remove(Impl->EngineInitHandle);
	FCoreDelegates::OnPreExit.Remove(Impl->PreExitHandle);
	FDreamDeferral::RemoveTicker(Impl->DeferredStartTicker);
	Impl->DeferredStartTicker.Reset();

	// 2. The session ini and the journal's final snapshots and CLEAN: a module unloaded without OnPreExit still writes them.
	Impl->HandlePreExit();

	// 3. The tab widgets, while every service they unbind from is alive (CR-23), and before their spawner goes.
	Impl->CloseTab();

	// 4. The entry points: console commands, menus and the WS-07 item, the spawner, the global chord, key routing.
	DreamConsoleCommands::Unregister();
	if (!Impl->bHeadless)
	{
		UToolMenus::UnRegisterStartupCallback(Impl->MenusStartupHandle);
		DreamMenus::Unregister(this);
		if (Impl->bSpawnerRegistered && FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(FName(TabIdName));
		}
		Impl->bSpawnerRegistered = false;
		if (FModuleManager::Get().IsModuleLoaded(TEXT("MainFrame")) && FDreamCommands::IsRegistered())
		{
			IMainFrameModule& MainFrame = FModuleManager::GetModuleChecked<IMainFrameModule>(TEXT("MainFrame"));
			MainFrame.GetMainFrameCommandBindings()->UnmapAction(FDreamCommands::Get().GetInfo(EDreamCommand::OpenDream));
		}
		FDreamKeyRouter::Get().Shutdown();
		FDreamNumpadEnter::Get().Unregister();
	}

	// 5. The module's bindings on its services.
	Impl->Python->OnInitialized().Remove(Impl->PythonInitializedHandle);
	Impl->Python->OnShutdown().Remove(Impl->PythonShutdownHandle);
	Impl->RunService->OnRunFinished().Remove(Impl->RunFinishedHandle);
	Impl->BufferManager->OnBufferOpened().Remove(Impl->BufferOpenedHandle);
	Impl->BufferManager->OnBufferClosed().Remove(Impl->BufferClosedHandle);
	Impl->BufferManager->OnBufferSaved().Remove(Impl->BufferSavedHandle);
	Impl->QueuedAtRunEnd.Reset();
	FDreamDeferral::RemoveTicker(Impl->OpenLocationTicker);
	Impl->OpenLocationTicker.Reset();
	Impl->QueuedApiOpens.Reset();
	Impl->PendingApiOpenIds.Reset();

	// 6. Each service stops, users before what they use (§3.8 module shutdown: Python is finalized already; none touches
	//    it). The supervisor's teardown destroys a Run window's live view, which still unbinds from the store and the run
	//    service. The device leaves GLog and dies before the store it registered with (CR-23).
	Impl->ConsoleExecutor->Unregister();
	Impl->LanguageServices->Shutdown();     // (lang) a no-op after OnPreExit; else the servers stop here (<= 1 s)
	Impl->Completion->Shutdown();           // (completion) its idle ticks and its run-service and Python bindings
	Impl->RunService->Shutdown();           // the OnEndFrame handler; Submit refuses from here
	Impl->Python->Shutdown();               // the lifecycle delegates and the idle ticks it queued
	Impl->Supervisor->Shutdown();           // the watchdog joined; protections, pre-processor and Run window removed
	Impl->Transactions->Shutdown();
	if (GLog)
	{
		GLog->RemoveOutputDevice(Impl->OutputDevice.Get());
	}
	Impl->OutputDevice.Reset();
	Impl->OutputStore->Shutdown();          // the drain ticker; the ingest thread and the run-log pipe joined
	Impl->ReplHistory->Close();
	Impl->BufferManager->Shutdown();        // (buffers) the file watch: directory watches, its tickers, run and focus bindings
	Impl->Instances->Shutdown();            // (instances) a no-op after OnPreExit
	Impl->LocalHistory->Shutdown();         // (persist) its result ticker; the writer already dropped queued work at OnPreExit
	Impl->Audit->Shutdown();                // (run group) a no-op after OnPreExit
	Impl->CrashContext->Shutdown();         // (run group) every Dream crash key removed

	// 7. From here IsAvailable() is false: the §3.7 gate never runs a late ticker body (an async save or load result, a
	//    debounced session save), and widget destructors that outlive the module skip their unbinding. Gated tickers still
	//    registered are removed first, so a runtime unload leaves no Dream lambda in the core ticker.
	UDreamUserSettings::CancelSaveRetry();   // (chrome) a pending settings-file retry: one last flush, then its ticker goes
	FDreamDeferral::RemoveAllTickers();
	DreamModuleLocal::GServicesAlive.store(false, std::memory_order_release);

	// 8. Destroyed in reverse creation order (the device went in step 6).
	Impl->LanguageServices.Reset();         // (lang)
	Impl->Completion.Reset();               // (completion)
	Impl->Audit.Reset();
	Impl->CrashContext.Reset();
	Impl->RestoreGuard->OnSafeModeChanged().Remove(Impl->SafeModeHandle);
	Impl->Instances.Reset();                // (instances)
	Impl->RestoreGuard.Reset();
	Impl->LocalHistory.Reset();
	Impl->Journal.Reset();
	Impl->BufferManager.Reset();
	Impl->RuntimeProblems.Reset();
	Impl->ReplHistory.Reset();
	Impl->ConsoleExecutor.Reset();
	Impl->OutputStore.Reset();
	Impl->Transactions.Reset();
	Impl->Supervisor.Reset();
	Impl->RunService.Reset();
	Impl->Python.Reset();

	if (!Impl->bHeadless)
	{
		FDreamCommands::Unregister();
		FDreamEditorStyle::Shutdown();
	}
	Impl.Reset();
}

bool FDreamEditorModule::IsAvailable()
{
	return DreamModuleLocal::GServicesAlive.load(std::memory_order_acquire);
}

// ---- Services ----

FDreamPython& FDreamEditorModule::GetPython() const
{
	check(Impl.IsValid() && Impl->Python.IsValid());
	return *Impl->Python;
}

FDreamRunService& FDreamEditorModule::GetRunService() const
{
	check(Impl.IsValid() && Impl->RunService.IsValid());
	return *Impl->RunService;
}

TSharedRef<FDreamRunService> FDreamEditorModule::GetRunServiceRef() const
{
	check(Impl.IsValid() && Impl->RunService.IsValid());
	return Impl->RunService.ToSharedRef();
}

FRunSupervisor& FDreamEditorModule::GetSupervisor() const
{
	check(Impl.IsValid() && Impl->Supervisor.IsValid());
	return *Impl->Supervisor;
}

FDreamTransactions& FDreamEditorModule::GetTransactions() const
{
	check(Impl.IsValid() && Impl->Transactions.IsValid());
	return *Impl->Transactions;
}

FDreamOutputStore& FDreamEditorModule::GetOutputStore() const
{
	check(Impl.IsValid() && Impl->OutputStore.IsValid());
	return *Impl->OutputStore;
}

FDreamOutputDevice& FDreamEditorModule::GetOutputDevice() const
{
	check(Impl.IsValid() && Impl->OutputDevice.IsValid());
	return *Impl->OutputDevice;
}

FDreamConsoleExecutor& FDreamEditorModule::GetConsoleExecutor() const
{
	check(Impl.IsValid() && Impl->ConsoleExecutor.IsValid());
	return *Impl->ConsoleExecutor;
}

FDreamReplHistory& FDreamEditorModule::GetReplHistory() const
{
	check(Impl.IsValid() && Impl->ReplHistory.IsValid());
	return *Impl->ReplHistory;
}

FDreamRuntimeProblems& FDreamEditorModule::GetRuntimeProblems() const
{
	check(Impl.IsValid() && Impl->RuntimeProblems.IsValid());
	return *Impl->RuntimeProblems;
}

FDreamBufferManager& FDreamEditorModule::GetBufferManager() const
{
	check(Impl.IsValid() && Impl->BufferManager.IsValid());
	return *Impl->BufferManager;
}

FDreamJournal& FDreamEditorModule::GetJournal() const
{
	check(Impl.IsValid() && Impl->Journal.IsValid());
	return *Impl->Journal;
}

FDreamLocalHistory& FDreamEditorModule::GetLocalHistory() const
{
	check(Impl.IsValid() && Impl->LocalHistory.IsValid());
	return *Impl->LocalHistory;
}

FDreamRestoreGuard& FDreamEditorModule::GetRestoreGuard() const
{
	check(Impl.IsValid() && Impl->RestoreGuard.IsValid());
	return *Impl->RestoreGuard;
}

FDreamInstances& FDreamEditorModule::GetInstances() const
{
	check(Impl.IsValid() && Impl->Instances.IsValid());   // (instances)
	return *Impl->Instances;
}

FDreamAudit& FDreamEditorModule::GetAudit() const
{
	check(Impl.IsValid() && Impl->Audit.IsValid());
	return *Impl->Audit;
}

FDreamCrashContext& FDreamEditorModule::GetCrashContext() const
{
	check(Impl.IsValid() && Impl->CrashContext.IsValid());
	return *Impl->CrashContext;
}

FDreamLanguageServices& FDreamEditorModule::GetLanguageServices() const
{
	check(Impl.IsValid() && Impl->LanguageServices.IsValid());   // (lang)
	return *Impl->LanguageServices;
}

FDreamCompletionService& FDreamEditorModule::GetCompletion() const
{
	check(Impl.IsValid() && Impl->Completion.IsValid());   // (completion)
	return *Impl->Completion;
}

// ---- The tab ----

TSharedPtr<SDreamTab> FDreamEditorModule::GetTab() const
{
	return Impl.IsValid() ? Impl->Tab.Pin() : nullptr;
}

TSharedPtr<SDreamTab> FDreamEditorModule::InvokeTab()
{
	if (!Impl.IsValid() || !Impl->bSpawnerRegistered)
	{
		return nullptr;
	}
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(FName(TabIdName)));
	return GetTab();
}

FString FDreamEditorModule::OpenLocation(const FString& PathOrPseudoFile, int32 Line, int32 Column)
{
	FDreamBufferManager& Buffers = GetBufferManager();
	if (GetRunService().IsRunActive())
	{
		// Inside a run the request is queued and applied at run end (§11.7); the tab ID is known now when the buffer is open,
		// else one is pre-allocated for the buffer the queued open will create (A20).
		TSharedPtr<FDreamBuffer> Existing = Buffers.ResolveFilename(PathOrPseudoFile);
		const FGuid Id = Existing.IsValid() ? Existing->GetId() : FGuid::NewGuid();
		Impl->QueuedAtRunEnd.Add([this, PathOrPseudoFile, Line, Column, Id]()
		{
			if (TSharedPtr<SDreamTab> Live = InvokeTab())
			{
				Live->OpenLocation(PathOrPseudoFile, Line, Column, Id);
			}
		});
		return PathOrPseudoFile.StartsWith(TEXT("<dream:")) && !Existing.IsValid() ? FString() : DreamText::GuidToHex(Id);
	}

	TSharedPtr<SDreamTab> Live = InvokeTab();
	if (!Live.IsValid() || !Live->OpenLocation(PathOrPseudoFile, Line, Column))
	{
		return FString();
	}
	TSharedPtr<FDreamBuffer> Opened = Buffers.ResolveFilename(PathOrPseudoFile);
	if (!Opened.IsValid())
	{
		Opened = Live->GetActiveBuffer();
	}
	return Opened.IsValid() ? Opened->GetIdHex() : FString();
}

FString FDreamEditorModule::OpenLocationFromApi(const FString& PathOrPseudoFile, int32 Line, int32 Column)
{
	// §11.7 open(path, line): queued to run end inside a run, applied at the next idle tick otherwise, so the tab spawn, the
	// journal open and the file read never happen inside the caller's Python call. What the queued open would refuse is
	// refused now, in both cases, so open() raises the same way: no tab can open (headless), a real path that does not exist
	// or is over 1 MB (A40), a pseudo file that names neither an open nor a closed tab.
	if (!Impl->bSpawnerRegistered)
	{
		return FString();
	}
	FDreamBufferManager& Buffers = GetBufferManager();
	const bool bPseudo = PathOrPseudoFile.StartsWith(TEXT("<dream:"));
	const FString Path = bPseudo ? PathOrPseudoFile : FPaths::ConvertRelativePathToFull(PathOrPseudoFile);
	TSharedPtr<FDreamBuffer> Existing = Buffers.ResolveFilename(Path);
	if (!Existing.IsValid() && bPseudo)
	{
		// A closed scratch or pseudo-file tab: the queued SDreamTab::OpenLocation reopens it (§6.4 Reopen tab, A2-01).
		FString Name;
		FString IdHex;
		if (Path.LeftChop(1).Split(TEXT("#"), &Name, &IdHex, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			Existing = Buffers.FindClosedByIdHex(IdHex);
		}
	}
	if (!Existing.IsValid() && (bPseudo || !IFileManager::Get().FileExists(*Path) || IFileManager::Get().FileSize(*Path) > FDreamBufferManager::PlainModeMaxBytes))
	{
		return FString();
	}
	// The path key follows FDreamBufferManager's path rules (normalized, case-insensitive on Windows).
	FString Key;
	if (!Existing.IsValid())
	{
		// (architect, scratch-p2-m2) A UNC path keeps its leading "//", as FDreamBufferManager's NormalizePath does.
		const bool bUnc = Path.StartsWith(TEXT("\\\\")) || Path.StartsWith(TEXT("//"));
		Key = Path;
		FPaths::NormalizeFilename(Key);
		FPaths::CollapseRelativeDirectories(Key);
		FPaths::RemoveDuplicateSlashes(Key);
		if (bUnc && !Key.StartsWith(TEXT("//")))
		{
			Key = TEXT("/") + Key;
		}
#if PLATFORM_WINDOWS
		Key.ToLowerInline();
#endif
	}
	const FGuid* Pending = Key.IsEmpty() ? nullptr : Impl->PendingApiOpenIds.Find(Key);
	// Pre-allocated for the queued open (A20), once per file until that open is applied.
	const FGuid Id = Existing.IsValid() ? Existing->GetId() : (Pending != nullptr ? *Pending : FGuid::NewGuid());
	if (!Key.IsEmpty())
	{
		Impl->PendingApiOpenIds.Add(Key, Id);
	}
	TFunction<void()> Apply = [this, Path, Line, Column, Id, Key]()
	{
		if (!Key.IsEmpty())
		{
			Impl->PendingApiOpenIds.Remove(Key);
		}
		if (TSharedPtr<SDreamTab> Live = InvokeTab())
		{
			Live->OpenLocation(Path, Line, Column, Id);
		}
	};
	if (GetRunService().IsRunActive())
	{
		Impl->QueuedAtRunEnd.Add(MoveTemp(Apply));
	}
	else
	{
		Impl->QueuedApiOpens.Add(MoveTemp(Apply));
		if (!Impl->OpenLocationTicker.IsValid())
		{
			Impl->OpenLocationTicker = FDreamDeferral::AddTicker(TEXT("DreamOpenLocation"), EDreamDeferPolicy::DeferWhileExecuting, 0.f, false,
				[this](float)
				{
					Impl->OpenLocationTicker.Reset();
					TArray<TFunction<void()>> Queued = MoveTemp(Impl->QueuedApiOpens);
					for (TFunction<void()>& Action : Queued)
					{
						Action();
					}
					return false;
				});
		}
	}
	return DreamText::GuidToHex(Id);
}

FString FDreamEditorModule::NewScratchTab(const FString& Text, const FString& Name)
{
	const FGuid Id = FGuid::NewGuid();
	if (GetRunService().IsRunActive())
	{
		// new_tab(): the ID is returned now and the tab appears at run end (§11.7, §12.1.1).
		Impl->QueuedAtRunEnd.Add([this, Text, Name, Id]()
		{
			if (TSharedPtr<SDreamTab> Live = InvokeTab())
			{
				Live->NewScratchTab(Text, Name, Id);
			}
			else
			{
				CreateScratchWithoutTab(Text, Name, Id);   // (scratch)
			}
		});
		return DreamText::GuidToHex(Id);
	}
	if (TSharedPtr<SDreamTab> Live = InvokeTab())
	{
		return Live->NewScratchTab(Text, Name, Id)->GetIdHex();
	}
	return CreateScratchWithoutTab(Text, Name, Id)->GetIdHex();   // (scratch)
}

TSharedRef<FDreamBuffer> FDreamEditorModule::CreateScratchWithoutTab(const FString& Text, const FString& Name, const FGuid& Id)
{
	// (scratch, PO-05) new_tab() with no tab to show it: an unnamed tab is still a scratch file, as SDreamTab::NewScratchTab
	// makes it; the journaled Untitled-N only when the scratch folder cannot be written.
	if (Name.IsEmpty())
	{
		FText Error;
		if (const TSharedPtr<FDreamBuffer> File = GetBufferManager().CreateScratchFile(Text, Error, Id))
		{
			return File.ToSharedRef();
		}
		DreamScratch::WarnNotWritableOnce(Error);   // once per folder per session (scratch-p1-m7)
	}
	return GetBufferManager().CreateScratch(Text, Name, Id);
}

IMPLEMENT_MODULE(FDreamEditorModule, DreamEditor)

#undef LOCTEXT_NAMESPACE
