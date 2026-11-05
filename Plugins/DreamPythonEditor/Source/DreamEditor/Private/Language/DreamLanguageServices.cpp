// SPDX-License-Identifier: Apache-2.0
// FDreamLanguageServices (TDD §7.3, §7.8). See DreamLanguageServices.h.
#include "Language/DreamLanguageServices.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "CodeView/DreamEditGroup.h"
#include "CodeView/DreamUndoHistory.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Language/DreamLspTransport.h"
#include "Language/DreamStubSnapshot.h"
#include "Language/DreamSyntaxCheck.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Output/DreamRuntimeProblems.h"
#include "Persistence/DreamInstances.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamRestoreGuard.h"
#include "Python/DreamPython.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Run/DreamRunTypes.h"
#include "Shell/SDreamTab.h"
#include "dream/JsonRpc.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamLanguageLocal
{
	constexpr int64 StopGraceMs = 500;              // §7.1.5: shutdown <= 500 ms, exit <= 500 ms, then the kill
	constexpr int64 ConfigCheckMs = 1000;
	constexpr int32 DrainBatch = 64;

	FString SanitizeName(const FString& Name)
	{
		FString Out;
		for (const TCHAR Char : Name)
		{
			Out.AppendChar(FChar::IsAlnum(Char) || Char == TEXT('-') || Char == TEXT('_') ? Char : TEXT('_'));
		}
		return Out.IsEmpty() ? FString(TEXT("Untitled")) : Out;
	}

	FString PluginDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()) : FString();
	}

	FString EnginePython()
	{
#if PLATFORM_WINDOWS
		return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
#elif PLATFORM_MAC
		return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Mac/bin/python3"));
#else
		return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Linux/bin/python3"));
#endif
	}

	FString Quote(const FString& Path)
	{
		return FString::Printf(TEXT("\"%s\""), *Path);
	}
}

FDreamLanguageServices::FDreamLanguageServices()
	: Snapshot(MakeUnique<FDreamStubSnapshot>())
{
}

FDreamLanguageServices::~FDreamLanguageServices()
{
	Shutdown();
	Problems.UnbindRuntime();
}

int64 FDreamLanguageServices::NowMs()
{
	return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
}

void FDreamLanguageServices::Initialize(FDreamRunService& InRuns, FDreamPython& InPython, FDreamBufferManager& InBuffers,
	FDreamRestoreGuard& InGuard, FDreamRuntimeProblems& InRuntime, FDreamJournal& InJournal, bool bInHeadless)
{
	Runs = &InRuns;
	Python = &InPython;
	Buffers = &InBuffers;
	Guard = &InGuard;
	Journal = &InJournal;
	bHeadless = bInHeadless;
	Health = bHeadless ? EDreamLanguageHealth::Disabled : EDreamLanguageHealth::Deferred;

	Problems.BindRuntime(&InRuntime);
	Snapshot->Initialize(InPython);
	SnapshotHandle = Snapshot->OnComplete().AddRaw(this, &FDreamLanguageServices::HandleSnapshotComplete);

	BufferOpenedHandle = Buffers->OnBufferOpened().AddLambda([this](const TSharedRef<FDreamBuffer>& Buffer)
	{
		if (bStarted && !bStopping)
		{
			AddDocument(Buffer);
		}
		if (!bHeadless && !bStopping)
		{
			TrackSyntaxCheck(Buffer);   // (lang) PO-06 F4: needs no server, so from the first buffer on
		}
	});
	BufferClosedHandle = Buffers->OnBufferClosed().AddLambda([this](const TSharedRef<FDreamBuffer>& Buffer)
	{
		RemoveDocument(Buffer->GetId());
		UntrackSyntaxCheck(Buffer->GetId());     // (lang)
		Problems.ClearBuffer(Buffer->GetId());
	});
	if (!bHeadless)
	{
		for (const TSharedRef<FDreamBuffer>& Buffer : Buffers->GetOpenBuffers())     // (lang) buffers opened before the bindings
		{
			TrackSyntaxCheck(Buffer);
		}
	}
	RunStateHandle = Runs->OnRunStateChanged().AddRaw(this, &FDreamLanguageServices::HandleRunStateChanged);
	RunFinishedHandle = Runs->OnRunFinished().AddRaw(this, &FDreamLanguageServices::HandleRunFinished);
	SafeModeHandle = Guard->OnSafeModeChanged().AddRaw(this, &FDreamLanguageServices::HandleSafeModeChanged);
}

void FDreamLanguageServices::OnDeferredStart()
{
	check(IsInGameThread());
	if (bHeadless || bStopping)
	{
		return;
	}
	bDeferredStartSeen = true;
	if (Guard != nullptr && Guard->IsSafeMode())
	{
		Health = EDreamLanguageHealth::Disabled;     // §10.8: off in safe mode; Leave safe mode starts it
		return;
	}
	if (!bStarted)
	{
		Begin();
	}
	else
	{
		Snapshot->Start();                          // a Python restart: the walk resumes if it was not complete
	}
}

