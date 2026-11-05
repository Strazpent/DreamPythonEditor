// SPDX-License-Identifier: Apache-2.0
// SDreamTab's Recent Files action, file bars and the RUN-26 hint chip, beside SDreamTab.cpp as SDreamTabRecovery.cpp is:
// - File ▸ Recent Files (BUF-03 M1): opens a recent path, or drops one that no longer exists;
// - the active buffer's bars, under the code area's pending bar: "<name> was deleted on disk. [Save] [Close]" (§10.15),
//   "<name> changed on disk. [Keep mine] [Reload]" (the pre-M2 dirty bar; Deferred (M2): its Compare), a disk version that
//   plain mode cannot load (over 1 MB, binary, UTF-32: [Keep mine]), and the last failed save (§10.14 step 5):
//   "Not saved: <OS error>. [Retry] [Save As…]", the read-only target's [Make writable] [Save As…] (BUF-05 M1, §11.6.5;
//   Deferred (M4): Check out) and the encoding refusal's [Save as UTF-8] (BUF-06, §4.13). Bars never take focus (U26).
// - the RUN-26 hint chip (§4.9.4): until the first RunLineAdvance in the project, a focused buffer tab shows
//   "<chord> runs this line and moves to the next" at the end of a statement-start caret line, while the command has a
//   chord, no popup is open and no edit came in the last second; a click on it hides it for good.
// - (scratch, PO-05) scratch files: an unnamed new tab is scratch_N.py (the journaled Untitled-N fallback with one warning
//   when the scratch folder cannot be written), a scratch file saves when focus leaves its code view, restored journaled
//   Untitled-N buffers become scratch files, and Help > Restore Sample Scripts.
// Game thread; nothing here runs Python. Owner: buffers, then scratch.
#include "Shell/SDreamTab.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamScratch.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamKeyRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Shell/SDreamTabImpl.h"
#include "Shell/DreamChrome.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "dream/StatementExtent.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamTabFilesLocal
{
	FDreamEditorModule& Module()
	{
		return FDreamEditorModule::Get();
	}

	/** §4.9.4: the chip waits this long after the last edit ("not within 1 s of the last keystroke"). */
	constexpr double HintQuietSeconds = 1.0;
	/** Space between the caret line's last character and the chip. */
	constexpr float HintGap = 24.f;

	struct FBarButton
	{
		FText Label;
		FText Tooltip;
		FSimpleDelegate Action;
	};

	/** (scratch) "Untitled-<digits>": the journaled scratch buffers the migration converts (not a named tab such as a
	 *  "(recovered)" copy or a spec's DreamT4-* tab). */
	bool IsUntitledName(const FString& Name)
	{
		if (!Name.StartsWith(TEXT("Untitled-"), ESearchCase::CaseSensitive) || Name.Len() <= 9)
		{
			return false;
		}
		for (const TCHAR Ch : Name.RightChop(9))
		{
			if (!FChar::IsDigit(Ch))
			{
				return false;
			}
		}
		return true;
	}

	/** (scratch, PER-02) The first line that is at least half shown: the stock layout counts a line whose bottom edge sits
	 *  exactly at the top as visible, so the plain first visible line can be one above the line a scroll put at the top.
	 *  INDEX_NONE before the first layout. */
	int32 FirstMostlyVisibleLine(const SDreamCodeView& View)
	{
		TArray<FDreamLineGeometry> Lines;
		View.GetVisibleLines(Lines);
		for (const FDreamLineGeometry& Line : Lines)
		{
			if (Line.Top + Line.Height * 0.5f >= 0.f)
			{
				return Line.Line;
			}
		}
		return Lines.Num() > 0 ? Lines.Last().Line : INDEX_NONE;
	}

	/** One inline bar (chrome, PO-04: the slim banner look): a thin warning mark, the message, small text actions and an
	 *  optional ×, in the theme's chrome colours. Never focusable (U26). */
	TSharedRef<SWidget> MakeBar(const FText& Message, const TArray<FBarButton>& Buttons, const FSimpleDelegate& OnDismiss)
	{
		const FSlateFontInfo Font = FDreamChrome::Font(9);
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
			[
				SNew(SBox)
				.WidthOverride(2.f)
				[
					SNew(SImage).Image(FDreamChrome::Brush(EDreamChromeSlot::Warning))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 2.f))
			[
				SNew(STextBlock)
				.Font(Font)
				.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
				.Text(Message)
				.AutoWrapText(true)
			];
		for (const FBarButton& Button : Buttons)
		{
			const FSimpleDelegate Action = Button.Action;
			Row->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(&FDreamChrome::FlatButton())
					.ContentPadding(FMargin(5.f, 0.f))
					.ToolTipText(Button.Tooltip)
					.IsFocusable(false)
					.OnClicked_Lambda([Action]() { Action.ExecuteIfBound(); return FReply::Handled(); })
					[
						SNew(STextBlock)
						.Font(Font)
						.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::Accent))
						.Text(Button.Label)
					]
				];
		}
		if (OnDismiss.IsBound())
		{
			Row->AddSlot()
				.AutoWidth()
				.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(&FDreamChrome::FlatButton())
					.ContentPadding(FMargin(3.f, 1.f))
					.IsFocusable(false)
					.ToolTipText(LOCTEXT("Files.BarDismissTip", "Hide this message"))
					.OnClicked_Lambda([OnDismiss]() { OnDismiss.ExecuteIfBound(); return FReply::Handled(); })
					[
						SNew(SImage)
						.Image(FDreamChrome::Icon(EDreamChromeIcon::Close))
						.DesiredSizeOverride(FVector2D(10.0, 10.0))
						.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
					]
				];
		}
		return SNew(SBorder)
			.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamTab.FileBar")))
			.BorderImage(FDreamChrome::Brush(EDreamChromeSlot::Background))
			.Padding(FMargin(0.f, 1.f, 4.f, 1.f))
			[
				Row
			];
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Recent Files (BUF-03 M1)
// ---------------------------------------------------------------------------------------------------------------------

