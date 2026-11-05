// SPDX-License-Identifier: Apache-2.0
// Dream.Repl.History: load robustness of the REPL history file (TDD §6.5 history, §6.7, T-REPL-007 subset, OUT-01, OUT-03).
// Each case writes a damaged <root>/Repl/history.jsonl on a private temp root and opens its own FDreamReplHistory, so the
// editor's history is untouched: a torn last line is dropped and cut off the file, so a later append is never glued onto
// it; a corrupt middle line sets the file aside as .corrupt; a missing file with its .1 generation is restored from it, and
// a damaged .1 is the file set aside; a history.jsonl that exists but cannot be read is never written that session, even
// once the other process lets it go (P2-out-04, P5-tst-01).
// Memory-only, as every editor opens it at engine init until its first tab spawn is the primary: nothing on disk changes,
// a damaged file is set aside only once the file is this editor's, entries another editor appended meanwhile are kept and
// this editor's join them, and an unchanged file gets appends, never a rewrite (P3-out-01, P3-out-05, A2-01). When the file
// becomes this editor's: one unreadable at engine init is read again and appended to, and one still unreadable is left as
// it is for the session; a changed file without the import flag is imported into and written whole; an unchanged file
// with nothing added is not written at all (P4-hsd-04). The switch of an unchanged file at the cap is a stat and an
// empty-pipe wait, timed against a 2 ms gate (A2-01).
// No Python and no Slate: it runs in a headless session too.
#include "Support/DreamTestUtil.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Output/DreamReplHistory.h"
#include "Support/DreamBotSteps.h"

namespace DreamReplHistorySpec
{
	const TCHAR* Header = TEXT("{\"format\":\"dream.repl-history\",\"schema\":1,\"importedConsoleHistory\":true}\n");

