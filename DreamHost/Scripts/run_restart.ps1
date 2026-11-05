# SPDX-License-Identifier: Apache-2.0
# Restart driver for T-GAP-019 (TDD 14.6: the output area's position and height survive restarts; the fresh profile shows
# the exact 40 % split). It starts the DreamHost editor once per step of the Dream.Restart.Layout spec
# (Source/DreamEditorTests/Private/Specs/DreamRestartLayoutSpec.cpp): each session runs the step with
# -DreamRestartStep=<n> -DreamRestartDir=<dir>, writes <dir>/step<n>.txt and exits normally, so OnPreExit saves the session
# state as a user's exit does. Before step 1 the driver gives the editor a fresh layout profile by removing the layout
# keys (OutputPosition in the user-global EditorSettings.ini, OutputAreaFraction in DreamHost's
# EditorPerProjectUserSettings.ini); afterwards it puts those two keys back as they were
# (the two keys only, from backups kept in Saved\Automation\RestartBackup, which the next run restores first if this
# one was killed).
#
# Needs the UE lock (it launches DreamHost editors) and an unlocked interactive desktop (steps 3 and 4 drag the splitter
# with the input bot). Opens an editor window five times, about a minute each.
#
# Usage:  powershell -ExecutionPolicy Bypass -File DreamHost\Scripts\run_restart.ps1 [-Steps 5] [-TimeoutMinutes 6]
# Exit code 0 when every step passed; on a timeout (or Ctrl+C) it kills only the editor it started, after checking that
# its command line names DreamHost.uproject.
# The engine root defaults to the local UE 5.8 install; set UE_ROOT to use another.
param(
	[int]$Steps = 5,
	[int]$TimeoutMinutes = 6
)

$host_ = Split-Path -Parent $PSScriptRoot
$engine = if ($env:UE_ROOT) { $env:UE_ROOT } else { "C:\Program Files\Epic Games\UE_5.8" }
$editor = Join-Path $engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$project = Join-Path $host_ "DreamHost.uproject"
$out = Join-Path $host_ "Saved\Automation\Restart"
# (scratch, PO-05) The steps' scratch files (new unnamed tabs) go to DreamHost's test folder, never the user's real one. It
# starts empty (R-p1-6) and is shared by the steps (step 2 checks the scratch file step 1 left).
$scratch = Join-Path $host_ "Saved\DreamTests\Scratches"
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
if (-not (Test-Path $editor)) { "run_restart: editor not found at $editor"; exit 2 }
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

$globalIni = Join-Path $env:LOCALAPPDATA "UnrealEngine\5.8\Saved\Config\WindowsEditor\EditorSettings.ini"
$projectIni = Join-Path $host_ "Saved\Config\WindowsEditor\EditorPerProjectUserSettings.ini"
# The two layout keys this driver removes, and only those, are backed up and put back: a whole-file restore would roll back
# whatever else changed in the ini meanwhile. The backups live outside $out (which every run deletes), so a run that was
# killed before its finally block (window closed, process killed) is undone by the next one, which restores a leftover
# set first. A backup file holds the key's original line, or nothing when the key was absent (then it is removed again).
$backupDir = Join-Path $host_ "Saved\Automation\RestartBackup"
$keys = @(
	@{ Name = "global-OutputPosition"; Path = $globalIni; Section = "/Script/DreamEditor.DreamUserSettings"; Key = "OutputPosition" },
	@{ Name = "project-OutputAreaFraction"; Path = $projectIni; Section = "/Script/DreamEditor.DreamPerProjectUserSettings"; Key = "OutputAreaFraction" }
)

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

# The Key= line of Section, or "" when there is none.
function Get-IniKeyLine([string]$Path, [string]$Section, [string]$Key) {
	if (-not (Test-Path $Path)) { return "" }
	$inSection = $false
	foreach ($line in (Read-Ini $Path).Lines) {
		if ($line -match '^\[(.*)\]\s*$') { $inSection = ($Matches[1] -eq $Section) }
		elseif ($inSection -and $line -match ("^" + [regex]::Escape($Key) + "=")) { return $line }
	}
	return ""
}

# Removes Key= from Section, then adds Line ("" adds nothing) right after the section header (a missing section is added),
# keeping every other line as it is.
function Set-IniKeyLine([string]$Path, [string]$Section, [string]$Key, [string]$Line) {
	if (-not (Test-Path $Path)) {
		if ($Line -eq "") { return }
		Write-Ini $Path @("[$Section]", $Line, "") (New-Object System.Text.UTF8Encoding($false))
		return
	}
	$ini = Read-Ini $Path
	$inSection = $false
	$placed = ($Line -eq "")
	$out = New-Object System.Collections.Generic.List[string]
	foreach ($l in $ini.Lines) {
		if ($l -match '^\[(.*)\]\s*$') {
			$inSection = ($Matches[1] -eq $Section)
			$out.Add($l)
			if ($inSection -and -not $placed) { $out.Add($Line); $placed = $true }
			continue
		}
		if ($inSection -and $l -match ("^" + [regex]::Escape($Key) + "=")) { continue }
		$out.Add($l)
	}
	if (-not $placed) { $out.Add("[$Section]"); $out.Add($Line) }
	Write-Ini $Path $out $ini.Encoding
}

