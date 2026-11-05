// SPDX-License-Identifier: Apache-2.0
// Input during a supervised run (TDD §5.6, RT-04, RT-05, E7, PO-03): the §5.6.2 decision table, the Esc rule (§5.6.3),
// Stop targets and the OS drag-drop guard (§5.6.4). Registered from the first pump, so it runs inside our pump's
// PumpMessages or the engine's own modal loops; game thread only. A Stop here only sets the supervisor's atomics
// through FDreamRunService (delivery happens in the pending call, §5.4.2 step 9).
#include "Commands/DreamInputPreProcessor.h"

#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamCommands.h"
#include "Run/DreamRunService.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandInfo.h"
#include "GenericPlatform/GenericApplication.h"
#include "Input/Events.h"
#include "Layout/WidgetPath.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/SMultiLineEditableText.h"

namespace DreamInputPreProcessorPrivate
{
	/** The three columns of §5.6.2. */
	enum class EModalContext : uint8
	{
		None,       // docked, or CanAddModalWindow() false (§5.5.1)
		Ours,       // the Run window or the Paused window is the active modal
		Foreign,    // a script's dialog or slow task, or an engine modal
	};

	/** The run-level Stop chord: the Stop command's active chords (Shift+F5 by default, §4.9.4). */
	bool IsStopChord(const FKeyEvent& Event)
	{
		const FInputChord EventChord(Event.GetKey(),
			EModifierKey::FromBools(Event.IsControlDown(), Event.IsAltDown(), Event.IsShiftDown(), Event.IsCommandDown()));
		if (FDreamCommands::IsRegistered())
		{
			const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(EDreamCommand::Stop);
			if (Info.IsValid() && (Info->GetActiveChord(EMultipleKeyBindingIndex::Primary)->IsValidChord()
				|| Info->GetActiveChord(EMultipleKeyBindingIndex::Secondary)->IsValidChord()))
			{
				return Info->HasActiveChord(EventChord);
			}
		}
		return EventChord == FInputChord(EKeys::F5, EModifierKey::Shift);
	}

	/** One entry per pointer and button, so a down and its up pair up (§5.6.2 "Mouse up"). */
	uint32 PointerButtonKey(const FPointerEvent& Event)
	{
		return HashCombine(GetTypeHash(Event.GetPointerIndex()), GetTypeHash(Event.GetEffectingButton()));
	}

	/** The top-level window under the pointer, ignoring the enabled state that a modal takes from the others. */
	TSharedPtr<SWindow> WindowUnderPointer(FSlateApplication& App, const FPointerEvent& Event, FWidgetPath* OutPath = nullptr)
	{
		FWidgetPath Path = App.LocateWindowUnderMouse(Event.GetScreenSpacePosition(), App.GetInteractiveTopLevelWindows(),
			/*bIgnoreEnabledStatus=*/true, static_cast<int32>(Event.GetUserIndex()));
		if (!Path.IsValid())
		{
			return nullptr;
		}
		TSharedPtr<SWindow> Window = Path.GetWindow();
		if (OutPath != nullptr)
		{
			*OutPath = MoveTemp(Path);
		}
		return Window;
	}

	bool IsWithinWindowTree(const TSharedPtr<SWindow>& Window, const TSharedPtr<SWindow>& Root)
	{
		return Window.IsValid() && Root.IsValid() && (Window == Root || Window->IsDescendantOf(Root));
	}
}

FDreamInputPreProcessor::FDreamInputPreProcessor(TWeakPtr<FDreamRunService> InService)
	: Service(MoveTemp(InService))
{
}

void FDreamInputPreProcessor::SetContext(const FContext& InContext)
{
	Context = InContext;
}

// ---- Keys (§5.6.2, §5.6.3) ------------------------------------------------------------------------------------------

