# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = "Continue"
try { $null = [Console]::In.ReadToEnd() } catch { }
$hook = Join-Path $PSScriptRoot "..\..\tools\dashboard\Invoke-DashboardHook.ps1"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $hook -FromWorkingTree
Write-Output "{}"
exit 0
