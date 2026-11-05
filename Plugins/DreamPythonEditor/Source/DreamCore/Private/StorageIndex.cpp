// SPDX-License-Identifier: Apache-2.0
// Dream core: namespace names, lock content, owner.json and projects.json (TDD §10.1, §10.5, §10.6). Data only.
#include "dream/StorageIndex.h"

#include <algorithm>
#include <cstdio>

namespace dream::storage_detail
{
	using json::Value;
	using storage::ProjectEntry;

	bool AllDigits(std::string_view Text)
	{
		if (Text.empty() || Text.size() > 19)
		{
			return false;
		}
		return std::all_of(Text.begin(), Text.end(), [](char C) { return C >= '0' && C <= '9'; });
	}

	uint64_t ToNumber(std::string_view Digits)
	{
		uint64_t Out = 0;
		for (const char C : Digits)
		{
			Out = Out * 10 + static_cast<uint64_t>(C - '0');
		}
		return Out;
	}

	std::vector<std::string> StringList(const Value& Array)
	{
		std::vector<std::string> Out;
		for (size_t Index = 0; Index < Array.Size(); ++Index)
		{
			if (Array.At(Index).IsString() && !Array.At(Index).AsString().empty())
			{
				Out.push_back(Array.At(Index).AsString());
			}
		}
		return Out;
	}

	Value ToArray(const std::vector<std::string>& Items)
	{
		Value Out = Value::MakeArray();
		for (const std::string& Item : Items)
		{
			Out.Append(Value::MakeString(Item));
		}
		return Out;
	}

	bool Contains(const std::vector<std::string>& Items, std::string_view Item)
	{
		return std::find(Items.begin(), Items.end(), Item) != Items.end();
	}

	/** Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm, proleptic Gregorian). */
	void CivilFromDays(int64_t Days, int64_t& Y, unsigned& M, unsigned& D)
	{
		Days += 719468;
		const int64_t Era = (Days >= 0 ? Days : Days - 146096) / 146097;
		const unsigned Doe = static_cast<unsigned>(Days - Era * 146097);
		const unsigned Yoe = (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
		Y = static_cast<int64_t>(Yoe) + Era * 400;
		const unsigned Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
		const unsigned Mp = (5 * Doy + 2) / 153;
		D = Doy - (153 * Mp + 2) / 5 + 1;
		M = Mp < 10 ? Mp + 3 : Mp - 9;
		Y += M <= 2 ? 1 : 0;
	}

	int64_t DaysFromCivil(int64_t Y, unsigned M, unsigned D)
	{
		Y -= M <= 2 ? 1 : 0;
		const int64_t Era = (Y >= 0 ? Y : Y - 399) / 400;
		const unsigned Yoe = static_cast<unsigned>(Y - Era * 400);
		const unsigned Doy = (153 * (M > 2 ? M - 3 : M + 9) + 2) / 5 + D - 1;
		const unsigned Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
		return Era * 146097 + static_cast<int64_t>(Doe) - 719468;
	}

	bool ParseFixed(std::string_view Text, size_t At, size_t Count, int64_t& Out)
	{
		if (At + Count > Text.size())
		{
			return false;
		}
		const std::string_view Part = Text.substr(At, Count);
		if (!AllDigits(Part))
		{
			return false;
		}
		Out = static_cast<int64_t>(ToNumber(Part));
		return true;
	}

	ProjectEntry EntryFrom(const std::string& Hash, const Value& Object)
	{
		ProjectEntry E;
		E.Hash = Hash;
		E.UProject = Object.Get("uproject").AsString();
		E.Name = Object.Get("name").AsString();
		E.FileId = Object.Get("file_id").AsString();
		E.Root = Object.Get("root").AsString();
		E.LastSeenUtc = Object.Get("last_seen_utc").AsString();
		E.PendingImports = StringList(Object.Get("pending_imports"));
		E.DecidedImports = StringList(Object.Get("decided_imports"));
		E.Raw = Object;
		return E;
	}

	Value EntryTo(const ProjectEntry& E)
	{
		Value Object = E.Raw.IsObject() ? E.Raw : Value::MakeObject();
		Object.Set("uproject", Value::MakeString(E.UProject));
		Object.Set("name", Value::MakeString(E.Name));
		Object.Set("file_id", Value::MakeString(E.FileId));
		Object.Set("root", Value::MakeString(E.Root));
		Object.Set("last_seen_utc", Value::MakeString(E.LastSeenUtc));
		if (E.PendingImports.empty())
		{
			Object.Remove("pending_imports");
		}
		else
		{
			Object.Set("pending_imports", ToArray(E.PendingImports));
		}
		if (E.DecidedImports.empty())
		{
			Object.Remove("decided_imports");
		}
		else
		{
			Object.Set("decided_imports", ToArray(E.DecidedImports));
		}
		return Object;
	}
}

namespace dream::storage
{
	using json::Value;
	using namespace storage_detail;

