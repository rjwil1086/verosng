# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$Qemu = "C:\Program Files\qemu\qemu-system-i386.exe"
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$out = Join-Path $SourceRoot "output\plugin-test"
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Find-Gcc64 {
    $names = @(
        "x86_64-w64-mingw32-gcc.exe",
        "gcc.exe"
    )
    $dirs = @(
        "C:\RosBE\amd64\bin",
        "D:\RosBE\amd64\bin",
        "C:\RosBE\bin",
        "D:\RosBE\bin",
        "C:\msys64\mingw64\bin",
        "C:\Program Files\Git\mingw64\bin"
    )
    foreach ($dir in $dirs) {
        foreach ($name in $names) {
            $path = Join-Path $dir $name
            if (Test-Path $path) { return $path }
        }
    }
    $cmd = Get-Command gcc.exe, x86_64-w64-mingw32-gcc.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    return $null
}

$gcc = Find-Gcc64
if (-not $gcc) {
    Write-Error "No gcc was found to build abi.dll. Install RosBE or a 64-bit mingw-w64 gcc."
}
$gccDir = Split-Path $gcc -Parent
$rosbeBin = Join-Path (Split-Path (Split-Path $gccDir -Parent) -Parent) "bin"
if (Test-Path $rosbeBin) {
    $env:PATH = "$rosbeBin;$gccDir;" + $env:PATH
}

$dll = Join-Path $here "abi.dll"
& $gcc -shared -O2 -o $dll (Join-Path $here "abi.c")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$bytes = [byte[]]@(
    0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0x8E, 0xC0,
    0xC7, 0x06, 0xB8, 0x00, 0x1B, 0x7C,
    0xC7, 0x06, 0xBA, 0x00, 0x00, 0x00,
    0xE8, 0x06, 0x00,
    0xCD, 0x2E,
    0x0F, 0x34,
    0xF4,
    0xCF,
    0xC3
)
$image = New-Object byte[] 512
[Array]::Copy($bytes, $image, $bytes.Length)
$image[510] = 0x55
$image[511] = 0xAA
$img = Join-Path $out "guest.img"
[IO.File]::WriteAllBytes($img, $image)
$map = Join-Path $out "exports.map"
Set-Content -Path $map -Value "ntoskrnl.exe!IoCallDriver 0x7C1C"
$log = Join-Path $out "abi.jsonl"
$err = Join-Path $out "qemu.err"
$dllFwd = $dll -replace '\\', '/'
$logFwd = $log -replace '\\', '/'
$mapFwd = $map -replace '\\', '/'
$bios = "C:\Program Files\qemu\share"
$qemuArgs = "-accel tcg -L `"$bios`" -m 64 -fda `"$img`" -boot a -display none -no-reboot -plugin `"file=$dllFwd,log=$logFwd,map=$mapFwd`""
$proc = Start-Process -FilePath $Qemu -ArgumentList $qemuArgs -PassThru -RedirectStandardError $err -WindowStyle Hidden
if (-not $proc.WaitForExit(15000)) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}
if (-not (Test-Path $log)) {
    Write-Host (Get-Content $err -Raw -ErrorAction SilentlyContinue)
    Write-Error "Plugin log was not written."
}
$text = Get-Content $log -Raw
if ($text -notmatch '"kind":"syscall"') { Write-Error "Missing syscall event in $log" }
if ($text -notmatch 'IoCallDriver') { Write-Error "Missing ntoskrnl export event in $log" }
Write-Host "Plugin fixture passed."
Write-Host $log
