// SPDX-License-Identifier: Apache-2.0
// Builds Slate widgets from a JSON spec and keeps, per widget id, the state that SetProp and GetValue work on.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Widgets/SWidget.h"

class UAnimTKPanel;
class SAnimTKCanvas;
template <typename ItemType> class SListView;
template <typename ItemType> class STileView;
class STextComboBox;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class SWidgetSwitcher;

/** One row of a list or tile view. */
struct FAnimTKItem
{
	FString Id;
	FString Label;
	FString Icon;      // brush name or image file
	FString Tooltip;
	FLinearColor Color = FLinearColor::Transparent;
	bool bHasColor = false;
};
using FAnimTKItemPtr = TSharedPtr<FAnimTKItem>;

/** State behind one widget id. Props holds the latest value of every property; attributes read from it. */
struct FAnimTKNode : public TSharedFromThis<FAnimTKNode>
{
	FString Id;
	FString Type;
	TSharedPtr<FJsonObject> Props;
	TSharedPtr<SWidget> Widget;   // the outermost widget (size box, drop target or menu wrapper included)

	// Type specific
	TArray<FAnimTKItemPtr> Items;
	TSharedPtr<SListView<FAnimTKItemPtr>> List;
	TSharedPtr<STileView<FAnimTKItemPtr>> Tiles;
	TArray<TSharedPtr<FString>> Options;
	TSharedPtr<STextComboBox> Combo;
	TSharedPtr<SEditableTextBox> TextBox;
	TSharedPtr<SMultiLineEditableTextBox> MultiText;
	TSharedPtr<SWidgetSwitcher> Switcher;
	TSharedPtr<SAnimTKCanvas> Canvas;
	bool bSuppressEvents = false;

	FString GetString(const FString& Key, const FString& Default = FString()) const;
	double GetNumber(const FString& Key, double Default = 0.0) const;
	bool GetBool(const FString& Key, bool Default = false) const;
	bool Has(const FString& Key) const { return Props.IsValid() && Props->HasField(Key); }
};

class FAnimTKWidgetTree : public TSharedFromThis<FAnimTKWidgetTree>
{
public:
	explicit FAnimTKWidgetTree(UAnimTKPanel* InPanel);

	/** Builds the whole tree. Returns the root widget (an error text when the spec is unusable). */
	TSharedRef<SWidget> Build(const TSharedPtr<FJsonObject>& Spec);

	bool SetProp(const FString& WidgetId, const FString& Prop, const TSharedPtr<FJsonValue>& Value);
	TSharedPtr<FJsonValue> GetValue(const FString& WidgetId) const;

	void Emit(const FString& WidgetId, const FString& EventName, const TSharedPtr<FJsonObject>& Payload) const;

	/** Context menu from a "menu" spec array; picking an entry emits ("menu", {action, item, items}). */
	TSharedRef<SWidget> MakeMenu(const FString& WidgetId, const TArray<TSharedPtr<FJsonValue>>& Entries, const FString& ItemId, const TArray<FString>& ItemIds) const;

	// Shared helpers (also used by the canvas)
	static FLinearColor ParseColor(const TSharedPtr<FJsonValue>& Value, const FLinearColor& Default);
	static FMargin ParseMargin(const TSharedPtr<FJsonValue>& Value, const FMargin& Default);
	static TSharedPtr<FJsonObject> ModifierPayload();

private:
	TSharedRef<SWidget> BuildNode(const TSharedPtr<FJsonObject>& Spec);
	TSharedRef<SWidget> BuildInner(const TSharedRef<FAnimTKNode>& Node, const TSharedPtr<FJsonObject>& Spec);
	TSharedRef<SWidget> WrapCommon(const TSharedRef<FAnimTKNode>& Node, TSharedRef<SWidget> Inner);
	void ReadItems(const TSharedRef<FAnimTKNode>& Node, const TSharedPtr<FJsonValue>& Value);
	void ApplySelection(const TSharedRef<FAnimTKNode>& Node);
	TArray<TSharedPtr<FJsonObject>> Children(const TSharedPtr<FJsonObject>& Spec) const;

	TWeakObjectPtr<UAnimTKPanel> Panel;
	TMap<FString, TSharedRef<FAnimTKNode>> Nodes;
	int32 AutoId = 0;
};
