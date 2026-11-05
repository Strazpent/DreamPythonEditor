// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::storage (TDD §10.1, §10.5, §10.6): namespace names (i-<pid>-<start>, compat-, imported-, the delete
// guard), the lock content and its tolerant reader (§10.5 "content empty or unparseable"), owner.json (RV-08: missing,
// mine, another account, unreadable), projects.json (round trip keeping unknown keys, a newer schema never rewritten,
// corrupt input refused so the caller falls back to .bak), the moved-project rule (U29, T-PER-013 shape: a new folder,
// the same name, a path that no longer exists, Ignore remembered), the same-identity rule, and the ISO time codec.
#include "dream_test.h"

#include "dream/StorageIndex.h"

#include <set>
#include <string>

using namespace dream::storage;

TEST(Storage_NamespaceNamesRoundTrip)
{
	const std::string Sec = MakeSecondaryNamespace(1234, 1761872625123);
	CHECK_EQ(Sec, std::string("i-1234-1761872625123"));
	NamespaceName N = ParseNamespace(Sec);
	CHECK(N.Kind == NamespaceKind::Secondary);
	CHECK_EQ(N.Pid, 1234u);
	CHECK_EQ(N.ProcessStartMs, static_cast<int64_t>(1761872625123));

	CHECK(ParseNamespace("main").Kind == NamespaceKind::Main);

	const std::string Compat = MakeCompatNamespace(1, 0);
	CHECK_EQ(Compat, std::string("compat-1.0"));
	N = ParseNamespace(Compat);
	CHECK(N.Kind == NamespaceKind::Compat);
	CHECK_EQ(N.Version, std::string("1.0"));

	CHECK_EQ(MakeImportedNamespace("CitySample-7c41e2", "main"), std::string("imported-CitySample-7c41e2"));
	N = ParseNamespace("imported-CitySample-7c41e2");
	CHECK(N.Kind == NamespaceKind::Imported);
	CHECK_EQ(N.OldHash, std::string("CitySample-7c41e2"));
	CHECK_EQ(N.SourceNamespace, std::string("main"));
	const std::string ImportedSec = MakeImportedNamespace("CitySample-7c41e2", "i-9-10");
	N = ParseNamespace(ImportedSec);
	CHECK(N.Kind == NamespaceKind::Imported);
	CHECK_EQ(N.OldHash, std::string("CitySample-7c41e2"));
	CHECK_EQ(N.SourceNamespace, std::string("i-9-10"));

	// A malformed secondary name is still a well-formed folder: listed, never treated as a live instance's.
	CHECK(ParseNamespace("i-abc-1").Kind == NamespaceKind::Other);
	CHECK(ParseNamespace("i-99999999999-1").Kind == NamespaceKind::Other);
	CHECK(ParseNamespace("something").Kind == NamespaceKind::Other);
}

TEST(Storage_ImportedNamespaceRefusesUnsafeHashesAndKeepsDashes)
{
	// The old hash is a projects.json key nobody validated: it becomes a copy-target folder name only when it is safe.
	for (const char* Bad : { "../evil", "..", ".", "a/b", "a\\b", "c:", "x.", "dot.ted", "", "with space" })
	{
		CHECK(MakeImportedNamespace(Bad, "main").empty());
		CHECK(MakeImportedNamespace(Bad, "i-1-2").empty());
	}
	CHECK(MakeImportedNamespace("Good-7c41e2", "../x").empty());
	CHECK(MakeImportedNamespace(std::string(130, 'a'), "main").empty());

	// A project name with dashes (U29 keeps '-') and a namespace with dashes round-trip.
	const std::string Main = MakeImportedNamespace("my-cool-proj-7c41e2", "main");
	CHECK_EQ(Main, std::string("imported-my-cool-proj-7c41e2"));
	NamespaceName N = ParseNamespace(Main);
	CHECK(N.Kind == NamespaceKind::Imported);
	CHECK_EQ(N.OldHash, std::string("my-cool-proj-7c41e2"));
	CHECK_EQ(N.SourceNamespace, std::string("main"));
	const std::string Sec = MakeImportedNamespace("my-cool-proj-7c41e2", "i-4242-1000");
	CHECK(IsSafeName(Sec));
	N = ParseNamespace(Sec);
	CHECK(N.Kind == NamespaceKind::Imported);
	CHECK_EQ(N.OldHash, std::string("my-cool-proj-7c41e2"));
	CHECK_EQ(N.SourceNamespace, std::string("i-4242-1000"));
	N = ParseNamespace(MakeImportedNamespace("a-b-c-d-12345678", "compat-1.0"));
	CHECK_EQ(N.OldHash, std::string("a-b-c-d-12345678"));
	CHECK_EQ(N.SourceNamespace, std::string("compat-1.0"));
	// Names Dream never makes are folders of no known kind.
	CHECK(ParseNamespace("imported-.x").Kind == NamespaceKind::Other);
}

