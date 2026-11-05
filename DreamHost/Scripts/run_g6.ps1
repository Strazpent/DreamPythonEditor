# SPDX-License-Identifier: Apache-2.0
# G6 driver B prototype (TDD 10.10, T-PER-036, T-E2E-010): the DreamHost editor, killed with TerminateProcess (kill -9)
# while it types into a Dream scratch tab, and once while a long run executes; every next start restores through
# FDreamJournal::Open and SDreamTab::RestoreSession and checks the recovered text against the sidecar the typing session
# wrote before every keystroke: I1 (the journal reads), I2 (no more than 2 s of typing lost), I3 (the buffer is listed),
# I4 (the text is exactly one typed state) and I5 (the run active at the kill is reported lost).
# Each session runs the console command Dream.Test.G6Step (Source/DreamEditorTests/Private/Specs/DreamG6DriverSpec.cpp) with
# -DreamG6Step=<k>, not an automation test: the tier-4 pass hygiene would discard the very recoveries it checks. Each
# session journals to a private storage root outside the project (-DreamStorageRoot=%TEMP%\DreamG6-<stamp>, this session
# only; TDD 10.1 refuses a root inside the project tree): the user's own DreamHost journal and storage settings are never
# touched. DreamHost's EditorPerProjectUserSettings.ini (the tab list) is restored exactly as it was afterwards.
# The full G6 gate (10,000 editor kills on 10 agents, RV-15) is moved; this runs a small count.
#
# Needs the UE lock (it launches DreamHost editors) and an interactive desktop (the editor window opens the Dream tab).
# Usage:  powershell -ExecutionPolicy Bypass -File DreamHost\Scripts\run_g6.ps1 [-Kills 5] [-RunKill 3] [-Seed 1]
# Exit code 0 when every step passed. It kills only editors it started, and only when their command line names
# DreamHost.uproject. The engine root defaults to the local UE 5.8 install; set UE_ROOT to use another.
param(
	[int]$Kills = 5,
	[int]$RunKill = 3,
	[int]$Seed = 1,
	[int]$TimeoutMinutes = 5
)

