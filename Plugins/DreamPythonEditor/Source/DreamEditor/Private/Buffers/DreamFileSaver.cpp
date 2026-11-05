// SPDX-License-Identifier: Apache-2.0
// Saving files (TDD §10.14): a failure at any step leaves the original bytes on disk, and the buffer stays dirty.
#include "Buffers/DreamFileSaver.h"

#include "Buffers/DreamFileWatch.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Math/RandomStream.h"
#include "Misc/Paths.h"
#include "Module/DreamText.h"
#include "Persistence/DreamWin32FileOps.h"
#include "Run/DreamDeferral.h"
#include "Tasks/Task.h"
#include "Buffers/DreamHash.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamFileSaverLocal
{
	FText NotSaved(const FString& Reason)
	{
		return FText::Format(LOCTEXT("NotSaved", "Not saved: {0}"), FText::FromString(Reason));
	}

	FString SiblingName(const FString& Target, const TCHAR* Extension)
	{
		// ".<name>.dream-<random>.tmp" in the target's directory: a rename is atomic only within one volume (§10.14 step 3).
		const uint32 Random = static_cast<uint32>(FPlatformTime::Cycles64() ^ (FPlatformTLS::GetCurrentThreadId() * 2654435761u));
		return FPaths::GetPath(Target) / FString::Printf(TEXT(".%s.dream-%08x%s"), *FPaths::GetCleanFilename(Target), Random, Extension);
	}

	struct FPendingSave
	{
		std::atomic<bool> bDone { false };
		FDreamSaveResult Result;
	};

	/** (scratch) Worker writes started by Save and not finished yet (WaitForWorkerWrites). */
	std::atomic<int32> WorkerWrites { 0 };
}

