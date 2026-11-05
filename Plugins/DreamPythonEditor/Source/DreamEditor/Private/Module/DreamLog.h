// SPDX-License-Identifier: Apache-2.0
// LogDream: Dream's own lines (run header and footer summaries, notes, refusals), never Python output (§6.3 rule 4).
// Defined in Module/DreamLog.cpp (shell slice). Python output is echoed under LogPython by FDreamLogForwarder.
#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

DREAMEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogDream, Log, All);

/** User-facing text namespace (brief: LOCTEXT_NAMESPACE "Dream"). Each .cpp defines and undefines LOCTEXT_NAMESPACE itself. */
#define DREAM_LOCTEXT_NAMESPACE "Dream"
