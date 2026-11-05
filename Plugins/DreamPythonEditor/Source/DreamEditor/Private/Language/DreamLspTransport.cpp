// SPDX-License-Identifier: Apache-2.0
// FLspTransport: spawn and containment, the two I/O threads, framing, queues, the stderr log and shutdown (TDD §7.1).
// See DreamLspTransport.h.
#include "Language/DreamLspTransport.h"

#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "dream/JsonRpc.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <psapi.h>
#include <tlhelp32.h>
#include "Windows/HideWindowsPlatformTypes.h"
#pragma comment(lib, "psapi.lib")
#endif

namespace DreamLspTransportLocal
{
	constexpr int64 EscapeCheckDelayMs = 2000;      // §7.1.3
	constexpr int64 RssSampleMs = 1000;             // §7.3.5: 1 Hz, the G3 gate's rate
	constexpr int64 RssOverCapMs = 60000;
	constexpr int64 CommitLogEveryMs = 60000;
	constexpr int64 BusyWindowMs = 50;              // §7.1.4: 1 ms polls while bytes arrived in the last 50 ms

#if PLATFORM_WINDOWS
	FCriticalSection& JobLock()
	{
		static FCriticalSection Lock;
		return Lock;
	}
	HANDLE& JobHandle()
	{
		static HANDLE Handle = nullptr;
		return Handle;
	}
	bool& JobFailed()
	{
		static bool bFailed = false;
		return bFailed;
	}
#endif
}

// ---- FDreamChildJob ---------------------------------------------------------------------------------------------------------

bool FDreamChildJob::AssignProcess(void* ProcessHandle)
{
#if PLATFORM_WINDOWS
	using namespace DreamLspTransportLocal;
	FScopeLock Guard(&JobLock());
	if (!EnsureCreated() || ProcessHandle == nullptr)
	{
		return false;
	}
	if (!::AssignProcessToJobObject(JobHandle(), static_cast<HANDLE>(ProcessHandle)))
	{
		UE_LOG(LogDream, Warning, TEXT("Language services: a server could not be put in the job object (error %u); it relies on its parent watch."), ::GetLastError());
		return false;
	}
	return true;
#else
	(void)ProcessHandle;
	return false;
#endif
}

bool FDreamChildJob::Exists()
{
#if PLATFORM_WINDOWS
	using namespace DreamLspTransportLocal;
	FScopeLock Guard(&JobLock());
	return JobHandle() != nullptr;
#else
	return false;
#endif
}

bool FDreamChildJob::EnsureCreated()
{
#if PLATFORM_WINDOWS
	using namespace DreamLspTransportLocal;
	FScopeLock Guard(&JobLock());       // recursive: AssignProcess holds it already
	if (JobHandle() == nullptr && !JobFailed())
	{
		HANDLE Job = ::CreateJobObjectW(nullptr, nullptr);
		if (Job != nullptr)
		{
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits = {};
			Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (::SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)))
			{
				JobHandle() = Job;
			}
			else
			{
				::CloseHandle(Job);
			}
		}
		if (JobHandle() == nullptr)
		{
			JobFailed() = true;
			UE_LOG(LogDream, Warning, TEXT("Language services: the kill-on-close job object could not be created (error %u); servers rely on their parent watch."), ::GetLastError());
		}
	}
	return JobHandle() != nullptr;
#else
	return false;
#endif
}

bool FDreamChildJob::IsProcessInJob(void* ProcessHandle)
{
#if PLATFORM_WINDOWS
	using namespace DreamLspTransportLocal;
	FScopeLock Guard(&JobLock());
	BOOL bIn = 0;
	return JobHandle() != nullptr && ::IsProcessInJob(static_cast<HANDLE>(ProcessHandle), JobHandle(), &bIn) && bIn;
#else
	(void)ProcessHandle;
	return false;
#endif
}

void FDreamChildJob::Close()
{
#if PLATFORM_WINDOWS
	using namespace DreamLspTransportLocal;
	FScopeLock Guard(&JobLock());
	if (JobHandle() != nullptr)
	{
		::CloseHandle(JobHandle());
		JobHandle() = nullptr;
	}
	JobFailed() = false;
#endif
}

