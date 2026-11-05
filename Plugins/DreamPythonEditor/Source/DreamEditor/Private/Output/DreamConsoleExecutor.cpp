// SPDX-License-Identifier: Apache-2.0
// "Python (Dream)" in the Output Log console (TDD §6.6, D19, UEI-08). See DreamConsoleExecutor.h.
// Exec runs inside SConsoleInputBox::OnTextCommitted, a Slate input handler (SOutputLog.cpp:433-470): it only submits,
// and the run starts later from OnEndFrame (D14, RT-01). GetSuggestedCompletions never calls Python (RV-23): it reads
// FDreamCompletionService's caches and queues misses for the resolver's idle tick.
#include "Output/DreamConsoleExecutor.h"

#include "Completion/DreamCompletionService.h"
#include "Features/IModularFeatures.h"
#include "Framework/Commands/InputChord.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Output/DreamLogForwarder.h"
#include "Output/DreamReplHistory.h"
#include "Python/DreamPython.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamProjectSettings.h"
#include "Toolkits/GlobalEditorCommonCommands.h"
#include "dream/CompletionMerge.h"

#include <algorithm>

#define LOCTEXT_NAMESPACE "Dream"

FName FDreamConsoleExecutor::StaticName()
{
	static const FName Name(TEXT("DreamEditor"));
	return Name;
}

void FDreamConsoleExecutor::Register()
{
	const UDreamProjectSettings* Settings = UDreamProjectSettings::Get();
	if (bRegistered || (Settings != nullptr && !Settings->bRegisterConsoleExecutor))
	{
		return;                       // UEI-08 "disabled per project"; Dream never writes PreferredCommandExecutor
	}
	IModularFeatures::Get().RegisterModularFeature(IConsoleCommandExecutor::ModularFeatureName(), this);
	bRegistered = true;
}

void FDreamConsoleExecutor::Unregister()
{
	if (!bRegistered)
	{
		return;
	}
	IModularFeatures::Get().UnregisterModularFeature(IConsoleCommandExecutor::ModularFeatureName(), this);
	bRegistered = false;
}

FName FDreamConsoleExecutor::GetName() const
{
	return StaticName();
}

FText FDreamConsoleExecutor::GetDisplayName() const
{
	return LOCTEXT("Out.ConsoleExecutorName", "Python (Dream)");
}

FText FDreamConsoleExecutor::GetDescription() const
{
	return LOCTEXT("Out.ConsoleExecutorDescription", "Run Python in the Dream session, with Stop and one undo step per run");
}

FText FDreamConsoleExecutor::GetHintText() const
{
	return LOCTEXT("Out.ConsoleExecutorHint", "Enter Python (Dream session)");
}

namespace DreamConsoleExecutorLocal
{
	constexpr int32 MaxSuggestions = 100;      // §6.6 step 3
	constexpr int32 SortedPrefix = 4 * MaxSuggestions;   // ranked fully; room for labels repeated by the keyword table

	struct FScored
	{
		int32 Score = 0;
		int32 Demotion = 0;
		const dream::CompletionCandidate* Candidate = nullptr;
	};

	/** '_' names sink and dunders come last unless the partial name starts with '_' (§7.6.1). */
	int32 Demotion(const std::string& Label, const std::string& Prefix)
	{
		if (!Prefix.empty() && Prefix.front() == '_')
		{
			return 0;
		}
		const bool bDunder = Label.size() > 4 && Label.compare(0, 2, "__") == 0 && Label.compare(Label.size() - 2, 2, "__") == 0;
		return bDunder ? 2 : (!Label.empty() && Label.front() == '_') ? 1 : 0;
	}
}

