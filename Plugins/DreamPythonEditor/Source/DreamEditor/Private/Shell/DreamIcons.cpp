// SPDX-License-Identifier: Apache-2.0
// (icons) FDreamIcons: the dream icon set's registry (PO-06 F6). See DreamIcons.h for the style rules.
#include "Shell/DreamIcons.h"

#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamTheme.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

namespace DreamIconsLocal
{
	struct FEntry
	{
		const TCHAR* Name;
		const TCHAR* File;
		EDreamIconTint Tint;
	};

	// In EDreamIcon order (static_assert below). The files are Resources/Icons/<File>, written by Tools/Icons/dream_icons.py.
	const FEntry Entries[] = {
		{ TEXT("Brand"), TEXT("brand.svg"), EDreamIconTint::None },
		{ TEXT("BrandMono"), TEXT("brand_mono.svg"), EDreamIconTint::Text },
		{ TEXT("Open"), TEXT("open.svg"), EDreamIconTint::Text },
		{ TEXT("Save"), TEXT("save.svg"), EDreamIconTint::Text },
		{ TEXT("Run"), TEXT("run.svg"), EDreamIconTint::Accent },
		{ TEXT("RunLine"), TEXT("run_line.svg"), EDreamIconTint::Text },
		{ TEXT("Stop"), TEXT("stop.svg"), EDreamIconTint::Danger },
		{ TEXT("Clear"), TEXT("clear.svg"), EDreamIconTint::Text },
		{ TEXT("UndoRun"), TEXT("undo_run.svg"), EDreamIconTint::Text },
		{ TEXT("Echo"), TEXT("echo.svg"), EDreamIconTint::Text },
		{ TEXT("LineNumbers"), TEXT("line_numbers.svg"), EDreamIconTint::Text },
		{ TEXT("ChevronDown"), TEXT("chevron_down.svg"), EDreamIconTint::Muted },
		{ TEXT("Close"), TEXT("close.svg"), EDreamIconTint::Muted },
		{ TEXT("Plus"), TEXT("plus.svg"), EDreamIconTint::Muted },
		{ TEXT("Recovered"), TEXT("recovered.svg"), EDreamIconTint::Moon },
		{ TEXT("FolderClosed"), TEXT("folder_closed.svg"), EDreamIconTint::Muted },
		{ TEXT("FolderOpen"), TEXT("folder_open.svg"), EDreamIconTint::Muted },
		{ TEXT("FilePython"), TEXT("file_python.svg"), EDreamIconTint::Sky },
		{ TEXT("File"), TEXT("file.svg"), EDreamIconTint::Muted },
		{ TEXT("Scratches"), TEXT("scratches.svg"), EDreamIconTint::Lavender },
		{ TEXT("Samples"), TEXT("samples.svg"), EDreamIconTint::Moon },
		{ TEXT("ProjectPlugins"), TEXT("project_plugins.svg"), EDreamIconTint::Text },
		{ TEXT("Engine"), TEXT("engine.svg"), EDreamIconTint::Muted },
		{ TEXT("ReadOnly"), TEXT("read_only.svg"), EDreamIconTint::Muted },
		{ TEXT("Class"), TEXT("class.svg"), EDreamIconTint::Keyword },
		{ TEXT("Function"), TEXT("function.svg"), EDreamIconTint::Definition },
		{ TEXT("Method"), TEXT("method.svg"), EDreamIconTint::Definition },
		{ TEXT("Async"), TEXT("async.svg"), EDreamIconTint::Builtin },
		{ TEXT("Cell"), TEXT("cell.svg"), EDreamIconTint::Comment },
		{ TEXT("Error"), TEXT("error.svg"), EDreamIconTint::Error },
		{ TEXT("Warning"), TEXT("warning.svg"), EDreamIconTint::Warning },
		{ TEXT("Info"), TEXT("info.svg"), EDreamIconTint::Sky },
		{ TEXT("Lightbulb"), TEXT("lightbulb.svg"), EDreamIconTint::Moon },
		{ TEXT("Breakpoint"), TEXT("breakpoint.svg"), EDreamIconTint::Error },
		{ TEXT("BreakpointDisabled"), TEXT("breakpoint_disabled.svg"), EDreamIconTint::Muted },
		{ TEXT("CurrentLine"), TEXT("current_line.svg"), EDreamIconTint::Moon },
		{ TEXT("Continue"), TEXT("continue.svg"), EDreamIconTint::Accent },
		{ TEXT("StepOver"), TEXT("step_over.svg"), EDreamIconTint::Accent },
		{ TEXT("StepInto"), TEXT("step_into.svg"), EDreamIconTint::Accent },
		{ TEXT("StepOut"), TEXT("step_out.svg"), EDreamIconTint::Accent },
		{ TEXT("DebugRun"), TEXT("debug_run.svg"), EDreamIconTint::Mint },
		{ TEXT("RunWindow"), TEXT("run_window.svg"), EDreamIconTint::Accent },
		{ TEXT("PausedWindow"), TEXT("paused_window.svg"), EDreamIconTint::Lavender },
		{ TEXT("Notify"), TEXT("notify.svg"), EDreamIconTint::Text },
		{ TEXT("Success"), TEXT("success.svg"), EDreamIconTint::Mint },
	};
	static_assert(UE_ARRAY_COUNT(Entries) == static_cast<int32>(EDreamIcon::Count), "one entry per EDreamIcon, in order");