// ---- The write thread ------------------------------------------------------------------------------------------------------

class FLspTransport::FWriter final : public FRunnable
{
public:
	explicit FWriter(void* InPipe)
		: Pipe(InPipe)
	{
		Wake = FPlatformProcess::GetSynchEventFromPool(false);
	}

	virtual ~FWriter() override
	{
		FPlatformProcess::ReturnSynchEventToPool(Wake);
	}

	void Enqueue(std::vector<uint8_t>&& Frame)
	{
		Pending.fetch_add(static_cast<int64>(Frame.size()));
		Frames.Enqueue(MoveTemp(Frame));
		Wake->Trigger();
	}

	int64 GetPendingBytes() const { return Pending.load(); }
	bool HasFailed() const { return bFailed.load(); }

	virtual uint32 Run() override
	{
		while (!bQuit.load())
		{
			std::vector<uint8_t> Frame;
			while (!bQuit.load() && Frames.Dequeue(Frame))
			{
				int64 Offset = 0;
				const int64 Size = static_cast<int64>(Frame.size());
				while (Offset < Size)
				{
					// The binary WritePipe; POSIX can write short (N4), so loop until every byte is out.
					int32 Written = 0;
					const int32 Chunk = static_cast<int32>(FMath::Min<int64>(Size - Offset, 1 << 20));
					const bool bOk = FPlatformProcess::WritePipe(Pipe, Frame.data() + Offset, Chunk, &Written);
					if (Written > 0)
					{
						Offset += Written;
					}
					if (!bOk && Written <= 0)
					{
						bFailed = true;     // the child closed its stdin or died; the read thread sees the exit
						break;
					}
				}
				Pending.fetch_sub(Size);
				if (bFailed.load())
				{
					break;
				}
			}
			if (bFailed.load())
			{
				// Keep draining so the pending count stays honest; nothing more can be written.
				while (Frames.Dequeue(Frame))
				{
					Pending.fetch_sub(static_cast<int64>(Frame.size()));
				}
			}
			Wake->Wait(100);
		}
		return 0;
	}

	virtual void Stop() override
	{
		bQuit = true;
		Wake->Trigger();
	}

private:
	void* Pipe = nullptr;
	FEvent* Wake = nullptr;
	TQueue<std::vector<uint8_t>, EQueueMode::Mpsc> Frames;
	std::atomic<int64> Pending{ 0 };
	std::atomic<bool> bQuit{ false };
	std::atomic<bool> bFailed{ false };
};

/** dream::ILspIo over the write thread, with the 32 MB outbound cap (§7.1.4). Read thread only. */
class FLspTransport::FIo final : public dream::ILspIo
{
public:
	explicit FIo(FWriter& InWriter)
		: Writer(InWriter)
	{
	}

	virtual void SendFrame(std::vector<uint8_t>&& Frame) override
	{
		if (Writer.GetPendingBytes() + static_cast<int64>(Frame.size()) > FLspTransport::MaxPendingOutboundBytes)
		{
			bOverCap = true;
			return;
		}
		Writer.Enqueue(MoveTemp(Frame));
	}

	bool IsOverCap() const { return bOverCap; }

private:
	FWriter& Writer;
	bool bOverCap = false;
};

/** <root>/Logs/LSP/<serverId>.log: rotation at 10 MB with 3 generations, 1 MB/s rate cap with a drop marker. Read thread. */
class FLspTransport::FLog
{
public:
	explicit FLog(const FString& InPath)
		: Path(InPath)
	{
		if (!Path.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
			Size = FMath::Max<int64>(IFileManager::Get().FileSize(*Path), 0);
			Open();
			if (!Archive.IsValid())
			{
				// (lang choice) Another editor on the same storage root holds the file: this one logs to <serverId>.<pid>.log.
				Path = FPaths::GetBaseFilename(Path, /*bRemovePath*/ false) + FString::Printf(TEXT(".%u.log"), FPlatformProcess::GetCurrentProcessId());
				Size = FMath::Max<int64>(IFileManager::Get().FileSize(*Path), 0);
				Open();
			}
		}
	}

