// SPDX-License-Identifier: Apache-2.0
// The typed requests and answers of the live resolver helpers (TDD §7.5.6, §12.1.3; A15 typed wrappers instead of a
// generic CallHelper). Plain structs, no Python: FDreamPython's resolver wrappers (Python/DreamPythonCompletion.cpp) fill
// them, FDreamCompletionService (Completion/DreamCompletionService.h) consumes them. Game thread. Completion group.
#pragma once

#include "CoreMinimal.h"
#include "dream/CompletionMerge.h"

#include <vector>

/** _impl.resolver.complete(req) (§7.5.6). */
struct FDreamResolverRequest
{
	/** Context, receiver steps, prefix, and for StringArg the function and argument index. */
	dream::CompletionChain Chain;
	/** "session", "isolated:<buffer id hex>", "frame:<id>" (M3) or "none" (D8 roots). */
	FString Namespace = TEXT("session");
	/** The resolver's deadline from the call start (D6: 5 ms). */
	int64 BudgetNs = 5'000'000;
	/** want_members_for: type tokens whose member lists C++ already holds; the resolver lists members only for others. */
	TArray<FString> KnownTokens;
	/** FDreamRunService::GetRunGeneration(): the resolver's caches are per generation (D6). */
	uint32 Generation = 0;
	/** The Session epoch (U19 alias rewrites); 0 until FDreamPython::GetSessionEpoch exists (B35). */
	uint32 Epoch = 0;
};

struct FDreamResolverResult
{
	/** "<tag>:<id hex>:<qualname>": I instance members, C class members, M module (the FLiveMemberCache key). */
	FString Token;
	FString TypeName;               // the resolved receiver's type, for " · runtime: X" and the footer
	FString RootType;               // the chain root's type name: the circuit-breaker key
	FString Breaker;                // non-empty: this call tripped the resolver's breaker for that root type
	FString Error;                  // the helper's error text, "breaker", or the C++ conversion failure
	/** Live candidates (Source Live; Group Scope for Name context, LiveMembers otherwise). Empty when bMembersCached. */
	std::vector<dream::CompletionCandidate> Items;
	bool bIncomplete = false;
	bool bRootBound = false;        // the root name is bound in the namespace (file buffers then use the REPL order, D6)
	bool bStable = false;           // the member list depends only on a C or unreal type: it may outlive a run
	bool bMembersCached = false;    // Token was in KnownTokens
	int64 ElapsedUs = 0;            // the resolver's own measure
	double CallSeconds = 0.0;       // C++ measure of the whole call, conversions included (the 20 ms breaker, §7.5.3)
};

/** One Session name from the post-run copy (RV-23). */
struct FDreamResolverRoot
{
	FString Name;
	dream::CompletionKind Kind = dream::CompletionKind::Variable;
	FString Token;
	FString TypeName;
};

struct FDreamResolverMembers
{
	FString Token;
	FString TypeName;
	bool bStable = false;
	std::vector<dream::CompletionCandidate> Items;
};

/** _impl.resolver.prewarm(req): one slice of the post-run Session copy (RV-23, §6.6 step 5). */
struct FDreamResolverPrewarmRequest
{
	FString Namespace = TEXT("session");
	int32 Cursor = 0;               // 0 starts a pass
	int64 BudgetNs = 2'000'000;     // idle slices of ≤ 2 ms (§7.5.3, proposed)
	TArray<FString> KnownTokens;
	uint32 Generation = 0;
	uint32 Epoch = 0;
};

struct FDreamResolverPrewarmResult
{
	TArray<FDreamResolverRoot> Roots;    // empty for a namespace over 10,000 names
	TArray<FDreamResolverMembers> Members;
	int32 Cursor = -1;                  // -1: the pass is done
	int32 Count = 0;
	bool bIncomplete = false;
	FString Error;
};

/** _impl.resolver.unreal_names(req): one slice of the unreal module's top-level names (the FUnrealNameIndex fill). */
struct FDreamResolverNamesResult
{
	std::vector<dream::CompletionCandidate> Items;
	int32 Cursor = -1;
	int32 Count = 0;
	FString Error;
};
