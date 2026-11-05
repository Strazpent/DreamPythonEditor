// SPDX-License-Identifier: Apache-2.0
// Shared project defaults (TDD §11.1.1 UDreamProjectSettings): config=Editor, defaultconfig, so
// <Project>/Config/DefaultEditor.ini [/Script/DreamEditor.DreamProjectSettings]; Project Settings > Plugins > Dream.
// (slice A choice) Only the two keys slice A reads. Deferred (M4): the read-only-file save flow of §11.1.3 (the checkout of
// DefaultEditor.ini comes with the full layering and the settings inspector).
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DreamProjectSettings.generated.h"

UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Dream Python Editor"))
class DREAMEDITOR_API UDreamProjectSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	static const UDreamProjectSettings* Get() { return GetDefault<UDreamProjectSettings>(); }

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Dream"); }

	UPROPERTY(config)
	int32 SettingsVersion = 1;

	/** UEI-08 "disabled per project": register the Python (Dream) console executor (§6.6). */
	UPROPERTY(config, EditAnywhere, Category = "Console", meta = (DisplayName = "Register the Python (Dream) console executor"))
	bool bRegisterConsoleExecutor = true;

	/** output.echo_cap_lines_per_s (D19): LogPython echo cap per second; 0 = unlimited. */
	UPROPERTY(config, EditAnywhere, Category = "Output", meta = (DisplayName = "Log echo cap (lines per second)", ClampMin = "0"))
	int32 EchoCapLinesPerSecond = 5000;
};