	~FLog()
	{
		Flush();
	}

	void WriteBytes(const uint8* Data, int32 Count, int64 NowMs)
	{
		if (!Archive.IsValid() || Count <= 0)
		{
			return;
		}
		if (NowMs - WindowStartMs >= 1000)
		{
			WindowStartMs = NowMs;
			WindowBytes = 0;
			if (Dropped > 0)
			{
				const std::string Marker = "[dream: " + std::to_string(Dropped) + " bytes dropped]\n";
				Dropped = 0;
				Append(reinterpret_cast<const uint8*>(Marker.data()), static_cast<int32>(Marker.size()));
			}
		}
		if (WindowBytes + Count > FLspTransport::LogMaxBytesPerSecond)
		{
			Dropped += Count;
			return;
		}
		WindowBytes += Count;
		Append(Data, Count);
	}

	void WriteLine(const std::string& Line, int64 NowMs)
	{
		std::string Text = Line;
		Text.push_back('\n');
		WriteBytes(reinterpret_cast<const uint8*>(Text.data()), static_cast<int32>(Text.size()), NowMs);
	}

	void Flush()
	{
		if (Archive.IsValid())
		{
			Archive->Flush();
		}
	}

private:
	void Open()
	{
		Archive.Reset(IFileManager::Get().CreateFileWriter(*Path, FILEWRITE_Append | FILEWRITE_AllowRead));
	}

	void Append(const uint8* Data, int32 Count)
	{
		if (Size + Count > FLspTransport::LogRotateBytes)
		{
			Rotate();
		}
		if (Archive.IsValid())
		{
			Archive->Serialize(const_cast<uint8*>(Data), Count);
			Size += Count;
			if (++WritesSinceFlush >= 16)
			{
				Archive->Flush();
				WritesSinceFlush = 0;
			}
		}
	}

	void Rotate()
	{
		Archive.Reset();
		IFileManager& Files = IFileManager::Get();
		const FString Oldest = FString::Printf(TEXT("%s.%d"), *Path, FLspTransport::LogGenerations);
		Files.Delete(*Oldest, false, true, true);
		for (int32 Generation = FLspTransport::LogGenerations - 1; Generation >= 1; --Generation)
		{
			const FString From = FString::Printf(TEXT("%s.%d"), *Path, Generation);
			if (Files.FileExists(*From))
			{
				Files.Move(*FString::Printf(TEXT("%s.%d"), *Path, Generation + 1), *From, true, true, false, true);
			}
		}
		Files.Move(*FString::Printf(TEXT("%s.1"), *Path), *Path, true, true, false, true);
		Size = 0;
		Open();
	}

	FString Path;
	TUniquePtr<FArchive> Archive;
	int64 Size = 0;
	int64 WindowStartMs = 0;
	int64 WindowBytes = 0;
	int64 Dropped = 0;
	int32 WritesSinceFlush = 0;
};

// ---- FLspTransport -----------------------------------------------------------------------------------------------------------

FLspTransport::FLspTransport(const FDreamLaunchSpec& InSpec, dream::LspClientOptions InOptions)
	: Spec(InSpec)
	, Options(MoveTemp(InOptions))
{
	WakeRead = FPlatformProcess::GetSynchEventFromPool(false);
}

TUniquePtr<FLspTransport> FLspTransport::Start(const FDreamLaunchSpec& Spec, dream::LspClientOptions Options)
{
	TUniquePtr<FLspTransport> Transport(new FLspTransport(Spec, MoveTemp(Options)));
	const FString Name = FString::Printf(TEXT("Dream.Lsp.%s.Read"), *Spec.ServerId);
	Transport->ReadThread = FRunnableThread::Create(Transport.Get(), *Name, 0, TPri_BelowNormal);
	if (Transport->ReadThread == nullptr)
	{
		Transport->State = EDreamTransportState::Failed;
		FLspEvent Failed;
		Failed.Kind = FLspEvent::EKind::SpawnFailed;
		Failed.Text = TEXT("the transport's read thread could not be created");
		Transport->PostEvent(MoveTemp(Failed));
	}
	return Transport;
}