void FDreamConsoleExecutor::GetSuggestedCompletions(const TCHAR* Input, TArray<FConsoleSuggestion>& Out)
{
	using namespace DreamConsoleExecutorLocal;
	if (!FDreamEditorModule::IsAvailable() || Input == nullptr || *Input == TEXT('\0'))
	{
		return;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	FDreamPython& Python = Module.GetPython();

	// (1) The dotted chain before the end of Input: base and partial name (its last line; AllowMultiLine is true).
	const FString Text(Input);
	const int32 LastBreak = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const int32 LineStart = LastBreak == INDEX_NONE ? 0 : LastBreak + 1;
	const FStringView Line = FStringView(Text).Mid(LineStart);
	FDreamCompletionQuery Query;
	Query.Chain = dream::ExtractCompletionChain(DreamText::ToU16(Line), Line.Len());
	Query.Kind = dream::BufferKind::ConsoleExecutor;
	Query.Namespace = TEXT("session");
	if (Query.Chain.Context == dream::ChainContext::None || Query.Chain.Context == dream::ChainContext::StringArg
		|| (Query.Chain.Context == dream::ChainContext::Name && Query.Chain.Prefix.empty()))
	{
		if (Python.GetState() == EDreamPythonState::Cold)
		{
			Python.RequestStart(EDreamStartReason::ConsoleCompletion);     // the first keystroke still boots (T-LIFE-003)
		}
		return;
	}

	// (2) C++ caches only. A miss (or an incomplete answer) goes to the resolver's next idle tick; Cold boots Python.
	FDreamCompletionService& Completion = Module.GetCompletion();
	FDreamCompletionAnswer Answer;
	const bool bHit = Completion.QueryCached(Query, Answer);
	if (!bHit || Answer.bIncomplete)
	{
		Completion.RequestLive(Query, FDreamOnCompletionAnswer(), EDreamStartReason::ConsoleCompletion);
	}

	// (3) Filter by the partial name with the §7.5.5 matcher; names also get the keyword table (builtins, keywords).
	const std::string& Prefix = Query.Chain.Prefix;
	TArray<FScored> Scored;
	auto Consider = [&Scored, &Prefix](const dream::CompletionCandidate& Candidate)
	{
		int32 Score = 0;
		if (dream::FuzzyScore(Prefix, Candidate.GetFilterText(), Score))
		{
			Scored.Add(FScored{ Score, Demotion(Candidate.Label, Prefix), &Candidate });
			return true;
		}
		return false;
	};
	if (Answer.Candidates.IsValid())
	{
		// A longer partial name matches a subset of what the shorter one matched (the matcher needs the query's characters
		// in order), so the previous matches are all that need scoring again while the list and the chain are the same.
		const std::vector<dream::CompletionCandidate>& List = *Answer.Candidates;
		const std::string Key = Query.Chain.Key();
		const bool bNarrow = NarrowList == &List && NarrowKey == Key && Prefix.size() >= NarrowPrefix.size()
			&& Prefix.compare(0, NarrowPrefix.size(), NarrowPrefix) == 0 && !Answer.bIncomplete;
		TArray<int32> Matches;
		if (bNarrow)
		{
			for (const int32 Index : NarrowMatches)
			{
				if (Index < static_cast<int32>(List.size()) && Consider(List[static_cast<size_t>(Index)]))
				{
					Matches.Add(Index);
				}
			}
		}
		else
		{
			for (int32 Index = 0; Index < static_cast<int32>(List.size()); ++Index)
			{
				if (Consider(List[static_cast<size_t>(Index)]))
				{
					Matches.Add(Index);
				}
			}
		}
		NarrowList = Answer.bIncomplete ? nullptr : &List;   // a list still filling may gain names: no narrowing from it
		NarrowListPin = Answer.Candidates;      // keeps the address valid while it is remembered
		NarrowKey = Key;
		NarrowPrefix = Prefix;
		NarrowMatches = MoveTemp(Matches);
	}
	static const std::vector<dream::CompletionCandidate> Keywords = dream::KeywordCandidates();
	if (Query.Chain.Context == dream::ChainContext::Name)
	{
		for (const dream::CompletionCandidate& Candidate : Keywords)
		{
			Consider(Candidate);
		}
	}
	auto Ranks = [](const FScored& A, const FScored& B)
	{
		if (A.Demotion != B.Demotion)
		{
			return A.Demotion < B.Demotion;
		}
		if (A.Score != B.Score)
		{
			return A.Score > B.Score;
		}
		return A.Candidate->Label < B.Candidate->Label;
	};
	// Only the best ones are shown (at most 100, after dropping repeated labels), so only those are put in order: the
	// first keystrokes after "unreal." match thousands of names.
	FScored* Begin = Scored.GetData();
	FScored* End = Begin + Scored.Num();
	FScored* Ordered = Begin + FMath::Min(Scored.Num(), SortedPrefix);
	if (Ordered < End)
	{
		std::nth_element(Begin, Ordered, End, Ranks);
	}
	std::sort(Begin, Ordered, Ranks);

	// Name is the whole line, because choosing a suggestion replaces the box text with it (SOutputLog.cpp:676).
	const FString Head = Text.Left(LineStart + Query.Chain.PrefixStart);
	TSet<FString> Seen;         // labels already suggested: a Session name shadows the builtin of the same name
	const dream::CompletionCandidate* First = Scored.Num() > 0 ? Scored[0].Candidate : nullptr;
	const dream::CompletionCandidate* Exact = nullptr;
	bool bSeveral = false;      // more than one distinct label matched
	for (const FScored& Item : Scored)
	{
		bSeveral = bSeveral || Item.Candidate->Label != First->Label;
		Exact = (Exact == nullptr && Item.Candidate->Label == Prefix) ? Item.Candidate : Exact;
		if (Out.Num() >= MaxSuggestions)
		{
			if (bSeveral && Exact != nullptr)
			{
				break;          // nothing more to learn from the rest
			}
			continue;
		}
		const FString Label = DreamText::Utf8ToFString(Item.Candidate->Label);
		if (Seen.Contains(Label))
		{
			continue;
		}
		Seen.Add(Label);
		Out.Add(FConsoleSuggestion(Head + DreamText::Utf8ToFString(Item.Candidate->GetInsertText()),
			DreamText::Utf8ToFString(Item.Candidate->Detail)));
	}

	// (5) A single class or module match, or one typed in full: its members are requested speculatively, so the cache
	// answers when '.' is typed (RV-23).
	const dream::CompletionCandidate* Single = Exact != nullptr ? Exact : (!bSeveral ? First : nullptr);
	if (Single != nullptr && (Single->Kind == dream::CompletionKind::Class || Single->Kind == dream::CompletionKind::Module))
	{
		FDreamCompletionQuery Next;
		Next.Kind = Query.Kind;
		Next.Namespace = Query.Namespace;
		Next.Chain.Context = dream::ChainContext::Member;
		if (Query.Chain.Context == dream::ChainContext::Member)
		{
			Next.Chain.Steps = Query.Chain.Steps;
		}
		dream::ChainStep Step;
		Step.Kind = Next.Chain.Steps.empty() ? dream::ChainStepKind::Name : dream::ChainStepKind::Attr;
		Step.Text = Single->Label;
		Next.Chain.Steps.push_back(MoveTemp(Step));
		Completion.Prefetch(Next);
	}
}

void FDreamConsoleExecutor::GetExecHistory(TArray<FString>& Out)
{
	// 64 matches the stock behaviour; Dream's own history, never ConsoleHistory.ini (T-REPL-014).
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetReplHistory().GetLatest(64, Out);
	}
}

