// SPDX-License-Identifier: Apache-2.0
// FLspTransport (TDD §7.1, D4, INT-11, TEL-01): one child process with three separate pipes, contained so it dies with the
// editor on every exit path (Win64: a kill-on-close Job Object), Content-Length framing without altering bytes, and queues
// so the game thread never touches a pipe. Two FRunnables per child (§7.1.4): Dream.Lsp.<id>.Read spawns the child, polls
// stdout and stderr, drains the command queue into its dream::LspClient and posts events; Dream.Lsp.<id>.Write writes frames
// until every byte is out, so a server that floods stdout without reading stdin cannot deadlock both processes.
// stderr and window/logMessage go to <storage root>/Logs/LSP/<serverId>.log, rotated at 10 MB with 3 generations, above
// 1 MB/s dropped behind a "[dream: N bytes dropped]" marker.
// Threads: Start, Post, Drain, RequestStop and the destructor on the game thread (Post on any thread); everything else on
// the two I/O threads. Body: DreamLspTransport.cpp (lang).
// Deferred (M2): spawn mode Suspended (CREATE_SUSPENDED with PROC_THREAD_ATTRIBUTE_JOB_LIST; M1's only child, dream-check,
// starts no grandchildren, and the 2 s escape check below kills any that escape), the manifest SHA-256 check of bundled
// binaries (ExpectedSha256), the Linux dream-spawn shim and the protocol trace file (LanguageServices.Trace).
#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"
#include "Misc/Timespan.h"
#include "dream/LspClient.h"

#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class FRunnableThread;
class FEvent;

enum class EDreamSpawnMode : uint8
{
	Engine,             // CreatePipe x3, CreateProc(FProcessStartInfo) with an explicit StdErr, then AssignProcessToJobObject
	Suspended,          // Win64, M2
	Shim,               // Linux, macOS (dream-spawn), 1.0 Preview / 1.2
};

enum class EDreamTransportMode : uint8
{
	Lsp,
	Batch,              // stubgen, export (M2): the same framing, no LspClient
};

enum class EDreamTransportState : uint8
{
	Spawning,
	Running,
	Exited,
	Failed,
};

struct FDreamLaunchSpec
{
	FString ServerId;
	FString Executable;
	FString Arguments;
	FString WorkingDirectory;
	FString StderrLogPath;
	EDreamSpawnMode SpawnMode = EDreamSpawnMode::Engine;
	EDreamTransportMode Mode = EDreamTransportMode::Lsp;
	TOptional<FString> ExpectedSha256;      // M2: manifest or policy (§7.3.4)
	/** §7.3.5 memory: a working set above this for 60 s posts RssOverCap (G3, D22). 0 disables the watch. */
	uint32 MaxRssMB = 1536;
	/** initialize: processId (the editor), rootUri and workspace folders. */
	int64 ParentPid = 0;
	std::string RootUri;
	std::vector<dream::LspWorkspaceFolder> WorkspaceFolders;
};

/** Game thread -> read thread. */
struct FLspCommand
{
	enum class EKind : uint8
	{
		Open,               // Doc, Uri, LanguageId, Text
		Change,             // Doc, Changes (one frame's edits)
		Close,              // Doc
		Notify,             // Method, Params
		CodeActions,        // Doc, Range, Context, Tag
		Request,            // Method, Doc, Version, Params, TimeoutMs, Tag
		Cancel,             // Tag
		UserAction,         // a code action the user picked (workspace/applyEdit window, §7.2.4)
		SetConfiguration,   // Uri, Params: the "dream" section for that scopeUri (workspace/configuration)
	};

	EKind Kind = EKind::Notify;
	dream::LspDocId Doc = 0;
	std::string Uri;
	std::string LanguageId;
	dream::TextSnapshot Text;
	std::vector<dream::LspDocChange> Changes;
	std::string Method;
	dream::json::Value Params;
	uint64 Version = 0;
	int64 TimeoutMs = 0;
	dream::LspRange Range;
	std::vector<dream::LspDiagnostic> Context;
	uint64 Tag = 0;                         // the caller's request tag, echoed on the answer's event
};

/** Read thread -> game thread. */
struct FLspEvent
{
	enum class EKind : uint8
	{
		Spawned,            // Pid
		SpawnFailed,        // Text
		Client,             // Lsp (Tag set for answers to tagged requests)
		ProtocolError,      // Text: a framing error, an outbound cap, or the client's own protocol error
		RssOverCap,         // Text: the working set stayed over MaxRssMB for 60 s
		EscapedChild,       // Text: a grandchild outside the job was found and terminated (§7.1.3 escape check)
		Exited,             // ExitCode
	};

	EKind Kind = EKind::Client;
	dream::LspEvent Lsp;
	uint64 Tag = 0;
	uint32 Pid = 0;
	int32 ExitCode = 0;
	FString Text;
};

/** §7.1.3: one kill-on-close Job Object per editor process; its handle stays open until module shutdown. Win64. */
class DREAMEDITOR_API FDreamChildJob
{
public:
	/** Creates the job on first use (any thread). Returns false where there is no job (other platforms, failure). */
	static bool AssignProcess(void* ProcessHandle);
	/** Creates the job if it does not exist yet; false where there is none (other platforms, failure). Any thread. */
	static bool EnsureCreated();
	/** True while the job exists (the escape check does nothing without one: nothing could be outside it). */
	static bool Exists();
	/** True when the process is in our job (escape check). */
	static bool IsProcessInJob(void* ProcessHandle);
	/** Module shutdown, after every transport stopped: closes the handle, which kills anything still inside. */
	static void Close();
};

