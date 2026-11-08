# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$Disk = "D:\VMware\Windows 10 x64\Windows 10 x64.vmdk",
    [string]$Qemu64 = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [switch]$SelfTest,
    [switch]$RunGuests,
    [switch]$SkipWindows,
    [switch]$SkipReactOS
)

$ErrorActionPreference = "Stop"

if ($SelfTest) {
    & (Join-Path $PSScriptRoot "Compare-KernelAbi.ps1") -SelfTest
    exit $LASTEXITCODE
}

function Assert-GuestsFree {
    $running = @(Get-Process qemu-system-x86_64, qemu-system-i386 -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw "QEMU is already running. The kernel probe will not start another guest."
    }
    $vmrun = "C:\Program Files\VMware\VMware Workstation\vmrun.exe"
    if (Test-Path $vmrun) {
        $listed = & $vmrun list
        if ($listed -match "Total running VMs:\s*[1-9]") {
            throw "VMware still has a running VM. Power it off before capturing."
        }
    }
}

function Build-Probe {
    param([string]$OutDir)
    $gcc = "D:\RosBE\amd64\bin\x86_64-w64-mingw32-gcc.exe"
    $dlltool = "D:\RosBE\amd64\bin\x86_64-w64-mingw32-dlltool.exe"
    if (-not (Test-Path $gcc)) { throw "RosBE amd64 gcc was not found at $gcc" }
    if (-not (Test-Path $dlltool)) { throw "RosBE dlltool was not found at $dlltool" }
    $env:PATH = "D:\RosBE\bin;D:\RosBE\amd64\bin;" + $env:PATH
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $def = Join-Path $OutDir "ntoskrnl.def"
    $lib = Join-Path $OutDir "libntoskrnl.a"
    [System.IO.File]::WriteAllText($def, "LIBRARY ntoskrnl.exe`r`nEXPORTS`r`nMmGetSystemRoutineAddress`r`n")
    & $dlltool @("-d", $def, "-l", $lib)
    if ($LASTEXITCODE -ne 0) { throw "dlltool failed" }
    $sys = Join-Path $OutDir "probe.sys"
    & $gcc @(
        "-std=c99", "-Wall", "-Wextra", "-ffreestanding", "-fno-stack-protector",
        "-nostdlib", "-shared", "-Wl,--subsystem,native", "-Wl,--entry,DriverEntry",
        "-o", $sys, (Join-Path $PSScriptRoot "probe.c"), $lib
    )
    if ($LASTEXITCODE -ne 0) { throw "probe.sys build failed" }
    $exe = Join-Path $OutDir "loader.exe"
    & $gcc @("-std=c99", "-Wall", "-Wextra", "-O2", "-o", $exe, (Join-Path $PSScriptRoot "loader.c"))
    if ($LASTEXITCODE -ne 0) { throw "loader.exe build failed" }
    return [ordered]@{ Sys = $sys; Exe = $exe }
}

function New-ProbeIso {
    param([string]$Exe, [string]$Sys, [string]$OutDir)
    $mkisofs = Join-Path $SourceRoot "build-amd64\host-tools\bin\mkisofs.exe"
    if (-not (Test-Path $mkisofs)) { $mkisofs = Join-Path $SourceRoot "build\host-tools\bin\mkisofs.exe" }
    if (-not (Test-Path $mkisofs)) { throw "mkisofs was not found" }
    $dir = Join-Path $OutDir "iso"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item $Exe (Join-Path $dir "loader.exe") -Force
    Copy-Item $Sys (Join-Path $dir "probe.sys") -Force
    $iso = Join-Path $OutDir "probe.iso"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $mkisofs -o $iso -J -r -V KABI $dir 2>&1 | Out-Null
    $ErrorActionPreference = $previous
    if ($LASTEXITCODE -ne 0) { throw "mkisofs failed" }
    return $iso
}

