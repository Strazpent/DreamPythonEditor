// SPDX-License-Identifier: Apache-2.0
// Input during a supervised run (TDD §5.6, RT-04, RT-05, E7, PO-03). Registered on the first pump in the PreEngine
// bucket (FInputPreprocessorRegistrationKey{PreEngine, 0}), unregistered in finish step 3. Game thread.
// Body: DreamInputPreProcessor.cpp (supervisor slice). Signatures match IInputProcessor
// (Engine/Source/Runtime/Slate/Public/Framework/Application/IInputProcessor.h:23-53).
#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "InputCoreTypes.h"
#include "Run/DreamRunTypes.h"

class FSlateApplication;
class FDreamRunService;
class SWindow;
class SWidget;
struct FAnalogInputEvent;
struct FMotionEvent;

class DREAMEDITOR_API FDreamInputPreProcessor final : public IInputProcessor
{
public:
	/** §5.6.5. RunWindow/PausedWindow are our modals; StopTargets pass clicks; LiveRunView passes wheel, scrollbar and chips. */
	struct FContext
	{
		EDreamRunPresentation Presentation = EDreamRunPresentation::RunWindow;
		TWeakPtr<SWindow> RunWindow;
		TWeakPtr<SWindow> PausedWindow;             // M3
		TArray<TWeakPtr<SWidget>> StopTargets;
		TWeakPtr<SWidget> OutputView;
		TWeakPtr<SWidget> LiveRunView;
	};

	explicit FDreamInputPreProcessor(TWeakPtr<FDreamRunService> InService);

	void SetContext(const FContext& InContext);

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
	/** Shift+F5: Stop and swallow (every context). Esc: Stop under the RT-05 rule (§5.6.3), else pass. Other keys per §5.6.2. */
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	/** The key-up of a swallowed key-down is swallowed too. */
	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	/** Follows its down per pointer index; a mouse-up with no recorded down while IsDragDropping() is swallowed and the
	 *  OS drag is cancelled (E7, §5.6.4). */
	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	/** Passes inside our window (it scrolls the live run view), else swallows. */
	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override;
	virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent) override { return true; }
	virtual bool HandleMotionDetectedEvent(FSlateApplication& SlateApp, const FMotionEvent& MotionEvent) override { return true; }
	virtual const TCHAR* GetDebugName() const override { return TEXT("DreamRunInput"); }

private:
	/** §5.6.3: our modal or none is active, no menu is visible, and no editable text widget has keyboard focus. */
	bool IsEscStop(FSlateApplication& App) const;
	bool IsTextEntryFocused(FSlateApplication& App) const;
	bool IsInsideWindow(FSlateApplication& App, const FPointerEvent& Event, const TWeakPtr<SWindow>& Window) const;
	bool IsOverStopTarget(FSlateApplication& App, const FPointerEvent& Event) const;

	TWeakPtr<FDreamRunService> Service;
	FContext Context;
	TSet<FKey> SwallowedKeys;
	TMap<uint32, bool> DownSwallowedByPointer;
};
