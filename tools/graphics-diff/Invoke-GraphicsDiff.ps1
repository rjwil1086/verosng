# SPDX-License-Identifier: GPL-2.0-or-later

param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$Disk = "D:\VMware\Windows 10 x64\Windows 10 x64.vmdk",
    [string]$Qemu64 = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [switch]$SelfTest,
    [switch]$SkipWindows,
    [switch]$SkipReactOS
)

$ErrorActionPreference = "Stop"
if ($SelfTest) {
    & (Join-Path $PSScriptRoot "Test-Compare.ps1")
    exit $LASTEXITCODE
}

$out = Join-Path $SourceRoot "output\graphics-diff"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$script:MonitorPort = 4466

function Assert-GuestsFree {
    $running = @(Get-Process qemu-system-x86_64, qemu-system-i386 -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw "QEMU is already running. The graphics probe will not start another guest."
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
    $gcc = "D:\RosBE\amd64\bin\x86_64-w64-mingw32-gcc.exe"
    if (-not (Test-Path $gcc)) { throw "RosBE amd64 gcc was not found at $gcc" }
    $env:PATH = "D:\RosBE\bin;D:\RosBE\amd64\bin;" + $env:PATH
    $exe = Join-Path $out "probe.exe"
    & $gcc -std=c99 -Wall -Wextra -Os -s -o $exe (Join-Path $PSScriptRoot "probe.c") -lgdi32
    if ($LASTEXITCODE -ne 0) { throw "probe build failed" }
    return $exe
}

function Get-Fat12 {
    param($Bytes, [int]$Fat, [int]$Cluster)
    $offset = $Fat + $Cluster + [Math]::Floor($Cluster / 2)
    $value = [BitConverter]::ToUInt16($Bytes, $offset)
    if (($Cluster % 2) -eq 0) { return ($value -band 0x0FFF) }
    return (($value -shr 4) -band 0x0FFF)
}

function Set-Fat12 {
    param($Bytes, [int]$Fat, [int]$Cluster, [int]$Value)
    $offset = $Fat + $Cluster + [Math]::Floor($Cluster / 2)
    $old = [BitConverter]::ToUInt16($Bytes, $offset)
    if (($Cluster % 2) -eq 0) {
        $new = ($old -band 0xF000) -bor ($Value -band 0x0FFF)
    }
    else {
        $new = ($old -band 0x000F) -bor (($Value -band 0x0FFF) -shl 4)
    }
    [BitConverter]::GetBytes([uint16]$new).CopyTo($Bytes, $offset)
}

function New-SinkImage {
    param([string]$Path)
    $sector = 512
    $total = 2880
    $bytes = New-Object byte[] ($total * $sector)
    $bytes[0] = 0xEB
    $bytes[1] = 0x3C
    $bytes[2] = 0x90
    [Text.Encoding]::ASCII.GetBytes("MSDOS5.0").CopyTo($bytes, 3)
    $bytes[11] = 0x00
    $bytes[12] = 0x02
    $bytes[13] = 1
    $bytes[14] = 1
    $bytes[16] = 2
    $bytes[17] = 0xE0
    $bytes[18] = 0x00
    [BitConverter]::GetBytes([uint16]$total).CopyTo($bytes, 19)
    $bytes[21] = 0xF0
    $bytes[22] = 9
    $bytes[24] = 18
    $bytes[26] = 2
    $bytes[38] = 0x29
    [Text.Encoding]::ASCII.GetBytes("GFXDIFF   ").CopyTo($bytes, 43)
    [Text.Encoding]::ASCII.GetBytes("FAT12   ").CopyTo($bytes, 54)
    $bytes[510] = 0x55
    $bytes[511] = 0xAA
    $fat = $sector
    $bytes[$fat] = 0xF0
    $bytes[$fat + 1] = 0xFF
    $bytes[$fat + 2] = 0xFF
    Set-Fat12 $bytes $fat 2 0xFFF
    [Array]::Copy($bytes, $fat, $bytes, $fat + (9 * $sector), 9 * $sector)
    $root = (1 + (2 * 9)) * $sector
    [Text.Encoding]::ASCII.GetBytes("SINK    TXT").CopyTo($bytes, $root)
    $bytes[$root + 11] = 0x20
    $bytes[$root + 26] = 2
    $bytes[$root + 28] = 4
    $data = $root + (224 * 32)
    [Text.Encoding]::ASCII.GetBytes("SINK").CopyTo($bytes, $data)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Add-SinkFile {
    param([string]$Image, [string]$Source, [string]$FatName)
    if ($FatName.Length -ne 11) { throw "FAT name must be 11 characters" }
    $bytes = [IO.File]::ReadAllBytes($Image)
    $payload = [IO.File]::ReadAllBytes($Source)
    $sector = 512
    $reserved = [BitConverter]::ToUInt16($bytes, 14)
    $spf = [BitConverter]::ToUInt16($bytes, 22)
    $rootEntries = [BitConverter]::ToUInt16($bytes, 17)
    $spc = $bytes[13]
    $fat = $reserved * $sector
    $root = ($reserved + (2 * $spf)) * $sector
    $data = $root + ($rootEntries * 32)
    $cluster = 2
    while ($cluster -lt 0xFF0) {
        if ((Get-Fat12 $bytes $fat $cluster) -eq 0) { break }
        $cluster++
    }
    $first = $cluster
    $offset = 0
    while ($offset -lt $payload.Length) {
        $at = $data + (($cluster - 2) * $spc * $sector)
        $take = [Math]::Min($spc * $sector, $payload.Length - $offset)
        [Array]::Copy($payload, $offset, $bytes, $at, $take)
        $offset += $take
        $next = $cluster + 1
        $mark = $(if ($offset -ge $payload.Length) { 0xFFF } else { $next })
        Set-Fat12 $bytes $fat $cluster $mark
        Set-Fat12 $bytes ($fat + ($spf * $sector)) $cluster $mark
        $cluster = $next
    }
    $slot = -1
    for ($i = 0; $i -lt $rootEntries; $i++) {
        $off = $root + ($i * 32)
        if ($bytes[$off] -eq 0 -or $bytes[$off] -eq 0xE5) { $slot = $off; break }
    }
    if ($slot -lt 0) { throw "The sink root directory is full" }
    [Text.Encoding]::ASCII.GetBytes($FatName).CopyTo($bytes, $slot)
    $bytes[$slot + 11] = 0x20
    [BitConverter]::GetBytes([uint16]$first).CopyTo($bytes, $slot + 26)
    [BitConverter]::GetBytes([uint32]$payload.Length).CopyTo($bytes, $slot + 28)
    [IO.File]::WriteAllBytes($Image, $bytes)
}

function Read-SinkText {
    param([string]$Image)
    if (-not (Test-Path $Image)) { return "" }
    $bytes = [IO.File]::ReadAllBytes($Image)
    $sector = [BitConverter]::ToUInt16($bytes, 11)
    if ($sector -eq 0) { return "" }
    $reserved = [BitConverter]::ToUInt16($bytes, 14)
    $spf = [BitConverter]::ToUInt16($bytes, 22)
    $rootEntries = [BitConverter]::ToUInt16($bytes, 17)
    $spc = $bytes[13]
    $root = ($reserved + (2 * $spf)) * $sector
    $data = $root + ($rootEntries * 32)
    $fat = $reserved * $sector
    for ($i = 0; $i -lt $rootEntries; $i++) {
        $off = $root + ($i * 32)
        if ($off + 32 -gt $bytes.Length) { break }
        if ($bytes[$off] -eq 0) { break }
        if ($bytes[$off] -eq 0xE5) { continue }
        $raw = [Text.Encoding]::ASCII.GetString($bytes, $off, 11)
        if ($raw -ne "RESULT  LOG") { continue }
        $cluster = [BitConverter]::ToUInt16($bytes, $off + 26)
        $size = [int][BitConverter]::ToUInt32($bytes, $off + 28)
        $chunks = New-Object System.Collections.Generic.List[byte]
        $guard = 0
        while ($cluster -ge 2 -and $cluster -lt 0xFF8 -and $guard -lt 4000 -and $chunks.Count -lt $size) {
            $at = $data + (($cluster - 2) * $spc * $sector)
            $take = [Math]::Min($spc * $sector, $size - $chunks.Count)
            for ($b = 0; $b -lt $take; $b++) {
                if ($at + $b -ge $bytes.Length) { break }
                $chunks.Add($bytes[$at + $b])
            }
            $cluster = Get-Fat12 $bytes $fat $cluster
            $guard++
        }
        return [Text.Encoding]::ASCII.GetString($chunks.ToArray())
    }
    return ""
}

function Publish-Sink {
    param([string]$Image, [string]$Log)
    $text = Read-SinkText $Image
    if ($text -and $text.Contains("DIFF ")) {
        [IO.File]::WriteAllText($Log, $text)
    }
}

function Test-DiffEnd {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $stream = $null
    try {
        $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        $reader = New-Object System.IO.StreamReader($stream)
        $text = $reader.ReadToEnd()
        $reader.Close()
        $stream = $null
        return $text.Contains("DIFF END OK")
    }
    catch { return $false }
    finally { if ($stream) { $stream.Close() } }
}

function Wait-Probe {
    param($Proc, [string]$Log, [int]$Seconds)
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-DiffEnd $Log) { return $true }
        if ($Proc -and $Proc.HasExited) { return $false }
        Start-Sleep -Seconds 3
    }
    return $false
}

