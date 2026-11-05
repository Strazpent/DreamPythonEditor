# SPDX-License-Identifier: Apache-2.0
# Normal-exit smoke for the Dream plugin: an attended DreamHost editor opens the Dream tab, runs shutdown_smoke.py through
# Dream.RunFile, and that script asks for a normal exit, so OnPreExit and ShutdownModule run. A tier-4 pass cannot check
# this, because the automation Quit force-exits. Opens an editor window (minimized) for about a minute.
#
# Usage:  powershell -ExecutionPolicy Bypass -File DreamHost\Scripts\run_smoke.ps1 [-TimeoutMinutes 10] [-Log <path>]
# Passes when the editor exits 0 after the run and the full LogExit sequence, with no Ensure, Fatal, LogDream Error or
# LogPython Error line. On a timeout it kills only the editor it started ((icons) review q1 m4).
# The engine root defaults to the local UE 5.8 install; set UE_ROOT to use another.
param(
	[int]$TimeoutMinutes = 10,
	[string]$Log = ""
)

$host_ = Split-Path -Parent $PSScriptRoot
$engine = if ($env:UE_ROOT) { $env:UE_ROOT } else { "C:\Program Files\Epic Games\UE_5.8" }
$editor = Join-Path $engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$project = Join-Path $host_ "DreamHost.uproject"
$script = Join-Path $PSScriptRoot "shutdown_smoke.py"
if ($Log -eq "") { $Log = Join-Path $host_ "Saved\Automation\Smoke\editor.log" }
if (-not (Test-Path $editor)) { "run_smoke: editor not found at $editor"; exit 2 }
New-Item -ItemType Directory -Force (Split-Path -Parent $Log) | Out-Null
if (Test-Path $Log) { Remove-Item $Log -Force }

# (scratch, PO-05) A new unnamed tab is a scratch file: in DreamHost's test folder, never the user's real one.
$scratch = Join-Path $host_ "Saved\DreamTests\Scratches"
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }   # (scratch, R-p1-6) it starts empty
$argList = @("`"$project`"", "-ExecCmds=`"Dream.Open,Dream.RunFile $script`"", "-nosplash", "-NoSound", "-log", "-DreamScratchFolder=`"$scratch`"",
	"-LogCmds=`"LogDream Verbose`"", "-abslog=`"$Log`"",
	"-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False")
# (icons) R-q1-3: the editor runs under Tests\ci\editor_layout.py run (the fixed test layout, the Dream tab docked; the user's
# EditorLayout.json is put back byte-identical when the editor is gone, also after Ctrl+C). DreamHost\Scripts\editor_layout.ps1.
. (Join-Path $PSScriptRoot "editor_layout.ps1")
$layoutRestored = $false
$run = $null
$p = $null
$code = $null
$timedOut = $false
try {
	$run = Start-EditorUnderTestLayout $editor $argList -Minimized
	$p = $run.Editor
	if (-not $p) { Write-Host "run_smoke: the editor did not start under the layout wrapper" }
	else {
		$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
		while (-not $p.HasExited) {
			if ((Get-Date) -gt $deadline) {
				$timedOut = $true
				# Review q1 m4: only the editor this driver started, never another DreamHost editor (the owner's).
				Write-Host "run_smoke: timeout after $TimeoutMinutes min; killing the editor it started (pid $($p.Id))"
				if (Test-OwnDreamHostEditor $p) { Stop-Process -Id $p.Id -Force }
				break
			}
			Start-Sleep -Seconds 3
		}
		$null = $p.WaitForExit(10000)
		$null = $run.Wrapper.WaitForExit(60000)
		$code = $run.Wrapper.ExitCode   # the editor's exit code, passed through (75: the layout was not restored)
	}
}
finally {
	# Restore first (review q1 M1): Complete-TestLayout stops the editor if it still runs and prints with Write-Host only.
	$layoutRestored = Complete-TestLayout $run
}

$failures = @()
if (-not $layoutRestored -or $code -eq 75) { $failures += "the user's editor layout was NOT restored (kept as %LOCALAPPDATA%\UnrealEngine\Editor\EditorLayout.json.dream-backup)" }   # (icons)
if ($timedOut) { $failures += "the editor did not exit within $TimeoutMinutes min" }
elseif ($null -eq $code) { $failures += "the editor did not start" }
elseif ($code -ne 0) { $failures += "editor exit code $code" }
if (-not (Test-Path $Log)) {
	$failures += "no log at $Log"
} else {
	$text = Get-Content $Log
	foreach ($needle in @("dream-shutdown-smoke: ran inside Dream", "LogExit: Preparing to exit.", "LogExit: Editor shut down", "LogExit: Exiting.")) {
		if (-not ($text | Select-String -SimpleMatch $needle -Quiet)) { $failures += "missing log line: $needle" }
	}
	$bad = $text | Select-String -Pattern 'Ensure condition failed|LogWindows: Error: === Critical error|Fatal error|LogDream: Error|LogPython: Error'
	foreach ($line in $bad) { $failures += "log: $($line.Line)" }
}

"run_smoke: editor exit $code; log $Log"
if ($failures.Count -eq 0) { "run_smoke: passed"; exit 0 }
foreach ($f in $failures) { "run_smoke: FAILED: $f" }
exit 1
