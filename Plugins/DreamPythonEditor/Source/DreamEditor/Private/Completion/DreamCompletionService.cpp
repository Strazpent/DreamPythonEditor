// SPDX-License-Identifier: Apache-2.0
// Live completion caches and the resolver's idle ticks (TDD §7.5.3, §6.6, D6, D14(5), RV-23). See DreamCompletionService.h.
// Every Python call goes through FDreamPython's typed resolver wrappers from an FDreamDeferral ticker with bNeedsPython
// (INV-6): such a body never runs while a run is active, a pump is on the stack or Python is not Ready, and the deferral
// gate's 5 ms per-frame helper budget covers it. QueryCached reads C++ data only.
#include "Completion/DreamCompletionService.h"

#include "Buffers/DreamBuffer.h"
#include "Completion/DreamCompletionTypes.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"

namespace DreamCompletionPrivate
{
	const FString SessionNamespace(TEXT("session"));
	const FName LiveClient(TEXT("DreamCompletionLive"));

	bool IsPythonUnusable(EDreamPythonState State)
	{
		switch (State)
		{
		case EDreamPythonState::Unavailable:
		case EDreamPythonState::Headless:
		case EDreamPythonState::Disabled:
		case EDreamPythonState::BootFailed:
		case EDreamPythonState::ShuttingDown:
		case EDreamPythonState::ShutDown:
			return true;
		default:
			return false;
		}
	}

	FDreamCandidateListPtr EmptyList()
	{
		static const FDreamCandidateListPtr Empty = MakeShared<FDreamCandidateList>();
		return Empty;
	}
}

FDreamCompletionService::FDreamCompletionService() = default;

FDreamCompletionService::~FDreamCompletionService() = default;

void FDreamCompletionService::Initialize(FDreamRunService& Runs, FDreamPython& Python)
{
	check(IsInGameThread());
	if (bInitialized)
	{
		return;
	}
	RunFinishedHandle = Runs.OnRunFinished().AddRaw(this, &FDreamCompletionService::HandleRunFinished);
	PythonStateHandle = Python.OnStateChanged().AddRaw(this, &FDreamCompletionService::HandlePythonState);
	bInitialized = true;
	if (Python.IsReady())
	{
		HandlePythonState(EDreamPythonState::Ready);   // a late module load finds Python booted already
	}
}

void FDreamCompletionService::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}
	bInitialized = false;
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		Module.GetRunService().OnRunFinished().Remove(RunFinishedHandle);
		Module.GetPython().OnStateChanged().Remove(PythonStateHandle);
	}
	for (FTSTicker::FDelegateHandle* Handle : { &LiveTicker, &PrewarmTicker, &IndexTicker })
	{
		if (Handle->IsValid())
		{
			FDreamDeferral::RemoveTicker(*Handle);
			Handle->Reset();
		}
	}
	ClearAll();
}

// ---- Generations and cache plumbing ---------------------------------------------------------------------------------

void FDreamCompletionService::SyncGenerations()
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	const uint32 Py = Module.GetPython().GetGeneration();
	const uint32 Run = Module.GetRunService().GetRunGeneration();
	const uint32 Epoch = Module.GetPython().GetSessionEpoch();
	if (Py != PyGeneration)
	{
		// A new interpreter: every token (id(type)) and name is stale.
		ClearAll();
		PyGeneration = Py;
		RunGeneration = Run;
		SessionEpoch = Epoch;
		return;
	}
	if (Run != RunGeneration || Epoch != SessionEpoch)
	{
		// A run end, Reset Session or an alias rewrite (U19): cached roots go; member lists of C and unreal types stay
		// (§7.5.3), member lists of user types (which the run may have changed) go.
		ChainCache.Reset();
		for (auto It = MemberCache.CreateIterator(); It; ++It)
		{
			if (!It.Value().bStable)
			{
				It.RemoveCurrent();
			}
		}
		RunGeneration = Run;
		SessionEpoch = Epoch;
	}
}

