// SPDX-License-Identifier: Apache-2.0
#include "Fixtures/DreamTestProbes.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Layout/WidgetPath.h"
#include "Misc/ScopeLock.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Run/SDreamRunWindow.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamOutputView.h"
#include "Widgets/SWindow.h"

namespace DreamTest
{
	namespace
	{
		struct FProbeState
		{
			FCriticalSection Lock;
			FDreamProbeSnapshot Snapshot;
			// Game thread only.
			TMap<FName, TWeakPtr<SWidget>> Tracked;
			FDelegateHandle PreTickHandle;
			FDelegateHandle ModalTickHandle;
			int32 LastRunNumber = 0;
			int32 PumpPublishes = 0;
			int32 RunWindowWasActiveRegular = 0;
			bool bStarted = false;
		};

		FProbeState& State()
		{
			static FProbeState Instance;
			return Instance;
		}

		/** The SDreamRunWindow inside Window, or null. */
		TSharedPtr<SDreamRunWindow> FindRunWindowContent(const TSharedPtr<SWindow>& Window)
		{
			if (!Window.IsValid())
			{
				return nullptr;
			}
			const TSharedRef<SWidget> Content = Window->GetContent();
			if (Content->GetType() == FName(TEXT("SDreamRunWindow")))
			{
				return StaticCastSharedRef<SDreamRunWindow>(Content);
			}
			return nullptr;
		}

		FSlateRect RectOf(const TWeakPtr<SWidget>& Weak)
		{
			FSlateRect Rect(0.f, 0.f, 0.f, 0.f);
			FDreamTestProbes::GetDesktopRect(Weak.Pin(), Rect);
			return Rect;
		}

		void Publish()
		{
			if (!FSlateApplication::IsInitialized() || !IsInGameThread())
			{
				return;
			}
			FProbeState& S = State();
			FDreamProbeSnapshot Next;
			Next.PublishedAt = FPlatformTime::Seconds();
			FSlateApplication& App = FSlateApplication::Get();
			Next.bMenusVisible = App.AnyMenusVisible();
			{
				const FWidgetPath UnderCursor = App.LocateWindowUnderMouse(App.GetCursorPos(), App.GetInteractiveTopLevelWindows(), /*bIgnoreEnabledStatus*/ true);
				TArray<FString> Parts;
				for (int32 Index = FMath::Max(0, UnderCursor.Widgets.Num() - 3); Index < UnderCursor.Widgets.Num(); ++Index)
				{
					Parts.Add(UnderCursor.Widgets[Index].Widget->GetTypeAsString());
				}
				Next.WidgetUnderCursor = FString::Join(Parts, TEXT(" > "));
			}

			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule& Module = FDreamEditorModule::Get();
				const FDreamRunService& Runs = Module.GetRunService();
				const FRunSupervisor& Supervisor = Module.GetSupervisor();
				if (const FDreamRunRecord* Active = Runs.GetActiveRecord())
				{
					Next.ActiveRunNumber = Active->RunNumber;
					Next.RunState = static_cast<uint8>(Active->State);
				}
				Next.PumpDepth = Supervisor.GetPumpDepth();
				Next.bRunWindowOpen = Supervisor.IsRunWindowOpen();
				const FDreamRunId CurrentId = Supervisor.GetCurrentRunId();
				Next.bStopRequested = CurrentId != 0 && Supervisor.IsStopRequested(CurrentId);
				if (const TSharedPtr<SDreamOutputView> Live = Supervisor.GetLiveRunViewForTest())
				{
					Next.bLiveViewFollowing = Live->IsFollowing();
				}
				const FDreamRunLiveStats& Live = Supervisor.GetLiveStats();
				Next.ProgressDone = Live.ProgressDone;
				Next.ProgressTotal = Live.ProgressTotal.IsSet() ? Live.ProgressTotal.GetValue() : -1;
			}
			if (Next.ActiveRunNumber != S.LastRunNumber)
			{
				S.LastRunNumber = Next.ActiveRunNumber;
				S.PumpPublishes = 0;
			}
			if (Next.PumpDepth > 0 && Next.ActiveRunNumber != 0)
			{
				++S.PumpPublishes;
			}
			Next.PumpPublishes = S.PumpPublishes;

