// SPDX-License-Identifier: Apache-2.0
// The C++ side of live completion (TDD §7.5.3, §6.6 steps 1-5, D6, RV-23, UEI-08): FUnrealNameIndex, FLiveMemberCache,
// the post-run Session root table, the chain-to-token cache, the live request queue served by the resolver on idle ticks,
// the post-run prewarm, the speculative prefetch and the 20 ms circuit breaker. Game thread.
//
// Two entry points:
//  - QueryCached: any game-thread context, Slate handlers included (the console box's OnTextChanged, a code view's key
//    handler). Answers from C++ data only, never calls Python (D14(5), RV-23), within about 1 ms on a hit.
//  - RequestLive: queues the chain for the resolver. An FDreamDeferral ticker that needs Python serves one request per idle
//    tick (never while a run is active, D6), fills the caches and calls the requester back on the game thread.
// Owned by FDreamEditorModule (GetCompletion). Completion group.
// (completion choice) M1 has no FDreamLanguageServices dependency: this service is the live half of §7.3.7's
// BeginCompletion / QueryLiveCompletionCached, which the language services can route to when the static source lands
// (M2). FUnrealNameIndex is filled from the resolver's unreal_names helper on idle slices after the boot.
// Deferred (M2): loading FUnrealNameIndex from the Stage 1 snapshot on a background task instead (§7.5.3).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Python/DreamPython.h"
#include "dream/CompletionMerge.h"

#include <string>
#include <vector>

class FDreamBuffer;
struct FDreamResolverResult;

/** Candidate lists are shared, not copied: a member list of an unreal class has hundreds of entries and the console box
 *  asks on every keystroke. Game thread only. */
using FDreamCandidateList = std::vector<dream::CompletionCandidate>;
using FDreamCandidateListPtr = TSharedPtr<const FDreamCandidateList>;

/** One completion request: the chain at the caret, the buffer kind and the namespace roots (D8). */
struct FDreamCompletionQuery
{
	dream::CompletionChain Chain;
	dream::BufferKind Kind = dream::BufferKind::Repl;
	/** "session", "isolated:<buffer id hex>" or "none" (FDreamCompletionService::NamespaceFor). */
	FString Namespace = TEXT("session");
};

/** What the caches or the resolver know for a query. Candidates are unfiltered: the caller merges them with its prefix. */
struct FDreamCompletionAnswer
{
	bool bHit = false;              // answered from C++ data
	bool bIncomplete = false;       // the resolver said incomplete, or the index is still filling: re-request next keystroke
	bool bRootBound = false;        // the chain's root is bound in the namespace (file buffers use the REPL order, D6)
	bool bLivePaused = false;       // a run is active: "Runtime results paused while code runs" (§7.5.3)
	bool bLiveOff = false;          // the circuit breaker or the Python state rules live resolution out for this chain
	FString TypeName;               // the receiver's runtime type ("StaticMeshActor"), for the popup footer
	/** Unfiltered; never null after QueryCached or an OnAnswer call (an empty list when nothing is known). */
	FDreamCandidateListPtr Candidates;

	int32 Num() const { return Candidates.IsValid() ? static_cast<int32>(Candidates->size()) : 0; }
};

DECLARE_DELEGATE_OneParam(FDreamOnCompletionAnswer, const FDreamCompletionAnswer& /*Answer*/);

class DREAMEDITOR_API FDreamCompletionService
{
public:
	static constexpr double BreakerSeconds = 0.020;         // §7.5.3: 4x the 5 ms budget (proposed)
	static constexpr int64 LiveBudgetNs = 5'000'000;         // D6
	static constexpr int64 SliceBudgetNs = 2'000'000;        // prewarm and index slices (§7.5.3, proposed)
	static constexpr int32 MemberCacheCapacity = 512;        // FLiveMemberCache LRU (proposed)
	static constexpr int32 UnrealClassCapacity = 256;        // FUnrealNameIndex class LRU (proposed)
	static constexpr int32 ChainCacheCapacity = 1024;

	FDreamCompletionService();
	~FDreamCompletionService();

	/** StartupModule, after the services exist: binds the run service and FDreamPython events. No Python, no ticker. */
	void Initialize(class FDreamRunService& Runs, FDreamPython& Python);
	/** ShutdownModule step 6, before the run service and Python stop: unbinds, removes its tickers, clears the caches. */
	void Shutdown();

	/** GT, any context. Never calls Python. True on a hit (Out holds the candidates). */
	bool QueryCached(const FDreamCompletionQuery& Query, FDreamCompletionAnswer& Out);

	/**
	 * Queues the query for the resolver (served at the next idle tick that may call Python). OnAnswer runs on the game
	 * thread from that tick, once, unless CancelLive(Ticket) came first. A queued request with the same chain and
	 * namespace is replaced (its callback is dropped). In state Cold, RequestStart(Reason) boots Python (RT-17); when
	 * Python cannot run at all, nothing is queued and 0 is returned.
	 * §3.7 policy Refuse: while a run is active or a pump is on the stack the request is refused (0; the caches answer
	 * alone), and a request still queued when a run ends is dropped, its callbacks told with an empty incomplete answer.
	 */
	uint64 RequestLive(const FDreamCompletionQuery& Query, FDreamOnCompletionAnswer OnAnswer, EDreamStartReason Reason = EDreamStartReason::Keystroke);
	void CancelLive(uint64 Ticket);

	/** §6.6 step 5: after a lookup with a single class or module match, its members are requested speculatively. */
	void Prefetch(const FDreamCompletionQuery& Query);

