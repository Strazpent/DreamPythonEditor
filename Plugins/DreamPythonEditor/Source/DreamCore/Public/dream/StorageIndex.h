// SPDX-License-Identifier: Apache-2.0
// Dream core: the small storage files of the multi-instance and storage-root work (TDD §10.1, §10.5, §10.6): journal
// namespace names (main, i-<pid>-<process_start>, compat-<version>, imported-<oldhash>), the lock-file content
// {schema, pid, process_start, host, plugin_version, namespace, utc}, the override base folder's owner.json, and the
// per-engine-version project index projects.json with the moved-project and same-identity rules. Data only (SEC-06): no
// I/O, no clock, no threads; DreamEditor reads and writes the files (Persistence/DreamInstances.cpp) and passes the facts
// these rules need (which paths still exist, the time now).
#pragma once

#include "dream/Export.h"
#include "dream/Json.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dream::storage
{
	// ---- Journal namespaces (§10.5, §10.6, §10.1) ------------------------------------------------------------------

	inline constexpr std::string_view MainNamespace = "main";

	enum class NamespaceKind : uint8_t
	{
		Main,                       // journal/main: the primary's
		Secondary,                  // journal/i-<pid>-<process_start>: a second editor instance (§10.5)
		Compat,                     // journal/compat-<version>: an older build on newer data (§10.6)
		Imported,                   // journal/imported-<oldhash>[.<ns>]: a moved project's journal (§10.1)
		Other,                      // a well-formed name of none of these (listed as an orphan, never written)
		Invalid,                    // not a namespace name Dream would ever create or delete (".", "..", separators)
	};

	struct NamespaceName
	{
		NamespaceKind Kind = NamespaceKind::Invalid;
		uint32_t Pid = 0;                   // Secondary
		int64_t ProcessStartMs = 0;         // Secondary: UTC ms of the process creation time
		std::string Version;                // Compat: "<major>.<minor>"
		std::string OldHash;                // Imported: the moved project's "<Name>-<hex6>"
		std::string SourceNamespace;        // Imported: the namespace it was copied from ("main" when the suffix is absent)
	};

	/** "i-<pid>-<process_start>" (§10.5; FDreamAudit's secondary file names use the same id). */
	DREAMCORE_API std::string MakeSecondaryNamespace(uint32_t Pid, int64_t ProcessStartMs);
	/** "compat-<major>.<minor>": the journal format this build writes (§10.6, slice B choice). */
	DREAMCORE_API std::string MakeCompatNamespace(uint16_t Major, uint16_t Minor);
	/** "imported-<oldhash>" for a moved project's main namespace, "imported-<oldhash>.<ns>" for its others (§10.1; a hash
	 *  has no '.', so the parse is unambiguous). Empty when OldHash or the namespace is not a safe name (a projects.json key
	 *  nobody validated): the import is then refused. */
	DREAMCORE_API std::string MakeImportedNamespace(std::string_view OldHash, std::string_view SourceNamespace);
	DREAMCORE_API NamespaceName ParseNamespace(std::string_view Name);
	/** Only [A-Za-z0-9._-], 1..128 characters, not "." or "..", no trailing '.': the guard before a namespace folder is
	 *  created from data or deleted. */
	DREAMCORE_API bool IsSafeName(std::string_view Name);

	// ---- Lock files (§10.5): instance.lock and journal/<ns>/ns.lock ----------------------------------------------------

	inline constexpr int LockSchema = 1;

	struct LockContent
	{
		int Schema = LockSchema;
		uint32_t Pid = 0;
		int64_t ProcessStartMs = 0;
		std::string Host;
		std::string PluginVersion;
		std::string Namespace;
		int64_t UtcMs = 0;                  // the heartbeat (remote roots, §10.5)
	};

	DREAMCORE_API std::string SerializeLock(const LockContent& Content);
	/** False when the bytes are empty or not a lock object (§10.5: the reader then retries for 200 ms, then treats it as live). */
	DREAMCORE_API bool ParseLock(std::string_view Bytes, LockContent& Out);

	// ---- owner.json in an override base folder (§10.1, RV-08) -----------------------------------------------------

	inline constexpr int OwnerSchema = 1;

	enum class OwnerCheck : uint8_t
	{
		Missing,                    // no owner.json: written at first use
		Mine,                       // names this account
		Other,                      // names another account: the root is rejected, never read, listed or deleted from
		Unreadable,                 // present but not an owner object: treated as another account's (never trusted)
	};

	/** Bytes empty = the file is missing. Sid is the account's string SID (or POSIX uid as decimal). */
	DREAMCORE_API OwnerCheck CheckOwner(std::string_view Bytes, bool bFileExists, std::string_view Sid);
	/** {"schema": 1, "sid": "...", "created_utc": "..."}; NowIso is the caller's clock (no clock in DreamCore). */
	DREAMCORE_API std::string SerializeOwner(std::string_view Sid, std::string_view NowIso);

	// ---- projects.json (§10.1, U29) -------------------------------------------------------------------------------

	inline constexpr int ProjectsSchema = 1;

	/** One project of %LOCALAPPDATA%\DreamPythonEditor\<EngineVer>\projects.json, keyed by its "<Name>-<hex6>". */
	struct ProjectEntry
	{
		std::string Hash;
		std::string UProject;               // the normalised final .uproject path
		std::string Name;
		std::string FileId;                 // "<volume serial>:<file ID>" (GetFileInformationByHandle)
		std::string Root;                   // the storage root it last used
		std::string LastSeenUtc;            // ISO 8601
		/** (slice B choice) Moved-project offers this project's folder still owes an answer to, and the answers given:
		 *  the old hashes it imported or ignored (§10.1 "Ignore is remembered in projects.json"). */
		std::vector<std::string> PendingImports;
		std::vector<std::string> DecidedImports;
		json::Value Raw;                    // the whole object as read: unknown keys survive a rewrite (§10.6)
	};

	struct DREAMCORE_API ProjectsIndex
	{
		bool bNewerSchema = false;          // used read-only, never rewritten (§10.6)
		json::Value Raw;                    // the whole document as read (unknown top-level keys survive)
		std::vector<ProjectEntry> Projects;

		const ProjectEntry* Find(std::string_view Hash) const;
		ProjectEntry* Find(std::string_view Hash);
		/** Adds or replaces the entry with this hash, keeping its unknown keys. */
		ProjectEntry& Upsert(const ProjectEntry& Entry);
	};

	/** False when the bytes are not a projects document (corrupt: the caller tries the .bak, then rebuilds, §10.11). */
	DREAMCORE_API bool ParseProjects(std::string_view Bytes, ProjectsIndex& Out);
	DREAMCORE_API std::string SerializeProjects(const ProjectsIndex& Index);

	/**
	 * §10.1 "Moved projects": entries with this project's name and another hash whose .uproject no longer exists (PathExists
	 * says false) and that this project has not decided on yet. bOurFolderIsNew: the offer starts only when this project's
	 * hash folder was created at this start; once offered, an old hash stays in Ours->PendingImports until Import or Ignore,
	 * so a session that ends without an answer offers it again.
	 */
	DREAMCORE_API std::vector<ProjectEntry> FindMovedProjects(const ProjectsIndex& Index, std::string_view OurHash, std::string_view OurName,
		bool bOurFolderIsNew, const std::function<bool(std::string_view)>& PathExists);
	/** §10.1 "The same project under another path": entries with this .uproject's file identity and another hash. */
	DREAMCORE_API std::vector<ProjectEntry> FindSameIdentity(const ProjectsIndex& Index, std::string_view OurHash, std::string_view FileId);

	/** "2026-10-31T01:23:45Z" from UTC milliseconds since 1970 (no clock here; the caller passes the time). */
	DREAMCORE_API std::string FormatIsoUtc(int64_t UtcMs);
	/** The inverse for the forms FormatIsoUtc writes (fractional seconds ignored); false otherwise. */
	DREAMCORE_API bool ParseIsoUtc(std::string_view Iso, int64_t& OutUtcMs);
}