	/** The dream accents, sRGB: (dark-theme value, light-theme value). The light values are darker so a glyph keeps its
	 *  contrast on white. Same values as ACCENTS in dream_icons.py (the preview). */
	struct FAccent
	{
		uint32 Dark;
		uint32 Light;
	};
	const FAccent Accents[] = {
		{ 0xE8C15A, 0xB07F12 },   // Moon
		{ 0x4FC99A, 0x1F8A62 },   // Mint
		{ 0xA98BEA, 0x7552C8 },   // Lavender
		{ 0x5AAEEA, 0x1F6FB8 },   // Sky
		{ 0xEA7A93, 0xC2405F },   // Rose
	};

	FLinearColor FromHex(uint32 Rgb)
	{
		return FLinearColor::FromSRGBColor(FColor(static_cast<uint8>((Rgb >> 16) & 0xFF), static_cast<uint8>((Rgb >> 8) & 0xFF), static_cast<uint8>(Rgb & 0xFF), 255));
	}

	const FEntry* Find(EDreamIcon Icon)
	{
		const int32 Index = static_cast<int32>(Icon);
		return (Index >= 0 && Index < static_cast<int32>(EDreamIcon::Count)) ? &Entries[Index] : nullptr;
	}
}

void FDreamIcons::Register(FSlateStyleSet& Style)
{
	const FVector2f Icon16(16.f, 16.f);
	for (int32 Index = 0; Index < static_cast<int32>(EDreamIcon::Count); ++Index)
	{
		const EDreamIcon Icon = static_cast<EDreamIcon>(Index);
		Style.Set(StyleKey(Icon), new FSlateVectorImageBrush(FilePath(Icon), Icon16));
	}
}

FName FDreamIcons::StyleKey(EDreamIcon Icon)
{
	const DreamIconsLocal::FEntry* Entry = DreamIconsLocal::Find(Icon);
	return Entry != nullptr ? FName(*FString::Printf(TEXT("Dream.Icon.%s"), Entry->Name)) : NAME_None;
}

const TCHAR* FDreamIcons::Name(EDreamIcon Icon)
{
	const DreamIconsLocal::FEntry* Entry = DreamIconsLocal::Find(Icon);
	return Entry != nullptr ? Entry->Name : TEXT("");
}

const TCHAR* FDreamIcons::FileName(EDreamIcon Icon)
{
	const DreamIconsLocal::FEntry* Entry = DreamIconsLocal::Find(Icon);
	return Entry != nullptr ? Entry->File : TEXT("");
}

FString FDreamIcons::GetIconsDir()
{
	// (icons) Phase 3 renames the plugin; this lookup follows the descriptor's name then.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor"));
	const FString Base = Plugin.IsValid() ? Plugin->GetBaseDir() : FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("DreamPythonEditor"));
	return FPaths::Combine(Base, TEXT("Resources"), TEXT("Icons"));
}

FString FDreamIcons::FilePath(EDreamIcon Icon)
{
	return FPaths::Combine(GetIconsDir(), FileName(Icon));
}

const FSlateBrush* FDreamIcons::Brush(EDreamIcon Icon)
{
	if (DreamIconsLocal::Find(Icon) == nullptr)
	{
		return nullptr;
	}
	const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(FDreamEditorStyle::GetStyleSetName());
	return Style != nullptr ? Style->GetOptionalBrush(StyleKey(Icon), nullptr, nullptr) : nullptr;
}

FSlateIcon FDreamIcons::SlateIcon(EDreamIcon Icon)
{
	return FSlateIcon(FDreamEditorStyle::GetStyleSetName(), StyleKey(Icon));
}

EDreamIconTint FDreamIcons::DefaultTint(EDreamIcon Icon)
{
	const DreamIconsLocal::FEntry* Entry = DreamIconsLocal::Find(Icon);
	return Entry != nullptr ? Entry->Tint : EDreamIconTint::Text;
}

FLinearColor FDreamIcons::TintColor(EDreamIconTint Tint)
{
	return TintColor(Tint, FDreamThemes::Current());
}

FLinearColor FDreamIcons::TintColor(EDreamIconTint Tint, const FDreamTheme& Theme)
{
	using S = EDreamThemeSlot;
	switch (Tint)
	{
	case EDreamIconTint::None: return FLinearColor::White;
	case EDreamIconTint::Text: return Theme.Get(S::ChromeText);
	case EDreamIconTint::Muted: return Theme.Get(S::ChromeMutedText);
	case EDreamIconTint::Accent: return Theme.Get(S::Accent);
	case EDreamIconTint::Danger: return Theme.Get(S::Danger);
	case EDreamIconTint::Error: return Theme.Get(S::OutError);
	case EDreamIconTint::Warning: return Theme.Get(S::OutWarning);
	case EDreamIconTint::Keyword: return Theme.Get(S::SynKeyword);
	case EDreamIconTint::Definition: return Theme.Get(S::SynDefinition);
	case EDreamIconTint::Builtin: return Theme.Get(S::SynBuiltin);
	case EDreamIconTint::Comment: return Theme.Get(S::SynComment);
	case EDreamIconTint::Moon:
	case EDreamIconTint::Mint:
	case EDreamIconTint::Lavender:
	case EDreamIconTint::Sky:
	case EDreamIconTint::Rose:
	{
		const DreamIconsLocal::FAccent& Accent = DreamIconsLocal::Accents[static_cast<int32>(Tint) - static_cast<int32>(EDreamIconTint::Moon)];
		return DreamIconsLocal::FromHex(Theme.bLight ? Accent.Light : Accent.Dark);
	}
	default: return Theme.Get(S::ChromeText);
	}
}

FSlateColor FDreamIcons::Tint(EDreamIcon Icon)
{
	return FSlateColor(TintColor(DefaultTint(Icon)));
}

TAttribute<FSlateColor> FDreamIcons::TintAttr(EDreamIcon Icon)
{
	return TAttribute<FSlateColor>::CreateLambda([Icon]() { return FDreamIcons::Tint(Icon); });
}
