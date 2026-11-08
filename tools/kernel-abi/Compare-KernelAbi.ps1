# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$WindowsLog = "",
    [string]$ReactOSLog = "",
    [string]$OutputDir = "",
    [string]$WebsitePath = "",
    [switch]$SelfTest,
    [switch]$SkipFreshness
)

$ErrorActionPreference = "Stop"

function ConvertTo-PlainJson {
    param($Value, [int]$Indent = 0)
    $pad = "  " * $Indent
    $next = "  " * ($Indent + 1)
    if ($null -eq $Value) { return "null" }
    if ($Value -is [bool]) {
        if ($Value) { return "true" }
        return "false"
    }
    if ($Value -is [string]) {
        $text = $Value.Replace("\", "\\").Replace('"', '\"').Replace("`r", "\r").Replace("`n", "\n")
        return '"' + $text + '"'
    }
    $isWhole = $Value -is [int] -or $Value -is [long] -or $Value -is [decimal]
    $isFloat = $Value -is [double] -or $Value -is [float] -or $Value -is [single]
    if ($isWhole -or $isFloat) {
        $number = [double]$Value
        $rounded = [math]::Round($number, 1)
        if ([math]::Abs($rounded - [math]::Truncate($rounded)) -lt 0.001) {
            return ([int64][math]::Truncate($rounded)).ToString([Globalization.CultureInfo]::InvariantCulture)
        }
        return $rounded.ToString("0.0", [Globalization.CultureInfo]::InvariantCulture)
    }
    if ($Value -is [System.Collections.IList]) {
        if ($Value.Count -eq 0) { return "[]" }
        $lines = @()
        foreach ($item in $Value) {
            $lines += ($next + (ConvertTo-PlainJson -Value $item -Indent ($Indent + 1)))
        }
        return "[" + [Environment]::NewLine + ($lines -join ("," + [Environment]::NewLine)) + [Environment]::NewLine + $pad + "]"
    }
    $map = $Value
    if ($Value -is [pscustomobject]) {
        $map = [ordered]@{}
        foreach ($prop in $Value.PSObject.Properties) { $map[$prop.Name] = $prop.Value }
    }
    if ($map -is [System.Collections.IDictionary]) {
        if ($map.Count -eq 0) { return "{}" }
        $lines = @()
        foreach ($key in @($map.Keys)) {
            $encoded = ConvertTo-PlainJson -Value $map[$key] -Indent ($Indent + 1)
            $lines += ($next + '"' + $key + '": ' + $encoded)
        }
        return "{" + [Environment]::NewLine + ($lines -join ("," + [Environment]::NewLine)) + [Environment]::NewLine + $pad + "}"
    }
    throw "Unsupported JSON value: $($Value.GetType().FullName)"
}

function Read-ReferenceNames {
    param([string]$Root)
    $names = @()
    $files = @(
        (Join-Path $Root "compat\reference\win10-22h2-ntoskrnl.txt"),
        (Join-Path $Root "compat\reference\win10-22h2-hal.txt")
    )
    foreach ($file in $files) {
        foreach ($line in (Get-Content $file)) {
            $trim = $line.Trim()
            if (-not $trim -or $trim.StartsWith("#")) { continue }
            $names += ($trim -split "\s+")[0]
        }
    }
    return $names
}

function Read-DiffLog {
    param([string]$Path)
    $found = @{}
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $found }
    foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ($line -notmatch "DIFF\s+(\S+)\s+(\S+)") { continue }
        $name = $Matches[1]
        $token = $Matches[2]
        if ($name -eq "END") { continue }
        $found[$name] = $token
    }
    return $found
}

function Test-DiffEndOk {
    param([string]$Path)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $false }
    return [bool](Select-String -LiteralPath $Path -Pattern "DIFF END OK" -Quiet)
}

function Convert-StatusCode {
    param([string]$Token)
    if ($Token -match "^0x[0-9A-Fa-f]+$") {
        return [uint32]::Parse($Token.Substring(2), [Globalization.NumberStyles]::AllowHexSpecifier)
    }
    return $null
}

function Get-Verdict {
    param([string]$Windows, [string]$ReactOS)
    if (-not $Windows -or -not $ReactOS) { return "untested" }
    if ($ReactOS -eq "MISSING") { return "missing" }
    if ($Windows -eq "MISSING") { return "broken" }
    if ($Windows -eq "CRASH" -or $ReactOS -eq "CRASH") { return "broken" }
    if ($Windows -eq $ReactOS) { return "works" }
    $left = Convert-StatusCode $Windows
    $right = Convert-StatusCode $ReactOS
    if ($null -eq $left -or $null -eq $right) { return "broken" }
    $leftOk = ($left -eq 0 -or ($left -ge 0x40000000 -and $left -lt 0x80000000))
    $rightOk = ($right -eq 0 -or ($right -ge 0x40000000 -and $right -lt 0x80000000))
    if ($leftOk -ne $rightOk) { return "broken" }
    return "partial"
}

