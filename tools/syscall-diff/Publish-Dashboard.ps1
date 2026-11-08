# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$Report = "",
    [string]$WindowsLog = "",
    [string]$ReactOSLog = "",
    [string]$OutputPath = "",
    [switch]$SelfTest
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

function Get-ExactPercent {
    param($Calls)
    $scored = @($Calls | Where-Object { $_.status -ne "untested" })
    if ($scored.Count -eq 0) { return $null }
    $matches = @($scored | Where-Object { $_.status -eq "works" }).Count
    return [math]::Round(100.0 * $matches / $scored.Count, 1)
}

function Read-PreviousSnapshot {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    return (Get-Content $Path -Raw | ConvertFrom-Json)
}

function Get-AgedScore {
    param($Previous, [string]$Field, [double]$MinDays)
    if (-not $Previous -or -not $Previous.capturedAt -or -not $Previous.northStar) { return $null }
    $when = [datetime]::Parse($Previous.capturedAt).ToUniversalTime()
    $age = [datetime]::UtcNow - $when
    if ($age.TotalDays -ge $MinDays -and $null -ne $Previous.northStar.current) {
        return [double]$Previous.northStar.current
    }
    if ($Previous.northStar.$Field -ne $null) { return [double]$Previous.northStar.$Field }
    return $null
}

