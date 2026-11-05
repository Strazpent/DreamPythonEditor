// SPDX-License-Identifier: Apache-2.0
// The only two ExecPythonCommandEx call sites (TDD §2.5 rule 5, §3.6 INV-3, D14, RT-15): the run entry and the boot.
// Both are literal templates, EvaluateStatement with Flags None (attended, D14(2)); EnterRun is private with one friend.
#include "Python/DreamPython.h"

#include "Framework/Application/SlateApplication.h"
#include "IPythonScriptPlugin.h"
#include "Language/DreamLanguageServices.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Python/DreamPythonImpl.h"
#include "Run/DreamRunService.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamPythonEntryLocal
{
	/**
	 * Ruling R3: a _boot that returns False wrote "Dream did not start: <reason>" to sys.stderr first. The engine's stderr
	 * logs through unreal.log_error, which the plugin captures into the command's LogOutput (PythonScriptPlugin.cpp:1865,
	 * PyCore.cpp LogError), so the reason's first line can name the cause in the banner, the shadowing package's folder
	 * included (§2.7, ENV-07, T-ARCH-017). Empty when no entry carries it.
	 */
	FString FindBootRefusal(const TArray<FPythonLogOutputEntry>& LogOutput)
	{
		static const TCHAR* const Prefix = TEXT("Dream Python Editor did not start: ");
		for (int32 Index = LogOutput.Num() - 1; Index >= 0; --Index)
		{
			const FString& Output = LogOutput[Index].Output;
			const int32 At = Output.Find(Prefix, ESearchCase::CaseSensitive);
			if (At == INDEX_NONE)
			{
				continue;
			}
			FString Reason = Output.Mid(At + FCString::Strlen(Prefix));
			int32 Newline = INDEX_NONE;
			if (Reason.FindChar(TEXT('\n'), Newline))
			{
				Reason.LeftInline(Newline);
			}
			Reason.TrimStartAndEndInline();
			if (!Reason.IsEmpty())
			{
				return Reason;
			}
		}
		return FString();
	}
}

bool FDreamPython::IsEntryContextValid(FString& OutWhy)
{
	if (!IsInGameThread())
	{
		OutWhy = TEXT("not on the game thread");
		return false;
	}
	if (!FDreamEditorModule::IsAvailable() || !FDreamEditorModule::Get().GetRunService().IsInEndFrameStart())
	{
		OutWhy = TEXT("not inside the OnEndFrame start handler");
		return false;
	}
	if (FSlateApplication::IsInitialized())
	{
		const FSlateApplication& App = FSlateApplication::Get();
		if (App.IsTicking())
		{
			OutWhy = TEXT("Slate is ticking");
			return false;
		}
		if (App.IsProcessingInput())
		{
			OutWhy = TEXT("Slate is processing input");
			return false;
		}
		if (App.HasAnyMouseCaptor())
		{
			OutWhy = TEXT("a widget holds the mouse capture");
			return false;
		}
	}
	return true;
}

bool FDreamPython::EnterRun(FDreamRunId Id, FString& OutCommandResult, int32& OutLogOutputEntries)
{
	OutLogOutputEntries = 0;
	FString Why;
	if (!ensureMsgf(IsEntryContextValid(Why), TEXT("Dream: run entry refused (%s)"), *Why))
	{
		OutCommandResult = FString::Printf(TEXT("Dream Python Editor refused to enter the run: %s."), *Why);
		return false;
	}
	IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
	if (Plugin == nullptr || !Plugin->IsPythonAvailable() || !IsReady())
	{
		OutCommandResult = TEXT("Python is not available.");
		return false;
	}

	FPythonCommandEx Command;
	Command.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
	Command.Flags = EPythonCommandFlags::None;   // attended: script dialogs stay alive (INV-3)
	Command.Command = FString::Printf(TEXT("__import__('dream_editor')._run(%llu)"), static_cast<unsigned long long>(Id));
	const bool bOk = Plugin->ExecPythonCommandEx(Command);

	// LogOutput is not used; its size is a G2(3) statistic (RT-07, §5.2.4).
	OutLogOutputEntries = Command.LogOutput.Num();
	OutCommandResult = MoveTemp(Command.CommandResult);
	return bOk;
}

bool FDreamPython::Boot()
{
	check(IsInGameThread());
	IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
	if (Plugin == nullptr || !Plugin->IsPythonAvailable())
	{
		Impl->FailureReason = LOCTEXT("BootNoPython", "Python is not available in this editor session.");
		Impl->SetState(*this, EDreamPythonState::BootFailed);
		return false;
	}

	const uint32 BootGeneration = Impl->Generation.load();
	FPythonCommandEx Command;
	Command.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
	Command.Flags = EPythonCommandFlags::None;
	// globals() is the console dict here (RunString evaluates in it), which _boot captures as the Session (D8, E4).
	Command.Command = FString::Printf(TEXT("__import__('dream_editor')._boot(globals(), %u)"), BootGeneration);
	const bool bOk = Plugin->ExecPythonCommandEx(Command);

	if (bOk && Command.CommandResult == TEXT("True"))
	{
		Impl->bBooted = true;
		Impl->SetState(*this, EDreamPythonState::Ready);
		UE_LOG(LogDream, Log, TEXT("dream_editor booted (Python generation %u)."), BootGeneration);
		// (lang) §3.4, D4, RT-17: the language services start after a good boot (dream-check, the Stage 1 snapshot).
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetLanguageServices().OnDeferredStart();
		}
		return true;
	}

	// Ruling R3. A failed call carries the traceback in CommandResult: its last line ("ImportError: Dream Python package
	// and native module do not match; reinstall Dream") goes in the banner, the whole text in LogDream (§3.9 "Boot fails").
	// Any other result is _boot's False: the banner shows the reason _boot wrote to stderr when LogOutput has it, else
	// "returned <repr>; see LogPython".
	FString Detail;
	if (bOk)
	{
		const FString Returned = FString::Printf(TEXT("dream_editor._boot returned %s; see LogPython"), *Command.CommandResult);
		const FString Refusal = DreamPythonEntryLocal::FindBootRefusal(Command.LogOutput);
		Detail = Refusal.IsEmpty() ? Returned : Refusal;
		const FString Logged = Refusal.IsEmpty() ? Returned : FString::Printf(TEXT("%s (%s)"), *Returned, *Refusal);
		UE_LOG(LogDream, Error, TEXT("dream_editor did not boot: %s."), *Logged);
	}
	else
	{
		TArray<FString> Lines;
		Command.CommandResult.ParseIntoArrayLines(Lines, /*InCullEmpty*/ true);
		while (Lines.Num() > 0 && Lines.Last().TrimStartAndEnd().IsEmpty())
		{
			Lines.Pop();
		}
		Detail = Lines.Num() > 0 ? Lines.Last().TrimStartAndEnd() : FString(TEXT("the boot call failed without an error text; see LogPython"));
		UE_LOG(LogDream, Error, TEXT("dream_editor did not boot:\n%s"), Command.CommandResult.IsEmpty() ? *Detail : *Command.CommandResult);
	}
	Impl->FailureReason = FText::Format(LOCTEXT("BootFailedDetail", "Dream Python Editor could not start its Python package: {0}"), FText::FromString(Detail));
	Impl->SetState(*this, EDreamPythonState::BootFailed);
	return false;
}

#undef LOCTEXT_NAMESPACE
