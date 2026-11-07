# SPDX-License-Identifier: GPL-2.0-or-later

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here "..\..")).Path
$out = Join-Path $root "output\syscall-diff-fixture"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
& (Join-Path $here "Compare-Syscalls.ps1") `
    -WindowsLog (Join-Path $here "fixtures\windows.log") `
    -ReactOSLog (Join-Path $here "fixtures\reactos.log") `
    -OutputDir $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$report = Get-Content (Join-Path $out "report.json") -Raw | ConvertFrom-Json
function Assert-Call {
    param($Name, $Status)
    $row = $report.calls | Where-Object { $_.name -eq $Name } | Select-Object -First 1
    if (-not $row) { throw "Missing $Name" }
    if ($row.status -ne $Status) { throw "$Name is $($row.status), expected $Status" }
}
Assert-Call "NtClose" "works"
Assert-Call "NtCreateFile" "broken"
Assert-Call "NtReadFile" "partial"
Assert-Call "NtOpenProcess" "missing"
Assert-Call "NtYieldExecution" "broken"
$open = $report.calls | Where-Object { $_.name -eq "NtOpenProcess" } | Select-Object -First 1
if ($open.windows -ne "STATUS_SUCCESS") { throw "Windows name was $($open.windows)" }
if ($open.reactos -ne "missing") { throw "ReactOS name was $($open.reactos)" }
$text = Get-Content (Join-Path $out "report.txt") -Raw
if ($text -notmatch "NtCreateFile\r?\nWindows: STATUS_SUCCESS\r?\nReactOS: STATUS_INVALID_PARAMETER\r?\n\r?\nMismatch") {
    throw "Mismatch block was not written"
}
$untested = @($report.calls | Where-Object { $_.status -eq "untested" })
if ($untested.Count -lt 90) { throw "Expected the rest to stay untested, got $($untested.Count)" }
Write-Host "Compare fixture passed."
