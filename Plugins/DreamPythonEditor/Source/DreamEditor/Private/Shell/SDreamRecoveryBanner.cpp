// SPDX-License-Identifier: Apache-2.0
// Inline banners of the Dream tab (TDD §10.7.1): never modal, never taking focus (U26). (chrome) PO-04: one slim line in
// the theme's chrome colours: a thin mark, the message on one line (several lines join with " · "; the whole text is the
// tooltip), small text actions, and a dismiss × (or the inline confirm's Cancel).
#include "Shell/SDreamRecoveryBanner.h"

#include "Shell/DreamChrome.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamRecoveryBannerLocal
{
	constexpr int32 FontSize = 9;

	/** The message on one line: the banner's lines joined with a middle dot. */
	FText OneLine(const FText& Message)
	{
		FString Text = Message.ToString();
		if (!Text.Contains(TEXT("\n")))
		{
			return Message;
		}
		Text.ReplaceInline(TEXT("\r"), TEXT(""));
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty*/ true);
		return FText::FromString(FString::Join(Lines, TEXT("  ·  ")));
	}
}

void SDreamRecoveryBanner::Construct(const FArguments& InArgs)
{
	using namespace DreamRecoveryBannerLocal;
	const FSimpleDelegate OnRestoreAll = InArgs._OnRestoreAll;
	const FSimpleDelegate OnDiscard = InArgs._OnDiscard;
	const FSimpleDelegate OnDismiss = InArgs._OnDismiss;
	const FSimpleDelegate OnExtra = InArgs._OnExtraAction;
	const FSimpleDelegate OnReview = InArgs._OnReview;
	const FSimpleDelegate OnSecond = InArgs._OnSecondAction;
	const TAttribute<FText> Message = InArgs._Message;
	const EVisibility Actions = InArgs._bShowActions ? EVisibility::Visible : EVisibility::Collapsed;
	const FSlateFontInfo Font = FDreamChrome::Font(FontSize);

	// A small text action: accent text on the flat face, no focus.
	auto MakeAction = [&Font](const TAttribute<FText>& Text, const FSimpleDelegate& Action, const FText& Tooltip, EVisibility Visibility) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(&FDreamChrome::FlatButton())
			.ContentPadding(FMargin(5.f, 0.f))
			.ToolTipText(Tooltip)
			.Visibility(Visibility)
			.IsFocusable(false)
			.OnClicked_Lambda([Action]() { Action.ExecuteIfBound(); return FReply::Handled(); })
			[
				SNew(STextBlock)
				.Font(Font)
				.Text(Text)
				.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::Accent))
			];
	};
	const bool bTextDismiss = OnDismiss.IsBound() && !InArgs._DismissText.IsEmpty();

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FDreamChrome::Brush(EDreamChromeSlot::Background))
			.Padding(FMargin(0.f, 1.f, 4.f, 1.f))
			[
				SNew(SHorizontalBox)
				// The thin mark: warning colour for a warning, accent for a notice.
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.f, 0.f, 8.f, 0.f))
				[
					SNew(SBox)
					.WidthOverride(2.f)
					[
						SNew(SImage).Image(FDreamChrome::Brush(InArgs._bWarning ? EDreamChromeSlot::Warning : EDreamChromeSlot::Accent))
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(Font)
					.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.Text_Lambda([Message, Cache = MakeShared<TPair<FString, FText>>()]()
					{
						// Parsed again only when the message changes, not at every paint.
						const FText Now = Message.Get();
						if (!Now.ToString().Equals(Cache->Key, ESearchCase::CaseSensitive))
						{
							Cache->Key = Now.ToString();
							Cache->Value = OneLine(Now);
						}
						return Cache->Value;
					})
					.ToolTipText(Message)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakeAction(InArgs._ExtraActionText, OnExtra, FText::GetEmpty(), OnExtra.IsBound() ? EVisibility::Visible : EVisibility::Collapsed)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakeAction(InArgs._SecondActionText, OnSecond, FText::GetEmpty(), OnSecond.IsBound() ? EVisibility::Visible : EVisibility::Collapsed)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakeAction(LOCTEXT("RestoreAll", "Restore all"), OnRestoreAll,
						LOCTEXT("RestoreAllTip", "Each buffer takes its recovered text as one undoable edit; nothing is written to disk"), Actions)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakeAction(LOCTEXT("Review", "Review"), OnReview,
						LOCTEXT("ReviewTip", "Each recovered buffer with Restore, Restore as new tab, Compare and Discard; the editor stays usable"),
						Actions == EVisibility::Visible && OnReview.IsBound() ? EVisibility::Visible : EVisibility::Collapsed)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakeAction(LOCTEXT("Discard", "Discard"), OnDiscard,
						LOCTEXT("DiscardTip", "Keeps the disk text; the recovered texts move to local history (asks first)"), Actions)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakeAction(InArgs._DismissText, OnDismiss, FText::GetEmpty(), bTextDismiss ? EVisibility::Visible : EVisibility::Collapsed)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.f, 0.f, 0.f, 0.f))
				[
					SNew(SButton)
					.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamChrome.BannerDismiss")))
					.ButtonStyle(&FDreamChrome::FlatButton())
					.ContentPadding(FMargin(3.f, 1.f))
					.IsFocusable(false)
					.ToolTipText(LOCTEXT("DismissTip", "Hide this message"))
					.Visibility(OnDismiss.IsBound() && !bTextDismiss ? EVisibility::Visible : EVisibility::Collapsed)
					.OnClicked_Lambda([OnDismiss]() { OnDismiss.ExecuteIfBound(); return FReply::Handled(); })
					[
						SNew(SImage)
						.Image(FDreamChrome::Icon(EDreamChromeIcon::Close))
						.DesiredSizeOverride(FVector2D(10.0, 10.0))
						.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
					]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).HeightOverride(1.f)[ SNew(SImage).Image(FDreamChrome::Brush(EDreamChromeSlot::Border)) ]
		]
	];
}

#undef LOCTEXT_NAMESPACE