TEST(Storage_UnsafeNamesAreNeverNamespaces)
{
	for (const char* Bad : { "", ".", "..", "a/b", "a\\b", "c:", "..\\x", "name with space" })
	{
		CHECK(!IsSafeName(Bad));
		CHECK(ParseNamespace(Bad).Kind == NamespaceKind::Invalid);
	}
	CHECK(!IsSafeName(std::string(129, 'a')));
	CHECK(IsSafeName("i-1-2"));
	CHECK(IsSafeName("compat-1.0"));
}

TEST(Storage_LockContentRoundTripAndTolerantRead)
{
	LockContent In;
	In.Pid = 4242;
	In.ProcessStartMs = 1761872625000;
	In.Host = "WORKSTATION";
	In.PluginVersion = "0.1.0";
	In.Namespace = "i-4242-1761872625000";
	In.UtcMs = 1761872630000;
	LockContent Out;
	REQUIRE(ParseLock(SerializeLock(In), Out));
	CHECK_EQ(Out.Pid, In.Pid);
	CHECK_EQ(Out.ProcessStartMs, In.ProcessStartMs);
	CHECK_EQ(Out.Host, In.Host);
	CHECK_EQ(Out.PluginVersion, In.PluginVersion);
	CHECK_EQ(Out.Namespace, In.Namespace);
	CHECK_EQ(Out.UtcMs, In.UtcMs);

	// §10.5: empty or unparseable content (a lock being written, slice A's {"pid": n}) reads as unknown or partial.
	CHECK(!ParseLock("", Out));
	CHECK(!ParseLock("{\"pi", Out));
	CHECK(!ParseLock("[1,2]", Out));
	CHECK(!ParseLock("{\"pid\": 0}", Out));
	REQUIRE(ParseLock("{\"pid\": 77}\n", Out));
	CHECK_EQ(Out.Pid, 77u);
	CHECK(Out.Namespace.empty());
}

TEST(Storage_OwnerJson)
{
	const std::string Sid = "S-1-5-21-1004336348-1177238915-682003330-512";
	const std::string Text = SerializeOwner(Sid, "2026-10-31T01:00:00Z");
	CHECK(CheckOwner(Text, true, Sid) == OwnerCheck::Mine);
	CHECK(CheckOwner(Text, true, "s-1-5-21-1004336348-1177238915-682003330-512") == OwnerCheck::Mine);   // SIDs ignore case
	CHECK(CheckOwner(Text, true, "S-1-5-21-1004336348-1177238915-682003330-1001") == OwnerCheck::Other);
	CHECK(CheckOwner("", false, Sid) == OwnerCheck::Missing);
	CHECK(CheckOwner("", true, Sid) == OwnerCheck::Unreadable);       // present but empty: never trusted
	CHECK(CheckOwner("{\"schema\": 1}", true, Sid) == OwnerCheck::Unreadable);
	CHECK(CheckOwner("not json", true, Sid) == OwnerCheck::Unreadable);
}

