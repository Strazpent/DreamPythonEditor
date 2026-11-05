// SPDX-License-Identifier: Apache-2.0
// Dream.Supervisor.RunWindowFocus (run group; TDD §5.5.2 "Focus returns to the widget that had it before", §5.5.3 teardown,
// A59, RT-05; chrome's focus trace; review r6 m3). The editor is brought to the front first, as the SendInput specs do
// (DreamTest::RequireEditorInFront); a case that cannot get there fails, it never passes having checked nothing.
//   1. A floating top-level window owned by the main frame (as a floating Message Log tab window is) is open, the code view
//      is focused and the Dream tab's window active; a buffer run of about a second opens the Run window, which takes the
//      foreground. After the run the Dream tab's window is the active top-level window again and the code view holds
//      keyboard focus: the destroyed active Run window handed activation back instead of letting the OS pick.
//   2. (Win64) During the run a top-level window of the editor process that the main frame does not own (as an unparented
//      PySide window is) takes the foreground. After the run it is still the foreground window: the Run window was not the
//      foreground window at teardown, so nothing was handed back over it.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Run/SDreamRunWindow.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace DreamRunWindowFocusSpec
{
	struct FState
	{
		bool bForeignWindow = false;
		TSharedPtr<SWindow> Extra;
		TWeakPtr<SWindow> DreamWindow;
		TWeakPtr<SDreamCodeView> Code;
		FDelegateHandle PreTickHandle;
		bool bRunWindowSeen = false;
		bool bForeignShown = false;
		bool bForeignForegroundInRun = false;
#if PLATFORM_WINDOWS
		HWND Foreign = nullptr;
#endif
		double SettleFrom = 0.0;
		bool bSettingsSaved = false;
		bool bSavedRunPump = true;
		EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
	};

	FString WindowName(const TSharedPtr<SWindow>& Window)
	{
		return Window.IsValid() ? FString::Printf(TEXT("'%s'"), *Window->GetTitle().ToString()) : FString(TEXT("none"));
	}

	/** Slate pre-tick: notes the Run window, and in case 2 shows the process's unowned window once a pump is under way. */
	void OnPreTick(FState& State, FRunSupervisor& Supervisor)
	{
		const bool bOpen = Supervisor.IsRunWindowOpen();
		State.bRunWindowSeen |= bOpen;
#if PLATFORM_WINDOWS
		if (State.bForeignWindow && bOpen && !State.bForeignShown && Supervisor.GetPumpDepth() > 0)
		{
			State.bForeignShown = true;
			// A top-level window of this process with no owner (WS_OVERLAPPEDWINDOW, the system STATIC class), as a PySide
			// tool window without a parent is.
			State.Foreign = ::CreateWindowExW(0, L"STATIC", L"Dream test unowned window", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
				120, 120, 360, 140, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
			if (State.Foreign != nullptr)
			{
				::SetForegroundWindow(State.Foreign);
				State.bForeignForegroundInRun = ::GetForegroundWindow() == State.Foreign;
			}
		}
#endif
	}
}

BEGIN_DEFINE_SPEC(FDreamRunWindowFocusSpec, "Dream.Supervisor.RunWindowFocus", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	void DefineCase(const TCHAR* Name, bool bForeignWindow);
END_DEFINE_SPEC(FDreamRunWindowFocusSpec)

void FDreamRunWindowFocusSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
	});

	DefineCase(TEXT("gives activation back to the Dream tab's window and focus to the code view, not to a floating window"), false);
	DefineCase(TEXT("leaves the foreground to a window of the process the main frame does not own (an unparented tool window)"), true);
}