FLspTransport::~FLspTransport()
{
	if (ReadThread != nullptr)
	{
		RequestStop(FTimespan::FromMilliseconds(500));
		Join();
	}
	CloseHandles();
	FPlatformProcess::ReturnSynchEventToPool(WakeRead);
	WakeRead = nullptr;
}

void FLspTransport::Post(FLspCommand&& Command)
{
	CommandQueue.Enqueue(MoveTemp(Command));
	WakeRead->Trigger();
}

int32 FLspTransport::Drain(TArray<FLspEvent>& Out, int32 MaxEvents)
{
	check(IsInGameThread());
	int32 Count = 0;
	FLspEvent Event;
	while (Count < MaxEvents && EventQueue.Dequeue(Event))
	{
		Out.Add(MoveTemp(Event));
		++Count;
	}
	return Count;
}

void FLspTransport::RequestStop(FTimespan Grace)
{
	GraceMs = FMath::Max<int64>(0, static_cast<int64>(Grace.GetTotalMilliseconds()));
	bStopRequested = true;
	if (WakeRead != nullptr)
	{
		WakeRead->Trigger();
	}
}

void FLspTransport::Join()
{
	if (ReadThread != nullptr)
	{
		ReadThread->WaitForCompletion();
		delete ReadThread;
		ReadThread = nullptr;
	}
}

void FLspTransport::Stop()
{
	RequestStop(FTimespan::FromMilliseconds(static_cast<double>(GraceMs.load())));
}

int64 FLspTransport::NowMs()
{
	return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
}

void FLspTransport::PostEvent(FLspEvent&& Event)
{
	EventQueue.Enqueue(MoveTemp(Event));
}

bool FLspTransport::Spawn()
{
	if (!FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite) || !FPlatformProcess::CreatePipe(StdErrRead, StdErrWrite) ||
		!FPlatformProcess::CreatePipe(StdInRead, StdInWrite, /*bWritePipeLocal*/ true))
	{
		return false;
	}
	UE::HAL::FProcessStartInfo Info;
	Info.Uri = *Spec.Executable;
	Info.Arguments = *Spec.Arguments;
	Info.WorkingDirectory = Spec.WorkingDirectory.IsEmpty() ? nullptr : *Spec.WorkingDirectory;
	Info.bDetached = true;          // with bHidden: CREATE_NO_WINDOW, no console flashes for python.exe
	Info.bHidden = true;
	Info.bInheritHandles = false;   // PROC_THREAD_ATTRIBUTE_HANDLE_LIST holds only the three child pipe ends
	Info.StdIn.Value = StdInRead;
	Info.StdOut.Value = StdOutWrite;
	Info.StdErr.Value = StdErrWrite;    // explicit: FProcessStartInfo defaults StdErr to StdOut (§7.1.2)
	TTuple<FProcHandle, UE::HAL::EProcessId> Created = FPlatformProcess::CreateProc(Info);
	Process = Created.Get<0>();
	if (!Process.IsValid())
	{
		return false;
	}
	FDreamChildJob::AssignProcess(Process.Get());   // the next statement after the spawn (§7.1.3)
	Pid = static_cast<uint32>(Created.Get<1>());
	// The child's ends belong to the child now; closing ours lets EOF and broken pipes reach both sides.
	FPlatformProcess::ClosePipe(StdInRead, nullptr);
	FPlatformProcess::ClosePipe(nullptr, StdOutWrite);
	FPlatformProcess::ClosePipe(nullptr, StdErrWrite);
	StdInRead = StdOutWrite = StdErrWrite = nullptr;
	return true;
}

void FLspTransport::CloseHandles()
{
	FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
	FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
	FPlatformProcess::ClosePipe(StdErrRead, StdErrWrite);
	StdInRead = StdInWrite = StdOutRead = StdOutWrite = StdErrRead = StdErrWrite = nullptr;
	if (Process.IsValid())
	{
		FPlatformProcess::CloseProc(Process);
	}
}

