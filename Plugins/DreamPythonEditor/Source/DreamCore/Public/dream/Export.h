// SPDX-License-Identifier: Apache-2.0
// Dream core: the export macro (TDD §2.3.4). Standard library only.
#pragma once

// DREAMCORE_API is defined by UBT for the engine build; the CMake build (tier 1) has no engine and exports nothing.
#ifndef DREAMCORE_API
#define DREAMCORE_API
#endif

// UBT defines DREAMCORE_API as DLLEXPORT (DreamCore) or DLLIMPORT (its users) in the module's Definitions.h, and the
// engine defines those two in HAL/Platform.h. DreamCore includes no engine header, so it supplies the same definitions
// when they are missing, token for token as Windows/WindowsPlatform.h:209-210 and Unix/UnixPlatform.h:124-125 have
// them, so a later engine definition is an identical (silent) redefinition.
#if !defined(DLLEXPORT)
#if defined(_MSC_VER)
#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)
#else
#define DLLEXPORT __attribute__((visibility("default")))
#define DLLIMPORT __attribute__((visibility("default")))
#endif
#endif

// Exported DreamCore classes hold std:: members (std::string, std::vector, std::unique_ptr). MSVC reports C4251 ("needs
// to have dll-interface") for each one when DREAMCORE_API is __declspec(dllexport) or dllimport, which UBT's DreamCore
// build compiles at /W4 with no engine header to silence it. DreamCore and its users are always built together with one
// toolset and one CRT, so the warning carries no risk here; the engine disables it the same way
// (Engine/Source/Runtime/Core/Public/Windows/WindowsPlatformCompilerSetup.h:142).
#if defined(_MSC_VER)
#pragma warning(disable : 4251)
#endif