function Install-StartupProbe {
    param([string]$Exe, [string]$Sys, [string]$OutDir)
    $vhd = Join-Path $OutDir "windows.vhdx"
    $marker = Join-Path $OutDir "windows-startup.ok"
    if ((Test-Path $vhd) -and (Test-Path $marker)) { return $vhd }
    if (-not (Test-Path $vhd)) {
        $source = Join-Path $SourceRoot "output\syscall-diff\windows.vhdx"
        if (Test-Path $source) {
            Write-Host "Copying the Windows disk"
            Copy-Item $source $vhd
        }
        else {
            Write-Host "Converting the Windows disk"
            & "C:\Program Files\qemu\qemu-img.exe" convert -O vhdx $Disk $vhd
            if ($LASTEXITCODE -ne 0) { throw "qemu-img convert failed" }
        }
    }
    $image = Mount-DiskImage -ImagePath $vhd -PassThru
    Start-Sleep -Seconds 3
    try {
        $disk = Get-DiskImage -ImagePath $vhd | Get-Disk
        if ($disk.IsOffline) { Set-Disk -Number $disk.Number -IsOffline $false }
        if ($disk.IsReadOnly) { Set-Disk -Number $disk.Number -IsReadOnly $false }
        $letter = $null
        foreach ($part in (Get-Partition -DiskNumber $disk.Number)) {
            if (-not $part.DriveLetter) { continue }
            $root = "$($part.DriveLetter):\"
            if (Test-Path (Join-Path $root "Windows\System32")) {
                $letter = $part.DriveLetter
                break
            }
        }
        if (-not $letter) { throw "The Windows partition did not mount" }
        $startup = "${letter}:\ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp"
        New-Item -ItemType Directory -Force -Path $startup | Out-Null
        Copy-Item $Exe (Join-Path $startup "loader.exe") -Force
        Copy-Item $Sys (Join-Path $startup "probe.sys") -Force
        Set-Content -Path $marker -Value "ok" -Encoding ASCII
    }
    finally {
        Dismount-DiskImage -ImagePath $vhd | Out-Null
    }
    return $vhd
}

function Send-Monitor {
    param([string]$Command)
    $mon = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $script:MonitorPort)
    $writer = New-Object System.IO.StreamWriter($mon.GetStream())
    $writer.NewLine = "`n"
    $writer.AutoFlush = $true
    Start-Sleep -Milliseconds 80
    $writer.WriteLine($Command)
    Start-Sleep -Milliseconds 40
    $writer.Close()
    $mon.Close()
}

function Send-GuestText {
    param([string]$Text)
    foreach ($ch in $Text.ToCharArray()) {
        $key = switch ($ch) {
            ' ' { 'spc' }
            '\' { 'backslash' }
            ':' { 'shift-semicolon' }
            '.' { 'dot' }
            default {
                if ([string]$ch -cmatch '[A-Z]') { "shift-" + ([string]$ch).ToLower() }
                else { ([string]$ch).ToLower() }
            }
        }
        Send-Monitor "sendkey $key"
    }
}

function Test-DiffEnd {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $false }
    return [bool](Select-String -LiteralPath $Path -Pattern "DIFF END OK" -Quiet)
}

function Wait-DiffEnd {
    param([string]$Path, [int]$Seconds)
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-DiffEnd $Path) { return $true }
        Start-Sleep -Seconds 5
    }
    return $false
}