void FLspTransport::ApplyCommand(FLspCommand& Command, int64 Now)
{
	auto NotRouted = [this](uint64 Tag, const char* Why)
	{
		FLspEvent Event;
		Event.Kind = FLspEvent::EKind::Client;
		Event.Tag = Tag;
		Event.Lsp.Kind = dream::LspEventKind::Error;
		Event.Lsp.Text = Why;
		PostEvent(MoveTemp(Event));
	};
	switch (Command.Kind)
	{
	case FLspCommand::EKind::Open:
		Client->Open(Command.Doc, Command.Uri, Command.LanguageId, Command.Text);
		break;
	case FLspCommand::EKind::Change:
		Client->Change(Command.Doc, Command.Changes);
		break;
	case FLspCommand::EKind::Close:
		Client->Close(Command.Doc);
		break;
	case FLspCommand::EKind::Notify:
		Client->Notify(Command.Method, MoveTemp(Command.Params));
		break;
	case FLspCommand::EKind::CodeActions:
	{
		const dream::LspRequestId Id = Client->RequestCodeActions(Command.Doc, Command.Range, Command.Context, Now);
		if (Id == 0)
		{
			NotRouted(Command.Tag, "the server is not ready or the document is not open there");
			break;
		}
		TagsById.Add(Id, Command.Tag);
		IdsByTag.Add(Command.Tag, Id);
		break;
	}
	case FLspCommand::EKind::Request:
	{
		const dream::LspRequestId Id = Client->Request(Command.Method, Command.Doc, Command.Version, MoveTemp(Command.Params),
			Command.TimeoutMs, Now);
		if (Id == 0)
		{
			NotRouted(Command.Tag, "the server does not offer this feature");
			break;
		}
		TagsById.Add(Id, Command.Tag);
		IdsByTag.Add(Command.Tag, Id);
		break;
	}
	case FLspCommand::EKind::Cancel:
		if (const dream::LspRequestId* Id = IdsByTag.Find(Command.Tag))
		{
			Client->Cancel(*Id);
			TagsById.Remove(*Id);
			IdsByTag.Remove(Command.Tag);
		}
		break;
	case FLspCommand::EKind::UserAction:
		Client->NoteUserAction(Now);
		break;
	case FLspCommand::EKind::SetConfiguration:
		ConfigByUri[Command.Uri] = MoveTemp(Command.Params);
		break;
	}
}

void FLspTransport::PostClientEvents()
{
	std::vector<dream::LspEvent> Events;
	Client->TakeEvents(Events);
	const int64 Now = NowMs();
	for (dream::LspEvent& E : Events)
	{
		if (E.Kind == dream::LspEventKind::Log)
		{
			Log->WriteLine("[lsp] " + E.Text, Now);
			continue;
		}
		FLspEvent Event;
		Event.Kind = FLspEvent::EKind::Client;
		if (E.Id != 0)
		{
			if (const uint64* Tag = TagsById.Find(E.Id))
			{
				Event.Tag = *Tag;
				IdsByTag.Remove(*Tag);
				TagsById.Remove(E.Id);
			}
		}
		if (E.Kind == dream::LspEventKind::ProtocolError)
		{
			Log->WriteLine("[dream] protocol error: " + E.Text, Now);
		}
		Event.Lsp = MoveTemp(E);
		PostEvent(MoveTemp(Event));
	}
}

void FLspTransport::CheckMemory(int64 Now)
{
#if PLATFORM_WINDOWS
	using namespace DreamLspTransportLocal;
	if (Spec.MaxRssMB == 0 || Now - LastRssSampleMs < RssSampleMs || !Process.IsValid())
	{
		return;
	}
	LastRssSampleMs = Now;
	PROCESS_MEMORY_COUNTERS_EX Counters = {};
	if (!::GetProcessMemoryInfo(static_cast<HANDLE>(Process.Get()), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Counters), sizeof(Counters)))
	{
		return;
	}
	// §7.3.5: the working set is enforced; private commit is only logged (it can sit far above it for healthy servers).
	const uint64 WorkingSetMB = static_cast<uint64>(Counters.WorkingSetSize) >> 20;
	if (Now - LastCommitLogMs >= CommitLogEveryMs)
	{
		LastCommitLogMs = Now;
		Log->WriteLine("[dream] working set " + std::to_string(WorkingSetMB) + " MB, private commit " +
			std::to_string(static_cast<uint64>(Counters.PrivateUsage) >> 20) + " MB", Now);
	}
	if (WorkingSetMB > Spec.MaxRssMB)
	{
		if (OverCapSinceMs < 0)
		{
			OverCapSinceMs = Now;
		}
		else if (Now - OverCapSinceMs >= RssOverCapMs)
		{
			OverCapSinceMs = -1;
			FLspEvent Event;
			Event.Kind = FLspEvent::EKind::RssOverCap;
			Event.Text = FString::Printf(TEXT("working set %llu MB above %u MB for 60 s"), static_cast<unsigned long long>(WorkingSetMB), Spec.MaxRssMB);
			PostEvent(MoveTemp(Event));
		}
	}
	else
	{
		OverCapSinceMs = -1;
	}
