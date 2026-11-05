// SPDX-License-Identifier: Apache-2.0
// The widget vocabulary of animtk.ui. Every widget accepts the common keys:
//   id, tooltip, enabled, visible, width, height, min_width, accept_drop, menu
// and, as a child of a box, the slot keys: stretch, padding, halign, valign.
// docs/UI_REFERENCE.md lists every type with its own keys and events.
#include "AnimTKWidgetTree.h"

#include "AnimTKBrushes.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "AnimTKPanel.h"
#include "SAnimTKCanvas.h"

#include "DragAndDrop/AssetDragDropOp.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Input/DragAndDrop.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STileView.h"

namespace
{
	/** Accepts files from the OS and assets from the Content Browser; reports them as a "drop" event. */
	class SAnimTKDropTarget : public SCompoundWidget
	{
	public:
		DECLARE_DELEGATE_TwoParams(FOnDropped, const TArray<FString>& /*Files*/, const TArray<FString>& /*Assets*/);

		SLATE_BEGIN_ARGS(SAnimTKDropTarget) {}
			SLATE_DEFAULT_SLOT(FArguments, Content)
			SLATE_EVENT(FOnDropped, OnDropped)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			OnDropped = InArgs._OnDropped;
			ChildSlot
			[
				SNew(SBorder)
				.Padding(0)
				.BorderImage(FAppStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this]() { return FSlateColor(bHovering ? FLinearColor(0.2f, 0.5f, 1.f, 0.25f) : FLinearColor::Transparent); })
				[
					InArgs._Content.Widget
				]
			];
		}

		static bool Accepts(const FDragDropEvent& Event)
		{
			if (TSharedPtr<FExternalDragOperation> Ext = Event.GetOperationAs<FExternalDragOperation>())
			{
				return Ext->HasFiles();
			}
			return Event.GetOperationAs<FAssetDragDropOp>().IsValid();
		}

		virtual void OnDragEnter(const FGeometry&, const FDragDropEvent& Event) override { bHovering = Accepts(Event); }
		virtual void OnDragLeave(const FDragDropEvent&) override { bHovering = false; }
		virtual FReply OnDragOver(const FGeometry&, const FDragDropEvent& Event) override
		{
			return Accepts(Event) ? FReply::Handled() : FReply::Unhandled();
		}
		virtual FReply OnDrop(const FGeometry&, const FDragDropEvent& Event) override
		{
			bHovering = false;
			TArray<FString> Files;
			TArray<FString> Assets;
			if (TSharedPtr<FExternalDragOperation> Ext = Event.GetOperationAs<FExternalDragOperation>())
			{
				if (Ext->HasFiles())
				{
					Files = Ext->GetFiles();
				}
			}
			else if (TSharedPtr<FAssetDragDropOp> AssetOp = Event.GetOperationAs<FAssetDragDropOp>())
			{
				for (const FAssetData& Asset : AssetOp->GetAssets())
				{
					Assets.Add(Asset.GetObjectPathString());
				}
			}
			if (Files.Num() == 0 && Assets.Num() == 0)
			{
				return FReply::Unhandled();
			}
			OnDropped.ExecuteIfBound(Files, Assets);
			return FReply::Handled();
		}

	private:
		FOnDropped OnDropped;
		bool bHovering = false;
	};

	EHorizontalAlignment ParseHAlign(const FString& Value, EHorizontalAlignment Default)
	{
		if (Value == TEXT("left")) return HAlign_Left;
		if (Value == TEXT("center")) return HAlign_Center;
		if (Value == TEXT("right")) return HAlign_Right;
		if (Value == TEXT("fill")) return HAlign_Fill;
		return Default;
	}

	EVerticalAlignment ParseVAlign(const FString& Value, EVerticalAlignment Default)
	{
		if (Value == TEXT("top")) return VAlign_Top;
		if (Value == TEXT("center")) return VAlign_Center;
		if (Value == TEXT("bottom")) return VAlign_Bottom;
		if (Value == TEXT("fill")) return VAlign_Fill;
		return Default;
	}

	/** How much a child takes of a box by default: views fill, simple controls stay at their desired size. */
	float DefaultStretch(const FString& Type, bool bHorizontal)
	{
		static const TSet<FString> FillBoth = { TEXT("list"), TEXT("tiles"), TEXT("canvas"), TEXT("scroll"), TEXT("multiline"), TEXT("splitter"), TEXT("tabs") };
		static const TSet<FString> FillWide = { TEXT("slider"), TEXT("text"), TEXT("spin"), TEXT("combo"), TEXT("progress") };
		if (FillBoth.Contains(Type))
		{
			return 1.f;
		}
		return bHorizontal && FillWide.Contains(Type) ? 1.f : 0.f;
	}

	TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	}

	TArray<FString> ReadStrings(const TSharedPtr<FJsonValue>& Value)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Value.IsValid() && Value->TryGetArray(Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
				{
					Out.Add(S);
				}
			}
		}
		return Out;
	}

	/** Button style for coloured buttons: white rounded brushes, so ButtonColorAndOpacity shows the exact colour. */
	const FButtonStyle& ColoredButtonStyle()
	{
		static FButtonStyle Style = []()
		{
			FButtonStyle Result = FAppStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
			Result.SetNormal(FSlateRoundedBoxBrush(FLinearColor(0.85f, 0.85f, 0.85f), 4.f));
			Result.SetHovered(FSlateRoundedBoxBrush(FLinearColor::White, 4.f));
			Result.SetPressed(FSlateRoundedBoxBrush(FLinearColor(0.6f, 0.6f, 0.6f), 4.f));
			Result.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(0.3f, 0.3f, 0.3f), 4.f));
			return Result;
		}();
		return Style;
	}

	FSlateFontInfo FontFor(const FAnimTKNode& Node)
	{
		const FString Style = Node.GetString(TEXT("style"));
		int32 Size = (int32)Node.GetNumber(TEXT("size"), Style == TEXT("title") ? 12 : (Style == TEXT("small") ? 8 : 9));
		const bool bBold = Node.GetBool(TEXT("bold"), Style == TEXT("title"));
		if (Node.GetBool(TEXT("code")))
		{
			return FCoreStyle::GetDefaultFontStyle("Mono", Size);
		}
		return FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
	}
}

// ---------------------------------------------------------------------------------------------------------------- node

FString FAnimTKNode::GetString(const FString& Key, const FString& Default) const
{
	if (!Props.IsValid())
	{
		return Default;
	}
	const TSharedPtr<FJsonValue> Value = Props->TryGetField(Key);
	FString Out;
	if (Value.IsValid() && !Value->IsNull() && Value->TryGetString(Out))
	{
		return Out;
	}
	return Default;
}

double FAnimTKNode::GetNumber(const FString& Key, double Default) const
{
	if (!Props.IsValid())
	{
		return Default;
	}
	const TSharedPtr<FJsonValue> Value = Props->TryGetField(Key);
	double Out = Default;
	if (Value.IsValid() && Value->TryGetNumber(Out))
	{
		return Out;
	}
	bool bOut = false;
	if (Value.IsValid() && Value->TryGetBool(bOut))
	{
		return bOut ? 1.0 : 0.0;
	}
	return Default;
}

