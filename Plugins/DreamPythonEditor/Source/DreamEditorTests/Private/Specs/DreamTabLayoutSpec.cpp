// SPDX-License-Identifier: Apache-2.0
// Dream.Shell.TabLayout: the nomad tab DreamEditor.Main labelled Dream (WS-01, §11.6.1) and the centre column of
// T-GAP-019 (WS-02, WS-03, PO-01): the output area above the code area by default, below after layout.output_position
// changes (SDreamTab::SetOutputPosition, the Move Output commands and the setting itself), with min 120 px per area.
// The order is read from the centre SSplitter's slots; painted geometry is checked too when the window repaints.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "CoreGlobals.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Module/DreamEditorModule.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamTab.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SSplitter.h"

namespace DreamTabLayoutSpec
{
	/** The centre column as the splitter holds it. */
	struct FLayoutProbe
	{
		FString Error;
		EOrientation Orientation = Orient_Horizontal;
		int32 OutputSlot = INDEX_NONE;
		int32 CodeSlot = INDEX_NONE;
		float OutputSize = 0.f;
		float CodeSize = 0.f;
		float OutputMinSize = 0.f;
		float CodeMinSize = 0.f;
		FGeometry OutputGeometry;
		FGeometry CodeGeometry;

		bool IsValid() const { return Error.IsEmpty(); }
		bool IsOutputAbove() const { return OutputSlot < CodeSlot; }
	};

	bool IsDescendant(const TSharedPtr<SWidget>& Ancestor, TSharedPtr<SWidget> Widget)
	{
		for (; Widget.IsValid(); Widget = Widget->GetParentWidget())
		{
			if (Widget == Ancestor)
			{
				return true;
			}
		}
		return false;
	}

	/** Index of the splitter slot whose widget is, or contains, Widget. */
	int32 FindSlot(SSplitter& Splitter, TSharedPtr<SWidget> Widget)
	{
		for (; Widget.IsValid(); Widget = Widget->GetParentWidget())
		{
			const TSharedPtr<SWidget> Parent = Widget->GetParentWidget();
			if (Parent.Get() != &Splitter)
			{
				continue;
			}
			FChildren* Children = Splitter.GetChildren();
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				if (&Children->GetChildAt(Index).Get() == Widget.Get())
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}
		return INDEX_NONE;
	}

	FLayoutProbe ProbeLayout(SDreamTab& Tab)
	{
		FLayoutProbe Probe;
		const TSharedPtr<SWidget> Output = Tab.GetOutputArea();
		const TSharedPtr<SWidget> Code = Tab.GetActiveCodeView();
		if (!Output.IsValid() || !Code.IsValid())
		{
			Probe.Error = TEXT("the tab has no output area or no active code view");
			return Probe;
		}

		// The centre splitter is the nearest SSplitter ancestor of the output area that also holds the code view.
		TSharedPtr<SSplitter> Splitter;
		for (TSharedPtr<SWidget> Widget = Output->GetParentWidget(); Widget.IsValid(); Widget = Widget->GetParentWidget())
		{
			if (Widget->GetType() == FName(TEXT("SSplitter")) && IsDescendant(Widget, Code))
			{
				Splitter = StaticCastSharedPtr<SSplitter>(Widget);
				break;
			}
		}
		if (!Splitter.IsValid())
		{
			Probe.Error = TEXT("no SSplitter holds both the output area and the code area");
			return Probe;
		}

		Probe.Orientation = Splitter->GetOrientation();
		Probe.OutputSlot = FindSlot(*Splitter, Output);
		Probe.CodeSlot = FindSlot(*Splitter, Code);
		if (Probe.OutputSlot == INDEX_NONE || Probe.CodeSlot == INDEX_NONE || Probe.OutputSlot == Probe.CodeSlot)
		{
			Probe.Error = FString::Printf(TEXT("the output area (slot %d) and the code area (slot %d) are not in two slots of the centre splitter"),
				Probe.OutputSlot, Probe.CodeSlot);
			return Probe;
		}
		Probe.OutputSize = Splitter->SlotAt(Probe.OutputSlot).GetSizeValue();
		Probe.CodeSize = Splitter->SlotAt(Probe.CodeSlot).GetSizeValue();
		Probe.OutputMinSize = Splitter->SlotAt(Probe.OutputSlot).GetMinSize();
		Probe.CodeMinSize = Splitter->SlotAt(Probe.CodeSlot).GetMinSize();
		Probe.OutputGeometry = Output->GetTickSpaceGeometry();
		Probe.CodeGeometry = Code->GetTickSpaceGeometry();
		return Probe;
	}