FDreamSaveResult FDreamFileSaver::SaveNow(const FString& Path, const dream::TextSnapshot& Snapshot, const dream::codec::FileFormat& Format,
	const FDreamSaveOptions& Options)
{
	using namespace DreamFileSaverLocal;
	FDreamSaveResult Result;

	// 1. Encode (§4.13 Save): BOM, the dominant terminator and its exceptions. Unrepresentable characters block the save with
	//    "Save as UTF-8?" (BUF-06): the tab's bar offers the conversion.
	std::string Bytes;
	dream::Offset BadOffset = 0;
	if (!dream::codec::Encode(Snapshot.Text(), Format, Bytes, &BadOffset))
	{
		const int32 Line = static_cast<int32>(Snapshot.LineOf(BadOffset)) + 1;
		Result.Failure = EDreamSaveFailure::Encoding;
		Result.BadLine = Line;
		Result.Error = FText::Format(LOCTEXT("EncodeBlocked", "Not saved: the encoding {0} cannot hold a character on line {1}. Save as UTF-8?"),
			FText::FromString(DreamText::Utf8ToFString(dream::codec::EncodingName(Format.Enc))), FText::AsNumber(Line));
		return Result;
	}

	// 2. Resolve the target: a symlink, junction or subst path writes the real file; the link stays a link.
	FString Target = Path;
	FDreamWin32FileOps::GetFinalPath(Path, Target);
	const bool bExists = IFileManager::Get().FileExists(*Target);
	if (bExists && IFileManager::Get().IsReadOnly(*Target))
	{
		// BUF-05 M1 (§11.6.5): Make writable clears the attribute here, on the worker; otherwise the tab's bar offers Make
		// writable and Save As. Deferred (M4): the async source-control checkout.
		if (!Options.bMakeWritable || !FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Target, false))
		{
			Result.Failure = EDreamSaveFailure::ReadOnlyTarget;
			Result.Error = Options.bMakeWritable
				? NotSaved(FString::Printf(TEXT("the file could not be made writable (%s)."), *FDreamWin32FileOps::GetLastErrorText()))
				: NotSaved(TEXT("the file is read-only. Use Save As, or make it writable."));
			return Result;
		}
	}

	// 6 (first half). The expected stamp, registered before the replace so the watcher's event for this write is recognised
	// as Dream's own save whichever comes first (§10.14 step 6, §10.15). Size and SHA-256: the mtime is not known before the
	// replace, and UE's file times have one-second precision anyway (B-note).
	const FString Sha256 = DreamHash::Sha256Hex(Bytes);
	const int64 Size = static_cast<int64>(Bytes.size());
	FDreamFileWatch::ExpectStamp(Path, Size, Sha256);
	if (!Target.Equals(Path, ESearchCase::IgnoreCase))
	{
		FDreamFileWatch::ExpectStamp(Target, Size, Sha256);
	}
	auto Fail = [&Result, &Path, &Target, &Sha256](const FString& Error)
	{
		FDreamFileWatch::ForgetStamp(Path, Sha256);
		FDreamFileWatch::ForgetStamp(Target, Sha256);
		Result.Failure = EDreamSaveFailure::Io;
		Result.Error = NotSaved(Error);
	};

	FDreamWin32FileOps Ops;
	if (bExists && FDreamWin32FileOps::GetLinkCount(Target) > 1)
	{
		// Hard links: written in place so every link sees the new bytes (§10.14 step 4).
		FString Error;
		if (!FDreamWin32FileOps::WriteInPlace(Target, Bytes, SiblingName(Target, TEXT(".bak")), Error))
		{
			Fail(Error);
			return Result;
		}
	}
	else
	{
		// 3. Temp file in the target's directory, flushed. 4. Replace (or move for a new file), write-through.
		const FString Temp = SiblingName(Target, TEXT(".tmp"));
		const std::string TempUtf8 = FDreamWin32FileOps::ToUtf8Path(Temp);
		dream::journal::FileOps::Handle H = dream::journal::FileOps::InvalidHandle;
		const bool bWritten = Ops.CreateExclusive(TempUtf8, H) && Ops.Append(H, Bytes) && Ops.Flush(H);
		Ops.Close(H);
		if (!bWritten || !Ops.ReplaceAtomic(FDreamWin32FileOps::ToUtf8Path(Target), TempUtf8, std::string_view()))
		{
			const FString Error = FDreamWin32FileOps::ToFString(Ops.LastError());
			Ops.Remove(TempUtf8);   // 5. the target is untouched
			Fail(Error);
			return Result;
		}
	}

	// 6. The new base version: size, mtime and SHA-256 of the written bytes.
	Result.bOk = true;
	Result.FinalPath = Target;
	Result.Base.Size = Size;
	Result.Base.MtimeUtc = IFileManager::Get().GetTimeStamp(*Target);
	Result.Base.Sha256Hex = Sha256;
	return Result;
}

void FDreamFileSaver::Save(const FString& Path, const dream::TextSnapshot& Snapshot, const dream::codec::FileFormat& Format,
	TFunction<void(const FDreamSaveResult&)> OnGameThread, const FDreamSaveOptions& Options)
{
	using namespace DreamFileSaverLocal;
	TSharedRef<FPendingSave, ESPMode::ThreadSafe> Pending = MakeShared<FPendingSave, ESPMode::ThreadSafe>();

	WorkerWrites.fetch_add(1, std::memory_order_acq_rel);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [Pending, Path, Snapshot, Format, Options]()
	{
		Pending->Result = SaveNow(Path, Snapshot, Format, Options);
		Pending->bDone.store(true, std::memory_order_release);
		WorkerWrites.fetch_sub(1, std::memory_order_acq_rel);
	});

	// The result is an async result that mutates a buffer: it goes through the §3.7 gate.
	FDreamDeferral::AddTicker(TEXT("DreamFileSave"), EDreamDeferPolicy::DeferWhileExecuting, 0.f, false,
		[Pending, Callback = MoveTemp(OnGameThread)](float) -> bool
		{
			if (!Pending->bDone.load(std::memory_order_acquire))
			{
				return true;
			}
			Callback(Pending->Result);
			return false;
		});
}

bool FDreamFileSaver::WaitForWorkerWrites(double TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (DreamFileSaverLocal::WorkerWrites.load(std::memory_order_acquire) > 0)
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			return false;
		}
		FPlatformProcess::Sleep(0.005f);
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
