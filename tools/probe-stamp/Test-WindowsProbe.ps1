# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [ValidateSet('', 'syscall-diff', 'graphics-diff', 'kernel-abi')]
    [string]$Name = '',
    [ValidateSet('Check', 'Stamp')]
    [string]$Action = 'Check',
    [switch]$PassThru,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"

$script:ProbeCatalog = [ordered]@{
    'syscall-diff' = @('tools/syscall-diff/probe.c')
    'graphics-diff' = @('tools/graphics-diff/probe.c')
    'kernel-abi' = @('tools/kernel-abi/probe.c', 'tools/kernel-abi/loader.c')
}

function Get-Sha256Hex {
    param([byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash($Bytes)
    }
    finally { $sha.Dispose() }
    return ([BitConverter]::ToString($hash)).Replace('-', '').ToLower()
}

function Read-SharedBytes {
    param([string]$Path)
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $memory = New-Object System.IO.MemoryStream
        $stream.CopyTo($memory)
        return $memory.ToArray()
    }
    finally { $stream.Dispose() }
}

function Get-SourceIdentityHash {
    param([string[]]$Files, [string[]]$Labels)
    if ($Files.Count -ne $Labels.Count) { throw "Probe file list does not match labels" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        for ($i = 0; $i -lt $Files.Count; $i++) {
            $label = [Text.Encoding]::UTF8.GetBytes((($Labels[$i] -replace '\\', '/') + "`n"))
            [void]$sha.TransformBlock($label, 0, $label.Length, $null, 0)
            $body = [System.IO.File]::ReadAllBytes($Files[$i])
            if ($body.Length -gt 0) {
                [void]$sha.TransformBlock($body, 0, $body.Length, $null, 0)
            }
        }
        [void]$sha.TransformFinalBlock(@(), 0, 0)
        return ([BitConverter]::ToString($sha.Hash)).Replace('-', '').ToLower()
    }
    finally { $sha.Dispose() }
}

function Test-LogFinished {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $bytes = Read-SharedBytes $Path
    $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    return $text.Contains('DIFF END OK')
}

function Read-ProbeStamp {
    param([string]$Path)
    $probe = $null
    $log = $null
    foreach ($line in ([System.IO.File]::ReadAllText($Path) -split "`r?`n")) {
        if ($line -match '^probe=([0-9a-f]{64})$') { $probe = $Matches[1] }
        if ($line -match '^log=([0-9a-f]{64})$') { $log = $Matches[1] }
    }
    return @{ Probe = $probe; Log = $log }
}

function Write-ProbeStamp {
    param([string]$Path, [string]$ProbeHash, [string]$LogHash)
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $text = "probe=$ProbeHash`r`nlog=$LogHash`r`n"
    [System.IO.File]::WriteAllText($Path, $text)
}

function Get-ReasonText {
    param([string]$Reason)
    switch ($Reason) {
        'match' { return '' }
        'adopted' { return 'adopted existing windows.log' }
        'missing-log' { return 'missing log' }
        'incomplete-log' { return 'log has no DIFF END OK' }
        'probe-changed' { return 'probe changed' }
        'log-changed' { return 'log changed' }
        default { return $Reason }
    }
}

function Resolve-WindowsProbe {
    param(
        [string[]]$Files,
        [string[]]$Labels,
        [string]$Log,
        [string]$StampPath,
        [string]$Name,
        [switch]$Adopt
    )
    $probeHash = Get-SourceIdentityHash -Files $Files -Labels $Labels
    $hasLog = Test-Path -LiteralPath $Log
    $finished = Test-LogFinished $Log
    $logHash = $null
    if ($hasLog) { $logHash = Get-Sha256Hex (Read-SharedBytes $Log) }
    $reuse = $false
    $reason = $null

    if (-not (Test-Path -LiteralPath $StampPath)) {
        if ($finished -and $Adopt) {
            Write-ProbeStamp -Path $StampPath -ProbeHash $probeHash -LogHash $logHash
            $reuse = $true
            $reason = 'adopted'
        }
        elseif (-not $hasLog) { $reason = 'missing-log' }
        else { $reason = 'incomplete-log' }
    }
    else {
        $saved = Read-ProbeStamp $StampPath
        if ($saved.Probe -ne $probeHash) { $reason = 'probe-changed' }
        elseif (-not $hasLog) { $reason = 'missing-log' }
        elseif (-not $finished) { $reason = 'incomplete-log' }
        elseif ($saved.Log -ne $logHash) { $reason = 'log-changed' }
        else {
            $reuse = $true
            $reason = 'match'
        }
    }

    return [pscustomobject]@{
        Name = $Name
        Reuse = [bool]$reuse
        Reason = $reason
        ProbeHash = $probeHash
        Detail = (Get-ReasonText $reason)
    }
}

