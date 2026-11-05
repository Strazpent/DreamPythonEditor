// SPDX-License-Identifier: Apache-2.0
#include "AnimTKPanel.h"

#include "AnimTKWidgetTree.h"
#include "AnimToolkitModule.h"

#include "Editor.h"
#include "EditorModeManager.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IPythonScriptPlugin.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnimToolkit, Log, All);

namespace
{
	TMap<FString, TWeakObjectPtr<UAnimTKPanel>>& Registry()
	{
		static TMap<FString, TWeakObjectPtr<UAnimTKPanel>> Panels;
		return Panels;
	}

	/** Parses any JSON value (scalars included) by wrapping it in an object. */
	TSharedPtr<FJsonValue> ParseValue(const FString& Json)
	{
		TSharedPtr<FJsonObject> Wrapper;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FString::Printf(TEXT("{\"v\":%s}"), Json.IsEmpty() ? TEXT("null") : *Json));
		if (!FJsonSerializer::Deserialize(Reader, Wrapper) || !Wrapper.IsValid())
		{
			return nullptr;
		}
		return Wrapper->TryGetField(TEXT("v"));
	}

	FString WriteValue(const TSharedPtr<FJsonValue>& Value)
	{
		TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetField(TEXT("v"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Wrapper, Writer);
		// {"v":X}
		return Out.Mid(5, Out.Len() - 6);
	}
}

// ------------------------------------------------------------------------------------------------------------- registry

UAnimTKPanel* UAnimTKPanel::FindOrCreatePanel(const FString& PanelId, const FString& Title)
{
	if (PanelId.IsEmpty())
	{
		UE_LOG(LogAnimToolkit, Error, TEXT("FindOrCreatePanel: the panel id is empty"));
		return nullptr;
	}
	if (UAnimTKPanel* Existing = FindPanel(PanelId))
	{
		if (!Title.IsEmpty() && Title != Existing->Title)
		{
			Existing->SetTitle(Title);
		}
		return Existing;
	}
	UAnimTKPanel* Panel = NewObject<UAnimTKPanel>(GetTransientPackage(), NAME_None, RF_Transient);
	Panel->AddToRoot();
	Panel->PanelId = PanelId;
	Panel->Title = Title.IsEmpty() ? PanelId : Title;
	Registry().Add(PanelId, Panel);
	return Panel;
}

UAnimTKPanel* UAnimTKPanel::FindPanel(const FString& PanelId)
{
	if (const TWeakObjectPtr<UAnimTKPanel>* Found = Registry().Find(PanelId))
	{
		return Found->Get();
	}
	return nullptr;
}

TArray<FString> UAnimTKPanel::GetPanelIds()
{
	TArray<FString> Ids;
	for (const TPair<FString, TWeakObjectPtr<UAnimTKPanel>>& Pair : Registry())
	{
		if (Pair.Value.IsValid())
		{
			Ids.Add(Pair.Key);
		}
	}
	Ids.Sort();
	return Ids;
}

void UAnimTKPanel::ShutdownAll()
{
	const bool bSlate = FSlateApplication::IsInitialized();
	for (const TPair<FString, TWeakObjectPtr<UAnimTKPanel>>& Pair : Registry())
	{
		if (UAnimTKPanel* Panel = Pair.Value.Get())
		{
			if (bSlate && Panel->bSpawnerRegistered)
			{
				FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(Panel->GetTabName());
			}
			Panel->bSpawnerRegistered = false;
			Panel->Tree.Reset();
			Panel->Root.Reset();
			Panel->HostBox.Reset();
			Panel->RemoveFromRoot();
		}
	}
	Registry().Empty();
}

// ------------------------------------------------------------------------------------------------------------- tabs

FName UAnimTKPanel::GetTabName() const
{
	return FName(*(TEXT("AnimTK.") + PanelId));
}

void UAnimTKPanel::RegisterTab(const FString& InFactoryCommand)
{
	FactoryCommand = InFactoryCommand;
	if (bSpawnerRegistered && !bListed)
	{
		// Re-register so the entry moves from hidden to the menu.
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GetTabName());
		bSpawnerRegistered = false;
	}
	bListed = true;
	EnsureSpawner();
}

