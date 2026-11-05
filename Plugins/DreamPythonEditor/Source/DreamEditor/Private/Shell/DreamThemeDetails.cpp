// SPDX-License-Identifier: Apache-2.0
// The ThemeId dropdown in Editor Preferences ▸ Plugins ▸ Dream (see the header for why it is a detail customization).
#include "Shell/DreamThemeDetails.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IDetailCustomization.h"
#include "IDetailPropertyRow.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/DreamTheme.h"
#include "Widgets/Input/SComboButton.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamThemeDetailsLocal
{
	bool bRegistered = false;

	FName SettingsClassName()
	{
		return UDreamUserSettings::StaticClass()->GetFName();
	}

	/** The theme's display name for an id, or the id itself when no theme has it (a hand-edited ini). */
	FText DisplayName(const FString& Id)
	{
		const TSharedPtr<const FDreamTheme> Theme = FDreamThemes::FindLoaded(Id);   // per frame: no folder listing (review r7 m1)
		return Theme.IsValid() ? Theme->Name : FText::FromString(Id);
	}

	class FUserSettingsDetails final : public IDetailCustomization
	{
	public:
		static TSharedRef<IDetailCustomization> MakeInstance()
		{
			return MakeShared<FUserSettingsDetails>();
		}

		virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
		{
			const TSharedRef<IPropertyHandle> Handle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, ThemeId));
			if (!Handle->IsValidHandle())
			{
				return;
			}
			IDetailPropertyRow* Row = DetailBuilder.EditDefaultProperty(Handle);
			if (Row == nullptr)
			{
				return;
			}
			// The widgets hold the handle strongly: the layout builder keeps no reference to a handle it returned, so a weak one
			// expires when this function returns (the row then showed "Multiple values" and a choice did nothing).
			ThemeIdHandle = Handle;
			const TSharedPtr<IPropertyHandle> StrongHandle = Handle;
			Row->CustomWidget()
				.NameContent()
				[
					Handle->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(180.f)
				[
					SNew(SComboButton)
					.AddMetaData<FTagMetaData>(FTagMetaData(DreamThemeDetails::GetComboTag()))
					.ToolTipText(LOCTEXT("Theme.SettingTip", "The colours of the whole Dream tab. Applies at once; View > Theme sets it too."))
					.OnGetMenuContent_Lambda([StrongHandle]() { return DreamThemeDetails::MakeThemeMenu(StrongHandle); })
					.ButtonContent()
					[
						SNew(STextBlock)
						.Font(IDetailLayoutBuilder::GetDetailFont())
						.Text_Lambda([StrongHandle]()
						{
							FString Id;
							if (!StrongHandle.IsValid() || StrongHandle->GetValue(Id) != FPropertyAccess::Success)
							{
								return LOCTEXT("Theme.Multiple", "Multiple values");
							}
							return DisplayName(Id);
						})
					]
				];
		}

	private:
		TSharedPtr<IPropertyHandle> ThemeIdHandle;
	};
}

void DreamThemeDetails::Register()
{
	using namespace DreamThemeDetailsLocal;
	if (bRegistered)
	{
		return;
	}
	// Loaded by UnrealEd long before plugins start; a session without it (a commandlet) has no Editor Preferences.
	if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
	{
		PropertyEditor->RegisterCustomClassLayout(SettingsClassName(), FOnGetDetailCustomizationInstance::CreateStatic(&FUserSettingsDetails::MakeInstance));
		PropertyEditor->NotifyCustomizationModuleChanged();   // an open Editor Preferences page picks it up
		bRegistered = true;
	}
}

void DreamThemeDetails::Unregister()
{
	using namespace DreamThemeDetailsLocal;
	if (!bRegistered)
	{
		return;
	}
	bRegistered = false;
	if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
	{
		PropertyEditor->UnregisterCustomClassLayout(SettingsClassName());
		PropertyEditor->NotifyCustomizationModuleChanged();
	}
}

bool DreamThemeDetails::IsRegisteredForSettings()
{
	const FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	return PropertyEditor != nullptr && PropertyEditor->GetClassNameToDetailLayoutNameMap().Contains(DreamThemeDetailsLocal::SettingsClassName());
}

TArray<DreamThemeDetails::FThemeOption> DreamThemeDetails::GetThemeOptions()
{
	TArray<FThemeOption> Options;
	for (const TSharedRef<const FDreamTheme>& Theme : FDreamThemes::List())
	{
		Options.Add(FThemeOption{ Theme->Id, Theme->Name, Theme->bBuiltIn, Theme->SourcePath });
	}
	return Options;
}

TSharedRef<SWidget> DreamThemeDetails::MakeThemeMenu(const TSharedPtr<IPropertyHandle>& Handle)
{
	const TSharedPtr<IPropertyHandle> StrongHandle = Handle;
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	bool bUserSection = false;
	for (const FThemeOption& Option : GetThemeOptions())
	{
		if (!Option.bBuiltIn && !bUserSection)
		{
			Menu.AddSeparator();
			bUserSection = true;
		}
		const FString Id = Option.Id;
		Menu.AddMenuEntry(Option.Name,
			Option.bBuiltIn ? FText::GetEmpty() : FText::FromString(Option.SourcePath),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([StrongHandle, Id]()
				{
					if (StrongHandle.IsValid())
					{
						StrongHandle->SetValue(Id);   // PostEditChangeProperty -> OnViewSettingsChanged -> FDreamThemes applies it
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([StrongHandle, Id]()
				{
					FString Current;
					return StrongHandle.IsValid() && StrongHandle->GetValue(Current) == FPropertyAccess::Success && Current.Equals(Id, ESearchCase::IgnoreCase);
				})),
			NAME_None,
			EUserInterfaceActionType::RadioButton);
	}
	return Menu.MakeWidget();
}

FName DreamThemeDetails::GetComboTag()
{
	return FName(TEXT("Dream.ThemeIdCombo"));
}

#undef LOCTEXT_NAMESPACE