$host_ = Split-Path -Parent $PSScriptRoot
$engine = if ($env:UE_ROOT) { $env:UE_ROOT } else { "C:\Program Files\Epic Games\UE_5.8" }
$editor = Join-Path $engine "Engine\Binaries\Win64\UnrealEditor.exe"
$project = Join-Path $host_ "DreamHost.uproject"
$out = Join-Path $host_ "Saved\Automation\G6"
# (scratch, PO-05) New unnamed tabs are scratch files: DreamHost's test folder, never the user's real one. It starts empty
# (R-p1-6) and is shared by the steps.
$scratch = Join-Path $host_ "Saved\DreamTests\Scratches"
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("DreamG6-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
if (-not (Test-Path $editor)) { "run_g6: editor not found at $editor"; exit 2 }

# What the G6 sessions change in DreamHost's EditorPerProjectUserSettings.ini is Dream's session section (the tab list, the
# active tab, the Untitled counter, the output split), so that section, and only it, is backed up and put back: a whole-file
# restore would roll back whatever else changed in the ini meanwhile (review r4). The backup lives outside $out, which every
# run deletes: a run killed before its finally leaves its backup (or the marker that no ini existed), and the next run puts
# that section back before anything else.
$projectIni = Join-Path $host_ "Saved\Config\WindowsEditor\EditorPerProjectUserSettings.ini"
$section = "/Script/DreamEditor.DreamPerProjectUserSettings"
$iniKeep = Join-Path $host_ "Saved\Automation\G6-ini"
$sectionBackup = Join-Path $iniKeep "DreamPerProjectUserSettings.section"   # the section's lines (empty: it was absent)
$iniAbsent = Join-Path $iniKeep "ini-was-absent.txt"

# An ini as UE writes it (UTF-16 with a BOM, or UTF-8): its lines and its encoding.
function Read-Ini([string]$Path) {
	$raw = [System.IO.File]::ReadAllBytes($Path)
	$encoding = New-Object System.Text.UTF8Encoding($false)
	if ($raw.Length -ge 2 -and $raw[0] -eq 0xFF -and $raw[1] -eq 0xFE) { $encoding = [System.Text.Encoding]::Unicode }
	$text = $encoding.GetString($raw)
	if ($encoding -eq [System.Text.Encoding]::Unicode) { $text = $text.TrimStart([char]0xFEFF) }
	return @{ Lines = @($text -split "`r?`n"); Encoding = $encoding }
}

function Write-Ini([string]$Path, $Lines, $Encoding) {
	$bytes = $Encoding.GetBytes(($Lines -join "`r`n"))
	if ($Encoding -eq [System.Text.Encoding]::Unicode) { $bytes = [byte[]](0xFF, 0xFE) + $bytes }
	[System.IO.File]::WriteAllBytes($Path, $bytes)
}

# The lines of Section (without its header), or an empty list when the ini or the section is absent.
function Get-IniSection([string]$Path, [string]$Name) {
	$found = New-Object System.Collections.Generic.List[string]
	if (-not (Test-Path $Path)) { return ,$found }
	$inSection = $false
	foreach ($line in (Read-Ini $Path).Lines) {
		if ($line -match '^\[(.*)\]\s*$') { $inSection = ($Matches[1] -eq $Name); continue }
		if ($inSection) { $found.Add($line) }
	}
	return ,$found
}

# Replaces Section's lines with Body in place (an empty Body removes the section; a missing section is appended), keeping
# every other line of the ini as it is.
function Set-IniSection([string]$Path, [string]$Name, $Body) {
	if (-not (Test-Path $Path)) {
		if ($Body.Count -eq 0) { return }
		Write-Ini $Path (@("[$Name]") + $Body) (New-Object System.Text.UTF8Encoding($false))
		return
	}
	$ini = Read-Ini $Path
	$out = New-Object System.Collections.Generic.List[string]
	$inSection = $false
	$placed = ($Body.Count -eq 0)
	foreach ($line in $ini.Lines) {
		if ($line -match '^\[(.*)\]\s*$') {
			$inSection = ($Matches[1] -eq $Name)
			if ($inSection) {
				if (-not $placed) { $out.Add($line); foreach ($b in $Body) { $out.Add($b) }; $placed = $true }
				continue
			}
		}
		if (-not $inSection) { $out.Add($line) }
	}
	if (-not $placed) { $out.Add("[$Name]"); foreach ($b in $Body) { $out.Add($b) } }
	Write-Ini $Path $out $ini.Encoding
}

function Restore-ProjectIni([string]$Why) {
	if (Test-Path $iniAbsent) {
		# There was no ini at all: everything in it now came from the driver's sessions.
		if (Test-Path $projectIni) { Remove-Item -Force $projectIni }
		Remove-Item -Force $iniAbsent
		if (Test-Path $sectionBackup) { Remove-Item -Force $sectionBackup }
		Write-Host "run_g6: removed the $projectIni the driver created ($Why)"   # (icons) Write-Host: called in finally
	}
	elseif (Test-Path $sectionBackup) {
		$body = New-Object System.Collections.Generic.List[string]
		foreach ($line in [System.IO.File]::ReadAllLines($sectionBackup)) { $body.Add($line) }
		Set-IniSection $projectIni $section $body
		Remove-Item -Force $sectionBackup
		Write-Host "run_g6: restored [$section] in $projectIni ($Why)"   # (icons) Write-Host: called in finally
	}
}
Restore-ProjectIni "left by a run that was killed before it finished"

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
New-Item -ItemType Directory -Force $iniKeep | Out-Null
if (Test-Path $projectIni) {
	[System.IO.File]::WriteAllLines($sectionBackup, [string[]](Get-IniSection $projectIni $section))
}
else {
	Set-Content -Path $iniAbsent -Value "absent" -Encoding ascii
}

$rng = New-Object System.Random($Seed)

function Get-UtcMs { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }

function Stop-DreamHostEditor([System.Diagnostics.Process]$Process) {
	# Only an editor this driver started, and only when its command line names DreamHost.uproject.
	$info = Get-CimInstance Win32_Process -Filter "ProcessId = $($Process.Id)"
	if ($info -and $info.Name -like 'UnrealEditor*' -and $info.CommandLine -like '*DreamHost.uproject*') {
		Stop-Process -Id $Process.Id -Force   # TerminateProcess: kill -9 (no OnPreExit, no CLEAN)
		return $true
	}
	return $false
}

function Wait-ForFile([string]$Path, [System.Diagnostics.Process]$Process, [datetime]$Deadline) {
	while (-not (Test-Path $Path)) {
		if ($Process.HasExited -or (Get-Date) -gt $Deadline) { return $false }
		Start-Sleep -Milliseconds 200
	}
	return $true
}

function Read-KeyValues([string]$Path) {
	$values = @{}
	if (Test-Path $Path) {
		foreach ($line in Get-Content $Path) {
			$eq = $line.IndexOf('=')
			if ($eq -gt 0) { $values[$line.Substring(0, $eq)] = $line.Substring($eq + 1) }
		}
	}
	return $values
}

# (icons) R-q1-3: each step's editor runs under Tests\ci\editor_layout.py run (the fixed test layout, the Dream tab docked;
# the user's EditorLayout.json is put back byte-identical when that editor is gone: exited, killed by a step, or after Ctrl+C).
. (Join-Path $PSScriptRoot "editor_layout.ps1")
$run = $null

$failures = @()
$summary = @()
$p = $null
try {
	for ($step = 1; $step -le $Kills + 1; $step++) {
		$log = Join-Path $out ("editor-step{0}.log" -f $step)
		$argList = @("`"$project`"", "-ExecCmds=`"Dream.Test.G6Step`"", "-DreamG6Step=$step",
			"-DreamG6Kills=$Kills", "-DreamG6RunStep=$RunKill", "-DreamG6Dir=`"$out`"", "-DreamStorageRoot=`"$root`"",
			"-nosplash", "-NoSound", "-log", "-abslog=`"$log`"", "-DreamScratchFolder=`"$scratch`"",
			"-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False")
		"run_g6: step $step"
		$run = Start-EditorUnderTestLayout $editor $argList
		$p = $run.Editor
		if (-not $p) { $failures += "step ${step}: the editor did not start under the layout wrapper"; break }
		$deadline = (Get-Date).AddMinutes($TimeoutMinutes)

		if ($step -le $Kills) {
			$signal = if ($step -eq $RunKill) { Join-Path $out ("run{0}.txt" -f $step) } else { Join-Path $out ("typing{0}.txt" -f $step) }
			if (-not (Wait-ForFile $signal $p $deadline)) {
				$failures += "step ${step}: no signal $signal (see $log)"
				$notKilled = Join-Path $out ("notkilled{0}.txt" -f $step)
				if (Test-Path $notKilled) { foreach ($line in Get-Content $notKilled) { if ($line -like 'error=*') { $failures += "step ${step}: $line" } } }
				if (-not $p.HasExited) { Stop-DreamHostEditor $p | Out-Null }
				break
			}
			# A random point: 3-8 s into the typing (so the buffer existed 2 s before the kill, I3), or 1-3 s into the run.
			$delayMs = if ($step -eq $RunKill) { $rng.Next(1000, 3000) } else { $rng.Next(3000, 8000) }
			Start-Sleep -Milliseconds $delayMs
			$killMs = Get-UtcMs
			Set-Content -Path (Join-Path $out ("kill{0}.txt" -f $step)) -Value $killMs -Encoding ascii
			if (-not (Stop-DreamHostEditor $p)) { $failures += "step ${step}: refused to kill pid $($p.Id)"; break }
			$p.WaitForExit(30000) | Out-Null
			$typing = Read-KeyValues (Join-Path $out ("typing{0}.txt" -f $step))
			$summary += "step ${step}: killed after $delayMs ms" + $(if ($step -eq $RunKill) { " of the run" } else { " of typing" }) +
				" (keystrokes $($typing['keystrokes']), model fallbacks $($typing['fallbacks']))"
		}
		else {
			while (-not $p.HasExited) {
				if ((Get-Date) -gt $deadline) {
					Stop-DreamHostEditor $p | Out-Null
					$failures += "step ${step}: the editor did not exit within $TimeoutMinutes min"
					break
				}
				Start-Sleep -Seconds 1
			}
			$p.WaitForExit(10000) | Out-Null
			$final = Read-KeyValues (Join-Path $out "final.txt")
			if ($final['ok'] -ne '1') { $failures += "step ${step}: final.txt not ok (see $out\final.txt and $log)" }
		}

		$null = $run.Wrapper.WaitForExit(60000)   # (icons) the wrapper restores the user's layout after the editor
		if ($run.Wrapper.ExitCode -eq 75) { $failures += "step ${step}: the user's editor layout was not restored"; break }
		if ($step -gt 1) {
			$verifyPath = Join-Path $out ("verify{0}.txt" -f $step)
			$verify = Read-KeyValues $verifyPath
			if ($verify['ok'] -ne '1') { $failures += "step ${step}: verify not ok (see $verifyPath and $log)" }
			$summary += "  verify step ${step}: recovered $($verify['recovered_chars']) of $($verify['typed_chars']) typed chars, lost $($verify['lost_ms']) ms" +
				$(if ($verify.ContainsKey('lost_run')) { ", lost run reported $($verify['lost_run'])" } else { "" })
		}
		$bad = Get-Content $log -ErrorAction SilentlyContinue | Select-String -Pattern 'Ensure condition failed|=== Critical error|Fatal error|LogDream: Error'
		foreach ($line in $bad) { $failures += "step ${step} log: $($line.Line)" }
		if ($failures.Count -gt 0) { break }
	}
}
finally {
	# (icons) Review q1 M1: restore first, Write-Host only. Complete-TestLayout stops the step's editor if it still runs (only
	# the one this driver started, so it cannot write the G6 tab list or use $root after the restores), waits for the wrapper
	# and restores the user's layout; then the project ini section (review r4).
	$layoutOk = Complete-TestLayout $run
	Restore-ProjectIni "this run"
	if (-not $layoutOk) { $failures += "the user's editor layout was NOT restored (kept as %LOCALAPPDATA%\UnrealEngine\Editor\EditorLayout.json.dream-backup)" }
	if ($failures.Count -eq 0 -and (Test-Path $root)) { Remove-Item -Recurse -Force $root }   # kept for inspection on a failure
}

foreach ($line in $summary) { "run_g6: $line" }
if ($failures.Count -eq 0) { "run_g6: passed ($Kills kills, run kill at step $RunKill)"; exit 0 }
foreach ($f in $failures) { "run_g6: FAILED: $f" }
exit 1