class DREAMEDITOR_API FLspTransport final : public FRunnable
{
public:
	/** Game thread. Thread creation only (§7.1.5 deferred start <= 1 ms per server); the spawn runs on the read thread. */
	static TUniquePtr<FLspTransport> Start(const FDreamLaunchSpec& Spec, dream::LspClientOptions Options);
	virtual ~FLspTransport() override;

	/** Any thread. Non-blocking enqueue. */
	void Post(FLspCommand&& Command);
	/** Game thread only. Moves out at most MaxEvents events; returns the count. */
	int32 Drain(TArray<FLspEvent>& Out, int32 MaxEvents);
	/** shutdown -> wait <= Grace -> exit -> wait <= Grace -> kill the tree (§7.1.5). Non-blocking; Join waits. */
	void RequestStop(FTimespan Grace);
	/** Game thread: waits for the read thread to finish (after RequestStop it ends within about 2 x Grace). */
	void Join();

	uint32 GetPid() const { return Pid.load(); }
	EDreamTransportState GetState() const { return State.load(); }
	const FString& GetServerId() const { return Spec.ServerId; }

	virtual uint32 Run() override;
	virtual void Stop() override;

	/** §7.1.4 log rules (proposed): rotate at 10 MB, keep 3 generations, drop above 1 MB/s behind a marker. */
	static constexpr int64 LogRotateBytes = int64(10) << 20;
	static constexpr int32 LogGenerations = 3;
	static constexpr int64 LogMaxBytesPerSecond = int64(1) << 20;
	/** §7.1.4 outbound cap (proposed). */
	static constexpr int64 MaxPendingOutboundBytes = int64(32) << 20;

	/** A process the escape check found outside the job (§7.1.3). */
	struct FEscapedChild
	{
		uint32 Pid = 0;
		FString Executable;
	};
	/** A process's creation time as a FILETIME value (100 ns since 1601); 0 when it cannot be read. Win64; 0 elsewhere. */
	static uint64 GetProcessCreationTime(uint32 ProcessId);
	/**
	 * The PID-reuse guard of the escape check: Windows keeps a process's creator PID forever and reuses PIDs, so a process
	 * created before the server is never its child, whatever parent PID it records. False when either time is unknown.
	 */
	static bool IsEscapeCandidate(uint64 CandidateCreated, uint64 ServerCreated);
	/**
	 * §7.1.3 escape check: the processes whose recorded parent is ServerPid, created at or after ServerCreated, and not in
	 * the job. Nothing without a job. With bTerminate each is terminated through the handle its times were read from.
	 * Returns how many were found. Any thread (the read thread; tier 4 calls it with bTerminate false).
	 */
	static int32 ScanEscapedChildren(uint32 ServerPid, uint64 ServerCreated, bool bTerminate, TArray<FEscapedChild>& Out);

private:
	class FWriter;
	class FIo;
	class FLog;

	FLspTransport(const FDreamLaunchSpec& InSpec, dream::LspClientOptions InOptions);

	bool Spawn();
	void ApplyCommand(FLspCommand& Command, int64 NowMs);
	void PostEvent(FLspEvent&& Event);
	void PostClientEvents();
	void CheckMemory(int64 NowMs);
	void CheckEscapes();
	void CloseHandles();
	static int64 NowMs();

	FDreamLaunchSpec Spec;
	dream::LspClientOptions Options;

	std::atomic<EDreamTransportState> State{ EDreamTransportState::Spawning };
	std::atomic<uint32> Pid{ 0 };
	std::atomic<bool> bStopRequested{ false };
	std::atomic<int64> GraceMs{ 500 };

	FRunnableThread* ReadThread = nullptr;
	FEvent* WakeRead = nullptr;

	TQueue<FLspCommand, EQueueMode::Mpsc> CommandQueue;
	TQueue<FLspEvent, EQueueMode::Spsc> EventQueue;

	// Read-thread state.
	FProcHandle Process;
	void* StdInRead = nullptr;
	void* StdInWrite = nullptr;
	void* StdOutRead = nullptr;
	void* StdOutWrite = nullptr;
	void* StdErrRead = nullptr;
	void* StdErrWrite = nullptr;
	TUniquePtr<FWriter> Writer;
	TUniquePtr<FIo> Io;
	TUniquePtr<dream::LspClient> Client;
	TUniquePtr<FLog> Log;
	TMap<dream::LspRequestId, uint64> TagsById;
	TMap<uint64, dream::LspRequestId> IdsByTag;
	int64 LastBytesMs = 0;
	int64 SpawnedMs = 0;
	int64 LastRssSampleMs = 0;
	int64 OverCapSinceMs = -1;
	bool bEscapeChecked = false;
	int64 LastCommitLogMs = 0;
	std::unordered_map<std::string, dream::json::Value> ConfigByUri;     // workspace/configuration answers, read thread
};