void FDreamCompletionService::ClearAll()
{
	MemberCache.Reset();
	ChainCache.Reset();
	SessionRoots.Reset();
	SessionRootIndex.Reset();
	bSessionRootsValid = false;
	SessionRootsGeneration = MAX_uint32;
	UnrealTopLevel = MakeShared<FDreamCandidateList>();
	bUnrealIndexComplete = false;
	UnrealCursor = 0;
	UnrealClassTokens.Reset();
	UnrealClassOrder.Reset();
	Pending.Reset();
	bPrewarmActive = false;
	bPrewarmWanted = false;
	PrewarmCursor = 0;
	PrewarmRoots.Reset();
	// BrokenTypes stay: the breaker holds for the rest of the editor session (§7.5.3).
}

void FDreamCompletionService::ResetForTest()
{
	ClearAll();
	PyGeneration = 0;
	SyncGenerations();
}

FString FDreamCompletionService::CacheKey(const FDreamCompletionQuery& Query) const
{
	return Query.Namespace + TEXT("\n") + DreamText::Utf8ToFString(Query.Chain.Key());
}

bool FDreamCompletionService::IsSessionTableFresh() const
{
	if (!bSessionRootsValid || !FDreamEditorModule::IsAvailable())
	{
		return false;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	return SessionRootsGeneration == Module.GetRunService().GetRunGeneration() && PyGeneration == Module.GetPython().GetGeneration()
		&& SessionEpoch == Module.GetPython().GetSessionEpoch();
}

bool FDreamCompletionService::IsUnrealShadowed() const
{
	// A Session name "unreal" that is not the module (rare) keeps the index from answering for it.
	const int32* Index = SessionRootIndex.Find(TEXT("unreal"));
	return Index != nullptr && SessionRoots[*Index].Kind != dream::CompletionKind::Module;
}

FString FDreamCompletionService::RootTypeHint(const FDreamCompletionQuery& Query) const
{
	if (Query.Chain.Steps.empty() || Query.Namespace != DreamCompletionPrivate::SessionNamespace || !bSessionRootsValid)
	{
		return FString();
	}
	const int32* Index = SessionRootIndex.Find(DreamText::Utf8ToFString(Query.Chain.Steps.front().Text));
	if (Index == nullptr)
	{
		return FString();
	}
	const FRootEntry& Root = SessionRoots[*Index];
	return Root.Kind == dream::CompletionKind::Module ? TEXT("module ") + Root.TypeName : Root.TypeName;   // the resolver's root_type
}

bool FDreamCompletionService::LookupMembers(const FString& Token, FDreamCompletionAnswer& Out)
{
	FMemberEntry* Entry = Token.IsEmpty() ? nullptr : MemberCache.Find(Token);
	if (Entry == nullptr || !Entry->Items.IsValid())
	{
		return false;
	}
	Entry->LastUse = ++UseCounter;
	Out.Candidates = Entry->Items;
	if (Out.TypeName.IsEmpty())
	{
		Out.TypeName = Entry->TypeName;
	}
	return true;
}

void FDreamCompletionService::StoreMembers(const FString& Token, const FString& TypeName, bool bStable, FDreamCandidateList&& Items)
{
	if (Token.IsEmpty())
	{
		return;
	}
	FMemberEntry& Entry = MemberCache.FindOrAdd(Token);
	Entry.Items = MakeShared<FDreamCandidateList>(MoveTemp(Items));
	Entry.TypeName = TypeName;
	Entry.bStable = bStable;
	Entry.RunGeneration = RunGeneration;
	Entry.LastUse = ++UseCounter;
	TrimLru();
}

void FDreamCompletionService::StoreChain(const FString& Key, FChainEntry&& Entry)
{
	Entry.LastUse = ++UseCounter;
	ChainCache.Add(Key, MoveTemp(Entry));
	TrimLru();
}

void FDreamCompletionService::TrimLru()
{
	auto Trim = [](auto& Map, int32 Capacity)
	{
		while (Map.Num() > Capacity)
		{
			auto Oldest = Map.CreateIterator();
			FString OldestKey = Oldest.Key();
			uint64 OldestUse = Oldest.Value().LastUse;
			for (auto It = Map.CreateIterator(); It; ++It)
			{
				if (It.Value().LastUse < OldestUse)
				{
					OldestUse = It.Value().LastUse;
					OldestKey = It.Key();
				}
			}
			Map.Remove(OldestKey);
		}
	};
	Trim(MemberCache, MemberCacheCapacity);
	Trim(ChainCache, ChainCacheCapacity);
}

void FDreamCompletionService::RememberUnrealClass(const FString& ClassName, const FString& Token)
{
	UnrealClassOrder.Remove(ClassName);
	UnrealClassOrder.Add(ClassName);
	UnrealClassTokens.Add(ClassName, Token);
	while (UnrealClassOrder.Num() > UnrealClassCapacity)
	{
		UnrealClassTokens.Remove(UnrealClassOrder[0]);
		UnrealClassOrder.RemoveAt(0);
	}
}

FDreamCandidateListPtr FDreamCompletionService::BuildSessionNames() const
{
	TSharedRef<FDreamCandidateList> Names = MakeShared<FDreamCandidateList>();
	Names->reserve(static_cast<size_t>(SessionRoots.Num()) + 1);
	bool bHasUnreal = false;
	for (const FRootEntry& Root : SessionRoots)
	{
		dream::CompletionCandidate Candidate;
		Candidate.Label = DreamText::ToUtf8(Root.Name);
		Candidate.Detail = DreamText::ToUtf8(Root.Kind == dream::CompletionKind::Module ? TEXT("module ") + Root.TypeName : Root.TypeName);
		Candidate.Kind = Root.Kind;
		Candidate.Source = dream::CompletionSource::Live;
		Candidate.Group = dream::ScopeGroup::Scope;
		bHasUnreal = bHasUnreal || Root.Name == TEXT("unreal");
		Names->push_back(MoveTemp(Candidate));
	}
	if (!bHasUnreal && !UnrealTopLevel->empty())
	{
		// `unreal` resolves everywhere (§7.5.3) even when no script imported it into the Session yet.
		dream::CompletionCandidate Unreal;
		Unreal.Label = "unreal";
		Unreal.Detail = "module";
		Unreal.Kind = dream::CompletionKind::Module;
		Unreal.Source = dream::CompletionSource::Live;
		Unreal.Group = dream::ScopeGroup::Scope;
		Names->push_back(MoveTemp(Unreal));
	}
	return Names;
}

// ---- Queries -------------------------------------------------------------------------------------------------------

bool FDreamCompletionService::IsLiveAvailable() const
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return false;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	return Module.GetPython().IsReady() && !Module.GetRunService().IsRunActive();
}