#else
	(void)Now;
#endif
}

uint64 FLspTransport::GetProcessCreationTime(uint32 ProcessId)
{
#if PLATFORM_WINDOWS
	HANDLE Handle = ProcessId != 0 ? ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, ProcessId) : nullptr;
	if (Handle == nullptr)
	{
		return 0;
	}
	FILETIME Created = {}, Exited = {}, Kernel = {}, User = {};
	const uint64 Result = ::GetProcessTimes(Handle, &Created, &Exited, &Kernel, &User)
		? (static_cast<uint64>(Created.dwHighDateTime) << 32) | Created.dwLowDateTime : 0;
	::CloseHandle(Handle);
	return Result;
#else
	(void)ProcessId;
	return 0;
#endif
}

bool FLspTransport::IsEscapeCandidate(uint64 CandidateCreated, uint64 ServerCreated)
{
	return CandidateCreated != 0 && ServerCreated != 0 && CandidateCreated >= ServerCreated;
}

int32 FLspTransport::ScanEscapedChildren(uint32 ServerPid, uint64 ServerCreated, bool bTerminate, TArray<FEscapedChild>& Out)
{
	int32 Found = 0;
#if PLATFORM_WINDOWS
	// Without a job nothing is contained, so nothing can have escaped it: never terminate on IsProcessInJob alone.
	if (ServerPid == 0 || ServerCreated == 0 || !FDreamChildJob::Exists())
	{
		return 0;
	}
	HANDLE Snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (Snapshot == INVALID_HANDLE_VALUE)
	{
		return 0;
	}
	PROCESSENTRY32W Entry = {};
	Entry.dwSize = sizeof(Entry);
	for (BOOL bMore = ::Process32FirstW(Snapshot, &Entry); bMore; bMore = ::Process32NextW(Snapshot, &Entry))
	{
		if (Entry.th32ParentProcessID != ServerPid || Entry.th32ProcessID == ServerPid)
		{
			continue;
		}
		HANDLE Child = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | (bTerminate ? PROCESS_TERMINATE : 0), 0, Entry.th32ProcessID);
		if (Child == nullptr)
		{
			continue;
		}
		// The times and the termination use this one handle, so a PID that dies and is reused in between is never hit.
		FILETIME Created = {}, Exited = {}, Kernel = {}, User = {};
		const uint64 ChildCreated = ::GetProcessTimes(Child, &Created, &Exited, &Kernel, &User)
			? (static_cast<uint64>(Created.dwHighDateTime) << 32) | Created.dwLowDateTime : 0;
		if (IsEscapeCandidate(ChildCreated, ServerCreated) && !FDreamChildJob::IsProcessInJob(Child))
		{
			++Found;
			if (bTerminate)
			{
				::TerminateProcess(Child, 1);
			}
			Out.Add(FEscapedChild{ Entry.th32ProcessID, FString(Entry.szExeFile) });
		}
		::CloseHandle(Child);
	}
	::CloseHandle(Snapshot);
#else
	(void)ServerPid;
	(void)ServerCreated;
	(void)bTerminate;
	(void)Out;
#endif
	return Found;
}

