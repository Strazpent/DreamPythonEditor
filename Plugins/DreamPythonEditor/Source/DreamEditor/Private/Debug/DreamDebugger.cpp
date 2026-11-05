// SPDX-License-Identifier: Apache-2.0
// FDreamDebugger (TDD §8.1-§8.4, DBG-01..DBG-05, DBG-08): breakpoints, their persistence and the DebugConfig, the C++ pause
// rules and the Paused window's modal loop. Debug group (PO-06 F5). Game thread; no Python here (rule 6).
#include "Debug/DreamDebugger.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Debug/SDreamPausedWindow.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Persistence/DreamJournal.h"
#include "Python/DreamPythonDebug.h"
#include "Misc/CoreDelegates.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Shell/DreamTheme.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/CoreMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tasks/Task.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectThreadContext.h"
#include "Widgets/SWindow.h"
#include "dream/AnchorSet.h"
#include "dream/TextBuffer.h"

#include <string>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamDebuggerPrivate
{
	/** The AnchorSet owner tag of breakpoint anchors (§4.1.3: "breakpoint, mark, fold, ..."). */
	constexpr uint32 BreakpointOwner = 0xB9EA0001u;
	constexpr int32 StoreVersion = 1;

	std::string ToUtf8(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return std::string(Utf8.Get(), Utf8.Length());
	}

	FString FromUtf8(const std::string& Text)
	{
		return FString(UTF8_TO_TCHAR(Text.c_str()));
	}

	bool IsPseudo(const FString& Name)
	{
		return Name.StartsWith(TEXT("<")) && Name.EndsWith(TEXT(">"));
	}

	/** Compile filenames compared as the runner's canonic() does: pseudo names exactly, paths by slashes and case. */
	FString NormalizeSource(const FString& Source)
	{
		if (IsPseudo(Source))
		{
			return Source;
		}
		FString Path = FPaths::ConvertRelativePathToFull(Source);
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Path;
	}

	bool SameSource(const FString& A, const FString& B)
	{
		if (IsPseudo(A) || IsPseudo(B))
		{
			return A.Equals(B, ESearchCase::CaseSensitive);
		}
		return NormalizeSource(A).Equals(NormalizeSource(B), ESearchCase::IgnoreCase);
	}

	FDreamEditorModule* Module()
	{
		return FDreamEditorModule::IsAvailable() ? &FDreamEditorModule::Get() : nullptr;
	}
}

FDreamDebugger& FDreamDebugger::Get()
{
	check(IsInGameThread());
	static FDreamDebugger Instance;
	return Instance;
}

// =====================================================================================================================
// Breakpoints (§8.2)
// =====================================================================================================================

FString FDreamDebugger::KeyOf(const FDreamBuffer& Buffer) const
{
	return Buffer.HasPath() ? DreamDebuggerPrivate::NormalizeSource(Buffer.GetPath()) : FString(TEXT("buffer:")) + Buffer.GetIdHex();
}

FDreamDebugger::FBufferMarks& FDreamDebugger::MarksOf(FDreamBuffer& Buffer)
{
	using namespace DreamDebuggerPrivate;
	EnsureHooks();
	FBufferMarks& Entry = Marks.FindOrAdd(Buffer.GetId());
	if (!Entry.bLoaded)
	{
		Entry.bLoaded = true;
		Entry.Key = KeyOf(Buffer);
		const FGuid Id = Buffer.GetId();
		// Review q1 M1: moved markers are saved after edits (debounced), on save and on close.
		Entry.ChangedHandle = Buffer.OnChanged().AddLambda([this, Id](const dream::ChangeSet&) { OnBufferEdited(Id); });
		Entry.StateHandle = Buffer.OnStateChanged().AddLambda([this, Id]() { OnBufferStateChanged(Id); });
		LoadStore();
		if (const TArray<int32>* Lines = Stored.Find(KeyOf(Buffer)))
		{
			dream::TextBuffer& Model = Buffer.GetModel();
			const dream::TextSnapshot Snapshot = Model.Snapshot();
			for (const int32 Line1 : *Lines)
			{
				if (Line1 >= 1 && Line1 <= static_cast<int32>(Snapshot.LineCount()))
				{
					Entry.Anchors.Add(Model.Anchors().Add(Snapshot.LineStart(Line1 - 1), dream::AnchorKind::LineStart,
						dream::AnchorGravity::After, dream::AnchorDeletePolicy::Collapse, BreakpointOwner));
				}
			}
		}
	}
	return Entry;
}

