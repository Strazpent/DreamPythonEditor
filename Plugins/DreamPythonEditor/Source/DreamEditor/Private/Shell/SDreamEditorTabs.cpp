// SPDX-License-Identifier: Apache-2.0
// The input tab row (BUF-02 M1; PO-04 point 1). The dirty "*" and the glyphs read the buffer on paint, so they follow
// without a rebuild; colours are the theme's chrome slots, read at paint.
#include "Shell/SDreamEditorTabs.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"   // (scratch) R-p1-4
#include "Module/DreamEditorModule.h"
#include "Commands/DreamKeyRouter.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/Paths.h"
#include "Shell/DreamChrome.h"
#include "Shell/DreamTheme.h"   // (theme)
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamEditorTabsLocal
{
	constexpr int32 FontSize = 9;
	constexpr float GlyphSize = 10.f;

	/** "New tab (Ctrl+T)" with the chord as bound now (§4.9.2: chords are rebindable, T-KEY-005), or the bare label. */
	FText WithChord(const FText& Label, EDreamCommand Command)
	{
		const FText Chord = FDreamKeyRouter::Get().GetChordText(Command);
		return Chord.IsEmpty() ? Label : FText::Format(LOCTEXT("TabChordTip", "{0} ({1})"), Label, Chord);
	}

	EVisibility Shown(bool bShown)
	{
		return bShown ? EVisibility::Visible : EVisibility::Collapsed;
	}
}

FDreamTabBadges SDreamEditorTabs::GetBadges(const FDreamBuffer& Buffer)
{
	FDreamTabBadges Badges;
	// (B-note) Every buffer is Python in 1.0 except a file with another extension, which shows that extension.
	const FString Extension = Buffer.HasPath() ? FPaths::GetExtension(Buffer.GetPath()).ToUpper() : FString();
	if (Extension.IsEmpty() || Extension == TEXT("PY") || Extension == TEXT("PYW") || Extension == TEXT("PYI"))
	{
		Badges.Language = LOCTEXT("Tab.LanguagePython", "PY");
	}
	else
	{
		Badges.Language = FText::AsCultureInvariant(Extension.Left(4));
	}
	// (scratch, R-p1-4) A scratch file whose auto-save is armed has its text on disk within 2 s: no "*". It shows only while
	// the auto-save is paused (a failure bar, a disk change, deleted on disk, read-only, a pending recovery).
	Badges.bDirty = Buffer.IsDirty()
		&& !(Buffer.IsScratchFile() && FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetBufferManager().IsScratchAutoSaveArmed(Buffer));
	Badges.bLock = Buffer.IsReadOnly() || Buffer.IsReadOnlyOnDisk();
	Badges.bRecovered = Buffer.HasPendingRecovery() || Buffer.GetKind() == EDreamBufferKind::Recovered;
	return Badges;
}

FText SDreamEditorTabs::GetTooltip(const FDreamBuffer& Buffer)
{
	FText Where = Buffer.HasPath() ? FText::FromString(Buffer.GetPath()) : LOCTEXT("ScratchTip", "Scratch tab (journaled, not saved to a file)");
	if (Buffer.HasPendingRecovery())
	{
		return FText::Format(LOCTEXT("PendingTip", "{0}\nNot restored yet: recovered changes are available."), Where);
	}
	if (Buffer.IsDeletedOnDisk())
	{
		Where = FText::Format(LOCTEXT("Tab.DeletedTip", "{0}\nDeleted on disk: Save recreates the file."), Where);
	}
	if (Buffer.IsReadOnly())
	{
		return FText::Format(LOCTEXT("ReadOnlyTip", "{0}\nRead-only: {1}"), Where, Buffer.GetReadOnlyReason());
	}
	if (Buffer.IsReadOnlyOnDisk())
	{
		return FText::Format(LOCTEXT("Tab.ReadOnlyOnDiskTip", "{0}\nThe file is read-only on disk: saving offers Make writable or Save As."), Where);
	}
	return Where;
}

