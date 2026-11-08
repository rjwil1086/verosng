# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$BuildDir = "",
    [string]$RosBE = "",
    [ValidateSet("amd64", "i386")]
    [string]$Arch = "amd64"
)

$ErrorActionPreference = "Stop"

function Find-RosBE {
    param([string]$Hint)
    if ($Hint -and (Test-Path (Join-Path $Hint "rosbe-gcc-env.cmd"))) {
        return (Resolve-Path $Hint).Path
    }
    foreach ($candidate in @("C:\RosBE", "D:\RosBE", "$env:SystemDrive\RosBE")) {
        if (Test-Path (Join-Path $candidate "rosbe-gcc-env.cmd")) {
            return $candidate
        }
    }
    return $null
}

if (-not $BuildDir) {
    if ($Arch -eq "amd64") {
        $BuildDir = Join-Path $SourceRoot "build-amd64"
    }
    else {
        $BuildDir = Join-Path $SourceRoot "build"
    }
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
$envCmd = Join-Path $RosBERoot "rosbe-gcc-env.cmd"
$buildArgs = "--build `"$BuildDir`" --target bootcd"
$runner = Join-Path $BuildDir "build-bootcd.cmd"
if ($Arch -eq "i386") {
    $toolchain = Join-Path $SourceRoot "toolchain-gcc.cmake"
    $cmakeArgs = "-S `"$SourceRoot`" -B `"$BuildDir`" -G Ninja -DCMAKE_TOOLCHAIN_FILE:FILEPATH=`"$toolchain`" -DARCH:STRING=i386 -DCMAKE_BUILD_TYPE=Debug -DENABLE_CCACHE:BOOL=0"
    $lines = @(
        "@echo off",
        "set ROS_ARCH=i386",
        "set BUILD_ENVIRONMENT=MinGW",
        "set _ROSBE_BASEDIR=$RosBERoot",
        "set _ROSBE_HOST_MINGWPATH=$RosBERoot\i386",
        "set _ROSBE_TARGET_MINGWPATH=$RosBERoot\i386",
        "set BISON_PKGDATADIR=$RosBERoot\share\bison",
        "set M4=$RosBERoot\bin\m4.exe",
        "set _ROSBE_ORIGINALPATH=$RosBERoot;$RosBERoot\bin;%PATH%",
        "set PATH=$RosBERoot\bin;$RosBERoot\i386\bin;%PATH%",
        "call `"$envCmd`""
    )
}
else {
    $clang = $null
    foreach ($candidate in @(
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
}
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