TArray<int32> FDreamDebugger::GetBreakpointLines(FDreamBuffer& Buffer)
{
	FBufferMarks& Entry = MarksOf(Buffer);
	const dream::TextBuffer& Model = Buffer.GetModel();
	const dream::TextSnapshot Snapshot = Model.Snapshot();
	TArray<int32> Lines;
	for (int32 Index = Entry.Anchors.Num() - 1; Index >= 0; --Index)
	{
		const dream::Offset At = Model.Anchors().Find(Entry.Anchors[Index]);
		if (At < 0)
		{
			Entry.Anchors.RemoveAt(Index);
			continue;
		}
		Lines.AddUnique(static_cast<int32>(Snapshot.LineOf(At)));
	}
	Lines.Sort();
	return Lines;
}

bool FDreamDebugger::HasBreakpoint(FDreamBuffer& Buffer, int32 Line)
{
	return GetBreakpointLines(Buffer).Contains(Line);
}

bool FDreamDebugger::ToggleBreakpoint(FDreamBuffer& Buffer, int32 Line)
{
	const bool bOn = !HasBreakpoint(Buffer, Line);
	SetBreakpoint(Buffer, Line, bOn);
	return bOn;
}

void FDreamDebugger::SetBreakpoint(FDreamBuffer& Buffer, int32 Line, bool bOn)
{
	using namespace DreamDebuggerPrivate;
	FBufferMarks& Entry = MarksOf(Buffer);
	dream::TextBuffer& Model = Buffer.GetModel();
	const dream::TextSnapshot Snapshot = Model.Snapshot();
	if (Line < 0 || Line >= static_cast<int32>(Snapshot.LineCount()))
	{
		return;
	}
	bool bHad = false;
	for (int32 Index = Entry.Anchors.Num() - 1; Index >= 0; --Index)
	{
		const dream::Offset At = Model.Anchors().Find(Entry.Anchors[Index]);
		if (At < 0 || static_cast<int32>(Snapshot.LineOf(At)) == Line)
		{
			bHad |= At >= 0;
			if (!bOn || At < 0)
			{
				Model.Anchors().Remove(Entry.Anchors[Index]);
				Entry.Anchors.RemoveAt(Index);
			}
		}
	}
	if (bOn && !bHad)
	{
		Entry.Anchors.Add(Model.Anchors().Add(Snapshot.LineStart(Line), dream::AnchorKind::LineStart, dream::AnchorGravity::After,
			dream::AnchorDeletePolicy::Collapse, BreakpointOwner));
	}

	TArray<int32> Lines = GetBreakpointLines(Buffer);
	TArray<int32> Lines1;
	for (const int32 L : Lines)
	{
		Lines1.Add(L + 1);
	}
	if (Lines1.Num() > 0)
	{
		Stored.Add(KeyOf(Buffer), Lines1);
	}
	else
	{
		Stored.Remove(KeyOf(Buffer));
	}
	SaveStore();
	if (bPaused)
	{
		if (FDreamEditorModule* M = Module())
		{
			SendBreakpointsForSource(M->GetBufferManager().GetCompileFilename(Buffer), Lines1);
		}
	}
	NotifyChanged();
}

void FDreamDebugger::ClearBreakpoints(FDreamBuffer& Buffer)
{
	for (const int32 Line : GetBreakpointLines(Buffer))
	{
		SetBreakpoint(Buffer, Line, false);
	}
}

TSharedPtr<FDreamBuffer> FDreamDebugger::FindBufferForSource(const FString& Source) const
{
	FDreamEditorModule* M = DreamDebuggerPrivate::Module();
	if (M == nullptr || Source.IsEmpty())
	{
		return nullptr;
	}
	FDreamBufferManager& Buffers = M->GetBufferManager();
	for (const TSharedRef<FDreamBuffer>& Buffer : Buffers.GetOpenBuffers())
	{
		if (DreamDebuggerPrivate::SameSource(Buffers.GetCompileFilename(*Buffer), Source))
		{
			return Buffer;
		}
	}
	return nullptr;
}

