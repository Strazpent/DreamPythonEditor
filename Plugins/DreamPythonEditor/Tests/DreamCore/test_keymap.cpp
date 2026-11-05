// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::Keymap (TDD §4.9.1 flowchart, T-KEY-001; AltGr, T-KEY-002; consumed bound chords and pass-through,
// T-KEY-003).
#include "dream_test.h"

#include "dream/Keymap.h"

using namespace dream;

namespace
{
	enum : KeyCode
	{
		K_Enter = 1, K_Z, K_S, K_Tab, K_Left, K_F4, K_F7, K_N, K_A, K_P, K_Esc, K_Slash, K_F1,
	};

	enum : CommandId
	{
		C_RunSelection = 1, C_RunNumpad, C_UndoLastRun, C_NextError, C_Save, C_Stop, C_ToggleComment, C_Old, C_New,
	};

	Stroke Key(KeyCode K, uint8_t Mods, char32_t Char, KeyClass Class)
	{
		Stroke S;
		S.Key = K;
		S.Mods = Mods;
		S.Char = Char;
		S.Class = Class;
		return S;
	}

	Keymap MakeCharcoal()
	{
		Keymap M;
		std::vector<Binding> B;
		B.push_back(Binding{C_RunSelection, K_Enter, Mod_Ctrl, false, Ctx_Focused, 0});
		B.push_back(Binding{C_RunNumpad, K_Enter, Mod_None, true, Ctx_Focused | Ctx_Buffer, 0});
		B.push_back(Binding{C_UndoLastRun, K_Z, Mod_Ctrl | Mod_Alt, false, Ctx_Focused, 0});
		B.push_back(Binding{C_NextError, K_F4, Mod_None, false, Ctx_Focused, 0});
		B.push_back(Binding{C_Save, K_S, Mod_Ctrl, false, Ctx_Focused | Ctx_Buffer, 0});
		B.push_back(Binding{C_Stop, K_Esc, Mod_None, false, Ctx_Focused | Ctx_RunActive, 0});
		B.push_back(Binding{C_ToggleComment, K_Slash, Mod_Ctrl, false, Ctx_Focused | Ctx_Editable, Ctx_Prompt});
		B.push_back(Binding{C_Old, K_A, Mod_Ctrl | Mod_Shift, false, 0, 0});
		B.push_back(Binding{C_New, K_A, Mod_Ctrl | Mod_Shift, false, 0, 0});   // later entries win ties
		M.SetBindings(B);
		M.SetConsumeList({Chord{K_Z, Mod_Ctrl}, Chord{K_S, Mod_Ctrl}, Chord{K_A, Mod_Ctrl}});
		M.SetPassThrough({Chord{K_Tab, Mod_Ctrl}, Chord{K_P, Mod_Ctrl}, Chord{K_F1, Mod_None}});
		return M;
	}

	ResolveKind Kind(const Keymap& M, const Stroke& S, uint32_t Ctx = Ctx_Focused | Ctx_Buffer | Ctx_Editable)
	{
		return M.Resolve(S, Ctx, 0.0, false, false).Kind;
	}
}