void FDreamLanguageServices::Begin()
{
	bStarted = true;
	bServerWanted = true;
	Health = EDreamLanguageHealth::Starting;
	LogPath = ResolveLogPath();     // shown before the first spawn; each spawn resolves it again (the role may be known by then)
	for (const TSharedRef<FDreamBuffer>& Buffer : Buffers->GetOpenBuffers())
	{
		AddDocument(Buffer);
	}
	// §7.4.2: typing in the REPL prompt is a Dream keystroke too (the prompt is no server document, §7.2.3).
	{
		const TSharedRef<FDreamBuffer> Repl = Buffers->GetReplInputBuffer();
		ReplInput = Repl;
		ReplInputHandle = Repl->OnChanged().AddLambda([this](const dream::ChangeSet&) { Snapshot->NoteKeystroke(); });
	}
	Snapshot->Start();      // the key first (a worker); dream-check spawns once it is known (--data, --key)
	if (!TickHandle.IsValid())
	{
		TickHandle = FDreamDeferral::AddTicker(TEXT("DreamLanguageServices"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
			/*bNeedsPython*/ false, [this](float DeltaTime) { return Tick(DeltaTime); });
	}
}

void FDreamLanguageServices::StartServer()
{
	using namespace DreamLanguageLocal;
	const FString Plugin = PluginDir();
	const FString Launcher = Plugin / TEXT("Tools/Python/dream_launch.py");
	const uint32 EditorPid = FPlatformProcess::GetCurrentProcessId();

	FDreamLaunchSpec Spec;
	Spec.ServerId = DreamCheckId;
	Spec.Executable = EnginePython();
	// §7.2.5: python.exe -I <plugin>/Tools/Python/dream_launch.py check --data <cache>/<key>/snapshot --parent-pid <pid>.
	Spec.Arguments = FString::Printf(TEXT("-I -B %s check --parent-pid %u"), *Quote(Launcher), EditorPid);
	if (Snapshot->IsKeyReady())
	{
		Spec.Arguments += FString::Printf(TEXT(" --data %s --key %s"), *Quote(Snapshot->GetSnapshotDir()), *Snapshot->GetKey());
	}
	Spec.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	LogPath = ResolveLogPath();     // at spawn time: the journal's role decides between <root> and <sec> (§10.5)
	Spec.StderrLogPath = LogPath;
	Spec.ParentPid = static_cast<int64>(EditorPid);
	const std::string ProjectUri = dream::JsonRpc::PathToFileUri(DreamText::ToUtf8(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())));
	Spec.RootUri = ProjectUri;
	Spec.WorkspaceFolders.push_back(dream::LspWorkspaceFolder{ ProjectUri, DreamText::ToUtf8(FApp::GetProjectName()) });

	dream::LspClientOptions Options;
	Options.ServerId = DreamText::ToUtf8(Spec.ServerId);
	Options.ClientName = "Dream";
	if (const TSharedPtr<IPlugin> Descriptor = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor")))
	{
		Options.ClientVersion = DreamText::ToUtf8(Descriptor->GetDescriptor().VersionName);
	}
	Options.bSendSkippedNotification = true;    // §7.1.5: dream/skipped instead of a buffer over 5 MB (SUM902)

	if (Plugin.IsEmpty() || !FPaths::FileExists(Spec.Executable) || !FPaths::FileExists(Launcher))
	{
		UE_LOG(LogDream, Warning, TEXT("Language services: dream-check cannot start (%s or %s is missing)."), *Spec.Executable, *Launcher);
		HandleCrash(TEXT("missing executable"), NowMs());
		return;
	}

	Transport = FLspTransport::Start(Spec, MoveTemp(Options));
	Health = RestartCount > 0 ? EDreamLanguageHealth::Restarting : EDreamLanguageHealth::Starting;
	// Every open document goes to the new client; it sends them once the server is Ready (§7.3.5 "After a restart").
	for (TPair<FGuid, FDoc>& Pair : Docs)
	{
		if (const TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin())
		{
			Pair.Value.ConfigKey.clear();
			SyncConfiguration(Pair.Value, *Buffer, /*bNotify*/ false);
			OpenOnServer(Pair.Value, Buffer.ToSharedRef());
		}
	}
}

void FDreamLanguageServices::SetRunActive(bool bActive)
{
	bRunActive = bActive;
}

void FDreamLanguageServices::Shutdown()
{
	if (bStopping)
	{
		return;
	}
	bStopping = true;
	FDreamDeferral::RemoveTicker(TickHandle);
	TickHandle.Reset();
	FDreamDeferral::RemoveTicker(SyntaxTickHandle);     // (lang)
	SyntaxTickHandle.Reset();
	for (TPair<FGuid, FSyntaxCheck>& Pair : SyntaxChecks)
	{
		if (const TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin())
		{
			Buffer->OnChanged().Remove(Pair.Value.ChangedHandle);
		}
	}
	SyntaxChecks.Reset();
	StopServers(DreamLanguageLocal::StopGraceMs);
	FailPendingActions();
	if (Snapshot.IsValid())
	{
		Snapshot->OnComplete().Remove(SnapshotHandle);
		Snapshot->Shutdown();
	}
	for (TPair<FGuid, FDoc>& Pair : Docs)
	{
		if (const TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin())
		{
			Buffer->OnChanged().Remove(Pair.Value.ChangedHandle);
			Buffer->OnStateChanged().Remove(Pair.Value.StateHandle);
		}
	}
	Docs.Reset();
	BufferByDoc.Reset();
	if (const TSharedPtr<FDreamBuffer> Repl = ReplInput.Pin())
	{
		Repl->OnChanged().Remove(ReplInputHandle);
	}
	ReplInput.Reset();
	if (Buffers != nullptr)
	{
		Buffers->OnBufferOpened().Remove(BufferOpenedHandle);
		Buffers->OnBufferClosed().Remove(BufferClosedHandle);
	}
	if (Runs != nullptr)
	{
		Runs->OnRunStateChanged().Remove(RunStateHandle);
		Runs->OnRunFinished().Remove(RunFinishedHandle);
	}
	if (Guard != nullptr)
	{
		Guard->OnSafeModeChanged().Remove(SafeModeHandle);
	}
	Buffers = nullptr;
	Runs = nullptr;
	Guard = nullptr;
	if (Health != EDreamLanguageHealth::Disabled)
	{
		Health = EDreamLanguageHealth::Stopped;
	}
}

void FDreamLanguageServices::StopServers(int64 GraceMs)
{
	// §7.1.5: all children stop in parallel; then the job closes last (FDreamChildJob::Close, the kill-on-close backstop).
	if (Transport.IsValid())
	{
		Transport->RequestStop(FTimespan::FromMilliseconds(static_cast<double>(GraceMs)));
		Dying.Add(MoveTemp(Transport));
	}
	for (TUniquePtr<FLspTransport>& Old : Dying)
	{
		Old->RequestStop(FTimespan::FromMilliseconds(static_cast<double>(GraceMs)));
	}
	ReapDying(/*bWait*/ true);
	FDreamChildJob::Close();
}

void FDreamLanguageServices::ReapDying(bool bWait)
{
	for (int32 Index = Dying.Num() - 1; Index >= 0; --Index)
	{
		const EDreamTransportState State = Dying[Index]->GetState();
		if (bWait || State == EDreamTransportState::Exited || State == EDreamTransportState::Failed)
		{
			Dying[Index]->Join();
			Dying.RemoveAt(Index);
		}
	}
}

void FDreamLanguageServices::RestartServers()
{
	if (!bStarted || bStopping)
	{
		return;
	}
	if (Transport.IsValid())
	{
		Transport->RequestStop(FTimespan::Zero());
		Dying.Add(MoveTemp(Transport));
	}
	FailPendingActions();       // their server is gone: every waiting code-action request gets its empty answer
	Backoff.Reset();
	RestartCount = 0;
	NextSpawnMs = 0;
	bServerWanted = true;
	Health = EDreamLanguageHealth::Starting;
}

void FDreamLanguageServices::SetServerExcludedForTest(const FGuid& BufferId, bool bExcluded)
{
	if (bExcluded)
	{
		ServerExcludedForTest.Add(BufferId);
		RemoveDocument(BufferId);
		Problems.ReplaceSource(BufferId, EDreamProblemSource::DreamCheck, {});
		Problems.ForgetAnalysed(BufferId, EDreamProblemSource::DreamCheck);
		return;
	}
	ServerExcludedForTest.Remove(BufferId);
	const TSharedPtr<FDreamBuffer> Buffer = Buffers != nullptr ? Buffers->FindById(BufferId) : nullptr;
	if (bStarted && !bStopping && Buffer.IsValid())
	{
		AddDocument(Buffer.ToSharedRef());
	}
}

void FDreamLanguageServices::KillServerForTest()
{
	const uint32 Pid = GetServerPid();
	if (Pid == 0)
	{
		return;
	}
	FProcHandle Handle = FPlatformProcess::OpenProcess(Pid);
	if (Handle.IsValid())
	{
		FPlatformProcess::TerminateProc(Handle, /*KillTree*/ true);
		FPlatformProcess::CloseProc(Handle);
	}
}

uint32 FDreamLanguageServices::GetServerPid() const
{
	return Transport.IsValid() ? Transport->GetPid() : 0;
}

FText FDreamLanguageServices::GetHealthText() const
{
	switch (Health)
	{
	case EDreamLanguageHealth::Deferred: return LOCTEXT("Lang.HealthDeferred", "Lint: starts when you begin editing");
	case EDreamLanguageHealth::Disabled:
		return bHeadless ? LOCTEXT("Lang.HealthHeadless", "Lint: off in a headless editor")
			: LOCTEXT("Lang.HealthSafeMode", "Lint: off in safe mode");
	case EDreamLanguageHealth::Starting: return LOCTEXT("Lang.HealthStarting", "Lint: starting");
	case EDreamLanguageHealth::Ready: return LOCTEXT("Lang.HealthReady", "Lint: ready");
	case EDreamLanguageHealth::Restarting:
		return FText::Format(LOCTEXT("Lang.HealthRestarting", "Lint: restarting ({0})"), FText::AsNumber(RestartCount));
	case EDreamLanguageHealth::Paused:
		return LOCTEXT("Lang.HealthPaused", "Language features paused: dream-check stopped 5 times in 3 minutes");
	case EDreamLanguageHealth::Stopped: return LOCTEXT("Lang.HealthStopped", "Lint: stopped");
	}
	return FText::GetEmpty();
}

FString FDreamLanguageServices::MakeLogPath(const FDreamJournal& InJournal, const FString& ServerId)
{
	if (!InJournal.IsOpen())
	{
		return FString();
	}
	// TEL-01 and the §10.5 table: a secondary instance stages its files under <sec> (journal/i-<pid>-<start>/); the primary
	// (in main, or in compat-<format> after §10.6) logs under the root.
	const FString NamespaceDir = InJournal.GetNamespaceDir();
	const FString Base = InJournal.GetRole() == EDreamInstanceRole::Secondary && !NamespaceDir.IsEmpty() ? NamespaceDir
		: InJournal.GetStorageRoot().Root;
	return Base / TEXT("Logs") / TEXT("LSP") / (ServerId + TEXT(".log"));
}

FString FDreamLanguageServices::ResolveLogPath()
{
	if (Journal != nullptr && Journal->IsOpen())
	{
		return MakeLogPath(*Journal, DreamCheckId);
	}
	// The journal is not open (before the first tab spawn): the root this session resolved at engine init (an override, the
	// platform default or the temp fallback). None (a strict -DreamStorageRoot that failed, review r2): no log file at all,
	// never the platform default; the transport still drains stderr.
	if (FDreamEditorModule::IsAvailable())
	{
		const TOptional<FDreamStorageRoot>& Root = FDreamEditorModule::Get().GetInstances().GetResolvedRoot();
		if (Root.IsSet() && !Root->Root.IsEmpty())
		{
			return Root->Root / TEXT("Logs") / TEXT("LSP") / (FString(DreamCheckId) + TEXT(".log"));
		}
	}
	return FString();
}

// ---- Documents ------------------------------------------------------------------------------------------------------------

bool FDreamLanguageServices::IsEligible(const FDreamBuffer& Buffer) const
{
	// The REPL prompt's diagnostics are never shown (§7.2.3); M1 does not open it at all.
	return Buffer.GetKind() != EDreamBufferKind::ReplInput;
}

std::string FDreamLanguageServices::MakeUri(const FDreamBuffer& Buffer) const
{
	if (Buffer.HasPath())
	{
		return dream::JsonRpc::PathToFileUri(DreamText::ToUtf8(Buffer.GetPath()));
	}
	// §7.2.3: a scratch tab is a virtual path that is never written, so imports resolve from the project.
	const FString Virtual = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()) / TEXT("Dream") / TEXT("Scratch") /
		FString::Printf(TEXT("%s-%s.py"), *DreamLanguageLocal::SanitizeName(Buffer.GetDisplayName()), *Buffer.GetIdHex().Left(8));
	return dream::JsonRpc::PathToFileUri(DreamText::ToUtf8(Virtual));
}