bool FDreamCompletionService::IsLivePaused() const
{
	return FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetRunService().IsRunActive();
}

bool FDreamCompletionService::QueryCached(const FDreamCompletionQuery& Query, FDreamCompletionAnswer& Out)
{
	using namespace DreamCompletionPrivate;
	Out = FDreamCompletionAnswer();
	Out.Candidates = EmptyList();
	if (!IsInGameThread() || !FDreamEditorModule::IsAvailable() || !bInitialized)
	{
		return false;
	}
	Out.bLivePaused = IsLivePaused();
	SyncGenerations();
	const dream::CompletionChain& Chain = Query.Chain;
	if (Chain.Context == dream::ChainContext::None)
	{
		return false;
	}

	// The unreal module's first two levels come from FUnrealNameIndex, live or not (§7.5.3, UEI-08).
	if (Chain.Context == dream::ChainContext::Member && Chain.IsUnrealRooted() && !IsUnrealShadowed())
	{
		if (Chain.Steps.size() == 1 && !UnrealTopLevel->empty())
		{
			Out.Candidates = UnrealTopLevel;
			Out.bIncomplete = !bUnrealIndexComplete;
			Out.TypeName = TEXT("unreal");
			Out.bHit = true;
			return true;
		}
		if (Chain.Steps.size() == 2 && Chain.Steps[1].Kind == dream::ChainStepKind::Attr)
		{
			if (const FString* Token = UnrealClassTokens.Find(DreamText::Utf8ToFString(Chain.Steps[1].Text)))
			{
				if (LookupMembers(*Token, Out))
				{
					Out.bHit = true;
					return true;
				}
			}
		}
	}

	if (FChainEntry* Entry = ChainCache.Find(CacheKey(Query)))
	{
		Entry->LastUse = ++UseCounter;
		Out.bRootBound = Entry->bRootBound;
		Out.TypeName = Entry->TypeName;
		Out.bIncomplete = Entry->bIncomplete;
		if (Entry->Items.IsValid() || Entry->Token.IsEmpty())
		{
			// Name and string-argument answers, an incomplete member answer, or a chain that resolved to nothing (an empty
			// hit, so the next keystroke does not ask again within this generation).
			Out.Candidates = Entry->Items.IsValid() ? Entry->Items : EmptyList();
			Out.bHit = true;
			return true;
		}
		if (LookupMembers(Entry->Token, Out))
		{
			Out.bHit = true;
			return true;
		}
	}

	if (Query.Namespace == SessionNamespace && IsSessionTableFresh())
	{
		if (Chain.Context == dream::ChainContext::Name)
		{
			Out.Candidates = BuildSessionNames();
			Out.bHit = true;
			return true;
		}
		if (Chain.Context == dream::ChainContext::Member && Chain.Steps.size() == 1)
		{
			// The first keystroke after a run: `my_tool.` from the post-run copy (RV-23, T-REPL-013).
			if (const int32* Index = SessionRootIndex.Find(DreamText::Utf8ToFString(Chain.Steps.front().Text)))
			{
				const FRootEntry& Root = SessionRoots[*Index];
				Out.TypeName = Root.TypeName;
				if (LookupMembers(Root.Token, Out))
				{
					Out.bRootBound = true;
					Out.bHit = true;
					return true;
				}
			}
		}
	}
	if (Chain.Context == dream::ChainContext::Name && !UnrealTopLevel->empty() && Query.Namespace != SessionNamespace)
	{
		// Isolated and namespace-less buffers: no Session names, but `unreal` still resolves (§7.5.3).
		FDreamCandidateList Names;
		dream::CompletionCandidate Unreal;
		Unreal.Label = "unreal";
		Unreal.Detail = "module";
		Unreal.Kind = dream::CompletionKind::Module;
		Unreal.Group = dream::ScopeGroup::Scope;
		Names.push_back(MoveTemp(Unreal));
		Out.Candidates = MakeShared<FDreamCandidateList>(MoveTemp(Names));
		Out.bIncomplete = true;     // the namespace's own names need the resolver
		return false;
	}
	return false;
}

