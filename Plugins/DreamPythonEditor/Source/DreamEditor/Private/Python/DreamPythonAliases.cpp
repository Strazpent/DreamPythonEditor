// SPDX-License-Identifier: Apache-2.0
// The selection aliases' editor side (TDD §5.3.8, §3.7 "Alias refresh (U19); map-change release", §3.8 UObject rules,
// §12.1.3 _impl.aliases.refresh() / on_map_change(), UEI-03, U19, D3, T-LIFE-014). Run group.
//
// (run group choice) Python reads the editor selection (EditorActorSubsystem, EditorUtilityLibrary, UnrealEditorSubsystem
// through the unreal module) and owns the U19 table; C++ only decides when: prepare in Starting (FDreamPython::Prepare
// passes "aliases"), an idle tick after a level-editor selection change, and an idle tick after FEditorDelegates::MapChange
// (Editor.h:231). The handlers run no Python (they may fire inside a pump or a Slate handler, D14(5)); each queues one
// FDreamDeferral ticker (Defer while a run executes, needs Python Ready) that calls the helper. Game thread.
#include "Python/DreamPython.h"

#include "Editor.h"
#include "Selection.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Python/DreamPyUtil.h"
#include "Python/DreamPythonImpl.h"
#include "Run/DreamDeferral.h"
#include "Settings/DreamUserSettings.h"

namespace DreamPythonAliasesLocal
{
	/** A burst of selection changes (a marquee drag) costs one refresh (proposed 250 ms). */
	constexpr float SelectionRefreshDelaySeconds = 0.25f;

	bool AliasesEnabled()
	{
		const UDreamUserSettings* Settings = UDreamUserSettings::Get();
		return Settings == nullptr || Settings->bSelectionAliases;
	}

#if WITH_PYTHON
	/** Calls _impl.aliases.<Function>(<args>) and returns its int result (0 on any failure). GIL held. */
	int32 CallAliasHelper(FDreamPyRef& Cache, const char* Function, PyObject* ArgsTuple)
	{
		if (ArgsTuple == nullptr)
		{
			PyErr_Clear();
			return 0;
		}
		if (!Cache)
		{
			PyObject* Module = PyImport_ImportModule("dream_editor._impl.aliases");
			if (Module != nullptr)
			{
				Cache = FDreamPyRef::Steal(PyObject_GetAttrString(Module, Function));
				Py_DECREF(Module);
			}
		}
		PyObject* Result = Cache ? PyObject_CallObject(Cache.Get(), ArgsTuple) : nullptr;
		Py_DECREF(ArgsTuple);
		if (Result == nullptr)
		{
			UE_LOG(LogDream, Warning, TEXT("aliases.%hs failed: %s"), Function, *DreamPy::FetchExceptionText());
			PyErr_Clear();
			return 0;
		}
		const long Value = DreamPy::IsStrictInt(Result) ? PyLong_AsLong(Result) : 0;
		Py_DECREF(Result);
		PyErr_Clear();
		return static_cast<int32>(FMath::Max<long>(Value, 0));
	}
#endif
}

void FDreamPython::FImpl::CancelAliasWork()
{
	if (AliasRefreshTicker.IsValid())
	{
		FDreamDeferral::RemoveTicker(AliasRefreshTicker);
		AliasRefreshTicker.Reset();
	}
	if (MapChangeTicker.IsValid())
	{
		FDreamDeferral::RemoveTicker(MapChangeTicker);
		MapChangeTicker.Reset();
	}
}

void FDreamPython::BindEditorEvents()
{
	check(IsInGameThread());
	if (Impl->bHeadless)
	{
		return;     // no level editor selection and no tab: -run=PythonScript gets no aliases (§5.3.8 is editor-only)
	}
	// USelection broadcasts both for actor and for object selections (Engine/Selection.h); the alias reads what it needs.
	Impl->SelectionChangedHandle = USelection::SelectionChangedEvent.AddRaw(this, &FDreamPython::HandleSelectionChanged);
	Impl->SelectObjectHandle = USelection::SelectObjectEvent.AddRaw(this, &FDreamPython::HandleSelectionChanged);
	Impl->SelectNoneHandle = USelection::SelectNoneEvent.AddRaw(this, &FDreamPython::HandleSelectionChanged, static_cast<UObject*>(nullptr));
	Impl->MapChangeHandle = FEditorDelegates::MapChange.AddRaw(this, &FDreamPython::HandleMapChange);
}

