# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$WindowsLog = "",
    [string]$ReactOSLog = "",
    [string]$OutputDir = "",
    [string]$Names = ""
)

$ErrorActionPreference = "Stop"
if (-not $Names) { $Names = Join-Path $SourceRoot "compat\reference\win10-22h2-graphics.txt" }
if (-not $OutputDir) { $OutputDir = Join-Path $SourceRoot "output\graphics-diff" }
if (-not $WindowsLog) { $WindowsLog = Join-Path $OutputDir "windows.log" }
if (-not $ReactOSLog) { $ReactOSLog = Join-Path $OutputDir "reactos.log" }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Read-DiffLog {
    param([string]$Path)
    $found = @{}
    $finished = $false
    if (-not (Test-Path -LiteralPath $Path)) { return @{ Done = $false; Rows = $found } }
    foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ($line -match "DIFF\s+END\s+OK") { $finished = $true }
        if ($line -notmatch "DIFF\s+(\S+)\s+(\S+)") { continue }
        $name = $Matches[1]
        $token = $Matches[2]
        if ($name -eq "END") { continue }
        if (-not $found.ContainsKey($name) -or $token -ne "STARTED") {
            $found[$name] = $token
        }
    }
    return @{ Done = $finished; Rows = $found }
}

function Test-WindowsCompleted {
    param([string]$Kind, [string]$Token)
    if (-not $Token) { return $false }
    switch ($Kind) {
        "module" { return $Token -eq "0x00000000" }
        "status" { return $Token -eq "0x00000000" }
        "number" { return $Token -match "^0x[0-9A-Fa-f]{8}$" }
        "draw" { return $Token -match "^0x[0-9A-Fa-f]{8}$" }
        "device" { return $Token -match "^0x00000000:0x[0-9A-Fa-f]{8}$" }
        default { return $false }
    }
}

if (-not (Test-Path -LiteralPath $Names)) { throw "Graphics name list was not found at $Names" }
$windows = Read-DiffLog $WindowsLog
$reactos = Read-DiffLog $ReactOSLog
if (-not $windows.Done) { throw "Windows graphics log does not contain DIFF END OK" }
if (-not $reactos.Done) { throw "VerOS graphics log does not contain DIFF END OK" }

$calls = @()
$lines = @()
$capHits = 0
$capTotal = 0
$agreeHits = 0
$agreeTotal = 0
foreach ($line in (Get-Content -LiteralPath $Names)) {
    $trim = $line.Trim()
    if (-not $trim -or $trim.StartsWith("#")) { continue }
    $parts = $trim -split "\s+"
    if ($parts.Count -lt 2) { throw "Graphics name list row needs a kind: $trim" }
    $name = $parts[0]
    $kind = $parts[1]
    $win = $null
    $ros = $null
    if ($windows.Rows.ContainsKey($name)) { $win = [string]$windows.Rows[$name] }
    if ($reactos.Rows.ContainsKey($name)) { $ros = [string]$reactos.Rows[$name] }
    $completed = Test-WindowsCompleted $kind $win
    $equal = ($null -ne $win -and $null -ne $ros -and $win -eq $ros)
    $capMatch = ($completed -and $equal)
    if ($null -ne $win) { $agreeTotal++ }
    if ($equal) { $agreeHits++ }
    if ($completed) {
        $capTotal++
        if ($capMatch) { $capHits++ }
    }
    $status = "untested"
    if ($null -eq $win) { $status = "untested" }
    elseif ($ros -eq "MISSING") { $status = "missing" }
    elseif ($capMatch) { $status = "works" }
    elseif ($equal) { $status = "partial" }
    elseif ($null -ne $ros) { $status = "broken" }
    $winText = $(if ($win) { $win } else { "untested" })
    $rosText = $(if ($ros) { $ros } else { "untested" })
    $label = "Mismatch"
    if ($equal) { $label = "Match" }
    elseif ($null -eq $win) { $label = "Not reached" }
    $lines += $name
    $lines += "Windows: $winText"
    $lines += "ReactOS: $rosText"
    $lines += ""
    $lines += $label
    $lines += ""
    $calls += [ordered]@{
        name              = $name
        kind              = $kind
        status            = $status
        windows           = $winText
        reactos           = $rosText
        windowsCompleted  = [bool]$completed
        capabilityMatch   = [bool]$capMatch
        agreementMatch    = [bool]$equal
    }
}

$capability = $null
$agreement = $null
if ($capTotal -gt 0) { $capability = [math]::Round(100.0 * $capHits / $capTotal, 1) }
if ($agreeTotal -gt 0) { $agreement = [math]::Round(100.0 * $agreeHits / $agreeTotal, 1) }
$report = [ordered]@{
    capability       = $capability
    capabilityHits   = $capHits
    capabilityTotal  = $capTotal
    agreement        = $agreement
    agreementHits    = $agreeHits
    agreementTotal   = $agreeTotal
    calls            = $calls
}
$json = $report | ConvertTo-Json -Depth 5
Set-Content -LiteralPath (Join-Path $OutputDir "report.json") -Value $json -Encoding UTF8
$header = @(
    "Capability: $capHits of $capTotal ($capability%)",
    "Agreement: $agreeHits of $agreeTotal ($agreement%)",
    ""
)
Set-Content -LiteralPath (Join-Path $OutputDir "report.txt") -Value (($header + $lines) -join "`r`n") -Encoding UTF8
Write-Host "Capability: $capHits of $capTotal ($capability%)"
Write-Host "Agreement: $agreeHits of $agreeTotal ($agreement%)"
exit 0