function Get-CatalogProbe {
    param([string]$Root, [string]$ProbeName)
    $labels = @($script:ProbeCatalog[$ProbeName])
    $files = @()
    foreach ($rel in $labels) {
        $path = Join-Path $Root ($rel -replace '/', '\')
        if (-not (Test-Path -LiteralPath $path)) { throw "Probe source was not found at $path" }
        $files += $path
    }
    $dir = Join-Path $Root "output\$ProbeName"
    return [pscustomobject]@{
        Files = $files
        Labels = $labels
        Log = (Join-Path $dir 'windows.log')
        Stamp = (Join-Path $dir 'windows.probe')
    }
}

function Write-ProbeDecision {
    param($Decision)
    if ($Decision.Reuse) {
        if ($Decision.Reason -eq 'adopted') {
            Write-Host "REUSE $($Decision.Name) $($Decision.Detail)"
        }
        else { Write-Host "REUSE $($Decision.Name)" }
    }
    else { Write-Host "CAPTURE $($Decision.Name) $($Decision.Detail)" }
}

function Invoke-ProbeStampSelfTest {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ("probe-stamp-" + [guid]::NewGuid().ToString('n'))
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    try {
        $probe = Join-Path $root 'probe.c'
        $log = Join-Path $root 'windows.log'
        $stamp = Join-Path $root 'windows.probe'
        $labels = @('probe.c')
        [System.IO.File]::WriteAllText($probe, "int main(void) { return 1; }`n")
        [System.IO.File]::WriteAllText($log, "DIFF NtClose 0x00000000`r`n")

        $open = Resolve-WindowsProbe -Files @($probe) -Labels $labels -Log $log -StampPath $stamp -Name 'self' -Adopt
        if ($open.Reuse -or $open.Reason -ne 'incomplete-log') { throw "An unfinished log was $($open.Reason)" }
        if (Test-Path -LiteralPath $stamp) { throw "An unfinished log wrote a stamp" }

        [System.IO.File]::WriteAllText($log, "DIFF NtClose 0x00000000`r`nDIFF END OK`r`n")
        $first = Resolve-WindowsProbe -Files @($probe) -Labels $labels -Log $log -StampPath $stamp -Name 'self' -Adopt
        if (-not $first.Reuse -or $first.Reason -ne 'adopted') { throw "Adopt failed: $($first.Reason)" }
        if (-not (Test-Path -LiteralPath $stamp)) { throw "Adopt did not write a stamp" }

        $second = Resolve-WindowsProbe -Files @($probe) -Labels $labels -Log $log -StampPath $stamp -Name 'self' -Adopt
        if (-not $second.Reuse -or $second.Reason -ne 'match') { throw "Second check was $($second.Reason)" }

        [System.IO.File]::WriteAllText($probe, "int main(void) { return 2; }`n")
        $changed = Resolve-WindowsProbe -Files @($probe) -Labels $labels -Log $log -StampPath $stamp -Name 'self' -Adopt
        if ($changed.Reuse -or $changed.Reason -ne 'probe-changed') { throw "Probe change was $($changed.Reason)" }
        $saved = Read-ProbeStamp $stamp
        if ($saved.Probe -eq $changed.ProbeHash) { throw "Probe change rewrote the stamp" }

        [System.IO.File]::WriteAllText($probe, "int main(void) { return 1; }`n")
        [System.IO.File]::WriteAllText($log, "DIFF NtClose 0x00000000`r`nDIFF END OK`r`nEXTRA`r`n")
        $replaced = Resolve-WindowsProbe -Files @($probe) -Labels $labels -Log $log -StampPath $stamp -Name 'self' -Adopt
        if ($replaced.Reuse -or $replaced.Reason -ne 'log-changed') { throw "Replaced log was $($replaced.Reason)" }

        $probeHash = Get-SourceIdentityHash -Files @($probe) -Labels $labels
        $logHash = Get-Sha256Hex (Read-SharedBytes $log)
        Write-ProbeStamp -Path $stamp -ProbeHash $probeHash -LogHash $logHash
        $after = Resolve-WindowsProbe -Files @($probe) -Labels $labels -Log $log -StampPath $stamp -Name 'self' -Adopt
        if (-not $after.Reuse -or $after.Reason -ne 'match') { throw "Restamp was $($after.Reason)" }

        Write-Host "Probe stamp self-test passed."
    }
    finally {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($SelfTest) {
    Invoke-ProbeStampSelfTest
    exit 0
}

if ($Action -eq 'Stamp' -and -not $Name) { throw "Stamp needs a probe name" }
$names = @($script:ProbeCatalog.Keys)
if ($Name) { $names = @($Name) }
$capture = $false
foreach ($probeName in $names) {
    $paths = Get-CatalogProbe -Root $SourceRoot -ProbeName $probeName
    if ($Action -eq 'Stamp') {
        if (-not (Test-LogFinished $paths.Log)) { throw "Windows log has no DIFF END OK" }
        $probeHash = Get-SourceIdentityHash -Files $paths.Files -Labels $paths.Labels
        $logHash = Get-Sha256Hex (Read-SharedBytes $paths.Log)
        Write-ProbeStamp -Path $paths.Stamp -ProbeHash $probeHash -LogHash $logHash
        Write-Host "STAMP $probeName"
        if ($PassThru) {
            Write-Output ([pscustomobject]@{
                Name = $probeName
                Reuse = $true
                Reason = 'match'
                ProbeHash = $probeHash
                Detail = ''
            })
        }
        continue
    }
    $decision = Resolve-WindowsProbe -Files $paths.Files -Labels $paths.Labels -Log $paths.Log -StampPath $paths.Stamp -Name $probeName -Adopt
    Write-ProbeDecision $decision
    if (-not $decision.Reuse) { $capture = $true }
    if ($PassThru) { Write-Output $decision }
}
if ($capture) { exit 2 }
exit 0
