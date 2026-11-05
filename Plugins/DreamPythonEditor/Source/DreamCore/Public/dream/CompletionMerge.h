// SPDX-License-Identifier: Apache-2.0
// Dream core: completion merging and ranking (TDD §7.6.1, D6, U4, INT-03), the snake_case fuzzy matcher (§7.5.5), the
// keyword table and the completion chain extractor (§7.5.3 "Chain extraction"). Pure functions with no state, no I/O and
// no allocation outside their results; any thread (the editor calls them on the game thread). Tier 1: T-MRG-001..006,
// T-MRG-009, T-MRG-012, T-MRG-014, T-MRG-019, T-RES-016 (Tests/DreamCore/test_completion.cpp).
//
// Strings are UTF-8 (labels come from Python identifiers). The matcher folds ASCII case only; other bytes match exactly.
// (completion choice) M1 merges the live resolver with the keyword table; the static (type server) source is M2, but
// the merge already takes any mix of sources, so the M2 language services only add a caller.
#pragma once

#include "dream/Export.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream
{
	/** What a completion item names; the popup draws an icon per kind. */
	enum class CompletionKind : uint8_t
	{
		Text,
		Variable,
		Function,
		Method,
		Class,
		Module,
		Property,
		Field,
		Constant,
		EnumMember,
		EditorProperty,         // an Editor Properties entry (INT-02): insert text may be get_editor_property("name")
		Keyword,
		Snippet,
	};

	/** Where a candidate came from (§7.6.1). */
	enum class CompletionSource : uint8_t
	{
		Live,                   // the resolver (§7.5), FUnrealNameIndex and the post-run Session cache
		Static,                 // the type server (M2)
		Keyword,                // KeywordCandidates() below
		Snippet,                // FDreamLibrary (M4)
	};

	/** The ranking groups of §7.6.1. The order between them depends on the buffer kind (GroupRank). */
	enum class ScopeGroup : uint8_t
	{
		Scope = 0,              // G0: Session names, frame locals, parameters (file buffers: static locals)
		LiveMembers = 1,        // G1
		StaticMembers = 2,      // G2
		KeywordsAndSnippets = 3,// G3
	};

	/** The four badges of U4 (T-MRG-003). There is no "project" badge. */
	enum class CompletionBadge : uint8_t
	{
		Runtime,                // live-only items
		Static,                 // static items, and items both sources have (static supplies the docs)
		Keyword,
		Snippet,
	};

	/** The buffer the popup belongs to; it decides the group order (§7.6.1 table). */
	enum class BufferKind : uint8_t
	{
		Repl,
		Scratch,
		ConsoleExecutor,
		File,
	};

	struct CompletionCandidate
	{
		std::string Label;          // the merge key: exact and case-sensitive (D6)
		std::string FilterText;     // matched against the prefix; empty = Label
		std::string SortText;       // server sortText, or the resolver's rank hint ("0" enum entries); empty = none
		std::string Detail;         // "StaticMeshComponent [Read-Write]", "-> Vector", "module"
		std::string Documentation;
		std::string InsertText;     // empty = Label
		std::string RuntimeType;    // the live concrete type, shown as " · runtime: X" when a static item has it too
		CompletionKind Kind = CompletionKind::Text;
		CompletionSource Source = CompletionSource::Live;
		ScopeGroup Group = ScopeGroup::LiveMembers;
		bool bInsertIsSnippet = false;
		uint64_t ResolveHandle = 0; // completionItem/resolve (M2)

		const std::string& GetFilterText() const { return FilterText.empty() ? Label : FilterText; }
		const std::string& GetInsertText() const { return InsertText.empty() ? Label : InsertText; }
	};

	struct MergedItem
	{
		/** The merged view: when both sources have the label, static Detail/Documentation/InsertText plus the live type. */
		CompletionCandidate Item;
		CompletionBadge Badge = CompletionBadge::Runtime;
		int32_t Score = 0;          // FuzzyScore against the context prefix
		uint32_t Id = 0;            // identity across merges (stable for the pinned items of the no-reorder rule)
		bool bHasLive = false;
		bool bHasStatic = false;
	};

	struct MergeContext
	{
		BufferKind Kind = BufferKind::Repl;
		/** File buffers use the REPL order when the chain's root is bound in the Session and the tab runs in Session (D6). */
		bool bRootBoundInSession = false;
		/** The identifier typed so far. Candidates that do not FuzzyScore against it are dropped. */
		std::string_view Prefix;
		/** The selection S of the no-reorder rule: items 0..S of Previous keep identity and order. -1 = nothing to pin
		 *  (first answer, or a new prefix: the caller re-merges its full candidate set). */
		int32_t SelectedIndex = -1;
		/** The incoming source said isIncomplete / incomplete: the next keystroke re-requests (§7.6.1). */
		bool bIncomingIncomplete = false;
	};

	struct MergedList
	{
		std::vector<MergedItem> Items;
		int32_t SelectedIndex = 0;
		bool bIncomplete = false;
	};

	/**
	 * Merges Incoming (all from IncomingSource) into Previous (§7.6.1). Pure and deterministic (T-MRG-001).
	 * - Key: the label, exact and case-sensitive. Static and live with one label become one item: the static item supplies
	 *   Detail, Documentation and InsertText, the live item its RuntimeType, and the badge is static (T-MRG-002).
	 *   Live beats keyword and snippet for content and badge; a repeated label from the same source replaces the content.
	 * - Groups in the order GroupRank gives for the buffer kind; within a group: fuzzy score (higher first), then SortText,
	 *   then label. Names starting with '_' sink unless the prefix starts with '_', dunders last (T-MRG-012). Snippets
	 *   match by prefix only.
	 * - No-reorder rule (D6, T-MRG-004): with S = Context.SelectedIndex >= 0, items 0..S of Previous keep identity and
	 *   order (they may be enriched in place); everything else is ranked below them, so a new item that would rank above S
	 *   lands at S+1. The result's SelectedIndex is S (clamped), or 0 when nothing was pinned.
	 * - bIncomplete is Previous.bIncomplete || Context.bIncomingIncomplete.
	 */
	DREAMCORE_API MergedList MergeCompletions(const MergedList& Previous, std::span<const CompletionCandidate> Incoming,
		CompletionSource IncomingSource, const MergeContext& Context);

	/** 0 for the first group shown. REPL, scratch, console: G0, G1, G2, G3. File: G0, G2, G1, G3, or the REPL order when
	 *  bRootBoundInSession (T-MRG-005, T-MRG-006). */
	DREAMCORE_API int32_t GroupRank(ScopeGroup Group, BufferKind Kind, bool bRootBoundInSession);

	/**
	 * The snake_case fuzzy matcher (§7.5.5), shared by completion and the Go To palette. Query characters must appear in
	 * order, ASCII case-insensitively. Score: +32 when the query is a prefix of the whole label, +40 per match at a word
	 * start (string start, after '_', a camelCase hump), +8 per consecutive match, +4 per exact-case match, -1 per
	 * skipped character after the first match. O(|Query| x |Label|) DP; labels are scored over their first 128 bytes.
	 * (completion choice, tuned on T-MRG-019) The word-start weight is 40, not §7.5.5's proposed 16: with 16 the prefix
	 * bonus makes "salt" outrank "set_actor_location" for "sal", against T-RES-016. A whole-label prefix is scored along
	 * its contiguous alignment only, so a word-start jump inside one label never outranks another label with the same
	 * typed prefix ("EditorAss": EditorAssetLibrary and EditorAssetSubsystem tie, then sort by label).
	 * An empty query matches everything with score 0. Returns false when the query does not match.
	 */
	DREAMCORE_API bool FuzzyScore(std::string_view Query, std::string_view Label, int32_t& OutScore);

	inline constexpr int32_t FuzzyMaxLabelBytes = 128;

	/** §7.6.1 "Incomplete lists": a growing prefix is re-filtered locally unless the list was incomplete (T-MRG-009). */
	DREAMCORE_API bool CanRefilterLocally(bool bIncomplete, std::string_view OldPrefix, std::string_view NewPrefix);

	/** "runtime", "static", "keyword", "snippet" (U4). */
	DREAMCORE_API const char* BadgeName(CompletionBadge Badge);

	/** The detail line the popup shows: Item.Detail, plus " · runtime: <RuntimeType>" when a static item has a live type. */
	DREAMCORE_API std::string DisplayDetail(const MergedItem& Item);

	/** The 35 hard keywords of 3.11, its soft keywords (_, case, match) and dir(builtins) of the shipped 3.11.8, as
	 *  Keyword-source candidates in group G3 (§7.6.1 "keyword" badge). */
	DREAMCORE_API std::vector<CompletionCandidate> KeywordCandidates();

	// ---- Chain extraction (§7.5.3) -------------------------------------------------------------------------------

	enum class ChainContext : uint8_t
	{
		None,                   // a comment, a closed or unsupported string, a number, a name being defined, ...
		Name,                   // a bare identifier (or nothing, at an explicit Ctrl+Space): root names
		Member,                 // after '.': members of what the steps resolve to
		StringArg,              // inside the first string argument of get_editor_property / set_editor_property
	};

	enum class ChainStepKind : uint8_t
	{
		Name,                   // the root identifier
		Attr,                   // .attr
		Sub,                    // [literal]
		Call,                   // (...) — arguments are ignored: calls resolve at type level (§7.5.2)
	};

	struct ChainStep
	{
		ChainStepKind Kind = ChainStepKind::Name;
		std::string Text;       // Name/Attr identifier; Sub string literal value
		int64_t Int = 0;        // Sub int literal (may be negative)
		bool bIntKey = false;   // Sub: Int is the key, else Text

		bool operator==(const ChainStep&) const = default;
	};

	struct CompletionChain
	{
		ChainContext Context = ChainContext::None;
		std::vector<ChainStep> Steps;   // receiver steps, root first (Member, StringArg)
		std::string Prefix;             // UTF-8: the partial identifier, or the string text typed so far
		int32_t PrefixStart = 0;        // UTF-16 column where Prefix starts: an accept replaces [PrefixStart, caret)
		std::string Func;               // StringArg: "get_editor_property" or "set_editor_property"
		int32_t ArgIndex = 0;           // StringArg: always 0 in M1
		char16_t Quote = 0;             // StringArg: the opening quote

		/** A stable cache key of everything but the prefix: context, steps, function and argument index. */
		DREAMCORE_API std::string Key() const;
		/** True when the root step is the identifier "unreal" (FUnrealNameIndex serves the first two levels). */
		bool IsUnrealRooted() const { return !Steps.empty() && Steps.front().Kind == ChainStepKind::Name && Steps.front().Text == "unreal"; }
	};

	inline constexpr int32_t MaxChainSteps = 64;

	/**
	 * The chain before Caret on one line (§7.5.3: C++ tokenises, Python never does). Lexes Line[0, Caret) with PyLexer and
	 * scans backwards: identifiers joined by '.', calls with balanced parentheses and literal subscripts (an int, a
	 * negative int or a plain quoted string); anything else ends the chain. A comment, a number, or a name after def,
	 * class, import, from, as, global or nonlocal gives None. bLineStartsInString (the line continues a triple-quoted
	 * string) gives None. M1 reads one line: a chain that starts on an earlier line resolves from its last line.
	 */
	DREAMCORE_API CompletionChain ExtractCompletionChain(std::u16string_view Line, int32_t Caret, bool bLineStartsInString = false);
}