uint64 FDreamCompletionService::RequestLive(const FDreamCompletionQuery& Query, FDreamOnCompletionAnswer OnAnswer, EDreamStartReason Reason)
{
	using namespace DreamCompletionPrivate;
	check(IsInGameThread());
	if (!bInitialized || !FDreamEditorModule::IsAvailable() || Query.Chain.Context == dream::ChainContext::None)
	{
		return 0;
	}
	FDreamPython& Python = FDreamEditorModule::Get().GetPython();
	const EDreamPythonState State = Python.GetState();
	if (IsPythonUnusable(State))
	{
		return 0;
	}
	// §3.7: live completion is Refuse while a run is active or a pump is on the stack (D6). Python's own state does not
	// refuse here: a miss in Cold boots it, and the ticker's gate waits for Ready.
	if (!FDreamDeferral::CanRunNow(LiveClient, EDreamDeferPolicy::RefuseWhileExecuting, /*bNeedsPython*/ false))
	{
		return 0;
	}
	if (State == EDreamPythonState::Cold)
	{
		Python.RequestStart(Reason);    // RT-17: a completion miss boots Python; the next keystroke gets results
	}
	const FString Key = CacheKey(Query);
	FPendingRequest* Request = Pending.FindByPredicate([&Key](const FPendingRequest& Item) { return Item.Key == Key; });
	if (Request == nullptr)
	{
		Request = &Pending.AddDefaulted_GetRef();
		Request->Query = Query;
		Request->Key = Key;
	}
	Request->QueuedRunGeneration = FDreamEditorModule::Get().GetRunService().GetRunGeneration();
	const uint64 Ticket = NextTicket++;
	if (OnAnswer.IsBound())
	{
		Request->Callbacks.Emplace(Ticket, MoveTemp(OnAnswer));
		Request->bSpeculative = false;
	}
	EnsureLiveTicker();
	return Ticket;
}

