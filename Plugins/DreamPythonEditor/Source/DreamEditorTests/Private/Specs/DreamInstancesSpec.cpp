// SPDX-License-Identifier: Apache-2.0
// Dream.Persistence.Instances (any editor, private roots under the user temp folder; nothing touches DreamHost's own storage
// root or settings): the multi-instance and storage-root work of TDD §10.1, §10.5 and §10.6. A spec cannot start a second
// editor, so "two instances" are two FDreamJournal objects of this process on one private root (the instance lock is a file
// handle, so the second one is refused as a second editor would be); a crash is CloseWithoutCleanForTest.
//  - a second journal journals in journal/i-<pid>-<start>/ under its own ns.lock, stages its history there, keeps its
//    restore guard in <sec>/state, and never changes main; View its tabs reads the primary's buffers (T-PER-023 shape);
//  - the primary merges a crashed secondary's namespace: listed as an orphan, its staged history and REPL entries merged,
//    its texts in history, then deleted (T-PER-045 shape);
//  - the secondary's poll sees the primary end without CLEAN and offers its buffers read-only (T-PER-023);
//  - an older build on a newer journal journals in compat-<format> and never changes main; the newer side lists compat-*
//    as an orphan (T-PER-025);
//  - storage.root_override: owner.json written at first use; another account's owner.json, a root inside the project tree
//    and a probe that does not answer fall back with a notice (T-PER-053 user layer);
//  - projects.json: this project's entry, a moved project offered, Ignore remembered; Import copies its journal to
//    imported-<oldhash> and lists it (T-PER-013 shape);
//  - the cross-root scan lists another root's unclean namespace and deletes it once resolved, and leaves alone a root with a
//    live primary and a root whose owner.json names another account (T-PER-055 shape);
//  - (review r1, persist-M1) a namespace whose staged history or REPL entries cannot be kept (a blocked blob folder, a
//    locked history.jsonl, a locked staged file) is not deleted and is merged at a later start; a namespace folder without
//    its ns.lock yet (a secondary starting) is neither listed nor deleted; (review r2) an orphan whose newest wal or history
//    index cannot be read is neither listed nor merged nor deleted, and merges once it can be read;
//  - (review r1, persist-M3) -DreamStorageRoot is strict: a root it cannot use leaves the session without a journal, never
//    on the platform default.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamHash.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamText.h"
#include "Output/DreamReplHistory.h"
#include "Persistence/DreamInstances.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamLocalHistory.h"
#include "Persistence/DreamStorageRoot.h"
#include "Persistence/DreamWin32FileOps.h"
#include "dream/HistoryFormat.h"
#include "dream/JournalFormat.h"
#include "dream/StorageIndex.h"

namespace DreamInstancesSpec
{
	/** One "editor instance": a journal, its history and REPL history, and its FDreamInstances, on a root. */
	struct FNode
	{
		FDreamStorageRoot Root;
		TSharedPtr<FDreamJournal> Journal;
		TSharedPtr<FDreamLocalHistory> History;
		TSharedPtr<FDreamReplHistory> Repl;
		TSharedPtr<FDreamInstances> Instances;
		EDreamInstanceRole Role = EDreamInstanceRole::Primary;
		bool bScanned = false;
		FDreamRecoveredSession Session;
	};

	FString PrivateBase()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"), TEXT("inst-") + DreamTest::UniqueSuffix()));
	}

	FDreamStorageRoot MakeRoot(const FString& Base, const FString& Hash)
	{
		FDreamStorageRoot Root;
		Root.Root = Base / TEXT("5.8") / Hash;
		Root.ProjectHash = Hash;
		Root.BaseDir = Base;
		IFileManager::Get().MakeDirectory(*Root.Root, /*Tree*/ true);
		return Root;
	}

	bool Open(FAutomationTestBase& Test, FNode& Node, const FString& SecondaryId = FString())
	{
		Node.bScanned = false;
		Node.Session = FDreamRecoveredSession();
		Node.Journal = MakeShared<FDreamJournal>();
		if (!SecondaryId.IsEmpty())
		{
			Node.Journal->SetSecondaryIdForTest(SecondaryId);
		}
		Node.History = MakeShared<FDreamLocalHistory>(*Node.Journal);
		Node.Repl = MakeShared<FDreamReplHistory>();
		Node.Repl->SetMemoryOnly(true);
		Node.Repl->Open(Node.Root.Root);
		Node.Instances = MakeShared<FDreamInstances>(*Node.Journal, *Node.History, *Node.Repl);
		if (!Node.Journal->Open(Node.Root, Node.Role))
		{
			Test.AddError(FString::Printf(TEXT("Open failed on %s"), *Node.Root.Root));
			return false;
		}
		Node.Repl->SetMemoryOnly(Node.Role == EDreamInstanceRole::Secondary);
		Node.Instances->OnJournalOpened();
		FNode* Ptr = &Node;
		Node.Journal->RequestRecoveryScan(FDreamOnRecoveryScan::CreateLambda([Ptr](FDreamRecoveredSession& Session) -> bool
		{
			Ptr->Session = MoveTemp(Session);
			Ptr->bScanned = true;
			return true;
		}));
		return true;
	}

	void Close(FNode& Node, bool bClean = true)
	{
		if (Node.Instances.IsValid())
		{
			Node.Instances->Shutdown();
		}
		FDreamJournal::ReleaseOrphanLocks(Node.Session.Orphans);
		if (Node.History.IsValid())
		{
			Node.History->Shutdown();
		}
		if (Node.Journal.IsValid() && Node.Journal->IsOpen())
		{
			if (bClean)
			{
				Node.Journal->Shutdown();
			}
			else
			{
				Node.Journal->CloseWithoutCleanForTest();
			}
		}
		if (Node.Repl.IsValid())
		{
			Node.Repl->Close();
		}
		Node.Instances.Reset();
		Node.History.Reset();
		Node.Repl.Reset();
		Node.Journal.Reset();
	}

	TSharedRef<FDreamBuffer> MakeScratch(const FString& Purpose, const FString& Text)
	{
		TSharedRef<FDreamBuffer> Buffer = MakeShared<FDreamBuffer>(FGuid::NewGuid(), EDreamBufferKind::Scratch, DreamTest::ScratchName(Purpose));
		Buffer->ResetText(Text, dream::EditOrigin::Load);
		return Buffer;
	}

	/** A pre-run snapshot is the durability probe: its ticket turns durable once the commit is flushed (§10.2.5). */
	FDreamJournalTicket Snapshot(FNode& Node, const TSharedRef<FDreamBuffer>& Buffer, bool bEndRun = true)
	{
		static int32 Number = 0;
		++Number;
		const FDreamRunId Id = (0x7ull << 32) | static_cast<FDreamRunId>(Number);
		const FDreamJournalTicket Ticket = Node.Journal->SnapshotForRun(Id, Number, Buffer->GetId(), Buffer->Snapshot(), EDreamRunRoute::Buffer, false);
		if (bEndRun)
		{
			Node.Journal->MarkRunEnded(Id, EDreamRunOutcome::Ok);
		}
		return Ticket;
	}

	/** Every file under Dir with its bytes (main's "byte-identical" checks). */
	TMap<FString, TArray<uint8>> ReadTree(const FString& Dir)
	{
		TMap<FString, TArray<uint8>> Out;
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*"), true, false);
		for (const FString& File : Files)
		{
			if (FPaths::GetCleanFilename(File) == TEXT("ns.lock"))
			{
				continue;   // held by its owner: its content is the owner's business
			}
			// Through the journal's own reader (FILE_SHARE_WRITE): the live writer holds the current generation open.
			FDreamWin32FileOps Ops;
			std::string Bytes;
			Ops.ReadAll(FDreamWin32FileOps::ToUtf8Path(File), Bytes);
			Out.Add(File, TArray<uint8>(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size())));
		}
		return Out;
	}

	bool SameTree(const TMap<FString, TArray<uint8>>& A, const TMap<FString, TArray<uint8>>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (const TPair<FString, TArray<uint8>>& Pair : A)
		{
			const TArray<uint8>* Other = B.Find(Pair.Key);
			if (Other == nullptr || *Other != Pair.Value)
			{
				return false;
			}
		}
		return true;
	}

	DreamTest::EStep WaitIdle(FNode& Node)
	{
		Node.Instances->PumpForTest();
		return Node.Instances->IsIdleForTest() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
	}

	void RemoveTree(const FString& Base)
	{
		if (!Base.IsEmpty() && Base.Contains(TEXT("DreamTier4")))
		{
			IFileManager::Get().DeleteDirectory(*Base, /*RequireExists*/ false, /*Tree*/ true);
		}
	}
}

