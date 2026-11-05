// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::CompletionMerge (TDD §7.6.1, §7.5.5, §7.5.3): T-MRG-001 (determinism), T-MRG-002 (static + live),
// T-MRG-003 (badges), T-MRG-004 (no-reorder property test), T-MRG-005 / T-MRG-006 (group order), T-MRG-009 (local
// re-filter), T-MRG-012 ('_' and dunders), T-MRG-014 (sortText, filterText), T-MRG-019 (golden merge corpus),
// T-RES-016 (fuzzy goldens), the keyword table and the chain extractor.
#include "dream_test.h"
#include "test_util.h"

#include "dream/CompletionMerge.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace dream;
using dream_test_util::Rng;
using dream_test_util::U;

namespace
{
	CompletionCandidate Cand(const char* Label, CompletionSource Source, ScopeGroup Group, const char* Detail = "")
	{
		CompletionCandidate C;
		C.Label = Label;
		C.Source = Source;
		C.Group = Group;
		C.Detail = Detail;
		C.Kind = Source == CompletionSource::Keyword ? CompletionKind::Keyword : CompletionKind::Method;
		return C;
	}

	MergedList MergeOne(const std::vector<CompletionCandidate>& Items, CompletionSource Source, BufferKind Kind,
		const char* Prefix = "", bool bBound = false)
	{
		MergeContext Context;
		Context.Kind = Kind;
		Context.Prefix = Prefix;
		Context.bRootBoundInSession = bBound;
		return MergeCompletions(MergedList(), Items, Source, Context);
	}

	std::vector<std::string> Labels(const MergedList& List)
	{
		std::vector<std::string> Out;
		for (const MergedItem& Item : List.Items)
		{
			Out.push_back(Item.Item.Label);
		}
		return Out;
	}

	int IndexOf(const MergedList& List, const char* Label)
	{
		for (size_t Index = 0; Index < List.Items.size(); ++Index)
		{
			if (List.Items[Index].Item.Label == Label)
			{
				return static_cast<int>(Index);
			}
		}
		return -1;
	}

	int32_t Score(const char* Query, const char* Label)
	{
		int32_t Out = 0;
		return FuzzyScore(Query, Label, Out) ? Out : -1000000;
	}

	std::string RandomLabel(Rng& R)
	{
		static const char* Words[] = { "get", "set", "actor", "location", "editor", "property", "mesh", "load", "asset",
			"save", "name", "salt", "spawn", "world", "Static", "Mesh", "Component", "x", "_private", "value" };
		std::string Label;
		const int Parts = 1 + static_cast<int>(R.Next() % 3);
		for (int Part = 0; Part < Parts; ++Part)
		{
			if (Part > 0)
			{
				Label += "_";
			}
			Label += Words[R.Next() % (sizeof(Words) / sizeof(Words[0]))];
		}
		if (R.Next() % 10 == 0)
		{
			Label = "__" + Label + "__";
		}
		return Label;
	}

	std::vector<CompletionCandidate> RandomCandidates(Rng& R, CompletionSource Source, int Count)
	{
		std::vector<CompletionCandidate> Out;
		for (int Index = 0; Index < Count; ++Index)
		{
			CompletionCandidate C = Cand("", Source, static_cast<ScopeGroup>(R.Next() % 4));
			C.Label = RandomLabel(R);
			C.Detail = "d" + std::to_string(R.Next() % 7);
			if (R.Next() % 4 == 0)
			{
				C.SortText = std::to_string(R.Next() % 3);
			}
			Out.push_back(std::move(C));
		}
		return Out;
	}
}

// ---- T-RES-016: fuzzy goldens --------------------------------------------------------------------------------------

