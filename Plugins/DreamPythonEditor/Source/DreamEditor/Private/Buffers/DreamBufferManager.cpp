// SPDX-License-Identifier: Apache-2.0
// Open buffers, Untitled-N, pseudo filenames, load and save (TDD §4.13, §5.3.2, §10.14, BUF-01, BUF-03, RUN-06), external
// changes (§10.15, BUF-04), save failures (BUF-05 M1, BUF-06), Recent Files and the per-buffer reopen (§6.4).
#include "Buffers/DreamBufferManager.h"

#include "Buffers/DreamFileSaver.h"
#include "Buffers/DreamFileWatch.h"
#include "Buffers/DreamScratch.h"
#include "CodeView/DreamUndoHistory.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamLocalHistory.h"
#include "Python/DreamPython.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Settings/DreamUserSettings.h"
#include "Buffers/DreamHash.h"
#include "dream/OutputLines.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamBufferManagerLocal
{
	constexpr int32 MaxClosed = 32;

	FString NormalizePath(const FString& Path)
	{
		// (scratch-p2-m2) A UNC path keeps its leading "//" (the Project tree lists network roots, and their files must open).
		const bool bUnc = Path.StartsWith(TEXT("\\\\")) || Path.StartsWith(TEXT("//"));
		FString Full = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Full);
		FPaths::CollapseRelativeDirectories(Full);
		FPaths::RemoveDuplicateSlashes(Full);
		if (bUnc && !Full.StartsWith(TEXT("//")))
		{
			Full = TEXT("/") + Full;
		}
		return Full;
	}

	bool PathsEqual(const FString& A, const FString& B)
	{
#if PLATFORM_WINDOWS
		return A.Equals(B, ESearchCase::IgnoreCase);
#else
		return A.Equals(B, ESearchCase::CaseSensitive);
#endif
	}

	/** BUF-09: the generated stubs under <Project>/Intermediate/Dream/ always open read-only. */
	bool IsStubPath(const FString& NormalizedPath)
	{
		FString StubDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("Dream"));
		FPaths::NormalizeDirectoryName(StubDir);
		return FPaths::IsUnderDirectory(NormalizedPath, StubDir);
	}

	FText RefusalText(dream::codec::Refusal Refused)
	{
		switch (Refused)
		{
		case dream::codec::Refusal::Binary:
			return LOCTEXT("RefuseBinary", "This file looks binary (a NUL byte in its first 8 KB), so Dream Python Editor does not open it.");
		case dream::codec::Refusal::Utf32:
			return LOCTEXT("RefuseUtf32", "UTF-32 files are not supported.");
		case dream::codec::Refusal::TooLarge:
			return LOCTEXT("RefuseOverPlainMode", "Files over 1 MB are not opened in this version.");
		default:
			return FText::GetEmpty();
		}
	}

	struct FDecodedFile
	{
		bool bOk = false;
		FText Error;
		FString Text;
		dream::codec::FileFormat Format;
		bool bReadOnly = false;
		FText ReadOnlyReason;
		bool bReadOnlyOnDisk = false;
		FDreamBaseVersion Base;
	};

	FDecodedFile LoadAndDecode(const FString& Path)
	{
		FDecodedFile Out;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
		{
			Out.Error = FText::Format(LOCTEXT("CannotRead", "Cannot read {0}."), FText::FromString(Path));
			return Out;
		}
		const std::string_view View(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
		dream::codec::DecodeResult Decoded = dream::codec::Decode(View, static_cast<uint64>(FDreamBufferManager::PlainModeMaxBytes));
		if (Decoded.Refused != dream::codec::Refusal::None)
		{
			Out.Error = RefusalText(Decoded.Refused);
			return Out;
		}
		Out.bOk = true;
		Out.Text = DreamText::ToFString(Decoded.Text);
		Out.Format = MoveTemp(Decoded.Format);
		Out.bReadOnly = Decoded.bReadOnly;
		Out.ReadOnlyReason = FText::FromString(DreamText::Utf8ToFString(Decoded.ReadOnlyReason));
		Out.bReadOnlyOnDisk = IFileManager::Get().IsReadOnly(*Path);
		Out.Base.Size = Bytes.Num();
		Out.Base.MtimeUtc = IFileManager::Get().GetTimeStamp(*Path);
		Out.Base.Sha256Hex = DreamHash::Sha256Hex(View);
		return Out;
	}

	/** The buffer's own read-only state after a load or reload: the decoder's, else a stub's, else editable. A pending
	 *  recovery keeps its "Not restored yet" state (§10.7.1). */
	void ApplyReadOnly(FDreamBuffer& Buffer, const FString& Path, bool bDecodedReadOnly, const FText& DecodedReason)
	{
		if (Buffer.HasPendingRecovery())
		{
			return;
		}
		if (bDecodedReadOnly)
		{
			Buffer.SetReadOnly(true, DecodedReason);
		}
		else if (IsStubPath(Path))
		{
			Buffer.SetReadOnly(true, LOCTEXT("StubReadOnly", "Generated stub (read-only)"));
		}
		else
		{
			Buffer.SetReadOnly(false, FText::GetEmpty());
		}
	}

	void ApplyDecoded(FDreamBuffer& Buffer, const FString& Path, FDecodedFile&& Decoded)
	{
		Buffer.ResetText(Decoded.Text, dream::EditOrigin::Load);
		Buffer.SetFormat(Decoded.Format);
		Buffer.MarkSaved(Path, Decoded.Base);
		Buffer.SetReadOnlyOnDisk(Decoded.bReadOnlyOnDisk);
		ApplyReadOnly(Buffer, Path, Decoded.bReadOnly, Decoded.ReadOnlyReason);
	}

	bool SameBase(const TOptional<FDreamBaseVersion>& A, const TOptional<FDreamBaseVersion>& B)
	{
		if (A.IsSet() != B.IsSet())
		{
			return false;
		}
		return !A.IsSet() || (A->Size == B->Size && A->MtimeUtc == B->MtimeUtc && A->Sha256Hex == B->Sha256Hex);
	}

	/** The forced META commit of §10.15 (new base, dirty flag) once the journal is up. */
	void CommitMeta(const TSharedRef<FDreamBuffer>& Buffer)
	{
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetJournal().OnBufferMetaChanged(Buffer);
		}
	}

	/**
	 * The span of a PEP 263 cookie's encoding name in the model text, per tokenize.detect_encoding (§4.13 step 4):
	 * "^[ \t\f]*#.*?coding[:=][ \t]*([-\w.]+)" on line 1, or on line 2 when line 1 is blank or a comment.
	 */
	bool FindCookieName(const dream::TextSnapshot& Snapshot, int32& OutLine, int32& OutStart, int32& OutEnd)
	{
		const int32 Lines = static_cast<int32>(FMath::Min<dream::LineNo>(Snapshot.LineCount(), 2));
		for (int32 Line = 0; Line < Lines; ++Line)
		{
			std::u16string Text;
			const dream::Offset Begin = Snapshot.LineStart(Line);
			Snapshot.CopyRange(Begin, Snapshot.LineEnd(Line) - Begin, Text);
			int32 Pos = 0;
			const int32 Len = static_cast<int32>(Text.size());
			while (Pos < Len && (Text[Pos] == u' ' || Text[Pos] == u'\t' || Text[Pos] == u'\f'))
			{
				++Pos;
			}
			const bool bComment = Pos < Len && Text[Pos] == u'#';
			if (bComment)
			{
				for (int32 At = Pos + 1; At + 7 <= Len; ++At)
				{
					if (Text.compare(static_cast<size_t>(At), 6, u"coding") == 0 && (Text[At + 6] == u':' || Text[At + 6] == u'='))
					{
						int32 Start = At + 7;
						while (Start < Len && (Text[Start] == u' ' || Text[Start] == u'\t'))
						{
							++Start;
						}
						int32 End = Start;
						while (End < Len && (FChar::IsAlnum(static_cast<TCHAR>(Text[End])) || Text[End] == u'_' || Text[End] == u'-' || Text[End] == u'.'))
						{
							++End;
						}
						if (End > Start)
						{
							OutLine = Line;
							OutStart = Start;
							OutEnd = End;
							return true;
						}
						// No name here: the regex's lazy ".*?" goes on to a later "coding[:=]" on the same line.
					}
				}
			}
			if (!bComment && Pos < Len)
			{
				return false;   // line 1 is code: line 2 is never read
			}
		}
		return false;
	}

	/** The PEP 263 cookie name of the text as it is now (tokenize.detect_encoding over lines 1 and 2); empty when none. */
	std::string TextCookie(const dream::TextSnapshot& Snapshot)
	{
		std::u16string Head;
		const dream::LineNo Lines = FMath::Min<dream::LineNo>(Snapshot.LineCount(), 2);
		for (dream::LineNo Line = 0; Line < Lines; ++Line)
		{
			std::u16string Text;
			const dream::Offset Begin = Snapshot.LineStart(Line);
			Snapshot.CopyRange(Begin, Snapshot.LineEnd(Line) - Begin, Text);
			Head += Text;
			Head += u'\n';
		}
		return dream::codec::DetectCookie(DreamText::ToUtf8(DreamText::ToFString(Head)));
	}

	/**
	 * (B-note) The format a save writes, so that the bytes always read back as this text where Python and Dream read them
	 * (§4.13 step 4: a UTF-16 BOM, else the PEP 263 cookie on lines 1-2, else UTF-8):
	 * - a UTF-16 buffer keeps its encoding and BOM: load reads no cookie under a UTF-16 BOM;
	 * - otherwise a cookie in the text as it is now that names another supported encoding decides the encoding (also after
	 *   an undo of "Save as UTF-8"'s coding-line edit; a character that encoding cannot hold blocks the save with the UTF-8
	 *   offer again);
	 * - with no cookie, a Latin-1, cp1252 or ASCII buffer without a BOM (its coding line was deleted) is read back as
	 *   UTF-8: pure ASCII text is saved as UTF-8 (the same bytes), and other text blocks the save with "Save as UTF-8?"
	 *   (OutCookieGoneLine: the 1-based line of its first non-ASCII character; 0 = save).
	 */
	dream::codec::FileFormat FormatForSave(const dream::TextSnapshot& Snapshot, const dream::codec::FileFormat& BufferFormat,
		int32& OutCookieGoneLine)
	{
		using dream::codec::Encoding;
		OutCookieGoneLine = 0;
		dream::codec::FileFormat Format = BufferFormat;
		if (Format.Enc == Encoding::Utf16LE || Format.Enc == Encoding::Utf16BE)
		{
			return Format;   // a UTF-16 BOM wins over any coding line
		}
		const std::string Cookie = TextCookie(Snapshot);
		if (Cookie.empty())
		{
			Format.CookieName.clear();
			if (Format.bBom || (Format.Enc != Encoding::Latin1 && Format.Enc != Encoding::Cp1252 && Format.Enc != Encoding::Ascii))
			{
				return Format;   // UTF-8, with or without its BOM
			}
			const std::u16string Text = Snapshot.Text();
			size_t NonAscii = std::u16string::npos;
			for (size_t Index = 0; Index < Text.size(); ++Index)
			{
				if (Text[Index] >= 0x80)
				{
					NonAscii = Index;
					break;
				}
			}
			if (NonAscii == std::u16string::npos)
			{
				Format.Enc = Encoding::Utf8;   // the same bytes, now also what the reader assumes
			}
			else
			{
				OutCookieGoneLine = static_cast<int32>(Snapshot.LineOf(static_cast<dream::Offset>(NonAscii))) + 1;
			}
			return Format;
		}
		Encoding Enc = Encoding::Utf8;
		bool bSig = false;
		if (!dream::codec::LookupEncoding(Cookie, Enc, bSig) || Enc == Encoding::Utf16LE || Enc == Encoding::Utf16BE)
		{
			return Format;   // a cookie the load rule refuses (that buffer is read-only): the buffer's encoding
		}
		Format.CookieName = Cookie;
		if (Enc != Format.Enc)
		{
			Format.Enc = Enc;
			Format.bBom = Format.bBom && Enc == Encoding::Utf8;   // a UTF-8 BOM under another cookie is a tokenize error
		}
		return Format;
	}
}

