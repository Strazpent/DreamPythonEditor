// SPDX-License-Identifier: Apache-2.0
// UDreamUserSettings (TDD §11.1.1 subset, slice A choice A37): the user layer only.
#include "Settings/DreamUserSettings.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamLog.h"
#include "Run/DreamDeferral.h"
#include "UObject/UnrealType.h"

bool UDreamUserSettings::GetTransactionDefault(EDreamRunRoute Route) const
{
	// RT-08, §5.2.1: the route's own default, replaced by the user's per-route setting where one exists. The U6 override
	// on the request wins over both; FDreamRunService applies it (§5.9.1).
	switch (Route)
	{
	case EDreamRunRoute::Selection:
	case EDreamRunRoute::Line:
	case EDreamRunRoute::Cell:
		// (run group, B40) The per-route seed of an untouched tab toggle, and a re-run whose tab is closed (P2-run-02).
		return bTransactionSelectionAndLine;
	case EDreamRunRoute::Buffer:
		return bTransactionBuffer;
	case EDreamRunRoute::Repl:
		return bTransactionRepl;
	case EDreamRunRoute::ConsoleExecutor:
		return bTransactionConsole;
	case EDreamRunRoute::RunFile:
		return bTransactionRunFile;
	default:
		return DreamRunText::RouteTransactionDefault(Route);
	}
}

// ---- (chrome) PO-04 view settings ----

FSimpleMulticastDelegate& UDreamUserSettings::OnViewSettingsChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

namespace DreamUserSettingsLocal
{
	template <typename TSettings>
	auto ViewSettingMember(TSettings& Settings, EDreamViewSetting Setting) -> decltype(&Settings.bShowReplTab)
	{
		switch (Setting)
		{
		case EDreamViewSetting::ReplTab: return &Settings.bShowReplTab;
		case EDreamViewSetting::ProblemsTab: return &Settings.bShowProblemsTab;
		case EDreamViewSetting::ReplPrompt: return &Settings.bShowReplPrompt;
		case EDreamViewSetting::RunHeaders: return &Settings.bShowRunHeaders;
		case EDreamViewSetting::StatusLine: return &Settings.bShowStatusLine;
		case EDreamViewSetting::LineNumbers: return &Settings.bShowLineNumbers;
		case EDreamViewSetting::ProjectPanel: return &Settings.bShowProjectPanel;       // (sidebar)
		case EDreamViewSetting::StructurePanel: return &Settings.bShowStructurePanel;   // (sidebar)
		default: return nullptr;
		}
	}
}

bool UDreamUserSettings::GetViewSetting(EDreamViewSetting Setting) const
{
	const bool* Member = DreamUserSettingsLocal::ViewSettingMember(*this, Setting);
	return Member != nullptr && *Member;
}

void UDreamUserSettings::SetViewSetting(EDreamViewSetting Setting, bool bValue)
{
	UDreamUserSettings* Settings = GetMutable();
	bool* Member = DreamUserSettingsLocal::ViewSettingMember(*Settings, Setting);
	if (Member == nullptr || *Member == bValue)
	{
		return;
	}
	*Member = bValue;
	static const FName Names[] = {
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowReplTab), GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowProblemsTab),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowReplPrompt), GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowRunHeaders),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowStatusLine), GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowLineNumbers),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowProjectPanel), GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowStructurePanel) };   // (sidebar) the last two
	static_assert(UE_ARRAY_COUNT(Names) == static_cast<int32>(EDreamViewSetting::StructurePanel) + 1, "One key name per EDreamViewSetting");
	SaveKey(Names[static_cast<int32>(Setting)]);   // per user, at once (PO-04: "persisted per user"), this key only
	OnViewSettingsChanged().Broadcast();
}

void UDreamUserSettings::SetThemeId(const FString& Id)
{
	UDreamUserSettings* Settings = GetMutable();
	if (Id.IsEmpty() || Settings->ThemeId == Id)
	{
		return;
	}
	Settings->ThemeId = Id;
	SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, ThemeId));
	OnViewSettingsChanged().Broadcast();
}