function Start-Qemu {
    param([string]$Exe, [string]$Arguments, [string]$ErrorLog)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $Arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $script:QemuErrStream = New-Object System.IO.FileStream(
        $ErrorLog,
        [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::ReadWrite)
    $proc.StandardError.BaseStream.CopyToAsync($script:QemuErrStream) | Out-Null
    return $proc
}

function Stop-Qemu {
    param($Proc)
    if ($Proc -and -not $Proc.HasExited) {
        Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
        [void]$Proc.WaitForExit(20000)
    }
    if ($script:QemuErrStream) {
        $script:QemuErrStream.Dispose()
        $script:QemuErrStream = $null
    }
}

function Install-StartupProbe {
    param([string]$Exe)
    $vhd = Join-Path $out "windows.vhdx"
    $sparse = Join-Path $out "windows-sparse.vhdx"
    $marker = Join-Path $out "windows-startup.ok"
    if ((Test-Path $vhd) -and (Test-Path $marker)) { return $vhd }
    if (-not (Test-Path $sparse)) {
        if ((Test-Path $vhd) -and -not (Test-Path $marker)) {
            Move-Item -LiteralPath $vhd -Destination $sparse
        }
        else {
            Write-Host "Converting the Windows disk"
            & "C:\Program Files\qemu\qemu-img.exe" convert -O vhdx $Disk $sparse
            if ($LASTEXITCODE -ne 0) { throw "qemu-img convert failed" }
        }
    }
    if (-not (Test-Path $vhd)) {
        Write-Host "Writing a mountable copy"
        $inputStream = [System.IO.File]::OpenRead($sparse)
        $outputStream = [System.IO.File]::Create($vhd)
        try {
            $inputStream.CopyTo($outputStream)
        }
        finally {
            $inputStream.Close()
            $outputStream.Close()
        }
    }
    $mounted = $false
    try {
        $image = Mount-DiskImage -ImagePath $vhd -PassThru
        $mounted = $true
        Start-Sleep -Seconds 3
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
        Copy-Item $Exe (Join-Path $startup "probe.exe") -Force
        Set-Content -Path $marker -Value "ok" -Encoding ASCII
    }
    catch {
        if ($mounted) { throw }
        $writer = Join-Path $SourceRoot "output\syscall-diff\ntfs-replace.py"
        if (-not (Test-Path -LiteralPath $writer)) { throw }
        Write-Host "Mount was refused; writing probe.exe into the startup folder"
        & python $writer $vhd $Exe --create *>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            & python $writer $vhd $Exe *>&1 | ForEach-Object { Write-Host $_ }
        }
        if ($LASTEXITCODE -ne 0) { throw "startup probe was not installed" }
        Set-Content -Path $marker -Value "ok" -Encoding ASCII
    }
    finally {
        if ($mounted) { Dismount-DiskImage -ImagePath $vhd | Out-Null }
    }
    return $vhd
}