	std::string MakeSecondaryNamespace(uint32_t Pid, int64_t ProcessStartMs)
	{
		return "i-" + std::to_string(Pid) + "-" + std::to_string(ProcessStartMs < 0 ? 0 : ProcessStartMs);
	}

	std::string MakeCompatNamespace(uint16_t Major, uint16_t Minor)
	{
		return "compat-" + std::to_string(Major) + "." + std::to_string(Minor);
	}

	std::string MakeImportedNamespace(std::string_view OldHash, std::string_view SourceNamespace)
	{
		// The old hash comes from a projects.json key nobody validated: it must be a hash Dream could have made (a safe name
		// without '.', so the '.' below separates it unambiguously), and the result must be a safe folder name.
		if (!IsSafeName(OldHash) || OldHash.find('.') != std::string_view::npos)
		{
			return std::string();
		}
		std::string Out = "imported-";
		Out.append(OldHash);
		if (!SourceNamespace.empty() && SourceNamespace != MainNamespace)
		{
			if (!IsSafeName(SourceNamespace))
			{
				return std::string();
			}
			Out += ".";
			Out.append(SourceNamespace);
		}
		return IsSafeName(Out) ? Out : std::string();
	}

	bool IsSafeName(std::string_view Name)
	{
		// Windows strips a trailing '.' or ' ' from a folder name, so such a name would not be the folder it names.
		if (Name.empty() || Name.size() > 128 || Name == "." || Name == ".." || Name.back() == '.')
		{
			return false;
		}
		return std::all_of(Name.begin(), Name.end(), [](char C)
		{
			return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '.' || C == '_' || C == '-';
		});
	}

	NamespaceName ParseNamespace(std::string_view Name)
	{
		NamespaceName Out;
		if (!IsSafeName(Name))
		{
			return Out;
		}
		if (Name == MainNamespace)
		{
			Out.Kind = NamespaceKind::Main;
			return Out;
		}
		Out.Kind = NamespaceKind::Other;
		if (Name.substr(0, 2) == "i-")
		{
			const std::string_view Rest = Name.substr(2);
			const size_t Dash = Rest.find('-');
			if (Dash != std::string_view::npos && AllDigits(Rest.substr(0, Dash)) && AllDigits(Rest.substr(Dash + 1)))
			{
				const uint64_t Pid = ToNumber(Rest.substr(0, Dash));
				if (Pid <= 0xFFFFFFFFull)
				{
					Out.Kind = NamespaceKind::Secondary;
					Out.Pid = static_cast<uint32_t>(Pid);
					Out.ProcessStartMs = static_cast<int64_t>(ToNumber(Rest.substr(Dash + 1)));
				}
			}
			return Out;
		}
		if (Name.substr(0, 7) == "compat-" && Name.size() > 7)
		{
			Out.Kind = NamespaceKind::Compat;
			Out.Version = std::string(Name.substr(7));
			return Out;
		}
		if (Name.substr(0, 9) == "imported-" && Name.size() > 9)
		{
			// "<Name>-<hex>" is the old hash; a project hash never holds a '.' (U29 names are [A-Za-z0-9_-]), so the first '.'
			// starts the namespace it came from, whatever dashes the project name or that namespace hold.
			const std::string_view Rest = Name.substr(9);
			const size_t Dot = Rest.find('.');
			if (Dot == 0 || Dot == Rest.size() - 1)
			{
				return Out;   // an empty hash or namespace: not one Dream made
			}
			Out.Kind = NamespaceKind::Imported;
			Out.OldHash = std::string(Rest.substr(0, Dot));
			Out.SourceNamespace = Dot == std::string_view::npos ? std::string(MainNamespace) : std::string(Rest.substr(Dot + 1));
			return Out;
		}
		return Out;
	}

