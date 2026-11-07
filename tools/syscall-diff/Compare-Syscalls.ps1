# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$WindowsLog = "",
    [string]$ReactOSLog = "",
    [string]$OutputDir = "",
    [string]$Names = ""
)

$ErrorActionPreference = "Stop"
if (-not $Names) { $Names = Join-Path $SourceRoot "compat\reference\win10-22h2-syscalls.txt" }
if (-not $OutputDir) { $OutputDir = Join-Path $SourceRoot "output\syscall-diff" }
if (-not $WindowsLog) { $WindowsLog = Join-Path $OutputDir "windows.log" }
if (-not $ReactOSLog) { $ReactOSLog = Join-Path $OutputDir "reactos.log" }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Read-StatusNames {
    $map = @{}
    $path = Join-Path $SourceRoot "tools\windows-trace\ntstatus.txt"
    foreach ($line in (Get-Content $path)) {
        $trim = $line.Trim()
        if (-not $trim -or $trim.StartsWith("#")) { continue }
        $parts = $trim -split "\s+"
        if ($parts.Count -ge 2) { $map[$parts[0].ToUpper()] = $parts[1] }
    }
    return $map
}

function Read-DiffLog {
    param([string]$Path)
    $found = @{}
    if (-not (Test-Path $Path)) { return $found }
    foreach ($line in (Get-Content $Path)) {
        if ($line -notmatch "DIFF\s+(\S+)\s+(\S+)") { continue }
        $name = $Matches[1]
        $token = $Matches[2]
        if ($name -eq "END") { continue }
        if (-not $found.ContainsKey($name) -or $token -ne "STARTED") {
            $found[$name] = $token
        }
    }
    return $found
}

function Convert-StatusCode {
    param([string]$Token)
    if ($Token -match "^0x[0-9A-Fa-f]+$") {
        return [uint32]::Parse($Token.Substring(2), [Globalization.NumberStyles]::AllowHexSpecifier)
    }
    return $null
}

function Format-Status {
    param([string]$Token, $Names)
    if (-not $Token) { return "untested" }
    if ($Token -eq "MISSING") { return "missing" }
    if ($Token -eq "STARTED" -or $Token -eq "CRASH") { return "crash" }
    $code = Convert-StatusCode $Token
    if ($null -eq $code) { return $Token }
    $hex = "0x{0:X8}" -f $code
    if ($Names.ContainsKey($hex)) { return $Names[$hex] }
    return $hex
}

function Get-Verdict {
    param([string]$Windows, [string]$ReactOS)
    if (-not $Windows -or -not $ReactOS) { return "untested" }
    if ($ReactOS -eq "MISSING") { return "missing" }
    if ($Windows -eq "STARTED" -or $Windows -eq "CRASH" -or $ReactOS -eq "STARTED" -or $ReactOS -eq "CRASH") {
        return "broken"
    }
    $left = Convert-StatusCode $Windows
    $right = Convert-StatusCode $ReactOS
    if ($null -eq $left -or $null -eq $right) { return "untested" }
    if ($left -eq $right) { return "works" }
    $leftOk = ($left -eq 0)
    $rightOk = ($right -eq 0)
    if ($leftOk -ne $rightOk) { return "broken" }
    return "partial"
}

$statusNames = Read-StatusNames
$windows = Read-DiffLog $WindowsLog
$reactos = Read-DiffLog $ReactOSLog
$calls = @()
$lines = @()
foreach ($line in (Get-Content $Names)) {
    $name = $line.Trim()
    if (-not $name -or $name.StartsWith("#")) { continue }
    $win = $null
    $ros = $null
    if ($windows.ContainsKey($name)) { $win = [string]$windows[$name] }
    if ($reactos.ContainsKey($name)) { $ros = [string]$reactos[$name] }
    $verdict = Get-Verdict $win $ros
    $winText = Format-Status $win $statusNames
    $rosText = Format-Status $ros $statusNames
    $label = "Mismatch"
    if ($verdict -eq "works") { $label = "Match" }
    elseif ($verdict -eq "untested") { $label = "Not reached" }
    $lines += $name
    $lines += "Windows: $winText"
    $lines += "ReactOS: $rosText"
    $lines += ""
    $lines += $label
    $lines += ""
    $calls += [ordered]@{
        name     = $name
        status   = $verdict
        windows  = $winText
        reactos  = $rosText
    }
}

$report = [ordered]@{ calls = $calls }
$json = $report | ConvertTo-Json -Depth 5
Set-Content -Path (Join-Path $OutputDir "report.json") -Value $json -Encoding UTF8
Set-Content -Path (Join-Path $OutputDir "report.txt") -Value ($lines -join "`r`n") -Encoding UTF8
$scored = @($calls | Where-Object { $_.status -ne "untested" })
Write-Host "Wrote $(Join-Path $OutputDir 'report.txt') ($($scored.Count) scored)"
