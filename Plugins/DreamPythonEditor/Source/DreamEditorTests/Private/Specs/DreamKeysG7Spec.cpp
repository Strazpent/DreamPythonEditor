// SPDX-License-Identifier: Apache-2.0
// Dream.Keys.G7: Numpad Enter on Win64 (TDD §4.16.2, §4.9.3, G7, RUN-01). FDreamNumpadEnter records lParam bit 24 of each
// VK_RETURN key-down in its IWindowsMessageHandler and pops one entry per EKeys::Enter in Slate's pre-input listener; a
// listener of this spec, registered after it, reads IsCurrentEnterNumpad() for every Enter the platform delivers and
// compares it with what was sent.
//   T-KEY-014: 10,000 Enter key-downs posted as WM_KEYDOWN / WM_SYSKEYDOWN with and without bit 24 (and auto-repeat,
//     bit 30), in random batches with other keys and VK_PROCESSKEY (an IME key-down, which must not push) between them,
//     to a sink window that swallows them: 100% classified as sent. Then 100 presses through SendInput with and without
//     KEYEVENTF_EXTENDEDKEY, one at a time, by the input bot.
//   T-KEY-015: a synthetic Slate Enter (no window message) counts as main Enter; a Numpad Enter that Slate never routes
//     (posted to the main frame while the Run window is the active modal, so the platform filter drops it) is purged
//     after OnPostTick and never classifies a later main Enter; in a code view Numpad Enter runs the buffer and main
//     Enter types a newline, and with the failed-gate state (the hooks unregistered and the keymap rebuilt) Numpad Enter
//     types a newline.
//   T-REPL-015: Numpad Enter in the REPL prompt forces the submission.
// The PostMessage interleavings are the "synthetic WM_KEYDOWN injector with and without bit 24" of §14.2. Focus moving
// between the code view, the find box and the viewport, and the RDP agent pass (T-KEY-016, tier 8) are not here (report).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamKeyRouter.h"
#include "Commands/DreamNumpadEnter.h"
#include "Fixtures/DreamTestProbes.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"

#include <atomic>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace DreamKeysG7Spec
{
	/** A focusable widget that swallows every key, so the posted keys have no effect anywhere. */
	class SKeySink : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SKeySink) {}
		SLATE_END_ARGS()
		void Construct(const FArguments&) {}
		virtual bool SupportsKeyboardFocus() const override { return true; }
		virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent&) override { return FReply::Handled(); }
		virtual FReply OnKeyUp(const FGeometry&, const FKeyEvent&) override { return FReply::Handled(); }
		virtual FReply OnKeyChar(const FGeometry&, const FCharacterEvent&) override { return FReply::Handled(); }
		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(240.0, 80.0); }
	};

	/**
	 * What the spec saw: one classification per EKeys::Enter key-down, in order. Game thread. Read from an input
	 * pre-processor, which Slate runs after every OnApplicationPreInputKeyDownListener (SlateApplication.cpp, key-down):
	 * a second listener on that multicast would run before FDreamNumpadEnter's (Broadcast walks the invocation list from
	 * the newest binding) and read the previous Enter's classification.
	 */
	struct FObserved
	{
		class FProcessor : public IInputProcessor
		{
		public:
			explicit FProcessor(FObserved& InOwner) : Owner(InOwner) {}
			virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
			virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override
			{
				if (Event.GetKey() == EKeys::Enter)
				{
					Owner.Classified.Add(FDreamNumpadEnter::Get().IsCurrentEnterNumpad());
					Owner.Count.fetch_add(1);
				}
				return false;
			}
			virtual const TCHAR* GetDebugName() const override { return TEXT("DreamKeysG7Observer"); }
		private:
			FObserved& Owner;
		};

		TArray<bool> Classified;
		TSharedPtr<FProcessor> Processor;
		std::atomic<int32> Count{ 0 };

		void Start()
		{
			Processor = MakeShared<FProcessor>(*this);
			FSlateApplication::Get().RegisterInputPreProcessor(Processor);
		}
		void Stop()
		{
			if (Processor.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().UnregisterInputPreProcessor(Processor);
			}
			Processor.Reset();
		}
	};

