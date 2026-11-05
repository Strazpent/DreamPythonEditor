// SPDX-License-Identifier: Apache-2.0
// UAnimTKPanel: a dockable editor panel whose Slate content is built from a JSON widget spec.
//
// Python never touches Slate directly. It sends a declarative spec (animtk.ui builds it), receives widget events through
// OnEvent, and pushes later changes with SetProp. One panel is one nomad tab ("AnimTK.<PanelId>"). RegisterTab lists the
// tab under Window > Tools > Anim Toolkit and gives it a Python factory command, so the tab survives editor restarts:
// when the layout restores the tab before any Python built it, the factory command runs and builds it.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AnimTKPanel.generated.h"

class FJsonObject;
class FAnimTKWidgetTree;
class SDockTab;
class SBox;
class SWindow;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FAnimTKPanelEvent, const FString&, WidgetId, const FString&, EventName, const FString&, PayloadJson);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAnimTKPanelClosed);

UCLASS(BlueprintType)
class ANIMTOOLKIT_API UAnimTKPanel : public UObject
{
	GENERATED_BODY()

public:
	/** Returns the panel with this id, creating it (rooted, so it lives until Destroy) when it does not exist. */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	static UAnimTKPanel* FindOrCreatePanel(const FString& PanelId, const FString& Title);

	/** Returns the panel with this id or None. */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	static UAnimTKPanel* FindPanel(const FString& PanelId);

	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	static TArray<FString> GetPanelIds();

	/**
	 * Lists the panel's tab in Window > Tools > Anim Toolkit. FactoryCommand is Python that builds and shows the panel
	 * (for example "import animtk.tools.picker as m; m.show()"); it runs when the tab is opened from the menu or restored
	 * with the layout before Python built it.
	 */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	void RegisterTab(const FString& FactoryCommand);

	/** Replaces the panel's content with the widget tree described by SpecJson. Returns false when the JSON is invalid. */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	bool SetSpec(const FString& SpecJson);

	/** Opens (or focuses) the panel as a dockable tab, or as a floating window when bFloatingWindow is true. */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	void Show(bool bFloatingWindow = false);

	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	void Close();

	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	bool IsOpen() const;

	/** Changes one property of one widget ("text", "value", "items", "selected", "enabled", "visible", ...). */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	bool SetProp(const FString& WidgetId, const FString& Prop, const FString& ValueJson);

	/** The widget's current value as JSON ("null" when the widget has none). */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	FString GetValue(const FString& WidgetId) const;

	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	void SetTitle(const FString& Title);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AnimToolkit|UI")
	FString GetPanelId() const { return PanelId; }

	/** Closes the panel, unregisters its tab and lets the object be garbage collected. */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit|UI")
	void Destroy();

	/** Fired for every widget event: (widget id, event name, JSON payload). */
	UPROPERTY(BlueprintAssignable, Category = "AnimToolkit|UI")
	FAnimTKPanelEvent OnEvent;

	/** Fired when the tab or window closes. */
	UPROPERTY(BlueprintAssignable, Category = "AnimToolkit|UI")
	FAnimTKPanelClosed OnClosed;

	// C++ side
	void Emit(const FString& WidgetId, const FString& EventName, const TSharedPtr<FJsonObject>& Payload);
	static void ShutdownAll();

private:
	FName GetTabName() const;
	void EnsureSpawner();
	TSharedRef<SDockTab> SpawnTab(const class FSpawnTabArgs& Args);
	void RefreshContent();
	void RunFactory();
	void HandleClosed();

	FString PanelId;
	FString Title;
	FString FactoryCommand;
	bool bSpawnerRegistered = false;
	bool bListed = false;
	bool bSpawning = false;
	bool bHasSpec = false;

	TSharedPtr<FAnimTKWidgetTree> Tree;
	TSharedPtr<SWidget> Root;
	TSharedPtr<SBox> HostBox;
	TWeakPtr<SDockTab> TabWeak;
	TWeakPtr<SWindow> WindowWeak;
};

/** Small editor helpers Python has no direct access to. */
UCLASS()
class ANIMTOOLKIT_API UAnimTKLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Shows an editor toast. Kind is "info", "success", "warning" or "error". */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit")
	static void Notify(const FString& Message, const FString& Kind = TEXT("info"), float Seconds = 3.0f);

	/** The modifier keys held right now, as a comma separated list ("shift,ctrl"); empty when none. */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit")
	static FString GetModifierKeys();

	UFUNCTION(BlueprintCallable, Category = "AnimToolkit")
	static void ClipboardCopy(const FString& Text);

	UFUNCTION(BlueprintCallable, Category = "AnimToolkit")
	static FString ClipboardPaste();

	/** Activates a level editor mode by id ("EditMode.ControlRig" is Animation mode). Returns true when it is active. */
	UFUNCTION(BlueprintCallable, Category = "AnimToolkit")
	static bool ActivateEditorMode(const FString& ModeId);

	UFUNCTION(BlueprintCallable, Category = "AnimToolkit")
	static bool IsEditorModeActive(const FString& ModeId);
};
