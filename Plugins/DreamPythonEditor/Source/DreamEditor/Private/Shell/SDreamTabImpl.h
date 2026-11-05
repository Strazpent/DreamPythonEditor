// SPDX-License-Identifier: Apache-2.0
// SDreamTab's private state, shared by SDreamTab.cpp and SDreamTabToolbar.cpp. Game thread. Slice-internal (shell).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Framework/Commands/UICommandList.h"
#include "Persistence/DreamJournal.h"
#include "Shell/SDreamTab.h"

class SBox;
class SComboButton;
class SDreamCodeView;
class SDreamEditorTabs;
class SDreamSideBar;   // (sidebar)
class SVerticalBox;

struct SDreamTab::FImpl
{
	// Editor tabs (code area)
	TArray<FGuid> TabOrder;
	FGuid ActiveId;
	TMap<FGuid, TSharedRef<SDreamCodeView>> Views;   // created on first activation (PERF-02: others load on activation)
	TSharedPtr<SBox> CodeHost;
	TSharedPtr<SDreamEditorTabs> EditorTabs;
	TSharedPtr<SVerticalBox> BannerHost;
	TSharedPtr<FUICommandList> MenuCommands;        // menus and tooltips only; keys go through FDreamKeyRouter (§4.9.1)
	/** The last focus home inside the tab that took keyboard focus (a code view's text, the REPL prompt, a view's list; never
	 *  a segment, button or link): a click on chrome gives focus back to it (OnFocusReceived, P3-shl-01, P4-shl-02). */
	TWeakPtr<SWidget> LastFocusedInside;

	// Centre splitter (WS-02, WS-03): each slot's size value travels with its widget when the slots swap
	float OutputValue = 0.4f;
	float CodeValue = 0.6f;
	EDreamOutputPosition Position = EDreamOutputPosition::Top;

	// (sidebar) PO-05: the side bar beside the centre column (outer splitter) and the centre's grip next to the side bar's rule.
	TSharedPtr<SDreamSideBar> SideBar;
	TSharedPtr<SSplitter> OuterSplitter;
	TSharedPtr<SWidget> CentreGrip;

	// Status strip
	FText StatusMessage;
	double StatusMessageUntil = 0.0;

	// (chrome) PO-04: the menu bar's titles (its hover rule), the status line (shown by View ▸ Status Line), and the banner
	// lines closed with × that View ▸ Show Hidden Notices brings back.
	TArray<TSharedPtr<SComboButton>> MenuTitles;
	TSharedPtr<SWidget> StatusLine;
	// A banner's × hides what it showed then, nothing later (PO-04-R1): the recovery lines shown (by id, review r7: the
	// count line keeps its id when its count changes, a new line gets a new id even with the same text), the safe-mode
	// reason, the Python state with its reason, the current local-history pause. A new line or state shows again.
	TSet<uint32> HiddenRecoveryLineIds;
	FString HiddenSafeModeKey;
	FString HiddenPythonKey;
	bool bHideHistoryBanner = false;
	FDelegateHandle ViewSettingsHandle;
	FTSTicker::FDelegateHandle FocusCheckTicker;   // after a View toggle: focus back on a shown widget

	// Runs
	TOptional<bool> TransactionOverride;            // U6 "Run in / without transaction", one shot
	double UndoConfirmUntil = 0.0;                  // §5.9.5 check 4: a partial record's inline confirm