TEST(Keymap_FlowchartBranches)
{
	const Keymap M = MakeCharcoal();
	const uint32_t Ctx = Ctx_Focused | Ctx_Buffer | Ctx_Editable;
	const Stroke CtrlEnter = Key(K_Enter, Mod_Ctrl, U'\r', KeyClass::Editing);
	CHECK(M.Resolve(CtrlEnter, Ctx, 0.0, true, false).Kind == ResolveKind::ConsumeOnly);     // IME composing
	CHECK(M.Resolve(CtrlEnter, Ctx, 0.0, false, true).Kind == ResolveKind::ToSubHandler);    // prompt or popup
	const Resolution R = M.Resolve(CtrlEnter, Ctx, 0.0, false, false);
	CHECK(R.Kind == ResolveKind::Command);
	CHECK_EQ(R.Command, static_cast<CommandId>(C_RunSelection));
	CHECK(Kind(M, Key(K_Z, Mod_Ctrl, U'z', KeyClass::LetterDigitOem)) == ResolveKind::ConsumeOnly);   // consume list
	CHECK(Kind(M, Key(K_A, Mod_None, U'a', KeyClass::LetterDigitOem)) == ResolveKind::TextInput);     // printable
	CHECK(Kind(M, Key(K_A, Mod_Shift, U'A', KeyClass::LetterDigitOem)) == ResolveKind::TextInput);
	CHECK(Kind(M, Key(K_Left, Mod_None, 0, KeyClass::Navigation)) == ResolveKind::ConsumeOnly);       // primitive
	CHECK(Kind(M, Key(K_Left, Mod_Ctrl | Mod_Shift, 0, KeyClass::Navigation)) == ResolveKind::ConsumeOnly);
	CHECK(Kind(M, Key(K_Enter, Mod_None, U'\r', KeyClass::Editing)) == ResolveKind::ConsumeOnly);     // main Enter: newline
	CHECK(Kind(M, Key(K_Tab, Mod_Ctrl, U'\t', KeyClass::Editing)) == ResolveKind::Unhandled);         // pass-through
	CHECK(Kind(M, Key(K_F1, Mod_None, 0, KeyClass::Function)) == ResolveKind::Unhandled);
	CHECK(Kind(M, Key(K_N, Mod_Ctrl, U'n', KeyClass::LetterDigitOem)) == ResolveKind::ConsumeOnly);   // the extension
	CHECK(Kind(M, Key(K_N, Mod_Ctrl | Mod_Shift, U'N', KeyClass::LetterDigitOem)) == ResolveKind::ConsumeOnly);
	CHECK(Kind(M, Key(K_N, Mod_Ctrl | Mod_Alt, U'n', KeyClass::LetterDigitOem)) == ResolveKind::ConsumeOnly);
	CHECK(Kind(M, Key(K_N, Mod_Ctrl | Mod_Alt | Mod_Shift, U'N', KeyClass::LetterDigitOem)) == ResolveKind::Unhandled);
	CHECK(Kind(M, Key(K_N, Mod_Alt, U'n', KeyClass::LetterDigitOem)) == ResolveKind::Unhandled);      // Alt+letter
	CHECK(Kind(M, Key(K_F7, Mod_None, 0, KeyClass::Function)) == ResolveKind::Unhandled);             // unbound F-key
	CHECK(Kind(M, Key(K_F4, Mod_None, 0, KeyClass::Function)) == ResolveKind::Command);
	Keymap NoExt = MakeCharcoal();
	NoExt.SetConsumeExtension(false);
	CHECK(Kind(NoExt, Key(K_N, Mod_Ctrl, U'n', KeyClass::LetterDigitOem)) == ResolveKind::Unhandled);
	CHECK(Keymap::IsPrintable(Key(K_A, Mod_None, U'a', KeyClass::LetterDigitOem)));
	CHECK(!Keymap::IsPrintable(Key(K_A, Mod_Ctrl, U'a', KeyClass::LetterDigitOem)));
	CHECK(!Keymap::IsPrintable(Key(K_A, Mod_None, 0x7F, KeyClass::Other)));
}

TEST(Keymap_AltGrIsTextAndLeftCtrlAltIsACommand)
{
	const Keymap M = MakeCharcoal();
	// Polish AltGr+Z arrives as LeftCtrl+RightAlt with the character 'ż': text, never Undo Last Run (RT-16).
	Stroke AltGrZ = Key(K_Z, Mod_Ctrl, 0x17C, KeyClass::LetterDigitOem);
	AltGrZ.bRightAlt = true;
	CHECK(Kind(M, AltGrZ) == ResolveKind::TextInput);
	const Resolution Undo = M.Resolve(Key(K_Z, Mod_Ctrl | Mod_Alt, U'z', KeyClass::LetterDigitOem), Ctx_Focused, 0.0, false, false);
	CHECK(Undo.Kind == ResolveKind::Command);
	CHECK_EQ(Undo.Command, static_cast<CommandId>(C_UndoLastRun));
	Stroke AltGrShiftEnter = Key(K_Enter, Mod_Shift, U'\r', KeyClass::Editing);
	AltGrShiftEnter.bRightAlt = true;
	CHECK(Kind(M, AltGrShiftEnter) != ResolveKind::TextInput);
}