	std::string SerializeLock(const LockContent& Content)
	{
		Value Object = Value::MakeObject();
		Object.Set("schema", Value::MakeInt(Content.Schema));
		Object.Set("pid", Value::MakeInt(Content.Pid));
		Object.Set("process_start", Value::MakeInt(Content.ProcessStartMs));
		Object.Set("host", Value::MakeString(Content.Host));
		Object.Set("plugin_version", Value::MakeString(Content.PluginVersion));
		Object.Set("namespace", Value::MakeString(Content.Namespace));
		Object.Set("utc", Value::MakeInt(Content.UtcMs));
		return json::Serialize(Object) + "\n";
	}

	bool ParseLock(std::string_view Bytes, LockContent& Out)
	{
		Out = LockContent();
		Value Object;
		if (Bytes.empty() || !json::Parse(Bytes, Object) || !Object.IsObject())
		{
			return false;
		}
		const int64_t Pid = Object.Get("pid").AsInt(-1);
		if (Pid <= 0 || Pid > 0xFFFFFFFFll)
		{
			return false;
		}
		Out.Schema = static_cast<int>(Object.Get("schema").AsInt(LockSchema));
		Out.Pid = static_cast<uint32_t>(Pid);
		Out.ProcessStartMs = Object.Get("process_start").AsInt(0);
		Out.Host = Object.Get("host").AsString();
		Out.PluginVersion = Object.Get("plugin_version").AsString();
		Out.Namespace = Object.Get("namespace").AsString();
		Out.UtcMs = Object.Get("utc").AsInt(0);
		return true;
	}

	OwnerCheck CheckOwner(std::string_view Bytes, bool bFileExists, std::string_view Sid)
	{
		if (!bFileExists)
		{
			return OwnerCheck::Missing;
		}
		Value Object;
		if (!json::Parse(Bytes, Object) || !Object.IsObject())
		{
			return OwnerCheck::Unreadable;
		}
		const std::string& Stored = Object.Get("sid").AsString();
		if (Stored.empty())
		{
			return OwnerCheck::Unreadable;
		}
		// SIDs are case-insensitive ("S-1-5-21-..."); compare them so.
		if (Stored.size() != Sid.size())
		{
			return OwnerCheck::Other;
		}
		for (size_t Index = 0; Index < Stored.size(); ++Index)
		{
			const char A = Stored[Index] >= 'a' && Stored[Index] <= 'z' ? static_cast<char>(Stored[Index] - 32) : Stored[Index];
			const char B = Sid[Index] >= 'a' && Sid[Index] <= 'z' ? static_cast<char>(Sid[Index] - 32) : Sid[Index];
			if (A != B)
			{
				return OwnerCheck::Other;
			}
		}
		return OwnerCheck::Mine;
	}

	std::string SerializeOwner(std::string_view Sid, std::string_view NowIso)
	{
		Value Object = Value::MakeObject();
		Object.Set("schema", Value::MakeInt(OwnerSchema));
		Object.Set("sid", Value::MakeString(std::string(Sid)));
		Object.Set("created_utc", Value::MakeString(std::string(NowIso)));
		return json::Serialize(Object, true) + "\n";
	}

	const ProjectEntry* ProjectsIndex::Find(std::string_view Hash) const
	{
		for (const ProjectEntry& E : Projects)
		{
			if (E.Hash == Hash)
			{
				return &E;
			}
		}
		return nullptr;
	}

	ProjectEntry* ProjectsIndex::Find(std::string_view Hash)
	{
		for (ProjectEntry& E : Projects)
		{
			if (E.Hash == Hash)
			{
				return &E;
			}
		}
		return nullptr;
	}

	ProjectEntry& ProjectsIndex::Upsert(const ProjectEntry& Entry)
	{
		if (ProjectEntry* Existing = Find(Entry.Hash))
		{
			Value KeptRaw = Existing->Raw;
			*Existing = Entry;
			if (!Entry.Raw.IsObject())
			{
				Existing->Raw = KeptRaw;   // keep the unknown keys of what was read
			}
			return *Existing;
		}
		Projects.push_back(Entry);
		return Projects.back();
	}

