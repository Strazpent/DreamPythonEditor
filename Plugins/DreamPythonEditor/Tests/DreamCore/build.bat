@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Tier-1 build and run of DreamCore (TDD section 2.3.4, section 14.1) with a standalone MSVC toolset: 14.44 (the one
rem UBT builds the plugin with), else 14.38; DREAM_VCVARS_VER overrides the choice.
rem Compiles Source/DreamCore/Private/**/*.cpp except Private/UE/ (the engine adapter) plus Tests/DreamCore/**/*.cpp,
rem with no engine include path, so any engine include fails to compile (section 2.5 rule 1).
rem Before running, it writes the golden data with the engine's python.exe -I -B (DREAM_ENGINE_DIR overrides the engine
rem folder) unless --quick is given: T-CORE-039 extents (extent_golden.py over the engine's Lib), T-CORE-017 Python re
rem results (search_golden.py) and T-LEX-001 tokenize boundaries (lex_golden.py over the engine's Lib; reused from the
rem output folder while its stamp line matches, so only the first run there pays its minute).
rem Usage:  build.bat [--build-only] [--quick] [test-name-filter]
rem Output: Tests/DreamCore/out/dreamcore_tests.exe, objects and PDB in the same folder.
setlocal EnableExtensions EnableDelayedExpansion

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
for %%I in ("%HERE%\..\..") do set "PLUGIN=%%~fI"
set "CORE=%PLUGIN%\Source\DreamCore"
set "OUT=%HERE%\out"
rem DREAM_TIER1_OUT gives a private output folder (outside Tests\DreamCore), so two checkouts or two people can build at once.
if defined DREAM_TIER1_OUT set "OUT=%DREAM_TIER1_OUT%"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not defined DREAM_ENGINE_DIR set "DREAM_ENGINE_DIR=C:\Program Files\Epic Games\UE_5.8"
set "PYEXE=%DREAM_ENGINE_DIR%\Engine\Binaries\ThirdParty\Python3\Win64\python.exe"

set "RUN=1"
set "AST=1"
set "FILTER="
:args
if "%~1"=="" goto argsdone
if /I "%~1"=="--build-only" (
	set "RUN=0"
	shift
	goto args
)
if /I "%~1"=="--quick" (
	set "AST=0"
	shift
	goto args
)
set "FILTER=%~1"
shift
goto args
:argsdone

if not exist "%VCVARS%" (
	echo build.bat: vcvarsall.bat not found at "%VCVARS%"
	exit /b 2
)
rem vcvarsall looks for vswhere.exe on PATH; the installer folder holds it.
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "VCVER=%DREAM_VCVARS_VER%"
if not defined VCVER set "VCVER=14.44"
call "%VCVARS%" x64 -vcvars_ver=%VCVER% >nul 2>&1
if errorlevel 1 (
	if defined DREAM_VCVARS_VER (
		echo build.bat: vcvarsall x64 -vcvars_ver=%VCVER% failed
		exit /b 2
	)
	set "VCVER=14.38"
	call "%VCVARS%" x64 -vcvars_ver=14.38 >nul
	if errorlevel 1 (
		echo build.bat: vcvarsall x64 failed for 14.44 and 14.38
		exit /b 2
	)
)

if not exist "%OUT%" mkdir "%OUT%"

rem Collect sources: DreamCore Private (minus Private\UE) and the tests (minus out\).
set "SRC="
set "COUNT=0"
for /R "%CORE%\Private" %%F in (*.cpp) do (
	set "P=%%~fF"
	if "!P:\Private\UE\=!"=="!P!" (
		set SRC=!SRC! "!P!"
		set /a COUNT+=1
	)
)
for /R "%HERE%" %%F in (*.cpp) do (
	set "P=%%~fF"
	if "!P:\DreamCore\out\=!"=="!P!" (
		set SRC=!SRC! "!P!"
		set /a COUNT+=1
	)
)
echo build.bat: compiling !COUNT! translation units with MSVC %VCVER%

rem Flags mirror the UBT build of DreamCore: C++20, no exceptions (_HAS_EXCEPTIONS=0 as UBT defines), no RTTI,
rem warnings as errors at /W4, plus the warnings UBT makes errors (shadowing 4456/4458/4459, 4668, unreachable 4702),
rem with system headers external at /W0 as UBT has them.
cl /nologo /std:c++20 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /W4 /WX /we4456 /we4458 /we4459 /we4668 /we4702 /external:anglebrackets /external:W0 ^
	/EHs-c- /GR- /D_HAS_EXCEPTIONS=0 /DNOMINMAX /O2 /Zi /MD /MP ^
	/I "%CORE%\Public" /I "%CORE%\Private" /I "%HERE%" ^
	/Fo"%OUT%\\" /Fd"%OUT%\dreamcore_tests.pdb" /Fe"%OUT%\dreamcore_tests.exe" ^
	!SRC! /link /DEBUG /INCREMENTAL:NO
if errorlevel 1 (
	echo build.bat: compile or link failed
	exit /b 1
)

if "%RUN%"=="0" exit /b 0
set "DREAM_EXTENT_GOLDEN="
set "DREAM_SEARCH_GOLDEN="
set "DREAM_LEX_GOLDEN="
set "PYLIB=%DREAM_ENGINE_DIR%\Engine\Binaries\ThirdParty\Python3\Win64\Lib"
if "%AST%"=="1" if exist "%PYEXE%" (
	"%PYEXE%" -I -B "%HERE%\extent_golden.py" "%PYLIB%" "%OUT%\extent_golden.tsv" <nul >nul 2>&1
	if not errorlevel 1 set "DREAM_EXTENT_GOLDEN=%OUT%\extent_golden.tsv"
	"%PYEXE%" -I -B "%HERE%\search_golden.py" "%OUT%\search_golden.tsv" <nul >nul 2>&1
	if not errorlevel 1 set "DREAM_SEARCH_GOLDEN=%OUT%\search_golden.tsv"
	"%PYEXE%" -I -B "%HERE%\lex_golden.py" "%PYLIB%" "%OUT%\lex_golden.tsv" <nul >nul 2>&1
	if not errorlevel 1 set "DREAM_LEX_GOLDEN=%OUT%\lex_golden.tsv"
)
"%OUT%\dreamcore_tests.exe" %FILTER%
exit /b %ERRORLEVEL%