void FDreamPython::UnbindEditorEvents()
{
	USelection::SelectionChangedEvent.Remove(Impl->SelectionChangedHandle);
	USelection::SelectObjectEvent.Remove(Impl->SelectObjectHandle);
	USelection::SelectNoneEvent.Remove(Impl->SelectNoneHandle);
	FEditorDelegates::MapChange.Remove(Impl->MapChangeHandle);
	Impl->SelectionChangedHandle.Reset();
	Impl->SelectObjectHandle.Reset();
	Impl->SelectNoneHandle.Reset();
	Impl->MapChangeHandle.Reset();
	Impl->CancelAliasWork();
}

void FDreamPython::HandleSelectionChanged(UObject* Selection)
{
	(void)Selection;
	// UEI-03: refresh on a selection change while idle. Only a booted dream_editor has a Session to write into; before the
	// boot the next prepare writes the aliases anyway.
	if (!IsInGameThread() || !FDreamEditorModule::IsAvailable() || !IsReady() || !Impl->bBooted
		|| Impl->AliasRefreshTicker.IsValid() || !DreamPythonAliasesLocal::AliasesEnabled())
	{
		return;
	}
	const uint32 Generation = GetGeneration();
	Impl->AliasRefreshTicker = FDreamDeferral::AddTicker(TEXT("DreamAliasRefresh"), EDreamDeferPolicy::DeferWhileExecuting,
		DreamPythonAliasesLocal::SelectionRefreshDelaySeconds, /*bNeedsPython*/ true, [this, Generation](float)
		{
			Impl->AliasRefreshTicker.Reset();
#if WITH_PYTHON
			if (Generation != GetGeneration() || !Impl->bBooted || !DreamPythonAliasesLocal::AliasesEnabled())
			{
				return false;
			}
			int32 Written = 0;
			{
				FDreamScopedGil Gil;
				Written = DreamPythonAliasesLocal::CallAliasHelper(Impl->AliasRefreshFn, "refresh", Py_BuildValue("(i)", 0));
			}
			if (Written > 0)
			{
				++Impl->SessionEpoch;
			}
#endif
			return false;
		});
}

void FDreamPython::HandleMapChange(uint32 MapChangeFlags)
{
	(void)MapChangeFlags;
	// §3.8 UObject rules: Dream-written aliases and the last-run namespace are released through a deferred helper, so Dream
	// never keeps an old world alive (D3); user-bound names stay the user's.
	if (!IsInGameThread() || !FDreamEditorModule::IsAvailable() || !Impl->bBooted || Impl->MapChangeTicker.IsValid())
	{
		return;
	}
	const uint32 Generation = GetGeneration();
	Impl->MapChangeTicker = FDreamDeferral::AddTicker(TEXT("DreamAliasMapChange"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
		/*bNeedsPython*/ true, [this, Generation](float)
		{
			Impl->MapChangeTicker.Reset();
#if WITH_PYTHON
			if (Generation != GetGeneration() || !Impl->bBooted)
			{
				return false;
			}
			int32 Released = 0;
			{
				FDreamScopedGil Gil;
				Released = DreamPythonAliasesLocal::CallAliasHelper(Impl->AliasMapChangeFn, "on_map_change", PyTuple_New(0));
			}
			UE_LOG(LogDream, Verbose, TEXT("Map change: %d selection alias(es) and the last-run namespace released."), Released);
			if (Released > 0)
			{
				++Impl->SessionEpoch;
			}
#endif
			return false;
		});
}

void FDreamPython::SimulateMapChangeForTest()
{
	HandleMapChange(MapChangeEventFlags::NewMap);
}