function New-DashboardSnapshot {
    param($Calls, $Previous, [string]$CapturedAt, [string]$BootResult, $CrashCount)
    $percent = Get-ExactPercent $Calls
    $works = @($Calls | Where-Object { $_.status -eq "works" }).Count
    $partial = @($Calls | Where-Object { $_.status -eq "partial" }).Count
    $broken = @($Calls | Where-Object { $_.status -eq "broken" }).Count
    $missing = @($Calls | Where-Object { $_.status -eq "missing" }).Count
    $untested = @($Calls | Where-Object { $_.status -eq "untested" }).Count
    $scored = $works + $partial + $broken + $missing

    $syscalls = @()
    $divergences = @()
    foreach ($call in @($Calls)) {
        $measured = $call.status -ne "untested"
        $match = $call.status -eq "works"
        $row = [ordered]@{
            name           = [string]$call.name
            compatibility  = $(if ($measured) { $(if ($match) { 100.0 } else { 0.0 }) } else { $null })
            windowsCalls   = $(if ($measured) { 1 } else { $null })
            matches        = $(if ($measured) { $(if ($match) { 1 } else { 0 }) } else { $null })
            mismatches     = $(if ($measured) { $(if ($match) { 0 } else { 1 }) } else { $null })
        }
        $syscalls += $row
        if ($measured -and -not $match) {
            $divergences += [ordered]@{
                syscall = [string]$call.name
                windows = [string]$call.windows
                veros   = [string]$call.reactos
                count   = 1
            }
        }
    }

    $lastWeek = Get-AgedScore -Previous $Previous -Field "lastWeek" -MinDays 7
    $lastMonth = Get-AgedScore -Previous $Previous -Field "lastMonth" -MinDays 30
    $previousOverall = $null
    $change = $null
    $improvements = $null
    $regressions = $null
    if ($Previous -and $null -ne $Previous.overall -and $null -ne $percent) {
        $previousOverall = [math]::Round([double]$Previous.overall, 1)
        $change = [math]::Round($percent - $previousOverall, 1)
        $improvements = @()
        $regressions = @()
        $older = @{}
        foreach ($row in @($Previous.syscalls)) { $older[[string]$row.name] = $row.compatibility }
        foreach ($row in $syscalls) {
            if ($null -eq $row.compatibility -or -not $older.ContainsKey($row.name)) { continue }
            if ($null -eq $older[$row.name]) { continue }
            $delta = [math]::Round([double]$row.compatibility - [double]$older[$row.name], 1)
            if ($delta -gt 0) { $improvements += [ordered]@{ name = $row.name; delta = $delta } }
            elseif ($delta -lt 0) { $regressions += [ordered]@{ name = $row.name; delta = $delta } }
        }
    }

    $note = "overall and syscallOverall are exact status matches from the amd64 probe"
    if ($null -eq $percent) {
        $note += ": no name had both a Windows result and a ReactOS result."
    }
    else {
        $note += ": $works of $scored names. $partial names failed on both sides with different codes, $broken disagreed on success, and $missing were missing on the amd64 side. A different error is a mismatch. $untested names were not reached."
    }
    $kernelAbi = $null
    $kernelAbiDetail = $null
    if ($Previous -and $null -ne $Previous.kernelAbiDetail) {
        $kernelAbi = [double]$Previous.kernelAbi
        $kernelAbiDetail = $Previous.kernelAbiDetail
    }
    $skipped = "lastBuild, applications, drivers, subsystems, traceCorpus, task, driverAbi, and graphics"
    if ($null -eq $kernelAbi) { $skipped = "kernelAbi, $skipped" }
    $note += " $skipped were not measured by this probe. northStar.lastWeek and northStar.lastMonth come only from an older snapshot in this file. The formula weights are the website contract and were not applied."

    $previousLabel = "No earlier snapshot"
    if ($null -ne $previousOverall) { $previousLabel = "Previous snapshot" }
    $gaps = @()
    if ($null -eq $kernelAbi) {
        $gaps += [ordered]@{ section = "kernelAbi"; reason = "The Windows 10 22H2 kernel and the VerOS kernel have not both finished the kernel probe." }
    }
    $gaps += [ordered]@{ section = "lastBuild"; reason = "The guest banner is a date and commit, not a monotonic build number." }
    if ($null -eq $lastWeek) { $gaps += [ordered]@{ section = "northStar.lastWeek"; reason = "No snapshot in this file is at least seven days old." } }
    if ($null -eq $lastMonth) { $gaps += [ordered]@{ section = "northStar.lastMonth"; reason = "No snapshot in this file is at least thirty days old." } }
    $gaps += [ordered]@{ section = "applicationsSummary"; reason = "The amd64 probe does not launch applications." }
    $gaps += [ordered]@{ section = "applications"; reason = "The amd64 probe does not launch applications." }
    $gaps += [ordered]@{ section = "driversSummary"; reason = "The amd64 probe does not load drivers." }
    $gaps += [ordered]@{ section = "driverGroups"; reason = "The amd64 probe does not load drivers." }
    if ($null -eq $previousOverall) { $gaps += [ordered]@{ section = "regression.previousOverall"; reason = "No earlier snapshot is in the repo." } }
    $gaps += [ordered]@{ section = "regression.currentBuild"; reason = "The guest banner is a date and commit, not a monotonic build number." }
    if ($null -eq $change) { $gaps += [ordered]@{ section = "regression.change"; reason = "No earlier snapshot is in the repo." } }
    if ($null -eq $improvements) { $gaps += [ordered]@{ section = "regression.improvements"; reason = "No earlier snapshot is in the repo." } }
    if ($null -eq $regressions) { $gaps += [ordered]@{ section = "regression.regressions"; reason = "No earlier snapshot is in the repo." } }
    $gaps += [ordered]@{ section = "subsystems"; reason = "The probe does not score subsystems." }
    $gaps += [ordered]@{ section = "highestImpact"; reason = "The probe does not score subsystems." }
    $gaps += [ordered]@{ section = "traceCorpus"; reason = "The probe records one call per name, not a usage share." }
    $gaps += [ordered]@{ section = "task"; reason = "Each mismatch is one probe call. No estimated gain was measured." }
    $gaps += [ordered]@{ section = "driverAbi"; reason = "The amd64 probe does not exercise IRP, PnP, power, DMA, or KMDF." }
    $gaps += [ordered]@{ section = "graphics"; reason = "The amd64 probe does not exercise the display stack." }

    return [ordered]@{
        spdx                 = "GPL-2.0-or-later"
        schemaVersion        = 1
        sample               = $false
        capturedAt           = $CapturedAt
        target               = "Win10 22H2"
        overall              = $percent
        kernelAbi            = $kernelAbi
        kernelAbiDetail      = $kernelAbiDetail
        lastBuild            = $null
        northStar            = [ordered]@{
            label     = "amd64 probe syscalls"
            current   = $percent
            lastWeek  = $lastWeek
            lastMonth = $lastMonth
        }
        syscallOverall       = $percent
        syscalls             = $syscalls
        divergences          = $divergences
        applicationsSummary  = [ordered]@{ working = $null; partial = $null; failing = $null }
        applications         = $null
        driversSummary       = [ordered]@{ working = $null; partial = $null; failing = $null }
        driverGroups         = $null
        regression           = [ordered]@{
            previousLabel    = $previousLabel
            previousOverall  = $previousOverall
            currentBuild     = $null
            currentOverall   = $percent
            change           = $change
            improvements     = $improvements
            regressions      = $regressions
        }
        subsystems           = $null
        highestImpact        = $null
        traceCorpus          = $null
        task                 = $null
        driverAbi            = $null
        graphics             = $null
        formula              = @(
            [ordered]@{ label = "Syscall compatibility"; weight = 40 },
            [ordered]@{ label = "Application compatibility"; weight = 25 },
            [ordered]@{ label = "Driver compatibility"; weight = 20 },
            [ordered]@{ label = "Graphics compatibility"; weight = 10 },
            [ordered]@{ label = "Test pass rate"; weight = 5 }
        )
        lastHarnessRun       = [ordered]@{
            guest       = "ReactOS amd64"
            harness     = "syscall-diff"
            bootResult  = $BootResult
            crashCount  = $CrashCount
        }
        notes                = $note
        unmeasured           = @($gaps)
    }
}

function Get-CrashCount {
    param([string[]]$Logs)
    $sawLog = $false
    foreach ($log in $Logs) {
        if (-not $log -or -not (Test-Path $log)) { continue }
        $sawLog = $true
        $hit = Select-String -Path $log -Pattern "BugCheck|Fatal System Error" -Quiet
        if ($hit) { return 1 }
    }
    if ($sawLog) { return 0 }
    return $null
}

