// SPDX-License-Identifier: Apache-2.0
// The property-kinds fixture of T-RES-026 (TDD §7.5.2, N1, E2; run group): one property of each object-reference kind, so
// a test checks which names _dream.hard_object_props("DreamTestPropKindsFixture") returns. Expected: hardobject, classref,
// weakobject, renamedtarget (ScriptName) and prefixed (the "In" prefix PythonScriptPlugin strips); never the soft, soft
// class, lazy, int or array properties, and neither of the colliding pair Twin_Name / TwinName. Header only: no member
// has an out-of-line body. Repository only (DreamEditorTests).
#pragma once

#include "CoreMinimal.h"
#include "UObject/LazyObjectPtr.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPtr.h"
#include "Templates/SubclassOf.h"
#include "DreamTestPropKindsFixture.generated.h"

UCLASS(BlueprintType, Transient, meta = (DisplayName = "Dream Python Editor Test Prop Kinds Fixture"))
class UDreamTestPropKindsFixture : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Transient)
	TObjectPtr<UObject> HardObject;             // FObjectProperty: hard

	UPROPERTY(Transient)
	TSubclassOf<UObject> ClassRef;              // FClassProperty (an FObjectProperty): hard

	UPROPERTY(Transient)
	TWeakObjectPtr<UObject> WeakObject;         // FWeakObjectProperty: kept (N1)

	UPROPERTY(Transient)
	TSoftObjectPtr<UObject> SoftObject;         // FSoftObjectProperty: never

	UPROPERTY(Transient)
	TSoftClassPtr<UObject> SoftClass;           // FSoftClassProperty: never

	UPROPERTY(Transient)
	TLazyObjectPtr<UObject> LazyObject;         // FLazyObjectProperty: never

	UPROPERTY(Transient, meta = (ScriptName = "renamed_target"))
	TObjectPtr<UObject> OriginalTarget;         // listed under its ScriptName: renamedtarget

	UPROPERTY(Transient)
	TObjectPtr<UObject> InPrefixed;             // Python name "prefixed": prefixed

	UPROPERTY(Transient)
	TObjectPtr<UObject> Twin_Name;              // collides with TwinName after normalisation: both dropped

	UPROPERTY(Transient)
	TObjectPtr<UObject> TwinName;

	UPROPERTY(Transient)
	int32 PlainCount = 0;                       // not an object reference

	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> ObjectArray;    // a container: resolves at type level
};