bool FDreamInputPreProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	using namespace DreamInputPreProcessorPrivate;
	const FKey Key = InKeyEvent.GetKey();

	// Shift+F5 (and the Stop command's chords): Stop and swallow in every context.
	if (IsStopChord(InKeyEvent))
	{
		if (!InKeyEvent.IsRepeat())
		{
			if (const TSharedPtr<FDreamRunService> Pinned = Service.Pin())
			{
				Pinned->RequestStopActive(EDreamStopSource::ShiftF5);
			}
		}
		SwallowedKeys.Add(Key);
		return true;
	}

	// Esc: Stop under the RT-05 rule, else pass so a script's dialog gets Cancel (Dialogs.cpp:253-258).
	if (Key == EKeys::Escape)
	{
		if (IsEscStop(SlateApp))
		{
			if (!InKeyEvent.IsRepeat())
			{
				if (const TSharedPtr<FDreamRunService> Pinned = Service.Pin())
				{
					Pinned->RequestStopActive(EDreamStopSource::Esc);
				}
			}
			SwallowedKeys.Add(Key);
			return true;
		}
		return false;
	}

	// Other keys: a modal (ours or foreign) gets them; with no modal they are swallowed with their key-up.
	const TSharedPtr<SWindow> Modal = SlateApp.GetActiveModalWindow();
	if (Modal.IsValid())
	{
		return false;
	}
	SwallowedKeys.Add(Key);
	return true;
}

bool FDreamInputPreProcessor::HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	if (SwallowedKeys.Remove(InKeyEvent.GetKey()) > 0)
	{
		return true;
	}
	// No modal: every key-up is swallowed; with a modal the key-up of a passed key-down passes too.
	return !SlateApp.GetActiveModalWindow().IsValid();
}

bool FDreamInputPreProcessor::IsEscStop(FSlateApplication& App) const
{
	// §5.6.3: our modal or none is active, no menu is visible, and no editable text widget has keyboard focus.
	const TSharedPtr<SWindow> Modal = App.GetActiveModalWindow();
	if (Modal.IsValid() && Modal != Context.RunWindow.Pin() && Modal != Context.PausedWindow.Pin())
	{
		return false;
	}
	if (App.AnyMenusVisible())
	{
		return false;
	}
	return !IsTextEntryFocused(App);
}

bool FDreamInputPreProcessor::IsTextEntryFocused(FSlateApplication& App) const
{
	// The focused-widget test (SlateApplication.h:1653): editable text whose IsTextReadOnly() is false
	// (ISlateEditableTextWidget.h:208), or a Dream code view that is not read-only. Code views are read-only while a
	// run is active (A27), so in the Run window Esc stops; the Paused window's text fields keep it (M3).
	const TSharedPtr<SWidget> Focused = App.GetKeyboardFocusedWidget();
	if (!Focused.IsValid())
	{
		return false;
	}
	// A Dream code view's text widget answers from its owner: the widget's own read-only flag is a TSlateAttribute whose
	// cached value is refreshed only at Slate's attribute update, so in the first pump of a run (PumpMessages runs queued
	// input before Tick) it still says "editable" and Esc would not stop (RT-05).
	int32 Depth = 0;
	for (TSharedPtr<SWidget> Widget = Focused; Widget.IsValid() && Depth < 8; Widget = Widget->GetParentWidget(), ++Depth)
	{
		if (Widget->GetType() == FName(TEXT("SDreamCodeView")))
		{
			return !StaticCastSharedPtr<SDreamCodeView>(Widget)->IsReadOnly();
		}
	}
	const FName Type = Focused->GetType();
	if (Type == FName(TEXT("SEditableText")))
	{
		return !StaticCastSharedPtr<SEditableText>(Focused)->IsTextReadOnly();
	}
	if (Type == FName(TEXT("SMultiLineEditableText")))
	{
		return !StaticCastSharedPtr<SMultiLineEditableText>(Focused)->IsTextReadOnly();
	}
	if (Type == FName(TEXT("SDreamCodeView")))
	{
		return !StaticCastSharedPtr<SDreamCodeView>(Focused)->IsReadOnly();
	}
	return false;
}

// ---- Pointer (§5.6.2, §5.6.4) ---------------------------------------------------------------------------------------

bool FDreamInputPreProcessor::IsInsideWindow(FSlateApplication& App, const FPointerEvent& Event, const TWeakPtr<SWindow>& Window) const
{
	return DreamInputPreProcessorPrivate::IsWithinWindowTree(DreamInputPreProcessorPrivate::WindowUnderPointer(App, Event), Window.Pin());
}