void FDreamDebugger::ToggleBreakpointAtSource(const FString& Source, int32 Line)
{
	using namespace DreamDebuggerPrivate;
	if (const TSharedPtr<FDreamBuffer> Buffer = FindBufferForSource(Source))
	{
		ToggleBreakpoint(*Buffer, Line - 1);
		return;
	}
	if (IsPseudo(Source) || Line <= 0)
	{
		return;     // an untitled tab that is no longer open has nothing to keep breakpoints on
	}
	LoadStore();
	TArray<int32>& Lines = Stored.FindOrAdd(NormalizeSource(Source));
	if (Lines.Remove(Line) == 0)
	{
		Lines.Add(Line);
		Lines.Sort();
	}
	const TArray<int32> Copy = Lines;
	if (Lines.Num() == 0)
	{
		Stored.Remove(NormalizeSource(Source));
	}
	SaveStore();
	if (bPaused)
	{
		SendBreakpointsForSource(Source, Copy);
	}
	NotifyChanged();
}

TArray<int32> FDreamDebugger::GetSourceBreakpointLines(const FString& Source)
{
	TArray<int32> Out;
	if (const TSharedPtr<FDreamBuffer> Buffer = FindBufferForSource(Source))
	{
		for (const int32 Line : GetBreakpointLines(*Buffer))
		{
			Out.Add(Line + 1);
		}
		return Out;
	}
	LoadStore();
	if (const TArray<int32>* Lines = Stored.Find(DreamDebuggerPrivate::NormalizeSource(Source)))
	{
		Out = *Lines;
	}
	return Out;
}

void FDreamDebugger::SendBreakpointsForSource(const FString& Source, const TArray<int32>& Lines1Based)
{
	using namespace dream::json;
	Value Args = Value::MakeObject();
	Value SourceValue = Value::MakeObject();
	SourceValue.Set("path", Value::MakeString(DreamDebuggerPrivate::ToUtf8(Source)));
	Args.Set("source", MoveTemp(SourceValue));
	Value List = Value::MakeArray();
	for (const int32 Line : Lines1Based)
	{
		Value Item = Value::MakeObject();
		Item.Set("line", Value::MakeInt(Line));
		List.Append(MoveTemp(Item));
	}
	Args.Set("breakpoints", MoveTemp(List));
	QueueRequest(TEXT("setBreakpoints"), MoveTemp(Args), nullptr);
}

void FDreamDebugger::NotifyChanged()
{
	BreakpointsChanged.Broadcast();
}

FString FDreamDebugger::BuildDebugConfigJson(FDreamRunId RunId, const FGuid& RunBufferId, const FString& RunFilename)
{
	using namespace dream::json;
	using namespace DreamDebuggerPrivate;
	Value Config = Value::MakeObject();
	Config.Set("run_id", Value::MakeUInt(RunId));
	Config.Set("break_on_uncaught", Value::MakeBool(true));
	Config.Set("just_my_code", Value::MakeBool(true));
	Config.Set("stop_on_entry", Value::MakeBool(false));
	Value List = Value::MakeArray();
	int64 NextId = 1;
	TSet<FString> Covered;
	auto AddSpec = [&List, &NextId](const FString& SourceKey, int32 Line1)
	{
		Value Spec = Value::MakeObject();
		Spec.Set("id", Value::MakeInt(NextId++));
		Spec.Set("source_key", Value::MakeString(ToUtf8(SourceKey)));
		Spec.Set("line", Value::MakeInt(Line1));
		Spec.Set("enabled", Value::MakeBool(true));
		List.Append(MoveTemp(Spec));
	};
	if (FDreamEditorModule* M = Module())
	{
		FDreamBufferManager& Buffers = M->GetBufferManager();
		for (const TSharedRef<FDreamBuffer>& Buffer : Buffers.GetOpenBuffers())
		{
			Covered.Add(KeyOf(*Buffer));
			const TArray<int32> Lines = GetBreakpointLines(*Buffer);
			if (Lines.Num() == 0)
			{
				continue;
			}
			const FString SourceKey = Buffer->GetId() == RunBufferId && !RunFilename.IsEmpty() ? RunFilename : Buffers.GetCompileFilename(*Buffer);
			for (const int32 Line : Lines)
			{
				AddSpec(SourceKey, Line + 1);
			}
		}
	}
	LoadStore();
	for (const TPair<FString, TArray<int32>>& Pair : Stored)
	{
		if (Covered.Contains(Pair.Key) || Pair.Key.StartsWith(TEXT("buffer:")))
		{
			continue;
		}
		for (const int32 Line : Pair.Value)
		{
			AddSpec(Pair.Key, Line);
		}
	}
	Config.Set("breakpoints", MoveTemp(List));
	SaveStore();    // the lines as they are at run start, edits since the last toggle included
	return FromUtf8(Serialize(Config));
}

// ---- Keeping the store current (review q1 M1) ------------------------------------------------------------------------