bool FDreamConsoleExecutor::Exec(const TCHAR* Input)
{
	if (!FDreamEditorModule::IsAvailable() || Input == nullptr)
	{
		return true;
	}
	const FString Code(Input);
	if (Code.TrimStartAndEnd().IsEmpty())
	{
		return true;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	FDreamRunService& RunService = Module.GetRunService();

	// (1) Refused while a run or task is active, or Python is unavailable (§6.6, §6.7 "Executor Exec during a task").
	FText Reason;
	if (!RunService.CanSubmit(EDreamRunRoute::ConsoleExecutor, Reason))
	{
		UE_LOG(LogDream, Warning, TEXT("Python (Dream): %s"), *Reason.ToString());
		return true;
	}

	// (2) History with source console.
	Module.GetReplHistory().Add(Code, EDreamHistorySource::Console);

	// (3) The input once in the Output Log, as the stock executors do (PythonScriptPlugin.cpp:365); GDreamForwarding keeps
	// FDreamOutputDevice from capturing it again (T-OUT-021).
	FDreamForwardLine Echo;
	Echo.Category = FDreamLogForwarder::LogPythonName();
	Echo.Verbosity = ELogVerbosity::Log;
	Echo.Text = Code;
	FDreamLogForwarder::EchoNow(Echo);

	// (4) A numbered run: Session, auto-print, transaction from the route default (RT-08), <dream:Console#N> set by Submit.
	FDreamRunRequest Request;
	Request.Route = EDreamRunRoute::ConsoleExecutor;
	Request.TabTitle = TEXT("Console");
	Request.Text = Code;
	Request.FirstLine = 1;
	Request.Namespace = EDreamNamespaceMode::Session;
	const FDreamSubmitResult Result = RunService.Submit(MoveTemp(Request));
	if (!Result.bAccepted)
	{
		UE_LOG(LogDream, Warning, TEXT("Python (Dream): %s"), *Result.Reason.ToString());
	}
	// (5)
	return true;
}

FInputChord FDreamConsoleExecutor::GetHotKey() const
{
	return FGlobalEditorCommonCommands::Get().OpenConsoleCommandBox->GetActiveChord(EMultipleKeyBindingIndex::Primary).Get();
}

FInputChord FDreamConsoleExecutor::GetIterateExecutorHotKey() const
{
	return FGlobalEditorCommonCommands::Get().SelectNextConsoleExecutor->GetActiveChord(EMultipleKeyBindingIndex::Primary).Get();
}

#undef LOCTEXT_NAMESPACE
