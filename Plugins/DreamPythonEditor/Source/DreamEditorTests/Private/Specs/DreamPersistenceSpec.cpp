// SPDX-License-Identifier: Apache-2.0
// Slice-B persistence specs (TDD §10.2.3, §10.4, §10.7, §10.8):
//  - Dream.Persistence.History (any editor, private root): an entry per save (ten saves, ten entries), identical runs
//    merged into one entry with every run number, reload and closed-scratch entries, texts loaded back from their
//    deduplicated blobs (T-PER-017); Clear removes every entry and blob and the journal still recovers (T-PER-018);
//    retention tombstones old entries and sweeps their blobs with a shifted clock (T-PER-010 at tier 4); an index file held
//    by another program pauses history, the entries wait and land once it is released (review r2 persist-r2-M2); a scratch tab
//    closed just before exit keeps its closed entry (the bounded exit drain, review r1); a pause ends on its own after the
//    release, and a held index that proves a newer format is read only (review r3).
//  - Dream.Persistence.Writer (any editor, private root): a rotation's checkpoint is written in slices while commits go on,
//    and the next session reads the newest text from the new generation (B10, §10.2.3); a generation the recovery could
//    not read is set aside before the session's own starts, never deleted (review r1 persist-M2); a pre-run commit is served
//    while a history slice waits out a held file (review r1, §10.2.1).
//  - Dream.Persistence.SafeMode (any editor, private root): the restore guard's protocol, the automation exemption, the
//    10 s clear and the small-file rules (§10.8, T-PER-033/034 shape); this editor's own guard did not put it in safe mode.
//  - Dream.Persistence.Recovery (attended, the Dream tab): a counted scratch buffer waits over its clean text with the
//    pending bar and Restore is one Ctrl+Z (B16, T-PER-029 shape); Discard moves the text into local history (T-PER-031);
//    the Review panel lists the pending buffers and Restore as new tab opens the recovered text (§10.7.3); Leave safe mode
//    applies the skipped restore (§10.8). The recovered sessions are synthetic and use fresh buffer ids; the specs close
//    every buffer they open.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/DreamUndoHistory.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamLocalHistory.h"
#include "Persistence/DreamRestoreGuard.h"
#include "Framework/Application/SlateApplication.h"
#include "Shell/SDreamTab.h"
#include "dream/HistoryFormat.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace DreamPersistenceSpec
{
	constexpr int64 DayMs = 24ll * 60 * 60 * 1000;

	struct FPrivate
	{
		FDreamStorageRoot Root;
		TSharedPtr<FDreamJournal> Journal;
		TSharedPtr<FDreamLocalHistory> History;
		bool bScanned = false;
		FDreamRecoveredSession Session;
	};

	FDreamStorageRoot MakePrivateRoot()
	{
		const FString Id = DreamTest::UniqueSuffix();
		FDreamStorageRoot Root;
		Root.Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"), Id));
		Root.ProjectHash = FString::Printf(TEXT("DreamTier4-%s"), *Id.Left(6));
		return Root;
	}

	/** A journal and its history on the private root, the recovery scan queued; the history is created before Open, as the
	 *  module creates it, so commits carry their history notes. */
	bool OpenAndScan(FAutomationTestBase& Test, FPrivate& State)
	{
		IFileManager::Get().MakeDirectory(*State.Root.Root, /*Tree*/ true);
		State.bScanned = false;
		State.Session = FDreamRecoveredSession();
		State.Journal = MakeShared<FDreamJournal>();
		State.History = MakeShared<FDreamLocalHistory>(*State.Journal);
		EDreamInstanceRole Role = EDreamInstanceRole::Secondary;
		if (!State.Journal->Open(State.Root, Role))
		{
			Test.AddError(FString::Printf(TEXT("Open failed on %s"), *State.Root.Root));
			return false;
		}
		FPrivate* Ptr = &State;
		State.Journal->RequestRecoveryScan(FDreamOnRecoveryScan::CreateLambda([Ptr](FDreamRecoveredSession& Session) -> bool
		{
			Ptr->Session = MoveTemp(Session);
			Ptr->bScanned = true;
			return true;
		}));
		return true;
	}

	void Close(FPrivate& State, bool bClean = true)
	{
		if (State.History.IsValid())
		{
			State.History->Shutdown();
		}
		if (State.Journal.IsValid() && State.Journal->IsOpen())
		{
			if (bClean)
			{
				State.Journal->Shutdown();
			}
			else
			{
				State.Journal->CloseWithoutCleanForTest();
			}
		}
		State.History.Reset();
		State.Journal.Reset();
	}

	void RemoveRoot(FPrivate& State)
	{
		Close(State);
		if (!State.Root.Root.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*State.Root.Root, /*RequireExists*/ false, /*Tree*/ true);
		}
	}

	TSharedRef<FDreamBuffer> MakeBuffer(EDreamBufferKind Kind, const FString& Purpose, const FString& Text)
	{
		TSharedRef<FDreamBuffer> Buffer = MakeShared<FDreamBuffer>(FGuid::NewGuid(), Kind, DreamTest::ScratchName(Purpose));
		Buffer->ResetText(Text, dream::EditOrigin::Load);
		return Buffer;
	}

	/** A file held open with no sharing at all, as a scanner or backup tool can (review r1 persist-M2): no read, no write,
	 *  no rename, no delete until Release. */
	struct FHeldFile
	{
#if PLATFORM_WINDOWS
		HANDLE Handle = INVALID_HANDLE_VALUE;
#endif
		bool Hold(const FString& Path)
		{
			Release();
#if PLATFORM_WINDOWS
			Handle = ::CreateFileW(*FPaths::ConvertRelativePathToFull(Path), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			return Handle != INVALID_HANDLE_VALUE;
#else
			return false;
#endif
		}
		void Release()
		{
#if PLATFORM_WINDOWS
			if (Handle != INVALID_HANDLE_VALUE)
			{
				::CloseHandle(Handle);
				Handle = INVALID_HANDLE_VALUE;
			}
#endif
		}
		~FHeldFile() { Release(); }
	};

	/** The file names in Dir matching Wildcard, sorted. */
	TArray<FString> ListNames(const FString& Dir, const FString& Wildcard)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / Wildcard), /*Files*/ true, /*Directories*/ false);
		Names.Sort();
		return Names;
	}

	/** The blob files under <root>/history/objects. */
	int32 CountBlobs(const FDreamStorageRoot& Root)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *(Root.HistoryDir() / TEXT("objects")), TEXT("*.z"), true, false);
		return Files.Num();
	}

	dream::EditBatch AppendEdit(const FDreamBuffer& Buffer, const FString& Text)
	{
		dream::EditBatch Batch;
		Batch.Origin = dream::EditOrigin::Typing;
		dream::EditRange& Range = Batch.Ranges.emplace_back();
		Range.At = Buffer.GetModel().Length();
		Range.Insert = std::u16string(DreamText::ToU16(Text));
		return Batch;
	}

	FDreamRunId RunId(int32 Number)
	{
		return (static_cast<FDreamRunId>(FPlatformTime::Cycles64()) & 0xFFFF0000ull) | static_cast<FDreamRunId>(Number) | (0x6u * 0x100000000ull);
	}

	/** A synthetic recovered session after an unclean exit with counted scratch buffers (§10.7.1). */
	FDreamRecoveredSession MakeCrashedSession(const TArray<TPair<FString, FString>>& CleanAndRecovered, TArray<FGuid>& OutIds,
		const FString& Purpose)
	{
		FDreamRecoveredSession Session;
		Session.bPreviousExitClean = false;
		Session.PreviousEndUtc = FDateTime::UtcNow();
		for (const TPair<FString, FString>& Texts : CleanAndRecovered)
		{
			FDreamRecoveredBuffer& Rec = Session.Buffers.AddDefaulted_GetRef();
			Rec.Id = FGuid::NewGuid();
			Rec.Kind = EDreamBufferKind::Scratch;
			Rec.DisplayName = DreamTest::ScratchName(Purpose);
			Rec.CleanText = Texts.Key;
			Rec.RecoveredText = Texts.Value;
			Rec.bCounted = true;
			Rec.LastEditUtc = Session.PreviousEndUtc;
			Session.SessionTabOrder.Add(Rec.Id);
			Session.CountedBuffers += 1;
			OutIds.Add(Rec.Id);
		}
		return Session;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Persistence.History
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamHistorySpec, "Dream.Persistence.History", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamHistorySpec)

