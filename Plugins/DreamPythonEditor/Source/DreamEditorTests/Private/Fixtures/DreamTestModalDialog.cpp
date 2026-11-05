// SPDX-License-Identifier: Apache-2.0
#include "Fixtures/DreamTestModalDialog.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformTime.h"
#include "Input/Reply.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamTest
{
	namespace
	{
		class SFixtureDialog : public SCompoundWidget
		{
		public:
			SLATE_BEGIN_ARGS(SFixtureDialog) {}
				SLATE_ARGUMENT(FText, Message)
			SLATE_END_ARGS()

			void Construct(const FArguments& InArgs)
			{
				auto Button = [this](const FText& Label, const TCHAR* Result)
				{
					return SNew(SButton)
						.Text(Label)
						.OnClicked_Lambda([this, Result]() { Close(Result); return FReply::Handled(); });
				};
				ChildSlot
				[
					SNew(SBorder)
					.Padding(16.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
						[
							SNew(STextBlock).Text(InArgs._Message)
						]
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f)[Button(LOCTEXT("Tests.Modal.Yes", "Yes"), TEXT("yes"))]
							+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f)[Button(LOCTEXT("Tests.Modal.No", "No"), TEXT("no"))]
							+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f)[Button(LOCTEXT("Tests.Modal.Cancel", "Cancel"), TEXT("cancel"))]
						]
					]
				];
			}

			virtual bool SupportsKeyboardFocus() const override { return true; }

			virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
			{
				if (InKeyEvent.GetKey() == EKeys::Escape)
				{
					Close(TEXT("cancel"));
					return FReply::Handled();
				}
				return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
			}

			void SetWindow(const TSharedRef<SWindow>& InWindow) { Window = InWindow; }
			/** The self-timeout: closes the dialog with "timeout" (ShowFixtureModal). */
			void TimeOut() { Close(TEXT("timeout")); }
			FString GetResult() const { return Result; }

		private:
			void Close(const TCHAR* InResult)
			{
				Result = InResult;
				if (const TSharedPtr<SWindow> Pinned = Window.Pin())
				{
					Pinned->RequestDestroyWindow();
				}
			}

			TWeakPtr<SWindow> Window;
			FString Result = TEXT("cancel");
		};
	}

	FString ShowFixtureModal(const FString& Title, const FString& Message)
	{
		check(IsInGameThread());
		if (!FSlateApplication::IsInitialized())
		{
			return TEXT("cancel");
		}
		FSlateApplication& App = FSlateApplication::Get();
		TSharedRef<SFixtureDialog> Content = SNew(SFixtureDialog).Message(FText::AsCultureInvariant(Message));
		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(FText::AsCultureInvariant(Title))
			.SizingRule(ESizingRule::Autosized)
			.AutoCenter(EAutoCenter::PreferredWorkArea)
			.SupportsMinimize(false)
			.SupportsMaximize(false)
			.FocusWhenFirstShown(true);
		Window->SetContent(Content);
		Content->SetWindow(Window);
		Window->SetWidgetToFocusOnActivate(Content);
		// Parented to the active modal (the Run window) when there is one, as a script's dialog is, else the main frame.
		TSharedPtr<SWidget> Parent = App.GetActiveModalWindow();
		if (!Parent.IsValid())
		{
			Parent = FGlobalTabmanager::Get()->GetRootWindow();
		}
		// Self-timeout (review tests-M3): nothing else ticks inside this modal loop (not the core ticker, the spec steps or the
		// automation timeouts), and Dream never closes a script's dialog (RT-05). A missed Esc (another window in front) would
		// otherwise hold the editor, and the UE lock, until a person answers; after TimeoutSeconds the dialog closes itself.
		constexpr double TimeoutSeconds = 20.0;
		const double OpenedAt = FPlatformTime::Seconds();
		TWeakPtr<SFixtureDialog> WeakContent = Content;
		const FDelegateHandle TickHandle = App.GetOnModalLoopTickEvent().AddLambda([WeakContent, OpenedAt](float)
		{
			if (FPlatformTime::Seconds() - OpenedAt > TimeoutSeconds)
			{
				if (const TSharedPtr<SFixtureDialog> Pinned = WeakContent.Pin())
				{
					Pinned->TimeOut();
				}
			}
		});
		App.AddModalWindow(Window, Parent, /*bSlowTaskWindow*/ false);
		App.GetOnModalLoopTickEvent().Remove(TickHandle);
		return Content->GetResult();
	}
}

#undef LOCTEXT_NAMESPACE
