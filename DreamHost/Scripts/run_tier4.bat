@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Tier 4 (TDD section 14.1): the Dream automation specs (Source/DreamEditorTests, "Dream." prefix) in the DreamHost editor.
rem The editor's exit code does not carry the test result (the automation Quit exits 0), so the exported report
rem (index.json) is checked afterwards by Tests/ci/check_automation_report.py.
rem
rem Usage:  run_tier4.bat [--attended] [--build] [--filter <prefix>] [--report <dir>] [--args "<editor args>"]
rem   (default)   UnrealEditor-Cmd.exe -ExecCmds="Automation RunTests Dream;Quit" -unattended -nosplash -nullrhi -NoSound
rem               -log -stdout -ReportExportPath=<dir>. Dream is Headless under -unattended or -nullrhi (TDD 3.4, 3.5),
rem               so this pass runs Dream.Lifecycle and the specs that need neither the tab nor a run (Dream.Repl.History,
rem               Dream.Supervisor.SourceText): every spec that opens the tab or runs code carries NonNullRHI, which the
rem               automation framework filters out of a -nullrhi session.
rem   --attended  the same without -unattended and -nullrhi, as the GPU agents run tier 4 (TDD 14.1): every Dream spec
rem               runs. Needs an interactive desktop; the editor window opens for the duration of the run.
rem               WARNING: it takes over the desktop (an editor window in front, real mouse and key input, a blocking
rem               Paused window; the G2 pass alone takes about 40 minutes). Opt-in only: the architect asks the owner
rem               before any attended pass (owner instruction, PO-06 phase 3).
rem   (every pass) the editor runs at BelowNormal priority (Tests\ci\editor_layout.py; DREAM_EDITOR_PRIORITY=normal for Normal).
rem   --build     builds DreamHostEditor first (Plugins/DreamPythonEditor/Tests/run_build.bat).
rem   --filter    automation filter, default "Dream" (for example "Dream.Run" or "Dream.Supervisor.Stop"); several are
rem               joined with "+" (for example "Dream.Keys+Dream.Output.Follow"), one editor launch for all of them;
rem               every part must match at least one test that ran, or the pass fails.
rem   --report    report folder, default DreamHost/Saved/Automation/Tier4/<mode>.
rem   (every pass) -DreamScratchFolder=<DreamHost>\Saved\DreamTests\Scratches: new unnamed tabs are scratch files there.
rem   --args      extra editor arguments for this pass only, quoted, for example --args "-DreamG2Stops=20" (a shorter
rem               G2 cycle count) or --args "-DreamRestartStep=2" (the restart driver's steps). Dream.Lifecycle.Dynamic
rem               (RUN-10, T-LIFE-006) needs its own session: --attended --filter Dream.Lifecycle.Dynamic --args
rem               "-ini:Engine:[ConsoleVariables]:Engine.Python.IsPythonLifeCycleDynamic=1 -DreamLifeCycles=100".
rem The engine root defaults to the local UE 5.8 install; set UE_ROOT to use another.
setlocal EnableExtensions

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
for %%I in ("%HERE%\..") do set "HOST=%%~fI"
for %%I in ("%HOST%\..\Plugins\DreamPythonEditor") do set "PLUGIN=%%~fI"
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
set "EDITOR=%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
set "PY=%UE_ROOT%\Engine\Binaries\ThirdParty\Python3\Win64\python.exe"
set "PROJECT=%HOST%\DreamHost.uproject"

set "MODE=headless"
set "BUILD=0"
set "FILTER=Dream"
set "REPORT="
set "EXTRA="

:parse
if "%~1"=="" goto parsed
if /I "%~1"=="--attended" (
	set "MODE=attended"
	shift
	goto parse
)
if /I "%~1"=="--build" (
	set "BUILD=1"
	shift
	goto parse
)
if /I "%~1"=="--filter" (
	set "FILTER=%~2"
	shift
	shift
	goto parse
)
if /I "%~1"=="--report" (
	set "REPORT=%~2"
	shift
	shift
	goto parse
)
if /I "%~1"=="--args" (
	set "EXTRA=%~2"
	shift
	shift
	goto parse
)
echo run_tier4.bat: unknown argument "%~1"
exit /b 2
:parsed

if not exist "%EDITOR%" (
	echo run_tier4.bat: UnrealEditor-Cmd.exe not found at "%EDITOR%"
	exit /b 2
)
if not exist "%PY%" (
	echo run_tier4.bat: engine python not found at "%PY%"
	exit /b 2
)
if not defined REPORT set "REPORT=%HOST%\Saved\Automation\Tier4\%MODE%"

if "%BUILD%"=="1" (
	call "%PLUGIN%\Tests\run_build.bat"
	if errorlevel 1 (
		echo run_tier4.bat: build failed
		exit /b 1
	)
)

rem A stale report must not pass for this run.
if exist "%REPORT%" rmdir /s /q "%REPORT%"
mkdir "%REPORT%"
rem (scratch, R-p1-6) The tests' own scratch folder starts empty in every pass (it is never the user's folder).
if exist "%HOST%\Saved\DreamTests\Scratches" rmdir /s /q "%HOST%\Saved\DreamTests\Scratches"

