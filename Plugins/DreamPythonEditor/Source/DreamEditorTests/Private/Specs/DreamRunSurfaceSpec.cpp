// SPDX-License-Identifier: Apache-2.0
// Run-state wording and the _dream rules that slice B added (run group, B32, B33, B34):
//   Dream.Run.StartingText (any editor) §3.9: a request in Starting shows "Starting" and, once it has waited 1 s for an
//       idle end of frame (queued, about to prepare, or its entry refused or deferred), "Starting… waiting for the editor
//       to become idle"; never while it waits for its journal snapshot or its Run File save.
//   Dream.Run.NativeSurface (attended) in a real run: _dream.write(STREAM_AGENT, ...) raises ValueError outside agent and
//       bridge runs (§6.1, T-OUT-013's STREAM_AGENT row), _dream.check_cancel(True) is False without a Stop, and
//       check_cancel's argument rules match the null module (§12.1, ARCHITECTURE section 6).
//   Dream.Run.ResolverHelpers (attended) T-RES-026: _dream.hard_object_props on the property-kinds fixture
//       (Fixtures/DreamTestPropKindsFixture.h) returns the hard object, class and weak object properties only, by ScriptName
//       and without the "In" prefix, drops a colliding pair, is stable on a second (cached) call, is empty for an unknown
//       class, an empty name and a name longer than FName holds (NAME_SIZE), and raises TypeError for a non-str; _dream.container_element_kind returns None ("do not read") for an
//       unreal.Array, Map and Set and for any other value (§7.5.2; reading the element property is Deferred (M2)).
#include "Support/DreamTestUtil.h"

#include "HAL/PlatformTime.h"
#include "Run/DreamRunTypes.h"

BEGIN_DEFINE_SPEC(FDreamStartingTextSpec, "Dream.Run.StartingText", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamStartingTextSpec)

void FDreamStartingTextSpec::Define()
{
	It("says Starting, then after 1 s without an idle end of frame that it waits for the editor (TDD 3.9)", [this]()
	{
		const double Now = FPlatformTime::Seconds();
		FDreamRunRecord Rec;
		Rec.State = EDreamRunState::Starting;
		Rec.StartingStep = EDreamStartingStep::Queued;
		Rec.StartSeconds = Now - 0.4;
		TestFalse(TEXT("0.4 s: not waiting yet"), DreamRunText::IsWaitingForIdle(Rec, Now));
		TestEqual(TEXT("0.4 s: Starting"), DreamRunText::StartingStateText(Rec, Now).ToString(), DreamRunText::StateName(EDreamRunState::Starting).ToString());
		Rec.StartSeconds = Now - 1.5;
		TestTrue(TEXT("1.5 s queued: waiting for idle"), DreamRunText::IsWaitingForIdle(Rec, Now));
		TestTrue(TEXT("the waiting text"), DreamRunText::StartingStateText(Rec, Now).ToString().Contains(TEXT("waiting for the editor to become idle")));
		Rec.StartingStep = EDreamStartingStep::Enter;
		TestTrue(TEXT("an entry refused or deferred waits for idle too"), DreamRunText::IsWaitingForIdle(Rec, Now));
		Rec.StartingStep = EDreamStartingStep::AwaitSnapshot;
		TestFalse(TEXT("the journal snapshot has its own 2 s bound"), DreamRunText::IsWaitingForIdle(Rec, Now));
		Rec.StartingStep = EDreamStartingStep::AwaitSave;
		TestFalse(TEXT("a Run File save is progress, not waiting for idle"), DreamRunText::IsWaitingForIdle(Rec, Now));
		Rec.StartingStep = EDreamStartingStep::Queued;
		Rec.State = EDreamRunState::Running;
		TestFalse(TEXT("only a run in Starting waits"), DreamRunText::IsWaitingForIdle(Rec, Now));
	});
}

BEGIN_DEFINE_SPEC(FDreamNativeSurfaceSpec, "Dream.Run.NativeSurface", DreamTest::AttendedFlags)
END_DEFINE_SPEC(FDreamNativeSurfaceSpec)

