// SPDX-License-Identifier: Apache-2.0
// The Win64 dream::journal::FileOps (TDD §10.2.1, §10.2.3, §10.14; slice A choice A5): CreateFileW, WriteFile,
// FlushFileBuffers, MoveFileExW(MOVEFILE_WRITE_THROUGH) and ReplaceFileW, with the sharing-violation retries of §10.14.
// One instance per thread (LastError is per instance). The optional fence is the journal's abandon fence (§10.2.5): once it
// is set, every operation fails without touching the disk. Also the lock-file and final-path helpers of §10.1 and §10.5.
// Off Windows the type still exists and fails every call (Linux and macOS are M4/1.2).
#pragma once

#include "CoreMinimal.h"
#include "dream/JournalFormat.h"

#include <atomic>
#include <string>

class DREAMEDITOR_API FDreamWin32FileOps final : public dream::journal::FileOps
{
public:
	explicit FDreamWin32FileOps(TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> InFence = nullptr);
	virtual ~FDreamWin32FileOps() override = default;

	/**
	 * While one is alive on a thread, the sharing-violation retries (10..500 ms) of the FileOps calls made on that thread
	 * wait through Wait instead of sleeping (§10.2.1, §10.2.3, review r1): the journal writer serves the due commits while a
	 * history or maintenance slice waits out a file another program holds. FileOps calls made inside Wait sleep normally.
	 */
	/** Waits Seconds the way a retry would: through this thread's FScopedRetryWait (the journal writer serves commits), else a
	 *  sleep. A history task that must wait (an index another program holds, persist-r2-M2) uses it. */
	static void WaitLikeARetry(float Seconds);
	/** The game thread's FileOps calls must not sleep (review r2): while one is alive on a thread, the retries are tried at
	 *  once, with no wait between them. */
	class FScopedNoRetryWait;

	class FScopedRetryWait
	{
	public:
		explicit FScopedRetryWait(TFunction<void(float)> InWait);
		~FScopedRetryWait();
		FScopedRetryWait(const FScopedRetryWait&) = delete;
		FScopedRetryWait& operator=(const FScopedRetryWait&) = delete;

	private:
		TFunction<void(float)> Wait;
		const TFunction<void(float)>* Previous = nullptr;
	};

	class FScopedNoRetryWait
	{
	public:
		FScopedNoRetryWait() : Scope([](float) {}) {}

	private:
		FScopedRetryWait Scope;
	};

	virtual bool CreateExclusive(std::string_view Path, Handle& Out) override;
	virtual bool OpenAppend(std::string_view Path, Handle& Out) override;
	virtual bool Append(Handle H, std::string_view Bytes) override;
	virtual bool Flush(Handle H) override;
	virtual void Close(Handle H) override;
	virtual bool ReadAll(std::string_view Path, std::string& Out) override;
	virtual bool RenameNew(std::string_view From, std::string_view To) override;
	virtual bool ReplaceAtomic(std::string_view Target, std::string_view Temp, std::string_view Backup) override;
	virtual bool Remove(std::string_view Path) override;
	virtual bool List(std::string_view Dir, std::vector<std::string>& OutNames) override;
	virtual bool MakeDirs(std::string_view Dir) override;
	virtual bool FlushDir(std::string_view Dir) override;
	virtual bool Exists(std::string_view Path) override;
	virtual std::string LastError() const override;
	/** SetEndOfFile at Size (a failed commit cut back off, PER-04); the file pointer is left at the new end. Fenced. */
	virtual bool Truncate(Handle H, uint64_t Size) override;
	/** The file's last write time in Unix ms (the 30-day .corrupt expiry, PER-05). Fenced. */
	virtual bool ModifiedUtcMs(std::string_view Path, int64_t& OutUtcMs) override;
	/** Sets the file's last write time to now (a damaged generation set aside as .corrupt is kept 30 days from then, §10.2.4). Fenced. */
	virtual bool Touch(std::string_view Path) override;

	/** The file's size in bytes (local-history retention, §10.4). Fenced. */
	bool FileSize(std::string_view Path, int64& OutBytes);

	/** At most MaxBytes from the start of a file (a generation header, §10.6); fenced like ReadAll. */
	virtual bool ReadPrefix(std::string_view Path, size_t MaxBytes, std::string& Out) override;

	// ---- Helpers (any thread) ----

	/** UTF-8 forward-slash path <-> FString. */
	static FString ToFString(std::string_view Utf8Path);
	static std::string ToUtf8Path(const FString& Path);

	/** GetFinalPathNameByHandleW with "\\?\UNC\" mapped to "\\" and "\\?\" stripped (§10.1, §10.14 step 2). False when the
	 *  path cannot be opened (a new file); OutPath is then the input. */
	static bool GetFinalPath(const FString& Path, FString& OutPath);
	/** nNumberOfLinks of an existing file; 0 when it cannot be opened. */
	static int32 GetLinkCount(const FString& Path);
	/** True when the path's volume is a network drive or a UNC share (§10.1 bIsRemote). */
	static bool IsRemotePath(const FString& Path);
	/** The OS error text of the calling thread's last Win32 error. */
	static FString GetLastErrorText();