bool FAnimTKNode::GetBool(const FString& Key, bool Default) const
{
	if (!Props.IsValid())
	{
		return Default;
	}
	const TSharedPtr<FJsonValue> Value = Props->TryGetField(Key);
	bool Out = Default;
	if (Value.IsValid() && Value->TryGetBool(Out))
	{
		return Out;
	}
	double Number = 0.0;
	if (Value.IsValid() && Value->TryGetNumber(Number))
	{
		return Number != 0.0;
	}
	return Default;
}

// ---------------------------------------------------------------------------------------------------------------- tree

FAnimTKWidgetTree::FAnimTKWidgetTree(UAnimTKPanel* InPanel)
	: Panel(InPanel)
{
}

FLinearColor FAnimTKWidgetTree::ParseColor(const TSharedPtr<FJsonValue>& Value, const FLinearColor& Default)
{
	if (!Value.IsValid() || Value->IsNull())
	{
		return Default;
	}
	FString Hex;
	if (Value->TryGetString(Hex))
	{
		if (Hex.IsEmpty())
		{
			return Default;
		}
		return FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Value->TryGetArray(Arr) && Arr->Num() >= 3)
	{
		FLinearColor C(1, 1, 1, 1);
		(*Arr)[0]->TryGetNumber(C.R);
		(*Arr)[1]->TryGetNumber(C.G);
		(*Arr)[2]->TryGetNumber(C.B);
		if (Arr->Num() > 3)
		{
			(*Arr)[3]->TryGetNumber(C.A);
		}
		return C;
	}
	return Default;
}

FMargin FAnimTKWidgetTree::ParseMargin(const TSharedPtr<FJsonValue>& Value, const FMargin& Default)
{
	if (!Value.IsValid())
	{
		return Default;
	}
	double N = 0;
	if (Value->TryGetNumber(N))
	{
		return FMargin((float)N);
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Value->TryGetArray(Arr))
	{
		TArray<double> V;
		for (const TSharedPtr<FJsonValue>& E : *Arr)
		{
			double D = 0;
			E->TryGetNumber(D);
			V.Add(D);
		}
		if (V.Num() == 2) return FMargin((float)V[0], (float)V[1]);
		if (V.Num() == 4) return FMargin((float)V[0], (float)V[1], (float)V[2], (float)V[3]);
	}
	return Default;
}

TSharedPtr<FJsonObject> FAnimTKWidgetTree::ModifierPayload()
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	const FModifierKeysState Mods = FSlateApplication::Get().GetModifierKeys();
	Payload->SetBoolField(TEXT("shift"), Mods.IsShiftDown());
	Payload->SetBoolField(TEXT("ctrl"), Mods.IsControlDown());
	Payload->SetBoolField(TEXT("alt"), Mods.IsAltDown());
	return Payload;
}

void FAnimTKWidgetTree::Emit(const FString& WidgetId, const FString& EventName, const TSharedPtr<FJsonObject>& Payload) const
{
	if (UAnimTKPanel* Owner = Panel.Get())
	{
		Owner->Emit(WidgetId, EventName, Payload);
	}
}

TArray<TSharedPtr<FJsonObject>> FAnimTKWidgetTree::Children(const TSharedPtr<FJsonObject>& Spec) const
{
	TArray<TSharedPtr<FJsonObject>> Out;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Spec.IsValid() && Spec->TryGetArrayField(TEXT("children"), Arr))
	{
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (V.IsValid() && V->TryGetObject(Obj))
			{
				Out.Add(*Obj);
			}
		}
	}
	return Out;
}

TSharedRef<SWidget> FAnimTKWidgetTree::Build(const TSharedPtr<FJsonObject>& Spec)
{
	Nodes.Reset();
	AutoId = 0;
	if (!Spec.IsValid())
	{
		return SNew(STextBlock).Text(FText::FromString(TEXT("AnimToolkit: empty widget spec")));
	}
	const TSharedPtr<FJsonObject>* Root = nullptr;
	if (Spec->TryGetObjectField(TEXT("root"), Root))
	{
		return BuildNode(*Root);
	}
	return BuildNode(Spec);
}

TSharedRef<SWidget> FAnimTKWidgetTree::BuildNode(const TSharedPtr<FJsonObject>& Spec)
{
	TSharedRef<FAnimTKNode> Node = MakeShared<FAnimTKNode>();
	Spec->TryGetStringField(TEXT("type"), Node->Type);
	if (!Spec->TryGetStringField(TEXT("id"), Node->Id) || Node->Id.IsEmpty())
	{
		Node->Id = FString::Printf(TEXT("_w%d"), ++AutoId);
	}
	Node->Props = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Spec->Values)
	{
		if (Pair.Key != TEXT("children"))
		{
			Node->Props->SetField(Pair.Key, Pair.Value);
		}
	}
	Nodes.Add(Node->Id, Node);

	TSharedRef<SWidget> Inner = BuildInner(Node, Spec);
	TSharedRef<SWidget> Outer = WrapCommon(Node, Inner);
	Node->Widget = Outer;
	return Outer;
}

