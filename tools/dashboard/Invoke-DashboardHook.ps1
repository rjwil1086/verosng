# SPDX-License-Identifier: GPL-2.0-or-later

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $root

$staged = @(& git diff --cached --name-only --diff-filter=ACMRD)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($staged.Count -eq 0) { exit 0 }

function Test-PathHit {
    param([string]$Path, [string[]]$Prefixes)
    $normalized = ($Path -replace '\\', '/').TrimStart('./')
    foreach ($prefix in $Prefixes) {
        $needle = $prefix -replace '\\', '/'
        if ($normalized.StartsWith($needle, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
        if ($normalized -eq $needle.TrimEnd('/')) { return $true }
    }
    return $false
}

$syscallPaths = @(
    "ntoskrnl/ke/",
    "dll/ntdll/",
    "tools/syscall-diff/",
    "compat/reference/win10-22h2-syscalls.txt"
)
$kernelPaths = @(
    "ntoskrnl/",
    "hal/",
    "sdk/lib/drivers/wdf/",
    "tools/qemu-harness/",
    "compat/reference/win10-22h2-ntoskrnl.txt",
    "compat/reference/win10-22h2-hal.txt"
)
$appPaths = @(
    "base/applications/",
    "base/shell/",
    "dll/win32/",
    "compat/catalog/apps.json"
)
$driverPaths = @(
    "drivers/",
    "win32ss/drivers/",
    "ntoskrnl/io/",
    "compat/catalog/drivers.json"
)

$sections = New-Object System.Collections.Generic.List[string]
foreach ($path in $staged) {
    if (Test-PathHit $path $syscallPaths) { $sections.Add("Syscalls") | Out-Null }
    if (Test-PathHit $path $kernelPaths) { $sections.Add("KernelAbi") | Out-Null }
    if (Test-PathHit $path $appPaths) { $sections.Add("Applications") | Out-Null }
    if (Test-PathHit $path $driverPaths) { $sections.Add("Drivers") | Out-Null }
}
$unique = @($sections | Select-Object -Unique)
if ($unique.Count -eq 0) { exit 0 }

Write-Host ("Refreshing dashboard sections: " + ($unique -join ", "))
& (Join-Path $PSScriptRoot "Update-Dashboard.ps1") -SourceRoot $root -Section $unique
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$paths = @(
    "veros/dashboard/latest.json",
    "dashboard/data/snapshot.json",
    "dashboard/data/snapshot.js"
)
$changed = @(& git diff --name-only -- $paths)
if ($changed.Count -gt 0) {
    & git add -- $changed
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host ("Staged " + ($changed -join ", "))
}
exit 0