dream::json::Value FDreamLanguageServices::MakeConfiguration(const FDreamBuffer& Buffer) const
{
	// §7.2.4: dream-check asks for section "dream" and gets {oneUndoStepPerRun, bufferKind, namespaceMode}.
	const FDreamBufferViewState& View = Buffer.GetViewState();
	dream::json::Value Config = dream::json::Value::MakeObject();
	Config.Set("oneUndoStepPerRun", dream::json::Value::MakeBool(FDreamRunService::ResolveTabTransaction(View, EDreamRunRoute::Buffer)));
	Config.Set("bufferKind", dream::json::Value::MakeString(Buffer.GetKind() == EDreamBufferKind::File ? "file" : "scratch"));
	Config.Set("namespaceMode", dream::json::Value::MakeString(View.Namespace == EDreamNamespaceMode::Isolated ? "isolated" : "session"));
	return Config;
}

void FDreamLanguageServices::SyncConfiguration(FDoc& Doc, const FDreamBuffer& Buffer, bool bNotify)
{
	dream::json::Value Config = MakeConfiguration(Buffer);
	std::string Serialized = dream::json::Serialize(Config);
	if (Serialized == Doc.ConfigKey)
	{
		return;
	}
	Doc.ConfigKey = MoveTemp(Serialized);
	if (!Transport.IsValid())
	{
		return;
	}
	FLspCommand Set;
	Set.Kind = FLspCommand::EKind::SetConfiguration;
	Set.Uri = Doc.Uri;
	Set.Params = MoveTemp(Config);
	Transport->Post(MoveTemp(Set));
	if (bNotify)
	{
		FLspCommand Notify;
		Notify.Kind = FLspCommand::EKind::Notify;
		Notify.Method = "workspace/didChangeConfiguration";
		Notify.Params = dream::json::Value::MakeObject();
		Notify.Params.Set("settings", dream::json::Value::MakeNull());
		Transport->Post(MoveTemp(Notify));
	}
}