function Restore-Backups {
	if (-not (Test-Path $backupDir)) { return }
	foreach ($k in $keys) {
		$file = Join-Path $backupDir ($k.Name + ".line")
		if (Test-Path $file) { Set-IniKeyLine $k.Path $k.Section $k.Key ([System.IO.File]::ReadAllText($file)) }
	}
	Remove-Item -Recurse -Force $backupDir
	Write-Host ("run_restart: restored " + (($keys | ForEach-Object { $_.Key }) -join ", "))   # (icons) Write-Host: called in finally
}

if (Test-Path $backupDir) { "run_restart: an earlier run did not finish; restoring its key backups first"; Restore-Backups }
New-Item -ItemType Directory -Force $backupDir | Out-Null
foreach ($k in $keys) {
	[System.IO.File]::WriteAllText((Join-Path $backupDir ($k.Name + ".line")), (Get-IniKeyLine $k.Path $k.Section $k.Key))
}
foreach ($k in $keys) { Set-IniKeyLine $k.Path $k.Section $k.Key "" }

# (icons) R-q1-3: each step's editor runs under Tests\ci\editor_layout.py run (the fixed test layout, the Dream tab docked;
# the user's EditorLayout.json is put back byte-identical when that editor is gone, also after Ctrl+C).
. (Join-Path $PSScriptRoot "editor_layout.ps1")
$run = $null

$failures = @()
$p = $null
try {
	for ($step = 1; $step -le $Steps; $step++) {
		$log = Join-Path $out ("editor-step{0}.log" -f $step)
		$argList = @("`"$project`"", "-ExecCmds=`"Automation RunTests Dream.Restart.Layout`"", "-DreamRestartStep=$step",
			"-DreamRestartDir=`"$out`"", "-nosplash", "-NoSound", "-log", "-abslog=`"$log`"", "-DreamScratchFolder=`"$scratch`"",
			"-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False")
		"run_restart: step $step"
		$run = Start-EditorUnderTestLayout $editor $argList
		$p = $run.Editor
		if (-not $p) { $failures += "step ${step}: the editor did not start under the layout wrapper"; break }
		$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
		$timedOut = $false
		while (-not $p.HasExited) {
			if ((Get-Date) -gt $deadline) {
				$timedOut = $true
				# Only the editor this driver started (after the same check as below), never another DreamHost editor.
				if (Test-OwnDreamHostEditor $p) { Stop-Process -Id $p.Id -Force }
				break
			}
			Start-Sleep -Seconds 2
		}
		$null = $p.WaitForExit(10000)
		$null = $run.Wrapper.WaitForExit(60000)   # (icons) the wrapper restores the user's layout after the editor
		if ($run.Wrapper.ExitCode -eq 75) { $failures += "step ${step}: the user's editor layout was not restored"; break }
		$result = Join-Path $out ("step{0}.txt" -f $step)
		if ($timedOut) { $failures += "step ${step}: the editor did not exit within $TimeoutMinutes min" }
		if (-not (Test-Path $result)) { $failures += "step ${step}: no result file (see $log)"; break }
		$lines = Get-Content $result
		foreach ($line in $lines) { "  $line" }
		if (-not ($lines -contains "ok=1")) { $failures += "step ${step}: failed (see $result and $log)" }
		$bad = Get-Content $log | Select-String -Pattern 'Ensure condition failed|=== Critical error|Fatal error|LogDream: Error'
		foreach ($line in $bad) { $failures += "step ${step} log: $($line.Line)" }
		if ($failures.Count -gt 0) { break }
	}
}
finally {
	# (icons) Review q1 M1: restore first, Write-Host only. Complete-TestLayout stops the step's editor if it still runs (only
	# the one this driver started), waits for the wrapper and restores the user's layout; then the two ini keys.
	$layoutOk = Complete-TestLayout $run
	Restore-Backups
	if (-not $layoutOk) { $failures += "the user's editor layout was NOT restored (kept as %LOCALAPPDATA%\UnrealEngine\Editor\EditorLayout.json.dream-backup)" }
}

if ($failures.Count -eq 0) { "run_restart: passed ($Steps steps)"; exit 0 }
foreach ($f in $failures) { "run_restart: FAILED: $f" }
exit 1