	bool ParseProjects(std::string_view Bytes, ProjectsIndex& Out)
	{
		Out = ProjectsIndex();
		Value Doc;
		if (!json::Parse(Bytes, Doc) || !Doc.IsObject())
		{
			return false;
		}
		const Value* Projects = Doc.Find("projects");
		if (Projects != nullptr && !Projects->IsObject())
		{
			return false;
		}
		Out.bNewerSchema = Doc.Get("schema").AsInt(ProjectsSchema) > ProjectsSchema;
		if (Projects != nullptr)
		{
			for (const Value::Member& Member : Projects->Members())
			{
				if (Member.Val.IsObject() && !Member.Key.empty())
				{
					Out.Projects.push_back(EntryFrom(Member.Key, Member.Val));
				}
			}
		}
		Out.Raw = std::move(Doc);
		return true;
	}

	std::string SerializeProjects(const ProjectsIndex& Index)
	{
		Value Doc = Index.Raw.IsObject() ? Index.Raw : Value::MakeObject();
		if (!Index.bNewerSchema)
		{
			Doc.Set("schema", Value::MakeInt(ProjectsSchema));
		}
		Value Projects = Value::MakeObject();
		for (const ProjectEntry& E : Index.Projects)
		{
			Projects.Set(E.Hash, EntryTo(E));
		}
		Doc.Set("projects", std::move(Projects));
		return json::Serialize(Doc, true) + "\n";
	}

	std::vector<ProjectEntry> FindMovedProjects(const ProjectsIndex& Index, std::string_view OurHash, std::string_view OurName,
		bool bOurFolderIsNew, const std::function<bool(std::string_view)>& PathExists)
	{
		std::vector<ProjectEntry> Out;
		const ProjectEntry* Ours = Index.Find(OurHash);
		for (const ProjectEntry& E : Index.Projects)
		{
			if (E.Hash == OurHash || E.Name != OurName || E.UProject.empty())
			{
				continue;
			}
			const bool bPending = Ours != nullptr && Contains(Ours->PendingImports, E.Hash);
			const bool bDecided = Ours != nullptr && Contains(Ours->DecidedImports, E.Hash);
			if (bDecided || (!bOurFolderIsNew && !bPending))
			{
				continue;
			}
			if (PathExists && PathExists(E.UProject))
			{
				continue;           // still there: another checkout of the same name, not a moved project
			}
			Out.push_back(E);
		}
		return Out;
	}

	std::vector<ProjectEntry> FindSameIdentity(const ProjectsIndex& Index, std::string_view OurHash, std::string_view FileId)
	{
		std::vector<ProjectEntry> Out;
		if (FileId.empty())
		{
			return Out;
		}
		for (const ProjectEntry& E : Index.Projects)
		{
			if (E.Hash != OurHash && E.FileId == FileId)
			{
				Out.push_back(E);
			}
		}
		return Out;
	}

	std::string FormatIsoUtc(int64_t UtcMs)
	{
		const int64_t Seconds = (UtcMs >= 0 ? UtcMs : UtcMs - 999) / 1000;
		const int64_t Days = (Seconds >= 0 ? Seconds : Seconds - 86399) / 86400;
		const int64_t InDay = Seconds - Days * 86400;
		int64_t Y = 1970;
		unsigned M = 1;
		unsigned D = 1;
		CivilFromDays(Days, Y, M, D);
		char Buffer[40];
		std::snprintf(Buffer, sizeof(Buffer), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(Y), M, D,
			static_cast<long long>(InDay / 3600), static_cast<long long>((InDay / 60) % 60), static_cast<long long>(InDay % 60));
		return Buffer;
	}

	bool ParseIsoUtc(std::string_view Iso, int64_t& OutUtcMs)
	{
		// YYYY-MM-DDTHH:MM:SS[.fff]Z
		int64_t Y = 0, M = 0, D = 0, H = 0, Min = 0, S = 0;
		if (Iso.size() < 20 || Iso[4] != '-' || Iso[7] != '-' || (Iso[10] != 'T' && Iso[10] != ' ') || Iso[13] != ':' || Iso[16] != ':'
			|| Iso.back() != 'Z' || !ParseFixed(Iso, 0, 4, Y) || !ParseFixed(Iso, 5, 2, M) || !ParseFixed(Iso, 8, 2, D)
			|| !ParseFixed(Iso, 11, 2, H) || !ParseFixed(Iso, 14, 2, Min) || !ParseFixed(Iso, 17, 2, S))
		{
			return false;
		}
		if (M < 1 || M > 12 || D < 1 || D > 31 || H > 23 || Min > 59 || S > 60)
		{
			return false;
		}
		const int64_t Days = DaysFromCivil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D));
		OutUtcMs = ((Days * 24 + H) * 60 + Min) * 60000 + S * 1000;
		return true;
	}
}