void FDreamLanguageServices::AddDocument(const TSharedRef<FDreamBuffer>& Buffer)
{
	if (!IsEligible(*Buffer) || Docs.Contains(Buffer->GetId()) || ServerExcludedForTest.Contains(Buffer->GetId()))
	{
		return;
	}
	const FGuid BufferId = Buffer->GetId();
	FDoc& Doc = Docs.Add(BufferId);
	Doc.Id = NextDocId++;
	Doc.Buffer = Buffer;
	Doc.Uri = MakeUri(*Buffer);
	BufferByDoc.Add(Doc.Id, BufferId);
	Doc.ChangedHandle = Buffer->OnChanged().AddLambda([this, BufferId](const dream::ChangeSet& Changes)
	{
		HandleBufferChanged(BufferId, Changes);
	});
	Doc.StateHandle = Buffer->OnStateChanged().AddLambda([this, BufferId]() { HandleBufferStateChanged(BufferId); });
	if (Transport.IsValid())
	{
		SyncConfiguration(Doc, *Buffer, /*bNotify*/ false);
		OpenOnServer(Doc, Buffer);
	}
}

void FDreamLanguageServices::OpenOnServer(const FDoc& Doc, const TSharedRef<FDreamBuffer>& Buffer)
{
	FLspCommand Open;
	Open.Kind = FLspCommand::EKind::Open;
	Open.Doc = Doc.Id;
	Open.Uri = Doc.Uri;
	Open.LanguageId = "python";
	Open.Text = Buffer->Snapshot();
	Transport->Post(MoveTemp(Open));
}

void FDreamLanguageServices::RemoveDocument(const FGuid& BufferId)
{
	FDoc Doc;
	if (!Docs.RemoveAndCopyValue(BufferId, Doc))
	{
		return;
	}
	BufferByDoc.Remove(Doc.Id);
	if (const TSharedPtr<FDreamBuffer> Buffer = Doc.Buffer.Pin())
	{
		Buffer->OnChanged().Remove(Doc.ChangedHandle);
		Buffer->OnStateChanged().Remove(Doc.StateHandle);
	}
	if (Transport.IsValid())
	{
		FLspCommand Close;
		Close.Kind = FLspCommand::EKind::Close;
		Close.Doc = Doc.Id;
		Transport->Post(MoveTemp(Close));
	}
}

bool FDreamLanguageServices::IsDocumentOpen(const FGuid& BufferId) const
{
	return Transport.IsValid() && Docs.Contains(BufferId);
}

void FDreamLanguageServices::HandleBufferChanged(const FGuid& BufferId, const dream::ChangeSet& Changes)
{
	Snapshot->NoteKeystroke();
	Problems.OnBufferEdited(BufferId, Changes);
	FDoc* Doc = Docs.Find(BufferId);
	const TSharedPtr<FDreamBuffer> Buffer = Doc != nullptr ? Doc->Buffer.Pin() : nullptr;
	if (Doc == nullptr || !Buffer.IsValid() || !Transport.IsValid() || Changes.IsEmpty())
	{
		return;
	}
	// §7.2.3: the edits and the snapshot right after them; the read thread converts columns with its line index.
	FLspCommand Change;
	Change.Kind = FLspCommand::EKind::Change;
	Change.Doc = Doc->Id;
	Change.Changes.push_back(dream::LspDocChange{ Changes, Buffer->Snapshot() });
	Transport->Post(MoveTemp(Change));
}

void FDreamLanguageServices::HandleBufferStateChanged(const FGuid& BufferId)
{
	FDoc* Doc = Docs.Find(BufferId);
	const TSharedPtr<FDreamBuffer> Buffer = Doc != nullptr ? Doc->Buffer.Pin() : nullptr;
	if (Doc == nullptr || !Buffer.IsValid())
	{
		return;
	}
	const std::string Uri = MakeUri(*Buffer);
	if (Uri != Doc->Uri)
	{
		// Save As or a scratch tab saved to a file: the document is closed under its old URI and opened under the new one.
		const TSharedRef<FDreamBuffer> Ref = Buffer.ToSharedRef();
		RemoveDocument(BufferId);
		Problems.ReplaceSource(BufferId, EDreamProblemSource::DreamCheck, {});
		Problems.ForgetAnalysed(BufferId, EDreamProblemSource::DreamCheck);   // (lang) review lang-q1-m2
		AddDocument(Ref);
		return;
	}
	SyncConfiguration(*Doc, *Buffer, /*bNotify*/ true);
}

// ---- Tick, events and supervision -----------------------------------------------------------------------------------------

bool FDreamLanguageServices::Tick(float /*DeltaTime*/)
{
	// RT-01, §7.3.6: results apply only while no run is on the stack (FDreamDeferral defers this body then, too).
	if ((Runs != nullptr && Runs->IsRunActive()) || bRunActive)
	{
		return true;
	}
	if (bStopping)
	{
		return false;
	}
	const double Began = FPlatformTime::Seconds();
	const int64 Now = NowMs();
	ReapDying(/*bWait*/ false);

	// Spawn once the snapshot key is known (dream-check gets --data and --key) and no backoff is pending.
	if (bServerWanted && !Transport.IsValid() && Health != EDreamLanguageHealth::Paused && Now >= NextSpawnMs && Snapshot->IsKeyReady())
	{
		StartServer();
	}

	if (Transport.IsValid())
	{
		TArray<FLspEvent> Events;
		while (Transport.IsValid() && FPlatformTime::Seconds() - Began < DrainBudgetSeconds)
		{
			Events.Reset();
			if (Transport->Drain(Events, DreamLanguageLocal::DrainBatch) == 0)
			{
				break;
			}
			for (FLspEvent& Event : Events)
			{
				HandleEvent(Event, Now);
				if (!Transport.IsValid())
				{
					break;      // a crash dropped the rest of this server's events with it
				}
			}
		}
	}

	if (Health == EDreamLanguageHealth::Ready && Now - LastHealthyCheckMs >= 1000)
	{
		LastHealthyCheckMs = Now;
		Backoff.OnHealthy(Now);
		if (Backoff.RestartCount() == 0)
		{
			RestartCount = 0;   // 300 s of healthy uptime reset the backoff, so "restarting (n)" starts again from 1
		}
	}
	if (Now - LastConfigCheckMs >= DreamLanguageLocal::ConfigCheckMs)
	{
		// The tab's toggles (one undo step per run, Session or Isolated) carry no event; a 1 s check sends what changed.
		LastConfigCheckMs = Now;
		for (TPair<FGuid, FDoc>& Pair : Docs)
		{
			if (const TSharedPtr<FDreamBuffer> Buffer = Pair.Value.Buffer.Pin())
			{
				SyncConfiguration(Pair.Value, *Buffer, /*bNotify*/ true);
			}
		}
	}
	Problems.ExpireStale(FPlatformTime::Seconds());
	return true;
}