void FDreamCompletionService::CancelLive(uint64 Ticket)
{
	if (Ticket == 0)
	{
		return;
	}
	for (FPendingRequest& Request : Pending)
	{
		Request.Callbacks.RemoveAll([Ticket](const TPair<uint64, FDreamOnCompletionAnswer>& Entry) { return Entry.Key == Ticket; });
	}
}

void FDreamCompletionService::Prefetch(const FDreamCompletionQuery& Query)
{
	FDreamCompletionAnswer Known;
	if (Query.Chain.Context != dream::ChainContext::Member || QueryCached(Query, Known))
	{
		return;
	}
	RequestLive(Query, FDreamOnCompletionAnswer(), EDreamStartReason::Keystroke);
}

FString FDreamCompletionService::NamespaceFor(const FDreamBuffer& Buffer)
{
	if (Buffer.GetKind() == EDreamBufferKind::ReplInput || Buffer.GetViewState().Namespace == EDreamNamespaceMode::Session)
	{
		return DreamCompletionPrivate::SessionNamespace;
	}
	if (FDreamEditorModule::IsAvailable())
	{
		// The last-run namespace is kept from the end of an Isolated run until the next run starts (§5.3.3, D3).
		const TConstArrayView<FDreamRunRecord> History = FDreamEditorModule::Get().GetRunService().GetHistory();
		for (int32 Index = History.Num() - 1; Index >= 0; --Index)
		{
			const FDreamRunRecord& Record = History[Index];
			if (Record.Outcome == EDreamRunOutcome::NotExecuted)
			{
				continue;   // never entered: the namespace kept is still the one before it
			}
			if (Record.Namespace == EDreamNamespaceMode::Isolated && Record.BufferId == Buffer.GetId())
			{
				return TEXT("isolated:") + Buffer.GetIdHex();
			}
			break;
		}
	}
	return TEXT("none");
}

dream::BufferKind FDreamCompletionService::BufferKindFor(const FDreamBuffer& Buffer)
{
	switch (Buffer.GetKind())
	{
	case EDreamBufferKind::ReplInput:
		return dream::BufferKind::Repl;
	case EDreamBufferKind::Scratch:
		return dream::BufferKind::Scratch;
	default:
		return dream::BufferKind::File;
	}
}

// ---- The live ticker ----------------------------------------------------------------------------------------------

void FDreamCompletionService::EnsureLiveTicker()
{
	if (LiveTicker.IsValid())
	{
		return;
	}
	// INV-6: the resolver runs only here, at an idle tick with Python Ready (the gate never runs the body while a run is
	// active or a pump is on the stack, and it stops at 5 ms of helper time per frame, §3.7).
	LiveTicker = FDreamDeferral::AddTicker(DreamCompletionPrivate::LiveClient, EDreamDeferPolicy::RefuseWhileExecuting, 0.f,
		/*bNeedsPython*/ true, [this](float)
		{
			const bool bKeep = TickLive();
			if (!bKeep)
			{
				LiveTicker.Reset();
			}
			return bKeep;
		});
}