void SDreamEditorTabs::Construct(const FArguments& InArgs)
{
	using namespace DreamEditorTabsLocal;
	OnActivate = InArgs._OnActivate;
	OnClose = InArgs._OnClose;
	OnNewTab = InArgs._OnNewTab;
	OnMessageClicked = InArgs._OnMessageClicked;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamChrome::Brush(EDreamChromeSlot::Background))
		.Padding(FMargin(2.f, 1.f, 0.f, 0.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SNew(SScrollBox)
				.ScrollBarStyle(&FDreamThemes::ScrollBarStyle())   // (theme)
				.Orientation(Orient_Horizontal)
				.ScrollBarThickness(FVector2D(3.0, 3.0))
				+ SScrollBox::Slot()
				[
					SAssignNew(Strip, SHorizontalBox)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
			[
				SNew(SBox)
				.MaxDesiredWidth(420.f)
				[
					// A flat button: a health warning's click does its action (PO-04-R1); a message's click does nothing.
					SNew(SButton)
					.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamChrome.TabRowMessageButton")))
					.ButtonStyle(&FDreamChrome::FlatButton())
					.ContentPadding(FMargin(3.f, 1.f))
					.IsFocusable(false)
					.ToolTipText(InArgs._MessageTooltip)
					.OnClicked_Lambda([this]() { OnMessageClicked.ExecuteIfBound(); return FReply::Handled(); })
					[
						SNew(STextBlock)
						.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamChrome.TabRowMessage")))
						.Font(FDreamChrome::Font(FontSize))
						.ColorAndOpacity(InArgs._MessageColor.IsSet() ? InArgs._MessageColor : FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.Text(InArgs._MessageText)
					]
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(10.f, 0.f, 8.f, 0.f))
			[
				SNew(STextBlock)
				.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamChrome.CaretPosition")))
				.Font(FDreamChrome::Font(FontSize))
				.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
				.Text(InArgs._CaretText)
			]
		]
	];
	Strip->AddSlot().AutoWidth()[ MakeNewTabButton() ];
}

TSharedRef<SWidget> SDreamEditorTabs::MakeNewTabButton()
{
	return SNew(SButton)
		.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamChrome.NewTab")))
		.ButtonStyle(&FDreamChrome::FlatButton())
		.ContentPadding(FMargin(5.f, 3.f))
		.VAlign(VAlign_Center)
		.IsFocusable(false)     // the new tab's code view takes focus (SDreamTab, NewTab; SHL-02)
		.ToolTipText_Lambda([]() { return DreamEditorTabsLocal::WithChord(LOCTEXT("NewTabTip", "New tab"), EDreamCommand::NewTab); })
		.OnClicked_Lambda([this]() { OnNewTab.ExecuteIfBound(); return FReply::Handled(); })
		[
			SNew(SImage)
			.Image(FDreamChrome::Icon(EDreamChromeIcon::Plus))
			.DesiredSizeOverride(FVector2D(DreamEditorTabsLocal::GlyphSize, DreamEditorTabsLocal::GlyphSize))
			.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
		];
}

void SDreamEditorTabs::SetTabs(const TArray<TSharedRef<FDreamBuffer>>& Buffers, const FGuid& ActiveId)
{
	Strip->ClearChildren();
	for (const TSharedRef<FDreamBuffer>& Buffer : Buffers)
	{
		Strip->AddSlot()
			.AutoWidth()
			.Padding(FMargin(0.f, 0.f, 1.f, 0.f))
			[
				MakeTab(Buffer, Buffer->GetId() == ActiveId)
			];
	}
	Strip->AddSlot().AutoWidth().VAlign(VAlign_Center)[ MakeNewTabButton() ];
}

TSharedRef<SWidget> SDreamEditorTabs::MakeTab(const TSharedRef<FDreamBuffer>& Buffer, bool bActive)
{
	using namespace DreamEditorTabsLocal;
	const TWeakPtr<FDreamBuffer> Weak = Buffer;
	const FGuid Id = Buffer->GetId();
	auto Badges = [Weak]() -> FDreamTabBadges
	{
		const TSharedPtr<FDreamBuffer> B = Weak.Pin();
		return B.IsValid() ? GetBadges(*B) : FDreamTabBadges();
	};
	const FSlateFontInfo Font = FDreamChrome::Font(FontSize);
	TSharedPtr<SBorder> Face;
	TSharedPtr<SButton> CloseButton;
	const TSharedRef<SWidget> Tab = SAssignNew(Face, SBorder)
		.Padding(FMargin(8.f, 3.f, 3.f, 3.f))
		.ToolTipText_Lambda([Weak]()
		{
			const TSharedPtr<FDreamBuffer> B = Weak.Pin();
			return B.IsValid() ? GetTooltip(*B) : FText::GetEmpty();
		})
		.OnMouseButtonDown_Lambda([this, Id](const FGeometry&, const FPointerEvent& Event)
		{
			if (Event.GetEffectingButton() == EKeys::MiddleMouseButton)
			{
				OnClose.ExecuteIfBound(Id);   // middle-click closes (BUF-02)
				return FReply::Handled();
			}
			if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				OnActivate.ExecuteIfBound(Id);
				return FReply::Handled();
			}
			return FReply::Unhandled();
		})
		[
			SNew(SHorizontalBox)
			// The name alone, so the label reads the buffer's display name (Dream.Keys.Focus finds it by that text).
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(Font)
				.Text_Lambda([Weak]()
				{
					const TSharedPtr<FDreamBuffer> B = Weak.Pin();
					return B.IsValid() ? FText::FromString(B->GetDisplayName()) : FText::GetEmpty();
				})
				.ColorAndOpacity(FDreamChrome::ColorAttr(bActive ? EDreamChromeSlot::Text : EDreamChromeSlot::MutedText))
			]
			// The dirty "*" right after the name (BUF-02, PO-04).
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamTab.Dirty")))
				.Font(Font)
				.Text(FText::AsCultureInvariant(TEXT("*")))
				.ColorAndOpacity(FDreamChrome::ColorAttr(bActive ? EDreamChromeSlot::Text : EDreamChromeSlot::MutedText))
				.Visibility_Lambda([Badges]() { return Shown(Badges().bDirty); })
			]
			// The lock glyph, only while the buffer or its file is read-only (BUF-02; §10.15 read-only change).
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
			[
				SNew(SImage)
				.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamTab.Lock")))
				.Image(FDreamChrome::Icon(EDreamChromeIcon::Lock))
				.DesiredSizeOverride(FVector2D(GlyphSize, GlyphSize))
				.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
				.Visibility_Lambda([Badges]() { return Shown(Badges().bLock); })
			]
			// The recovered glyph, only while a recovery is pending or for a "(recovered)" tab (PER-03).
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
			[
				SNew(SImage)
				.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamTab.Recovered")))
				.Image(FDreamChrome::Icon(EDreamChromeIcon::Recovered))
				.DesiredSizeOverride(FVector2D(GlyphSize, GlyphSize))
				.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::Accent))
				.Visibility_Lambda([Badges]() { return Shown(Badges().bRecovered); })
			]
			// The close ×: on the active tab, and on another while the pointer is over it (it keeps its room either way). Its
			// visibility binds after this statement, once Face is set (SAssignNew assigns after the whole expression).
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
			[
				SAssignNew(CloseButton, SButton)
				.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamTab.Close")))
				.ButtonStyle(&FDreamChrome::FlatButton())
				.ContentPadding(FMargin(2.f, 0.f))
				.IsFocusable(false)
				.ToolTipText_Lambda([]() { return DreamEditorTabsLocal::WithChord(LOCTEXT("CloseTabTip", "Close"), EDreamCommand::CloseTab); })
				.OnClicked_Lambda([this, Id]() { OnClose.ExecuteIfBound(Id); return FReply::Handled(); })
				[
					SNew(STextBlock)
					.Font(Font)
					.Text(FText::AsCultureInvariant(TEXT("×")))
					.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
				]
			]
		];
	CloseButton->SetVisibility(TAttribute<EVisibility>::CreateLambda([bActive, WeakFace = TWeakPtr<SBorder>(Face)]()
	{
		const TSharedPtr<SBorder> Pinned = WeakFace.Pin();
		return bActive || (Pinned.IsValid() && Pinned->IsHovered()) ? EVisibility::Visible : EVisibility::Hidden;
	}));
	// The active tab takes the editor's background, so it reads as one surface with the code below; another tab shows the
	// hover colour under the pointer.
	Face->SetBorderImage(TAttribute<const FSlateBrush*>::CreateLambda([bActive, WeakFace = TWeakPtr<SBorder>(Face)]() -> const FSlateBrush*
	{
		if (bActive)
		{
			return FDreamChrome::Brush(EDreamChromeSlot::EditorBackground);
		}
		const TSharedPtr<SBorder> Pinned = WeakFace.Pin();
		return Pinned.IsValid() && Pinned->IsHovered() ? FDreamChrome::Brush(EDreamChromeSlot::Hover) : FDreamChrome::Brush(EDreamChromeSlot::Background);
	}));
	return Tab;
}

#undef LOCTEXT_NAMESPACE