void FDreamLanguageServices::HandleEvent(FLspEvent& Event, int64 Now)
{
	switch (Event.Kind)
	{
	case FLspEvent::EKind::Spawned:
		UE_LOG(LogDream, Log, TEXT("Language services: dream-check started (pid %u)."), Event.Pid);
		return;
	case FLspEvent::EKind::SpawnFailed:
		HandleCrash(FString::Printf(TEXT("spawn failed: %s"), *Event.Text), Now);
		return;
	case FLspEvent::EKind::ProtocolError:
		HandleCrash(FString::Printf(TEXT("protocol error: %s"), *Event.Text), Now);
		return;
	case FLspEvent::EKind::RssOverCap:
		HandleCrash(FString::Printf(TEXT("restarted: %s"), *Event.Text), Now);
		return;
	case FLspEvent::EKind::EscapedChild:
		UE_LOG(LogDream, Warning, TEXT("Language services: %s."), *Event.Text);
		return;
	case FLspEvent::EKind::Exited:
		HandleCrash(FString::Printf(TEXT("exited with code %d"), Event.ExitCode), Now);
		return;
	case FLspEvent::EKind::Client:
		break;
	}

	dream::LspEvent& Lsp = Event.Lsp;
	if (Event.Tag != 0)
	{
		FPendingActions Pending;
		if (PendingActions.RemoveAndCopyValue(Event.Tag, Pending) && Pending.OnDone)
		{
			TArray<dream::LspCodeAction> Actions;
			if (Lsp.Kind == dream::LspEventKind::Response)
			{
				for (dream::LspCodeAction& Action : Lsp.Actions)
				{
					if (!Action.bTouchesOtherDocuments && !Action.Edits.empty())
					{
						Actions.Add(MoveTemp(Action));
					}
				}
			}
			Pending.OnDone(MoveTemp(Actions));
		}
		return;
	}
	switch (Lsp.Kind)
	{
	case dream::LspEventKind::StateChanged:
		if (Lsp.State == dream::LspState::Ready)
		{
			Health = EDreamLanguageHealth::Ready;
			Backoff.OnReady(Now);
			LastHealthyCheckMs = Now;
			UE_LOG(LogDream, Log, TEXT("Language services: dream-check ready."));
		}
		else if (Lsp.State == dream::LspState::Stalled)
		{
			HandleCrash(TEXT("stalled"), Now);   // §7.2.3: three timeouts and 10 s of silence count as a crash
		}
		return;
	case dream::LspEventKind::Diagnostics:
		ApplyDiagnostics(Lsp);
		return;
	case dream::LspEventKind::ProtocolError:
		HandleCrash(FString::Printf(TEXT("protocol error: %hs"), Lsp.Text.c_str()), Now);
		return;
	case dream::LspEventKind::ApplyEdit:
		if (!Lsp.Actions.empty())
		{
			FText Error;
			ApplyCodeAction(Lsp.Actions[0], Error);
		}
		return;
	default:
		return;         // Log lines went to the server's log file; untagged answers have no owner
	}
}

void FDreamLanguageServices::HandleCrash(const FString& Why, int64 Now)
{
	if (bStopping)
	{
		return;
	}
	UE_LOG(LogDream, Log, TEXT("Language services: dream-check %s."), *Why);
	if (Transport.IsValid())
	{
		Transport->RequestStop(FTimespan::Zero());
		Dying.Add(MoveTemp(Transport));
	}
	FailPendingActions();
	// §7.3.5: the old diagnostics stay, greyed, until the first new publish or for at most 10 s.
	Problems.MarkSourceStale(EDreamProblemSource::DreamCheck, FPlatformTime::Seconds());
	const dream::LspBackoff::Decision Decision = Backoff.OnCrash(Now);
	++RestartCount;
	if (Decision.bPaused)
	{
		Health = EDreamLanguageHealth::Paused;
		UE_LOG(LogDream, Warning, TEXT("Language features paused: dream-check stopped %d times within 3 minutes (log: %s)."),
			dream::LspBackoff::PauseCrashCount, *LogPath);
		return;
	}
	Health = EDreamLanguageHealth::Restarting;
	NextSpawnMs = Now + Decision.DelayMs;
}

void FDreamLanguageServices::FailPendingActions()
{
	TMap<uint64, FPendingActions> Pending = MoveTemp(PendingActions);
	PendingActions.Reset();
	for (TPair<uint64, FPendingActions>& Pair : Pending)
	{
		if (Pair.Value.OnDone)
		{
			Pair.Value.OnDone(TArray<dream::LspCodeAction>());
		}
	}
}

void FDreamLanguageServices::ApplyDiagnostics(const dream::LspEvent& Event)
{
	const FGuid* BufferId = BufferByDoc.Find(Event.Doc);
	const TSharedPtr<FDreamBuffer> Buffer = BufferId != nullptr && Buffers != nullptr ? Buffers->FindById(*BufferId) : nullptr;
	if (!Buffer.IsValid())
	{
		return;
	}
	// §7.2.3 "Diagnostic versions": an older set is mapped forward through the edit log; beyond the log it is dropped. The
	// game thread checks again here because edits kept coming while the event waited (§7.3.6).
	const dream::TextBuffer& Model = Buffer->GetModel();
	const uint64 Current = Model.Version();
	if (Event.Version > Current || (Event.Version < Current && Event.Version < Model.OldestMappableVersion()))
	{
		return;
	}
	TArray<FDreamProblem> Items;
	Items.Reserve(static_cast<int32>(Event.Diagnostics.size()));
	for (const dream::LspDiagnostic& Diagnostic : Event.Diagnostics)
	{
		dream::LspRange Range = Diagnostic.Range;
		if (Event.Version != Current && !dream::MapRangeForward(Model, Event.Version, Range))
		{
			continue;       // its line was deleted since
		}
		FDreamProblem Item;
		Item.Severity = FDreamProblemsModel::SeverityFromLsp(Diagnostic.Severity);
		Item.Code = DreamText::Utf8ToFString(Diagnostic.Code);
		Item.Message = DreamText::Utf8ToFString(Diagnostic.Message);
		// §7.8.2: "syntax" for a syntax error (dream-check's code "syntax-error"), otherwise "<Source>:<Code>", which never merges.
		Item.DedupClass = Diagnostic.Code == "syntax-error" ? FString(TEXT("syntax")) : FString::Printf(TEXT("dream-check:%s"), *Item.Code);
		Item.StartLine = Range.StartLine;
		Item.StartColumn = Range.StartCol;
		Item.EndLine = Range.EndLine;
		Item.EndColumn = Range.EndCol;
		Item.DocVersion = Current;
		Item.bHasQuickFix = Diagnostic.bFixable;
		Items.Add(MoveTemp(Item));
	}
	// (lang) PO-06 F4: the publish's version is dream-check's syntax verdict; one broadcast for both.
	Problems.ReplaceAnalysed(*BufferId, EDreamProblemSource::DreamCheck, Event.Version, MoveTemp(Items));
}