TEST(Completion_Fuzzy_Goldens)
{
	// "gep" -> get_editor_property first; "sal" ranks set_actor_location above salt (§7.5.5, T-RES-016).
	const std::vector<CompletionCandidate> Members = {
		Cand("get_editor_property", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("get_property", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("gep_value", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("set_editor_property", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("geometry", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("set_actor_location", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("salt", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("get_actor_location", CompletionSource::Live, ScopeGroup::LiveMembers),
	};
	const MergedList Gep = MergeOne(Members, CompletionSource::Live, BufferKind::Repl, "gep");
	REQUIRE(!Gep.Items.empty());
	CHECK_EQ(Gep.Items[0].Item.Label, std::string("get_editor_property"));
	CHECK(IndexOf(Gep, "geometry") < 0);            // g, e then no p after
	const MergedList Sal = MergeOne(Members, CompletionSource::Live, BufferKind::Repl, "sal");
	CHECK(IndexOf(Sal, "set_actor_location") >= 0);
	CHECK(IndexOf(Sal, "salt") >= 0);
	CHECK(IndexOf(Sal, "set_actor_location") < IndexOf(Sal, "salt"));
	CHECK(Score("sal", "set_actor_location") > Score("sal", "salt"));
}

TEST(Completion_Fuzzy_Rules)
{
	int32_t Out = 7;
	CHECK(FuzzyScore("", "anything", Out));
	CHECK_EQ(Out, 0);
	CHECK(!FuzzyScore("xyz", "abc", Out));
	CHECK(!FuzzyScore("pge", "get_editor_property", Out));    // order matters
	CHECK(!FuzzyScore("toolong", "short", Out));
	// Case-insensitive, with a bonus for the exact case.
	CHECK(FuzzyScore("GEP", "get_editor_property", Out));
	CHECK(Score("gep", "get_editor_property") > Score("GEP", "get_editor_property"));
	// Word starts: snake_case, camelCase humps and separators.
	CHECK(Score("sma", "StaticMeshActor") > Score("sma", "smallactor"));
	CHECK(Score("loc", "location") > Score("loc", "get_actor_location"));   // a whole-label prefix
	// Consecutive matches beat scattered ones.
	CHECK(Score("act", "xactor") > Score("act", "xaxcxt"));
	// Labels are scored over their first 128 bytes only.
	std::string Long(140, 'a');
	Long += "zz";
	CHECK(!FuzzyScore("zz", Long, Out));
	CHECK(FuzzyScore("aa", Long, Out));
}

// ---- T-MRG-001..006 -------------------------------------------------------------------------------------------------

TEST(Completion_Merge_Deterministic)
{
	// T-MRG-001: the same inputs give the same output, field by field.
	for (uint64_t Seed = 1; Seed <= 40; ++Seed)
	{
		Rng R(Seed);
		const std::vector<CompletionCandidate> Live = RandomCandidates(R, CompletionSource::Live, 30);
		const std::vector<CompletionCandidate> Static = RandomCandidates(R, CompletionSource::Static, 30);
		MergeContext Context;
		Context.Kind = static_cast<BufferKind>(Seed % 4);
		Context.Prefix = (Seed % 3 == 0) ? "s" : "";
		const MergedList A1 = MergeCompletions(MergedList(), Live, CompletionSource::Live, Context);
		const MergedList B1 = MergeCompletions(MergedList(), Live, CompletionSource::Live, Context);
		Context.SelectedIndex = static_cast<int32_t>(Seed % 5);
		const MergedList A2 = MergeCompletions(A1, Static, CompletionSource::Static, Context);
		const MergedList B2 = MergeCompletions(B1, Static, CompletionSource::Static, Context);
		REQUIRE_EQ(A2.Items.size(), B2.Items.size());
		for (size_t Index = 0; Index < A2.Items.size(); ++Index)
		{
			CHECK_EQ(A2.Items[Index].Item.Label, B2.Items[Index].Item.Label);
			CHECK_EQ(A2.Items[Index].Id, B2.Items[Index].Id);
			CHECK_EQ(A2.Items[Index].Badge, B2.Items[Index].Badge);
			CHECK_EQ(A2.Items[Index].Item.Detail, B2.Items[Index].Item.Detail);
		}
		CHECK_EQ(A2.SelectedIndex, B2.SelectedIndex);
	}
}

TEST(Completion_Merge_StaticAndLive)
{
	// T-MRG-002: one item; static docs, insert text and detail; the live type as annotation; badge static. Both orders.
	CompletionCandidate Live = Cand("static_mesh_component", CompletionSource::Live, ScopeGroup::LiveMembers, "StaticMeshComponent [Read-Only]");
	Live.RuntimeType = "StaticMeshComponent";
	Live.Documentation = "live doc";
	CompletionCandidate Static = Cand("static_mesh_component", CompletionSource::Static, ScopeGroup::StaticMembers, "StaticMeshComponent");
	Static.Documentation = "Static docs from the stub";
	Static.InsertText = "static_mesh_component";

	MergeContext Context;
	Context.Kind = BufferKind::Repl;
	for (int Order = 0; Order < 2; ++Order)
	{
		MergedList List;
		if (Order == 0)
		{
			List = MergeCompletions(List, std::vector<CompletionCandidate>{ Live }, CompletionSource::Live, Context);
			Context.SelectedIndex = 0;
			List = MergeCompletions(List, std::vector<CompletionCandidate>{ Static }, CompletionSource::Static, Context);
		}
		else
		{
			Context.SelectedIndex = -1;
			List = MergeCompletions(List, std::vector<CompletionCandidate>{ Static }, CompletionSource::Static, Context);
			Context.SelectedIndex = 0;
			List = MergeCompletions(List, std::vector<CompletionCandidate>{ Live }, CompletionSource::Live, Context);
		}
		REQUIRE_EQ(List.Items.size(), static_cast<size_t>(1));
		const MergedItem& Item = List.Items[0];
		CHECK_EQ(Item.Badge, CompletionBadge::Static);
		CHECK(Item.bHasLive);
		CHECK(Item.bHasStatic);
		CHECK_EQ(Item.Item.Documentation, std::string("Static docs from the stub"));
		CHECK_EQ(Item.Item.Detail, std::string("StaticMeshComponent"));
		CHECK_EQ(Item.Item.RuntimeType, std::string("StaticMeshComponent"));
		CHECK_EQ(DisplayDetail(Item), std::string("StaticMeshComponent \xC2\xB7 runtime: StaticMeshComponent"));
	}
}

TEST(Completion_Merge_Badges)
{
	// T-MRG-003: only runtime, static, keyword, snippet.
	MergeContext Context;
	MergedList List = MergeCompletions(MergedList(), std::vector<CompletionCandidate>{ Cand("live_only", CompletionSource::Live, ScopeGroup::LiveMembers) },
		CompletionSource::Live, Context);
	List = MergeCompletions(List, std::vector<CompletionCandidate>{ Cand("static_only", CompletionSource::Static, ScopeGroup::StaticMembers) },
		CompletionSource::Static, Context);
	List = MergeCompletions(List, std::vector<CompletionCandidate>{ Cand("while", CompletionSource::Keyword, ScopeGroup::KeywordsAndSnippets) },
		CompletionSource::Keyword, Context);
	List = MergeCompletions(List, std::vector<CompletionCandidate>{ Cand("forloop", CompletionSource::Snippet, ScopeGroup::KeywordsAndSnippets) },
		CompletionSource::Snippet, Context);
	REQUIRE_EQ(List.Items.size(), static_cast<size_t>(4));
	CHECK_EQ(List.Items[IndexOf(List, "live_only")].Badge, CompletionBadge::Runtime);
	CHECK_EQ(List.Items[IndexOf(List, "static_only")].Badge, CompletionBadge::Static);
	CHECK_EQ(List.Items[IndexOf(List, "while")].Badge, CompletionBadge::Keyword);
	CHECK_EQ(List.Items[IndexOf(List, "forloop")].Badge, CompletionBadge::Snippet);
	const std::set<std::string> Names = { BadgeName(CompletionBadge::Runtime), BadgeName(CompletionBadge::Static),
		BadgeName(CompletionBadge::Keyword), BadgeName(CompletionBadge::Snippet) };
	CHECK(Names == (std::set<std::string>{ "runtime", "static", "keyword", "snippet" }));
	// Live beats a keyword entry of the same name (a Session name "print" shadows the builtin).
	List = MergeCompletions(MergedList(), std::vector<CompletionCandidate>{ Cand("print", CompletionSource::Keyword, ScopeGroup::KeywordsAndSnippets) },
		CompletionSource::Keyword, Context);
	List = MergeCompletions(List, std::vector<CompletionCandidate>{ Cand("print", CompletionSource::Live, ScopeGroup::Scope, "function") },
		CompletionSource::Live, Context);
	REQUIRE_EQ(List.Items.size(), static_cast<size_t>(1));
	CHECK_EQ(List.Items[0].Badge, CompletionBadge::Runtime);
	CHECK_EQ(List.Items[0].Item.Group, ScopeGroup::Scope);
}

TEST(Completion_Merge_NoReorder)
{
	// T-MRG-004: items 0..S keep identity and order through any late merge; new items that rank above S land at S+1.
	for (uint64_t Seed = 1; Seed <= 300; ++Seed)
	{
		Rng R(Seed * 7919);
		MergeContext Context;
		Context.Kind = static_cast<BufferKind>(R.Next() % 4);
		const MergedList First = MergeCompletions(MergedList(), RandomCandidates(R, CompletionSource::Live, 1 + static_cast<int>(R.Next() % 25)),
			CompletionSource::Live, Context);
		if (First.Items.empty())
		{
			continue;
		}
		const int32_t S = static_cast<int32_t>(R.Next() % First.Items.size());
		Context.SelectedIndex = S;
		const CompletionSource LateSource = (R.Next() % 2 == 0) ? CompletionSource::Static : CompletionSource::Live;
		std::vector<CompletionCandidate> Late = RandomCandidates(R, LateSource, static_cast<int>(R.Next() % 25));
		// A late item that would rank first: group 0, a label that sorts before everything.
		CompletionCandidate Top = Cand("aaa_first", LateSource, ScopeGroup::Scope);
		Top.SortText = " ";
		Late.push_back(Top);
		const MergedList Second = MergeCompletions(First, Late, LateSource, Context);
		REQUIRE(Second.Items.size() >= First.Items.size());
		for (int32_t Index = 0; Index <= S; ++Index)
		{
			CHECK_EQ(Second.Items[Index].Id, First.Items[Index].Id);
			CHECK_EQ(Second.Items[Index].Item.Label, First.Items[Index].Item.Label);
		}
		CHECK_EQ(Second.SelectedIndex, S);
		const int Placed = IndexOf(Second, "aaa_first");
		if (IndexOf(First, "aaa_first") < 0)
		{
			CHECK_EQ(Placed, S + 1);
		}
		// Every label appears once.
		std::set<std::string> Seen;
		for (const MergedItem& Item : Second.Items)
		{
			CHECK(Seen.insert(Item.Item.Label).second);
		}
	}
}

TEST(Completion_Merge_ReplOrder)
{
	// T-MRG-005: REPL, scratch and console: G0 scope names, G1 live members, G2 static members, G3 keywords.
	for (BufferKind Kind : { BufferKind::Repl, BufferKind::Scratch, BufferKind::ConsoleExecutor })
	{
		MergeContext Context;
		Context.Kind = Kind;
		MergedList List = MergeCompletions(MergedList(), std::vector<CompletionCandidate>{
			Cand("zz_scope", CompletionSource::Live, ScopeGroup::Scope),
			Cand("aa_live", CompletionSource::Live, ScopeGroup::LiveMembers) }, CompletionSource::Live, Context);
		List = MergeCompletions(List, std::vector<CompletionCandidate>{ Cand("aa_static", CompletionSource::Static, ScopeGroup::StaticMembers) },
			CompletionSource::Static, Context);
		List = MergeCompletions(List, std::vector<CompletionCandidate>{ Cand("aa_kw", CompletionSource::Keyword, ScopeGroup::KeywordsAndSnippets) },
			CompletionSource::Keyword, Context);
		CHECK(Labels(List) == (std::vector<std::string>{ "zz_scope", "aa_live", "aa_static", "aa_kw" }));
	}
}

TEST(Completion_Merge_FileOrder)
{
	// T-MRG-006: file buffers rank static before live, unless the root is bound in the Session (REPL order, D6).
	const std::vector<CompletionCandidate> Live = { Cand("aa_live", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("zz_scope", CompletionSource::Live, ScopeGroup::Scope) };
	const std::vector<CompletionCandidate> Static = { Cand("bb_static", CompletionSource::Static, ScopeGroup::StaticMembers) };
	for (int Bound = 0; Bound < 2; ++Bound)
	{
		MergeContext Context;
		Context.Kind = BufferKind::File;
		Context.bRootBoundInSession = Bound == 1;
		MergedList List = MergeCompletions(MergedList(), Live, CompletionSource::Live, Context);
		List = MergeCompletions(List, Static, CompletionSource::Static, Context);
		if (Bound == 0)
		{
			CHECK(Labels(List) == (std::vector<std::string>{ "zz_scope", "bb_static", "aa_live" }));
		}
		else
		{
			CHECK(Labels(List) == (std::vector<std::string>{ "zz_scope", "aa_live", "bb_static" }));
		}
	}
	CHECK_EQ(GroupRank(ScopeGroup::StaticMembers, BufferKind::File, false), 1);
	CHECK_EQ(GroupRank(ScopeGroup::LiveMembers, BufferKind::File, false), 2);
	CHECK_EQ(GroupRank(ScopeGroup::LiveMembers, BufferKind::File, true), 1);
}

// ---- T-MRG-009, 012, 014 --------------------------------------------------------------------------------------------

TEST(Completion_Merge_PrefixExtension)
{
	// T-MRG-009: a growing prefix is re-filtered locally unless the list was incomplete.
	CHECK(CanRefilterLocally(false, "se", "set"));
	CHECK(CanRefilterLocally(false, "", "s"));
	CHECK(CanRefilterLocally(false, "set", "set"));
	CHECK(!CanRefilterLocally(true, "se", "set"));
	CHECK(!CanRefilterLocally(false, "set", "se"));     // shrinking
	CHECK(!CanRefilterLocally(false, "se", "sa"));      // changed
	// The local re-filter is a re-merge of the full candidate set with the new prefix.
	const std::vector<CompletionCandidate> All = { Cand("set_actor_location", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("save", CompletionSource::Live, ScopeGroup::LiveMembers), Cand("select", CompletionSource::Live, ScopeGroup::LiveMembers) };
	const MergedList S = MergeOne(All, CompletionSource::Live, BufferKind::Repl, "s");
	const MergedList Se = MergeOne(All, CompletionSource::Live, BufferKind::Repl, "set");
	CHECK_EQ(S.Items.size(), static_cast<size_t>(3));
	CHECK(Labels(Se) == (std::vector<std::string>{ "set_actor_location", "select" }));
	MergeContext Context;
	Context.bIncomingIncomplete = true;
	CHECK(MergeCompletions(MergedList(), All, CompletionSource::Live, Context).bIncomplete);
}

TEST(Completion_Merge_UnderscoreAndDunder)
{
	// T-MRG-012: '_' names sink and dunders come last, unless the prefix starts with '_'.
	const std::vector<CompletionCandidate> Items = { Cand("__init__", CompletionSource::Live, ScopeGroup::LiveMembers),
		Cand("_private", CompletionSource::Live, ScopeGroup::LiveMembers), Cand("zpublic", CompletionSource::Live, ScopeGroup::LiveMembers) };
	CHECK(Labels(MergeOne(Items, CompletionSource::Live, BufferKind::Repl)) == (std::vector<std::string>{ "zpublic", "_private", "__init__" }));
	const MergedList Underscore = MergeOne(Items, CompletionSource::Live, BufferKind::Repl, "_");
	CHECK(IndexOf(Underscore, "zpublic") < 0);
	CHECK_EQ(Underscore.Items.size(), static_cast<size_t>(2));
	CHECK_EQ(Underscore.Items[0].Item.Label, std::string("_private"));   // equal tier: fuzzy score, then label
}

TEST(Completion_Merge_SortAndFilterText)
{
	// T-MRG-014: within a group, equal scores order by sortText; filterText is what the prefix matches.
	CompletionCandidate B = Cand("beta", CompletionSource::Static, ScopeGroup::StaticMembers);
	B.SortText = "0";
	CompletionCandidate A = Cand("alpha", CompletionSource::Static, ScopeGroup::StaticMembers);
	A.SortText = "1";
	CompletionCandidate C = Cand("gamma", CompletionSource::Static, ScopeGroup::StaticMembers);
	CHECK(Labels(MergeOne({ A, B, C }, CompletionSource::Static, BufferKind::Repl)) == (std::vector<std::string>{ "beta", "alpha", "gamma" }));
	CompletionCandidate F = Cand("render(target)", CompletionSource::Static, ScopeGroup::StaticMembers);
	F.FilterText = "render";
	CHECK_EQ(MergeOne({ F }, CompletionSource::Static, BufferKind::Repl, "rend").Items.size(), static_cast<size_t>(1));
	CHECK(MergeOne({ F }, CompletionSource::Static, BufferKind::Repl, "targ").Items.empty());
	// Enum entries rank first through the resolver's sort hint (T-RES-014 in C++).
	CompletionCandidate Entry = Cand("RED", CompletionSource::Live, ScopeGroup::LiveMembers);
	Entry.SortText = "0";
	CompletionCandidate Method = Cand("cast", CompletionSource::Live, ScopeGroup::LiveMembers);
	Method.SortText = "1";
	CHECK(Labels(MergeOne({ Method, Entry }, CompletionSource::Live, BufferKind::Repl)) == (std::vector<std::string>{ "RED", "cast" }));
}

TEST(Completion_Merge_SnippetsByPrefixOnly)
{
	const std::vector<CompletionCandidate> Items = { Cand("for_each_actor", CompletionSource::Snippet, ScopeGroup::KeywordsAndSnippets) };
	CHECK_EQ(MergeOne(Items, CompletionSource::Snippet, BufferKind::Repl, "for").Items.size(), static_cast<size_t>(1));
	CHECK(MergeOne(Items, CompletionSource::Snippet, BufferKind::Repl, "fea").Items.empty());
}

TEST(Completion_Merge_GoldenCorpus)
{
	// T-MRG-019: hand-checked merges; the weights of §7.5.5 are tuned on these.
	struct Case
	{
		const char* Prefix;
		BufferKind Kind;
		std::vector<CompletionCandidate> Live;
		std::vector<CompletionCandidate> Keywords;
		std::vector<std::string> Expected;
	};
	const std::vector<Case> Cases = {
		{ "pr", BufferKind::Repl,
			{ Cand("prop", CompletionSource::Live, ScopeGroup::Scope), Cand("preview", CompletionSource::Live, ScopeGroup::Scope) },
			{ Cand("print", CompletionSource::Keyword, ScopeGroup::KeywordsAndSnippets), Cand("property", CompletionSource::Keyword, ScopeGroup::KeywordsAndSnippets) },
			{ "preview", "prop", "print", "property" } },
		{ "sa", BufferKind::Repl,
			{ Cand("save_asset", CompletionSource::Live, ScopeGroup::LiveMembers), Cand("save_directory", CompletionSource::Live, ScopeGroup::LiveMembers),
			  Cand("does_asset_exist", CompletionSource::Live, ScopeGroup::LiveMembers), Cand("set_actor_location", CompletionSource::Live, ScopeGroup::LiveMembers) },
			{},
			{ "save_asset", "save_directory", "set_actor_location", "does_asset_exist" } },
		{ "EditorAss", BufferKind::ConsoleExecutor,
			{ Cand("EditorAssetLibrary", CompletionSource::Live, ScopeGroup::LiveMembers), Cand("EditorAssetSubsystem", CompletionSource::Live, ScopeGroup::LiveMembers),
			  Cand("EditorActorSubsystem", CompletionSource::Live, ScopeGroup::LiveMembers) },
			{},
			{ "EditorAssetLibrary", "EditorAssetSubsystem", "EditorActorSubsystem" } },
		{ "gal", BufferKind::Repl,
			{ Cand("get_actor_location", CompletionSource::Live, ScopeGroup::LiveMembers), Cand("get_all_level_actors", CompletionSource::Live, ScopeGroup::LiveMembers),
			  Cand("gallery", CompletionSource::Live, ScopeGroup::LiveMembers) },
			{},
			{ "get_all_level_actors", "get_actor_location", "gallery" } },
	};
	for (const Case& C : Cases)
	{
		MergeContext Context;
		Context.Kind = C.Kind;
		Context.Prefix = C.Prefix;
		MergedList List = MergeCompletions(MergedList(), C.Live, CompletionSource::Live, Context);
		List = MergeCompletions(List, C.Keywords, CompletionSource::Keyword, Context);
		CHECK(Labels(List) == C.Expected);
		if (Labels(List) != C.Expected)
		{
			std::string Got;
			for (const std::string& L : Labels(List))
			{
				Got += L + " ";
			}
			std::fprintf(stderr, "  golden \"%s\": got %s\n", C.Prefix, Got.c_str());
		}
	}
}

TEST(Completion_KeywordTable)
{
	const std::vector<CompletionCandidate> Table = KeywordCandidates();
	std::set<std::string> Seen;
	for (const CompletionCandidate& C : Table)
	{
		CHECK(Seen.insert(C.Label).second);
		CHECK_EQ(C.Source, CompletionSource::Keyword);
		CHECK_EQ(C.Group, ScopeGroup::KeywordsAndSnippets);
	}
	for (const char* Word : { "for", "while", "None", "True", "lambda", "match", "case", "_", "print", "len", "isinstance", "ValueError" })
	{
		CHECK(Seen.count(Word) == 1);
	}
	CHECK(Table.size() > 150);
}

// ---- Chain extraction (§7.5.3) ------------------------------------------------------------------------------------

namespace
{
	CompletionChain Chain(const char* Line, int Caret = -1)
	{
		const std::u16string Text = U(Line);
		return ExtractCompletionChain(Text, Caret < 0 ? static_cast<int32_t>(Text.size()) : Caret);
	}

	ChainStep Step(ChainStepKind Kind, const char* Text = "")
	{
		ChainStep S;
		S.Kind = Kind;
		S.Text = Text;
		return S;
	}

	ChainStep IntSub(int64_t Value)
	{
		ChainStep S;
		S.Kind = ChainStepKind::Sub;
		S.Int = Value;
		S.bIntKey = true;
		return S;
	}
}

TEST(Completion_Chain_Member)
{
	// The TDD example: [("name","sel"), ("sub",0), ("attr","static_mesh_component"), ("prefix","set_")].
	const CompletionChain C = Chain("    sel[0].static_mesh_component.set_");
	CHECK_EQ(C.Context, ChainContext::Member);
	REQUIRE_EQ(C.Steps.size(), static_cast<size_t>(3));
	CHECK(C.Steps[0] == Step(ChainStepKind::Name, "sel"));
	CHECK(C.Steps[1] == IntSub(0));
	CHECK(C.Steps[2] == Step(ChainStepKind::Attr, "static_mesh_component"));
	CHECK_EQ(C.Prefix, std::string("set_"));
	CHECK_EQ(C.PrefixStart, 33);
	CHECK_EQ(C.Key(), std::string("m|n:sel|i:0|a:static_mesh_component"));

	const CompletionChain Calls = Chain("unreal.EditorActorSubsystem().get_selected_level_actors()[0].");
	CHECK_EQ(Calls.Context, ChainContext::Member);
	REQUIRE_EQ(Calls.Steps.size(), static_cast<size_t>(6));
	CHECK(Calls.Steps[0] == Step(ChainStepKind::Name, "unreal"));
	CHECK(Calls.Steps[1] == Step(ChainStepKind::Attr, "EditorActorSubsystem"));
	CHECK(Calls.Steps[2] == Step(ChainStepKind::Call));
	CHECK(Calls.Steps[3] == Step(ChainStepKind::Attr, "get_selected_level_actors"));
	CHECK(Calls.Steps[4] == Step(ChainStepKind::Call));
	CHECK(Calls.Steps[5] == IntSub(0));
	CHECK(Calls.Prefix.empty());
	CHECK(Calls.IsUnrealRooted());

	const CompletionChain Args = Chain("x = lib.load_asset('/Game/a', (1, 2)).get_na");
	CHECK_EQ(Args.Context, ChainContext::Member);
	REQUIRE_EQ(Args.Steps.size(), static_cast<size_t>(3));
	CHECK(Args.Steps[2] == Step(ChainStepKind::Call));
	CHECK_EQ(Args.Prefix, std::string("get_na"));

	const CompletionChain Keys = Chain("d['key'][-1].");
	REQUIRE_EQ(Keys.Steps.size(), static_cast<size_t>(3));
	CHECK(Keys.Steps[1] == Step(ChainStepKind::Sub, "key"));
	CHECK(Keys.Steps[2] == IntSub(-1));

	const CompletionChain Mid = Chain("unreal.EditorAss = 3", 16);
	CHECK_EQ(Mid.Context, ChainContext::Member);
	CHECK_EQ(Mid.Prefix, std::string("EditorAss"));
}

TEST(Completion_Chain_NameAndNone)
{
	const CompletionChain Name = Chain("print(my_va");
	CHECK_EQ(Name.Context, ChainContext::Name);
	CHECK_EQ(Name.Prefix, std::string("my_va"));
	CHECK_EQ(Name.PrefixStart, 6);
	CHECK(Name.Steps.empty());
	CHECK_EQ(Chain("").Context, ChainContext::Name);
	CHECK_EQ(Chain("x = ").Context, ChainContext::Name);

	CHECK_EQ(Chain("x = 1  # sel.").Context, ChainContext::None);       // comment
	CHECK_EQ(Chain("x = 1.").Context, ChainContext::None);              // number
	CHECK_EQ(Chain("x = 12").Context, ChainContext::None);
	CHECK_EQ(Chain("def my_fun").Context, ChainContext::None);          // a name being defined
	CHECK_EQ(Chain("class Fo").Context, ChainContext::None);
	CHECK_EQ(Chain("import o").Context, ChainContext::None);
	CHECK_EQ(Chain("from os import pa").Context, ChainContext::None);
	CHECK_EQ(Chain("(a + b).").Context, ChainContext::None);            // not a chain
	CHECK_EQ(Chain("x[i].").Context, ChainContext::None);               // a non-literal key
	CHECK_EQ(Chain("s = 'abc.de").Context, ChainContext::None);         // inside a plain string
	CHECK_EQ(Chain("None.").Context, ChainContext::None);               // a keyword root
	const std::u16string Text = U("sel.x");
	CHECK_EQ(ExtractCompletionChain(Text, 5, true).Context, ChainContext::None);   // the line continues a triple-quoted string
}

TEST(Completion_Chain_StringArg)
{
	const CompletionChain C = Chain("comp = actor.get_editor_property('stat");
	CHECK_EQ(C.Context, ChainContext::StringArg);
	REQUIRE_EQ(C.Steps.size(), static_cast<size_t>(1));
	CHECK(C.Steps[0] == Step(ChainStepKind::Name, "actor"));
	CHECK_EQ(C.Func, std::string("get_editor_property"));
	CHECK_EQ(C.ArgIndex, 0);
	CHECK_EQ(C.Prefix, std::string("stat"));
	CHECK_EQ(C.Quote, u'\'');
	CHECK_EQ(C.PrefixStart, 34);
	CHECK_EQ(C.Key(), std::string("s|n:actor|f:get_editor_property#0"));

	const CompletionChain Set = Chain("sel[0].set_editor_property(\"");
	CHECK_EQ(Set.Context, ChainContext::StringArg);
	CHECK_EQ(Set.Prefix, std::string());
	CHECK_EQ(Set.Steps.size(), static_cast<size_t>(2));

	CHECK_EQ(Chain("x.get_editor_property('a', '").Context, ChainContext::None);   // argument 1
	CHECK_EQ(Chain("x.load('stat").Context, ChainContext::None);                   // another function
	CHECK_EQ(Chain("x.get_editor_property('done')").Context, ChainContext::None);  // closed
	CHECK_EQ(Chain("x.get_editor_property(f'st").Context, ChainContext::None);     // prefixed string
}
