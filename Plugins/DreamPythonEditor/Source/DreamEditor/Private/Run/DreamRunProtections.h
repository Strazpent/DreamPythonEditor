// SPDX-License-Identifier: Apache-2.0
// The common run protections (TDD §5.5.2), registered on the first pump in both presentations and removed in finish
// step 3. Game thread. Body: DreamRunProtections.cpp (supervisor slice), driven by FRunSupervisor.
#pragma once

#include "CoreMinimal.h"
#include "Run/DreamRunTypes.h"

class FDreamInputPreProcessor;
class SWindow;
class SWidget;

class DREAMEDITOR_API FDreamRunProtections
{
public:
	/**
	 * Registers, in order: the input pre-processor (PreEngine bucket, before the first PumpMessages, RT-04); the editor
	 * close veto (IMainFrameModule::RegisterCanCloseEditor, toast "Run #22 is active. Stop it before closing the editor.");
	 * the window-action veto (RegisterOnWindowActionNotification aborting ClickedNonClientArea, Maximize, Restore, WindowMenu).
	 * Read-only buffers need no push: views bind IsReadOnly to FDreamRunService::IsRunActive() (slice A choice).
	 */
	void Install(int32 RunNumber, EDreamRunPresentation Presentation);
	/** Unregisters everything Install registered. Idempotent. */
	void Remove();
	bool IsInstalled() const { return bInstalled; }

	/** Updates the pre-processor's Stop targets once the Run window exists (§5.6.4). */
	void SetRunWindow(const TSharedPtr<SWindow>& RunWindow, const TSharedPtr<SWidget>& LiveRunView);

	TSharedPtr<FDreamInputPreProcessor> GetPreProcessor() const { return PreProcessor; }

private:
	TSharedPtr<FDreamInputPreProcessor> PreProcessor;
	FDelegateHandle CanCloseHandle;
	FDelegateHandle WindowActionHandle;
	EDreamRunPresentation ActivePresentation = EDreamRunPresentation::RunWindow;
	int32 ActiveRunNumber = 0;
	bool bInstalled = false;
};