bool FDreamCompletionService::TickLive()
{
	if (!bInitialized || Pending.IsEmpty())
	{
		return false;
	}
	// Refuse (§3.7): requests queued before a run (or a Reset Session) ended are not served with the Session after it.
	const uint32 Generation = FDreamEditorModule::Get().GetRunService().GetRunGeneration();
	for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
	{
		if (Pending[Index].QueuedRunGeneration != Generation)
		{
			FPendingRequest Stale = MoveTemp(Pending[Index]);
			Pending.RemoveAt(Index);
			FDreamCompletionAnswer Refused;
			Refused.Candidates = DreamCompletionPrivate::EmptyList();
			Refused.bIncomplete = true;     // the next keystroke asks again
			for (TPair<uint64, FDreamOnCompletionAnswer>& Callback : Stale.Callbacks)
			{
				Callback.Value.ExecuteIfBound(Refused);
			}
		}
	}
	if (Pending.IsEmpty())
	{
		return false;
	}
	// The newest interactive request first (the keystroke the user is waiting on), speculative ones after.
	int32 Pick = INDEX_NONE;
	for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
	{
		if (!Pending[Index].bSpeculative)
		{
			Pick = Index;
			break;
		}
	}
	if (Pick == INDEX_NONE)
	{
		Pick = 0;
	}
	FPendingRequest Request = MoveTemp(Pending[Pick]);
	Pending.RemoveAt(Pick);
	ServeRequest(Request);
	return bInitialized && !Pending.IsEmpty();
}

void FDreamCompletionService::ServeRequest(FPendingRequest& Request)
{
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	SyncGenerations();
	const FDreamCompletionQuery& Query = Request.Query;

	FDreamCompletionAnswer Answer;
	if (QueryCached(Query, Answer) && !Answer.bIncomplete)
	{
		// Another request (a prewarm slice, the same chain from another view) answered it meanwhile.
		for (TPair<uint64, FDreamOnCompletionAnswer>& Callback : Request.Callbacks)
		{
			Callback.Value.ExecuteIfBound(Answer);
		}
		return;
	}

	const FString Hint = RootTypeHint(Query);
	if (!Hint.IsEmpty() && BrokenTypes.Contains(Hint))
	{
		Answer.bLiveOff = true;
		for (TPair<uint64, FDreamOnCompletionAnswer>& Callback : Request.Callbacks)
		{
			Callback.Value.ExecuteIfBound(Answer);
		}
		return;
	}

	FDreamResolverRequest Resolver;
	Resolver.Chain = Query.Chain;
	Resolver.Namespace = Query.Namespace;
	Resolver.BudgetNs = LiveBudgetNs;
	Resolver.Generation = RunGeneration;
	Resolver.Epoch = SessionEpoch;
	const dream::CompletionChain& Chain = Query.Chain;
	const bool bUnrealClass = Chain.Context == dream::ChainContext::Member && Chain.IsUnrealRooted() && Chain.Steps.size() == 2
		&& Chain.Steps[1].Kind == dream::ChainStepKind::Attr;
	if (bUnrealClass)
	{
		if (const FString* Token = UnrealClassTokens.Find(DreamText::Utf8ToFString(Chain.Steps[1].Text)))
		{
			if (MemberCache.Contains(*Token))
			{
				Resolver.KnownTokens.Add(*Token);
			}
		}
	}
	else if (Chain.Context == dream::ChainContext::Member && Chain.Steps.size() == 1 && bSessionRootsValid)
	{
		if (const int32* Index = SessionRootIndex.Find(DreamText::Utf8ToFString(Chain.Steps.front().Text)))
		{
			if (MemberCache.Contains(SessionRoots[*Index].Token))
			{
				Resolver.KnownTokens.Add(SessionRoots[*Index].Token);
			}
		}
	}

	FDreamResolverResult Result;
	const bool bCalled = Module.GetPython().ResolverComplete(Resolver, Result);
	if (!Result.Breaker.IsEmpty())
	{
		NoteBreaker(Result.Breaker, Result.CallSeconds, true);
	}
	else if (bCalled && Result.CallSeconds > BreakerSeconds && !Result.RootType.IsEmpty())
	{
		NoteBreaker(Result.RootType, Result.CallSeconds, false);
	}
	if (!bCalled || (!Result.Error.IsEmpty() && Result.Error != TEXT("breaker")))
	{
		if (bCalled)
		{
			UE_LOG(LogDream, Verbose, TEXT("Live completion: the resolver failed: %s"), *Result.Error);
		}
		FDreamCompletionAnswer Empty;
		Empty.Candidates = DreamCompletionPrivate::EmptyList();
		Empty.bLiveOff = !bCalled;
		for (TPair<uint64, FDreamOnCompletionAnswer>& Callback : Request.Callbacks)
		{
			Callback.Value.ExecuteIfBound(Empty);
		}
		return;
	}

	FChainEntry Entry;
	Entry.Token = Result.Token;
	Entry.TypeName = Result.TypeName;
	Entry.RootType = Result.RootType;
	Entry.bRootBound = Result.bRootBound;
	Entry.bIncomplete = Result.bIncomplete;
	if (Chain.Context == dream::ChainContext::Member && !Result.Token.IsEmpty() && !Result.bIncomplete)
	{
		if (!Result.bMembersCached)
		{
			StoreMembers(Result.Token, Result.TypeName, Result.bStable, MoveTemp(Result.Items));
		}
		if (bUnrealClass)
		{
			RememberUnrealClass(DreamText::Utf8ToFString(Chain.Steps[1].Text), Result.Token);
		}
	}
	else
	{
		Entry.Items = MakeShared<FDreamCandidateList>(MoveTemp(Result.Items));
	}
	StoreChain(CacheKey(Query), MoveTemp(Entry));

	QueryCached(Query, Answer);
	for (TPair<uint64, FDreamOnCompletionAnswer>& Callback : Request.Callbacks)
	{
		Callback.Value.ExecuteIfBound(Answer);
	}
}