TSharedRef<SWidget> FAnimTKWidgetTree::WrapCommon(const TSharedRef<FAnimTKNode>& Node, TSharedRef<SWidget> Inner)
{
	TWeakPtr<FAnimTKNode> W = Node;
	TWeakPtr<FAnimTKWidgetTree> T = AsShared();
	TSharedRef<SWidget> Outer = Inner;

	if (Node->Has(TEXT("width")) || Node->Has(TEXT("height")) || Node->Has(TEXT("min_width")) || Node->Has(TEXT("min_height")))
	{
		TSharedRef<SBox> Box = SNew(SBox)[Outer];
		if (Node->Has(TEXT("width"))) Box->SetWidthOverride((float)Node->GetNumber(TEXT("width")));
		if (Node->Has(TEXT("height"))) Box->SetHeightOverride((float)Node->GetNumber(TEXT("height")));
		if (Node->Has(TEXT("min_width"))) Box->SetMinDesiredWidth((float)Node->GetNumber(TEXT("min_width")));
		if (Node->Has(TEXT("min_height"))) Box->SetMinDesiredHeight((float)Node->GetNumber(TEXT("min_height")));
		Outer = Box;
	}

	if (Node->GetBool(TEXT("accept_drop")))
	{
		const FString Id = Node->Id;
		Outer = SNew(SAnimTKDropTarget)
			.OnDropped_Lambda([T, Id](const TArray<FString>& Files, const TArray<FString>& Assets)
			{
				if (TSharedPtr<FAnimTKWidgetTree> Tree = T.Pin())
				{
					TSharedPtr<FJsonObject> Payload = ModifierPayload();
					Payload->SetArrayField(TEXT("files"), StringArray(Files));
					Payload->SetArrayField(TEXT("assets"), StringArray(Assets));
					Tree->Emit(Id, TEXT("drop"), Payload);
				}
			})
			[Outer];
	}

	// Views and the canvas open their own menus (they know the item under the mouse).
	const bool bOwnMenu = Node->Type == TEXT("list") || Node->Type == TEXT("tiles") || Node->Type == TEXT("canvas");
	if (!bOwnMenu && Node->Has(TEXT("menu")))
	{
		const FString Id = Node->Id;
		Outer = SNew(SBorder)
			.Padding(0)
			.BorderImage(FAppStyle::Get().GetBrush("NoBorder"))
			.OnMouseButtonUp_Lambda([T, W, Id](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				TSharedPtr<FAnimTKWidgetTree> Tree = T.Pin();
				TSharedPtr<FAnimTKNode> N = W.Pin();
				if (!Tree || !N || Event.GetEffectingButton() != EKeys::RightMouseButton || !N->Widget.IsValid())
				{
					return FReply::Unhandled();
				}
				const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
				if (!N->Props->TryGetArrayField(TEXT("menu"), Entries) || Entries->Num() == 0)
				{
					return FReply::Unhandled();
				}
				const FWidgetPath Path = Event.GetEventPath() != nullptr ? *Event.GetEventPath() : FWidgetPath();
				FSlateApplication::Get().PushMenu(N->Widget.ToSharedRef(), Path, Tree->MakeMenu(Id, *Entries, FString(), TArray<FString>()), Event.GetScreenSpacePosition(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
				return FReply::Handled();
			})
			[Outer];
	}

	Outer->SetEnabled(TAttribute<bool>::CreateLambda([W]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return !N || N->GetBool(TEXT("enabled"), true); }));
	Outer->SetVisibility(TAttribute<EVisibility>::CreateLambda([W]()
	{
		TSharedPtr<FAnimTKNode> N = W.Pin();
		return (!N || N->GetBool(TEXT("visible"), true)) ? EVisibility::Visible : EVisibility::Collapsed;
	}));
	if (Node->Has(TEXT("tooltip")))
	{
		Outer->SetToolTipText(TAttribute<FText>::CreateLambda([W]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return N ? FText::FromString(N->GetString(TEXT("tooltip"))) : FText::GetEmpty(); }));
	}
	return Outer;
}

void FAnimTKWidgetTree::ReadItems(const TSharedRef<FAnimTKNode>& Node, const TSharedPtr<FJsonValue>& Value)
{
	Node->Items.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Value.IsValid() || !Value->TryGetArray(Arr))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		FAnimTKItemPtr Item = MakeShared<FAnimTKItem>();
		FString S;
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (V->TryGetString(S))
		{
			Item->Id = S;
			Item->Label = S;
		}
		else if (V->TryGetObject(Obj))
		{
			(*Obj)->TryGetStringField(TEXT("id"), Item->Id);
			if (!(*Obj)->TryGetStringField(TEXT("label"), Item->Label))
			{
				Item->Label = Item->Id;
			}
			(*Obj)->TryGetStringField(TEXT("icon"), Item->Icon);
			(*Obj)->TryGetStringField(TEXT("tooltip"), Item->Tooltip);
			if ((*Obj)->HasField(TEXT("color")))
			{
				Item->Color = ParseColor((*Obj)->TryGetField(TEXT("color")), FLinearColor::White);
				Item->bHasColor = true;
			}
		}
		else
		{
			continue;
		}
		Node->Items.Add(Item);
	}
}

void FAnimTKWidgetTree::ApplySelection(const TSharedRef<FAnimTKNode>& Node)
{
	const TSet<FString> Wanted(ReadStrings(Node->Props->TryGetField(TEXT("selected"))));
	TSharedPtr<SListView<FAnimTKItemPtr>> View = Node->List.IsValid() ? Node->List : StaticCastSharedPtr<SListView<FAnimTKItemPtr>>(Node->Tiles);
	if (!View.IsValid())
	{
		return;
	}
	View->ClearSelection();
	FAnimTKItemPtr First;
	for (const FAnimTKItemPtr& Item : Node->Items)
	{
		if (Wanted.Contains(Item->Id))
		{
			View->SetItemSelection(Item, true, ESelectInfo::Direct);
			if (!First.IsValid())
			{
				First = Item;
			}
		}
	}
	if (First.IsValid())
	{
		View->RequestScrollIntoView(First);
	}
}