$exe = Build-Probe
Write-Host "Built $exe"
if ($SkipWindows -and $SkipReactOS) { exit 0 }

Assert-GuestsFree
$windowsLog = Join-Path $out "windows.log"
$reactosLog = Join-Path $out "reactos.log"

if (-not $SkipWindows) {
    if (-not (Test-Path $Disk)) { throw "No installed disk at $Disk" }
    if (-not (Test-Path $Qemu64)) { throw "QEMU was not found at $Qemu64" }
    if (Test-Path $windowsLog) { Remove-Item $windowsLog -Force }
    $winSink = Join-Path $out "windows-sink.img"
    New-SinkImage $winSink
    $vhd = Install-StartupProbe $exe
    $codeFd = "C:\Program Files\qemu\share\edk2-x86_64-code.fd"
    $varsFd = Join-Path $out "ovmf-vars.fd"
    if (-not (Test-Path $varsFd)) {
        Copy-Item "C:\Program Files\qemu\share\edk2-i386-vars.fd" $varsFd -Force
    }
    $script:MonitorPort = 4466
    $serial = ($windowsLog -replace '\\', '/')
    $arg = "-accel tcg -snapshot -machine q35 -cpu max -m 8192 -smp 4 -display none -vga std " +
        "-serial file:$serial " +
        "-monitor tcp:127.0.0.1:$($script:MonitorPort),server,nowait " +
        "-drive if=pflash,format=raw,readonly=on,file=`"$codeFd`" " +
        "-drive if=pflash,format=raw,file=`"$varsFd`" " +
        "-drive `"file=$vhd,format=vhdx,if=none,id=hd`" -device nvme,drive=hd,serial=nvme0,bootindex=1 " +
        "-drive `"file=$winSink,format=raw,if=none,id=sink`" -device ich9-ahci,id=ahci -device ide-hd,bus=ahci.0,drive=sink"
    Write-Host "Booting Windows"
    $proc = Start-Qemu $Qemu64 $arg (Join-Path $out "windows.err")
    Wait-Probe $proc $windowsLog 2400 | Out-Null
    Stop-Qemu $proc
    Publish-Sink $winSink $windowsLog
    Write-Host "Windows log $windowsLog"
}