void FDreamLanguageServices::HandleSnapshotComplete(const FString& SnapshotDir, const FString& Key)
{
	// §7.3.3, §7.4.6 step 4: dream-check reloads its Unreal data.
	if (!Transport.IsValid())
	{
		return;
	}
	FLspCommand Notify;
	Notify.Kind = FLspCommand::EKind::Notify;
	Notify.Method = "dream/snapshotChanged";
	Notify.Params = dream::json::Value::MakeObject();
	Notify.Params.Set("path", dream::json::Value::MakeString(DreamText::ToUtf8(SnapshotDir)));
	Notify.Params.Set("key", dream::json::Value::MakeString(DreamText::ToUtf8(Key)));
	Transport->Post(MoveTemp(Notify));
}

void FDreamLanguageServices::HandleSafeModeChanged()
{
	if (Guard == nullptr || bStopping)
	{
		return;
	}
	if (Guard->IsSafeMode())
	{
		if (Transport.IsValid())
		{
			StopServers(DreamLanguageLocal::StopGraceMs);
		}
		bServerWanted = false;
		Health = EDreamLanguageHealth::Disabled;
		return;
	}
	// §10.8 Leaving safe mode: the deferred services start now (the module asks Python to start; its boot calls us again).
	if (bDeferredStartSeen)
	{
		if (!bStarted)
		{
			Begin();
		}
		else
		{
			bServerWanted = true;
			Health = EDreamLanguageHealth::Starting;
		}
	}
}

// ---- (lang) PO-06 F4: the in-process syntax check ------------------------------------------------------------------------