TSharedRef<SWidget> FAnimTKWidgetTree::MakeMenu(const FString& WidgetId, const TArray<TSharedPtr<FJsonValue>>& Entries, const FString& ItemId, const TArray<FString>& ItemIds) const
{
	TWeakPtr<const FAnimTKWidgetTree> T = AsShared();

	struct FLocal
	{
		static void Fill(FMenuBuilder& Builder, TWeakPtr<const FAnimTKWidgetTree> T, const FString& WidgetId, const TArray<TSharedPtr<FJsonValue>>& Entries, const FString& ItemId, const TArray<FString>& ItemIds)
		{
			bool bInSection = false;
			for (const TSharedPtr<FJsonValue>& V : Entries)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!V.IsValid() || !V->TryGetObject(Obj))
				{
					continue;
				}
				const TSharedPtr<FJsonObject> E = *Obj;
				FString Header;
				if (E->TryGetStringField(TEXT("header"), Header))
				{
					if (bInSection)
					{
						Builder.EndSection();
					}
					Builder.BeginSection(NAME_None, FText::FromString(Header));
					bInSection = true;
					continue;
				}
				bool bSeparator = false;
				if (E->TryGetBoolField(TEXT("separator"), bSeparator) && bSeparator)
				{
					Builder.AddSeparator();
					continue;
				}
				FString Label, Action, Tooltip, Icon;
				E->TryGetStringField(TEXT("label"), Label);
				if (!E->TryGetStringField(TEXT("action"), Action))
				{
					Action = Label;
				}
				E->TryGetStringField(TEXT("tooltip"), Tooltip);
				E->TryGetStringField(TEXT("icon"), Icon);
				const FSlateIcon SlateIcon = Icon.IsEmpty() ? FSlateIcon() : FSlateIcon(FAppStyle::GetAppStyleSetName(), FName(*Icon));

				const TArray<TSharedPtr<FJsonValue>>* Sub = nullptr;
				if (E->TryGetArrayField(TEXT("submenu"), Sub))
				{
					const TArray<TSharedPtr<FJsonValue>> SubCopy = *Sub;
					Builder.AddSubMenu(FText::FromString(Label), FText::FromString(Tooltip), FNewMenuDelegate::CreateLambda([T, WidgetId, SubCopy, ItemId, ItemIds](FMenuBuilder& SubBuilder)
					{
						Fill(SubBuilder, T, WidgetId, SubCopy, ItemId, ItemIds);
					}), false, SlateIcon);
					continue;
				}

				bool bChecked = false;
				const bool bCheckable = E->TryGetBoolField(TEXT("checked"), bChecked);
				FUIAction UIAction(
					FExecuteAction::CreateLambda([T, WidgetId, Action, ItemId, ItemIds]()
					{
						if (TSharedPtr<const FAnimTKWidgetTree> Tree = T.Pin())
						{
							TSharedPtr<FJsonObject> Payload = ModifierPayload();
							Payload->SetStringField(TEXT("action"), Action);
							Payload->SetStringField(TEXT("item"), ItemId);
							Payload->SetArrayField(TEXT("items"), StringArray(ItemIds));
							Tree->Emit(WidgetId, TEXT("menu"), Payload);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([bChecked]() { return bChecked; }));
				Builder.AddMenuEntry(FText::FromString(Label), FText::FromString(Tooltip), SlateIcon, UIAction, NAME_None,
					bCheckable ? EUserInterfaceActionType::ToggleButton : EUserInterfaceActionType::Button);
			}
			if (bInSection)
			{
				Builder.EndSection();
			}
		}
	};

	FMenuBuilder Builder(true, nullptr);
	FLocal::Fill(Builder, T, WidgetId, Entries, ItemId, ItemIds);
	return Builder.MakeWidget();
}

TSharedRef<SWidget> FAnimTKWidgetTree::BuildInner(const TSharedRef<FAnimTKNode>& Node, const TSharedPtr<FJsonObject>& Spec)
{
	const FString& Type = Node->Type;
	const FString Id = Node->Id;
	TWeakPtr<FAnimTKNode> W = Node;
	TWeakPtr<FAnimTKWidgetTree> T = AsShared();

	auto TextAttr = [W](const TCHAR* Key)
	{
		const FString K(Key);
		return TAttribute<FText>::CreateLambda([W, K]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return N ? FText::FromString(N->GetString(K)) : FText::GetEmpty(); });
	};
	auto EmitValue = [T, Id](const FString& EventName, const TSharedPtr<FJsonValue>& Value, bool bWithModifiers = false)
	{
		if (TSharedPtr<FAnimTKWidgetTree> Tree = T.Pin())
		{
			TSharedPtr<FJsonObject> Payload = bWithModifiers ? ModifierPayload() : MakeShared<FJsonObject>();
			if (Value.IsValid())
			{
				Payload->SetField(TEXT("value"), Value);
			}
			Tree->Emit(Id, EventName, Payload);
		}
	};
	auto SlotOf = [](const TSharedPtr<FJsonObject>& Child, bool bHorizontal, float& OutStretch, FMargin& OutPad, EHorizontalAlignment& OutH, EVerticalAlignment& OutV)
	{
		FString ChildType;
		Child->TryGetStringField(TEXT("type"), ChildType);
		OutStretch = DefaultStretch(ChildType, bHorizontal);
		double Stretch = 0;
		bool bStretch = false;
		if (Child->TryGetNumberField(TEXT("stretch"), Stretch))
		{
			OutStretch = (float)Stretch;
		}
		else if (Child->TryGetBoolField(TEXT("stretch"), bStretch))
		{
			OutStretch = bStretch ? 1.f : 0.f;
		}
		else if (Child->HasField(bHorizontal ? TEXT("width") : TEXT("height")))
		{
			OutStretch = 0.f;   // a fixed size along the box means "keep it"
		}
		OutPad = ParseMargin(Child->TryGetField(TEXT("padding")), FMargin(2.f));
		FString H, V;
		Child->TryGetStringField(TEXT("halign"), H);
		Child->TryGetStringField(TEXT("valign"), V);
		OutH = ParseHAlign(H, HAlign_Fill);
		OutV = ParseVAlign(V, bHorizontal ? VAlign_Center : VAlign_Fill);
	};
	auto IconWidget = [W](float DefaultSize) -> TSharedRef<SWidget>
	{
		TSharedPtr<FAnimTKNode> N = W.Pin();
		const float Size = N ? (float)N->GetNumber(TEXT("icon_size"), DefaultSize) : DefaultSize;
		return SNew(SImage)
			.DesiredSizeOverride(FVector2D(Size, Size))
			.Image_Lambda([W, Size]() -> const FSlateBrush*
			{
				TSharedPtr<FAnimTKNode> Pinned = W.Pin();
				return Pinned ? AnimTK::ResolveBrush(Pinned->GetString(TEXT("icon")), FVector2D(Size, Size)) : nullptr;
			});
	};

	// ------------------------------------------------------------------------------------------------ containers
	if (Type == TEXT("vbox") || Type == TEXT("hbox") || Type == TEXT("panel"))
	{
		const bool bHorizontal = Type == TEXT("hbox");
		TSharedRef<SVerticalBox> VBox = SNew(SVerticalBox);
		TSharedRef<SHorizontalBox> HBox = SNew(SHorizontalBox);
		for (const TSharedPtr<FJsonObject>& Child : Children(Spec))
		{
			float Stretch; FMargin Pad; EHorizontalAlignment H; EVerticalAlignment V;
			SlotOf(Child, bHorizontal, Stretch, Pad, H, V);
			TSharedRef<SWidget> ChildWidget = BuildNode(Child);
			if (bHorizontal)
			{
				SHorizontalBox::FScopedWidgetSlotArguments Slot = HBox->AddSlot();
				Slot.Padding(Pad).HAlign(H).VAlign(V);
				if (Stretch > 0.f) { Slot.FillWidth(Stretch); } else { Slot.AutoWidth(); }
				Slot[ChildWidget];
			}
			else
			{
				SVerticalBox::FScopedWidgetSlotArguments Slot = VBox->AddSlot();
				Slot.Padding(Pad).HAlign(H).VAlign(V);
				if (Stretch > 0.f) { Slot.FillHeight(Stretch); } else { Slot.AutoHeight(); }
				Slot[ChildWidget];
			}
		}
		TSharedRef<SWidget> Box = bHorizontal ? StaticCastSharedRef<SWidget>(HBox) : StaticCastSharedRef<SWidget>(VBox);
		if (Type == TEXT("panel") || Node->Has(TEXT("background")))
		{
			return SNew(SBorder)
				.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
				.BorderBackgroundColor(ParseColor(Node->Props->TryGetField(TEXT("background")), FLinearColor::White))
				.Padding(ParseMargin(Node->Props->TryGetField(TEXT("inner_padding")), FMargin(4.f)))
				[Box];
		}
		return Box;
	}
	if (Type == TEXT("scroll"))
	{
		const bool bHorizontal = Node->GetString(TEXT("orientation")) == TEXT("horizontal");
		TSharedRef<SScrollBox> Scroll = SNew(SScrollBox).Orientation(bHorizontal ? Orient_Horizontal : Orient_Vertical);
		for (const TSharedPtr<FJsonObject>& Child : Children(Spec))
		{
			float Stretch; FMargin Pad; EHorizontalAlignment H; EVerticalAlignment V;
			SlotOf(Child, bHorizontal, Stretch, Pad, H, V);
			Scroll->AddSlot().Padding(Pad).HAlign(H).VAlign(V)[BuildNode(Child)];
		}
		return Scroll;
	}
	if (Type == TEXT("wrap"))
	{
		const float Spacing = (float)Node->GetNumber(TEXT("spacing"), 2.0);
		TSharedRef<SWrapBox> Wrap = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(Spacing, Spacing));
		for (const TSharedPtr<FJsonObject>& Child : Children(Spec))
		{
			Wrap->AddSlot()[BuildNode(Child)];
		}
		return Wrap;
	}
	if (Type == TEXT("grid"))
	{
		const int32 Columns = FMath::Max(1, (int32)Node->GetNumber(TEXT("columns"), 2));
		const float Spacing = (float)Node->GetNumber(TEXT("spacing"), 2.0);
		TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(FMargin(Spacing));
		int32 Index = 0;
		for (const TSharedPtr<FJsonObject>& Child : Children(Spec))
		{
			Grid->AddSlot(Index % Columns, Index / Columns)[BuildNode(Child)];
			++Index;
		}
		return Grid;
	}
	if (Type == TEXT("splitter"))
	{
		const bool bVertical = Node->GetString(TEXT("orientation")) == TEXT("vertical");
		TSharedRef<SSplitter> Split = SNew(SSplitter).Orientation(bVertical ? Orient_Vertical : Orient_Horizontal);
		for (const TSharedPtr<FJsonObject>& Child : Children(Spec))
		{
			double Size = 1.0;
			Child->TryGetNumberField(TEXT("size"), Size);
			Split->AddSlot().Value((float)Size)[BuildNode(Child)];
		}
		return Split;
	}
	if (Type == TEXT("section"))
	{
		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
		for (const TSharedPtr<FJsonObject>& Child : Children(Spec))
		{
			float Stretch; FMargin Pad; EHorizontalAlignment H; EVerticalAlignment V;
			SlotOf(Child, false, Stretch, Pad, H, V);
			SVerticalBox::FScopedWidgetSlotArguments Slot = Body->AddSlot();
			Slot.Padding(Pad).HAlign(H).VAlign(V);
			if (Stretch > 0.f) { Slot.FillHeight(Stretch); } else { Slot.AutoHeight(); }
			Slot[BuildNode(Child)];
		}
		return SNew(SExpandableArea)
			.AreaTitle(TextAttr(TEXT("text")))
			.InitiallyCollapsed(Node->GetBool(TEXT("collapsed")))
			.Padding(FMargin(4.f, 2.f))
			.OnAreaExpansionChanged_Lambda([W, EmitValue](bool bExpanded)
			{
				if (TSharedPtr<FAnimTKNode> N = W.Pin())
				{
					N->Props->SetBoolField(TEXT("collapsed"), !bExpanded);
				}
				EmitValue(TEXT("changed"), MakeShared<FJsonValueBoolean>(bExpanded));
			})
			.BodyContent()[Body];
	}
	if (Type == TEXT("tabs"))
	{
		TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox);
		TSharedRef<SWidgetSwitcher> Switcher = SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([W]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return N ? (int32)N->GetNumber(TEXT("value"), 0) : 0; });
		Node->Switcher = Switcher;
		int32 Index = 0;
		for (const TSharedPtr<FJsonObject>& Child : Children(Spec))
		{
			FString Label;
			if (!Child->TryGetStringField(TEXT("label"), Label))
			{
				Label = FString::Printf(TEXT("Tab %d"), Index + 1);
			}
			Header->AddSlot().AutoWidth().Padding(1.f, 0.f)
			[
				SNew(SCheckBox)
				.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("ToggleButtonCheckbox"))
				.IsChecked_Lambda([W, Index]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return (N && (int32)N->GetNumber(TEXT("value"), 0) == Index) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([W, EmitValue, Index](ECheckBoxState)
				{
					if (TSharedPtr<FAnimTKNode> N = W.Pin())
					{
						N->Props->SetNumberField(TEXT("value"), Index);
					}
					EmitValue(TEXT("changed"), MakeShared<FJsonValueNumber>(Index));
				})
				[
					SNew(STextBlock).Text(FText::FromString(Label)).Margin(FMargin(6.f, 2.f))
				]
			];
			Switcher->AddSlot()[BuildNode(Child)];
			++Index;
		}
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)[Header]
			+ SVerticalBox::Slot().FillHeight(1.f)[Switcher];
	}

	// ------------------------------------------------------------------------------------------------ leaves
	if (Type == TEXT("label"))
	{
		TSharedRef<STextBlock> Text = SNew(STextBlock)
			.Text(TextAttr(TEXT("text")))
			.Font(FontFor(*Node))
			.AutoWrapText(Node->GetBool(TEXT("wrap")));
		if (Node->Has(TEXT("color")))
		{
			Text->SetColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([W]()
			{
				TSharedPtr<FAnimTKNode> N = W.Pin();
				return FSlateColor(N ? ParseColor(N->Props->TryGetField(TEXT("color")), FLinearColor::White) : FLinearColor::White);
			}));
		}
		return Text;
	}
	if (Type == TEXT("button") || Type == TEXT("toggle"))
	{
		TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);
		const bool bHasText = !Node->GetString(TEXT("text")).IsEmpty();
		const bool bVerticalLayout = Node->GetBool(TEXT("icon_above"));
		if (Node->Has(TEXT("icon")) && !bVerticalLayout)
		{
			Content->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, bHasText ? 4.f : 0.f, 0.f)[IconWidget(16.f)];
		}
		if (bHasText || !Node->Has(TEXT("icon")))
		{
			TSharedRef<STextBlock> Label = SNew(STextBlock).Text(TextAttr(TEXT("text"))).Font(FontFor(*Node)).Justification(ETextJustify::Center);
			if (Node->Has(TEXT("text_color")))
			{
				Label->SetColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([W]()
				{
					TSharedPtr<FAnimTKNode> N = W.Pin();
					return FSlateColor(N ? ParseColor(N->Props->TryGetField(TEXT("text_color")), FLinearColor::White) : FLinearColor::White);
				}));
			}
			Content->AddSlot().FillWidth(1.f).VAlign(VAlign_Center).HAlign(HAlign_Center)[Label];
		}
		TSharedRef<SWidget> Body = Content;
		if (bVerticalLayout)
		{
			Body = SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)[IconWidget(32.f)]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)[Content];
		}

		if (Type == TEXT("toggle"))
		{
			return SNew(SCheckBox)
				.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("ToggleButtonCheckbox"))
				.IsChecked_Lambda([W]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return (N && N->GetBool(TEXT("value"))) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([W, EmitValue](ECheckBoxState State)
				{
					const bool bOn = State == ECheckBoxState::Checked;
					if (TSharedPtr<FAnimTKNode> N = W.Pin())
					{
						N->Props->SetBoolField(TEXT("value"), bOn);
					}
					EmitValue(TEXT("changed"), MakeShared<FJsonValueBoolean>(bOn), true);
				})
				[Body];
		}

		TSharedRef<SButton> Button = SNew(SButton)
			.ContentPadding(ParseMargin(Node->Props->TryGetField(TEXT("content_padding")), FMargin(6.f, 2.f)))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.ButtonColorAndOpacity_Lambda([W]()
			{
				TSharedPtr<FAnimTKNode> N = W.Pin();
				return FSlateColor(N ? ParseColor(N->Props->TryGetField(TEXT("color")), FLinearColor::White) : FLinearColor::White);
			})
			.OnClicked_Lambda([EmitValue]()
			{
				EmitValue(TEXT("clicked"), nullptr, true);
				return FReply::Handled();
			})
			[Body];
		if (Node->GetBool(TEXT("flat")))
		{
			Button->SetButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"));
		}
		else if (Node->Has(TEXT("color")))
		{
			Button->SetButtonStyle(&ColoredButtonStyle());
		}
		return Button;
	}
	if (Type == TEXT("checkbox"))
	{
		return SNew(SCheckBox)
			.IsChecked_Lambda([W]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return (N && N->GetBool(TEXT("value"))) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([W, EmitValue](ECheckBoxState State)
			{
				const bool bOn = State == ECheckBoxState::Checked;
				if (TSharedPtr<FAnimTKNode> N = W.Pin())
				{
					N->Props->SetBoolField(TEXT("value"), bOn);
				}
				EmitValue(TEXT("changed"), MakeShared<FJsonValueBoolean>(bOn));
			})
			[
				SNew(STextBlock).Text(TextAttr(TEXT("text")))
			];
	}
	if (Type == TEXT("text"))
	{
		return SAssignNew(Node->TextBox, SEditableTextBox)
			.Text(FText::FromString(Node->GetString(TEXT("value"))))
			.HintText(TextAttr(TEXT("hint")))
			.IsReadOnly(Node->GetBool(TEXT("read_only")))
			.OnTextChanged_Lambda([W, EmitValue](const FText& Text)
			{
				TSharedPtr<FAnimTKNode> N = W.Pin();
				if (!N || N->bSuppressEvents)
				{
					return;
				}
				N->Props->SetStringField(TEXT("value"), Text.ToString());
				EmitValue(TEXT("changed"), MakeShared<FJsonValueString>(Text.ToString()));
			})
			.OnTextCommitted_Lambda([W, EmitValue](const FText& Text, ETextCommit::Type Commit)
			{
				if (TSharedPtr<FAnimTKNode> N = W.Pin())
				{
					N->Props->SetStringField(TEXT("value"), Text.ToString());
				}
				if (Commit == ETextCommit::OnEnter || Commit == ETextCommit::OnUserMovedFocus)
				{
					EmitValue(TEXT("committed"), MakeShared<FJsonValueString>(Text.ToString()));
				}
			});
	}
	if (Type == TEXT("multiline"))
	{
		return SAssignNew(Node->MultiText, SMultiLineEditableTextBox)
			.Text(FText::FromString(Node->GetString(TEXT("value"))))
			.HintText(TextAttr(TEXT("hint")))
			.Font(FontFor(*Node))
			.AutoWrapText(Node->GetBool(TEXT("wrap")))
			.IsReadOnly(Node->GetBool(TEXT("read_only")))
			.OnTextChanged_Lambda([W, EmitValue](const FText& Text)
			{
				TSharedPtr<FAnimTKNode> N = W.Pin();
				if (!N || N->bSuppressEvents)
				{
					return;
				}
				N->Props->SetStringField(TEXT("value"), Text.ToString());
				EmitValue(TEXT("changed"), MakeShared<FJsonValueString>(Text.ToString()));
			});
	}
	if (Type == TEXT("slider"))
	{
		return SNew(SSlider)
			.MinValue((float)Node->GetNumber(TEXT("min"), 0.0))
			.MaxValue((float)Node->GetNumber(TEXT("max"), 1.0))
			.StepSize((float)Node->GetNumber(TEXT("step"), 0.01))
			.Orientation(Node->GetString(TEXT("orientation")) == TEXT("vertical") ? Orient_Vertical : Orient_Horizontal)
			.Value_Lambda([W]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return N ? (float)N->GetNumber(TEXT("value"), 0.0) : 0.f; })
			.OnValueChanged_Lambda([W, EmitValue](float Value)
			{
				if (TSharedPtr<FAnimTKNode> N = W.Pin())
				{
					N->Props->SetNumberField(TEXT("value"), Value);
				}
				EmitValue(TEXT("changed"), MakeShared<FJsonValueNumber>(Value));
			})
			.OnMouseCaptureBegin_Lambda([W, EmitValue]()
			{
				TSharedPtr<FAnimTKNode> N = W.Pin();
				EmitValue(TEXT("begin"), MakeShared<FJsonValueNumber>(N ? N->GetNumber(TEXT("value")) : 0.0), true);
			})
			.OnMouseCaptureEnd_Lambda([W, EmitValue]()
			{
				TSharedPtr<FAnimTKNode> N = W.Pin();
				EmitValue(TEXT("end"), MakeShared<FJsonValueNumber>(N ? N->GetNumber(TEXT("value")) : 0.0), true);
			});
	}
	if (Type == TEXT("spin"))
	{
		const bool bInteger = Node->GetBool(TEXT("integer"));
		return SNew(SSpinBox<double>)
			.MinValue(Node->Has(TEXT("min")) ? TOptional<double>(Node->GetNumber(TEXT("min"))) : TOptional<double>())
			.MaxValue(Node->Has(TEXT("max")) ? TOptional<double>(Node->GetNumber(TEXT("max"))) : TOptional<double>())
			.MinSliderValue(Node->Has(TEXT("min")) ? TOptional<double>(Node->GetNumber(TEXT("min"))) : TOptional<double>())
			.MaxSliderValue(Node->Has(TEXT("max")) ? TOptional<double>(Node->GetNumber(TEXT("max"))) : TOptional<double>())
			.Delta(Node->GetNumber(TEXT("step"), bInteger ? 1.0 : 0.0))
			.MinFractionalDigits(bInteger ? TOptional<int32>(0) : TOptional<int32>())
			.MaxFractionalDigits(bInteger ? TOptional<int32>(0) : TOptional<int32>(3))
			.Value_Lambda([W]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return N ? N->GetNumber(TEXT("value"), 0.0) : 0.0; })
			.OnValueChanged_Lambda([W, EmitValue, bInteger](double Value)
			{
				if (bInteger)
				{
					Value = FMath::RoundToDouble(Value);
				}
				if (TSharedPtr<FAnimTKNode> N = W.Pin())
				{
					N->Props->SetNumberField(TEXT("value"), Value);
				}
				EmitValue(TEXT("changed"), MakeShared<FJsonValueNumber>(Value));
			})
			.OnValueCommitted_Lambda([W, EmitValue, bInteger](double Value, ETextCommit::Type)
			{
				if (bInteger)
				{
					Value = FMath::RoundToDouble(Value);
				}
				if (TSharedPtr<FAnimTKNode> N = W.Pin())
				{
					N->Props->SetNumberField(TEXT("value"), Value);
				}
				EmitValue(TEXT("committed"), MakeShared<FJsonValueNumber>(Value));
			});
	}
	if (Type == TEXT("combo"))
	{
		const FString Current = Node->GetString(TEXT("value"));
		TSharedPtr<FString> Initial;
		for (const FString& Option : ReadStrings(Node->Props->TryGetField(TEXT("options"))))
		{
			Node->Options.Add(MakeShared<FString>(Option));
			if (Option == Current)
			{
				Initial = Node->Options.Last();
			}
		}
		return SAssignNew(Node->Combo, STextComboBox)
			.OptionsSource(&Node->Options)
			.InitiallySelectedItem(Initial)
			.OnSelectionChanged_Lambda([W, T, Id](TSharedPtr<FString> Selected, ESelectInfo::Type Info)
			{
				TSharedPtr<FAnimTKNode> N = W.Pin();
				TSharedPtr<FAnimTKWidgetTree> Tree = T.Pin();
				if (!N || !Tree || !Selected.IsValid() || Info == ESelectInfo::Direct)
				{
					return;
				}
				N->Props->SetStringField(TEXT("value"), *Selected);
				TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("value"), *Selected);
				Payload->SetNumberField(TEXT("index"), N->Options.IndexOfByKey(Selected));
				Tree->Emit(Id, TEXT("changed"), Payload);
			});
	}
	if (Type == TEXT("list") || Type == TEXT("tiles"))
	{
		ReadItems(Node, Node->Props->TryGetField(TEXT("items")));
		const ESelectionMode::Type Mode = Node->GetBool(TEXT("multi"), Type == TEXT("list")) ? ESelectionMode::Multi : ESelectionMode::Single;

		auto SelectedIds = [W]()
		{
			TArray<FString> Ids;
			if (TSharedPtr<FAnimTKNode> N = W.Pin())
			{
				TSharedPtr<SListView<FAnimTKItemPtr>> View = N->List.IsValid() ? N->List : StaticCastSharedPtr<SListView<FAnimTKItemPtr>>(N->Tiles);
				if (View.IsValid())
				{
					// Keep the list's order rather than the click order.
					for (const FAnimTKItemPtr& Item : N->Items)
					{
						if (View->IsItemSelected(Item))
						{
							Ids.Add(Item->Id);
						}
					}
				}
			}
			return Ids;
		};
		auto OnSelection = [W, T, Id, SelectedIds](FAnimTKItemPtr, ESelectInfo::Type Info)
		{
			TSharedPtr<FAnimTKNode> N = W.Pin();
			TSharedPtr<FAnimTKWidgetTree> Tree = T.Pin();
			if (!N || !Tree || Info == ESelectInfo::Direct)
			{
				return;
			}
			const TArray<FString> Ids = SelectedIds();
			N->Props->SetArrayField(TEXT("selected"), StringArray(Ids));
			TSharedPtr<FJsonObject> Payload = ModifierPayload();
			Payload->SetArrayField(TEXT("items"), StringArray(Ids));
			Tree->Emit(Id, TEXT("selection"), Payload);
		};
		auto OnDoubleClick = [T, Id](FAnimTKItemPtr Item)
		{
			if (TSharedPtr<FAnimTKWidgetTree> Tree = T.Pin())
			{
				TSharedPtr<FJsonObject> Payload = ModifierPayload();
				Payload->SetStringField(TEXT("item"), Item.IsValid() ? Item->Id : FString());
				Tree->Emit(Id, TEXT("double_click"), Payload);
			}
		};
		auto OnContext = [W, T, Id, SelectedIds]() -> TSharedPtr<SWidget>
		{
			TSharedPtr<FAnimTKNode> N = W.Pin();
			TSharedPtr<FAnimTKWidgetTree> Tree = T.Pin();
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!N || !Tree || !N->Props->TryGetArrayField(TEXT("menu"), Entries) || Entries->Num() == 0)
			{
				return nullptr;
			}
			const TArray<FString> Ids = SelectedIds();
			return Tree->MakeMenu(Id, *Entries, Ids.Num() > 0 ? Ids[0] : FString(), Ids);
		};

		if (Type == TEXT("list"))
		{
			return SAssignNew(Node->List, SListView<FAnimTKItemPtr>)
				.ListItemsSource(&Node->Items)
				.SelectionMode(Mode)
				.OnSelectionChanged_Lambda(OnSelection)
				.OnMouseButtonDoubleClick_Lambda(OnDoubleClick)
				.OnContextMenuOpening_Lambda(OnContext)
				.OnGenerateRow_Lambda([](FAnimTKItemPtr Item, const TSharedRef<STableViewBase>& Owner) -> TSharedRef<ITableRow>
				{
					TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
					if (Item->bHasColor)
					{
						Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(2.f, 0.f, 4.f, 0.f)
						[
							SNew(SImage).Image(FAppStyle::Get().GetBrush("WhiteBrush")).ColorAndOpacity(Item->Color).DesiredSizeOverride(FVector2D(10, 10))
						];
					}
					if (!Item->Icon.IsEmpty())
					{
						Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
						[
							SNew(SImage).Image(AnimTK::ResolveBrush(Item->Icon, FVector2D(16, 16))).DesiredSizeOverride(FVector2D(16, 16))
						];
					}
					Row->AddSlot().FillWidth(1.f).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(Item->Label))];
					return SNew(STableRow<FAnimTKItemPtr>, Owner)
						.ToolTipText(FText::FromString(Item->Tooltip))
						.Padding(FMargin(2.f, 1.f))
						[Row];
				});
		}

		const float Tile = (float)Node->GetNumber(TEXT("tile_size"), 96.0);
		return SAssignNew(Node->Tiles, STileView<FAnimTKItemPtr>)
			.ListItemsSource(&Node->Items)
			.SelectionMode(Mode)
			.ItemWidth(Tile + 8.f)
			.ItemHeight(Tile + 26.f)
			.OnSelectionChanged_Lambda(OnSelection)
			.OnMouseButtonDoubleClick_Lambda(OnDoubleClick)
			.OnContextMenuOpening_Lambda(OnContext)
			.OnGenerateTile_Lambda([Tile](FAnimTKItemPtr Item, const TSharedRef<STableViewBase>& Owner) -> TSharedRef<ITableRow>
			{
				const FSlateBrush* Brush = AnimTK::ResolveBrush(Item->Icon, FVector2D(Tile, Tile));
				TSharedRef<SWidget> Thumb = Brush != nullptr
					? StaticCastSharedRef<SWidget>(SNew(SImage).Image(Brush).DesiredSizeOverride(FVector2D(Tile, Tile)))
					: StaticCastSharedRef<SWidget>(SNew(SBorder)
						.BorderImage(FAppStyle::Get().GetBrush("WhiteBrush"))
						.BorderBackgroundColor(Item->bHasColor ? Item->Color : FLinearColor(0.12f, 0.12f, 0.12f))
						.HAlign(HAlign_Center).VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(FText::FromString(Item->Label.Left(2).ToUpper())).Font(FCoreStyle::GetDefaultFontStyle("Bold", 18))
						]);
				return SNew(STableRow<FAnimTKItemPtr>, Owner)
					.ToolTipText(FText::FromString(Item->Tooltip.IsEmpty() ? Item->Label : Item->Tooltip))
					.Padding(FMargin(4.f))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
						[
							SNew(SBox).WidthOverride(Tile).HeightOverride(Tile)[Thumb]
						]
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 2.f)
						[
							SNew(STextBlock).Text(FText::FromString(Item->Label)).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						]
					];
			});
	}
	if (Type == TEXT("image"))
	{
		const float Size = (float)Node->GetNumber(TEXT("size"), 32.0);
		return SNew(SImage)
			.DesiredSizeOverride(FVector2D(Size, Size))
			.Image_Lambda([W, Size]() -> const FSlateBrush*
			{
				TSharedPtr<FAnimTKNode> N = W.Pin();
				return N ? AnimTK::ResolveBrush(N->GetString(TEXT("icon")), FVector2D(Size, Size)) : nullptr;
			});
	}
	if (Type == TEXT("separator"))
	{
		return SNew(SSeparator).Orientation(Node->GetString(TEXT("orientation")) == TEXT("vertical") ? Orient_Vertical : Orient_Horizontal);
	}
	if (Type == TEXT("spacer"))
	{
		const float Size = (float)Node->GetNumber(TEXT("size"), 8.0);
		return SNew(SSpacer).Size(FVector2D(Size, Size));
	}
	if (Type == TEXT("progress"))
	{
		return SNew(SProgressBar)
			.Percent_Lambda([W]() { TSharedPtr<FAnimTKNode> N = W.Pin(); return N ? TOptional<float>((float)N->GetNumber(TEXT("value"), 0.0)) : TOptional<float>(); });
	}
	if (Type == TEXT("canvas"))
	{
		TSharedRef<SAnimTKCanvas> Canvas = SNew(SAnimTKCanvas, AsShared(), Id);
		Node->Canvas = Canvas;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Node->Props->TryGetArrayField(TEXT("items"), Arr)) Canvas->SetItems(*Arr);
		if (Node->Props->TryGetArrayField(TEXT("menu"), Arr)) Canvas->SetMenu(*Arr);
		Canvas->SetSelected(ReadStrings(Node->Props->TryGetField(TEXT("selected"))));
		Canvas->SetEditMode(Node->GetBool(TEXT("edit")));
		Canvas->SetBackground(Node->GetString(TEXT("background")));
		Canvas->SetSnap((float)Node->GetNumber(TEXT("snap"), 0.0));
		if (Node->Props->TryGetArrayField(TEXT("canvas_size"), Arr) && Arr->Num() == 2)
		{
			Canvas->SetCanvasSize(FVector2D((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber()));
		}
		return Canvas;
	}

	return SNew(STextBlock)
		.ColorAndOpacity(FLinearColor(1.f, 0.4f, 0.3f))
		.Text(FText::FromString(FString::Printf(TEXT("AnimToolkit: unknown widget type '%s'"), *Type)));
}

