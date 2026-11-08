# SPDX-License-Identifier: GPL-2.0-or-later

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here "..\..")).Path
$out = Join-Path $root "output\graphics-diff-fixture"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
& (Join-Path $here "Compare-Graphics.ps1") `
    -SourceRoot $root `
    -WindowsLog (Join-Path $here "fixtures\windows.log") `
    -ReactOSLog (Join-Path $here "fixtures\reactos.log") `
    -OutputDir $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$report = Get-Content (Join-Path $out "report.json") -Raw | ConvertFrom-Json
function Assert-Call {
    param($Name, $Status)
    $row = @($report.calls | Where-Object { $_.name -eq $Name })[0]
    if (-not $row) { throw "Missing $Name" }
    if ($row.status -ne $Status) { throw "$Name is $($row.status), expected $Status" }
}
Assert-Call "Gdi.Fill" "works"
Assert-Call "Gdi.Stretch" "partial"
Assert-Call "Module.Dxgi" "missing"
Assert-Call "Gdi.Rect" "broken"
Assert-Call "Gdi.BitsPixel" "broken"
Assert-Call "D3d11.CreateDevice" "broken"
if ($report.capabilityHits -ne 9 -or $report.capabilityTotal -ne 17) {
    throw "Capability count was $($report.capabilityHits) of $($report.capabilityTotal)"
}
if ($report.agreementHits -ne 10 -or $report.agreementTotal -ne 18) {
    throw "Agreement count was $($report.agreementHits) of $($report.agreementTotal)"
}
if ([double]$report.capability -ne 52.9) { throw "Capability percent was $($report.capability)" }
if ([double]$report.agreement -ne 55.6) { throw "Agreement percent was $($report.agreement)" }
$fill = @($report.calls | Where-Object { $_.name -eq "Gdi.Fill" })[0]
if (-not $fill.windowsCompleted -or -not $fill.capabilityMatch) { throw "Fill was not a capability match" }
$stretch = @($report.calls | Where-Object { $_.name -eq "Gdi.Stretch" })[0]
if ($stretch.windowsCompleted) { throw "A shared failure counted as a Windows success" }
if (-not $stretch.agreementMatch) { throw "Shared failure was not agreement" }
$text = Get-Content (Join-Path $out "report.txt") -Raw
if ($text -notmatch "Capability: 9 of 17 \(52.9%\)") { throw "Capability headline was not written" }
if ($text -notmatch "Gdi.Rect\r?\nWindows: 0x22222222\r?\nReactOS: 0x22223333\r?\n\r?\nMismatch") {
    throw "Mismatch block was not written"
}
Write-Host "Graphics compare fixture passed."