void FDreamDebugger::EnsureHooks()
{
	if (!PreExitHandle.IsValid())
	{
		PreExitHandle = FCoreDelegates::OnPreExit.AddRaw(this, &FDreamDebugger::OnPreExit);
	}
	FDreamEditorModule* M = DreamDebuggerPrivate::Module();
	if (M != nullptr && HookedManager != &M->GetBufferManager())
	{
		HookedManager = &M->GetBufferManager();
		ClosedHandle = M->GetBufferManager().OnBufferClosed().AddRaw(this, &FDreamDebugger::HandleBufferClosed);
	}
}

void FDreamDebugger::OnBufferEdited(const FGuid& Id)
{
	const FBufferMarks* Entry = Marks.Find(Id);
	if (Entry != nullptr && Entry->Anchors.Num() > 0)
	{
		ScheduleSave();
	}
}

void FDreamDebugger::OnBufferStateChanged(const FGuid& Id)
{
	FDreamEditorModule* M = DreamDebuggerPrivate::Module();
	FBufferMarks* Entry = Marks.Find(Id);
	if (M == nullptr || Entry == nullptr)
	{
		return;
	}
	const TSharedPtr<FDreamBuffer> Buffer = M->GetBufferManager().FindById(Id);
	if (!Buffer.IsValid() || Buffer->IsDirty())
	{
		return;
	}
	// Saved (or reloaded clean): the lines as they are now; a Save As moves them to the new path's key.
	const FString Key = KeyOf(*Buffer);
	if (Key != Entry->Key)
	{
		LoadStore();
		Stored.Remove(Entry->Key);
		Entry->Key = Key;
	}
	if (Entry->Anchors.Num() > 0 || Stored.Contains(Key))
	{
		SaveStore();
	}
}

void FDreamDebugger::HandleBufferClosed(const TSharedRef<FDreamBuffer>& Buffer)
{
	FBufferMarks* Entry = Marks.Find(Buffer->GetId());
	if (Entry == nullptr)
	{
		return;
	}
	LoadStore();
	const FString Key = KeyOf(*Buffer);
	TArray<int32> Lines1;
	for (const int32 Line : GetBreakpointLines(*Buffer))
	{
		Lines1.Add(Line + 1);
	}
	// A file keeps its breakpoints for the next open (BUF-07); a closed untitled tab is gone, and so are its (review q1 nit).
	if (Lines1.Num() > 0 && Buffer->HasPath())
	{
		Stored.Add(Key, Lines1);
	}
	else
	{
		Stored.Remove(Key);
	}
	Buffer->OnChanged().Remove(Entry->ChangedHandle);
	Buffer->OnStateChanged().Remove(Entry->StateHandle);
	for (const uint64 Anchor : Entry->Anchors)
	{
		Buffer->GetModel().Anchors().Remove(Anchor);
	}
	Marks.Remove(Buffer->GetId());
	SaveStore();
	NotifyChanged();
}

void FDreamDebugger::ScheduleSave()
{
	if (bSaveScheduled)
	{
		return;
	}
	bSaveScheduled = true;
	FDreamDeferral::AddTicker(TEXT("DreamBreakpointsSave"), EDreamDeferPolicy::DeferWhileExecuting, 2.0f, /*bNeedsPython=*/false,
		[this](float)
		{
			bSaveScheduled = false;
			if (FDreamEditorModule::IsAvailable())
			{
				SaveStore();
			}
			return false;
		});
}

void FDreamDebugger::OnPreExit()
{
	// The editor is exiting: the last write lands, then the lines as they are now, written here and now.
	if (PendingWrite.IsValid())
	{
		PendingWrite.Wait();
	}
	if (bStoreLoaded || Marks.Num() > 0)
	{
		SaveStoreImpl(/*bSync=*/true);
	}
	if (FDreamEditorModule* M = DreamDebuggerPrivate::Module())
	{
		if (HookedManager == &M->GetBufferManager())
		{
			M->GetBufferManager().OnBufferClosed().Remove(ClosedHandle);
		}
		for (const TSharedRef<FDreamBuffer>& Buffer : M->GetBufferManager().GetOpenBuffers())
		{
			if (const FBufferMarks* Entry = Marks.Find(Buffer->GetId()))
			{
				Buffer->OnChanged().Remove(Entry->ChangedHandle);
				Buffer->OnStateChanged().Remove(Entry->StateHandle);
			}
		}
	}
	// The static outlives the module (review q1 nit): nothing of this session survives into a module restart.
	FCoreDelegates::OnPreExit.Remove(PreExitHandle);
	PreExitHandle.Reset();
	ClosedHandle.Reset();
	HookedManager = nullptr;
	Marks.Reset();
	Stored.Reset();
	bStoreLoaded = false;
	bSaveScheduled = false;
	PausedTickForTest = nullptr;
	BreakpointsChanged.Clear();
}