void FDreamHistorySpec::Define()
{
	LatentIt("pauses history while another program holds an index file, then writes what waited (review r2 persist-r2-M2)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using DreamTest::EStep;
		AddExpectedMessagePlain(TEXT("history is paused until it can be"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			FGuid Buffer;
			TSharedPtr<FDreamBuffer> Scratch;
			TOptional<bool> Added;
			TOptional<TPair<int32, int32>> Retention;
			TOptional<TArray<FDreamLocalHistoryEntry>> Entries;
			TOptional<TArray<FDreamLocalHistoryEntry>> ClosedEntries;
			DreamPersistenceSpec::FHeldFile Held;
			TArray<FString> IndexBefore;
			double ClosedAt = 0.0;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		C->Buffer = FGuid::NewGuid();
		auto Add = [C](const TCHAR* Text)
		{
			C->Added.Reset();
			C->P.History->AddEntry(C->Buffer, EDreamHistoryKind::Save, dream::TextBuffer(DreamText::ToU16(FString(Text))).Snapshot(), FString(),
				TEXT("held.py"), TOptional<int32>(), [C](bool bOk) { C->Added = bOk; });
		};
		DreamTest::FDreamSteps Steps(*this);
		for (const TCHAR* Text : { TEXT("first session"), TEXT("second session") })
		{
			Steps.Do(TEXT("open a session"), [this, C]() { return DreamPersistenceSpec::OpenAndScan(*this, C->P); });
			Steps.Poll(TEXT("its scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
			Steps.Do(TEXT("add an entry"), [C, Add, Text]() { C->P.Journal->Reconcile({}); Add(Text); return true; });
			Steps.Poll(TEXT("the entry is durable"), [C]() { return C->Added.IsSet() ? EStep::Next : EStep::Wait; }, 15.0);
			Steps.Do(TEXT("end the session"), [this, C]()
			{
				TestTrue(TEXT("the entry was kept"), C->Added.Get(false));
				DreamPersistenceSpec::Close(C->P);
				return true;
			});
		}
		Steps.Do(TEXT("another program holds the newest index file; a third session opens"), [this, C]()
		{
			C->IndexBefore = DreamPersistenceSpec::ListNames(C->P.Root.HistoryDir(), TEXT("index.*"));
			if (!TestTrue(TEXT("an index file exists"), C->IndexBefore.Num() > 0))
			{
				return false;
			}
			if (!TestTrue(TEXT("it is held with no sharing"), C->Held.Hold(C->P.Root.HistoryDir() / C->IndexBefore.Last())))
			{
				return false;
			}
			return DreamPersistenceSpec::OpenAndScan(*this, C->P);
		});
		Steps.Poll(TEXT("its scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("a scratch tab, a retention run and an entry while the file is held"), [C, Add]()
		{
			C->Scratch = DreamPersistenceSpec::MakeBuffer(EDreamBufferKind::Scratch, TEXT("ClosedWhileHeld"), TEXT("closed_while_held = 1\n"));
			C->P.Journal->Reconcile({ C->Scratch.ToSharedRef() });
			C->P.History->EnforceRetention([C](int32 Tombstoned, int32 Swept) { C->Retention = TPair<int32, int32>(Tombstoned, Swept); });
			Add(TEXT("third session"));
			return true;
		});
		Steps.Poll(TEXT("history pauses"), [C]() { return C->P.History->IsPaused() && C->Retention.IsSet() ? EStep::Next : EStep::Wait; }, 20.0);
		Steps.Do(TEXT("close the scratch tab while history is paused"), [this, C]()
		{
			TestEqual(TEXT("no retention while paused: nothing tombstoned"), C->Retention->Key, 0);
			TestEqual(TEXT("nothing swept"), C->Retention->Value, 0);
			C->P.Journal->OnBufferClosed(C->Scratch->GetId(), TEXT("closed"));   // its "closed" entry is the text's only copy
			C->ClosedAt = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("a few seconds with the file held"), [C]() { return FPlatformTime::Seconds() - C->ClosedAt > 3.0 ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the entry waits instead of failing, nothing is written above the held file; release it"), [this, C]()
		{
			TestFalse(TEXT("the entry has not failed: it waits"), C->Added.IsSet());
			TestTrue(TEXT("history still paused"), C->P.History->IsPaused());
			TestTrue(TEXT("the index files are exactly as they were"),
				DreamPersistenceSpec::ListNames(C->P.Root.HistoryDir(), TEXT("index.*")) == C->IndexBefore);
			C->Held.Release();
			return true;
		});
		Steps.Poll(TEXT("the waiting entry lands and history resumes"), [C]()
		{
			return C->Added.IsSet() && !C->P.History->IsPaused() ? EStep::Next : EStep::Wait;
		}, 20.0);
		Steps.Do(TEXT("query"), [this, C]()
		{
			TestTrue(TEXT("the entry that waited was kept"), C->Added.Get(false));
			C->P.History->Query(C->Buffer, [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->Entries = MoveTemp(Entries); });
			C->P.History->Query(C->Scratch->GetId(), [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->ClosedEntries = MoveTemp(Entries); });
			return true;
		});
		Steps.Poll(TEXT("the entries"), [C]() { return C->Entries.IsSet() && C->ClosedEntries.IsSet() ? EStep::Next : EStep::Wait; }, 15.0);
		Steps.Do(TEXT("nothing was lost; history writes again"), [this, C, Add]()
		{
			TestEqual(TEXT("the three sessions' entries"), C->Entries->Num(), 3);
			TestNotNull(TEXT("the scratch tab closed while paused has its closed entry"),
				C->ClosedEntries->FindByPredicate([](const FDreamLocalHistoryEntry& E) { return E.Kind == EDreamHistoryKind::Closed; }));
			Add(TEXT("after the release"));
			return true;
		});
		Steps.Poll(TEXT("a new entry"), [C]() { return C->Added.IsSet() ? EStep::Next : EStep::Wait; }, 15.0);
		Steps.Do(TEXT("written"), [this, C]()
		{
			TestTrue(TEXT("history writes again"), C->Added.Get(false));
			return true;
		});
		Steps.Finally(TEXT("release and remove the private root"), [C]()
		{
			C->Held.Release();
			DreamPersistenceSpec::RemoveRoot(C->P);
		});
		Steps.Run(Done);
	});

	LatentIt("ends a pause on its own after the release, and a held index that proves newer is read only (review r3)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using DreamTest::EStep;
		AddExpectedMessagePlain(TEXT("history is paused until it can be"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		AddExpectedMessagePlain(TEXT("was written by a newer Dream Python Editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			DreamPersistenceSpec::FPrivate Newer;
			DreamPersistenceSpec::FHeldFile Held;
			TOptional<bool> Added;
			TOptional<int32> Listed;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		C->Newer.Root = DreamPersistenceSpec::MakePrivateRoot();
		DreamTest::FDreamSteps Steps(*this);
		// A: an index file exists, then another program holds it while only a query (no AddEntry) touches history.
		Steps.Do(TEXT("open a session"), [this, C]() { return DreamPersistenceSpec::OpenAndScan(*this, C->P); });
		Steps.Poll(TEXT("its scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("an entry"), [C]()
		{
			C->P.Journal->Reconcile({});
			C->P.History->AddEntry(FGuid::NewGuid(), EDreamHistoryKind::Save, dream::TextBuffer(u"probe = 1\n").Snapshot(), FString(),
				TEXT("probe.py"), TOptional<int32>(), [C](bool bOk) { C->Added = bOk; });
			return true;
		});
		Steps.Poll(TEXT("durable"), [C]() { return C->Added.IsSet() ? EStep::Next : EStep::Wait; }, 15.0);
		Steps.Do(TEXT("hold the index; the next session only queries"), [this, C]()
		{
			DreamPersistenceSpec::Close(C->P);
			const TArray<FString> Index = DreamPersistenceSpec::ListNames(C->P.Root.HistoryDir(), TEXT("index.*"));
			if (!TestTrue(TEXT("held"), Index.Num() > 0 && C->Held.Hold(C->P.Root.HistoryDir() / Index.Last())))
			{
				return false;
			}
			return DreamPersistenceSpec::OpenAndScan(*this, C->P);
		});
		Steps.Poll(TEXT("its scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("a query"), [C]()
		{
			C->P.Journal->Reconcile({});
			C->P.History->Query(FGuid(), [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->Listed = Entries.Num(); });
			return true;
		});
		Steps.Poll(TEXT("history pauses"), [C]() { return C->Listed.IsSet() && C->P.History->IsPaused() ? EStep::Next : EStep::Wait; }, 20.0);
		Steps.Do(TEXT("release, with nothing queued"), [C]() { C->Held.Release(); return true; });
		Steps.Poll(TEXT("the pause ends on its own"), [C]() { return !C->P.History->IsPaused() ? EStep::Next : EStep::Wait; }, 15.0);
		// B: a held index that proves a newer format once released: read only, the waiting entry answers false.
		Steps.Do(TEXT("a newer format's index, held; a session adds an entry"), [this, C]()
		{
			DreamPersistenceSpec::Close(C->P);
			IFileManager::Get().MakeDirectory(*C->Newer.Root.HistoryDir(), /*Tree*/ true);
			dream::journal::FileHeader Header;
			Header.Generation = 1;
			Header.Major = static_cast<uint16_t>(dream::history::IndexMajor + 1);
			std::string Bytes;
			dream::journal::EncodeHeader(Header, Bytes, dream::history::IndexLogFormat);
			Bytes += "entries a newer Dream Python Editor wrote";
			const FString Path = C->Newer.Root.HistoryDir() / DreamText::Utf8ToFString(dream::journal::LogFileName(dream::history::IndexLogFormat, 1));
			TArray<uint8> Data(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size()));
			if (!TestTrue(TEXT("written and held"), FFileHelper::SaveArrayToFile(Data, *Path) && C->Held.Hold(Path)))
			{
				return false;
			}
			return DreamPersistenceSpec::OpenAndScan(*this, C->Newer);
		});
		Steps.Poll(TEXT("its scan"), [C]() { return C->Newer.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("an entry while it is held"), [C]()
		{
			C->Newer.Journal->Reconcile({});
			C->Added.Reset();
			C->Newer.History->AddEntry(FGuid::NewGuid(), EDreamHistoryKind::Save, dream::TextBuffer(u"newer = 1\n").Snapshot(), FString(),
				TEXT("newer.py"), TOptional<int32>(), [C](bool bOk) { C->Added = bOk; });
			return true;
		});
		Steps.Poll(TEXT("history pauses"), [C]() { return C->Newer.History->IsPaused() ? EStep::Next : EStep::Wait; }, 20.0);
		Steps.Do(TEXT("release"), [C]() { C->Held.Release(); return true; });
		Steps.Poll(TEXT("the entry answers"), [C]() { return C->Added.IsSet() ? EStep::Next : EStep::Wait; }, 20.0);
		Steps.Do(TEXT("read only, not paused"), [this, C]()
		{
			TestFalse(TEXT("a newer format's history takes no entry"), C->Added.Get(true));
			TestFalse(TEXT("and is not paused"), C->Newer.History->IsPaused());
			return true;
		});
		Steps.Finally(TEXT("release and remove both private roots"), [C]()
		{
			C->Held.Release();
			DreamPersistenceSpec::RemoveRoot(C->P);
			DreamPersistenceSpec::RemoveRoot(C->Newer);
		});
		Steps.Run(Done);
	});

	LatentIt("keeps the closed entry of a scratch tab closed just before exit (review r1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using DreamTest::EStep;
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			TSharedPtr<FDreamBuffer> Scratch;
			TOptional<TArray<FDreamLocalHistoryEntry>> Entries;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open"), [this, C]() { return DreamPersistenceSpec::OpenAndScan(*this, C->P); });
		Steps.Poll(TEXT("scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("a scratch tab"), [C]()
		{
			C->Scratch = DreamPersistenceSpec::MakeBuffer(EDreamBufferKind::Scratch, TEXT("ClosedAtExit"), TEXT("closed_at_exit = 1\n"));
			C->P.Journal->Reconcile({ C->Scratch.ToSharedRef() });
			return true;
		});
		Steps.Poll(TEXT("the session's generation"), [C]() { return C->P.Journal->GetGenerationForTest() > 0 ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("history work queued, the tab closed, then the editor exits at once"), [this, C]()
		{
			const FGuid Other = FGuid::NewGuid();
			for (int32 K = 0; K < 6; ++K)
			{
				FString Text = FString::Printf(TEXT("busy_%d = '"), K);
				Text += FString::ChrN(200 * 1024, TCHAR('a' + K));
				Text += TEXT("'\n");
				C->P.History->AddEntry(Other, EDreamHistoryKind::Save, dream::TextBuffer(DreamText::ToU16(Text)).Snapshot(), FString(), TEXT("busy.py"));
			}
			C->P.Journal->OnBufferClosed(C->Scratch->GetId(), TEXT("closed"));   // BUF_CLOSE, then its "closed" entry, its only copy
			DreamPersistenceSpec::Close(C->P);                                     // OnPreExit: CLEAN, the drain, the join
			return DreamPersistenceSpec::OpenAndScan(*this, C->P);
		});
		Steps.Poll(TEXT("the next scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("query the closed tab's history"), [this, C]()
		{
			const FGuid Id = C->Scratch->GetId();
			TestNull(TEXT("the closed tab is not recovered from the journal"),
				C->P.Session.Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; }));
			C->P.Journal->Reconcile({});
			C->P.History->Query(Id, [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->Entries = MoveTemp(Entries); });
			return true;
		});
		Steps.Poll(TEXT("the entries"), [C]() { return C->Entries.IsSet() ? EStep::Next : EStep::Wait; }, 15.0);
		Steps.Do(TEXT("its closed entry is in local history"), [this, C]()
		{
			const FDreamLocalHistoryEntry* Closed = C->Entries->FindByPredicate([](const FDreamLocalHistoryEntry& E) { return E.Kind == EDreamHistoryKind::Closed; });
			TestNotNull(TEXT("the closed entry survived the exit"), Closed);
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [C]() { DreamPersistenceSpec::RemoveRoot(C->P); });
		Steps.Run(Done);
	});

	LatentIt("keeps an entry per save, merges identical runs, and loads texts back (T-PER-017)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			TSharedPtr<FDreamBuffer> File;
			TSharedPtr<FDreamBuffer> Scratch;
			TArray<FString> Texts;
			bool bQueried = false;
			TArray<FDreamLocalHistoryEntry> Entries;
			TArray<FDreamLocalHistoryEntry> All;
			bool bAllQueried = false;
			bool bQuerying = false;
			TOptional<FString> Loaded;
			bool bLoaded = false;
			bool bReloadAdded = false;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open a journal and its history on a private root"), [this, C]() { return DreamPersistenceSpec::OpenAndScan(*this, C->P); });
		Steps.Poll(TEXT("the recovery scan"), [C]() { return C->P.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("ten saves of three texts, three runs of one text, a reload and a closed scratch buffer"), [this, C]()
		{
			C->File = DreamPersistenceSpec::MakeBuffer(EDreamBufferKind::File, TEXT("HistoryFile"), TEXT("v = 0\n"));
			C->File->SetFileIdentity(C->P.Root.Root / TEXT("history_file.py"), TOptional<FDreamBaseVersion>());
			C->Scratch = DreamPersistenceSpec::MakeBuffer(EDreamBufferKind::Scratch, TEXT("HistoryScratch"), TEXT("closed_scratch = 1"));
			C->P.Journal->Reconcile({ C->File.ToSharedRef(), C->Scratch.ToSharedRef() });
			C->Texts = { TEXT("v = 1\n"), TEXT("v = 2\n"), TEXT("v = 3\n") };
			for (int32 Save = 0; Save < 10; ++Save)
			{
				C->File->ResetText(C->Texts[Save % 3], dream::EditOrigin::Typing);
				C->P.Journal->OnBufferSaved(C->File.ToSharedRef());   // META commit, then the "save" entry (§10.4)
			}
			C->File->ResetText(TEXT("run_me = 1\n"), dream::EditOrigin::Typing);
			for (int32 Run = 1; Run <= 3; ++Run)
			{
				const FDreamRunId Id = DreamPersistenceSpec::RunId(Run);
				C->P.Journal->SnapshotForRun(Id, Run, C->File->GetId(), C->File->Snapshot(), EDreamRunRoute::Buffer, false);
				C->P.Journal->MarkRunEnded(Id, EDreamRunOutcome::Ok);
			}
			C->P.History->AddEntry(C->File->GetId(), EDreamHistoryKind::Reload, dream::TextBuffer(u"before reload").Snapshot(), C->File->GetPath(),
				C->File->GetDisplayName(), TOptional<int32>(), [C](bool bOk) { C->bReloadAdded = bOk; });
			C->P.Journal->OnBufferClosed(C->Scratch->GetId(), TEXT("closed"));
			return true;
		});
		Steps.Poll(TEXT("the reload entry is durable"), [C]() { return C->bReloadAdded ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Poll(TEXT("query until every entry is indexed"), [C]()
		{
			// The run and closed entries ride on their commits, so they may land after the reload entry: query again until
			// all fifteen are there (or the step times out and the checks below say what is missing).
			if (C->bQueried && C->bAllQueried)
			{
				C->bQuerying = false;
				if (C->All.Num() >= 13)
				{
					return DreamTest::EStep::Next;
				}
			}
			if (!C->bQuerying)
			{
				C->bQuerying = true;
				C->bQueried = false;
				C->bAllQueried = false;
				C->P.History->Query(C->File->GetId(), [C](TArray<FDreamLocalHistoryEntry>&& Entries)
				{
					C->Entries = MoveTemp(Entries);
					C->bQueried = true;
				});
				C->P.History->Query(FGuid(), [C](TArray<FDreamLocalHistoryEntry>&& Entries)
				{
					C->All = MoveTemp(Entries);
					C->bAllQueried = true;
				});
			}
			return DreamTest::EStep::Wait;
		}, 20.0);
		Steps.Do(TEXT("ten save entries, one run entry with three numbers, one reload entry, one closed entry"), [this, C]()
		{
			int32 Saves = 0;
			int32 Reloads = 0;
			const FDreamLocalHistoryEntry* RunEntry = nullptr;
			int32 RunEntries = 0;
			for (const FDreamLocalHistoryEntry& E : C->Entries)
			{
				Saves += E.Kind == EDreamHistoryKind::Save ? 1 : 0;
				Reloads += E.Kind == EDreamHistoryKind::Reload ? 1 : 0;
				if (E.Kind == EDreamHistoryKind::Run)
				{
					++RunEntries;
					RunEntry = &E;
				}
			}
			TestEqual(TEXT("ten saves give ten entries (PER-04)"), Saves, 10);
			TestEqual(TEXT("three identical runs give one entry"), RunEntries, 1);
			if (RunEntry != nullptr)
			{
				TestTrue(*FString::Printf(TEXT("that entry names the three runs (%d)"), RunEntry->Runs.Num()), RunEntry->Runs == TArray<int32>({ 1, 2, 3 }));
			}
			TestEqual(TEXT("the reload entry"), Reloads, 1);
			const FGuid ScratchId = C->Scratch->GetId();
			const FDreamLocalHistoryEntry* Closed = C->All.FindByPredicate([&ScratchId](const FDreamLocalHistoryEntry& E) { return E.Buffer == ScratchId; });
			if (TestNotNull(TEXT("the closed scratch buffer's entry"), Closed))
			{
				TestTrue(TEXT("kind closed"), Closed->Kind == EDreamHistoryKind::Closed);
			}
			// Content addressing: the three save texts, the run text, the reload text and the closed text are six blobs.
			TestEqual(TEXT("one blob per distinct text"), DreamPersistenceSpec::CountBlobs(C->P.Root), 6);
			for (int32 K = 1; K < C->Entries.Num(); ++K)
			{
				TestTrue(TEXT("entries come newest first"), C->Entries[K - 1].Utc >= C->Entries[K].Utc);
			}
			if (RunEntry == nullptr)
			{
				return false;
			}
			C->P.History->LoadText(*RunEntry, [C](TOptional<FString>&& Text)
			{
				C->Loaded = MoveTemp(Text);
				C->bLoaded = true;
			});
			return true;
		});
		Steps.Poll(TEXT("the run entry's text"), [C]() { return C->bLoaded ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("the text the runs compiled comes back"), [this, C]()
		{
			if (TestTrue(TEXT("the blob loads"), C->Loaded.IsSet()))
			{
				TestEqual(TEXT("its text"), C->Loaded.GetValue(), FString(TEXT("run_me = 1\n")));
			}
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [C]() { DreamPersistenceSpec::RemoveRoot(C->P); });
		Steps.Run(Done);
	});

	LatentIt("clears every entry and blob, and the journal still recovers (T-PER-018)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			TSharedPtr<FDreamBuffer> Buffer;
			int32 Added = 0;
			TOptional<bool> Cleared;
			TOptional<int32> EntriesAfter;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open"), [this, C]() { return DreamPersistenceSpec::OpenAndScan(*this, C->P); });
		Steps.Poll(TEXT("scan"), [C]() { return C->P.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("three entries"), [C]()
		{
			C->Buffer = DreamPersistenceSpec::MakeBuffer(EDreamBufferKind::Scratch, TEXT("Clear"), TEXT("kept_by_the_journal = 1"));
			C->P.Journal->Reconcile({ C->Buffer.ToSharedRef() });
			for (const TCHAR* Text : { TEXT("a"), TEXT("b"), TEXT("c") })
			{
				C->P.History->AddEntry(C->Buffer->GetId(), EDreamHistoryKind::Discarded, dream::TextBuffer(DreamText::ToU16(FString(Text))).Snapshot(),
					FString(), C->Buffer->GetDisplayName(), TOptional<int32>(), [C](bool bOk) { C->Added += bOk ? 1 : 0; });
			}
			C->P.Journal->ForceCommit();
			return true;
		});
		Steps.Poll(TEXT("the entries"), [C]() { return C->Added == 3 ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("Clear local history"), [this, C]()
		{
			TestEqual(TEXT("three blobs before"), DreamPersistenceSpec::CountBlobs(C->P.Root), 3);
			C->P.History->Clear([C](bool bOk) { C->Cleared = bOk; });
			return true;
		});
		Steps.Poll(TEXT("cleared"), [C]() { return C->Cleared.IsSet() ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("nothing is left in history"), [this, C]()
		{
			TestTrue(TEXT("Clear succeeded"), C->Cleared.Get(false));
			TestEqual(TEXT("no blob is left"), DreamPersistenceSpec::CountBlobs(C->P.Root), 0);
			C->P.History->Query(FGuid(), [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->EntriesAfter = Entries.Num(); });
			return true;
		});
		Steps.Poll(TEXT("query"), [C]() { return C->EntriesAfter.IsSet() ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("the journal is untouched: the next session recovers the buffer"), [this, C]()
		{
			TestEqual(TEXT("no entry is left"), C->EntriesAfter.Get(-1), 0);
			DreamPersistenceSpec::Close(C->P, /*bClean*/ false);
			return DreamPersistenceSpec::OpenAndScan(*this, C->P);
		});
		Steps.Poll(TEXT("the next scan"), [C]() { return C->P.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the scratch buffer is recovered"), [this, C]()
		{
			const FGuid Id = C->Buffer->GetId();
			const FDreamRecoveredBuffer* Rec = C->P.Session.Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; });
			if (TestNotNull(TEXT("recovered"), Rec))
			{
				TestEqual(TEXT("its text"), Rec->RecoveredText, C->Buffer->GetTextAsString());
				// §10.7.1: restored at the reconcile, unchanged since: its clean text is its text, so it is not counted.
				TestFalse(TEXT("an unchanged restored scratch buffer is not counted"), Rec->bCounted);
			}
			C->P.Journal->Reconcile({});
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [C]() { DreamPersistenceSpec::RemoveRoot(C->P); });
		Steps.Run(Done);
	});

	LatentIt("tombstones old entries and sweeps their blobs, keeping the newest (T-PER-010 at tier 4)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			FGuid Buffer;
			int32 Added = 0;
			TOptional<TPair<int32, int32>> Result;
			TOptional<int32> EntriesAfter;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		C->Buffer = FGuid::NewGuid();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open"), [this, C]() { return DreamPersistenceSpec::OpenAndScan(*this, C->P); });
		Steps.Poll(TEXT("scan"), [C]() { return C->P.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("two entries 40 days old, then the newest"), [C]()
		{
			C->P.Journal->Reconcile({});
			const dream::history::RetentionLimits Defaults;
			C->P.History->SetRetentionForTest(Defaults.MaxAgeMs, Defaults.MaxBytes, -40 * DreamPersistenceSpec::DayMs);
			auto Add = [C](const TCHAR* Text)
			{
				C->P.History->AddEntry(C->Buffer, EDreamHistoryKind::Save, dream::TextBuffer(DreamText::ToU16(FString(Text))).Snapshot(), FString(),
					TEXT("retention.py"), TOptional<int32>(), [C](bool bOk) { C->Added += bOk ? 1 : 0; });
			};
			Add(TEXT("old one"));
			Add(TEXT("old two"));
			C->P.History->SetRetentionForTest(Defaults.MaxAgeMs, Defaults.MaxBytes, 0);
			Add(TEXT("newest"));
			return true;
		});
		Steps.Poll(TEXT("the entries"), [C]() { return C->Added == 3 ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("retention two days later"), [C]()
		{
			// Two days on, the blobs written now are past the 24 h sweep protection; the old entries are 42 days old.
			const dream::history::RetentionLimits Defaults;
			C->P.History->SetRetentionForTest(Defaults.MaxAgeMs, Defaults.MaxBytes, 2 * DreamPersistenceSpec::DayMs);
			C->P.History->EnforceRetention([C](int32 Tombstoned, int32 Swept) { C->Result = TPair<int32, int32>(Tombstoned, Swept); });
			return true;
		});
		Steps.Poll(TEXT("retention"), [C]() { return C->Result.IsSet() ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("two tombstones, two blobs swept, the newest entry kept"), [this, C]()
		{
			TestEqual(TEXT("tombstoned"), C->Result->Key, 2);
			TestEqual(TEXT("swept"), C->Result->Value, 2);
			TestEqual(TEXT("one blob left"), DreamPersistenceSpec::CountBlobs(C->P.Root), 1);
			C->P.History->Query(C->Buffer, [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->EntriesAfter = Entries.Num(); });
			return true;
		});
		Steps.Poll(TEXT("query"), [C]() { return C->EntriesAfter.IsSet() ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("one entry"), [this, C]()
		{
			TestEqual(TEXT("the newest entry survives"), C->EntriesAfter.Get(-1), 1);
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [C]() { DreamPersistenceSpec::RemoveRoot(C->P); });
		Steps.Run(Done);
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Persistence.Writer
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamJournalWriterSpec, "Dream.Persistence.Writer", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamJournalWriterSpec)

void FDreamJournalWriterSpec::Define()
{
	LatentIt("serves a pre-run commit while a history slice waits out a held file (review r1, TDD 10.2.1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using DreamTest::EStep;
		AddExpectedMessagePlain(TEXT("history is paused until it can be"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			TSharedPtr<FDreamBuffer> Buffer;
			DreamPersistenceSpec::FHeldFile Held;
			TOptional<bool> Added;
			FDreamJournalTicket Ticket;
			FDreamRunId Run = 0;
			double HistoryAt = 0.0;
			double PushedAt = 0.0;
			double DurableAt = -1.0;
			double HistoryDoneAt = -1.0;
			double ReleasedAt = -1.0;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		DreamTest::FDreamSteps Steps(*this);
		for (int32 Session = 0; Session < 2; ++Session)
		{
			Steps.Do(TEXT("open a session"), [this, C]() { return DreamPersistenceSpec::OpenAndScan(*this, C->P); });
			Steps.Poll(TEXT("its scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
			Steps.Do(TEXT("one history entry"), [C]()
			{
				C->P.Journal->Reconcile({});
				C->Added.Reset();
				C->P.History->AddEntry(FGuid::NewGuid(), EDreamHistoryKind::Save, dream::TextBuffer(u"entry = 1\n").Snapshot(), FString(),
					TEXT("serve.py"), TOptional<int32>(), [C](bool bOk) { C->Added = bOk; });
				return true;
			});
			Steps.Poll(TEXT("durable"), [C]() { return C->Added.IsSet() ? EStep::Next : EStep::Wait; }, 15.0);
			Steps.Do(TEXT("end the session"), [C]() { DreamPersistenceSpec::Close(C->P); return true; });
		}
		Steps.Do(TEXT("another program holds the newest index file; a third session journals a buffer"), [this, C]()
		{
			const TArray<FString> Index = DreamPersistenceSpec::ListNames(C->P.Root.HistoryDir(), TEXT("index.*"));
			if (!TestTrue(TEXT("an index file"), Index.Num() > 0) || !TestTrue(TEXT("held"), C->Held.Hold(C->P.Root.HistoryDir() / Index.Last())))
			{
				return false;
			}
			if (!DreamPersistenceSpec::OpenAndScan(*this, C->P))
			{
				return false;
			}
			return true;
		});
		Steps.Poll(TEXT("its scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("reconcile a buffer"), [C]()
		{
			C->Buffer = DreamPersistenceSpec::MakeBuffer(EDreamBufferKind::Scratch, TEXT("ServeCommits"), TEXT("print('served')\n"));
			C->P.Journal->Reconcile({ C->Buffer.ToSharedRef() });
			return true;
		});
		Steps.Poll(TEXT("the generation"), [C]() { return C->P.Journal->GetGenerationForTest() > 0 ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("a history entry: its first slice reads the held index file through the retries"), [C]()
		{
			C->Added.Reset();
			C->HistoryAt = FPlatformTime::Seconds();
			C->P.History->AddEntry(C->Buffer->GetId(), EDreamHistoryKind::Save, C->Buffer->Snapshot(), FString(), C->Buffer->GetDisplayName(),
				TOptional<int32>(), [C](bool bOk) { C->Added = bOk; C->HistoryDoneAt = FPlatformTime::Seconds(); });
			return true;
		});
		Steps.Poll(TEXT("the slice is waiting"), [C]() { return FPlatformTime::Seconds() - C->HistoryAt > 0.25 ? EStep::Next : EStep::Wait; }, 5.0);
		Steps.Do(TEXT("a run's pre-run snapshot meanwhile"), [C]()
		{
			C->Run = DreamPersistenceSpec::RunId(1);
			C->PushedAt = FPlatformTime::Seconds();
			C->Ticket = C->P.Journal->SnapshotForRun(C->Run, 1, C->Buffer->GetId(), C->Buffer->Snapshot(), EDreamRunRoute::Buffer, false);
			return true;
		});
		Steps.Poll(TEXT("the pre-run commit is durable"), [C]()
		{
			if (C->DurableAt < 0.0 && C->P.Journal->IsDurable(C->Ticket))
			{
				C->DurableAt = FPlatformTime::Seconds();
			}
			return C->DurableAt >= 0.0 ? EStep::Next : EStep::Wait;
		}, 15.0);
		Steps.Poll(TEXT("history stays paused a while longer"), [C]() { return FPlatformTime::Seconds() - C->HistoryAt > 2.0 ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the history entry still waits; release the file"), [this, C]()
		{
			TestFalse(TEXT("the history entry waits (persist-r2-M2), it does not fail"), C->Added.IsSet());
			C->Held.Release();
			C->ReleasedAt = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("the history entry lands"), [C]() { return C->Added.IsSet() ? EStep::Next : EStep::Wait; }, 20.0);
		Steps.Do(TEXT("the commit did not wait behind the history slice"), [this, C]()
		{
			const double Commit = C->DurableAt - C->PushedAt;
			const double History = C->HistoryDoneAt - C->HistoryAt;
			AddInfo(FString::Printf(TEXT("pre-run commit durable after %.3f s; the history entry landed after %.3f s"), Commit, History));
			TestTrue(TEXT("the pre-run commit was durable within 0.75 s"), Commit < 0.75);
			TestTrue(TEXT("while the history slice waited"), C->DurableAt < C->ReleasedAt);
			TestTrue(TEXT("the history entry was kept once the file was released"), C->Added.Get(false) && C->HistoryDoneAt >= C->ReleasedAt);
			C->P.Journal->MarkRunEnded(C->Run, EDreamRunOutcome::Ok);
			return true;
		});
		Steps.Finally(TEXT("release and remove the private root"), [C]()
		{
			C->Held.Release();
			DreamPersistenceSpec::RemoveRoot(C->P);
		});
		Steps.Run(Done);
	});

	LatentIt("never starts a generation above one the recovery could not read, and keeps that one (review r1 persist-M2)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using DreamTest::EStep;
		AddExpectedMessagePlain(TEXT("does not start yet"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			TSharedPtr<FDreamBuffer> Buffer;
			DreamPersistenceSpec::FHeldFile Held;
			FString HeldPath;
			double Since = 0.0;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		DreamTest::FDreamSteps Steps(*this);
		for (const TCHAR* Text : { TEXT("one\n"), TEXT("two\n") })
		{
			Steps.Do(TEXT("open a session"), [this, C]() { return DreamPersistenceSpec::OpenAndScan(*this, C->P); });
			Steps.Poll(TEXT("its scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
			Steps.Do(TEXT("journal the buffer"), [C, Text]()
			{
				if (!C->Buffer.IsValid())
				{
					C->Buffer = DreamPersistenceSpec::MakeBuffer(EDreamBufferKind::Scratch, TEXT("HeldGeneration"), Text);
				}
				C->Buffer->ResetText(Text, dream::EditOrigin::Load);
				C->P.Journal->Reconcile({ C->Buffer.ToSharedRef() });
				return true;
			});
			Steps.Poll(TEXT("its generation"), [C]() { return C->P.Journal->GetGenerationForTest() > 0 ? EStep::Next : EStep::Wait; }, 10.0);
			Steps.Do(TEXT("end it cleanly"), [C]() { DreamPersistenceSpec::Close(C->P); return true; });
		}
		Steps.Do(TEXT("another program holds the newest generation; the next session opens"), [this, C]()
		{
			const FString Journal = C->P.Root.JournalDir(TEXT("main"));
			const TArray<FString> Generations = DreamPersistenceSpec::ListNames(Journal, TEXT("wal.*.sjl"));
			if (!TestEqual(TEXT("two generations"), Generations.Num(), 2))
			{
				return false;
			}
			C->HeldPath = Journal / Generations.Last();
			if (!TestTrue(TEXT("held with no sharing"), C->Held.Hold(C->HeldPath)))
			{
				return false;
			}
			return DreamPersistenceSpec::OpenAndScan(*this, C->P);
		});
		Steps.Poll(TEXT("its scan (the reads retry first)"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 30.0);
		Steps.Do(TEXT("the previous generation is used and says so; reconcile"), [this, C]()
		{
			TestTrue(TEXT("fallback used"), C->P.Session.bFallbackUsed);
			const FGuid Id = C->Buffer->GetId();
			const FDreamRecoveredBuffer* Rec = C->P.Session.Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; });
			if (TestNotNull(TEXT("recovered"), Rec))
			{
				TestEqual(TEXT("from the previous generation"), Rec->RecoveredText, FString(TEXT("one\n")));
			}
			C->P.Journal->Reconcile({});
			C->Since = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("a while with the file held"), [C]() { return FPlatformTime::Seconds() - C->Since > 4.0 ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("no generation started above it; then the program lets go"), [this, C]()
		{
			TestEqual(TEXT("no generation started"), C->P.Journal->GetGenerationForTest(), uint64(0));
			TestTrue(TEXT("journal health Degraded"), C->P.Journal->GetHealth() == EDreamJournalHealth::Degraded);
			TestTrue(TEXT("the held generation is in place"), IFileManager::Get().FileExists(*C->HeldPath));
			C->Held.Release();
			return true;
		});
		Steps.Poll(TEXT("the start is retried"), [C]() { return C->P.Journal->GetGenerationForTest() > 0 ? EStep::Next : EStep::Wait; }, 20.0);
		Steps.Do(TEXT("it was set aside first, and the next session keeps it"), [this, C]()
		{
			TestEqual(TEXT("generation 3"), C->P.Journal->GetGenerationForTest(), uint64(3));
			TestTrue(TEXT("journal health Ok"), C->P.Journal->GetHealth() == EDreamJournalHealth::Ok);
			TestFalse(TEXT("moved"), IFileManager::Get().FileExists(*C->HeldPath));
			TestTrue(TEXT("kept as .corrupt"), IFileManager::Get().FileExists(*(C->HeldPath + TEXT(".corrupt"))));
			DreamPersistenceSpec::Close(C->P);
			return DreamPersistenceSpec::OpenAndScan(*this, C->P);
		});
		Steps.Poll(TEXT("the next scan"), [C]() { return C->P.bScanned ? EStep::Next : EStep::Wait; }, 10.0);
		Steps.Do(TEXT("never deleted"), [this, C]()
		{
			TestTrue(TEXT("still kept"), IFileManager::Get().FileExists(*(C->HeldPath + TEXT(".corrupt"))));
			TestEqual(TEXT("read from generation 3"), C->P.Session.SourceGeneration, uint64(3));
			C->P.Journal->Reconcile({});
			return true;
		});
		Steps.Finally(TEXT("release and remove the private root"), [C]()
		{
			C->Held.Release();
			DreamPersistenceSpec::RemoveRoot(C->P);
		});
		Steps.Run(Done);
	});

	LatentIt("rotates in slices while commits go on, and the next session reads the new generation (B10)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			TSharedPtr<FDreamBuffer> Buffer;
			uint64 StartGeneration = 0;
			FString Expected;
			int32 Edits = 0;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open"), [this, C]() { return DreamPersistenceSpec::OpenAndScan(*this, C->P); });
		Steps.Poll(TEXT("scan"), [C]() { return C->P.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("a 6 MiB buffer, so the checkpoint takes more than one 4 MiB slice"), [C]()
		{
			FString Big;
			Big.Reserve(6 << 20);
			while (Big.Len() < (6 << 20))
			{
				Big += TEXT("print('rotation slice')  # padding padding padding padding\n");
			}
			C->Buffer = DreamPersistenceSpec::MakeBuffer(EDreamBufferKind::Scratch, TEXT("Rotation"), Big);
			C->P.Journal->Reconcile({ C->Buffer.ToSharedRef() });
			return true;
		});
		Steps.Poll(TEXT("the session's generation"), [C]()
		{
			C->StartGeneration = C->P.Journal->GetGenerationForTest();
			return C->StartGeneration > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 20.0);
		Steps.Do(TEXT("force a rotation at the next commit, and keep typing"), [C]()
		{
			C->P.Journal->ForceRotationForTest();
			C->Buffer->ApplyEdit(DreamPersistenceSpec::AppendEdit(*C->Buffer, TEXT("edit = 0\n")));
			C->P.Journal->ForceCommit();
			return true;
		});
		Steps.Poll(TEXT("commits during the rotation, until it finishes"), [C]()
		{
			if (C->Edits < 40)
			{
				// One small edit per frame, each forced: they land in the old generation and join the catch-up.
				++C->Edits;
				C->Buffer->ApplyEdit(DreamPersistenceSpec::AppendEdit(*C->Buffer, FString::Printf(TEXT("edit = %d\n"), C->Edits)));
				C->P.Journal->ForceCommit();
			}
			return C->P.Journal->GetGenerationForTest() > C->StartGeneration && C->Edits >= 40 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Do(TEXT("end without CLEAN"), [this, C]()
		{
			TestEqual(TEXT("one rotation"), C->P.Journal->GetGenerationForTest(), C->StartGeneration + 1);
			TestTrue(TEXT("journal health Ok"), C->P.Journal->GetHealth() == EDreamJournalHealth::Ok);
			C->Expected = C->Buffer->GetTextAsString();
			DreamPersistenceSpec::Close(C->P, /*bClean*/ false);
			const FString Journal = C->P.Root.JournalDir(TEXT("main"));
			TArray<FString> Tmp;
			IFileManager::Get().FindFiles(Tmp, *(Journal / TEXT("*.tmp")), true, false);
			TestEqual(TEXT("no checkpoint .tmp is left"), Tmp.Num(), 0);
			return DreamPersistenceSpec::OpenAndScan(*this, C->P);
		});
		Steps.Poll(TEXT("the next scan"), [C]() { return C->P.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 20.0);
		Steps.Do(TEXT("the newest text comes back from the rotated generation"), [this, C]()
		{
			const FGuid Id = C->Buffer->GetId();
			const FDreamRecoveredBuffer* Rec = C->P.Session.Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; });
			if (TestNotNull(TEXT("recovered"), Rec))
			{
				TestTrue(TEXT("the text after every commit made during the rotation"), Rec->RecoveredText == C->Expected);
			}
			TestEqual(TEXT("read from the rotated generation"), C->P.Session.SourceGeneration, C->StartGeneration + 1);
			TestFalse(TEXT("no fallback"), C->P.Session.bFallbackUsed);
			C->P.Journal->Reconcile({});
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [C]() { DreamPersistenceSpec::RemoveRoot(C->P); });
		Steps.Run(Done);
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Persistence.SafeMode
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamSafeModeSpec, "Dream.Persistence.SafeMode", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamSafeModeSpec)

void FDreamSafeModeSpec::Define()
{
	It("counts restores, enters safe mode after two crashes during restore, and leaves it at 0 (TDD 10.8)", [this]()
	{
		// Start 3 logs that it opens in safe mode: expected once here.
		AddExpectedMessagePlain(TEXT("Dream Python Editor opens in safe mode"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		const DreamPersistenceSpec::FPrivate P{ DreamPersistenceSpec::MakePrivateRoot() };
		const FString State = P.Root.StateDir();
		const FString File = FDreamRestoreGuard::GuardPath(State);
		int32 Pending = -1;
		bool bNewer = false;
		{
			FDreamRestoreGuard Guard([]() { return true; });
			TestFalse(TEXT("start 1: no safe mode"), Guard.BeginRestore(State, false));
			TestTrue(TEXT("pending is on disk before any restore work"), FDreamRestoreGuard::ReadPending(File, Pending, bNewer) && Pending == 1);
		}
		{
			FDreamRestoreGuard Guard([]() { return true; });   // start 1 crashed during restore: no clear
			TestFalse(TEXT("start 2: no safe mode"), Guard.BeginRestore(State, false));
			TestEqual(TEXT("pending 2"), Guard.GetPending(), 2);
		}
		{
			FDreamRestoreGuard Guard([]() { return true; });   // start 2 crashed too
			TestTrue(TEXT("start 3: safe mode by the guard"), Guard.BeginRestore(State, false));
			Guard.EnterSafeMode(EDreamSafeModeReason::Guard);
			TestTrue(TEXT("in safe mode"), Guard.IsSafeMode());
			TestTrue(TEXT("the count is not raised in safe mode"), FDreamRestoreGuard::ReadPending(File, Pending, bNewer) && Pending == 2);
			Guard.LeaveSafeMode();
			TestFalse(TEXT("left safe mode"), Guard.IsSafeMode());
			TestTrue(TEXT("Leave safe mode sets pending = 0"), FDreamRestoreGuard::ReadPending(File, Pending, bNewer) && Pending == 0);
		}
		{
			// An automation session never enters safe mode by the guard: the count restarts (tier 4 ends with a forced Quit).
			FDreamRestoreGuard::WritePending(File, 2);
			FDreamRestoreGuard Guard([]() { return true; });
			TestFalse(TEXT("automation: no safe mode at 2"), Guard.BeginRestore(State, /*bAutomationSession*/ true));
			TestTrue(TEXT("automation: the count restarts at 1"), FDreamRestoreGuard::ReadPending(File, Pending, bNewer) && Pending == 1);
			Guard.Clear();
			TestTrue(TEXT("clean shutdown: 0"), FDreamRestoreGuard::ReadPending(File, Pending, bNewer) && Pending == 0);
		}
		{
			// Small-file rules (§10.2.3): a torn file falls back to its .bak; a newer schema is used but never rewritten.
			FDreamRestoreGuard::WritePending(File, 1);
			FDreamRestoreGuard::WritePending(File, 0);   // the .bak now holds 1
			FFileHelper::SaveStringToFile(FString(TEXT("{\"schema\": 1, \"pend")), *File);
			TestTrue(TEXT("a torn file reads its .bak"), FDreamRestoreGuard::ReadPending(File, Pending, bNewer) && Pending == 1);
			FFileHelper::SaveStringToFile(FString(TEXT("{\"schema\": 9, \"pending\": 1, \"future\": true}")), *File);
			FDreamRestoreGuard Guard([]() { return true; });
			TestFalse(TEXT("a newer schema: no safe mode at 1"), Guard.BeginRestore(State, false));
			FString Text;
			FFileHelper::LoadFileToString(Text, *File);
			TestTrue(TEXT("a newer schema is never rewritten"), Text.Contains(TEXT("\"schema\": 9")) && Text.Contains(TEXT("\"pending\": 1")));
		}
		IFileManager::Get().DeleteDirectory(*P.Root.Root, /*RequireExists*/ false, /*Tree*/ true);
	});

	LatentIt("clears the guard 10 s (here 0.5 s) after the deferred import with every tab restored", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			DreamPersistenceSpec::FPrivate P;
			TUniquePtr<FDreamRestoreGuard> Guard;
			bool bImported = false;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->P.Root = DreamPersistenceSpec::MakePrivateRoot();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("begin the restore"), [this, C]()
		{
			FCase* Raw = &C.Get();
			C->Guard = MakeUnique<FDreamRestoreGuard>([Raw]() { return Raw->bImported; });
			C->Guard->SetClearDelayForTest(0.5);
			TestFalse(TEXT("no safe mode"), C->Guard->BeginRestore(C->P.Root.StateDir(), false));
			C->Guard->OnRestoreApplied();
			return true;
		});
		Steps.WaitFrames(30);
		Steps.Do(TEXT("before the import the guard stays"), [this, C]()
		{
			TestEqual(TEXT("pending still 1"), C->Guard->GetPending(), 1);
			C->bImported = true;
			return true;
		});
		Steps.Poll(TEXT("cleared after the delay"), [C]() { return C->Guard->GetPending() == 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("on disk too"), [this, C]()
		{
			int32 Pending = -1;
			bool bNewer = false;
			TestTrue(TEXT("pending 0 on disk"), FDreamRestoreGuard::ReadPending(FDreamRestoreGuard::GuardPath(C->P.Root.StateDir()), Pending, bNewer) && Pending == 0);
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [C]()
		{
			C->Guard.Reset();
			IFileManager::Get().DeleteDirectory(*C->P.Root.Root, /*RequireExists*/ false, /*Tree*/ true);
		});
		Steps.Run(Done);
	});

	It("did not put this editor's own session in safe mode (repeated tier-4 passes)", [this]()
	{
		FDreamEditorModule* M = DreamTest::GetModule(*this);
		if (M == nullptr)
		{
			return;
		}
		const FDreamRestoreGuard& Guard = M->GetRestoreGuard();
		AddInfo(FString::Printf(TEXT("this session's restore guard reads %d (-1: no tab restore ran)"), Guard.GetPending()));
		TestFalse(TEXT("this editor session is not in safe mode by the guard"), Guard.GetSafeModeReason() == EDreamSafeModeReason::Guard);
		TestTrue(TEXT("an automation session is recognised"), FDreamRestoreGuard::IsAutomationSession());
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Persistence.Recovery
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamRecoverySpec, "Dream.Persistence.Recovery", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamRecoverySpec)

void FDreamRecoverySpec::Define()
{
	AfterEach([this]()
	{
		if (Tab.IsValid() && Tab->IsInSafeMode())
		{
			// A case that stopped half-way never leaves the shared session in safe mode (the language services and the deferred
			// start listen to it); the skipped synthetic buffers it opens are in BufferIds and closed below.
			Tab->LeaveSafeMode();
		}
		if (Tab.IsValid() && Tab->IsRecoveryReviewOpen())
		{
			Tab->ToggleRecoveryReview();
		}
		for (const FGuid& Id : BufferIds)
		{
			if (FDreamEditorModule::IsAvailable())
			{
				if (TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(Id))
				{
					Buffer->SetPendingRecovery(TOptional<FString>());   // the synthetic recovery is not offered again
				}
			}
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		Tab.Reset();
	});

	It("shows a counted scratch buffer's clean text under the pending bar; Restore is one Ctrl+Z (B16, T-PER-029)", [this]()
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			return;
		}
		TArray<FGuid> Ids;
		const FString Clean = FString::Printf(TEXT("clean_%s = 1\n"), *DreamTest::UniqueSuffix());
		const FString Recovered = Clean + TEXT("recovered = 2\nmore = 3\n");
		Tab->ApplyRecoveryForTest(DreamPersistenceSpec::MakeCrashedSession({ { Clean, Recovered } }, Ids, TEXT("Pending")));
		BufferIds.Append(Ids);
		TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(Ids[0]);
		if (!TestNotNull(TEXT("the scratch buffer is restored"), Buffer.Get()))
		{
			return;
		}
		TestEqual(TEXT("it shows its clean text"), Buffer->GetTextAsString(), Clean);
		TestTrue(TEXT("its recovery is pending"), Buffer->HasPendingRecovery());
		TestTrue(TEXT("read-only under Not restored yet"), Buffer->IsReadOnly());
		TestTrue(TEXT("listed as pending"), Tab->GetPendingRecoveries().Contains(Ids[0]));
		Tab->ActivateBuffer(Ids[0]);
		const FString Bar = Tab->GetPendingBarTextForTest().ToString();
		TestTrue(*FString::Printf(TEXT("the pending bar says Not restored yet (%s)"), *Bar), Bar.Contains(TEXT("Not restored yet")));
		TestTrue(*FString::Printf(TEXT("with +2 -0 lines (%s)"), *Bar), Bar.Contains(TEXT("+2")));

		Tab->ToggleRecoveryCompare(Ids[0]);
		Tab->ToggleRecoveryCompare(Ids[0]);
		Tab->RestoreRecovered(Ids[0], false);
		TestEqual(TEXT("Restore gives the recovered text"), Buffer->GetTextAsString(), Recovered);
		TestFalse(TEXT("no longer pending"), Buffer->HasPendingRecovery());
		TestFalse(TEXT("editable again"), Buffer->IsReadOnly());
		TestTrue(TEXT("dirty (nothing written)"), Buffer->IsDirty());
		TestTrue(TEXT("the bar is gone"), Tab->GetPendingBarTextForTest().IsEmpty());
		TSharedPtr<FDreamUndoHistory> Undo = FDreamUndoHistory::Find(*Buffer);
		if (TestTrue(TEXT("Restore is an undoable edit"), Undo.IsValid() && Undo->CanUndo()))
		{
			Undo->Undo();
			TestEqual(TEXT("one Ctrl+Z gives the clean text back"), Buffer->GetTextAsString(), Clean);
		}
	});

	LatentIt("moves a discarded text into local history (B15, T-PER-031)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid() || !FDreamEditorModule::Get().GetJournal().IsOpen())
		{
			if (Tab.IsValid())
			{
				AddInfo(TEXT("this editor's journal is not open (a second instance?): Discard needs local history; skipped"));
			}
			Done.Execute();
			return;
		}
		struct FCase
		{
			TArray<FGuid> Ids;
			FString Recovered;
			TArray<FDreamLocalHistoryEntry> Entries;
			bool bQueried = false;
			TOptional<FString> Loaded;
			bool bLoaded = false;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Recovered = FString::Printf(TEXT("discarded_%s = 1\n"), *DreamTest::UniqueSuffix());
		Tab->ApplyRecoveryForTest(DreamPersistenceSpec::MakeCrashedSession({ { FString(), C->Recovered } }, C->Ids, TEXT("Discard")));
		BufferIds.Append(C->Ids);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("Discard"), [this, C]()
		{
			TestTrue(TEXT("pending before"), Tab->GetPendingRecoveries().Contains(C->Ids[0]));
			Tab->DiscardRecovered(C->Ids);
			return true;
		});
		Steps.Poll(TEXT("resolved once its history entry is durable"), [this, C]()
		{
			return Tab->GetPendingRecoveries().Contains(C->Ids[0]) ? DreamTest::EStep::Wait : DreamTest::EStep::Next;
		}, 15.0);
		Steps.Do(TEXT("query the buffer's history"), [this, C]()
		{
			TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(C->Ids[0]);
			if (TestNotNull(TEXT("the scratch buffer stays open"), Buffer.Get()))
			{
				TestFalse(TEXT("no longer pending"), Buffer->HasPendingRecovery());
				TestTrue(TEXT("it keeps its clean (empty) text"), Buffer->GetTextAsString().IsEmpty());
			}
			FDreamEditorModule::Get().GetLocalHistory().Query(C->Ids[0], [C](TArray<FDreamLocalHistoryEntry>&& Entries)
			{
				C->Entries = MoveTemp(Entries);
				C->bQueried = true;
			});
			return true;
		});
		Steps.Poll(TEXT("query"), [C]() { return C->bQueried ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("a discarded entry"), [this, C]()
		{
			const FDreamLocalHistoryEntry* Discarded = C->Entries.FindByPredicate([](const FDreamLocalHistoryEntry& E) { return E.Kind == EDreamHistoryKind::Discarded; });
			if (!TestNotNull(TEXT("a discarded entry"), Discarded))
			{
				return false;
			}
			FDreamEditorModule::Get().GetLocalHistory().LoadText(*Discarded, [C](TOptional<FString>&& Text)
			{
				C->Loaded = MoveTemp(Text);
				C->bLoaded = true;
			});
			return true;
		});
		Steps.Poll(TEXT("its text"), [C]() { return C->bLoaded ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("the discarded text is kept"), [this, C]()
		{
			TestTrue(TEXT("loaded"), C->Loaded.IsSet() && C->Loaded.GetValue() == C->Recovered);
			return true;
		});
		Steps.Run(Done);
	});

	It("lists pending buffers in Review; Restore as new tab opens the recovered text (TDD 10.7.3)", [this]()
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			return;
		}
		// The shared DreamHost profile may carry real pending recoveries (a killed earlier session; they stay pending until a
		// person resolves them, I7): this case touches only its own buffers and expects those to stay listed.
		const TArray<FGuid> PendingBefore = Tab->GetPendingRecoveries();
		if (PendingBefore.Num() > 0)
		{
			AddInfo(FString::Printf(TEXT("this session already has %d pending recoveries; they stay pending"), PendingBefore.Num()));
		}
		TArray<FGuid> Ids;
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString RecoveredB = FString::Printf(TEXT("b_%s = 'recovered'\n"), *Sfx);
		Tab->ApplyRecoveryForTest(DreamPersistenceSpec::MakeCrashedSession(
			{ { TEXT("a = 1\n"), TEXT("a = 1\na2 = 2\n") }, { TEXT("b = 1\n"), RecoveredB } }, Ids, TEXT("Review")));
		BufferIds.Append(Ids);
		TestTrue(TEXT("both pending"), Tab->GetPendingRecoveries().Contains(Ids[0]) && Tab->GetPendingRecoveries().Contains(Ids[1]));
		Tab->ToggleRecoveryReview();
		TestTrue(TEXT("Review is open (non-modal: no modal window)"), Tab->IsRecoveryReviewOpen() && !FSlateApplication::Get().GetActiveModalWindow().IsValid());
		Tab->ToggleRecoveryCompare(Ids[1]);
		Tab->RestoreRecovered(Ids[1], /*bAsNewTab*/ true);
		const TSharedPtr<FDreamBuffer> NewTab = Tab->GetActiveBuffer();
		if (TestTrue(TEXT("a new tab is active"), NewTab.IsValid() && NewTab->GetId() != Ids[1]))
		{
			BufferIds.Add(NewTab->GetId());
			TestEqual(TEXT("it holds the recovered text"), NewTab->GetTextAsString(), RecoveredB);
		}
		const TSharedPtr<FDreamBuffer> Original = FDreamEditorModule::Get().GetBufferManager().FindById(Ids[1]);
		if (TestNotNull(TEXT("the original stays"), Original.Get()))
		{
			TestFalse(TEXT("no longer pending"), Original->HasPendingRecovery());
			TestEqual(TEXT("it keeps its clean text"), Original->GetTextAsString(), FString(TEXT("b = 1\n")));
		}
		TestTrue(TEXT("the other buffer still waits"), Tab->GetPendingRecoveries().Contains(Ids[0]));
		Tab->RestoreRecovered(Ids[0], false);
		TestFalse(TEXT("restored: no longer pending"), Tab->GetPendingRecoveries().Contains(Ids[0]));
		if (PendingBefore.Num() == 0)
		{
			TestFalse(TEXT("Review closes with nothing left"), Tab->IsRecoveryReviewOpen());
		}
		else
		{
			TestTrue(TEXT("Review stays open for the recoveries that were already pending"), Tab->IsRecoveryReviewOpen());
			TestTrue(TEXT("only those are left"), Tab->GetPendingRecoveries() == PendingBefore);
		}
	});

	It("applies the skipped restore when it leaves safe mode (TDD 10.8)", [this]()
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			return;
		}
		TArray<FGuid> Ids;
		const FString Recovered = FString::Printf(TEXT("skipped_%s = 1\n"), *DreamTest::UniqueSuffix());
		Tab->EnterSafeModeForTest(DreamPersistenceSpec::MakeCrashedSession({ { FString(), Recovered } }, Ids, TEXT("Safe")));
		BufferIds.Append(Ids);
		TestTrue(TEXT("in safe mode"), Tab->IsInSafeMode() && FDreamEditorModule::Get().GetRestoreGuard().IsSafeMode());
		TestFalse(TEXT("the skipped buffer is not restored"), FDreamEditorModule::Get().GetBufferManager().FindById(Ids[0]).IsValid());
		Tab->ToggleRecoveryReview();
		TestTrue(TEXT("Review recovered buffers opens the panel"), Tab->IsRecoveryReviewOpen());
		Tab->LeaveSafeMode();
		TestFalse(TEXT("left safe mode"), Tab->IsInSafeMode() || FDreamEditorModule::Get().GetRestoreGuard().IsSafeMode());
		const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(Ids[0]);
		if (TestNotNull(TEXT("Leave safe mode restored the skipped buffer"), Buffer.Get()))
		{
			TestTrue(TEXT("its recovery waits"), Buffer->HasPendingRecovery());
			TestTrue(TEXT("with its recovered text"), Buffer->GetPendingRecoveryText().Get(FString()) == Recovered);
		}
	});
}