void FDreamLanguageServices::TrackSyntaxCheck(const TSharedRef<FDreamBuffer>& Buffer)
{
	if (!IsEligible(*Buffer) || SyntaxChecks.Contains(Buffer->GetId()))
	{
		return;
	}
	const FGuid BufferId = Buffer->GetId();
	FSyntaxCheck& Check = SyntaxChecks.Add(BufferId);
	Check.Buffer = Buffer;
	Check.DueSeconds = FPlatformTime::Seconds();     // a file opened with an error shows it at once
	Check.ChangedHandle = Buffer->OnChanged().AddLambda([this, BufferId](const dream::ChangeSet& Changes)
	{
		if (FSyntaxCheck* Found = SyntaxChecks.Find(BufferId); Found != nullptr && !Changes.IsEmpty())
		{
			Found->DueSeconds = FPlatformTime::Seconds() + SyntaxCheckDelaySeconds;   // INT-07: 300 ms after the last edit
		}
	});
	if (!SyntaxTickHandle.IsValid())
	{
		SyntaxTickHandle = FDreamDeferral::AddTicker(TEXT("DreamSyntaxCheck"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
			/*bNeedsPython*/ false, [this](float DeltaTime) { return TickSyntaxChecks(DeltaTime); });
	}
}

void FDreamLanguageServices::UntrackSyntaxCheck(const FGuid& BufferId)
{
	FSyntaxCheck Check;
	if (!SyntaxChecks.RemoveAndCopyValue(BufferId, Check))
	{
		return;
	}
	if (const TSharedPtr<FDreamBuffer> Buffer = Check.Buffer.Pin())
	{
		Buffer->OnChanged().Remove(Check.ChangedHandle);
	}
}

bool FDreamLanguageServices::TickSyntaxChecks(float /*DeltaTime*/, bool bAll)
{
	// RT-01: like every result, the check's items apply only while no run is on the stack.
	if ((Runs != nullptr && Runs->IsRunActive()) || bRunActive)
	{
		return true;
	}
	if (bStopping)
	{
		return false;
	}
	const double Now = FPlatformTime::Seconds();
	auto Apply = [this](const FGuid& BufferId, FSyntaxCheck& Check)
	{
		Check.bInFlight = false;
		FSyntaxResult Result = Check.Task.GetResult();
		Check.Task = {};
		const TSharedPtr<FDreamBuffer> Buffer = Check.Buffer.Pin();
		// Applied by version: a result for older text is dropped (the edit that made it old scheduled the next check).
		if (Buffer.IsValid() && Result.bChecked && Buffer->GetModel().Version() == Check.TaskVersion)
		{
			Problems.ReplaceAnalysed(BufferId, EDreamProblemSource::Syntax, Check.TaskVersion, MoveTemp(Result.Items));
		}
	};
	bool bLaunched = false;
	for (TPair<FGuid, FSyntaxCheck>& Pair : SyntaxChecks)
	{
		FSyntaxCheck& Check = Pair.Value;
		if (Check.bInFlight && (bAll || Check.Task.IsCompleted()))
		{
			Check.Task.Wait();
			Apply(Pair.Key, Check);
		}
		if (Check.bInFlight || Check.DueSeconds <= 0.0 || (!bAll && (Now < Check.DueSeconds || bLaunched)))
		{
			continue;
		}
		const TSharedPtr<FDreamBuffer> Buffer = Check.Buffer.Pin();
		if (!Buffer.IsValid())
		{
			continue;
		}
		Check.DueSeconds = 0.0;
		const dream::TextSnapshot Text = Buffer->Snapshot();
		Check.TaskVersion = Text.Version();
		Check.bInFlight = true;
		Check.Task = UE::Tasks::Launch(TEXT("DreamSyntaxCheck"), [Text]()
		{
			FSyntaxResult Result;
			Result.bChecked = FDreamSyntaxCheck::Check(Text, Result.Items);
			return Result;
		});
		bLaunched = true;   // one new check per frame: a restore of many tabs spreads its workers
		if (bAll)
		{
			Check.Task.Wait();
			Apply(Pair.Key, Check);
		}
	}
	return true;
}

// ---- Parse items ----------------------------------------------------------------------------------------------------------

void FDreamLanguageServices::HandleRunStateChanged(const FDreamRunRecord& Record)
{
	// §7.8.1: a run of the buffer starts; its pre-run parse decides whether a Parse item comes back.
	if (Record.State == EDreamRunState::Starting && Record.BufferId.IsValid())
	{
		Problems.ClearParse(Record.BufferId);
	}
}

void FDreamLanguageServices::HandleRunFinished(const FDreamRunRecord& Record)
{
	// U7, RUN-16: a compile error leaves the run not executed with the error's buffer line in its Location.
	if (Record.Outcome != EDreamRunOutcome::NotExecuted || Record.Location.Line <= 0 || !Record.BufferId.IsValid() || Buffers == nullptr)
	{
		return;
	}
	const TSharedPtr<FDreamBuffer> Buffer = Buffers->FindById(Record.BufferId);
	if (!Buffer.IsValid() || !IsEligible(*Buffer))
	{
		return;
	}
	FDreamProblem Item;
	Item.Severity = EDreamSeverity::Error;
	FString Type;
	Item.Code = Record.Detail.Split(TEXT(": "), &Type, nullptr) ? Type : FString(TEXT("SyntaxError"));
	Item.Message = Record.Detail;
	Item.DedupClass = TEXT("syntax");
	Item.StartLine = Item.EndLine = Record.Location.Line - 1;
	Item.StartColumn = Item.EndColumn = FMath::Max(Record.Location.Column, 0);
	Item.DocVersion = Buffer->GetModel().Version();
	Item.RunNumber = static_cast<uint32>(Record.RunNumber);
	TArray<FDreamProblem> Items;
	Items.Add(MoveTemp(Item));
	Problems.ReplaceSource(Record.BufferId, EDreamProblemSource::Parse, MoveTemp(Items));
}

// ---- Code actions, the walk and the quick-fix menu ----------------------------------------------------------------------

bool FDreamLanguageServices::RequestCodeActions(const FGuid& BufferId, const dream::LspRange& Range, FDreamOnCodeActions OnDone)
{
	const FDoc* Doc = Docs.Find(BufferId);
	if (Doc == nullptr || !Transport.IsValid() || Health != EDreamLanguageHealth::Ready)
	{
		return false;
	}
	const uint64 Tag = NextTag++;
	PendingActions.Add(Tag, FPendingActions{ MoveTemp(OnDone) });
	FLspCommand Request;
	Request.Kind = FLspCommand::EKind::CodeActions;
	Request.Doc = Doc->Id;
	Request.Range = Range;
	Request.Tag = Tag;
	Transport->Post(MoveTemp(Request));
	return true;
}

bool FDreamLanguageServices::ApplyCodeAction(const dream::LspCodeAction& Action, FText& OutError)
{
	const FGuid* BufferId = BufferByDoc.Find(Action.Doc);
	const TSharedPtr<FDreamBuffer> Buffer = BufferId != nullptr && Buffers != nullptr ? Buffers->FindById(*BufferId) : nullptr;
	if (!Buffer.IsValid())
	{
		OutError = LOCTEXT("Lang.FixNoBuffer", "The tab of this fix is closed");
		return false;
	}
	if (Runs != nullptr && Runs->IsRunActive())
	{
		OutError = LOCTEXT("Lang.FixRunActive", "Run in progress");
		return false;
	}
	// §7.8.3: a fix computed for an older version is dropped (the user asks again on the new text).
	if (Action.bHasVersion && Action.Version != Buffer->GetModel().Version())
	{
		OutError = LOCTEXT("Lang.FixStale", "The text changed since this fix was computed; try again");
		return false;
	}
	FDreamEditGroup Group;
	Group.Label = FText::Format(LOCTEXT("Lang.FixLabel", "Quick fix: {0}"), FText::FromString(DreamText::Utf8ToFString(Action.Title)));
	for (const dream::LspTextEdit& Edit : Action.Edits)
	{
		FDreamTextEdit& Out = Group.Edits.AddDefaulted_GetRef();
		Out.StartLine = Edit.Range.StartLine;
		Out.StartColumn = Edit.Range.StartCol;
		Out.EndLine = Edit.Range.EndLine;
		Out.EndColumn = Edit.Range.EndCol;
		Out.Text = DreamText::ToFString(Edit.NewText);
	}
	// EDT-08: one undo step for the whole fix, through the view when the buffer has one (its selection follows).
	bool bApplied = false;
	const TSharedPtr<SDreamTab> Tab = FDreamEditorModule::IsAvailable() ? FDreamEditorModule::Get().GetTab() : nullptr;
	if (const TSharedPtr<SDreamCodeView> View = Tab.IsValid() ? Tab->FindCodeView(*BufferId) : nullptr)
	{
		bApplied = View->ApplyEditGroup(Group);
	}
	else
	{
		bApplied = FDreamUndoHistory::ApplyEditGroup(*Buffer, Group);
	}
	if (!bApplied)
	{
		OutError = Buffer->IsReadOnly() ? Buffer->GetReadOnlyReason() : LOCTEXT("Lang.FixFailed", "The fix could not be applied");
		return false;
	}
	if (Transport.IsValid())
	{
		FLspCommand UserAction;
		UserAction.Kind = FLspCommand::EKind::UserAction;     // §7.2.4: workspace/applyEdit is accepted for 5 s
		Transport->Post(MoveTemp(UserAction));
	}
	return true;
}

void FDreamLanguageServices::ShowCodeActionMenu(const TSharedRef<SWidget>& Anchor, const FVector2D& ScreenPosition,
	TArray<dream::LspCodeAction>&& Actions)
{
	if (!FSlateApplication::IsInitialized() || Actions.IsEmpty())
	{
		return;
	}
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	Menu.BeginSection(NAME_None, LOCTEXT("Lang.FixMenu", "Quick fixes"));
	for (dream::LspCodeAction& Action : Actions)
	{
		const FText Title = FText::FromString(DreamText::Utf8ToFString(Action.Title));
		Menu.AddMenuEntry(Title, Action.bPreferred ? LOCTEXT("Lang.FixPreferred", "Preferred fix") : FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Action = MoveTemp(Action)]()
			{
				if (!FDreamEditorModule::IsAvailable())
				{
					return;
				}
				FDreamEditorModule& Module = FDreamEditorModule::Get();
				FText Error;
				if (!Module.GetLanguageServices().ApplyCodeAction(Action, Error))
				{
					if (const TSharedPtr<SDreamTab> Tab = Module.GetTab())
					{
						Tab->ShowStatusMessage(Error);
					}
				}
			})));
	}
	Menu.EndSection();
	FSlateApplication::Get().PushMenu(Anchor, FWidgetPath(), Menu.MakeWidget(), ScreenPosition, FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
}