// ---- Persistence (§8.2.6) -------------------------------------------------------------------------------------------

void FDreamDebugger::SetStorePathForTest(const FString& Path)
{
	if (PendingWrite.IsValid())
	{
		PendingWrite.Wait();
	}
	if (bStoreLoaded)
	{
		SaveStoreImpl(/*bSync=*/true);    // the store being left gets the lines as they are now
	}
	StorePathOverride = Path;
	// The open buffers forget the markers of the old store: anchors and listeners go with them.
	if (FDreamEditorModule* M = DreamDebuggerPrivate::Module())
	{
		for (const TSharedRef<FDreamBuffer>& Buffer : M->GetBufferManager().GetOpenBuffers())
		{
			if (const FBufferMarks* Entry = Marks.Find(Buffer->GetId()))
			{
				Buffer->OnChanged().Remove(Entry->ChangedHandle);
				Buffer->OnStateChanged().Remove(Entry->StateHandle);
				for (const uint64 Anchor : Entry->Anchors)
				{
					Buffer->GetModel().Anchors().Remove(Anchor);
				}
			}
		}
	}
	Stored.Reset();
	Marks.Reset();
	bStoreLoaded = false;
	NotifyChanged();
}

FString FDreamDebugger::GetStorePath() const
{
	if (!StorePathOverride.IsEmpty())
	{
		return StorePathOverride;
	}
	if (FDreamEditorModule* M = DreamDebuggerPrivate::Module())
	{
		const FDreamStorageRoot& Root = M->GetJournal().GetStorageRoot();
		if (!Root.Root.IsEmpty())
		{
			return Root.StateDir() / TEXT("breakpoints.json");
		}
	}
	return FPaths::ProjectSavedDir() / TEXT("Dream") / TEXT("breakpoints.json");
}

void FDreamDebugger::LoadStore()
{
	using namespace dream::json;
	if (bStoreLoaded)
	{
		return;
	}
	bStoreLoaded = true;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *GetStorePath()))
	{
		return;
	}
	Value Root;
	if (!Parse(DreamDebuggerPrivate::ToUtf8(Text), Root) || !Root.IsObject())
	{
		UE_LOG(LogDream, Warning, TEXT("Breakpoints file %s is not valid JSON; starting without saved breakpoints."), *GetStorePath());
		return;
	}
	const Value& Files = Root.Get("files");
	if (!Files.IsObject())
	{
		return;
	}
	for (const Value::Member& Member : Files.Members())
	{
		TArray<int32> Lines;
		for (size_t Index = 0; Index < Member.Val.Size(); ++Index)
		{
			const int64 Line = Member.Val.At(Index).AsInt(0);
			if (Line > 0 && Line < MAX_int32)
			{
				Lines.AddUnique(static_cast<int32>(Line));
			}
		}
		if (Lines.Num() > 0)
		{
			Lines.Sort();
			Stored.FindOrAdd(DreamDebuggerPrivate::FromUtf8(Member.Key)) = Lines;
		}
	}
}

void FDreamDebugger::SaveStore()
{
	SaveStoreImpl(/*bSync=*/false);
}

