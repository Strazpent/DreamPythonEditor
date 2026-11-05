// SPDX-License-Identifier: Apache-2.0
// Console commands (TDD §11.7, API-03): Dream.Open [path[:line]], Dream.RunFile <path>, Dream.ClearOutput. They run from the
// console box, a Slate input handler, so they only submit (D14(1)); Dream.RunFile works with the tab closed (WS-07).
// Deferred (M4): Dream.Doctor, Dream.Policy.Reload, Dream.Diagnostics.Export.
#include "Module/DreamModuleInternal.h"

#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Output/DreamOutputStore.h"
#include "Run/DreamRunFile.h"
#include "Run/DreamRunService.h"

namespace DreamConsoleCommandsLocal
{
	TArray<IConsoleObject*> Registered;

	FString JoinArgs(const TArray<FString>& Args)
	{
		FString Joined = FString::Join(Args, TEXT(" ")).TrimStartAndEnd();
		Joined.TrimQuotesInline();
		return Joined;
	}

	/** "path:line" with Windows drive letters left alone: only a trailing ":<digits>" is a line. */
	void SplitPathAndLine(const FString& Arg, FString& OutPath, int32& OutLine)
	{
		OutPath = Arg;
		OutLine = 0;
		int32 Colon = INDEX_NONE;
		if (Arg.FindLastChar(TEXT(':'), Colon) && Colon > 1 && Colon + 1 < Arg.Len())
		{
			const FString Tail = Arg.Mid(Colon + 1);
			if (Tail.IsNumeric())
			{
				OutPath = Arg.Left(Colon);
				OutLine = FCString::Atoi(*Tail);
			}
		}
	}

	void Open(const TArray<FString>& Args)
	{
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		const FString Arg = JoinArgs(Args);
		if (Arg.IsEmpty())
		{
			Module.InvokeTab();
			return;
		}
		FString Path;
		int32 Line = 0;
		SplitPathAndLine(Arg, Path, Line);
		if (Module.OpenLocation(FPaths::ConvertRelativePathToFull(Path), Line, 0).IsEmpty())
		{
			UE_LOG(LogDream, Warning, TEXT("Dream.Open: cannot open %s"), *Path);
		}
	}

	void RunFile(const TArray<FString>& Args)
	{
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		const FString Path = FPaths::ConvertRelativePathToFull(JoinArgs(Args));
		if (Path.IsEmpty() || !FPaths::FileExists(Path))
		{
			UE_LOG(LogDream, Warning, TEXT("Dream.RunFile: no such file: %s"), *Path);
			return;
		}

		// Isolated, one exec, transaction off by default (§5.2.1); an open buffer runs its text and a dirty one is saved at
		// Starting step 4 (§5.1.2); a file that is not open is decoded as a file tab would be (§4.13).
		FDreamRunRequest Request;
		FText Error;
		if (!DreamRunFile::MakeFileRequest(Path, EDreamRunRoute::RunFile, Request, Error))
		{
			UE_LOG(LogDream, Warning, TEXT("Dream.RunFile: %s"), *Error.ToString());
			return;
		}

		const FDreamSubmitResult Result = Module.GetRunService().Submit(MoveTemp(Request));
		if (!Result.bAccepted)
		{
			UE_LOG(LogDream, Warning, TEXT("Dream.RunFile: %s"), *Result.Reason.ToString());
		}
	}

	void ClearOutput(const TArray<FString>&)
	{
		FDreamEditorModule::Get().GetOutputStore().Clear();
	}
}

void DreamConsoleCommands::Register()
{
	using namespace DreamConsoleCommandsLocal;
	IConsoleManager& Console = IConsoleManager::Get();
	Registered.Add(Console.RegisterConsoleCommand(TEXT("Dream.Open"),
		TEXT("Opens the Dream tab, or a file at an optional line: Dream.Open [path[:line]]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Open), ECVF_Default));
	Registered.Add(Console.RegisterConsoleCommand(TEXT("Dream.RunFile"),
		TEXT("Runs a Python file through Dream Python Editor (Isolated namespace, one exec): Dream.RunFile <path>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunFile), ECVF_Default));
	Registered.Add(Console.RegisterConsoleCommand(TEXT("Dream.ClearOutput"),
		TEXT("Clears Dream's output area; run logs and history are kept"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&ClearOutput), ECVF_Default));
}

void DreamConsoleCommands::Unregister()
{
	using namespace DreamConsoleCommandsLocal;
	for (IConsoleObject* Command : Registered)
	{
		if (Command != nullptr)
		{
			IConsoleManager::Get().UnregisterConsoleObject(Command);
		}
	}
	Registered.Reset();
}