struct FDreamBufferManager::FImpl
{
	TArray<TSharedRef<FDreamBuffer>> Open;
	TArray<TSharedRef<FDreamBuffer>> Closed;     // most recent last; kept with their text for Ctrl+Shift+T (§10.2.5)
	TSharedPtr<FDreamBuffer> ReplInput;
	TSharedPtr<FDreamFileWatch> Watch;           // §10.15
	TMap<FGuid, FDreamDiskChange> DiskChanges;
	TMap<FGuid, FDreamSaveFailureInfo> SaveFailures;
	int32 ReloadCount = 0;
	int32 DiskChangeCount = 0;

	/** (scratch, PO-05) Each open buffer's OnChanged binding, and a scratch file's auto-save state (game thread). */
	struct FTracked
	{
		TWeakPtr<FDreamBuffer> Buffer;
		FDelegateHandle ChangedHandle;
		FDelegateHandle StateHandle;
		double FirstUnsavedEdit = 0.0;      // the oldest edit not yet saved; 0 = nothing waits for the timer
		double LastEdit = 0.0;
		double FirstEditDuringSave = 0.0;   // an edit after the in-flight save's snapshot: due again when it lands
		bool bSaving = false;
		bool bAgain = false;                // a run start or focus loss came while a save was in flight: save once it lands
		EScratchSave AgainReason = EScratchSave::Other;
		bool bLastFailed = false;           // the last save failed: the next one always adds a history entry (R-p1-3)
		double LastHistorySeconds = 0.0;    // the last save that added a local-history entry
		FScratchSaveStatsForTest Stats;
	};
	TMap<FGuid, FTracked> Tracked;
	FTSTicker::FDelegateHandle ScratchTicker;
	bool bScratchBound = false;
	FDelegateHandle RunStateHandle;
	FDelegateHandle ActivationHandle;
	FDelegateHandle FolderHandle;
	bool bShutDown = false;
	TMap<FGuid, int32> SavesStartedForTest;  // scratch saves started per buffer (test hook), kept after the close
	bool bFocusSaves = true;                // test hook: the focus-loss saves (the timing specs switch them off)

	/** Every buffer a pseudo filename may still name: open, closed, the REPL input. */
	void ForEachKnown(TFunctionRef<void(const TSharedRef<FDreamBuffer>&)> Fn) const
	{
		for (const TSharedRef<FDreamBuffer>& B : Open)
		{
			Fn(B);
		}
		for (const TSharedRef<FDreamBuffer>& B : Closed)
		{
			Fn(B);
		}
		if (ReplInput.IsValid())
		{
			Fn(ReplInput.ToSharedRef());
		}
	}
};

FDreamBufferManager::FDreamBufferManager()
	: Impl(MakeUnique<FImpl>())
{
	Impl->Watch = MakeShared<FDreamFileWatch>(*this, [this](TArray<FDreamFileCheckResult>&& Results)
	{
		HandleFileChecks(MoveTemp(Results));
	});
	// (scratch) A static delegate: no folder is read or made here (B1).
	Impl->FolderHandle = DreamScratch::OnFolderChanged().AddRaw(this, &FDreamBufferManager::HandleScratchFolderChanged);
}

FDreamBufferManager::~FDreamBufferManager()
{
	if (Impl.IsValid() && Impl->Watch.IsValid())
	{
		Impl->Watch->Shutdown();
	}
	if (Impl.IsValid())
	{
		DreamScratch::OnFolderChanged().Remove(Impl->FolderHandle);
		for (TPair<FGuid, FImpl::FTracked>& Pair : Impl->Tracked)
		{
			if (const TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin())
			{
				Buffer->OnChanged().Remove(Pair.Value.ChangedHandle);
				Buffer->OnStateChanged().Remove(Pair.Value.StateHandle);
			}
		}
		Impl->Tracked.Reset();
	}
}

void FDreamBufferManager::Shutdown()
{
	Impl->Watch->Shutdown();
	Impl->DiskChanges.Reset();
	// (scratch) The auto-save's ticker and its run and focus bindings (the services still exist until step 8).
	Impl->bShutDown = true;
	FDreamDeferral::RemoveTicker(Impl->ScratchTicker);
	Impl->ScratchTicker.Reset();
	if (Impl->bScratchBound)
	{
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetRunService().OnRunStateChanged().Remove(Impl->RunStateHandle);
		}
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(Impl->ActivationHandle);
		}
		Impl->bScratchBound = false;
	}
}

void FDreamBufferManager::SyncWatch()
{
	Impl->Watch->Sync();
}

FDreamFileWatch& FDreamBufferManager::GetFileWatch() const
{
	return *Impl->Watch;
}