bool SDreamTab::OpenRecentFile(const FString& Path)
{
	if (!IFileManager::Get().FileExists(*Path))
	{
		DreamTabFilesLocal::Module().GetBufferManager().RemoveRecentFile(Path);
		ShowStatusMessage(FText::Format(LOCTEXT("Files.RecentGone", "{0} no longer exists; removed from Recent Files"), FText::FromString(Path)));
		return false;
	}
	if (!OpenLocation(Path, 0, 0))
	{
		return false;   // OpenLocation showed why (over 1 MB, binary, unreadable)
	}
	DreamTabFilesLocal::Module().GetBufferManager().AddRecentFile(Path);   // an already open file moves to the front too
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// File bars (§10.14 step 5, §10.15, BUF-05 M1, BUF-06)
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::RefreshFileBar()
{
	using namespace DreamTabFilesLocal;
	Impl->FileBarMessages.Reset();
	Impl->FileBarActions.Reset();
	if (!Impl->FileBarHost.IsValid())
	{
		return;
	}
	const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::IsAvailable() ? GetActiveBuffer() : nullptr;
	if (!Buffer.IsValid())
	{
		Impl->FileBarHost->SetContent(SNullWidget::NullWidget);
		return;
	}
	const FDreamBufferManager& Buffers = Module().GetBufferManager();
	const FGuid Id = Buffer->GetId();
	const FText Name = FText::FromString(Buffer->GetDisplayName());
	const TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	auto Act = [WeakTab](TFunction<void(SDreamTab&)> Body)
	{
		return FSimpleDelegate::CreateLambda([WeakTab, Body]()
		{
			if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin(); Tab.IsValid() && FDreamEditorModule::IsAvailable())
			{
				Body(*Tab);
			}
		});
	};
	const FBarButton SaveAsButton{ LOCTEXT("Files.BarSaveAs", "Save As…"), LOCTEXT("Files.BarSaveAsTip", "Saves this tab's text to another file"),
		Act([Id](SDreamTab& Tab) { Tab.ActivateBuffer(Id); Tab.SaveActive(/*bSaveAs*/ true); }) };

	TSharedRef<SVerticalBox> Bars = SNew(SVerticalBox);
	auto AddBar = [this, &Bars](const FText& Message, const TArray<FBarButton>& Buttons, const FSimpleDelegate& OnDismiss)
	{
		Impl->FileBarMessages.Add(Message);
		for (const FBarButton& Button : Buttons)
		{
			Impl->FileBarActions.Add({ Button.Label, Button.Action });
		}
		Bars->AddSlot()
			.AutoHeight()
			[
				MakeBar(Message, Buttons, OnDismiss)
			];
	};

	// §10.15 Deleted file: the buffer stays open, dirty and journaled until Save recreates the file or the tab closes.
	if (Buffer->IsDeletedOnDisk())
	{
		AddBar(FText::Format(LOCTEXT("Files.DeletedBar", "{0} was deleted on disk."), Name), {
			{ LOCTEXT("Files.DeletedSave", "Save"), LOCTEXT("Files.DeletedSaveTip", "Recreates the file with this tab's text, encoding and line endings"),
				Act([Id](SDreamTab& Tab) { Tab.SaveBufferTo(Id, FString(), /*bMakeWritable*/ false); }) },
			{ LOCTEXT("Files.DeletedClose", "Close"), LOCTEXT("Files.DeletedCloseTip", "Closes the tab; Reopen Closed Tab brings its text back"),
				Act([Id](SDreamTab& Tab) { Tab.CloseBuffer(Id); }) },
		}, FSimpleDelegate());
	}

	// §10.15 Dirty buffer (the pre-M2 bar), or a disk version plain mode cannot load.
	if (const FDreamDiskChange* Change = Buffers.GetDiskChange(Id))
	{
		const FBarButton KeepMine{ LOCTEXT("Files.KeepMine", "Keep mine"),
			LOCTEXT("Files.KeepMineTip", "Keeps this tab's text; the next Save overwrites the file, and this disk version raises no second bar"),
			Act([Id](SDreamTab& Tab) { Module().GetBufferManager().KeepMine(Id); }) };
		if (Change->bCanReload)
		{
			AddBar(FText::Format(LOCTEXT("Files.ChangedBar", "{0} changed on disk."), Name), {
				KeepMine,
				{ LOCTEXT("Files.Reload", "Reload"),
					LOCTEXT("Files.ReloadTip", "Loads the disk text as one undoable edit; this tab's text goes to local history, and Ctrl+Z brings it back"),
					Act([Id](SDreamTab& Tab)
					{
						if (Module().GetRunService().IsRunActive())
						{
							Tab.ShowStatusMessage(LOCTEXT("Files.ReloadInRun", "Reload waits until the run ends (RT-01)"));
						}
						else if (!Module().GetBufferManager().ReloadFromDisk(Id))
						{
							Tab.ShowStatusMessage(LOCTEXT("Files.ReloadFailed", "The disk text could not be loaded into this tab"));
						}
					}) },
			}, FSimpleDelegate());
		}
		else
		{
			AddBar(FText::Format(LOCTEXT("Files.ChangedUnloadable", "{0} changed on disk, but {1}."), Name, Change->Error), { KeepMine }, FSimpleDelegate());
		}
	}

	// §10.14 step 5: the last failed save, with the actions of its kind, until a save succeeds or the bar is dismissed.
	if (const FDreamSaveFailureInfo* Failure = Buffers.GetSaveFailure(Id))
	{
		const FString Target = Failure->Path;
		TArray<FBarButton> Buttons;
		switch (Failure->Kind)
		{
		case EDreamSaveFailure::ReadOnlyTarget:
			Buttons.Add({ LOCTEXT("Files.MakeWritable", "Make writable"),
				LOCTEXT("Files.MakeWritableTip", "Clears the file's read-only attribute, then saves"),
				Act([Id, Target](SDreamTab& Tab) { Tab.SaveBufferTo(Id, Target, /*bMakeWritable*/ true); }) });
			break;
		case EDreamSaveFailure::Encoding:
			Buttons.Add({ LOCTEXT("Files.SaveAsUtf8", "Save as UTF-8"),
				LOCTEXT("Files.SaveAsUtf8Tip", "Saves the file as UTF-8 (line endings kept; a coding line on line 1 or 2 becomes utf-8, as one undoable edit)"),
				Act([Id, Target](SDreamTab& Tab) { Tab.SaveAsUtf8(Id, Target); }) });
			break;
		default:
			Buttons.Add({ LOCTEXT("Files.Retry", "Retry"), LOCTEXT("Files.RetryTip", "Tries the save again"),
				Act([Id, Target](SDreamTab& Tab) { Tab.SaveBufferTo(Id, Target, /*bMakeWritable*/ false); }) });
			break;
		}
		Buttons.Add(SaveAsButton);
		AddBar(Failure->Error, Buttons, Act([Id](SDreamTab& Tab) { Module().GetBufferManager().ClearSaveFailure(Id); }));
	}

	Impl->FileBarHost->SetContent(Impl->FileBarMessages.Num() > 0 ? StaticCastSharedRef<SWidget>(Bars) : SNullWidget::NullWidget);
}