			const TSharedPtr<SWindow> Modal = App.GetActiveModalWindow();
			if (const TSharedPtr<SDreamRunWindow> RunWindow = FindRunWindowContent(Modal))
			{
				Next.Modal = EDreamModalKind::RunWindow;
				Next.RunWindowHwnd = FDreamTestProbes::GetOsWindowHandle(Modal);
				if (App.GetActiveTopLevelRegularWindow() == Modal)
				{
					++S.RunWindowWasActiveRegular;
				}
				FDreamTestProbes::GetDesktopRect(Modal, Next.RunWindow);
				const TArray<TWeakPtr<SWidget>> Targets = RunWindow->GetStopTargets();
				if (Targets.Num() > 0)
				{
					Next.StopButton = RectOf(Targets[0]);
				}
				if (Targets.Num() > 1)
				{
					Next.InterruptButton = RectOf(Targets[1]);
				}
				if (Targets.Num() > 2)
				{
					Next.LiveRunView = RectOf(Targets[2]);
				}
			}
			else if (Modal.IsValid())
			{
				Next.Modal = EDreamModalKind::Foreign;
				Next.ForeignModalHwnd = FDreamTestProbes::GetOsWindowHandle(Modal);
			}
			Next.RunWindowWasActiveRegular = S.RunWindowWasActiveRegular;
			Next.MainFrameHwnd = FDreamTestProbes::GetOsWindowHandle(FGlobalTabmanager::Get()->GetRootWindow());

			for (auto It = S.Tracked.CreateIterator(); It; ++It)
			{
				const TSharedPtr<SWidget> Widget = It.Value().Pin();
				if (!Widget.IsValid())
				{
					continue;
				}
				FSlateRect Rect;
				if (FDreamTestProbes::GetDesktopRect(Widget, Rect))
				{
					Next.Tracked.Add(It.Key(), Rect);
					Next.TrackedHwnd.Add(It.Key(), FDreamTestProbes::GetOsWindowHandle(App.FindWidgetWindow(Widget.ToSharedRef())));
				}
			}

			FScopeLock Lock(&S.Lock);
			Next.Serial = S.Snapshot.Serial + 1;
			S.Snapshot = MoveTemp(Next);
		}
	}

	void FDreamTestProbes::Start()
	{
		FProbeState& S = State();
		if (S.bStarted || !FSlateApplication::IsInitialized())
		{
			return;
		}
		S.bStarted = true;
		FSlateApplication& App = FSlateApplication::Get();
		S.PreTickHandle = App.OnPreTick().AddLambda([](float) { Publish(); });
		S.ModalTickHandle = App.GetOnModalLoopTickEvent().AddLambda([](float) { Publish(); });
	}

	void FDreamTestProbes::Stop()
	{
		FProbeState& S = State();
		if (!S.bStarted)
		{
			return;
		}
		S.bStarted = false;
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& App = FSlateApplication::Get();
			App.OnPreTick().Remove(S.PreTickHandle);
			App.GetOnModalLoopTickEvent().Remove(S.ModalTickHandle);
		}
		S.Tracked.Reset();
	}

	FDreamProbeSnapshot FDreamTestProbes::GetSnapshot()
	{
		FProbeState& S = State();
		FScopeLock Lock(&S.Lock);
		return S.Snapshot;
	}

	void FDreamTestProbes::PublishNow()
	{
		Publish();
	}

	void FDreamTestProbes::Track(FName Name, const TSharedPtr<SWidget>& Widget)
	{
		check(IsInGameThread());
		State().Tracked.Add(Name, Widget);
	}

	void FDreamTestProbes::Untrack(FName Name)
	{
		check(IsInGameThread());
		State().Tracked.Remove(Name);
	}

	void FDreamTestProbes::UntrackAll()
	{
		check(IsInGameThread());
		State().Tracked.Reset();
	}

	bool FDreamTestProbes::GetDesktopRect(const TSharedPtr<SWidget>& Widget, FSlateRect& OutRect)
	{
		OutRect = FSlateRect(0.f, 0.f, 0.f, 0.f);
		if (!Widget.IsValid() || !FSlateApplication::IsInitialized())
		{
			return false;
		}
		// Tick-space geometry is in desktop pixels (SDreamOutputArea::GetDesktopRect reads the same cached geometry); paint
		// space is relative to the widget's window, which put every Run-window click at window coordinates on the desktop.
		const FGeometry& Geometry = Widget->GetTickSpaceGeometry();
		const FVector2D Size = FVector2D(Geometry.GetAbsoluteSize());
		if (Size.X < 1.0 || Size.Y < 1.0)
		{
			return false;
		}
		const FVector2D Position = FVector2D(Geometry.GetAbsolutePosition());
		OutRect = FSlateRect(static_cast<float>(Position.X), static_cast<float>(Position.Y),
			static_cast<float>(Position.X + Size.X), static_cast<float>(Position.Y + Size.Y));
		return true;
	}

	void* FDreamTestProbes::GetOsWindowHandle(const TSharedPtr<SWindow>& Window)
	{
		if (!Window.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FGenericWindow> Native = Window->GetNativeWindow();
		return Native.IsValid() ? Native->GetOSWindowHandle() : nullptr;
	}
}