#if PLATFORM_WINDOWS
	LPARAM KeyLParam(uint32 Vk, bool bExtended, bool bRepeat, bool bUp, bool bAlt)
	{
		const uint32 Scan = ::MapVirtualKeyW(Vk, MAPVK_VK_TO_VSC) & 0xFF;
		uint64 Value = 1u | (static_cast<uint64>(Scan) << 16);
		if (bExtended) { Value |= 1ull << 24; }
		if (bAlt) { Value |= 1ull << 29; }
		if (bRepeat || bUp) { Value |= 1ull << 30; }
		if (bUp) { Value |= 1ull << 31; }
		return static_cast<LPARAM>(Value);
	}

	/** Posts one key press (down, then up) of Vk to Hwnd. */
	void PostPress(HWND Hwnd, uint32 Vk, bool bExtended, bool bRepeat, bool bSys)
	{
		::PostMessageW(Hwnd, bSys ? WM_SYSKEYDOWN : WM_KEYDOWN, Vk, KeyLParam(Vk, bExtended, bRepeat, false, bSys));
		::PostMessageW(Hwnd, bSys ? WM_SYSKEYUP : WM_KEYUP, Vk, KeyLParam(Vk, bExtended, false, true, bSys));
	}
#endif

	/** The 10,000-press state machine (T-KEY-014): a few presses per frame, checked once Slate has routed them. */
	struct FInterleave
	{
		FRandomStream Random{ 0x5017 };
		int32 Target = 10000;
		TArray<bool> Expected;
		int32 Batches = 0;
		int32 Others = 0;
		int32 Repeats = 0;
		int32 SysKeys = 0;
	};
}

BEGIN_DEFINE_SPEC(FDreamKeysG7Spec, "Dream.Keys.G7", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	TSharedPtr<SWindow> SinkWindow;
	TSharedPtr<DreamKeysG7Spec::SKeySink> Sink;
	TSharedPtr<DreamKeysG7Spec::FObserved> Observed;
	bool OpenSink();
	void* SinkHwnd() const;
END_DEFINE_SPEC(FDreamKeysG7Spec)

bool FDreamKeysG7Spec::OpenSink()
{
	if (!TestTrue(TEXT("FDreamNumpadEnter's hooks are registered (Win64)"), FDreamNumpadEnter::Get().IsAvailable()))
	{
		return false;
	}
	Sink = SNew(DreamKeysG7Spec::SKeySink);
	SinkWindow = SNew(SWindow)
		.Title(FText::AsCultureInvariant(TEXT("Dream Python Editor G7 key sink")))
		.ClientSize(FVector2D(240.0, 80.0))
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		.FocusWhenFirstShown(true);
	SinkWindow->SetContent(Sink.ToSharedRef());
	FSlateApplication::Get().AddWindow(SinkWindow.ToSharedRef(), /*bShowImmediately*/ true);
	FSlateApplication::Get().SetKeyboardFocus(Sink, EFocusCause::SetDirectly);
	Observed = MakeShared<DreamKeysG7Spec::FObserved>();
	Observed->Start();
	return TestNotNull(TEXT("the sink window has an OS window"), SinkHwnd());
}

void* FDreamKeysG7Spec::SinkHwnd() const
{
	return DreamTest::FDreamTestProbes::GetOsWindowHandle(SinkWindow);
}

void FDreamKeysG7Spec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		if (Observed.IsValid())
		{
			Observed->Stop();
			Observed.Reset();
		}
		if (SinkWindow.IsValid())
		{
			SinkWindow->RequestDestroyWindow();
			SinkWindow.Reset();
		}
		Sink.Reset();
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		Tab.Reset();
	});

