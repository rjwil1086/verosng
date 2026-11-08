# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$Iso = "",
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [int]$TimeoutSec = 1200,
    [string]$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [string]$Plugin = "",
    [string]$Map = "",
    [string]$OutputDir = "",
    [string]$AppsDir = "",
    [switch]$ShowWindow
)

$ErrorActionPreference = "Stop"

if (-not $OutputDir) {
    $OutputDir = Join-Path $SourceRoot "output"
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

if (-not $Iso) {
    $Iso = Join-Path $SourceRoot "build-amd64\bootcd.iso"
}
if (-not (Test-Path $Qemu)) {
    Write-Error "QEMU was not found at $Qemu"
}
if (-not (Test-Path $Iso)) {
    Write-Error "ISO was not found at $Iso. Run Build-Bootcd.ps1 first."
}

if (-not $Plugin) {
    $Candidate = Join-Path $SourceRoot "tools\qemu-harness\abi.dll"
    if (Test-Path $Candidate) {
        $Plugin = $Candidate
    }
}

$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
$serial = Join-Path $OutputDir "boot-$stamp.log"
$jsonl = Join-Path $OutputDir "abi-$stamp.jsonl"
$qemuLog = Join-Path $OutputDir "qemu-$stamp.log"

$bios = "C:\Program Files\qemu\share"
$argLine = "-accel tcg -L `"$bios`" -m 8192 -smp 4 -cdrom `"$Iso`" -boot d -no-reboot -serial `"file:$serial`""
if ($ShowWindow) {
    $argLine += " -display gtk -name `"VerOS NG`""
} else {
    $argLine += " -display none"
}
if ($AppsDir) {
    if (-not (Test-Path $AppsDir)) {
        Write-Error "Apps directory was not found at $AppsDir"
    }
    $appsFwd = ($AppsDir -replace '\\', '/')
    $argLine += " -drive file=fat:rw:${appsFwd},format=raw,if=ide,index=0,media=disk"
}
if ($Plugin) {
    $pluginFwd = ($Plugin -replace '\\', '/')
    $jsonlFwd = ($jsonl -replace '\\', '/')
    $pluginArg = "file=$pluginFwd,log=$jsonlFwd"
    if ($Map) {
        $pluginArg += ",map=" + ($Map -replace '\\', '/')
    }
    $argLine += " -plugin `"$pluginArg`""
}

Write-Host "QEMU: $Qemu"
Write-Host "accelerator=tcg"
$start = @{
    FilePath = $Qemu
    ArgumentList = $argLine
    PassThru = $true
    RedirectStandardError = $qemuLog
}
if (-not $ShowWindow) {
    $start.WindowStyle = "Hidden"
}
$proc = Start-Process @start
$finished = $proc.WaitForExit($TimeoutSec * 1000)
if (-not $finished) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    $result = "hang"
}
elseif ($proc.ExitCode -eq 0) {
    $result = "exited"
}
else {
    $result = "exited"
}

$summary = [ordered]@{
    accelerator = "tcg"
    iso         = $Iso
    serialLog   = $serial
    pluginLog   = $jsonl
    qemuLog     = $qemuLog
    result      = $result
    timeoutSec  = $TimeoutSec
}
$summary | ConvertTo-Json | Set-Content -Path (Join-Path $OutputDir "boot-latest.json")
Write-Host "Serial log: $serial"
if ($result -eq "hang") {
    exit 2
}
exit 0
