// SPDX-License-Identifier: Apache-2.0
// The Win64 FileOps (TDD §10.2.1, §10.2.3, §10.14). UE's FWindowsPlatformFile::MoveFile calls MoveFileW without flags, so
// renames here use MoveFileExW(MOVEFILE_WRITE_THROUGH) and replaces ReplaceFileW (§10.2.3 step 3, §10.14 step 4).
#include "Persistence/DreamWin32FileOps.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Module/DreamText.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <sddl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace DreamFileOpsLocal
{
	/** §10.14: sharing violations (antivirus, an indexer) are retried at 10, 50, 100, 250 and 500 ms. */
	constexpr float RetryDelays[] = { 0.010f, 0.050f, 0.100f, 0.250f, 0.500f };

#if PLATFORM_WINDOWS
	FString ToWindowsPath(std::string_view Utf8)
	{
		FString Path = FDreamWin32FileOps::ToFString(Utf8);
		Path.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
		return Path;
	}

	bool IsRetryable(DWORD Code)
	{
		return Code == ERROR_SHARING_VIOLATION || Code == ERROR_LOCK_VIOLATION || Code == ERROR_ACCESS_DENIED
			|| Code == ERROR_UNABLE_TO_REMOVE_REPLACED || Code == ERROR_UNABLE_TO_MOVE_REPLACEMENT;
	}

	/** The retry wait of this thread (FScopedRetryWait); null: sleep. */
	thread_local const TFunction<void(float)>* GRetryWait = nullptr;

	void WaitBeforeRetry(float Delay)
	{
		const TFunction<void(float)>* Wait = GRetryWait;
		if (Wait == nullptr || !*Wait)
		{
			FPlatformProcess::Sleep(Delay);
			return;
		}
		GRetryWait = nullptr;   // FileOps calls made inside the wait (a commit) sleep through their own retries
		(*Wait)(Delay);
		GRetryWait = Wait;
	}

	/** Fn, then the retries. IsFenced is asked before every retry: once the journal writer is abandoned it performs no further
	 *  file operation, even inside a retry loop that was already sleeping (§10.2.5, T-PER-041). */
	template <typename FenceType, typename FnType>
	bool WithRetries(FenceType&& IsFenced, FnType&& Fn)
	{
		if (Fn())
		{
			return true;
		}
		for (const float Delay : RetryDelays)
		{
			if (!IsRetryable(::GetLastError()))
			{
				return false;
			}
			WaitBeforeRetry(Delay);
			if (IsFenced())
			{
				return false;
			}
			if (Fn())
			{
				return true;
			}
		}
		return false;
	}

	bool NeverFenced()
	{
		return false;
	}

	HANDLE ToHandle(dream::journal::FileOps::Handle H)
	{
		return reinterpret_cast<HANDLE>(static_cast<UPTRINT>(H));
	}

	dream::journal::FileOps::Handle FromHandle(HANDLE H)
	{
		return static_cast<dream::journal::FileOps::Handle>(reinterpret_cast<UPTRINT>(H));
	}
#endif
}

FDreamWin32FileOps::FDreamWin32FileOps(TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> InFence)
	: Fence(MoveTemp(InFence))
{
}

FString FDreamWin32FileOps::ToFString(std::string_view Utf8Path)
{
	return DreamText::Utf8ToFString(Utf8Path);
}

std::string FDreamWin32FileOps::ToUtf8Path(const FString& Path)
{
	FString Forward = Path;
	Forward.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	return DreamText::ToUtf8(Forward);
}

void FDreamWin32FileOps::WaitLikeARetry(float Seconds)
{
#if PLATFORM_WINDOWS
	DreamFileOpsLocal::WaitBeforeRetry(Seconds);
#else
	FPlatformProcess::Sleep(Seconds);
#endif
}

FDreamWin32FileOps::FScopedRetryWait::FScopedRetryWait(TFunction<void(float)> InWait)
	: Wait(MoveTemp(InWait))
{
#if PLATFORM_WINDOWS
	Previous = DreamFileOpsLocal::GRetryWait;
	DreamFileOpsLocal::GRetryWait = &Wait;
#endif
}

FDreamWin32FileOps::FScopedRetryWait::~FScopedRetryWait()
{
#if PLATFORM_WINDOWS
	DreamFileOpsLocal::GRetryWait = Previous;
#endif
}

bool FDreamWin32FileOps::IsFenced()
{
	if (Fence.IsValid() && Fence->load(std::memory_order_acquire))
	{
		Error = "the journal writer was abandoned";
		return true;
	}
	return false;
}