void FDreamNativeSurfaceSpec::Define()
{
	LatentIt("refuses STREAM_AGENT outside agent and bridge runs and keeps check_cancel's argument rules", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Code = FString::Join(TArray<FString>{
			TEXT("import _dream as _s_native"),
			TEXT("try:"),
			TEXT("    _s_native.write(_s_native.STREAM_AGENT, _s_native.current_run_id(), 'agent line\\n')"),
			TEXT("    print('agent-accepted')"),
			TEXT("except ValueError as exc:"),
			TEXT("    print('agent-refused', 'agent or bridge' in str(exc))"),
			TEXT("print('deliver-none', _s_native.check_cancel(True), _s_native.check_cancel())"),
			TEXT("for bad in ((1,), (True, True)):"),
			TEXT("    try:"),
			TEXT("        _s_native.check_cancel(*bad)"),
			TEXT("        print('arg-accepted', bad)"),
			TEXT("    except TypeError:"),
			TEXT("        print('arg-refused', len(bad))"),
			TEXT("del _s_native"),
		}, TEXT("\n"));
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("the surface probe"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Run, 60.0);
		Steps.Do(TEXT("check"), [this, Run]()
		{
			if (DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				for (const TCHAR* Verdict : { TEXT("agent-refused True"), TEXT("deliver-none False False"), TEXT("arg-refused 1"), TEXT("arg-refused 2") })
				{
					TestTrue(*FString::Printf(TEXT("%s: %s"), Verdict, *Run->Describe()), Run->HasLineContaining(dream::LineType::Stdout, Verdict));
				}
				TestFalse(TEXT("no agent line was accepted"), Run->HasLineContaining(dream::LineType::Stdout, TEXT("agent-accepted")));
			}
			return true;
		});
		Steps.Run(Done);
	});
}

BEGIN_DEFINE_SPEC(FDreamResolverHelpersSpec, "Dream.Run.ResolverHelpers", DreamTest::AttendedFlags)
END_DEFINE_SPEC(FDreamResolverHelpersSpec)

void FDreamResolverHelpersSpec::Define()
{
	LatentIt("lists only hard object, class and weak object properties and reads no container element (T-RES-026)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Code = FString::Join(TArray<FString>{
			TEXT("import _dream as _s_native, unreal as _s_unreal"),
			TEXT("_s_first = _s_native.hard_object_props('DreamTestPropKindsFixture')"),
			TEXT("_s_again = _s_native.hard_object_props('DreamTestPropKindsFixture')"),
			TEXT("print('props', type(_s_first).__name__, ','.join(sorted(_s_first)), _s_first == _s_again)"),
			TEXT("print('unknown', len(_s_native.hard_object_props('NoSuchDreamClass_x7')), len(_s_native.hard_object_props('')),"),
			TEXT("    len(_s_native.hard_object_props('x' * 2000)))"),
			TEXT("try:"),
			TEXT("    _s_native.hard_object_props(5)"),
			TEXT("    print('nonstr-accepted')"),
			TEXT("except TypeError:"),
			TEXT("    print('nonstr-refused')"),
			TEXT("_s_kinds = [_s_native.container_element_kind(v) for v in (_s_unreal.Array(_s_unreal.Object), _s_unreal.Array(int),"),
			TEXT("    _s_unreal.Map(str, _s_unreal.Object), _s_unreal.Set(int), [1], {'a': 1}, None)]"),
			TEXT("print('kinds', all(k is None for k in _s_kinds), len(_s_kinds))"),
			TEXT("del _s_native, _s_unreal, _s_first, _s_again, _s_kinds"),
		}, TEXT("\n"));
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("the resolver helper probe"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Run, 60.0);
		Steps.Do(TEXT("check"), [this, Run]()
		{
			if (DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				for (const TCHAR* Verdict : {
					TEXT("props frozenset classref,hardobject,prefixed,renamedtarget,weakobject True"),
					TEXT("unknown 0 0 0"),
					TEXT("nonstr-refused"),
					TEXT("kinds True 7") })
				{
					TestTrue(*FString::Printf(TEXT("%s: %s"), Verdict, *Run->Describe()), Run->HasLineContaining(dream::LineType::Stdout, Verdict));
				}
			}
			return true;
		});
		Steps.Run(Done);
	});
}
