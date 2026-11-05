// SPDX-License-Identifier: Apache-2.0
// "Python (Dream)" in the Output Log console (TDD §6.6, D19, UEI-08). Registered as an IConsoleCommandExecutor modular
// feature from FDreamPython::OnInitialized, unregistered at OnShutdown, re-registered after a re-enable (D16); never the
// preferred executor. Exec only submits (it runs in a Slate input handler, D14). Game thread. Body: output slice.
// Signatures match IConsoleCommandExecutor (Engine/Source/Runtime/Core/Public/HAL/IConsoleManager.h:894-963).
#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

#include <string>
#include <vector>

namespace dream { struct CompletionCandidate; }

class DREAMEDITOR_API FDreamConsoleExecutor final : public IConsoleCommandExecutor
{
public:
	static FName StaticName();                                  // "DreamEditor"

	/** Registers when UDreamProjectSettings::bRegisterConsoleExecutor is true; idempotent. */
	void Register();
	void Unregister();
	bool IsRegistered() const { return bRegistered; }

	virtual FName GetName() const override;                     // "DreamEditor"
	virtual FText GetDisplayName() const override;              // "Python (Dream)"
	virtual FText GetDescription() const override;              // "Run Python in the Dream session, with Stop and one undo step per run"
	virtual FText GetHintText() const override;                 // "Enter Python (Dream session)"
	/**
	 * §6.6 completion steps 1-5 (UEI-08, D6, RV-23). Called from SConsoleInputBox::OnTextChanged, a Slate handler, so it
	 * never calls Python (D14(5)): the chain before the end of Input (dream::ExtractCompletionChain) is answered from
	 * FDreamCompletionService's C++ caches (FUnrealNameIndex, the post-run Session copy, the member LRU). A miss queues the
	 * chain for the resolver's next idle tick (and boots Python in state Cold, RT-17), so the next keystroke has results.
	 * Each suggestion's Name is the whole input up to the partial name plus the item (the box replaces its text with it),
	 * Help the item's detail; at most 100, best fuzzy scores first (the box sorts again, §6.6 step 4). A single class or
	 * module match has its members prefetched (step 5).
	 */
	virtual void GetSuggestedCompletions(const TCHAR* Input, TArray<FConsoleSuggestion>& Out) override;
	/** FDreamReplHistory::GetLatest(64), oldest first. */
	virtual void GetExecHistory(TArray<FString>& Out) override;
	/** §6.6 steps 1-5: CanSubmit (else LogDream warning), history (source console), EchoNow to LogPython, Submit route
	 *  ConsoleExecutor (Session, auto-print, transaction on, <dream:Console#N>), return true. */
	virtual bool Exec(const TCHAR* Input) override;
	virtual bool AllowHotKeyClose() const override { return true; }
	virtual bool AllowMultiLine() const override { return true; }
	/** The active chords of FGlobalEditorCommonCommands::OpenConsoleCommandBox / SelectNextConsoleExecutor, as stock. */
	virtual FInputChord GetHotKey() const override;
	virtual FInputChord GetIterateExecutorHotKey() const override;

private:
	bool bRegistered = false;

	/** (completion choice) Typing narrows: when the box asks again for the same chain with a longer partial name over the
	 *  same shared candidate list, only the previous matches are scored again (the unreal top level has ~11,000 names;
	 *  §6.6's hit budget is 1 ms). Game thread. */
	const std::vector<dream::CompletionCandidate>* NarrowList = nullptr;
	TSharedPtr<const std::vector<dream::CompletionCandidate>> NarrowListPin;
	std::string NarrowKey;
	std::string NarrowPrefix;
	TArray<int32> NarrowMatches;
};
