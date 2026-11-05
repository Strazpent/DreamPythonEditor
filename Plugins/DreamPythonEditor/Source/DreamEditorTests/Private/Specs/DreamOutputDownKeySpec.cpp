// SPDX-License-Identifier: Apache-2.0
// Dream.Output.DownKey: P3-out-03's no-selection step (TDD §6.4 follow mode, OUT-09, A11Y-01, ARCHITECTURE A59; STATUS
// "Open after the review rounds"; slice B item B93). With no selected row and no selector row, as in a view nobody has
// clicked since its rows were made, Down and PageDown step from row 0 (SListView's navigation without a selector): with
// more rows than fit and the view following at the bottom, row 0 lies above the first shown row, so Down leaves follow
// mode and the lines that arrive afterwards wait below with the "n new lines" chip. Dream.Output.Follow covers Down from a
// selected line (above the view, and still shown).
#include "Support/DreamTestUtil.h"

#include "Framework/Application/SlateApplication.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamOutputStore.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamTab.h"

namespace DreamOutputDownKeySpec
{
	/** The first shown widget under Root that takes keyboard focus: the Output view's list. */
	TSharedPtr<SWidget> FindFocusable(const TSharedRef<SWidget>& Root)
	{
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			const TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			if (!Child->GetVisibility().IsVisible())
			{
				continue;
			}
			if (Child->SupportsKeyboardFocus())
			{
				return Child;
			}
			if (const TSharedPtr<SWidget> Found = FindFocusable(Child))
			{
				return Found;
			}
		}
		return nullptr;
	}

	void PressKey(const FKey& Key)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState NoMods;
		App.ProcessKeyDownEvent(FKeyEvent(Key, NoMods, App.GetUserIndexForKeyboard(), false, 0, 0));
		App.ProcessKeyUpEvent(FKeyEvent(Key, NoMods, App.GetUserIndexForKeyboard(), false, 0, 0));
	}

	void PushLines(const FString& Tag, int32 From, int32 Count)
	{
		FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		for (int32 Index = From; Index < From + Count; ++Index)
		{
			Store.PushDreamLine(0, EDreamStream::Stdout, FString::Printf(TEXT("%s line %03d"), *Tag, Index));
		}
	}
}

BEGIN_DEFINE_SPEC(FDreamOutputDownKeySpec, "Dream.Output.DownKey", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TSharedPtr<SDreamOutputView> RequireOutputView();
END_DEFINE_SPEC(FDreamOutputDownKeySpec)

TSharedPtr<SDreamOutputView> FDreamOutputDownKeySpec::RequireOutputView()
{
	const TSharedPtr<SDreamOutputView> View = Tab.IsValid() && Tab->GetOutputArea().IsValid() ? Tab->GetOutputArea()->GetOutputView() : nullptr;
	if (!View.IsValid())
	{
		AddError(TEXT("the Dream tab has no Output view"));
	}
	return View;
}

void FDreamOutputDownKeySpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
		{
			if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
			{
				View->ScrollToEnd();
			}
		}
		Tab.Reset();
	});

	LatentIt("leaves follow mode on Down with no selected row (P3-out-03)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		const FString Tag = FString::Printf(TEXT("nosel_%s"), *DreamTest::UniqueSuffix());
		TSharedRef<int32> Next = MakeShared<int32>(0);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("show the Output view, following, with more lines than fit"), [this, Tag, Next]()
		{
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ScrollToEnd();
			DreamOutputDownKeySpec::PushLines(Tag, *Next, 300);
			*Next += 300;
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("no selection and no selector row; focus the list"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ResetSelectionForTest();
			const TSharedPtr<SWidget> List = DreamOutputDownKeySpec::FindFocusable(View.ToSharedRef());
			if (!TestTrue(TEXT("the Output view has a focusable list"), List.IsValid()))
			{
				return false;
			}
			FSlateApplication::Get().SetKeyboardFocus(List, EFocusCause::SetDirectly);
			return TestTrue(TEXT("the list has keyboard focus"), List->HasKeyboardFocus());
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("Down with nothing selected"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid() || !TestTrue(TEXT("the view follows before Down"), View->IsFollowing())
				|| !TestTrue(TEXT("nothing is selected before Down"), View->GetSelectedText().IsEmpty()))
			{
				return false;
			}
			DreamOutputDownKeySpec::PressKey(EKeys::Down);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("more lines arrive after Down"), [Tag, Next]()
		{
			DreamOutputDownKeySpec::PushLines(Tag, *Next, 20);
			*Next += 20;
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the view held after Down"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			TestFalse(TEXT("Down from no selection reaches row 0, above the view, and leaves follow mode (A59, P3-out-03)"), View->IsFollowing());
			TestTrue(*FString::Printf(TEXT("the new lines wait below (%d)"), View->GetNewLinesBelow()), View->GetNewLinesBelow() > 0);
			return true;
		});
		Steps.Run(Done);
	});
}