if (-not $SkipReactOS) {
    $bootcd = Join-Path $SourceRoot "build-amd64\bootcd.iso"
    if (-not (Test-Path $bootcd)) { throw "ReactOS bootcd was not found at $bootcd" }
    if (-not (Test-Path $Qemu64)) { throw "QEMU was not found at $Qemu64" }
    if (Test-Path $reactosLog) { Remove-Item $reactosLog -Force }
    $rosSink = Join-Path $out "reactos-sink.img"
    New-SinkImage $rosSink
    Add-SinkFile $rosSink $exe "PROBE   EXE"
    $bootLog = Join-Path $out "reactos-boot.log"
    $bios = "C:\Program Files\qemu\share"
    $script:MonitorPort = 4467
    $bootSerial = ($bootLog -replace '\\', '/')
    $probeSerial = ($reactosLog -replace '\\', '/')
    $arg = "-accel tcg -L `"$bios`" -m 8192 -smp 4 -cdrom `"$bootcd`" -boot d -display none -no-reboot " +
        "-serial file:$bootSerial -serial file:$probeSerial " +
        "-monitor tcp:127.0.0.1:$($script:MonitorPort),server,nowait " +
        "-drive file=`"$rosSink`",format=raw,if=floppy,index=0"
    Write-Host "Booting VerOS"
    $proc = Start-Qemu $Qemu64 $arg (Join-Path $out "reactos.err")
    $ready = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $ready) {
        if ((Test-Path $bootLog) -and (Select-String -LiteralPath $bootLog -Pattern "Parent is Desktop" -Quiet)) { break }
        if ($proc.HasExited) { break }
        Start-Sleep -Seconds 3
    }
    if (-not $proc.HasExited) {
        Wait-Probe $proc $reactosLog 180 | Out-Null
        if (-not (Test-DiffEnd $reactosLog)) { Wait-Probe $proc $bootLog 20 | Out-Null }
    }
    Stop-Qemu $proc
    Publish-Sink $rosSink $reactosLog
    if (-not (Test-DiffEnd $reactosLog) -and (Test-DiffEnd $bootLog)) {
        Copy-Item $bootLog $reactosLog -Force
    }
    Write-Host "VerOS log $reactosLog"
}

if ((Test-DiffEnd $windowsLog) -and (Test-DiffEnd $reactosLog)) {
    & (Join-Path $PSScriptRoot "Compare-Graphics.ps1") -SourceRoot $SourceRoot -WindowsLog $windowsLog -ReactOSLog $reactosLog -OutputDir $out
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $site = Join-Path $SourceRoot "veros\dashboard\latest.json"
    if (Test-Path -LiteralPath $site) {
        & (Join-Path $SourceRoot "tools\dashboard\Publish-TabProbes.ps1") -SourceRoot $SourceRoot -WebsitePath $site -Section Graphics
    }
}
else {
    Write-Host "Probe log is incomplete; comparison was not published"
    exit 1
}
exit 0