	/** The namespace roots of a buffer (D8): Session-mode buffers, the REPL and the console use "session"; an Isolated
	 *  buffer uses "isolated:<id>" while the newest executed run was this buffer's Isolated run (its kept last-run
	 *  namespace, §5.3.3), else "none". */
	static FString NamespaceFor(const FDreamBuffer& Buffer);
	/** The §7.6.1 buffer kind of a code view's buffer. */
	static dream::BufferKind BufferKindFor(const FDreamBuffer& Buffer);

	/** True when a live request could be served now (Python Ready, no run active). */
	bool IsLiveAvailable() const;
	/** True while a run is active (the popup footer, §7.5.3). */
	bool IsLivePaused() const;

	// ---- Test probes (tier 4) ----
	bool IsUnrealIndexComplete() const { return bUnrealIndexComplete; }
	int32 GetUnrealIndexCount() const { return static_cast<int32>(UnrealTopLevel->size()); }
	bool IsSessionTableFresh() const;
	bool HasPendingWork() const { return !Pending.IsEmpty() || bPrewarmActive || (!bUnrealIndexComplete && UnrealCursor > 0); }
	int32 GetBrokenTypeCount() const { return BrokenTypes.Num(); }
	/** Drops every cache and pending request (the next keystroke misses); tests use it to start from a known state. */
	void ResetForTest();

private:
	struct FMemberEntry
	{
		FDreamCandidateListPtr Items;
		FString TypeName;
		bool bStable = false;
		uint32 RunGeneration = 0;
		uint64 LastUse = 0;
	};

	struct FChainEntry
	{
		FString Token;
		FString TypeName;
		FString RootType;
		bool bRootBound = false;
		bool bIncomplete = false;
		/** Name and StringArg contexts (no token), and incomplete member answers, keep their items here. */
		FDreamCandidateListPtr Items;
		uint64 LastUse = 0;
	};

	struct FRootEntry
	{
		FString Name;
		dream::CompletionKind Kind = dream::CompletionKind::Variable;
		FString Token;
		FString TypeName;
	};

	struct FPendingRequest
	{
		FDreamCompletionQuery Query;
		FString Key;
		/** Every requester of this chain and namespace, by ticket (a later RequestLive of the same chain joins). */
		TArray<TPair<uint64, FDreamOnCompletionAnswer>> Callbacks;
		bool bSpeculative = true;   // only Prefetch callers
		uint32 QueuedRunGeneration = 0;     // FDreamRunService::GetRunGeneration() when queued (Refuse, §3.7)
	};

	/** Drops what a new Python generation or run generation invalidates (D6: caches are per generation). */
	void SyncGenerations();
	void ClearAll();
	FString CacheKey(const FDreamCompletionQuery& Query) const;
	FString RootTypeHint(const FDreamCompletionQuery& Query) const;

	bool LookupMembers(const FString& Token, FDreamCompletionAnswer& Out);
	void StoreMembers(const FString& Token, const FString& TypeName, bool bStable, FDreamCandidateList&& Items);
	void StoreChain(const FString& Key, FChainEntry&& Entry);
	void TrimLru();
	void RememberUnrealClass(const FString& ClassName, const FString& Token);
	bool IsUnrealShadowed() const;
	FDreamCandidateListPtr BuildSessionNames() const;

	void EnsureLiveTicker();
	bool TickLive();
	void ServeRequest(FPendingRequest& Request);
	void NoteBreaker(const FString& RootType, double Seconds, bool bReportedByResolver);

	void StartPrewarm();
	bool TickPrewarm();
	void StartUnrealIndex();
	bool TickUnrealIndex();

	void HandleRunFinished(const struct FDreamRunRecord& Record);
	void HandlePythonState(EDreamPythonState State);

	// Generations the caches belong to.
	uint32 PyGeneration = 0;
	uint32 RunGeneration = 0;
	uint32 SessionEpoch = 0;
	uint64 UseCounter = 0;

	TMap<FString, FMemberEntry> MemberCache;        // FLiveMemberCache: type token -> member list
	TMap<FString, FChainEntry> ChainCache;          // namespace + chain key -> token / items (per run generation)
	TArray<FRootEntry> SessionRoots;                // the post-run copy of the Session's top-level names (RV-23)
	TMap<FString, int32> SessionRootIndex;
	uint32 SessionRootsGeneration = MAX_uint32;
	bool bSessionRootsValid = false;

	// FUnrealNameIndex: the unreal module's top level, and unreal.X -> type token for the class records (an LRU through
	// MemberCache; both live for the Python generation).
	/** Appended in place while the index fills (answers share it; everything runs on the game thread). */
	TSharedRef<FDreamCandidateList> UnrealTopLevel = MakeShared<FDreamCandidateList>();
	bool bUnrealIndexComplete = false;
	int32 UnrealCursor = 0;
	TMap<FString, FString> UnrealClassTokens;
	TArray<FString> UnrealClassOrder;

	TArray<FPendingRequest> Pending;
	uint64 NextTicket = 1;
	TSet<FString> BrokenTypes;                      // §7.5.3 circuit breaker: root types with live resolution off

	// Prewarm pass state.
	bool bPrewarmActive = false;
	bool bPrewarmWanted = false;
	int32 PrewarmCursor = 0;
	uint32 PrewarmGeneration = 0;
	TArray<FRootEntry> PrewarmRoots;

	FTSTicker::FDelegateHandle LiveTicker;
	FTSTicker::FDelegateHandle PrewarmTicker;
	FTSTicker::FDelegateHandle IndexTicker;
	FDelegateHandle RunFinishedHandle;
	FDelegateHandle PythonStateHandle;
	bool bInitialized = false;
};