void SDreamTab::HandleFileStateChanged(const TSharedRef<FDreamBuffer>& Buffer)
{
	if (Buffer->GetId() == Impl->ActiveId)
	{
		RefreshFileBar();
	}
}

void SDreamTab::SaveBufferTo(const FGuid& BufferId, const FString& Target, bool bMakeWritable)
{
	FDreamBufferManager& Buffers = DreamTabFilesLocal::Module().GetBufferManager();
	const TSharedPtr<FDreamBuffer> Buffer = Buffers.FindById(BufferId);
	if (!Buffer.IsValid())
	{
		return;
	}
	const FString Path = Target.IsEmpty() ? Buffer->GetPath() : Target;
	if (Path.IsEmpty())
	{
		ActivateBuffer(BufferId);
		SaveActive(/*bSaveAs*/ true);   // a scratch tab has no file yet: Save As
		return;
	}
	const TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	const FString Name = FPaths::GetCleanFilename(Path);
	FDreamSaveOptions Options;
	Options.bMakeWritable = bMakeWritable;
	Buffers.SaveAsAsync(BufferId, Path, FDreamOnSaved::CreateLambda([WeakTab, Name](bool bOk, const FText& Error)
	{
		if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
		{
			Tab->ShowStatusMessage(bOk ? FText::Format(LOCTEXT("Files.Saved", "Saved {0}"), FText::FromString(Name)) : Error);
			Tab->RefreshTabStrip();
			Tab->SaveSessionState(false);
		}
	}), Options);
}

