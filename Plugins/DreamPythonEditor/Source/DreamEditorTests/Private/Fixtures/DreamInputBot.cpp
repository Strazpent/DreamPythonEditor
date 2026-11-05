// SPDX-License-Identifier: Apache-2.0
#include "Fixtures/DreamInputBot.h"

#include "Containers/Queue.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace DreamTest
{
	namespace
	{
		struct FQueuedJob
		{
			FDreamInputBotJob Job;
			TSharedPtr<FDreamInputBotTicket> Ticket;
		};

		class FBotRunnable : public FRunnable
		{
		public:
			FBotRunnable()
				: Wake(FPlatformProcess::GetSynchEventFromPool(false))
			{
			}
			virtual ~FBotRunnable() override
			{
				FPlatformProcess::ReturnSynchEventToPool(Wake);
			}

			virtual uint32 Run() override
			{
				while (!bStop)
				{
					FQueuedJob Next;
					if (!Jobs.Dequeue(Next))
					{
						Wake->Wait(50);
						continue;
					}
					RunOne(Next);
				}
				// Jobs still queued at shutdown fail, so no spec waits forever.
				FQueuedJob Left;
				while (Jobs.Dequeue(Left))
				{
					Left.Ticket->Errors.Add(TEXT("input bot: shut down before the job ran"));
					Left.Ticket->bDone = true;
				}
				return 0;
			}

			virtual void Stop() override
			{
				bStop = true;
				Wake->Trigger();
			}

			void Enqueue(FQueuedJob&& Job)
			{
				Jobs.Enqueue(MoveTemp(Job));
				Wake->Trigger();
			}

		private:
			void RunOne(FQueuedJob& Item)
			{
				FDreamInputBotContext Context;
#if PLATFORM_WINDOWS
				POINT Saved{};
				const bool bSaved = ::GetCursorPos(&Saved) != 0;
#endif
				Context.ReleaseStuckModifiers();
				Item.Job(Context);
				Context.ReleaseHeldButtons();
#if PLATFORM_WINDOWS
				if (bSaved)
				{
					::SetCursorPos(Saved.x, Saved.y);
				}
#endif
				Item.Ticket->Errors = Context.GetErrors();
				Item.Ticket->Notes = Context.GetNotes();
				Item.Ticket->bDone = true;
			}

			TQueue<FQueuedJob, EQueueMode::Mpsc> Jobs;
			FEvent* Wake = nullptr;
			std::atomic<bool> bStop{ false };
		};

		struct FBotThread
		{
			FBotRunnable* Runnable = nullptr;
			FRunnableThread* Thread = nullptr;
		};

		FBotThread& Bot()
		{
			static FBotThread Instance;
			return Instance;
		}

#if PLATFORM_WINDOWS
		/** The root (owner chain top) of a window, for "is this window ours". */
		bool BelongsToThisProcess(HWND Window)
		{
			if (Window == nullptr)
			{
				return false;
			}
			DWORD Pid = 0;
			::GetWindowThreadProcessId(Window, &Pid);
			return Pid == ::GetCurrentProcessId();
		}

		FString DescribeWindow(HWND Window)
		{
			if (Window == nullptr)
			{
				return TEXT("no window");
			}
			// InternalGetWindowText reads the title without sending WM_GETTEXT: GetWindowTextW on one of this editor's windows
			// waits until the game thread pumps, which a blocking native call holds off (run, PO-04).
			TCHAR Title[256] = {};
			::InternalGetWindowText(Window, Title, UE_ARRAY_COUNT(Title));
			DWORD Pid = 0;
			::GetWindowThreadProcessId(Window, &Pid);
			return FString::Printf(TEXT("\"%s\" (pid %u)"), Title, static_cast<uint32>(Pid));
		}

		/** Raises Window to the top of the z-order without activating it, asynchronously (the game thread may be inside user
		 *  code): a window Slate shows without activating it (the Run window, RV-02) keeps its z-order slot under windows that
		 *  were above the editor. */
		void RaiseWithoutActivating(HWND Window)
		{
			::SetWindowPos(Window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
		}

		/** The window under Point is ExpectedHwnd (or one of its children), or with no ExpectedHwnd any window of this process. */
		bool IsPointOurs(const POINT& Point, void* ExpectedHwnd, HWND* OutRoot = nullptr)
		{
			const HWND Under = ::WindowFromPoint(Point);
			const HWND Root = Under ? ::GetAncestor(Under, GA_ROOT) : nullptr;
			if (OutRoot != nullptr)
			{
				*OutRoot = Root;
			}
			return ExpectedHwnd != nullptr ? (Root == static_cast<HWND>(ExpectedHwnd) || Under == static_cast<HWND>(ExpectedHwnd))
				: BelongsToThisProcess(Root);
		}

		double SendInputs(TArray<INPUT>& Inputs, FDreamInputBotContext& Context, const TCHAR* What)
		{
			const double Now = FPlatformTime::Seconds();
			const UINT Sent = ::SendInput(static_cast<UINT>(Inputs.Num()), Inputs.GetData(), sizeof(INPUT));
			if (Sent != static_cast<UINT>(Inputs.Num()))
			{
				Context.Fail(FString::Printf(TEXT("input bot: SendInput sent %u of %d events for %s (error %u): the input is blocked "
					"(a locked desktop, UIPI or another desktop); tier 4 --attended needs an unlocked interactive session"),
					Sent, Inputs.Num(), What, static_cast<uint32>(::GetLastError())));
				return 0.0;
			}
			return Now;
		}

		INPUT KeyInput(const FDreamBotKey& Key, bool bUp)
		{
			INPUT Input{};
			Input.type = INPUT_KEYBOARD;
			Input.ki.wVk = Key.VirtualKey;
			Input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(Key.VirtualKey, MAPVK_VK_TO_VSC));
			Input.ki.dwFlags = (Key.bExtended ? KEYEVENTF_EXTENDEDKEY : 0) | (bUp ? KEYEVENTF_KEYUP : 0);
			return Input;
		}

		INPUT MouseInput(DWORD Flags, int32 Data = 0)
		{
			INPUT Input{};
			Input.type = INPUT_MOUSE;
			Input.mi.dwFlags = Flags;
			Input.mi.mouseData = static_cast<DWORD>(Data);
			return Input;
		}
#endif
	}

	// ---- Context (bot thread) -------------------------------------------------------------------------------------

	bool FDreamInputBotContext::WaitFor(TFunctionRef<bool(const FDreamProbeSnapshot&)> Condition, double TimeoutSeconds, const FString& What)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!bFailed)
		{
			if (Condition(FDreamTestProbes::GetSnapshot()))
			{
				return true;
			}
			if (FPlatformTime::Seconds() > Deadline)
			{
				Fail(FString::Printf(TEXT("input bot: timed out after %.1f s waiting for %s"), TimeoutSeconds, *What));
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return false;
	}

	bool FDreamInputBotContext::WaitUntil(TFunctionRef<bool()> Condition, double TimeoutSeconds, const FString& What)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!bFailed)
		{
			if (Condition())
			{
				return true;
			}
			if (FPlatformTime::Seconds() > Deadline)
			{
				Fail(FString::Printf(TEXT("input bot: timed out after %.1f s waiting for %s"), TimeoutSeconds, *What));
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return false;
	}

	bool FDreamInputBotContext::RequireForeground()
	{
		if (!FDreamInputBot::IsDesktopInteractive())
		{
			Fail(TEXT("input bot: the input desktop is locked or not interactive; SendInput needs an unlocked desktop session (TDD 14.1)"));
			return false;
		}
		FString What;
		if (!FDreamInputBot::IsEditorForeground(What))
		{
			Fail(FString::Printf(TEXT("input bot: the editor is not the foreground window (the foreground is %s). SendInput keys go to the "
				"foreground window: bring the DreamHost editor to the front and keep the desktop free while tier 4 --attended runs"), *What));
			return false;
		}
		return true;
	}

	bool FDreamInputBotContext::MoveTo(const FSlateRect& Rect, void* ExpectedHwnd, const FString& What)
	{
		if (Rect.GetSize().X < 1.f || Rect.GetSize().Y < 1.f)
		{
			Fail(FString::Printf(TEXT("input bot: %s has no published geometry"), *What));
			return false;
		}
		FVector2D Point(Rect.GetCenter());
		FindUncoveredPoint(Rect, ExpectedHwnd, Point);     // keeps the centre when every point is covered: MoveToPoint says so
		return MoveToPoint(Point, ExpectedHwnd, What);
	}

	bool FDreamInputBotContext::FindUncoveredPoint(const FSlateRect& Rect, void* ExpectedHwnd, FVector2D& OutPoint)
	{
#if PLATFORM_WINDOWS
		const auto Ours = [ExpectedHwnd](const FVector2D& P)
		{
			const POINT At{ FMath::RoundToInt(P.X), FMath::RoundToInt(P.Y) };
			return IsPointOurs(At, ExpectedHwnd);
		};
		const FVector2D Centre(Rect.GetCenter());
		if (Ours(Centre))
		{
			OutPoint = Centre;
			return true;
		}
		constexpr int32 Grid = 7;
		const FVector2D Size(Rect.GetSize());
		for (int32 Row = 0; Row < Grid; ++Row)
		{
			for (int32 Column = 0; Column < Grid; ++Column)
			{
				const FVector2D P(Rect.Left + Size.X * (Column + 0.5) / Grid, Rect.Top + Size.Y * (Row + 0.5) / Grid);
				if (Ours(P))
				{
					OutPoint = P;
					return true;
				}
			}
		}
#endif
		return false;
	}

	bool FDreamInputBotContext::FindUncoveredPointRaising(const FSlateRect& Rect, void* ExpectedHwnd, FVector2D& OutPoint, const FString& What)
	{
		if (FindUncoveredPoint(Rect, ExpectedHwnd, OutPoint))
		{
			return true;
		}
#if PLATFORM_WINDOWS
		if (ExpectedHwnd != nullptr)
		{
			RaiseWithoutActivating(static_cast<HWND>(ExpectedHwnd));
			const double RaiseDeadline = FPlatformTime::Seconds() + 1.0;
			while (FPlatformTime::Seconds() < RaiseDeadline)
			{
				if (FindUncoveredPoint(Rect, ExpectedHwnd, OutPoint))
				{
					Note(FString::Printf(TEXT("input bot: raised the window of %s above another application's window"), *What));
					return true;
				}
				FPlatformProcess::Sleep(0.005f);
			}
		}
#endif
		return false;
	}

	bool FDreamInputBotContext::MoveToPoint(const FVector2D& Point, void* ExpectedHwnd, const FString& What)
	{
#if PLATFORM_WINDOWS
		const int32 X = FMath::RoundToInt(Point.X);
		const int32 Y = FMath::RoundToInt(Point.Y);
		if (!::SetCursorPos(X, Y))
		{
			Fail(FString::Printf(TEXT("input bot: SetCursorPos(%d, %d) for %s failed (error %u)"), X, Y, *What, static_cast<uint32>(::GetLastError())));
			return false;
		}
		POINT At{};
		::GetCursorPos(&At);
		if (FMath::Abs(At.x - X) > 1 || FMath::Abs(At.y - Y) > 1)
		{
			Fail(FString::Printf(TEXT("input bot: the cursor is at (%d, %d), not at %s (%d, %d)"), At.x, At.y, *What, X, Y));
			return false;
		}
		HWND Root = nullptr;
		if (!IsPointOurs(At, ExpectedHwnd, &Root) && ExpectedHwnd != nullptr)
		{
			// The editor is in front, yet another application's window is above ours at this point: raise ours and look again.
			RaiseWithoutActivating(static_cast<HWND>(ExpectedHwnd));
			const double RaiseDeadline = FPlatformTime::Seconds() + 1.0;
			while (!IsPointOurs(At, ExpectedHwnd, &Root) && FPlatformTime::Seconds() < RaiseDeadline)
			{
				FPlatformProcess::Sleep(0.005f);
			}
			if (IsPointOurs(At, ExpectedHwnd, &Root))
			{
				Note(FString::Printf(TEXT("input bot: raised the window of %s above another application's window"), *What));
			}
		}
		if (!IsPointOurs(At, ExpectedHwnd, &Root))
		{
			Fail(FString::Printf(TEXT("input bot: the window under %s at (%d, %d) is %s, not the expected editor window; something covers it"),
				*What, X, Y, *DescribeWindow(Root)));
			return false;
		}
		return true;
#else
		Fail(TEXT("input bot: Win64 only"));
		return false;
#endif
	}

	double FDreamInputBotContext::LeftDown()
	{
#if PLATFORM_WINDOWS
		TArray<INPUT> Inputs{ MouseInput(MOUSEEVENTF_LEFTDOWN) };
		const double At = SendInputs(Inputs, *this, TEXT("a left button down"));
		bLeftDown = bLeftDown || At > 0.0;
		return At;
#else
		Fail(TEXT("input bot: Win64 only"));
		return 0.0;
#endif
	}

	double FDreamInputBotContext::LeftUp()
	{
#if PLATFORM_WINDOWS
		TArray<INPUT> Inputs{ MouseInput(MOUSEEVENTF_LEFTUP) };
		const double At = SendInputs(Inputs, *this, TEXT("a left button up"));
		bLeftDown = bLeftDown && At <= 0.0;
		return At;
#else
		Fail(TEXT("input bot: Win64 only"));
		return 0.0;
#endif
	}

	void FDreamInputBotContext::ReleaseStuckModifiers()
	{
#if PLATFORM_WINDOWS
		// A modifier the OS still holds down (an earlier job or spec that pressed it and never released it) would turn this
		// job's chords into other chords: Alt+Enter into Ctrl+Alt+Enter. Release it, and say so.
		static const WORD Modifiers[] = { VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
		TArray<INPUT> Inputs;
		FString Names;
		for (const WORD Vk : Modifiers)
		{
			if ((::GetAsyncKeyState(Vk) & 0x8000) != 0)
			{
				const bool bExtended = Vk == VK_RCONTROL || Vk == VK_RMENU || Vk == VK_LWIN || Vk == VK_RWIN;
				Inputs.Add(KeyInput(FDreamBotKey{ Vk, bExtended }, /*bUp*/ true));
				Names += FString::Printf(TEXT(" 0x%02X"), Vk);
			}
		}
		if (Inputs.Num() > 0)
		{
			::SendInput(static_cast<UINT>(Inputs.Num()), Inputs.GetData(), sizeof(INPUT));
			Note(FString::Printf(TEXT("input bot: released modifier keys the OS still held down:%s"), *Names));
		}
#endif
	}

	void FDreamInputBotContext::ReleaseHeldButtons()
	{
#if PLATFORM_WINDOWS
		if (bLeftDown)
		{
			TArray<INPUT> Inputs{ MouseInput(MOUSEEVENTF_LEFTUP) };
			::SendInput(static_cast<UINT>(Inputs.Num()), Inputs.GetData(), sizeof(INPUT));
			bLeftDown = false;
			Note(TEXT("input bot: released the left button the job left down"));
		}
#endif
	}

	double FDreamInputBotContext::LeftClick()
	{
#if PLATFORM_WINDOWS
		TArray<INPUT> Inputs{ MouseInput(MOUSEEVENTF_LEFTDOWN), MouseInput(MOUSEEVENTF_LEFTUP) };
		return SendInputs(Inputs, *this, TEXT("a left click"));
#else
		Fail(TEXT("input bot: Win64 only"));
		return 0.0;
#endif
	}

	double FDreamInputBotContext::Wheel(int32 Notches)
	{
#if PLATFORM_WINDOWS
		TArray<INPUT> Inputs{ MouseInput(MOUSEEVENTF_WHEEL, Notches * WHEEL_DELTA) };
		return SendInputs(Inputs, *this, TEXT("a wheel scroll"));
#else
		Fail(TEXT("input bot: Win64 only"));
		return 0.0;
#endif
	}

	double FDreamInputBotContext::Chord(const TArray<FDreamBotKey>& Keys)
	{
#if PLATFORM_WINDOWS
		TArray<INPUT> Inputs;
		for (const FDreamBotKey& K : Keys)
		{
			Inputs.Add(KeyInput(K, false));
		}
		for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
		{
			Inputs.Add(KeyInput(Keys[Index], true));
		}
		return SendInputs(Inputs, *this, TEXT("a chord"));
#else
		Fail(TEXT("input bot: Win64 only"));
		return 0.0;
#endif
	}

	double FDreamInputBotContext::Key(const FDreamBotKey& InKey, bool bUp)
	{
#if PLATFORM_WINDOWS
		TArray<INPUT> Inputs{ KeyInput(InKey, bUp) };
		return SendInputs(Inputs, *this, bUp ? TEXT("a key up") : TEXT("a key down"));
#else
		Fail(TEXT("input bot: Win64 only"));
		return 0.0;
#endif
	}

	double FDreamInputBotContext::TypeUnicode(const FString& Text)
	{
#if PLATFORM_WINDOWS
		TArray<INPUT> Inputs;
		for (const TCHAR Ch : Text)
		{
			for (const bool bUp : { false, true })
			{
				INPUT Input{};
				Input.type = INPUT_KEYBOARD;
				Input.ki.wScan = static_cast<WORD>(Ch);
				Input.ki.dwFlags = KEYEVENTF_UNICODE | (bUp ? KEYEVENTF_KEYUP : 0);
				Inputs.Add(Input);
			}
		}
		return SendInputs(Inputs, *this, TEXT("typed text"));
#else
		Fail(TEXT("input bot: Win64 only"));
		return 0.0;
#endif
	}

	void FDreamInputBotContext::Fail(const FString& Message)
	{
		bFailed = true;
		Errors.Add(Message);
	}

	void FDreamInputBotContext::Note(const FString& Message)
	{
		Notes.Add(Message);
	}

	// ---- The bot ----------------------------------------------------------------------------------------------------

	bool FDreamInputBot::IsSupported()
	{
		return PLATFORM_WINDOWS != 0;
	}

	TSharedRef<FDreamInputBotTicket> FDreamInputBot::Submit(FDreamInputBotJob&& Job)
	{
		check(IsInGameThread());
		TSharedRef<FDreamInputBotTicket> Ticket = MakeShared<FDreamInputBotTicket>();
		if (!IsSupported())
		{
			Ticket->Errors.Add(TEXT("input bot: SendInput is Win64 only"));
			Ticket->bDone = true;
			return Ticket;
		}
		FBotThread& B = Bot();
		if (B.Thread == nullptr)
		{
			B.Runnable = new FBotRunnable();
			B.Thread = FRunnableThread::Create(B.Runnable, TEXT("DreamTestInputBot"), 0, TPri_AboveNormal);
		}
		B.Runnable->Enqueue(FQueuedJob{ MoveTemp(Job), Ticket });
		return Ticket;
	}

	void FDreamInputBot::Shutdown()
	{
		FBotThread& B = Bot();
		if (B.Thread != nullptr)
		{
			B.Thread->Kill(/*bShouldWait*/ true);
			delete B.Thread;
			B.Thread = nullptr;
		}
		delete B.Runnable;
		B.Runnable = nullptr;
	}

#if PLATFORM_WINDOWS
	FDreamBotKey FDreamInputBot::VkEscape() { return { VK_ESCAPE, false }; }
	FDreamBotKey FDreamInputBot::VkShift() { return { VK_LSHIFT, false }; }
	FDreamBotKey FDreamInputBot::VkControl() { return { VK_LCONTROL, false }; }
	FDreamBotKey FDreamInputBot::VkAlt() { return { VK_LMENU, false }; }
	FDreamBotKey FDreamInputBot::VkRightAlt() { return { VK_RMENU, true }; }
	FDreamBotKey FDreamInputBot::VkF5() { return { VK_F5, false }; }
	FDreamBotKey FDreamInputBot::VkReturn() { return { VK_RETURN, false }; }
	FDreamBotKey FDreamInputBot::VkNumpadEnter() { return { VK_RETURN, true }; }
	FDreamBotKey FDreamInputBot::VkLetter(TCHAR Letter) { return { static_cast<uint16>(FChar::ToUpper(Letter)), false }; }
#else
	FDreamBotKey FDreamInputBot::VkEscape() { return {}; }
	FDreamBotKey FDreamInputBot::VkShift() { return {}; }
	FDreamBotKey FDreamInputBot::VkControl() { return {}; }
	FDreamBotKey FDreamInputBot::VkAlt() { return {}; }
	FDreamBotKey FDreamInputBot::VkRightAlt() { return {}; }
	FDreamBotKey FDreamInputBot::VkF5() { return {}; }
	FDreamBotKey FDreamInputBot::VkReturn() { return {}; }
	FDreamBotKey FDreamInputBot::VkNumpadEnter() { return {}; }
	FDreamBotKey FDreamInputBot::VkLetter(TCHAR) { return {}; }
#endif

	bool FDreamInputBot::ParseChord(const FString& Text, TArray<FDreamBotKey>& OutKeys)
	{
		OutKeys.Reset();
#if PLATFORM_WINDOWS
		TArray<FString> Parts;
		Text.ToLower().ParseIntoArray(Parts, TEXT("+"));
		for (const FString& Part : Parts)
		{
			const FString P = Part.TrimStartAndEnd();
			if (P == TEXT("ctrl") || P == TEXT("control")) { OutKeys.Add(VkControl()); }
			else if (P == TEXT("shift")) { OutKeys.Add(VkShift()); }
			else if (P == TEXT("alt")) { OutKeys.Add(VkAlt()); }
			else if (P == TEXT("altgr")) { OutKeys.Add(VkControl()); OutKeys.Add(VkRightAlt()); }
			else if (P == TEXT("esc") || P == TEXT("escape")) { OutKeys.Add(VkEscape()); }
			else if (P == TEXT("enter") || P == TEXT("return")) { OutKeys.Add(VkReturn()); }
			else if (P == TEXT("numpadenter")) { OutKeys.Add(VkNumpadEnter()); }
			else if (P == TEXT("tab")) { OutKeys.Add({ VK_TAB, false }); }
			else if (P == TEXT("space")) { OutKeys.Add({ VK_SPACE, false }); }
			else if (P == TEXT("delete") || P == TEXT("del")) { OutKeys.Add({ VK_DELETE, true }); }
			else if (P == TEXT("backspace")) { OutKeys.Add({ VK_BACK, false }); }
			else if (P == TEXT("up")) { OutKeys.Add({ VK_UP, true }); }
			else if (P == TEXT("down")) { OutKeys.Add({ VK_DOWN, true }); }
			else if (P == TEXT("left")) { OutKeys.Add({ VK_LEFT, true }); }
			else if (P == TEXT("right")) { OutKeys.Add({ VK_RIGHT, true }); }
			else if (P == TEXT("home")) { OutKeys.Add({ VK_HOME, true }); }
			else if (P == TEXT("end")) { OutKeys.Add({ VK_END, true }); }
			else if (P == TEXT("pageup")) { OutKeys.Add({ VK_PRIOR, true }); }
			else if (P == TEXT("pagedown")) { OutKeys.Add({ VK_NEXT, true }); }
			else if (P.Len() >= 2 && P[0] == TEXT('f') && P.Mid(1).IsNumeric())
			{
				const int32 N = FCString::Atoi(*P.Mid(1));
				if (N < 1 || N > 24)
				{
					return false;
				}
				OutKeys.Add({ static_cast<uint16>(VK_F1 + N - 1), false });
			}
			else if (P.Len() == 1 && FChar::IsAlnum(P[0]))
			{
				OutKeys.Add(VkLetter(P[0]));
			}
			else
			{
				return false;
			}
		}
		return OutKeys.Num() > 0;
#else
		return false;
#endif
	}

	bool FDreamInputBot::BringEditorToFront(void* WindowHwnd)
	{
#if PLATFORM_WINDOWS
		check(IsInGameThread());
		HWND Target = static_cast<HWND>(WindowHwnd);
		if (Target == nullptr)
		{
			Target = static_cast<HWND>(FDreamTestProbes::GetSnapshot().MainFrameHwnd);
		}
		if (Target == nullptr)
		{
			return false;
		}
		FString What;
		// With no window named, any editor window in front will do; a named window must be the foreground one itself (the
		// floating Dream tab's window, say, so OS keys reach the window that holds Slate's focus).
		if (WindowHwnd == nullptr ? IsEditorForeground(What) : ::GetForegroundWindow() == Target)
		{
			return true;
		}
		// The foreground lock lets a process take the foreground only while it shares the foreground thread's input state.
		const HWND Foreground = ::GetForegroundWindow();
		const DWORD ForegroundThread = Foreground ? ::GetWindowThreadProcessId(Foreground, nullptr) : 0;
		const DWORD ThisThread = ::GetCurrentThreadId();
		const bool bAttached = ForegroundThread != 0 && ForegroundThread != ThisThread && ::AttachThreadInput(ForegroundThread, ThisThread, 1) != 0;
		if (::IsIconic(Target))
		{
			::ShowWindow(Target, SW_RESTORE);
		}
		::BringWindowToTop(Target);
		::SetForegroundWindow(Target);
		if (bAttached)
		{
			::AttachThreadInput(ForegroundThread, ThisThread, 0);
		}
		return WindowHwnd == nullptr ? IsEditorForeground(What) : ::GetForegroundWindow() == Target;
#else
		return false;
#endif
	}

	bool FDreamInputBot::IsEditorForeground(FString& OutWhat)
	{
#if PLATFORM_WINDOWS
		const HWND Foreground = ::GetForegroundWindow();
		OutWhat = DescribeWindow(Foreground);
		return BelongsToThisProcess(Foreground);
#else
		OutWhat = TEXT("unsupported platform");
		return false;
#endif
	}

	bool FDreamInputBot::IsDesktopInteractive()
	{
#if PLATFORM_WINDOWS
		const HDESK Desktop = ::OpenInputDesktop(0, 0, DESKTOP_READOBJECTS);
		if (Desktop == nullptr)
		{
			return false;
		}
		TCHAR Name[64] = {};
		DWORD Needed = 0;
		const bool bNamed = ::GetUserObjectInformationW(Desktop, UOI_NAME, Name, sizeof(Name), &Needed) != 0;
		::CloseDesktop(Desktop);
		return bNamed && FCString::Stricmp(Name, TEXT("Default")) == 0;
#else
		return false;
#endif
	}
}
