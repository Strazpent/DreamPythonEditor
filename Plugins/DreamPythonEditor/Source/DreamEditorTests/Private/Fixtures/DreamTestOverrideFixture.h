// SPDX-License-Identifier: Apache-2.0
// CallPythonOverride (TDD §14.2, §5.15, T-SUP-003): a UObject whose BlueprintImplementableEvent a Python subclass overrides,
// and a native method that calls it through ProcessEvent. From inside a run the call is native re-entry into Python
// (PyGILState_Ensure nested in a C call, §5.4.5), so the supervisor must defer: no pump and no delivery inside the hook.
//   @unreal.uclass()
//   class Hook(unreal.DreamTestOverrideFixture):
//       @unreal.ufunction(override=True)
//       def on_python_override(self, value): ...
//   Hook().call_python_override(3)      # native -> ProcessEvent -> the Python override, 3 times
// Repository only (DreamEditorTests).
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DreamTestOverrideFixture.generated.h"

UCLASS(Blueprintable, BlueprintType, meta = (DisplayName = "Dream Python Editor Test Override Fixture"))
class UDreamTestOverrideFixture : public UObject
{
	GENERATED_BODY()

public:
	/** The event a Python subclass overrides. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Dream|Tests")
	void OnPythonOverride(int32 Value);

	/** CallPythonOverride: calls OnPythonOverride Times times from native code (ProcessEvent); returns the calls made. */
	UFUNCTION(BlueprintCallable, Category = "Dream|Tests")
	int32 CallPythonOverride(int32 Times = 1);

	/** Native calls made through CallPythonOverride on this object. */
	UPROPERTY(BlueprintReadOnly, Category = "Dream|Tests")
	int32 NativeCalls = 0;
};