TSharedRef<FDreamBuffer> FDreamBufferManager::CreateScratch(const FString& Text, const FString& Name, const FGuid& Id)
{
	FString DisplayName = Name;
	if (DisplayName.IsEmpty())
	{
		// Untitled-N continues across sessions (§10.3 session state).
		UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
		const int32 Number = FMath::Max(1, Settings->NextUntitledNumber);
		Settings->NextUntitledNumber = Number + 1;
		DisplayName = FString::Printf(TEXT("Untitled-%d"), Number);
	}
	TSharedRef<FDreamBuffer> Buffer = MakeShared<FDreamBuffer>(Id.IsValid() ? Id : FGuid::NewGuid(), EDreamBufferKind::Scratch, DisplayName);
	if (!Text.IsEmpty())
	{
		Buffer->ResetText(Text, dream::EditOrigin::Load);
		Buffer->MarkClean();
	}
	Impl->Open.Add(Buffer);
	TrackBuffer(Buffer);   // (scratch) ConvertToScratchFile can make it a scratch file later
	OpenedEvent.Broadcast(Buffer);
	return Buffer;
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::OpenFile(const FString& Path, FText& OutError, const FGuid& Id)
{
	using namespace DreamBufferManagerLocal;
	const FString Normalized = NormalizePath(Path);
	if (TSharedPtr<FDreamBuffer> Existing = FindByPath(Normalized))
	{
		return Existing;
	}

	const int64 Size = IFileManager::Get().FileSize(*Normalized);
	if (Size < 0)
	{
		OutError = FText::Format(LOCTEXT("FileNotFound", "{0} does not exist."), FText::FromString(Normalized));
		return nullptr;
	}
	if (Size > PlainModeMaxBytes)
	{
		// (slice A choice, A40) Plain mode only below 1 MB (§4.15 row (3), BUF-09): the stock widget would flow and shape
		// every line of a larger file on the game thread. Deferred (G1): the progressive tier of §4.13.
		OutError = RefusalText(dream::codec::Refusal::TooLarge);
		return nullptr;
	}

	// §4.13 step 1: files up to 1 MB load on the game thread.
	FDecodedFile Decoded = LoadAndDecode(Normalized);
	if (!Decoded.bOk)
	{
		OutError = Decoded.Error;
		return nullptr;
	}
	TSharedRef<FDreamBuffer> Buffer = MakeShared<FDreamBuffer>(Id.IsValid() ? Id : FGuid::NewGuid(), EDreamBufferKind::File,
		FPaths::GetCleanFilename(Normalized));
	ApplyDecoded(*Buffer, Normalized, MoveTemp(Decoded));
	// The tab's One undo step per run starts at the user's buffer-run default; the toggle then decides (§5.9.1, U3).
	if (const UDreamUserSettings* Settings = UDreamUserSettings::Get())
	{
		Buffer->GetViewState().bOneUndoStepPerRun = Settings->GetTransactionDefault(EDreamRunRoute::Buffer);
	}

	Impl->Open.Add(Buffer);
	AddRecentFile(Normalized);          // BUF-03 M1 Recent Files (AddRecentFile skips a scratch file)
	TrackBuffer(Buffer);                // (scratch) a scratch file saves itself
	OpenedEvent.Broadcast(Buffer);
	SyncWatch();                        // §10.15: its directory is watched from now on
	return Buffer;
}

TSharedRef<FDreamBuffer> FDreamBufferManager::AddRestored(const FGuid& Id, EDreamBufferKind Kind, const FString& DisplayName,
	const FString& Path, const FString& Text, const dream::codec::FileFormat& Format, const TOptional<FDreamBaseVersion>& Base)
{
	if (Kind == EDreamBufferKind::Scratch && DisplayName.StartsWith(TEXT("Untitled-"), ESearchCase::CaseSensitive))
	{
		// A restored Untitled-N keeps N taken: after an unclean exit the ini may not hold the counter it reached (§10.3).
		const FString Digits = DisplayName.RightChop(9);
		const int32 Number = Digits.IsNumeric() ? FCString::Atoi(*Digits) : 0;
		UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
		if (Number > 0 && Number < MAX_int32 && Settings->NextUntitledNumber <= Number)
		{
			Settings->NextUntitledNumber = Number + 1;
		}
	}
	TSharedRef<FDreamBuffer> Buffer = MakeShared<FDreamBuffer>(Id.IsValid() ? Id : FGuid::NewGuid(), Kind, DisplayName);
	Buffer->ResetText(Text, dream::EditOrigin::Load);
	Buffer->SetFormat(Format);
	Buffer->MarkClean();
	if (!Path.IsEmpty())
	{
		Buffer->SetFileIdentity(DreamBufferManagerLocal::NormalizePath(Path), Base);
	}
	Impl->Open.Add(Buffer);
	TrackBuffer(Buffer);   // (scratch)
	OpenedEvent.Broadcast(Buffer);
	SyncWatch();
	return Buffer;
}

TSharedRef<FDreamBuffer> FDreamBufferManager::GetReplInputBuffer()
{
	if (!Impl->ReplInput.IsValid())
	{
		Impl->ReplInput = MakeShared<FDreamBuffer>(FGuid::NewGuid(), EDreamBufferKind::ReplInput, TEXT("REPL"));
	}
	return Impl->ReplInput.ToSharedRef();
}

bool FDreamBufferManager::Close(const FGuid& Id)
{
	const int32 Index = Impl->Open.IndexOfByPredicate([&Id](const TSharedRef<FDreamBuffer>& B) { return B->GetId() == Id; });
	if (Index == INDEX_NONE)
	{
		return false;
	}
	TSharedRef<FDreamBuffer> Buffer = Impl->Open[Index];
	if (Buffer->IsScratchFile() && Buffer->IsDirty())
	{
		// (scratch, PO-05) No prompt: the text goes to its file, which stays (nothing is ever deleted), or into local history
		// when it cannot be written (CloseScratch). The save completes after the close.
		CloseScratch(Buffer);
	}
	else if (Buffer->IsScratchFile() && Buffer->GetModel().Length() == 0)
	{
		DreamScratch::NoteClosedUnused(Buffer->GetPath());   // (scratch-p3-m1) an untouched scratch file of this editor: reusable
	}
	UntrackBuffer(Id);
	Impl->Open.RemoveAt(Index);
	Impl->DiskChanges.Remove(Id);
	Impl->SaveFailures.Remove(Id);

	// A non-empty scratch buffer stays reopenable; a file buffer reopens with its unsaved text (§10.2.5).
	const bool bKeep = (Buffer->GetKind() != EDreamBufferKind::Scratch || Buffer->GetModel().Length() > 0) && !Buffer->HasPendingRecovery();
	if (bKeep)
	{
		Impl->Closed.Add(Buffer);
		if (Impl->Closed.Num() > DreamBufferManagerLocal::MaxClosed)
		{
			Impl->Closed.RemoveAt(0);
		}
	}
	if (Buffer->GetKind() == EDreamBufferKind::File && Buffer->HasPath())
	{
		AddRecentFile(Buffer->GetPath());   // a closed file is the most recent one to come back to (BUF-03)
	}
	ClosedEvent.Broadcast(Buffer);
	SyncWatch();
	return bKeep;
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::ReopenClosed()
{
	while (Impl->Closed.Num() > 0)
	{
		TSharedRef<FDreamBuffer> Buffer = Impl->Closed.Pop();
		if (Buffer->HasPath() && FindByPath(Buffer->GetPath()).IsValid())
		{
			continue;   // reopened by path meanwhile
		}
		Impl->Open.Add(Buffer);
		TrackBuffer(Buffer);   // (scratch)
		OpenedEvent.Broadcast(Buffer);
		SyncWatch();
		if (Buffer->GetKind() == EDreamBufferKind::File && Buffer->HasPath())
		{
			Impl->Watch->Queue(Buffer->GetPath(), /*bForceHash*/ true);   // the file may have changed while the tab was closed
		}
		return Buffer;
	}
	return nullptr;
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::ReopenClosed(const FGuid& Id)
{
	const int32 Index = Impl->Closed.IndexOfByPredicate([&Id](const TSharedRef<FDreamBuffer>& B) { return B->GetId() == Id; });
	if (Index == INDEX_NONE)
	{
		return nullptr;
	}
	TSharedRef<FDreamBuffer> Buffer = Impl->Closed[Index];
	Impl->Closed.RemoveAt(Index);
	if (Buffer->HasPath())
	{
		if (TSharedPtr<FDreamBuffer> OpenTwin = FindByPath(Buffer->GetPath()))
		{
			return OpenTwin;   // the file was opened again meanwhile: that tab is the file's buffer now
		}
	}
	Impl->Open.Add(Buffer);
	TrackBuffer(Buffer);   // (scratch)
	OpenedEvent.Broadcast(Buffer);
	SyncWatch();
	if (Buffer->GetKind() == EDreamBufferKind::File && Buffer->HasPath())
	{
		Impl->Watch->Queue(Buffer->GetPath(), /*bForceHash*/ true);
	}
	return Buffer;
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::FindById(const FGuid& Id) const
{
	for (const TSharedRef<FDreamBuffer>& B : Impl->Open)
	{
		if (B->GetId() == Id)
		{
			return B;
		}
	}
	if (Impl->ReplInput.IsValid() && Impl->ReplInput->GetId() == Id)
	{
		return Impl->ReplInput;
	}
	return nullptr;
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::FindByIdHex(const FString& HexOrPrefix) const
{
	const FString Prefix = HexOrPrefix.ToLower();
	if (Prefix.Len() < 4)
	{
		return nullptr;
	}
	TSharedPtr<FDreamBuffer> Match;
	for (const TSharedRef<FDreamBuffer>& B : Impl->Open)
	{
		if (B->GetIdHex().StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			if (Match.IsValid())
			{
				return nullptr;   // ambiguous
			}
			Match = B;
		}
	}
	return Match;
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::FindClosedByIdHex(const FString& HexOrPrefix) const
{
	const FString Prefix = HexOrPrefix.ToLower();
	if (Prefix.Len() < 4)
	{
		return nullptr;
	}
	for (int32 Index = Impl->Closed.Num() - 1; Index >= 0; --Index)
	{
		if (Impl->Closed[Index]->GetIdHex().StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return Impl->Closed[Index];
		}
	}
	return nullptr;
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::FindClosedByFilename(const FString& PseudoFilename) const
{
	if (!PseudoFilename.StartsWith(TEXT("<dream:")))
	{
		return nullptr;
	}
	const std::string Utf8 = DreamText::ToUtf8(PseudoFilename);
	std::string_view Name;
	std::string_view Id;
	if (!dream::ParsePseudoFilename(Utf8, Name, Id) || Name == "REPL" || Name == "Console")
	{
		return nullptr;
	}
	return FindClosedByIdHex(DreamText::Utf8ToFString(Id));
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::FindByPath(const FString& Path) const
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	const FString Normalized = DreamBufferManagerLocal::NormalizePath(Path);
	for (const TSharedRef<FDreamBuffer>& B : Impl->Open)
	{
		if (B->HasPath() && DreamBufferManagerLocal::PathsEqual(B->GetPath(), Normalized))
		{
			return B;
		}
	}
	return nullptr;
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::FindByName(const FString& DisplayName) const
{
	for (const TSharedRef<FDreamBuffer>& B : Impl->Open)
	{
		if (B->GetDisplayName().Equals(DisplayName, ESearchCase::CaseSensitive))
		{
			return B;
		}
	}
	return nullptr;
}

TArray<TSharedRef<FDreamBuffer>> FDreamBufferManager::GetOpenBuffers() const
{
	return Impl->Open;
}

bool FDreamBufferManager::IsPathInUse(const FString& Path) const
{
	if (Path.IsEmpty())
	{
		return false;
	}
	const FString Normalized = DreamBufferManagerLocal::NormalizePath(Path);
	auto Holds = [&Normalized](const TSharedRef<FDreamBuffer>& B) { return B->HasPath() && DreamBufferManagerLocal::PathsEqual(B->GetPath(), Normalized); };
	return Impl->Open.ContainsByPredicate(Holds) || Impl->Closed.ContainsByPredicate(Holds);
}

FString FDreamBufferManager::GetCompileFilename(const FDreamBuffer& Buffer) const
{
	if (Buffer.HasPath())
	{
		return Buffer.GetPath();
	}

	// RUN-06: the first 4 hex digits of the FGuid, extended until unique among known buffers, so links survive renames.
	const FString Hex = Buffer.GetIdHex();
	int32 Length = 4;
	for (; Length < Hex.Len(); ++Length)
	{
		const FString Prefix = Hex.Left(Length);
		bool bClash = false;
		Impl->ForEachKnown([&](const TSharedRef<FDreamBuffer>& Other)
		{
			if (Other->GetId() != Buffer.GetId() && Other->GetIdHex().StartsWith(Prefix, ESearchCase::CaseSensitive))
			{
				bClash = true;
			}
		});
		if (!bClash)
		{
			break;
		}
	}

	FString Name = Buffer.GetDisplayName();
	for (TCHAR& Ch : Name)
	{
		if (Ch == TEXT('#') || Ch == TEXT('<') || Ch == TEXT('>') || Ch == TEXT('\n'))
		{
			Ch = TEXT('_');
		}
	}
	return FString::Printf(TEXT("<dream:%s#%s>"), *Name, *Hex.Left(Length));
}

TSharedPtr<FDreamBuffer> FDreamBufferManager::ResolveFilename(const FString& Filename) const
{
	if (Filename.StartsWith(TEXT("<dream:")))
	{
		const std::string Utf8 = DreamText::ToUtf8(Filename);
		std::string_view Name;
		std::string_view Id;
		if (!dream::ParsePseudoFilename(Utf8, Name, Id))
		{
			return nullptr;
		}
		if (Name == "REPL" || Name == "Console")
		{
			return nullptr;   // run-numbered inputs: the output view scrolls to the run's echo (§6.4)
		}
		return FindByIdHex(DreamText::Utf8ToFString(Id));
	}
	return FindByPath(Filename);
}

// ---------------------------------------------------------------------------------------------------------------------
// Saving (§10.14) and its failures (§10.14 step 5, BUF-05 M1, BUF-06)
// ---------------------------------------------------------------------------------------------------------------------

void FDreamBufferManager::SaveAsync(const FGuid& Id, FDreamOnSaved OnDone, const FDreamSaveOptions& Options)
{
	TSharedPtr<FDreamBuffer> Buffer = FindById(Id);
	if (!Buffer.IsValid())
	{
		OnDone.ExecuteIfBound(false, LOCTEXT("SaveNoBuffer", "Not saved: the tab is closed."));
		return;
	}
	if (!Buffer->HasPath())
	{
		OnDone.ExecuteIfBound(false, LOCTEXT("SaveNeedsPath", "Not saved: this tab has no file yet. Use Save As."));
		return;
	}
	SaveAsAsync(Id, Buffer->GetPath(), MoveTemp(OnDone), Options);
}

void FDreamBufferManager::SaveAsAsync(const FGuid& Id, const FString& NewPath, FDreamOnSaved OnDone, const FDreamSaveOptions& Options)
{
	TSharedPtr<FDreamBuffer> Buffer = FindById(Id);
	if (!Buffer.IsValid() || Buffer->GetKind() == EDreamBufferKind::ReplInput)
	{
		OnDone.ExecuteIfBound(false, LOCTEXT("SaveNoBuffer2", "Not saved: the tab is closed."));
		return;
	}
	if (Buffer->HasPendingRecovery())
	{
		// §10.7.1: nobody saves a text whose recovery is still open.
		OnDone.ExecuteIfBound(false, LOCTEXT("SavePending", "Not saved: recovered changes are not restored yet."));
		return;
	}
	if (Buffer->IsReadOnly() && Buffer->HasPath() && DreamBufferManagerLocal::PathsEqual(Buffer->GetPath(), NewPath))
	{
		OnDone.ExecuteIfBound(false, FText::Format(LOCTEXT("SaveReadOnly", "Not saved: {0}"), Buffer->GetReadOnlyReason()));
		return;
	}
	const FString Target = DreamBufferManagerLocal::NormalizePath(NewPath);
	if (TSharedPtr<FDreamBuffer> Other = FindByPath(Target); Other.IsValid() && Other != Buffer)
	{
		OnDone.ExecuteIfBound(false, LOCTEXT("SaveOpenElsewhere", "Not saved: that file is open in another tab."));
		return;
	}

	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	TWeakPtr<FDreamBuffer> WeakBuffer = Buffer;
	const FString OldPath = Buffer->GetPath();
	// Ruling R2 (§5.3.2 entry lifetime): runs of a dirty file buffer registered its text in linecache under its real path
	// (its compile filename); once that text is on disk at that path the runner drops the entry. A Save As to another path
	// leaves the old path's file unchanged, so its entry stays.
	const FString RunnerPath = Buffer->IsDirty() && Buffer->HasPath() && DreamBufferManagerLocal::PathsEqual(Buffer->GetPath(), Target)
		? Buffer->GetPath() : FString();
	int32 CookieGoneLine = 0;
	const dream::codec::FileFormat SaveFormat = DreamBufferManagerLocal::FormatForSave(Snapshot, Buffer->GetFormat(), CookieGoneLine);
	if (CookieGoneLine > 0)
	{
		// The coding line is gone from a Latin-1 / cp1252 buffer: its bytes would read back as UTF-8, differently. Blocked
		// like an unencodable character, with the same "Save as UTF-8" action (BUF-06, §4.13).
		const FText Error = FText::Format(LOCTEXT("Files.CookieGone",
			"Not saved: without a coding line Python and Dream Python Editor read this file as UTF-8, but it is {0}, and line {1} would read differently. Save as UTF-8?"),
			FText::FromString(DreamText::Utf8ToFString(dream::codec::EncodingName(SaveFormat.Enc))), FText::AsNumber(CookieGoneLine));
		UE_LOG(LogDream, Warning, TEXT("%s"), *Error.ToString());
		FDreamSaveFailureInfo& Failure = Impl->SaveFailures.FindOrAdd(Id);
		Failure.Kind = EDreamSaveFailure::Encoding;
		Failure.Error = Error;
		Failure.Path = Target;
		Failure.BadLine = CookieGoneLine;
		FileStateChangedEvent.Broadcast(Buffer.ToSharedRef());
		OnDone.ExecuteIfBound(false, Error);
		return;
	}
	FDreamFileSaver::Save(Target, Snapshot, SaveFormat,
		[this, WeakBuffer, Snapshot, SaveFormat, RunnerPath, Target, OldPath, Options, OnDone = MoveTemp(OnDone)](const FDreamSaveResult& Result)
		{
			if (Result.bOk)
			{
				// §10.14 step 6: the expected stamp only covers the event of this write. Once the save has landed the base is
				// its stamp, and a check that saw the old base re-queues (HandleFileChecks), so the stamp goes now: a later
				// external write of the same bytes (a git checkout back to this version) is an external change again.
				FDreamFileWatch::ForgetStamp(Target, Result.Base.Sha256Hex);
				if (!Result.FinalPath.IsEmpty())
				{
					FDreamFileWatch::ForgetStamp(Result.FinalPath, Result.Base.Sha256Hex);
				}
			}
			TSharedPtr<FDreamBuffer> Live = WeakBuffer.Pin();
			if (Live.IsValid() && !FindById(Live->GetId()).IsValid())
			{
				// (scratch, PO-05) A scratch tab's close save landed after the close: the closed buffer (kept for Reopen Closed
				// Tab) takes the saved state; the journal is not told (its BUF_CLOSE ended that buffer, and a save record would
				// track it again). A failed one keeps the text in local history, as a closed scratch tab's text is kept.
				// (scratch-p1 nit) The save keeps its history entry (PER-04) and the runner's linecache notice as an open one would.
				if (Result.bOk)
				{
					Live->MarkSaved(Target, Result.Base, Snapshot);
					if (FDreamEditorModule::IsAvailable())
					{
						FDreamEditorModule::Get().GetLocalHistory().AddEntry(Live->GetId(), EDreamHistoryKind::Save, Snapshot, Target,
							Live->GetDisplayName());
						if (!RunnerPath.IsEmpty())
						{
							FDreamEditorModule::Get().GetPython().NotifyBufferSaved(RunnerPath);
						}
					}
				}
				else
				{
					UE_LOG(LogDream, Warning, TEXT("%s (the tab was closed)"), *Result.Error.ToString());
					// A scratch file's closed text is FinishScratchSave's (it keeps the newest text); another file's goes here.
					if (!Impl->Tracked.Contains(Live->GetId()) && FDreamEditorModule::IsAvailable())
					{
						FDreamEditorModule::Get().GetLocalHistory().AddEntry(Live->GetId(), EDreamHistoryKind::Closed, Snapshot, Target,
							Live->GetDisplayName());
					}
				}
				OnDone.ExecuteIfBound(Result.bOk, Result.Error);
				return;
			}
			if (!Result.bOk || !Live.IsValid())
			{
				if (!Result.bOk)
				{
					UE_LOG(LogDream, Warning, TEXT("%s"), *Result.Error.ToString());
					if (Live.IsValid() && FindById(Live->GetId()).IsValid())
					{
						// §10.14 step 5: the buffer stays dirty and journaled; the tab's bar shows the error with its actions.
						FDreamSaveFailureInfo& Failure = Impl->SaveFailures.FindOrAdd(Live->GetId());
						Failure.Kind = Result.Failure == EDreamSaveFailure::None ? EDreamSaveFailure::Io : Result.Failure;
						Failure.Error = Result.Error;
						Failure.Path = Target;
						Failure.BadLine = Result.BadLine;
						FileStateChangedEvent.Broadcast(Live.ToSharedRef());
					}
				}
				OnDone.ExecuteIfBound(Result.bOk, Result.Error);
				return;
			}
			if (!DreamBufferManagerLocal::PathsEqual(Live->GetPath(), OldPath))
			{
				// (scratch-p1-m3) The tab moved to another file while this save ran (a rename in the Project tree): the old
				// path's result is not applied (it would take the tab back); the text is due again at the new path.
				UE_LOG(LogDream, Log, TEXT("%s moved to %s while a save ran; it is saved again there"), *OldPath, *Live->GetPath());
				OnDone.ExecuteIfBound(false, FText::Format(LOCTEXT("Files.SaveMoved", "Not saved under the new name: {0} was renamed while it was being saved. Save again."),
					FText::FromString(FPaths::GetCleanFilename(OldPath))));
				MarkScratchPending(Live->GetId());   // a scratch file saves itself at the new path; another file waits for the next Save
				return;
			}
			// The buffer keeps the path the user opened or chose (§10.14 step 2: only the write follows a link, subst drive
			// or mapped drive to the final path; the link stays a link), so tab name, FindByPath and compile filename hold.
			const bool bHadFailure = Impl->SaveFailures.Remove(Live->GetId()) > 0;
			const bool bHadDiskChange = Impl->DiskChanges.Remove(Live->GetId()) > 0;
			const bool bHadFileState = bHadFailure || bHadDiskChange || Live->IsDeletedOnDisk();
			{
				// The file now holds SaveFormat's encoding (the cookie's, FormatForSave); the line-ending state stays the buffer's.
				dream::codec::FileFormat Current = Live->GetFormat();
				if (Current.Enc != SaveFormat.Enc || Current.bBom != SaveFormat.bBom || Current.CookieName != SaveFormat.CookieName)
				{
					Current.Enc = SaveFormat.Enc;
					Current.bBom = SaveFormat.bBom;
					Current.CookieName = SaveFormat.CookieName;
					Live->SetFormat(Current);
				}
			}
			Live->MarkSaved(Target, Result.Base, Snapshot);
			Live->SetReadOnlyOnDisk(false);   // just written (Make writable cleared the attribute)
			if (Options.bHistoryEntry)
			{
				SavedEvent.Broadcast(Live.ToSharedRef());   // the journal forces a META commit (§10.14 step 6) and adds the history entry
			}
			else
			{
				DreamBufferManagerLocal::CommitMeta(Live.ToSharedRef());   // (scratch, R-p1-3) a throttled auto-save: the META alone
			}
			if (!DreamBufferManagerLocal::PathsEqual(OldPath, Target))
			{
				AddRecentFile(Target);
			}
			SyncWatch();
			if (bHadFileState)
			{
				FileStateChangedEvent.Broadcast(Live.ToSharedRef());
			}
			if (!RunnerPath.IsEmpty() && FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetPython().NotifyBufferSaved(RunnerPath);   // queues an idle tick; runs no Python here
			}
			OnDone.ExecuteIfBound(true, FText::GetEmpty());
		}, Options);
}

const FDreamSaveFailureInfo* FDreamBufferManager::GetSaveFailure(const FGuid& Id) const
{
	return Impl->SaveFailures.Find(Id);
}

void FDreamBufferManager::ClearSaveFailure(const FGuid& Id)
{
	if (Impl->SaveFailures.Remove(Id) > 0)
	{
		if (TSharedPtr<FDreamBuffer> Buffer = FindById(Id))
		{
			FileStateChangedEvent.Broadcast(Buffer.ToSharedRef());
		}
		MarkScratchPending(Id);   // (scratch) the bar is gone: the auto-save resumes
	}
}

bool FDreamBufferManager::ConvertToUtf8(const FGuid& Id, FText& OutError)
{
	TSharedPtr<FDreamBuffer> Buffer = FindById(Id);
	if (!Buffer.IsValid())
	{
		OutError = LOCTEXT("Files.Utf8Closed", "The tab is closed.");
		return false;
	}
	if (Buffer->IsReadOnly())
	{
		OutError = FText::Format(LOCTEXT("Files.Utf8ReadOnly", "Not converted: {0}"), Buffer->GetReadOnlyReason());
		return false;
	}
	dream::codec::FileFormat Format = Buffer->GetFormat();
	// The cookie in the text as it is now (not the one the file had at load): the save derives its encoding from it.
	const std::string Cookie = DreamBufferManagerLocal::TextCookie(Buffer->Snapshot());
	dream::codec::Encoding CookieEnc = dream::codec::Encoding::Utf8;
	bool bSig = false;
	const bool bForeignCookie = !Cookie.empty()
		&& !(dream::codec::LookupEncoding(Cookie, CookieEnc, bSig) && CookieEnc == dream::codec::Encoding::Utf8);
	Format.CookieName = Cookie;
	if (bForeignCookie)
	{
		int32 Line = 0;
		int32 Start = 0;
		int32 End = 0;
		if (!DreamBufferManagerLocal::FindCookieName(Buffer->Snapshot(), Line, Start, End))
		{
			OutError = LOCTEXT("Files.Utf8NoCookieSpan", "Not converted: the coding line could not be changed to utf-8.");
			return false;
		}
		{
			// The cookie and the bytes must agree, or Python and Dream decode the saved file with the old codec (§4.13 step 4).
			FDreamEditGroup Group;
			Group.Label = LOCTEXT("Files.Utf8Label", "Save as UTF-8");
			FDreamTextEdit& Edit = Group.Edits.AddDefaulted_GetRef();
			Edit.StartLine = Line;
			Edit.StartColumn = Start;
			Edit.EndLine = Line;
			Edit.EndColumn = End;
			Edit.Text = TEXT("utf-8");
			if (!FDreamUndoHistory::ApplyEditGroup(*Buffer, Group))
			{
				OutError = LOCTEXT("Files.Utf8CookieFailed", "Not converted: the coding line could not be changed now.");
				return false;
			}
		}
		Format.CookieName = "utf-8";
	}
	Format.Enc = dream::codec::Encoding::Utf8;
	Format.bBom = false;
	Buffer->SetFormat(Format);
	ClearSaveFailure(Id);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// External changes (§10.15 M1, BUF-04)
// ---------------------------------------------------------------------------------------------------------------------

void FDreamBufferManager::HandleFileChecks(TArray<FDreamFileCheckResult>&& Results)
{
	using namespace DreamBufferManagerLocal;
	for (FDreamFileCheckResult& Result : Results)
	{
		TSharedPtr<FDreamBuffer> Buffer = FindById(Result.Buffer);
		if (!Buffer.IsValid() || Buffer->GetKind() != EDreamBufferKind::File || !PathsEqual(Buffer->GetPath(), Result.Path))
		{
			continue;   // closed, renamed or turned into another kind since the check was queued
		}
		if (!SameBase(Buffer->GetBase(), Result.QueuedBase))
		{
			// A save or reload landed after the check was queued: its result describes the old base. Look again.
			Impl->Watch->Queue(Result.Path, /*bForceHash*/ true);
			continue;
		}
		const TSharedRef<FDreamBuffer> Ref = Buffer.ToSharedRef();
		const FGuid Id = Buffer->GetId();
		bool bFileState = false;
		if (Result.Outcome != EDreamFileCheck::Deleted && Result.Outcome != EDreamFileCheck::Unreadable)
		{
			// §10.15 Read-only change: only the tab's lock badge changes (BUF-02).
			Buffer->SetReadOnlyOnDisk(Result.bReadOnlyOnDisk);
		}
		switch (Result.Outcome)
		{
		case EDreamFileCheck::Unchanged:
		case EDreamFileCheck::MtimeOnly:
			if (Result.Outcome == EDreamFileCheck::MtimeOnly)
			{
				Buffer->SetFileIdentity(Buffer->GetPath(), Result.NewBase);   // an equal hash only updates the stored mtime
			}
			// The file holds the base again: a deleted file that reappears with the base hash clears its bar, and a pending
			// disk change is gone.
			if (Buffer->IsDeletedOnDisk())
			{
				Buffer->SetDeletedOnDisk(false);
				CommitMeta(Ref);
				bFileState = true;
			}
			bFileState = Impl->DiskChanges.Remove(Id) > 0 || bFileState;
			break;
		case EDreamFileCheck::SelfWrite:
			break;              // Dream's own save (§10.14 step 6): its completion sets the base
		case EDreamFileCheck::Unreadable:
			UE_LOG(LogDream, Verbose, TEXT("Dream Python Editor could not read %s to check it for external changes"), *Result.Path);
			break;
		case EDreamFileCheck::Deleted:
			if (!Buffer->IsDeletedOnDisk())
			{
				// §10.15 Deleted file: the buffer stays open, becomes dirty and stays journaled, with the Save / Close bar.
				Buffer->SetDeletedOnDisk(true);
				Impl->DiskChanges.Remove(Id);
				CommitMeta(Ref);
				bFileState = true;
				UE_LOG(LogDream, Log, TEXT("%s was deleted on disk; its tab keeps the text"), *Result.Path);
			}
			break;
		case EDreamFileCheck::Refused:
		{
			FDreamDiskChange Change;
			Change.NewBase = Result.NewBase;
			Change.bCanReload = false;
			Change.Error = Result.Error;
			Buffer->SetDeletedOnDisk(false);
			Impl->DiskChanges.Add(Id, MoveTemp(Change));
			++Impl->DiskChangeCount;
			bFileState = true;
			break;
		}
		case EDreamFileCheck::Changed:
			Buffer->SetDeletedOnDisk(false);
			if (!Buffer->IsTextDirty())
			{
				// §10.15 Clean buffer (M1): one undo group "Reloaded from disk", the old text in local history, META.
				if (ApplyDiskVersion(Ref, Result.Text, Result.Format, Result.NewBase, Result.bDecodedReadOnly, Result.DecodedReadOnlyReason))
				{
					bFileState = Impl->DiskChanges.Remove(Id) > 0 || bFileState;
					break;
				}
				UE_LOG(LogDream, Warning, TEXT("Dream Python Editor could not reload %s from disk; its tab offers Reload"), *Result.Path);
			}
			{
				// §10.15 Dirty buffer: the pre-M2 bar "[Keep mine] [Reload]".
				FDreamDiskChange Change;
				Change.NewBase = Result.NewBase;
				Change.Text = MoveTemp(Result.Text);
				Change.Format = MoveTemp(Result.Format);
				Change.bDecodedReadOnly = Result.bDecodedReadOnly;
				Change.DecodedReadOnlyReason = Result.DecodedReadOnlyReason;
				Impl->DiskChanges.Add(Id, MoveTemp(Change));
				++Impl->DiskChangeCount;
				bFileState = true;
			}
			break;
		}
		if (bFileState)
		{
			FileStateChangedEvent.Broadcast(Ref);
		}
	}
}

bool FDreamBufferManager::ApplyDiskVersion(const TSharedRef<FDreamBuffer>& Buffer, const FString& Text, const dream::codec::FileFormat& Format,
	const FDreamBaseVersion& NewBase, bool bDecodedReadOnly, const FText& DecodedReadOnlyReason)
{
	const dream::TextSnapshot OldText = Buffer->Snapshot();
	const bool bWasTextDirty = Buffer->IsTextDirty();
	// (B-note) A new disk version whose text equals the buffer's (only the BOM, the line endings or the encoding changed)
	// takes the new format and base without an undo step or a history entry.
	const FString ModelText = DreamText::ToModelText(Text);
	const bool bSameText = OldText.Length() == ModelText.Len() && OldText.Text() == DreamText::ToU16(ModelText);
	// One undo group through codeview's grouped-edit API (dream::Diff::ToMinimalEdits): unchanged lines, carets and scroll
	// stay; one Ctrl+Z restores the old text. A file that is read-only on disk or a "Not restored yet" buffer reloads too.
	if (!bSameText && !FDreamUndoHistory::ReplaceAllText(*Buffer, Text, LOCTEXT("Files.ReloadedLabel", "Reloaded from disk"),
		dream::EditOrigin::Reload, /*bEvenIfReadOnly*/ true))
	{
		return false;
	}
	// The old text goes to local history (§10.4 kind reload; the user's text for a Reload of a dirty buffer). The snapshot is
	// immutable, so it is taken before the replace and written after it succeeded.
	if (!bSameText && FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetLocalHistory().AddEntry(Buffer->GetId(), EDreamHistoryKind::Reload, OldText, Buffer->GetPath(),
			Buffer->GetDisplayName());
	}
	Buffer->SetFormat(Format);
	Buffer->MarkSaved(Buffer->GetPath(), NewBase, Buffer->Snapshot());   // clean at the new disk version (the undo save point)
	DreamBufferManagerLocal::ApplyReadOnly(*Buffer, Buffer->GetPath(), bDecodedReadOnly, DecodedReadOnlyReason);
	DreamBufferManagerLocal::CommitMeta(Buffer);
	if (bWasTextDirty && FDreamEditorModule::IsAvailable())
	{
		// Ruling R2: runs of the dirty text left it in linecache under the real path; the disk now holds the buffer's text.
		FDreamEditorModule::Get().GetPython().NotifyBufferSaved(Buffer->GetPath());
	}
	++Impl->ReloadCount;
	UE_LOG(LogDream, Log, TEXT("Reloaded %s from disk (one undo step)"), *Buffer->GetPath());
	ReloadedEvent.Broadcast(Buffer);
	return true;
}

const FDreamDiskChange* FDreamBufferManager::GetDiskChange(const FGuid& Id) const
{
	return Impl->DiskChanges.Find(Id);
}

void FDreamBufferManager::KeepMine(const FGuid& Id)
{
	const FDreamDiskChange* Change = Impl->DiskChanges.Find(Id);
	TSharedPtr<FDreamBuffer> Buffer = FindById(Id);
	if (Change == nullptr || !Buffer.IsValid())
	{
		return;
	}
	// The new disk stamp is the base: no bar for this version again, and the next Save overwrites it (§10.15).
	FDreamBaseVersion NewBase = Change->NewBase;
	Impl->DiskChanges.Remove(Id);
	Buffer->SetFileIdentity(Buffer->GetPath(), NewBase);
	DreamBufferManagerLocal::CommitMeta(Buffer.ToSharedRef());
	FileStateChangedEvent.Broadcast(Buffer.ToSharedRef());
	if (Buffer->IsScratchFile())
	{
		SaveScratchNow(Id, EScratchSave::Other);   // (scratch) Keep mine on a scratch file: its text goes to the file now, with its history entry
	}
}

bool FDreamBufferManager::ReloadFromDisk(const FGuid& Id)
{
	const FDreamDiskChange* Change = Impl->DiskChanges.Find(Id);
	TSharedPtr<FDreamBuffer> Buffer = FindById(Id);
	if (Change == nullptr || !Change->bCanReload || !Buffer.IsValid())
	{
		return false;
	}
	const FDreamDiskChange Copy = *Change;
	if (!ApplyDiskVersion(Buffer.ToSharedRef(), Copy.Text, Copy.Format, Copy.NewBase, Copy.bDecodedReadOnly, Copy.DecodedReadOnlyReason))
	{
		return false;
	}
	Impl->DiskChanges.Remove(Id);
	FileStateChangedEvent.Broadcast(Buffer.ToSharedRef());
	return true;
}

int32 FDreamBufferManager::GetReloadCountForTest() const
{
	return Impl->ReloadCount;
}

int32 FDreamBufferManager::GetDiskChangeCountForTest() const
{
	return Impl->DiskChangeCount;
}

// ---------------------------------------------------------------------------------------------------------------------
// Recent Files (BUF-03 M1)
// ---------------------------------------------------------------------------------------------------------------------

TArray<FString> FDreamBufferManager::GetRecentFiles() const
{
	return UDreamPerProjectUserSettings::Get()->RecentFiles;
}

void FDreamBufferManager::AddRecentFile(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return;
	}
	const FString Normalized = DreamBufferManagerLocal::NormalizePath(Path);
	if (DreamScratch::IsScratchPath(Normalized))
	{
		return;   // (scratch, PO-05) scratch files and samples are listed under the Project tree's Scratches, not here
	}
	TArray<FString>& Recent = UDreamPerProjectUserSettings::GetMutable()->RecentFiles;
	Recent.RemoveAll([&Normalized](const FString& Existing) { return DreamBufferManagerLocal::PathsEqual(Existing, Normalized); });
	Recent.Insert(Normalized, 0);
	if (Recent.Num() > MaxRecentFiles)
	{
		Recent.SetNum(MaxRecentFiles);
	}
	// (B-note) Saved with the session state (its 30 s debounce and OnPreExit), not here: a restore opens many files at once.
}

void FDreamBufferManager::RemoveRecentFile(const FString& Path)
{
	UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
	const FString Normalized = DreamBufferManagerLocal::NormalizePath(Path);
	if (Settings->RecentFiles.RemoveAll([&Normalized](const FString& Existing) { return DreamBufferManagerLocal::PathsEqual(Existing, Normalized); }) > 0)
	{
		Settings->SaveConfig();
	}
}

void FDreamBufferManager::ClearRecentFiles()
{
	UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
	Settings->RecentFiles.Reset();
	Settings->SaveConfig();
}

// ---------------------------------------------------------------------------------------------------------------------
// (scratch, PO-05) Scratch files: creation, the auto-save, the close and exit saves and the Untitled-N migration
// ---------------------------------------------------------------------------------------------------------------------

TSharedPtr<FDreamBuffer> FDreamBufferManager::CreateScratchFile(const FString& Text, FText& OutError, const FGuid& Id)
{
	const FString Path = DreamScratch::CreateNextFile(OutError);
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	DreamScratch::SeedSamplesOnce();   // first use of the folder: the samples, once per user (copied on a worker)
	TSharedPtr<FDreamBuffer> Buffer = OpenFile(Path, OutError, Id);
	if (!Buffer.IsValid())
	{
		return nullptr;
	}
	if (!Text.IsEmpty())
	{
		// The text arrives as a load (no undo step back to the empty file) and leaves the buffer dirty against the empty file
		// on disk, so the auto-save writes it through the ordinary save path (encoding, cookie, stamps).
		Buffer->ResetText(Text, dream::EditOrigin::Load);
	}
	UE_LOG(LogDream, Verbose, TEXT("New scratch tab %s"), *Path);
	return Buffer;
}

void FDreamBufferManager::TrackBuffer(const TSharedRef<FDreamBuffer>& Buffer)
{
	if (Buffer->GetKind() == EDreamBufferKind::ReplInput)
	{
		return;
	}
	const FGuid Id = Buffer->GetId();
	FImpl::FTracked* Existing = Impl->Tracked.Find(Id);
	FImpl::FTracked& Entry = Existing != nullptr ? *Existing : Impl->Tracked.Add(Id);
	Entry.Buffer = Buffer;
	if (!Entry.ChangedHandle.IsValid())
	{
		// Reopened while its close save was still in flight: the entry stayed, only its bindings went.
		Entry.ChangedHandle = Buffer->OnChanged().AddLambda([this, Id](const dream::ChangeSet&) { HandleScratchChanged(Id); });
		// A buffer that becomes a scratch file while dirty (a rename into the folder, a restore) is due at once.
		Entry.StateHandle = Buffer->OnStateChanged().AddLambda([this, Id]() { MarkScratchPending(Id); });
	}
	if (Buffer->IsScratchFile())
	{
		EnsureScratchBindings();
		if (Buffer->IsTextDirty())
		{
			MarkScratchPending(Id);   // reopened or restored with unsaved text (a clean exit that could not save it)
		}
	}
}

void FDreamBufferManager::UntrackBuffer(const FGuid& Id)
{
	FImpl::FTracked* Entry = Impl->Tracked.Find(Id);
	if (Entry == nullptr)
	{
		return;
	}
	if (const TSharedPtr<FDreamBuffer> Buffer = Entry->Buffer.Pin())
	{
		Buffer->OnChanged().Remove(Entry->ChangedHandle);
		Buffer->OnStateChanged().Remove(Entry->StateHandle);
	}
	Entry->ChangedHandle.Reset();
	Entry->StateHandle.Reset();
	Entry->FirstUnsavedEdit = 0.0;
	Entry->bAgain = false;
	if (!Entry->bSaving)
	{
		Impl->Tracked.Remove(Id);   // a save in flight keeps it: its result decides what the closed text still needs
	}
}

void FDreamBufferManager::HandleScratchChanged(const FGuid& Id)
{
	FImpl::FTracked* Entry = Impl->Tracked.Find(Id);
	const TSharedPtr<FDreamBuffer> Buffer = Entry != nullptr ? Entry->Buffer.Pin() : nullptr;
	if (!Buffer.IsValid() || !Buffer->IsScratchFile())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	Entry->LastEdit = Now;
	if (Entry->bSaving)
	{
		if (Entry->FirstEditDuringSave == 0.0)
		{
			Entry->FirstEditDuringSave = Now;
		}
		return;
	}
	if (!Buffer->IsTextDirty())
	{
		Entry->FirstUnsavedEdit = 0.0;   // undone back to the saved text
		return;
	}
	if (Entry->FirstUnsavedEdit == 0.0)
	{
		Entry->FirstUnsavedEdit = Now;
	}
	EnsureScratchBindings();
	ArmScratchTicker();
}

void FDreamBufferManager::MarkScratchPending(const FGuid& Id)
{
	FImpl::FTracked* Entry = Impl->Tracked.Find(Id);
	const TSharedPtr<FDreamBuffer> Buffer = Entry != nullptr ? Entry->Buffer.Pin() : nullptr;
	if (!Buffer.IsValid() || !Buffer->IsScratchFile() || !Buffer->IsDirty() || Entry->bSaving || Entry->FirstUnsavedEdit != 0.0
		|| !FindById(Id).IsValid())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	Entry->FirstUnsavedEdit = Now;
	Entry->LastEdit = FMath::Max(Entry->LastEdit, Now);
	EnsureScratchBindings();
	ArmScratchTicker();
}

void FDreamBufferManager::ArmScratchTicker()
{
	if (Impl->ScratchTicker.IsValid() || Impl->bShutDown)
	{
		return;
	}
	// (B-note) One ticker for every scratch buffer, armed by an edit and gone when nothing waits: polled ten times a second,
	// deferred while a run executes (§3.7; a run's own start saves first, RunStart).
	Impl->ScratchTicker = FDreamDeferral::AddTicker(TEXT("DreamScratchSave"), EDreamDeferPolicy::DeferWhileExecuting, 0.1f,
		/*bNeedsPython*/ false, [this](float) { return TickScratch(); });
}

bool FDreamBufferManager::IsScratchAutoSaveArmed(const FDreamBuffer& Buffer) const
{
	const FGuid& Id = Buffer.GetId();
	return Buffer.IsScratchFile() && !Buffer.IsDeletedOnDisk() && !Buffer.HasPendingRecovery() && !Buffer.IsReadOnly()
		&& !Impl->SaveFailures.Contains(Id) && !Impl->DiskChanges.Contains(Id);
}

int32 FDreamBufferManager::GetSavesStartedForTest(const FGuid& Id) const
{
	return Impl->SavesStartedForTest.FindRef(Id);
}

bool FDreamBufferManager::IsPathBusy(const FString& Path) const
{
	if (Path.IsEmpty())
	{
		return false;
	}
	const FString Normalized = DreamBufferManagerLocal::NormalizePath(Path);
	auto Holds = [&Normalized](const FDreamBuffer& B) { return B.HasPath() && DreamBufferManagerLocal::PathsEqual(B.GetPath(), Normalized); };
	for (const TSharedRef<FDreamBuffer>& B : Impl->Open)
	{
		if (Holds(*B))
		{
			return true;
		}
	}
	for (const TSharedRef<FDreamBuffer>& B : Impl->Closed)
	{
		if (Holds(*B) && B->IsDirty())
		{
			return true;   // its close save could not run or failed: its text belongs to that file
		}
	}
	for (const TPair<FGuid, FImpl::FTracked>& Pair : Impl->Tracked)
	{
		const TSharedPtr<FDreamBuffer> B = Pair.Value.Buffer.Pin();
		if (B.IsValid() && Holds(*B) && (Pair.Value.bSaving || Pair.Value.FirstUnsavedEdit != 0.0))
		{
			return true;   // a save in flight or due (a close save that has not landed)
		}
	}
	return false;
}

bool FDreamBufferManager::IsSaveInFlight(const FGuid& Id) const
{
	const FImpl::FTracked* Entry = Impl->Tracked.Find(Id);
	return Entry != nullptr && Entry->bSaving;
}

bool FDreamBufferManager::TickScratch()
{
	const double Now = FPlatformTime::Seconds();
	bool bWaiting = false;
	TArray<FGuid> Due;
	for (TPair<FGuid, FImpl::FTracked>& Pair : Impl->Tracked)
	{
		FImpl::FTracked& Entry = Pair.Value;
		if (Entry.bSaving)
		{
			bWaiting = true;   // its result decides whether it is due again
			continue;
		}
		if (Entry.FirstUnsavedEdit == 0.0)
		{
			continue;
		}
		const TSharedPtr<FDreamBuffer> Buffer = Entry.Buffer.Pin();
		if (!Buffer.IsValid() || !Buffer->IsDirty() || !IsScratchAutoSaveArmed(*Buffer))
		{
			// Saved meanwhile, or held by a bar the user answers (a failed save, BUF-04): nothing waits for the timer now.
			Entry.FirstUnsavedEdit = 0.0;
			continue;
		}
		if (Now - Entry.LastEdit >= ScratchQuietSeconds || Now - Entry.FirstUnsavedEdit >= ScratchMaxDelaySeconds)
		{
			Due.Add(Pair.Key);
		}
		else
		{
			bWaiting = true;
		}
	}
	for (const FGuid& Id : Due)
	{
		bWaiting |= SaveScratchNow(Id, EScratchSave::Timer);
	}
	if (!bWaiting)
	{
		Impl->ScratchTicker.Reset();
	}
	return bWaiting;
}

bool FDreamBufferManager::SaveScratchNow(const FGuid& Id, EScratchSave Reason)
{
	const TSharedPtr<FDreamBuffer> Buffer = FindById(Id);
	FImpl::FTracked* Entry = Impl->Tracked.Find(Id);
	if (!Buffer.IsValid() || Entry == nullptr || !Buffer->IsScratchFile() || !Buffer->HasPath())
	{
		return false;
	}
	if (!Buffer->IsDirty())
	{
		Entry->FirstUnsavedEdit = 0.0;
		return false;
	}
	if (Entry->bSaving)
	{
		// Never two writes of one file at once: the text typed since the in-flight save's snapshot goes when that save lands.
		Entry->bAgain = true;
		Entry->AgainReason = Reason;
		return false;
	}
	if (Buffer->IsDeletedOnDisk() || Impl->DiskChanges.Contains(Id) || Buffer->HasPendingRecovery() || Buffer->IsReadOnly())
	{
		return false;   // BUF-04 and the recovery flow decide (a deleted scratch file is not brought back behind the user's back)
	}
	if (Impl->SaveFailures.Contains(Id) && Reason != EScratchSave::Exit && Reason != EScratchSave::Other)
	{
		return false;   // the failure bar is up: Retry, its x or a successful save resume the auto-save
	}
	// (R-p1-3) Local history keeps one entry per buffer per HistoryThrottleSeconds from the timer, focus-loss and run-start
	// saves; every other save (a cleared failure, Keep mine, the first save after a failure) adds one.
	const double Now = FPlatformTime::Seconds();
	const bool bThrottled = Reason == EScratchSave::Timer || Reason == EScratchSave::FocusLost || Reason == EScratchSave::RunStart;
	FDreamSaveOptions Options;
	Options.bHistoryEntry = !bThrottled || Entry->bLastFailed || Entry->LastHistorySeconds == 0.0
		|| Now - Entry->LastHistorySeconds >= HistoryThrottleSeconds;
	if (Options.bHistoryEntry)
	{
		Entry->LastHistorySeconds = Now;
		Entry->Stats.HistoryEntries += 1;
	}
	Entry->bSaving = true;
	Entry->FirstEditDuringSave = 0.0;
	Entry->Stats.Saves += 1;
	Impl->SavesStartedForTest.FindOrAdd(Id) += 1;
	Entry->Stats.LastReason = Reason;
	Entry->Stats.LastSaveStartSeconds = Now;
	Entry->Stats.LastFirstEditSeconds = Entry->FirstUnsavedEdit;
	Entry->Stats.LastEditSeconds = Entry->LastEdit;
	Entry->FirstUnsavedEdit = 0.0;
	SaveAsAsync(Id, Buffer->GetPath(), FDreamOnSaved::CreateLambda([this, Id](bool bOk, const FText&)
	{
		FinishScratchSave(Id, bOk);
	}), Options);
	return true;
}

void FDreamBufferManager::FinishScratchSave(const FGuid& Id, bool bOk)
{
	FImpl::FTracked* Entry = Impl->Tracked.Find(Id);
	if (Entry == nullptr)
	{
		return;
	}
	Entry->bSaving = false;
	Entry->bLastFailed = !bOk;
	const TSharedPtr<FDreamBuffer> Buffer = Entry->Buffer.Pin();
	if (!Buffer.IsValid() || !FindById(Id).IsValid())
	{
		// (scratch-p1-M1) Closed while this save ran, and not reopened. Text typed after its snapshot is still only in the
		// closed buffer: it gets the close save now (never two writes of one file at once), or, when that cannot run or this
		// one failed, a "closed" local-history entry, so no text is lost at exit.
		if (Buffer.IsValid() && Buffer->IsDirty())
		{
			if (!bOk || !StartClosedSave(Buffer.ToSharedRef()))
			{
				KeepClosedTextInHistory(*Buffer);
			}
		}
		if (FImpl::FTracked* Still = Impl->Tracked.Find(Id); Still != nullptr && !Still->bSaving)
		{
			Impl->Tracked.Remove(Id);
		}
		return;
	}
	if (Entry->bAgain)
	{
		Entry->bAgain = false;
		if (SaveScratchNow(Id, Entry->AgainReason))
		{
			return;
		}
	}
	if (Entry->FirstEditDuringSave != 0.0)
	{
		// Typed while the save ran: its oldest edit is the first one after the save's snapshot.
		Entry->FirstUnsavedEdit = Entry->FirstEditDuringSave;
		Entry->FirstEditDuringSave = 0.0;
		ArmScratchTicker();
	}
	// A failure keeps the text in the buffer and the journal; the save-failure bar shows it (§10.14 step 5).
}

void FDreamBufferManager::CloseScratch(const TSharedRef<FDreamBuffer>& Buffer)
{
	// (scratch-p1-M1) No prompt, and the text is never lost: it goes to its file now, or after the save in flight (which
	// keeps the tracked entry), or into local history when it cannot be written (deleted on disk, a pending disk change, a
	// coding line gone, read-only). Nothing is deleted.
	FImpl::FTracked* Entry = Impl->Tracked.Find(Buffer->GetId());
	if (Entry != nullptr && Entry->bSaving)
	{
		return;   // FinishScratchSave saves the closed buffer once the in-flight save lands
	}
	if (!StartClosedSave(Buffer))
	{
		KeepClosedTextInHistory(*Buffer);
	}
}

bool FDreamBufferManager::StartClosedSave(const TSharedRef<FDreamBuffer>& Buffer)
{
	const FGuid Id = Buffer->GetId();
	if (!Buffer->IsScratchFile() || !Buffer->HasPath() || Buffer->IsDeletedOnDisk() || Impl->DiskChanges.Contains(Id)
		|| Buffer->HasPendingRecovery() || Buffer->IsReadOnly())
	{
		return false;
	}
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	int32 CookieGoneLine = 0;
	const dream::codec::FileFormat Format = DreamBufferManagerLocal::FormatForSave(Snapshot, Buffer->GetFormat(), CookieGoneLine);
	if (CookieGoneLine > 0)
	{
		return false;
	}
	FImpl::FTracked& Entry = Impl->Tracked.FindOrAdd(Id);
	Entry.Buffer = Buffer;
	Entry.bSaving = true;
	Entry.Stats.Saves += 1;
	Impl->SavesStartedForTest.FindOrAdd(Id) += 1;
	Entry.Stats.HistoryEntries += 1;
	Entry.Stats.LastReason = EScratchSave::Close;
	Entry.Stats.LastSaveStartSeconds = FPlatformTime::Seconds();
	const FString Path = Buffer->GetPath();
	const TWeakPtr<FDreamBuffer> WeakBuffer = Buffer;
	FDreamFileSaver::Save(Path, Snapshot, Format, [this, WeakBuffer, Id, Snapshot, Format, Path](const FDreamSaveResult& Result)
	{
		if (Result.bOk)
		{
			FDreamFileWatch::ForgetStamp(Path, Result.Base.Sha256Hex);
			if (!Result.FinalPath.IsEmpty())
			{
				FDreamFileWatch::ForgetStamp(Result.FinalPath, Result.Base.Sha256Hex);
			}
		}
		const TSharedPtr<FDreamBuffer> Live = WeakBuffer.Pin();
		const bool bOpen = Live.IsValid() && FindById(Id).IsValid();
		if (Live.IsValid() && Result.bOk && DreamBufferManagerLocal::PathsEqual(Live->GetPath(), Path))
		{
			dream::codec::FileFormat Current = Live->GetFormat();
			Current.Enc = Format.Enc;
			Current.bBom = Format.bBom;
			Current.CookieName = Format.CookieName;
			Live->SetFormat(Current);
			Live->MarkSaved(Path, Result.Base, Snapshot);
			if (bOpen)
			{
				SavedEvent.Broadcast(Live.ToSharedRef());   // reopened meanwhile: the journal tracks it again, with its entry
				SyncWatch();
			}
			else if (FDreamEditorModule::IsAvailable())
			{
				// R-p1-3: a close always leaves a history entry (the journal is not told about a closed buffer).
				FDreamEditorModule::Get().GetLocalHistory().AddEntry(Id, EDreamHistoryKind::Save, Snapshot, Path, Live->GetDisplayName());
			}
		}
		else if (Live.IsValid() && !Result.bOk)
		{
			UE_LOG(LogDream, Warning, TEXT("%s"), *Result.Error.ToString());
			if (bOpen)
			{
				FDreamSaveFailureInfo& Failure = Impl->SaveFailures.FindOrAdd(Id);
				Failure.Kind = Result.Failure == EDreamSaveFailure::None ? EDreamSaveFailure::Io : Result.Failure;
				Failure.Error = Result.Error;
				Failure.Path = Path;
				Failure.BadLine = Result.BadLine;
				FileStateChangedEvent.Broadcast(Live.ToSharedRef());
			}
		}
		FinishScratchSave(Id, Result.bOk);   // a closed buffer still dirty gets its history entry there
	});
	return true;
}

void FDreamBufferManager::KeepClosedTextInHistory(const FDreamBuffer& Buffer)
{
	UE_LOG(LogDream, Warning, TEXT("%s could not be saved when its tab closed; its text goes to local history"), *Buffer.GetPath());
	if (FDreamEditorModule::IsAvailable())
	{
		const FString Path = Buffer.GetPath();
		FDreamEditorModule::Get().GetLocalHistory().AddEntry(Buffer.GetId(), EDreamHistoryKind::Closed, Buffer.Snapshot(), Path,
			Buffer.GetDisplayName(), TOptional<int32>(), [Path](bool bKept)
			{
				// (scratch-p2 nit) Said only once the entry is durable, or that it could not be written.
				if (bKept)
				{
					UE_LOG(LogDream, Log, TEXT("The text of %s is kept in local history"), *Path);
				}
				else
				{
					UE_LOG(LogDream, Warning, TEXT("The text of %s could not be kept in local history either"), *Path);
				}
			});
	}
}

void FDreamBufferManager::SaveAllScratchNow(EScratchSave Reason)
{
	if (Reason == EScratchSave::FocusLost && !Impl->bFocusSaves)
	{
		return;
	}
	for (const TSharedRef<FDreamBuffer>& Buffer : TArray<TSharedRef<FDreamBuffer>>(Impl->Open))
	{
		if (Buffer->IsScratchFile() && Buffer->IsDirty())
		{
			SaveScratchNow(Buffer->GetId(), Reason);
		}
	}
}

void FDreamBufferManager::SetFocusSavesForTest(bool bEnabled)
{
	Impl->bFocusSaves = bEnabled;
}

bool FDreamBufferManager::AreFocusSavesEnabled() const
{
	return Impl->bFocusSaves;
}

void FDreamBufferManager::SaveScratchFilesAtExit()
{
	// The open scratch files, and the closed ones whose close save had not landed (their text exists only in memory).
	TArray<TSharedRef<FDreamBuffer>> Due;
	for (const TSharedRef<FDreamBuffer>& Buffer : Impl->Open)
	{
		if (Buffer->IsScratchFile() && Buffer->IsDirty())
		{
			Due.Add(Buffer);
		}
	}
	for (const TPair<FGuid, FImpl::FTracked>& Pair : Impl->Tracked)
	{
		const TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin();
		if (Buffer.IsValid() && !FindById(Pair.Key).IsValid() && Buffer->IsScratchFile() && Buffer->IsDirty())
		{
			Due.AddUnique(Buffer.ToSharedRef());
		}
	}
	if (Due.Num() == 0)
	{
		return;
	}
	// A worker write still running would land after a synchronous one and put older text back: it finishes first.
	if (!FDreamFileSaver::WaitForWorkerWrites(2.0))
	{
		UE_LOG(LogDream, Warning, TEXT("A file save was still running at exit; the scratch files are saved anyway"));
	}
	for (const TSharedRef<FDreamBuffer>& Buffer : Due)
	{
		const FGuid Id = Buffer->GetId();
		const bool bOpen = FindById(Id).IsValid();
		if (Buffer->IsDeletedOnDisk() || Impl->DiskChanges.Contains(Id) || Buffer->HasPendingRecovery() || Buffer->IsReadOnly())
		{
			if (!bOpen)
			{
				KeepClosedTextInHistory(*Buffer);   // an open one stays in the journal
			}
			continue;
		}
		const dream::TextSnapshot Snapshot = Buffer->Snapshot();
		int32 CookieGoneLine = 0;
		const dream::codec::FileFormat Format = DreamBufferManagerLocal::FormatForSave(Snapshot, Buffer->GetFormat(), CookieGoneLine);
		if (CookieGoneLine > 0)
		{
			UE_LOG(LogDream, Warning, TEXT("%s was not saved at exit: without its coding line it would read differently"), *Buffer->GetPath());
			if (!bOpen)
			{
				KeepClosedTextInHistory(*Buffer);
			}
			continue;
		}
		if (FImpl::FTracked* Entry = Impl->Tracked.Find(Id))
		{
			Entry->Stats.Saves += 1;
			Entry->Stats.HistoryEntries += 1;
			Entry->Stats.LastReason = EScratchSave::Exit;
			Entry->Stats.LastSaveStartSeconds = FPlatformTime::Seconds();
			Entry->FirstUnsavedEdit = 0.0;
		}
		// (B-note) The one synchronous save on the game thread: at OnPreExit no async result could land any more.
		const FDreamSaveResult Result = FDreamFileSaver::SaveNow(Buffer->GetPath(), Snapshot, Format);
		if (!Result.bOk)
		{
			UE_LOG(LogDream, Warning, TEXT("%s (%s)"), *Result.Error.ToString(), *Buffer->GetDisplayName());
			if (!bOpen)
			{
				KeepClosedTextInHistory(*Buffer);   // an open one stays in the journal
			}
			continue;
		}
		FDreamFileWatch::ForgetStamp(Buffer->GetPath(), Result.Base.Sha256Hex);
		Buffer->SetFormat(Format);
		Buffer->MarkSaved(Buffer->GetPath(), Result.Base, Snapshot);
		if (bOpen)
		{
			SavedEvent.Broadcast(Buffer);   // the journal's META and its history entry, before its CLEAN (HandlePreExit order)
		}
		else if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetLocalHistory().AddEntry(Id, EDreamHistoryKind::Save, Snapshot, Buffer->GetPath(), Buffer->GetDisplayName());
		}
		UE_LOG(LogDream, Log, TEXT("Saved the scratch file %s at exit"), *Buffer->GetPath());
	}
}

void FDreamBufferManager::HoldRestoredDiskVersion(const FGuid& Id, const FString& DiskText, const TOptional<FDreamBaseVersion>& JournalBase)
{
	// (scratch-p1-m2) A dirty scratch tab restored after a clean exit whose file changed since: the disk version waits behind
	// the [Keep mine] [Reload] bar (BUF-04), and the auto-save stays paused until the user answers it.
	const TSharedPtr<FDreamBuffer> Buffer = FindById(Id);
	if (!Buffer.IsValid() || !Buffer->GetBase().IsSet())
	{
		return;
	}
	const FDreamBaseVersion DiskBase = Buffer->GetBase().GetValue();
	FDreamDiskChange Change;
	Change.NewBase = DiskBase;
	Change.bCanReload = true;
	Change.Text = DiskText;
	Change.Format = Buffer->GetFormat();
	Impl->DiskChanges.Add(Id, MoveTemp(Change));
	++Impl->DiskChangeCount;
	if (FImpl::FTracked* Entry = Impl->Tracked.Find(Id))
	{
		Entry->FirstUnsavedEdit = 0.0;
	}
	// (scratch-p2-M1) The base is the one the journal's text was written against, not the disk's: the watch's checks then
	// report the disk as Changed (never Unchanged or MtimeOnly, which would drop the bar) and keep the DiskChange as BUF-04
	// builds it. Keep mine and Reload take the disk's. (scratch-p3 nit) Only a real journal base that differs from the disk's
	// is used, never a made-up empty one (the next start's compare would ignore it); every caller has one, since bConflict
	// means the disk differs from it.
	if (JournalBase.IsSet() && JournalBase->Sha256Hex != DiskBase.Sha256Hex)
	{
		Buffer->SetFileIdentity(Buffer->GetPath(), JournalBase);
	}
	Impl->Watch->Queue(Buffer->GetPath(), /*bForceHash*/ true);
	FileStateChangedEvent.Broadcast(Buffer.ToSharedRef());
	UE_LOG(LogDream, Log, TEXT("%s changed on disk since the last session; its tab offers Keep mine and Reload"), *Buffer->GetPath());
}

void FDreamBufferManager::ConvertToScratchFile(const FGuid& Id, FDreamOnSaved OnDone)
{
	const TSharedPtr<FDreamBuffer> Buffer = FindById(Id);
	if (!Buffer.IsValid() || Buffer->GetKind() != EDreamBufferKind::Scratch || Buffer->HasPath() || Buffer->HasPendingRecovery())
	{
		OnDone.ExecuteIfBound(false, LOCTEXT("Scratch.ConvertRefused", "Only an untitled tab becomes a scratch file."));
		return;
	}
	FText Error;
	const FString Path = DreamScratch::CreateNextFile(Error);
	if (Path.IsEmpty())
	{
		OnDone.ExecuteIfBound(false, Error);
		return;
	}
	DreamScratch::SeedSamplesOnce();
	const FString OldName = Buffer->GetDisplayName();
	SaveAsAsync(Id, Path, FDreamOnSaved::CreateLambda([this, Id, Path, OldName, OnDone](bool bOk, const FText& SaveError)
	{
		if (bOk)
		{
			UE_LOG(LogDream, Log, TEXT("%s is now the scratch file %s"), *OldName, *Path);
		}
		else
		{
			// The buffer stays the journaled Untitled-N it was: no failure bar for a migration, and the empty file goes again.
			UE_LOG(LogDream, Warning, TEXT("%s stays untitled: %s"), *OldName, *SaveError.ToString());
			ClearSaveFailure(Id);
			if (IFileManager::Get().FileSize(*Path) == 0 && !IsPathInUse(Path))
			{
				IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
			}
		}
		OnDone.ExecuteIfBound(bOk, SaveError);
	}));
}

void FDreamBufferManager::EnsureScratchBindings()
{
	if (Impl->bScratchBound || Impl->bShutDown || !FDreamEditorModule::IsAvailable())
	{
		return;
	}
	Impl->bScratchBound = true;
	// Before every run: at Running, after its journal snapshot and save steps, every dirty scratch file starts its save (the
	// write runs on a worker during the run; its result lands after it, §3.7).
	Impl->RunStateHandle = FDreamEditorModule::Get().GetRunService().OnRunStateChanged().AddLambda([this](const FDreamRunRecord& Record)
	{
		if (Record.State == EDreamRunState::Running)
		{
			SaveAllScratchNow(EScratchSave::RunStart);
		}
	});
	// Focus loss of the whole editor (another application in front); a code view's own focus loss comes from SDreamTab.
	if (FSlateApplication::IsInitialized())
	{
		Impl->ActivationHandle = FSlateApplication::Get().OnApplicationActivationStateChanged().AddLambda([this](bool bActive)
		{
			if (!bActive)
			{
				SaveAllScratchNow(EScratchSave::FocusLost);
			}
		});
	}
}

void FDreamBufferManager::HandleScratchFolderChanged()
{
	// The folder (its path, or its existence) changed: which buffers are scratch files follows it (a tab in the old folder is
	// a plain file now).
	for (const TSharedRef<FDreamBuffer>& Buffer : Impl->Open)
	{
		if (Buffer->RefreshScratchFlag() && Buffer->IsScratchFile() && Buffer->IsTextDirty())
		{
			EnsureScratchBindings();
			MarkScratchPending(Buffer->GetId());
		}
	}
	for (const TSharedRef<FDreamBuffer>& Buffer : Impl->Closed)
	{
		Buffer->RefreshScratchFlag();
	}
}

FDreamBufferManager::FScratchSaveStatsForTest FDreamBufferManager::GetScratchSaveStatsForTest(const FGuid& Id) const
{
	const FImpl::FTracked* Entry = Impl->Tracked.Find(Id);
	if (Entry == nullptr)
	{
		return FScratchSaveStatsForTest();
	}
	FScratchSaveStatsForTest Stats = Entry->Stats;
	Stats.bSaving = Entry->bSaving;
	Stats.bPending = Entry->FirstUnsavedEdit != 0.0;
	return Stats;
}

#undef LOCTEXT_NAMESPACE