void FDreamDebugger::SaveStoreImpl(bool bSync)
{
	using namespace dream::json;
	LoadStore();
	// The open buffers' lines as they are now (they moved with edits since the last toggle).
	if (FDreamEditorModule* M = DreamDebuggerPrivate::Module())
	{
		for (const TSharedRef<FDreamBuffer>& Buffer : M->GetBufferManager().GetOpenBuffers())
		{
			const FBufferMarks* Entry = Marks.Find(Buffer->GetId());
			if (Entry == nullptr || !Entry->bLoaded)
			{
				continue;
			}
			TArray<int32> Lines1;
			for (const int32 Line : GetBreakpointLines(*Buffer))
			{
				Lines1.Add(Line + 1);
			}
			if (Lines1.Num() > 0)
			{
				Stored.Add(KeyOf(*Buffer), Lines1);
			}
			else
			{
				Stored.Remove(KeyOf(*Buffer));
			}
		}
	}
	Value Root = Value::MakeObject();
	Root.Set("version", Value::MakeInt(DreamDebuggerPrivate::StoreVersion));
	Value Files = Value::MakeObject();
	for (const TPair<FString, TArray<int32>>& Pair : Stored)
	{
		Value Lines = Value::MakeArray();
		for (const int32 Line : Pair.Value)
		{
			Lines.Append(Value::MakeInt(Line));
		}
		Files.Set(DreamDebuggerPrivate::ToUtf8(Pair.Key), MoveTemp(Lines));
	}
	Root.Set("files", MoveTemp(Files));
	const FString Text = DreamDebuggerPrivate::FromUtf8(Serialize(Root, /*bPretty=*/true));
	const FString Path = GetStorePath();
	// Off the game thread: a few hundred bytes, written whole through a temporary file and a move.
	auto Write = [Text, Path]()
	{
		const FString Temp = Path + TEXT(".tmp");
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		if (FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			IFileManager::Get().Move(*Path, *Temp, /*bReplace=*/true);
		}
	};
	if (bSync)
	{
		Write();
		return;
	}
	// In order, one after the other: the newest store lands last.
	PendingWrite = PendingWrite.IsValid()
		? UE::Tasks::Launch(UE_SOURCE_LOCATION, MoveTemp(Write), UE::Tasks::Prerequisites(PendingWrite))
		: UE::Tasks::Launch(UE_SOURCE_LOCATION, MoveTemp(Write));
}

void FDreamDebugger::FlushStoreForTest()
{
	if (PendingWrite.IsValid())
	{
		PendingWrite.Wait();
	}
	SaveStoreImpl(/*bSync=*/true);
}

// =====================================================================================================================
// The pause (§8.3, §8.4)
// =====================================================================================================================

EDreamPauseRefusal FDreamDebugger::CanPause(FDreamRunId RunId) const
{
	if (!IsInGameThread())
	{
		return EDreamPauseRefusal::NotGameThread;                        // rule 5
	}
	FDreamEditorModule* M = DreamDebuggerPrivate::Module();
	if (M == nullptr || RunId == 0 || M->GetRunService().GetActiveRunId() != RunId)
	{
		return EDreamPauseRefusal::NotDebugRun;
	}
	if (bPaused)
	{
		return EDreamPauseRefusal::AlreadyPaused;                        // rule 4
	}
	if (M->GetSupervisor().IsStopRequested(RunId))
	{
		return EDreamPauseRefusal::NotDebugRun;                          // review q1 nit: Stop came first; no pause, no notice
	}
	// (debug choice) FApp::IsUnattended() is also true for every automation test (GIsAutomationTesting, App.cpp), which would
	// refuse every G4 fixture step; the -unattended switch is what §8.4 rule 7 means.
	static const bool bUnattendedSwitch = FParse::Param(FCommandLine::Get(), TEXT("UNATTENDED"));
	if (GIsRunningUnattendedScript || bUnattendedSwitch)
	{
		return EDreamPauseRefusal::Unattended;                           // rule 7
	}
	if (!FSlateApplication::IsInitialized())
	{
		return EDreamPauseRefusal::WindowFailed;
	}
	if (FSlateApplication::Get().IsTicking())
	{
		return EDreamPauseRefusal::SlateTicking;                         // rule 6
	}
	if (UE::IsSavingPackage() || IsGarbageCollecting() || FUObjectThreadContext::Get().IsRoutingPostLoad)
	{
		return EDreamPauseRefusal::EngineBusy;                           // rule 8
	}
	if (M->GetSupervisor().GetPumpDepth() > 0)
	{
		return EDreamPauseRefusal::InsidePump;                           // rule 11
	}
	if (!FSlateApplication::Get().CanAddModalWindow())
	{
		return EDreamPauseRefusal::WindowFailed;
	}
	return EDreamPauseRefusal::None;
}

namespace DreamDebuggerPrivate
{
	void ParseStop(const FString& StopJson, FDreamDebugStop& Out)
	{
		using namespace dream::json;
		Value Root;
		if (!Parse(ToUtf8(StopJson), Root) || !Root.IsObject())
		{
			return;
		}
		Out.Reason = FromUtf8(Root.Get("reason").AsString());
		Out.Description = FromUtf8(Root.Get("description").AsString());
		Out.Source = FromUtf8(Root.Get("source").AsString());
		Out.Line = static_cast<int32>(Root.Get("line").AsInt(0));
		Out.bCanStep = Root.Get("canStep").AsBool(true);
		const Value& Frames = Root.Get("stackFrames");
		for (size_t Index = 0; Index < Frames.Size(); ++Index)
		{
			const Value& Item = Frames.At(Index);
			FDreamDebugFrame Frame;
			Frame.Id = static_cast<int32>(Item.Get("id").AsInt(static_cast<int64>(Index) + 1));
			Frame.Name = FromUtf8(Item.Get("name").AsString());
			Frame.Source = FromUtf8(Item.Get("source").AsString());
			Frame.Line = static_cast<int32>(Item.Get("line").AsInt(0));
			Out.Frames.Add(MoveTemp(Frame));
		}
	}
}