namespace
{
	const char* SampleProjects = R"({
  "schema": 1,
  "studio_note": "kept",
  "projects": {
    "CitySample-7c41e2": {"uproject": "d:/old/citysample/citysample.uproject", "name": "CitySample", "file_id": "1:2", "root": "c:/u/dream/5.8/CitySample-7c41e2", "last_seen_utc": "2026-10-28T10:00:00Z", "extra": 5},
    "Other-000001": {"uproject": "d:/other/other.uproject", "name": "Other", "file_id": "3:4", "root": "c:/u/dream/5.8/Other-000001", "last_seen_utc": "2026-10-30T10:00:00Z"}
  }
})";
}

TEST(Storage_ProjectsRoundTripKeepsUnknownKeys)
{
	ProjectsIndex Index;
	REQUIRE(ParseProjects(SampleProjects, Index));
	CHECK(!Index.bNewerSchema);
	CHECK_EQ(Index.Projects.size(), static_cast<size_t>(2));
	const ProjectEntry* City = Index.Find("CitySample-7c41e2");
	REQUIRE(City != nullptr);
	CHECK_EQ(City->Name, std::string("CitySample"));
	CHECK_EQ(City->FileId, std::string("1:2"));

	ProjectEntry Ours;
	Ours.Hash = "CitySample-aaaaaa";
	Ours.UProject = "d:/new/citysample/citysample.uproject";
	Ours.Name = "CitySample";
	Ours.FileId = "1:2";
	Ours.Root = "c:/u/dream/5.8/CitySample-aaaaaa";
	Ours.LastSeenUtc = "2026-10-31T01:00:00Z";
	Index.Upsert(Ours);
	ProjectEntry Touch = *Index.Find("CitySample-7c41e2");
	Touch.LastSeenUtc = "2026-10-31T02:00:00Z";
	Touch.Raw = dream::json::Value();        // an entry rebuilt from fields keeps the unknown keys it was read with
	Index.Upsert(Touch);

	ProjectsIndex Again;
	REQUIRE(ParseProjects(SerializeProjects(Index), Again));
	CHECK_EQ(Again.Projects.size(), static_cast<size_t>(3));
	CHECK_EQ(Again.Raw.Get("studio_note").AsString(), std::string("kept"));
	REQUIRE(Again.Find("CitySample-7c41e2") != nullptr);
	CHECK_EQ(Again.Find("CitySample-7c41e2")->Raw.Get("extra").AsInt(), static_cast<int64_t>(5));
	CHECK_EQ(Again.Find("CitySample-7c41e2")->LastSeenUtc, std::string("2026-10-31T02:00:00Z"));
	CHECK_EQ(Again.Find("CitySample-aaaaaa")->Root, Ours.Root);
}

TEST(Storage_ProjectsNewerSchemaAndCorruptInput)
{
	ProjectsIndex Index;
	REQUIRE(ParseProjects(R"({"schema": 9, "projects": {}})", Index));
	CHECK(Index.bNewerSchema);
	// A newer schema is used read-only by the caller; the serializer never writes our schema number over it.
	ProjectsIndex Again;
	REQUIRE(ParseProjects(SerializeProjects(Index), Again));
	CHECK_EQ(Again.Raw.Get("schema").AsInt(), static_cast<int64_t>(9));

	CHECK(!ParseProjects("", Index));
	CHECK(!ParseProjects("{\"schema\": 1, \"projects\": [", Index));
	CHECK(!ParseProjects("{\"schema\": 1, \"projects\": []}", Index));
	CHECK(!ParseProjects("[]", Index));
}