BEGIN_DEFINE_SPEC(FDreamInstancesSpec, "Dream.Persistence.Instances", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamInstancesSpec)

void FDreamInstancesSpec::Define()
{
	using namespace DreamInstancesSpec;

	LatentIt("a second journal journals in its own namespace, stages its history there and never changes main (T-PER-023 shape)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Base;
			FNode Primary;
			FNode Secondary;
			TSharedPtr<FDreamBuffer> PrimaryBuffer;
			TSharedPtr<FDreamBuffer> SecondaryBuffer;
			FDreamJournalTicket Ticket;
			TMap<FString, TArray<uint8>> MainBefore;
			bool bStaged = false;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Base = PrivateBase();
		C->Primary.Root = MakeRoot(C->Base, TEXT("DreamT4Inst-a00001"));
		C->Secondary.Root = C->Primary.Root;
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("the primary opens and journals a scratch buffer"), [this, C]()
		{
			if (!Open(*this, C->Primary))
			{
				return false;
			}
			TestTrue(TEXT("the first journal is the primary"), C->Primary.Role == EDreamInstanceRole::Primary);
			TestEqual(TEXT("in main"), C->Primary.Journal->GetNamespace(), FString(TEXT("main")));
			return true;
		});
		Steps.Poll(TEXT("its scan"), [C]() { return C->Primary.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("reconcile and a durable snapshot"), [C]()
		{
			C->PrimaryBuffer = MakeScratch(TEXT("InstPrimary"), TEXT("primary_text = 1\n"));
			C->Primary.Journal->Reconcile({ C->PrimaryBuffer.ToSharedRef() });
			C->Ticket = Snapshot(C->Primary, C->PrimaryBuffer.ToSharedRef(), /*bEndRun*/ false);   // nothing queued behind it
			return true;
		});
		Steps.Poll(TEXT("durable"), [C]() { return C->Primary.Journal->IsDurable(C->Ticket) ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.WaitFrames(10);
		Steps.Do(TEXT("a second journal on the same root is the secondary in i-<pid>-<start>"), [this, C]()
		{
			C->MainBefore = ReadTree(C->Primary.Root.JournalDir(TEXT("main")));
			if (!Open(*this, C->Secondary, TEXT("i-4242-1000")))
			{
				return false;
			}
			TestTrue(TEXT("the second journal is a secondary"), C->Secondary.Role == EDreamInstanceRole::Secondary);
			TestTrue(TEXT("and its journal is open (A39's Disabled journal is gone)"), C->Secondary.Journal->IsOpen());
			TestEqual(TEXT("its namespace"), C->Secondary.Journal->GetNamespace(), FString(TEXT("i-4242-1000")));
			const FString Sec = C->Secondary.Root.JournalDir(TEXT("i-4242-1000"));
			TestEqual(TEXT("its restore guard lives in <sec>/state (§10.5)"), C->Secondary.Journal->GetStateDir(), Sec / TEXT("state"));
			TestEqual(TEXT("its history entries are staged in <sec>/pending-history"), C->Secondary.Journal->GetHistoryDir(), Sec / TEXT("pending-history"));
			TestTrue(TEXT("its ns.lock is held"), FDreamWin32FileOps::IsLockHeld(Sec / TEXT("ns.lock")));
			TestEqual(TEXT("the primary's PID is read from instance.lock"), static_cast<int32>(C->Secondary.Instances->GetPrimaryPid()),
				static_cast<int32>(FPlatformProcess::GetCurrentProcessId()));
			return true;
		});
		Steps.Poll(TEXT("the secondary's scan"), [C]() { return C->Secondary.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the secondary journals, stages a history entry and asks to view the primary's tabs"), [this, C]()
		{
			TestTrue(TEXT("a new namespace has nothing to recover"), C->Secondary.Session.Buffers.Num() == 0 && C->Secondary.Session.bPreviousExitClean);
			TestEqual(TEXT("a secondary lists no orphans"), C->Secondary.Session.Orphans.Num(), 0);
			C->SecondaryBuffer = MakeScratch(TEXT("InstSecondary"), TEXT("secondary_text = 2\n"));
			C->Secondary.Journal->Reconcile({ C->SecondaryBuffer.ToSharedRef() });
			C->Ticket = Snapshot(C->Secondary, C->SecondaryBuffer.ToSharedRef());
			C->Secondary.History->AddEntry(C->SecondaryBuffer->GetId(), EDreamHistoryKind::Save, C->SecondaryBuffer->Snapshot(), FString(),
				C->SecondaryBuffer->GetDisplayName(), TOptional<int32>(), [C](bool bOk) { C->bStaged = bOk; });
			C->Secondary.Instances->RequestPrimaryView();
			return true;
		});
		Steps.Poll(TEXT("durable, staged, and the view read"), [C]()
		{
			C->Secondary.Instances->PumpForTest();
			return C->Secondary.Journal->IsDurable(C->Ticket) && C->bStaged && C->Secondary.Instances->GetPrimaryView().IsSet()
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 15.0);
		Steps.Do(TEXT("main is byte-identical; the secondary's files are in its namespace; the view is read-only"), [this, C]()
		{
			const FString Sec = C->Secondary.Root.JournalDir(TEXT("i-4242-1000"));
			TestTrue(TEXT("main is byte-identical after the secondary's work (§10.5: a secondary never writes another namespace)"),
				SameTree(C->MainBefore, ReadTree(C->Primary.Root.JournalDir(TEXT("main")))));
			TArray<FString> Wal;
			IFileManager::Get().FindFiles(Wal, *(Sec / TEXT("wal.*.sjl")), true, false);
			TestTrue(TEXT("the secondary wrote its own generation"), Wal.Num() >= 1);
			TArray<FString> Blobs;
			IFileManager::Get().FindFilesRecursive(Blobs, *(Sec / TEXT("pending-history") / TEXT("objects")), TEXT("*.z"), true, false);
			TestTrue(TEXT("its history blob is staged in <sec>/pending-history"), Blobs.Num() >= 1);
			const FString Blob = DreamText::Utf8ToFString(dream::history::BlobRelativePath(DreamText::ToUtf8(DreamHash::Sha256Hex("secondary_text = 2\n"))));
			TestTrue(TEXT("the secondary's text is staged as a blob in <sec>/pending-history"), IFileManager::Get().FileExists(*(Sec / TEXT("pending-history") / Blob)));
			TestFalse(TEXT("and never written to <root>/history (only the primary writes it, §10.5)"),
				IFileManager::Get().FileExists(*(C->Primary.Root.HistoryDir() / Blob)));
			const FDreamOrphanNamespace& View = C->Secondary.Instances->GetPrimaryView().GetValue();
			TestTrue(TEXT("the view is read-only"), View.bReadOnly);
			const FGuid PrimaryId = C->PrimaryBuffer->GetId();
			const FDreamRecoveredBuffer* Seen = View.Buffers.FindByPredicate([&PrimaryId](const FDreamRecoveredBuffer& B) { return B.Id == PrimaryId; });
			if (TestNotNull(TEXT("View its tabs lists the primary's buffer"), Seen))
			{
				TestEqual(TEXT("with its text"), Seen->RecoveredText, FString(TEXT("primary_text = 1\n")));
			}
			return true;
		});
		Steps.Finally(TEXT("close both and remove the private root"), [C]()
		{
			Close(C->Secondary);
			Close(C->Primary);
			RemoveTree(C->Base);
		});
		Steps.Run(Done);
	});

	LatentIt("the primary merges a crashed secondary's namespace: its texts, history and REPL entries, then deletes it (T-PER-045 shape)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Base;
			FNode First;
			FNode Secondary;
			FNode Next;
			TSharedPtr<FDreamBuffer> SecondaryBuffer;
			FDreamJournalTicket Ticket;
			bool bStaged = false;
			FString Key;
			TOptional<TArray<FDreamLocalHistoryEntry>> Entries;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Base = PrivateBase();
		C->First.Root = MakeRoot(C->Base, TEXT("DreamT4Inst-b00002"));
		C->Secondary.Root = C->First.Root;
		C->Next.Root = C->First.Root;
		const FString Sec = C->First.Root.JournalDir(TEXT("i-4343-2000"));
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a primary, then a secondary beside it"), [this, C]()
		{
			if (!Open(*this, C->First))
			{
				return false;
			}
			C->First.Journal->Reconcile({});
			return Open(*this, C->Secondary, TEXT("i-4343-2000"));
		});
		Steps.Poll(TEXT("the scans"), [C]() { return C->First.bScanned && C->Secondary.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the secondary types, runs a REPL line and stages a history entry"), [C]()
		{
			C->SecondaryBuffer = MakeScratch(TEXT("InstCrash"), TEXT("unsaved_in_secondary = 3\n"));
			C->Secondary.Journal->Reconcile({ C->SecondaryBuffer.ToSharedRef() });
			C->Ticket = Snapshot(C->Secondary, C->SecondaryBuffer.ToSharedRef());
			C->Secondary.Repl->Add(TEXT("print('from the secondary')"), EDreamHistorySource::Repl);
			C->Secondary.History->AddEntry(C->SecondaryBuffer->GetId(), EDreamHistoryKind::Save, C->SecondaryBuffer->Snapshot(), FString(),
				C->SecondaryBuffer->GetDisplayName(), TOptional<int32>(), [C](bool bOk) { C->bStaged = bOk; });
			return true;
		});
		Steps.Poll(TEXT("durable and staged"), [C]() { return C->Secondary.Journal->IsDurable(C->Ticket) && C->bStaged ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("the secondary crashes; the primary exits cleanly; a new primary opens"), [this, C, Sec]()
		{
			C->Secondary.Repl->WaitForWrites();
			TestTrue(TEXT("the secondary staged its REPL entry in <sec>/repl-history.jsonl"), IFileManager::Get().FileExists(*(Sec / TEXT("repl-history.jsonl"))));
			Close(C->Secondary, /*bClean*/ false);
			Close(C->First, /*bClean*/ true);
			return Open(*this, C->Next);
		});
		Steps.Poll(TEXT("the new primary's scan"), [C]() { return C->Next.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the crashed namespace is an orphan with its buffer, never counted"), [this, C]()
		{
			C->Next.Journal->Reconcile({});
			TestEqual(TEXT("nothing of the orphan is counted in n (§10.7.1)"), C->Next.Session.CountedBuffers, 0);
			const FDreamOrphanNamespace* Orphan = C->Next.Session.Orphans.FindByPredicate([](const FDreamOrphanNamespace& O) { return O.Namespace == TEXT("i-4343-2000"); });
			if (!TestNotNull(TEXT("the scan lists i-4343-2000 (§10.5 Orphans)"), Orphan))
			{
				return false;
			}
			TestTrue(TEXT("a second editor instance"), Orphan->Source == EDreamOrphanSource::SecondaryInstance);
			TestFalse(TEXT("that ended without CLEAN"), Orphan->bClean);
			TestEqual(TEXT("its PID comes from its name"), static_cast<int32>(Orphan->Pid), 4343);
			TestTrue(TEXT("its ns.lock is held while it is listed"), Orphan->NamespaceLock != 0);
			const FGuid Id = C->SecondaryBuffer->GetId();
			const FDreamRecoveredBuffer* Rec = Orphan->Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; });
			if (TestNotNull(TEXT("its unsaved scratch buffer is offered"), Rec))
			{
				TestEqual(TEXT("with its text"), Rec->RecoveredText, FString(TEXT("unsaved_in_secondary = 3\n")));
			}
			C->Key = Orphan->Key;
			C->Next.Instances->AddSameRootOrphans(MoveTemp(C->Next.Session.Orphans));
			C->Next.Session.Orphans.Reset();
			TestEqual(TEXT("FDreamInstances offers it"), C->Next.Instances->CountOfferedBuffers(), 1);
			C->Next.Instances->ResolveOrphanBuffer(C->Key, Id, /*bDiscarded*/ false);
			return true;
		});
		Steps.Poll(TEXT("merged and deleted"), [C]()
		{
			C->Next.Instances->PumpForTest();
			return C->Next.Instances->GetDeletedForTest().Contains(C->Key) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Do(TEXT("the namespace is gone; its REPL entry joined; history holds its staged and recovered entries"), [this, C, Sec]()
		{
			TestFalse(TEXT("the namespace folder is deleted only after the merge"), IFileManager::Get().DirectoryExists(*Sec));
			const TArray<FDreamHistoryEntry> Repl = C->Next.Repl->Query(TEXT("print('from the secondary')"), EDreamHistoryMatch::Prefix, 0);
			TestEqual(TEXT("the secondary's REPL entry joined the primary's history (§10.5 Merge)"), Repl.Num(), 1);
			C->Next.History->Query(C->SecondaryBuffer->GetId(), [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->Entries = MoveTemp(Entries); });
			return true;
		});
		Steps.Poll(TEXT("the history query"), [C]() { return C->Entries.IsSet() ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("a staged save entry and a recovered entry for that buffer"), [this, C]()
		{
			int32 Saves = 0;
			int32 Recovered = 0;
			for (const FDreamLocalHistoryEntry& E : C->Entries.GetValue())
			{
				Saves += E.Kind == EDreamHistoryKind::Save ? 1 : 0;
				Recovered += E.Kind == EDreamHistoryKind::Recovered ? 1 : 0;
			}
			TestEqual(TEXT("the staged save entry was merged by id"), Saves, 1);
			TestEqual(TEXT("its text was copied into history before the delete"), Recovered, 1);
			return true;
		});
		Steps.Finally(TEXT("close and remove the private root"), [C]()
		{
			Close(C->Secondary, false);
			Close(C->First);
			Close(C->Next);
			RemoveTree(C->Base);
		});
		Steps.Run(Done);
	});

	LatentIt("the secondary's poll sees the primary end without CLEAN and offers its buffers read-only (T-PER-023)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Base;
			FNode Primary;
			FNode Secondary;
			TSharedPtr<FDreamBuffer> Buffer;
			FDreamJournalTicket Ticket;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Base = PrivateBase();
		C->Primary.Root = MakeRoot(C->Base, TEXT("DreamT4Inst-c00003"));
		C->Secondary.Root = C->Primary.Root;
		AddExpectedMessagePlain(TEXT("The other editor instance ended unexpectedly"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a primary with a buffer and a secondary that polls every 0.2 s"), [this, C]()
		{
			if (!Open(*this, C->Primary))
			{
				return false;
			}
			C->Buffer = MakeScratch(TEXT("InstPoll"), TEXT("primary_unsaved = 4\n"));
			C->Primary.Journal->Reconcile({ C->Buffer.ToSharedRef() });
			C->Ticket = Snapshot(C->Primary, C->Buffer.ToSharedRef());
			if (!Open(*this, C->Secondary, TEXT("i-4444-3000")))
			{
				return false;
			}
			C->Secondary.Instances->SetPollSecondsForTest(0.2);
			return true;
		});
		Steps.Poll(TEXT("durable"), [C]() { return C->Primary.Journal->IsDurable(C->Ticket) ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the primary crashes"), [this, C]()
		{
			TestFalse(TEXT("nothing is offered while the primary lives"), C->Secondary.Instances->HasPrimaryEndedUnexpectedly());
			Close(C->Primary, /*bClean*/ false);
			return true;
		});
		Steps.Poll(TEXT("the poll notices"), [C]()
		{
			C->Secondary.Instances->PumpForTest();
			return C->Secondary.Instances->HasPrimaryEndedUnexpectedly() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 20.0);
		Steps.Do(TEXT("its buffers are offered read-only (Open a copy)"), [this, C]()
		{
			const TOptional<FDreamOrphanNamespace>& View = C->Secondary.Instances->GetPrimaryView();
			if (!TestTrue(TEXT("Review its buffers has the primary's namespace"), View.IsSet()))
			{
				return false;
			}
			TestTrue(TEXT("read-only: a secondary never writes, rotates or deletes another namespace"), View->bReadOnly);
			const FGuid Id = C->Buffer->GetId();
			TestTrue(TEXT("the crashed primary's buffer is listed"), View->Buffers.ContainsByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; }));
			TestTrue(TEXT("main is still there (only a primary deletes)"), IFileManager::Get().DirectoryExists(*C->Primary.Root.JournalDir(TEXT("main"))));
			return true;
		});
		Steps.Finally(TEXT("close and remove"), [C]()
		{
			Close(C->Secondary);
			Close(C->Primary, false);
			RemoveTree(C->Base);
		});
		Steps.Run(Done);
	});

	LatentIt("an older build on a newer journal journals in compat-<format>, and the newer side lists it as an orphan (T-PER-025)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Base;
			FNode Old;
			FNode Newer;
			TSharedPtr<FDreamBuffer> Buffer;
			FDreamJournalTicket Ticket;
			FString NewerFile;
			TArray<uint8> NewerBytes;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Base = PrivateBase();
		C->Old.Root = MakeRoot(C->Base, TEXT("DreamT4Inst-d00004"));
		C->Newer.Root = C->Old.Root;
		const FString Compat = DreamText::Utf8ToFString(dream::storage::MakeCompatNamespace(dream::journal::FormatMajor, dream::journal::FormatMinor));
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("main holds a generation of a newer major; this build opens the root"), [this, C]()
		{
			const FString Main = C->Old.Root.JournalDir(TEXT("main"));
			IFileManager::Get().MakeDirectory(*Main, true);
			dream::journal::FileHeader Header;
			Header.Major = static_cast<uint16_t>(dream::journal::FormatMajor + 2);
			Header.Minor = 0;
			Header.Generation = 7;
			std::string Bytes;
			dream::journal::EncodeHeader(Header, Bytes);
			Bytes += "payload of a format this build cannot read";
			C->NewerFile = Main / DreamText::Utf8ToFString(dream::journal::GenerationFileName(7));
			C->NewerBytes = TArray<uint8>(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size()));
			FFileHelper::SaveArrayToFile(C->NewerBytes, *C->NewerFile);
			return Open(*this, C->Old);
		});
		Steps.Poll(TEXT("the scan"), [C]() { return C->Old.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("it says newer format, compat, and journals there"), [this, C, Compat]()
		{
			TestTrue(TEXT("a newer format"), C->Old.Session.bNewerFormat);
			TestTrue(TEXT("this session journals in compat-* (§10.6)"), C->Old.Session.bCompat);
			TestEqual(TEXT("the banner can name the newer major"), C->Old.Session.NewerMajor, static_cast<int32>(dream::journal::FormatMajor + 2));
			TestEqual(TEXT("the namespace written"), C->Old.Journal->GetNamespace(), Compat);
			TestTrue(TEXT("not read-only: it journals"), C->Old.Journal->GetHealth() != EDreamJournalHealth::ReadOnly);
			C->Buffer = MakeScratch(TEXT("InstCompat"), TEXT("older_build = 5\n"));
			C->Old.Journal->Reconcile({ C->Buffer.ToSharedRef() });
			C->Ticket = Snapshot(C->Old, C->Buffer.ToSharedRef());
			return true;
		});
		Steps.Poll(TEXT("durable"), [C]() { return C->Old.Journal->IsDurable(C->Ticket) ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("main is unchanged; compat has the session; it crashes"), [this, C, Compat]()
		{
			TArray<uint8> Now;
			FFileHelper::LoadFileToArray(Now, *C->NewerFile);
			TestTrue(TEXT("the newer generation is byte-identical"), Now == C->NewerBytes);
			TArray<FString> MainFiles;
			IFileManager::Get().FindFiles(MainFiles, *(C->Old.Root.JournalDir(TEXT("main")) / TEXT("wal.*")), true, false);
			TestEqual(TEXT("nothing was added to main"), MainFiles.Num(), 1);
			TArray<FString> CompatFiles;
			IFileManager::Get().FindFiles(CompatFiles, *(C->Old.Root.JournalDir(Compat) / TEXT("wal.*.sjl")), true, false);
			TestTrue(TEXT("compat-* holds this session's generation"), CompatFiles.Num() >= 1);
			Close(C->Old, /*bClean*/ false);
			// The newer build's side: here, main readable again (the newer file removed), so this build plays it.
			IFileManager::Get().Delete(*C->NewerFile);
			return Open(*this, C->Newer);
		});
		Steps.Poll(TEXT("the newer side's scan"), [C]() { return C->Newer.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("compat-* is listed as an orphan with its buffer"), [this, C, Compat]()
		{
			C->Newer.Journal->Reconcile({});
			const FDreamOrphanNamespace* Orphan = C->Newer.Session.Orphans.FindByPredicate([&Compat](const FDreamOrphanNamespace& O) { return O.Namespace == Compat; });
			if (TestNotNull(TEXT("the compat namespace is an orphan (§10.6)"), Orphan))
			{
				TestTrue(TEXT("of source Compat"), Orphan->Source == EDreamOrphanSource::Compat);
				const FGuid Id = C->Buffer->GetId();
				TestTrue(TEXT("its buffer is offered"), Orphan->Buffers.ContainsByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; }));
			}
			return true;
		});
		Steps.Finally(TEXT("close and remove"), [C]()
		{
			Close(C->Old, false);
			Close(C->Newer);
			RemoveTree(C->Base);
		});
		Steps.Run(Done);
	});

	It("storage.root_override: owner.json at first use; another account, the project tree and silence fall back with a notice (T-PER-053)", [this]()
	{
		const FString Base = PrivateBase();
		const FString UProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		FDreamJournal Journal;
		FDreamLocalHistory History(Journal);
		FDreamReplHistory Repl;
		{
			FDreamInstances Instances(Journal, History, Repl);
			const FString Good = Base / TEXT("override-good");
			TValueOrError<FDreamStorageRoot, FText> Root = Instances.ResolveStorageRoot(UProject, Good, 10.0);
			if (TestTrue(TEXT("a root"), Root.HasValue()))
			{
				TestTrue(TEXT("the override is used"), Root.GetValue().bIsOverride);
				TestTrue(TEXT("<EngineVer>/<ProjectHash> is appended to the override"), Root.GetValue().Root.StartsWith(Good / DreamStorageRoot::EngineVersionDir()));
				TestTrue(TEXT("its hash folder is new"), Root.GetValue().bIsNew);
				TestEqual(TEXT("no notice"), Instances.GetStorageNotices().Num(), 0);
				FString Owner;
				FFileHelper::LoadFileToString(Owner, *(Good / TEXT("owner.json")));
				TestTrue(TEXT("owner.json names this account's SID (written at first use)"), !Owner.IsEmpty()
					&& Owner.Contains(FDreamWin32FileOps::GetCurrentUserSid()));
			}

			const FString Other = Base / TEXT("override-other");
			IFileManager::Get().MakeDirectory(*Other, true);
			FFileHelper::SaveStringToFile(TEXT("{\"schema\": 1, \"sid\": \"S-1-5-21-1-2-3-1001\"}"), *(Other / TEXT("owner.json")));
			AddExpectedMessagePlain(TEXT("belongs to another account"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
			Root = Instances.ResolveStorageRoot(UProject, Other, 10.0);
			if (TestTrue(TEXT("a fallback root"), Root.HasValue()))
			{
				TestFalse(TEXT("another account's override is rejected"), Root.GetValue().bIsOverride);
				TestTrue(TEXT("the platform default is used"), Root.GetValue().Root.StartsWith(DreamStorageRoot::PlatformDefaultBase()));
				TestTrue(TEXT("with a notice"), Instances.GetStorageNotices().Num() == 1
					&& Instances.GetStorageNotices()[0].ToString().Contains(TEXT("another account")));
			}
			TArray<FString> Written;
			IFileManager::Get().FindFilesRecursive(Written, *Other, TEXT("*"), true, true);
			TestEqual(TEXT("nothing is written under a rejected override (owner.json only)"), Written.Num(), 1);

			AddExpectedMessagePlain(TEXT("is under the engine or the project folder"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
			Root = Instances.ResolveStorageRoot(UProject, FPaths::ProjectSavedDir() / TEXT("DreamT4NeverUsed"), 10.0);
			TestTrue(TEXT("an override inside the project tree is refused (§10.1)"), Root.HasValue() && !Root.GetValue().bIsOverride
				&& Instances.GetStorageNotices().Num() == 1);
			TestFalse(TEXT("and nothing was created there"), IFileManager::Get().DirectoryExists(*(FPaths::ProjectSavedDir() / TEXT("DreamT4NeverUsed"))));

			AddExpectedMessagePlain(TEXT("did not answer"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
			Instances.BlockProbeForTest(1.5);
			const double Start = FPlatformTime::Seconds();
			Root = Instances.ResolveStorageRoot(UProject, Base / TEXT("override-silent"), 0.3);
			const double Waited = FPlatformTime::Seconds() - Start;
			TestTrue(*FString::Printf(TEXT("a silent override costs the bounded wait only (%.2f s)"), Waited), Waited < 1.2);
			TestTrue(TEXT("it counts as not writable for this session"), Root.HasValue() && !Root.GetValue().bIsOverride);
			TestTrue(TEXT("with the did-not-answer notice"), Instances.GetStorageNotices().Num() == 1
				&& Instances.GetStorageNotices()[0].ToString().Contains(TEXT("did not answer")));
			for (int32 Spin = 0; Spin < 100 && !Instances.IsIdleForTest(); ++Spin)
			{
				FPlatformProcess::Sleep(0.05f);   // the late probe finishes before the root is removed
			}

			// (review r1, persist-M3) Strict, as -DreamStorageRoot is: no fallback to the platform default, ever.
			AddExpectedMessagePlain(TEXT("this session runs without a journal"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
			Root = Instances.ResolveStorageRoot(UProject, Other, 10.0, /*bStrict*/ true);
			TestTrue(TEXT("strict: another account's root is an error, not the platform default"), Root.HasError());
			TestFalse(TEXT("strict: nothing resolved"), Instances.GetResolvedRoot().IsSet());
			TestTrue(TEXT("strict: with the no-journal notice"), Instances.GetStorageNotices().Num() == 1
				&& Instances.GetStorageNotices()[0].ToString().Contains(TEXT("runs without a journal")));
			Instances.BlockProbeForTest(1.5);
			Root = Instances.ResolveStorageRoot(UProject, Base / TEXT("override-silent-strict"), 0.3, /*bStrict*/ true);
			TestTrue(TEXT("strict: a silent root is an error too"), Root.HasError() && !Instances.GetResolvedRoot().IsSet());
			for (int32 Spin = 0; Spin < 100 && !Instances.IsIdleForTest(); ++Spin)
			{
				FPlatformProcess::Sleep(0.05f);
			}
			Root = Instances.ResolveStorageRoot(UProject, Good, 10.0, /*bStrict*/ true);
			TestTrue(TEXT("strict: a usable root is used"), Root.HasValue() && Root.GetValue().bIsOverride && Instances.GetResolvedRoot().IsSet());
			Instances.Shutdown();
		}
		History.Shutdown();
		RemoveTree(Base);
	});

	LatentIt("projects.json: this project's entry, a moved project offered, Ignore remembered, Import listed (T-PER-013 shape)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Base;
			FString ProjectsJson;
			FString UProject;
			FNode Old;            // the moved project's last session: a crash with an unsaved buffer
			FNode Ours;
			TSharedPtr<FDreamBuffer> OldBuffer;
			FDreamJournalTicket Ticket;
			FString Name;
			FString ImportedKey;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Base = PrivateBase();
		C->ProjectsJson = C->Base / TEXT("projects.json");
		C->UProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		C->Name = DreamStorageRoot::SanitizeName(FPaths::GetBaseFilename(C->UProject));
		C->Old.Root = MakeRoot(C->Base, C->Name + TEXT("-0ld000"));
		C->Ours.Root = MakeRoot(C->Base, C->Name + TEXT("-0ur000"));
		C->Ours.Root.bIsNew = true;
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("the moved project's last session crashes with an unsaved buffer; projects.json names its gone path"), [this, C]()
		{
			const FString Json = FString::Printf(TEXT("{\"schema\": 1, \"projects\": {\"%s-0ld000\": {\"uproject\": \"c:/dreamt4/gone/%s.uproject\", \"name\": \"%s\", \"file_id\": \"\", \"root\": \"%s\", \"last_seen_utc\": \"2026-10-28T10:00:00Z\"}}}"),
				*C->Name, *C->Name, *C->Name, *C->Old.Root.Root);
			FFileHelper::SaveStringToFile(Json, *C->ProjectsJson);
			return Open(*this, C->Old);
		});
		Steps.Poll(TEXT("scan"), [C]() { return C->Old.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("journal it"), [C]()
		{
			C->OldBuffer = MakeScratch(TEXT("InstMoved"), TEXT("moved_project_text = 6\n"));
			C->Old.Journal->Reconcile({ C->OldBuffer.ToSharedRef() });
			C->Ticket = Snapshot(C->Old, C->OldBuffer.ToSharedRef());
			return true;
		});
		Steps.Poll(TEXT("durable"), [C]() { return C->Old.Journal->IsDurable(C->Ticket) ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("it crashes; this project opens in a new folder and updates projects.json"), [this, C]()
		{
			Close(C->Old, /*bClean*/ false);
			if (!Open(*this, C->Ours))
			{
				return false;
			}
			C->Ours.Instances->UpdateProjects(C->ProjectsJson, C->UProject, C->Ours.Root);
			return true;
		});
		Steps.Poll(TEXT("projects.json updated"), [C]() { return C->Ours.bScanned && WaitIdle(C->Ours) == DreamTest::EStep::Next ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("the moved project is offered; our entry is written"), [this, C]()
		{
			C->Ours.Journal->Reconcile({});
			const TArray<FDreamMovedProject>& Moved = C->Ours.Instances->GetMovedProjects();
			if (TestEqual(TEXT("one moved project offered (§10.1)"), Moved.Num(), 1))
			{
				TestEqual(TEXT("the old hash"), Moved[0].OldHash, C->Name + TEXT("-0ld000"));
			}
			FString Text;
			FFileHelper::LoadFileToString(Text, *C->ProjectsJson);
			dream::storage::ProjectsIndex Index;
			if (TestTrue(TEXT("projects.json parses"), dream::storage::ParseProjects(DreamText::ToUtf8(Text), Index)))
			{
				const dream::storage::ProjectEntry* Ours = Index.Find(DreamText::ToUtf8(C->Name + TEXT("-0ur000")));
				if (TestNotNull(TEXT("our entry"), Ours))
				{
					TestEqual(TEXT("its root"), DreamText::Utf8ToFString(Ours->Root), C->Ours.Root.Root);
					TestFalse(TEXT("its .uproject identity (file_id)"), Ours->FileId.empty());
					TestEqual(TEXT("the offer is pending until answered"), static_cast<int32>(Ours->PendingImports.size()), 1);
				}
			}
			C->Ours.Instances->ImportMovedProject(C->Name + TEXT("-0ld000"));
			return true;
		});
		Steps.Poll(TEXT("imported and listed"), [C]()
		{
			C->Ours.Instances->PumpForTest();
			return C->Ours.Instances->GetOrphans().Num() > 0 && C->Ours.Instances->IsIdleForTest() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 20.0);
		Steps.Do(TEXT("imported-<oldhash> is an orphan with the buffer; the answer is remembered"), [this, C]()
		{
			const FDreamOrphanNamespace& Orphan = C->Ours.Instances->GetOrphans()[0];
			TestEqual(TEXT("copied to imported-<oldhash>"), Orphan.Namespace, TEXT("imported-") + C->Name + TEXT("-0ld000"));
			TestTrue(TEXT("of source Imported"), Orphan.Source == EDreamOrphanSource::Imported);
			const FGuid Id = C->OldBuffer->GetId();
			TestTrue(TEXT("its unsaved buffer is offered"), Orphan.Buffers.ContainsByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; }));
			TestTrue(TEXT("the old root is left as it was (copied, never moved)"), IFileManager::Get().DirectoryExists(*C->Old.Root.JournalDir(TEXT("main"))));
			FString Text;
			FFileHelper::LoadFileToString(Text, *C->ProjectsJson);
			dream::storage::ProjectsIndex Index;
			dream::storage::ParseProjects(DreamText::ToUtf8(Text), Index);
			const dream::storage::ProjectEntry* Ours = Index.Find(DreamText::ToUtf8(C->Name + TEXT("-0ur000")));
			TestTrue(TEXT("Import is remembered in projects.json"), Ours != nullptr && Ours->PendingImports.empty() && Ours->DecidedImports.size() == 1);
			// A later start (the folder not new) is offered nothing.
			FDreamStorageRoot Later = C->Ours.Root;
			Later.bIsNew = false;
			C->Ours.Instances->UpdateProjects(C->ProjectsJson, C->UProject, Later);
			return true;
		});
		Steps.Poll(TEXT("the second update"), [C]() { return WaitIdle(C->Ours); }, 15.0);
		Steps.Do(TEXT("no offer at the next start"), [this, C]()
		{
			TestEqual(TEXT("after Import or Ignore nothing is offered again"), C->Ours.Instances->GetMovedProjects().Num(), 0);
			return true;
		});
		Steps.Finally(TEXT("close and remove"), [C]()
		{
			Close(C->Old, false);
			Close(C->Ours);
			RemoveTree(C->Base);
		});
		Steps.Run(Done);
	});

	LatentIt("the cross-root scan lists another root's unclean namespace, deletes it once resolved, and leaves live and foreign roots alone (T-PER-055 shape)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Base;
			FNode Crashed;        // root A: an unclean session, nobody holds it
			FNode Live;           // root C: a running primary
			FNode Foreign;        // root D: unclean, but its owner.json names another account
			FNode Ours;           // root B
			TArray<TSharedPtr<FDreamBuffer>> Buffers;
			TArray<FDreamJournalTicket> Tickets;
			FString CrashedKey;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Base = PrivateBase();
		C->Crashed.Root = MakeRoot(C->Base / TEXT("baseA"), TEXT("DreamT4Inst-e00005"));
		C->Live.Root = MakeRoot(C->Base / TEXT("baseC"), TEXT("DreamT4Inst-e00005"));
		C->Foreign.Root = MakeRoot(C->Base / TEXT("baseD"), TEXT("DreamT4Inst-e00005"));
		C->Ours.Root = MakeRoot(C->Base / TEXT("baseB"), TEXT("DreamT4Inst-e00005"));
		FFileHelper::SaveStringToFile(TEXT("{\"schema\": 1, \"sid\": \"S-1-5-21-9-9-9-1001\"}"), *(C->Base / TEXT("baseD") / TEXT("owner.json")));
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("three other roots each journal a buffer"), [this, C]()
		{
			return Open(*this, C->Crashed) && Open(*this, C->Live) && Open(*this, C->Foreign);
		});
		Steps.Poll(TEXT("their scans"), [C]() { return C->Crashed.bScanned && C->Live.bScanned && C->Foreign.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("journal"), [C]()
		{
			for (FNode* Node : { &C->Crashed, &C->Live, &C->Foreign })
			{
				TSharedRef<FDreamBuffer> Buffer = MakeScratch(TEXT("InstCross"), FString::Printf(TEXT("text_of_%s = 7\n"), *FPaths::GetCleanFilename(Node->Root.BaseDir)));
				Node->Journal->Reconcile({ Buffer });
				C->Tickets.Add(Snapshot(*Node, Buffer));
				C->Buffers.Add(Buffer);
			}
			return true;
		});
		Steps.Poll(TEXT("durable"), [C]()
		{
			return C->Crashed.Journal->IsDurable(C->Tickets[0]) && C->Live.Journal->IsDurable(C->Tickets[1]) && C->Foreign.Journal->IsDurable(C->Tickets[2])
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("A and D crash, C keeps running; our primary on B scans A, C and D"), [this, C]()
		{
			Close(C->Crashed, false);
			Close(C->Foreign, false);
			if (!Open(*this, C->Ours))
			{
				return false;
			}
			return true;
		});
		Steps.Poll(TEXT("our scan"), [C]() { return C->Ours.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the cross-root scan"), [C]()
		{
			C->Ours.Journal->Reconcile({});
			C->Ours.Instances->StartCrossRootScan({ C->Crashed.Root.Root, C->Live.Root.Root, C->Foreign.Root.Root, C->Ours.Root.Root });
			return true;
		});
		Steps.Poll(TEXT("its result"), [C]() { return WaitIdle(C->Ours); }, 20.0);
		Steps.Do(TEXT("only A is listed"), [this, C]()
		{
			const TArray<FDreamOrphanNamespace>& Orphans = C->Ours.Instances->GetOrphans();
			TestEqual(TEXT("one namespace from another storage folder"), Orphans.Num(), 1);
			if (Orphans.Num() != 1)
			{
				return false;
			}
			TestEqual(TEXT("A's main"), Orphans[0].Root, C->Crashed.Root.Root);
			TestTrue(TEXT("of source OtherRoot"), Orphans[0].Source == EDreamOrphanSource::OtherRoot);
			TestEqual(TEXT("no lock is held for another root's namespace"), static_cast<int64>(Orphans[0].NamespaceLock), static_cast<int64>(0));
			C->CrashedKey = Orphans[0].Key;
			C->Ours.Instances->ResolveOrphanBuffer(C->CrashedKey, C->Buffers[0]->GetId(), /*bDiscarded*/ true);
			return true;
		});
		Steps.Poll(TEXT("deleted"), [C]()
		{
			C->Ours.Instances->PumpForTest();
			return C->Ours.Instances->GetDeletedForTest().Contains(C->CrashedKey) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Do(TEXT("A's namespace is gone; C's and D's are intact"), [this, C]()
		{
			TestFalse(TEXT("A's resolved namespace is deleted (both locks held, wal files unchanged)"),
				IFileManager::Get().DirectoryExists(*C->Crashed.Root.JournalDir(TEXT("main"))));
			TestTrue(TEXT("the live root's journal is intact"), IFileManager::Get().DirectoryExists(*C->Live.Root.JournalDir(TEXT("main"))));
			TestTrue(TEXT("the other account's root was never touched"), IFileManager::Get().DirectoryExists(*C->Foreign.Root.JournalDir(TEXT("main"))));
			return true;
		});
		Steps.Finally(TEXT("close and remove"), [C]()
		{
			Close(C->Crashed, false);
			Close(C->Live);
			Close(C->Foreign, false);
			Close(C->Ours);
			RemoveTree(C->Base);
		});
		Steps.Run(Done);
	});

	LatentIt("a namespace whose staged history or REPL entries cannot be kept is not deleted, and merges at a later start (review r1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Base;
			FNode First;
			FNode Secondary;
			FNode Next;
			FString Sec;
			FString Starting;
			FString Blocker;
			FGuid Closed;
			bool bStaged = false;
			IFileHandle* Locked = nullptr;
			int32 Phase = 0;
			TOptional<TArray<FDreamLocalHistoryEntry>> Entries;

			void Unlock()
			{
				delete Locked;
				Locked = nullptr;
			}
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Base = PrivateBase();
		C->First.Root = MakeRoot(C->Base, TEXT("DreamT4Inst-f00006"));
		C->Secondary.Root = C->First.Root;
		C->Next.Root = C->First.Root;
		C->Sec = C->First.Root.JournalDir(TEXT("i-4545-5000"));
		C->Starting = C->First.Root.JournalDir(TEXT("i-4646-6000"));
		C->Closed = FGuid::NewGuid();
		const FString StagedText = TEXT("closed_in_the_secondary = 8\n");
		AddExpectedMessagePlain(TEXT("Local history could not copy"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		AddExpectedMessagePlain(TEXT("could not be read"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		AddExpectedMessagePlain(TEXT("was not deleted"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a primary and a secondary; the secondary stages a closed tab's text and a REPL line, then crashes"), [this, C]()
		{
			if (!Open(*this, C->First))
			{
				return false;
			}
			C->First.Journal->Reconcile({});
			return Open(*this, C->Secondary, TEXT("i-4545-5000"));
		});
		Steps.Poll(TEXT("the scans"), [C]() { return C->First.bScanned && C->Secondary.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("stage"), [C, StagedText]()
		{
			C->Secondary.Journal->Reconcile({});
			C->Secondary.History->AddEntry(C->Closed, EDreamHistoryKind::Closed, dream::TextBuffer(DreamText::ToU16(StagedText)).Snapshot(), FString(),
				TEXT("Untitled-8"), TOptional<int32>(), [C](bool bOk) { C->bStaged = bOk; });
			C->Secondary.Repl->Add(TEXT("print('staged by the secondary')"), EDreamHistorySource::Repl);
			return true;
		});
		Steps.Poll(TEXT("staged"), [C]() { return C->bStaged ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("A: the secondary crashes, a second one is starting (its folder, no ns.lock yet), the blob folder is blocked"), [this, C, StagedText]()
		{
			C->Secondary.Repl->WaitForWrites();
			Close(C->Secondary, /*bClean*/ false);
			Close(C->First);
			// The offered buffers are none (the tab was closed), so the namespace is finalized at once.
			IFileManager::Get().MakeDirectory(*C->Starting, true);
			// A file where the blob's objects/<aa> folder must go: every copy into history fails on this side.
			const FString Sha = DreamHash::Sha256Hex(DreamText::ToUtf8(StagedText));
			IFileManager::Get().MakeDirectory(*(C->First.Root.HistoryDir() / TEXT("objects")), true);
			C->Blocker = C->First.Root.HistoryDir() / TEXT("objects") / Sha.Left(2);
			FFileHelper::SaveStringToFile(TEXT("not a folder"), *C->Blocker);
			C->Phase = 1;
			return Open(*this, C->Next);
		});
		auto Finalized = [C]()
		{
			if (!C->Next.bScanned)
			{
				return DreamTest::EStep::Wait;
			}
			if (C->Next.Session.Orphans.Num() > 0)
			{
				C->Next.Journal->Reconcile({});
				C->Next.Instances->AddSameRootOrphans(MoveTemp(C->Next.Session.Orphans));
				C->Next.Session.Orphans.Reset();
			}
			return WaitIdle(C->Next);
		};
		Steps.Poll(TEXT("A: finalize"), Finalized, 20.0);
		Steps.Do(TEXT("A: the merge failed on this side: the namespace is kept; the starting one is untouched"), [this, C]()
		{
			TestTrue(TEXT("a blocked blob copy keeps the namespace (persist-M1)"), IFileManager::Get().DirectoryExists(*C->Sec));
			TestEqual(TEXT("nothing was deleted"), C->Next.Instances->GetDeletedForTest().Num(), 0);
			TestTrue(TEXT("a folder without its ns.lock yet is not taken or deleted (review r1)"), IFileManager::Get().DirectoryExists(*C->Starting)
				&& !IFileManager::Get().FileExists(*(C->Starting / TEXT("ns.lock"))));
			TestFalse(TEXT("nor listed"), C->Next.Instances->GetOrphans().ContainsByPredicate([](const FDreamOrphanNamespace& O) { return O.Namespace == TEXT("i-4646-6000"); }));
			Close(C->Next);
			IFileManager::Get().Delete(*C->Blocker);
			// B: history.jsonl locked without read sharing: the primary cannot write its REPL history (OUT-03).
			C->Locked = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*(C->First.Root.ReplDir() / TEXT("history.jsonl")), /*bAppend*/ true, /*bAllowRead*/ false);
			TestNotNull(TEXT("history.jsonl is locked for this phase"), C->Locked);
			C->Phase = 2;
			return Open(*this, C->Next);
		});
		Steps.Poll(TEXT("B: finalize"), Finalized, 20.0);
		Steps.Do(TEXT("B: the REPL entries could not be written: kept"), [this, C]()
		{
			TestTrue(TEXT("unwritable history.jsonl keeps the namespace (persist-M1)"), IFileManager::Get().DirectoryExists(*C->Sec));
			TestEqual(TEXT("nothing was deleted"), C->Next.Instances->GetDeletedForTest().Num(), 0);
			Close(C->Next);
			C->Unlock();
			// B2: the staged file itself locked: present but unreadable.
			C->Locked = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*(C->Sec / TEXT("repl-history.jsonl")), /*bAppend*/ true, /*bAllowRead*/ false);
			TestNotNull(TEXT("the staged file is locked for this phase"), C->Locked);
			C->Phase = 3;
			return Open(*this, C->Next);
		});
		Steps.Poll(TEXT("B2: finalize"), Finalized, 20.0);
		Steps.Do(TEXT("B2: an unreadable staged file: kept"), [this, C]()
		{
			TestTrue(TEXT("an unreadable staged REPL file keeps the namespace (persist-M1)"), IFileManager::Get().DirectoryExists(*C->Sec));
			TestEqual(TEXT("nothing was deleted"), C->Next.Instances->GetDeletedForTest().Num(), 0);
			Close(C->Next);
			C->Unlock();
			C->Phase = 4;
			return Open(*this, C->Next);
		});
		Steps.Poll(TEXT("C: finalize"), [C, Finalized]()
		{
			const DreamTest::EStep Step = Finalized();
			return Step == DreamTest::EStep::Next && C->Next.Instances->GetDeletedForTest().Num() == 0 ? DreamTest::EStep::Wait : Step;
		}, 20.0);
		Steps.Do(TEXT("C: nothing blocks it now: merged once, then deleted"), [this, C]()
		{
			TestFalse(TEXT("the namespace is deleted once everything is kept"), IFileManager::Get().DirectoryExists(*C->Sec));
			TestEqual(TEXT("the REPL entry joined once"), C->Next.Repl->Query(TEXT("print('staged by the secondary')"), EDreamHistoryMatch::Prefix, 0).Num(), 1);
			C->Next.History->Query(C->Closed, [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->Entries = MoveTemp(Entries); });
			return true;
		});
		Steps.Poll(TEXT("the history query"), [C]() { return C->Entries.IsSet() ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the closed tab's text is in history once"), [this, C]()
		{
			TestEqual(TEXT("the staged closed entry, merged by id (no duplicate from the earlier attempt)"), C->Entries->Num(), 1);
			return true;
		});
		Steps.Finally(TEXT("unlock, close and remove"), [C]()
		{
			C->Unlock();
			Close(C->Secondary, false);
			Close(C->First);
			Close(C->Next);
			if (!C->Blocker.IsEmpty())
			{
				IFileManager::Get().Delete(*C->Blocker);
			}
			RemoveTree(C->Base);
		});
		Steps.Run(Done);
	});

	LatentIt("an orphan whose newest wal or index is held is neither listed nor deleted, and merges once released (review r2)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Base;
			FNode First;
			FNode Secondary;
			FNode Next;
			FString Sec;
			FGuid ClosedOld;
			FGuid ClosedNew;
			TSharedPtr<FDreamBuffer> Newest;
			FDreamJournalTicket Ticket;
			int32 Staged = 0;
			IFileHandle* Held = nullptr;
			FString Key;
			TOptional<TArray<FDreamLocalHistoryEntry>> Entries;

			void Release()
			{
				delete Held;
				Held = nullptr;
			}

			/** The newest generation file matching Pattern in Dir (names sort by their 16 hex digits). */
			static FString NewestFile(const FString& Dir, const TCHAR* Pattern)
			{
				TArray<FString> Names;
				IFileManager::Get().FindFiles(Names, *(Dir / Pattern), true, false);
				Names.Sort();
				return Names.Num() > 0 ? Dir / Names.Last() : FString();
			}
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		C->Base = PrivateBase();
		C->First.Root = MakeRoot(C->Base, TEXT("DreamT4Inst-g00007"));
		C->Secondary.Root = C->First.Root;
		C->Next.Root = C->First.Root;
		C->Sec = C->First.Root.JournalDir(TEXT("i-4747-7000"));
		C->ClosedOld = FGuid::NewGuid();
		C->ClosedNew = FGuid::NewGuid();
		AddExpectedMessagePlain(TEXT("was not deleted"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		auto Stage = [C](const FGuid& Id, const TCHAR* Text)
		{
			C->Secondary.History->AddEntry(Id, EDreamHistoryKind::Closed, dream::TextBuffer(DreamText::ToU16(FString(Text))).Snapshot(), FString(),
				TEXT("Untitled-7"), TOptional<int32>(), [C](bool bOk) { C->Staged += bOk ? 1 : 0; });
		};
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a primary; a secondary session stages a closed text and exits cleanly"), [this, C]()
		{
			if (!Open(*this, C->First))
			{
				return false;
			}
			C->First.Journal->Reconcile({});
			return Open(*this, C->Secondary, TEXT("i-4747-7000"));
		});
		Steps.Poll(TEXT("scans"), [C]() { return C->First.bScanned && C->Secondary.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("stage in generation 1"), [C, Stage]()
		{
			C->Secondary.Journal->Reconcile({ MakeScratch(TEXT("InstHeldOld"), TEXT("older_text = 1\n")) });
			Stage(C->ClosedOld, TEXT("closed_in_session_one = 1\n"));
			return true;
		});
		Steps.Poll(TEXT("staged 1"), [C]() { return C->Staged == 1 ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 15.0);
		Steps.Do(TEXT("a second session of that namespace: its own generations above, then a crash"), [this, C]()
		{
			Close(C->Secondary);
			return Open(*this, C->Secondary, TEXT("i-4747-7000"));
		});
		Steps.Poll(TEXT("scan 2"), [C]() { return C->Secondary.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("journal and stage in generation 2"), [C, Stage]()
		{
			C->Newest = MakeScratch(TEXT("InstHeldNew"), TEXT("only_in_the_newest_generation = 2\n"));
			C->Secondary.Journal->Reconcile({ C->Newest.ToSharedRef() });
			C->Ticket = Snapshot(C->Secondary, C->Newest.ToSharedRef());
			Stage(C->ClosedNew, TEXT("closed_in_session_two = 2\n"));
			return true;
		});
		Steps.Poll(TEXT("durable and staged 2"), [C]()
		{
			return C->Staged == 2 && C->Secondary.Journal->IsDurable(C->Ticket) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 15.0);
		Steps.Do(TEXT("crash; the newest wal is held without sharing; the primary starts"), [this, C]()
		{
			Close(C->Secondary, /*bClean*/ false);
			Close(C->First);
			TArray<FString> Wal;
			IFileManager::Get().FindFiles(Wal, *(C->Sec / TEXT("wal.*.sjl")), true, false);
			TestTrue(TEXT("two generations of the secondary's journal"), Wal.Num() >= 2);
			const FString Newest = FCase::NewestFile(C->Sec, TEXT("wal.*.sjl"));
			C->Held = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Newest, /*bAppend*/ true, /*bAllowRead*/ false);
			TestNotNull(TEXT("the newest wal is held"), C->Held);
			return Open(*this, C->Next);
		});
		Steps.Poll(TEXT("its scan"), [C]() { return C->Next.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 30.0);
		Steps.Do(TEXT("not listed, not deleted"), [this, C]()
		{
			C->Next.Journal->Reconcile({});
			TestFalse(TEXT("an orphan read from an older generation is not listed (review r2)"),
				C->Next.Session.Orphans.ContainsByPredicate([](const FDreamOrphanNamespace& O) { return O.Namespace == TEXT("i-4747-7000"); }));
			C->Next.Instances->AddSameRootOrphans(MoveTemp(C->Next.Session.Orphans));
			C->Next.Session.Orphans.Reset();
			return true;
		});
		Steps.Poll(TEXT("idle"), [C]() { return WaitIdle(C->Next); }, 15.0);
		Steps.Do(TEXT("kept; now the newest history index is held instead"), [this, C]()
		{
			TestTrue(TEXT("the namespace is kept"), IFileManager::Get().DirectoryExists(*C->Sec));
			TestEqual(TEXT("nothing deleted"), C->Next.Instances->GetDeletedForTest().Num(), 0);
			Close(C->Next);
			C->Release();
			TArray<FString> Index;
			IFileManager::Get().FindFiles(Index, *(C->Sec / TEXT("pending-history") / TEXT("index.*.shi")), true, false);
			TestTrue(TEXT("two generations of its staged history index"), Index.Num() >= 2);
			C->Held = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*FCase::NewestFile(C->Sec / TEXT("pending-history"), TEXT("index.*.shi")),
				/*bAppend*/ true, /*bAllowRead*/ false);
			TestNotNull(TEXT("the newest index is held"), C->Held);
			return Open(*this, C->Next);
		});
		Steps.Poll(TEXT("scan with the index held"), [C]() { return C->Next.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 30.0);
		Steps.Do(TEXT("listed now (its journal reads); resolving it starts the merge"), [this, C]()
		{
			C->Next.Journal->Reconcile({});
			const FDreamOrphanNamespace* Orphan = C->Next.Session.Orphans.FindByPredicate([](const FDreamOrphanNamespace& O) { return O.Namespace == TEXT("i-4747-7000"); });
			if (!TestNotNull(TEXT("listed from its newest generation"), Orphan))
			{
				return false;
			}
			C->Key = Orphan->Key;
			const FGuid Id = C->Newest->GetId();
			const FDreamRecoveredBuffer* Rec = Orphan->Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; });
			TestTrue(TEXT("with the newest generation's text"), Rec != nullptr && Rec->RecoveredText == TEXT("only_in_the_newest_generation = 2\n"));
			C->Next.Instances->AddSameRootOrphans(MoveTemp(C->Next.Session.Orphans));
			C->Next.Session.Orphans.Reset();
			C->Next.Instances->ResolveOrphanBuffer(C->Key, Id, /*bDiscarded*/ false);
			return true;
		});
		Steps.Poll(TEXT("the merge"), [C]() { return WaitIdle(C->Next); }, 20.0);
		Steps.Do(TEXT("the merge failed on the held index: kept"), [this, C]()
		{
			TestTrue(TEXT("a staged index read from an older generation is not merged, and the namespace is kept (review r2)"),
				IFileManager::Get().DirectoryExists(*C->Sec));
			TestEqual(TEXT("nothing deleted"), C->Next.Instances->GetDeletedForTest().Num(), 0);
			Close(C->Next);
			C->Release();
			return Open(*this, C->Next);
		});
		Steps.Poll(TEXT("scan, released"), [C]() { return C->Next.bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 30.0);
		Steps.Do(TEXT("listed again; resolve"), [this, C]()
		{
			C->Next.Journal->Reconcile({});
			const FDreamOrphanNamespace* Orphan = C->Next.Session.Orphans.FindByPredicate([](const FDreamOrphanNamespace& O) { return O.Namespace == TEXT("i-4747-7000"); });
			if (!TestNotNull(TEXT("listed again at the next start"), Orphan))
			{
				return false;
			}
			C->Key = Orphan->Key;
			C->Next.Instances->AddSameRootOrphans(MoveTemp(C->Next.Session.Orphans));
			C->Next.Session.Orphans.Reset();
			C->Next.Instances->ResolveOrphanBuffer(C->Key, C->Newest->GetId(), false);
			return true;
		});
		Steps.Poll(TEXT("merged and deleted"), [C]()
		{
			C->Next.Instances->PumpForTest();
			return C->Next.Instances->GetDeletedForTest().Contains(C->Key) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Do(TEXT("both staged texts are in history"), [C]()
		{
			C->Next.History->Query(FGuid(), [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->Entries = MoveTemp(Entries); });
			return true;
		});
		Steps.Poll(TEXT("query"), [C]() { return C->Entries.IsSet() ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("check"), [this, C]()
		{
			const FGuid Old = C->ClosedOld;
			const FGuid New = C->ClosedNew;
			TestTrue(TEXT("the first session's closed text was merged"), C->Entries->ContainsByPredicate([&Old](const FDreamLocalHistoryEntry& E) { return E.Buffer == Old; }));
			TestTrue(TEXT("and the text only the newest index held"), C->Entries->ContainsByPredicate([&New](const FDreamLocalHistoryEntry& E) { return E.Buffer == New; }));
			TestFalse(TEXT("the namespace is gone once everything is kept"), IFileManager::Get().DirectoryExists(*C->Sec));
			return true;
		});
		Steps.Finally(TEXT("release, close and remove"), [C]()
		{
			C->Release();
			Close(C->Secondary, false);
			Close(C->First);
			Close(C->Next);
			RemoveTree(C->Base);
		});
		Steps.Run(Done);
	});
}