	FString Entry(const FString& Text)
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"t\":1,\"src\":\"repl\",\"text\":\"%s\"}\n"),
			*FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower), *Text);
	}

	FString MakeRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"), TEXT("History_") + DreamTest::UniqueSuffix()));
	}

	FString HistoryPath(const FString& Root)
	{
		return FPaths::Combine(Root, TEXT("Repl"), TEXT("history.jsonl"));
	}

	bool Has(const FDreamReplHistory& History, const FString& Text)
	{
		return History.Query(Text, EDreamHistoryMatch::Prefix, 0).ContainsByPredicate([&Text](const FDreamHistoryEntry& E) { return E.Text == Text; });
	}

	FString Load(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	/** The header line of a history file holds the import flag (§6.7: set even when nothing was imported). */
	bool HeaderSaysImported(const FString& Text)
	{
		const int32 End = Text.Find(TEXT("\n"));
		return Text.StartsWith(TEXT("{\"format\":\"dream.repl-history\""))
			&& Text.Left(End == INDEX_NONE ? Text.Len() : End).Contains(TEXT("\"importedConsoleHistory\":true"));
	}

	/** What another editor's primary history does between this editor's read and its tab spawn: one appended line (§6.5). */
	void AppendFromOutside(const FString& Path, const FString& Line)
	{
		FFileHelper::SaveStringToFile(Line, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
	}
}

BEGIN_DEFINE_SPEC(FDreamReplHistorySpec, "Dream.Repl.History", DreamTest::AnyEditorFlags)
	FString Root;
END_DEFINE_SPEC(FDreamReplHistorySpec)

void FDreamReplHistorySpec::Define()
{
	using namespace DreamReplHistorySpec;

	BeforeEach([this]()
	{
		Root = MakeRoot();
		IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT("Repl")), /*Tree*/ true);
	});

	AfterEach([this]()
	{
		IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true);
	});

	It("drops a torn last line and never glues a later entry onto it (OUT-01)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString First = TEXT("first_") + Sfx;
		const FString Second = TEXT("second_") + Sfx;
		const FString Torn = TEXT("{\"id\":\"9f\",\"t\":17");      // a kill during an append: no newline
		FFileHelper::SaveStringToFile(FString(Header) + Entry(First) + Torn, *HistoryPath(Root), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		{
			FDreamReplHistory History;
			History.Open(Root);
			TestTrue(TEXT("the entry before the torn line loads"), Has(History, First));
			History.WaitForWrites();
			FString Bytes;
			FFileHelper::LoadFileToString(Bytes, *HistoryPath(Root));
			TestTrue(TEXT("the file was rewritten without the torn tail"), !Bytes.Contains(Torn) && Bytes.EndsWith(TEXT("\n")));
			History.Add(Second, EDreamHistorySource::Repl);
			History.WaitForWrites();
			History.Close();
		}
		FDreamReplHistory Again;
		Again.Open(Root);
		TestFalse(TEXT("the next load is not corrupt"), IFileManager::Get().FileExists(*(HistoryPath(Root) + TEXT(".corrupt"))));
		TestTrue(TEXT("the first entry survives"), Has(Again, First));
		TestTrue(TEXT("the entry added after the torn line survives"), Has(Again, Second));
		Again.WaitForWrites();
		Again.Close();
	});

	It("sets a file with a corrupt middle line aside as .corrupt (TDD 6.7)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		FFileHelper::SaveStringToFile(FString(Header) + TEXT("not json at all\n") + Entry(TEXT("after_") + Sfx), *HistoryPath(Root),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FDreamReplHistory History;
		History.Open(Root);
		History.WaitForWrites();
		TestTrue(TEXT("history.jsonl.corrupt holds the old file"), IFileManager::Get().FileExists(*(HistoryPath(Root) + TEXT(".corrupt"))));
		TestFalse(TEXT("nothing of the corrupt file loads"), Has(History, TEXT("after_") + Sfx));
		TestTrue(TEXT("a new history.jsonl starts"), IFileManager::Get().FileExists(*HistoryPath(Root)));
		History.Close();
	});

	It("restores a missing history.jsonl from its .1 generation (T-REPL-007)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Kept = TEXT("kept_") + Sfx;
		FFileHelper::SaveStringToFile(FString(Header) + Entry(Kept), *(HistoryPath(Root) + TEXT(".1")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		{
			FDreamReplHistory History;
			History.Open(Root);
			TestTrue(TEXT("the .1 generation's entry loads"), Has(History, Kept));
			History.WaitForWrites();
			TestTrue(TEXT("history.jsonl is written again"), IFileManager::Get().FileExists(*HistoryPath(Root)));
			History.Close();
		}
		FDreamReplHistory Again;
		Again.Open(Root);
		TestTrue(TEXT("the entry is in history.jsonl now"), Has(Again, Kept));
		Again.Close();
	});

	It("sets a damaged .1 aside when it was the only generation (P2-out-04)", [this]()
	{
		const FString Damaged = TEXT("damaged backup ") + DreamTest::UniqueSuffix();
		FFileHelper::SaveStringToFile(Damaged + TEXT("\n"), *(HistoryPath(Root) + TEXT(".1")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FDreamReplHistory History;
		History.Open(Root);
		History.WaitForWrites();
		FString Aside;
		FFileHelper::LoadFileToString(Aside, *(HistoryPath(Root) + TEXT(".corrupt")));
		TestTrue(TEXT("history.jsonl.corrupt holds the damaged .1"), Aside.Contains(Damaged));
		FString Backup;
		FFileHelper::LoadFileToString(Backup, *(HistoryPath(Root) + TEXT(".1")));
		TestFalse(TEXT("no rewrite replaced the damaged bytes in place"), Backup.Contains(Damaged));
		TestTrue(TEXT("a new history.jsonl starts"), IFileManager::Get().FileExists(*HistoryPath(Root)));
		History.Close();
	});

	It("never replaces a history.jsonl it cannot read, nor writes it once it is let go (P2-out-04, P5-tst-01)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Original = FString(Header) + Entry(TEXT("newest_") + Sfx);
		FFileHelper::SaveStringToFile(Original, *HistoryPath(Root), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FFileHelper::SaveStringToFile(FString(Header) + Entry(TEXT("older_") + Sfx), *(HistoryPath(Root) + TEXT(".1")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		// Another process holds the file without read sharing (a backup or sync tool).
		IFileHandle* Lock = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*HistoryPath(Root), /*bAppend*/ true, /*bAllowRead*/ false);
		if (!TestNotNull(TEXT("the file is held without read sharing"), Lock))
		{
			return;
		}
		{
			FDreamReplHistory History;
			History.Open(Root);
			TestFalse(TEXT("the older .1 generation is not loaded in its place"), Has(History, TEXT("older_") + Sfx));
			History.Add(TEXT("typed_") + Sfx, EDreamHistorySource::Repl);
			TestTrue(TEXT("this session's history lives in memory"), Has(History, TEXT("typed_") + Sfx));
			History.WaitForWrites();
			// The other process lets the file go while this session is open: the session still never writes the file it
			// could not read (OUT-03), so the lock alone cannot keep the file intact here.
			delete Lock;
			History.Add(TEXT("later_") + Sfx, EDreamHistorySource::Repl);
			TestTrue(TEXT("an entry added after the file was let go lives in memory too"), Has(History, TEXT("later_") + Sfx));
			History.WaitForWrites();
			History.Close();
		}
		FString After;
		FFileHelper::LoadFileToString(After, *HistoryPath(Root));
		TestEqual(TEXT("history.jsonl is unchanged"), After, Original);
		FString Backup;
		FFileHelper::LoadFileToString(Backup, *(HistoryPath(Root) + TEXT(".1")));
		TestTrue(TEXT("the .1 generation is unchanged"), Backup.Contains(TEXT("older_") + Sfx) && !Backup.Contains(TEXT("newest_") + Sfx));
		TestFalse(TEXT("no rewrite was attempted"), IFileManager::Get().FileExists(*(HistoryPath(Root) + TEXT(".tmp"))));
	});

	It("keeps what another editor appended while this one was memory-only, and adds its own entries (P3-out-01)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		const FString Original = FString(Header) + Entry(TEXT("first_") + Sfx);
		FFileHelper::SaveStringToFile(Original, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		{
			FDreamReplHistory History;
			History.SetMemoryOnly(true);
			History.Open(Root);
			History.WaitForWrites();
			TestEqual(TEXT("a memory-only open writes nothing"), Load(Path), Original);
			History.Add(TEXT("mine_") + Sfx, EDreamHistorySource::Repl);
			History.WaitForWrites();
			TestEqual(TEXT("an entry added while memory-only is not written yet"), Load(Path), Original);
			AppendFromOutside(Path, Entry(TEXT("outside_") + Sfx));
			History.SetMemoryOnly(false);
			History.WaitForWrites();
			TestTrue(TEXT("the other editor's entry is in this editor's history"), Has(History, TEXT("outside_") + Sfx));
			TestTrue(TEXT("this editor's own entry stays"), Has(History, TEXT("mine_") + Sfx));
			History.Close();
		}
		TestFalse(TEXT("no rewrite rotated the file to .1"), IFileManager::Get().FileExists(*(Path + TEXT(".1"))));
		FDreamReplHistory Again;
		Again.Open(Root);
		TestTrue(TEXT("the first entry is on disk"), Has(Again, TEXT("first_") + Sfx));
		TestTrue(TEXT("the other editor's entry is on disk"), Has(Again, TEXT("outside_") + Sfx));
		TestTrue(TEXT("this editor's entry is on disk"), Has(Again, TEXT("mine_") + Sfx));
		Again.WaitForWrites();
		Again.Close();
	});

	It("appends to an unchanged file when it becomes this editor's, and never rewrites it (A2-01)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		const FString Original = FString(Header) + Entry(TEXT("first_") + Sfx);
		FFileHelper::SaveStringToFile(Original, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FDreamReplHistory History;
		History.SetMemoryOnly(true);
		History.Open(Root);
		History.Add(TEXT("mine_") + Sfx, EDreamHistorySource::Repl);
		History.SetMemoryOnly(false);
		History.WaitForWrites();
		const FString After = Load(Path);
		TestTrue(TEXT("the file keeps its bytes and gains the entry as one appended line"),
			After.StartsWith(Original) && After.Mid(Original.Len()).Contains(TEXT("mine_") + Sfx));
		TestFalse(TEXT("no rewrite rotated the file to .1"), IFileManager::Get().FileExists(*(Path + TEXT(".1"))));
		History.Close();
	});

	It("sets a corrupt file aside only once the file is this editor's (P3-out-05)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		const FString Damaged = FString(Header) + TEXT("not json at all\n") + Entry(TEXT("after_") + Sfx);
		FFileHelper::SaveStringToFile(Damaged, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FDreamReplHistory History;
		History.SetMemoryOnly(true);
		History.Open(Root);
		History.WaitForWrites();
		TestEqual(TEXT("a memory-only open leaves the corrupt file in place"), Load(Path), Damaged);
		TestFalse(TEXT("and sets nothing aside"), IFileManager::Get().FileExists(*(Path + TEXT(".corrupt"))));
		History.Add(TEXT("mine_") + Sfx, EDreamHistorySource::Repl);
		History.SetMemoryOnly(false);
		History.WaitForWrites();
		TestEqual(TEXT("history.jsonl.corrupt holds the damaged bytes"), Load(Path + TEXT(".corrupt")), Damaged);
		const FString Fresh = Load(Path);
		TestTrue(TEXT("a new history.jsonl starts with the import flag set"), HeaderSaysImported(Fresh));
		TestTrue(TEXT("and holds the entry added while memory-only"), Fresh.Contains(TEXT("mine_") + Sfx));
		TestFalse(TEXT("nothing of the corrupt file loads"), Has(History, TEXT("after_") + Sfx));
		History.Close();
	});

	It("keeps a good file another editor wrote over a corrupt one before this one became writable (P3-out-01)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		FFileHelper::SaveStringToFile(FString(Header) + TEXT("not json at all\n"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FDreamReplHistory History;
		History.SetMemoryOnly(true);
		History.Open(Root);
		History.Add(TEXT("mine_") + Sfx, EDreamHistorySource::Repl);
		// The other editor, primary meanwhile, set the corrupt file aside and started a good one.
		IFileManager::Get().Move(*(Path + TEXT(".corrupt")), *Path, /*bReplace*/ true);
		const FString Good = FString(Header) + Entry(TEXT("good_") + Sfx);
		FFileHelper::SaveStringToFile(Good, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const FString AsideBefore = Load(Path + TEXT(".corrupt"));
		History.SetMemoryOnly(false);
		History.WaitForWrites();
		TestEqual(TEXT("the good file is not set aside over the corrupt copy"), Load(Path + TEXT(".corrupt")), AsideBefore);
		TestTrue(TEXT("its entry loads"), Has(History, TEXT("good_") + Sfx));
		TestTrue(TEXT("this editor's entry joins it"), Has(History, TEXT("mine_") + Sfx));
		const FString After = Load(Path);
		TestTrue(TEXT("the good file keeps its bytes and gains this editor's entry"), After.StartsWith(Good) && After.Contains(TEXT("mine_") + Sfx));
		History.Close();
	});

	It("sets a damaged .1 aside only once the file is this editor's (P3-out-05)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		const FString Damaged = TEXT("damaged backup ") + Sfx + TEXT("\n");
		FFileHelper::SaveStringToFile(Damaged, *(Path + TEXT(".1")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FDreamReplHistory History;
		History.SetMemoryOnly(true);
		History.Open(Root);
		History.WaitForWrites();
		TestEqual(TEXT("a memory-only open leaves the damaged .1 in place"), Load(Path + TEXT(".1")), Damaged);
		TestFalse(TEXT("and writes no history.jsonl"), IFileManager::Get().FileExists(*Path));
		TestFalse(TEXT("and sets nothing aside"), IFileManager::Get().FileExists(*(Path + TEXT(".corrupt"))));
		History.Add(TEXT("mine_") + Sfx, EDreamHistorySource::Repl);
		History.SetMemoryOnly(false);
		History.WaitForWrites();
		TestEqual(TEXT("history.jsonl.corrupt holds the damaged .1"), Load(Path + TEXT(".corrupt")), Damaged);
		const FString Fresh = Load(Path);
		TestTrue(TEXT("a new history.jsonl starts with the import flag set"), HeaderSaysImported(Fresh));
		TestTrue(TEXT("and holds the entry added while memory-only"), Fresh.Contains(TEXT("mine_") + Sfx));
		History.Close();
	});

	It("reads a file it could not read at engine init again once the file is this editor's, and appends to it (P4-hsd-04)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		const FString Original = FString(Header) + Entry(TEXT("newest_") + Sfx);
		const FString Older = FString(Header) + Entry(TEXT("older_") + Sfx);
		FFileHelper::SaveStringToFile(Original, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FFileHelper::SaveStringToFile(Older, *(Path + TEXT(".1")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		// Another process holds the file without read sharing at engine init and lets it go before the first tab spawn.
		IFileHandle* Lock = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path, /*bAppend*/ true, /*bAllowRead*/ false);
		if (!TestNotNull(TEXT("the file is held without read sharing"), Lock))
		{
			return;
		}
		FDreamReplHistory History;
		History.SetMemoryOnly(true);
		History.Open(Root);
		History.Add(TEXT("typed_") + Sfx, EDreamHistorySource::Repl);
		delete Lock;
		History.SetMemoryOnly(false);
		History.WaitForWrites();
		TestTrue(TEXT("the file's entry loads once it can be read"), Has(History, TEXT("newest_") + Sfx));
		TestTrue(TEXT("the entry typed meanwhile stays"), Has(History, TEXT("typed_") + Sfx));
		TestFalse(TEXT("the older .1 generation is not loaded"), Has(History, TEXT("older_") + Sfx));
		const FString After = Load(Path);
		TestTrue(TEXT("the file keeps its bytes and gains the typed entry as an appended line"),
			After.StartsWith(Original) && After.Mid(Original.Len()).Contains(TEXT("typed_") + Sfx));
		TestEqual(TEXT("the .1 generation is unchanged"), Load(Path + TEXT(".1")), Older);
		TestFalse(TEXT("nothing is set aside"), IFileManager::Get().FileExists(*(Path + TEXT(".corrupt"))));
		History.Close();
	});

	It("leaves a file that is still unreadable when it becomes this editor's as it is, also once it is let go (P4-hsd-04, P5-tst-01, OUT-03)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		const FString Original = FString(Header) + Entry(TEXT("newest_") + Sfx);
		const FString Older = FString(Header) + Entry(TEXT("older_") + Sfx);
		FFileHelper::SaveStringToFile(Original, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FFileHelper::SaveStringToFile(Older, *(Path + TEXT(".1")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		IFileHandle* Lock = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path, /*bAppend*/ true, /*bAllowRead*/ false);
		if (!TestNotNull(TEXT("the file is held without read sharing"), Lock))
		{
			return;
		}
		{
			FDreamReplHistory History;
			History.SetMemoryOnly(true);
			History.Open(Root);
			History.Add(TEXT("typed_") + Sfx, EDreamHistorySource::Repl);
			History.SetMemoryOnly(false);
			History.Add(TEXT("later_") + Sfx, EDreamHistorySource::Repl);
			History.WaitForWrites();
			// The other process lets the file go later in the session: this session still keeps its history in memory and
			// neither appends to nor replaces the file it never read (OUT-03, A39), so the lock alone cannot keep it intact.
			delete Lock;
			History.Add(TEXT("released_") + Sfx, EDreamHistorySource::Repl);
			TestTrue(TEXT("this session's history lives in memory"), Has(History, TEXT("typed_") + Sfx) && Has(History, TEXT("later_") + Sfx)
				&& Has(History, TEXT("released_") + Sfx));
			History.Clear();              // the rewrite path too, on the file that is no longer held
			History.WaitForWrites();
			History.Close();
		}
		TestEqual(TEXT("history.jsonl is unchanged"), Load(Path), Original);
		TestEqual(TEXT("the .1 generation is unchanged"), Load(Path + TEXT(".1")), Older);
		TestFalse(TEXT("nothing is set aside"), IFileManager::Get().FileExists(*(Path + TEXT(".corrupt"))));
		TestFalse(TEXT("no rewrite was attempted"), IFileManager::Get().FileExists(*(Path + TEXT(".tmp"))));
	});

	It("imports into a changed file without the import flag and writes it whole once the file is this editor's (P4-hsd-04)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		FFileHelper::SaveStringToFile(FString(Header) + Entry(TEXT("first_") + Sfx), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FDreamReplHistory History;
		History.SetMemoryOnly(true);
		History.Open(Root);
		// Another editor, primary meanwhile, wrote a file whose header has no import flag yet (and a different size).
		const FString NotImported = FString(TEXT("{\"format\":\"dream.repl-history\",\"schema\":1,\"importedConsoleHistory\":false}\n"))
			+ Entry(TEXT("outside_") + Sfx);
		FFileHelper::SaveStringToFile(NotImported, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		History.SetMemoryOnly(false);
		History.WaitForWrites();
		const FString After = Load(Path);
		TestTrue(TEXT("the file is written whole with the import flag set"), HeaderSaysImported(After));
		TestTrue(TEXT("the other editor's entry loads"), Has(History, TEXT("outside_") + Sfx));
		TestTrue(TEXT("and is in the rewritten file"), After.Contains(TEXT("outside_") + Sfx));
		TestFalse(TEXT("nothing of the older read is written back"), After.Contains(TEXT("first_") + Sfx));
		History.Close();
	});

	It("writes nothing to an unchanged file when it becomes this editor's with nothing added (P4-hsd-04, A2-01)", [this]()
	{
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		const FString Original = FString(Header) + Entry(TEXT("first_") + Sfx);
		FFileHelper::SaveStringToFile(Original, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FDreamReplHistory History;
		History.SetMemoryOnly(true);
		History.Open(Root);
		History.SetMemoryOnly(false);
		History.WaitForWrites();
		TestEqual(TEXT("history.jsonl keeps its bytes"), Load(Path), Original);
		TestFalse(TEXT("no rewrite rotated the file to .1"), IFileManager::Get().FileExists(*(Path + TEXT(".1"))));
		TestTrue(TEXT("its entry loads"), Has(History, TEXT("first_") + Sfx));
		History.Close();
	});

	It("switches an unchanged full history to this editor's with one stat and an empty-pipe wait, <= 2 ms (A2-01 timing gate)", [this]()
	{
		// A file at the cap (10,000 entries), read at engine init; the first tab spawn's switch must not read it again when it is
		// unchanged (§6.5, A39). The changed-file switch, which reads it again, is measured too and reported for scale.
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Path = HistoryPath(Root);
		FString Full = FString(Header);
		for (int32 Index = 0; Index < FDreamReplHistory::Cap; ++Index)
		{
			Full += Entry(FString::Printf(TEXT("entry_%d_%s"), Index, *Sfx));
		}
		FFileHelper::SaveStringToFile(Full, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const auto MeasureSwitch = [this, &Path, &Sfx](bool bChangeFirst, int32 Trial) -> double
		{
			FDreamReplHistory History;
			History.SetMemoryOnly(true);
			History.Open(Root);
			History.WaitForWrites();
			if (bChangeFirst)
			{
				// Another editor appended one entry since the read (size and time change).
				AppendFromOutside(Path, Entry(FString::Printf(TEXT("outside_%d_%s"), Trial, *Sfx)));
			}
			const double Start = FPlatformTime::Seconds();
			History.SetMemoryOnly(false);
			const double Seconds = FPlatformTime::Seconds() - Start;
			History.WaitForWrites();
			History.Close();
			return Seconds;
		};
		TArray<double> Unchanged;
		TArray<double> Changed;
		for (int32 Trial = 0; Trial < 5; ++Trial)
		{
			Unchanged.Add(MeasureSwitch(false, Trial));
			Changed.Add(MeasureSwitch(true, Trial));
		}
		const double UnchangedMedian = DreamTest::Percentile(Unchanged, 50.0);
		AddInfo(FString::Printf(TEXT("switch of an unchanged %d-entry history: %s; of a changed one (read again): %s"), FDreamReplHistory::Cap,
			*DreamTest::DescribeMs(Unchanged), *DreamTest::DescribeMs(Changed)));
		TestTrue(*FString::Printf(TEXT("the unchanged switch takes <= 2 ms (median %.3f ms of 5)"), UnchangedMedian * 1000.0), UnchangedMedian <= 0.002);
		TestFalse(TEXT("no switch rewrote the file (no .1 generation)"), IFileManager::Get().FileExists(*(Path + TEXT(".1"))));
	});
}