namespace DreamUserSettingsSave
{
	// (chrome, reviews r8/r9) One checked write per SaveKey. A refused write (the file still held for about 100 ms after the
	// previous one, a scanner) leaves the write pending: the next attempt is due BackoffSeconds after the END of the last one
	// (0.5, 1, 2, 4 ... 30 s; in a user editor the engine's Move sleeps 0.5 s on each failure, so an attempt is never due
	// every frame), with one Warning after WarnAfterSeconds pending. A read-only file is not retried: one Warning, and the
	// next SaveKey tries again. A successful write (SaveKey or retry) ends the pending state.
	constexpr double FirstBackoffSeconds = 0.5;
	constexpr double MaxBackoffSeconds = 30.0;
	constexpr double WarnAfterSeconds = 5.0;

	FTSTicker::FDelegateHandle RetryTicker;     // checks NextAttemptSeconds each frame (cheap); no attempt before it
	bool bWritePending = false;
	bool bWarned = false;
	bool bReadOnlyWarned = false;
	double FirstFailureSeconds = 0.0;
	double NextAttemptSeconds = 0.0;
	double BackoffSeconds = FirstBackoffSeconds;
	int32 ForcedFailuresForTest = 0;
	bool bForcedReadOnlyForTest = false;
	TArray<double> AttemptStarts;               // this pending episode's attempts (test hook)
	TArray<double> AttemptEnds;

	FSimpleMulticastDelegate& FileWritten()
	{
		static FSimpleMulticastDelegate Delegate;
		return Delegate;
	}

	FString ConfigFile()
	{
		return UDreamUserSettings::StaticClass()->GetConfigName();
	}

	FString FileName()
	{
		return FPaths::GetCleanFilename(ConfigFile());
	}

	/** The file the config cache writes (the branch's path; the config name is that path for EditorSettings). */
	FString IniPath()
	{
		if (GConfig != nullptr)
		{
			if (const FConfigBranch* Branch = GConfig->FindBranch(FName(*ConfigFile()), ConfigFile()))
			{
				if (!Branch->IniPath.IsEmpty())
				{
					return Branch->IniPath;
				}
			}
		}
		return ConfigFile();
	}

	enum class EWrite : uint8 { Written, ReadOnly, Failed };

	/** One flush of the settings file from the config cache, timed for the test hook. */
	EWrite Attempt(double& OutStart, double& OutEnd)
	{
		OutStart = FPlatformTime::Seconds();
		EWrite Result = EWrite::Failed;
		const FString Path = IniPath();
		if (ForcedFailuresForTest > 0)
		{
			--ForcedFailuresForTest;   // a refused (or read-only) write, as the engine reports it, without touching the file
			Result = bForcedReadOnlyForTest ? EWrite::ReadOnly : EWrite::Failed;
		}
		else if (IFileManager::Get().FileExists(*Path) && IFileManager::Get().IsReadOnly(*Path))
		{
			Result = EWrite::ReadOnly;
		}
		else if (GConfig != nullptr && GConfig->Flush(/*bRemoveFromCache*/ false, ConfigFile()))
		{
			Result = EWrite::Written;
		}
		OutEnd = FPlatformTime::Seconds();
		return Result;
	}

	void StopPending()
	{
		FDreamDeferral::RemoveTicker(RetryTicker);
		RetryTicker.Reset();
		bWritePending = false;
		bWarned = false;
		FirstFailureSeconds = 0.0;
		NextAttemptSeconds = 0.0;
		BackoffSeconds = FirstBackoffSeconds;
	}

	void HandleResult(EWrite Result, double Start, double End, bool bRetry);

