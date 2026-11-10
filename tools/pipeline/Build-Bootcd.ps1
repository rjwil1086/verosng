# PROJECT:     VerOS NT
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tools/pipeline/Build-Bootcd.ps1
# PURPOSE:     Build the amd64 bootcd
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
# PROGRAMMERS: Ryan Wilson

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$BuildDir = "",
    [string]$RosBE = "",
    [string]$Arch = "amd64"
)

$ErrorActionPreference = "Stop"

function Find-RosBE {
    param([string]$Hint)
    if ($Hint -and (Test-Path (Join-Path $Hint "rosbe-gcc-env.cmd"))) {
        return (Resolve-Path $Hint).Path
    }
    $local = Join-Path (Split-Path $SourceRoot -Parent) "tools\rosbe"
    foreach ($candidate in @($local, "C:\RosBE", "D:\RosBE", "$env:SystemDrive\RosBE")) {
        if (Test-Path (Join-Path $candidate "rosbe-gcc-env.cmd")) {
            return $candidate
        }
    }
    return $null
}

if ($Arch -ne "amd64") {
    Write-Error "ARCH=amd64 is the only supported OS build. A native i386 boot is not supported. 32-bit applications run on the amd64 boot."
}
if (-not $BuildDir) {
    $BuildDir = Join-Path $SourceRoot "build-amd64"
}
$RosBERoot = Find-RosBE -Hint $RosBE
if (-not $RosBERoot) {
    $logDir = Join-Path $SourceRoot "output"
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $message = "RosBE was not found. Install RosBE 2.2.1 for Windows and re-run. See docs/LOCAL_BUILD.md."
    Set-Content -Path (Join-Path $logDir "build-error.txt") -Value $message
    Write-Error $message
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$buildArgs = "--build `"$BuildDir`" --target bootcd"
$runner = Join-Path $BuildDir "build-bootcd.cmd"
$clang = $null
$localClang = Join-Path (Split-Path $SourceRoot -Parent) "tools\clang\bin\clang.exe"
foreach ($candidate in @(
    $localClang,
    "D:\LLVM-18\clang+llvm-18.1.8-x86_64-pc-windows-msvc\bin\clang.exe",
    "C:\Program Files\LLVM\bin\clang.exe"
)) {
    if (Test-Path $candidate) { $clang = $candidate; break }
}
if (-not $clang) {
    Write-Error "amd64 needs Clang. RosBE's amd64 GCC was built without plugin support, so it cannot compile this tree."
}
$clangBin = Split-Path $clang -Parent
$toolchain = Join-Path $SourceRoot "toolchain-clang.cmake"
$cmakeArgs = "-S `"$SourceRoot`" -B `"$BuildDir`" -G Ninja -DCMAKE_TOOLCHAIN_FILE:FILEPATH=`"$toolchain`" -DARCH:STRING=amd64 -DCMAKE_BUILD_TYPE=Debug -DENABLE_CCACHE:BOOL=0"
$lines = @(
    "@echo off",
    "set ROS_ARCH=amd64",
    "set _ROSBE_ROSSCRIPTDIR=$RosBERoot",
    "set BISON_PKGDATADIR=$RosBERoot\share\bison",
    "set M4=$RosBERoot\bin\m4.exe",
    "set PATH=$clangBin;$RosBERoot\amd64\bin;$RosBERoot\bin;%PATH%"
)
$lines += @(
    "cmake $cmakeArgs",
    "if errorlevel 1 exit /b %errorlevel%",
    "set TRIES=0",
    ":linkretry",
    "cmake $buildArgs -- -j 8",
    "if not errorlevel 1 goto :buildok",
    "set /a TRIES+=1",
    "if %TRIES% lss 8 goto :linkretry",
    "exit /b 1",
    ":buildok"
)
$lines | Set-Content -Path $runner -Encoding ASCII
Write-Host "Building $Arch Debug bootcd with $RosBERoot"
& cmd.exe /d /c "`"$runner`""
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
$iso = Join-Path $BuildDir "bootcd.iso"
if (-not (Test-Path $iso)) {
    Write-Error "Build finished but $iso is missing."
}
Write-Host "ISO: $iso"
