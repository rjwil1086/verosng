# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [switch]$Stage,
    [switch]$FromWorkingTree
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $root

if ($FromWorkingTree) {
    $staged = @(& git diff --name-only --diff-filter=ACMRD)
    $staged += @(& git diff --cached --name-only --diff-filter=ACMRD)
    $staged = @($staged | Where-Object { $_ } | Select-Object -Unique)
}
else {
    $staged = @(& git diff --cached --name-only --diff-filter=ACMRD)
}
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
    "compat/reference/win10-22h2-hal.txt",
    "tools/kernel-abi/"
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
$probePaths = @(
    "ntoskrnl/mm/",
    "ntoskrnl/ps/",
    "ntoskrnl/ob/",
    "ntoskrnl/se/",
    "ntoskrnl/config/",
    "compat/catalog/syscall-subsystems.json",
    "tools/dashboard/Publish-TabProbes.ps1"
)
$tracePaths = @(
    "tools/windows-trace/",
    "tools/qemu-harness/abi.c"
)
$driverAbiPaths = @(
    "ntoskrnl/io/",
    "hal/",
    "sdk/lib/drivers/wdf/",
    "compat/catalog/driver-abi-groups.json",
    "tools/qemu-harness/"
)
$graphicsPaths = @(
    "win32ss/",
    "dll/directx/",
    "compat/catalog/graphics.json",
    "tools/graphics-diff/",
    "compat/reference/win10-22h2-graphics.txt"
)

$sections = New-Object System.Collections.Generic.List[string]
foreach ($path in $staged) {
    if (Test-PathHit $path $syscallPaths) { $sections.Add("Syscalls") | Out-Null }
    if (Test-PathHit $path $kernelPaths) { $sections.Add("KernelAbi") | Out-Null }
    if (Test-PathHit $path $appPaths) { $sections.Add("Applications") | Out-Null }
    if (Test-PathHit $path $driverPaths) { $sections.Add("Drivers") | Out-Null }
    if (Test-PathHit $path $probePaths) { $sections.Add("HeatMap") | Out-Null; $sections.Add("Tasks") | Out-Null; $sections.Add("Regressions") | Out-Null }
    if (Test-PathHit $path $tracePaths) { $sections.Add("Traces") | Out-Null }
    if (Test-PathHit $path $driverAbiPaths) { $sections.Add("DriverAbi") | Out-Null }
    if (Test-PathHit $path $graphicsPaths) { $sections.Add("Graphics") | Out-Null }
}
$unique = @($sections | Select-Object -Unique)
if ($unique.Count -eq 0) { exit 0 }

Write-Host ("Refreshing dashboard sections: " + ($unique -join ", "))
& (Join-Path $PSScriptRoot "Update-Dashboard.ps1") -SourceRoot $root -Section $unique
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Stage) {
    $paths = @(
        "veros/dashboard/latest.json",
        "veros/dashboard/history.json",
        "dashboard/data/snapshot.json",
        "dashboard/data/snapshot.js"
    )
    $changed = @(& git diff --name-only -- $paths)
    if ($changed.Count -gt 0) {
        & git add -- $changed
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Host ("Staged " + ($changed -join ", "))
    }
}
exit 0