#if PLATFORM_WINDOWS
	LatentIt("classifies 10,000 interleaved Enter key-downs with and without bit 24 (T-KEY-014)", FTimespan::FromSeconds(400.0),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !OpenSink())
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamKeysG7Spec::FInterleave> Mix = MakeShared<DreamKeysG7Spec::FInterleave>();
		Mix->Target = FMath::Max(100, DreamTest::CommandLineInt(TEXT("DreamG7Presses"), 10000));
		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(380.0);
		Steps.WaitFrames(2);
		Steps.Poll(TEXT("post the interleavings and let Slate route them"), [this, Mix]()
		{
			// Every Enter posted in earlier frames has been routed (one PumpMessages pass delivers the whole queue).
			if (Observed->Classified.Num() < Mix->Expected.Num())
			{
				return DreamTest::EStep::Wait;
			}
			if (Mix->Expected.Num() >= Mix->Target)
			{
				return DreamTest::EStep::Next;
			}
			FSlateApplication::Get().SetKeyboardFocus(Sink, EFocusCause::SetDirectly);
			const HWND Hwnd = static_cast<HWND>(SinkHwnd());
			const int32 Presses = Mix->Random.RandRange(1, 6);
			for (int32 Index = 0; Index < Presses && Mix->Expected.Num() < Mix->Target; ++Index)
			{
				const int32 Kind = Mix->Random.RandRange(0, 9);
				if (Kind == 0)
				{
					// Another key between the Enters: a letter, or an IME key-down (VK_PROCESSKEY), which must not push.
					const uint32 Vk = Mix->Random.RandRange(0, 1) == 0 ? static_cast<uint32>('A' + Mix->Random.RandRange(0, 25)) : VK_PROCESSKEY;
					DreamKeysG7Spec::PostPress(Hwnd, Vk, false, false, false);
					++Mix->Others;
					continue;
				}
				const bool bNumpad = Mix->Random.RandRange(0, 1) == 1;
				const bool bRepeat = Mix->Random.RandRange(0, 7) == 0;
				const bool bSys = Mix->Random.RandRange(0, 15) == 0;
				Mix->Repeats += bRepeat ? 1 : 0;
				Mix->SysKeys += bSys ? 1 : 0;
				DreamKeysG7Spec::PostPress(Hwnd, VK_RETURN, bNumpad, bRepeat, bSys);
				Mix->Expected.Add(bNumpad);
			}
			++Mix->Batches;
			return DreamTest::EStep::Wait;
		}, 370.0);
		Steps.Do(TEXT("compare the classifications"), [this, Mix]()
		{
			const TArray<bool>& Got = Observed->Classified;
			int32 Wrong = 0;
			int32 FirstWrong = INDEX_NONE;
			for (int32 Index = 0; Index < Mix->Expected.Num(); ++Index)
			{
				if (!Got.IsValidIndex(Index) || Got[Index] != Mix->Expected[Index])
				{
					++Wrong;
					FirstWrong = FirstWrong == INDEX_NONE ? Index : FirstWrong;
				}
			}
			AddInfo(FString::Printf(TEXT("%d Enter key-downs in %d batches (%d with bit 30, %d WM_SYSKEYDOWN), %d other keys between them"),
				Mix->Expected.Num(), Mix->Batches, Mix->Repeats, Mix->SysKeys, Mix->Others));
			TestEqual(TEXT("Slate routed exactly the Enters posted"), Got.Num(), Mix->Expected.Num());
			TestEqual(*FString::Printf(TEXT("misclassified Enters (first at %d): 100%% must match (G7, RUN-01)"), FirstWrong), Wrong, 0);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("classifies SendInput Enter with and without KEYEVENTF_EXTENDEDKEY (T-KEY-014)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !OpenSink() || !DreamTest::RequireEditorInFront(*this))
		{
			Done.Execute();
			return;
		}
		DreamTest::FDreamInputBot::BringEditorToFront(SinkHwnd());
		FSlateApplication::Get().SetKeyboardFocus(Sink, EFocusCause::SetDirectly);
		const int32 Presses = FMath::Clamp(DreamTest::CommandLineInt(TEXT("DreamG7SendInputPresses"), 100), 2, 1000);
		TSharedRef<TArray<bool>> Expected = MakeShared<TArray<bool>>();
		for (int32 Index = 0; Index < Presses; ++Index)
		{
			Expected->Add(Index % 3 != 0);
		}
		TSharedPtr<DreamKeysG7Spec::FObserved> Seen = Observed;
		TSharedRef<TSharedPtr<DreamTest::FDreamInputBotTicket>> Ticket = MakeShared<TSharedPtr<DreamTest::FDreamInputBotTicket>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.WaitFrames(2);
		Steps.Do(TEXT("start the bot"), [Expected, Seen, Ticket]()
		{
			const TArray<bool> Sequence = *Expected;
			*Ticket = DreamTest::FDreamInputBot::Submit([Sequence, Seen](DreamTest::FDreamInputBotContext& Bot)
			{
				if (!Bot.RequireForeground())
				{
					return;
				}
				for (int32 Index = 0; Index < Sequence.Num() && !Bot.HasFailed(); ++Index)
				{
					const int32 Before = Seen->Count.load();
					Bot.Chord({ Sequence[Index] ? DreamTest::FDreamInputBot::VkNumpadEnter() : DreamTest::FDreamInputBot::VkReturn() });
					Bot.WaitUntil([Seen, Before]() { return Seen->Count.load() > Before; }, 5.0,
						FString::Printf(TEXT("press %d to reach Slate"), Index + 1));
				}
			});
			return true;
		});
		Steps.Poll(TEXT("the bot's presses"), [this, Ticket]()
		{
			FSlateApplication::Get().SetKeyboardFocus(Sink, EFocusCause::SetDirectly);
			return DreamTest::IsTicketDone(Ticket->ToSharedRef()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 120.0);
		Steps.Do(TEXT("compare"), [this, Ticket, Expected]()
		{
			DreamTest::ReportTicket(*this, Ticket->ToSharedRef(), TEXT("G7 SendInput"));
			const TArray<bool>& Got = Observed->Classified;
			int32 Wrong = 0;
			for (int32 Index = 0; Index < Expected->Num(); ++Index)
			{
				Wrong += (!Got.IsValidIndex(Index) || Got[Index] != (*Expected)[Index]) ? 1 : 0;
			}
			TestEqual(TEXT("Slate saw one Enter per press"), Got.Num(), Expected->Num());
			TestEqual(TEXT("SendInput Enters misclassified (KEYEVENTF_EXTENDEDKEY = Numpad Enter)"), Wrong, 0);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("treats a synthetic Slate Enter as main and purges a Numpad Enter Slate never routed (T-KEY-015)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !OpenSink() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("import time\nfor _ in range(80): time.sleep(0.01)"), TEXT("G7Purge"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<bool> bPosted = MakeShared<bool>(false);
		TSharedRef<int32> SeenDuringRun = MakeShared<int32>(0);
		TSharedRef<FDelegateHandle> PreTick = MakeShared<FDelegateHandle>();
		struct FSavedRunSettings
		{
			bool bSaved = false;
			bool bRunPump = true;
			EDreamRunPresentationSetting Presentation = EDreamRunPresentationSetting::RunWindow;
		};
		TSharedRef<FSavedRunSettings> Saved = MakeShared<FSavedRunSettings>();
		FRunSupervisor* Supervisor = &FDreamEditorModule::Get().GetSupervisor();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("a synthetic Slate Enter is main Enter"), [this]()
		{
			FSlateApplication::Get().SetKeyboardFocus(Sink, EFocusCause::SetDirectly);
			const int32 Before = Observed->Classified.Num();
			const FKeyEvent Enter(EKeys::Enter, FModifierKeysState(), FSlateApplication::Get().GetUserIndexForKeyboard(), false, 13, 13);
			FSlateApplication::Get().ProcessKeyDownEvent(Enter);
			FSlateApplication::Get().ProcessKeyUpEvent(Enter);
			return TestTrue(TEXT("the listener saw the synthetic Enter"), Observed->Classified.Num() == Before + 1)
				&& TestFalse(TEXT("a synthetic Enter counts as main Enter (empty FIFO)"), Observed->Classified.Last());
		});
		Steps.Do(TEXT("start a run and post a Numpad Enter to the main frame from inside its pump"), [this, Buffer, Run, bPosted, SeenDuringRun, PreTick, Supervisor, Saved]()
		{
			TSharedPtr<DreamKeysG7Spec::FObserved> Seen = Observed;
			*PreTick = FSlateApplication::Get().OnPreTick().AddLambda([bPosted, SeenDuringRun, Supervisor, Seen](float)
			{
				if (*bPosted || Supervisor->GetPumpDepth() <= 0 || !Supervisor->IsRunWindowOpen())
				{
					return;
				}
				*bPosted = true;
				*SeenDuringRun = Seen->Classified.Num();
				// The Run window is the active modal: the platform filter drops this key-down after Dream's hook recorded it.
				DreamKeysG7Spec::PostPress(static_cast<HWND>(DreamTest::FDreamTestProbes::GetSnapshot().MainFrameHwnd), VK_RETURN, /*bExtended*/ true, false, false);
			});
			// The case needs the Run window as the active modal: force the supervised presentation, put the user's back after.
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Saved->bRunPump = Settings->bRunPump;
			Saved->Presentation = Settings->RunPresentation;
			Saved->bSaved = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 2));
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the pumped run"), Run, 30.0);
		Steps.WaitFrames(3);
		Steps.Do(TEXT("post a main Enter: the stale Numpad entry was purged"), [this, bPosted, SeenDuringRun]()
		{
			if (!TestTrue(TEXT("the Numpad Enter was posted inside the pump"), *bPosted))
			{
				return false;
			}
			TestEqual(TEXT("Slate routed no Enter while the Run window was modal (the platform filter dropped it)"), Observed->Classified.Num(), *SeenDuringRun);
			FSlateApplication::Get().SetKeyboardFocus(Sink, EFocusCause::SetDirectly);
			DreamKeysG7Spec::PostPress(static_cast<HWND>(SinkHwnd()), VK_RETURN, /*bExtended*/ false, false, false);
			return true;
		});
		Steps.Poll(TEXT("the main Enter is routed"), [this, SeenDuringRun]()
		{
			return Observed->Classified.Num() > *SeenDuringRun ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("check it"), [this, Run]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			TestFalse(TEXT("the main Enter after the purge is main Enter (FIFO purged after OnPostTick, T-KEY-015)"), Observed->Classified.Last());
			return true;
		});
		Steps.Finally(TEXT("remove the hook, restore the run settings"), [PreTick, Saved]()
		{
			if (PreTick->IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().OnPreTick().Remove(*PreTick);
			}
			if (Saved->bSaved)
			{
				UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
				Settings->bRunPump = Saved->bRunPump;
				Settings->RunPresentation = Saved->Presentation;
			}
		});
		Steps.Run(Done);
	});

	LatentIt("runs on Numpad Enter, types a newline on main Enter, submits the REPL, and falls back to a newline with the gate off (T-KEY-015, T-REPL-015)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this) || !TestTrue(TEXT("Numpad Enter hooks"), FDreamNumpadEnter::Get().IsAvailable()))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Script = FString::Printf(TEXT("g7_numpad_%s = 1"), *Sfx);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Script, TEXT("G7Keys"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		TSharedRef<int32> RunsBefore = MakeShared<int32>(0);
		TSharedRef<DreamTest::FRunResult> ReplRun = MakeShared<DreamTest::FRunResult>();
		TSharedRef<FString> PromptBefore = MakeShared<FString>();
		auto Post = [this](bool bNumpad)
		{
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			const TSharedPtr<SWindow> Window = View.IsValid() ? FSlateApplication::Get().FindWidgetWindow(View.ToSharedRef()) : nullptr;
			DreamKeysG7Spec::PostPress(static_cast<HWND>(DreamTest::FDreamTestProbes::GetOsWindowHandle(Window)), VK_RETURN, bNumpad, false, false);
		};
		auto FocusScript = [this, Buffer]()
		{
			Tab->ActivateBuffer(Buffer->GetId());
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			if (View.IsValid())
			{
				View->FocusEditor();
				View->SetSelection(FDreamTextSelection{ 0, View->GetText().Len(), 0, View->GetText().Len() });
			}
			return View.IsValid() && View->HasEditorFocus();
		};
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("Numpad Enter in the script"), [this, RunsBefore, Post, FocusScript]()
		{
			if (!TestTrue(TEXT("the script's code view has focus"), FocusScript()))
			{
				return false;
			}
			*RunsBefore = FDreamEditorModule::Get().GetRunService().GetHistory().Num() > 0 ? FDreamEditorModule::Get().GetRunService().GetHistory().Last().RunNumber : 0;
			Post(true);
			return true;
		});
		Steps.Poll(TEXT("Numpad Enter ran the buffer (RUN-01)"), [this, RunsBefore, Script]()
		{
			const FDreamRunRecord* Last = FDreamEditorModule::Get().GetRunService().GetLastFinished();
			if (Last == nullptr || Last->RunNumber <= *RunsBefore)
			{
				return DreamTest::EStep::Wait;
			}
			TestEqual(TEXT("Numpad Enter's run is ok"), DreamTest::OutcomeText(Last->Outcome), DreamTest::OutcomeText(EDreamRunOutcome::Ok));
			return DreamTest::EStep::Next;
		}, 20.0);
		Steps.WaitFrames(2);
		Steps.Do(TEXT("main Enter types a newline and runs nothing"), [this, Buffer, RunsBefore, Post, FocusScript, Script]()
		{
			FocusScript();
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			View->SetSelection(FDreamTextSelection{ 0, Script.Len(), 0, Script.Len() });
			*RunsBefore = FDreamEditorModule::Get().GetRunService().GetLastFinished() ? FDreamEditorModule::Get().GetRunService().GetLastFinished()->RunNumber : 0;
			Post(false);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("check the newline"), [this, Buffer, RunsBefore, Script]()
		{
			TestTrue(*FString::Printf(TEXT("main Enter inserted a newline: \"%s\""), *Buffer->GetTextAsString().ReplaceCharWithEscapedChar()),
				Buffer->GetTextAsString().StartsWith(Script + TEXT("\n")));
			TestTrue(TEXT("main Enter submitted no run"), DreamTest::IsIdle()
				&& (FDreamEditorModule::Get().GetRunService().GetLastFinished() == nullptr || FDreamEditorModule::Get().GetRunService().GetLastFinished()->RunNumber == *RunsBefore));
			if (const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView())
			{
				View->SetTextNoUndo(Script);
			}
			return true;
		});
		Steps.Do(TEXT("Numpad Enter in the REPL prompt forces the submission (T-REPL-015)"), [this, Sfx, PromptBefore, ReplRun]()
		{
			const TSharedPtr<SDreamReplStrip> Strip = Tab->GetOutputArea().IsValid() ? Tab->GetOutputArea()->GetReplStrip() : nullptr;
			const TSharedPtr<SDreamCodeView> Prompt = Strip.IsValid() ? Strip->GetPromptView() : nullptr;
			if (!TestTrue(TEXT("the REPL prompt exists"), Prompt.IsValid()))
			{
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::FocusRepl);
			const TSharedRef<FDreamBuffer> Repl = FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer();
			*PromptBefore = Repl->GetTextAsString();
			// An incomplete input: plain Enter would add a line; Numpad Enter, like Ctrl+Enter, submits it as it is (§6.5).
			const FString Second = FString::Printf(TEXT("    print('g7 repl %s', g7_i)"), *Sfx);
			Prompt->SetTextNoUndo(TEXT("for g7_i in range(1):\n") + Second);
			Prompt->SetSelection(FDreamTextSelection{ 1, Second.Len(), 1, Second.Len() });
			const FDreamRunRecord* Last = FDreamEditorModule::Get().GetRunService().GetLastFinished();
			ReplRun->RunNumber = (Last ? Last->RunNumber : 0) + 1;
			const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Prompt.ToSharedRef());
			DreamKeysG7Spec::PostPress(static_cast<HWND>(DreamTest::FDreamTestProbes::GetOsWindowHandle(Window)), VK_RETURN, true, false, false);
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the REPL submission"), ReplRun, 20.0);
		Steps.Do(TEXT("check the REPL run"), [this, ReplRun, Sfx]()
		{
			if (DreamTest::ExpectOutcome(*this, *ReplRun, EDreamRunOutcome::Ok))
			{
				TestEqual(TEXT("it ran from the REPL"), static_cast<int32>(ReplRun->Record.Route), static_cast<int32>(EDreamRunRoute::Repl));
				TestTrue(*FString::Printf(TEXT("the incomplete input was submitted: %s"), *ReplRun->Describe()),
					ReplRun->HasLineContaining(dream::LineType::Stdout, TEXT("g7 repl ") + Sfx));
			}
			return true;
		});
		Steps.Do(TEXT("the failed-gate state: Numpad Enter types a newline (T-KEY-015)"), [this, Buffer, RunsBefore, Post, FocusScript, Script]()
		{
			FDreamNumpadEnter::Get().Unregister();
			FDreamKeyRouter::Get().Rebuild();
			FocusScript();
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			View->SetSelection(FDreamTextSelection{ 0, Script.Len(), 0, Script.Len() });
			*RunsBefore = FDreamEditorModule::Get().GetRunService().GetLastFinished() ? FDreamEditorModule::Get().GetRunService().GetLastFinished()->RunNumber : 0;
			Post(true);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("check the fallback"), [this, Buffer, RunsBefore, Script]()
		{
			TestTrue(*FString::Printf(TEXT("with the gate off Numpad Enter inserted a newline: \"%s\""), *Buffer->GetTextAsString().ReplaceCharWithEscapedChar()),
				Buffer->GetTextAsString().StartsWith(Script + TEXT("\n")));
			TestTrue(TEXT("and ran nothing"), DreamTest::IsIdle()
				&& (FDreamEditorModule::Get().GetRunService().GetLastFinished() == nullptr || FDreamEditorModule::Get().GetRunService().GetLastFinished()->RunNumber == *RunsBefore));
			return true;
		});
		Steps.Finally(TEXT("restore the gate and the prompt"), [PromptBefore]()
		{
			if (!FDreamNumpadEnter::Get().IsAvailable())
			{
				FDreamNumpadEnter::Get().Register();
				FDreamKeyRouter::Get().Rebuild();
			}
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->ResetText(*PromptBefore, dream::EditOrigin::Load);
			}
		});
		Steps.Run(Done);
	});
#endif
}
