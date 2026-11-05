@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Tier 4 (TDD section 14.1): forwards to the host project's runner, DreamHost/Scripts/run_tier4.bat, which starts
rem UnrealEditor-Cmd.exe on DreamHost.uproject with "Automation RunTests Dream;Quit" and checks the exported report.
rem Usage:  run_tier4.bat [--attended] [--build] [--filter <prefix>] [--report <dir>]   (see the host script)
setlocal EnableExtensions

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
for %%I in ("%HERE%\..\..\..\DreamHost\Scripts\run_tier4.bat") do set "HOST_RUNNER=%%~fI"

if not exist "%HOST_RUNNER%" (
	echo run_tier4.bat: host runner not found at "%HOST_RUNNER%"
	exit /b 2
)
call "%HOST_RUNNER%" %*
exit /b %ERRORLEVEL%
