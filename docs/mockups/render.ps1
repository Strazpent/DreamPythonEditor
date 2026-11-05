# Renders every mockups/*.html to images/<name>.png with headless Microsoft Edge.
# Usage:  powershell -ExecutionPolicy Bypass -File docs\mockups\render.ps1 [-Only name]
# Each HTML file declares its canvas size with <meta name="viewport-size" content="WIDTHxHEIGHT">.
param([string]$Only = "")

$ErrorActionPreference = "Stop"
$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path (Split-Path -Parent $here) "images"
New-Item -ItemType Directory -Force $out | Out-Null

Get-ChildItem $here -Filter *.html | Where-Object { $Only -eq "" -or $_.BaseName -eq $Only } | ForEach-Object {
    # One browser profile per page, so several renders can run at the same time.
    $profile = Join-Path $env:TEMP ("upe-mockup-render-" + $_.BaseName)
    $html = Get-Content $_.FullName -Raw
    $size = "1600,1000"
    if ($html -match 'name="viewport-size"\s+content="(\d+)x(\d+)"') { $size = "$($Matches[1]),$($Matches[2])" }
    $png = Join-Path $out ($_.BaseName + ".png")
    $url = "file:///" + ($_.FullName -replace '\\', '/')
    $p = Start-Process -FilePath $edge -NoNewWindow -Wait -PassThru -ArgumentList @(
        "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
        "--window-size=$size", "--user-data-dir=$profile", "--virtual-time-budget=2000",
        "--screenshot=$png", $url)
    if ($p.ExitCode -ne 0) { throw "Edge failed on $($_.Name) (exit $($p.ExitCode))" }
    Write-Host "rendered $($_.BaseName).png ($size)"
}