	/** §10.14 hard links: copies the old bytes to Backup and flushes it, truncates and writes the target through one handle
	 *  and flushes, then deletes the backup; on a failed write the backup is copied back. */
	static bool WriteInPlace(const FString& Target, std::string_view Bytes, const FString& Backup, FString& OutError);

	/**
	 * A held-open lock file (§10.5 instance.lock, ns.lock): 0 when another process holds it. The handle is
	 * GENERIC_READ | GENERIC_WRITE with FILE_SHARE_READ, so a second writer gets ERROR_SHARING_VIOLATION (held) while a
	 * reader (ReadLockFile) can still read who holds it. Content, when given, is written through the handle (seek 0, write,
	 * SetEndOfFile, flush): the file is never renamed or replaced.
	 */
	static uint64 AcquireLockFile(const FString& Path, bool& bOutHeldByOther, std::string_view Content = std::string_view(),
		bool bOnlyIfExists = false);
	static void ReleaseLockFile(uint64 LockHandle);
	/** Rewrites a held lock's content through its handle (the heartbeat path, §10.5). */
	static bool WriteLockContent(uint64 LockHandle, std::string_view Content);
	/** Reads a lock file another process may hold (FILE_SHARE_READ | FILE_SHARE_WRITE). False when it does not exist. */
	static bool ReadLockFile(const FString& Path, std::string& OutContent);
	/** True when another process holds the lock now (a free lock is acquired and released at once to find out). */
	static bool IsLockHeld(const FString& Path);

	// ---- Instances and storage roots (§10.1, §10.5) ----

	/** The account's string SID (GetTokenInformation(TokenUser) + ConvertSidToStringSidW); empty when unknown. */
	static FString GetCurrentUserSid();
	/** The sub-folder names of Dir (no "." or ".."); false when Dir cannot be listed. */
	static bool ListDirs(const FString& Dir, TArray<FString>& OutNames);
	/** "<volume serial>:<file index>" of an existing file or folder (GetFileInformationByHandle): one identity for every
	 *  path that reaches it (a junction, a symlink, a subst drive). Empty when it cannot be opened. */
	static FString GetIdentity(const FString& Path);
	/** Deletes a folder tree (files first, then the folders); false when something stayed. Never follows reparse points. */
	static bool DeleteTree(const FString& Dir);
	/** Copies one file, overwriting nothing; false when the target exists or the copy failed. */
	static bool CopyNew(const FString& From, const FString& To);

	/** True once the fence is set (the journal writer or DreamStorageProbe abandoned): jobs stop touching files. */
	bool IsFenced();

private:
	void SetError(const TCHAR* Operation, std::string_view Path);

	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Fence;
	std::string Error;
};

/**
 * Reads through another FileOps and refuses every write, rename and delete (§10.1: the cross-root scan and "View its tabs"
 * read namespaces another root or another instance owns; ReadNamespace with bMayModify false already writes nothing, and
 * this makes it impossible). Same thread rules as the FileOps it wraps.
 */
class FDreamReadOnlyFileOps final : public dream::journal::FileOps
{
public:
	explicit FDreamReadOnlyFileOps(dream::journal::FileOps& InInner) : Inner(InInner) {}

	virtual bool CreateExclusive(std::string_view, Handle& Out) override { Out = InvalidHandle; return false; }
	virtual bool OpenAppend(std::string_view, Handle& Out) override { Out = InvalidHandle; return false; }
	virtual bool Append(Handle, std::string_view) override { return false; }
	virtual bool Flush(Handle) override { return false; }
	virtual void Close(Handle) override {}
	virtual bool ReadAll(std::string_view Path, std::string& Out) override { return Inner.ReadAll(Path, Out); }
	virtual bool RenameNew(std::string_view, std::string_view) override { return false; }
	virtual bool ReplaceAtomic(std::string_view, std::string_view, std::string_view) override { return false; }
	virtual bool Remove(std::string_view) override { return false; }
	virtual bool List(std::string_view Dir, std::vector<std::string>& OutNames) override { return Inner.List(Dir, OutNames); }
	virtual bool MakeDirs(std::string_view) override { return false; }
	virtual bool FlushDir(std::string_view) override { return false; }
	virtual bool Exists(std::string_view Path) override { return Inner.Exists(Path); }
	virtual std::string LastError() const override { return Inner.LastError(); }
	virtual bool ModifiedUtcMs(std::string_view Path, int64_t& OutUtcMs) override { return Inner.ModifiedUtcMs(Path, OutUtcMs); }
	virtual bool Touch(std::string_view) override { return false; }
	virtual bool ReadPrefix(std::string_view Path, size_t MaxBytes, std::string& Out) override { return Inner.ReadPrefix(Path, MaxBytes, Out); }

private:
	dream::journal::FileOps& Inner;
};