	void EnsureTicker()
	{
		if (RetryTicker.IsValid())
		{
			return;
		}
		RetryTicker = FDreamDeferral::AddTicker(TEXT("DreamSettingsSaveRetry"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
			/*bNeedsPython*/ false, [](float)
			{
				if (!bWritePending)
				{
					RetryTicker.Reset();
					return false;
				}
				const double Now = FPlatformTime::Seconds();
				if (!bWarned && Now - FirstFailureSeconds >= WarnAfterSeconds)
				{
					bWarned = true;
					UE_LOG(LogDream, Warning, TEXT("Could not write %s for %.0f s; still retrying."), *FileName(), WarnAfterSeconds);
				}
				if (Now < NextAttemptSeconds)
				{
					return true;
				}
				double Start = 0.0;
				double End = 0.0;
				const EWrite Result = Attempt(Start, End);
				HandleResult(Result, Start, End, /*bRetry*/ true);
				return bWritePending;   // HandleResult reset the handle when it ended the pending state
			});
	}

	void HandleResult(EWrite Result, double Start, double End, bool bRetry)
	{
		if (Result == EWrite::Written)
		{
			if (bWritePending)
			{
				AttemptStarts.Add(Start);
				AttemptEnds.Add(End);
				// After the 5 s warning the success is said at Display, whichever write it was (review r10).
				if (bWarned)
				{
					UE_LOG(LogDream, Display, TEXT("Wrote %s after retrying."), *FileName());
				}
				else if (bRetry)
				{
					UE_LOG(LogDream, Log, TEXT("Wrote %s after retrying."), *FileName());
				}
			}
			StopPending();
			bReadOnlyWarned = false;
			FileWritten().Broadcast();
			return;
		}
		if (Result == EWrite::ReadOnly)
		{
			if (!bReadOnlyWarned)
			{
				bReadOnlyWarned = true;
				UE_LOG(LogDream, Warning, TEXT("%s is read-only; Dream Python Editor settings are not saved."), *FileName());
			}
			StopPending();   // no retry: the next SaveKey tries again
			return;
		}
		if (!bWritePending)
		{
			bWritePending = true;
			bWarned = false;
			FirstFailureSeconds = End;
			BackoffSeconds = FirstBackoffSeconds;
			AttemptStarts.Reset();
			AttemptEnds.Reset();
			UE_LOG(LogDream, Log, TEXT("Could not write %s; retrying."), *FileName());
		}
		else
		{
			BackoffSeconds = FMath::Min(BackoffSeconds * 2.0, MaxBackoffSeconds);
		}
		AttemptStarts.Add(Start);
		AttemptEnds.Add(End);
		NextAttemptSeconds = End + BackoffSeconds;   // from the end of this attempt
		EnsureTicker();
	}
}

FSimpleMulticastDelegate& UDreamUserSettings::OnFileWritten()
{
	return DreamUserSettingsSave::FileWritten();
}

void UDreamUserSettings::CancelSaveRetry()
{
	// Not the ticker handle: FEngineLoop::Exit resets the core ticker before the module unloads (review r9).
	using namespace DreamUserSettingsSave;
	const bool bWasPending = bWritePending;
	RetryTicker.Reset();
	bWritePending = false;
	if (!bWasPending)
	{
		return;
	}
	double Start = 0.0;
	double End = 0.0;
	if (Attempt(Start, End) == EWrite::Written)
	{
		UE_LOG(LogDream, Log, TEXT("Wrote %s at shutdown."), *FileName());
		FileWritten().Broadcast();
	}
	else
	{
		UE_LOG(LogDream, Warning, TEXT("Could not write %s at shutdown; the last setting changes may be lost."), *FileName());
	}
	StopPending();
}

void UDreamUserSettings::SetSaveFailuresForTest(int32 Count, ESaveFailureForTest Kind)
{
	DreamUserSettingsSave::ForcedFailuresForTest = FMath::Max(0, Count);
	DreamUserSettingsSave::bForcedReadOnlyForTest = Kind == ESaveFailureForTest::ReadOnly;
}

UDreamUserSettings::FSaveRetryStateForTest UDreamUserSettings::GetSaveRetryStateForTest()
{
	using namespace DreamUserSettingsSave;
	FSaveRetryStateForTest Out;
	Out.bPending = bWritePending;
	Out.bWarned = bWarned;
	Out.FirstFailureSeconds = FirstFailureSeconds;
	Out.NextAttemptSeconds = NextAttemptSeconds;
	Out.AttemptStarts = AttemptStarts;
	Out.AttemptEnds = AttemptEnds;
	return Out;
}

void UDreamUserSettings::SaveKey(FName PropertyName)
{
	const FProperty* Property = FindFProperty<FProperty>(StaticClass(), PropertyName);
	if (Property == nullptr || !Property->HasAnyPropertyFlags(CPF_Config) || GConfig == nullptr)
	{
		return;
	}
	// Not SaveConfig(FSaveConfigContext with PropertyNames): it flushes internally and drops the result, and a checked flush
	// after it wrote the file twice (the second move over EditorSettings.ini failed right after the first). So the key is set
	// as Obj.cpp's SaveConfig sets it (the class's section, the property's name, its PPF_SerializedAsImportText text on the
	// class's config file; an array as its repeated keys) and the file is flushed once, checked (review r8 ruling). The mirror
	// covers this class's kinds only (review r9 nit); another kind goes through SaveConfig, then the same checked flush.
	UDreamUserSettings* Settings = GetMutable();
	bool bMirrored = Property->ArrayDim == 1 && !Property->IsA<FSetProperty>() && !Property->IsA<FMapProperty>()
		&& !Property->HasAnyPropertyFlags(CPF_GlobalConfig | CPF_Deprecated) && Property->GetOwnerClass() == StaticClass();
#if WITH_METADATA
	bMirrored = bMirrored && !Property->HasMetaData(TEXT("ConsoleVariable")) && !Property->HasMetaData(TEXT("DeprecatedProperty"));
#endif
	if (!ensureMsgf(bMirrored, TEXT("UDreamUserSettings::SaveKey: %s is a kind the key mirror does not cover"), *PropertyName.ToString()))
	{
		UObject::FSaveConfigContext Context;
		Context.PropertyNames.Add(PropertyName);
		Settings->SaveConfig(Context);
	}
	else
	{
		FString Section = StaticClass()->GetPathName();
		Settings->OverrideConfigSection(Section);
		const FString ConfigFile = DreamUserSettingsSave::ConfigFile();
		const FName Key = Property->GetFName();
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
		{
			if (GConfig->GetSection(*Section, /*Force*/ false, ConfigFile) != nullptr)
			{
				GConfig->ResetKeyInSection(*Section, Key, ConfigFile);
			}
			FScriptArrayHelper_InContainer Helper(Array, Settings);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				FString Buffer;
				Array->Inner->ExportTextItem_Direct(Buffer, Helper.GetRawPtr(Index), Helper.GetRawPtr(Index), Settings, PPF_SerializedAsImportText);
				GConfig->AddToSection(*Section, Key, Buffer, ConfigFile);
			}
		}
		else
		{
			FString Text;
			Property->ExportText_InContainer(0, Text, Settings, Settings, Settings, PPF_SerializedAsImportText);
			GConfig->SetString(*Section, *Key.ToString(), *Text, ConfigFile);
		}
	}
	// One checked write; a refused one stays pending (the config cache keeps the value) and retries with a backoff.
	double Start = 0.0;
	double End = 0.0;
	const DreamUserSettingsSave::EWrite Result = DreamUserSettingsSave::Attempt(Start, End);
	DreamUserSettingsSave::HandleResult(Result, Start, End, /*bRetry*/ false);
}

#if WITH_EDITOR
void UDreamUserSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);   // OnSettingChanged, as before
	const FName Property = PropertyChangedEvent.GetMemberPropertyName();
	static const FName ViewProperties[] =
	{
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, ThemeId),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowStatusLine),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowReplPrompt),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowReplTab),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowProblemsTab),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowRunHeaders),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowLineNumbers),
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowProjectPanel),     // (sidebar)
		GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowStructurePanel),   // (sidebar)
	};
	for (const FName& View : ViewProperties)
	{
		if (Property.IsNone() || Property == View)
		{
			OnViewSettingsChanged().Broadcast();
			return;
		}
	}
}
#endif