bool FDreamInputPreProcessor::IsOverStopTarget(FSlateApplication& App, const FPointerEvent& Event) const
{
	// The Translation Picker pattern (07b Q5): the widget path under the pointer against the registered targets.
	FWidgetPath Path;
	const TSharedPtr<SWindow> Under = DreamInputPreProcessorPrivate::WindowUnderPointer(App, Event, &Path);
	if (!Under.IsValid())
	{
		return false;
	}
	auto Contains = [&Path, &Under](const TWeakPtr<SWidget>& Target)
	{
		const TSharedPtr<SWidget> Pinned = Target.Pin();
		if (!Pinned.IsValid())
		{
			return false;
		}
		if (Pinned == StaticCastSharedPtr<SWidget>(Under) || Path.ContainsWidget(Pinned.Get()))
		{
			return true;
		}
		// A window registered as a target covers its child windows (menus of its own controls).
		return Pinned->Advanced_IsWindow() && Under->IsDescendantOf(StaticCastSharedPtr<SWindow>(Pinned));
	};
	for (const TWeakPtr<SWidget>& Target : Context.StopTargets)
	{
		if (Contains(Target))
		{
			return true;
		}
	}
	// Docked mode passes the output view (M4); the Run window's live run view sits inside the Run window target.
	return Contains(Context.OutputView) || Contains(Context.LiveRunView);
}

bool FDreamInputPreProcessor::HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	// Real moves pass to any modal; with no modal they are swallowed outside Stop targets (synthetic moves bypass
	// pre-processors, 07b Q5).
	if (SlateApp.GetActiveModalWindow().IsValid())
	{
		return false;
	}
	return !IsOverStopTarget(SlateApp, MouseEvent);
}

bool FDreamInputPreProcessor::HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	using namespace DreamInputPreProcessorPrivate;
	const TSharedPtr<SWindow> Modal = SlateApp.GetActiveModalWindow();
	bool bPass = false;
	if (!Modal.IsValid())
	{
		bPass = IsOverStopTarget(SlateApp, MouseEvent);
	}
	else
	{
		// Ours or foreign: pass inside the active modal's window tree (the Run window's Stop, Interrupt cleanup and the
		// live view's scrollbar and chips; a script dialog's controls), else swallow.
		bPass = IsInsideWindow(SlateApp, MouseEvent, Modal);
	}
	// Swallowing a down requires swallowing its up: Slate clears pressed-button state on a swallowed up only for touch.
	DownSwallowedByPointer.Add(PointerButtonKey(MouseEvent), !bPass);
	return !bPass;
}

bool FDreamInputPreProcessor::HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	return HandleMouseButtonDownEvent(SlateApp, MouseEvent);
}

bool FDreamInputPreProcessor::HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	using namespace DreamInputPreProcessorPrivate;
	bool bSwallowedDown = false;
	if (DownSwallowedByPointer.RemoveAndCopyValue(PointerButtonKey(MouseEvent), bSwallowedDown))
	{
		return bSwallowedDown;                             // follows its down, per pointer index
	}

	// No recorded down. While dragging this is an OS drop (E7: OnDragDrop synthesizes the up,
	// SlateApplication.cpp:7585): swallow it and discard the external drag so it cannot drop later.
	const TSharedPtr<SWindow> Modal = SlateApp.GetActiveModalWindow();
	if (SlateApp.IsDragDropping())
	{
		if (!Modal.IsValid() && IsOverStopTarget(SlateApp, MouseEvent))
		{
			return false;
		}
		SlateApp.CancelDragDrop();
		return true;
	}

	// A down from before the pre-processor existed: decide by position, as a down would be.
	if (!Modal.IsValid())
	{
		return !IsOverStopTarget(SlateApp, MouseEvent);
	}
	return !IsInsideWindow(SlateApp, MouseEvent, Modal);
}

bool FDreamInputPreProcessor::HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent)
{
	const TSharedPtr<SWindow> Modal = SlateApp.GetActiveModalWindow();
	if (Modal.IsValid())
	{
		// Inside our window it scrolls the Run window's live run view (PO-03); inside a foreign modal, that modal.
		return !IsInsideWindow(SlateApp, InWheelEvent, Modal);
	}
	// No modal (docked, M4): pass over the output view only.
	return !IsOverStopTarget(SlateApp, InWheelEvent);
}