TEST(Keymap_BoundChordsConsumedNumpadAndContexts)
{
	const Keymap M = MakeCharcoal();
	Stroke Numpad = Key(K_Enter, Mod_None, U'\r', KeyClass::Editing);
	Numpad.bNumpadEnter = true;
	const Resolution N = M.Resolve(Numpad, Ctx_Focused | Ctx_Buffer, 0.0, false, false);
	CHECK(N.Kind == ResolveKind::Command);
	CHECK_EQ(N.Command, static_cast<CommandId>(C_RunNumpad));
	Stroke CtrlNumpad = Key(K_Enter, Mod_Ctrl, U'\r', KeyClass::Editing);
	CtrlNumpad.bNumpadEnter = true;
	CHECK_EQ(M.Resolve(CtrlNumpad, Ctx_Focused, 0.0, false, false).Command, static_cast<CommandId>(C_RunSelection));
	// Save is bound only in a buffer; elsewhere Ctrl+S is still consumed (D8), never Save Current Level.
	const Stroke CtrlS = Key(K_S, Mod_Ctrl, U's', KeyClass::LetterDigitOem);
	CHECK_EQ(M.Resolve(CtrlS, Ctx_Focused | Ctx_Buffer, 0.0, false, false).Command, static_cast<CommandId>(C_Save));
	CHECK(M.Resolve(CtrlS, Ctx_Focused | Ctx_Output, 0.0, false, false).Kind == ResolveKind::ConsumeOnly);
	// Esc is Stop during a run and a consumed primitive otherwise.
	const Stroke Esc = Key(K_Esc, Mod_None, 0x1B, KeyClass::Editing);
	CHECK_EQ(M.Resolve(Esc, Ctx_Focused | Ctx_RunActive, 0.0, false, false).Command, static_cast<CommandId>(C_Stop));
	CHECK(M.Resolve(Esc, Ctx_Focused, 0.0, false, false).Kind == ResolveKind::ConsumeOnly);
	// Excluded context bits.
	const Stroke CtrlSlash = Key(K_Slash, Mod_Ctrl, U'/', KeyClass::LetterDigitOem);
	CHECK(M.Resolve(CtrlSlash, Ctx_Focused | Ctx_Editable, 0.0, false, false).Kind == ResolveKind::Command);
	CHECK(M.Resolve(CtrlSlash, Ctx_Focused | Ctx_Editable | Ctx_Prompt, 0.0, false, false).Kind == ResolveKind::ConsumeOnly);
	// Ties: the later binding wins; FindCommand skips the numpad-only binding.
	CHECK_EQ(M.Resolve(Key(K_A, Mod_Ctrl | Mod_Shift, U'A', KeyClass::LetterDigitOem), 0, 0.0, false, false).Command,
		static_cast<CommandId>(C_New));
	CHECK_EQ(M.FindCommand(K_Enter, Mod_Ctrl, Ctx_Focused), static_cast<CommandId>(C_RunSelection));
	CHECK_EQ(M.FindCommand(K_Enter, Mod_None, Ctx_Focused | Ctx_Buffer), 0u);
	CHECK_EQ(M.FindCommand(K_F7, Mod_None, Ctx_Focused), 0u);
	// The pass-through list wins over the extension (T-KEY-003).
	CHECK(Kind(M, Key(K_P, Mod_Ctrl, U'p', KeyClass::LetterDigitOem)) == ResolveKind::Unhandled);
}
