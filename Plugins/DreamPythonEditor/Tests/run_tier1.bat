@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Tier 1 (TDD section 14.1): the static source checks of section 2.5 (Tests/ci, on the engine's python.exe -I -B),
rem then the standalone DreamCore build and its tests (Tests/DreamCore/build.bat). Both run; the exit code is non-zero
rem when either fails.
rem Usage:  run_tier1.bat [--scans-only] [build.bat arguments: --build-only, --quick, a test-name filter]
rem --quick skips the T-CORE-039 golden data step (about 30 s of the engine's python.exe); a plain run includes it.
rem The engine root defaults to the local UE 5.8 install; set UE_ROOT to use another.
setlocal EnableExtensions

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
set "PY=%UE_ROOT%\Engine\Binaries\ThirdParty\Python3\Win64\python.exe"

set "CORE=1"
if /I "%~1"=="--scans-only" (
	set "CORE=0"
	shift
)

if not exist "%PY%" (
	echo run_tier1.bat: engine python not found at "%PY%"
	exit /b 2
)

"%PY%" -I -B "%HERE%\ci\dream_ci_scans.py"
set "SCANS=%ERRORLEVEL%"

set "TESTS=0"
if "%CORE%"=="0" goto report
call "%HERE%\DreamCore\build.bat" %1 %2 %3
set "TESTS=%ERRORLEVEL%"

:report
echo run_tier1.bat: scans exit %SCANS%, DreamCore exit %TESTS%
if not "%SCANS%"=="0" exit /b 1
if not "%TESTS%"=="0" exit /b 1
exit /b 0