bool FAnimTKWidgetTree::SetProp(const FString& WidgetId, const FString& Prop, const TSharedPtr<FJsonValue>& Value)
{
	TSharedRef<FAnimTKNode>* Found = Nodes.Find(WidgetId);
	if (Found == nullptr)
	{
		return false;
	}
	TSharedRef<FAnimTKNode> Node = *Found;
	Node->Props->SetField(Prop, Value.IsValid() ? Value : MakeShared<FJsonValueNull>());

	if (Node->Canvas.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Prop == TEXT("items") && Value->TryGetArray(Arr)) Node->Canvas->SetItems(*Arr);
		else if (Prop == TEXT("menu") && Value->TryGetArray(Arr)) Node->Canvas->SetMenu(*Arr);
		else if (Prop == TEXT("selected")) Node->Canvas->SetSelected(ReadStrings(Value));
		else if (Prop == TEXT("edit")) Node->Canvas->SetEditMode(Node->GetBool(TEXT("edit")));
		else if (Prop == TEXT("background")) Node->Canvas->SetBackground(Node->GetString(TEXT("background")));
		else if (Prop == TEXT("snap")) Node->Canvas->SetSnap((float)Node->GetNumber(TEXT("snap")));
		else if (Prop == TEXT("fit")) Node->Canvas->RequestFit();
		else if (Prop == TEXT("canvas_size") && Value->TryGetArray(Arr) && Arr->Num() == 2)
		{
			Node->Canvas->SetCanvasSize(FVector2D((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber()));
		}
		return true;
	}

	TSharedPtr<SListView<FAnimTKItemPtr>> View = Node->List.IsValid() ? Node->List : StaticCastSharedPtr<SListView<FAnimTKItemPtr>>(Node->Tiles);
	if (View.IsValid())
	{
		if (Prop == TEXT("items"))
		{
			ReadItems(Node, Value);
			View->RebuildList();
			ApplySelection(Node);
		}
		else if (Prop == TEXT("selected"))
		{
			ApplySelection(Node);
		}
		return true;
	}

	if (Node->Combo.IsValid())
	{
		if (Prop == TEXT("options"))
		{
			Node->Options.Reset();
			for (const FString& Option : ReadStrings(Value))
			{
				Node->Options.Add(MakeShared<FString>(Option));
			}
			Node->Combo->RefreshOptions();
		}
		if (Prop == TEXT("options") || Prop == TEXT("value"))
		{
			const FString Current = Node->GetString(TEXT("value"));
			for (const TSharedPtr<FString>& Option : Node->Options)
			{
				if (*Option == Current)
				{
					Node->Combo->SetSelectedItem(Option);
					break;
				}
			}
		}
		return true;
	}

	if (Prop == TEXT("value") && (Node->TextBox.IsValid() || Node->MultiText.IsValid()))
	{
		TGuardValue<bool> Guard(Node->bSuppressEvents, true);
		const FText Text = FText::FromString(Node->GetString(TEXT("value")));
		if (Node->TextBox.IsValid()) Node->TextBox->SetText(Text);
		if (Node->MultiText.IsValid()) Node->MultiText->SetText(Text);
	}
	return true;
}

TSharedPtr<FJsonValue> FAnimTKWidgetTree::GetValue(const FString& WidgetId) const
{
	const TSharedRef<FAnimTKNode>* Found = Nodes.Find(WidgetId);
	if (Found == nullptr)
	{
		return MakeShared<FJsonValueNull>();
	}
	const TSharedRef<FAnimTKNode>& Node = *Found;
	if (Node->Canvas.IsValid())
	{
		return MakeShared<FJsonValueArray>(StringArray(Node->Canvas->GetSelected()));
	}
	if (Node->List.IsValid() || Node->Tiles.IsValid())
	{
		const TSharedPtr<FJsonValue> Selected = Node->Props->TryGetField(TEXT("selected"));
		return Selected.IsValid() ? Selected : MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
	}
	const TSharedPtr<FJsonValue> Value = Node->Props->TryGetField(TEXT("value"));
	return Value.IsValid() ? Value : MakeShared<FJsonValueNull>();
}