	/** Changes layout.output_position the way Editor Preferences does: Pre/PostEditChangeProperty on the settings object. */
	void SetOutputPositionSetting(EDreamOutputPosition Position)
	{
		UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
		FProperty* Property = FindFProperty<FProperty>(UDreamUserSettings::StaticClass(), GET_MEMBER_NAME_CHECKED(UDreamUserSettings, OutputPosition));
		Settings->PreEditChange(Property);
		Settings->OutputPosition = Position;
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Settings->PostEditChangeProperty(Event);
	}
}

BEGIN_DEFINE_SPEC(FDreamTabLayoutSpec, "Dream.Shell.TabLayout", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	EDreamOutputPosition SavedPosition = EDreamOutputPosition::Top;
	/** The painted y of the output area before the last move; geometry is compared only when it changed (a repaint). */
	double LastOutputY = 0.0;

	/** Checks the slot order; false (with an error) when the layout could not be read. */
	bool ExpectOrder(const TCHAR* Context, bool bExpectAbove);
	/** Checks painted geometry when the window repainted since the move; notes it otherwise. */
	void ExpectPaintedOrder(const TCHAR* Context, bool bExpectAbove);
END_DEFINE_SPEC(FDreamTabLayoutSpec)

bool FDreamTabLayoutSpec::ExpectOrder(const TCHAR* Context, bool bExpectAbove)
{
	const DreamTabLayoutSpec::FLayoutProbe Probe = DreamTabLayoutSpec::ProbeLayout(*Tab);
	if (!Probe.IsValid())
	{
		AddError(FString::Printf(TEXT("%s: %s"), Context, *Probe.Error));
		return false;
	}
	TestTrue(*FString::Printf(TEXT("%s: the centre splitter is vertical"), Context), Probe.Orientation == Orient_Vertical);
	TestTrue(*FString::Printf(TEXT("%s: output area %s the code area (output slot %d, code slot %d)"), Context,
		bExpectAbove ? TEXT("above") : TEXT("below"), Probe.OutputSlot, Probe.CodeSlot), Probe.IsOutputAbove() == bExpectAbove);
	TestTrue(*FString::Printf(TEXT("%s: both areas keep at least 120 px (min %.0f and %.0f)"), Context, Probe.OutputMinSize, Probe.CodeMinSize),
		Probe.OutputMinSize >= 120.f && Probe.CodeMinSize >= 120.f);
	return true;
}

void FDreamTabLayoutSpec::ExpectPaintedOrder(const TCHAR* Context, bool bExpectAbove)
{
	const DreamTabLayoutSpec::FLayoutProbe Probe = DreamTabLayoutSpec::ProbeLayout(*Tab);
	if (!Probe.IsValid())
	{
		return;
	}
	const auto OutputSize = Probe.OutputGeometry.GetLocalSize();
	const auto CodeSize = Probe.CodeGeometry.GetLocalSize();
	const double OutputY = Probe.OutputGeometry.GetAbsolutePosition().Y;
	if (OutputSize.Y <= 0.0 || CodeSize.Y <= 0.0 || FMath::IsNearlyEqual(OutputY, LastOutputY))
	{
		AddInfo(FString::Printf(TEXT("%s: the window did not repaint; painted geometry not checked"), Context));
		return;
	}
	const double CodeY = Probe.CodeGeometry.GetAbsolutePosition().Y;
	TestTrue(*FString::Printf(TEXT("%s: painted output area y %.0f is %s code area y %.0f"), Context, OutputY,
		bExpectAbove ? TEXT("above") : TEXT("below"), CodeY), bExpectAbove ? OutputY < CodeY : OutputY > CodeY);
	LastOutputY = OutputY;
}

void FDreamTabLayoutSpec::Define()
{
	BeforeEach([this]()
	{
		SavedPosition = UDreamUserSettings::Get()->OutputPosition;
		Tab = DreamTest::RequireTab(*this);
		if (Tab.IsValid())
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("layout_probe = 1"), TEXT("Layout"));
			BufferId = Buffer.IsValid() ? Buffer->GetId() : FGuid();
		}
	});

	AfterEach([this]()
	{
		if (Tab.IsValid())
		{
			Tab->SetOutputPosition(SavedPosition);
		}
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
	});

	It("spawns the nomad tab DreamEditor.Main labelled Dream Python Editor (WS-01)", [this]()
	{
		if (!Tab.IsValid())
		{
			return;
		}
		const FTabId TabId{ FName(FDreamEditorModule::TabIdName) };
		TSharedPtr<SDockTab> DockTab = FGlobalTabmanager::Get()->FindExistingLiveTab(TabId);
		if (!DockTab.IsValid() && Tab->GetOwnerTab().IsValid())
		{
			// (chrome) A nomad tab the saved layout docked into the Level Editor's area lives in that tab manager's areas, which
			// FGlobalTabmanager::FindExistingLiveTab does not search (TabManager.cpp: its own DockAreas only).
			DockTab = Tab->GetOwnerTab();
			AddInfo(TEXT("the Dream tab is docked in another tab manager's area (the Level Editor's); checked through its body"));
		}
		if (!TestTrue(TEXT("a live DreamEditor.Main tab exists"), DockTab.IsValid() && DockTab->GetLayoutIdentifier() == TabId))
		{
			return;
		}
		TestEqual(TEXT("tab label"), DockTab->GetTabLabel().ToString(), FString(TEXT("Dream Python Editor")));
		TestTrue(TEXT("FDreamEditorModule::GetTab is the spawned body"), FDreamEditorModule::Get().GetTab() == Tab);
		TestTrue(TEXT("the tab body has an output area"), Tab->GetOutputArea().IsValid());
		TestTrue(TEXT("the scratch buffer has a code view"), Tab->FindCodeView(BufferId).IsValid());
	});

	It("defaults layout.output_position to Top (PO-01)", [this]()
	{
		// (chrome, tests review r6 M1) A settings object built by its C++ constructor alone (no archetype copy, no ini): what a
		// profile that never moved the output area reads, whatever this profile stores.
		UClass* Class = UDreamUserSettings::StaticClass();
		UObject* Raw = StaticAllocateObject(Class, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), Class, FName(TEXT("DreamT4FreshLayoutSettings"))), RF_Transient);
		(*Class->ClassConstructor)(FObjectInitializer(Raw, nullptr, EObjectInitializerOptions::None));
		TestTrue(TEXT("UDreamUserSettings::OutputPosition defaults to Top"),
			CastChecked<UDreamUserSettings>(Raw)->OutputPosition == EDreamOutputPosition::Top);
		FString Stored;
		if (GConfig->GetString(TEXT("/Script/DreamEditor.DreamUserSettings"), TEXT("OutputPosition"), Stored, GEditorSettingsIni))
		{
			AddInfo(FString::Printf(TEXT("This profile stores OutputPosition=%s"), *Stored));
		}
	});

	LatentIt("puts the output area above the code area at Top and below it at Bottom (WS-03, T-GAP-019)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		TSharedRef<float> OutputSizeAtTop = MakeShared<float>(0.f);

		Steps.Do(TEXT("set Top"), [this, OutputSizeAtTop]()
		{
			if (!Tab.IsValid() || !Tab->FindCodeView(BufferId).IsValid())
			{
				AddError(TEXT("no Dream tab with a code view"));
				return false;
			}
			Tab->SetOutputPosition(EDreamOutputPosition::Top);
			const DreamTabLayoutSpec::FLayoutProbe Probe = DreamTabLayoutSpec::ProbeLayout(*Tab);
			*OutputSizeAtTop = Probe.OutputSize;
			LastOutputY = -1.0;
			if (Probe.IsValid())
			{
				AddInfo(FString::Printf(TEXT("output area share %.2f of the centre column (proposed 0.40)"),
					Probe.OutputSize / FMath::Max(Probe.OutputSize + Probe.CodeSize, UE_SMALL_NUMBER)));
			}
			return ExpectOrder(TEXT("Top"), true);
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("painted at Top"), [this]() { ExpectPaintedOrder(TEXT("Top"), true); return true; });

		Steps.Do(TEXT("set Bottom"), [this, OutputSizeAtTop]()
		{
			Tab->SetOutputPosition(EDreamOutputPosition::Bottom);
			TestTrue(TEXT("SetOutputPosition saves layout.output_position = Bottom"),
				UDreamUserSettings::Get()->OutputPosition == EDreamOutputPosition::Bottom);
			const DreamTabLayoutSpec::FLayoutProbe Probe = DreamTabLayoutSpec::ProbeLayout(*Tab);
			TestEqual(TEXT("the output area keeps its size when it moves"), Probe.OutputSize, *OutputSizeAtTop, 0.001f);
			return ExpectOrder(TEXT("Bottom"), false);
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("painted at Bottom"), [this]() { ExpectPaintedOrder(TEXT("Bottom"), false); return true; });

		Steps.Do(TEXT("Move Output to Top command"), [this]()
		{
			FText Reason;
			if (!Tab->CanExecuteDreamCommand(EDreamCommand::MoveOutputToTop, Reason))
			{
				AddError(FString::Printf(TEXT("Move Output to Top cannot execute: %s"), *Reason.ToString()));
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::MoveOutputToTop);
			return ExpectOrder(TEXT("Move Output to Top"), true);
		});
		Steps.Do(TEXT("Move Output to Bottom command"), [this]()
		{
			FText Reason;
			if (!Tab->CanExecuteDreamCommand(EDreamCommand::MoveOutputToBottom, Reason))
			{
				AddError(FString::Printf(TEXT("Move Output to Bottom cannot execute: %s"), *Reason.ToString()));
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::MoveOutputToBottom);
			return ExpectOrder(TEXT("Move Output to Bottom"), false);
		});

		Steps.Finally(TEXT("restore"), [this]()
		{
			if (Tab.IsValid())
			{
				Tab->SetOutputPosition(SavedPosition);
			}
		});
		Steps.Run(Done);
	});

	LatentIt("follows a change of the setting itself, as Editor Preferences makes it (WS-03: applies at once)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("start at Top"), [this]()
		{
			if (!Tab.IsValid() || !Tab->FindCodeView(BufferId).IsValid())
			{
				AddError(TEXT("no Dream tab with a code view"));
				return false;
			}
			Tab->SetOutputPosition(EDreamOutputPosition::Top);
			return ExpectOrder(TEXT("before the setting change"), true);
		});
		Steps.Do(TEXT("setting = Bottom"), [this]()
		{
			DreamTabLayoutSpec::SetOutputPositionSetting(EDreamOutputPosition::Bottom);
			return true;
		});
		Steps.WaitFrames(1);
		Steps.Do(TEXT("layout follows Bottom"), [this]() { return ExpectOrder(TEXT("setting Bottom"), false); });
		Steps.Do(TEXT("setting = Top"), [this]()
		{
			DreamTabLayoutSpec::SetOutputPositionSetting(EDreamOutputPosition::Top);
			return true;
		});
		Steps.WaitFrames(1);
		Steps.Do(TEXT("layout follows Top"), [this]() { return ExpectOrder(TEXT("setting Top"), true); });
		Steps.Finally(TEXT("restore"), [this]()
		{
			DreamTabLayoutSpec::SetOutputPositionSetting(SavedPosition);
			if (Tab.IsValid())
			{
				Tab->SetOutputPosition(SavedPosition);
			}
			// (chrome) "start at Top" saved Top through SetOutputPosition; an edit like Editor Preferences' sets the value in
			// memory only, so the user's value is written back here: this key only, and only when the config cache holds
			// another value (review r7).
			FString Cached;
			const FString SavedText = StaticEnum<EDreamOutputPosition>()->GetNameStringByValue(static_cast<int64>(SavedPosition));
			if (GConfig->GetString(TEXT("/Script/DreamEditor.DreamUserSettings"), TEXT("OutputPosition"), Cached, GEditorSettingsIni)
				&& !Cached.Equals(SavedText))
			{
				UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, OutputPosition));
			}
		});
		Steps.Run(Done);
	});
}