function Get-NewestSource {
    param([string]$Root)
    $newest = [datetime]::MinValue
    $paths = @(
        (Join-Path $Root "ntoskrnl"),
        (Join-Path $Root "hal"),
        (Join-Path $Root "compat\reference\win10-22h2-ntoskrnl.txt"),
        (Join-Path $Root "compat\reference\win10-22h2-hal.txt"),
        (Join-Path $Root "tools\kernel-abi\probe.c")
    )
    foreach ($path in $paths) {
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $item = Get-Item -LiteralPath $path
        if (-not $item.PSIsContainer) {
            if ($item.LastWriteTimeUtc -gt $newest) { $newest = $item.LastWriteTimeUtc }
            continue
        }
        foreach ($file in (Get-ChildItem -LiteralPath $path -Recurse -File -ErrorAction SilentlyContinue)) {
            if ($file.LastWriteTimeUtc -gt $newest) { $newest = $file.LastWriteTimeUtc }
        }
    }
    return $newest
}

function Get-KernelReport {
    param([string]$Root, [string]$Windows, [string]$Veros)
    $names = Read-ReferenceNames $Root
    $winMap = Read-DiffLog $Windows
    $rosMap = Read-DiffLog $Veros
    $rows = @()
    foreach ($name in $names) {
        $win = $null
        $ros = $null
        if ($winMap.ContainsKey($name)) { $win = [string]$winMap[$name] }
        if ($rosMap.ContainsKey($name)) { $ros = [string]$rosMap[$name] }
        $rows += [ordered]@{
            name    = $name
            status  = (Get-Verdict $win $ros)
            windows = $(if ($win) { $win } else { "untested" })
            veros   = $(if ($ros) { $ros } else { "untested" })
        }
    }
    $matched = @($rows | Where-Object { $_.status -eq "works" }).Count
    $partial = @($rows | Where-Object { $_.status -eq "partial" }).Count
    $broken = @($rows | Where-Object { $_.status -eq "broken" }).Count
    $missing = @($rows | Where-Object { $_.status -eq "missing" }).Count
    $untested = @($rows | Where-Object { $_.status -eq "untested" }).Count
    $percent = $null
    if ($names.Count -gt 0) {
        $percent = [math]::Round(100.0 * $matched / $names.Count, 1)
    }
    return [ordered]@{
        percent   = $percent
        reference = $names.Count
        matched   = $matched
        partial   = $partial
        broken    = $broken
        missing   = $missing
        untested  = $untested
        names     = $rows
    }
}

function Set-KernelGap {
    param($Gaps, [bool]$Measured, [string]$Reason)
    $list = New-Object System.Collections.Generic.List[object]
    $found = $false
    $flat = @($Gaps)
    if ($flat.Count -eq 1 -and $flat[0] -is [System.Collections.IList] -and -not ($flat[0] -is [string])) {
        $flat = @($flat[0])
    }
    foreach ($gap in $flat) {
        if ($null -eq $gap) { continue }
        if ([string]$gap.section -eq "kernelAbi") {
            $found = $true
            if (-not $Measured) {
                $list.Add([ordered]@{ section = "kernelAbi"; reason = $Reason }) | Out-Null
            }
            continue
        }
        $list.Add($gap) | Out-Null
    }
    if (-not $Measured -and -not $found) {
        $list.Add([ordered]@{ section = "kernelAbi"; reason = $Reason }) | Out-Null
    }
    return ,$list.ToArray()
}

function Update-KernelNote {
    param([string]$Notes, [bool]$Measured, $Percent, $Report)
    $notes = [regex]::Replace($Notes, ' kernelAbi is [0-9.]+ from the kernel-exports category \([^)]*\)\. Untested rows stay out of that percent\.', '')
    $notes = [regex]::Replace($notes, ' kernelAbi is [0-9.]+ from exact status agreement on the kernel probe \([^)]*\)\.', '')
    if ($notes -match '([^.]*?) were not measured by this probe\.') {
        $list = $Matches[1].Trim()
        $list = [regex]::Replace($list, '(^|, )kernelAbi(, )?', '')
        $list = $list.Trim().TrimStart(',').Trim()
        if (-not $Measured -and $list -notmatch '(^|, )kernelAbi(,|$)') {
            if ($list) { $list = "kernelAbi, $list" }
            else { $list = "kernelAbi" }
        }
        $notes = [regex]::Replace($notes, '[^.]* were not measured by this probe\.', " $list were not measured by this probe.", 1)
    }
    if ($Measured) {
        $score = [math]::Round([double]$Percent, 1)
        $text = $score.ToString([Globalization.CultureInfo]::InvariantCulture)
        if ([math]::Abs($score - [math]::Truncate($score)) -lt 0.001) {
            $text = ([int64][math]::Truncate($score)).ToString([Globalization.CultureInfo]::InvariantCulture)
        }
        $notes += " kernelAbi is $text from exact status agreement on the kernel probe (matched $($Report.matched) of $($Report.reference))."
    }
    return $notes
}