FDreamResume FDreamDebugger::RunPause(FDreamRunId RunId, const FString& StopJson)
{
	FDreamResume Out;
	Out.Refusal = CanPause(RunId);
	if (Out.Refusal != EDreamPauseRefusal::None)
	{
		return Out;
	}
	FDreamEditorModule& M = FDreamEditorModule::Get();
	Stop = FDreamDebugStop();
	DreamDebuggerPrivate::ParseStop(StopJson, Stop);
	Stop.RunId = RunId;
	if (const FDreamRunRecord* Record = M.GetRunService().GetActiveRecord())
	{
		Stop.RunNumber = Record->RunNumber;
	}
	Out.bInspectOnly = Stop.Reason == TEXT("exception");

	bPaused = true;
	bShown = false;
	bResumeSet = false;
	Resume = FDreamResume();
	Pending.Reset();
	++PauseCount;
	bClosing = false;
	// §8.3.1 step 2: the supervisor's paused gate is already up (_dream.pause raised it before releasing the GIL).
	NotifyChanged();

	FSlateApplication& App = FSlateApplication::Get();
	const TSharedRef<SDreamPausedWindow> Content = SNew(SDreamPausedWindow).Stop(Stop);
	const bool bFocus = FApp::HasFocus();
	const bool bHaveGeometry = SavedSize.X > 100.0 && SavedSize.Y > 100.0;
	// §8.3.1 step 7: the slow-task modal's popup and activation settings (RV-02), kept in desktop pixels across steps.
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::Format(LOCTEXT("Paused.WindowTitle", "Dream · Paused · Run #{0}"),
			FText::AsNumber(Stop.RunNumber, &FNumberFormattingOptions::DefaultNoGrouping())))
		.SizingRule(ESizingRule::UserSized)
		.AutoCenter(bHaveGeometry ? EAutoCenter::None : EAutoCenter::PreferredWorkArea)
		.ScreenPosition(SavedPosition)
		.ClientSize(bHaveGeometry ? SavedSize : FVector2D(960.0, 600.0))
		.AdjustInitialSizeAndPositionForDPIScale(!bHaveGeometry)
		.IsPopupWindow(true)
		.HasCloseButton(false)
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		.ActivationPolicy(bFocus ? EWindowActivationPolicy::Always : EWindowActivationPolicy::Never)
		.FocusWhenFirstShown(bFocus);
	Window->SetContent(Content);
	Window->SetWidgetToFocusOnActivate(Content);
	// Alt+F4 and every other close request do nothing: the run is paused, Continue or Stop ends the pause.
	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride::CreateLambda([](const TSharedRef<SWindow>&) {}));
	PausedWindow = Window;
	PausedContent = Content;

	const FDelegateHandle TickHandle = App.GetOnModalLoopTickEvent().AddRaw(this, &FDreamDebugger::OnModalLoopTick);
	{
		// §8.3.1 steps 5 and 6: a Blueprint breakpoint hit by Dream refuses instead of nesting; never EnterDebuggingMode.
		FSlateApplication::FScopedPreventDebuggingMode NoNesting(LOCTEXT("Paused.PreventDebugging", "The Dream debugger is paused"));
		TSharedPtr<const SWidget> Parent = App.GetActiveModalWindow();
		if (!Parent.IsValid() && FGlobalTabmanager::Get()->GetRootWindow().IsValid())
		{
			Parent = FGlobalTabmanager::Get()->GetRootWindow();
		}
		App.AddModalWindow(Window, Parent, /*bSlowTaskWindow=*/false);   // blocks until the window is destroyed (E6)
	}
	App.GetOnModalLoopTickEvent().Remove(TickHandle);

	const bool bWasShown = bShown;
	if (const TSharedPtr<SWindow> Left = PausedWindow.Pin())
	{
		// AddModalWindow returned without the loop (no renderer, unattended, §8.3.1 step 10): never leave it behind.
		Left->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		App.RequestDestroyWindow(Left.ToSharedRef());
	}
	PausedWindow.Reset();
	PausedContent.Reset();
	Pending.Reset();
	bPaused = false;
	NotifyChanged();

	if (!bWasShown)
	{
		Out.Refusal = EDreamPauseRefusal::WindowFailed;
		return Out;
	}
	Out.Kind = bResumeSet ? Resume.Kind : EDreamResumeKind::Continue;
	if (Out.bInspectOnly && Out.Kind != EDreamResumeKind::Stop)
	{
		Out.Kind = EDreamResumeKind::Continue;     // §8.2.5: inspection only; steps are disabled
	}
	return Out;
}