void SDreamTab::SaveAsUtf8(const FGuid& BufferId, const FString& Target)
{
	FText Error;
	if (!DreamTabFilesLocal::Module().GetBufferManager().ConvertToUtf8(BufferId, Error))
	{
		ShowStatusMessage(Error);
		return;
	}
	SaveBufferTo(BufferId, Target, /*bMakeWritable*/ false);
}

TArray<FText> SDreamTab::GetFileBarMessagesForTest() const
{
	return Impl->FileBarMessages;
}

TArray<FText> SDreamTab::GetFileBarActionsForTest() const
{
	TArray<FText> Labels;
	for (const FImpl::FFileBarAction& Action : Impl->FileBarActions)
	{
		Labels.Add(Action.Label);
	}
	return Labels;
}

bool SDreamTab::InvokeFileBarActionForTest(int32 Index)
{
	if (!Impl->FileBarActions.IsValidIndex(Index))
	{
		return false;
	}
	const FSimpleDelegate Action = Impl->FileBarActions[Index].Action;   // the action may rebuild the bars
	return Action.ExecuteIfBound();
}

// ---------------------------------------------------------------------------------------------------------------------
// The RUN-26 hint chip (§4.9.4, T-KEY-018)
// ---------------------------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SDreamTab::BuildCodeOverlay(const TSharedRef<SWidget>& CodeHostWidget)
{
	TSharedRef<SOverlay> Overlay = SNew(SOverlay)
		+ SOverlay::Slot()
		[
			CodeHostWidget
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(TAttribute<FMargin>::CreateSP(this, &SDreamTab::GetRunLineHintPadding))
		[
			SNew(SBorder)
			.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamTab.RunLineHint")))
			.Visibility_Lambda([this]() { return IsRunLineHintShown() ? EVisibility::Visible : EVisibility::Collapsed; })
			.BorderImage(FDreamChrome::Brush(EDreamChromeSlot::Hover))   // (chrome) the theme's colours
			.Padding(FMargin(6.f, 0.f))
			.Cursor(EMouseCursor::Hand)
			.ToolTipText(LOCTEXT("Files.RunLineHintTip", "Click to hide this hint for good"))
			.OnMouseButtonDown_Lambda([this](const FGeometry&, const FPointerEvent& Event)
			{
				if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
				{
					return FReply::Unhandled();
				}
				DismissRunLineHint();
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle(TEXT("SmallFont")))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]() { return GetRunLineHintTextForTest(); })
			]
		];
	Impl->CodeOverlay = Overlay;
	return Overlay;
}