void FDreamLanguageServices::ShowQuickFixes(SDreamTab& Tab)
{
	const TSharedPtr<FDreamBuffer> Buffer = Tab.GetActiveBuffer();
	const TSharedPtr<SDreamCodeView> View = Tab.GetActiveCodeView();
	if (!Buffer.IsValid() || !View.IsValid())
	{
		Tab.ShowStatusMessage(LOCTEXT("Lang.FixNoTab", "No tab is open"));
		return;
	}
	// The caret line: dream-check answers with the fixes of every diagnostic that touches it, then "Suppress SUMxxx".
	const FDreamTextSelection Selection = View->GetSelection();
	const int32 Line = Selection.CaretLine;
	const dream::TextSnapshot Text = Buffer->Snapshot();
	dream::LspRange Range;
	Range.StartLine = Range.EndLine = Line;
	Range.StartCol = 0;
	Range.EndCol = static_cast<int32>(Text.LineEnd(Line) - Text.LineStart(Line));
	const FGuid BufferId = Buffer->GetId();
	const uint64 Version = Buffer->GetModel().Version();
	const TWeakPtr<SDreamTab> WeakTab = StaticCastSharedRef<SDreamTab>(Tab.AsShared());
	const bool bAsked = RequestCodeActions(BufferId, Range, [this, WeakTab, BufferId, Version](TArray<dream::LspCodeAction>&& Actions)
	{
		const TSharedPtr<SDreamTab> Pinned = WeakTab.Pin();
		const TSharedPtr<FDreamBuffer> Active = Pinned.IsValid() ? Pinned->GetActiveBuffer() : nullptr;
		const TSharedPtr<SDreamCodeView> ActiveView = Pinned.IsValid() ? Pinned->GetActiveCodeView() : nullptr;
		if (!Active.IsValid() || !ActiveView.IsValid() || Active->GetId() != BufferId || Active->GetModel().Version() != Version)
		{
			return;     // the user moved on; the menu would point at another text
		}
		if (Actions.IsEmpty())
		{
			Pinned->ShowStatusMessage(LOCTEXT("Lang.NoFixes", "No quick fixes on this line"));
			return;
		}
		const FSlateRect Caret = ActiveView->GetCaretScreenRect();
		ShowCodeActionMenu(ActiveView.ToSharedRef(), FVector2D(Caret.Left, Caret.Bottom), MoveTemp(Actions));
	});
	if (!bAsked)
	{
		Tab.ShowStatusMessage(FText::Format(LOCTEXT("Lang.FixUnavailable", "Quick fixes are unavailable now ({0})"), GetHealthText()));
	}
}

bool FDreamLanguageServices::OpenProblem(SDreamTab& Tab, const FDreamProblem& Problem)
{
	if (Problem.Buffer.IsValid() && Buffers != nullptr && Buffers->FindById(Problem.Buffer).IsValid())
	{
		Tab.ActivateBuffer(Problem.Buffer);
		if (const TSharedPtr<SDreamCodeView> View = Tab.FindCodeView(Problem.Buffer))
		{
			View->SetCaret(Problem.StartLine, Problem.StartColumn);
			View->ScrollToLine(Problem.StartLine);
			View->FocusEditor();
			return true;
		}
	}
	if (!Problem.File.IsEmpty() && FDreamEditorModule::IsAvailable())
	{
		return !FDreamEditorModule::Get().OpenLocation(Problem.File, Problem.StartLine + 1, Problem.StartColumn).IsEmpty();
	}
	return false;
}

void FDreamLanguageServices::WalkProblems(SDreamTab& Tab, int32 Direction)
{
	const TArray<FDreamProblem> Visible = Problems.GetVisible();
	if (Visible.IsEmpty())
	{
		Tab.ShowStatusMessage(LOCTEXT("Lang.NoProblems", "No problems"));
		return;
	}
	FGuid BufferId;
	int32 Line = -1;
	int32 Column = -1;
	if (const TSharedPtr<FDreamBuffer> Buffer = Tab.GetActiveBuffer())
	{
		BufferId = Buffer->GetId();
		if (const TSharedPtr<SDreamCodeView> View = Tab.GetActiveCodeView())
		{
			const FDreamTextSelection Selection = View->GetSelection();
			Line = Selection.CaretLine;
			Column = Selection.CaretColumn;
		}
	}
	const int32 Index = FDreamProblemsModel::FindNeighbour(Visible, BufferId, Line, Column, Direction > 0, LastWalkIndex);
	if (!Visible.IsValidIndex(Index))
	{
		return;
	}
	LastWalkIndex = Index;
	const FDreamProblem& Target = Visible[Index];
	if (!OpenProblem(Tab, Target))
	{
		Tab.ShowStatusMessage(FText::Format(LOCTEXT("Lang.ProblemUnresolved", "Cannot open {0}"), FText::FromString(Target.File)));
		return;
	}
	Tab.ShowStatusMessage(FText::Format(LOCTEXT("Lang.ProblemWalk", "Problem {0} of {1}: {2}"), FText::AsNumber(Index + 1),
		FText::AsNumber(Visible.Num()), FText::FromString(Target.Message)));
}

void FDreamLanguageServices::WalkProblemsInBuffer(SDreamTab& Tab, int32 Direction)
{
	// (lang) PO-06 F4: PyCharm's F2 / Shift+F2 stay in the file; F8 / Shift+F8 keep following the Problems list (U13).
	const TSharedPtr<FDreamBuffer> Buffer = Tab.GetActiveBuffer();
	const TSharedPtr<SDreamCodeView> View = Tab.GetActiveCodeView();
	if (!Buffer.IsValid() || !View.IsValid())
	{
		Tab.ShowStatusMessage(LOCTEXT("Lang.FixNoTab", "No tab is open"));
		return;
	}
	TArray<FDreamProblem> Items = Problems.GetForBuffer(Buffer->GetId());
	// Errors and warnings only, as PyCharm's F2 (review lang-q1 nit).
	Items.RemoveAll([](const FDreamProblem& Item) { return Item.Severity == EDreamSeverity::Hint || Item.Severity == EDreamSeverity::Info; });
	if (Items.IsEmpty())
	{
		Tab.ShowStatusMessage(LOCTEXT("Lang.NoProblemsInFile", "No problems in this file"));
		return;
	}
	const FDreamTextSelection Selection = View->GetSelection();
	const int32 Line = Selection.CaretLine;
	const int32 Column = Selection.CaretColumn;
	int32 Target = INDEX_NONE;
	if (Direction > 0)
	{
		Target = Items.IndexOfByPredicate([Line, Column](const FDreamProblem& Item)
		{
			return Item.StartLine > Line || (Item.StartLine == Line && Item.StartColumn > Column);
		});
		Target = Target == INDEX_NONE ? 0 : Target;
	}
	else
	{
		for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
		{
			if (Items[Index].StartLine < Line || (Items[Index].StartLine == Line && Items[Index].StartColumn < Column))
			{
				Target = Index;
				break;
			}
		}
		Target = Target == INDEX_NONE ? Items.Num() - 1 : Target;
	}
	const FDreamProblem& Item = Items[Target];
	View->SetCaret(Item.StartLine, Item.StartColumn);
	View->ScrollToLine(Item.StartLine);
	View->FocusEditor();
	Tab.ShowStatusMessage(FText::Format(LOCTEXT("Lang.ProblemInFile", "{0} of {1}: {2}"), FText::AsNumber(Target + 1),
		FText::AsNumber(Items.Num()), FText::FromString(Item.Message)));
}

#undef LOCTEXT_NAMESPACE