set "FLAGS=-nosplash -NoSound -log -stdout"
rem (scratch, PO-05) New unnamed tabs are scratch files: a pass makes them in DreamHost's own folder, never in the user's
rem real scratch folder (%%LOCALAPPDATA%%\Dream\Scratches or the Scratch folder setting).
set "FLAGS=%FLAGS% -DreamScratchFolder="%HOST%\Saved\DreamTests\Scratches""
if /I "%MODE%"=="headless" set "FLAGS=-unattended -nullrhi %FLAGS%"
rem An attended editor that is not the foreground window throttles to about 3 FPS ("Use Less CPU when in Background"), and
rem the automation controller waits up to 600 s for 10 FPS before the first test (FWaitForInteractiveFrameRate). This
rem session-only override keeps a run started from a terminal from idling those 10 minutes; the user's settings are untouched.
if /I "%MODE%"=="attended" echo run_tier4.bat: ATTENDED pass: this takes over the desktop until it ends.
if /I "%MODE%"=="attended" set "FLAGS=%FLAGS% -ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False"

rem (icons) R-q1-3: the editor runs under Tests\ci\editor_layout.py run, which owns its lifetime: it puts the fixed test
rem layout in place (Tests\ci\EditorLayout.tier4.json, the Dream tab docked), starts the editor with this exact command line,
rem waits for it and puts the user's %%LOCALAPPDATA%%\UnrealEngine\Editor\EditorLayout.json back byte-identical in its own
rem finally. It ignores Ctrl+C, so "Terminate batch job?" can no longer skip the restore (review q1 M2); a backup left by a
rem closed console is restored at the next start. Some specs assume the docked placement (Dream.Shell.Chrome clicks and grip,
rem Dream.Visual.Capture pixels, Dream.CodeView.TabFocus). Exit 75: the user's layout could not be restored.
echo run_tier4.bat: %MODE% pass, filter "%FILTER%", report "%REPORT%" %EXTRA%
"%PY%" -I -B "%PLUGIN%\Tests\ci\editor_layout.py" run -- "%EDITOR%" "%PROJECT%" -ExecCmds="Automation RunTests %FILTER%;Quit" %FLAGS% %EXTRA% -ReportExportPath="%REPORT%" -abslog="%REPORT%\editor.log"
set "EDITOR_EXIT=%ERRORLEVEL%"
echo run_tier4.bat: editor exited with %EDITOR_EXIT%
if "%EDITOR_EXIT%"=="75" (
	echo run_tier4.bat: the user's editor layout was NOT restored; it is kept as "%LOCALAPPDATA%\UnrealEngine\Editor\EditorLayout.json.dream-backup"
	exit /b 1
)

rem The full pass must have reached the specs its mode can run: Dream.Lifecycle always, the attended specs with --attended.
set "REQUIRE="
if /I "%FILTER%"=="Dream" (
	if /I "%MODE%"=="headless" (
		set "REQUIRE=--require Dream.Lifecycle"
	) else (
		set "REQUIRE=--require Dream.Lifecycle --require Dream.Shell.TabLayout --require Dream.Supervisor.Stop"
	)
)
"%PY%" -I -B "%PLUGIN%\Tests\ci\check_automation_report.py" "%REPORT%" --prefix "%FILTER%" %REQUIRE%
exit /b %ERRORLEVEL%