function Stop-Qemu {
    param($Proc)
    try { Send-Monitor "quit" } catch {}
    if ($Proc -and -not $Proc.HasExited) {
        if (-not $Proc.WaitForExit(20000)) {
            Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

function Start-WindowsProbe {
    param([string]$Iso, [string]$Log)
    $codeFd = "C:\Program Files\qemu\share\edk2-x86_64-code.fd"
    $varsFd = Join-Path $out "ovmf-vars.fd"
    Copy-Item "C:\Program Files\qemu\share\edk2-i386-vars.fd" $varsFd -Force
    $script:MonitorPort = 4456
    $serial = ($Log -replace '\\', '/')
    $disk = ($Disk -replace '\\', '/')
    $arg = "-name `"Windows kernel ABI`" -accel tcg -snapshot -machine q35 -cpu max -m 4096 -smp 2 -vga std " +
        "-serial file:$serial " +
        "-monitor tcp:127.0.0.1:$($script:MonitorPort),server,nowait " +
        "-drive if=pflash,format=raw,readonly=on,file=`"$codeFd`" " +
        "-drive if=pflash,format=raw,file=`"$varsFd`" " +
        "-drive `"file=$disk,format=vmdk,if=none,id=hd`" -device nvme,drive=hd,serial=nvme0,bootindex=1 " +
        "-drive file=`"$Iso`",media=cdrom,if=none,id=probe -device ide-cd,drive=probe,bootindex=2"
    Write-Host "Booting Windows. The probe CD holds loader.exe and probe.sys."
    $err = Join-Path $out "windows.err"
    $proc = Start-Process -FilePath $Qemu64 -ArgumentList $arg -PassThru -RedirectStandardError $err -WindowStyle Normal
    $finished = Wait-DiffEnd $Log 1200
    Stop-Qemu $proc
    if (-not $finished) { Write-Host "Windows log has no DIFF END OK" }
}

function Start-ReactOSProbe {
    param([string]$Iso, [string]$Log)
    $bootcd = Join-Path $SourceRoot "build-amd64\bootcd.iso"
    if (-not (Test-Path $bootcd)) { throw "ReactOS bootcd was not found at $bootcd" }
    $bios = "C:\Program Files\qemu\share"
    $script:MonitorPort = 4457
    $serial = ($Log -replace '\\', '/')
    $arg = "-name `"VerOS kernel ABI`" -accel tcg -L `"$bios`" -m 2048 -smp 2 -cdrom `"$bootcd`" -boot d -vga std -no-reboot " +
        "-serial file:$serial " +
        "-monitor tcp:127.0.0.1:$($script:MonitorPort),server,nowait " +
        "-drive file=`"$Iso`",media=cdrom,if=ide,index=1"
    Write-Host "Booting VerOS"
    $err = Join-Path $out "reactos.err"
    $proc = Start-Process -FilePath $Qemu64 -ArgumentList $arg -PassThru -RedirectStandardError $err -WindowStyle Normal
    $deadline = (Get-Date).AddSeconds(900)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        if (Test-DiffEnd $Log) { $ready = $true; break }
        if ((Test-Path $Log) -and (Select-String -LiteralPath $Log -Pattern "explorer.exe|No waveform-audio" -Quiet)) {
            $ready = $true
            break
        }
        Start-Sleep -Seconds 5
    }
    if (-not (Test-DiffEnd $Log)) { Wait-DiffEnd $Log 1200 | Out-Null }
    Stop-Qemu $proc
    if (-not (Test-DiffEnd $Log)) { Write-Host "VerOS log has no DIFF END OK" }
}

$out = Join-Path $SourceRoot "output\kernel-abi"
$built = Build-Probe $out
Write-Host "Built $($built.Sys)"

if (-not $RunGuests) {
    & (Join-Path $PSScriptRoot "Compare-KernelAbi.ps1") -SourceRoot $SourceRoot -OutputDir $out
    exit $LASTEXITCODE
}

Assert-GuestsFree
if (-not (Test-Path $Disk)) { throw "No installed disk at $Disk" }
if (-not (Test-Path $Qemu64)) { throw "QEMU was not found at $Qemu64" }
$windowsLog = Join-Path $out "windows.log"
$reactosLog = Join-Path $out "reactos.log"
$iso = New-ProbeIso -Exe $built.Exe -Sys $built.Sys -OutDir $out
if (-not $SkipWindows) {
    if (Test-Path $windowsLog) { Remove-Item $windowsLog -Force }
    Start-WindowsProbe -Iso $iso -Log $windowsLog
}
if (-not $SkipReactOS) {
    if (Test-Path $reactosLog) { Remove-Item $reactosLog -Force }
    Start-ReactOSProbe -Iso $iso -Log $reactosLog
}
& (Join-Path $PSScriptRoot "Compare-KernelAbi.ps1") -SourceRoot $SourceRoot -OutputDir $out -WindowsLog $windowsLog -ReactOSLog $reactosLog
exit $LASTEXITCODE
