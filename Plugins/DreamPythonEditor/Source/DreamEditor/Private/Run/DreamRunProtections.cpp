// SPDX-License-Identifier: Apache-2.0
// The common run protections (TDD §5.5.2): registered from the first pump in both presentations, before the first
// PumpMessages (RT-04), and removed in finish step 3. Game thread (supervisor slice).
#include "Run/DreamRunProtections.h"

#include "Commands/DreamInputPreProcessor.h"
#include "Language/DreamLanguageServices.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Run/DreamRunService.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Interfaces/IMainFrameModule.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamRunProtectionsPrivate
{
	const TCHAR* const MainFrameModuleName = TEXT("MainFrame");

	/** RT-03: no Win32 move or size loop may start inside our PumpMessages (GenericApplicationMessageHandler.h:126-129). */
	bool VetoWindowAction(const TSharedRef<FGenericWindow>& Window, EWindowAction::Type Action)
	{
		switch (Action)
		{
		case EWindowAction::ClickedNonClientArea:
		case EWindowAction::Maximize:
		case EWindowAction::Restore:
		case EWindowAction::WindowMenu:
			return true;                                   // true aborts the action
		default:
			return false;
		}
	}

	/** Alt+F4 or File > Exit during a run: refused with a toast (§5.14). */
	bool VetoEditorClose(int32 RunNumber)
	{
		FNotificationInfo Info(FText::Format(LOCTEXT("Protections.CloseVeto", "Run #{0} is active. Stop it before closing the editor."),
			FText::AsNumber(RunNumber, &FNumberFormattingOptions::DefaultNoGrouping())));
		Info.ExpireDuration = 5.f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return false;
	}
}

void FDreamRunProtections::Install(int32 RunNumber, EDreamRunPresentation Presentation)
{
	check(IsInGameThread());
	if (bInstalled)
	{
		return;
	}
	ActiveRunNumber = RunNumber;
	ActivePresentation = Presentation;

	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& App = FSlateApplication::Get();

		// 1. The input pre-processor, PreEngine bucket at priority 0 (§5.6.1).
		TWeakPtr<FDreamRunService> Service;
		if (FDreamEditorModule::IsAvailable())
		{
			Service = FDreamEditorModule::Get().GetRunServiceRef();
		}
		PreProcessor = MakeShared<FDreamInputPreProcessor>(Service);
		FDreamInputPreProcessor::FContext Context;
		Context.Presentation = Presentation;
		PreProcessor->SetContext(Context);
		FInputPreprocessorRegistrationKey Key;
		Key.Type = EInputPreProcessorType::PreEngine;
		Key.Priority = 0;
		if (!App.RegisterInputPreProcessor(PreProcessor, Key))
		{
			UE_LOG(LogDream, Warning, TEXT("Run #%d: the input pre-processor could not be registered."), RunNumber);
		}

		// 3. The window-action veto, in both presentations (stricter than RT-03, harmless).
		WindowActionHandle = App.RegisterOnWindowActionNotification(FOnWindowAction::CreateStatic(&DreamRunProtectionsPrivate::VetoWindowAction));
	}

	// 2. The editor close veto (IMainFrameModule.h:231).
	if (FModuleManager::Get().IsModuleLoaded(DreamRunProtectionsPrivate::MainFrameModuleName))
	{
		CanCloseHandle = IMainFrameModule::Get().RegisterCanCloseEditor(
			IMainFrameModule::FMainFrameCanCloseEditor::CreateStatic(&DreamRunProtectionsPrivate::VetoEditorClose, RunNumber));
	}

	// Read-only buffers need no push: views bind IsReadOnly to FDreamRunService::IsRunActive() (A27).
	// Ticker clients defer through FDreamDeferral while IsRunActive() (RT-01); async exceptions are gated on the pump
	// depth by the supervisor (§5.7.5).
	// (lang) D2, §5.5.2: no language result applies and no snapshot slice runs while protected.
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetLanguageServices().SetRunActive(true);
	}
	// Deferred (M2, M3): live hover (M2), the inspector, watches and the Variables refresh (M3) turn off here too (D2).
	// Deferred (M3, M4): installing for supervised task steps and while Paused (§5.6.1, M3), and the Live (docked) Stop
	// targets: the run banner, the output view and the WS-07 Stop (§5.5.4, §5.6.4).
	bInstalled = true;
}

void FDreamRunProtections::Remove()
{
	if (!bInstalled)
	{
		return;
	}
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& App = FSlateApplication::Get();
		if (PreProcessor.IsValid())
		{
			App.UnregisterInputPreProcessor(PreProcessor);
		}
		if (WindowActionHandle.IsValid())
		{
			App.UnregisterOnWindowActionNotification(WindowActionHandle);
		}
	}
	if (CanCloseHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(DreamRunProtectionsPrivate::MainFrameModuleName))
	{
		IMainFrameModule::Get().UnregisterCanCloseEditor(CanCloseHandle);
	}
	PreProcessor.Reset();
	WindowActionHandle.Reset();
	CanCloseHandle.Reset();
	ActiveRunNumber = 0;
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetLanguageServices().SetRunActive(false);     // (lang) §5.5.2
	}
	bInstalled = false;
}

void FDreamRunProtections::SetRunWindow(const TSharedPtr<SWindow>& RunWindow, const TSharedPtr<SWidget>& LiveRunView)
{
	if (!PreProcessor.IsValid())
	{
		return;
	}
	// §5.6.4: the whole Run window is a Stop target; its live run view passes the wheel, scrollbar and chips (PO-03).
	FDreamInputPreProcessor::FContext Context;
	Context.Presentation = ActivePresentation;
	Context.RunWindow = RunWindow;
	if (RunWindow.IsValid())
	{
		Context.StopTargets.Add(TWeakPtr<SWidget>(StaticCastSharedPtr<SWidget>(RunWindow)));
	}
	Context.LiveRunView = LiveRunView;
	PreProcessor->SetContext(Context);
}

#undef LOCTEXT_NAMESPACE
