# SPDX-License-Identifier: Apache-2.0
# (icons) Review q1 ruling R-q1-3: every DreamHost editor a driver starts runs under
# Plugins\DreamPythonEditor\Tests\ci\editor_layout.py run -- <editor ...>, which puts the fixed test layout
# (Tests\ci\EditorLayout.tier4.json, the Dream tab docked) in place, waits for the editor, and puts the user's
# %LOCALAPPDATA%\UnrealEngine\Editor\EditorLayout.json back byte-identical in its own finally. The wrapper ignores Ctrl+C,
# so it outlives the editor and restores after it. Some specs assume the docked placement.
# Dot-source after $engine is set:  . (Join-Path $PSScriptRoot "editor_layout.ps1")
# Everything here prints with Write-Host only: in PowerShell 5.1 a finally block stops at the first statement that writes to
# the output stream after Ctrl+C (review q1 M1), so callers can use these inside finally.

$script:LayoutPy = Join-Path $engine "Engine\Binaries\ThirdParty\Python3\Win64\python.exe"
$script:LayoutGuard = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "Plugins\DreamPythonEditor\Tests\ci\editor_layout.py"

# Starts the editor under the layout wrapper. Returns @{ Wrapper = <python process>; Editor = <editor process or $null> }.
# The editor is the wrapper's child; the caller watches and kills .Editor as it did the editor before (only its own).
function Start-EditorUnderTestLayout([string]$Editor, [string[]]$ArgList, [switch]$Minimized) {
	$show = @()
	if ($Minimized) { $show = @("--show", "minimized") }
	$wrapArgs = @("-I", "-B", "`"$script:LayoutGuard`"", "run") + $show + @("--", "`"$Editor`"") + $ArgList
	$wrapper = Start-Process -FilePath $script:LayoutPy -ArgumentList $wrapArgs -PassThru -WindowStyle Minimized
	$null = $wrapper.Handle
	$child = $null
	$deadline = (Get-Date).AddSeconds(60)
	while (-not $child -and -not $wrapper.HasExited -and (Get-Date) -lt $deadline) {
		$info = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($wrapper.Id)" | Where-Object { $_.Name -like 'UnrealEditor*' } |
			Select-Object -First 1
		if ($info) {
			$child = Get-Process -Id $info.ProcessId -ErrorAction SilentlyContinue
			if ($child) { $null = $child.Handle }
		}
		else { Start-Sleep -Milliseconds 200 }
	}
	return @{ Wrapper = $wrapper; Editor = $child }
}

# True when $Process is an editor this driver started: an UnrealEditor process whose command line names DreamHost.uproject.
function Test-OwnDreamHostEditor([System.Diagnostics.Process]$Process) {
	if (-not $Process) { return $false }
	$info = Get-CimInstance Win32_Process -Filter "ProcessId = $($Process.Id)"
	return [bool]($info -and $info.Name -like 'UnrealEditor*' -and $info.CommandLine -like '*DreamHost.uproject*')
}

# After the editor: waits for the wrapper (it restores in its finally), then runs restore once more as a safety net (a no-op
# without a backup). True when the user's layout is back. Stops the run's editor first if it still runs (only its own).
function Complete-TestLayout($Run) {
	if ($Run -and $Run.Editor -and -not $Run.Editor.HasExited) {
		if (Test-OwnDreamHostEditor $Run.Editor) {
			Write-Host "editor_layout: stopping the editor (pid $($Run.Editor.Id)) before restoring the layout"
			Stop-Process -Id $Run.Editor.Id -Force
		}
		$null = $Run.Editor.WaitForExit(15000)
	}
	if ($Run -and $Run.Wrapper) { $null = $Run.Wrapper.WaitForExit(60000) }
	& $script:LayoutPy -I -B $script:LayoutGuard restore | ForEach-Object { Write-Host "  $_" }
	return ($LASTEXITCODE -eq 0)
}