void FLspTransport::CheckEscapes()
{
	bEscapeChecked = true;
	// §7.1.3 escape check: a process the server started must be in our job; one that escaped is terminated. Only processes
	// created after the server count (lang-M1: PIDs are reused, and an old process may still record it as its creator).
	const uint32 ServerPid = Pid.load();
	TArray<FEscapedChild> Escaped;
	ScanEscapedChildren(ServerPid, GetProcessCreationTime(ServerPid), /*bTerminate*/ true, Escaped);
	for (const FEscapedChild& Child : Escaped)
	{
		FLspEvent Event;
		Event.Kind = FLspEvent::EKind::EscapedChild;
		Event.Pid = Child.Pid;
		Event.Text = FString::Printf(TEXT("process %u (%s) started by %s escaped the job and was terminated"), Child.Pid, *Child.Executable,
			*Spec.ServerId);
		PostEvent(MoveTemp(Event));
	}
}

uint32 FLspTransport::Run()
{
	Log = MakeUnique<FLog>(Spec.StderrLogPath);
	const int64 Started = NowMs();
	Log->WriteLine("[dream] starting " + DreamText::ToUtf8(Spec.Executable) + " " + DreamText::ToUtf8(Spec.Arguments), Started);
	if (!Spawn())
	{
		const FString Why = FString::Printf(TEXT("could not start %s"), *Spec.Executable);
		Log->WriteLine("[dream] " + DreamText::ToUtf8(Why), Started);
		FLspEvent Failed;
		Failed.Kind = FLspEvent::EKind::SpawnFailed;
		Failed.Text = Why;
		PostEvent(MoveTemp(Failed));
		CloseHandles();
		State = EDreamTransportState::Failed;
		return 1;
	}
	State = EDreamTransportState::Running;
	SpawnedMs = NowMs();
	LastBytesMs = SpawnedMs;
	{
		FLspEvent Spawned;
		Spawned.Kind = FLspEvent::EKind::Spawned;
		Spawned.Pid = Pid.load();
		PostEvent(MoveTemp(Spawned));
	}

	Writer = MakeUnique<FWriter>(StdInWrite);
	FRunnableThread* WriteThread = FRunnableThread::Create(Writer.Get(), *FString::Printf(TEXT("Dream.Lsp.%s.Write"), *Spec.ServerId), 0,
		TPri_BelowNormal);
	Io = MakeUnique<FIo>(*Writer);
	Options.Configuration = [this](std::string_view ScopeUri, std::string_view Section) -> dream::json::Value
	{
		if (Section != "dream")
		{
			return dream::json::Value();
		}
		const auto It = ConfigByUri.find(std::string(ScopeUri));
		return It != ConfigByUri.end() ? It->second : dream::json::Value();
	};
	Client = MakeUnique<dream::LspClient>(*Io, Options);

	dream::JsonRpc::FrameDecoder Decoder;
	std::vector<dream::json::Value> Frames;
	TArray<uint8> Buffer;
	int32 ExitCode = 0;
	bool bExited = false;
	bool bKill = false;
	enum class EStopPhase : uint8 { None, WaitAnswer, WaitExit };
	EStopPhase StopPhase = EStopPhase::None;
	int64 StopDeadline = 0;

	auto ReadOutput = [&](int64 Now) -> bool
	{
		bool bAny = false;
		Buffer.Reset();
		if (FPlatformProcess::ReadPipeToArray(StdOutRead, Buffer) && Buffer.Num() > 0)
		{
			bAny = true;
			LastBytesMs = Now;
			Client->NoteInput(Now);
			Frames.clear();
			const dream::JsonRpc::FrameError Error = Decoder.Feed(std::span<const uint8_t>(Buffer.GetData(), Buffer.Num()), Frames);
			for (dream::json::Value& Frame : Frames)
			{
				Client->OnFrame(MoveTemp(Frame), Now);
			}
			if (Error != dream::JsonRpc::FrameError::None)
			{
				FLspEvent Event;
				Event.Kind = FLspEvent::EKind::ProtocolError;
				Event.Text = FString::Printf(TEXT("frame error %hs"), dream::JsonRpc::FrameErrorName(Error));
				Log->WriteLine(std::string("[dream] frame error ") + dream::JsonRpc::FrameErrorName(Error), Now);
				PostEvent(MoveTemp(Event));
				bKill = true;
			}
		}
		Buffer.Reset();
		if (FPlatformProcess::ReadPipeToArray(StdErrRead, Buffer) && Buffer.Num() > 0)
		{
			bAny = true;
			Log->WriteBytes(Buffer.GetData(), Buffer.Num(), Now);
		}
		return bAny;
	};

	Client->Start(Spec.ParentPid, Spec.RootUri, Spec.WorkspaceFolders, NowMs());
	PostClientEvents();

	while (!bKill)
	{
		const int64 Now = NowMs();
		const bool bGotBytes = ReadOutput(Now);
		if (bKill)
		{
			break;
		}
		FLspCommand Command;
		while (CommandQueue.Dequeue(Command))
		{
			ApplyCommand(Command, Now);
		}
		Client->Tick(Now);
		PostClientEvents();
		if (Io->IsOverCap())
		{
			FLspEvent Event;
			Event.Kind = FLspEvent::EKind::ProtocolError;
			Event.Text = TEXT("more than 32 MB waiting for the server's stdin");
			Log->WriteLine("[dream] outbound cap exceeded", Now);
			PostEvent(MoveTemp(Event));
			break;
		}
		CheckMemory(Now);
		if (!bEscapeChecked && Now - SpawnedMs >= DreamLspTransportLocal::EscapeCheckDelayMs)
		{
			CheckEscapes();
		}
		if (!FPlatformProcess::IsProcRunning(Process))
		{
			ReadOutput(NowMs());   // what the child wrote before it exited
			PostClientEvents();
			FPlatformProcess::GetProcReturnCode(Process, &ExitCode);
			bExited = true;
			break;
		}
		if (bStopRequested.load())
		{
			const int64 Grace = GraceMs.load();
			if (StopPhase == EStopPhase::None)
			{
				StopPhase = EStopPhase::WaitAnswer;
				StopDeadline = Now + Grace;
				if (!Client->Shutdown(Now))
				{
					StopDeadline = Now;     // never initialized: straight to exit and the kill
				}
			}
			else if (StopPhase == EStopPhase::WaitAnswer && (Client->IsShutdownAnswered() || Now >= StopDeadline))
			{
				if (!Client->IsShutdownAnswered())
				{
					// shutdown was ignored or never sent: send exit ourselves, then wait once more (§7.1.5).
					Io->SendFrame(dream::JsonRpc::EncodeFrame(dream::JsonRpc::MakeNotification("exit", dream::json::Value())));
				}
				StopPhase = EStopPhase::WaitExit;
				StopDeadline = Now + Grace;
			}
			else if (StopPhase == EStopPhase::WaitExit && Now >= StopDeadline)
			{
				break;
			}
		}
		const bool bBusy = bGotBytes || Client->PendingRequestCount() > 0 || Now - LastBytesMs < DreamLspTransportLocal::BusyWindowMs ||
			bStopRequested.load();
		WakeRead->Wait(bBusy ? 1 : 10);
	}

	if (!bExited && Process.IsValid())
	{
		FPlatformProcess::TerminateProc(Process, /*KillTree*/ true);
		const double Until = FPlatformTime::Seconds() + 1.0;
		while (FPlatformProcess::IsProcRunning(Process) && FPlatformTime::Seconds() < Until)
		{
			FPlatformProcess::Sleep(0.005f);
		}
		ExitCode = -1;
		FPlatformProcess::GetProcReturnCode(Process, &ExitCode);
	}
	Client->OnExited();
	PostClientEvents();
	Log->WriteLine("[dream] exited with code " + std::to_string(ExitCode), NowMs());
	Log->Flush();
	{
		FLspEvent Exited;
		Exited.Kind = FLspEvent::EKind::Exited;
		Exited.ExitCode = ExitCode;
		PostEvent(MoveTemp(Exited));
	}
	if (WriteThread != nullptr)
	{
		Writer->Stop();
		WriteThread->WaitForCompletion();
		delete WriteThread;
	}
	CloseHandles();
	State = EDreamTransportState::Exited;
	return 0;
}