function Invoke-DashboardSelfTest {
    $fixture = Join-Path $PSScriptRoot "fixtures"
    $win = Join-Path $fixture "windows.log"
    $ros = Join-Path $fixture "reactos.log"
    if (-not (Select-String -Path $win -Pattern "DIFF END OK" -Quiet)) { throw "fixture windows log lacks DIFF END OK" }
    $out = Join-Path $SourceRoot "output\syscall-diff-dashboard"
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    & (Join-Path $PSScriptRoot "Compare-Syscalls.ps1") -SourceRoot $SourceRoot -WindowsLog $win -ReactOSLog $ros -OutputDir $out
    $report = Get-Content (Join-Path $out "report.json") -Raw | ConvertFrom-Json
    $snap = New-DashboardSnapshot -Calls $report.calls -Previous $null -CapturedAt "2026-11-07T00:00:00Z" -BootResult "fixture" -CrashCount 0
    if ($snap.sample) { throw "sample must be false" }
    if ($snap.overall -ne 20.0) { throw "fixture overall should be 20, got $($snap.overall)" }
    $close = @($snap.syscalls | Where-Object { $_.name -eq "NtClose" })[0]
    if ($close.compatibility -ne 100.0 -or $close.matches -ne 1 -or $close.windowsCalls -ne 1) { throw "NtClose was not an exact match" }
    $read = @($snap.syscalls | Where-Object { $_.name -eq "NtReadFile" })[0]
    if ($read.compatibility -ne 0.0 -or $read.mismatches -ne 1) { throw "partial NtReadFile must stay a mismatch" }
    $idle = @($snap.syscalls | Where-Object { $_.name -eq "NtCreateSection" })[0]
    if ($null -ne $idle.compatibility -or $null -ne $idle.windowsCalls) { throw "untested name must stay null" }
    if ($null -ne $snap.kernelAbi -or $null -ne $snap.lastBuild -or $null -ne $snap.northStar.lastWeek) { throw "unmeasured scores were filled" }
    if ($snap.applications -or $snap.driverGroups -or $snap.graphics) { throw "unmeasured sections were filled" }
    $json = ConvertTo-PlainJson $snap
    if ($json -match "67\.3" -or $json -match "1832" -or $json -match "Chrome") { throw "specimen value leaked into the snapshot" }
    Write-Host "Dashboard fixture passed."
}

if ($SelfTest) {
    Invoke-DashboardSelfTest
    exit 0
}

if (-not $Report) { $Report = Join-Path $SourceRoot "output\syscall-diff\report.json" }
$reportPath = $Report
if (-not $WindowsLog) { $WindowsLog = Join-Path $SourceRoot "output\syscall-diff\windows.log" }
if (-not $ReactOSLog) { $ReactOSLog = Join-Path $SourceRoot "output\syscall-diff\reactos.log" }
if (-not $OutputPath) { $OutputPath = Join-Path $SourceRoot "veros\dashboard\latest.json" }

if (-not (Test-Path $reportPath)) { throw "Probe report was not found at $reportPath" }
if (-not (Test-Path $WindowsLog)) { throw "Windows probe log was not found at $WindowsLog" }
if (-not (Select-String -Path $WindowsLog -Pattern "DIFF END OK" -Quiet)) {
    throw "Windows probe log does not contain DIFF END OK"
}

$parsed = Get-Content $reportPath -Raw | ConvertFrom-Json
$bootLog = Join-Path (Split-Path $ReactOSLog -Parent) "reactos-boot.log"
$rosEnd = $false
if (Test-Path $ReactOSLog) { $rosEnd = [bool](Select-String -Path $ReactOSLog -Pattern "DIFF END OK" -Quiet) }
$bootResult = "Windows probe DIFF END OK"
if ($rosEnd) { $bootResult += "; ReactOS probe DIFF END OK" }
else { $bootResult += "; ReactOS probe log has no DIFF END OK trailer" }
$crashes = Get-CrashCount -Logs @($ReactOSLog, $bootLog)
$captured = (Get-Item $reportPath).LastWriteTimeUtc.ToString("yyyy-MM-ddTHH:mm:ssZ")
$previous = Read-PreviousSnapshot $OutputPath
if ($previous -and $previous.lastHarnessRun -and [string]$previous.lastHarnessRun.guest -ne "ReactOS amd64") {
    $previous = $null
}
$snapshot = New-DashboardSnapshot -Calls $parsed.calls -Previous $previous -CapturedAt $captured -BootResult $bootResult -CrashCount $crashes

if ($snapshot.overall -eq 67.3 -or $snapshot.lastBuild -eq 1832) {
    throw "Refusing a design-specimen score"
}

$dir = Split-Path $OutputPath -Parent
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$json = ConvertTo-PlainJson $snapshot
$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($OutputPath, $json + [Environment]::NewLine, $utf8)
Write-Host "Wrote $OutputPath overall=$($snapshot.overall)"
& (Join-Path $SourceRoot "tools\compat\Merge-DashboardAbi.ps1") -SourceRoot $SourceRoot -WebsitePath $OutputPath