TEST(Storage_MovedProjectRule)
{
	ProjectsIndex Index;
	REQUIRE(ParseProjects(SampleProjects, Index));
	const std::set<std::string> Existing = { "d:/other/other.uproject" };
	auto Exists = [&Existing](std::string_view Path) { return Existing.count(std::string(Path)) != 0; };

	// A new folder for CitySample under another hash: the old entry's path is gone, so it is offered.
	std::vector<ProjectEntry> Moved = FindMovedProjects(Index, "CitySample-aaaaaa", "CitySample", true, Exists);
	REQUIRE(Moved.size() == 1);
	CHECK_EQ(Moved[0].Hash, std::string("CitySample-7c41e2"));

	// Not a new folder and nothing pending: no offer.
	CHECK(FindMovedProjects(Index, "CitySample-aaaaaa", "CitySample", false, Exists).empty());

	// Offered once (pending) and the session ended without an answer: offered again although the folder is not new.
	ProjectEntry Ours;
	Ours.Hash = "CitySample-aaaaaa";
	Ours.Name = "CitySample";
	Ours.PendingImports = { "CitySample-7c41e2" };
	Index.Upsert(Ours);
	CHECK_EQ(FindMovedProjects(Index, "CitySample-aaaaaa", "CitySample", false, Exists).size(), static_cast<size_t>(1));

	// Ignore (or Import) is remembered: never offered again, even for a new folder.
	Index.Find("CitySample-aaaaaa")->PendingImports.clear();
	Index.Find("CitySample-aaaaaa")->DecidedImports = { "CitySample-7c41e2" };
	CHECK(FindMovedProjects(Index, "CitySample-aaaaaa", "CitySample", true, Exists).empty());

	// The old path still exists: another checkout of the same name, never offered.
	ProjectsIndex Fresh;
	REQUIRE(ParseProjects(SampleProjects, Fresh));
	const std::set<std::string> Both = { "d:/old/citysample/citysample.uproject" };
	auto BothExist = [&Both](std::string_view Path) { return Both.count(std::string(Path)) != 0; };
	CHECK(FindMovedProjects(Fresh, "CitySample-aaaaaa", "CitySample", true, BothExist).empty());
	// Another name is never offered.
	CHECK(FindMovedProjects(Fresh, "Different-aaaaaa", "Different", true, Exists).empty());
}

TEST(Storage_SameIdentityRule)
{
	ProjectsIndex Index;
	REQUIRE(ParseProjects(SampleProjects, Index));
	std::vector<ProjectEntry> Same = FindSameIdentity(Index, "CitySample-aaaaaa", "1:2");
	REQUIRE(Same.size() == 1);
	CHECK_EQ(Same[0].Hash, std::string("CitySample-7c41e2"));
	CHECK(FindSameIdentity(Index, "CitySample-7c41e2", "1:2").empty());   // our own entry is never listed
	CHECK(FindSameIdentity(Index, "CitySample-aaaaaa", "").empty());      // an unknown identity matches nothing
}

TEST(Storage_IsoTime)
{
	CHECK_EQ(FormatIsoUtc(0), std::string("1970-01-01T00:00:00Z"));
	CHECK_EQ(FormatIsoUtc(1761872625123), std::string("2025-10-31T01:03:45Z"));
	CHECK_EQ(FormatIsoUtc(951782400000), std::string("2000-02-29T00:00:00Z"));
	int64_t Ms = 0;
	REQUIRE(ParseIsoUtc("2025-10-31T01:03:45Z", Ms));
	CHECK_EQ(Ms, static_cast<int64_t>(1761872625000));
	REQUIRE(ParseIsoUtc("2000-02-29T00:00:00.500Z", Ms));
	CHECK_EQ(Ms, static_cast<int64_t>(951782400000));
	for (int64_t T : { static_cast<int64_t>(0), static_cast<int64_t>(86399000), static_cast<int64_t>(4102444799000) })
	{
		int64_t Back = -1;
		REQUIRE(ParseIsoUtc(FormatIsoUtc(T), Back));
		CHECK_EQ(Back, T);
	}
	CHECK(!ParseIsoUtc("yesterday", Ms));
	CHECK(!ParseIsoUtc("2025-13-01T00:00:00Z", Ms));
}