FText SDreamTab::GetRunLineHintTextForTest() const
{
	return FText::Format(LOCTEXT("Files.RunLineHint", "{0} runs this line and moves to the next"),
		FDreamKeyRouter::Get().GetChordText(EDreamCommand::RunLineAdvance));
}

bool SDreamTab::IsRunLineHintShown() const
{
	if (!FDreamEditorModule::IsAvailable() || UDreamPerProjectUserSettings::Get()->bRunLineAdvanceHintDismissed)
	{
		return false;
	}
	// Only while RunLineAdvance has a chord (never in a preset that leaves it unbound, T-KEY-018).
	if (FDreamKeyRouter::Get().GetChordText(EDreamCommand::RunLineAdvance).IsEmpty())
	{
		return false;
	}
	const TSharedPtr<SDreamCodeView> View = GetActiveCodeView();
	const TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
	if (!View.IsValid() || !Buffer.IsValid() || !View->HasEditorFocus() || !View->GetSelection().IsEmpty())
	{
		return false;
	}
	// Not while typing, and not while a popup is open (completion, a menu).
	if (FPlatformTime::Seconds() - Impl->LastEditSeconds < DreamTabFilesLocal::HintQuietSeconds
		|| (FSlateApplication::IsInitialized() && FSlateApplication::Get().AnyMenusVisible()))
	{
		return false;
	}
	const FSlateRect Caret = View->GetCaretScreenRect();
	if (Caret.Right <= Caret.Left)
	{
		return false;   // the caret line is scrolled out of view
	}
	// Only on a statement start (§4.5): cached per buffer version and caret line, so a paint does not re-read the lexer.
	int32 Line = 0;
	int32 Column = 0;
	View->GetCaret(Line, Column);
	const uint64 Version = Buffer->GetModel().Version();
	if (Impl->HintCacheBuffer != Buffer->GetId() || Impl->HintCacheVersion != Version || Impl->HintCacheLine != Line)
	{
		const dream::TextSnapshot Snapshot = Buffer->Snapshot();
		const std::optional<dream::LineRange> Extent = dream::StatementExtent(Snapshot, Buffer->GetLexer(), Line);
		Impl->HintCacheBuffer = Buffer->GetId();
		Impl->HintCacheVersion = Version;
		Impl->HintCacheLine = Line;
		Impl->bHintCacheStart = Extent.has_value() && Extent->First == static_cast<dream::LineNo>(Line);
	}
	return Impl->bHintCacheStart;
}

