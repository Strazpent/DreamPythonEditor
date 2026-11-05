@echo off
rem SPDX-License-Identifier: Apache-2.0
rem UE build of the plugin through the host project (TDD section 2.3, ARCHITECTURE.md section 8): DreamHostEditor Win64
rem Development, which compiles DreamCore, DreamEditor and DreamEditorTests with UBT's MSVC toolchain.
rem Usage:  run_build.bat [extra UBT arguments, for example -SingleFile=<path> or -FileList=<list>]
rem The engine root defaults to the local UE 5.8 install; set UE_ROOT to use another.
rem UBT runs at most half the logical cores (-MaxParallelActions), so the machine stays usable while it builds
rem (owner instruction); set DREAM_BUILD_JOBS=<n> to choose the number of parallel actions.
setlocal EnableExtensions

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
for %%I in ("%HERE%\..\..\..\DreamHost\DreamHost.uproject") do set "PROJECT=%%~fI"
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
set "BUILD=%UE_ROOT%\Engine\Build\BatchFiles\Build.bat"

if not exist "%BUILD%" (
	echo run_build.bat: Build.bat not found at "%BUILD%"
	exit /b 2
)
if not exist "%PROJECT%" (
	echo run_build.bat: host project not found at "%PROJECT%"
	exit /b 2
)

if not defined DREAM_BUILD_JOBS set /a DREAM_BUILD_JOBS=%NUMBER_OF_PROCESSORS% / 2
if "%DREAM_BUILD_JOBS%"=="0" set "DREAM_BUILD_JOBS=1"
call "%BUILD%" DreamHostEditor Win64 Development "-Project=%PROJECT%" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=%DREAM_BUILD_JOBS% %*
exit /b %ERRORLEVEL%