void UAnimTKPanel::EnsureSpawner()
{
	if (bSpawnerRegistered || !FSlateApplication::IsInitialized())
	{
		return;
	}
	FTabSpawnerEntry& Entry = FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GetTabName(), FOnSpawnTab::CreateUObject(this, &UAnimTKPanel::SpawnTab))
		.SetDisplayName(FText::FromString(Title))
		.SetTooltipText(FText::FromString(FString::Printf(TEXT("Open %s (Anim Toolkit)"), *Title)));
	if (bListed)
	{
		Entry.SetGroup(FAnimToolkitModule::GetMenuGroup());
	}
	else
	{
		Entry.SetMenuType(ETabSpawnerMenuType::Hidden);
	}
	bSpawnerRegistered = true;
}

TSharedRef<SDockTab> UAnimTKPanel::SpawnTab(const FSpawnTabArgs& Args)
{
	TGuardValue<bool> Guard(bSpawning, true);
	TWeakObjectPtr<UAnimTKPanel> Weak(this);
	TSharedRef<SDockTab> Tab = SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(FText::FromString(Title))
		.OnTabClosed_Lambda([Weak](TSharedRef<SDockTab>)
		{
			if (UAnimTKPanel* Panel = Weak.Get())
			{
				Panel->HandleClosed();
			}
		})
		[
			SAssignNew(HostBox, SBox)
		];
	TabWeak = Tab;
	if (!bHasSpec)
	{
		RunFactory();
	}
	RefreshContent();
	return Tab;
}

void UAnimTKPanel::RunFactory()
{
	if (FactoryCommand.IsEmpty())
	{
		return;
	}
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (Python == nullptr || !Python->IsPythonInitialized())
	{
		UE_LOG(LogAnimToolkit, Warning, TEXT("Panel '%s': Python is not ready, so the factory command did not run"), *PanelId);
		return;
	}
	Python->ExecPythonCommand(*FactoryCommand);
}

void UAnimTKPanel::RefreshContent()
{
	if (!HostBox.IsValid())
	{
		return;
	}
	if (Root.IsValid())
	{
		HostBox->SetContent(Root.ToSharedRef());
	}
	else
	{
		HostBox->SetContent(
			SNew(STextBlock)
			.Margin(FMargin(8.f))
			.Text(FText::FromString(FString::Printf(TEXT("%s has no content yet: Python builds it (factory: %s)."), *Title, FactoryCommand.IsEmpty() ? TEXT("none") : *FactoryCommand))));
	}
}

void UAnimTKPanel::HandleClosed()
{
	TabWeak.Reset();
	WindowWeak.Reset();
	HostBox.Reset();
	OnClosed.Broadcast();
}

// ------------------------------------------------------------------------------------------------------------- API

bool UAnimTKPanel::SetSpec(const FString& SpecJson)
{
	TSharedPtr<FJsonObject> Spec;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecJson);
	if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
	{
		UE_LOG(LogAnimToolkit, Error, TEXT("Panel '%s': the widget spec is not valid JSON: %s"), *PanelId, *Reader->GetErrorMessage());
		return false;
	}
	FString NewTitle;
	if (Spec->TryGetStringField(TEXT("title"), NewTitle) && !NewTitle.IsEmpty())
	{
		SetTitle(NewTitle);
	}
	Tree = MakeShared<FAnimTKWidgetTree>(this);
	Root = Tree->Build(Spec);
	bHasSpec = true;
	RefreshContent();
	return true;
}

void UAnimTKPanel::Show(bool bFloatingWindow)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	if (const TSharedPtr<SDockTab> Tab = TabWeak.Pin())
	{
		Tab->DrawAttention();
		return;
	}
	if (const TSharedPtr<SWindow> Window = WindowWeak.Pin())
	{
		Window->BringToFront();
		return;
	}
	if (bSpawning)
	{
		return;   // a factory command calling show() while its tab is being spawned
	}

	if (bFloatingWindow)
	{
		TWeakObjectPtr<UAnimTKPanel> Weak(this);
		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(FText::FromString(Title))
			.ClientSize(FVector2D(420, 640))
			.SupportsMaximize(true)
			.SupportsMinimize(true)
			[
				SAssignNew(HostBox, SBox)
			];
		Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([Weak](const TSharedRef<SWindow>&)
		{
			if (UAnimTKPanel* Panel = Weak.Get())
			{
				Panel->HandleClosed();
			}
		}));
		WindowWeak = Window;
		RefreshContent();
		FSlateApplication::Get().AddWindow(Window);
		return;
	}

	EnsureSpawner();
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(GetTabName()));
}

void UAnimTKPanel::Close()
{
	if (const TSharedPtr<SDockTab> Tab = TabWeak.Pin())
	{
		Tab->RequestCloseTab();
	}
	if (const TSharedPtr<SWindow> Window = WindowWeak.Pin())
	{
		Window->RequestDestroyWindow();
	}
}