FMargin SDreamTab::GetRunLineHintPadding() const
{
	const TSharedPtr<SDreamCodeView> View = GetActiveCodeView();
	const TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
	if (!View.IsValid() || !Buffer.IsValid() || !Impl->CodeOverlay.IsValid() || !IsRunLineHintShown())
	{
		return FMargin(0.f);
	}
	// "At the end of the caret line": the caret's desktop rectangle, moved right by the distance from the caret to the
	// line's last character in the text's own space (both widgets share the window's scale).
	const FSlateRect Caret = View->GetCaretScreenRect();
	int32 Line = 0;
	int32 Column = 0;
	View->GetCaret(Line, Column);
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	const dream::Offset LineStart = Snapshot.LineStart(Line);
	const dream::Offset LineEnd = Snapshot.LineEnd(Line);
	TArray<FSlateRect> Rects;
	View->GetRangeRects(LineStart, FMath::Max(LineEnd, LineStart + 1), Rects);
	if (Rects.Num() == 0)
	{
		return FMargin(0.f);
	}
	const float LineRight = static_cast<float>(Rects[0].Right);
	float CaretX = static_cast<float>(Rects[0].Left);
	const dream::Offset CaretOffset = FMath::Min<dream::Offset>(LineStart + Column, LineEnd);
	if (CaretOffset > LineStart)
	{
		Rects.Reset();
		View->GetRangeRects(LineStart, CaretOffset, Rects);
		if (Rects.Num() > 0)
		{
			CaretX = static_cast<float>(Rects[0].Right);
		}
	}
	const FGeometry& Geometry = Impl->CodeOverlay->GetCachedGeometry();
	const FVector2D CaretLocal = Geometry.AbsoluteToLocal(FVector2D(Caret.Left, Caret.Top));
	const float Width = static_cast<float>(Geometry.GetLocalSize().X);
	const float X = FMath::Clamp(static_cast<float>(CaretLocal.X) + (LineRight - CaretX) + DreamTabFilesLocal::HintGap, 0.f, FMath::Max(0.f, Width - 48.f));
	return FMargin(X, FMath::Max(0.f, static_cast<float>(CaretLocal.Y)), 0.f, 0.f);
}