bool FDreamDebugger::GetPausedLine(const FDreamBuffer& Buffer, int32& OutLine) const
{
	FDreamEditorModule* M = DreamDebuggerPrivate::Module();
	if (!bPaused || M == nullptr || Stop.Line <= 0)
	{
		return false;
	}
	if (!DreamDebuggerPrivate::SameSource(M->GetBufferManager().GetCompileFilename(Buffer), Stop.Source))
	{
		return false;
	}
	OutLine = Stop.Line - 1;
	return true;
}

void FDreamDebugger::RequestResume(EDreamResumeKind Kind)
{
	if (!bPaused || bResumeSet)
	{
		return;
	}
	Resume.Kind = Kind;
	bResumeSet = true;
	if (Kind == EDreamResumeKind::Stop)
	{
		// Stop always works (§8.3.3): the run-level RequestStop at once, then the window closes with "terminate".
		if (FDreamEditorModule* M = DreamDebuggerPrivate::Module())
		{
			M->GetRunService().RequestStopActive(EDreamStopSource::PausedWindow);
		}
	}
	CloseWindow();
}

void FDreamDebugger::OnRunStopRequested()
{
	if (!bPaused)
	{
		return;
	}
	Resume.Kind = EDreamResumeKind::Stop;
	bResumeSet = true;
	CloseWindow();
}

void FDreamDebugger::CloseWindow()
{
	const TSharedPtr<SWindow> Window = PausedWindow.Pin();
	if (bClosing || !Window.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	bClosing = true;
	SavedPosition = Window->GetPositionInScreen();
	SavedSize = Window->GetClientSizeInScreen();
	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
}

void FDreamDebugger::QueueRequest(const FString& Command, dream::json::Value Arguments, TFunction<void(const dream::json::Value&)> OnResponse)
{
	using namespace dream::json;
	if (!bPaused)
	{
		return;
	}
	Value Request = Value::MakeObject();
	Request.Set("seq", Value::MakeInt(NextSeq++));
	Request.Set("type", Value::MakeString("request"));
	Request.Set("command", Value::MakeString(DreamDebuggerPrivate::ToUtf8(Command)));
	Request.Set("arguments", Arguments.IsObject() ? MoveTemp(Arguments) : Value::MakeObject());
	FRequest Item;
	Item.Json = DreamDebuggerPrivate::FromUtf8(Serialize(Request));
	Item.OnResponse = MoveTemp(OnResponse);
	Pending.Add(MoveTemp(Item));
}

void FDreamDebugger::OnModalLoopTick(float DeltaTime)
{
	using namespace dream::json;
	// §8.3.3: only our own modal loop drains, one request per iteration, never while a request is running (an evaluate's
	// EditorDialog broadcasts this event again inside it).
	const TSharedPtr<SWindow> Window = PausedWindow.Pin();
	if (!Window.IsValid() || !FSlateApplication::IsInitialized() || FSlateApplication::Get().GetActiveModalWindow() != Window || bInRequest)
	{
		return;
	}
	bShown = true;
	if (bResumeSet)
	{
		return;
	}
	if (Pending.Num() > 0)
	{
		FRequest Item = MoveTemp(Pending[0]);
		Pending.RemoveAt(0);
		bInRequest = true;
		FString ResponseText;
		const bool bCalled = DreamPythonDebug::CallHandleRequest(Item.Json, ResponseText);
		bInRequest = false;
		Value Response;
		if (!bCalled || !Parse(DreamDebuggerPrivate::ToUtf8(ResponseText), Response) || !Response.IsObject())
		{
			Response = Value::MakeObject();
			Response.Set("success", Value::MakeBool(false));
			Response.Set("message", Value::MakeString(DreamDebuggerPrivate::ToUtf8(bCalled ? FString(TEXT("bad response")) : ResponseText)));
		}
		if (Item.OnResponse)
		{
			Item.OnResponse(Response);
		}
		return;
	}
	if (PausedTickForTest)
	{
		PausedTickForTest(*this);
	}
}

#undef LOCTEXT_NAMESPACE