bool UAnimTKPanel::IsOpen() const
{
	return TabWeak.IsValid() || WindowWeak.IsValid();
}

bool UAnimTKPanel::SetProp(const FString& WidgetId, const FString& Prop, const FString& ValueJson)
{
	if (!Tree.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Value = ParseValue(ValueJson);
	if (!Value.IsValid())
	{
		UE_LOG(LogAnimToolkit, Error, TEXT("Panel '%s': SetProp(%s, %s): the value is not valid JSON"), *PanelId, *WidgetId, *Prop);
		return false;
	}
	return Tree->SetProp(WidgetId, Prop, Value);
}

FString UAnimTKPanel::GetValue(const FString& WidgetId) const
{
	return Tree.IsValid() ? WriteValue(Tree->GetValue(WidgetId)) : TEXT("null");
}

void UAnimTKPanel::SetTitle(const FString& InTitle)
{
	Title = InTitle;
	if (const TSharedPtr<SDockTab> Tab = TabWeak.Pin())
	{
		Tab->SetLabel(FText::FromString(Title));
	}
	if (const TSharedPtr<SWindow> Window = WindowWeak.Pin())
	{
		Window->SetTitle(FText::FromString(Title));
	}
}

void UAnimTKPanel::Destroy()
{
	Close();
	if (bSpawnerRegistered && FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GetTabName());
	}
	bSpawnerRegistered = false;
	Registry().Remove(PanelId);
	OnEvent.Clear();
	OnClosed.Clear();
	Tree.Reset();
	Root.Reset();
	HostBox.Reset();
	RemoveFromRoot();
}

void UAnimTKPanel::Emit(const FString& WidgetId, const FString& EventName, const TSharedPtr<FJsonObject>& Payload)
{
	FString Json = TEXT("{}");
	if (Payload.IsValid())
	{
		Json.Reset();
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Payload.ToSharedRef(), Writer);
	}
	// Keep this object alive while Python handles the event (a handler may call Destroy).
	TStrongObjectPtr<UAnimTKPanel> KeepAlive(this);
	OnEvent.Broadcast(WidgetId, EventName, Json);
}

// ------------------------------------------------------------------------------------------------------------- library

void UAnimTKLibrary::Notify(const FString& Message, const FString& Kind, float Seconds)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FNotificationInfo Info(FText::FromString(Message));
	Info.ExpireDuration = FMath::Max(0.5f, Seconds);
	Info.bFireAndForget = true;
	if (Kind == TEXT("warning"))
	{
		Info.Image = FAppStyle::Get().GetBrush("Icons.WarningWithColor");
	}
	TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid())
	{
		if (Kind == TEXT("success"))
		{
			Item->SetCompletionState(SNotificationItem::CS_Success);
		}
		else if (Kind == TEXT("error"))
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
	}
}

FString UAnimTKLibrary::GetModifierKeys()
{
	if (!FSlateApplication::IsInitialized())
	{
		return FString();
	}
	const FModifierKeysState Mods = FSlateApplication::Get().GetModifierKeys();
	TArray<FString> Parts;
	if (Mods.IsShiftDown()) Parts.Add(TEXT("shift"));
	if (Mods.IsControlDown()) Parts.Add(TEXT("ctrl"));
	if (Mods.IsAltDown()) Parts.Add(TEXT("alt"));
	return FString::Join(Parts, TEXT(","));
}

void UAnimTKLibrary::ClipboardCopy(const FString& Text)
{
	FPlatformApplicationMisc::ClipboardCopy(*Text);
}

FString UAnimTKLibrary::ClipboardPaste()
{
	FString Text;
	FPlatformApplicationMisc::ClipboardPaste(Text);
	return Text;
}

bool UAnimTKLibrary::ActivateEditorMode(const FString& ModeId)
{
	if (!GIsEditor || IsRunningCommandlet())
	{
		return false;
	}
	FEditorModeTools& Tools = GLevelEditorModeTools();
	const FName Id(*ModeId);
	if (!Tools.IsModeActive(Id))
	{
		Tools.ActivateMode(Id);
	}
	return Tools.IsModeActive(Id);
}

bool UAnimTKLibrary::IsEditorModeActive(const FString& ModeId)
{
	return GIsEditor && !IsRunningCommandlet() && GLevelEditorModeTools().IsModeActive(FName(*ModeId));
}