	// Recovery (§10.7) and the session (§10.3)
	TArray<FGuid> PendingRecoveries;
	FText RecoveryMessage;                          // RecoveryLines, one per line
	TArray<FText> RecoveryLines;
	TArray<uint32> RecoveryLineIds;                 // (chrome) one id per line (the banner's × hides by id)
	uint32 NextRecoveryLineId = 1;
	int32 EarlierSessionLine = INDEX_NONE;          // the line that counts the pending buffers: it follows their count
	bool bShowRecoveryBanner = false;
	bool bConfirmDiscard = false;                   // PER-03: Discard asks inline first
	bool bSafeMode = false;
	TOptional<FDreamRecoveredSession> SafeModeSession;
	bool bRestoreRequested = false;
	bool bDismissedStorageNotice = false;
	// (persist) SDreamTabRecovery.cpp: what the scan said of each pending buffer (rows, +/- lines, conflict), the Review panel,
	// the open Compare, the discards waiting for their history entry, and the Save over confirm (§10.7.1-§10.7.3).
	TMap<FGuid, FDreamRecoveredBuffer> RecoveryDetails;
	bool bShowReview = false;
	FGuid CompareFor;
	TSet<FGuid> DiscardsInFlight;
	FGuid ConfirmSaveOver;
	TSharedPtr<SBox> PendingBarHost;
	TSharedPtr<SBox> ReviewHost;
	FDelegateHandle HistoryPausedHandle;   // (persist) persist-r2-M2: the "local history is paused" notice follows it
	// (instances) SDreamTabRecovery.cpp: the section of the open Compare ("" = this session's rows), the notices hidden with
	// their close button, and the binding on FDreamInstances::OnChanged (§10.5, §10.6, §10.1).
	FString CompareSection;
	TSet<FString> DismissedInstanceNotices;
	FDelegateHandle InstancesHandle;
	FTSTicker::FDelegateHandle SaveTicker;

	// (buffers) SDreamTabFiles.cpp: the active buffer's file bars (external change, deleted file, failed save: §10.14 step 5,
	// §10.15, BUF-05 M1, BUF-06) and the RUN-26 hint chip over the code area (§4.9.4).
	struct FFileBarAction
	{
		FText Label;
		FSimpleDelegate Action;
	};
	TSharedPtr<SBox> FileBarHost;
	TArray<FText> FileBarMessages;                  // what the shown bars say, top to bottom (test hook)
	TArray<FFileBarAction> FileBarActions;          // their buttons, in order (test hook)
	TSharedPtr<SWidget> CodeOverlay;                // the code host with the chip layer above it
	double LastEditSeconds = -1000.0;               // the last edit in a code view: the chip waits 1 s after typing
	uint64 HintCacheVersion = 0;                    // the statement-start answer for (buffer, version, caret line)
	FGuid HintCacheBuffer;
	int32 HintCacheLine = INDEX_NONE;
	bool bHintCacheStart = false;
	FDelegateHandle FileStateHandle;

	// Event bindings
	FDelegateHandle BufferOpenedHandle;
	FDelegateHandle BufferClosedHandle;
	FDelegateHandle RunFinishedHandle;
	FDelegateHandle SettingChangedHandle;
	FDelegateHandle PythonStateHandle;

	/** A new set of lines (a restore): each line gets a new id. */
	void SetRecoveryLines(TArray<FText> Lines, int32 InEarlierSessionLine)
	{
		RecoveryLines = MoveTemp(Lines);
		RecoveryLineIds.Reset();
		for (int32 Index = 0; Index < RecoveryLines.Num(); ++Index)
		{
			RecoveryLineIds.Add(NextRecoveryLineId++);
		}
		EarlierSessionLine = InEarlierSessionLine;
		RecoveryMessage = FText::Join(FText::FromString(TEXT("\n")), RecoveryLines);
	}

	/** (chrome) One more line (a Discard that could not be kept), with a new id even when its text repeats a line's. */
	void AddRecoveryLine(FText Line)
	{
		RecoveryLines.Add(MoveTemp(Line));
		RecoveryLineIds.Add(NextRecoveryLineId++);
		RecoveryMessage = FText::Join(FText::FromString(TEXT("\n")), RecoveryLines);
	}

	/** (chrome) The count line follows the pending buffers: Text replaces it and keeps its id (a hidden line stays hidden),
	 *  unset removes it. Nothing without a count line. */
	void SetEarlierSessionLine(const TOptional<FText>& Text)
	{
		if (!RecoveryLines.IsValidIndex(EarlierSessionLine))
		{
			return;
		}
		if (Text.IsSet())
		{
			RecoveryLines[EarlierSessionLine] = Text.GetValue();
		}
		else
		{
			RecoveryLines.RemoveAt(EarlierSessionLine);
			RecoveryLineIds.RemoveAt(EarlierSessionLine);
			EarlierSessionLine = INDEX_NONE;
		}
		RecoveryMessage = FText::Join(FText::FromString(TEXT("\n")), RecoveryLines);
	}
};
