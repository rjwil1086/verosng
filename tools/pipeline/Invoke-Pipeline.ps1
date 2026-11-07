# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [int]$TimeoutSec = 1200
)

$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "Build-Bootcd.ps1") -SourceRoot $SourceRoot
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot "Test-Boot.ps1") -SourceRoot $SourceRoot -TimeoutSec $TimeoutSec
$bootCode = $LASTEXITCODE
$latest = Get-Content (Join-Path $SourceRoot "output\boot-latest.json") -Raw | ConvertFrom-Json
& (Join-Path $PSScriptRoot "Get-CrashReport.ps1") -SerialLog $latest.serialLog -PluginLog $latest.pluginLog -SourceRoot $SourceRoot
& (Join-Path $SourceRoot "tools\compat\Build-Snapshot.ps1") -SourceRoot $SourceRoot -PluginLog $latest.pluginLog -SerialLog $latest.serialLog
& (Join-Path $SourceRoot "tools\compat\Merge-DashboardAbi.ps1") -SourceRoot $SourceRoot
exit $bootCode