void SDreamTab::DismissRunLineHint()
{
	UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
	if (!Settings->bRunLineAdvanceHintDismissed)
	{
		Settings->bRunLineAdvanceHintDismissed = true;
		Settings->SaveConfig();   // at once: the chip never shows again in this project, also after a restart (T-KEY-018)
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// (scratch, PO-05) Scratch files and the sample scripts
// ---------------------------------------------------------------------------------------------------------------------

TSharedRef<FDreamBuffer> SDreamTab::NewScratchFileTab(const FString& Text, const FGuid& Id)
{
	using namespace DreamTabFilesLocal;
	FDreamBufferManager& Buffers = Module().GetBufferManager();
	FText Error;
	TSharedPtr<FDreamBuffer> Buffer = Buffers.CreateScratchFile(Text, Error, Id);
	if (!Buffer.IsValid())
	{
		// Typing never fails: the tab is the journaled Untitled-N of before, and the folder is tried again for the next one.
		if (DreamScratch::WarnNotWritableOnce(Error))
		{
			ShowStatusMessage(FText::Format(LOCTEXT("Scratch.NotWritable", "Scratch folder is not writable: {0}"), FText::FromString(DreamScratch::GetFolder())));
		}
		Buffer = Buffers.CreateScratch(Text, FString(), Id);
	}
	// (R-p1-2) A refused Scratch folder setting is said once per value, here where its effect shows.
	const FText SettingWarning = DreamScratch::TakeSettingWarning();
	if (!SettingWarning.IsEmpty())
	{
		ShowStatusMessage(SettingWarning);
	}
	// The user's buffer-run default seeds the tab's toggle, which then decides for the tab's runs (§5.9.1, U3, §9.4).
	Buffer->GetViewState().bOneUndoStepPerRun = UDreamUserSettings::Get()->GetTransactionDefault(EDreamRunRoute::Buffer);
	ActivateBuffer(Buffer->GetId());
	return Buffer.ToSharedRef();
}

void SDreamTab::ResetScratchWarningForTest()
{
	DreamScratch::ResetWarningsForTest();
}

void SDreamTab::HandleScratchFocusChange(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath)
{
	if (!FDreamEditorModule::IsAvailable() || !Impl.IsValid() || !DreamTabFilesLocal::Module().GetBufferManager().AreFocusSavesEnabled())
	{
		return;
	}
	FDreamBufferManager& Buffers = DreamTabFilesLocal::Module().GetBufferManager();
	for (const TPair<FGuid, TSharedRef<SDreamCodeView>>& View : Impl->Views)
	{
		const SWidget* Widget = &View.Value.Get();
		if (!PreviousFocusPath.ContainsWidget(Widget) || (NewWidgetPath.IsValid() && NewWidgetPath.ContainsWidget(Widget)))
		{
			continue;
		}
		const TSharedPtr<FDreamBuffer> Buffer = Buffers.FindById(View.Key);
		if (Buffer.IsValid() && Buffer->IsScratchFile() && Buffer->IsDirty())
		{
			Buffers.SaveScratchNow(View.Key, FDreamBufferManager::EScratchSave::FocusLost);
		}
	}
}

void SDreamTab::MigrateUntitledToScratchFiles(const FDreamRecoveredSession& Session)
{
	using namespace DreamTabFilesLocal;
	const FDreamBufferManager& Buffers = Module().GetBufferManager();
	// (scratch-p1-m1) Only the buffers this restore brought back: another open Untitled-N (a fallback tab of this session, a
	// tab of an earlier restore) stays as it is.
	TArray<FGuid> Restored;
	for (const FDreamRecoveredBuffer& Rec : Session.Buffers)
	{
		Restored.Add(Rec.Id);
	}
	bool bAny = false;
	for (const FGuid& Id : Restored)
	{
		const TSharedPtr<FDreamBuffer> Buffer = Buffers.FindById(Id);
		bAny |= Buffer.IsValid() && Buffer->GetKind() == EDreamBufferKind::Scratch && !Buffer->HasPath() && !Buffer->HasPendingRecovery()
			&& IsUntitledName(Buffer->GetDisplayName());
	}
	if (!bAny)
	{
		return;
	}
	if (DreamScratch::IsCommandLineFolder())
	{
		// (B-note) A session folder (-DreamScratchFolder, the tier-4 runner's) is not where the developer's own untitled tabs
		// belong: they stay journaled until a session with their own scratch folder restores them.
		UE_LOG(LogDream, Log, TEXT("The restored untitled tabs stay journaled: this session's scratch folder %s comes from -DreamScratchFolder"),
			*DreamScratch::GetFolder());
		return;
	}
	FText Error;
	if (!DreamScratch::EnsureFolder(Error))
	{
		DreamScratch::WarnNotWritableOnce(Error);   // the restored untitled tabs stay journaled
		return;
	}
	for (const FGuid& Id : Restored)
	{
		MigrateUntitledToScratchFile(Id);
	}
}

void SDreamTab::MigrateUntitledToScratchFile(const FGuid& BufferId)
{
	FDreamBufferManager& Buffers = DreamTabFilesLocal::Module().GetBufferManager();
	const TSharedPtr<FDreamBuffer> Buffer = Buffers.FindById(BufferId);
	if (!Buffer.IsValid() || Buffer->GetKind() != EDreamBufferKind::Scratch || Buffer->HasPath() || Buffer->HasPendingRecovery()
		|| !DreamTabFilesLocal::IsUntitledName(Buffer->GetDisplayName()) || DreamScratch::IsCommandLineFolder())
	{
		return;
	}
	const TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	Buffers.ConvertToScratchFile(BufferId, FDreamOnSaved::CreateLambda([WeakTab](bool bOk, const FText&)
	{
		if (const TSharedPtr<SDreamTab> Tab = WeakTab.Pin(); Tab.IsValid() && bOk)
		{
			Tab->RefreshTabStrip();          // the label is the file name now
			Tab->SaveSessionState(false);    // and the tab a File tab under the scratch folder
		}
	}));
}

void SDreamTab::RefreshScrollStates()
{
	if (!FDreamEditorModule::IsAvailable() || !Impl.IsValid())
	{
		return;
	}
	const FDreamBufferManager& Buffers = DreamTabFilesLocal::Module().GetBufferManager();
	for (const TPair<FGuid, TSharedRef<SDreamCodeView>>& View : Impl->Views)
	{
		const TSharedPtr<FDreamBuffer> Buffer = Buffers.FindById(View.Key);
		// A view keeps its last layout while another tab is shown; one never laid out has none and keeps its restored state.
		const int32 First = DreamTabFilesLocal::FirstMostlyVisibleLine(View.Value.Get());
		if (Buffer.IsValid() && First != INDEX_NONE)
		{
			Buffer->GetViewState().FirstVisibleLine = First;
		}
	}
}

void SDreamTab::ScheduleRestoredScroll(const TSharedRef<FDreamBuffer>& Buffer, const TSharedRef<SDreamCodeView>& View, int32 RestoredFirstLine)
{
	// The view's construction placed the caret, which wrote its (not yet laid out) first line 0 into the state: the restored
	// value goes back, so a session save before the first layout keeps it.
	FDreamBufferViewState& State = Buffer->GetViewState();
	const int32 First = RestoredFirstLine;
	if (First <= 0)
	{
		return;
	}
	State.FirstVisibleLine = First;
	// (B-note) The stock widget only scrolls a location "into view": from the top a lower line lands at the bottom edge. So
	// the view goes to the end first, and then back up to the saved first line, which a scroll upwards puts at the top
	// edge. Each step waits for the layout of the one before; the caret the view was built with must still be where the
	// state put it (a traceback link or a click that moved it wins).
	struct FJob
	{
		int32 Phase = 0;
		int32 Frames = 0;
		double Started = 0.0;       // a view made during startup may wait long for its first layout (the window opens later)
		double LaidOut = 0.0;       // the first layout: the steps must finish within 2 s of it
	};
	const TSharedRef<FJob> Job = MakeShared<FJob>();
	Job->Started = FPlatformTime::Seconds();
	const int32 CaretLine = State.CaretLine;
	const int32 CaretColumn = State.CaretColumn;
	const TWeakPtr<SDreamCodeView> WeakView = View;
	FDreamDeferral::AddTicker(TEXT("DreamRestoreScroll"), EDreamDeferPolicy::DeferWhileExecuting, 0.f, /*bNeedsPython*/ false,
		[WeakView, Job, First, CaretLine, CaretColumn](float) -> bool
		{
			const TSharedPtr<SDreamCodeView> Live = WeakView.Pin();
			const double Now = FPlatformTime::Seconds();
			if (!Live.IsValid() || Now - Job->Started > 120.0 || (Job->LaidOut > 0.0 && Now - Job->LaidOut > 2.0))
			{
				return false;
			}
			int32 Line = 0;
			int32 Column = 0;
			Live->GetCaret(Line, Column);
			if (Line != CaretLine || Column != CaretColumn)
			{
				return false;
			}
			TArray<FDreamLineGeometry> Lines;
			Live->GetVisibleLines(Lines);
			if (Lines.Num() == 0)
			{
				return true;   // not laid out yet
			}
			if (Job->LaidOut == 0.0)
			{
				Job->LaidOut = Now;
			}
			if (Job->Phase == 0)
			{
				if (DreamTabFilesLocal::FirstMostlyVisibleLine(*Live) == First)
				{
					return false;
				}
				Live->ScrollToLine(Live->GetLineCount() - 1, /*Margin*/ 0);
				Job->Phase = 1;
				Job->Frames = 0;
				return true;
			}
			if (Job->Phase == 1)
			{
				if (++Job->Frames < 2)
				{
					return true;
				}
				Live->ScrollToLine(First, /*Margin*/ 0);
				Job->Phase = 2;
				Job->Frames = 0;
				return true;
			}
			return ++Job->Frames < 2;
		});
}

int32 SDreamTab::RestoreSampleScripts()
{
	FText Error;
	const int32 Copied = DreamScratch::CopyMissingSamples(Error);
	const FText Folder = FText::FromString(DreamScratch::GetSamplesFolder());
	if (Copied < 0)
	{
		UE_LOG(LogDream, Warning, TEXT("The sample scripts were not restored: %s"), *Error.ToString());
		ShowStatusMessage(FText::Format(LOCTEXT("Scratch.SamplesFailed", "The sample scripts were not restored: {0}"), Error));
	}
	else if (Copied == 0)
	{
		ShowStatusMessage(FText::Format(LOCTEXT("Scratch.SamplesPresent", "Every sample script is already in {0}"), Folder));
	}
	else
	{
		ShowStatusMessage(FText::Format(LOCTEXT("Scratch.SamplesRestored", "Restored {0} {0}|plural(one=sample script,other=sample scripts) to {1}"),
			FText::AsNumber(Copied), Folder));
	}
	return Copied;
}

#undef LOCTEXT_NAMESPACE