function Write-WebsiteKernel {
    param([string]$Path, [bool]$Measured, [string]$Reason, $Report)
    if (-not (Test-Path -LiteralPath $Path)) {
        Write-Host "No website snapshot at $Path"
        return
    }
    $site = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $percent = $null
    $detail = $null
    if ($Measured) {
        $percent = [double]$Report.percent
        $detail = [ordered]@{
            reference = $Report.reference
            matched   = $Report.matched
            partial   = $Report.partial
            broken    = $Report.broken
            missing   = $Report.missing
            untested  = $Report.untested
            rule      = "Exact token agreement between the Windows 10 22H2 kernel and the VerOS kernel. Untested names count as zero."
            names     = $Report.names
        }
    }
    $site.kernelAbi = $percent
    $site | Add-Member -NotePropertyName kernelAbiDetail -NotePropertyValue $detail -Force
    $site.notes = Update-KernelNote -Notes ([string]$site.notes) -Measured $Measured -Percent $percent -Report $Report
    $site.unmeasured = Set-KernelGap -Gaps $site.unmeasured -Measured $Measured -Reason $Reason
    $json = ConvertTo-PlainJson $site
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $utf8)
    if ($Measured) { Write-Host "Updated $Path kernelAbi=$percent" }
    else { Write-Host "Cleared $Path kernelAbi ($Reason)" }
}

function Invoke-KernelSelfTest {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ("kernel-abi-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    try {
        $ref = Join-Path $root "compat\reference"
        New-Item -ItemType Directory -Force -Path $ref | Out-Null
        $utf8 = New-Object System.Text.UTF8Encoding $false
        [System.IO.File]::WriteAllText((Join-Path $ref "win10-22h2-ntoskrnl.txt"), "One`r`nTwo`r`n", $utf8)
        [System.IO.File]::WriteAllText((Join-Path $ref "win10-22h2-hal.txt"), "Three`r`n", $utf8)
        $win = Join-Path $root "windows.log"
        $ros = Join-Path $root "veros.log"
        [System.IO.File]::WriteAllText($win, "DIFF One 0x00000000`r`nDIFF Two 0xC0000001`r`nDIFF END OK`r`n", $utf8)
        [System.IO.File]::WriteAllText($ros, "DIFF One 0x00000000`r`nDIFF Two 0xC0000002`r`nDIFF END OK`r`n", $utf8)
        $report = Get-KernelReport -Root $root -Windows $win -Veros $ros
        if ($report.reference -ne 3) { throw "reference count was $($report.reference)" }
        if ($report.matched -ne 1 -or $report.partial -ne 1 -or $report.untested -ne 1) {
            throw "counts were matched=$($report.matched) partial=$($report.partial) untested=$($report.untested)"
        }
        if ([math]::Abs([double]$report.percent - 33.3) -gt 0.05) { throw "percent was $($report.percent)" }
        $same = Get-Verdict "RETURNED" "RETURNED"
        $class = Get-Verdict "RETURNED" "NULL"
        $miss = Get-Verdict "0x00000000" "MISSING"
        if ($same -ne "works" -or $class -ne "broken" -or $miss -ne "missing") {
            throw "token verdicts were $same $class $miss"
        }
        Write-Host "Kernel ABI compare fixture passed."
    }
    finally {
        if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    }
}

if ($SelfTest) {
    Invoke-KernelSelfTest
    exit 0
}

if (-not $WindowsLog) { $WindowsLog = Join-Path $SourceRoot "output\kernel-abi\windows.log" }
if (-not $ReactOSLog) { $ReactOSLog = Join-Path $SourceRoot "output\kernel-abi\reactos.log" }
if (-not $OutputDir) { $OutputDir = Join-Path $SourceRoot "output\kernel-abi" }
if (-not $WebsitePath) { $WebsitePath = Join-Path $SourceRoot "veros\dashboard\latest.json" }

$reason = "The Windows 10 22H2 kernel and the VerOS kernel have not both finished the kernel probe."
$ready = (Test-DiffEndOk $WindowsLog) -and (Test-DiffEndOk $ReactOSLog)
if ($ready -and -not $SkipFreshness) {
    $newest = Get-NewestSource $SourceRoot
    $winTime = (Get-Item -LiteralPath $WindowsLog).LastWriteTimeUtc
    $rosTime = (Get-Item -LiteralPath $ReactOSLog).LastWriteTimeUtc
    if ($winTime -lt $newest -or $rosTime -lt $newest) {
        $ready = $false
        $reason = "The kernel probe log is older than the kernel sources or the probe."
    }
}

if (-not $ready) {
    Write-WebsiteKernel -Path $WebsitePath -Measured $false -Reason $reason -Report $null
    exit 0
}

$report = Get-KernelReport -Root $SourceRoot -Windows $WindowsLog -Veros $ReactOSLog
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$json = ConvertTo-PlainJson ([ordered]@{ calls = $report.names })
$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText((Join-Path $OutputDir "report.json"), $json + [Environment]::NewLine, $utf8)
Write-WebsiteKernel -Path $WebsitePath -Measured $true -Reason "" -Report $report
Write-Host "Kernel ABI $($report.matched) of $($report.reference) matched."