void FDreamWin32FileOps::SetError(const TCHAR* Operation, std::string_view Path)
{
	const FString Text = FString::Printf(TEXT("%s %s: %s"), Operation, *ToFString(Path), *GetLastErrorText());
	Error = DreamText::ToUtf8(Text);
}

std::string FDreamWin32FileOps::LastError() const
{
	return Error;
}

FString FDreamWin32FileOps::GetLastErrorText()
{
#if PLATFORM_WINDOWS
	const uint32 Code = FPlatformMisc::GetLastError();
	TCHAR Buffer[512] = {};
	FPlatformMisc::GetSystemErrorMessage(Buffer, UE_ARRAY_COUNT(Buffer), static_cast<int32>(Code));
	FString Text(Buffer);
	Text.TrimEndInline();
	return Text.IsEmpty() ? FString::Printf(TEXT("error %u"), Code) : Text;
#else
	return TEXT("not supported on this platform");
#endif
}

#if PLATFORM_WINDOWS

bool FDreamWin32FileOps::CreateExclusive(std::string_view Path, Handle& Out)
{
	Out = InvalidHandle;
	if (IsFenced())
	{
		return false;
	}
	const FString WinPath = DreamFileOpsLocal::ToWindowsPath(Path);
	HANDLE H = ::CreateFileW(*WinPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (H == INVALID_HANDLE_VALUE)
	{
		SetError(TEXT("Create"), Path);
		return false;
	}
	Out = DreamFileOpsLocal::FromHandle(H);
	return true;
}

bool FDreamWin32FileOps::OpenAppend(std::string_view Path, Handle& Out)
{
	Out = InvalidHandle;
	if (IsFenced())
	{
		return false;
	}
	const FString WinPath = DreamFileOpsLocal::ToWindowsPath(Path);
	// GENERIC_WRITE, not FILE_APPEND_DATA alone, so Truncate can cut a failed commit back off (SetEndOfFile needs
	// FILE_WRITE_DATA). Writes then go to the file pointer: it starts at the end and only Truncate moves it. A scanner that
	// holds a just-renamed generation is retried like a rename (§10.11, review r1 persist-M4).
	HANDLE H = INVALID_HANDLE_VALUE;
	DreamFileOpsLocal::WithRetries([this]() { return IsFenced(); }, [&]()
	{
		H = ::CreateFileW(*WinPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		return H != INVALID_HANDLE_VALUE;
	});
	if (H == INVALID_HANDLE_VALUE)
	{
		SetError(TEXT("Open"), Path);
		return false;
	}
	LARGE_INTEGER Zero;
	Zero.QuadPart = 0;
	if (!::SetFilePointerEx(H, Zero, nullptr, FILE_END))
	{
		SetError(TEXT("Seek"), Path);
		::CloseHandle(H);
		return false;
	}
	Out = DreamFileOpsLocal::FromHandle(H);
	return true;
}

bool FDreamWin32FileOps::Append(Handle H, std::string_view Bytes)
{
	const char* Data = Bytes.data();
	size_t Left = Bytes.size();
	do
	{
		if (IsFenced())
		{
			return false;   // checked before every chunk write (§10.2.5)
		}
		if (Left == 0)
		{
			break;
		}
		const DWORD Chunk = static_cast<DWORD>(FMath::Min<size_t>(Left, 1u << 24));
		DWORD Written = 0;
		if (!::WriteFile(DreamFileOpsLocal::ToHandle(H), Data, Chunk, &Written, nullptr) || Written == 0)
		{
			SetError(TEXT("Write"), std::string_view());
			return false;
		}
		Data += Written;
		Left -= Written;
	}
	while (Left > 0);
	return true;
}

bool FDreamWin32FileOps::Flush(Handle H)
{
	if (IsFenced())
	{
		return false;
	}
	if (!::FlushFileBuffers(DreamFileOpsLocal::ToHandle(H)))
	{
		SetError(TEXT("Flush"), std::string_view());
		return false;
	}
	return true;
}

bool FDreamWin32FileOps::Truncate(Handle H, uint64_t Size)
{
	if (IsFenced())
	{
		return false;
	}
	const HANDLE File = DreamFileOpsLocal::ToHandle(H);
	LARGE_INTEGER To;
	To.QuadPart = static_cast<LONGLONG>(Size);
	if (::SetFilePointerEx(File, To, nullptr, FILE_BEGIN) && ::SetEndOfFile(File))
	{
		return true;
	}
	SetError(TEXT("Truncate"), std::string_view());
	LARGE_INTEGER Zero;
	Zero.QuadPart = 0;
	::SetFilePointerEx(File, Zero, nullptr, FILE_END);   // later appends still go to the end
	return false;
}

bool FDreamWin32FileOps::ModifiedUtcMs(std::string_view Path, int64_t& OutUtcMs)
{
	if (IsFenced())
	{
		return false;
	}
	WIN32_FILE_ATTRIBUTE_DATA Data;
	if (!::GetFileAttributesExW(*DreamFileOpsLocal::ToWindowsPath(Path), GetFileExInfoStandard, &Data))
	{
		SetError(TEXT("Stat"), Path);
		return false;
	}
	ULARGE_INTEGER Ticks;
	Ticks.LowPart = Data.ftLastWriteTime.dwLowDateTime;
	Ticks.HighPart = Data.ftLastWriteTime.dwHighDateTime;
	// FILETIME counts 100 ns from 1601-01-01; 11644473600000 ms separate that from the Unix epoch.
	OutUtcMs = static_cast<int64_t>(Ticks.QuadPart / 10000ull) - 11644473600000ll;
	return true;
}

bool FDreamWin32FileOps::FileSize(std::string_view Path, int64& OutBytes)
{
	if (IsFenced())
	{
		return false;
	}
	WIN32_FILE_ATTRIBUTE_DATA Data;
	if (!::GetFileAttributesExW(*DreamFileOpsLocal::ToWindowsPath(Path), GetFileExInfoStandard, &Data))
	{
		SetError(TEXT("Stat"), Path);
		return false;
	}
	ULARGE_INTEGER Size;
	Size.LowPart = Data.nFileSizeLow;
	Size.HighPart = Data.nFileSizeHigh;
	OutBytes = static_cast<int64>(Size.QuadPart);
	return true;
}

bool FDreamWin32FileOps::Touch(std::string_view Path)
{
	if (IsFenced())
	{
		return false;
	}
	const FString WinPath = DreamFileOpsLocal::ToWindowsPath(Path);
	HANDLE H = INVALID_HANDLE_VALUE;
	DreamFileOpsLocal::WithRetries([this]() { return IsFenced(); }, [&]()
	{
		H = ::CreateFileW(*WinPath, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		return H != INVALID_HANDLE_VALUE;
	});
	if (H == INVALID_HANDLE_VALUE)
	{
		SetError(TEXT("Touch"), Path);
		return false;
	}
	// A rename keeps the last write time, so the .corrupt file is dated from this call (ModifiedUtcMs reads the same field).
	FILETIME Now;
	::GetSystemTimeAsFileTime(&Now);
	const bool bOk = ::SetFileTime(H, nullptr, nullptr, &Now) != 0;
	if (!bOk)
	{
		SetError(TEXT("Touch"), Path);
	}
	::CloseHandle(H);
	return bOk;
}

void FDreamWin32FileOps::Close(Handle H)
{
	if (H != InvalidHandle)
	{
		::CloseHandle(DreamFileOpsLocal::ToHandle(H));
	}
}

bool FDreamWin32FileOps::ReadAll(std::string_view Path, std::string& Out)
{
	Out.clear();
	if (IsFenced())
	{
		return false;
	}
	const FString WinPath = DreamFileOpsLocal::ToWindowsPath(Path);
	// A scanner or backup tool holding the file without read sharing is retried (§10.11, review r1 persist-M2).
	HANDLE H = INVALID_HANDLE_VALUE;
	DreamFileOpsLocal::WithRetries([this]() { return IsFenced(); }, [&]()
	{
		H = ::CreateFileW(*WinPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		return H != INVALID_HANDLE_VALUE;
	});
	if (H == INVALID_HANDLE_VALUE)
	{
		SetError(TEXT("Open"), Path);
		return false;
	}
	LARGE_INTEGER Size;
	if (!::GetFileSizeEx(H, &Size) || Size.QuadPart < 0 || Size.QuadPart > (2ll << 30))
	{
		SetError(TEXT("Size"), Path);
		::CloseHandle(H);
		return false;
	}
	Out.resize(static_cast<size_t>(Size.QuadPart));
	size_t Done = 0;
	while (Done < Out.size())
	{
		DWORD Read = 0;
		const DWORD Want = static_cast<DWORD>(FMath::Min<size_t>(Out.size() - Done, 1u << 24));
		if (!::ReadFile(H, Out.data() + Done, Want, &Read, nullptr))
		{
			SetError(TEXT("Read"), Path);
			::CloseHandle(H);
			return false;
		}
		if (Read == 0)
		{
			break;
		}
		Done += Read;
	}
	Out.resize(Done);
	::CloseHandle(H);
	return true;
}

bool FDreamWin32FileOps::ReadPrefix(std::string_view Path, size_t MaxBytes, std::string& Out)
{
	Out.clear();
	if (IsFenced())
	{
		return false;
	}
	const FString WinPath = DreamFileOpsLocal::ToWindowsPath(Path);
	// A scanner or backup tool holding the file without read sharing is retried (§10.11, review r1 persist-M2).
	HANDLE H = INVALID_HANDLE_VALUE;
	DreamFileOpsLocal::WithRetries([this]() { return IsFenced(); }, [&]()
	{
		H = ::CreateFileW(*WinPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		return H != INVALID_HANDLE_VALUE;
	});
	if (H == INVALID_HANDLE_VALUE)
	{
		SetError(TEXT("Open"), Path);
		return false;
	}
	Out.resize(FMath::Min<size_t>(MaxBytes, 1u << 20));
	DWORD Read = 0;
	const bool bOk = ::ReadFile(H, Out.data(), static_cast<DWORD>(Out.size()), &Read, nullptr) != 0;
	::CloseHandle(H);
	if (!bOk)
	{
		SetError(TEXT("Read"), Path);
		Out.clear();
		return false;
	}
	Out.resize(Read);
	return true;
}

bool FDreamWin32FileOps::RenameNew(std::string_view From, std::string_view To)
{
	if (IsFenced())
	{
		return false;
	}
	const FString WinFrom = DreamFileOpsLocal::ToWindowsPath(From);
	const FString WinTo = DreamFileOpsLocal::ToWindowsPath(To);
	const bool bOk = DreamFileOpsLocal::WithRetries([this]() { return IsFenced(); }, [&]() { return ::MoveFileExW(*WinFrom, *WinTo, MOVEFILE_WRITE_THROUGH) != 0; });
	if (!bOk)
	{
		SetError(TEXT("Rename"), To);
	}
	return bOk;
}

bool FDreamWin32FileOps::ReplaceAtomic(std::string_view Target, std::string_view Temp, std::string_view Backup)
{
	if (IsFenced())
	{
		return false;
	}
	const FString WinTarget = DreamFileOpsLocal::ToWindowsPath(Target);
	const FString WinTemp = DreamFileOpsLocal::ToWindowsPath(Temp);
	const FString WinBackup = DreamFileOpsLocal::ToWindowsPath(Backup);
	const bool bTargetExists = ::GetFileAttributesW(*WinTarget) != INVALID_FILE_ATTRIBUTES;
	bool bOk = false;
	if (bTargetExists)
	{
		// ReplaceFileW keeps the target's ACL, attributes, creation time and alternate streams (§10.14 step 4).
		bOk = DreamFileOpsLocal::WithRetries([this]() { return IsFenced(); }, [&]()
		{
			return ::ReplaceFileW(*WinTarget, *WinTemp, Backup.empty() ? nullptr : *WinBackup,
				REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0;
		});
	}
	else
	{
		bOk = DreamFileOpsLocal::WithRetries([this]() { return IsFenced(); }, [&]() { return ::MoveFileExW(*WinTemp, *WinTarget, MOVEFILE_WRITE_THROUGH) != 0; });
	}
	if (!bOk)
	{
		SetError(TEXT("Replace"), Target);
	}
	return bOk;
}

bool FDreamWin32FileOps::Remove(std::string_view Path)
{
	if (IsFenced())
	{
		return false;
	}
	const FString WinPath = DreamFileOpsLocal::ToWindowsPath(Path);
	const bool bOk = DreamFileOpsLocal::WithRetries([this]() { return IsFenced(); }, [&]() { return ::DeleteFileW(*WinPath) != 0; });
	if (!bOk)
	{
		if (::GetLastError() == ERROR_FILE_NOT_FOUND)
		{
			return true;
		}
		SetError(TEXT("Delete"), Path);
	}
	return bOk;
}

bool FDreamWin32FileOps::List(std::string_view Dir, std::vector<std::string>& OutNames)
{
	OutNames.clear();
	if (IsFenced())
	{
		return false;
	}
	const FString Pattern = DreamFileOpsLocal::ToWindowsPath(Dir) / TEXT("*");
	WIN32_FIND_DATAW Data;
	HANDLE Find = ::FindFirstFileW(*Pattern, &Data);
	if (Find == INVALID_HANDLE_VALUE)
	{
		if (::GetLastError() == ERROR_FILE_NOT_FOUND || ::GetLastError() == ERROR_PATH_NOT_FOUND)
		{
			return true;
		}
		SetError(TEXT("List"), Dir);
		return false;
	}
	do
	{
		if ((Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			OutNames.push_back(DreamText::ToUtf8(FString(Data.cFileName)));
		}
	}
	while (::FindNextFileW(Find, &Data));
	::FindClose(Find);
	return true;
}

bool FDreamWin32FileOps::MakeDirs(std::string_view Dir)
{
	if (IsFenced())
	{
		return false;
	}
	FString Path = ToFString(Dir);
	FPaths::NormalizeDirectoryName(Path);
	TArray<FString> Parts;
	Path.ParseIntoArray(Parts, TEXT("/"), true);
	FString Current = Path.StartsWith(TEXT("//")) ? FString(TEXT("\\\\")) : FString();
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		Current += Parts[Index];
		const bool bIsDriveOrShare = (Index == 0 && Current.EndsWith(TEXT(":"))) || (Path.StartsWith(TEXT("//")) && Index < 2);
		if (!bIsDriveOrShare)
		{
			if (!::CreateDirectoryW(*Current, nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
			{
				SetError(TEXT("Create folder"), Dir);
				return false;
			}
		}
		Current += TEXT("\\");
	}
	return true;
}

bool FDreamWin32FileOps::FlushDir(std::string_view Dir)
{
	return !IsFenced();   // directory entries are durable after MoveFileExW(WRITE_THROUGH) on NTFS
}

bool FDreamWin32FileOps::Exists(std::string_view Path)
{
	if (IsFenced())
	{
		return false;
	}
	const FString WinPath = DreamFileOpsLocal::ToWindowsPath(Path);
	return ::GetFileAttributesW(*WinPath) != INVALID_FILE_ATTRIBUTES;
}

bool FDreamWin32FileOps::GetFinalPath(const FString& Path, FString& OutPath)
{
	OutPath = Path;
	FString WinPath = Path;
	WinPath.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	HANDLE H = ::CreateFileW(*WinPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (H == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	TCHAR Buffer[MAX_PATH * 4] = {};
	const DWORD Len = ::GetFinalPathNameByHandleW(H, Buffer, UE_ARRAY_COUNT(Buffer), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	::CloseHandle(H);
	if (Len == 0 || Len >= UE_ARRAY_COUNT(Buffer))
	{
		return false;
	}
	FString Final(Buffer);
	if (Final.StartsWith(TEXT("\\\\?\\UNC\\")))
	{
		Final = TEXT("\\\\") + Final.Mid(8);
	}
	else if (Final.StartsWith(TEXT("\\\\?\\")))
	{
		Final = Final.Mid(4);
	}
	Final.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	OutPath = Final;
	return true;
}

int32 FDreamWin32FileOps::GetLinkCount(const FString& Path)
{
	FString WinPath = Path;
	WinPath.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	HANDLE H = ::CreateFileW(*WinPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (H == INVALID_HANDLE_VALUE)
	{
		return 0;
	}
	BY_HANDLE_FILE_INFORMATION Info = {};
	const bool bOk = ::GetFileInformationByHandle(H, &Info) != 0;
	::CloseHandle(H);
	return bOk ? static_cast<int32>(Info.nNumberOfLinks) : 0;
}

bool FDreamWin32FileOps::IsRemotePath(const FString& Path)
{
	if (Path.StartsWith(TEXT("//")) || Path.StartsWith(TEXT("\\\\")))
	{
		return true;
	}
	if (Path.Len() >= 2 && Path[1] == TEXT(':'))
	{
		const FString Root = Path.Left(2) + TEXT("\\");
		return ::GetDriveTypeW(*Root) == DRIVE_REMOTE;
	}
	return false;
}

bool FDreamWin32FileOps::WriteInPlace(const FString& Target, std::string_view Bytes, const FString& Backup, FString& OutError)
{
	FDreamWin32FileOps Ops;
	const std::string TargetUtf8 = ToUtf8Path(Target);
	const std::string BackupUtf8 = ToUtf8Path(Backup);

	std::string OldBytes;
	Handle BackupHandle = InvalidHandle;
	if (!Ops.ReadAll(TargetUtf8, OldBytes) || !Ops.CreateExclusive(BackupUtf8, BackupHandle)
		|| !Ops.Append(BackupHandle, OldBytes) || !Ops.Flush(BackupHandle))
	{
		Ops.Close(BackupHandle);
		Ops.Remove(BackupUtf8);
		OutError = ToFString(Ops.LastError());
		return false;
	}
	Ops.Close(BackupHandle);

	auto WriteWhole = [&Ops](const std::string& Path, std::string_view Data) -> bool
	{
		const FString WinPath = DreamFileOpsLocal::ToWindowsPath(Path);
		HANDLE H = INVALID_HANDLE_VALUE;
		DreamFileOpsLocal::WithRetries(&DreamFileOpsLocal::NeverFenced, [&]()
		{
			H = ::CreateFileW(*WinPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			return H != INVALID_HANDLE_VALUE;
		});
		if (H == INVALID_HANDLE_VALUE)
		{
			Ops.SetError(TEXT("Open"), Path);
			return false;
		}
		const Handle Wrapped = DreamFileOpsLocal::FromHandle(H);
		const bool bOk = Ops.Append(Wrapped, Data) && Ops.Flush(Wrapped);
		Ops.Close(Wrapped);
		return bOk;
	};

	if (!WriteWhole(TargetUtf8, Bytes))
	{
		OutError = ToFString(Ops.LastError());
		// The backup stays until the old bytes are back (§10.14 hard links).
		if (WriteWhole(TargetUtf8, OldBytes))
		{
			Ops.Remove(BackupUtf8);
		}
		return false;
	}
	Ops.Remove(BackupUtf8);
	return true;
}

uint64 FDreamWin32FileOps::AcquireLockFile(const FString& Path, bool& bOutHeldByOther, std::string_view Content, bool bOnlyIfExists)
{
	bOutHeldByOther = false;
	FString WinPath = Path;
	WinPath.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	// FILE_SHARE_READ only (§10.5): a second writer's open fails with a sharing violation for as long as this handle lives,
	// while a reader opening with FILE_SHARE_READ | FILE_SHARE_WRITE can still read who holds it.
	// bOnlyIfExists: a lock file is created only together with its holder's handle (OPEN_ALWAYS is one atomic step), so one
	// that does not exist yet belongs to a namespace whose owner is still starting (instances, review r1).
	HANDLE H = ::CreateFileW(*WinPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, bOnlyIfExists ? OPEN_EXISTING : OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (H == INVALID_HANDLE_VALUE)
	{
		bOutHeldByOther = ::GetLastError() == ERROR_SHARING_VIOLATION;
		return 0;
	}
	const uint64 Lock = static_cast<uint64>(reinterpret_cast<UPTRINT>(H));
	if (Content.empty())
	{
		// The owner's pid at least, for diagnostics; the content is written only through the lock handle.
		const std::string Pid = DreamText::ToUtf8(FString::Printf(TEXT("{\"pid\": %u}\n"), FPlatformProcess::GetCurrentProcessId()));
		WriteLockContent(Lock, Pid);
	}
	else
	{
		WriteLockContent(Lock, Content);
	}
	return Lock;
}

void FDreamWin32FileOps::ReleaseLockFile(uint64 LockHandle)
{
	if (LockHandle != 0)
	{
		::CloseHandle(reinterpret_cast<HANDLE>(static_cast<UPTRINT>(LockHandle)));
	}
}

bool FDreamWin32FileOps::WriteLockContent(uint64 LockHandle, std::string_view Content)
{
	if (LockHandle == 0)
	{
		return false;
	}
	// §10.5: seek to 0, write, SetEndOfFile, flush; never a rename or replace (the held handle refuses DELETE).
	HANDLE H = reinterpret_cast<HANDLE>(static_cast<UPTRINT>(LockHandle));
	DWORD Written = 0;
	return ::SetFilePointer(H, 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER
		&& ::WriteFile(H, Content.data(), static_cast<DWORD>(Content.size()), &Written, nullptr) != 0
		&& Written == static_cast<DWORD>(Content.size())
		&& ::SetEndOfFile(H) != 0
		&& ::FlushFileBuffers(H) != 0;
}

bool FDreamWin32FileOps::ReadLockFile(const FString& Path, std::string& OutContent)
{
	OutContent.clear();
	FString WinPath = Path;
	WinPath.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	HANDLE H = ::CreateFileW(*WinPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (H == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	char Buffer[4096];
	DWORD Read = 0;
	const bool bOk = ::ReadFile(H, Buffer, sizeof(Buffer), &Read, nullptr) != 0;
	::CloseHandle(H);
	if (bOk)
	{
		OutContent.assign(Buffer, Read);
	}
	return bOk;
}

bool FDreamWin32FileOps::IsLockHeld(const FString& Path)
{
	bool bHeld = false;
	FString WinPath = Path;
	WinPath.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	if (::GetFileAttributesW(*WinPath) == INVALID_FILE_ATTRIBUTES)
	{
		return false;   // no lock file: nobody holds it (and none is created by asking)
	}
	HANDLE H = ::CreateFileW(*WinPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (H == INVALID_HANDLE_VALUE)
	{
		bHeld = ::GetLastError() == ERROR_SHARING_VIOLATION;
		return bHeld;
	}
	::CloseHandle(H);
	return false;
}

FString FDreamWin32FileOps::GetCurrentUserSid()
{
	HANDLE Token = nullptr;
	if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &Token))
	{
		return FString();
	}
	FString Result;
	DWORD Size = 0;
	::GetTokenInformation(Token, TokenUser, nullptr, 0, &Size);
	if (Size > 0)
	{
		TArray<uint8> Buffer;
		Buffer.SetNumZeroed(static_cast<int32>(Size));
		if (::GetTokenInformation(Token, TokenUser, Buffer.GetData(), Size, &Size))
		{
			const TOKEN_USER* User = reinterpret_cast<const TOKEN_USER*>(Buffer.GetData());
			LPWSTR Text = nullptr;
			if (::ConvertSidToStringSidW(User->User.Sid, &Text) && Text != nullptr)
			{
				Result = FString(Text);
				::LocalFree(Text);
			}
		}
	}
	::CloseHandle(Token);
	return Result;
}

bool FDreamWin32FileOps::ListDirs(const FString& Dir, TArray<FString>& OutNames)
{
	OutNames.Reset();
	FString Pattern = Dir / TEXT("*");
	Pattern.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	WIN32_FIND_DATAW Data;
	HANDLE Find = ::FindFirstFileW(*Pattern, &Data);
	if (Find == INVALID_HANDLE_VALUE)
	{
		return ::GetLastError() == ERROR_FILE_NOT_FOUND || ::GetLastError() == ERROR_PATH_NOT_FOUND;
	}
	do
	{
		const FString Name(Data.cFileName);
		if ((Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && Name != TEXT(".") && Name != TEXT(".."))
		{
			OutNames.Add(Name);
		}
	}
	while (::FindNextFileW(Find, &Data));
	::FindClose(Find);
	return true;
}

FString FDreamWin32FileOps::GetIdentity(const FString& Path)
{
	FString WinPath = Path;
	WinPath.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	// FILE_FLAG_BACKUP_SEMANTICS opens a folder too; without FILE_FLAG_OPEN_REPARSE_POINT a junction or symlink is followed,
	// so every path that reaches the same folder gives the same identity (§10.1 "by folder identity, never by path").
	HANDLE H = ::CreateFileW(*WinPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (H == INVALID_HANDLE_VALUE)
	{
		return FString();
	}
	BY_HANDLE_FILE_INFORMATION Info;
	FString Result;
	if (::GetFileInformationByHandle(H, &Info))
	{
		const uint64 Index = (static_cast<uint64>(Info.nFileIndexHigh) << 32) | Info.nFileIndexLow;
		Result = FString::Printf(TEXT("%08x:%016llx"), static_cast<uint32>(Info.dwVolumeSerialNumber), Index);
	}
	::CloseHandle(H);
	return Result;
}

bool FDreamWin32FileOps::DeleteTree(const FString& Dir)
{
	FString WinDir = Dir;
	WinDir.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	const DWORD Attributes = ::GetFileAttributesW(*WinDir);
	if (Attributes == INVALID_FILE_ATTRIBUTES)
	{
		return true;
	}
	if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		return ::RemoveDirectoryW(*WinDir) != 0;   // a junction or link: the link goes, never what it points at
	}
	bool bOk = true;
	WIN32_FIND_DATAW Data;
	HANDLE Find = ::FindFirstFileW(*(WinDir + TEXT("\\*")), &Data);
	if (Find != INVALID_HANDLE_VALUE)
	{
		do
		{
			const FString Name(Data.cFileName);
			if (Name == TEXT(".") || Name == TEXT(".."))
			{
				continue;
			}
			const FString Child = WinDir + TEXT("\\") + Name;
			if ((Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				bOk = DeleteTree(Child) && bOk;
			}
			else
			{
				::SetFileAttributesW(*Child, FILE_ATTRIBUTE_NORMAL);
				bOk = ::DeleteFileW(*Child) != 0 && bOk;
			}
		}
		while (::FindNextFileW(Find, &Data));
		::FindClose(Find);
	}
	return ::RemoveDirectoryW(*WinDir) != 0 && bOk;
}

bool FDreamWin32FileOps::CopyNew(const FString& From, const FString& To)
{
	FString WinFrom = From;
	FString WinTo = To;
	WinFrom.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	WinTo.ReplaceInline(TEXT("/"), TEXT("\\"), ESearchCase::CaseSensitive);
	return ::CopyFileW(*WinFrom, *WinTo, /*bFailIfExists*/ 1) != 0;
}

#else   // !PLATFORM_WINDOWS: Linux Preview is M4 and macOS 1.2 (§10 table); every call fails.

bool FDreamWin32FileOps::CreateExclusive(std::string_view, Handle& Out) { Out = InvalidHandle; Error = "unsupported"; return false; }
bool FDreamWin32FileOps::OpenAppend(std::string_view, Handle& Out) { Out = InvalidHandle; Error = "unsupported"; return false; }
bool FDreamWin32FileOps::Append(Handle, std::string_view) { Error = "unsupported"; return false; }
bool FDreamWin32FileOps::Flush(Handle) { Error = "unsupported"; return false; }
bool FDreamWin32FileOps::Truncate(Handle, uint64_t) { Error = "unsupported"; return false; }
bool FDreamWin32FileOps::ModifiedUtcMs(std::string_view, int64_t&) { Error = "unsupported"; return false; }
bool FDreamWin32FileOps::Touch(std::string_view) { Error = "unsupported"; return false; }
bool FDreamWin32FileOps::FileSize(std::string_view, int64& OutBytes) { OutBytes = 0; Error = "unsupported"; return false; }
void FDreamWin32FileOps::Close(Handle) {}
bool FDreamWin32FileOps::ReadAll(std::string_view, std::string& Out) { Out.clear(); Error = "unsupported"; return false; }
bool FDreamWin32FileOps::ReadPrefix(std::string_view, size_t, std::string& Out) { Out.clear(); Error = "unsupported"; return false; }
bool FDreamWin32FileOps::RenameNew(std::string_view, std::string_view) { Error = "unsupported"; return false; }
bool FDreamWin32FileOps::ReplaceAtomic(std::string_view, std::string_view, std::string_view) { Error = "unsupported"; return false; }
bool FDreamWin32FileOps::Remove(std::string_view) { Error = "unsupported"; return false; }
bool FDreamWin32FileOps::List(std::string_view, std::vector<std::string>& OutNames) { OutNames.clear(); Error = "unsupported"; return false; }
bool FDreamWin32FileOps::MakeDirs(std::string_view) { Error = "unsupported"; return false; }
bool FDreamWin32FileOps::FlushDir(std::string_view) { return false; }
bool FDreamWin32FileOps::Exists(std::string_view) { return false; }
bool FDreamWin32FileOps::GetFinalPath(const FString& Path, FString& OutPath) { OutPath = Path; return false; }
int32 FDreamWin32FileOps::GetLinkCount(const FString&) { return 0; }
bool FDreamWin32FileOps::IsRemotePath(const FString&) { return false; }
bool FDreamWin32FileOps::WriteInPlace(const FString&, std::string_view, const FString&, FString& OutError) { OutError = TEXT("unsupported"); return false; }
uint64 FDreamWin32FileOps::AcquireLockFile(const FString&, bool& bOutHeldByOther, std::string_view, bool) { bOutHeldByOther = false; return 0; }
void FDreamWin32FileOps::ReleaseLockFile(uint64) {}
bool FDreamWin32FileOps::WriteLockContent(uint64, std::string_view) { return false; }
bool FDreamWin32FileOps::ReadLockFile(const FString&, std::string& OutContent) { OutContent.clear(); return false; }
bool FDreamWin32FileOps::IsLockHeld(const FString&) { return false; }
FString FDreamWin32FileOps::GetCurrentUserSid() { return FString(); }
bool FDreamWin32FileOps::ListDirs(const FString&, TArray<FString>& OutNames) { OutNames.Reset(); return false; }
FString FDreamWin32FileOps::GetIdentity(const FString&) { return FString(); }
bool FDreamWin32FileOps::DeleteTree(const FString&) { return false; }
bool FDreamWin32FileOps::CopyNew(const FString&, const FString&) { return false; }

#endif
