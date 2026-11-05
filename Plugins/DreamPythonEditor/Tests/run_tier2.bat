@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Tier 2 (TDD section 14.1): the dream_editor runner under the engine's CPython 3.11 with the null _dream.
rem Runs unittest discovery over Tests\python\test_*.py with python.exe -I -B (no user site, no bytecode files).
rem Usage:  run_tier2.bat [-v] [-k name-substring]
rem The engine root defaults to the local UE 5.8 install; set UE_ROOT to use another.
setlocal EnableExtensions

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
set "PY=%UE_ROOT%\Engine\Binaries\ThirdParty\Python3\Win64\python.exe"

if not exist "%PY%" (
	echo run_tier2.bat: engine python not found at "%PY%"
	exit /b 2
)

"%PY%" -I -B "%HERE%\python\run_tier2.py" %*
exit /b %ERRORLEVEL%