void FDreamRunWindowFocusSpec::DefineCase(const TCHAR* Name, bool bForeignWindow)
{
	LatentIt(Name, DreamTest::LatentTimeout(), [this, bForeignWindow](const FDoneDelegate& Done)
	{
		using namespace DreamRunWindowFocusSpec;
#if !PLATFORM_WINDOWS
		if (bForeignWindow)
		{
			AddInfo(TEXT("the unowned-window case needs Win64"));
			Done.Execute();
			return;
		}
#endif
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this) || !DreamTest::RequireEditorInFront(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
			TEXT("import time\n_rwf_t = time.perf_counter()\nwhile time.perf_counter() - _rwf_t < 1.0: pass"), TEXT("RunWindowFocus"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		TSharedRef<FState> State = MakeShared<FState>();
		State->bForeignWindow = bForeignWindow;
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedPtr<SDreamTab> TabRef = Tab;
		FRunSupervisor* Supervisor = &FDreamEditorModule::Get().GetSupervisor();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("open a floating window owned by the main frame, then focus the code view in the Dream tab's window"), [this, State, TabRef]()
		{
			FSlateApplication& App = FSlateApplication::Get();
			const TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
			const TSharedPtr<SWindow> DreamWindow = App.FindWidgetWindow(TabRef.ToSharedRef());
			const TSharedPtr<SDreamCodeView> Code = TabRef->GetActiveCodeView();
			if (!TestTrue(TEXT("the root window, the Dream tab's window and the code view exist"), Root.IsValid() && DreamWindow.IsValid() && Code.IsValid()))
			{
				return false;
			}
			State->DreamWindow = DreamWindow;
			State->Code = Code;
			if (!State->bForeignWindow)
			{
				State->Extra = SNew(SWindow)
					.Title(FText::AsCultureInvariant(TEXT("Dream test floating window")))
					.ClientSize(FVector2D(320.f, 140.f))
					.SupportsMaximize(false)
					.SupportsMinimize(false)
					[
						SNew(STextBlock).Text(FText::AsCultureInvariant(TEXT("A floating window, as a floating Message Log tab is")))
					];
				App.AddWindowAsNativeChild(State->Extra.ToSharedRef(), Root.ToSharedRef());
			}
			DreamWindow->BringToFront(/*bForce*/ true);
			Code->FocusEditor();
			State->SettleFrom = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("the Dream tab's window is active, the code view has focus and the editor has the OS foreground"), [this, State]()
		{
			const TSharedPtr<SWindow> DreamWindow = State->DreamWindow.Pin();
			const TSharedPtr<SDreamCodeView> Code = State->Code.Pin();
			if (!DreamWindow.IsValid() || !Code.IsValid())
			{
				return DreamTest::EStep::Fail;
			}
			FSlateApplication& App = FSlateApplication::Get();
			if (App.GetActiveTopLevelRegularWindow() != DreamWindow)
			{
				DreamWindow->BringToFront(true);
			}
			if (!Code->HasFocusedDescendants() && !Code->HasKeyboardFocus())
			{
				Code->FocusEditor();
			}
			const bool bReady = App.GetActiveTopLevelRegularWindow() == DreamWindow && (Code->HasFocusedDescendants() || Code->HasKeyboardFocus());
			if (bReady && FApp::HasFocus())
			{
				return DreamTest::EStep::Next;
			}
			if (FPlatformTime::Seconds() - State->SettleFrom > 3.0)
			{
				// RequireEditorInFront brought the editor forward; still not there means the desktop is not usable here.
				AddError(FString::Printf(TEXT("precondition not met after 3 s (OS focus %s, active window %s): the Dream tab's window must be active with the code view focused"),
					FApp::HasFocus() ? TEXT("yes") : TEXT("no"), *WindowName(App.GetActiveTopLevelRegularWindow())));
				return DreamTest::EStep::Fail;
			}
			return DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("run about a second, so the Run window opens"), [this, State, Buffer, Run, Supervisor]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedRunPump = Settings->bRunPump;
			State->SavedPresentation = Settings->RunPresentation;
			State->bSettingsSaved = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			State->PreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([State, Supervisor](float)
			{
				OnPreTick(*State, *Supervisor);
			});
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 3));
			return Run->RunNumber > 0;
		});
		Steps.Poll(TEXT("the run finishes"), [State, Run]()
		{
			if (Run->RunNumber > 0 && DreamTest::IsRunFinished(Run->RunNumber))
			{
				State->SettleFrom = FPlatformTime::Seconds();
				return DreamTest::EStep::Next;
			}
			return DreamTest::EStep::Wait;
		}, 60.0);
		Steps.Poll(TEXT("half a second for the deferred activation events"), [State]()
		{
			return FPlatformTime::Seconds() - State->SettleFrom >= 0.5 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.Do(TEXT("check"), [this, State]()
		{
			FSlateApplication& App = FSlateApplication::Get();
			const TSharedPtr<SWindow> DreamWindow = State->DreamWindow.Pin();
			const TSharedPtr<SDreamCodeView> Code = State->Code.Pin();
			const TSharedPtr<SWindow> Active = App.GetActiveTopLevelRegularWindow();
			const TSharedPtr<SWidget> Focused = App.GetKeyboardFocusedWidget();
			AddInfo(FString::Printf(TEXT("after the run: active window %s, keyboard focus %s"), *WindowName(Active),
				Focused.IsValid() ? *Focused->GetTypeAsString() : TEXT("none")));
#if PLATFORM_WINDOWS
			{
				// Another application in front (a click or Alt+Tab on the desktop during the attended pass) explains a failure here.
				wchar_t Title[256] = {};
				::GetWindowTextW(::GetForegroundWindow(), Title, 255);
				AddInfo(FString::Printf(TEXT("OS foreground window after the run: '%s'"), Title));
			}
#endif
			TestTrue(TEXT("the Run window opened for the run"), State->bRunWindowSeen);
			TestTrue(TEXT("the Run window chose the Dream tab's window to hand activation back to"),
				SDreamRunWindow::GetLastReturnWindowForTest() == DreamWindow);
			if (State->bForeignWindow)
			{
#if PLATFORM_WINDOWS
				if (!TestTrue(TEXT("the unowned window was created and took the foreground during the run"), State->Foreign != nullptr && State->bForeignForegroundInRun))
				{
					return true;
				}
				TestTrue(TEXT("the unowned window is still the foreground window: teardown handed nothing back over it (review r6 m3)"),
					::GetForegroundWindow() == State->Foreign);
#endif
				return true;
			}
			TestTrue(*FString::Printf(TEXT("the Dream tab's window is the active top-level window (it is %s)"), *WindowName(Active)),
				DreamWindow.IsValid() && Active == DreamWindow);
			TestTrue(TEXT("the floating window did not take activation"), !State->Extra.IsValid() || Active != State->Extra);
			TestTrue(TEXT("the code view has keyboard focus (A59 focus home)"), Code.IsValid() && (Code->HasFocusedDescendants() || Code->HasKeyboardFocus()));
			return true;
		});
		Steps.Finally(TEXT("close the extra windows and restore the settings"), [State]()
		{
			if (State->PreTickHandle.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().OnPreTick().Remove(State->PreTickHandle);
			}
			if (State->Extra.IsValid())
			{
				State->Extra->RequestDestroyWindow();
				State->Extra.Reset();
			}
#if PLATFORM_WINDOWS
			if (State->Foreign != nullptr)
			{
				::DestroyWindow(State->Foreign);
				State->Foreign = nullptr;
			}
#endif
			if (const TSharedPtr<SWindow> DreamWindow = State->DreamWindow.Pin())
			{
				DreamWindow->BringToFront(/*bForce*/ true);    // later specs start from the editor in front
			}
			if (State->bSettingsSaved)
			{
				UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
				Settings->bRunPump = State->bSavedRunPump;
				Settings->RunPresentation = State->SavedPresentation;
			}
		});
		Steps.Run(Done);
	});
}