void FDreamCompletionService::NoteBreaker(const FString& RootType, double Seconds, bool bReportedByResolver)
{
	if (RootType.IsEmpty() || BrokenTypes.Contains(RootType))
	{
		return;
	}
	BrokenTypes.Add(RootType);
	// §7.5.3: one warning per type; the type falls back to static results (§7.11 "Resolver too slow").
	UE_LOG(LogDream, Warning, TEXT("Live completion is off for %s values for the rest of this session: a resolver call took %.1f ms (over %.0f ms)%s."),
		*RootType, Seconds * 1000.0, BreakerSeconds * 1000.0, bReportedByResolver ? TEXT("") : TEXT(" in the editor"));
}

// ---- Post-run prewarm (RV-23) and the unreal index ----------------------------------------------------------------

void FDreamCompletionService::StartPrewarm()
{
	if (!bInitialized)
	{
		return;
	}
	if (bPrewarmActive)
	{
		bPrewarmWanted = true;      // restart from the top once the running pass ends (the Session changed again)
		return;
	}
	bPrewarmActive = true;
	PrewarmCursor = 0;
	PrewarmRoots.Reset();
	if (!PrewarmTicker.IsValid())
	{
		PrewarmTicker = FDreamDeferral::AddTicker(TEXT("DreamCompletionPrewarm"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
			/*bNeedsPython*/ true, [this](float)
			{
				const bool bKeep = TickPrewarm();
				if (!bKeep)
				{
					PrewarmTicker.Reset();
				}
				return bKeep;
			});
	}
}

bool FDreamCompletionService::TickPrewarm()
{
	if (!bInitialized || !bPrewarmActive)
	{
		bPrewarmActive = false;
		return false;
	}
	SyncGenerations();
	if (PrewarmCursor == 0)
	{
		PrewarmRoots.Reset();
		PrewarmGeneration = RunGeneration;
	}
	else if (PrewarmGeneration != RunGeneration)
	{
		PrewarmCursor = 0;          // the Session changed under the pass
		PrewarmRoots.Reset();
		PrewarmGeneration = RunGeneration;
	}

	FDreamResolverPrewarmRequest Request;
	Request.Namespace = DreamCompletionPrivate::SessionNamespace;
	Request.Cursor = PrewarmCursor;
	Request.BudgetNs = SliceBudgetNs;
	Request.Generation = RunGeneration;
	Request.Epoch = SessionEpoch;
	for (const TPair<FString, FMemberEntry>& Pair : MemberCache)
	{
		Request.KnownTokens.Add(Pair.Key);      // types already cached are skipped (RV-23)
	}
	FDreamResolverPrewarmResult Result;
	if (!FDreamEditorModule::Get().GetPython().ResolverPrewarm(Request, Result) || !Result.Error.IsEmpty())
	{
		UE_LOG(LogDream, Verbose, TEXT("Live completion: the post-run Session copy failed: %s"), *Result.Error);
		bPrewarmActive = false;
		bPrewarmWanted = false;
		return false;
	}
	for (FDreamResolverRoot& Root : Result.Roots)
	{
		FRootEntry& Entry = PrewarmRoots.AddDefaulted_GetRef();
		Entry.Name = MoveTemp(Root.Name);
		Entry.Kind = Root.Kind;
		Entry.Token = MoveTemp(Root.Token);
		Entry.TypeName = MoveTemp(Root.TypeName);
	}
	for (FDreamResolverMembers& Listing : Result.Members)
	{
		StoreMembers(Listing.Token, Listing.TypeName, Listing.bStable, MoveTemp(Listing.Items));
	}
	if (Result.Cursor >= 0)
	{
		PrewarmCursor = Result.Cursor;
		return true;
	}

	// The pass is done: the root table belongs to the generation it started in. A namespace over 10,000 names exports no
	// roots, and then only the per-type caches answer (§7.5.3).
	SessionRoots = MoveTemp(PrewarmRoots);
	PrewarmRoots.Reset();
	SessionRootIndex.Reset();
	for (int32 Index = 0; Index < SessionRoots.Num(); ++Index)
	{
		SessionRootIndex.Add(SessionRoots[Index].Name, Index);
	}
	bSessionRootsValid = Result.Count <= 10000;
	SessionRootsGeneration = PrewarmGeneration;
	bPrewarmActive = false;
	if (bPrewarmWanted)
	{
		bPrewarmWanted = false;
		bPrewarmActive = true;
		PrewarmCursor = 0;
		return true;
	}
	return false;
}

void FDreamCompletionService::StartUnrealIndex()
{
	if (!bInitialized || bUnrealIndexComplete || IndexTicker.IsValid())
	{
		return;
	}
	UnrealCursor = 0;
	UnrealTopLevel = MakeShared<FDreamCandidateList>();
	IndexTicker = FDreamDeferral::AddTicker(TEXT("DreamCompletionIndex"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
		/*bNeedsPython*/ true, [this](float)
		{
			const bool bKeep = TickUnrealIndex();
			if (!bKeep)
			{
				IndexTicker.Reset();
			}
			return bKeep;
		});
}

bool FDreamCompletionService::TickUnrealIndex()
{
	if (!bInitialized)
	{
		return false;
	}
	SyncGenerations();
	FDreamResolverNamesResult Result;
	if (!FDreamEditorModule::Get().GetPython().ResolverUnrealNames(UnrealCursor, SliceBudgetNs, Result) || !Result.Error.IsEmpty())
	{
		UE_LOG(LogDream, Verbose, TEXT("Live completion: the unreal name index failed: %s"), *Result.Error);
		UnrealCursor = 0;
		return false;
	}
	UnrealTopLevel->insert(UnrealTopLevel->end(), std::make_move_iterator(Result.Items.begin()), std::make_move_iterator(Result.Items.end()));
	if (Result.Cursor >= 0)
	{
		UnrealCursor = Result.Cursor;
		return true;
	}
	// No unreal module yet (nothing imported it): retried after the next run.
	bUnrealIndexComplete = Result.Count > 0;
	UnrealCursor = 0;
	return false;
}

void FDreamCompletionService::HandleRunFinished(const FDreamRunRecord& Record)
{
	(void)Record;
	SyncGenerations();
	StartPrewarm();                 // the idle slices after each run (RV-23)
	StartUnrealIndex();             // a no-op once complete
}

void FDreamCompletionService::HandlePythonState(EDreamPythonState State)
{
	if (State == EDreamPythonState::Ready)
	{
		SyncGenerations();
		StartUnrealIndex();         // once per Python generation, after the deferred start (§6.6 step 5)
		StartPrewarm();
	}
	else if (State == EDreamPythonState::ShuttingDown || State == EDreamPythonState::ShutDown)
	{
		ClearAll();                 // §7.11 "Python disabled or shut down": no runtime items; tokens are stale
	}
}
